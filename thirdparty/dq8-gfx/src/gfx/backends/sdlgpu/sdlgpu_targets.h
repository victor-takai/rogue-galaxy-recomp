// Render-target cache: GS framebuffer and Z-buffer regions held as GPU
// textures at the configured resolution scale.
//
// This is the difference between this backend and the compute rasteriser it
// replaces. Nothing draws into swizzled local memory, so a target can be any
// multiple of its GS size; local memory is the backing store, synchronised at
// the boundaries where something outside the GPU can observe it.
//
// GPU page ownership and exact CPU patches can coexist within one surface:
//
//   drawing        -> the texture is ahead of local memory   (gpuDirty)
//   host writes    -> exact native CT32 patches, or a resolved upload region
//   resolve()      -> texture back into local memory
//   refresh()      -> local memory back into the texture
#pragma once

#include "gfx/backends/sdlgpu/sdlgpu_device.h"
#include "gfx/gs/gs_vram.h"

#include <SDL3/SDL_gpu.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dq8::gfx {

class SdlGpuDevice;

// A half-open region of a surface, in GS pixels. Ownership is tracked per
// region rather than per surface: a game that redraws a status bar should not
// cost a 512x448 readback, and one that transfers a movie frame into part of a
// buffer should not invalidate the rest of it.
struct GsRegion {
    uint32_t x0 = 0u;
    uint32_t y0 = 0u;
    uint32_t x1 = 0u;  // exclusive
    uint32_t y1 = 0u;  // exclusive

    bool empty() const { return x1 <= x0 || y1 <= y0; }
    uint32_t width() const { return empty() ? 0u : x1 - x0; }
    uint32_t height() const { return empty() ? 0u : y1 - y0; }

    void clear() { x0 = y0 = x1 = y1 = 0u; }

    void merge(const GsRegion &other) {
        if (other.empty())
            return;
        if (empty()) {
            *this = other;
            return;
        }
        x0 = std::min(x0, other.x0);
        y0 = std::min(y0, other.y0);
        x1 = std::max(x1, other.x1);
        y1 = std::max(y1, other.y1);
    }

    bool intersects(const GsRegion &other) const {
        return !empty() && !other.empty() && x0 < other.x1 && other.x0 < x1 &&
               y0 < other.y1 && other.y0 < y1;
    }

    bool contains(const GsRegion &other) const {
        return other.empty() ||
               (!empty() && x0 <= other.x0 && y0 <= other.y0 && x1 >= other.x1 &&
                y1 >= other.y1);
    }
};

struct GsSurface {
    // Identity, as the FRAME or ZBUF register names it.
    uint32_t base = 0u;         // 256-byte block address (fbp << 5)
    uint32_t bufferWidth = 1u;  // FBW, in units of 64 pixels
    uint32_t psm = 0u;
    bool depth = false;

    // Extent in GS pixels; the texture is this multiplied by `scale`.
    uint32_t width = 0u;
    uint32_t height = 0u;
    uint32_t scale = 1u;

    SDL_GPUTexture *texture = nullptr;

    // Bounds of GPU writes; only ownedPages within these bounds may resolve.
    GsRegion gpuDirty;
    // Where local memory holds pixels the texture does not.
    GsRegion needsUpload;
    // Exact CPU writes may coexist with GPU-owned neighbours in the same page.
    std::vector<GsRegion> cpuPatches;
    // Nothing has been drawn or uploaded yet, so a load can be discarded.
    bool undefined = true;

    GsPageSet pages;
    // Allocated pages can overlap; only this subset may write back to VRAM.
    GsPageSet ownedPages;

    bool ownsAnything() const { return !gpuDirty.empty(); }
    GsRegion wholeRegion() const { return {0u, 0u, width, height}; }
};

