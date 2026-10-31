#include "gfx/backends/sdlgpu/sdlgpu_backend.h"

#include "gfx/backends/sdlgpu/sdlgpu_device.h"
#include "gfx/backends/sdlgpu/sdlgpu_input.h"
#include "gfx/backends/sdlgpu/sdlgpu_targets.h"
#include "gfx/backends/sdlgpu/sdlgpu_textures.h"
#include "gfx/gs/gs_state.h"
#include "gfx/gs/gs_transfer.h"
#include "gfx/gs/gs_vram.h"
#include "runtime/gs/ps2_gs_psmct16.h"
#include "runtime/gs/ps2_gs_psmct32.h"
#include "runtime/gs/ps2_gs_psmt8.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <vector>

// Debug accounting published by ps2xRuntime's GIF frontend (gs_frontend.cpp).
// Declared here rather than in a header so measuring costs no rebuild of the
// recompiled corpus, which includes those headers.
extern "C++" {
extern std::atomic<bool> g_gsFrontendTiming;
extern std::atomic<uint64_t> g_gsFrontendPacketNanos;
extern std::atomic<uint64_t> g_gsFrontendPacketCount;
extern std::atomic<uint64_t> g_gsUploadNativeNanos;
extern std::atomic<uint64_t> g_gsUploadNativeCount;
}

// The movie and EE counters live in ps2xRuntime's MPEG stub and EE scheduler.
// The GS dump tools, which have neither, define them in guest_stubs.cpp: a
// weak definition here would do for GCC and Clang, but MSVC has none, and a
// second one breaks the game's link there.
extern "C++" {
extern std::atomic<uint64_t> g_mpegGetPictureNanos;
extern std::atomic<uint64_t> g_mpegGetPictureCount;
extern std::atomic<uint64_t> g_mpegWriteFrameNanos;
extern std::atomic<uint64_t> g_mpegWriteFrameCount;
extern std::atomic<uint64_t> g_mpegDemuxNanos;
extern std::atomic<uint64_t> g_mpegDemuxCount;
extern std::atomic<uint64_t> g_mpegDemuxRefusedCount;
extern std::atomic<uint64_t> g_mpegPendingEsPeakBytes;
extern std::atomic<uint64_t> g_eeGuestDispatchCount;
extern std::atomic<uint64_t> g_eeTransferThrowCount;
extern std::atomic<uint64_t> g_eeTransferSuspendCount;
extern std::atomic<uint64_t> g_eeRunLoopIterations;
extern std::atomic<uint64_t> g_eeRunLoopResumeCount;
extern std::atomic<uint64_t> g_eeProcessPendingEventsCount;
extern std::atomic<uint64_t> g_eeEnterGuestCount;
extern std::atomic<uint32_t> g_mpegDemuxThreadId;
extern std::atomic<uint32_t> g_mpegGetPictureThreadId;
}

namespace dq8::gfx {

namespace {

// DQ8_GFX_SCREENSHOT_DIR makes the runtime latch CPU-composed pixels, so the
// paths that would skip or discard that compose have to stand down.
bool gsScreenshotEnabled() {
    static const bool enabled = [] {
        const char *value = std::getenv("DQ8_GFX_SCREENSHOT_DIR");
        return value != nullptr && *value != '\0';
    }();
    return enabled;
}

// DQ8_GFX_STATS_EVERY=N prints a counter delta every N presented frames; zero
// when unset. The timing behind those numbers is only taken when it is set.
std::function<std::string()> &statsExtraLine() {
    static std::function<std::string()> provider;
    return provider;
}

std::function<void(double)> &presentObserver() {
    static std::function<void(double)> observer;
    return observer;
}

uint32_t statsInterval() {
    static const uint32_t interval = [] {
        const char *value = std::getenv("DQ8_GFX_STATS_EVERY");
        const uint32_t every = value != nullptr ? static_cast<uint32_t>(std::strtoul(value, nullptr, 10)) : 0u;
        g_gsFrontendTiming.store(every != 0u, std::memory_order_relaxed);
        return every;
    }();
    return interval;
}

// SDL3 GPU normalises every backend onto a Y-up clip space, so a GS window
// coordinate (Y down from the top-left) is flipped on the way out of the
// vertex shader. Verified by sdlgpu_raster_tests' orientation case, which
// fails loudly rather than mirroring the image if this is wrong.
constexpr float kClipYDirection = -1.0f;

// GS samples at integer window coordinates; the host samples at n + 0.5.
// Without this shift, bilinear copies mix adjacent texels and corrupt the
// channel-shuffle passes which reinterpret framebuffer pixels as CT16.
constexpr float kSampleOffset = 0.5f;

uint32_t depthMaximumForPsm(uint32_t psm) {
    switch (psm & 0x3fu) {
    case GS_PSM_Z16:
    case GS_PSM_Z16S:
        return 0xffffu;
    case GS_PSM_Z24:
        return 0xffffffu;
    case GS_PSM_Z32:
    default:
        return 0xffffffffu;
    }
}

// FBMSK masks individual bits; a host colour write mask is per channel. A
// channel is dropped only when every one of its bits is masked, and a partial
// mask is counted so a scene that depends on one is visible in the stats.
uint8_t colorWriteMaskFromFbmsk(uint32_t fbmsk, bool &partial) {
    uint8_t mask = 0u;
    partial = false;
    const uint32_t channels[4] = {fbmsk & 0xffu, (fbmsk >> 8u) & 0xffu,
                                  (fbmsk >> 16u) & 0xffu, (fbmsk >> 24u) & 0xffu};
    const uint8_t flags[4] = {SDL_GPU_COLORCOMPONENT_R, SDL_GPU_COLORCOMPONENT_G,
                              SDL_GPU_COLORCOMPONENT_B, SDL_GPU_COLORCOMPONENT_A};
    for (int i = 0; i < 4; ++i) {
        if (channels[i] == 0xffu)
            continue;
        if (channels[i] != 0u)
            partial = true;
        mask |= flags[i];
    }
    return mask;
}

struct DrawBatch {
    GsSurface *color = nullptr;
    GsSurface *depth = nullptr;
    GsPipelineKey pipeline{};
    GsVertexUniforms vertexUniforms{};
    GsFragmentUniforms fragmentUniforms{};
    SDL_GPUTexture *texture = nullptr;
    bool usesBlendConstant = false;
    float blendConstant = 0.0f;
    SDL_Rect scissor{};
    GsRegion written{};
    GsRegion overwritten{};
    GsPageSet writtenPages{};
    uint32_t firstVertex = 0u;
    uint32_t vertexCount = 0u;
    // A live target sampled directly. Its texture is bound when the batch is
    // flushed, after any queued draws into it.
    GsSurface *source = nullptr;
    // Samples its own colour target through a snapshot, which the flush takes
    // before this batch when `snapshot` is set and otherwise shares with the
    // batch before it.
    bool feedback = false;
    bool snapshot = false;
    // Reads its colour target's pixels through a snapshot the flush takes
    // before it, bound as the second texture, and blends in the shader
    // (Rogue Galaxy: added). Always with a snapshot of its own.
    bool destination = false;

    // Everything except the vertex range; two consecutive primitives with the
    // same answer here can share one draw call. One that needs a snapshot of
    // its own starts a new batch.
    bool sameStateAs(const DrawBatch &other) const {
        return !other.snapshot && feedback == other.feedback && destination == other.destination &&
               source == other.source &&
               color == other.color && depth == other.depth &&
               pipeline == other.pipeline && texture == other.texture &&
               usesBlendConstant == other.usesBlendConstant &&
               blendConstant == other.blendConstant &&
               scissor.x == other.scissor.x && scissor.y == other.scissor.y &&
               scissor.w == other.scissor.w && scissor.h == other.scissor.h &&
               std::memcmp(&vertexUniforms, &other.vertexUniforms,
                           sizeof(vertexUniforms)) == 0 &&
               std::memcmp(&fragmentUniforms, &other.fragmentUniforms,
                           sizeof(fragmentUniforms)) == 0;
    }
};

GsRegion primitiveBounds(const GSPrimitiveBatch &batch) {
    const GSContext &context = batch.state.context;
    const GsRegion scissor{context.scissor.x0, context.scissor.y0,
                          context.scissor.x1 + 1u, context.scissor.y1 + 1u};
    const float offsetX = static_cast<float>(context.xyoffset.ofx) / 16.0f;
    const float offsetY = static_cast<float>(context.xyoffset.ofy) / 16.0f;
    float minX = batch.vertices[0].x - offsetX, maxX = minX;
    float minY = batch.vertices[0].y - offsetY, maxY = minY;
    for (uint8_t i = 0u; i < batch.vertexCount; ++i) {
        const float x = batch.vertices[i].x - offsetX;
        const float y = batch.vertices[i].y - offsetY;
        if (!std::isfinite(x) || !std::isfinite(y))
            return scissor;
        minX = std::min(minX, x);
        minY = std::min(minY, y);
        maxX = std::max(maxX, x);
        maxY = std::max(maxY, y);
    }
    // Include point/line expansion and fractional edges at every render scale.
    auto clipped = [](float value, uint32_t low, uint32_t high) {
        return static_cast<uint32_t>(std::clamp(value, static_cast<float>(low),
                                               static_cast<float>(high)));
    };
    return {clipped(std::floor(minX) - 1.0f, scissor.x0, scissor.x1),
            clipped(std::floor(minY) - 1.0f, scissor.y0, scissor.y1),
            clipped(std::ceil(maxX) + 1.0f, scissor.x0, scissor.x1),
            clipped(std::ceil(maxY) + 1.0f, scissor.y0, scissor.y1)};
}

GsRegion opaqueSpriteBounds(const GSPrimitiveBatch &batch, const GsSurface &surface,
                            const GsTestState &test, bool anyScale = false) {
    const auto &state = batch.state;
    const auto &context = state.context;
    // `anyScale`: for a sprite that covers the whole surface, where the scale
    // cannot matter; the per-region use keeps to scale 1.
    if ((surface.scale != 1u && !anyScale) || surface.psm != GS_PSM_CT32 ||
        state.prim.type != GS_PRIM_SPRITE || batch.vertexCount != 2u ||
        state.prim.tme || state.prim.abe ||
        context.frame.fbmsk != 0u || test.destinationAlphaTestEnabled ||
        (test.alphaTestEnabled && test.alphaTest != kAlphaAlways) ||
        (test.depthTestEnabled && test.depthTest != kDepthAlways))
        return {};

    const double x0 = batch.vertices[0].x - context.xyoffset.ofx / 16.0;
    const double y0 = batch.vertices[0].y - context.xyoffset.ofy / 16.0;
    const double x1 = batch.vertices[1].x - context.xyoffset.ofx / 16.0;
    const double y1 = batch.vertices[1].y - context.xyoffset.ofy / 16.0;
    for (double value : {x0, y0, x1, y1})
        if (!std::isfinite(value) || std::floor(value) != value) return {};
    auto clipped = [](double value, uint32_t low, uint32_t high) {
        return static_cast<uint32_t>(std::clamp(value, double(low), double(high)));
    };
    return {clipped(std::min(x0, x1), context.scissor.x0, context.scissor.x1 + 1u),
            clipped(std::min(y0, y1), context.scissor.y0, context.scissor.y1 + 1u),
            clipped(std::max(x0, x1), context.scissor.x0, context.scissor.x1 + 1u),
            clipped(std::max(y0, y1), context.scissor.y0, context.scissor.y1 + 1u)};
}

bool samplesFitSurface(const GSPrimitiveBatch &batch, const GsSurface &surface,
                       GsRegion *coverage = nullptr) {
    const auto &state = batch.state;
    if (!coverage && state.textureWidth <= surface.width && state.textureHeight <= surface.height)
        return true;
    if (!state.prim.fst)
        return false;
    const auto clamp = gsDecodeClamp(state.context.clamp);
    if (clamp.wrapU > kWrapClamp || clamp.wrapV > kWrapClamp)
        return false;
    double minU = batch.vertices[0].u / 16.0, maxU = minU;
    double minV = batch.vertices[0].v / 16.0, maxV = minV;
    for (uint8_t i = 1u; i < batch.vertexCount; ++i) {
        minU = std::min(minU, batch.vertices[i].u / 16.0);
        maxU = std::max(maxU, batch.vertices[i].u / 16.0);
        minV = std::min(minV, batch.vertices[i].v / 16.0);
        maxV = std::max(maxV, batch.vertices[i].v / 16.0);
    }
    // How the fragment shader samples at a scale above 1: a draw on the
    // native grid samples each GS pixel at its own position, as the GS does;
    // any other bilinear draw filters the target's physical texels.
    const double scale = std::max(surface.scale, 1u);
    const uint32_t framePsm = state.context.frame.psm;
    const bool nativeGrid = scale > 1.0 &&
        (!state.linearFilter || framePsm == GS_PSM_CT16 || framePsm == GS_PSM_CT16S);
    const bool physicalTaps = scale > 1.0 && state.linearFilter && !nativeGrid;
    double nativeMinU = minU, nativeMaxU = maxU, nativeMinV = minV, nativeMaxV = maxV;
    if (state.prim.type == GS_PRIM_SPRITE) {
        // Sprite edges are exclusive. Check the samples actually covered,
        // including subpixels at higher scales, rather than the unused far edge.
        // The vertex shader moves geometry half a host pixel, so host pixel h
        // is covered from h = ceil(x * scale) and samples GS position h / scale.
        const auto &a = batch.vertices[0];
        const auto &b = batch.vertices[1];
        auto range = [&](double p0, double p1, double t0, double t1, uint32_t clip0, uint32_t clip1,
                         double &lo, double &hi, double &nativeLo, double &nativeHi) {
            if (p0 == p1 || !std::isfinite(p0) || !std::isfinite(p1)) return false;
            const double first = std::max(std::ceil(std::min(p0, p1) * scale), clip0 * scale);
            const double last = std::min(std::ceil(std::max(p0, p1) * scale) - 1.0,
                                         (clip1 + 1.0) * scale - 1.0);
            if (first > last) return false;
            auto sample = [&](double pixel) {
                const double position = nativeGrid ? std::floor(pixel / scale) : pixel / scale;
                return t0 + (t1 - t0) * (position - p0) / (p1 - p0);
            };
            lo = std::min(sample(first), sample(last));
            hi = std::max(sample(first), sample(last));
            // Where the GS's own samples end, at each covered GS pixel's
            // position: a bound the upscaled sub-pixels past it may clamp to.
            const auto native = [&](double pixel) {
                return t0 + (t1 - t0) * (std::floor(pixel / scale) - p0) / (p1 - p0);
            };
            nativeLo = std::min(native(first), native(last));
            nativeHi = std::max(native(first), native(last));
            return true;
        };
        const auto &c = state.context;
        if (!range(a.x - c.xyoffset.ofx / 16.0, b.x - c.xyoffset.ofx / 16.0,
                   a.u / 16.0, b.u / 16.0, c.scissor.x0, c.scissor.x1, minU, maxU, nativeMinU, nativeMaxU) ||
            !range(a.y - c.xyoffset.ofy / 16.0, b.y - c.xyoffset.ofy / 16.0,
                   a.v / 16.0, b.v / 16.0, c.scissor.y0, c.scissor.y1, minV, maxV, nativeMinV, nativeMaxV))
            return false;
    }
    auto fits = [&](double lo, double hi, double nativeLo, double nativeHi, uint32_t size,
                    uint32_t extent, GsWrapMode mode, uint32_t &first, uint32_t &end) {
        if (physicalTaps) {
            // The two physical texels about position * scale - 0.5, as GS
            // texels. A 1:1 copy lands exactly on a texel, leaving the next
            // one no weight: not a tap.
            lo = std::floor(std::floor(lo * scale - 0.5) / scale);
            hi = std::floor(std::ceil(hi * scale - 0.5 - 1.0 / 256.0) / scale);
            // Sub-pixels past the GS's last sample can reach a texel beyond
            // the target the GS never reads. The shader clamps them to the
            // bound texture, so they only count when the GS's own taps
            // already leave the target.
            if (std::floor(nativeLo - 0.5) >= 0.0)
                lo = std::max(lo, 0.0);
            if (std::ceil(nativeHi - 0.5) < extent)
                hi = std::min(hi, double(extent) - 1.0);
        } else if (state.linearFilter) {
            lo = std::floor(lo - 0.5);
            hi = std::ceil(hi - 0.5);
        } else {
            // The shader's point-sample bias.
            lo = std::floor(lo);
            hi = std::floor(hi + 1.0 / 256.0);
        }
        if (mode == kWrapClamp) {
            lo = std::clamp(lo, 0.0, double(size - 1u));
            hi = std::clamp(hi, 0.0, double(size - 1u));
        } else {
            if (std::floor(lo / size) != std::floor(hi / size)) {
                lo = 0.0;
                hi = size - 1u;
            } else {
                const double wrap = std::floor(lo / size) * size;
                lo -= wrap;
                hi -= wrap;
            }
        }
        first = uint32_t(lo);
        end = uint32_t(hi) + 1u;
        return lo >= 0.0 && hi < extent;
    };
    GsRegion taps;
    const bool fitsU = fits(minU, maxU, nativeMinU, nativeMaxU, std::max<uint16_t>(state.textureWidth, 1u),
                           surface.width, clamp.wrapU, taps.x0, taps.x1);
    const bool fitsV = fits(minV, maxV, nativeMinV, nativeMaxV, std::max<uint16_t>(state.textureHeight, 1u),
                           surface.height, clamp.wrapV, taps.y0, taps.y1);
    if (coverage) *coverage = taps;
    return fitsU && fitsV;
}

// A UV sprite's texture rectangle, the art it takes from its texture (a
// glyph's cell, a label), when it lies within the texture; or a UV
// triangle's, half of a quad drawn as a strip (Rogue Galaxy's text in a
// gradient, each glyph two triangles over the same rectangle) (Rogue
// Galaxy: added).
bool spriteTexelRect(const GSPrimitiveBatch &batch, GsRegion &region) {
    const GSDrawState &state = batch.state;
    const bool sprite = state.prim.type == GS_PRIM_SPRITE;
    const bool triangle = state.prim.type == GS_PRIM_TRIANGLE || state.prim.type == GS_PRIM_TRISTRIP ||
                          state.prim.type == GS_PRIM_TRIFAN;
    const uint8_t count = sprite ? 2u : 3u;
    if ((!sprite && !triangle) || !state.prim.fst || batch.vertexCount < count)
        return false;
    uint32_t u0 = batch.vertices[0].u, u1 = u0, v0 = batch.vertices[0].v, v1 = v0;
    for (uint8_t i = 1u; i < count; ++i) {
        u0 = std::min<uint32_t>(u0, batch.vertices[i].u);
        u1 = std::max<uint32_t>(u1, batch.vertices[i].u);
        v0 = std::min<uint32_t>(v0, batch.vertices[i].v);
        v1 = std::max<uint32_t>(v1, batch.vertices[i].v);
    }
    region = {u0 / 16u, v0 / 16u, (u1 + 15u) / 16u, (v1 + 15u) / 16u};
    // A sprite's UVs span a texel more than its pixels: at the texture's
    // edge that texel is past it, and never shown (Rogue Galaxy's registered
    // mark, at the bottom of its special characters' picture).
    if (region.x1 == state.textureWidth + 1u)
        region.x1 = state.textureWidth;
    if (region.y1 == state.textureHeight + 1u)
        region.y1 = state.textureHeight;
    return !region.empty() && region.x1 <= state.textureWidth && region.y1 <= state.textureHeight;
}

// The texels the GS samples drawing a point-sampled UV sprite, at each
// pixel it covers as samplesFitSurface takes them, whatever the scissor: a
// sprite's UVs often span a texel more than its pixels, and that last
// texel, never shown, can hold a descender's end or the next piece of art.
// False when it covers no pixel (Rogue Galaxy: added).
bool spriteSampledTexels(const GSPrimitiveBatch &batch, GsRegion &sampled) {
    const auto &c = batch.state.context;
    const auto &a = batch.vertices[0];
    const auto &b = batch.vertices[1];
    auto axis = [](double p0, double p1, double t0, double t1, uint32_t &first, uint32_t &end) {
        if (p0 == p1 || !std::isfinite(p0) || !std::isfinite(p1))
            return false;
        const double start = std::ceil(std::min(p0, p1));
        const double last = std::ceil(std::max(p0, p1)) - 1.0;
        if (start > last)
            return false;
        auto texel = [&](double pixel) {
            return std::floor(t0 + (t1 - t0) * (pixel - p0) / (p1 - p0) + 1.0 / 256.0);
        };
        const double lo = std::min(texel(start), texel(last));
        const double hi = std::max(texel(start), texel(last));
        if (lo < 0.0)
            return false;
        first = uint32_t(lo);
        end = uint32_t(hi) + 1u;
        return true;
    };
    return axis(a.x - c.xyoffset.ofx / 16.0, b.x - c.xyoffset.ofx / 16.0, a.u / 16.0, b.u / 16.0, sampled.x0,
                sampled.x1) &&
           axis(a.y - c.xyoffset.ofy / 16.0, b.y - c.xyoffset.ofy / 16.0, a.v / 16.0, b.v / 16.0, sampled.y0,
                sampled.y1);
}

// A glyph's copy, as the host marks them for setRegionReplacements: a UV
// sprite from an indexed texture, its y `marks` sixteenths of a pixel above
// the whole pixel (Rogue Galaxy: added).
bool markedCopy(const GSPrimitiveBatch &batch, int32_t marks) {
    const GSDrawState &state = batch.state;
    if (state.prim.type != GS_PRIM_SPRITE || !state.prim.tme || !state.prim.fst || batch.vertexCount < 2u ||
        !gsIsIndexedPsm(state.context.tex0.psm))
        return false;
    for (uint8_t i = 0u; i < 2u; ++i)
        if ((static_cast<int32_t>(std::lround(batch.vertices[i].y * 16.0f)) & 15) != 16 - marks)
            return false;
    return true;
}

// One of a glyph's border copies, black or the dark grey (0x20) some of
// Rogue Galaxy's borders mix in: its marks, a sixteenth up for a border
// whose first copy is a pixel up, three for one whose first is a pixel up
// and left; 0 for any other draw.
int32_t borderCopy(const GSPrimitiveBatch &batch) {
    const int32_t marks = markedCopy(batch, 1) ? 1 : markedCopy(batch, 3) ? 3 : 0;
    if (marks == 0)
        return 0;
    for (uint8_t i = 0u; i < 2u; ++i) {
        const GSVertex &v = batch.vertices[i];
        if (v.r > 0x20u || v.g > 0x20u || v.b > 0x20u)
            return 0;
    }
    return marks;
}

// One of a glyph's shadow copies: marked two sixteenths up.
bool shadowCopy(const GSPrimitiveBatch &batch) {
    return markedCopy(batch, 2);
}

// The pixels a draw can write: exact for a sprite at scale 1, where the vertex
// shader's half-pixel shift covers [ceil(x0), ceil(x1)) as samplesFitSurface
// assumes, and padded like primitiveBounds otherwise.
GsRegion drawnPixels(const GSPrimitiveBatch &batch, const GsSurface &surface) {
    const GSContext &context = batch.state.context;
    if (surface.scale != 1u || batch.state.prim.type != GS_PRIM_SPRITE || batch.vertexCount != 2u)
        return primitiveBounds(batch);
    const double offsetX = context.xyoffset.ofx / 16.0, offsetY = context.xyoffset.ofy / 16.0;
    const double x0 = batch.vertices[0].x - offsetX, x1 = batch.vertices[1].x - offsetX;
    const double y0 = batch.vertices[0].y - offsetY, y1 = batch.vertices[1].y - offsetY;
    for (double value : {x0, x1, y0, y1})
        if (!std::isfinite(value))
            return primitiveBounds(batch);
    auto span = [](double a, double b, uint32_t low, uint32_t high, uint32_t &first, uint32_t &end) {
        first = static_cast<uint32_t>(std::clamp(std::ceil(std::min(a, b)), double(low), double(high)));
        end = static_cast<uint32_t>(std::clamp(std::ceil(std::max(a, b)), double(low), double(high)));
    };
    GsRegion region;
    span(x0, x1, context.scissor.x0, context.scissor.x1 + 1u, region.x0, region.x1);
    span(y0, y1, context.scissor.y0, context.scissor.y1 + 1u, region.y0, region.y1);
    return region;
}

// The texels a draw can sample from a surface, or all of them when its
// coordinates cannot be bounded.
GsRegion sampledTexels(const GSPrimitiveBatch &batch, const GsSurface &surface) {
    GsRegion taps;
    if (samplesFitSurface(batch, surface, &taps) && !taps.empty())
        return taps;
    return surface.wholeRegion();
}

// The texture pages a primitive can sample, for the queued-draw checks. The
// texture's whole footprint (`whole`, from sourcePagesFor) spans 2^TW x 2^TH
// texels: 512 lines over Rogue Galaxy's 448-line buffers, and that false
// overlap with other targets' queued draws flushed at every strip of its
// frame copies. Bounded from the UVs where they are plain (FST, clamp or an
// in-range repeat); `whole` otherwise. The palette's pages stay in.
// The texels a primitive can sample, in its texture, when its coordinates
// are plain (FST, clamp or an in-range repeat; bilinear's half texel
// included); false otherwise.
bool sampledTexelRect(const GSPrimitiveBatch &batch, GsRegion &rect) {
    const auto &state = batch.state;
    const auto &context = state.context;
    if (!state.prim.fst || batch.vertexCount == 0u)
        return false;
    const auto clamp = gsDecodeClamp(context.clamp);
    if (clamp.wrapU >= kWrapRegionClamp || clamp.wrapV >= kWrapRegionClamp)
        return false;
    double minU = batch.vertices[0].u / 16.0, maxU = minU;
    double minV = batch.vertices[0].v / 16.0, maxV = minV;
    for (uint8_t i = 1u; i < batch.vertexCount; ++i) {
        minU = std::min(minU, batch.vertices[i].u / 16.0);
        maxU = std::max(maxU, batch.vertices[i].u / 16.0);
        minV = std::min(minV, batch.vertices[i].v / 16.0);
        maxV = std::max(maxV, batch.vertices[i].v / 16.0);
    }
    for (double value : {minU, maxU, minV, maxV})
        if (!std::isfinite(value))
            return false;
    const double width = state.textureWidth, height = state.textureHeight;
    // Bilinear reaches half a texel further out.
    minU -= 0.5; maxU += 0.5; minV -= 0.5; maxV += 0.5;
    if ((clamp.wrapU == kWrapRepeat && (minU < 0.0 || maxU > width)) ||
        (clamp.wrapV == kWrapRepeat && (minV < 0.0 || maxV > height)))
        return false;
    const auto bound = [](double value, double limit) {
        return static_cast<uint32_t>(std::clamp(value, 0.0, limit));
    };
    rect.x0 = bound(std::floor(minU), width - 1.0);
    rect.y0 = bound(std::floor(minV), height - 1.0);
    rect.x1 = std::max(bound(std::ceil(maxU), width), rect.x0 + 1u);
    rect.y1 = std::max(bound(std::ceil(maxV), height), rect.y0 + 1u);
    return true;
}

GsPageSet sampledSourcePages(const GSPrimitiveBatch &batch, const GsPageSet &whole) {
    const auto &state = batch.state;
    const auto &context = state.context;
    GsRegion rect;
    if (!sampledTexelRect(batch, rect))
        return whole;
    const uint32_t tbw = std::max<uint32_t>(context.tex0.tbw, 1u);
    GsPageSet sampled, footprint;
    gsMarkPages(sampled, context.tex0.tbp0, tbw, context.tex0.psm, rect.width(), rect.height(), rect.x0, rect.y0);
    gsMarkPages(footprint, context.tex0.tbp0, tbw, context.tex0.psm, static_cast<uint32_t>(state.textureWidth),
                static_cast<uint32_t>(state.textureHeight), 0u, 0u);
    return sampled | (whole & ~footprint);
}

struct FeedbackPadding {
    // The column tail and bottom row are separate: their bounding rectangle
    // would include source pixels still owned by the GPU.
    GsRegion uploads[2];
    bool rightColumn = false;
};

bool canSnapshotFeedback(const GSPrimitiveBatch &batch, const GsSurface &surface,
                          FeedbackPadding &padding) {
    padding = {};
    if (surface.psm != GS_PSM_CT32 && surface.psm != GS_PSM_CT16 &&
        surface.psm != GS_PSM_CT16S) return false;
    if (samplesFitSurface(batch, surface)) return true;
    GsRegion taps;
    samplesFitSurface(batch, surface, &taps);
    if (taps.empty() || taps.x1 > surface.width + 1u ||
        taps.y1 > surface.height + 1u) return false;
    const bool rightColumn = taps.x1 > surface.width;
    const bool bottomRow = taps.y1 > surface.height;
    if (rightColumn) {
        if (surface.psm != GS_PSM_CT32 || surface.width != surface.bufferWidth * 64u ||
            surface.height < 32u || surface.height % 32u != 0u) return false;
        // CT32 pages are 64x32. At the complete GS row pitch, (width,y)
        // aliases (0,y+32): most padding belongs to the live GPU source.
        // Only the last page row lies outside that source and can use VRAM.
        padding.rightColumn = true;
        padding.uploads[0] = {surface.width, surface.height - 32u,
                             surface.width + 1u, surface.height};
    }
    if (bottomRow)
        padding.uploads[1] = {0u, surface.height, surface.width + uint32_t(rightColumn),
                             surface.height + 1u};
    GsPageSet tailPages;
    for (const auto &region : padding.uploads)
        gsMarkPages(tailPages, surface.base, surface.bufferWidth, surface.psm,
                    region.width(), region.height(), region.x0, region.y0);
    // CPU-supplied padding must not alias any live source page, even when
    // the GS address wraps around VRAM or the base is inside a page.
    if ((tailPages & surface.pages).any()) return false;
    return true;
}

// DQ8_GFX_TRACE_BATCHES prints one line per draw call: which target, which
// pipeline state, how much geometry. The question it answers is "why is this
// buffer empty" -- either nothing targets it, or something does and is being
// rejected by a test.
bool batchTraceEnabled() {
    static const bool enabled = [] {
        const char *value = std::getenv("DQ8_GFX_TRACE_BATCHES");
        return value != nullptr && *value != '\0';
    }();
    return enabled;
}

} // namespace


void setStatsExtraLine(std::function<std::string()> provider) {
    statsExtraLine() = std::move(provider);
}

void setPresentObserver(std::function<void(double)> observer) {
    presentObserver() = std::move(observer);
}
struct SdlGpuBackend::Impl {
    mutable std::mutex mutex;
    SdlGpuDevice device;
    GsVram vram;
    std::unique_ptr<GsTransferEngine> transfer;
    GSTransferCommand transferCommand{};
    bool patchingHostWrite = false;
    // A host write that skipped resolving the pages it covers is still arriving.
    bool coveredTransferOpen = false;
    // Pages of the open host write already released to local memory, so a
    // page goes once, when the rows that fill it have arrived (Rogue Galaxy:
    // added; see UploadImage).
    GsPageSet coveredReleased;

