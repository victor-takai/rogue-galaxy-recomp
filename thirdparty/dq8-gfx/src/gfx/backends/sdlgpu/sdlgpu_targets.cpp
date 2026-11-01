#include "gfx/backends/sdlgpu/sdlgpu_targets.h"

#include "gfx/backends/sdlgpu/sdlgpu_device.h"
#include "runtime/gs/gs_types.h"
#include "runtime/gs/ps2_gs_memory.h"
#include "runtime/gs/ps2_gs_psmct16.h"
#include "runtime/gs/ps2_gs_psmct32.h"
#include "runtime/gs/ps2_gs_psmt4.h"
#include "runtime/gs/ps2_gs_psmt8.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace dq8::gfx {

// Ownership moves between local memory and the GPU on every transfer, draw and
// present, and getting that order wrong shows up as a buffer that is correct
// but blank. DQ8_GFX_TRACE_TARGETS prints the moves.
namespace {
std::atomic<bool> &traceFlag() {
    static std::atomic<bool> flag{[] {
        const char *value = std::getenv("DQ8_GFX_TRACE_TARGETS");
        return value != nullptr && *value != '\0';
    }()};
    return flag;
}
} // namespace

bool gsTargetTraceEnabled() {
    return traceFlag().load(std::memory_order_relaxed);
}

void gsSetTargetTrace(bool enabled) {
    traceFlag().store(enabled, std::memory_order_relaxed);
}

const char *gsResolveReasonName(GsResolveReason reason) {
    switch (reason) {
    case GsResolveReason::HostWrite: return "host-write";
    case GsResolveReason::LocalCopy: return "local-copy";
    case GsResolveReason::LocalRead: return "local-read";
    case GsResolveReason::View: return "view";
    case GsResolveReason::Sample: return "sample";
    case GsResolveReason::Texture: return "texture";
    case GsResolveReason::Present: return "present";
    case GsResolveReason::Sync: return "sync";
    case GsResolveReason::Feedback: return "feedback";
    case GsResolveReason::Scale: return "scale";
    default: return "unknown";
    }
}

namespace {

// Wall time of the readbacks, for the stats (Rogue Galaxy: added).
struct ResolveTimer {
    uint64_t &sink;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    explicit ResolveTimer(uint64_t &sink) : sink(sink) {}
    ~ResolveTimer() {
        sink += static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
    }
};

// A GS buffer has no height register, so surfaces grow to fit what is drawn
// into them. The cap is generous next to the 1024-line maximum the CRTC can
// scan and keeps a nonsense scissor from asking for a huge allocation.
constexpr uint32_t kMaxSurfaceHeight = 2048u;
constexpr uint32_t kMaxSurfaceWidth = 2048u;

constexpr auto kBlockPositions = [] {
    std::array<std::array<uint32_t, 2>, 32> result{};
    for (uint32_t y = 0u; y < 4u; ++y)
        for (uint32_t x = 0u; x < 8u; ++x)
            result[GSPSMCT32::blockTable32[y][x]] = {x * 8u, y * 8u};
    return result;
}();

uint64_t ct32Words(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1) {
    if (x0 == 0u && y0 == 0u && x1 == 8u && y1 == 8u)
        return ~uint64_t(0u);
    uint64_t mask = 0u;
    for (uint32_t y = y0; y < y1; ++y)
        for (uint32_t x = x0; x < x1; ++x)
            mask |= uint64_t(1u) << GSPSMCT32::columnTable32[y][x];
    return mask;
}

// Block geometry of the formats a host write fills whole bytes of, so its
// texels can be accounted as CT32 words of the target holding the block
// (Rogue Galaxy: added). Units are bytes, or nibbles for PSMT4, whose
// address function counts them; a word is four bytes or eight nibbles.
struct BlockGeometry {
    uint32_t width = 0u, height = 0u;
    uint32_t unitsPerWord = 0u;
    uint32_t unitsPerTexel = 0u;
    bool valid() const { return width != 0u; }
};

BlockGeometry blockGeometry(uint32_t psm) {
    switch (psm & 0x3fu) {
    case GS_PSM_CT32: return {8u, 8u, 4u, 4u};
    case GS_PSM_CT16:
    case GS_PSM_CT16S: return {16u, 8u, 4u, 2u};
    case GS_PSM_T8: return {16u, 16u, 4u, 1u};
    case GS_PSM_T4: return {32u, 16u, 8u, 1u};
    default: return {};
    }
}

uint32_t unitAddress(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) {
    switch (psm & 0x3fu) {
    case GS_PSM_CT16: return GSPSMCT16::addrPSMCT16(base, bw, x, y);
    case GS_PSM_CT16S: return GSPSMCT16::addrPSMCT16S(base, bw, x, y);
    case GS_PSM_T8: return GSPSMT8::addrPSMT8(base, bw, x, y);
    case GS_PSM_T4: return GSPSMT4::addrPSMT4(base, bw, x, y);
    default: return GSPSMCT32::addrPSMCT32(base, bw, x, y);
    }
}

// The words of a block whose units (bytes, or nibbles for PSMT4) are all set
// in `units`, and the words with any of them set.
void wordsOfUnits(const uint64_t (&units)[8], uint32_t unitsPerWord, uint64_t &full, uint64_t &touched) {
    const uint64_t wordUnits = (uint64_t(1) << unitsPerWord) - 1u;
    full = touched = 0u;
    for (uint32_t word = 0u; word < 64u; ++word) {
        const uint32_t first = word * unitsPerWord;
        const uint64_t have = (units[first >> 6u] >> (first & 63u)) & wordUnits;
        if (have != 0u)
            touched |= uint64_t(1) << word;
        if (have == wordUnits)
            full |= uint64_t(1) << word;
    }
}

// Visits every 256-byte block a rectangle of texels touches, as
// visit(block, units): null for a block the rectangle covers whole, which
// costs one address computation; the blocks at its edges go texel by texel
// and pass the units (bytes, or nibbles for PSMT4) the rectangle fills.
template <class Visit>
void forEachBlock(uint32_t psm, uint32_t base, uint32_t bw, const GsRegion &rect, Visit &&visit) {
    const BlockGeometry geometry = blockGeometry(psm);
    if (!geometry.valid() || rect.empty())
        return;
    const uint32_t unitShift = geometry.unitsPerWord == 8u ? 9u : 8u;
    const uint32_t unitMask = (1u << unitShift) - 1u;
    for (uint32_t by = rect.y0 - rect.y0 % geometry.height; by < rect.y1; by += geometry.height) {
        for (uint32_t bx = rect.x0 - rect.x0 % geometry.width; bx < rect.x1; bx += geometry.width) {
            const uint32_t block = (unitAddress(psm, base, bw, bx, by) >> unitShift) & 0x3fffu;
            if (bx >= rect.x0 && by >= rect.y0 && bx + geometry.width <= rect.x1 && by + geometry.height <= rect.y1) {
                visit(block, static_cast<const uint64_t (*)[8]>(nullptr));
                continue;
            }
            uint64_t units[8]{};
            const uint32_t y1 = std::min(by + geometry.height, rect.y1), x1 = std::min(bx + geometry.width, rect.x1);
            for (uint32_t y = std::max(by, rect.y0); y < y1; ++y) {
                for (uint32_t x = std::max(bx, rect.x0); x < x1; ++x) {
                    const uint32_t unit = unitAddress(psm, base, bw, x, y) & unitMask;
                    for (uint32_t i = 0u; i < geometry.unitsPerTexel; ++i)
                        units[(unit + i) >> 6u] |= uint64_t(1) << ((unit + i) & 63u);
                }
            }
            visit(block, &units);
        }
    }
}

void addCpuPatch(GsSurface &surface, GsRegion region) {
    if (region.empty()) return;
    // Join rectangles only along a complete edge; bounding disjoint writes
    // would upload stale CPU pixels over their GPU-owned neighbours.
    for (auto it = surface.cpuPatches.rbegin(); it != surface.cpuPatches.rend(); ++it) {
        if (it->contains(region)) return;
        if ((it->x0 == region.x0 && it->x1 == region.x1 &&
             it->y0 <= region.y1 && region.y0 <= it->y1) ||
            (it->y0 == region.y0 && it->y1 == region.y1 &&
             it->x0 <= region.x1 && region.x0 <= it->x1)) {
            it->merge(region);
            return;
        }
    }
    surface.cpuPatches.push_back(region);
}

uint32_t packColorToPsm(uint32_t psm, const uint8_t *rgba) {
    switch (psm & 0x3fu) {
    case GS_PSM_CT24:
        return static_cast<uint32_t>(rgba[0]) |
               (static_cast<uint32_t>(rgba[1]) << 8u) |
               (static_cast<uint32_t>(rgba[2]) << 16u);
    case GS_PSM_CT16:
    case GS_PSM_CT16S:
        // RGBA5551. The GS treats alpha at or above 0x80 as the set bit.
        return static_cast<uint32_t>(((rgba[0] >> 3u) & 0x1fu) |
                                     (((rgba[1] >> 3u) & 0x1fu) << 5u) |
                                     (((rgba[2] >> 3u) & 0x1fu) << 10u) |
                                     ((rgba[3] >= 0x80u) ? 0x8000u : 0u));
    case GS_PSM_CT32:
    default:
        return static_cast<uint32_t>(rgba[0]) |
               (static_cast<uint32_t>(rgba[1]) << 8u) |
               (static_cast<uint32_t>(rgba[2]) << 16u) |
               (static_cast<uint32_t>(rgba[3]) << 24u);
    }
}

void unpackPsmToColor(uint32_t psm, uint32_t value, uint8_t *rgba) {
    switch (psm & 0x3fu) {
    case GS_PSM_CT16:
    case GS_PSM_CT16S: {
        const uint32_t r = value & 0x1fu;
        const uint32_t g = (value >> 5u) & 0x1fu;
        const uint32_t b = (value >> 10u) & 0x1fu;
        rgba[0] = static_cast<uint8_t>((r << 3u) | (r >> 2u));
        rgba[1] = static_cast<uint8_t>((g << 3u) | (g >> 2u));
        rgba[2] = static_cast<uint8_t>((b << 3u) | (b >> 2u));
        rgba[3] = ((value & 0x8000u) != 0u) ? 0x80u : 0u;
        break;
    }
    case GS_PSM_CT24:
        rgba[0] = static_cast<uint8_t>(value);
        rgba[1] = static_cast<uint8_t>(value >> 8u);
        rgba[2] = static_cast<uint8_t>(value >> 16u);
        rgba[3] = 0u;
        break;
    case GS_PSM_CT32:
    default:
        rgba[0] = static_cast<uint8_t>(value);
        rgba[1] = static_cast<uint8_t>(value >> 8u);
        rgba[2] = static_cast<uint8_t>(value >> 16u);
        rgba[3] = static_cast<uint8_t>(value >> 24u);
        break;
    }
}

bool traceEnabled() {
    return gsTargetTraceEnabled();
}

void traceSurface(const char *what, const GsSurface &surface, uint64_t extra = 0u) {
    if (!traceEnabled())
        return;
    std::fprintf(stderr,
                 "[target] %-10s %s base=%05x fbw=%u psm=%02x %ux%u scale=%u "
                 "gpuDirty=%ux%u upload=%ux%u undefined=%d extra=%llu\n",
                 what, surface.depth ? "Z" : "C", surface.base, surface.bufferWidth,
                 surface.psm, surface.width, surface.height, surface.scale,
                 surface.gpuDirty.width(), surface.gpuDirty.height(),
                 surface.needsUpload.width(), surface.needsUpload.height(),
                 surface.undefined ? 1 : 0, static_cast<unsigned long long>(extra));
}

} // namespace

