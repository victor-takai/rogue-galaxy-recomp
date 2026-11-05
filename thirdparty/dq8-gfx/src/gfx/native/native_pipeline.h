#pragma once

// The native pipeline (Rogue Galaxy: added; the game's docs/NATIVE-PIPELINE.md).
//
// Producers push records (native_records.h) from any thread, in the order
// the game built them; the renderer drains them on the thread that owns the
// GPU device. So far (step 3a of the plan) the renderer keeps the targets,
// cameras, textures and palettes it is sent, counts how ready the draws are
// for the features (R1-R5), and presents its scene target with the mesh
// draws of the last frame a Present record closed (step 4b). Nothing of the
// GS here.

#include "gfx/native/native_records.h"

#include <SDL3/SDL_gpu.h>

#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace dq8::gfx::native
{
// The switch: RG_NATIVE_PIPELINE=1, and only under the game's native
// renderer flag, which the game provides as `outer` (it is decided once,
// after the game's settings are read). Decided at the first enabled() call
// after setOuterSwitch; false until a query is set.
void setOuterSwitch(bool (*outer)());
bool enabled();

// Any thread. Dropped when the pipeline is off.
void push(Record &&record);

// Textures the renderer let go of (its memory budget): a producer that sends
// a texture once and names it by id afterwards drains these and sends them
// again when next used. Any thread.
std::vector<Id> takeEvicted();

// Step 7b: depth reads (the game's lens flare tests the sun against the
// scene's depth), answered a frame or two late, without waiting for the GPU.
// The caller asks and takes the last answer for a read in its place: reads
// are told apart by their order since the renderer last took them, and an
// answer is used only for a block of the same target and size. The
// renderer, at each frame it draws, publishes the blocks it copied out of
// its depth for the reads before, and copies out what was asked since.
struct DepthBlock
{
    Id target = 0; // a depth target
    int32_t x = 0, y = 0;
    uint32_t width = 0, height = 0; // in the game's pixels
};
constexpr uint32_t kMaxDepthReads = 4u; // a frame's
constexpr uint32_t kMaxDepthSide = 64u;
// Any thread: the block's depth, reversed (1 nearest, 0 the clear), one
// value a game pixel (the nearest of the pixels it covers), rows in order.
// False when there is no answer for it yet.
bool readDepth(const DepthBlock &block, std::vector<float> &out);

// Step 7c: the game's colour reads (the fade's and the menu's frame
// captures), from what the renderer drew last, each game pixel the mean of
// the pixels it covers. The caller opens a read and waits; the renderer's
// owner's presenting thread, which runs beside the game's (step 7d), sees
// colorReadsWaiting and calls Renderer::serveColorReads.
struct ColorRead
{
    Id target = 0;
    Rect rect; // in the target's game pixels
};
uint32_t beginColorRead(const ColorRead &read);
// Any thread: a read was opened and not answered yet.
bool colorReadsWaiting();
// RGBA8, rows in order, alpha as the game's (0x80 is 1.0). False when the
// renderer could not serve it, or did not within `timeoutMs`.
bool waitColorRead(uint32_t id, std::vector<uint8_t> &rgba, uint32_t timeoutMs);

// A line of the producers' own counts, printed with the renderer's.
void setProducerStats(std::function<std::string()> line);

// The readiness counters, docs/NATIVE-PIPELINE.md's R1-R5, over the mesh
// draws drained.
struct Readiness
{
    uint64_t meshDraws = 0;
    uint64_t cameraAndModel = 0; // R1
    uint64_t objectIdentity = 0; // R2
    uint64_t sceneFloatDepth = 0; // R3
    uint64_t materialKnown = 0;  // R4
    uint64_t normals = 0;        // R5
};

class Renderer
{
public:
    // Step 7c: answers every colour read waiting, from the targets as the
    // last frame drawn left them; waits for the GPU. Anything the owner has
    // pending must be submitted first.
    void serveColorReads(SDL_GPUDevice *device);
    // The triggered dump's colour targets, as PPM in build/run/targets
    // (colour, and alpha beside it), the frame after drawFrame asked.
    void dumpTargets(SDL_GPUDevice *device);
    bool dumpPending() const { return m_dumpTargets; }

    // What the presenter shows: the scene target, recorded into `commands`.
    // False with `error` set when the GPU refused.
    struct Frame
    {
        SDL_GPUTexture *texture = nullptr;
        uint32_t width = 0;
        uint32_t height = 0;
    };
    bool render(SDL_GPUDevice *device, SDL_GPUCommandBuffer *commands, uint32_t scale, Frame &out,
                std::string &error);

    // The window since the last call: records by kind, textures held, and R1-R5.
    std::string statsLine();

    void release(SDL_GPUDevice *device);

private:
    struct Image
    {
        SDL_GPUTexture *texture = nullptr;
        PixelFormat format = PixelFormat::Rgba8;
        uint32_t width = 0;
        uint32_t height = 0;
        Id palette = 0;
        uint64_t bytes = 0;
    };
    struct PendingUpload
    {
        SDL_GPUTexture *texture = nullptr;
        uint32_t width = 0;
        uint32_t height = 0;
        Bytes bytes;
    };

    void apply(SDL_GPUDevice *device, Record &record, std::string &error);
    void count(const MeshDraw &draw);
    SDL_GPUTexture *createTexture(SDL_GPUDevice *device, SDL_GPUTextureFormat format, uint32_t width,
                                  uint32_t height, std::string &error);
    bool upload(SDL_GPUDevice *device, SDL_GPUCommandBuffer *commands, std::string &error);
    bool ensureView(SDL_GPUDevice *device, std::string &error);
    void drawView(SDL_GPUCommandBuffer *commands, SDL_GPURenderPass *pass);
    void evict(SDL_GPUDevice *device);

    // Step 4b's mesh draws (native_meshes.cpp): the frame's draws into the
    // scene target, with a depth target of its own.
    bool ensureMeshes(SDL_GPUDevice *device, std::string &error);
    SDL_GPUGraphicsPipeline *meshPipeline(SDL_GPUDevice *device, uint32_t key);
    SDL_GPUSampler *meshSampler(SDL_GPUDevice *device, uint32_t key);
    bool uploadMeshes(SDL_GPUDevice *device, SDL_GPUCommandBuffer *commands, std::string &error);
    // The frame's draws in order, a render pass per run of draws into one
    // target (step 5a).
    bool drawFrame(SDL_GPUDevice *device, SDL_GPUCommandBuffer *commands, uint32_t scale, std::string &error);
    void drawMeshes(SDL_GPUDevice *device, SDL_GPUCommandBuffer *commands, SDL_GPURenderPass *pass, size_t first,
                    size_t last, Id target, bool depthAttached, float targetW, float targetH, uint32_t scale,
                    SDL_GPUTexture *copy, uint32_t copyW, uint32_t copyH);
    // m_gpuTargets' key for the copy a draw reading its own target reads.
    static constexpr Id kFeedbackCopy = 0xFFFFFFFFu;
    // A target's GPU texture (color, or depth), at least `minW` by `minH`
    // pixels: kept across frames, grown (and so cleared) when too small.
    SDL_GPUTexture *targetTexture(SDL_GPUDevice *device, Id id, bool depth, uint32_t minW, uint32_t minH,
                                  bool &fresh, std::string &error);
    void releaseMeshes(SDL_GPUDevice *device);
    // Step 7b: publishes the depth blocks the GPU copied out, and copies out
    // the ones asked since (from the last frame drawn, before this one's
    // draws), in a command buffer of its own.
    void serveDepth(SDL_GPUDevice *device);
    void releaseDepth(SDL_GPUDevice *device);
    struct MeshCounts
    {
        uint64_t drawn = 0, offscreen = 0, noTarget = 0, depthAsColor = 0, depthMismatch = 0, passes = 0;
        uint64_t noCamera = 0, noTexture = 0, noPipeline = 0, frames = 0;
        uint64_t window = 0, fromTarget = 0, sourceNotDrawn = 0, feedback = 0, copies = 0, destinationAlpha = 0;
    };
    struct GpuTarget
    {
        SDL_GPUTexture *texture = nullptr;
        uint32_t width = 0, height = 0; // allocated, in host pixels
        uint32_t scale = 0;
    };

    std::vector<Record> m_drained;
    std::unordered_map<Id, Target> m_targets;
    std::unordered_map<Id, Camera> m_cameras;
    std::unordered_map<Id, Image> m_images;   // textures
    std::unordered_map<Id, Image> m_palettes; // as 16x1 or 256x1 RGBA
    std::deque<Id> m_imageOrder;              // oldest first, for the budget
    uint64_t m_imageBytes = 0;
    std::vector<PendingUpload> m_uploads;
    std::deque<Id> m_recent; // the newest textures, for the view
    Id m_sceneTarget = 0;
    uint64_t m_records[std::variant_size_v<Record>] = {};
    uint64_t m_reported[std::variant_size_v<Record>] = {};
    uint64_t m_evicted = 0, m_evictedReported = 0;
    uint64_t m_refused = 0, m_refusedReported = 0;
    Readiness m_readiness;
    Readiness m_readinessReported;
    SDL_GPUTexture *m_scene = nullptr;
    uint32_t m_sceneWidth = 0;
    uint32_t m_sceneHeight = 0;
    SDL_GPUGraphicsPipeline *m_viewPipeline = nullptr;
    SDL_GPUSampler *m_viewSampler = nullptr;
    bool m_viewFailed = false;
    bool m_textureGrid = false; // RG_NATIVE_TEXTURE_GRID=1: step 3a's view over the scene

    std::vector<MeshDraw> m_meshes;      // drawn since the last Present record, any target
    std::vector<MeshDraw> m_frameMeshes; // the frame a Present record closed
    bool m_frameReady = false;
    bool m_dumpTargets = false; // the triggered dump: write the targets out
    SDL_GPUShader *m_meshVertex = nullptr;
    SDL_GPUShader *m_meshFragment = nullptr;
    bool m_meshFailed = false;
    SDL_GPUTextureFormat m_depthFormat = SDL_GPU_TEXTUREFORMAT_INVALID;
    std::unordered_map<Id, GpuTarget> m_gpuTargets; // offscreen colors, and every depth (by its id)
    SDL_GPUTexture *m_white = nullptr;
    std::unordered_map<uint32_t, SDL_GPUGraphicsPipeline *> m_meshPipelines;
    SDL_GPUSampler *m_meshSamplers[8] = {};
    SDL_GPUBuffer *m_vertexBuffer = nullptr;
    SDL_GPUBuffer *m_indexBuffer = nullptr;
    uint32_t m_vertexCapacity = 0, m_indexCapacity = 0;
    SDL_GPUTransferBuffer *m_meshTransfer = nullptr;
    uint32_t m_transferCapacity = 0;
    std::vector<uint32_t> m_firstIndex; // per frame mesh, into the index buffer
    std::vector<int32_t> m_vertexOffset;
    MeshCounts m_meshCounts, m_meshReported;
    // Step 7b's depth reads in flight (one copy at a time).
    SDL_GPUTransferBuffer *m_depthDownload = nullptr;
    uint32_t m_depthDownloadBytes = 0;
    SDL_GPUFence *m_depthFence = nullptr;
    std::vector<std::pair<uint32_t, DepthBlock>> m_depthCopied; // the read's index, its block
    uint32_t m_depthCopyScale = 1;
    uint32_t m_frameScale = 0; // the scale the last frame was drawn at; 0 none yet
    bool m_depthFailed = false;
    struct DepthCounts
    {
        uint64_t asked = 0, copied = 0, published = 0, noTarget = 0, waiting = 0, unserved = 0;
    };
    DepthCounts m_depthCounts, m_depthReported;
    uint64_t m_colorReads = 0, m_colorReadsFailed = 0, m_colorReadsReported = 0, m_colorFailedReported = 0;
};
} // namespace dq8::gfx::native
