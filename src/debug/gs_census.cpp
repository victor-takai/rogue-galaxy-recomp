#include "debug/gs_census.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "Stubs/Pad.h"
#include "vif1_thread.h"
#if RG_HAS_SDLGPU
#include "gfx/backends/sdlgpu/sdlgpu_targets.h"
#endif

namespace rg::debug
{
namespace
{

using Clock = std::chrono::steady_clock;

// TEST and ALPHA register fields (GS manual 7.2).
constexpr uint64_t kTestAte = 1u << 0;
constexpr uint64_t kTestAfail = 3u << 12;
constexpr uint64_t kTestDate = 1u << 14;
constexpr unsigned kAlphaC = 4; // C operand: 0 As, 1 Ad, 2 FIX

struct Key
{
    uint64_t test, alpha;
    uint32_t fbp, fbmsk, zbp;
    uint8_t psm, zpsm, prim, flags; // flags: abe tme zmask fba pabe
    uint32_t tbp;
    uint8_t tpsm;

    auto tie() const { return std::tie(test, alpha, fbp, fbmsk, zbp, psm, zpsm, prim, flags, tbp, tpsm); }
    bool operator<(const Key &o) const { return tie() < o.tie(); }
};

struct Tally
{
    uint64_t batches = 0;
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
};

bool partialMask(uint32_t fbmsk)
{
    for (unsigned shift = 0; shift < 32; shift += 8)
    {
        const uint32_t channel = (fbmsk >> shift) & 0xFFu;
        if (channel != 0 && channel != 0xFFu)
        {
            return true;
        }
    }
    return false;
}

bool unsupported(const GSDrawState &state)
{
    const uint64_t test = state.context.test;
    const bool afail = (test & kTestAte) && (test & kTestAfail);
    const bool destAlpha = state.prim.abe && ((state.context.alpha >> kAlphaC) & 3u) == 1u;
    return (test & kTestDate) || afail || destAlpha || partialMask(state.context.frame.fbmsk);
}

// F7 presses not yet turned into a captured frame (RG_GS_CAPTURE).
std::atomic<unsigned> g_captureRequests{0u};

class CensusBackend final : public GSRasterBackend
{
public:
    CensusBackend(std::unique_ptr<GSRasterBackend> inner, bool census, uint64_t skipMask, uint64_t skipValue,
                  std::string logPath, uint32_t logAt, uint32_t snapAt, std::string captureDir)
        : m_inner(std::move(inner)), m_census(census), m_skipMask(skipMask), m_skipValue(skipValue),
          m_logPath(std::move(logPath)), m_logAt(logAt), m_snapAt(snapAt), m_captureDir(std::move(captureDir)),
          m_logging(!m_logPath.empty() || !m_captureDir.empty()), m_last(Clock::now())
    {
        // RG_GS_FRAMESKIP=A-B[,C-D]: batches of the logged frame left out,
        // both ends included; "A-" runs to the end of the frame.
        if (const char *skip = std::getenv("RG_GS_FRAMESKIP"))
        {
            for (const char *p = skip; *p;)
            {
                char *end = nullptr;
                const uint32_t first = static_cast<uint32_t>(std::strtoul(p, &end, 10));
                uint32_t last = first;
                if (end != p && *end == '-')
                {
                    const char *afterDash = end + 1;
                    last = static_cast<uint32_t>(std::strtoul(afterDash, &end, 10));
                    if (end == afterDash)
                    {
                        last = UINT32_MAX;
                    }
                }
                if (end == p || (*end != ',' && *end != '\0'))
                {
                    std::fprintf(stderr, "[gs-census] RG_GS_FRAMESKIP: cannot read \"%s\"\n", p);
                    break;
                }
                m_frameSkip.emplace_back(first, last);
                p = (*end == ',') ? end + 1 : end;
            }
        }
    }