GsTargetCache::GsTargetCache(SdlGpuDevice &device, GsVram &vram)
    : m_device(device), m_vram(vram) {}

GsTargetCache::~GsTargetCache() {
    reset();
}

void GsTargetCache::setScale(uint32_t scale) {
    const uint32_t clamped = std::clamp<uint32_t>(scale, 1u, 8u);
    if (clamped == m_scale)
        return;
    // Every texture is sized in scaled pixels, so none of them survive.
    reset();
    m_scale = clamped;
}

void GsTargetCache::reset() {
    for (auto &surface : m_surfaces)
        destroySurface(*surface);
    m_surfaces.clear();
    m_pageGeneration.fill(0u);
    m_cpuWords.fill({});
    m_uploadPool.release(m_device);
    if (m_download && m_device.valid())
        SDL_ReleaseGPUTransferBuffer(m_device.handle(), m_download);
    m_download = nullptr;
    m_downloadCapacity = 0u;
    if (m_resolveStaging && m_device.valid())
        SDL_ReleaseGPUTexture(m_device.handle(), m_resolveStaging);
    m_resolveStaging = nullptr;
    m_resolveStagingWidth = m_resolveStagingHeight = 0u;
    if (m_refreshStaging && m_device.valid())
        SDL_ReleaseGPUTexture(m_device.handle(), m_refreshStaging);
    m_refreshStaging = nullptr;
    m_refreshStagingWidth = m_refreshStagingHeight = 0u;
}

SDL_GPUTransferBuffer *GsTargetCache::acquireDownload(uint32_t bytes, std::string &error) {
    if (m_download && m_downloadCapacity >= bytes)
        return m_download;
    if (m_download)
        SDL_ReleaseGPUTransferBuffer(m_device.handle(), m_download);
    // Grown in powers of two so a few sizes settle on one buffer.
    uint32_t capacity = 64u * 1024u;
    while (capacity < bytes)
        capacity *= 2u;
    SDL_GPUTransferBufferCreateInfo info{};
    info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    info.size = capacity;
    m_download = SDL_CreateGPUTransferBuffer(m_device.handle(), &info);
    m_downloadCapacity = m_download ? capacity : 0u;
    if (!m_download)
        error = std::string("SDL_CreateGPUTransferBuffer(resolve): ") + SDL_GetError();
    return m_download;
}

SDL_GPUTexture *GsTargetCache::acquireResolveStaging(uint32_t width, uint32_t height, std::string &error) {
    if (m_resolveStaging && m_resolveStagingWidth >= width && m_resolveStagingHeight >= height)
        return m_resolveStaging;
    if (m_resolveStaging)
        SDL_ReleaseGPUTexture(m_device.handle(), m_resolveStaging);
    m_resolveStagingWidth = std::max(width, m_resolveStagingWidth);
    m_resolveStagingHeight = std::max(height, m_resolveStagingHeight);
    SDL_GPUTextureCreateInfo info{};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    info.width = m_resolveStagingWidth;
    info.height = m_resolveStagingHeight;
    info.layer_count_or_depth = 1u;
    info.num_levels = 1u;
    info.sample_count = SDL_GPU_SAMPLECOUNT_1;
    info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    m_resolveStaging = SDL_CreateGPUTexture(m_device.handle(), &info);
    if (!m_resolveStaging) {
        m_resolveStagingWidth = m_resolveStagingHeight = 0u;
        error = std::string("SDL_CreateGPUTexture(resolve staging): ") + SDL_GetError();
    }
    return m_resolveStaging;
}

SDL_GPUTexture *GsTargetCache::acquireRefreshStaging(uint32_t width, uint32_t height, std::string &error) {
    if (m_refreshStaging && m_refreshStagingWidth >= width && m_refreshStagingHeight >= height)
        return m_refreshStaging;
    if (m_refreshStaging)
        SDL_ReleaseGPUTexture(m_device.handle(), m_refreshStaging);
    m_refreshStagingWidth = std::max(width, m_refreshStagingWidth);
    m_refreshStagingHeight = std::max(height, m_refreshStagingHeight);
    SDL_GPUTextureCreateInfo info{};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    info.width = m_refreshStagingWidth;
    info.height = m_refreshStagingHeight;
    info.layer_count_or_depth = 1u;
    info.num_levels = 1u;
    info.sample_count = SDL_GPU_SAMPLECOUNT_1;
    info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
    m_refreshStaging = SDL_CreateGPUTexture(m_device.handle(), &info);
    if (!m_refreshStaging) {
        m_refreshStagingWidth = m_refreshStagingHeight = 0u;
        error = std::string("SDL_CreateGPUTexture(refresh staging): ") + SDL_GetError();
    }
    return m_refreshStaging;
}

void GsTargetCache::destroySurface(GsSurface &surface) {
    if (surface.texture && m_device.valid())
        SDL_ReleaseGPUTexture(m_device.handle(), surface.texture);
    surface.texture = nullptr;
}

void GsTargetCache::recomputePages(GsSurface &surface) {
    surface.pages.reset();
    gsMarkPages(surface.pages, surface.base, surface.bufferWidth, surface.psm,
                surface.width, surface.height);
}

bool GsTargetCache::createTexture(GsSurface &surface, std::string &error) {
    if (!m_device.valid()) {
        error = "SDL GPU device is not available";
        return false;
    }
    if (surface.texture)
        SDL_ReleaseGPUTexture(m_device.handle(), surface.texture);
    surface.texture = nullptr;

    SDL_GPUTextureCreateInfo info{};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.width = surface.width * surface.scale;
    info.height = surface.height * surface.scale;
    info.layer_count_or_depth = 1u;
    info.num_levels = 1u;
    info.sample_count = SDL_GPU_SAMPLECOUNT_1;
    if (surface.depth) {
        info.format = m_device.depthFormat();
        info.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
    } else {
        info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        // SAMPLER so a target can later be sampled as a texture, which is how
        // render-to-texture is handled without a round trip through memory.
        info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    }

    surface.texture = SDL_CreateGPUTexture(m_device.handle(), &info);
    if (!surface.texture) {
        error = std::string("SDL_CreateGPUTexture(render target): ") + SDL_GetError();
        return false;
    }
    surface.undefined = true;
    return true;
}

GsSurface *GsTargetCache::acquire(uint32_t base,
                                  uint32_t bufferWidth,
                                  uint32_t psm,
                                  bool depth,
                                  uint32_t minWidth,
                                  uint32_t minHeight,
                                  std::string &error) {
    bufferWidth = std::max<uint32_t>(bufferWidth, 1u);
    const uint32_t wantWidth =
        std::clamp(std::max(minWidth, bufferWidth * 64u), 1u, kMaxSurfaceWidth);
    const uint32_t wantHeight = std::clamp(minHeight, 1u, kMaxSurfaceHeight);

    for (auto &candidate : m_surfaces) {
        GsSurface &surface = *candidate;
        if (surface.base != base || surface.psm != psm || surface.depth != depth ||
            surface.bufferWidth != bufferWidth)
            continue;

        if (wantWidth <= surface.width && wantHeight <= surface.height)
            return &surface;

        // Growing loses the old contents unless they are preserved. Resolving
        // first and letting the reload path bring them back keeps that simple
        // and correct; growth happens a handful of times per scene.
        if (!surface.depth && !surface.gpuDirty.empty() &&
            !resolveSurface(surface, error))
            return nullptr;
        surface.width = std::max(surface.width, wantWidth);
        surface.height = std::max(surface.height, wantHeight);
        if (!createTexture(surface, error))
            return nullptr;
        recomputePages(surface);
        surface.gpuDirty.clear();
        surface.ownedPages.reset();
        surface.needsUpload = surface.depth ? GsRegion{} : surface.wholeRegion();
        return &surface;
    }

    auto created = std::make_unique<GsSurface>();
    created->base = base;
    created->bufferWidth = bufferWidth;
    created->psm = psm;
    created->depth = depth;
    created->width = wantWidth;
    created->height = wantHeight;
    created->scale = m_scale;
    if (m_effectScale != 0u && !depth && wantWidth <= 256u &&
        std::find(m_effectBases.begin(), m_effectBases.end(), base) != m_effectBases.end())
        created->scale = std::min(m_scale, m_effectScale);
    if (!createTexture(*created, error))
        return nullptr;
    recomputePages(*created);
    // A colour surface starts by adopting whatever local memory already holds,
    // which is how a buffer the game filled with transfers becomes drawable.
    created->needsUpload = depth ? GsRegion{} : created->wholeRegion();
    ++m_stats.surfacesCreated;
    traceSurface("create", *created);
    m_surfaces.push_back(std::move(created));
    return m_surfaces.back().get();
}

