// SDL3 GPU implementation of ps2xRuntime's GSRasterBackend.
//
// Draws through the host graphics pipeline into render targets held as GPU
// textures at a configurable resolution scale, with GS local memory as the
// backing store rather than the drawing destination.
#pragma once

#include "runtime/gs/gs_backend.h"
#include "gfx/backends/sdlgpu/sdlgpu_regions.h"
#include "gfx/backends/sdlgpu/sdlgpu_window.h"

#include <cstdint>
#include <memory>
#include <functional>
#include <string>

namespace dq8::gfx {

// A line the embedder appends to every DQ8_GFX_STATS_EVERY report (without
// the trailing newline), e.g. how busy its own threads were over the
// interval; empty for none (Rogue Galaxy: added).
void setStatsExtraLine(std::function<std::string()> provider);

// Called at every present with the seconds since the previous one, on the
// thread that prepares presentations (the GS worker behind the threaded
// backend), e.g. to notice a hitch as it happens (Rogue Galaxy: added).
void setPresentObserver(std::function<void(double)> observer);


class SdlPadInput;

struct SdlGpuStats {
    uint64_t presents = 0u;
    // Frames read back and sent back unchanged, returned at scale (Rogue
    // Galaxy: added).
    uint64_t roundTrips = 0u;
    // Draws whose destination alpha test or destination-alpha factor was
    // done in the shader from a snapshot, without framebuffer fetch (Rogue
    // Galaxy: added).
    uint64_t destinationReads = 0u;
    // Depth a surface held, thrown away by a host write to its pages or by
    // growing the surface, and of those the losses a draw then tested
    // against before depth was written there again (Rogue Galaxy: added,
    // issue 53).
    uint64_t depthLosses = 0u;
    uint64_t depthLossesTested = 0u;
    // Primitives drawn from the host's larger versions of their regions,
    // glyph borders drawn from its border versions and shadows from its
    // shadow versions (Rogue Galaxy: added).
    uint64_t regionsReplaced = 0u;
    uint64_t bordersRounded = 0u;
    uint64_t shadowsDrawn = 0u;
    // Local-to-local transfers done on the GPU, not through local memory.
    uint64_t gpuLocalCopies = 0u;
    // Frames shown straight from a render target, with no readback or CPU
    // compose, versus ones that had to be composed on the CPU first.
    uint64_t nativePresents = 0u;
    uint64_t composedPresents = 0u;
    uint64_t gpuComposedPresents = 0u;
    // Host->local transfers, which is how a movie frame arrives: DQ8 uploads
    // one per 16x16 tile, ~900 per frame.
    uint64_t transfersBegun = 0u;
    uint64_t transferBytes = 0u;
    uint64_t transferFlushes = 0u;
    // Host writes that went ahead of the queued draws, which did not touch
    // their pages (Rogue Galaxy: added).
    uint64_t transferFlushesSkipped = 0u;

    uint64_t primitivesSubmitted = 0u;
    uint64_t primitivesDrawn = 0u;
    // GSNativeMesh strips drawn on the GPU, and those expanded on the CPU
    // because their draw reads its target (Rogue Galaxy: added).
    uint64_t nativeMeshes = 0u;
    uint64_t nativeMeshesExpanded = 0u;
    // The draw calls the GPU's strips took, merged, and the strips wholly off
    // screen, not drawn (Rogue Galaxy: added).
    uint64_t nativeDraws = 0u;
    uint64_t nativeMeshesCulled = 0u;
    // Sprites of no width or no height, which cover no pixel, dropped before
    // any work (Rogue Galaxy: added).
    uint64_t emptySprites = 0u;
    // Drawn through the same-state fast path (Rogue Galaxy: added).
    uint64_t sameStatePrimitives = 0u;
    uint64_t trianglesDrawn = 0u;
    uint64_t batches = 0u;
    uint64_t renderPasses = 0u;
    uint64_t drawCalls = 0u;
    // Flushes of the queued draws into the device's pending command buffer
    // (Rogue Galaxy: added); the device counts the submissions.
    uint64_t drawFlushes = 0u;
    // Why the queued draws were flushed, counted only when there were any
    // (Rogue Galaxy: added): a host write or transfer settling its pages, a
    // feedback snapshot, a draw into a view aliasing queued draws' pages, a
    // texture read of pages queued draws write (or an imminent eviction),
    // the flush after a recorded feedback draw, a GPU local copy, a present,
    // a texture rewrite. The rest of drawFlushes is everything else.
    uint64_t flushSettle = 0u, flushSnapshot = 0u, flushAlias = 0u, flushTexRead = 0u, flushFeedback = 0u,
             flushLocalCopy = 0u, flushPresent = 0u, flushRewrite = 0u;
    // Uploads that covered whole pages a live target owned, and those pages
    // (released to local memory instead of patched; Rogue Galaxy: added).
    uint64_t coveredReleases = 0u, coveredPages = 0u;