    void Initialize(uint8_t *vram, uint32_t vramSize) override { m_inner->Initialize(vram, vramSize); }
    void Reset() override { m_inner->Reset(); }
    void SubmitNativeMesh(const GSNativeMesh &mesh, const GSDrawState &state) override
    {
        m_inner->SubmitNativeMesh(mesh, state);
    }
    void Submit(const GSPrimitiveBatch &batch) override
    {
        if (keep(batch))
        {
            m_inner->Submit(batch);
        }
    }
    void SubmitMany(const GSPrimitiveBatch *const *batches, size_t count) override
    {
        if (!m_skipMask && !m_logging)
        {
            for (size_t i = 0; i < count; ++i)
            {
                keep(*batches[i]);
            }
            m_inner->SubmitMany(batches, count);
            return;
        }
        // One at a time: a batch may be dropped, and the file.K.vram dump
        // must come after every batch before K reached the backend.
        for (size_t i = 0; i < count; ++i)
        {
            if (keep(*batches[i]))
            {
                m_inner->Submit(*batches[i]);
            }
        }
    }
    void BeginTransfer(const GSTransferCommand &command) override
    {
        withLog([&] {
            const GSBitBltBuf &b = command.bitbltbuf;
            std::fprintf(m_log, "T dir=%u sbp=%x sbw=%u spsm=%x dbp=%x dbw=%u dpsm=%x src=%u,%u dst=%u,%u size=%ux%u\n",
                         command.direction, b.sbp, b.sbw, b.spsm, b.dbp, b.dbw, b.dpsm, command.trxpos.ssax,
                         command.trxpos.ssay, command.trxpos.dsax, command.trxpos.dsay, command.trxreg.rrw,
                         command.trxreg.rrh);
        });
        m_inner->BeginTransfer(command);
    }
    void UploadImage(const uint8_t *data, uint32_t sizeBytes) override { m_inner->UploadImage(data, sizeBytes); }
    void Flush() override { m_inner->Flush(); }
    void TextureFlush() override { m_inner->TextureFlush(); }
    void Sync(GSSyncReason reason) override { m_inner->Sync(reason); }
    PresentationFrame Present(const GSPresentationRequest &request) override
    {
        frameBoundary();
        return m_inner->Present(request);
    }
    bool ClearFramebuffer(const GSContext &context, uint32_t rgba) override
    {
        withLog([&] {
            std::fprintf(m_log, "C fbp=%x psm=%x rgba=%08x\n", context.frame.fbp, context.frame.psm, rgba);
        });
        return m_inner->ClearFramebuffer(context, rgba);
    }
    uint32_t ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes) override
    {
        return m_inner->ConsumeLocalToHostBytes(dst, maxBytes);
    }
    uint32_t ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const override
    {
        return m_inner->ReadVram(psm, base, bw, x, y);
    }
    void WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value) override
    {
        m_inner->WriteVram(psm, base, bw, x, y, value);
    }
    void SnapshotVram(std::vector<uint8_t> &out) const override { m_inner->SnapshotVram(out); }
    GSTransferSnapshot GetTransferSnapshot() const override { return m_inner->GetTransferSnapshot(); }
    bool SupportsPreparedPresentation() const override { return m_inner->SupportsPreparedPresentation(); }
    bool QueuesPreparedPresentation() const override { return m_inner->QueuesPreparedPresentation(); }
    GSPresentationTicket PreparePresentation(const GSPresentationRequest &request) override
    {
        frameBoundary();
        return m_inner->PreparePresentation(request);
    }
    PresentationFrame DisplayPreparedPresentation(const GSPresentationTicket &ticket) override
    {
        return m_inner->DisplayPreparedPresentation(ticket);
    }
    void CancelPreparedPresentations() noexcept override { m_inner->CancelPreparedPresentations(); }

private:
    bool keep(const GSPrimitiveBatch &batch)
    {
        const GSDrawState &state = batch.state;
        if (m_census)
        {
            note(batch);
        }
        bool kept = !m_skipMask || (state.context.test & m_skipMask) != m_skipValue;
        withLog([&] {
            if (m_logIndex == m_snapAt)
            {
                dumpVram("." + std::to_string(m_snapAt) + ".vram");
            }
            for (const auto &[first, last] : m_frameSkip)
            {
                kept = kept && (m_logIndex < first || m_logIndex > last);
            }
            logBatch(batch, kept);
        });
        return kept;
    }