GsSurface *GsTargetCache::findColorSurface(uint32_t base, uint32_t psm) const {
    for (const auto &candidate : m_surfaces) {
        if (!candidate->depth && candidate->base == base && candidate->psm == psm)
            return candidate.get();
    }
    return nullptr;
}

namespace {
bool sampleCompatible(const GsSurface &surface, uint32_t base, uint32_t psm, uint32_t bufferWidth) {
    const bool compatible = surface.psm == psm || (surface.psm == GS_PSM_CT32 && psm == GS_PSM_CT24);
    return !surface.depth && surface.base == base && compatible &&
           surface.bufferWidth == std::max<uint32_t>(bufferWidth, 1u) && !surface.undefined;
}
}

GsSurface *GsTargetCache::sampleCandidate(uint32_t base, uint32_t psm, uint32_t bufferWidth) const {
    for (const auto &candidate : m_surfaces)
        if (sampleCompatible(*candidate, base, psm, bufferWidth))
            return candidate.get();
    return nullptr;
}

GsSurface *GsTargetCache::findSampleSource(uint32_t base, uint32_t psm,
                                           uint32_t bufferWidth) {
    for (const auto &candidate : m_surfaces) {
        GsSurface &surface = *candidate;
        if (!sampleCompatible(surface, base, psm, bufferWidth))
            continue;

        // Native CT32 can upload exact CPU patches alongside owned GPU pages.
        // Other mixed views still require a resolve before sampling.
        if (!surface.cpuPatches.empty()) {
            std::string error;
            if (!applyCpuPatches(surface, error)) continue;
        }
        if (!surface.needsUpload.empty()) {
            if (!surface.gpuDirty.empty() &&
                !(surface.psm == GS_PSM_CT32 &&
                  (surface.base & 31u) == 0u && surface.width == surface.bufferWidth * 64u &&
                  surface.height % 32u == 0u))
                continue;
            std::string refreshError;
            if (!prepareColorView(surface, refreshError) || !refresh(surface, refreshError))
                continue;
        }
        return &surface;
    }
    return nullptr;
}

// Which part of a surface a set of local-memory pages covers. Pages tile the
// surface in the PSM's page geometry, so this is arithmetic rather than a
// search; the result is a bounding box, which over-covers a scattered set but
// never under-covers one.
GsRegion GsTargetCache::regionForPages(const GsSurface &surface,
                                       const GsPageSet &pages) const {
    GsRegion region{};
    const GsPageExtent extent = gsPageExtent(surface.psm);
    if (!extent.valid())
        return surface.wholeRegion();

    const uint32_t pixelsPerRow = std::max<uint32_t>(surface.bufferWidth, 1u) * 64u;
    const uint32_t pagesPerRow = std::max<uint32_t>(pixelsPerRow / extent.width, 1u);
    const uint32_t basePage = (surface.base >> 5u) % kGsPageCount;
    const uint32_t pageRows = (surface.height + extent.height - 1u) / extent.height;
    const uint32_t cells = pageRows * pagesPerRow;

    // From the set pages to their cells, rather than a test per cell of the
    // grid: this runs for every batch drawn and every host write (Rogue
    // Galaxy: changed). A grid past 512 pages wraps, so a page may be more
    // than one cell.
    gsForEachPage(pages, [&](uint32_t page) {
        for (uint32_t relative = (page + kGsPageCount - basePage) % kGsPageCount; relative < cells;
             relative += kGsPageCount) {
            const uint32_t row = relative / pagesPerRow;
            const uint32_t column = relative % pagesPerRow;
            GsRegion cell{column * extent.width, row * extent.height,
                          (column + 1u) * extent.width, (row + 1u) * extent.height};
            cell.x1 = std::min(cell.x1, surface.width);
            cell.y1 = std::min(cell.y1, surface.height);
            region.merge(cell);
        }
    });
    return region;
}

void GsTargetCache::markDrawn(GsSurface &surface, const GsRegion &region) {
    GsRegion clipped = region;
    clipped.x1 = std::min(clipped.x1, surface.width);
    clipped.y1 = std::min(clipped.y1, surface.height);
    surface.gpuDirty.merge(clipped);
    surface.undefined = false;
    GsPageSet writtenPages;
    gsMarkPages(writtenPages, surface.base, surface.bufferWidth, surface.psm,
                clipped.width(), clipped.height(), clipped.x0, clipped.y0);
    surface.ownedPages |= writtenPages;
    // A word at a time: this runs for every batch drawn.
    gsForEachPage(writtenPages, [&](uint32_t page) { ++m_pageGeneration[page]; });
    for (auto &candidate : m_surfaces) {
        GsSurface &other = *candidate;
        if (&other != &surface && !other.depth && (other.pages & writtenPages).any())
            other.needsUpload.merge(regionForPages(other, writtenPages));
    }
    // The texture is now ahead of local memory here, so a pending upload of the
    // same region would undo the draw.
    if (surface.needsUpload.empty())
        return;
    if (clipped.contains(surface.needsUpload))
        surface.needsUpload.clear();
}

void GsTargetCache::invalidate(const GsPageSet &pages, bool preserveOwned) {
    for (auto &candidate : m_surfaces) {
        GsSurface &surface = *candidate;
        if ((surface.pages & pages).none())
            continue;
        if (preserveOwned && !surface.depth && !surface.gpuDirty.empty()) {
            surface.needsUpload.merge(regionForPages(surface, pages & ~surface.ownedPages));
            continue;
        }
        if (surface.depth) {
            // Depth is never read back, so a host write to its region simply
            // retires the texture; the next acquire starts clean.
            traceSurface("depth-discard", surface);
            surface.undefined = true;
            surface.gpuDirty.clear();
            surface.ownedPages.reset();
            ++m_stats.depthDiscards;
            continue;
        }

        const GsRegion written = regionForPages(surface, pages);
        if (written.empty())
            continue;

        // Local memory is authoritative for this region now, so the texture
        // must not be resolved back over it. Anything the GPU still owned there
        // was resolved before the write landed (see Impl::settleFor); leaving
        // it marked dirty lets a stale surface overwrite the new contents at
        // the next resolve, which blanked the display buffer.
        surface.needsUpload.merge(written);
        surface.ownedPages &= ~pages;
        if (surface.ownedPages.none() || written.contains(surface.gpuDirty)) {
            surface.gpuDirty.clear();
            surface.ownedPages.reset();
        }
        traceSurface("invalidate", surface);
    }
}

uint32_t GsTargetCache::releaseCoveredPages(const GsPageSet &covered) {
    // The game streams its textures through the pages its scene buffer
    // occupied a frame earlier, 400 uploads a frame; recording a patch per
    // block for a buffer it then clears whole was a tenth of the worker.
    // Nothing of the GPU's pixels survives a page written completely, so
    // the page simply stops being GPU-owned: a later use of the target
    // imports it from local memory, where the bytes already are.
    uint32_t released = 0u;
    for (auto &candidate : m_surfaces) {
        GsSurface &surface = *candidate;
        if (surface.depth || surface.gpuDirty.empty())
            continue;
        const GsPageSet pages = surface.ownedPages & covered;
        if (pages.none())
            continue;
        const GsRegion written = regionForPages(surface, pages);
        released += static_cast<uint32_t>(pages.count());
        surface.needsUpload.merge(written);
        surface.ownedPages &= ~pages;
        if (surface.ownedPages.none() || written.contains(surface.gpuDirty)) {
            surface.gpuDirty.clear();
            surface.ownedPages.reset();
            surface.cpuPatches.clear();
        }
        traceSurface("covered-release", surface, released);
    }
    return released;
}

namespace {
// Whole CT32 pages: each GS page is one 64x32 cell of the surface (a block
// scale times that when upscaled), so host writes can be patched in and
// uploads done page by page. `anyScale` accepts an upscaled one too.
bool nativeCt32(const GsSurface &surface, bool anyScale = false) {
    return surface.psm == GS_PSM_CT32 && (anyScale || surface.scale == 1u) && (surface.base & 31u) == 0u &&
           surface.width == surface.bufferWidth * 64u &&
           surface.pages.count() == surface.bufferWidth * ((surface.height + 31u) / 32u) &&
           !surface.undefined;
}
}

GsSurface *GsTargetCache::nativeOwner(const GsPageSet &pages, bool allowScaled) {
    GsSurface *owner = nullptr;
    for (auto &candidate : m_surfaces) {
        GsSurface &surface = *candidate;
        if (surface.depth || surface.gpuDirty.empty() || (surface.ownedPages & pages).none())
            continue;
        if (owner)
            return nullptr;
        owner = &surface;
    }
    if (!owner || !nativeCt32(*owner, allowScaled) || (pages & ~owner->pages).any())
        return nullptr;
    return owner;
}

bool GsTargetCache::canPatchHostWrite(const GsPageSet &pages) const {
    bool owned = false;
    for (const auto &candidate : m_surfaces) {
        const GsSurface &surface = *candidate;
        if (surface.depth || surface.gpuDirty.empty() || (surface.pages & pages).none())
            continue;
        owned = true;
        // Pending uploads are fine: a patch only lands in pages the GPU owns,
        // and a whole-page surface refreshes only the pages it does not.
        if (!nativeCt32(surface, true) || surface.height % 32u != 0u)
            return false;
    }
    return owned;
}

bool GsTargetCache::gsPatchableHostWrite(uint32_t psm) {
    return blockGeometry(psm).valid();
}

