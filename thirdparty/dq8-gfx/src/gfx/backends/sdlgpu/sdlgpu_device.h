// SDL3 GPU device, shader modules and the graphics-pipeline cache.
//
// Separated from the backend so pipeline state translation -- the part that
// encodes GS semantics -- can be read and tested without a device.
#pragma once

#include "gfx/gs/gs_state.h"

#include <SDL3/SDL_gpu.h>

#include <array>
#include <cstdint>
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

namespace dq8::gfx {

// How a GS ALPHA equation was mapped onto the host blend unit.
//
// out = (A - B) * C / 128 + D, with A/B/D in {Cs, Cd, 0} and C in {As, Ad,
// FIX}. Every term is linear in Cs and Cd with at most one shared factor, so
// nearly all of the 3*3*3*3 combinations land on a fixed-function blend. The
// exceptions are the ones needing a coefficient of (1 + C) on an operand,
// which no blend factor can express; those are reported inexact and
// approximated rather than silently mapped.
struct GsBlendTranslation {
    bool enabled = false;
    SDL_GPUBlendFactor sourceFactor = SDL_GPU_BLENDFACTOR_ONE;
    SDL_GPUBlendFactor destinationFactor = SDL_GPU_BLENDFACTOR_ZERO;
    SDL_GPUBlendOp operation = SDL_GPU_BLENDOP_ADD;
    // Set when C is FIX; the caller programs it with SDL_SetGPUBlendConstants.
    bool usesConstant = false;
    float constant = 0.0f;
    // False when the equation had to be approximated.
    bool exact = true;
};

GsBlendTranslation gsTranslateBlend(const GsAlphaState &alpha, bool blendEnabled);

// GS depth compares are reversed relative to the usual convention: a larger Z
// is nearer.
SDL_GPUCompareOp gsTranslateDepthCompare(GsDepthTest test, bool enabled);

// One vertex as the pipeline consumes it. Positions stay in GS window pixels
// so resolution scaling is entirely a viewport concern.
struct GsGpuVertex {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;  // normalised depth, 0..1
    float q = 1.0f;
    float s = 0.0f;
    float t = 0.0f;
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float a = 0.0f;
    float fog = 1.0f;
    float pad = 0.0f;
};

static_assert(sizeof(GsGpuVertex) == 48u, "vertex layout must match gs_draw.vert");

// Uniform blocks, laid out to match the two shaders.
struct GsVertexUniforms {
    float targetSize[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    float adjust[4] = {0.0f, 0.0f, 1.0f, 0.0f};
};

// gs_native.vert's (Rogue Galaxy: added): GsVertexUniforms' two, then where
// the draw's meshes start in the storage buffer of GsNativeMeshRecords.
struct GsNativeVertexUniforms {
    GsVertexUniforms base;
    float meshes[4] = {}; // x: the submission's records before this flush's
};

// A native strip's transform (GSNativeMesh), one per mesh in a storage
// buffer, so the meshes of one draw state share a draw (Rogue Galaxy: added).
// Each vertex names its record.
struct GsNativeMeshRecord {
    float matrix[4][4] = {};
    float fog[4] = {};
    float colorMax[4] = {};
    float misc[4] = {}; // XYOFFSET x and y in pixels, 1 / the depth maximum
    float depth[4] = {1.0f, 3.402823466e38f, 0.0f, 0.0f}; // the projected z's scale and cap
};

static_assert(sizeof(GsNativeMeshRecord) == 128u, "record layout must match gs_native.vert");

struct GsFragmentUniforms {
    uint32_t control[4] = {0u, 0u, 0u, 0u};
    float fog[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float textureSize[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    float region[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float misc[4] = {1.0f, 0.0f, 0.0f, 0.0f};
    uint32_t blend[4] = {};
};

struct GsDisplayUniforms {
    uint32_t control[4]{}; // scale, half-height source, fixed alpha, background blend
    uint32_t circuit1[4]{}; // native origin x/y, scaled output width/height
    uint32_t circuit2[4]{};
    uint32_t format[4]{}; // PSM1, PSM2, ALP, BGCOLOR
};

// Feature bits in GsFragmentUniforms::control[0]; must match gs_draw.frag.
inline constexpr uint32_t kFragFlagTme = 1u << 0u;
inline constexpr uint32_t kFragFlagFst = 1u << 1u;
inline constexpr uint32_t kFragFlagTcc = 1u << 2u;
inline constexpr uint32_t kFragFlagFge = 1u << 3u;
inline constexpr uint32_t kFragFlagAte = 1u << 4u;
inline constexpr uint32_t kFragFlagLinear = 1u << 5u;
// Point-sampled UV (2D) draw into an upscaled target: take each texel where
// the GS would, at the native pixel's sample point. The target's scale sits
// at kFragTargetScaleShift.
inline constexpr uint32_t kFragFlagNativeGrid = 1u << 6u;
inline constexpr uint32_t kFragTargetScaleShift = 24u;

struct GsPipelineKey {
    uint32_t colorFormat = 0u;
    uint32_t depthFormat = 0u;
    uint8_t hasDepth = 0u;
    uint8_t depthCompare = 0u;
    uint8_t depthWrite = 0u;
    uint8_t colorWriteMask = 0xfu;
    uint8_t blendEnabled = 0u;
    uint8_t sourceFactor = 0u;
    uint8_t destinationFactor = 0u;
    uint8_t blendOperation = 0u;
    // The draw shader that reads the destination from a snapshot bound as a
    // second texture, with the blend done in the shader (Rogue Galaxy: added).
    uint8_t destinationCopy = 0u;
    // Native strips (Rogue Galaxy: added): gs_native.vert, as an indexed
    // triangle list, and the SDL_GPUCullMode their backfaces need.
    uint8_t nativeMesh = 0u;
    uint8_t cullMode = 0u;
    // A source-alpha blend whose factor comes from the shader's second
    // output, the alpha written exact in the same pass (Rogue Galaxy: added).
    uint8_t dualSource = 0u;

    bool operator==(const GsPipelineKey &other) const = default;
};

struct GsPipelineKeyHash {
    size_t operator()(const GsPipelineKey &key) const;
};

class SdlGpuDevice {
public:
    SdlGpuDevice() = default;
    ~SdlGpuDevice();

    SdlGpuDevice(const SdlGpuDevice &) = delete;
    SdlGpuDevice &operator=(const SdlGpuDevice &) = delete;

    bool create(std::string &error);
    void destroy();

    SDL_GPUDevice *handle() const { return m_device; }
    bool valid() const { return m_device != nullptr; }
    std::string driverName() const;

    // Depth format chosen at creation from what the device actually supports.
    SDL_GPUTextureFormat depthFormat() const { return m_depthFormat; }

    // Pipelines are created on demand and live for the device's lifetime;
    // the set of distinct GS states a game uses is small and bounded.
    SDL_GPUGraphicsPipeline *pipeline(const GsPipelineKey &key, std::string &error);

    // Reinterprets identical GS pages between CT32 and CT16 views, of
    // width x height GS pixels at `scale` physical pixels each.
    // The caller validates extents/ownership and commits their state afterward.
    bool reinterpretColor(SDL_GPUTexture *source, SDL_GPUTexture *destination,
                          uint32_t width, uint32_t height, bool destination16, uint32_t scale,
                          std::string &error);
    // The same recorded into a command buffer the caller owns and submits,
    // writing only `scissor` (physical pixels) when given and keeping the
    // rest of the destination: a feedback snapshot's padding row of a CT16
    // view, from the CT32 target holding its bytes (Rogue Galaxy: added).
    bool recordReinterpret(SDL_GPUCommandBuffer *commands, SDL_GPUTexture *source, SDL_GPUTexture *destination,
                           uint32_t width, uint32_t height, bool destination16, uint32_t scale,
                           const SDL_Rect *scissor, std::string &error);

    // Builds an 8-bit indexed texture from the CT32 target holding its bytes,
    // through `palette` (256 RGBA8). `pages`: the texture's base page and pages
    // per row, then the target's; the target must cover every page read.
    bool expandIndexed8(SDL_GPUTexture *target, const uint32_t *palette, SDL_GPUTexture *destination,
                        uint32_t width, uint32_t height, const SDL_Rect &region,
                        const std::array<uint32_t, 4> &pages, std::string &error);

    // A GS local-to-local transfer between CT32 targets at the same scale.
    // `mapping` holds four codes per pixel of `box` in the destination (see
    // gs_local_copy.frag). `source` may be `destination`; both are read from
    // before the pass writes.
    bool localCopy(SDL_GPUTexture *source, SDL_GPUTexture *destination, uint32_t width, uint32_t height,
                   uint32_t scale, const SDL_Rect &box, const std::vector<uint32_t> &mapping,
                   std::string &error);

    bool composeDisplay(SDL_GPUTexture *circuit1, SDL_GPUTexture *circuit2,
                        SDL_GPUTexture *destination, uint32_t width, uint32_t height,
                        const GsDisplayUniforms &uniforms, SDL_GPUFence *&completed, std::string &error);

    bool downloadTexture(SDL_GPUTexture *texture, uint32_t width, uint32_t height,
                         std::vector<uint8_t> &pixels, std::string &error) const;

    // Fills `target` (of `format`, width by height) with `source` scaled to
    // cover it, bilinear, blurred or not and times `brightness`, in a pass
    // recorded into `commands` (present_sides.frag; Rogue Galaxy: added).
    bool drawSides(SDL_GPUCommandBuffer *commands, SDL_GPUTexture *target, SDL_GPUTextureFormat format,
                   uint32_t width, uint32_t height, SDL_GPUTexture *source, uint32_t sourceWidth,
                   uint32_t sourceHeight, bool blur, float brightness, std::string &error);

    // Work recorded ahead of the next submission (Rogue Galaxy: added). A
    // texture upload, a target's patches, a page import, an 8-bit expansion,
    // a reinterpretation or a clear was each a command buffer submission of
    // its own, about 150 a frame in the field against ten flushes of queued
    // draws, and the driver's share of the GS worker was near half. They go
    // into one command buffer: the next flush of queued draws records those
    // draws into it and submits it, so what was recorded first runs first,
    // as with immediate submissions; a readback or a present submits it
    // first. Only the GS worker thread records into it.
    // what (Rogue Galaxy: added): with gpuTimes(), the label the GPU time
    // of the work recorded next is logged under.
    SDL_GPUCommandBuffer *pendingCommands(std::string &error, const char *what = nullptr);
    bool hasPending() const { return m_pending != nullptr; }
    // Submits what is pending, if anything, with a fence `completedSerial`
    // reports on.
    bool submitPending(std::string &error);
    // Submits and waits: before reading back what it wrote.
    bool submitPendingAndWait(std::string &error);
    // Drops what is pending, after a failure while recording into it.
    void cancelPending();
    // The serial of the submission the pending work will go out in; each
    // submission's is one more than the last's.
    uint64_t pendingSerial() const { return m_nextSerial; }
    // The last serial whose submission the GPU has finished.
    uint64_t completedSerial();
    // Resources SDL cycles (a local copy's snapshot) stay allocated per use
    // until their command buffer completes: a caller about to cycle `bytes`
    // of them asks first, and the pending work goes out once the budget is
    // spent.
    bool reservePending(uint64_t bytes, std::string &error);
    uint64_t submissions() const { return m_submissions; }

    // One point-sampled clamp sampler. Filtering and GS wrap modes are done in
    // the fragment shader, which needs raw texel fetches, so the sampler's own
    // parameters never vary.
    SDL_GPUSampler *sampler() const { return m_sampler; }

    size_t pipelineCount() const { return m_pipelines.size() + (m_reinterpretPipeline ? 1u : 0u) + (m_displayPipeline ? 1u : 0u); }
    // Nanoseconds spent in SDL_CreateGPUGraphicsPipeline for draw pipelines:
    // each new state combination compiles one, a hitch the first time it is
    // drawn (Rogue Galaxy: added, for the stats line).
    uint64_t pipelineCreateNanos() const { return m_pipelineCreateNanos; }
    bool framebufferFetch() const { return m_framebufferFetch; }
    // Pipelines with GsPipelineKey::destinationCopy can be made (Rogue
    // Galaxy: added).
    bool destinationCopy() const { return m_destinationShader != nullptr; }
    // Whether pipelines with dualSource can be made (Rogue Galaxy: added).
    bool dualSourceBlend() const { return m_dualSourceBlend; }
    // DQ8_GFX_GPU_TIMES=1 (Rogue Galaxy: added): the backend labels its GPU
    // work, and the patched SDL logs the GPU's time by label ("[gputime]").
    bool gpuTimes() const { return m_gpuTimes; }

private:
    SDL_GPUDevice *m_device = nullptr;
    SDL_GPUShader *m_vertexShader = nullptr;
    SDL_GPUShader *m_nativeVertexShader = nullptr;
    SDL_GPUShader *m_fragmentShader = nullptr;
    SDL_GPUShader *m_destinationShader = nullptr;
    SDL_GPUShader *m_dualSourceShader = nullptr;
    bool m_dualSourceBlend = false;
    const bool m_gpuTimes = [] {
        const char *value = std::getenv("DQ8_GFX_GPU_TIMES");
        return value && *value && *value != '0';
    }();
    SDL_GPUSampler *m_sampler = nullptr;
    SDL_GPUTextureFormat m_depthFormat = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
    bool m_framebufferFetch = false;
    bool createIndex8(std::string &error);
    void releaseIndex8();

    SDL_GPUGraphicsPipeline *m_reinterpretPipeline = nullptr;
    SDL_GPUGraphicsPipeline *m_localCopyPipeline = nullptr;
    SDL_GPUGraphicsPipeline *m_displayPipeline = nullptr;
    // drawSides': its pipeline for the window's format, and a bilinear sampler.
    SDL_GPUGraphicsPipeline *m_sidesPipeline = nullptr;
    SDL_GPUTextureFormat m_sidesFormat = SDL_GPU_TEXTUREFORMAT_INVALID;
    SDL_GPUSampler *m_linearSampler = nullptr;
    SDL_GPUGraphicsPipeline *m_index8Pipeline = nullptr;
    SDL_GPUTexture *m_index8Palette = nullptr;
    SDL_GPUTexture *m_index8Placement = nullptr;
    SDL_GPUTransferBuffer *m_index8Upload = nullptr;
    // Kept from one local copy to the next, grown as needed and cycled by SDL
    // while the GPU still reads them: on Vulkan each new transfer buffer was
    // its own memory allocation, and a frame can do hundreds of copies.
    bool reserveLocalCopy(uint32_t snapshotWidth, uint32_t snapshotHeight, uint32_t mappingWidth,
                          uint32_t mappingHeight, std::string &error);
    void releaseLocalCopy();
    SDL_GPUTexture *m_localCopySnapshot = nullptr;
    SDL_GPUTexture *m_localCopyMapping = nullptr;
    SDL_GPUTransferBuffer *m_localCopyUpload = nullptr;
    uint32_t m_localCopySnapshotWidth = 0u;
    uint32_t m_localCopySnapshotHeight = 0u;
    uint32_t m_localCopyMappingWidth = 0u;
    uint32_t m_localCopyMappingHeight = 0u;
    SDL_GPUCommandBuffer *m_pending = nullptr;
    uint64_t m_pendingBytes = 0u;
    uint64_t m_nextSerial = 1u;
    uint64_t m_submissions = 0u;
    struct Submission {
        uint64_t serial;
        SDL_GPUFence *fence;
    };
    std::deque<Submission> m_inFlight;
    std::unordered_map<GsPipelineKey, SDL_GPUGraphicsPipeline *, GsPipelineKeyHash> m_pipelines;
    uint64_t m_pipelineCreateNanos = 0u;
};

// Upload transfer buffers for the pending command buffer (Rogue Galaxy:
// added). A buffer is taken, filled, recorded into the pending command
// buffer and marked used with that submission's serial; it is free again
// once the device reports the serial complete. Sizes are rounded up to a
// power of two from 64 KiB, so a few sizes serve every upload. One buffer
// cycled by SDL would instead keep a copy of its whole capacity for every
// upload recorded ahead of a submission.
class SdlGpuUploadPool {
public:
    SDL_GPUTransferBuffer *take(SdlGpuDevice &device, size_t bytes, std::string &error);
    void used(SDL_GPUTransferBuffer *buffer, uint64_t serial);
    void release(SdlGpuDevice &device);

private:
    struct Slot {
        SDL_GPUTransferBuffer *buffer = nullptr;
        uint64_t serial = 0u; // 0: free
        size_t bytes = 0u;
    };
    std::vector<Slot> m_slots;
};

} // namespace dq8::gfx