    // Runs f with the log open, under its lock. Batches, transfers and clears
    // arrive on the game's thread, but a frame can end on the presenter's:
    // Present without the GS worker (RG_GS_WORKER=0), and PreparePresentation
    // when flip pacing falls back to vblanks. Closing the log under a write
    // to it would be a use after fclose.
    template <class F>
    void withLog(F &&f)
    {
        if (!m_logging)
        {
            return;
        }
        std::lock_guard lock(m_logMutex);
        if (m_log)
        {
            f();
        }
    }

    // RG_GS_FRAMELOG: the frame after the game's pad read count reaches
    // m_logAt, from one presentation to the next.
    void frameBoundary()
    {
        if (!m_logging)
        {
            return;
        }
        // The list GfxEndFrame started is still running on the VIF1 thread at
        // the flip; its batches reach this decorator from that thread. Cut
        // the frame after it, so the same game state gives the same log and
        // the same memory, whatever the threads' timing. Before the lock: the
        // VIF1 thread takes it for each batch.
        rg::vif1::drainForSnapshot();
        std::lock_guard lock(m_logMutex);
        if (m_log)
        {
            std::fclose(m_log);
            m_log = nullptr;
            // GS local memory as the frame left it, beside the log.
            dumpVram(".vram");
            m_logPath.clear();
            std::fprintf(stderr, "[gs-census] frame logged (%u batches)\n", m_logIndex);
            // RG_GS_FRAMELOG_COUNT=K: the K-1 frames after it too, as
            // file.1.txt, file.2.txt...
            if (m_moreFrames != 0u && !m_logBase.empty())
            {
                --m_moreFrames;
                m_logPath = m_logBase + "." + std::to_string(++m_extraFrame) + ".txt";
                m_logAt = 0u;
            }
            else
            {
                setTargetTrace(false);
                m_tracing = false;
            }
            return;
        }
        // RG_GS_CAPTURE=DIR: F7 logs the next frame to DIR/capture-N.txt,
        // the backend's target trace on for it.
        if (m_logPath.empty() && !m_captureDir.empty() &&
            g_captureRequests.exchange(0u, std::memory_order_relaxed) != 0u)
        {
            m_logPath = m_captureDir + "/capture-" + std::to_string(++m_captures) + ".txt";
            m_logAt = 0u;
            m_tracing = true;
            setTargetTrace(true);
            std::fprintf(stderr, "[gs-census] F7: capturing the next frame to %s\n", m_logPath.c_str());
        }
        const uint32_t padReads = ps2_stubs::getPadDebugSnapshot().ports[0][0].readCount;
        // RG_GS_TRACE_BEFORE=N: the backend's target trace
        // (DQ8_GFX_TRACE_TARGETS) from N pad reads before the logged frame
        // to its end. Left on for a whole run it prints a line per transfer
        // and slows the game to a crawl.
        if (!m_logPath.empty() && !m_tracing && m_traceBefore != 0 && padReads + m_traceBefore >= m_logAt)
        {
            m_tracing = true;
            std::fprintf(stderr, "[gs-census] target trace on at pad read %u\n", padReads);
            setTargetTrace(true);
        }
        if (!m_logPath.empty() && padReads >= m_logAt)
        {
            m_log = std::fopen(m_logPath.c_str(), "w");
            m_logIndex = 0;
            if (!m_log)
            {
                // Said once, not retried every frame.
                std::fprintf(stderr, "[gs-census] cannot write %s\n", m_logPath.c_str());
                m_logPath.clear();
            }
        }
    }