// Why a resolve (a render target read back into local memory) happened: the
// backend sets the reason around each call that can provoke one, and the
// stats and DQ8_GFX_TRACE_TARGETS report it (Rogue Galaxy: added).
enum class GsResolveReason : uint8_t {
    Unknown,
    HostWrite,   // a host-to-local transfer into pages the GPU owns
    LocalCopy,   // a local-to-local transfer through local memory
    LocalRead,   // a local-to-host read, or a transfer's source pages
    View,        // an aliased FRAME view importing another's pixels
    Sample,      // a texture read brought a live target up to date
    Texture,     // a texture built from pages a target owns
    Present,     // presentation reading a buffer from local memory
    Sync,        // an explicit sync or snapshot of local memory
    Feedback,    // a feedback snapshot's padding rows
    Scale,       // a change of internal resolution
    Count
};
const char *gsResolveReasonName(GsResolveReason reason);
// DQ8_GFX_TRACE_TARGETS is set, or gsSetTargetTrace turned the trace on: a
// tool can limit it to a window of frames, since it prints a line per
// transfer and slows the game to a crawl (Rogue Galaxy: added).
bool gsTargetTraceEnabled();
void gsSetTargetTrace(bool enabled);

struct GsTargetCacheStats {
    uint64_t surfacesCreated = 0u;
    uint64_t colorResolves = 0u;
    uint64_t colorRefreshes = 0u;
    uint64_t depthDiscards = 0u;
    uint64_t resolvedPixels = 0u;
    uint64_t refreshedPixels = 0u;
    // Surfaces a whole-surface sprite took over without an import
    // (Rogue Galaxy: added).
    uint64_t wholeClaims = 0u;
    // Resolves by reason, and the wall time spent in their readbacks
    // (Rogue Galaxy: added).
    std::array<uint64_t, static_cast<size_t>(GsResolveReason::Count)> resolvesByReason{};
    uint64_t resolveNanos = 0u;
};

class GsTargetCache {
public:
    GsTargetCache(SdlGpuDevice &device, GsVram &vram);
    ~GsTargetCache();

    GsTargetCache(const GsTargetCache &) = delete;
    GsTargetCache &operator=(const GsTargetCache &) = delete;

    void setScale(uint32_t scale);
    uint32_t scale() const { return m_scale; }
    // Effect buffers (Rogue Galaxy: added): colour surfaces at these bases,
    // at most 256 pixels wide, are made at no more than `scale` (0: the
    // scene's). Blurred to begin with, they look the same, and their passes
    // cost the square of the scale. Pages they share with surfaces at the
    // scene's scale go between them as nearest blits.
    void setEffectBuffers(std::vector<uint32_t> bases, uint32_t scale) {
        m_effectBases = std::move(bases);
        m_effectScale = scale;
    }

    void reset();

    // Finds or creates the surface for a FRAME/ZBUF region, growing it to at
    // least the requested extent. Returns null only on a device failure.
    GsSurface *acquire(uint32_t base,
                       uint32_t bufferWidth,
                       uint32_t psm,
                       bool depth,
                       uint32_t minWidth,
                       uint32_t minHeight,
                       std::string &error);

    // Makes local memory current for every surface overlapping `pages`.
    bool resolve(const GsPageSet &pages, std::string &error);
    // Before a host write to `pages`: brings back what the GPU owns there
    // and the write will not replace. `covered` are pages it writes whole.
    bool resolveForHostWrite(const GsPageSet &pages, std::string &error,
                             const GsPageSet &covered = {});
    bool resolveAll(std::string &error);

    // Records that local memory changed under `pages`.
    void invalidate(const GsPageSet &pages, bool preserveOwned = false);
    // A host write that replaces whole pages a colour target owns: those
    // pages go back to local memory (uploaded again before the target's
    // next use, if it has one) instead of being patched block by block.
    // Returns how many pages it released (Rogue Galaxy: added).
    uint32_t releaseCoveredPages(const GsPageSet &covered);

