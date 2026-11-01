#include "flip_pacing.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "ps2_runtime.h"
#include "return_hook.h"
#include "runtime/gs/gs_backend.h"
#include "scene_timing.h"

namespace rg::graphics
{
namespace
{

using Clock = std::chrono::steady_clock;

// GfxFlipBuffersMaybe: waits for the vsync interval, then copies the frame
// into the display buffer through the GIF; once it returns, that copy is in
// the GS stream.
constexpr uint32_t kGfxFlipBuffers = 0x00143650u;
// Without a flip for this long the game is not drawing frames of its own.
constexpr auto kFlipTimeout = std::chrono::milliseconds(200);

// Set on the presenter thread when it was handed no frame: the frontend
// then falls back to Flush, Sync and Present, which would drain the worker
// queue for nothing.
thread_local bool t_nothingToPresent = false;

class FlipPacedBackend final : public GSRasterBackend
{
public:
    explicit FlipPacedBackend(std::unique_ptr<GSRasterBackend> inner) : m_inner(std::move(inner)) {}

    // The game's thread, after each flip.
    void onGuestFlip()
    {
        GSPresentationRequest request;
        {
            std::lock_guard lock(m_mutex);
            if (!m_haveRequest)
            {
                return;
            }
            request = m_lastRequest;
        }
        // Queued behind the flip's copy: it shows exactly that frame.
        GSPresentationTicket ticket = m_inner->PreparePresentation(request);
        std::lock_guard lock(m_mutex);
        // A frame the presenter has not taken yet is dropped for the newer one.
        m_pending = std::move(ticket);
        m_lastFlip = Clock::now();
    }

    void Initialize(uint8_t *vram, uint32_t vramSize) override { m_inner->Initialize(vram, vramSize); }
    void Reset() override { m_inner->Reset(); }
    void SubmitNativeMesh(const GSNativeMesh &mesh, const GSDrawState &state) override
    {
        m_inner->SubmitNativeMesh(mesh, state);
    }
    void Submit(const GSPrimitiveBatch &batch) override { m_inner->Submit(batch); }
    void SubmitMany(const GSPrimitiveBatch *const *batches, size_t count) override
    {
        m_inner->SubmitMany(batches, count);
    }
    void BeginTransfer(const GSTransferCommand &command) override { m_inner->BeginTransfer(command); }
    void UploadImage(const uint8_t *data, uint32_t sizeBytes) override { m_inner->UploadImage(data, sizeBytes); }
    void Flush() override
    {
        if (!t_nothingToPresent)
        {
            m_inner->Flush();
        }
    }
    void TextureFlush() override { m_inner->TextureFlush(); }
    void Sync(GSSyncReason reason) override
    {
        if (!(t_nothingToPresent && reason == GSSyncReason::Presentation))
        {
            m_inner->Sync(reason);
        }
    }
    PresentationFrame Present(const GSPresentationRequest &request) override
    {
        if (t_nothingToPresent)
        {
            t_nothingToPresent = false;
            return {};
        }
        return m_inner->Present(request);
    }
    bool ClearFramebuffer(const GSContext &context, uint32_t rgba) override
    {
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

    // The presenter thread, at each new guest vblank.
    GSPresentationTicket PreparePresentation(const GSPresentationRequest &request) override
    {
        {
            std::lock_guard lock(m_mutex);
            m_lastRequest = request;
            m_haveRequest = true;
            if (Clock::now() - m_lastFlip < kFlipTimeout)
            {
                GSPresentationTicket ticket = std::move(m_pending);
                m_pending.reset();
                t_nothingToPresent = !ticket;
                return ticket;
            }
            m_pending.reset();
        }
        return m_inner->PreparePresentation(request);
    }
    PresentationFrame DisplayPreparedPresentation(const GSPresentationTicket &ticket) override
    {
        return m_inner->DisplayPreparedPresentation(ticket);
    }
    void CancelPreparedPresentations() noexcept override
    {
        {
            std::lock_guard lock(m_mutex);
            m_pending.reset();
        }
        m_inner->CancelPreparedPresentations();
    }

private:
    std::unique_ptr<GSRasterBackend> m_inner;
    std::mutex m_mutex;
    GSPresentationRequest m_lastRequest{};
    bool m_haveRequest = false;
    GSPresentationTicket m_pending;
    Clock::time_point m_lastFlip{};
};

FlipPacedBackend *g_paced = nullptr;
PS2Runtime::RecompiledFunction g_originalFlip = nullptr;
std::atomic<uint64_t> g_flips{0u};

void afterFlip(uint8_t *, R5900Context *, PS2Runtime *, uint32_t)
{
    g_flips.fetch_add(1u, std::memory_order_relaxed);
    if (g_paced)
    {
        g_paced->onGuestFlip();
    }
}

void flipHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    // The vsync interval this flip waits for (RG_FPS=60).
    rg::timing::beforeFlip(rdram, ctx);
    // Its vsync wait blocks on a fiber and keeps this native frame, but a
    // safe point inside the flip unwinds it: the frame is presented when the
    // flip has really returned (return_hook.h).
    rg::hooks::callThen(g_originalFlip, rdram, ctx, runtime, &afterFlip, 0u);
}

} // namespace

std::unique_ptr<GSRasterBackend> paceOnFlips(std::unique_ptr<GSRasterBackend> backend)
{
    const char *mode = std::getenv("RG_PRESENT");
    if ((mode && std::strcmp(mode, "vblank") == 0) || !backend->QueuesPreparedPresentation())
    {
        return backend;
    }
    auto paced = std::make_unique<FlipPacedBackend>(std::move(backend));
    g_paced = paced.get();
    return paced;
}

void installFlipHook(PS2Runtime &runtime)
{
    // Also without pacing: the hook counts the flips for the menu's frame
    // rate.
    g_originalFlip = runtime.lookupFunction(kGfxFlipBuffers);
    if (!g_originalFlip || !runtime.replaceFunction(kGfxFlipBuffers, &flipHook))
    {
        std::fprintf(stderr, "[rg] flip hook: 0x%08X is not in the function table; presenting at vblanks\n",
                     kGfxFlipBuffers);
        g_paced = nullptr;
        return;
    }
    if (g_paced)
    {
        std::printf("[rg] presenting on the game's flips\n");
    }
}

uint64_t guestFlips()
{
    return g_flips.load(std::memory_order_relaxed);
}

} // namespace rg::graphics