    static void setTargetTrace(bool on)
    {
#if RG_HAS_SDLGPU
        dq8::gfx::gsSetTargetTrace(on);
#else
        (void)on;
#endif
    }

    static unsigned highestCapture(const std::string &dir)
    {
        unsigned highest = 0u;
        std::error_code error;
        if (dir.empty())
        {
            return 0u;
        }
        for (const auto &entry : std::filesystem::directory_iterator(dir, error))
        {
            const std::string name = entry.path().filename().string();
            if (name.rfind("capture-", 0) == 0 && name.size() > 12u && name.compare(name.size() - 4u, 4u, ".txt") == 0)
            {
                highest = std::max(highest, static_cast<unsigned>(std::strtoul(name.c_str() + 8, nullptr, 10)));
            }
        }
        return highest;
    }

    static unsigned framesFromEnv()
    {
        const char *value = std::getenv("RG_GS_FRAMELOG_COUNT");
        const unsigned long count = value ? std::strtoul(value, nullptr, 10) : 1ul;
        return count > 1ul ? static_cast<unsigned>(std::min(count, 120ul) - 1ul) : 0u;
    }

    static std::string stripExtension(const std::string &path)
    {
        const size_t dot = path.rfind('.');
        return dot == std::string::npos ? path : path.substr(0, dot);
    }

    static uint32_t traceBeforeFromEnv()
    {
        const char *value = std::getenv("RG_GS_TRACE_BEFORE");
        return value ? static_cast<uint32_t>(std::strtoul(value, nullptr, 10)) : 0u;
    }

    void dumpVram(const std::string &suffix)
    {
        std::vector<uint8_t> vram;
        m_inner->SnapshotVram(vram);
        if (FILE *out = std::fopen((m_logPath + suffix).c_str(), "wb"))
        {
            std::fwrite(vram.data(), 1, vram.size(), out);
            std::fclose(out);
        }
    }

    void logBatch(const GSPrimitiveBatch &batch, bool kept)
    {
        const GSDrawState &st = batch.state;
        const GSContext &c = st.context;
        const GSTex0Reg &t = c.tex0;
        std::fprintf(m_log,
                     "B %u kept=%d prim=%u n=%u iip=%d tme=%d fge=%d abe=%d fst=%d ctxt=%d test=%llx alpha=%llx "
                     "fbp=%x fbw=%u psm=%x fbmsk=%x zbp=%x zpsm=%x zmsk=%d tbp=%x tbw=%u tpsm=%x tw=%u th=%u "
                     "tcc=%u tfx=%u cbp=%x cpsm=%x csa=%u clamp=%llx tex1=%llx texa=%u,%d,%u fba=%llu pabe=%d "
                     "colclamp=%llu dthe=%llu fog=%02x%02x%02x ofs=%u,%u sc=%u,%u,%u,%u",
                     m_logIndex++, kept, unsigned(st.prim.type), batch.vertexCount, st.prim.iip, st.prim.tme,
                     st.prim.fge, st.prim.abe, st.prim.fst, st.prim.ctxt, (unsigned long long)c.test,
                     (unsigned long long)c.alpha, c.frame.fbp, c.frame.fbw, c.frame.psm, c.frame.fbmsk, c.zbuf.zbp,
                     c.zbuf.psm, c.zbuf.zmask, t.tbp0, t.tbw, t.psm, t.tw, t.th, t.tcc, t.tfx, t.cbp, t.cpsm, t.csa,
                     (unsigned long long)c.clamp, (unsigned long long)c.tex1, st.texa.ta0, st.texa.aem, st.texa.ta1,
                     (unsigned long long)c.fba, st.pabe, (unsigned long long)st.colclamp,
                     (unsigned long long)st.dthe, st.fogR, st.fogG, st.fogB, c.xyoffset.ofx, c.xyoffset.ofy,
                     c.scissor.x0, c.scissor.y0, c.scissor.x1, c.scissor.y1);
        for (uint8_t i = 0; i < batch.vertexCount; ++i)
        {
            const GSVertex &v = batch.vertices[i];
            std::fprintf(m_log, " | %.4f %.4f %.0f %02x%02x%02x%02x q=%g s=%g t=%g uv=%u,%u f=%u", v.x, v.y, v.z, v.r,
                         v.g, v.b, v.a, v.q, v.s, v.t, v.u, v.v, v.fog);
        }
        std::fputc('\n', m_log);
    }