    // Host writes can patch resident native CT32 targets across pitch
    // changes, word by word: CT32 ones, and (Rogue Galaxy: added) the formats
    // that fill whole bytes, CT16, T8 and T4, whose texels land in the
    // target's words through the block layout. gsPatchableHostWrite names
    // them; canPatchHostWrite says whether every target holding GPU pixels
    // in `pages` can take a patch.
    static bool gsPatchableHostWrite(uint32_t psm);
    bool canPatchHostWrite(const GsPageSet &pages) const;
    // The one surface holding GPU-drawn content in these pages, when it is
    // native CT32 and covers all of them; null otherwise. After refresh() its
    // texture is the current content of every one of those pages.
    // `allowScaled`: also an upscaled owner, for readers that sample it at
    // each GS pixel's sample point.
    GsSurface *nativeOwner(const GsPageSet &pages, bool allowScaled = false);
    // Records the completed pixels [firstPixel, endPixel) of a host write of
    // `width`-pixel rows at (x, y) in `psm` as CPU-current words, and queues
    // them as patches on the targets owning their pages.
    void patchHostWrite(const GsPageSet &pages, uint32_t base, uint32_t bw, uint32_t psm,
                        uint32_t x, uint32_t y, uint32_t width,
                        uint32_t firstPixel, uint32_t endPixel);
    // Add only pages whose requested CT32 words are not already CPU-current.
    void markReadPagesCt32(GsPageSet &pages, uint32_t base, uint32_t bw,
                           const GsRegion &region) const;
    // The same for a region of texels in any patchable format: the words
    // they occupy through the block layout (Rogue Galaxy: added). Other
    // formats mark every page of the region.
    void markReadPages(GsPageSet &pages, uint32_t base, uint32_t bw, uint32_t psm,
                       const GsRegion &region) const;

    // True when a colour surface holds pixels for `pages` that local memory
    // does not, i.e. reading those pages would need a resolve first.
    bool ownsAny(const GsPageSet &pages) const;

    // Brings a surface's texture up to date with local memory if needed.
    bool refresh(GsSurface &surface, std::string &error);
    // Lands the host writes patched into the surface (its CPU patches) so a
    // draw can sample it live (Rogue Galaxy: added).
    bool settlePatches(GsSurface &surface, std::string &error) { return applyCpuPatches(surface, error); }
    // Only `region` of it: the CPU patches that touch the region (the rest
    // stay queued), and the whole surface when pixels local memory holds
    // and the texture does not lie there (Rogue Galaxy: added, for a
    // feedback snapshot's padding from a live target, whose queued patches
    // can be megabytes at a scale above 1).
    bool refreshRegion(GsSurface &surface, const GsRegion &region, std::string &error);

    // Copy native CT32 pages across pitches, reinterpret matching CT32/CT16
    // views, or resolve unsupported aliases before this view uses them.
    bool prepareColorView(GsSurface &surface, std::string &error);
    // In place of prepareColorView when the next draw replaces every pixel
    // of the surface: aliased views lose these pages without a copy, and
    // local memory's pending content for them is dropped (Rogue Galaxy:
    // added).
    void claimWhole(GsSurface &surface);

    // Records that a draw wrote this region of a surface.
    void markDrawn(GsSurface &surface, const GsRegion &region);

    // The region of a surface the given pages cover, clipped to it.
    GsRegion regionForPages(const GsSurface &surface, const GsPageSet &pages) const;

    // The colour surface currently holding a display buffer, or null. Lets
    // presentation composite from the scaled texture instead of forcing a
    // resolve down to native resolution.
    GsSurface *findColorSurface(uint32_t base, uint32_t psm) const;
    // An existing depth surface at least this big, or null; never creates or
    // grows one. For a draw that ignores depth but could keep the attachment
    // of the draws around it and share their render pass (Rogue Galaxy:
    // added).
    GsSurface *findDepthSurface(uint32_t base, uint32_t bufferWidth, uint32_t psm, uint32_t minWidth,
                                uint32_t minHeight) const;

    // A colour surface that can stand in for a texture read of this region --
    // same base, format and buffer width, and already holding drawn content.
    // Sampling it directly avoids resolving it to local memory and rebuilding
    // a native-resolution copy, and keeps the detail a scaled target holds.
    // Not const: a surface whose local memory moved on underneath is brought up
    // to date here rather than being rejected, since a partial upload beats
    // resolving it and rebuilding a texture from the result.
    GsSurface *findSampleSource(uint32_t base, uint32_t psm, uint32_t bufferWidth);

    // The first surface findSampleSource would consider, without bringing it
    // up to date. With no CPU patches or pending upload, it is what that
    // returns, and finding it changed nothing.
    GsSurface *sampleCandidate(uint32_t base, uint32_t psm, uint32_t bufferWidth) const;

    // Reads a colour surface back at its full scaled resolution, as RGBA8 rows
    // of width*scale pixels. Used by presentation, which wants the scaled
    // image rather than the native-resolution one resolve() produces.
    bool download(GsSurface &surface, std::vector<uint8_t> &out, std::string &error);