void GsTargetCache::patchHostWrite(const GsPageSet &pages, uint32_t base, uint32_t bw, uint32_t psm,
                                  uint32_t x, uint32_t y, uint32_t width,
                                  uint32_t firstPixel, uint32_t endPixel) {
    // The targets holding GPU pixels in each page the write touches, found
    // once: a texture's write visits hundreds of cells, and looking through
    // every surface at each was a tenth of the worker (Rogue Galaxy: added).
    struct PageOwners {
        uint32_t page;
        GsSurface *owners[4];
        uint32_t count;
    };
    std::vector<PageOwners> ownersByPage;
    ownersByPage.reserve(pages.count());
    // The table's slot of each page, so a cell finds its page's owners
    // without a search: a full-screen CT32 write visits thousands of cells
    // over a hundred pages (Rogue Galaxy: changed).
    constexpr uint16_t kNoSlot = 0xffffu;
    std::array<uint16_t, kGsPageCount> slotOfPage;
    slotOfPage.fill(kNoSlot);
    gsForEachPage(pages, [&](uint32_t page) {
        PageOwners entry{page, {}, 0u};
        for (auto &candidate : m_surfaces) {
            GsSurface &surface = *candidate;
            if (surface.depth || surface.gpuDirty.empty() || !surface.ownedPages.test(page))
                continue;
            if (entry.count < 4u)
                entry.owners[entry.count] = &surface;
            ++entry.count;
        }
        slotOfPage[page] = static_cast<uint16_t>(ownersByPage.size());
        ownersByPage.push_back(entry);
    });
    // Each owner of a page; every surface is looked through only for a page
    // outside `pages` or with more owners than the table keeps.
    auto forOwners = [&](uint32_t page, auto &&visit) {
        if (const uint16_t slot = slotOfPage[page]; slot != kNoSlot && ownersByPage[slot].count <= 4u) {
            const PageOwners &entry = ownersByPage[slot];
            for (uint32_t i = 0u; i < entry.count; ++i)
                visit(*entry.owners[i]);
            return;
        }
        for (auto &candidate : m_surfaces) {
            GsSurface &surface = *candidate;
            if (!surface.depth && !surface.gpuDirty.empty() && surface.ownedPages.test(page))
                visit(surface);
        }
    };
    // The words a block's patch covers, as regions of the CT32 surface:
    // the whole 8x8 cell, or runs of its pixels (Rogue Galaxy: added).
    auto addBlockPatch = [&](GsSurface &surface, uint32_t page, uint32_t localBlock, uint64_t words) {
        const uint32_t relativePage = (page + kGsPageCount - (surface.base >> 5u)) % kGsPageCount;
        const auto position = kBlockPositions[localBlock];
        const uint32_t dx = (relativePage % surface.bufferWidth) * 64u + position[0];
        const uint32_t dy = (relativePage / surface.bufferWidth) * 32u + position[1];
        if (words == ~uint64_t(0)) {
            addCpuPatch(surface, {dx, dy, std::min(dx + 8u, surface.width), std::min(dy + 8u, surface.height)});
            return;
        }
        for (uint32_t row = 0u; row < 8u; ++row) {
            for (uint32_t column = 0u; column < 8u;) {
                if (!((words >> GSPSMCT32::columnTable32[row][column]) & 1u)) {
                    ++column;
                    continue;
                }
                uint32_t end = column + 1u;
                while (end < 8u && ((words >> GSPSMCT32::columnTable32[row][end]) & 1u))
                    ++end;
                addCpuPatch(surface, {dx + column, dy + row, std::min(dx + end, surface.width),
                                      std::min(dy + row + 1u, surface.height)});
                column = end;
            }
        }
    };
    // Any patchable format other than CT32: its texels' bytes, through the
    // format's block layout, as the CT32 words they fill.
    const uint32_t unitsPerWord = blockGeometry(psm).unitsPerWord;
    auto patchRectBlocks = [&](const GsRegion &rect) {
        // Whole blocks gather per page and go out as runs of 8x8 cells
        // along each cell row: the format's block order scatters consecutive
        // blocks over the page, and one patch per block made hundreds of
        // uploads of a page.
        std::vector<std::pair<uint32_t, uint32_t>> wholeByPage;
        forEachBlock(psm, base, bw, rect, [&](uint32_t block, const uint64_t (*units)[8]) {
            const uint32_t page = block >> 5u;
            auto &words = m_cpuWords[block];
            if (words.generation != m_pageGeneration[page])
                words = {m_pageGeneration[page], 0u, {}};
            uint64_t full = ~uint64_t(0);
            if (units) {
                // Completed across this write's payloads, so a word split
                // between two of them still counts once the second lands.
                for (uint32_t i = 0u; i < 8u; ++i)
                    words.units[i] |= (*units)[i];
                uint64_t touched = 0u;
                wordsOfUnits(words.units, unitsPerWord, full, touched);
                full &= ~words.mask;
            }
            words.mask |= full;
            if (traceEnabled())
                std::fprintf(stderr, "[target] patch-block block=%04x page=%03x gen=%llu full=%016llx mask=%016llx\n",
                             block, page, static_cast<unsigned long long>(words.generation),
                             static_cast<unsigned long long>(full), static_cast<unsigned long long>(words.mask));
            if (full == 0u)
                return;
            if (!units) {
                if (wholeByPage.empty() || wholeByPage.back().first != page)
                    wholeByPage.emplace_back(page, 0u);
                wholeByPage.back().second |= 1u << (block & 31u);
                return;
            }
            forOwners(page, [&](GsSurface &surface) { addBlockPatch(surface, page, block & 31u, full); });
        });
        for (const auto &[page, blocks] : wholeByPage) {
            forOwners(page, [&, page = page, blocks = blocks](GsSurface &surface) {
                const uint32_t relativePage = (page + kGsPageCount - (surface.base >> 5u)) % kGsPageCount;
                const uint32_t px = (relativePage % surface.bufferWidth) * 64u;
                const uint32_t py = (relativePage / surface.bufferWidth) * 32u;
                for (uint32_t row = 0u; row < 4u; ++row) {
                    for (uint32_t column = 0u; column < 8u;) {
                        if (!((blocks >> GSPSMCT32::blockTable32[row][column]) & 1u)) {
                            ++column;
                            continue;
                        }
                        uint32_t end = column + 1u;
                        while (end < 8u && ((blocks >> GSPSMCT32::blockTable32[row][end]) & 1u))
                            ++end;
                        addCpuPatch(surface, {px + column * 8u, py + row * 8u, std::min(px + end * 8u, surface.width),
                                              std::min(py + row * 8u + 8u, surface.height)});
                        column = end;
                    }
                }
            });
        }
    };
    auto patchRect = [&](const GsRegion &rect) {
        if ((psm & 0x3fu) != GS_PSM_CT32) {
            patchRectBlocks(rect);
            return;
        }
        for (uint32_t ty = rect.y0; ty < rect.y1;) {
            const uint32_t ey = std::min(rect.y1, (ty | 7u) + 1u);
            for (uint32_t tx = rect.x0; tx < rect.x1;) {
                const uint32_t ex = std::min(rect.x1, (tx | 7u) + 1u);
                const uint32_t block = (GSPSMCT32::addrPSMCT32(base, bw, tx, ty) >> 8u) & 0x3fffu;
                const uint32_t page = block >> 5u;
                auto &words = m_cpuWords[block];
                if (words.generation != m_pageGeneration[page])
                    words = {m_pageGeneration[page], 0u};
                words.mask |= ct32Words(tx & 7u, ty & 7u, ((ex - 1u) & 7u) + 1u, ((ey - 1u) & 7u) + 1u);
                forOwners(page, [&](GsSurface &surface) {
                    const uint32_t relativePage = (page + kGsPageCount - (surface.base >> 5u)) % kGsPageCount;
                    const auto position = kBlockPositions[block & 31u];
                    const uint32_t dx = (relativePage % surface.bufferWidth) * 64u + position[0] + (tx & 7u);
                    const uint32_t dy = (relativePage / surface.bufferWidth) * 32u + position[1] + (ty & 7u);
                    addCpuPatch(surface, {dx, dy, std::min(dx + ex - tx, surface.width),
                                          std::min(dy + ey - ty, surface.height)});
                });
                tx = ex;
            }
            ty = ey;
        }
    };
    // A byte payload may finish midway through a row or even a packed pixel.
    // Only completed pixels reported by the transfer engine are authoritative.
    if (firstPixel < endPixel && firstPixel % width != 0u) {
        const uint32_t count = std::min(endPixel - firstPixel, width - firstPixel % width);
        patchRect({x + firstPixel % width, y + firstPixel / width,
                   x + firstPixel % width + count, y + firstPixel / width + 1u});
        firstPixel += count;
    }
    const uint32_t rows = (endPixel - firstPixel) / width;
    if (rows != 0u) {
        patchRect({x, y + firstPixel / width, x + width, y + firstPixel / width + rows});
        firstPixel += rows * width;
    }
    if (firstPixel < endPixel)
        patchRect({x, y + firstPixel / width, x + endPixel - firstPixel, y + firstPixel / width + 1u});
    invalidate(pages, true);
}

void GsTargetCache::markReadPagesCt32(GsPageSet &pages, uint32_t base, uint32_t bw,
                                     const GsRegion &region) const {
    for (uint32_t y = region.y0; y < region.y1;) {
        const uint32_t ey = std::min(region.y1, (y | 7u) + 1u);
        for (uint32_t x = region.x0; x < region.x1;) {
            const uint32_t ex = std::min(region.x1, (x | 7u) + 1u);
            const uint32_t block = (GSPSMCT32::addrPSMCT32(base, bw, x, y) >> 8u) & 0x3fffu;
            const uint32_t page = block >> 5u;
            const auto &words = m_cpuWords[block];
            const uint64_t wanted = ct32Words(x & 7u, y & 7u, ((ex - 1u) & 7u) + 1u, ((ey - 1u) & 7u) + 1u);
            if (words.generation != m_pageGeneration[page] || (words.mask & wanted) != wanted)
                pages.set(page);
            x = ex;
        }
        y = ey;
    }
}