    // Counts the open covered host write if something meets it before its last
    // pixel, once; called before a draw or another transfer.
    void noteTransferInterrupted() {
        if (!coveredTransferOpen)
            return;
        coveredTransferOpen = false;
        const GSTransferSnapshot state = transfer ? transfer->snapshot() : GSTransferSnapshot{};
        if (state.direction == 0u && state.copiedPixels < state.totalPixels)
            ++stats.interruptedCoveredTransfers;
    }
    std::unique_ptr<GsTargetCache> targets;
    std::unique_ptr<GsTextureCache> textures;

    std::vector<GsGpuVertex> vertices;
    std::vector<DrawBatch> batches;
    // The pages the queued batches write, in total and per target: every
    // primitive asks whether queued draws touch what it reads or writes, and
    // scanning hundreds of batches for each one was a tenth of the worker.
    struct PendingTarget {
        const GsSurface *color;
        GsPageSet pages;
    };
    GsPageSet pendingPages{};
    std::vector<PendingTarget> pendingTargets;
    void notePending(const GsSurface *color, const GsPageSet &pages) {
        pendingPages |= pages;
        for (auto &target : pendingTargets) {
            if (target.color == color) {
                target.pages |= pages;
                return;
            }
        }
        pendingTargets.push_back({color, pages});
    }
    // Pages the queued batches sample from live targets: a sampled target, or
    // a feedback batch's own colour target, through a snapshot the flush
    // takes. A host write into them has to wait for that flush.
    GsPageSet pendingSourcePages{};
    void noteSampled(const DrawBatch &draw) {
        if (draw.source)
            pendingSourcePages |= draw.source->pages;
        if ((draw.feedback || draw.destination) && draw.color)
            pendingSourcePages |= draw.color->pages;
    }
    void clearBatches() {
        batches.clear();
        pendingPages.reset();
        pendingTargets.clear();
        pendingSourcePages.reset();
        last.valid = false;
    }

    // The primitive before this one, when it went through the texture-cache
    // path (or untextured), was queued, and nothing has happened since: the
    // next primitive with byte-identical draw state draws into the same
    // surfaces with the same pipeline, uniforms and texture, so only what
    // depends on its vertices is redone. The frontend hands over one
    // primitive at a time and an object is thousands of them under one
    // state; the full path's target lookups, view preparation, page scans,
    // register decoding and texture-cache lookups were a fifth of the
    // worker (Rogue Galaxy: added).
    struct LastDraw {
        bool valid = false;
        GSDrawState state{};
        GsSurface *color = nullptr;
        bool alphaPass = false;
        bool textured = false;
        SDL_GPUTexture *texture = nullptr;
        // The host's larger texture the open batch draws from instead (Rogue
        // Galaxy: added; setRegionReplacements).
        SDL_GPUTexture *larger = nullptr;
        GsPageSet sourcePages{};
        // The texels acquired from the cache under this state so far: a
        // primitive within them needs no cache call. reachAll after an
        // unbounded acquire, which builds the whole texture.
        GsRegion reach{};
        bool reachAll = false;
        // The open batch's pages cover its whole bounding box (a batch the
        // full path started, or one this path has grown since), so a
        // primitive inside the box adds no page.
        bool boxPages = false;
        float offsetX = 0.0f, offsetY = 0.0f, depthScale = 0.0f;
    } last;

    // False sends the primitive down the full path.
    bool submitSameState(const GSPrimitiveBatch &batch) {
        GsSurface *color = last.color;
        GsRegion written = primitiveBounds(batch);
        if (written.empty())
            return true;
        written.x1 = std::min(written.x1, color->width);
        written.y1 = std::min(written.y1, color->height);
        if (written.empty())
            return true;
        const GSDrawState &state = batch.state;
        // A sprite that replaces the whole surface claims it: the full path.
        if (state.prim.type == GS_PRIM_SPRITE && !noWholeClaim &&
            opaqueSpriteBounds(batch, *color, gsDecodeTest(state.context.test), true)
                .contains(color->wholeRegion()))
            return false;
        const size_t open = batches.size();
        const size_t span = last.alphaPass ? 2u : 1u;
        if (open < span)
            return false;
        if (last.textured) {
            ++stats.texturedPrimitives;
            // This primitive may reach texels the one before did not. Within
            // the texels already acquired, with no queued draw into the
            // texture's pages, there is nothing to check or build; otherwise
            // the same checks and the same cache call as the full path, which
            // expands the texture when it must. Anything that would flush or
            // rebuild takes the full path.
            GsRegion reach;
            const bool bounded = sampledTexelRect(batch, reach);
            const bool covered = last.reachAll || (bounded && last.reach.contains(reach));
            const bool untouched = (last.sourcePages & pendingPages).none();
            if (!covered || !untouched) {
                if (!untouched && pendingDrawsTouch(sampledSourcePages(batch, last.sourcePages)))
                    return false;
                if (textures->evictionImminent())
                    return false;
                std::string textureError;
                ResolveReasonScope scope(*targets, GsResolveReason::Texture);
                SDL_GPUTexture *texture = textures->acquire(state, textureError, bounded ? &reach : nullptr);
                if (texture != last.texture || !textureError.empty())
                    return false;
                if (bounded)
                    last.reach.merge(reach);
                else
                    last.reachAll = true;
            }
        }
        // From the host's version of the same content or not, as the open
        // batch draws (Rogue Galaxy: added).
        SDL_GPUTexture *larger = nullptr;
        GsRegion largerArea;
        if (last.textured && regionPack) {
            uint32_t scale = 0u;
            bool nearest = false; // a texture's own, so merging by texture keeps it
            larger = largerRegion(batch, *color, false, false, scale, nearest, largerArea);
            if (larger != last.larger)
                return false;
            if (larger) {
                const GsClampState clamp = largerClamp(largerArea);
                const float *bounds = batches[open - span].fragmentUniforms.region;
                if (bounds[0] != float(clamp.minU) || bounds[1] != float(clamp.maxU) ||
                    bounds[2] != float(clamp.minV) || bounds[3] != float(clamp.maxV))
                    return false;
            }
        }
        const uint32_t firstVertex = static_cast<uint32_t>(vertices.size());
        appendPrimitive(batch, last.offsetX, last.offsetY, last.depthScale);
        const uint32_t addedVertices = static_cast<uint32_t>(vertices.size()) - firstVertex;
        if (larger)
            toLargerTexels(firstVertex, largerArea);
        if (addedVertices == 0u)
            return true;
        ++stats.primitivesDrawn;
        ++stats.sameStatePrimitives;
        stats.trianglesDrawn += addedVertices / 3u;
        // The batch's pages are those of its bounding box, a superset of the
        // full path's union of each primitive's, so a primitive inside the
        // box adds nothing; one that grows it marks the grown box once.
        DrawBatch &head = batches[open - span];
        const bool inside = last.boxPages && head.written.contains(written);
        GsPageSet grownPages;
        GsRegion grown = head.written;
        if (!inside) {
            grown.merge(written);
            gsMarkPages(grownPages, color->base, color->bufferWidth, color->psm,
                        grown.width(), grown.height(), grown.x0, grown.y0);
            last.boxPages = true;
        }
        for (size_t index = open - span; index < open; ++index) {
            batches[index].vertexCount += addedVertices;
            if (!inside) {
                batches[index].written = grown;
                batches[index].writtenPages |= grownPages;
            }
        }
        if (!inside)
            notePending(color, grownPages);
        return true;
    }

    // Whether a host write into these pages has to land the queued draws
    // first: they draw into the pages, or into a surface the pages belong to
    // (its resolve or refresh would miss them otherwise), or they sample
    // from them. Any other write goes ahead of the flush: the textures the
    // queued draws hold were expanded when they were queued, and a cached
    // texture is never rewritten under a queued draw (the texture cache's
    // before-rewrite hook lands the draws first). Rogue Galaxy uploads
    // about 350 textures a frame between its draws; flushing at each one
    // submitted that many command buffers a frame.
    bool transferConflicts(const GsPageSet &pages) const {
        if (flushOnTransfer || (pendingPages & pages).any() || (pendingSourcePages & pages).any())
            return true;
        for (const auto &target : pendingTargets)
            if ((target.color->pages & pages).any())
                return true;
        return false;
    }
    bool batchesUse(const SDL_GPUTexture *texture) const {
        for (const DrawBatch &batch : batches)
            if (batch.texture == texture)
                return true;
        return false;
    }
    // DQ8_GFX_FLUSH_ON_TRANSFER: the old behaviour, a flush at every transfer.
    const bool flushOnTransfer = std::getenv("DQ8_GFX_FLUSH_ON_TRANSFER") != nullptr;
    // DQ8_GFX_NO_CLAIM: a whole-surface sprite imports like any other draw.
    const bool noWholeClaim = std::getenv("DQ8_GFX_NO_CLAIM") != nullptr;
    // DQ8_GFX_NO_PASSIVE_DEPTH: a draw that ignores depth has no depth
    // attachment, as before.
    const bool noPassiveDepth = std::getenv("DQ8_GFX_NO_PASSIVE_DEPTH") != nullptr;
    // DQ8_GFX_PATCH_CT32_ONLY: only CT32 host writes patch a live target; the
    // others read it back first, as before.
    const bool patchCt32Only = std::getenv("DQ8_GFX_PATCH_CT32_ONLY") != nullptr;
    // An upload that replaces whole pages of a live target releases them to
    // local memory instead of patching them block by block (Rogue Galaxy:
    // added; see GsTargetCache::releaseCoveredPages). It takes a quarter
    // off the worker's time in the streets; DQ8_GFX_NO_COVERED_RELEASE=1
    // patches every page, as before.
    const bool coveredRelease = std::getenv("DQ8_GFX_NO_COVERED_RELEASE") == nullptr;
    // DQ8_GFX_NO_GPU_PADDING: a feedback snapshot's padding rows come from
    // local memory, resolving the target that owns them, as before.
    const bool noGpuPadding = std::getenv("DQ8_GFX_NO_GPU_PADDING") != nullptr;
    // DQ8_GFX_CYCLE_SNAPSHOTS: the copies into a snapshot texture cycle it,
    // as before, and count against the pending budget (see recordSnapshot).
    const bool cycleSnapshots = std::getenv("DQ8_GFX_CYCLE_SNAPSHOTS") != nullptr;
    // The bases of the buffers the last presentation showed, and of the
    // target last copied into one of them: the frame the HUD, menus and text
    // are drawn into. Rogue Galaxy draws its frame at 0 and copies it to the
    // displayed 0x70; the masks of its minimap go into a buffer of the same
    // size at 0x165, whose alpha levels say what to paint, and stay out
    // (Rogue Galaxy: added).
    std::atomic<uint32_t> displayBase1{~0u}, displayBase2{~0u};
    uint32_t frameSourceBase = ~0u;
    bool displayed(const GsSurface &target) const {
        return target.base == displayBase1.load(std::memory_order_relaxed) ||
               target.base == displayBase2.load(std::memory_order_relaxed);
    }
    bool frameTarget(const GsSurface &target) const {
        return target.base == frameSourceBase || displayed(target);
    }
    // setRegionReplacements (Rogue Galaxy: added).
    bool regionPack = false;
    std::atomic<bool> regionPackEnabled{true};
    // A glyph's border copies, marked (borderCopy): how many have come since
    // the last other primitive, and whether the first was drawn as the
    // host's border; while the first is drawn so, it asked for the border
    // version, and whether it was drawn.
    uint32_t borderIndex = 0u;
    // The texels of the first copy (its glyph's).
    GsRegion borderGlyph{};
    bool borderRounded = false;
    bool borderRequest = false;
    bool borderDrawn = false;
    // A shadow copy's quad is being drawn, from the shadow version, as a
    // border's is from the border version.
    bool shadowRequest = false;
    // The texels the first copy shows, as its glyph does: the border's quad
    // is grown past them.
    GsRegion borderShown{};

    // The host's version of the region a UV sprite samples (`region`), or
    // its border or shadow version, a texture of its own at `scale` texels
    // per GS texel; null when it has none, has not made it yet, or the
    // target is not upscaled.
    SDL_GPUTexture *largerRegion(const GSPrimitiveBatch &batch, const GsSurface &target, bool border,
                                 bool shadow, uint32_t &scale, bool &nearest, GsRegion &region) {
        if (!regionPack || target.scale <= 1u || !frameTarget(target) ||
            !gsIsIndexedPsm(batch.state.context.tex0.psm) ||
            !regionPackEnabled.load(std::memory_order_relaxed) || !spriteTexelRect(batch, region))
            return nullptr;
        // A blend factor past one shows the glyph bolder (RegionRequest::
        // bold); the border copies are drawn from their own version.
        bool bold = false;
        if (!border) {
            const uint8_t count = batch.state.prim.type == GS_PRIM_SPRITE ? 2u : 3u;
            for (uint8_t i = 0u; i < count && i < batch.vertexCount; ++i)
                bold = bold || batch.vertices[i].a > 0x80u;
        }
        // The texels the draw shows, whatever the scissor: a line of text
        // scrolling out of a window keeps its glyphs.
        GsRegion sampled;
        if (border || shadow)
            sampled = borderShown;
        else if (batch.state.prim.type != GS_PRIM_SPRITE || !spriteSampledTexels(batch, sampled))
            sampled = region;
        std::string error;
        SDL_GPUTexture *larger =
            textures->replacement(batch.state, region, sampled, target.scale, border, shadow, bold, scale, nearest, error);
        if (!error.empty())
            setError(std::move(error));
        return larger;
    }

    // The host's version is drawn whole. The texels a draw never shows
    // (the row and column a glyph's sprite spans past its pixels) were
    // handed over transparent (GsTextureCache::replacement), so no tap past
    // the shown ones brings in the next piece of art. Clamped to the shown
    // texels instead, the last shown row came out again below it: the
    // shader kept the position inside a clamped GS texel, and at 8x the
    // bottom of a "g" showed twice, a pixel apart (clampedSubTexel in
    // gs_draw.frag now takes the edge's outermost texel, which also ended a
    // light line left of glyphs).
    static GsClampState largerClamp(const GsRegion &region) {
        GsClampState clamp{};
        clamp.wrapU = kWrapRegionClamp;
        clamp.wrapV = kWrapRegionClamp;
        clamp.maxU = region.width() - 1u;
        clamp.maxV = region.height() - 1u;
        return clamp;
    }

    // A border version is drawn whole, margin and all.
    static GsClampState borderClamp(const GsRegion &region) {
        GsClampState clamp{};
        clamp.wrapU = kWrapRegionClamp;
        clamp.wrapV = kWrapRegionClamp;
        clamp.maxU = region.width() + 2u * kRegionBorderMargin - 1u;
        clamp.maxV = region.height() + 2u * kRegionBorderMargin - 1u;
        return clamp;
    }

    // A sprite drawn from its region's own texture samples it from the
    // region's corner.
    void toLargerTexels(uint32_t firstVertex, const GsRegion &region) {
        for (size_t index = firstVertex; index < vertices.size(); ++index) {
            vertices[index].s -= static_cast<float>(region.x0);
            vertices[index].t -= static_cast<float>(region.y0);
        }
    }

    // A border's quad, grown by the margin each side (borderQuad), samples
    // its border version from the corner out to the far edges: the sprite's
    // texture coordinates are those of the region before it grew.
    void toBorderTexels(uint32_t firstVertex, const GsRegion &region) {
        float minS = INFINITY, maxS = -INFINITY, minT = INFINITY, maxT = -INFINITY;
        for (size_t index = firstVertex; index < vertices.size(); ++index) {
            minS = std::min(minS, vertices[index].s);
            maxS = std::max(maxS, vertices[index].s);
            minT = std::min(minT, vertices[index].t);
            maxT = std::max(maxT, vertices[index].t);
        }
        const float margin = 2.0f * static_cast<float>(kRegionBorderMargin);
        for (size_t index = firstVertex; index < vertices.size(); ++index) {
            GsGpuVertex &v = vertices[index];
            v.s = v.s - static_cast<float>(region.x0) + (v.s > 0.5f * (minS + maxS) ? margin : 0.0f);
            v.t = v.t - static_cast<float>(region.y0) + (v.t > 0.5f * (minT + maxT) ? margin : 0.0f);
        }
    }

    // The first border copy as the border's quad: moved back down the pixel
    // the game moved it up, to the glyph's place (`down`), and grown by the
    // border version's margin each side at the sprite's own pixels per
    // texel. A shadow copy's quad stays where it is.
    static bool borderQuad(const GSPrimitiveBatch &copy, GSPrimitiveBatch &out, float down = 1.0f,
                           float right = 0.0f) {
        out = copy;
        GSVertex &a = out.vertices[0];
        GSVertex &b = out.vertices[1];
        const float du = (float(b.u) - float(a.u)) / 16.0f;
        const float dv = (float(b.v) - float(a.v)) / 16.0f;
        if (du == 0.0f || dv == 0.0f)
            return false;
        const float perU = (b.x - a.x) / du, perV = (b.y - a.y) / dv;
        const float margin = static_cast<float>(kRegionBorderMargin);
        // Each vertex outward along both texture axes, the texels of margin
        // turned into pixels.
        const float outU = a.u < b.u ? -margin : margin;
        const float outV = a.v < b.v ? -margin : margin;
        a.x += right + outU * perU;
        b.x += right - outU * perU;
        a.y += down + outV * perV;
        b.y += down - outV * perV;
        return true;
    }
    // DQ8_GFX_SUBMIT_DRAWS: a flush of the queued draws records them into the
    // device's pending command buffer and submits it only once this many
    // draws are in it (a readback, a present or the cycled-resource budget
    // sends it sooner); 0 submits at every flush, as before (Rogue Galaxy:
    // added). The game's frames flush about 70 times: feedback snapshots,
    // transfers into pages the queued draws touch, aliased views.
    const uint32_t submitAfterDraws = [] {
        const char *value = std::getenv("DQ8_GFX_SUBMIT_DRAWS");
        return value ? static_cast<uint32_t>(std::strtoul(value, nullptr, 10)) : 96u;
    }();
    // The pages of the host write being patched (patchingHostWrite), which a
    // local copy's source resolve must leave alone; and the pages of a local
    // copy's source whose words local memory already holds, which need no
    // readback either.
    GsPageSet patchPages;
    GsPageSet copySourceCurrent;
    bool patchableHostWrite(uint32_t psm, const GsPageSet &pages) const {
        return (patchCt32Only ? (psm & 0x3fu) == GS_PSM_CT32 : GsTargetCache::gsPatchableHostWrite(psm)) &&
               targets->canPatchHostWrite(pages);
    }

    SDL_GPUBuffer *vertexBuffer = nullptr;
    uint32_t vertexBufferCapacity = 0u;
    // A flush appends its vertices after the ones earlier flushes recorded
    // into the same submission (vertexBase) and cycles the buffer only at
    // the first flush of a submission (Rogue Galaxy: added). The transfer
    // buffers come from a pool guarded by the device's submission serials.
    SdlGpuUploadPool vertexUploads;
    uint32_t vertexBase = 0u;
    uint64_t vertexSerial = 0u;
    // Draws recorded into the pending command buffer since it last went out.
    uint32_t recordedDraws = 0u;
    SDL_GPUTexture *dummyTexture = nullptr;
    // Snapshot textures, one per size. A copy into one lands after the draws
    // recorded before it, which have sampled the last one by then.
    struct FeedbackTexture {
        uint32_t width, height;
        SDL_GPUTexture *texture;
    };
    std::vector<FeedbackTexture> feedbackTextures;
    // The snapshot tail upload, mapped with cycling each time.
    SDL_GPUTransferBuffer *feedbackUpload = nullptr;
    uint32_t feedbackUploadCapacity = 0u;
    // What the feedback batches at the end of the queue have drawn since their
    // snapshot. A draw that reads none of it can share that snapshot.
    GsRegion feedbackWritten;

    // Set when this backend owns the window. Present() then composes into the
    // swapchain instead of handing pixels back through host memory.
    SDL_Window *window = nullptr;
    // What openWindow was given; the FPS readout keeps it in front.
    std::string windowTitle;
    // Staging for the fallback path, where the frame had to be composed on the
    // CPU and has to get back onto the GPU to be shown.
    SDL_GPUTexture *presentUpload = nullptr;
    uint32_t presentUploadWidth = 0u;
    uint32_t presentUploadHeight = 0u;
    struct PresentationSlot {
        SDL_GPUTexture *texture = nullptr;
        uint32_t width = 0u, height = 0u;
        SDL_GPUFence *prepared = nullptr, *displayed = nullptr;
        bool leased = false;
    };
    struct PresentationPool {
        std::mutex mutex;
        std::condition_variable available;
        std::array<PresentationSlot, 3> slots{};
        bool closing = false;
    };
    struct PreparedFrame final : GSPreparedPresentation {
        std::shared_ptr<PresentationPool> pool;
        size_t slot;
        PresentationFrame frame;
        bool haveFrame = false, gsActive = false, download = false;
        PreparedFrame(std::shared_ptr<PresentationPool> pool, size_t slot)
            : pool(std::move(pool)), slot(slot) {}
        ~PreparedFrame() override {
            std::lock_guard lock(pool->mutex);
            pool->slots[slot].leased = false;
            pool->available.notify_one();
        }
    };
    std::shared_ptr<PresentationPool> presentationPool = std::make_shared<PresentationPool>();
    PresentationSlot *preparingSlot = nullptr;
    // Worker-only aliases into the slot currently being prepared.
    SDL_GPUTexture *presentSource = nullptr;
    uint32_t presentSourceWidth = 0u;
    uint32_t presentSourceHeight = 0u;
    bool windowCloseRequested = false;
    SdlPadInput pad;

    // Window-thread state: the overlay, how the picture is fitted, and the
    // textures the fitting and captures need.
    SdlGpuOverlay *overlay = nullptr;
    bool overlayCaptured = false;
    SdlGpuDisplayOptions displayOptions{};
    SDL_GPUPresentMode appliedPresentMode = SDL_GPU_PRESENTMODE_VSYNC;
    SdlGpuBackend::GameWidescreenQuery widescreenQuery = nullptr;
    size_t lastDisplayedSlot = SIZE_MAX;
    // Anything shown, and the last frame the game itself presented.
    std::chrono::steady_clock::time_point lastShown{};
    std::chrono::steady_clock::time_point lastGameFrame{};
    SdlGpuRect lastRect{};
    struct ScratchTexture {
        SDL_GPUTexture *texture = nullptr;
        uint32_t width = 0u, height = 0u;
        SDL_GPUTextureFormat format = SDL_GPU_TEXTUREFORMAT_INVALID;
    };
    ScratchTexture sharpTexture, compositeTexture, captureTexture;
    // What fills the window beside the picture (Rogue Galaxy: added): the
    // picture halved down to a small copy for the blurred sides, and the
    // player's image, loaded each time that choice is made.
    std::array<ScratchTexture, 8> blurLevels;
    SdlGpuSideImageLoader sideImageLoader;
    SDL_GPUTexture *sideImage = nullptr;
    uint32_t sideImageWidth = 0u, sideImageHeight = 0u;
    SdlGpuSides lastSides = SdlGpuSides::Black;
    bool sidesFailed = false;
    // A frame read back to the host and sent back unchanged (Rogue Galaxy:
    // added). The game's screen captures (ScreenFadeCaptureFrame: the
    // frozen frame of an event under an item's message, the fades) go
    // through EE memory at GS size, so a scaled frame came back at 1x. A
    // read of a scaled CT32 region keeps the region on the GPU and a hash of
    // the bytes the host got; an upload of the same size and format whose
    // bytes hash the same, onto a surface of that size, gets the kept copy.
    // Bytes the host changed hash otherwise and land as they are.
    struct RoundTrip {
        ScratchTexture copy;
        uint32_t width = 0u, height = 0u, scale = 0u;
        uint64_t hash = 0u, seen = 0u, expected = 0u;
        bool reading = false, valid = false;
    } roundTrip;
    struct RoundTripUpload {
        bool active = false;
        uint64_t hash = 0u, seen = 0u, expected = 0u;
    } roundTripUpload;
    static constexpr uint64_t kFnvOffset = 0xcbf29ce484222325ull;
    static uint64_t fnv(uint64_t hash, const uint8_t *data, size_t size) {
        for (size_t i = 0u; i < size; ++i) {
            hash ^= data[i];
            hash *= 0x100000001b3ull;
        }
        return hash;
    }
    struct CaptureRequest {
        bool withOverlay = false;
        SdlGpuCaptureCallback done;
    };
    std::vector<CaptureRequest> captureRequests;
    std::string lastWindowError;
    std::atomic<uint32_t> requestedScale{0u};
    std::atomic<uint32_t> activeScale{1u};
    // Removes the PS2's one-line display blend; read by the composing thread.
    // DQ8_GFX_REMOVE_LINE_BLEND sets it for the trace replay, which has no menu.
    std::atomic<bool> removeLineBlend{std::getenv("DQ8_GFX_REMOVE_LINE_BLEND") != nullptr};
    uint64_t previousPresentDraws = 0u;
    uint64_t previousPresentTransfers = 0u;
    uint64_t titlePresents = 0u;
    uint64_t titleActiveFrames = 0u;
    uint64_t titleCompletedStart = 0u;
    bool nativeCaptureAttempted = false;
    uint32_t nativeCaptureNumber = 0u;
    SdlGpuBackend::CompletedRenderCounter completedRenderCounter = nullptr;
    std::chrono::steady_clock::time_point titleSampleStart{};

    uint32_t scale = 1u;
    SdlGpuStats stats{};
    // Batches since the last present, for the target trace.
    uint32_t tracedBatch = 0u;
    std::string lastError;

    // Wall time spent inside backend entry points, so a slow frame can be
    // attributed to the renderer or to everything else -- guest code, the GIF
    // frontend, the movie decoder -- without a profiler.
    uint64_t backendNanos = 0u;
    std::atomic<uint64_t> displayNanos{0u};