    const GsTargetCacheStats &stats() const { return m_stats; }
    void resetStats() { m_stats = {}; }

    // The reason the next resolves are attributed to (Rogue Galaxy: added).
    void setResolveReason(GsResolveReason reason) { m_reason = reason; }
    GsResolveReason resolveReason() const { return m_reason; }

private:
    bool resolveSurfaces(const std::vector<GsSurface *> &surfaces, std::string &error);
    bool applyCpuPatches(GsSurface &surface, std::string &error);
    bool importColorPages(GsSurface &surface, const std::vector<GsSurface *> &sources,
                           std::string &error);
    bool resolveSurface(GsSurface &surface, std::string &error);
    // By value, not by reference: callers pass surface.gpuDirty, which this
    // clears partway through.
    bool resolveRegion(GsSurface &surface, GsRegion region, std::string &error);
    void writeResolved(GsSurface &surface, GsRegion region, const uint8_t *pixels);
    bool createTexture(GsSurface &surface, std::string &error);
    void destroySurface(GsSurface &surface);
    void recomputePages(GsSurface &surface);

    // Point-samples the scaled texture down to one host RGBA8 image at GS
    // resolution, or the reverse. `staging` is reused across calls.
    bool downloadScaled(GsSurface &surface, const GsRegion &region,
                        std::vector<uint8_t> &out, std::string &error);
    // One CPU patch region at native size through the resolve staging
    // texture and a nearest blit, for surfaces above 1x (Rogue Galaxy: added).
    bool patchNative(GsSurface &surface, const GsRegion &region, std::string &error);
    bool uploadNative(GsSurface &surface, const GsRegion &region,
                      const std::vector<uint8_t> &pixels, std::string &error);

    SdlGpuDevice &m_device;
    GsVram &m_vram;
    uint32_t m_scale = 1u;
    std::vector<uint32_t> m_effectBases;
    uint32_t m_effectScale = 0u;
    std::vector<std::unique_ptr<GsSurface>> m_surfaces;
    std::vector<uint8_t> m_staging;
    // Per 256-byte block: the words local memory holds current since the
    // page's generation, and (Rogue Galaxy: added) the bytes, or nibbles,
    // host writes in other formats have filled of words still incomplete,
    // since one write's payloads can split a word between calls.
    struct CpuWords {
        uint64_t generation = 0u;
        uint64_t mask = 0u;
        uint64_t units[8] = {};
    };
    std::array<uint64_t, kGsPageCount> m_pageGeneration{};
    std::array<CpuWords, kGsVramBytes / 256u> m_cpuWords{};
    // Patch and refresh uploads, recorded into the device's pending command
    // buffer (Rogue Galaxy: added).
    SdlGpuUploadPool m_uploadPool;
    // Resolves wait for their own fence before returning, so the download
    // buffer and the downscale staging texture are idle again by the next
    // one and can be reused rather than created and released every time
    // (Rogue Galaxy: added; it resolves about ten times a frame).
    SDL_GPUTransferBuffer *acquireDownload(uint32_t bytes, std::string &error);
    SDL_GPUTexture *acquireResolveStaging(uint32_t width, uint32_t height, std::string &error);
    SDL_GPUTransferBuffer *m_download = nullptr;
    uint32_t m_downloadCapacity = 0u;
    SDL_GPUTexture *m_resolveStaging = nullptr;
    uint32_t m_resolveStagingWidth = 0u;
    uint32_t m_resolveStagingHeight = 0u;
    // The native-size texture a scaled target's refresh uploads into before
    // the blit up to its scale, kept and grown the same way (Rogue Galaxy:
    // added; one created per refresh was the driver's texture creation, 1.7%
    // of the worker in the streets).
    SDL_GPUTexture *acquireRefreshStaging(uint32_t width, uint32_t height, std::string &error);
    SDL_GPUTexture *m_refreshStaging = nullptr;
    uint32_t m_refreshStagingWidth = 0u;
    uint32_t m_refreshStagingHeight = 0u;
    GsTargetCacheStats m_stats{};
    GsResolveReason m_reason = GsResolveReason::Unknown;
};

} // namespace dq8::gfx