void GsTargetCache::markReadPages(GsPageSet &pages, uint32_t base, uint32_t bw, uint32_t psm,
                                 const GsRegion &region) const {
    if ((psm & 0x3fu) == GS_PSM_CT32) {
        markReadPagesCt32(pages, base, bw, region);
        return;
    }
    if (!gsPatchableHostWrite(psm)) {
        if (!region.empty())
            gsMarkPages(pages, base, bw, psm, region.width(), region.height(), region.x0, region.y0);
        return;
    }
    const uint32_t unitsPerWord = blockGeometry(psm).unitsPerWord;
    forEachBlock(psm, base, bw, region, [&](uint32_t block, const uint64_t (*units)[8]) {
        const uint32_t page = block >> 5u;
        const auto &words = m_cpuWords[block];
        uint64_t touched = ~uint64_t(0);
        if (units) {
            uint64_t full = 0u;
            wordsOfUnits(*units, unitsPerWord, full, touched);
        }
        const bool stale = words.generation != m_pageGeneration[page] || (words.mask & touched) != touched;
        if (traceEnabled() && stale)
            std::fprintf(stderr, "[target] read-stale block=%04x page=%03x gen=%llu/%llu touched=%016llx mask=%016llx\n",
                         block, page, static_cast<unsigned long long>(words.generation),
                         static_cast<unsigned long long>(m_pageGeneration[page]),
                         static_cast<unsigned long long>(touched), static_cast<unsigned long long>(words.mask));
        if (stale)
            pages.set(page);
    });
}

bool GsTargetCache::ownsAny(const GsPageSet &pages) const {
    for (const auto &candidate : m_surfaces) {
        if (candidate->depth || candidate->gpuDirty.empty())
            continue;
        if ((candidate->ownedPages & pages).any())
            return true;
    }
    return false;
}

bool GsTargetCache::resolve(const GsPageSet &pages, std::string &error) {
    std::vector<GsSurface *> surfaces;
    for (auto &candidate : m_surfaces) {
        GsSurface &surface = *candidate;
        if (surface.depth || surface.gpuDirty.empty())
            continue;
        if ((surface.ownedPages & pages).none())
            continue;
        // Only the part the caller is about to read, intersected with the part
        // the GPU actually owns.
        const GsRegion wanted = regionForPages(surface, pages);
        if (!wanted.intersects(surface.gpuDirty))
            continue;
        surfaces.push_back(&surface);
    }
    return resolveSurfaces(surfaces, error);
}

bool GsTargetCache::resolveAll(std::string &error) {
    std::vector<GsSurface *> surfaces;
    for (auto &candidate : m_surfaces) {
        GsSurface &surface = *candidate;
        if (surface.depth || surface.gpuDirty.empty())
            continue;
        surfaces.push_back(&surface);
    }
    return resolveSurfaces(surfaces, error);
}

bool GsTargetCache::resolveForHostWrite(const GsPageSet &pages, std::string &error,
                                        const GsPageSet &covered) {
    std::vector<GsSurface *> surfaces;
    for (auto &candidate : m_surfaces) {
        auto &surface = *candidate;
        if (surface.depth || surface.gpuDirty.empty() || (surface.pages & pages).none())
            continue;
        // Whole CT32 pages refresh page by page, never over a page the GPU
        // still owns, so only the owned pages the write lands in part of
        // need their contents in local memory: it replaces the rest of
        // what it touches. At a scale above 1 a resolve is a full stall and
        // a downsample, and DQ8 reuses target memory for texture uploads.
        if (nativeCt32(surface, true) && surface.height % 32u == 0u) {
            const GsPageSet partial = pages & ~covered & surface.ownedPages;
            if (partial.none())
                continue;
            GsRegion region = regionForPages(surface, partial);
            region.x0 = std::max(region.x0, surface.gpuDirty.x0);
            region.y0 = std::max(region.y0, surface.gpuDirty.y0);
            region.x1 = std::min(region.x1, surface.gpuDirty.x1);
            region.y1 = std::min(region.y1, surface.gpuDirty.y1);
            if (!region.empty() && !resolveRegion(surface, region, error))
                return false;
            // Local memory now holds them, and the write is about to change
            // them there: hand them over, so the next write to the same pages
            // does not stall on another readback.
            surface.ownedPages &= ~partial;
            surface.needsUpload.merge(regionForPages(surface, partial));
            if (surface.ownedPages.none()) {
                surface.gpuDirty.clear();
                surface.ownedPages.reset();
            }
            continue;
        }
        // Include earlier CPU writes: two disjoint invalidations can merge
        // across a GPU-owned middle even when neither alone touches it.
        GsRegion upload = surface.needsUpload;
        upload.merge(regionForPages(surface, pages));
        if (upload.intersects(surface.gpuDirty))
            surfaces.push_back(&surface);
    }
    return resolveSurfaces(surfaces, error);
}

bool GsTargetCache::resolveSurfaces(const std::vector<GsSurface *> &surfaces,
                                    std::string &error) {
    for (auto *surface : surfaces)
        if (!applyCpuPatches(*surface, error)) return false;
    uint64_t bytes = 0u;
    bool canBatch = surfaces.size() > 1u;
    for (const auto *surface : surfaces) {
        canBatch = canBatch && surface->scale == 1u;
        bytes += (uint64_t(surface->gpuDirty.width()) * surface->gpuDirty.height() * 4u + 255u) & ~uint64_t(255u);
    }
    if (!canBatch || bytes > 16u * 1024u * 1024u) {
        for (auto *surface : surfaces)
            if (!resolveSurface(*surface, error)) return false;
        return true;
    }
    if (traceEnabled())
        std::fprintf(stderr, "[target] resolve-batch surfaces=%zu bytes=%llu\n",
                     surfaces.size(), static_cast<unsigned long long>(bytes));

    ResolveTimer timer(m_stats.resolveNanos);
    SDL_GPUDevice *device = m_device.handle();
    SDL_GPUTransferBuffer *download = acquireDownload(static_cast<uint32_t>(bytes), error);
    if (!download)
        return false;
    // Kept for the next resolve: this one waits for its fence before returning.
    auto release = []() {};
    SDL_GPUCommandBuffer *commands = m_device.pendingCommands(error, "resolve to local memory");
    if (!commands) {
        release();
        return false;
    }
    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(commands);
    if (!copy) {
        error = std::string("SDL_BeginGPUCopyPass(resolve batch): ") + SDL_GetError();
        m_device.cancelPending();
        release();
        return false;
    }
    uint32_t offset = 0u;
    for (auto *surface : surfaces) {
        const auto region = surface->gpuDirty;
        SDL_GPUTextureRegion source{};
        source.texture = surface->texture;
        source.x = region.x0;
        source.y = region.y0;
        source.w = region.width();
        source.h = region.height();
        source.d = 1u;
        SDL_GPUTextureTransferInfo destination{};
        destination.transfer_buffer = download;
        destination.offset = offset;
        destination.pixels_per_row = source.w;
        destination.rows_per_layer = source.h;
        SDL_DownloadFromGPUTexture(copy, &source, &destination);
        offset += (source.w * source.h * 4u + 255u) & ~255u;
    }
    SDL_EndGPUCopyPass(copy);
    // The patches recorded ahead of the download go out with it.
    if (!m_device.submitPendingAndWait(error)) {
        release();
        return false;
    }
    const auto *mapped = static_cast<const uint8_t *>(SDL_MapGPUTransferBuffer(device, download, false));
    if (!mapped) {
        error = std::string("SDL_MapGPUTransferBuffer(resolve batch): ") + SDL_GetError();
        release();
        return false;
    }
    // The textures are independent; preserve the original VRAM writeback order.
    offset = 0u;
    for (auto *surface : surfaces) {
        const auto region = surface->gpuDirty;
        writeResolved(*surface, region, mapped + offset);
        offset += (region.width() * region.height() * 4u + 255u) & ~255u;
    }
    SDL_UnmapGPUTransferBuffer(device, download);
    release();
    return true;
}

bool GsTargetCache::resolveSurface(GsSurface &surface, std::string &error) {
    if (surface.gpuDirty.empty())
        return true;
    return resolveRegion(surface, surface.gpuDirty, error);
}

bool GsTargetCache::resolveRegion(GsSurface &surface, GsRegion region,
                                  std::string &error) {
    if (region.empty())
        return true;
    if (!applyCpuPatches(surface, error))
        return false;
    {
        ResolveTimer timer(m_stats.resolveNanos);
        if (!downloadScaled(surface, region, m_staging, error))
            return false;
    }
    writeResolved(surface, region, m_staging.data());
    return true;
}

void GsTargetCache::writeResolved(GsSurface &surface, GsRegion region,
                                  const uint8_t *pixels) {
    if (traceEnabled()) {
        uint64_t nonBlack = 0u;
        const size_t bytes = size_t(region.width()) * region.height() * 4u;
        for (size_t i = 0u; i < bytes; i += 4u) {
            if (pixels[i] != 0u || pixels[i + 1u] != 0u || pixels[i + 2u] != 0u)
                ++nonBlack;
        }
        char what[32];
        std::snprintf(what, sizeof(what), "resolve/%s", gsResolveReasonName(m_reason));
        std::fprintf(stderr, "[target] resolve region=%ux%u@%u,%u\n", region.width(), region.height(),
                     region.x0, region.y0);
        traceSurface(what, surface, nonBlack);
    }
    ++m_stats.resolvesByReason[static_cast<size_t>(m_reason)];

    const uint32_t regionWidth = region.width();
    GsPageSet resolvedPages;
    gsMarkPages(resolvedPages, surface.base, surface.bufferWidth, surface.psm,
                region.width(), region.height(), region.x0, region.y0);
    if (surface.psm == GS_PSM_CT32 && (resolvedPages & ~surface.ownedPages).none()) {
        GSMem::WriteRunCT32(m_vram.data(), surface.base, surface.bufferWidth,
                            region.x0, region.x1, region.x0, region.y0,
                            pixels, regionWidth * region.height());
    } else {
        for (uint32_t y = region.y0; y < region.y1; ++y) {
            const uint8_t *row =
                pixels + static_cast<size_t>(y - region.y0) * regionWidth * 4u;
            for (uint32_t x = region.x0; x < region.x1; ++x) {
                if (surface.psm == GS_PSM_CT32) {
                    const uint32_t page = (GSPSMCT32::addrPSMCT32(surface.base,
                        surface.bufferWidth, x, y) >> 13u) % kGsPageCount;
                    if (!surface.ownedPages.test(page)) continue;
                }
                m_vram.write(surface.psm, surface.base, surface.bufferWidth, x, y,
                             packColorToPsm(surface.psm, row + (x - region.x0) * 4u));
            }
        }
    }

    if (region.contains(surface.gpuDirty)) {
        surface.gpuDirty.clear();
        surface.ownedPages.reset();
    }
    ++m_stats.colorResolves;
    m_stats.resolvedPixels += static_cast<uint64_t>(regionWidth) * region.height();
}