    // Only the stats report reads the total, and two clock reads per command
    // cost the worker a couple of percent.
    struct ScopedTimer {
        uint64_t &sink;
        bool timed = statsInterval() != 0u;
        std::chrono::steady_clock::time_point start =
            timed ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        explicit ScopedTimer(uint64_t &target) : sink(target) {}
        ~ScopedTimer() {
            if (!timed)
                return;
            sink += static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - start)
                    .count());
        }
    };
    struct DisplayTimer {
        std::atomic<uint64_t> &sink;
        std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
        explicit DisplayTimer(std::atomic<uint64_t> &sink) : sink(sink) {}
        ~DisplayTimer() {
            sink.fetch_add(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - start).count()), std::memory_order_relaxed);
        }
    };

    bool setError(std::string message) {
        lastError = std::move(message);
        return false;
    }

    // -----------------------------------------------------------------
    // Local-memory synchronisation
    // -----------------------------------------------------------------

    // Attributes the resolves a call provokes to a reason, for the stats and
    // the target trace (Rogue Galaxy: added).
    struct ResolveReasonScope {
        GsTargetCache &cache;
        GsResolveReason previous;
        ResolveReasonScope(GsTargetCache &cache, GsResolveReason reason)
            : cache(cache), previous(cache.resolveReason()) {
            cache.setResolveReason(reason);
        }
        ~ResolveReasonScope() { cache.setResolveReason(previous); }
    };

    // Draws are deferred, so anything that touches local memory has to land
    // them first: otherwise a transfer would overwrite a region whose pending
    // draws had not happened yet, and the draws would then be applied on top.
    bool settleFor(const GsPageSet &pages, GsResolveReason reason, bool hostWrite = false,
                   const GsPageSet &covered = {}, bool flush = true) {
        if (flush && !flushFor(stats.flushSettle))
            return false;
        std::string error;
        ResolveReasonScope scope(*targets, reason);
        if (!(hostWrite ? targets->resolveForHostWrite(pages, error, covered) : targets->resolve(pages, error)))
            return setError(std::move(error));
        return true;
    }

    bool settleAll() {
        if (!flushDraws())
            return false;
        std::string error;
        ResolveReasonScope scope(*targets, GsResolveReason::Sync);
        if (!targets->resolveAll(error))
            return setError(std::move(error));
        return true;
    }

    // True when a queued batch draws into any of these pages, so their content
    // is not yet in the render target -- let alone in local memory.
    bool pendingDrawsTouch(const GsPageSet &pages) const {
        return (pendingPages & pages).any();
    }

    bool pendingDrawsTouchOthers(const GsPageSet &pages, const GsSurface *surface) const {
        for (const auto &target : pendingTargets) {
            if (target.color != surface && (target.pages & pages).any())
                return true;
        }
        return false;
    }

    // A local-to-local transfer between CT32 render targets, done on the GPU.
    // The CPU path reads both targets back -- a stall each, and at a scale
    // above 1 a downsample -- and DQ8 copies 8-bit texture strips inside its
    // character layer for every frame of a dialogue close-up. The swizzle
    // tables say which source byte lands in which destination byte; the GPU
    // applies that per sub-sample plane. Anything else keeps the CPU path.
    // At a transfer's start: a read of a scaled CT32 region keeps it (its
    // hash is taken as the host consumes it), an upload that could be its
    // return starts hashing.
    void noteRoundTrip(const GSTransferCommand &command) {
        // DQ8_GFX_NO_ROUND_TRIP: the 1x frame back as the host sent it.
        static const bool disabled = std::getenv("DQ8_GFX_NO_ROUND_TRIP") != nullptr;
        roundTripUpload.active = false;
        if (disabled)
            return;
        const GSBitBltBuf &blt = command.bitbltbuf;
        const uint32_t width = command.trxreg.rrw, height = command.trxreg.rrh;
        if (command.direction == 1u) {
            roundTrip.reading = false;
            roundTrip.valid = false;
            const uint32_t sbw = std::max<uint32_t>(blt.sbw, 1u);
            const uint32_t x = command.trxpos.ssax, y = command.trxpos.ssay;
            if ((blt.spsm & 0x3fu) != GS_PSM_CT32 || width == 0u || height == 0u)
                return;
            GsPageSet sourcePages;
            gsMarkPages(sourcePages, blt.sbp, sbw, GS_PSM_CT32, width, height, x, y);
            GsSurface *from = targets->nativeOwner(sourcePages, true);
            if (!from || !from->texture || from->scale <= 1u || from->base != blt.sbp ||
                from->bufferWidth != sbw || x + width > from->width || y + height > from->height)
                return;
            const uint32_t s = from->scale;
            std::string error;
            if (!targets->refresh(*from, error) || !device.submitPending(error))
                return;
            if (!ensureScratch(roundTrip.copy, width * s, height * s, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM))
                return;
            SDL_GPUCommandBuffer *commands = SDL_AcquireGPUCommandBuffer(device.handle());
            if (!commands)
                return;
            blitRegion(commands, from->texture, {x * s, y * s, width * s, height * s}, roundTrip.copy.texture,
                       {0u, 0u, width * s, height * s}, SDL_GPU_FILTER_NEAREST, SDL_GPU_LOADOP_DONT_CARE);
            if (!SDL_SubmitGPUCommandBuffer(commands))
                return;
            roundTrip.width = width;
            roundTrip.height = height;
            roundTrip.scale = s;
            roundTrip.hash = kFnvOffset;
            roundTrip.seen = 0u;
            roundTrip.expected = uint64_t(width) * height * 4u;
            roundTrip.reading = true;
            if (gsTargetTraceEnabled())
                std::fprintf(stderr, "[target] round-trip read sbp=%05x sbw=%u %ux%u@%u,%u scale=%u\n", blt.sbp, sbw,
                             width, height, x, y, s);
        } else if (command.direction == 0u && roundTrip.valid && (blt.dpsm & 0x3fu) == GS_PSM_CT32 &&
                   width == roundTrip.width && height == roundTrip.height) {
            roundTripUpload.active = true;
            roundTripUpload.hash = kFnvOffset;
            roundTripUpload.seen = 0u;
            roundTripUpload.expected = roundTrip.expected;
        }
    }

    void noteRoundTripRead(const uint8_t *data, uint32_t size) {
        if (!roundTrip.reading || !data)
            return;
        roundTrip.hash = fnv(roundTrip.hash, data, size);
        roundTrip.seen += size;
        if (roundTrip.seen >= roundTrip.expected) {
            roundTrip.reading = false;
            roundTrip.valid = true;
        }
    }

    // True once the last payload of an upload that matches the read lands.
    bool noteRoundTripUpload(const uint8_t *data, uint32_t size) {
        if (!roundTripUpload.active || !data)
            return false;
        roundTripUpload.hash = fnv(roundTripUpload.hash, data, size);
        roundTripUpload.seen += size;
        if (roundTripUpload.seen < roundTripUpload.expected)
            return false;
        roundTripUpload.active = false;
        return roundTripUpload.seen == roundTripUpload.expected && roundTripUpload.hash == roundTrip.hash;
    }

    // After the upload has landed: the kept copy over the surface it filled,
    // when it filled one whole at the read's scale.
    void applyRoundTrip(const GSTransferCommand &command) {
        const GSBitBltBuf &blt = command.bitbltbuf;
        const uint32_t dbw = std::max<uint32_t>(blt.dbw, 1u);
        const uint32_t width = roundTrip.width, height = roundTrip.height, s = roundTrip.scale;
        if (command.trxpos.dsax != 0u || command.trxpos.dsay != 0u || width != dbw * 64u)
            return;
        std::string error;
        GsSurface *to = targets->acquire(blt.dbp, dbw, GS_PSM_CT32, false, width, height, error);
        if (!to || !to->texture || to->scale != s || to->width != width || to->height != height)
            return;
        targets->claimWhole(*to);
        if (!device.submitPending(error))
            return;
        SDL_GPUCommandBuffer *commands = SDL_AcquireGPUCommandBuffer(device.handle());
        if (!commands)
            return;
        blitRegion(commands, roundTrip.copy.texture, {0u, 0u, width * s, height * s}, to->texture,
                   {0u, 0u, width * s, height * s}, SDL_GPU_FILTER_NEAREST, SDL_GPU_LOADOP_DONT_CARE);
        if (!SDL_SubmitGPUCommandBuffer(commands))
            return;
        targets->markDrawn(*to, {0u, 0u, width, height});
        GsPageSet pages;
        gsMarkPages(pages, blt.dbp, dbw, GS_PSM_CT32, width, height, 0u, 0u);
        if (textures)
            textures->invalidate(pages, GsTextureCache::InvalidationSource::Draw);
        ++stats.roundTrips;
        if (gsTargetTraceEnabled())
            std::fprintf(stderr, "[target] round-trip return dbp=%05x dbw=%u %ux%u scale=%u\n", blt.dbp, dbw, width,
                         height, s);
    }

    bool gpuLocalCopy(const GSTransferCommand &command) {
        static const bool disabled = std::getenv("DQ8_GFX_CPU_LOCAL_COPY") != nullptr;
        const GSBitBltBuf &blt = command.bitbltbuf;
        const uint32_t psm = blt.spsm & 0x3fu;
        if (disabled || command.direction != 2u || psm != (blt.dpsm & 0x3fu))
            return false;
        uint32_t bytesPerPixel = 0u;
        switch (psm) {
        case GS_PSM_T8: bytesPerPixel = 1u; break;
        case GS_PSM_CT16:
        case GS_PSM_CT16S: bytesPerPixel = 2u; break;
        case GS_PSM_CT24: bytesPerPixel = 3u; break;
        case GS_PSM_CT32: bytesPerPixel = 4u; break;
        default: return false;
        }
        const uint32_t width = command.trxreg.rrw, height = command.trxreg.rrh;
        if (width == 0u || height == 0u || width * height > 512u * 512u)
            return false;
        const uint32_t sbw = std::max<uint32_t>(blt.sbw, 1u), dbw = std::max<uint32_t>(blt.dbw, 1u);
        GsPageSet sourcePages, destinationPages;
        gsMarkPages(sourcePages, blt.sbp, sbw, psm, width, height, command.trxpos.ssax, command.trxpos.ssay);
        gsMarkPages(destinationPages, blt.dbp, dbw, psm, width, height, command.trxpos.dsax, command.trxpos.dsay);
        GsSurface *from = targets->nativeOwner(sourcePages, true);
        GsSurface *to = targets->nativeOwner(destinationPages, true);
        if (!from || !to || from->scale != to->scale || !from->texture || !to->texture)
            return false;

        constexpr uint32_t kGsVramBytes = 4u * 1024u * 1024u;
        auto address = [psm](uint32_t base, uint32_t bw, uint32_t x, uint32_t y) {
            switch (psm) {
            case GS_PSM_T8: return GSPSMT8::addrPSMT8(base, bw, x, y);
            case GS_PSM_CT16: return GSPSMCT16::addrPSMCT16(base, bw, x, y);
            case GS_PSM_CT16S: return GSPSMCT16::addrPSMCT16S(base, bw, x, y);
            default: return GSPSMCT32::addrPSMCT32(base, bw, x, y);
            }
        };
        // Where each word of a page sits in a CT32 page: the inverse swizzle.
        static const std::array<uint16_t, 2048> wordPixel = [] {
            std::array<uint16_t, 2048> table{};
            for (uint32_t y = 0u; y < 32u; ++y)
                for (uint32_t x = 0u; x < 64u; ++x)
                    table[GSPSMCT32::addrPSMCT32(0u, 1u, x, y) >> 2u] = static_cast<uint16_t>(x | (y << 8u));
            return table;
        }();
        // A byte address as (pixel, byte) of a CT32 surface covering whole pages.
        auto place = [](const GsSurface &surface, uint32_t byteAddress, uint32_t &x, uint32_t &y) {
            const uint32_t page = (byteAddress >> 13u) % kGsPageCount;
            const uint32_t relative = (page + kGsPageCount - (surface.base >> 5u)) % kGsPageCount;
            const uint16_t pixel = wordPixel[(byteAddress & 8191u) >> 2u];
            x = (relative % surface.bufferWidth) * 64u + (pixel & 0xffu);
            y = (relative / surface.bufferWidth) * 32u + (pixel >> 8u);
            return x < surface.width && y < surface.height;
        };

        struct Byte {
            uint32_t x, y, byte, code;
        };
        std::vector<Byte> bytes;
        std::vector<uint32_t> sourceAddresses, destinationAddresses;
        bytes.reserve(size_t(width) * height * bytesPerPixel);
        GsRegion box{~0u, ~0u, 0u, 0u};
        for (uint32_t row = 0u; row < height; ++row) {
            for (uint32_t column = 0u; column < width; ++column) {
                const uint32_t source = address(blt.sbp, sbw, command.trxpos.ssax + column, command.trxpos.ssay + row);
                const uint32_t destination = address(blt.dbp, dbw, command.trxpos.dsax + column, command.trxpos.dsay + row);
                for (uint32_t i = 0u; i < bytesPerPixel; ++i) {
                    const uint32_t sourceByte = (source + i) & (kGsVramBytes - 1u);
                    const uint32_t destinationByte = (destination + i) & (kGsVramBytes - 1u);
                    uint32_t sx, sy, dx, dy;
                    if (!place(*from, sourceByte, sx, sy) || !place(*to, destinationByte, dx, dy))
                        return false;
                    bytes.push_back({dx, dy, destinationByte & 3u,
                                     0x80000000u | ((sourceByte & 3u) << 24u) | (sy << 12u) | sx});
                    sourceAddresses.push_back(sourceByte);
                    destinationAddresses.push_back(destinationByte);
                    box.x0 = std::min(box.x0, dx);
                    box.y0 = std::min(box.y0, dy);
                    box.x1 = std::max(box.x1, dx + 1u);
                    box.y1 = std::max(box.y1, dy + 1u);
                }
            }
        }
        // Overlapping ranges depend on the GS's scan order; the CPU follows it.
        std::sort(sourceAddresses.begin(), sourceAddresses.end());
        for (uint32_t destination : destinationAddresses)
            if (std::binary_search(sourceAddresses.begin(), sourceAddresses.end(), destination))
                return false;

        std::string error;
        if (!targets->refresh(*from, error) || (to != from && !targets->refresh(*to, error)))
            return setError(std::move(error));
        std::vector<uint32_t> mapping(size_t(box.width()) * box.height() * 4u, 0u);
        for (const Byte &byte : bytes)
            mapping[(size_t(byte.y - box.y0) * box.width() + (byte.x - box.x0)) * 4u + byte.byte] = byte.code;
        const SDL_Rect rect{int(box.x0), int(box.y0), int(box.width()), int(box.height())};
        if (!device.localCopy(from->texture, to->texture, to->width, to->height, to->scale, rect, mapping, error))
            return setError(std::move(error));
        targets->markDrawn(*to, box);
        // Other views of those pages are stale now; the destination keeps them.
        targets->invalidate(destinationPages, true);
        if (textures)
            textures->invalidate(destinationPages, GsTextureCache::InvalidationSource::Draw);
        ++stats.gpuLocalCopies;
        return true;
    }

    // A write to local memory invalidates both caches: a render target holding
    // that region is now stale, and so is any texture built from it.
    void invalidatePages(const GsPageSet &pages) {
        if (!patchingHostWrite)
            targets->invalidate(pages);
        if (textures)
            textures->invalidate(pages, GsTextureCache::InvalidationSource::HostWrite);
    }

    // -----------------------------------------------------------------
    // Primitive assembly
    // -----------------------------------------------------------------

    static GsGpuVertex makeVertex(const GSVertex &vertex,
                                  const GSDrawState &state,
                                  float offsetX,
                                  float offsetY,
                                  float depthScale) {
        GsGpuVertex out{};
        out.x = vertex.x - offsetX;
        out.y = vertex.y - offsetY;
        out.z = std::clamp(static_cast<float>(vertex.z * depthScale), 0.0f, 1.0f);
        out.q = vertex.q;
        if (state.prim.fst) {
            // UV is a 1/16-texel fixed-point pair.
            out.s = static_cast<float>(vertex.u) / 16.0f;
            out.t = static_cast<float>(vertex.v) / 16.0f;
        } else {
            out.s = vertex.s;
            out.t = vertex.t;
        }
        out.r = static_cast<float>(vertex.r) / 255.0f;
        out.g = static_cast<float>(vertex.g) / 255.0f;
        out.b = static_cast<float>(vertex.b) / 255.0f;
        out.a = static_cast<float>(vertex.a) / 255.0f;
        out.fog = static_cast<float>(vertex.fog) / 255.0f;
        return out;
    }

    // Expands one GS primitive into triangles. Points and lines become thin
    // quads rather than host points and lines, whose rasterisation rules and
    // width limits vary by driver.
    void appendPrimitive(const GSPrimitiveBatch &batch,
                         float offsetX,
                         float offsetY,
                         float depthScale) {
        const GSDrawState &state = batch.state;
        auto convert = [&](const GSVertex &vertex) {
            return makeVertex(vertex, state, offsetX, offsetY, depthScale);
        };
        auto quad = [&](GsGpuVertex a, GsGpuVertex b, GsGpuVertex c, GsGpuVertex d) {
            vertices.push_back(a);
            vertices.push_back(b);
            vertices.push_back(c);
            vertices.push_back(a);
            vertices.push_back(c);
            vertices.push_back(d);
        };

        switch (state.prim.type) {
        case GS_PRIM_POINT: {
            const GsGpuVertex v = convert(batch.vertices[0]);
            GsGpuVertex a = v, b = v, c = v, d = v;
            a.x = std::floor(v.x);
            a.y = std::floor(v.y);
            b.x = a.x + 1.0f;
            b.y = a.y;
            c.x = a.x + 1.0f;
            c.y = a.y + 1.0f;
            d.x = a.x;
            d.y = a.y + 1.0f;
            quad(a, b, c, d);
            break;
        }
        case GS_PRIM_LINE:
        case GS_PRIM_LINESTRIP: {
            const GsGpuVertex v0 = convert(batch.vertices[0]);
            const GsGpuVertex v1 = convert(batch.vertices[1]);
            float dx = v1.x - v0.x;
            float dy = v1.y - v0.y;
            const float length = std::sqrt(dx * dx + dy * dy);
            if (length < 1.0e-6f) {
                GsGpuVertex a = v0, b = v0, c = v0, d = v0;
                b.x += 1.0f;
                c.x += 1.0f;
                c.y += 1.0f;
                d.y += 1.0f;
                quad(a, b, c, d);
                break;
            }
            // Half a pixel either side of the centre line.
            const float nx = -dy / length * 0.5f;
            const float ny = dx / length * 0.5f;
            GsGpuVertex a = v0, b = v0, c = v1, d = v1;
            a.x = v0.x + nx;
            a.y = v0.y + ny;
            b.x = v0.x - nx;
            b.y = v0.y - ny;
            c.x = v1.x - nx;
            c.y = v1.y - ny;
            d.x = v1.x + nx;
            d.y = v1.y + ny;
            quad(a, b, c, d);
            break;
        }
        case GS_PRIM_TRIANGLE:
        case GS_PRIM_TRISTRIP:
        case GS_PRIM_TRIFAN:
            vertices.push_back(convert(batch.vertices[0]));
            vertices.push_back(convert(batch.vertices[1]));
            vertices.push_back(convert(batch.vertices[2]));
            break;
        case GS_PRIM_SPRITE: {
            const GsGpuVertex v0 = convert(batch.vertices[0]);
            const GsGpuVertex v1 = convert(batch.vertices[1]);
            // A sprite takes its colour, depth and fog from the second vertex;
            // only position and texture coordinates come from both.
            GsGpuVertex corner = v1;
            const float x0 = std::min(v0.x, v1.x);
            const float x1 = std::max(v0.x, v1.x);
            const float y0 = std::min(v0.y, v1.y);
            const float y1 = std::max(v0.y, v1.y);
            const float s0 = (v0.x <= v1.x) ? v0.s : v1.s;
            const float s1 = (v0.x <= v1.x) ? v1.s : v0.s;
            const float t0 = (v0.y <= v1.y) ? v0.t : v1.t;
            const float t1 = (v0.y <= v1.y) ? v1.t : v0.t;

            GsGpuVertex a = corner, b = corner, c = corner, d = corner;
            a.x = x0; a.y = y0; a.s = s0; a.t = t0;
            b.x = x1; b.y = y0; b.s = s1; b.t = t0;
            c.x = x1; c.y = y1; c.s = s1; c.t = t1;
            d.x = x0; d.y = y1; d.s = s0; d.t = t1;
            quad(a, b, c, d);
            break;
        }
        default:
            break;
        }
    }

    // -----------------------------------------------------------------
    // Batching
    // -----------------------------------------------------------------

    SDL_GPUTexture *feedbackTextureFor(uint32_t width, uint32_t height, std::string &error) {
        for (const FeedbackTexture &feedback : feedbackTextures)
            if (feedback.width == width && feedback.height == height)
                return feedback.texture;
        SDL_GPUTextureCreateInfo info{};
        info.type = SDL_GPU_TEXTURETYPE_2D;
        info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        info.width = width;
        info.height = height;
        info.layer_count_or_depth = 1u;
        info.num_levels = 1u;
        info.sample_count = SDL_GPU_SAMPLECOUNT_1;
        // A colour target too: a CT16 view's padding row is rendered into it
        // (snapshotFeedback).
        info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
        SDL_GPUTexture *texture = SDL_CreateGPUTexture(device.handle(), &info);
        if (!texture) {
            error = std::string("SDL_CreateGPUTexture(feedback): ") + SDL_GetError();
            return nullptr;
        }
        feedbackTextures.push_back({width, height, texture});
        return texture;
    }

    // Copies a surface into its snapshot texture as part of a flush, after
    // the batches recorded before it. With `region` (GS pixels), only that
    // part, at its place: a destination read samples no other (Rogue Galaxy:
    // added).
    SDL_GPUTexture *recordSnapshot(SDL_GPUCommandBuffer *&commands, const GsSurface &surface,
                                   const GsRegion *region = nullptr) {
        const uint32_t width = surface.width * surface.scale, height = surface.height * surface.scale;
        uint32_t x = 0u, y = 0u, copyWidth = width, copyHeight = height;
        if (region && !region->empty()) {
            x = std::min(region->x0 * surface.scale, width);
            y = std::min(region->y0 * surface.scale, height);
            copyWidth = std::min(region->x1 * surface.scale, width) - x;
            copyHeight = std::min(region->y1 * surface.scale, height) - y;
            if (copyWidth == 0u || copyHeight == 0u) {
                x = y = 0u;
                copyWidth = width;
                copyHeight = height;
            }
        }
        std::string error;
        SDL_GPUTexture *snapshot = feedbackTextureFor(width, height, error);
        if (!snapshot) {
            setError(std::move(error));
            return nullptr;
        }
        // The copy overwrites the snapshot in place: the GPU runs it after
        // the draws recorded before it, the ones that sampled the last copy
        // among them (SDL puts a barrier between), and the draw that samples
        // this one is recorded right after it. Cycled instead, each copy
        // recorded ahead of a submission kept its own storage, a whole
        // texture of the surface's size until the GPU was done with it:
        // about 110 copies a frame in Salgin's streets, so at 8x more than
        // the GPU's memory, and the frames paged in and out of it for up to
        // a second and a half (Rogue Galaxy: changed). A cycled copy counts
        // against the budget, and the pending work may go out here (the
        // caller is between passes).
        if (cycleSnapshots && !device.reservePending(uint64_t(copyWidth) * copyHeight * 4u, error)) {
            setError(std::move(error));
            return nullptr;
        }
        commands = device.pendingCommands(error);
        if (!commands) {
            setError(std::move(error));
            return nullptr;
        }
        SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(commands);
        if (!copy) {
            setError(std::string("SDL_BeginGPUCopyPass(snapshot): ") + SDL_GetError());
            return nullptr;
        }
        SDL_GPUTextureLocation source{}, destination{};
        source.texture = surface.texture;
        source.x = x;
        source.y = y;
        destination.texture = snapshot;
        destination.x = x;
        destination.y = y;
        SDL_CopyGPUTextureToTexture(copy, &source, &destination, copyWidth, copyHeight, 1u, cycleSnapshots);
        SDL_EndGPUCopyPass(copy);
        ++stats.feedbackCopies;
        return snapshot;
    }

    // Records the snapshot into the device's pending command buffer; the
    // caller flushes the draw that samples it right after, before another
    // snapshot into the same texture can get ahead of that draw.
    SDL_GPUTexture *snapshotFeedback(GsSurface &surface, std::string &error,
                                     const FeedbackPadding &padding) {
        if (!flushFor(stats.flushSnapshot) || !targets->refresh(surface, error))
            return nullptr;
        // Padding regions are in GS pixels; an upscaled snapshot holds each
        // of them as a scale x scale block, like the target's own pixels.
        const uint32_t scale = std::max(surface.scale, 1u);
        const uint32_t sourceWidth = surface.width * scale;
        const uint32_t sourceHeight = surface.height * scale;
        uint32_t width = sourceWidth, height = sourceHeight, uploadBytes = 0u;
        GsPageSet tailPages;
        for (const auto &region : padding.uploads) {
            width = std::max(width, region.x1 * scale);
            height = std::max(height, region.y1 * scale);
            uploadBytes += (region.width() * region.height() * scale * scale * 4u + 255u) & ~255u;
            gsMarkPages(tailPages, surface.base, surface.bufferWidth, surface.psm,
                        region.width(), region.height(), region.x0, region.y0);
        }
        // The padding's pages may be a live target's (Rogue Galaxy: the row
        // under the bloom buffer is the character buffer's last page row).
        // Then its texels come from that target's texture on the GPU, in
        // the copy pass below, instead of reading the target back to copy
        // them from local memory.
        // A CT16 view's padding row holds halfwords of that target's words:
        // the reinterpret shader makes it, from the page row copied into a
        // scratch laid out as this view's CT32 pages.
        const bool view16 = surface.psm == GS_PSM_CT16 || surface.psm == GS_PSM_CT16S;
        GsSurface *paddingSource = nullptr;
        // The tail pages a target owns come from its texture; the others (the
        // pixel past the last page, say) from local memory, uploaded first so
        // the copies land over it.
        GsPageSet gpuTail;
        if (uploadBytes != 0u && !noGpuPadding && (surface.psm == GS_PSM_CT32 || view16) &&
            (surface.base & 31u) == 0u && surface.width == surface.bufferWidth * 64u &&
            (!view16 || (!padding.rightColumn && padding.uploads[0].empty() && surface.height % 64u == 0u))) {
            for (uint32_t page = 0u; page < kGsPageCount; ++page) {
                if (!tailPages.test(page))
                    continue;
                GsPageSet one;
                one.set(page);
                if (targets->ownsAny(one))
                    gpuTail.set(page);
            }
            paddingSource = gpuTail.any() ? targets->nativeOwner(gpuTail, true) : nullptr;
            if (gsTargetTraceEnabled())
                std::fprintf(stderr, "[target] padding base=%05x psm=%02x %ux%u owner=%05x scale=%u/%u gpu=%zu/%zu\n",
                             surface.base, surface.psm, surface.width, surface.height,
                             paddingSource ? paddingSource->base : 0u, paddingSource ? paddingSource->scale : 0u, scale,
                             gpuTail.count(), tailPages.count());
            if (paddingSource && (paddingSource == &surface || paddingSource->scale != scale ||
                                  paddingSource->psm != GS_PSM_CT32 || (view16 && gpuTail != tailPages)))
                paddingSource = nullptr;
            if (paddingSource && view16 &&
                !ensurePaddingScratch(surface.width * scale, (surface.height / 2u + 32u) * scale, error))
                return nullptr;
            // Only the owner's cells the padding reads: its queued patches
            // elsewhere (texture uploads into its pages) can wait.
            if (paddingSource &&
                !targets->refreshRegion(*paddingSource, targets->regionForPages(*paddingSource, gpuTail), error))
                return nullptr;
        }
        const GsPageSet cpuTail = paddingSource ? tailPages & ~gpuTail : tailPages;
        SDL_GPUTransferBuffer *upload = nullptr;
        if (uploadBytes != 0u && cpuTail.any()) {
            if (gsTargetTraceEnabled() && !paddingSource)
                std::fprintf(stderr, "[target] padding-cpu base=%05x psm=%02x %ux%u fbw=%u right=%d up0=%ux%u up1=%ux%u\n",
                             surface.base, surface.psm, surface.width, surface.height, surface.bufferWidth,
                             padding.rightColumn, padding.uploads[0].width(), padding.uploads[0].height(),
                             padding.uploads[1].width(), padding.uploads[1].height());
            ResolveReasonScope scope(*targets, GsResolveReason::Feedback);
            if (!targets->resolve(cpuTail, error)) return nullptr;
            // One buffer, cycled on each map, instead of one created and
            // released per snapshot (Rogue Galaxy: added; SDL hands back a
            // free internal buffer while the last is still in flight).
            if (feedbackUploadCapacity < uploadBytes) {
                if (feedbackUpload)
                    SDL_ReleaseGPUTransferBuffer(device.handle(), feedbackUpload);
                feedbackUploadCapacity = 64u * 1024u;
                while (feedbackUploadCapacity < uploadBytes)
                    feedbackUploadCapacity *= 2u;
                SDL_GPUTransferBufferCreateInfo info{};
                info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
                info.size = feedbackUploadCapacity;
                feedbackUpload = SDL_CreateGPUTransferBuffer(device.handle(), &info);
                if (!feedbackUpload) {
                    feedbackUploadCapacity = 0u;
                    error = std::string("SDL_CreateGPUTransferBuffer(feedback tail): ") + SDL_GetError();
                    return nullptr;
                }
            }
            upload = feedbackUpload;
            auto *mapped = static_cast<uint8_t *>(SDL_MapGPUTransferBuffer(device.handle(), upload, true));
            if (!mapped) {
                error = std::string("SDL_MapGPUTransferBuffer(feedback tail): ") + SDL_GetError();
                return nullptr;
            }
            uint32_t offset = 0u;
            std::vector<uint32_t> wide;
            for (const auto &region : padding.uploads) {
                auto *pixels = reinterpret_cast<uint32_t *>(mapped + offset);
                const uint32_t rowPixels = region.width() * scale;
                wide.resize(rowPixels);
                for (uint32_t y = region.y0; y < region.y1; ++y) {
                    uint32_t *row = pixels + static_cast<size_t>(y - region.y0) * scale * rowPixels;
                    for (uint32_t x = region.x0; x < region.x1; ++x) {
                        uint32_t color = vram.read(surface.psm, surface.base, surface.bufferWidth, x, y);
                        if (surface.psm != GS_PSM_CT32) {
                            const uint32_t r = color & 31u, g = (color >> 5u) & 31u, b = (color >> 10u) & 31u;
                            color = ((r << 3u) | (r >> 2u)) | (((g << 3u) | (g >> 2u)) << 8u) |
                                    (((b << 3u) | (b >> 2u)) << 16u) | ((color & 0x8000u) << 16u);
                        }
                        for (uint32_t sx = 0u; sx < scale; ++sx)
                            wide[(x - region.x0) * scale + sx] = color;
                    }
                    // Built aside and only written out: the upload buffer is
                    // write-combined, and reading a row back out of it is slow.
                    for (uint32_t sy = 0u; sy < scale; ++sy)
                        std::memcpy(row + static_cast<size_t>(sy) * rowPixels, wide.data(), rowPixels * 4u);
                }
                offset += (region.width() * region.height() * scale * scale * 4u + 255u) & ~255u;
            }
            SDL_UnmapGPUTransferBuffer(device.handle(), upload);
        }
        SDL_GPUTexture *feedbackTexture = feedbackTextureFor(width, height, error);
        if (!feedbackTexture)
            return nullptr;
        // The copy below overwrites the snapshot in place (see recordSnapshot).
        if (cycleSnapshots && !device.reservePending(uint64_t(width) * height * 4u, error))
            return nullptr;
        SDL_GPUCommandBuffer *commands = device.pendingCommands(error);
        if (!commands)
            return nullptr;
        SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(commands);
        if (!copy) {
            error = std::string("SDL_BeginGPUCopyPass(feedback): ") + SDL_GetError();
            device.cancelPending();
            return nullptr;
        }
        SDL_GPUTextureLocation source{}, destination{};
        source.texture = surface.texture;
        destination.texture = feedbackTexture;
        SDL_CopyGPUTextureToTexture(copy, &source, &destination, sourceWidth, sourceHeight, 1u, cycleSnapshots);
        if (padding.rightColumn && sourceHeight > 32u * scale) {
            source.y = 32u * scale;
            destination.x = sourceWidth;
            SDL_CopyGPUTextureToTexture(copy, &source, &destination, scale, sourceHeight - 32u * scale, 1u, false);
        }
        if (upload) {
            // Local memory's padding first; the copies below land over it
            // where a target owns the page.
            uint32_t offset = 0u;
            for (const auto &region : padding.uploads) {
                if (region.empty()) continue;
                SDL_GPUTextureTransferInfo from{};
                from.transfer_buffer = upload;
                from.offset = offset;
                from.pixels_per_row = region.width() * scale;
                from.rows_per_layer = region.height() * scale;
                SDL_GPUTextureRegion to{};
                to.texture = feedbackTexture;
                to.x = region.x0 * scale;
                to.y = region.y0 * scale;
                to.w = region.width() * scale;
                to.h = region.height() * scale;
                to.d = 1u;
                // The preceding copy already cycled the snapshot's storage.
                SDL_UploadToGPUTexture(copy, &from, &to, false);
                offset += (region.width() * region.height() * scale * scale * 4u + 255u) & ~255u;
            }
        }
        if (paddingSource && view16) {
            // The CT32 page row under the view, into the scratch at its own
            // rows: the shader reads the padding row's halfwords from there.
            const uint32_t pageRow = surface.height / 64u;
            for (uint32_t column = 0u; column < surface.bufferWidth; ++column) {
                const uint32_t page = ((surface.base >> 5u) + pageRow * surface.bufferWidth + column) % kGsPageCount;
                const uint32_t relative = (page + kGsPageCount - (paddingSource->base >> 5u)) % kGsPageCount;
                SDL_GPUTextureLocation from{}, to{};
                from.texture = paddingSource->texture;
                from.x = (relative % paddingSource->bufferWidth) * 64u * scale;
                from.y = (relative / paddingSource->bufferWidth) * 32u * scale;
                to.texture = paddingScratch;
                to.x = column * 64u * scale;
                to.y = pageRow * 32u * scale;
                SDL_CopyGPUTextureToTexture(copy, &from, &to, 64u * scale, 32u * scale, 1u, false);
            }
        } else if (paddingSource) {
            // Each padding texel's page, through this surface's whole-page
            // CT32 layout, is a 64x32 cell of the source: a row segment
            // within one page is one row segment there.
            for (const auto &region : padding.uploads) {
                for (uint32_t y = region.y0; y < region.y1; ++y) {
                    for (uint32_t x = region.x0; x < region.x1;) {
                        const uint32_t segment = std::min(region.x1 - x, 64u - x % 64u);
                        const uint32_t page =
                            ((surface.base >> 5u) + (y / 32u) * surface.bufferWidth + x / 64u) % kGsPageCount;
                        if (!gpuTail.test(page)) {
                            x += segment;
                            continue;
                        }
                        const uint32_t relative = (page + kGsPageCount - (paddingSource->base >> 5u)) % kGsPageCount;
                        SDL_GPUTextureLocation from{}, to{};
                        from.texture = paddingSource->texture;
                        from.x = ((relative % paddingSource->bufferWidth) * 64u + x % 64u) * scale;
                        from.y = ((relative / paddingSource->bufferWidth) * 32u + y % 32u) * scale;
                        to.texture = feedbackTexture;
                        to.x = x * scale;
                        to.y = y * scale;
                        SDL_CopyGPUTextureToTexture(copy, &from, &to, segment * scale, scale, 1u, false);
                        x += segment;
                    }
                }
            }
        }
        SDL_EndGPUCopyPass(copy);
        if (paddingSource && view16) {
            const SDL_Rect row{0, int(surface.height * scale), int(surface.width * scale), int(scale)};
            if (!device.recordReinterpret(commands, paddingScratch, feedbackTexture, surface.width, height / scale,
                                          true, scale, &row, error)) {
                device.cancelPending();
                return nullptr;
            }
        }
        ++stats.feedbackCopies;
        return feedbackTexture;
    }

    // The scratch a CT16 view's padding row is reinterpreted from (Rogue
    // Galaxy: added): at least this big, RGBA8, sampled by the shader.
    SDL_GPUTexture *paddingScratch = nullptr;
    uint32_t paddingScratchWidth = 0u, paddingScratchHeight = 0u;
    bool ensurePaddingScratch(uint32_t width, uint32_t height, std::string &error) {
        if (paddingScratch && paddingScratchWidth >= width && paddingScratchHeight >= height)
            return true;
        if (paddingScratch)
            SDL_ReleaseGPUTexture(device.handle(), paddingScratch);
        paddingScratchWidth = std::max(width, paddingScratchWidth);
        paddingScratchHeight = std::max(height, paddingScratchHeight);
        SDL_GPUTextureCreateInfo info{};
        info.type = SDL_GPU_TEXTURETYPE_2D;
        info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        info.width = paddingScratchWidth;
        info.height = paddingScratchHeight;
        info.layer_count_or_depth = 1u;
        info.num_levels = 1u;
        info.sample_count = SDL_GPU_SAMPLECOUNT_1;
        info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
        paddingScratch = SDL_CreateGPUTexture(device.handle(), &info);
        if (!paddingScratch) {
            paddingScratchWidth = paddingScratchHeight = 0u;
            error = std::string("SDL_CreateGPUTexture(padding scratch): ") + SDL_GetError();
            return false;
        }
        return true;
    }

    // A glyph's border copies, marked by the host, stop here first
    // (setRegionReplacements): the first is drawn where the glyph goes from
    // the host's border version, and the others are left out; without one,
    // they are drawn as they are, at their own pixels. A marked shadow copy
    // is drawn from the shadow version where it goes, or as it is without
    // one (Rogue Galaxy: added).
    void submit(const GSPrimitiveBatch &batch) {
        if (regionPack && shadowCopy(batch)) {
            borderIndex = 0u;
            submitShadow(batch);
            return;
        }
        const int32_t marks = regionPack ? borderCopy(batch) : 0;
        if (marks == 0) {
            borderIndex = 0u;
            submitOne(batch);
            return;
        }
        GSPrimitiveBatch copy = batch;
        for (uint8_t i = 0u; i < 2u; ++i)
            copy.vertices[i].y += float(marks) / 16.0f;
        // A border has 4, 8 or 12 copies, all from the glyph's texels: one
        // from others starts a glyph of its own.
        GsRegion glyph;
        const bool sameGlyph = spriteTexelRect(copy, glyph) && glyph.x0 == borderGlyph.x0 &&
                               glyph.y0 == borderGlyph.y0 && glyph.x1 == borderGlyph.x1 && glyph.y1 == borderGlyph.y1;
        if (!sameGlyph)
            borderIndex = 0u;
        const uint32_t index = borderIndex++;
        if (index == 0u) {
            borderGlyph = glyph;
            borderRounded = false;
            GSPrimitiveBatch grown;
            GsRegion region;
            if (spriteTexelRect(copy, region) && !spriteSampledTexels(copy, borderShown))
                borderShown = region;
            if (borderQuad(copy, grown, 1.0f, marks == 3 ? 1.0f : 0.0f)) {
                // Its own batch, from the full path: the glyph before it has
                // the same state.
                last.valid = false;
                borderRequest = true;
                borderDrawn = false;
                submitOne(grown);
                borderRequest = false;
                last.valid = false;
                borderRounded = borderDrawn;
                if (borderRounded)
                    return;
            }
        } else if (borderRounded) {
            return;
        }
        submitOne(copy);
    }

    void submitShadow(const GSPrimitiveBatch &batch) {
        GSPrimitiveBatch copy = batch;
        for (uint8_t i = 0u; i < 2u; ++i)
            copy.vertices[i].y += 2.0f / 16.0f;
        GsRegion region;
        if (spriteTexelRect(copy, region) && !spriteSampledTexels(copy, borderShown))
            borderShown = region;
        GSPrimitiveBatch grown;
        if (borderQuad(copy, grown, 0.0f)) {
            last.valid = false;
            shadowRequest = true;
            borderDrawn = false;
            submitOne(grown);
            shadowRequest = false;
            last.valid = false;
            if (borderDrawn)
                return;
        }
        submitOne(copy);
    }

    void submitOne(const GSPrimitiveBatch &batch) {
        ++stats.primitivesSubmitted;
        // Numbered as a frame log numbers them (from the frame's start), so
        // a traced move can be put next to the batch that made it.
        if (gsTargetTraceEnabled())
            std::fprintf(stderr, "[target] batch %u fbp=%03x tbp=%05x\n", tracedBatch,
                         batch.state.context.frame.fbp, batch.state.context.tex0.tbp0);
        ++tracedBatch;
        noteTransferInterrupted();
        if (!device.valid() || !vram.attached() || batch.vertexCount == 0u)
            return;

        const GSDrawState &state = batch.state;
        const GSContext &context = state.context;
        if (context.scissor.x0 > context.scissor.x1 ||
            context.scissor.y0 > context.scissor.y1)
            return;
        if (!gsIsColorFramePsm(context.frame.psm))
            return;
        if (last.valid && std::memcmp(&state, &last.state, sizeof(state)) == 0 && submitSameState(batch))
            return;
        last.valid = false;

        GsRegion written = primitiveBounds(batch);
        if (written.empty())
            return;

        uint32_t neededWidth = static_cast<uint32_t>(context.scissor.x1) + 1u;
        const uint32_t neededHeight = static_cast<uint32_t>(context.scissor.y1) + 1u;
        // Scissors often end one pixel past the buffer (0..512 with FBW 8 in
        // Rogue Galaxy's menus). Growing the colour and depth surfaces for that
        // makes them alias the page after the buffer, and every texture upload
        // there then discarded the depth buffer mid-frame, so later geometry
        // showed through walls. Only geometry reaching past the buffer widens
        // them: a sprite or triangle edge at x == width covers no pixel there.
        const uint32_t bufferPixels = context.frame.fbw * 64u;
        if (neededWidth > bufferPixels && bufferPixels != 0u) {
            const float offsetX = static_cast<float>(context.xyoffset.ofx) / 16.0f;
            float maxX = -1.0f;
            for (uint8_t i = 0u; i < batch.vertexCount; ++i)
                maxX = std::max(maxX, batch.vertices[i].x - offsetX);
            const bool thin = state.prim.type == GS_PRIM_POINT || state.prim.type == GS_PRIM_LINE ||
                              state.prim.type == GS_PRIM_LINESTRIP;
            if (std::isfinite(maxX) && maxX + (thin ? 1.0f : 0.0f) <= static_cast<float>(bufferPixels))
                neededWidth = bufferPixels;
        }

        std::string error;
        GsSurface *color = targets->acquire(context.frame.fbp << 5u, context.frame.fbw,
                                            context.frame.psm, false, neededWidth,
                                            neededHeight, error);
        if (!color) {
            setError(std::move(error));
            return;
        }

        // Distinct FRAME views can name the same GS pages. Finish the old
        // view before importing its pixels into the new one.
        const bool pendingAlias = pendingDrawsTouchOthers(color->pages, color);
        if (pendingAlias && !flushFor(stats.flushAlias))
            return;
        const GsTestState test = gsDecodeTest(context.test);
        // A sprite that replaces every pixel of the surface (a clear) makes
        // whatever it held moot: nothing to import from aliased views or from
        // local memory. Rogue Galaxy clears its per-object offscreen buffer
        // before each object, and those buffers alias each other and the
        // pages its textures are uploaded to.
        const GsRegion overwritten = opaqueSpriteBounds(batch, *color, test);
        const bool clearsWhole =
            !noWholeClaim && opaqueSpriteBounds(batch, *color, test, true).contains(color->wholeRegion());
        if (clearsWhole) {
            targets->claimWhole(*color);
        } else {
            ResolveReasonScope scope(*targets, GsResolveReason::View);
            if (!targets->prepareColorView(*color, error)) {
                setError(std::move(error));
                return;
            }
        }
        // What lands outside the surface is not drawn (see neededWidth).
        written.x1 = std::min(written.x1, color->width);
        written.y1 = std::min(written.y1, color->height);
        if (written.empty())
            return;

        // Inactive depth must not grow/discard a live surface during colour-only passes.
        const bool wantsDepth = gsIsDepthPsm(context.zbuf.psm) && test.depthTestEnabled &&
            (!context.zbuf.zmask || test.depthTest != kDepthAlways);
        GsSurface *depth = nullptr;
        // A draw that ignores depth keeps the attachment of the draws around
        // it when their depth surface already exists at the size it needs:
        // tested ALWAYS and not written, it changes nothing, and the render
        // pass need not end for it. Rogue Galaxy clears each object's
        // offscreen buffer without depth, then draws the object with it.
        bool passiveDepth = false;
        if (wantsDepth) {
            depth = targets->acquire(context.zbuf.zbp << 5u, context.frame.fbw,
                                     context.zbuf.psm, true, neededWidth,
                                     neededHeight, error);
            if (!depth) {
                setError(std::move(error));
                return;
            }
        } else if (!noPassiveDepth && gsIsDepthPsm(context.zbuf.psm)) {
            depth = targets->findDepthSurface(context.zbuf.zbp << 5u, context.frame.fbw, context.zbuf.psm,
                                              neededWidth, neededHeight);
            passiveDepth = depth != nullptr;
        }

        DrawBatch draw{};
        draw.color = color;
        draw.depth = depth;
        draw.written = written;
        draw.overwritten = overwritten;
        gsMarkPages(draw.writtenPages, color->base, color->bufferWidth, color->psm,
                    written.width(), written.height(), written.x0, written.y0);

        GsAlphaState alpha = gsDecodeAlpha(context.alpha);
        // A constant factor leaves the shader free to store the original alpha.
        if (!device.framebufferFetch() && state.prim.abe && alpha.c == kBlendAlphaSource &&
            (!state.prim.tme || !context.tex0.tcc)) {
            const uint8_t a = batch.vertices[batch.vertexCount - 1u].a;
            const bool constant = !state.prim.iip || state.prim.type == GS_PRIM_SPRITE ||
                std::all_of(batch.vertices.begin(), batch.vertices.begin() + batch.vertexCount,
                            [a](const GSVertex &v) { return v.a == a; });
            if (constant && a <= 0x80u) {
                alpha.c = kBlendAlphaFixed;
                alpha.fix = a;
            }
        }
        const GsBlendTranslation blend = gsTranslateBlend(alpha, state.prim.abe);
        const bool framebufferFetch = device.framebufferFetch();
        // Without framebuffer fetch, a draw whose result depends on the
        // destination in a way fixed-function blending cannot carry (a
        // destination alpha test; a factor of destination alpha, /128 on the
        // GS and /255 here) reads it from a snapshot of its target, one per
        // primitive, and blends in the shader (Rogue Galaxy: added). Rogue
        // Galaxy draws about two a frame: the full-screen alpha test that
        // clears the depth of field's mask where it is unset, and the sun
        // flare's visibility, turned from alpha into colour in a 64x64 buffer.
        // Then the blends weighted past one, which a fixed-point attachment
        // clamps: FIX above 0x80, and vertex alpha above it where the texture
        // brings none (the town's additive sparkles and light shafts carry
        // up to 0xB4).
        bool overOne = blend.enabled && alpha.c == kBlendAlphaFixed && alpha.fix > 0x80u;
        if (blend.enabled && alpha.c == kBlendAlphaSource && (!state.prim.tme || !context.tex0.tcc))
            for (uint8_t index = 0u; index < batch.vertexCount && !overOne; ++index)
                overOne = batch.vertices[index].a > 0x80u;
        const bool destinationRead = !framebufferFetch && device.destinationCopy() &&
            (test.destinationAlphaTestEnabled || (blend.enabled && alpha.c == kBlendAlphaDest) || overOne);
        if (!framebufferFetch && !destinationRead && blend.enabled && !blend.exact)
            ++stats.inexactBlends;
        if (!framebufferFetch && !destinationRead && state.colclamp == 0u)
            ++stats.disabledColorClamps;
        if (!framebufferFetch && !destinationRead && test.destinationAlphaTestEnabled)
            ++stats.destinationAlphaTests;
        if (test.alphaTestEnabled && test.alphaFail != kAfailKeep)
            ++stats.alphaFailModes;

        bool partialMask = false;
        uint8_t writeMask = colorWriteMaskFromFbmsk(context.frame.fbmsk, partialMask);
        if (!framebufferFetch && !destinationRead && partialMask)
            ++stats.partialChannelMasks;

        // The GS blend factor is A/128 while the host reads the emitted alpha
        // as A/255, so the shader scales it. That makes the stored alpha wrong,
        // which only matters if the alpha channel is written at all -- so it is
        // dropped from the write mask instead. Framebuffer fetch avoids this
        // fallback limitation, including later effects which sample alpha.
        if (!framebufferFetch && !destinationRead && blend.enabled && alpha.c == kBlendAlphaDest)
            ++stats.destinationAlphaFactors;
        const bool scaleAlpha = !framebufferFetch && !destinationRead && blend.enabled &&
                                alpha.c == kBlendAlphaSource;
        // Those draws still write their alpha on the GS, and games read it
        // back: DQ8 draws its characters into a layer whose alpha becomes the
        // shadow mask. A second pass over the same primitives stores it.
        const bool alphaPass = scaleAlpha && (writeMask & SDL_GPU_COLORCOMPONENT_A) != 0u;
        if (scaleAlpha) {
            writeMask &= static_cast<uint8_t>(~SDL_GPU_COLORCOMPONENT_A);
            // Alpha above 0x80 asks for a blend factor greater than one, which
            // a fixed-point attachment cannot carry.
            for (uint8_t index = 0u; index < batch.vertexCount; ++index) {
                if (batch.vertices[index].a > 0x80u) {
                    ++stats.saturatedBlendFactors;
                    break;
                }
            }
        }

        draw.pipeline.colorFormat = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        draw.pipeline.depthFormat = device.depthFormat();
        draw.pipeline.hasDepth = depth != nullptr ? 1u : 0u;
        draw.pipeline.depthCompare = passiveDepth
            ? static_cast<uint8_t>(SDL_GPU_COMPAREOP_ALWAYS)
            : static_cast<uint8_t>(gsTranslateDepthCompare(test.depthTest, test.depthTestEnabled));
        draw.pipeline.depthWrite = (depth != nullptr && !passiveDepth && !context.zbuf.zmask) ? 1u : 0u;
        draw.pipeline.colorWriteMask = writeMask;
        const bool fixedBlend = !framebufferFetch && !destinationRead;
        draw.pipeline.blendEnabled = fixedBlend && blend.enabled ? 1u : 0u;
        draw.pipeline.destinationCopy = destinationRead ? 1u : 0u;
        draw.pipeline.sourceFactor = static_cast<uint8_t>(blend.sourceFactor);
        draw.pipeline.destinationFactor = static_cast<uint8_t>(blend.destinationFactor);
        draw.pipeline.blendOperation = static_cast<uint8_t>(blend.operation);
        draw.usesBlendConstant = fixedBlend && blend.usesConstant;
        draw.blendConstant = fixedBlend ? blend.constant : 0.0f;
        draw.fragmentUniforms.blend[0] = uint32_t(context.alpha & 0xffu) |
            (uint32_t((context.alpha >> 32u) & 0xffu) << 8u);
        draw.fragmentUniforms.blend[1] = context.frame.fbmsk;
        draw.fragmentUniforms.blend[2] = uint32_t(context.test);
        draw.fragmentUniforms.blend[3] = (state.prim.abe ? 1u : 0u) |
            (state.colclamp ? 2u : 0u) | (state.pabe ? 4u : 0u) |
            ((context.fba & 1u) ? 8u : 0u) | (uint32_t(context.frame.psm) << 8u);

        draw.vertexUniforms.targetSize[0] = static_cast<float>(color->width);
        draw.vertexUniforms.targetSize[1] = static_cast<float>(color->height);
        draw.vertexUniforms.targetSize[2] = 1.0f / static_cast<float>(color->width);
        draw.vertexUniforms.targetSize[3] = 1.0f / static_cast<float>(color->height);
        // In GS pixels, before the viewport scales them: half a host pixel,
        // so GS pixel p covers exactly host pixels [p*scale, (p+1)*scale) and
        // its sample point lands on the first of them. Half a GS pixel would
        // be right only at 1x; above it every primitive, and every copy DQ8
        // makes of the frame, sat half a native pixel off.
        const float sampleOffset = kSampleOffset / static_cast<float>(std::max(color->scale, 1u));
        draw.vertexUniforms.adjust[0] = sampleOffset;
        draw.vertexUniforms.adjust[1] = sampleOffset;
        draw.vertexUniforms.adjust[2] = kClipYDirection;

        uint32_t control = 0u;
        if (state.prim.fge)
            control |= kFragFlagFge;
        if (test.alphaTestEnabled)
            control |= kFragFlagAte;

        float textureWidth = 1.0f;
        float textureHeight = 1.0f;
        bool feedbackRecorded = false;
        GsClampState clampState{};
        draw.texture = dummyTexture;
        float textureScale = 1.0f;
        // For `last`: the texture came from the cache, and from these pages.
        bool plainTexture = false;
        SDL_GPUTexture *plainTextureHandle = nullptr;
        GsPageSet plainSourcePages;
        GsRegion plainReach;
        bool plainBounded = false;
        // Drawn from the host's larger texture, at this many texels per GS
        // texel (Rogue Galaxy: added; setRegionReplacements).
        SDL_GPUTexture *largerTexture = nullptr;
        bool largerNearest = false;
        // A whole texture the host has a larger version of, and its factor.
        uint32_t wholeScale = 0u;
        bool wholeNearest = false;
        uint32_t largerScale = 0u;
        GsRegion largerArea;
        if (state.prim.tme) {
            ++stats.texturedPrimitives;

            // A texture read of a region a render target already holds can
            // bind that target instead of resolving it to local memory and
            // rebuilding a native-resolution copy. It is the common case for
            // render-to-texture, and at a render scale above 1 it is also the
            // more accurate one: the copy would be downsampled.
            //
            // Not when it is the target being drawn into -- reading and
            // writing one attachment in a single draw is undefined.
            SDL_GPUTexture *texture = nullptr;
            GsSurface *sampled = nullptr;
            FeedbackPadding feedbackPadding;
            const GsPageSet sourcePages = textures->sourcePagesFor(state);
            plainSourcePages = sourcePages;
            // Only the pages this primitive can sample count against queued
            // draws (sampledSourcePages).
            GsPageSet sampledPages = sampledSourcePages(batch, sourcePages);
            // A live target that owes nothing to local memory needs no flush:
            // draws queued into it stay ahead of this one within the flush. The
            // target being drawn into is read through a snapshot taken there.
            GsSurface *const candidate = gsIsIndexedPsm(context.tex0.psm) ? nullptr
                : targets->sampleCandidate(context.tex0.tbp0, context.tex0.psm, context.tex0.tbw);
            // The sampled rectangle carries a texel of slack past the last
            // row and column, which for a texture the size of a buffer lands
            // in the next buffer's pages: queued draws there (the back buffer
            // being drawn while the frame buffer is read in strips, the next
            // object buffer while one is read) forced a flush per strip, 55
            // of the streets' 63 texture-read flushes a frame. A draw that
            // samples within the live target reads only its pages (Rogue
            // Galaxy: added).
            if (candidate && samplesFitSurface(batch, *candidate))
                sampledPages &= candidate->pages;
            // A candidate behind only on host writes patched into it is
            // brought up to date here instead: the patches record before
            // the queued draws, which the transfer-conflict rule kept off
            // their pages, so the order holds, and the draw samples the live
            // target. Otherwise this was a flush and a texture rebuilt from
            // local memory: 41 of the streets' 46 texture-read flushes a
            // frame (Rogue Galaxy: added).
            if (candidate && !candidate->cpuPatches.empty() && candidate->needsUpload.empty() &&
                !pendingDrawsTouchOthers(sampledPages, candidate)) {
                std::string patchError;
                if (!targets->settlePatches(*candidate, patchError))
                    setError(std::move(patchError));
            }
            const bool inOrder = candidate && candidate->cpuPatches.empty() && candidate->needsUpload.empty() &&
                                 !pendingDrawsTouchOthers(sampledPages, candidate);
            const bool readsItself = inOrder && candidate == color &&
                canSnapshotFeedback(batch, *color, feedbackPadding) &&
                feedbackPadding.uploads[0].empty() && feedbackPadding.uploads[1].empty();
            if (inOrder && candidate != color && samplesFitSurface(batch, *candidate)) {
                sampled = candidate;
            } else if (!readsItself) {
                // Trace only: the state before the flush below changes it.
                const bool othersBefore = candidate && pendingDrawsTouchOthers(sampledPages, candidate);
                GsRegion traceReach{};
                const bool traceBounded = gsTargetTraceEnabled() && sampledTexelRect(batch, traceReach);
                if (pendingDrawsTouch(sampledPages) && !flushFor(stats.flushTexRead))
                    return;
                if (!gsIsIndexedPsm(context.tex0.psm)) {
                    if (gsTargetTraceEnabled())
                        std::fprintf(stderr, "[target] texread tbp=%05x tbw=%u psm=%02x %ux%u -> fbp=%05x psm=%02x"
                                             " candidate=%d patches=%zu upload=%d others=%d/%d fits=%d reach=%d,%d-%d,%d/%d surface=%ux%u\n",
                                     context.tex0.tbp0, context.tex0.tbw, context.tex0.psm, 1u << context.tex0.tw,
                                     1u << context.tex0.th, color->base, color->psm, candidate != nullptr,
                                     candidate ? candidate->cpuPatches.size() : size_t(0),
                                     candidate && !candidate->needsUpload.empty() ? 1 : 0,
                                     othersBefore ? 1 : 0, candidate && pendingDrawsTouchOthers(sampledPages, candidate) ? 1 : 0,
                                     candidate && samplesFitSurface(batch, *candidate) ? 1 : 0, traceReach.x0, traceReach.y0,
                                     traceReach.x1, traceReach.y1, traceBounded ? 1 : 0, candidate ? candidate->width : 0u,
                                     candidate ? candidate->height : 0u);
                    ResolveReasonScope scope(*targets, GsResolveReason::Sample);
                    sampled = targets->findSampleSource(context.tex0.tbp0, context.tex0.psm,
                                                        context.tex0.tbw);
                }
            }
            const bool directSample = sampled && sampled != color && samplesFitSurface(batch, *sampled);
            const bool copyFeedback = sampled && !directSample &&
                canSnapshotFeedback(batch, *sampled, feedbackPadding);
            // Live targets have not passed through texture-cache TEXA expansion.
            const auto liveTargetTexa = [&] {
                if (context.tex0.psm == GS_PSM_CT24 || context.tex0.psm == GS_PSM_CT16 ||
                    context.tex0.psm == GS_PSM_CT16S) {
                    draw.fragmentUniforms.misc[2] = context.tex0.psm == GS_PSM_CT24 ? 1.0f : 2.0f;
                    draw.fragmentUniforms.misc[3] = static_cast<float>(
                        uint32_t(state.texa.ta0) | (uint32_t(state.texa.ta1) << 8u) |
                        (state.texa.aem ? 0x10000u : 0u));
                }
            };
            if (readsItself) {
                ++stats.textureFeedbackHazards;
                texture = feedbackTextureFor(color->width * color->scale, color->height * color->scale, error);
                if (!texture) {
                    setError(std::move(error));
                    return;
                }
                // The strips of one blur or channel-shuffle pass mostly read
                // pixels no earlier strip of the pass has drawn, so they can
                // share one snapshot, and then one draw call.
                draw.feedback = true;
                draw.snapshot = batches.empty() || !batches.back().feedback || batches.back().color != color ||
                                sampledTexels(batch, *color).intersects(feedbackWritten);
                textureScale = static_cast<float>(color->scale);
                liveTargetTexa();
            } else if (directSample) {
                // The frame copied into a displayed buffer (Rogue Galaxy:
                // added; frameTarget).
                if (displayed(*color) && sampled->width >= 512u)
                    frameSourceBase = sampled->base;
                texture = sampled->texture;
                draw.source = sampled;
                textureScale = static_cast<float>(sampled->scale);
                ++stats.texturesFromLiveTargets;
                liveTargetTexa();
            } else if (copyFeedback) {
                if (sampled == color)
                    ++stats.textureFeedbackHazards;
                else
                    ++stats.texturesFromLiveTargets;
                texture = snapshotFeedback(*sampled, error, feedbackPadding);
                feedbackRecorded = texture != nullptr;
                if (!texture) {
                    if (!error.empty())
                        setError(std::move(error));
                    return;
                }
                textureScale = static_cast<float>(sampled->scale);
                liveTargetTexa();
            } else {
                if (sampled == color && sampled != nullptr)
                    ++stats.textureFeedbackHazards;

                // Two reasons a queued batch has to land before a texture is
                // built. Building one reads local memory, so anything still
                // queued that draws into the region it reads owes it content.
                // And a full cache clears itself on the next miss, which would
                // leave every queued batch holding a released texture.
                if (!batches.empty() &&
                    (textures->evictionImminent() || pendingDrawsTouch(sampledPages))) {
                    if (!flushFor(stats.flushTexRead))
                        return;
                }

                if (gsTargetTraceEnabled())
                    std::fprintf(stderr, "[target] texbuild tbp=%05x tbw=%u psm=%02x %ux%u cbp=%05x -> fbp=%05x psm=%02x\n",
                                 context.tex0.tbp0, context.tex0.tbw, context.tex0.psm, 1u << context.tex0.tw,
                                 1u << context.tex0.th, context.tex0.cbp, color->base, color->psm);
                std::string textureError;
                ResolveReasonScope scope(*targets, GsResolveReason::Texture);
                GsRegion reach;
                const bool bounded = sampledTexelRect(batch, reach);
                texture = textures->acquire(state, textureError, bounded ? &reach : nullptr);
                if (!texture && !textureError.empty())
                    setError(std::move(textureError));
                plainTexture = true;
                plainTextureHandle = texture;
                plainReach = reach;
                plainBounded = bounded;
                if (texture && regionPack) {
                    largerTexture = largerRegion(batch, *color, borderRequest, shadowRequest, largerScale,
                                                 largerNearest, largerArea);
                    if (largerTexture)
                        texture = largerTexture;
                }
                if (texture && regionPack && !largerTexture && state.prim.tme) {
                    std::string wholeError;
                    if (SDL_GPUTexture *whole =
                            textures->wholeReplacement(state, wholeScale, wholeNearest, wholeError)) {
                        texture = whole;
                        ++stats.regionsReplaced;
                    } else {
                        wholeScale = 0u;
                    }
                }
            }
            // A border's or a shadow's quad is drawn from its version alone;
            // the caller draws the copies instead.
            const bool marginVersion = borderRequest || shadowRequest;
            if (marginVersion && !largerTexture)
                return;

            if (texture) {
                control |= kFragFlagTme;
                if (state.prim.fst)
                    control |= kFragFlagFst;
                if (context.tex0.tcc != 0u)
                    control |= kFragFlagTcc;
                // The host's larger version is filtered across its own
                // texels, as an upscaled target is (Rogue Galaxy: added).
                // Unless the host asked for its texels as they are (a pack of
                // pixel art: kRegionNearest).
                if (largerTexture ? !largerNearest : wholeScale ? !wholeNearest : state.linearFilter)
                    control |= kFragFlagLinear;
                if (largerTexture)
                    textureScale = static_cast<float>(largerScale);
                else if (wholeScale)
                    textureScale = static_cast<float>(wholeScale);
                // A sprite's UVs can span a texel more than its pixels (DQ8's
                // font: 23 texels over 22 pixels), so the GS never reaches the
                // last one. Sampling between native pixels would, and pull in
                // the neighbouring glyph as thin lines and dots.
                // Draws into a CT16 view are channel shuffles: bit-exact
                // copies of reinterpreted pixels. Filtering between native
                // pixels would blend neighbouring halfwords, so they sample
                // on the native grid too, bilinear or not.
                if (state.prim.fst && color->scale > 1u && !largerTexture && !wholeScale &&
                    (!state.linearFilter || color->psm == GS_PSM_CT16 || color->psm == GS_PSM_CT16S))
                    control |= kFragFlagNativeGrid |
                               (std::min<uint32_t>(color->scale, 15u) << kFragTargetScaleShift);
                textureWidth = static_cast<float>(std::max<uint16_t>(state.textureWidth, 1u));
                textureHeight = static_cast<float>(std::max<uint16_t>(state.textureHeight, 1u));
                clampState = gsDecodeClamp(context.clamp);
                // The host's version is the region alone (its border version
                // with a margin about it), clamped to the texels the GS samples
                // from it: no tap reaches past them (Rogue Galaxy: added).
                if (largerTexture && marginVersion) {
                    textureWidth = static_cast<float>(largerArea.width() + 2u * kRegionBorderMargin);
                    textureHeight = static_cast<float>(largerArea.height() + 2u * kRegionBorderMargin);
                    clampState = borderClamp(largerArea);
                } else if (largerTexture) {
                    textureWidth = static_cast<float>(largerArea.width());
                    textureHeight = static_cast<float>(largerArea.height());
                    clampState = largerClamp(largerArea);
                }
                draw.texture = texture;
            } else {
                // A texture that cannot be built leaves the primitive drawing
                // with vertex colour alone rather than dropping it, so the
                // geometry stays visible and the count says how often.
                ++stats.untranslatedTextures;
            }
        }

        draw.fragmentUniforms.control[0] = control;
        draw.fragmentUniforms.control[1] = static_cast<uint32_t>(test.alphaTest);
        draw.fragmentUniforms.control[2] = context.tex0.tfx;
        draw.fragmentUniforms.control[3] = static_cast<uint32_t>(clampState.wrapU) |
                                           (static_cast<uint32_t>(clampState.wrapV) << 2u);
        draw.fragmentUniforms.fog[0] = static_cast<float>(state.fogR) / 255.0f;
        draw.fragmentUniforms.fog[1] = static_cast<float>(state.fogG) / 255.0f;
        draw.fragmentUniforms.fog[2] = static_cast<float>(state.fogB) / 255.0f;
        draw.fragmentUniforms.fog[3] = static_cast<float>(test.alphaReference) / 255.0f;
        draw.fragmentUniforms.textureSize[0] = textureWidth;
        draw.fragmentUniforms.textureSize[1] = textureHeight;
        draw.fragmentUniforms.textureSize[2] = 1.0f / textureWidth;
        draw.fragmentUniforms.textureSize[3] = 1.0f / textureHeight;
        draw.fragmentUniforms.region[0] = static_cast<float>(clampState.minU);
        draw.fragmentUniforms.region[1] = static_cast<float>(clampState.maxU);
        draw.fragmentUniforms.region[2] = static_cast<float>(clampState.minV);
        draw.fragmentUniforms.region[3] = static_cast<float>(clampState.maxV);
        draw.fragmentUniforms.misc[0] = scaleAlpha ? (255.0f / 128.0f) : 1.0f;
        draw.fragmentUniforms.misc[1] = textureScale;
        if (destinationRead) {
            // A snapshot per primitive: the GS reads what the one before
            // left.
            draw.destination = true;
            draw.snapshot = true;
            ++stats.destinationReads;
        }

        const uint32_t scissorScale = color->scale;
        // Within the surface: a scissor past the buffer no longer widens it
        // (see neededWidth), and a host scissor past the attachment is invalid.
        const uint32_t scissorX1 = std::min<uint32_t>(context.scissor.x1 + 1u, color->width);
        const uint32_t scissorY1 = std::min<uint32_t>(context.scissor.y1 + 1u, color->height);
        draw.scissor.x = static_cast<int>(context.scissor.x0 * scissorScale);
        draw.scissor.y = static_cast<int>(context.scissor.y0 * scissorScale);
        draw.scissor.w = static_cast<int>((scissorX1 - std::min<uint32_t>(context.scissor.x0, scissorX1)) * scissorScale);
        draw.scissor.h = static_cast<int>((scissorY1 - std::min<uint32_t>(context.scissor.y0, scissorY1)) * scissorScale);

        const float offsetX = static_cast<float>(context.xyoffset.ofx) / 16.0f;
        const float offsetY = static_cast<float>(context.xyoffset.ofy) / 16.0f;
        const float depthScale =
            1.0f / static_cast<float>(depthMaximumForPsm(context.zbuf.psm));

        const uint32_t firstVertex = static_cast<uint32_t>(vertices.size());
        appendPrimitive(batch, offsetX, offsetY, depthScale);
        const uint32_t addedVertices = static_cast<uint32_t>(vertices.size()) - firstVertex;
        if (addedVertices == 0u)
            return;
        if (largerTexture && (borderRequest || shadowRequest)) {
            toBorderTexels(firstVertex, largerArea);
            borderDrawn = true;
            ++(shadowRequest ? stats.shadowsDrawn : stats.bordersRounded);
        } else if (largerTexture) {
            toLargerTexels(firstVertex, largerArea);
            ++stats.regionsReplaced;
        }

        ++stats.primitivesDrawn;
        stats.trianglesDrawn += addedVertices / 3u;

        // The alpha half of a source-alpha blend: the same primitives with
        // blending off, writing exact alpha only, where the colour pass wrote.
        // Colour blending here never reads destination alpha, so drawing all
        // the colour and then all the alpha matches the GS's per-primitive
        // order; with depth write off and the test widened to equality, each
        // pixel takes the alpha of the primitive that won it.
        DrawBatch alphaDraw;
        if (alphaPass) {
            alphaDraw = draw;
            alphaDraw.pipeline.blendEnabled = 0u;
            alphaDraw.pipeline.colorWriteMask = SDL_GPU_COLORCOMPONENT_A;
            alphaDraw.pipeline.depthWrite = 0u;
            if (alphaDraw.pipeline.depthCompare == SDL_GPU_COMPAREOP_GREATER) {
                alphaDraw.pipeline.depthCompare = SDL_GPU_COMPAREOP_GREATER_OR_EQUAL;
                ++stats.widenedAlphaReplays;
            } else if (alphaDraw.pipeline.depthCompare == SDL_GPU_COMPAREOP_LESS) {
                alphaDraw.pipeline.depthCompare = SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
                ++stats.widenedAlphaReplays;
            }
            alphaDraw.usesBlendConstant = false;
            alphaDraw.blendConstant = 0.0f;
            alphaDraw.fragmentUniforms.misc[0] = 1.0f;
            alphaDraw.overwritten = {};
            alphaDraw.snapshot = false;
        }

        // Merge into the open batch when nothing about the state changed. The
        // frontend hands over one primitive at a time, so without this every
        // triangle would be its own draw call. A colour/alpha pair grows as a
        // pair.
        const size_t open = batches.size();
        bool pushedNew = false;
        if (alphaPass && open >= 2u && batches[open - 2u].sameStateAs(draw) &&
            batches[open - 1u].sameStateAs(alphaDraw)) {
            for (size_t index = open - 2u; index < open; ++index) {
                batches[index].vertexCount += addedVertices;
                batches[index].written.merge(draw.written);
                batches[index].writtenPages |= draw.writtenPages;
            }
            notePending(draw.color, draw.writtenPages);
        } else if (!alphaPass && open >= 1u && batches.back().sameStateAs(draw)) {
            batches.back().vertexCount += addedVertices;
            batches.back().written.merge(draw.written);
            batches.back().writtenPages |= draw.writtenPages;
            notePending(draw.color, draw.writtenPages);
        } else {
            pushedNew = true;
            notePending(draw.color, draw.writtenPages);
            noteSampled(draw);
            draw.firstVertex = firstVertex;
            draw.vertexCount = addedVertices;
            batches.push_back(draw);
            ++stats.batches;
            if (alphaPass) {
                alphaDraw.firstVertex = firstVertex;
                alphaDraw.vertexCount = addedVertices;
                batches.push_back(alphaDraw);
                ++stats.batches;
            }
        }
        // What the next primitive may reuse (submitSameState). A scissor
        // past the buffer decides per vertex whether the surface widens.
        const bool widens = static_cast<uint32_t>(context.scissor.x1) + 1u > bufferPixels && bufferPixels != 0u;
        last.valid = !feedbackRecorded && !draw.feedback && !draw.destination && !draw.source && !clearsWhole &&
                     !widens &&
                     (!state.prim.tme || plainTexture);
        if (last.valid) {
            std::memcpy(&last.state, &state, sizeof(last.state));
            last.color = color;
            last.alphaPass = alphaPass;
            last.textured = state.prim.tme;
            last.texture = plainTextureHandle;
            last.larger = largerTexture;
            last.sourcePages = plainSourcePages;
            last.offsetX = offsetX;
            last.offsetY = offsetY;
            last.depthScale = depthScale;
            last.boxPages = pushedNew;
            last.reach = plainBounded ? plainReach : GsRegion{};
            last.reachAll = state.prim.tme && plainTexture && !plainBounded;
        }
        if (draw.snapshot)
            feedbackWritten.clear();
        if (draw.feedback)
            feedbackWritten.merge(drawnPixels(batch, *color));
        // Copy and consume the snapshot before another snapshot into the same
        // texture can get ahead of the draw.
        if (feedbackRecorded)
            flushFor(stats.flushFeedback);
    }

    // -----------------------------------------------------------------
    // Submission
    // -----------------------------------------------------------------

    bool ensureVertexBuffer(uint32_t requiredBytes) {
        if (vertexBuffer && vertexBufferCapacity >= requiredBytes)
            return true;
        if (vertexBuffer)
            SDL_ReleaseGPUBuffer(device.handle(), vertexBuffer);
        vertexBuffer = nullptr;

        uint32_t capacity = std::max<uint32_t>(requiredBytes, 64u * 1024u);
        // Round up so a slowly growing scene does not reallocate every frame.
        capacity = (capacity + 0xffffu) & ~0xffffu;
        SDL_GPUBufferCreateInfo info{};
        info.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
        info.size = capacity;
        vertexBuffer = SDL_CreateGPUBuffer(device.handle(), &info);
        if (!vertexBuffer)
            return setError(std::string("SDL_CreateGPUBuffer(vertices): ") + SDL_GetError());
        vertexBufferCapacity = capacity;
        return true;
    }

    bool uploadVertices(SDL_GPUCommandBuffer *commands) {
        const uint32_t bytes =
            static_cast<uint32_t>(vertices.size() * sizeof(GsGpuVertex));
        if (bytes == 0u)
            return true;
        uint32_t offset = vertexBase * static_cast<uint32_t>(sizeof(GsGpuVertex));
        if (!vertexBuffer || vertexBufferCapacity < offset + bytes) {
            // Start over at the front of a fresh backing (cycled below): the
            // draws recorded so far keep the one they bound.
            if (!ensureVertexBuffer(bytes))
                return false;
            offset = 0u;
            vertexBase = 0u;
        }

        std::string error;
        SDL_GPUTransferBuffer *upload = vertexUploads.take(device, bytes, error);
        if (!upload)
            return setError(std::move(error));
        void *mapped = SDL_MapGPUTransferBuffer(device.handle(), upload, false);
        if (!mapped)
            return setError(std::string("SDL_MapGPUTransferBuffer(vertices): ") + SDL_GetError());
        std::memcpy(mapped, vertices.data(), bytes);
        SDL_UnmapGPUTransferBuffer(device.handle(), upload);

        SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(commands);
        if (!copy)
            return setError(std::string("SDL_BeginGPUCopyPass(vertices): ") + SDL_GetError());
        const SDL_GPUTransferBufferLocation source{upload, 0u};
        const SDL_GPUBufferRegion destination{vertexBuffer, offset, bytes};
        // The first flush of a submission cycles storage the last
        // submission's draws may still read; the later ones append to it.
        SDL_UploadToGPUBuffer(copy, &source, &destination, vertexBase == 0u);
        SDL_EndGPUCopyPass(copy);
        vertexUploads.used(upload, device.pendingSerial());
        return true;
    }

    bool flushFor(uint64_t &reason) {
        if (!batches.empty())
            ++reason;
        return flushDraws();
    }

    bool flushDraws() {
        last.valid = false;
        if (batches.empty()) {
            vertices.clear();
            return true;
        }
        if (!device.valid())
            return setError("SDL GPU device is not available");
        // What was recorded under another serial has gone out: its vertices
        // are the GPU's, and this submission starts at the buffer's front.
        if (vertexSerial != device.pendingSerial()) {
            vertexSerial = device.pendingSerial();
            vertexBase = 0u;
            recordedDraws = 0u;
        }

        // Every target must agree with local memory before it is drawn into.
        std::string error;
        for (DrawBatch &batch : batches) {
            // The first queued primitive can replace every pixel an upload would supply.
            if (batch.color && batch.overwritten.contains(batch.color->needsUpload))
                batch.color->needsUpload.clear();
            if (batch.color && !targets->refresh(*batch.color, error))
                return setError(std::move(error));
            // A sampled target that grew since the batch was queued reloads here too.
            if (batch.source && !targets->refresh(*batch.source, error))
                return setError(std::move(error));
        }

        // The device's pending command buffer: the uploads, patches, imports
        // and expansions recorded since the last submission go out ahead of
        // these draws, in the one submission.
        SDL_GPUCommandBuffer *commands = device.pendingCommands(error);
        if (!commands)
            return setError(std::move(error));
        // Keep the upload ordered with its consumers without another submission.
        if (!uploadVertices(commands)) {
            return false;
        }

        SDL_GPURenderPass *pass = nullptr;
        GsSurface *activeColor = nullptr;
        GsSurface *activeDepth = nullptr;
        GsPageSet drawnPages;

        auto endPass = [&]() {
            if (pass) {
                SDL_EndGPURenderPass(pass);
                pass = nullptr;
            }
        };

        for (const DrawBatch &batch : batches) {
            SDL_GPUTexture *sampledTexture = batch.source ? batch.source->texture : batch.texture;
            SDL_GPUTexture *destinationTexture = nullptr;
            if (batch.snapshot) {
                endPass();
                SDL_GPUTexture *snapshot =
                    recordSnapshot(commands, *batch.color,
                                   batch.destination && !batch.feedback ? &batch.written : nullptr);
                if (!snapshot)
                    return false;
                if (batch.feedback || !batch.destination)
                    sampledTexture = snapshot;
                if (batch.destination)
                    destinationTexture = snapshot;
            }
            if (!pass || batch.color != activeColor || batch.depth != activeDepth) {
                endPass();
                SDL_GPUColorTargetInfo colorInfo{};
                colorInfo.texture = batch.color->texture;
                colorInfo.load_op = SDL_GPU_LOADOP_LOAD;
                colorInfo.store_op = SDL_GPU_STOREOP_STORE;

                SDL_GPUDepthStencilTargetInfo depthInfo{};
                if (batch.depth) {
                    depthInfo.texture = batch.depth->texture;
                    // A depth surface is never read back from local memory, so
                    // the first pass that touches one starts it at the far
                    // plane: GS Z counts up towards the viewer.
                    depthInfo.load_op = batch.depth->undefined ? SDL_GPU_LOADOP_CLEAR
                                                               : SDL_GPU_LOADOP_LOAD;
                    depthInfo.clear_depth = 0.0f;
                    depthInfo.store_op = SDL_GPU_STOREOP_STORE;
                    depthInfo.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
                    depthInfo.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
                    batch.depth->undefined = false;
                }

                pass = SDL_BeginGPURenderPass(commands, &colorInfo, 1u,
                                              batch.depth ? &depthInfo : nullptr);
                if (!pass) {
                    return setError(std::string("SDL_BeginGPURenderPass: ") + SDL_GetError());
                }
                activeColor = batch.color;
                activeDepth = batch.depth;
                ++stats.renderPasses;

                SDL_GPUViewport viewport{};
                viewport.x = 0.0f;
                viewport.y = 0.0f;
                viewport.w = static_cast<float>(batch.color->width * batch.color->scale);
                viewport.h = static_cast<float>(batch.color->height * batch.color->scale);
                viewport.min_depth = 0.0f;
                viewport.max_depth = 1.0f;
                SDL_SetGPUViewport(pass, &viewport);

                const SDL_GPUBufferBinding binding{vertexBuffer, 0u};
                SDL_BindGPUVertexBuffers(pass, 0u, &binding, 1u);
            }

            SDL_GPUGraphicsPipeline *pipeline = device.pipeline(batch.pipeline, error);
            if (!pipeline) {
                endPass();
                return setError(std::move(error));
            }
            SDL_BindGPUGraphicsPipeline(pass, pipeline);
            SDL_SetGPUScissor(pass, &batch.scissor);
            if (batch.usesBlendConstant) {
                const SDL_FColor constant{batch.blendConstant, batch.blendConstant,
                                          batch.blendConstant, batch.blendConstant};
                SDL_SetGPUBlendConstants(pass, constant);
            }

            const SDL_GPUTextureSamplerBinding textureBindings[2] = {
                {sampledTexture ? sampledTexture : dummyTexture, device.sampler()},
                {destinationTexture ? destinationTexture : dummyTexture, device.sampler()}};
            SDL_BindGPUFragmentSamplers(pass, 0u, textureBindings, batch.destination ? 2u : 1u);

            SDL_PushGPUVertexUniformData(commands, 0u, &batch.vertexUniforms,
                                         sizeof(batch.vertexUniforms));
            SDL_PushGPUFragmentUniformData(commands, 0u, &batch.fragmentUniforms,
                                           sizeof(batch.fragmentUniforms));
            SDL_DrawGPUPrimitives(pass, batch.vertexCount, 1u, vertexBase + batch.firstVertex, 0u);
            ++stats.drawCalls;

            if (batchTraceEnabled()) {
                std::fprintf(
                    stderr,
                    "[batch] target=%05x psm=%02x %ux%u depth=%s verts=%u tme=%d "
                    "ate=%d atst=%u aref=%.0f tfx=%u blend=%d src=%u dst=%u op=%u "
                    "mask=%x zcmp=%u zwrite=%d scissor=(%d,%d %dx%d)\n",
                    batch.color->base, batch.color->psm, batch.color->width,
                    batch.color->height, batch.depth ? "yes" : "no", batch.vertexCount,
                    (batch.fragmentUniforms.control[0] & kFragFlagTme) != 0u,
                    (batch.fragmentUniforms.control[0] & kFragFlagAte) != 0u,
                    batch.fragmentUniforms.control[1],
                    batch.fragmentUniforms.fog[3] * 255.0f,
                    batch.fragmentUniforms.control[2], batch.pipeline.blendEnabled,
                    batch.pipeline.sourceFactor, batch.pipeline.destinationFactor,
                    batch.pipeline.blendOperation, batch.pipeline.colorWriteMask,
                    batch.pipeline.depthCompare, batch.pipeline.depthWrite,
                    batch.scissor.x, batch.scissor.y, batch.scissor.w, batch.scissor.h);
            }

            targets->markDrawn(*batch.color, batch.written);
            drawnPages |= batch.writtenPages;
        }

        endPass();
        ++stats.drawFlushes;
        recordedDraws += static_cast<uint32_t>(batches.size());
        vertexBase += static_cast<uint32_t>(vertices.size());
        clearBatches();
        vertices.clear();
        // Enough recorded: out it goes, so the GPU works while the worker
        // queues the next batches. Otherwise a later flush, a readback or the
        // present sends it.
        if (submitAfterDraws == 0u || recordedDraws >= submitAfterDraws) {
            if (!device.submitPending(error))
                return setError(std::move(error));
            recordedDraws = 0u;
        }

        // Drawing into a render target changes what a texture built from that
        // region should contain. Without this the cache keeps serving the copy
        // it made earlier, which is how a render-to-texture blit ends up
        // showing the frame before last -- or, on the first frame, nothing.
        if (textures && drawnPages.any())
            textures->invalidate(drawnPages, GsTextureCache::InvalidationSource::Draw);
        return true;
    }

    bool clearSurface(GsSurface &surface, float r, float g, float b, float a) {
        std::string error;
        SDL_GPUCommandBuffer *commands = device.pendingCommands(error);
        if (!commands)
            return setError(std::move(error));
        SDL_GPUColorTargetInfo info{};
        info.texture = surface.texture;
        info.load_op = SDL_GPU_LOADOP_CLEAR;
        info.store_op = SDL_GPU_STOREOP_STORE;
        info.clear_color = SDL_FColor{r, g, b, a};
        SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(commands, &info, 1u, nullptr);
        if (!pass) {
            device.cancelPending();
            return setError(std::string("SDL_BeginGPURenderPass(clear): ") + SDL_GetError());
        }
        SDL_EndGPURenderPass(pass);
        // A clear covers the whole surface and supersedes any pending upload.
        surface.needsUpload.clear();
        surface.cpuPatches.clear();
        targets->markDrawn(surface, surface.wholeRegion());
        return true;
    }

    // -----------------------------------------------------------------
    // Presentation
    // -----------------------------------------------------------------

    // Reads one CRT read circuit into a tightly packed RGBA8 image at `factor`
    // times its GS size. Preferring the render target over local memory is
    // what makes 4K real: nothing is downsampled on the way to the screen.
    void readCircuit(const GSPresentationRequest &request,
                     const GsDisplaySetup &setup,
                     bool halfHeightSource,
                     bool allowPreferred,
                     uint32_t factor,
                     std::vector<uint8_t> &out,
                     uint32_t &sourceFbp,
                     bool &usedPreferred) {
        const uint32_t width = setup.width * factor;
        const uint32_t height = setup.height * factor;
        out.assign(static_cast<size_t>(width) * height * 4u, 0u);

        uint32_t sourceBase = setup.fbp << 5u;
        uint32_t sourceWidth = setup.fbw;
        uint32_t sourcePsm = setup.psm;
        uint32_t originX = setup.originX;
        uint32_t originY = setup.originY;
        sourceFbp = setup.fbp;
        usedPreferred = false;

        // The frontend can point at the context frame that actually holds the
        // image when the CRTC's own DISPFB has not caught up.
        if (allowPreferred && request.hasPreferredSource &&
            request.preferredDestFbp == setup.fbp &&
            gsIsColorFramePsm(request.preferredSource.psm)) {
            sourceBase = request.preferredSource.fbp;
            sourceWidth = std::max<uint32_t>(request.preferredSource.fbw, 1u);
            sourcePsm = request.preferredSource.psm;
            originX = 0u;
            originY = 0u;
            sourceFbp = request.preferredSource.fbp;
            usedPreferred = true;
        }
        if (!gsIsColorFramePsm(sourcePsm))
            return;

        // Output row y maps to native row y/factor; the sub-row index is kept
        // so a scaled source contributes all of its detail.
        auto sourceRowFor = [&](uint32_t y) {
            const uint32_t nativeY = y / factor;
            const uint32_t sub = y % factor;
            // FFMD=1 reads a half-height buffer, so each source row covers two
            // output rows.
            const uint32_t nativeSource =
                originY + (halfHeightSource ? nativeY / 2u : nativeY);
            return nativeSource * factor + sub;
        };

        // DQ8_GFX_PRESENT_FROM_MEMORY forces the local-memory path, which is
        // what the software backend reads. It answers "is the difference in
        // what was drawn, or in how it was read back for display" in one run.
        static const bool forceMemoryPath = [] {
            const char *value = std::getenv("DQ8_GFX_PRESENT_FROM_MEMORY");
            return value != nullptr && *value != '\0';
        }();

        GsSurface *surface = forceMemoryPath
                                 ? nullptr
                                 : targets->findColorSurface(sourceBase, sourcePsm);
        // Host transfers can replace a cached target without another draw.
        // Read local memory until that target has consumed the new pixels.
        if (surface && !surface->undefined && surface->scale == factor &&
            surface->needsUpload.empty()) {
            std::vector<uint8_t> scaled;
            std::string error;
            if (downloadSurfaceScaled(*surface, scaled, error)) {
                const uint32_t scaledWidth = surface->width * factor;
                const uint32_t scaledHeight = surface->height * factor;
                for (uint32_t y = 0u; y < height; ++y) {
                    const uint32_t sourceRow = sourceRowFor(y);
                    if (sourceRow >= scaledHeight)
                        break;
                    uint8_t *destination = out.data() + static_cast<size_t>(y) * width * 4u;
                    const uint8_t *source =
                        scaled.data() + static_cast<size_t>(sourceRow) * scaledWidth * 4u;
                    for (uint32_t x = 0u; x < width; ++x) {
                        const uint32_t sourceColumn = originX * factor + x;
                        if (sourceColumn >= scaledWidth)
                            break;
                        std::memcpy(destination + x * 4u, source + sourceColumn * 4u, 4u);
                        // A render target is RGBA8 whatever it represents, so
                        // it still carries the alpha the draws wrote. The GS
                        // buffer it stands for may have nowhere to keep that:
                        // CT24 has no alpha at all, and CT16 has one bit. The
                        // PCRTC blends the two circuits using this value, so
                        // handing it the leftover would tint every pixel the
                        // buffer's own format would have discarded.
                        destination[x * 4u + 3u] =
                            displayAlphaForPsm(sourcePsm, destination[x * 4u + 3u]);
                    }
                }
                return;
            }
            setError(std::move(error));
        }

        // Nothing owns the buffer on the GPU -- it was filled by transfers,
        // which is exactly how DQ8 shows a movie frame. Make sure any target
        // that owns part of it has resolved, then read local memory.
        GsPageSet pages;
        gsMarkPages(pages, sourceBase, sourceWidth, sourcePsm,
                    originX + setup.width, originY + setup.height);
        std::string error;
        ResolveReasonScope scope(*targets, GsResolveReason::Present);
        if (!targets->resolve(pages, error))
            setError(std::move(error));

        for (uint32_t y = 0u; y < height; ++y) {
            const uint32_t sourceRow = sourceRowFor(y) / factor;
            uint8_t *destination = out.data() + static_cast<size_t>(y) * width * 4u;
            for (uint32_t x = 0u; x < width; ++x) {
                const uint32_t value =
                    vram.read(sourcePsm, sourceBase, sourceWidth, originX + x / factor, sourceRow);
                unpackDisplayColor(sourcePsm, value, destination + x * 4u);
            }
        }
    }

    // What alpha a display buffer of this PSM can actually hold.
    static uint8_t displayAlphaForPsm(uint32_t psm, uint8_t stored) {
        switch (psm & 0x3fu) {
        case GS_PSM_CT24:
            return 0u;
        case GS_PSM_CT16:
        case GS_PSM_CT16S:
            // One bit, expanded the way the GS reads it back.
            return (stored >= 0x80u) ? 0x80u : 0u;
        case GS_PSM_CT32:
        default:
            return stored;
        }
    }

    // Alpha is kept, not forced opaque: the PCRTC blends the two read circuits
    // using circuit 1's alpha, so it is still live data at this point.
    static void unpackDisplayColor(uint32_t psm, uint32_t value, uint8_t *rgba) {
        if ((psm & 0x3fu) == GS_PSM_CT16 || (psm & 0x3fu) == GS_PSM_CT16S) {
            const uint32_t r = value & 31u;
            const uint32_t g = (value >> 5u) & 31u;
            const uint32_t b = (value >> 10u) & 31u;
            rgba[0] = static_cast<uint8_t>((r << 3u) | (r >> 2u));
            rgba[1] = static_cast<uint8_t>((g << 3u) | (g >> 2u));
            rgba[2] = static_cast<uint8_t>((b << 3u) | (b >> 2u));
            rgba[3] = ((value & 0x8000u) != 0u) ? 0x80u : 0u;
        } else {
            rgba[0] = static_cast<uint8_t>(value);
            rgba[1] = static_cast<uint8_t>(value >> 8u);
            rgba[2] = static_cast<uint8_t>(value >> 16u);
            rgba[3] = (psm & 0x3fu) == GS_PSM_CT24
                          ? 0u
                          : static_cast<uint8_t>(value >> 24u);
        }
    }

    static uint8_t blendCircuitChannel(uint8_t source, uint8_t destination, uint32_t factor) {
        const int delta = static_cast<int>(source) - static_cast<int>(destination);
        const int result = static_cast<int>(destination) +
                           (delta * static_cast<int>(factor)) / 255;
        return static_cast<uint8_t>(std::clamp(result, 0, 255));
    }

    bool downloadSurfaceScaled(GsSurface &surface,
                               std::vector<uint8_t> &out,
                               std::string &error) {
        return targets->download(surface, out, error);
    }

    // DQ8_GFX_STATS_EVERY=N prints a counter delta every N presented frames.
    // The movie path is transfer-dominated rather than draw-dominated, so
    // "how many transfers per frame and how long did the frame take" is the
    // question a profile of the whole process answers badly.
    void reportStats() {
        const uint32_t interval = statsInterval();
        using Clock = std::chrono::steady_clock;
        // The longest gap between two presents in the interval (Rogue Galaxy:
        // added): a hitch of a frame or two disappears into the mean fps.
        static Clock::time_point previousPresent = Clock::now();
        static double longestPresentGap = 0.0;
        if (interval != 0u || presentObserver()) {
            const Clock::time_point presented = Clock::now();
            const double gap = std::chrono::duration<double>(presented - previousPresent).count();
            previousPresent = presented;
            longestPresentGap = std::max(longestPresentGap, gap);
            if (const auto &observe = presentObserver())
                observe(gap);
        }
        if (interval == 0u || (stats.presents % interval) != 0u)
            return;

        static Clock::time_point last = Clock::now();
        static SdlGpuStats previous{};
        static bool primed = false;
        const Clock::time_point now = Clock::now();
        const double seconds = std::chrono::duration<double>(now - last).count();
        last = now;
        // The first call establishes the baseline; reporting it would divide by
        // the time since this function was first entered, which is zero.
        if (!primed) {
            primed = true;
            previous = stats;
            return;
        }

        const auto delta = [](uint64_t a, uint64_t b) { return a - b; };
        static uint64_t previousBackendNanos = 0u;
        const uint64_t totalBackendNanos = backendNanos + displayNanos.load(std::memory_order_relaxed);
        const double backendSeconds =
            static_cast<double>(totalBackendNanos - previousBackendNanos) / 1e9;
        previousBackendNanos = totalBackendNanos;

        // The frontend's total includes the backend calls it makes, so its own
        // share is the difference.
        static uint64_t previousFrontendNanos = 0u;
        static uint64_t previousFrontendPackets = 0u;
        const uint64_t frontendNanos =
            g_gsFrontendPacketNanos.load(std::memory_order_relaxed);
        const uint64_t frontendPackets =
            g_gsFrontendPacketCount.load(std::memory_order_relaxed);
        // The two totals are accumulated on different threads, so the
        // subtraction can land slightly negative on a quiet interval.
        const double frontendSeconds = std::max(
            0.0, static_cast<double>(frontendNanos - previousFrontendNanos) / 1e9 - backendSeconds);
        previousFrontendNanos = frontendNanos;

        // The native image-upload path is where DQ8's movie tiles arrive: one
        // DMA chain per 16x16 tile, never touching processGIFPacket.
        static uint64_t previousUploadNanos = 0u;
        static uint64_t previousUploadCount = 0u;
        const uint64_t uploadNanos = g_gsUploadNativeNanos.load(std::memory_order_relaxed);
        const uint64_t uploadCount = g_gsUploadNativeCount.load(std::memory_order_relaxed);
        const double uploadSeconds =
            static_cast<double>(uploadNanos - previousUploadNanos) / 1e9;
        previousUploadNanos = uploadNanos;

        // The movie path. get-picture contains write-frame, so the difference
        // is decode; demux is the ring feed that precedes both.
        auto sample = [&seconds](std::atomic<uint64_t> &nanos, std::atomic<uint64_t> &count,
                                 uint64_t &previousNanos, uint64_t &outCount) {
            (void)seconds;
            const uint64_t currentNanos = nanos.load(std::memory_order_relaxed);
            outCount = count.load(std::memory_order_relaxed);
            const double elapsed = static_cast<double>(currentNanos - previousNanos) / 1e9;
            previousNanos = currentNanos;
            return elapsed;
        };
        static uint64_t previousGetPictureNanos = 0u, previousGetPictureCount = 0u;
        static uint64_t previousWriteFrameNanos = 0u, previousWriteFrameCount = 0u;
        static uint64_t previousDemuxNanos = 0u, previousDemuxCount = 0u;
        uint64_t getPictureCount = 0u, writeFrameCount = 0u, demuxCount = 0u;
        const double getPictureSeconds =
            sample(g_mpegGetPictureNanos, g_mpegGetPictureCount, previousGetPictureNanos, getPictureCount);
        const double writeFrameSeconds =
            sample(g_mpegWriteFrameNanos, g_mpegWriteFrameCount, previousWriteFrameNanos, writeFrameCount);
        const double demuxSeconds =
            sample(g_mpegDemuxNanos, g_mpegDemuxCount, previousDemuxNanos, demuxCount);
        static uint64_t previousRefusedCount = 0u;
        const uint64_t refusedCount = g_mpegDemuxRefusedCount.load(std::memory_order_relaxed);
        static uint64_t previousDispatchCount = 0u;
        const uint64_t dispatchCount = g_eeGuestDispatchCount.load(std::memory_order_relaxed);
        static uint64_t previousThrowCount = 0u;
        const uint64_t throwCount = g_eeTransferThrowCount.load(std::memory_order_relaxed);
        static uint64_t previousSuspendCount = 0u;
        const uint64_t suspendCount = g_eeTransferSuspendCount.load(std::memory_order_relaxed);
        static uint64_t previousRunLoopCount = 0u;
        const uint64_t runLoopCount = g_eeRunLoopIterations.load(std::memory_order_relaxed);
        static uint64_t previousRunLoopResumeCount = 0u;
        const uint64_t runLoopResumeCount = g_eeRunLoopResumeCount.load(std::memory_order_relaxed);
        static uint64_t previousPumpCount = 0u;
        const uint64_t pumpCount = g_eeProcessPendingEventsCount.load(std::memory_order_relaxed);
        static uint64_t previousEnterGuestCount = 0u;
        const uint64_t enterGuestCount = g_eeEnterGuestCount.load(std::memory_order_relaxed);
        static uint64_t previousSubmissions = 0u;
        const uint64_t submissions = device.submissions();
        static uint64_t previousSameState = 0u, previousSubmitted = 0u;
        static SdlGpuStats previousReasons{};
        const auto d = [](uint64_t now, uint64_t before) { return static_cast<unsigned long long>(now - before); };
        std::fprintf(stderr,
                     "[gfx] same-state primitives %llu of %llu submitted | flushes: settle=%llu transfer=%llu "
                     "snapshot=%llu alias=%llu texread=%llu feedback=%llu localcopy=%llu present=%llu "
                     "rewrite=%llu of %llu | covered-releases=%llu (%llu pages)\n",
                     d(stats.sameStatePrimitives, previousSameState), d(stats.primitivesSubmitted, previousSubmitted),
                     d(stats.flushSettle, previousReasons.flushSettle),
                     d(stats.transferFlushes, previousReasons.transferFlushes),
                     d(stats.flushSnapshot, previousReasons.flushSnapshot), d(stats.flushAlias, previousReasons.flushAlias),
                     d(stats.flushTexRead, previousReasons.flushTexRead), d(stats.flushFeedback, previousReasons.flushFeedback),
                     d(stats.flushLocalCopy, previousReasons.flushLocalCopy), d(stats.flushPresent, previousReasons.flushPresent),
                     d(stats.flushRewrite, previousReasons.flushRewrite), d(stats.drawFlushes, previousReasons.drawFlushes),
                     d(stats.coveredReleases, previousReasons.coveredReleases),
                     d(stats.coveredPages, previousReasons.coveredPages));
        previousReasons = stats;
        previousSameState = stats.sameStatePrimitives;
        previousSubmitted = stats.primitivesSubmitted;
        if (const auto &extra = statsExtraLine()) {
            const std::string line = extra();
            if (!line.empty())
                std::fprintf(stderr, "  %s\n", line.c_str());
        }

        std::fprintf(stderr,
                     "[gfx] %.1f fps (backend %.0f%% of wall) over %u frames | transfers=%llu (%llu KiB, "
                     "%llu forced flushes) draws=%llu passes=%llu flushes=%llu submits=%llu | resolves=%llu "
                     "(%llu Kpx) refreshes=%llu (%llu Kpx) | tex builds=%llu "
                     "(%llu Ktexels) bound-live=%llu feedback-copies=%llu | tex-inval draw=%llu write=%llu | "
                     "partial-updates=%llu (%llu Ktexels) | gif packets=%llu frontend=%.0f%% | "
                     "native-uploads=%llu upload-path=%.0f%%\n"
                     "  movie: get-picture=%llu (%.0f%%) write-frame=%llu (%.0f%%) "
                     "demux=%llu (%.0f%%) refused=%llu es-buffer-peak=%lluKiB | threads: demux=%u get-picture=%u"
                     " | ee-dispatches=%llu (%llu/frame) throws=%llu\n"
                     "  sched: loop=%llu (%.0f/s) resumes=%llu pumps=%llu enter-guest=%llu suspends=%llu"
                     " | presents native=%llu composed=%llu gpu-composed=%llu dual-circuit=%llu"
                     " | widened-alpha-replays=%llu interrupted-covered-transfers=%llu\n",
                     seconds > 0.0 ? static_cast<double>(interval) / seconds : 0.0,
                     seconds > 0.0 ? (backendSeconds / seconds) * 100.0 : 0.0,
                     interval,
                     static_cast<unsigned long long>(delta(stats.transfersBegun, previous.transfersBegun)),
                     static_cast<unsigned long long>(delta(stats.transferBytes, previous.transferBytes) / 1024u),
                     static_cast<unsigned long long>(delta(stats.transferFlushes, previous.transferFlushes)),
                     static_cast<unsigned long long>(delta(stats.drawCalls, previous.drawCalls)),
                     static_cast<unsigned long long>(delta(stats.renderPasses, previous.renderPasses)),
                     static_cast<unsigned long long>(delta(stats.drawFlushes, previous.drawFlushes)),
                     static_cast<unsigned long long>(submissions - previousSubmissions),
                     static_cast<unsigned long long>(delta(targetStats().colorResolves, previous.colorResolves)),
                     static_cast<unsigned long long>(delta(targetStats().resolvedPixels, previous.resolvedPixels) / 1024u),
                     static_cast<unsigned long long>(delta(targetStats().colorRefreshes, previous.colorRefreshes)),
                     static_cast<unsigned long long>(delta(targetStats().refreshedPixels, previous.refreshedPixels) / 1024u),
                     static_cast<unsigned long long>(delta(textureStats().builds, previous.textureBuilds)),
                     static_cast<unsigned long long>(delta(textureStats().texelsExpanded, previous.texelsExpanded) / 1024u),
                     static_cast<unsigned long long>(delta(stats.texturesFromLiveTargets, previous.texturesFromLiveTargets)),
                     static_cast<unsigned long long>(delta(stats.feedbackCopies, previous.feedbackCopies)),
                     static_cast<unsigned long long>(delta(textureStats().invalidationsFromDraw, previous.textureInvalidationsFromDraw)),
                     static_cast<unsigned long long>(delta(textureStats().invalidationsFromHostWrite, previous.textureInvalidationsFromHostWrite)),
                     static_cast<unsigned long long>(delta(textureStats().partialUpdates, previous.texturePartialUpdates)),
                     static_cast<unsigned long long>(delta(textureStats().texelsUpdated, previous.texelsUpdated) / 1024u),
                     static_cast<unsigned long long>(frontendPackets - previousFrontendPackets),
                     seconds > 0.0 ? (frontendSeconds / seconds) * 100.0 : 0.0,
                     static_cast<unsigned long long>(uploadCount - previousUploadCount),
                     seconds > 0.0 ? (uploadSeconds / seconds) * 100.0 : 0.0,
                     static_cast<unsigned long long>(getPictureCount - previousGetPictureCount),
                     seconds > 0.0 ? (getPictureSeconds / seconds) * 100.0 : 0.0,
                     static_cast<unsigned long long>(writeFrameCount - previousWriteFrameCount),
                     seconds > 0.0 ? (writeFrameSeconds / seconds) * 100.0 : 0.0,
                     static_cast<unsigned long long>(demuxCount - previousDemuxCount),
                     seconds > 0.0 ? (demuxSeconds / seconds) * 100.0 : 0.0,
                     static_cast<unsigned long long>(refusedCount - previousRefusedCount),
                     static_cast<unsigned long long>(
                         g_mpegPendingEsPeakBytes.load(std::memory_order_relaxed) / 1024u),
                     g_mpegDemuxThreadId.load(std::memory_order_relaxed),
                     g_mpegGetPictureThreadId.load(std::memory_order_relaxed),
                     static_cast<unsigned long long>(dispatchCount - previousDispatchCount),
                     static_cast<unsigned long long>((dispatchCount - previousDispatchCount) /
                                                     std::max<uint32_t>(1u, interval)),
                     static_cast<unsigned long long>(throwCount - previousThrowCount),
                     static_cast<unsigned long long>(runLoopCount - previousRunLoopCount),
                     seconds > 0.0 ? static_cast<double>(runLoopCount - previousRunLoopCount) / seconds : 0.0,
                     static_cast<unsigned long long>(runLoopResumeCount - previousRunLoopResumeCount),
                     static_cast<unsigned long long>(pumpCount - previousPumpCount),
                     static_cast<unsigned long long>(enterGuestCount - previousEnterGuestCount),
                     static_cast<unsigned long long>(suspendCount - previousSuspendCount),
                     static_cast<unsigned long long>(stats.nativePresents),
                     static_cast<unsigned long long>(stats.composedPresents),
                     static_cast<unsigned long long>(stats.gpuComposedPresents),
                     static_cast<unsigned long long>(stats.secondaryDisplayCircuits),
                     static_cast<unsigned long long>(stats.widenedAlphaReplays),
                     static_cast<unsigned long long>(stats.interruptedCoveredTransfers));
        previousSubmissions = submissions;

        // Where the resolves came from and what their readbacks cost (Rogue
        // Galaxy: added).
        {
            static std::array<uint64_t, static_cast<size_t>(GsResolveReason::Count)> previousByReason{};
            static uint64_t previousResolveNanos = 0u;
            std::string line = "  resolves by reason:";
            for (size_t i = 0u; i < previousByReason.size(); ++i) {
                const uint64_t count = targetStats().resolvesByReason[i] - previousByReason[i];
                previousByReason[i] = targetStats().resolvesByReason[i];
                if (count == 0u)
                    continue;
                char buf[64];
                std::snprintf(buf, sizeof(buf), " %s=%llu", gsResolveReasonName(static_cast<GsResolveReason>(i)),
                              static_cast<unsigned long long>(count));
                line += buf;
            }
            char buf[96];
            std::snprintf(buf, sizeof(buf), " | readback wait %.2f ms/frame, %.0f%% of wall",
                          static_cast<double>(targetStats().resolveNanos - previousResolveNanos) / 1e6 /
                              std::max<uint32_t>(1u, interval),
                          seconds > 0.0 ? static_cast<double>(targetStats().resolveNanos - previousResolveNanos) / 1e9 /
                                              seconds * 100.0
                                        : 0.0);
            previousResolveNanos = targetStats().resolveNanos;
            line += buf;
            // New draw pipelines and what creating them cost (Rogue Galaxy:
            // added): a compile stalls the worker the first time a state is
            // drawn.
            static size_t previousPipelines = 0u;
            static uint64_t previousPipelineNanos = 0u;
            const size_t pipelines = device.pipelineCount();
            const uint64_t pipelineNanos = device.pipelineCreateNanos();
            if (pipelines != previousPipelines) {
                std::snprintf(buf, sizeof(buf), " | pipelines +%llu (%.1f ms)",
                              static_cast<unsigned long long>(pipelines - std::min(pipelines, previousPipelines)),
                              static_cast<double>(pipelineNanos - previousPipelineNanos) / 1e6);
                line += buf;
            }
            previousPipelines = pipelines;
            previousPipelineNanos = pipelineNanos;
            // Texture updates that found the texels unchanged (Rogue Galaxy:
            // added).
            static uint64_t previousUnchanged = 0u;
            if (textureStats().unchangedUpdates != previousUnchanged) {
                std::snprintf(buf, sizeof(buf), " | textures unchanged %llu",
                              static_cast<unsigned long long>(textureStats().unchangedUpdates - previousUnchanged));
                line += buf;
                previousUnchanged = textureStats().unchangedUpdates;
            }
            // Primitives drawn from the host's larger versions, borders from
            // its border versions and shadows from its shadow versions (Rogue
            // Galaxy: added).
            if (stats.regionsReplaced != previous.regionsReplaced || stats.bordersRounded != previous.bordersRounded ||
                stats.shadowsDrawn != previous.shadowsDrawn) {
                std::snprintf(buf, sizeof(buf), " | larger regions %llu borders %llu shadows %llu",
                              static_cast<unsigned long long>(stats.regionsReplaced - previous.regionsReplaced),
                              static_cast<unsigned long long>(stats.bordersRounded - previous.bordersRounded),
                              static_cast<unsigned long long>(stats.shadowsDrawn - previous.shadowsDrawn));
                line += buf;
            }
            // Draws blended in the shader from a snapshot (Rogue Galaxy: added).
            if (stats.destinationReads != previous.destinationReads) {
                std::snprintf(buf, sizeof(buf), " | destination reads %llu",
                              static_cast<unsigned long long>(stats.destinationReads - previous.destinationReads));
                line += buf;
            }
            // Frames read back and returned at scale (Rogue Galaxy: added).
            if (stats.roundTrips != previous.roundTrips) {
                std::snprintf(buf, sizeof(buf), " | round trips %llu",
                              static_cast<unsigned long long>(stats.roundTrips - previous.roundTrips));
                line += buf;
            }
            std::snprintf(buf, sizeof(buf), " | longest frame %.1f ms", longestPresentGap * 1e3);
            longestPresentGap = 0.0;
            line += buf;
            std::fprintf(stderr, "%s\n", line.c_str());
        }

        previous = stats;
        previous.textureInvalidationsFromDraw = textureStats().invalidationsFromDraw;
        previous.textureInvalidationsFromHostWrite = textureStats().invalidationsFromHostWrite;
        previous.texturePartialUpdates = textureStats().partialUpdates;
        previous.texelsUpdated = textureStats().texelsUpdated;
        previousRefusedCount = refusedCount;
        previousDispatchCount = dispatchCount;
        previousThrowCount = throwCount;
        previousSuspendCount = suspendCount;
        previousRunLoopCount = runLoopCount;
        previousRunLoopResumeCount = runLoopResumeCount;
        previousPumpCount = pumpCount;
        previousEnterGuestCount = enterGuestCount;
        previousFrontendPackets = frontendPackets;
        previousUploadCount = uploadCount;
        previousGetPictureCount = getPictureCount;
        previousWriteFrameCount = writeFrameCount;
        previousDemuxCount = demuxCount;
        previous.colorResolves = targetStats().colorResolves;
        previous.resolvedPixels = targetStats().resolvedPixels;
        previous.colorRefreshes = targetStats().colorRefreshes;
        previous.refreshedPixels = targetStats().refreshedPixels;
        previous.textureBuilds = textureStats().builds;
        previous.texelsExpanded = textureStats().texelsExpanded;
    }

    const GsTargetCacheStats &targetStats() const {
        static const GsTargetCacheStats empty{};
        return targets ? targets->stats() : empty;
    }
    const GsTextureCacheStats &textureStats() const {
        static const GsTextureCacheStats empty{};
        return textures ? textures->stats() : empty;
    }

    std::shared_ptr<PreparedFrame> leasePresentation() {
        std::unique_lock lock(presentationPool->mutex);
        presentationPool->available.wait(lock, [&] {
            return presentationPool->closing || std::any_of(presentationPool->slots.begin(),
                presentationPool->slots.end(), [](const auto &slot) { return !slot.leased; });
        });
        if (presentationPool->closing)
            throw std::runtime_error("SDL GPU presentation cancelled");
        size_t selected = presentationPool->slots.size();
        for (size_t index = 0; index < presentationPool->slots.size(); ++index) {
            auto &slot = presentationPool->slots[index];
            if (slot.leased) continue;
            if (selected == presentationPool->slots.size()) selected = index;
            if ((!slot.prepared || SDL_QueryGPUFence(device.handle(), slot.prepared)) &&
                (!slot.displayed || SDL_QueryGPUFence(device.handle(), slot.displayed))) {
                selected = index;
                break;
            }
        }
        auto result = std::make_shared<PreparedFrame>(presentationPool, selected);
        presentationPool->slots[selected].leased = true;
        return result;
    }

    void retirePresentationFences(PresentationSlot &slot) {
        for (auto **fence : {&slot.prepared, &slot.displayed}) {
            if (!*fence) continue;
            if (!SDL_WaitForGPUFences(device.handle(), true, fence, 1))
                throw std::runtime_error(std::string("SDL_WaitForGPUFences(presentation): ") + SDL_GetError());
            SDL_ReleaseGPUFence(device.handle(), *fence);
            *fence = nullptr;
        }
    }

    // A leased texture cannot be overwritten until both its CPU ticket and
    // all GPU reads are finished. Live render targets are never leased.
    bool ensurePresentSource(uint32_t width, uint32_t height) {
        if (presentSource != nullptr && presentSourceWidth == width &&
            presentSourceHeight == height)
            return true;
        if (presentSource)
            SDL_ReleaseGPUTexture(device.handle(), presentSource);
        presentSource = nullptr;
        preparingSlot->texture = nullptr;

        SDL_GPUTextureCreateInfo info{};
        info.type = SDL_GPU_TEXTURETYPE_2D;
        info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        info.width = width;
        info.height = height;
        info.layer_count_or_depth = 1u;
        info.num_levels = 1u;
        info.sample_count = SDL_GPU_SAMPLECOUNT_1;
        info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
        presentSource = SDL_CreateGPUTexture(device.handle(), &info);
        if (!presentSource)
            return setError(std::string("SDL_CreateGPUTexture(present source): ") + SDL_GetError());
        presentSourceWidth = width;
        presentSourceHeight = height;
        preparingSlot->texture = presentSource;
        preparingSlot->width = width;
        preparingSlot->height = height;
        return true;
    }

    // GPU-side copy of a render target into the present source. No swapchain,
    // no wait, so this is safe to do while holding the backend lock.
    bool copyToPresentSource(SDL_GPUTexture *source, uint32_t width, uint32_t height) {
        if (!ensurePresentSource(width, height))
            return false;
        // Whatever is recorded for the source (its patches, say) goes first.
        std::string error;
        if (!device.submitPending(error))
            return setError(std::move(error));
        SDL_GPUCommandBuffer *commands = SDL_AcquireGPUCommandBuffer(device.handle());
        if (!commands)
            return setError(std::string("SDL_AcquireGPUCommandBuffer(present copy): ") + SDL_GetError());
        SDL_GPUBlitInfo blit{};
        blit.source.texture = source;
        blit.source.w = width;
        blit.source.h = height;
        blit.destination.texture = presentSource;
        blit.destination.w = width;
        blit.destination.h = height;
        blit.load_op = SDL_GPU_LOADOP_DONT_CARE;
        blit.filter = SDL_GPU_FILTER_NEAREST;
        SDL_BlitGPUTexture(commands, &blit);
        preparingSlot->prepared = SDL_SubmitGPUCommandBufferAndAcquireFence(commands);
        if (!preparingSlot->prepared)
            return setError(std::string("SDL_SubmitGPUCommandBufferAndAcquireFence(present copy): ") + SDL_GetError());
        return true;
    }

    bool ensureScratch(ScratchTexture &scratch, uint32_t width, uint32_t height,
                       SDL_GPUTextureFormat format) {
        if (scratch.texture && scratch.width == width && scratch.height == height &&
            scratch.format == format)
            return true;
        releaseScratch(scratch);
        SDL_GPUTextureCreateInfo info{};
        info.type = SDL_GPU_TEXTURETYPE_2D;
        info.format = format;
        info.width = width;
        info.height = height;
        info.layer_count_or_depth = 1u;
        info.num_levels = 1u;
        info.sample_count = SDL_GPU_SAMPLECOUNT_1;
        info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
        scratch.texture = SDL_CreateGPUTexture(device.handle(), &info);
        if (!scratch.texture)
            return false;
        scratch.width = width;
        scratch.height = height;
        scratch.format = format;
        return true;
    }

    void releaseScratch(ScratchTexture &scratch) {
        if (scratch.texture)
            SDL_ReleaseGPUTexture(device.handle(), scratch.texture);
        scratch = {};
    }

    static void blitRegion(SDL_GPUCommandBuffer *commands, SDL_GPUTexture *source, SdlGpuRect from,
                           SDL_GPUTexture *destination, SdlGpuRect to, SDL_GPUFilter filter,
                           SDL_GPULoadOp loadOp) {
        SDL_GPUBlitInfo blit{};
        blit.source.texture = source;
        blit.source.x = from.x;
        blit.source.y = from.y;
        blit.source.w = from.width;
        blit.source.h = from.height;
        blit.destination.texture = destination;
        blit.destination.x = to.x;
        blit.destination.y = to.y;
        blit.destination.w = to.width;
        blit.destination.h = to.height;
        blit.load_op = loadOp;
        blit.clear_color = SDL_FColor{0.0f, 0.0f, 0.0f, 1.0f};
        blit.filter = filter;
        SDL_BlitGPUTexture(commands, &blit);
    }

    // The game picture into `target` at `rect`. CLEAR, not LOAD: the bars
    // around it have to be painted, and swapchain images are recycled.
    // The part of a slot that is shown: the crop at the slot's scale, at
    // most an eighth of it. Half a GS pixel more at a raised scale, where a
    // game's half-pixel offsets move its border by a texel or so.
    SdlGpuRect pictureRegion(const PresentationSlot &slot) const {
        const uint32_t factor = std::max(scale, 1u);
        const uint32_t crop = displayOptions.crop * factor + (displayOptions.crop != 0u ? factor / 2u : 0u);
        const uint32_t trim = std::min(crop, std::min(slot.width, slot.height) / 8u);
        return {trim, trim, slot.width - 2u * trim, slot.height - 2u * trim};
    }

    // CLEAR paints the bars black; LOAD keeps the sides drawn under it.
    void drawPicture(SDL_GPUCommandBuffer *commands, const PresentationSlot &slot,
                     SDL_GPUTexture *target, SdlGpuRect rect, SDL_GPULoadOp loadOp) {
        SDL_GPUTexture *source = slot.texture;
        SdlGpuRect from = pictureRegion(slot);
        uint32_t width = from.width;
        uint32_t height = from.height;
        SDL_GPUFilter filter = SDL_GPU_FILTER_LINEAR;
        if (displayOptions.filter == SdlGpuFilter::Nearest) {
            filter = SDL_GPU_FILTER_NEAREST;
        } else if (displayOptions.filter == SdlGpuFilter::Sharp) {
            if (rect.width % width == 0u && rect.height % height == 0u) {
                filter = SDL_GPU_FILTER_NEAREST;
            } else if (rect.width > width || rect.height > height) {
                // A fractional nearest scale duplicates some columns and not
                // others, which shreds one-pixel font stems. Nearest to the
                // whole multiple above the output, then bilinear down to it.
                // Per axis: with one factor, a picture stretched more across
                // than down has its bilinear pass shrink rows below half.
                constexpr uint32_t kLimit = 8192u;
                const auto factorFor = [](uint32_t output, uint32_t size) {
                    return std::max(1u, static_cast<uint32_t>(std::ceil(double(output) / size)));
                };
                const uint32_t factorX = factorFor(rect.width, width);
                const uint32_t factorY = factorFor(rect.height, height);
                if (width * factorX <= kLimit && height * factorY <= kLimit &&
                    ensureScratch(sharpTexture, width * factorX, height * factorY,
                                  SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM)) {
                    // The crop goes first: the bilinear pass then reads no
                    // texel beyond the picture.
                    blitRegion(commands, source, from, sharpTexture.texture,
                               {0u, 0u, width * factorX, height * factorY}, SDL_GPU_FILTER_NEAREST,
                               SDL_GPU_LOADOP_DONT_CARE);
                    source = sharpTexture.texture;
                    width *= factorX;
                    height *= factorY;
                    from = {0u, 0u, width, height};
                }
            }
        }
        blitRegion(commands, source, from, target, rect, filter, loadOp);
    }

    // The picture blurred for SdlGpuSides::Blurred: halved by bilinear
    // blits, each averaging four texels, until it is at most 256 wide (64
    // and 128 were too blurred for the user).
    SDL_GPUTexture *blurredPicture(SDL_GPUCommandBuffer *commands, const PresentationSlot &slot,
                                   uint32_t &width, uint32_t &height) {
        SDL_GPUTexture *source = slot.texture;
        SdlGpuRect from = pictureRegion(slot);
        for (ScratchTexture &level : blurLevels) {
            if (from.width <= 256u || from.height < 2u)
                break;
            const SdlGpuRect to{0u, 0u, from.width / 2u, from.height / 2u};
            if (!ensureScratch(level, to.width, to.height, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM))
                return nullptr;
            blitRegion(commands, source, from, level.texture, to, SDL_GPU_FILTER_LINEAR, SDL_GPU_LOADOP_DONT_CARE);
            source = level.texture;
            from = to;
        }
        if (source == slot.texture)
            return nullptr;
        width = from.width;
        height = from.height;
        return source;
    }

    void releaseSideImage() {
        if (sideImage)
            SDL_ReleaseGPUTexture(device.handle(), sideImage);
        sideImage = nullptr;
        sideImageWidth = sideImageHeight = 0u;
    }

    // The player's image, through the loader, uploaded in a copy pass ahead
    // of the frame's drawing.
    void loadSideImage(SDL_GPUCommandBuffer *commands) {
        releaseSideImage();
        uint32_t width = 0u, height = 0u;
        std::vector<uint8_t> rgba;
        if (!sideImageLoader || !sideImageLoader(width, height, rgba) || width == 0u || height == 0u ||
            width > 16384u || height > 16384u || rgba.size() < size_t(width) * height * 4u)
            return;
        SDL_GPUTextureCreateInfo info{};
        info.type = SDL_GPU_TEXTURETYPE_2D;
        info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        info.width = width;
        info.height = height;
        info.layer_count_or_depth = 1u;
        info.num_levels = 1u;
        info.sample_count = SDL_GPU_SAMPLECOUNT_1;
        info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
        SDL_GPUTexture *texture = SDL_CreateGPUTexture(device.handle(), &info);
        SDL_GPUTransferBufferCreateInfo transferInfo{};
        transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        transferInfo.size = width * height * 4u;
        SDL_GPUTransferBuffer *transfer =
            texture ? SDL_CreateGPUTransferBuffer(device.handle(), &transferInfo) : nullptr;
        void *mapped = transfer ? SDL_MapGPUTransferBuffer(device.handle(), transfer, false) : nullptr;
        SDL_GPUCopyPass *copy = nullptr;
        if (mapped) {
            std::memcpy(mapped, rgba.data(), transferInfo.size);
            SDL_UnmapGPUTransferBuffer(device.handle(), transfer);
            copy = SDL_BeginGPUCopyPass(commands);
        }
        if (copy) {
            SDL_GPUTextureTransferInfo from{};
            from.transfer_buffer = transfer;
            SDL_GPUTextureRegion to{};
            to.texture = texture;
            to.w = width;
            to.h = height;
            to.d = 1u;
            SDL_UploadToGPUTexture(copy, &from, &to, false);
            SDL_EndGPUCopyPass(copy);
            sideImage = texture;
            sideImageWidth = width;
            sideImageHeight = height;
        } else {
            std::fprintf(stderr, "[gfx] the side image could not be uploaded: %s\n", SDL_GetError());
            if (texture)
                SDL_ReleaseGPUTexture(device.handle(), texture);
        }
        // Released once the copy has run.
        if (transfer)
            SDL_ReleaseGPUTransferBuffer(device.handle(), transfer);
    }

    // The window beside a picture that leaves it uncovered, before the
    // picture is drawn over it; false leaves the bars to drawPicture's black.
    bool drawSides(SDL_GPUCommandBuffer *commands, const PresentationSlot &slot, SDL_GPUTexture *target,
                   uint32_t width, uint32_t height, SdlGpuRect rect) {
        const SdlGpuSides sides = displayOptions.sides;
        if (sides == SdlGpuSides::Image && lastSides != SdlGpuSides::Image)
            loadSideImage(commands);
        lastSides = sides;
        if (sides == SdlGpuSides::Black || (rect.width >= width && rect.height >= height))
            return false;
        SDL_GPUTexture *source = sideImage;
        uint32_t sourceWidth = sideImageWidth, sourceHeight = sideImageHeight;
        bool blur = false;
        float brightness = 1.0f;
        if (sides == SdlGpuSides::Blurred || !source) {
            source = blurredPicture(commands, slot, sourceWidth, sourceHeight);
            blur = true;
            // Half was too dark to make out anything.
            brightness = 0.75f;
        }
        std::string error;
        if (!source || !device.drawSides(commands, target, SDL_GetGPUSwapchainTextureFormat(device.handle(), window),
                                         width, height, source, sourceWidth, sourceHeight, blur, brightness,
                                         error)) {
            if (!error.empty() && !sidesFailed) {
                std::fprintf(stderr, "[gfx] the sides stay black: %s\n", error.c_str());
                sidesFailed = true;
            }
            return false;
        }
        return true;
    }

    void clearTarget(SDL_GPUCommandBuffer *commands, SDL_GPUTexture *target) {
        SDL_GPUColorTargetInfo info{};
        info.texture = target;
        info.load_op = SDL_GPU_LOADOP_CLEAR;
        info.store_op = SDL_GPU_STOREOP_STORE;
        info.clear_color = SDL_FColor{0.0f, 0.0f, 0.0f, 1.0f};
        if (SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(commands, &info, 1u, nullptr))
            SDL_EndGPURenderPass(pass);
    }

    int gameWidescreen() const { return widescreenQuery ? widescreenQuery() : -1; }

    void applyPresentMode() {
        if (displayOptions.presentMode == appliedPresentMode)
            return;
        SDL_GPUPresentMode mode = displayOptions.presentMode;
        if (!SDL_WindowSupportsGPUPresentMode(device.handle(), window, mode))
            mode = SDL_GPU_PRESENTMODE_VSYNC;
        if (SDL_SetGPUSwapchainParameters(device.handle(), window, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, mode))
            appliedPresentMode = displayOptions.presentMode;
    }

    // SDL moves resources to defragment memory at a submission that presents
    // (at any, without a window), once a new memory block was needed: it
    // copies each one within that submission and points its handle at the
    // copy. Commands the GS thread has recorded and not yet submitted would
    // still write the old one, and the copy keep what it held before; a
    // texture created and uploaded once lost its upload so (a texture pack
    // under test showed digits missing or as noise in the item menu). Such a
    // submission from the presenting thread goes out under the backend lock,
    // after the pending commands (Rogue Galaxy: added).
    template <typename Submission>
    auto submitAfterPending(Submission submission) {
        std::lock_guard lock(mutex);
        std::string error;
        if (!device.submitPending(error))
            std::fprintf(stderr, "[gfx] pending commands: %s\n", error.c_str());
        return submission();
    }

    // Fits a frame into the window, draws the overlay over it, and serves any
    // pending capture. A null slot shows the overlay over black.
    //
    // Called with the backend lock released: acquiring a swapchain texture
    // waits for the display, and holding the lock across that wait blocks
    // every Submit and transfer the EE thread makes for the whole frame.
    bool showInWindow(PresentationSlot *slot, std::string &error) {
        if (!window || (slot && !slot->texture))
            return true;
        const auto fail = [&](const char *context) {
            error = std::string(context) + SDL_GetError();
            return false;
        };
        if (slot && slot->displayed) {
            if (!SDL_WaitForGPUFences(device.handle(), true, &slot->displayed, 1))
                return fail("SDL_WaitForGPUFences(previous display): ");
            SDL_ReleaseGPUFence(device.handle(), slot->displayed);
            slot->displayed = nullptr;
        }
        applyPresentMode();

        SDL_GPUCommandBuffer *commands = SDL_AcquireGPUCommandBuffer(device.handle());
        if (!commands)
            return fail("SDL_AcquireGPUCommandBuffer(present): ");

        SDL_GPUTexture *swapchain = nullptr;
        uint32_t swapchainWidth = 0u;
        uint32_t swapchainHeight = 0u;
        if (!SDL_WaitAndAcquireGPUSwapchainTexture(commands, window, &swapchain,
                                                   &swapchainWidth, &swapchainHeight)) {
            SDL_CancelGPUCommandBuffer(commands);
            return fail("SDL_WaitAndAcquireGPUSwapchainTexture: ");
        }
        // Also when minimised, so an idle overlay does not retry every pump.
        lastShown = std::chrono::steady_clock::now();
        if (!swapchain || swapchainWidth == 0u || swapchainHeight == 0u) {
            // Minimised or otherwise unavailable this frame; nothing to show.
            if (!submitAfterPending([&] { return SDL_SubmitGPUCommandBuffer(commands); }))
                return fail("SDL_SubmitGPUCommandBuffer(minimized): ");
            return true;
        }
        const SdlGpuRect whole{0u, 0u, swapchainWidth, swapchainHeight};

        // A capture composes off screen first, since a swapchain image cannot
        // be read back.
        SDL_GPUTexture *target = swapchain;
        std::optional<CaptureRequest> capture;
        if (!captureRequests.empty() &&
            ensureScratch(compositeTexture, swapchainWidth, swapchainHeight,
                          SDL_GetGPUSwapchainTextureFormat(device.handle(), window))) {
            capture = std::move(captureRequests.front());
            captureRequests.erase(captureRequests.begin());
            target = compositeTexture.texture;
        }

        SdlGpuRect rect{};
        if (slot) {
            const SdlGpuRect picture = pictureRegion(*slot);
            rect = sdlGpuDisplayRect(displayOptions, picture.width, picture.height, swapchainWidth,
                                     swapchainHeight, gameWidescreen());
            const bool sides = drawSides(commands, *slot, target, swapchainWidth, swapchainHeight, rect);
            drawPicture(commands, *slot, target, rect, sides ? SDL_GPU_LOADOP_LOAD : SDL_GPU_LOADOP_CLEAR);
        } else {
            clearTarget(commands, target);
        }
        lastRect = rect;

        SdlGpuRect captured{};
        const auto copyOut = [&](SdlGpuRect from) {
            if (from.width != 0u && ensureScratch(captureTexture, from.width, from.height,
                                                  SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM)) {
                captured = {0u, 0u, from.width, from.height};
                blitRegion(commands, target, from, captureTexture.texture, captured,
                           SDL_GPU_FILTER_NEAREST, SDL_GPU_LOADOP_DONT_CARE);
            }
        };
        if (capture && !capture->withOverlay)
            copyOut(rect);
        if (overlay)
            overlay->render(commands, target, swapchainWidth, swapchainHeight);
        if (capture && capture->withOverlay)
            copyOut(whole);
        if (target != swapchain)
            blitRegion(commands, target, whole, swapchain, whole, SDL_GPU_FILTER_NEAREST,
                       SDL_GPU_LOADOP_DONT_CARE);

        SDL_GPUFence *displayed =
            submitAfterPending([&] { return SDL_SubmitGPUCommandBufferAndAcquireFence(commands); });
        if (!displayed)
            return fail("SDL_SubmitGPUCommandBufferAndAcquireFence(present): ");
        if (slot)
            slot->displayed = displayed;
        else
            SDL_ReleaseGPUFence(device.handle(), displayed);

        if (capture) {
            SdlGpuCapture result;
            std::string captureError;
            if (captured.width != 0u &&
                device.downloadTexture(captureTexture.texture, captured.width, captured.height,
                                       result.rgba, captureError)) {
                result.width = captured.width;
                result.height = captured.height;
            } else {
                std::fprintf(stderr, "[gfx] capture failed: %s\n",
                             captureError.empty() ? "nothing on screen" : captureError.c_str());
                result.rgba.clear();
            }
            if (capture->done)
                capture->done(std::move(result));
        }
        return true;
    }

    // Shows the last frame again under a fresh overlay, while the game shows
    // none: paused, loading, or not started yet.
    void refreshWindow() {
        PresentationSlot *slot = nullptr;
        {
            std::lock_guard lock(presentationPool->mutex);
            if (presentationPool->closing)
                return;
            if (lastDisplayedSlot < presentationPool->slots.size()) {
                PresentationSlot &candidate = presentationPool->slots[lastDisplayedSlot];
                // Leased means a new frame is on its way into it.
                if (candidate.leased)
                    return;
                if (candidate.texture) {
                    candidate.leased = true;
                    slot = &candidate;
                }
            }
        }
        std::string error;
        const bool shown = showInWindow(slot, error);
        if (slot) {
            std::lock_guard lock(presentationPool->mutex);
            slot->leased = false;
            presentationPool->available.notify_one();
        }
        if (!shown && error != lastWindowError)
            std::fprintf(stderr, "[gfx] window refresh: %s\n", error.c_str());
        lastWindowError = shown ? std::string() : error;
    }

    // Applied at the start of a presentation, an ordered point in the GS
    // command stream, so no draw of the frame in progress sees half a change.
    // Depth is never read back, so depth targets start clean at the new size.
    void applyRequestedScale() {
        const uint32_t requested = requestedScale.exchange(0u, std::memory_order_acq_rel);
        if (requested == 0u || !targets || requested == scale)
            return;
        if (!flushDraws())
            return;
        std::string error;
        ResolveReasonScope scope(*targets, GsResolveReason::Scale);
        if (!targets->resolveAll(error)) {
            std::fprintf(stderr, "[gfx] internal resolution stays %ux: %s\n", scale, error.c_str());
            return;
        }
        targets->setScale(requested);
        scale = targets->scale();
        activeScale.store(scale, std::memory_order_release);
        std::fprintf(stderr, "[gfx] internal resolution %ux\n", scale);
    }

    // Uploads a CPU-composed frame into the present source. Only used when the
    // frame could not be taken straight from a render target.
    bool showHostPixels(const PresentationFrame &frame) {
        if (frame.pixels.empty() || frame.width == 0u || frame.height == 0u)
            return true;
        if (!ensurePresentSource(frame.width, frame.height))
            return false;

        SDL_GPUDevice *handle = device.handle();
        if (presentUpload == nullptr || presentUploadWidth != frame.width ||
            presentUploadHeight != frame.height) {
            if (presentUpload)
                SDL_ReleaseGPUTexture(handle, presentUpload);
            SDL_GPUTextureCreateInfo info{};
            info.type = SDL_GPU_TEXTURETYPE_2D;
            info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
            info.width = frame.width;
            info.height = frame.height;
            info.layer_count_or_depth = 1u;
            info.num_levels = 1u;
            info.sample_count = SDL_GPU_SAMPLECOUNT_1;
            info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
            presentUpload = SDL_CreateGPUTexture(handle, &info);
            if (!presentUpload)
                return setError(std::string("SDL_CreateGPUTexture(present): ") + SDL_GetError());
            presentUploadWidth = frame.width;
            presentUploadHeight = frame.height;
        }

        const size_t bytes = static_cast<size_t>(frame.width) * frame.height * 4u;
        SDL_GPUTransferBufferCreateInfo transferInfo{};
        transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        transferInfo.size = static_cast<uint32_t>(bytes);
        SDL_GPUTransferBuffer *upload = SDL_CreateGPUTransferBuffer(handle, &transferInfo);
        if (!upload)
            return setError(std::string("SDL_CreateGPUTransferBuffer(present): ") + SDL_GetError());
        void *mapped = SDL_MapGPUTransferBuffer(handle, upload, false);
        if (!mapped) {
            SDL_ReleaseGPUTransferBuffer(handle, upload);
            return setError(std::string("SDL_MapGPUTransferBuffer(present): ") + SDL_GetError());
        }
        std::memcpy(mapped, frame.pixels.data(), bytes);
        SDL_UnmapGPUTransferBuffer(handle, upload);

        SDL_GPUCommandBuffer *commands = SDL_AcquireGPUCommandBuffer(handle);
        if (!commands) {
            SDL_ReleaseGPUTransferBuffer(handle, upload);
            return setError(std::string("SDL_AcquireGPUCommandBuffer(present upload): ") + SDL_GetError());
        }
        SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(commands);
        if (!copy) {
            SDL_CancelGPUCommandBuffer(commands);
            SDL_ReleaseGPUTransferBuffer(handle, upload);
            return setError(std::string("SDL_BeginGPUCopyPass(present): ") + SDL_GetError());
        }
        SDL_GPUTextureTransferInfo source{};
        source.transfer_buffer = upload;
        source.pixels_per_row = frame.width;
        source.rows_per_layer = frame.height;
        SDL_GPUTextureRegion destination{};
        destination.texture = presentUpload;
        destination.w = frame.width;
        destination.h = frame.height;
        destination.d = 1u;
        SDL_UploadToGPUTexture(copy, &source, &destination, false);
        SDL_EndGPUCopyPass(copy);
        const bool submitted = SDL_SubmitGPUCommandBuffer(commands);
        SDL_ReleaseGPUTransferBuffer(handle, upload);
        if (!submitted)
            return setError(std::string("SDL_SubmitGPUCommandBuffer(present upload): ") + SDL_GetError());

        return copyToPresentSource(presentUpload, frame.width, frame.height);
    }

    // Both composition stages run on the producer; window presentation only
    // reads the completed slot and never touches mutable GS state.
    struct PresentStage {
        bool haveFrame = false;
        bool gsActive = false;
        bool native = false;
        bool download = false;
        bool dualCircuit = false;
        GsPmodeState pmode{};
        GsDisplaySetup circuit1{}, circuit2{};
        uint32_t bgcolor = 0u;
        std::vector<uint8_t> crt1;
        std::vector<uint8_t> crt2;
        PresentationFrame frame{};
    };

    bool acquireGpuComposition(const GSPresentationRequest &request, const GsPmodeState &pmode,
                               const GsDisplaySetup &circuit1, const GsDisplaySetup &circuit2,
                               bool halfHeightSource, uint32_t factor, PresentStage &stage) {
        const char *disabled = std::getenv("DQ8_GFX_DISABLE_GPU_COMPOSE");
        const char *fromMemory = std::getenv("DQ8_GFX_PRESENT_FROM_MEMORY");
        if ((disabled && *disabled && std::strcmp(disabled, "0") != 0) ||
            (fromMemory && *fromMemory) || !gsIsColorFramePsm(circuit1.psm) ||
            !gsIsColorFramePsm(circuit2.psm))
            return false;
        GsSurface *surface1 = targets->findColorSurface(circuit1.fbp << 5u, circuit1.psm);
        GsSurface *surface2 = targets->findColorSurface(circuit2.fbp << 5u, circuit2.psm);
        const auto valid = [factor](const GsSurface *surface) {
            return surface && !surface->undefined && surface->scale == factor &&
                   surface->needsUpload.empty() && surface->cpuPatches.empty();
        };
        // Mirror readCircuit's valid texture view without changing ownership or
        // importing CPU pixels. Pending transfers and aliases use its fallback.
        if (!valid(surface1) || !valid(surface2))
            return false;
        const uint32_t width = std::max(circuit1.width, circuit2.width) * factor;
        const uint32_t height = std::max(circuit1.height, circuit2.height) * factor;
        if (!ensurePresentSource(width, height))
            return false;
        GsDisplayUniforms uniforms{};
        uniforms.control[0] = factor;
        uniforms.control[1] = halfHeightSource;
        uniforms.control[2] = pmode.useFixedAlpha;
        uniforms.control[3] = pmode.blendWithBackground;
        const auto configure = [factor](uint32_t (&out)[4], const GsDisplaySetup &circuit) {
            out[0] = circuit.originX;
            out[1] = circuit.originY;
            out[2] = circuit.width * factor;
            out[3] = circuit.height * factor;
        };
        configure(uniforms.circuit1, circuit1);
        configure(uniforms.circuit2, circuit2);
        uniforms.format[0] = circuit1.psm;
        uniforms.format[1] = circuit2.psm;
        uniforms.format[2] = pmode.fixedAlpha;
        uniforms.format[3] = static_cast<uint32_t>(request.bgcolor);
        std::string error;
        if (!device.submitPending(error) ||
            !device.composeDisplay(surface1->texture, surface2->texture, presentSource,
                                   width, height, uniforms, preparingSlot->prepared, error)) {
            setError(std::move(error));
            return false;
        }
        PresentationFrame &frame = stage.frame;
        frame.width = width;
        frame.height = height;
        frame.displayFbp = frame.sourceFbp = circuit1.fbp;
        if (!window || gsScreenshotEnabled()) {
            // The display consumer can read its immutable slot after later GS
            // work has started, without observing either live display circuit.
            stage.download = true;
            frame.mode = GSPresentationMode::HostPixels;
            frame.rowPitchBytes = width * 4u;
        } else {
            frame.mode = GSPresentationMode::BackendNative;
            frame.rowPitchBytes = 0u;
            ++stats.nativePresents;
        }
        ++stats.gpuComposedPresents;
        stage.native = true;
        stage.haveFrame = true;
        return true;
    }

    void acquirePresentation(const GSPresentationRequest &request, PresentStage &stage) {
        PresentationFrame &frame = stage.frame;
        ++stats.presents;
        tracedBatch = 0u;
        if (gsTargetTraceEnabled())
            std::fprintf(stderr, "[target] present #%llu\n", static_cast<unsigned long long>(stats.presents));
        if (!flushFor(stats.flushPresent) || !device.valid())
            return;
        stage.gsActive = stats.drawCalls != previousPresentDraws ||
                         stats.transferBytes != previousPresentTransfers;
        previousPresentDraws = stats.drawCalls;
        previousPresentTransfers = stats.transferBytes;

        const GsPmodeState pmode = gsDecodePmode(request.pmode);
        const GsSmode2State smode2 = gsDecodeSmode2(request.smode2);
        const bool halfHeightSource = smode2.interlaced && smode2.frameMode;

        GsDisplaySetup circuit1 =
            gsDecodeDisplay(request.dispfb1, request.display1, pmode.enableCircuit1);
        const GsDisplaySetup circuit2 =
            gsDecodeDisplay(request.dispfb2, request.display2, pmode.enableCircuit2);
        // DQ8 shows the same buffer on both circuits a line apart and blends
        // them 50/50, which hid interlace flicker on a TV. On a progressive
        // display it only blurs, by a native line at every internal scale.
        // Reading both at one origin leaves the blend a no-op.
        if (removeLineBlend.load(std::memory_order_relaxed) && circuit1.valid && circuit2.valid &&
            circuit1.fbp == circuit2.fbp && circuit1.psm == circuit2.psm &&
            circuit1.fbw == circuit2.fbw && circuit1.originX == circuit2.originX &&
            (circuit1.originY == circuit2.originY + 1u || circuit2.originY == circuit1.originY + 1u))
            circuit1.originY = circuit2.originY;
        if (!circuit1.valid && !circuit2.valid)
            return;
        if (circuit1.valid && circuit2.valid)
            ++stats.secondaryDisplayCircuits;
        // The buffers shown, for frameTarget (Rogue Galaxy: added).
        displayBase1.store(circuit1.valid ? circuit1.fbp << 5u : ~0u, std::memory_order_relaxed);
        displayBase2.store(circuit2.valid ? circuit2.fbp << 5u : ~0u, std::memory_order_relaxed);

        const uint32_t factor = std::max<uint32_t>(scale, 1u);
        frame.mode = GSPresentationMode::HostPixels;

        if (circuit1.valid && circuit2.valid &&
            acquireGpuComposition(request, pmode, circuit1, circuit2, halfHeightSource, factor, stage))
            return;

        // The path that makes owning the window worth it: when a single read
        // circuit is showing a buffer a render target still holds, the frame is
        // already on the GPU at full scale and goes straight to the swapchain.
        // No readback, no CPU compose, no upload.
        // Screenshots read the CPU-composed frame, which this path skips.
        if (window != nullptr && !gsScreenshotEnabled() && circuit1.valid != circuit2.valid) {
            const GsDisplaySetup &only = circuit1.valid ? circuit1 : circuit2;
            GsSurface *surface = targets->findColorSurface(only.fbp << 5u, only.psm);
            if (surface != nullptr && !surface->undefined && surface->scale == factor &&
                surface->needsUpload.empty() && surface->cpuPatches.empty() &&
                only.originX == 0u && only.originY == 0u &&
                !halfHeightSource) {
                frame.width = only.width * factor;
                frame.height = only.height * factor;
                frame.rowPitchBytes = 0u;
                frame.displayFbp = only.fbp;
                frame.sourceFbp = only.fbp;
                frame.mode = GSPresentationMode::BackendNative;
                ++stats.nativePresents;
                copyToPresentSource(surface->texture, surface->width * factor,
                                    surface->height * factor);
                stage.native = true;
                stage.haveFrame = true;
                return;
            }
        }

        if (circuit1.valid && circuit2.valid) {
            uint32_t fbp1 = 0u, fbp2 = 0u;
            bool preferred1 = false, preferred2 = false;
            // Neither circuit takes the preferred-source override when both are
            // live; it exists to rescue a single stale DISPFB.
            readCircuit(request, circuit1, halfHeightSource, false, factor, stage.crt1, fbp1, preferred1);
            readCircuit(request, circuit2, halfHeightSource, false, factor, stage.crt2, fbp2, preferred2);

            frame.width = std::max(circuit1.width, circuit2.width) * factor;
            frame.height = std::max(circuit1.height, circuit2.height) * factor;
            frame.rowPitchBytes = frame.width * 4u;
            frame.displayFbp = circuit1.fbp;
            frame.sourceFbp = fbp1;
        } else {
            const GsDisplaySetup &primary = circuit1.valid ? circuit1 : circuit2;
            uint32_t sourceFbp = 0u;
            bool usedPreferred = false;
            readCircuit(request, primary, halfHeightSource, true, factor, frame.pixels,
                        sourceFbp, usedPreferred);
            frame.width = primary.width * factor;
            frame.height = primary.height * factor;
            frame.rowPitchBytes = frame.width * 4u;
            frame.displayFbp = primary.fbp;
            frame.sourceFbp = sourceFbp;
            frame.usedPreferred = usedPreferred;
        }

        stage.pmode = pmode;
        stage.circuit1 = circuit1;
        stage.circuit2 = circuit2;
        stage.bgcolor = request.bgcolor;
        stage.dualCircuit = circuit1.valid && circuit2.valid;
        stage.haveFrame = true;
    }

    void updatePerformanceTitle(bool gsActive) {
        static const bool enabled = [] {
            const char *value = std::getenv("DQ8_GFX_SHOW_FPS");
            return value && std::strcmp(value, "0") != 0;
        }();
        if (!enabled || !window)
            return;
        const auto now = std::chrono::steady_clock::now();
        if (titleSampleStart == std::chrono::steady_clock::time_point{}) {
            titleSampleStart = now;
            titleCompletedStart = completedRenderCounter ? completedRenderCounter() : 0u;
            return;
        }
        ++titlePresents;
        titleActiveFrames += gsActive ? 1u : 0u;
        const double seconds = std::chrono::duration<double>(now - titleSampleStart).count();
        if (seconds < 2.0)
            return;
        // Rogue Galaxy: the window's own title in front, not "DQ8Recomp".
        char title[192];
        double renders = -1.0;
        if (completedRenderCounter) {
            const uint64_t completed = completedRenderCounter();
            renders = completed >= titleCompletedStart
                ? (completed - titleCompletedStart) / seconds : 0.0;
            std::snprintf(title, sizeof(title), "%s | %.1f render FPS | SDL GPU %.1f presents/s",
                          windowTitle.c_str(), renders, titlePresents / seconds);
            titleCompletedStart = completed;
        } else {
            std::snprintf(title, sizeof(title), "%s | SDL GPU: %.1f presents/s (%.1f active/s)",
                          windowTitle.c_str(), titlePresents / seconds, titleActiveFrames / seconds);
        }
        SDL_SetWindowTitle(window, title);
        if (renders >= 0.0) {
            std::fprintf(stderr, "[present] %.2f fps, %.2f GS-active/s, %.2f renders/s over %.3f s\n",
                         titlePresents / seconds, titleActiveFrames / seconds, renders, seconds);
        } else {
            std::fprintf(stderr, "[present] %.2f fps, %.2f GS-active/s over %.3f s\n",
                         titlePresents / seconds, titleActiveFrames / seconds, seconds);
        }
        titleSampleStart = now;
        titlePresents = titleActiveFrames = 0u;
    }

    void capturePresentedFrame(const PresentationSlot &slot, uint64_t tick) {
        static const char *path = std::getenv("DQ8_GFX_NATIVE_CAPTURE");
        static const bool series = std::getenv("DQ8_GFX_NATIVE_CAPTURE_SERIES") != nullptr;
        if (!path || !*path || (nativeCaptureAttempted && !series) || !slot.texture)
            return;
        const std::string trigger = std::string(path) + ".trigger";
        FILE *request = std::fopen(trigger.c_str(), "rb");
        if (!request)
            return;
        std::fclose(request);
        nativeCaptureAttempted = true;
        std::string output = path;
        if (series) {
            // Consume each request before readback so it cannot capture every frame.
            if (std::remove(trigger.c_str()) != 0)
                return;
            output += "." + std::to_string(++nativeCaptureNumber) + ".bmp";
        }

        // Read the leased snapshot that was actually displayed, after submission.
        // A one-shot request keeps readback stalls outside steady FPS measurements.
        std::vector<uint8_t> pixels;
        std::string error;
        const auto download = [&] { return device.downloadTexture(slot.texture, slot.width, slot.height, pixels, error); };
        if (window ? download() : submitAfterPending(download)) {
            std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)> surface(
                SDL_CreateSurfaceFrom(slot.width, slot.height, SDL_PIXELFORMAT_RGBA32,
                                      pixels.data(), slot.width * 4u), SDL_DestroySurface);
            if (surface && SDL_SaveBMP(surface.get(), output.c_str())) {
                std::fprintf(stderr, "[native-capture] %s %ux%u tick=%llu completed=%llu\n",
                             output.c_str(), slot.width, slot.height,
                             static_cast<unsigned long long>(tick),
                             static_cast<unsigned long long>(completedRenderCounter ? completedRenderCounter() : 0u));
                return;
            }
            error = SDL_GetError();
        }
        std::fprintf(stderr, "[native-capture] failed: %s\n", error.c_str());
    }

    void finishPresentation(PresentStage &stage) {
        PresentationFrame &frame = stage.frame;
        if (!stage.haveFrame || stage.native)
            return;

        if (stage.dualCircuit) {
            const GsPmodeState &pmode = stage.pmode;
            const GsDisplaySetup &circuit1 = stage.circuit1;
            const GsDisplaySetup &circuit2 = stage.circuit2;
            const uint32_t factor = std::max<uint32_t>(scale, 1u);

            // Background, then circuit 2, then circuit 1 blended over the top.
            const uint8_t background[3] = {static_cast<uint8_t>(stage.bgcolor),
                                           static_cast<uint8_t>(stage.bgcolor >> 8u),
                                           static_cast<uint8_t>(stage.bgcolor >> 16u)};
            frame.pixels.assign(static_cast<size_t>(frame.rowPitchBytes) * frame.height, 0u);
            for (uint32_t y = 0u; y < frame.height; ++y) {
                uint8_t *row = frame.pixels.data() + static_cast<size_t>(y) * frame.rowPitchBytes;
                for (uint32_t x = 0u; x < frame.width; ++x) {
                    row[x * 4u] = background[0];
                    row[x * 4u + 1u] = background[1];
                    row[x * 4u + 2u] = background[2];
                    row[x * 4u + 3u] = pmode.fixedAlpha;
                }
            }

            const uint32_t width2 = circuit2.width * factor;
            const uint32_t height2 = circuit2.height * factor;
            if (!pmode.blendWithBackground) {
                for (uint32_t y = 0u; y < std::min(height2, frame.height); ++y) {
                    std::memcpy(frame.pixels.data() + static_cast<size_t>(y) * frame.rowPitchBytes,
                                stage.crt2.data() + static_cast<size_t>(y) * width2 * 4u,
                                std::min(width2, frame.width) * 4u);
                }
            }

            const uint32_t width1 = circuit1.width * factor;
            const uint32_t height1 = circuit1.height * factor;
            for (uint32_t y = 0u; y < std::min(height1, frame.height); ++y) {
                const uint8_t *source = stage.crt1.data() + static_cast<size_t>(y) * width1 * 4u;
                uint8_t *destination = frame.pixels.data() +
                                       static_cast<size_t>(y) * frame.rowPitchBytes;
                for (uint32_t x = 0u; x < std::min(width1, frame.width); ++x) {
                    // MMOD picks ALP over circuit 1's own alpha; the doubling
                    // is the GS's /128 alpha convention again.
                    const uint32_t blendFactor =
                        pmode.useFixedAlpha
                            ? pmode.fixedAlpha
                            : std::min<uint32_t>(255u, static_cast<uint32_t>(source[x * 4u + 3u]) * 2u);
                    for (int channel = 0; channel < 3; ++channel) {
                        destination[x * 4u + channel] = blendCircuitChannel(
                            source[x * 4u + channel], destination[x * 4u + channel], blendFactor);
                    }
                    if (!pmode.alphaFromCircuit2)
                        destination[x * 4u + 3u] = source[x * 4u + 3u];
                }
            }
        }

        // The screen has no alpha channel; everything above needed it live.
        for (size_t offset = 3u; offset < frame.pixels.size(); offset += 4u)
            frame.pixels[offset] = 0xffu;

        // Composed on the CPU, but this backend still owns the window, so it
        // shows the frame itself rather than handing it back.
        if (window != nullptr) {
            ++stats.composedPresents;
            showHostPixels(frame);
            // Normally the pixels are dead once shown. While screenshots are on
            // the runtime wants them too, and it is this path -- not the native
            // one -- that it latches from.
            if (!gsScreenshotEnabled()) {
                frame.pixels.clear();
                frame.rowPitchBytes = 0u;
                frame.mode = GSPresentationMode::BackendNative;
            }
        }
    }

    bool pumpEvents() {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT ||
                event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED)
                windowCloseRequested = true;
            if (overlay && overlay->handleEvent(event))
                continue;
            // Hotplug always reaches the pad; input only while the game has it.
            const bool hotplug = event.type == SDL_EVENT_GAMEPAD_ADDED ||
                                 event.type == SDL_EVENT_GAMEPAD_REMOVED;
            if (hotplug || !overlay || !overlay->capturesInput())
                pad.handleEvent(event);
        }
        const bool captured = overlay && overlay->capturesInput();
        if (overlayCaptured && !captured)
            pad.suppressHeld();
        overlayCaptured = captured;
        pad.poll(SDL_GetKeyboardState(nullptr), SDL_GetKeyboardFocus() == window && !captured);
        // Only once the game has stopped presenting: between its own frames a
        // refresh would double the presents and could hold the next one up.
        const auto now = std::chrono::steady_clock::now();
        if (overlay && overlay->wantsFrames() && now - lastGameFrame >= std::chrono::milliseconds(50) &&
            now - lastShown >= std::chrono::milliseconds(15))
            refreshWindow();
        return !windowCloseRequested;
    }

    bool createDummyTexture() {
        SDL_GPUTextureCreateInfo info{};
        info.type = SDL_GPU_TEXTURETYPE_2D;
        info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        info.width = 1u;
        info.height = 1u;
        info.layer_count_or_depth = 1u;
        info.num_levels = 1u;
        info.sample_count = SDL_GPU_SAMPLECOUNT_1;
        info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
        dummyTexture = SDL_CreateGPUTexture(device.handle(), &info);
        if (!dummyTexture)
            return setError(std::string("SDL_CreateGPUTexture(placeholder): ") + SDL_GetError());
        return true;
    }
};