    // Places where GS semantics are approximated rather than reproduced. Each
    // one is a known gap; a nonzero count says how much a scene depends on it.
    uint64_t inexactBlends = 0u;
    // Blends whose source factor exceeds 1.0 because the GS divides alpha by
    // 128, not 255. A fixed-point colour attachment clamps blend factors to
    // [0,1], so these saturate; a floating-point render target would carry them.
    uint64_t saturatedBlendFactors = 0u;
    // Blends weighted by destination alpha. The same /128 scale applies, but
    // the value lives in the render target where the shader cannot reach it,
    // so the factor comes out at 128/255 of what the GS would use.
    uint64_t destinationAlphaFactors = 0u;
    uint64_t partialChannelMasks = 0u;
    uint64_t destinationAlphaTests = 0u;
    uint64_t alphaFailModes = 0u;
    uint64_t disabledColorClamps = 0u;
    uint64_t texturedPrimitives = 0u;
    uint64_t untranslatedTextures = 0u;
    uint64_t secondaryDisplayCircuits = 0u;
    // Alpha-only replays of a strict depth test, widened to pass at equal depth:
    // an equal-depth fragment that failed the colour pass can still write alpha.
    uint64_t widenedAlphaReplays = 0u;
    // Host writes that skipped resolving the pages their rectangle covers, then
    // met a draw or another transfer before their last pixel: the part not yet
    // written comes from local memory, which can be older than the GPU's copy.
    uint64_t interruptedCoveredTransfers = 0u;

    uint64_t colorResolves = 0u;
    uint64_t colorRefreshes = 0u;
    uint64_t resolvedPixels = 0u;
    uint64_t refreshedPixels = 0u;
    uint64_t surfacesCreated = 0u;
    uint64_t pipelinesCreated = 0u;

    uint64_t textureLookups = 0u;
    uint64_t textureHits = 0u;
    uint64_t textureBuilds = 0u;
    uint64_t textureInvalidations = 0u;
    uint64_t textureInvalidationsFromDraw = 0u;
    uint64_t textureInvalidationsFromHostWrite = 0u;
    uint64_t textureEvictions = 0u;
    uint64_t texelsExpanded = 0u;
    uint64_t texturePartialUpdates = 0u;
    uint64_t texelsUpdated = 0u;
    uint64_t texturesFromRenderTargets = 0u;
    // Textured draws that bound a live render target instead of rebuilding a
    // native-resolution copy of it.
    uint64_t texturesFromLiveTargets = 0u;
    // Draws that sample their own target use a snapshot or local-memory copy.
    uint64_t textureFeedbackHazards = 0u;
    // Ordered target snapshots, including padded samples of other live targets.
    uint64_t feedbackCopies = 0u;

    double textureHitRate() const {
        return textureLookups == 0u ? 0.0
                                    : static_cast<double>(textureHits) /
                                          static_cast<double>(textureLookups);
    }