GsSurface *GsTargetCache::findDepthSurface(uint32_t base, uint32_t bufferWidth, uint32_t psm,
                                           uint32_t minWidth, uint32_t minHeight) const {
    bufferWidth = std::max<uint32_t>(bufferWidth, 1u);
    for (const auto &candidate : m_surfaces) {
        GsSurface &surface = *candidate;
        if (surface.depth && surface.base == base && surface.psm == psm &&
            surface.bufferWidth == bufferWidth && surface.width >= std::max(minWidth, bufferWidth * 64u) &&
            surface.height >= minHeight)
            return &surface;
    }
    return nullptr;
}

void GsTargetCache::claimWhole(GsSurface &surface) {
    for (auto &candidate : m_surfaces) {
        GsSurface &other = *candidate;
        if (&other == &surface || other.depth || other.gpuDirty.empty() ||
            (other.ownedPages & surface.pages).none())
            continue;
        // As a page import leaves the source, minus the copy.
        other.ownedPages &= ~surface.pages;
        if (other.ownedPages.none()) {
            other.gpuDirty.clear();
        } else {
            const auto remaining = regionForPages(other, other.ownedPages);
            other.gpuDirty.x0 = std::max(other.gpuDirty.x0, remaining.x0);
            other.gpuDirty.y0 = std::max(other.gpuDirty.y0, remaining.y0);
            other.gpuDirty.x1 = std::min(other.gpuDirty.x1, remaining.x1);
            other.gpuDirty.y1 = std::min(other.gpuDirty.y1, remaining.y1);
        }
        traceSurface("page-claim", other, (surface.pages & other.pages).count());
    }
    surface.needsUpload.clear();
    surface.cpuPatches.clear();
    surface.undefined = false;
    ++m_stats.wholeClaims;
}

bool GsTargetCache::refresh(GsSurface &surface, std::string &error) {
    if (!applyCpuPatches(surface, error))
        return false;
    if (surface.needsUpload.empty())
        return true;
    const GsRegion region = surface.needsUpload;
    if (surface.depth) {
        surface.needsUpload.clear();
        return true;
    }

    if (surface.psm == GS_PSM_CT32 &&
        (surface.base & 31u) == 0u && surface.width == surface.bufferWidth * 64u &&
        surface.ownedPages.any()) {
        for (uint32_t row = 0u; row < (surface.height + 31u) / 32u; ++row) {
            for (uint32_t column = 0u; column < surface.bufferWidth; ++column) {
                const uint32_t page = ((surface.base >> 5u) + row * surface.bufferWidth + column) % kGsPageCount;
                if (surface.ownedPages.test(page)) continue;
                addCpuPatch(surface, {std::max(column * 64u, region.x0),
                    std::max(row * 32u, region.y0),
                    std::min((column + 1u) * 64u, region.x1),
                    std::min((row + 1u) * 32u, region.y1)});
            }
        }
        if (!applyCpuPatches(surface, error)) return false;
        surface.needsUpload.clear();
        surface.undefined = false;
        return true;
    }

    const uint32_t regionWidth = region.width();
    const uint32_t regionHeight = region.height();
    m_staging.resize(static_cast<size_t>(regionWidth) * regionHeight * 4u);
    for (uint32_t y = region.y0; y < region.y1; ++y) {
        uint8_t *row =
            m_staging.data() + static_cast<size_t>(y - region.y0) * regionWidth * 4u;
        if (surface.psm == GS_PSM_CT32 || surface.psm == GS_PSM_CT24) {
            GSMem::ReadRowCT32(m_vram.data(), surface.base, surface.bufferWidth,
                               region.x0, y, regionWidth, row);
            if (surface.psm == GS_PSM_CT24) {
                for (uint32_t x = 0; x < regionWidth; ++x)
                    row[x * 4u + 3u] = 0u;
            }
            continue;
        }
        for (uint32_t x = region.x0; x < region.x1; ++x) {
            const uint32_t value =
                m_vram.read(surface.psm, surface.base, surface.bufferWidth, x, y);
            unpackPsmToColor(surface.psm, value, row + (x - region.x0) * 4u);
        }
    }

    if (traceEnabled())
        traceSurface("refresh", surface, static_cast<uint64_t>(regionWidth) * regionHeight);

    if (!uploadNative(surface, region, m_staging, error))
        return false;
    surface.needsUpload.clear();
    surface.undefined = false;
    ++m_stats.colorRefreshes;
    m_stats.refreshedPixels += static_cast<uint64_t>(regionWidth) * regionHeight;
    return true;
}

bool GsTargetCache::refreshRegion(GsSurface &surface, const GsRegion &region, std::string &error) {
    if (region.empty())
        return true;
    // Whole-page CT32 targets refresh non-owned pages as patches, so a region
    // of owned pages owes nothing to needsUpload; anything else does.
    bool owned = nativeCt32(surface, true);
    if (owned) {
        for (uint32_t row = region.y0 / 32u; row <= (region.y1 - 1u) / 32u && owned; ++row)
            for (uint32_t column = region.x0 / 64u; column <= (region.x1 - 1u) / 64u; ++column) {
                const uint32_t page = ((surface.base >> 5u) + row * surface.bufferWidth + column) % kGsPageCount;
                if (!surface.ownedPages.test(page)) {
                    owned = false;
                    break;
                }
            }
    }
    if (!owned && !surface.needsUpload.empty() && surface.needsUpload.intersects(region))
        return refresh(surface, error);
    if (surface.cpuPatches.empty())
        return true;
    std::vector<GsRegion> keep, apply;
    for (const auto &patch : surface.cpuPatches)
        (patch.intersects(region) ? apply : keep).push_back(patch);
    if (apply.empty())
        return true;
    surface.cpuPatches.swap(apply);
    const bool applied = applyCpuPatches(surface, error);
    surface.cpuPatches = std::move(keep);
    return applied;
}

bool GsTargetCache::prepareColorView(GsSurface &surface, std::string &error) {
    std::vector<GsSurface *> surfaces;
    for (auto &candidate : m_surfaces) {
        GsSurface &other = *candidate;
        if (&other == &surface || other.depth || other.gpuDirty.empty() ||
            (other.ownedPages & surface.pages).none())
            continue;
        surfaces.push_back(&other);
    }
    if (surfaces.size() == 1u && surface.gpuDirty.empty()) {
        GsSurface &source = *surfaces.front();
        const bool to16 = source.psm == GS_PSM_CT32 && surface.psm == GS_PSM_CT16;
        const bool to32 = source.psm == GS_PSM_CT16 && surface.psm == GS_PSM_CT32;
        const uint32_t height32 = to16 ? source.height : surface.height;
        const uint32_t height16 = to16 ? surface.height : source.height;
        if ((to16 || to32) && source.scale == surface.scale &&
            source.base == surface.base && (source.base & 31u) == 0u &&
            source.bufferWidth == surface.bufferWidth && source.width == surface.width &&
            source.width == source.bufferWidth * 64u && height32 % 32u == 0u &&
            height16 == height32 * 2u && source.pages == surface.pages &&
            source.pages.count() == source.bufferWidth * (height32 / 32u) &&
            source.needsUpload.empty() && !source.undefined) {
            // The complete texture is authoritative: GPU-owned pixels plus
            // unchanged pixels imported from VRAM. Reinterpret those same GS
            // bytes and move ownership, so later resolves cannot write an old
            // view over the new format's draws. Mixed owners retain readback.
            if (!applyCpuPatches(source, error) ||
                !m_device.reinterpretColor(source.texture, surface.texture,
                                           surface.width, surface.height, to16, surface.scale, error))
                return false;
            source.gpuDirty.clear();
            source.ownedPages.reset();
            surface.needsUpload.clear();
            markDrawn(surface, surface.wholeRegion());
            traceSurface("reinterpret", surface, source.psm);
            return true;
        }
    }
    // Whole CT32 pages: GPU page copies between them keep an upscaled
    // target's detail, where a resolve would bring it down to native and
    // stall for the readback. Between surfaces of different scales the pages
    // go as nearest blits (Rogue Galaxy: changed).
    auto nativePages = [](const GsSurface &s) {
        return s.psm == GS_PSM_CT32 && (s.base & 31u) == 0u &&
               s.width == s.bufferWidth * 64u && s.height % 32u == 0u &&
               s.pages.count() == s.bufferWidth * (s.height / 32u);
    };
    if (!surfaces.empty() && nativePages(surface) &&
        std::all_of(surfaces.begin(), surfaces.end(), [&](const GsSurface *s) {
            return nativePages(*s) && !s->undefined;
        })) {
        return importColorPages(surface, surfaces, error);
    }
    if (traceEnabled() && !surfaces.empty())
        traceSurface("view-resolve", surface, surfaces.size());
    if (!resolveSurfaces(surfaces, error))
        return false;
    for (auto *other : surfaces) {
        surface.needsUpload.merge(regionForPages(surface, other->pages));
    }
    return true;
}