SdlGpuBackend::SdlGpuBackend() : m_impl(std::make_unique<Impl>()) {}

SdlGpuBackend::~SdlGpuBackend() {
    CancelPreparedPresentations();
    if (m_impl->device.valid()) {
        for (auto &slot : m_impl->presentationPool->slots) {
            for (auto *fence : {slot.prepared, slot.displayed}) {
                if (!fence) continue;
                SDL_WaitForGPUFences(m_impl->device.handle(), true, &fence, 1);
                SDL_ReleaseGPUFence(m_impl->device.handle(), fence);
            }
            if (slot.texture) SDL_ReleaseGPUTexture(m_impl->device.handle(), slot.texture);
            slot.texture = nullptr;
            slot.prepared = slot.displayed = nullptr;
        }
        std::string ignored;
        m_impl->device.submitPendingAndWait(ignored);
        m_impl->textures.reset();
        m_impl->targets.reset();
        if (m_impl->vertexBuffer)
            SDL_ReleaseGPUBuffer(m_impl->device.handle(), m_impl->vertexBuffer);
        m_impl->vertexUploads.release(m_impl->device);
        if (m_impl->dummyTexture)
            SDL_ReleaseGPUTexture(m_impl->device.handle(), m_impl->dummyTexture);
        for (const auto &feedback : m_impl->feedbackTextures)
            SDL_ReleaseGPUTexture(m_impl->device.handle(), feedback.texture);
        if (m_impl->paddingScratch)
            SDL_ReleaseGPUTexture(m_impl->device.handle(), m_impl->paddingScratch);
        if (m_impl->feedbackUpload)
            SDL_ReleaseGPUTransferBuffer(m_impl->device.handle(), m_impl->feedbackUpload);
        if (m_impl->presentUpload)
            SDL_ReleaseGPUTexture(m_impl->device.handle(), m_impl->presentUpload);
        for (auto *scratch : {&m_impl->sharpTexture, &m_impl->compositeTexture, &m_impl->captureTexture,
                              &m_impl->roundTrip.copy})
            m_impl->releaseScratch(*scratch);
        for (auto &level : m_impl->blurLevels)
            m_impl->releaseScratch(level);
        m_impl->releaseSideImage();
        if (m_impl->window) {
            SDL_ReleaseWindowFromGPUDevice(m_impl->device.handle(), m_impl->window);
            SDL_DestroyWindow(m_impl->window);
            m_impl->window = nullptr;
        }
    }
    m_impl->device.destroy();
}