    double averageTrianglesPerDraw() const {
        return drawCalls == 0u ? 0.0
                               : static_cast<double>(trianglesDrawn) /
                                     static_cast<double>(drawCalls);
    }
};

// Where openWindow puts the window (Rogue Galaxy: added): its top left on the
// desktop when placed, centred otherwise, and maximized.
struct SdlGpuWindowPlacement {
    bool placed = false;
    int x = 0;
    int y = 0;
    bool maximized = false;
};

class SdlGpuBackend final : public GSRasterBackend {
public:
    SdlGpuBackend();
    ~SdlGpuBackend() override;

    SdlGpuBackend(const SdlGpuBackend &) = delete;
    SdlGpuBackend &operator=(const SdlGpuBackend &) = delete;

    void Initialize(uint8_t *vram, uint32_t vramSize) override;
    void Reset() override;

    void Submit(const GSPrimitiveBatch &batch) override;
    // Transformed on the GPU, by gs_native.vert (Rogue Galaxy: added).
    void SubmitNativeMesh(const GSNativeMesh &mesh, const GSDrawState &state) override;
    void SubmitMany(const GSPrimitiveBatch *const *batches, size_t count) override;
    void BeginTransfer(const GSTransferCommand &command) override;
    void UploadImage(const uint8_t *data, uint32_t sizeBytes) override;

    void Flush() override;
    void TextureFlush() override;
    void Sync(GSSyncReason reason) override;
    PresentationFrame Present(const GSPresentationRequest &request) override;
    bool SupportsPreparedPresentation() const override { return true; }
    GSPresentationTicket PreparePresentation(const GSPresentationRequest &request) override;
    PresentationFrame DisplayPreparedPresentation(const GSPresentationTicket &ticket) override;
    void CancelPreparedPresentations() noexcept override;

    bool ClearFramebuffer(const GSContext &context, uint32_t rgba) override;
    uint32_t ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes) override;