    void note(const GSPrimitiveBatch &batch)
    {
        const GSDrawState &state = batch.state;
        const GSContext &c = state.context;
        std::lock_guard lock(m_mutex);
        ++m_total;
        // A scissor wider than the buffer grows the GPU backend's surfaces
        // past FBW pages, onto the pages after them.
        if (c.scissor.x1 + 1u > c.frame.fbw * 64u)
        {
            const uint64_t key = uint64_t(c.frame.fbp) << 48 | uint64_t(c.frame.fbw) << 40 |
                                 uint64_t(c.scissor.x1) << 20 | c.scissor.y1;
            if (m_wideScissors.insert(key).second)
            {
                std::fprintf(stderr,
                             "[gs-census] scissor wider than the buffer: fbp=%x fbw=%u sc=%u,%u,%u,%u test=%llx "
                             "zbp=%x prim=%u tme=%d (pad read %u)\n",
                             c.frame.fbp, c.frame.fbw, c.scissor.x0, c.scissor.y0, c.scissor.x1, c.scissor.y1,
                             (unsigned long long)c.test, c.zbuf.zbp, unsigned(state.prim.type), state.prim.tme,
                             ps2_stubs::getPadDebugSnapshot().ports[0][0].readCount);
            }
        }
        if (unsupported(state))
        {
            const Key key{c.test,
                          c.alpha,
                          c.frame.fbp,
                          c.frame.fbmsk,
                          c.zbuf.zbp,
                          c.frame.psm,
                          c.zbuf.psm,
                          static_cast<uint8_t>(state.prim.type),
                          static_cast<uint8_t>(state.prim.abe | state.prim.tme << 1 | c.zbuf.zmask << 2 |
                                               (c.fba & 1u) << 3 | state.pabe << 4),
                          state.prim.tme ? c.tex0.tbp0 : 0u,
                          static_cast<uint8_t>(state.prim.tme ? c.tex0.psm : 0u)};
            Tally &t = m_tallies[key];
            ++t.batches;
            const float ox = c.xyoffset.ofx / 16.0f, oy = c.xyoffset.ofy / 16.0f;
            for (uint8_t i = 0; i < batch.vertexCount; ++i)
            {
                t.x0 = std::min(t.x0, batch.vertices[i].x - ox);
                t.y0 = std::min(t.y0, batch.vertices[i].y - oy);
                t.x1 = std::max(t.x1, batch.vertices[i].x - ox);
                t.y1 = std::max(t.y1, batch.vertices[i].y - oy);
            }
        }
        const Clock::time_point now = Clock::now();
        if (now - m_last < std::chrono::seconds(2))
        {
            return;
        }
        std::fprintf(stderr, "[gs-census] %llu batches, %zu states the GPU path approximates:\n",
                     static_cast<unsigned long long>(m_total), m_tallies.size());
        for (const auto &[k, t] : m_tallies)
        {
            std::fprintf(stderr,
                         "[gs-census]   %6llu x test=%05llx alpha=%llx fix=%02llx fbp=%03x psm=%02x fbmsk=%08x "
                         "zbp=%03x zpsm=%02x zmsk=%d prim=%u abe=%d tme=%d tbp=%04x tpsm=%02x fba=%d pabe=%d "
                         "bounds=(%.0f,%.0f)-(%.0f,%.0f)\n",
                         static_cast<unsigned long long>(t.batches), static_cast<unsigned long long>(k.test),
                         static_cast<unsigned long long>(k.alpha & 0xFF),
                         static_cast<unsigned long long>((k.alpha >> 32) & 0xFF), k.fbp, k.psm, k.fbmsk, k.zbp,
                         k.zpsm, (k.flags >> 2) & 1, k.prim, k.flags & 1, (k.flags >> 1) & 1, k.tbp, k.tpsm,
                         (k.flags >> 3) & 1, (k.flags >> 4) & 1, t.x0, t.y0, t.x1, t.y1);
        }
        m_tallies.clear();
        m_total = 0;
        m_last = now;
    }