void SdlGpuBackend::Initialize(uint8_t *vram, uint32_t vramSize) {
    std::lock_guard lock(m_impl->mutex);
    m_impl->last.valid = false;
    m_impl->vram.attach(vram, vramSize);
    m_impl->transferCommand = {};
    m_impl->patchingHostWrite = false;
    m_impl->transfer = std::make_unique<GsTransferEngine>(m_impl->vram);
    m_impl->transfer->setResolveHook([impl = m_impl.get()](const GsPageSet &pages) {
        // A local copy reads its destination too (overlaps follow the scan
        // order); when that destination is being patched instead of read
        // back, its pages stay the GPU's.
        impl->settleFor((impl->patchingHostWrite ? pages & ~impl->patchPages : pages) & ~impl->copySourceCurrent,
                        GsResolveReason::LocalRead);
    });
    m_impl->transfer->setInvalidateHook([impl = m_impl.get()](const GsPageSet &pages) {
        impl->invalidatePages(pages);
    });
    if (!m_impl->targets)
        m_impl->targets = std::make_unique<GsTargetCache>(m_impl->device, m_impl->vram);
    if (!m_impl->textures) {
        m_impl->textures = std::make_unique<GsTextureCache>(m_impl->device, m_impl->vram,
                                                            *m_impl->targets);
        m_impl->textures->setBeforeRewrite([impl = m_impl.get()](SDL_GPUTexture *texture) {
            // Queued draws still sample it: they go first.
            if (!impl->batches.empty() && (!texture || impl->batchesUse(texture)))
                impl->flushFor(impl->stats.flushRewrite);
        });
    }
    m_impl->targets->setScale(m_impl->scale);
    m_impl->targets->reset();
    m_impl->textures->reset();
}