    uint32_t ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const override;
    void WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value) override;
    void SnapshotVram(std::vector<uint8_t> &out) const override;
    GSTransferSnapshot GetTransferSnapshot() const override;

    // Internal render resolution, as a multiple of the GS's own. Changing it
    // discards every render target, so it is set once at startup in practice.
    void setResolutionScale(uint32_t scale);
    uint32_t resolutionScale() const;
    // A change while the game runs: the next PreparePresentation, which is
    // ordered with the GS work, resolves the targets and rebuilds them.
    void requestResolutionScale(uint32_t scale);
    // The scale in use; any thread, without the backend lock.
    uint32_t activeResolutionScale() const;

    // Opens a window and claims it for the GPU device. Present() then composes
    // straight into the swapchain and reports BackendNative, so the frame never
    // makes a round trip through host memory.
    bool openWindow(const char *title, uint32_t width, uint32_t height, std::string &error,
                    const SdlGpuWindowPlacement &placement = {});
    bool hasWindow() const;
    // Install before execution. The callback reads a thread-safe game counter.
    using CompletedRenderCounter = uint64_t (*)();
    void setCompletedRenderCounter(CompletedRenderCounter counter);
    // The game's Screen Size option for SdlGpuAspect::Auto, read on the
    // presenting thread: 1 for a wide screen (the window's shape), 0 for
    // 4:3, -1 unknown.
    using GameWidescreenQuery = int (*)();
    void setGameWidescreenQuery(GameWidescreenQuery query);
    // The image SdlGpuSides::Image shows; before the window presents (Rogue
    // Galaxy: added).
    void setSideImageLoader(SdlGpuSideImageLoader loader);
    int gameWidescreen() const;
    // The host's larger versions of the 2D art UV sprites sample
    // (sdlgpu_regions.h): a UV sprite drawn from an indexed texture into the
    // frame (the buffer last shown, or the one last copied into it) of an
    // upscaled target is drawn from the host's picture of its region alone,
    // bilinear and clamped to it, once the host has one.
    //
    // Borders: the host can mark the copies of a glyph the game draws as its
    // border -- 4, 8 or 12, black or dark grey (up to 0x20), around the
    // glyph, before it -- by putting their y a sixteenth of a pixel above the
    // whole pixel when the first is a pixel above the glyph, three when it
    // is a pixel above and left. The first is then drawn where the glyph
    // goes, from the host's border version of the region (a margin of
    // kRegionBorderMargin texels about it, its alpha the border's coverage),
    // and the others from the same texels are left out; without one, they
    // are drawn as they are, at their own pixels.
    //
    // Shadows: a copy of a glyph the game draws down and right of it as its
    // shadow, of any colour, its y marked two sixteenths of a pixel above
    // the whole pixel, is drawn where it goes from the host's shadow version
    // (the same margin), which reaches back up toward the glyph: copies a
    // pixel apart, sharp, would show each its own outline.
    //
    // Called on the GS thread; set before the game runs (Rogue Galaxy:
    // added).
    void setRegionReplacements(RegionLookup lookup, RegionPalettes palettes);
    // Any thread (Rogue Galaxy: added).
    void setRegionReplacementsEnabled(bool on);
    // Any thread: the lookup's answers changed; see GsTextureCache::
    // invalidateRegions (Rogue Galaxy: added).
    void invalidateRegionReplacements();
    // Pumps SDL events into the overlay and the pad, and redraws the overlay
    // over the last frame while the game shows none. Returns false when the
    // window wants to close.
    bool pumpEvents();
    // Host keyboard/gamepad state in the runtime's pad encoding, sampled by
    // pumpEvents. Only meaningful when this backend owns the window.
    void hostPadState(uint32_t &held, uint32_t &pressed, uint32_t &sticks) const;

    // The rest belongs to the thread that opened the window. The overlay must
    // outlive its installation; install null before destroying it.
    void setOverlay(SdlGpuOverlay *overlay);
    void setDisplayOptions(const SdlGpuDisplayOptions &options);
    const SdlGpuDisplayOptions &displayOptions() const;
    // False when the window cannot present in that mode; VSync is used instead.
    bool supportsPresentMode(SDL_GPUPresentMode mode) const;
    SDL_Window *window() const;
    SDL_GPUDevice *gpuDevice() const;
    SDL_GPUTextureFormat windowTextureFormat() const;
    SdlPadInput &padInput();
    // Where the game picture was drawn at the last frame, in window pixels.
    SdlGpuRect displayRect() const;
    // Whether the FSR 1 filter would act on the picture as it is now shown
    // (Rogue Galaxy: added).
    SdlGpuFsrState fsrState() const;
    // Delivers the next frame shown: the game picture as displayed, or the
    // whole window with the overlay.
    void requestCapture(bool withOverlay, SdlGpuCaptureCallback done);

    bool deviceReady() const;
    std::string driverName() const;
    // SDL's video backend -- "wayland", "x11", "offscreen".
    std::string videoDriverName() const;
    std::string lastError() const;
    SdlGpuStats stats() const;
    void resetStats();

    // The native pipeline's present (Rogue Galaxy: added;
    // docs/NATIVE-PIPELINE.md step 7d): the renderer's scene into the window,
    // with no GS display registers and nothing on the GS queue. The
    // presenting thread calls it for each frame a flip closed. The frame
    // carries host pixels when screenshots are on.
    PresentationFrame presentNativeFrame();
    // Answers the native pipeline's waiting colour reads (step 7c) from what
    // was drawn last. Any thread; a no-op when none wait.
    void serveNativeReads();

private:
    friend std::unique_ptr<SdlGpuBackend> createSdlGpuBackend(std::string &error);
    GSPresentationTicket prepare(const GSPresentationRequest *request);

    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

// Returns null and fills `error` when SDL cannot create a GPU device.
std::unique_ptr<SdlGpuBackend> createSdlGpuBackend(std::string &error);

} // namespace dq8::gfx
