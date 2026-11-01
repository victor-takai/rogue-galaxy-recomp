#include "debug/gs_timing.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

namespace rg::debug
{
namespace
{

using Clock = std::chrono::steady_clock;

enum Method : size_t
{
    kSubmit,
    kSubmitMany,
    kTransfer,
    kUpload,
    kFlush,
    kTextureFlush,
    kSyncFinish,
    kSyncLocalToHost,
    kSyncPresentation,
    kSyncOther,
    kPresent,
    kClear,
    kConsumeLocalToHost,
    kReadVram,
    kWriteVram,
    kSnapshot,
    kTransferSnapshot,
    kPrepare,
    kDisplay,
    kMethodCount
};

constexpr const char *kNames[kMethodCount] = {
    "Submit", "SubmitMany", "BeginTransfer", "UploadImage", "Flush", "TextureFlush",
    "Sync(Finish)", "Sync(LocalToHost)", "Sync(Presentation)", "Sync(other)", "Present",
    "ClearFramebuffer", "ConsumeLocalToHost", "ReadVram", "WriteVram", "SnapshotVram",
    "GetTransferSnapshot", "PreparePresentation", "DisplayPrepared",
};

struct Counter
{
    std::atomic<uint64_t> calls{0};
    std::atomic<uint64_t> nanos{0};
    std::atomic<uint64_t> maxNanos{0};
};

class TimedBackend final : public GSRasterBackend
{
public:
    TimedBackend(std::unique_ptr<GSRasterBackend> inner, const char *label)
        : m_inner(std::move(inner)), m_label(label), m_last(Clock::now())
    {
    }

    void Initialize(uint8_t *vram, uint32_t vramSize) override { m_inner->Initialize(vram, vramSize); }
    void Reset() override { m_inner->Reset(); }

    void SubmitNativeMesh(const GSNativeMesh &mesh, const GSDrawState &state) override
    {
        m_inner->SubmitNativeMesh(mesh, state);
    }
    void Submit(const GSPrimitiveBatch &batch) override
    {
        Scope s(*this, kSubmit);
        m_inner->Submit(batch);
    }
    void SubmitMany(const GSPrimitiveBatch *const *batches, size_t count) override
    {
        Scope s(*this, kSubmitMany);
        m_inner->SubmitMany(batches, count);
    }
    void BeginTransfer(const GSTransferCommand &command) override
    {
        Scope s(*this, kTransfer);
        m_inner->BeginTransfer(command);
    }
    void UploadImage(const uint8_t *data, uint32_t sizeBytes) override
    {
        Scope s(*this, kUpload);
        m_inner->UploadImage(data, sizeBytes);
    }
    void Flush() override
    {
        Scope s(*this, kFlush);
        m_inner->Flush();
    }
    void TextureFlush() override
    {
        Scope s(*this, kTextureFlush);
        m_inner->TextureFlush();
    }
    void Sync(GSSyncReason reason) override
    {
        const Method m = reason == GSSyncReason::Finish         ? kSyncFinish
                         : reason == GSSyncReason::LocalToHost  ? kSyncLocalToHost
                         : reason == GSSyncReason::Presentation ? kSyncPresentation
                                                                : kSyncOther;
        Scope s(*this, m);
        m_inner->Sync(reason);
    }
    PresentationFrame Present(const GSPresentationRequest &request) override
    {
        Scope s(*this, kPresent);
        return m_inner->Present(request);
    }
    bool ClearFramebuffer(const GSContext &context, uint32_t rgba) override
    {
        Scope s(*this, kClear);
        return m_inner->ClearFramebuffer(context, rgba);
    }
    uint32_t ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes) override
    {
        Scope s(*this, kConsumeLocalToHost);
        return m_inner->ConsumeLocalToHostBytes(dst, maxBytes);
    }
    uint32_t ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const override
    {
        Scope s(*this, kReadVram);
        return m_inner->ReadVram(psm, base, bw, x, y);
    }
    void WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value) override
    {
        Scope s(*this, kWriteVram);
        m_inner->WriteVram(psm, base, bw, x, y, value);
    }
    void SnapshotVram(std::vector<uint8_t> &out) const override
    {
        Scope s(*this, kSnapshot);
        m_inner->SnapshotVram(out);
    }
    GSTransferSnapshot GetTransferSnapshot() const override
    {
        Scope s(*this, kTransferSnapshot);
        return m_inner->GetTransferSnapshot();
    }
    bool SupportsPreparedPresentation() const override { return m_inner->SupportsPreparedPresentation(); }
    bool QueuesPreparedPresentation() const override { return m_inner->QueuesPreparedPresentation(); }
    GSPresentationTicket PreparePresentation(const GSPresentationRequest &request) override
    {
        Scope s(*this, kPrepare);
        return m_inner->PreparePresentation(request);
    }
    PresentationFrame DisplayPreparedPresentation(const GSPresentationTicket &ticket) override
    {
        Scope s(*this, kDisplay);
        return m_inner->DisplayPreparedPresentation(ticket);
    }
    void CancelPreparedPresentations() noexcept override { m_inner->CancelPreparedPresentations(); }

private:
    struct Scope
    {
        const TimedBackend &owner;
        Method method;
        Clock::time_point start = Clock::now();
        Scope(const TimedBackend &o, Method m) : owner(o), method(m) {}
        ~Scope() { owner.record(method, Clock::now() - start); }
    };