void SdlGpuBackend::Reset() {
    std::lock_guard lock(m_impl->mutex);
    m_impl->clearBatches();
    m_impl->vertices.clear();
    m_impl->transferCommand = {};
    m_impl->patchingHostWrite = false;
    if (m_impl->transfer)
        m_impl->transfer->reset();
    if (m_impl->textures)
        m_impl->textures->reset();
    if (m_impl->targets)
        m_impl->targets->reset();
}

void SdlGpuBackend::Submit(const GSPrimitiveBatch &batch) {
    std::lock_guard lock(m_impl->mutex);
    Impl::ScopedTimer timer(m_impl->backendNanos);
    m_impl->submit(batch);
}

void SdlGpuBackend::SubmitMany(const GSPrimitiveBatch *const *batches, size_t count) {
    std::lock_guard lock(m_impl->mutex);
    Impl::ScopedTimer timer(m_impl->backendNanos);
    for (size_t i = 0; i < count; ++i)
        m_impl->submit(*batches[i]);
}

void SdlGpuBackend::BeginTransfer(const GSTransferCommand &command) {
    std::lock_guard lock(m_impl->mutex);
    m_impl->last.valid = false;
    if (!m_impl->transfer)
        return;
    Impl::ScopedTimer timer(m_impl->backendNanos);
    ++m_impl->stats.transfersBegun;
    m_impl->noteTransferInterrupted();
    if (command.direction == 2u) {
        if (!m_impl->flushFor(m_impl->stats.flushLocalCopy)) return;
        if (m_impl->gpuLocalCopy(command)) {
            m_impl->transferCommand = command;
            return;
        }
    }
    if (gsTargetTraceEnabled())
        std::fprintf(stderr, "[target] transfer dir=%u dbp=%05x dbw=%u dpsm=%02x %ux%u@%u,%u sbp=%05x sbw=%u spsm=%02x\n",
                     command.direction, command.bitbltbuf.dbp, command.bitbltbuf.dbw, command.bitbltbuf.dpsm,
                     command.trxreg.rrw, command.trxreg.rrh, command.trxpos.dsax, command.trxpos.dsay,
                     command.bitbltbuf.sbp, command.bitbltbuf.sbw, command.bitbltbuf.spsm);
    GsPageSet pages;
    bool patchLocalCopy = false;
    m_impl->coveredReleased.reset();
    if (command.direction == 0u || command.direction == 2u) {
        gsMarkPages(pages, command.bitbltbuf.dbp,
                    std::max<uint32_t>(command.bitbltbuf.dbw, 1u),
                    command.bitbltbuf.dpsm,
                    command.trxreg.rrw, command.trxreg.rrh,
                    command.trxpos.dsax, command.trxpos.dsay);
        // A host write lands ahead of the queued draws unless it conflicts
        // with them (transferConflicts); a local copy always waits for them.
        if (command.direction != 0u || m_impl->transferConflicts(pages)) {
            if (!m_impl->batches.empty())
                ++m_impl->stats.transferFlushes;
            if (!m_impl->flushDraws()) return;
        } else if (!m_impl->batches.empty()) {
            ++m_impl->stats.transferFlushesSkipped;
        }
        // A write the targets owning its pages can take as a patch leaves
        // them theirs: a host write patches per payload (UploadImage); a
        // local copy, done whole in begin(), patches here after it.
        if (!m_impl->patchableHostWrite(command.bitbltbuf.dpsm, pages)) {
            GsPageSet covered;
            gsMarkCoveredPages(covered, command.bitbltbuf.dbp,
                               std::max<uint32_t>(command.bitbltbuf.dbw, 1u),
                               command.bitbltbuf.dpsm, command.trxreg.rrw, command.trxreg.rrh,
                               command.trxpos.dsax, command.trxpos.dsay);
            if (!m_impl->settleFor(pages, command.direction == 0u ? GsResolveReason::HostWrite
                                                                   : GsResolveReason::LocalCopy,
                                   true, covered, false))
                return;
            m_impl->coveredTransferOpen = command.direction == 0u && covered.any();
        } else if (command.direction == 2u) {
            patchLocalCopy = true;
            m_impl->patchPages = pages;
            m_impl->patchingHostWrite = true;
        }
    }
    if (command.direction == 2u) {
        // Source pages whose words local memory holds (host-written, or
        // read back before) need no readback for the copy to read them.
        const GSBitBltBuf &blt = command.bitbltbuf;
        const uint32_t sbw = std::max<uint32_t>(blt.sbw, 1u);
        GsPageSet sourcePages, needed;
        gsMarkPages(sourcePages, blt.sbp, sbw, blt.spsm, command.trxreg.rrw, command.trxreg.rrh,
                    command.trxpos.ssax, command.trxpos.ssay);
        const uint32_t ssax = command.trxpos.ssax, ssay = command.trxpos.ssay;
        m_impl->targets->markReadPages(needed, blt.sbp, sbw, blt.spsm,
                                       {ssax, ssay, ssax + command.trxreg.rrw, ssay + command.trxreg.rrh});
        m_impl->copySourceCurrent = sourcePages & ~needed;
    }
    m_impl->noteRoundTrip(command);
    m_impl->transferCommand = command;
    m_impl->transfer->begin(command);
    m_impl->copySourceCurrent.reset();
    if (patchLocalCopy) {
        m_impl->targets->patchHostWrite(pages, command.bitbltbuf.dbp, std::max<uint32_t>(command.bitbltbuf.dbw, 1u),
                                        command.bitbltbuf.dpsm, command.trxpos.dsax, command.trxpos.dsay,
                                        command.trxreg.rrw, 0u, command.trxreg.rrw * command.trxreg.rrh);
        m_impl->patchingHostWrite = false;
    }
}