    std::unique_ptr<GSRasterBackend> m_inner;
    const bool m_census;
    const uint64_t m_skipMask;
    const uint64_t m_skipValue;
    std::string m_logPath;
    uint32_t m_logAt;
    const uint32_t m_snapAt;
    const std::string m_captureDir;
    // Numbered on from the folder's highest capture-N.txt, so a later run
    // keeps an earlier one's captures.
    unsigned m_captures = highestCapture(m_captureDir);
    // The frames logged after the first (RG_GS_FRAMELOG_COUNT), named from
    // RG_GS_FRAMELOG's path without its extension.
    unsigned m_moreFrames = framesFromEnv();
    unsigned m_extraFrame = 0u;
    std::string m_logBase = stripExtension(m_logPath);
    const bool m_logging; // a log path or a capture folder was given; m_logPath empties once it is written
    const uint32_t m_traceBefore = traceBeforeFromEnv();
    bool m_tracing = false;
    std::mutex m_logMutex;
    FILE *m_log = nullptr;
    uint32_t m_logIndex = 0;
    std::vector<std::pair<uint32_t, uint32_t>> m_frameSkip;
    std::mutex m_mutex;
    std::map<Key, Tally> m_tallies;
    std::set<uint64_t> m_wideScissors;
    uint64_t m_total = 0;
    Clock::time_point m_last;
};

} // namespace

std::unique_ptr<GSRasterBackend> censusGsBackend(std::unique_ptr<GSRasterBackend> backend)
{
    const char *census = std::getenv("RG_GS_CENSUS");
    const bool enabled = census && *census && std::strcmp(census, "0") != 0;
    uint64_t mask = 0, value = 0;
    if (const char *skip = std::getenv("RG_GS_SKIP"))
    {
        char *end = nullptr;
        mask = std::strtoull(skip, &end, 16);
        value = (end && *end == ':') ? std::strtoull(end + 1, nullptr, 16) : mask;
    }
    std::string logPath;
    uint32_t logAt = 0, snapAt = UINT32_MAX;
    if (const char *log = std::getenv("RG_GS_FRAMELOG"))
    {
        logPath = log;
        const size_t at = logPath.rfind('@');
        if (at != std::string::npos)
        {
            char *end = nullptr;
            logAt = static_cast<uint32_t>(std::strtoul(logPath.c_str() + at + 1, &end, 10));
            if (end && *end == '#')
            {
                snapAt = static_cast<uint32_t>(std::strtoul(end + 1, nullptr, 10));
            }
            logPath.resize(at);
        }
    }
    std::string captureDir;
    if (const char *dir = std::getenv("RG_GS_CAPTURE"); dir && *dir)
    {
        captureDir = dir;
    }
    if (!enabled && !mask && logPath.empty() && captureDir.empty())
    {
        return backend;
    }
    std::printf("[rg] GS census%s%s%s%s\n", enabled ? " on" : "", mask ? ", skipping some draws (RG_GS_SKIP)" : "",
                logPath.empty() ? "" : ", logging a frame (RG_GS_FRAMELOG)",
                captureDir.empty() ? "" : ", F7 logs a frame (RG_GS_CAPTURE)");
    return std::make_unique<CensusBackend>(std::move(backend), enabled, mask, value, std::move(logPath), logAt, snapAt,
                                           std::move(captureDir));
}

void requestFrameCapture()
{
    g_captureRequests.fetch_add(1u, std::memory_order_relaxed);
}

} // namespace rg::debug