    void record(Method method, Clock::duration elapsed) const
    {
        const uint64_t ns = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
        Counter &c = m_counters[method];
        c.calls.fetch_add(1, std::memory_order_relaxed);
        c.nanos.fetch_add(ns, std::memory_order_relaxed);
        uint64_t seen = c.maxNanos.load(std::memory_order_relaxed);
        while (ns > seen && !c.maxNanos.compare_exchange_weak(seen, ns, std::memory_order_relaxed))
        {
        }
        maybeReport();
    }

    void maybeReport() const
    {
        const Clock::time_point now = Clock::now();
        std::unique_lock lock(m_reportMutex, std::try_to_lock);
        if (!lock.owns_lock() || now - m_last < std::chrono::seconds(2))
        {
            return;
        }
        const double seconds = std::chrono::duration<double>(now - m_last).count();
        m_last = now;
        std::string line = "[gs-timing] " + m_label;
        char buf[160];
        for (size_t i = 0; i < kMethodCount; ++i)
        {
            Counter &c = m_counters[i];
            const uint64_t calls = c.calls.exchange(0, std::memory_order_relaxed);
            const uint64_t nanos = c.nanos.exchange(0, std::memory_order_relaxed);
            const uint64_t maxNanos = c.maxNanos.exchange(0, std::memory_order_relaxed);
            if (calls == 0)
            {
                continue;
            }
            std::snprintf(buf, sizeof(buf), " | %s %.0f/s %.1f%% max %.1fms", kNames[i], calls / seconds,
                          100.0 * nanos / (seconds * 1e9), maxNanos / 1e6);
            line += buf;
        }
        std::fprintf(stderr, "%s\n", line.c_str());
    }

    std::unique_ptr<GSRasterBackend> m_inner;
    std::string m_label;
    mutable std::array<Counter, kMethodCount> m_counters;
    mutable std::mutex m_reportMutex;
    mutable Clock::time_point m_last;
};

bool timingEnabled()
{
    static const bool on = [] {
        const char *value = std::getenv("RG_GS_TIMING");
        return value && *value && std::strcmp(value, "0") != 0;
    }();
    return on;
}

} // namespace

void notePresenterLoop()
{
    if (!timingEnabled())
    {
        return;
    }
    // Only the presenter thread calls this.
    static Clock::time_point last = Clock::now();
    static Clock::time_point reported = last;
    static uint64_t loops = 0, over20 = 0, over40 = 0;
    static double maxGap = 0.0;
    const Clock::time_point now = Clock::now();
    const double gap = std::chrono::duration<double, std::milli>(now - last).count();
    last = now;
    ++loops;
    over20 += gap > 20.0;
    over40 += gap > 40.0;
    maxGap = gap > maxGap ? gap : maxGap;
    if (now - reported >= std::chrono::seconds(2))
    {
        std::fprintf(stderr, "[gs-timing] presenter loops %llu, gaps >20ms %llu, >40ms %llu, max %.1fms\n",
                     static_cast<unsigned long long>(loops), static_cast<unsigned long long>(over20),
                     static_cast<unsigned long long>(over40), maxGap);
        reported = now;
        loops = over20 = over40 = 0;
        maxGap = 0.0;
    }
}

std::unique_ptr<GSRasterBackend> timeGsBackend(std::unique_ptr<GSRasterBackend> backend, const char *label)
{
    if (!timingEnabled())
    {
        return backend;
    }
    return std::make_unique<TimedBackend>(std::move(backend), label);
}

} // namespace rg::debug