void SdlGpuBackend::UploadImage(const uint8_t *data, uint32_t sizeBytes) {
    std::lock_guard lock(m_impl->mutex);
    m_impl->last.valid = false;
    Impl::ScopedTimer timer(m_impl->backendNanos);
    m_impl->stats.transferBytes += sizeBytes;
    if (!m_impl->transfer) return;
    const bool roundTripReturned = m_impl->noteRoundTripUpload(data, sizeBytes);
    const auto before = m_impl->transfer->snapshot();
    const auto &command = m_impl->transferCommand;
    const uint32_t dpsm = command.bitbltbuf.dpsm;
    const bool patchable = m_impl->patchCt32Only ? (dpsm & 0x3fu) == GS_PSM_CT32
                                                 : GsTargetCache::gsPatchableHostWrite(dpsm);
    GsPageSet pages;
    if (before.direction == 0u && before.totalPixels != 0u && patchable && data && sizeBytes != 0u) {
        gsMarkPages(pages, command.bitbltbuf.dbp,
                    std::max<uint32_t>(command.bitbltbuf.dbw, 1u), dpsm,
                    command.trxreg.rrw, command.trxreg.rrh,
                    command.trxpos.dsax, command.trxpos.dsay);
        // Draws can occur between IMAGE fragments, so recheck ownership at
        // each payload before any CPU bytes change; the queued draws go
        // first only when they touch these pages (transferConflicts).
        if (m_impl->transferConflicts(pages)) {
            if (!m_impl->batches.empty())
                ++m_impl->stats.transferFlushes;
            if (!m_impl->flushDraws()) return;
        }
        m_impl->patchingHostWrite = m_impl->targets->canPatchHostWrite(pages);
        GsPageSet covered;
        gsMarkCoveredPages(covered, command.bitbltbuf.dbp,
                           std::max<uint32_t>(command.bitbltbuf.dbw, 1u), dpsm,
                           command.trxreg.rrw, command.trxreg.rrh,
                           command.trxpos.dsax, command.trxpos.dsay);
        if (!m_impl->patchingHostWrite) {
            if (!m_impl->settleFor(pages, GsResolveReason::HostWrite, true, covered, false)) return;
            m_impl->coveredTransferOpen = m_impl->coveredTransferOpen || covered.any();
        }
    }
    m_impl->transfer->upload(data, sizeBytes);
    if (m_impl->coveredTransferOpen) {
        const auto state = m_impl->transfer->snapshot();
        m_impl->coveredTransferOpen = state.direction == 0u && state.copiedPixels < state.totalPixels;
    }
    if (m_impl->patchingHostWrite && m_impl->coveredRelease) {
        // Whole pages of a live target leave it, each once the rows that
        // fill it have landed: a draw between two payloads of a split
        // upload, or an upload cut short, then still finds the GPU's pixels
        // where the bytes never came. Only the partial pages are patched
        // (Rogue Galaxy: added; released at the first payload before).
        const auto after = m_impl->transfer->snapshot();
        const uint32_t width = std::max<uint32_t>(command.trxreg.rrw, 1u);
        const uint32_t rowsLanded = std::min<uint32_t>(after.copiedPixels / width, command.trxreg.rrh);
        if (rowsLanded != 0u) {
            GsPageSet landed;
            gsMarkCoveredPages(landed, command.bitbltbuf.dbp, std::max<uint32_t>(command.bitbltbuf.dbw, 1u), dpsm,
                               width, rowsLanded, command.trxpos.dsax, command.trxpos.dsay);
            landed &= ~m_impl->coveredReleased;
            if (landed.any()) {
                const uint32_t released = m_impl->targets->releaseCoveredPages(landed);
                if (released != 0u) {
                    ++m_impl->stats.coveredReleases;
                    m_impl->stats.coveredPages += released;
                }
                m_impl->coveredReleased |= landed;
                pages &= ~landed;
                if (pages.none())
                    m_impl->patchingHostWrite = false;
            }
        }
    }
    if (m_impl->patchingHostWrite) {
        const auto after = m_impl->transfer->snapshot();
        if (after.copiedPixels > before.copiedPixels)
            m_impl->targets->patchHostWrite(pages, command.bitbltbuf.dbp,
                std::max<uint32_t>(command.bitbltbuf.dbw, 1u), dpsm,
                command.trxpos.dsax, command.trxpos.dsay, command.trxreg.rrw,
                before.copiedPixels, after.copiedPixels);
        m_impl->patchingHostWrite = false;
    }
    if (roundTripReturned)
        m_impl->applyRoundTrip(command);
}

