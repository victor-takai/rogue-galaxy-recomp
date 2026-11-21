#include "gs_tripwire.h"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "runtime/gs/gs_backend.h"

#if defined(RG_HAS_SDLGPU)
#include "gfx/native/native_pipeline.h"
#endif

#if defined(__linux__) || defined(__APPLE__)
#include <execinfo.h>
#include <unistd.h>
#endif

namespace rg::graphics
{
namespace
{

enum class Mode
{
    Drop,
    Abort,
    Pass
};

Mode mode()
{
    static const Mode m = [] {
        const char *value = std::getenv("RG_NATIVE_GS");
        if (value && std::strcmp(value, "abort") == 0)
            return Mode::Abort;
        if (value && std::strcmp(value, "pass") == 0)
            return Mode::Pass;
        return Mode::Drop;
    }();
    return m;
}

bool pipelineOn()
{
#if defined(RG_HAS_SDLGPU)
    return dq8::gfx::native::enabled();
#else
    return false;
#endif
}

enum Call : size_t
{
    kSubmit,
    kSubmitMany,
    kSubmitNativeMesh,
    kBeginTransfer,
    kUploadImage,
    kFlush,
    kTextureFlush,
    kSync,
    kPresent,
    kClearFramebuffer,
    kConsumeLocalToHost,
    kReadVram,
    kWriteVram,
    kSnapshotVram,
    kTransferSnapshot,
    kPreparePresentation,
    kCallCount
};
constexpr std::array<const char *, kCallCount> kNames = {
    "Submit",       "SubmitMany",       "SubmitNativeMesh", "BeginTransfer",
    "UploadImage",  "Flush",            "TextureFlush",     "Sync",               "Present",
    "ClearFramebuffer", "ConsumeLocalToHostBytes", "ReadVram", "WriteVram",       "SnapshotVram",
    "GetTransferSnapshot", "PreparePresentation"};

std::array<std::atomic<uint64_t>, kCallCount> g_counts{};

// A crossing of `call`; true when it goes on to the backend.
bool cross(Call call, bool read)
{
    if (!pipelineOn())
        return true;
    const uint64_t n = g_counts[call].fetch_add(1u, std::memory_order_relaxed);
    if (n == 0u)
    {
        static std::mutex printing;
        std::lock_guard lock(printing);
        std::fprintf(stderr, "[native-gs] the GS backend was entered: %s%s\n", kNames[call],
                     mode() == Mode::Drop && !read ? " (dropped)" : "");
#if defined(__linux__) || defined(__APPLE__)
        void *frames[32];
        const int depth = backtrace(frames, 32);
        backtrace_symbols_fd(frames, depth, STDERR_FILENO);
#endif
        if (mode() == Mode::Abort)
            std::abort();
    }
    return mode() == Mode::Pass || read;
}

class TripwireBackend final : public GSRasterBackend
{
public:
    explicit TripwireBackend(std::unique_ptr<GSRasterBackend> inner) : m_inner(std::move(inner)) {}
    ~TripwireBackend() override
    {
        if (!pipelineOn())
            return;
        std::fprintf(stderr, "[native-gs] GS backend calls under the pipeline:");
        bool any = false;
        for (size_t i = 0; i < kCallCount; ++i)
            if (const uint64_t n = g_counts[i].load())
            {
                std::fprintf(stderr, " %s %llu", kNames[i], static_cast<unsigned long long>(n));
                any = true;
            }
        std::fprintf(stderr, "%s\n", any ? "" : " none");
    }

    void Initialize(uint8_t *vram, uint32_t vramSize) override { m_inner->Initialize(vram, vramSize); }
    void Reset() override { m_inner->Reset(); }
    void Submit(const GSPrimitiveBatch &batch) override
    {
        if (cross(kSubmit, false))
            m_inner->Submit(batch);
    }
    void SubmitMany(const GSPrimitiveBatch *const *batches, size_t count) override
    {
        if (cross(kSubmitMany, false))
            m_inner->SubmitMany(batches, count);
    }
    void SubmitNativeMesh(const GSNativeMesh &mesh, const GSDrawState &state) override
    {
        if (cross(kSubmitNativeMesh, false))
            m_inner->SubmitNativeMesh(mesh, state);
    }
    void BeginTransfer(const GSTransferCommand &command) override
    {
        if (cross(kBeginTransfer, false))
            m_inner->BeginTransfer(command);
    }
    void UploadImage(const uint8_t *data, uint32_t sizeBytes) override
    {
        if (cross(kUploadImage, false))
            m_inner->UploadImage(data, sizeBytes);
    }
    void Flush() override
    {
        if (cross(kFlush, false))
            m_inner->Flush();
    }
    void TextureFlush() override
    {
        if (cross(kTextureFlush, false))
            m_inner->TextureFlush();
    }
    void Sync(GSSyncReason reason) override
    {
        if (cross(kSync, false))
            m_inner->Sync(reason);
    }
    PresentationFrame Present(const GSPresentationRequest &request) override
    {
        cross(kPresent, true);
        return m_inner->Present(request);
    }
    bool ClearFramebuffer(const GSContext &context, uint32_t rgba) override
    {
        return cross(kClearFramebuffer, false) ? m_inner->ClearFramebuffer(context, rgba) : true;
    }
    uint32_t ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes) override
    {
        cross(kConsumeLocalToHost, true);
        return m_inner->ConsumeLocalToHostBytes(dst, maxBytes);
    }
    uint32_t ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const override
    {
        cross(kReadVram, true);
        return m_inner->ReadVram(psm, base, bw, x, y);
    }
    void WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value) override
    {
        if (cross(kWriteVram, false))
            m_inner->WriteVram(psm, base, bw, x, y, value);
    }
    void SnapshotVram(std::vector<uint8_t> &out) const override
    {
        cross(kSnapshotVram, true);
        m_inner->SnapshotVram(out);
    }
    GSTransferSnapshot GetTransferSnapshot() const override
    {
        cross(kTransferSnapshot, true);
        return m_inner->GetTransferSnapshot();
    }
    bool SupportsPreparedPresentation() const override { return m_inner->SupportsPreparedPresentation(); }
    bool QueuesPreparedPresentation() const override { return m_inner->QueuesPreparedPresentation(); }
    GSPresentationTicket PreparePresentation(const GSPresentationRequest &request) override
    {
        cross(kPreparePresentation, true);
        return m_inner->PreparePresentation(request);
    }
    PresentationFrame DisplayPreparedPresentation(const GSPresentationTicket &ticket) override
    {
        return m_inner->DisplayPreparedPresentation(ticket);
    }
    void CancelPreparedPresentations() noexcept override { m_inner->CancelPreparedPresentations(); }

private:
    std::unique_ptr<GSRasterBackend> m_inner;
};

} // namespace

std::unique_ptr<GSRasterBackend> tripOnGs(std::unique_ptr<GSRasterBackend> backend)
{
    return std::make_unique<TripwireBackend>(std::move(backend));
}

} // namespace rg::graphics