bool GsTargetCache::importColorPages(GsSurface &surface,
                                    const std::vector<GsSurface *> &sources,
                                    std::string &error) {
    if (!applyCpuPatches(surface, error)) return false;
    GsPageSet gpuPages = surface.ownedPages;
    for (auto *source : sources) {
        if (!applyCpuPatches(*source, error)) return false;
        gpuPages |= source->ownedPages;
    }
    // Import CPU pages around the GPU-owned pages, then fill those holes with
    // GPU copies. A bounding upload would silently overwrite resident data.
    for (uint32_t row = 0u; row < surface.height / 32u; ++row) {
        for (uint32_t column = 0u; column < surface.bufferWidth; ++column) {
            const uint32_t page = ((surface.base >> 5u) + row * surface.bufferWidth + column) % kGsPageCount;
            if (gpuPages.test(page)) continue;
            GsRegion region{column * 64u, row * 32u, (column + 1u) * 64u, (row + 1u) * 32u};
            region.x0 = std::max(region.x0, surface.needsUpload.x0);
            region.y0 = std::max(region.y0, surface.needsUpload.y0);
            region.x1 = std::min(region.x1, surface.needsUpload.x1);
            region.y1 = std::min(region.y1, surface.needsUpload.y1);
            addCpuPatch(surface, region);
        }
    }
    if (!applyCpuPatches(surface, error)) return false;
    auto fail = [&](const char *operation) {
        error = std::string(operation) + "(target page import): " + SDL_GetError();
        return false;
    };
    SDL_GPUCommandBuffer *commands = m_device.pendingCommands(error, "import pages from other targets");
    if (!commands) return false;
    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(commands);
    if (!copy) {
        m_device.cancelPending();
        return fail("SDL_BeginGPUCopyPass");
    }
    struct Blit {
        const GsSurface *source;
        uint32_t srcColumn, srcRow, dstColumn, dstRow, pages, rows;
    };
    std::vector<Blit> blits;
    for (auto *source : sources) {
        const GsPageSet shared = source->ownedPages & surface.pages;
        const uint32_t scale = std::max(surface.scale, 1u);
        // Consecutive pages along one row of both surfaces go as one copy:
        // the call per page, with SDL's layout transitions around each, was
        // most of the import's cost (Rogue Galaxy: added). The same run on
        // the next row of both surfaces joins it, so a block of whole rows is
        // one copy too, not one a row (Rogue Galaxy: added).
        struct Block {
            uint32_t srcColumn, srcRow, dstColumn, dstRow, pages, rows;
        };
        std::vector<Block> blocks;
        for (uint32_t page = 0u; page < kGsPageCount; ++page) {
            if (!shared.test(page)) continue;
            const uint32_t srcPage = (page + kGsPageCount - (source->base >> 5u)) % kGsPageCount;
            const uint32_t dstPage = (page + kGsPageCount - (surface.base >> 5u)) % kGsPageCount;
            const uint32_t srcColumn = srcPage % source->bufferWidth;
            const uint32_t dstColumn = dstPage % surface.bufferWidth;
            uint32_t run = 1u;
            while (page + run < kGsPageCount && shared.test(page + run) &&
                   srcColumn + run < source->bufferWidth && dstColumn + run < surface.bufferWidth &&
                   srcPage + run < kGsPageCount && dstPage + run < kGsPageCount)
                ++run;
            const uint32_t srcRow = srcPage / source->bufferWidth;
            const uint32_t dstRow = dstPage / surface.bufferWidth;
            Block *last = blocks.empty() ? nullptr : &blocks.back();
            if (last && last->srcColumn == srcColumn && last->dstColumn == dstColumn && last->pages == run &&
                last->srcRow + last->rows == srcRow && last->dstRow + last->rows == dstRow)
                ++last->rows;
            else
                blocks.push_back({srcColumn, srcRow, dstColumn, dstRow, run, 1u});
            page += run - 1u;
        }
        for (const Block &block : blocks) {
            if (std::max(source->scale, 1u) != scale) {
                blits.push_back({source, block.srcColumn, block.srcRow, block.dstColumn, block.dstRow, block.pages,
                                 block.rows});
                continue;
            }
            SDL_GPUTextureLocation from{};
            from.texture = source->texture;
            from.x = block.srcColumn * 64u * scale;
            from.y = block.srcRow * 32u * scale;
            SDL_GPUTextureLocation to{};
            to.texture = surface.texture;
            to.x = block.dstColumn * 64u * scale;
            to.y = block.dstRow * 32u * scale;
            SDL_CopyGPUTextureToTexture(copy, &from, &to, 64u * block.pages * scale, 32u * block.rows * scale, 1u,
                                        false);
        }
    }
    SDL_EndGPUCopyPass(copy);
    // The pages of a surface at another scale, scaled: nearest, so an
    // upscale repeats each pixel and a downscale keeps one of its samples,
    // as a resolve and an upload would (Rogue Galaxy: added).
    for (const Blit &block : blits) {
        const uint32_t from = std::max(block.source->scale, 1u), to = std::max(surface.scale, 1u);
        SDL_GPUBlitInfo blit{};
        blit.source.texture = block.source->texture;
        blit.source.x = block.srcColumn * 64u * from;
        blit.source.y = block.srcRow * 32u * from;
        blit.source.w = 64u * block.pages * from;
        blit.source.h = 32u * block.rows * from;
        blit.destination.texture = surface.texture;
        blit.destination.x = block.dstColumn * 64u * to;
        blit.destination.y = block.dstRow * 32u * to;
        blit.destination.w = 64u * block.pages * to;
        blit.destination.h = 32u * block.rows * to;
        blit.load_op = SDL_GPU_LOADOP_LOAD;
        blit.filter = SDL_GPU_FILTER_NEAREST;
        SDL_BlitGPUTexture(commands, &blit);
    }
    for (auto *source : sources) {
        const GsPageSet shared = source->ownedPages & surface.pages;
        surface.ownedPages |= shared;
        surface.gpuDirty.merge(regionForPages(surface, shared));
        source->ownedPages &= ~shared;
        if (source->ownedPages.none()) {
            source->gpuDirty.clear();
        } else {
            const auto remaining = regionForPages(*source, source->ownedPages);
            source->gpuDirty.x0 = std::max(source->gpuDirty.x0, remaining.x0);
            source->gpuDirty.y0 = std::max(source->gpuDirty.y0, remaining.y0);
            source->gpuDirty.x1 = std::min(source->gpuDirty.x1, remaining.x1);
            source->gpuDirty.y1 = std::min(source->gpuDirty.y1, remaining.y1);
        }
        traceSurface("page-import", surface, shared.count());
    }
    surface.needsUpload.clear();
    surface.undefined = false;
    return true;
}

bool GsTargetCache::download(GsSurface &surface,
                             std::vector<uint8_t> &out,
                             std::string &error) {
    if (!applyCpuPatches(surface, error) || !m_device.submitPending(error))
        return false;
    return m_device.downloadTexture(surface.texture, surface.width * surface.scale,
                                    surface.height * surface.scale, out, error);
}

// Reads one region of a surface back at GS resolution, as tightly packed RGBA8
// rows of region.width() pixels.
//
// At scale 1 the region comes straight out of the surface. Above it, the region
// is point-sampled down through a staging texture first, because local memory
// has nowhere to put the extra samples.
bool GsTargetCache::downloadScaled(GsSurface &surface,
                                   const GsRegion &region,
                                   std::vector<uint8_t> &out,
                                   std::string &error) {
    SDL_GPUDevice *device = m_device.handle();
    if (!device || !surface.texture || region.empty()) {
        error = "render target has no texture to resolve";
        return false;
    }

    const uint32_t width = region.width();
    const uint32_t height = region.height();
    const size_t bytes = static_cast<size_t>(width) * height * 4u;
    out.resize(bytes);

    SDL_GPUTexture *native = surface.texture;
    SDL_GPUTexture *staging = nullptr;
    uint32_t sourceX = region.x0;
    uint32_t sourceY = region.y0;

    if (surface.scale != 1u) {
        staging = acquireResolveStaging(width, height, error);
        if (!staging)
            return false;
        native = staging;
        sourceX = 0u;
        sourceY = 0u;
    }

    SDL_GPUTransferBuffer *download = acquireDownload(static_cast<uint32_t>(bytes), error);
    if (!download)
        return false;

    // Both are kept for the next resolve.
    auto release = []() {};

    SDL_GPUCommandBuffer *commands = m_device.pendingCommands(error, "download scaled");
    if (!commands) {
        release();
        return false;
    }

    if (staging) {
        SDL_GPUBlitInfo blit{};
        blit.source.texture = surface.texture;
        blit.source.x = region.x0 * surface.scale;
        blit.source.y = region.y0 * surface.scale;
        blit.source.w = width * surface.scale;
        blit.source.h = height * surface.scale;
        blit.destination.texture = staging;
        blit.destination.w = width;
        blit.destination.h = height;
        blit.load_op = SDL_GPU_LOADOP_DONT_CARE;
        blit.filter = SDL_GPU_FILTER_NEAREST;
        SDL_BlitGPUTexture(commands, &blit);
    }

    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(commands);
    if (!copy) {
        error = std::string("SDL_BeginGPUCopyPass(resolve): ") + SDL_GetError();
        m_device.cancelPending();
        release();
        return false;
    }
    SDL_GPUTextureRegion source{};
    source.texture = native;
    source.x = sourceX;
    source.y = sourceY;
    source.w = width;
    source.h = height;
    source.d = 1u;
    SDL_GPUTextureTransferInfo destination{};
    destination.transfer_buffer = download;
    destination.pixels_per_row = width;
    destination.rows_per_layer = height;
    SDL_DownloadFromGPUTexture(copy, &source, &destination);
    SDL_EndGPUCopyPass(copy);

    if (!m_device.submitPendingAndWait(error)) {
        release();
        return false;
    }

    void *mapped = SDL_MapGPUTransferBuffer(device, download, false);
    if (!mapped) {
        error = std::string("SDL_MapGPUTransferBuffer(resolve): ") + SDL_GetError();
        release();
        return false;
    }
    std::memcpy(out.data(), mapped, bytes);
    SDL_UnmapGPUTransferBuffer(device, download);
    release();
    return true;
}

// Writes one region of GS-resolution pixels into a surface, point-expanding it
// when the surface is scaled.
// A CPU patch region of this many GS pixels or more goes to the GPU at
// native size (patchNative) when the surface is upscaled; smaller ones, the
// 8x8 cells of a texture upload, are cheaper expanded on the CPU than with a
// copy pass and a blit each (Rogue Galaxy: added).
constexpr uint32_t kNativeBlitPixels = 1024u;