void SdlGpuBackend::Flush() {
    std::lock_guard lock(m_impl->mutex);
    m_impl->flushDraws();
}

void SdlGpuBackend::TextureFlush() {
    // The texture cache is keyed on local-memory ranges and invalidated by the
    // transfers that write them, so an explicit TEXFLUSH has nothing to do.
}

void SdlGpuBackend::Sync(GSSyncReason reason) {
    std::lock_guard lock(m_impl->mutex);
    m_impl->last.valid = false;
    switch (reason) {
    case GSSyncReason::LocalToHost:
    case GSSyncReason::DebugReadback:
    case GSSyncReason::Reset:
        m_impl->settleAll();
        break;
    case GSSyncReason::Finish:
    case GSSyncReason::Presentation:
    default:
        m_impl->flushDraws();
        break;
    }
}

PresentationFrame SdlGpuBackend::Present(const GSPresentationRequest &request) {
    return DisplayPreparedPresentation(PreparePresentation(request));
}

GSPresentationTicket SdlGpuBackend::PreparePresentation(const GSPresentationRequest &request) {
    auto prepared = m_impl->leasePresentation();
    prepared->sourceVsyncTick = request.vsyncTick;
    auto &slot = prepared->pool->slots[prepared->slot];
    Impl::PresentStage stage;
    {
        std::lock_guard lock(m_impl->mutex);
        Impl::ScopedTimer timer(m_impl->backendNanos);
        m_impl->retirePresentationFences(slot);
        m_impl->preparingSlot = &slot;
        struct ClearPreparingSlot {
            Impl &impl;
            ~ClearPreparingSlot() { impl.preparingSlot = nullptr; }
        } clearPreparingSlot{*m_impl};
        m_impl->presentSource = slot.texture;
        m_impl->presentSourceWidth = slot.width;
        m_impl->presentSourceHeight = slot.height;
        m_impl->reportStats();
        m_impl->lastError.clear();
        m_impl->applyRequestedScale();
        m_impl->acquirePresentation(request, stage);
        m_impl->finishPresentation(stage);
        if (!m_impl->lastError.empty())
            throw std::runtime_error(m_impl->lastError);
    }
    prepared->haveFrame = stage.haveFrame;
    prepared->gsActive = stage.gsActive;
    prepared->download = stage.download;
    prepared->frame = std::move(stage.frame);
    return prepared;
}

PresentationFrame SdlGpuBackend::DisplayPreparedPresentation(const GSPresentationTicket &ticket) {
    const auto *prepared = dynamic_cast<const Impl::PreparedFrame *>(ticket.get());
    if (!prepared || prepared->pool != m_impl->presentationPool)
        throw std::invalid_argument("SDL GPU presentation ticket belongs to another backend");
    Impl::DisplayTimer timer(m_impl->displayNanos);
    {
        std::lock_guard lock(prepared->pool->mutex);
        if (prepared->pool->closing)
            throw std::runtime_error("SDL GPU presentation cancelled");
    }
    auto &slot = prepared->pool->slots[prepared->slot];
    if (prepared->haveFrame && m_impl->window != nullptr) {
        if (slot.prepared && !SDL_WaitForGPUFences(m_impl->device.handle(), true, &slot.prepared, 1))
            throw std::runtime_error(std::string("SDL_WaitForGPUFences(snapshot): ") + SDL_GetError());
        std::string error;
        if (!m_impl->showInWindow(&slot, error))
            throw std::runtime_error(error);
        m_impl->lastDisplayedSlot = prepared->slot;
        m_impl->lastGameFrame = m_impl->lastShown;
        m_impl->updatePerformanceTitle(prepared->gsActive);
    }
    PresentationFrame frame = prepared->frame;
    if (prepared->download) {
        std::string error;
        const auto download = [&] {
            return m_impl->device.downloadTexture(slot.texture, slot.width, slot.height, frame.pixels, error);
        };
        // Without a window this submission may defragment (submitAfterPending).
        if (!(m_impl->window ? download() : m_impl->submitAfterPending(download)))
            throw std::runtime_error(error);
    }
    if (prepared->haveFrame)
        m_impl->capturePresentedFrame(slot, prepared->sourceVsyncTick);
    return frame;
}

void SdlGpuBackend::CancelPreparedPresentations() noexcept {
    std::lock_guard lock(m_impl->presentationPool->mutex);
    m_impl->presentationPool->closing = true;
    m_impl->presentationPool->available.notify_all();
}

bool SdlGpuBackend::ClearFramebuffer(const GSContext &context, uint32_t rgba) {
    std::lock_guard lock(m_impl->mutex);
    if (!m_impl->device.valid() || !gsIsColorFramePsm(context.frame.psm))
        return false;
    if (!m_impl->flushDraws())
        return false;

    // A clear covers the buffer and never the page after it: a scissor ending
    // past FBW pages must not widen the surface (see neededWidth in submit).
    const uint32_t bufferPixels = context.frame.fbw * 64u;
    uint32_t width = static_cast<uint32_t>(context.scissor.x1) + 1u;
    if (bufferPixels != 0u && width > bufferPixels)
        width = bufferPixels;
    std::string error;
    GsSurface *surface = m_impl->targets->acquire(
        context.frame.fbp << 5u, context.frame.fbw, context.frame.psm, false,
        width, static_cast<uint32_t>(context.scissor.y1) + 1u, error);
    Impl::ResolveReasonScope scope(*m_impl->targets, GsResolveReason::View);
    if (!surface || !m_impl->targets->prepareColorView(*surface, error)) {
        m_impl->setError(std::move(error));
        return false;
    }
    return m_impl->clearSurface(*surface,
                                static_cast<float>(rgba & 0xffu) / 255.0f,
                                static_cast<float>((rgba >> 8u) & 0xffu) / 255.0f,
                                static_cast<float>((rgba >> 16u) & 0xffu) / 255.0f,
                                static_cast<float>((rgba >> 24u) & 0xffu) / 255.0f);
}

uint32_t SdlGpuBackend::ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes) {
    std::lock_guard lock(m_impl->mutex);
    if (!m_impl->transfer)
        return 0u;
    const uint32_t consumed = m_impl->transfer->consumeLocalToHost(dst, maxBytes);
    m_impl->noteRoundTripRead(dst, consumed);
    return consumed;
}

uint32_t SdlGpuBackend::ReadVram(uint32_t psm, uint32_t base, uint32_t bw,
                                 uint32_t x, uint32_t y) const {
    std::lock_guard lock(m_impl->mutex);
    GsPageSet pages;
    gsMarkPages(pages, base, bw, psm, x + 1u, y + 1u);
    m_impl->settleFor(pages, GsResolveReason::LocalRead);
    return m_impl->vram.read(psm, base, bw, x, y);
}

void SdlGpuBackend::WriteVram(uint32_t psm, uint32_t base, uint32_t bw,
                              uint32_t x, uint32_t y, uint32_t value) {
    std::lock_guard lock(m_impl->mutex);
    m_impl->last.valid = false;
    GsPageSet pages;
    gsMarkPages(pages, base, bw, psm, x + 1u, y + 1u);
    m_impl->settleFor(pages, GsResolveReason::HostWrite, true);
    m_impl->vram.write(psm, base, bw, x, y, value);
    m_impl->invalidatePages(pages);
}

void SdlGpuBackend::SnapshotVram(std::vector<uint8_t> &out) const {
    std::lock_guard lock(m_impl->mutex);
    m_impl->settleAll();
    out.assign(m_impl->vram.data(), m_impl->vram.data() + m_impl->vram.size());
}

GSTransferSnapshot SdlGpuBackend::GetTransferSnapshot() const {
    std::lock_guard lock(m_impl->mutex);
    if (!m_impl->transfer)
        return {};
    return m_impl->transfer->snapshot();
}

void SdlGpuBackend::setCompletedRenderCounter(CompletedRenderCounter counter) {
    m_impl->completedRenderCounter = counter;
    m_impl->titleSampleStart = {};
    m_impl->titlePresents = m_impl->titleActiveFrames = 0u;
}

void SdlGpuBackend::setResolutionScale(uint32_t scale) {
    std::lock_guard lock(m_impl->mutex);
    m_impl->last.valid = false;
    m_impl->flushDraws();
    std::string error;
    if (m_impl->targets) {
        m_impl->targets->resolveAll(error);
        m_impl->targets->setScale(scale);
        m_impl->scale = m_impl->targets->scale();
    } else {
        m_impl->scale = std::clamp<uint32_t>(scale, 1u, 8u);
    }
    m_impl->activeScale.store(m_impl->scale, std::memory_order_release);
}

uint32_t SdlGpuBackend::resolutionScale() const {
    std::lock_guard lock(m_impl->mutex);
    return m_impl->scale;
}

void SdlGpuBackend::requestResolutionScale(uint32_t scale) {
    m_impl->requestedScale.store(std::clamp<uint32_t>(scale, 1u, 8u), std::memory_order_release);
}

uint32_t SdlGpuBackend::activeResolutionScale() const {
    return m_impl->activeScale.load(std::memory_order_acquire);
}

void SdlGpuBackend::setSideImageLoader(SdlGpuSideImageLoader loader) {
    m_impl->sideImageLoader = std::move(loader);
}

void SdlGpuBackend::setGameWidescreenQuery(GameWidescreenQuery query) {
    m_impl->widescreenQuery = query;
}

void SdlGpuBackend::setRegionReplacements(RegionLookup lookup, RegionPalettes palettes) {
    // The texture cache exists from createSdlGpuBackend on.
    if (!m_impl->textures)
        return;
    m_impl->regionPack = static_cast<bool>(lookup);
    m_impl->textures->setRegionLookup(std::move(lookup), std::move(palettes));
}

void SdlGpuBackend::setRegionReplacementsEnabled(bool on) {
    m_impl->regionPackEnabled.store(on, std::memory_order_relaxed);
}

void SdlGpuBackend::invalidateRegionReplacements() {
    if (m_impl->textures)
        m_impl->textures->invalidateRegions();
}

int SdlGpuBackend::gameWidescreen() const {
    return m_impl->gameWidescreen();
}

void SdlGpuBackend::setOverlay(SdlGpuOverlay *overlay) {
    m_impl->overlay = overlay;
    m_impl->overlayCaptured = false;
}

void SdlGpuBackend::setDisplayOptions(const SdlGpuDisplayOptions &options) {
    m_impl->displayOptions = options;
    m_impl->removeLineBlend.store(options.removeLineBlend, std::memory_order_relaxed);
}

const SdlGpuDisplayOptions &SdlGpuBackend::displayOptions() const {
    return m_impl->displayOptions;
}

bool SdlGpuBackend::supportsPresentMode(SDL_GPUPresentMode mode) const {
    return m_impl->window &&
           SDL_WindowSupportsGPUPresentMode(m_impl->device.handle(), m_impl->window, mode);
}

SDL_Window *SdlGpuBackend::window() const {
    return m_impl->window;
}

SDL_GPUDevice *SdlGpuBackend::gpuDevice() const {
    return m_impl->device.handle();
}

SDL_GPUTextureFormat SdlGpuBackend::windowTextureFormat() const {
    return m_impl->window ? SDL_GetGPUSwapchainTextureFormat(m_impl->device.handle(), m_impl->window)
                          : SDL_GPU_TEXTUREFORMAT_INVALID;
}

SdlPadInput &SdlGpuBackend::padInput() {
    return m_impl->pad;
}

SdlGpuRect SdlGpuBackend::displayRect() const {
    return m_impl->lastRect;
}

void SdlGpuBackend::requestCapture(bool withOverlay, SdlGpuCaptureCallback done) {
    m_impl->captureRequests.push_back({withOverlay, std::move(done)});
}

bool SdlGpuBackend::openWindow(const char *title, uint32_t width, uint32_t height,
                               std::string &error, const SdlGpuWindowPlacement &placement) {
    std::lock_guard lock(m_impl->mutex);
    if (!m_impl->device.valid()) {
        error = "SDL GPU device is not available";
        return false;
    }
    if (m_impl->window != nullptr)
        return true;

    if (width == 0u || height == 0u) {
        // 4:3, at the largest whole multiple of the GS height that leaves
        // room on the desktop.
        SDL_Rect usable{0, 0, 1280, 960};
        SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &usable);
        const uint32_t multiple = std::max<uint32_t>(
            1u, static_cast<uint32_t>(usable.h * 0.85 / 448.0));
        height = 448u * multiple;
        width = height * 4u / 3u;
    }
    // High pixel density: on a scaled desktop the swapchain is then as many
    // pixels as the window covers, rather than its logical size stretched by
    // the compositor, which would shrink a high internal resolution and blur
    // what is left.
    // DQ8_GFX_HIDDEN_WINDOW=1: the window is never shown or raised, for
    // scripted runs that should not take the desktop; the GS work is the
    // same, and frames reach DQ8_GFX_SCREENSHOT_DIR (Rogue Galaxy: added).
    const char *hiddenWindow = std::getenv("DQ8_GFX_HIDDEN_WINDOW");
    const bool hidden = hiddenWindow && *hiddenWindow && *hiddenWindow != '0';
    const SDL_PropertiesID properties = SDL_CreateProperties();
    SDL_SetStringProperty(properties, SDL_PROP_WINDOW_CREATE_TITLE_STRING, title);
    SDL_SetNumberProperty(properties, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, width);
    SDL_SetNumberProperty(properties, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, height);
    SDL_SetNumberProperty(properties, SDL_PROP_WINDOW_CREATE_X_NUMBER,
                          placement.placed ? placement.x : SDL_WINDOWPOS_CENTERED);
    SDL_SetNumberProperty(properties, SDL_PROP_WINDOW_CREATE_Y_NUMBER,
                          placement.placed ? placement.y : SDL_WINDOWPOS_CENTERED);
    SDL_SetNumberProperty(properties, SDL_PROP_WINDOW_CREATE_FLAGS_NUMBER,
                          SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY |
                              (hidden ? SDL_WINDOW_HIDDEN : 0u) |
                              (placement.maximized && !hidden ? SDL_WINDOW_MAXIMIZED : 0u));
    m_impl->window = SDL_CreateWindowWithProperties(properties);
    SDL_DestroyProperties(properties);
    if (!m_impl->window) {
        error = std::string("SDL_CreateWindow: ") + SDL_GetError();
        return false;
    }
    m_impl->windowTitle = title ? title : "";
    setWindowIcon(m_impl->window);
    if (!SDL_ClaimWindowForGPUDevice(m_impl->device.handle(), m_impl->window)) {
        error = std::string("SDL_ClaimWindowForGPUDevice: ") + SDL_GetError();
        SDL_DestroyWindow(m_impl->window);
        m_impl->window = nullptr;
        return false;
    }
    if (!hidden) {
        SDL_ShowWindow(m_impl->window);
        SDL_RaiseWindow(m_impl->window);
    }
    std::string inputError;
    if (!m_impl->pad.initialize(inputError))
        std::fprintf(stderr, "[pad] %s; keyboard remains available\n", inputError.c_str());
    return true;
}

bool SdlGpuBackend::hasWindow() const {
    std::lock_guard lock(m_impl->mutex);
    return m_impl->window != nullptr;
}

// Window thread only, like everything the window and pad touch, so neither
// takes the backend lock: the overlay calls back into the backend from here.
bool SdlGpuBackend::pumpEvents() {
    if (!m_impl->window)
        return true;
    return m_impl->pumpEvents();
}

void SdlGpuBackend::hostPadState(uint32_t &held, uint32_t &pressed, uint32_t &sticks) const {
    m_impl->pad.read(held, pressed, sticks);
}

bool SdlGpuBackend::deviceReady() const {
    std::lock_guard lock(m_impl->mutex);
    return m_impl->device.valid();
}

std::string SdlGpuBackend::driverName() const {
    std::lock_guard lock(m_impl->mutex);
    return m_impl->device.driverName();
}

std::string SdlGpuBackend::videoDriverName() const {
    const char *driver = SDL_GetCurrentVideoDriver();
    return driver != nullptr ? driver : "none";
}

std::string SdlGpuBackend::lastError() const {
    std::lock_guard lock(m_impl->mutex);
    return m_impl->lastError;
}

SdlGpuStats SdlGpuBackend::stats() const {
    std::lock_guard lock(m_impl->mutex);
    SdlGpuStats result = m_impl->stats;
    if (m_impl->targets) {
        const GsTargetCacheStats &cache = m_impl->targets->stats();
        result.colorResolves = cache.colorResolves;
        result.colorRefreshes = cache.colorRefreshes;
        result.resolvedPixels = cache.resolvedPixels;
        result.refreshedPixels = cache.refreshedPixels;
        result.surfacesCreated = cache.surfacesCreated;
    }
    if (m_impl->textures) {
        const GsTextureCacheStats &cache = m_impl->textures->stats();
        result.textureLookups = cache.lookups;
        result.textureHits = cache.hits;
        result.textureBuilds = cache.builds;
        result.textureInvalidations = cache.invalidations;
        result.textureEvictions = cache.evictions;
        result.texelsExpanded = cache.texelsExpanded;
        result.texturesFromRenderTargets = cache.renderTargetSources;
    }
    result.pipelinesCreated = m_impl->device.pipelineCount();
    return result;
}

void SdlGpuBackend::resetStats() {
    std::lock_guard lock(m_impl->mutex);
    m_impl->stats = {};
    if (m_impl->targets)
        m_impl->targets->resetStats();
    if (m_impl->textures)
        m_impl->textures->resetStats();
}

std::unique_ptr<SdlGpuBackend> createSdlGpuBackend(std::string &error) {
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        error = std::string("SDL_InitSubSystem(video): ") + SDL_GetError();
        return nullptr;
    }
    auto backend = std::make_unique<SdlGpuBackend>();
    if (!backend->m_impl->device.create(error))
        return nullptr;
    if (!backend->m_impl->createDummyTexture()) {
        error = backend->m_impl->lastError;
        return nullptr;
    }
    backend->m_impl->targets = std::make_unique<GsTargetCache>(
        backend->m_impl->device, backend->m_impl->vram);
    backend->m_impl->textures = std::make_unique<GsTextureCache>(
        backend->m_impl->device, backend->m_impl->vram, *backend->m_impl->targets);
    backend->m_impl->textures->setBeforeRewrite([impl = backend->m_impl.get()](SDL_GPUTexture *texture) {
        if (!impl->batches.empty() && (!texture || impl->batchesUse(texture)))
            impl->flushDraws();
    });
    return backend;
}

} // namespace dq8::gfx