bool GsTargetCache::patchNative(GsSurface &surface, const GsRegion &region, std::string &error) {
    SDL_GPUDevice *device = m_device.handle();
    const uint32_t width = region.width();
    const uint32_t height = region.height();
    const uint32_t scale = std::max(surface.scale, 1u);
    SDL_GPUTexture *staging = acquireResolveStaging(width, height, error);
    if (!staging) return false;
    auto fail = [&](const char *operation) {
        error = std::string(operation) + "(native patch): " + SDL_GetError();
        return false;
    };
    const uint32_t bytes = (width * height * 4u + 255u) & ~255u;
    SDL_GPUTransferBuffer *upload = m_uploadPool.take(m_device, bytes, error);
    if (!upload) return false;
    auto *mapped = static_cast<uint8_t *>(SDL_MapGPUTransferBuffer(device, upload, false));
    if (!mapped) return fail("SDL_MapGPUTransferBuffer");
    for (uint32_t y = region.y0; y < region.y1; ++y)
        GSMem::ReadRowCT32(m_vram.data(), surface.base, surface.bufferWidth, region.x0, y, width,
                           mapped + size_t(y - region.y0) * width * 4u);
    SDL_UnmapGPUTransferBuffer(device, upload);
    SDL_GPUCommandBuffer *commands = m_device.pendingCommands(error, "patch target (native)");
    if (!commands) return false;
    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(commands);
    if (!copy) {
        m_device.cancelPending();
        return fail("SDL_BeginGPUCopyPass");
    }
    SDL_GPUTextureTransferInfo source{};
    source.transfer_buffer = upload;
    source.pixels_per_row = width;
    source.rows_per_layer = height;
    SDL_GPUTextureRegion destination{};
    destination.texture = staging;
    destination.w = width;
    destination.h = height;
    destination.d = 1u;
    SDL_UploadToGPUTexture(copy, &source, &destination, false);
    SDL_EndGPUCopyPass(copy);
    // Nearest at an integer scale replicates each native pixel exactly, as
    // the CPU expansion did; the rest of the target is kept.
    SDL_GPUBlitInfo blit{};
    blit.source.texture = staging;
    blit.source.w = width;
    blit.source.h = height;
    blit.destination.texture = surface.texture;
    blit.destination.x = region.x0 * scale;
    blit.destination.y = region.y0 * scale;
    blit.destination.w = width * scale;
    blit.destination.h = height * scale;
    blit.load_op = SDL_GPU_LOADOP_LOAD;
    blit.filter = SDL_GPU_FILTER_NEAREST;
    SDL_BlitGPUTexture(commands, &blit);
    m_uploadPool.used(upload, m_device.pendingSerial());
    traceSurface("native-patch", surface, uint64_t(width) * height);
    m_stats.refreshedPixels += uint64_t(width) * height;
    return true;
}

bool GsTargetCache::applyCpuPatches(GsSurface &surface, std::string &error) {
    if (surface.cpuPatches.empty()) return true;
    // Patches are in GS pixels; an upscaled surface takes each as a
    // scale x scale block, like an upload of the whole surface would.
    const uint32_t scale = std::max(surface.scale, 1u);
    if (scale > 1u) {
        // The large regions (page imports, refreshes) at native size: their
        // scale x scale expansion into the write-combined upload buffer was
        // 24 MB a frame at 3x in Rogue Galaxy's streets.
        std::vector<GsRegion> small;
        bool native = false;
        for (const auto &region : surface.cpuPatches) {
            if (region.width() * region.height() >= kNativeBlitPixels) {
                if (!patchNative(surface, region, error)) return false;
                native = true;
            } else {
                small.push_back(region);
            }
        }
        if (native) {
            surface.cpuPatches.swap(small);
            if (surface.cpuPatches.empty()) {
                ++m_stats.colorRefreshes;
                return true;
            }
        }
    }
    const auto patchBytes = [scale](const GsRegion &region) {
        return (region.width() * scale * region.height() * scale * 4u + 255u) & ~255u;
    };
    uint32_t bytes = 0u;
    for (const auto &region : surface.cpuPatches)
        bytes += patchBytes(region);
    SDL_GPUDevice *device = m_device.handle();
    auto fail = [&](const char *operation) {
        error = std::string(operation) + "(target CPU patches): " + SDL_GetError();
        return false;
    };
    SDL_GPUTransferBuffer *upload = m_uploadPool.take(m_device, bytes, error);
    if (!upload) return false;
    auto *mapped = static_cast<uint8_t *>(SDL_MapGPUTransferBuffer(device, upload, false));
    if (!mapped) return fail("SDL_MapGPUTransferBuffer");
    uint32_t offset = 0u;
    std::vector<uint32_t> row, wide;
    for (const auto &region : surface.cpuPatches) {
        const uint32_t width = region.width();
        for (uint32_t y = region.y0; y < region.y1; ++y) {
            uint8_t *out = mapped + offset + size_t(y - region.y0) * scale * width * scale * 4u;
            if (scale == 1u) {
                GSMem::ReadRowCT32(m_vram.data(), surface.base, surface.bufferWidth,
                                   region.x0, y, width, out);
                continue;
            }
            row.resize(width);
            GSMem::ReadRowCT32(m_vram.data(), surface.base, surface.bufferWidth,
                               region.x0, y, width, reinterpret_cast<uint8_t *>(row.data()));
            // Expanded aside and only written out: upload buffers are
            // write-combined, so copying a row back out of one is very slow.
            wide.resize(size_t(width) * scale);
            for (uint32_t x = 0u; x < width; ++x)
                for (uint32_t sx = 0u; sx < scale; ++sx)
                    wide[size_t(x) * scale + sx] = row[x];
            for (uint32_t sy = 0u; sy < scale; ++sy)
                std::memcpy(out + size_t(sy) * width * scale * 4u, wide.data(), size_t(width) * scale * 4u);
        }
        offset += patchBytes(region);
    }
    SDL_UnmapGPUTransferBuffer(device, upload);
    SDL_GPUCommandBuffer *commands = m_device.pendingCommands(error, "patch target (cpu)");
    if (!commands) return false;
    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(commands);
    if (!copy) {
        m_device.cancelPending();
        return fail("SDL_BeginGPUCopyPass");
    }
    offset = 0u;
    uint64_t pixels = 0u;
    for (const auto &region : surface.cpuPatches) {
        SDL_GPUTextureTransferInfo source{};
        source.transfer_buffer = upload;
        source.offset = offset;
        source.pixels_per_row = region.width() * scale;
        source.rows_per_layer = region.height() * scale;
        SDL_GPUTextureRegion destination{};
        destination.texture = surface.texture;
        destination.x = region.x0 * scale;
        destination.y = region.y0 * scale;
        destination.w = region.width() * scale;
        destination.h = region.height() * scale;
        destination.d = 1u;
        SDL_UploadToGPUTexture(copy, &source, &destination, false);
        pixels += uint64_t(region.width()) * region.height();
        offset += patchBytes(region);
    }
    SDL_EndGPUCopyPass(copy);
    m_uploadPool.used(upload, m_device.pendingSerial());
    traceSurface("cpu-patch", surface, pixels);
    surface.cpuPatches.clear();
    ++m_stats.colorRefreshes;
    m_stats.refreshedPixels += pixels;
    return true;
}

bool GsTargetCache::uploadNative(GsSurface &surface,
                                 const GsRegion &region,
                                 const std::vector<uint8_t> &pixels,
                                 std::string &error) {
    SDL_GPUDevice *device = m_device.handle();
    if (!device || !surface.texture || region.empty())
        return true;

    const uint32_t width = region.width();
    const uint32_t height = region.height();
    const size_t bytes = static_cast<size_t>(width) * height * 4u;
    if (pixels.size() < bytes) {
        error = "render target upload has too few pixels";
        return false;
    }

    // At a scale the pixels land in the kept native-size staging texture,
    // at its corner, and a blit takes them up to the target's scale.
    SDL_GPUTexture *native = nullptr;
    if (surface.scale != 1u) {
        native = acquireRefreshStaging(width, height, error);
        if (!native)
            return false;
    }

    SDL_GPUTransferBuffer *upload = m_uploadPool.take(m_device, bytes, error);
    if (!upload) {
        return false;
    }
    void *mapped = SDL_MapGPUTransferBuffer(device, upload, false);
    if (!mapped) {
        error = std::string("SDL_MapGPUTransferBuffer(refresh): ") + SDL_GetError();
        return false;
    }
    std::memcpy(mapped, pixels.data(), bytes);
    SDL_UnmapGPUTransferBuffer(device, upload);

    SDL_GPUCommandBuffer *commands = m_device.pendingCommands(error, "upload local memory to target");
    if (!commands) {
        return false;
    }
    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(commands);
    if (!copy) {
        error = std::string("SDL_BeginGPUCopyPass(refresh): ") + SDL_GetError();
        m_device.cancelPending();
        return false;
    }
    SDL_GPUTextureTransferInfo source{};
    source.transfer_buffer = upload;
    source.pixels_per_row = width;
    source.rows_per_layer = height;
    SDL_GPUTextureRegion destination{};
    destination.texture = native ? native : surface.texture;
    destination.x = native ? 0u : region.x0;
    destination.y = native ? 0u : region.y0;
    destination.w = width;
    destination.h = height;
    destination.d = 1u;
    // The staging texture is cycled: what an earlier refresh still has to
    // blit from it keeps its own storage, and nothing outside the corner is
    // read. A target is not: its other pixels must stay.
    SDL_UploadToGPUTexture(copy, &source, &destination, native != nullptr);
    SDL_EndGPUCopyPass(copy);

    if (native) {
        SDL_GPUBlitInfo blit{};
        blit.source.texture = native;
        blit.source.w = width;
        blit.source.h = height;
        blit.destination.texture = surface.texture;
        blit.destination.x = region.x0 * surface.scale;
        blit.destination.y = region.y0 * surface.scale;
        blit.destination.w = width * surface.scale;
        blit.destination.h = height * surface.scale;
        // A partial refresh must preserve the other target pixels.
        blit.load_op = SDL_GPU_LOADOP_LOAD;
        blit.filter = SDL_GPU_FILTER_NEAREST;
        SDL_BlitGPUTexture(commands, &blit);
    }

    m_uploadPool.used(upload, m_device.pendingSerial());
    return true;
}

} // namespace dq8::gfx
