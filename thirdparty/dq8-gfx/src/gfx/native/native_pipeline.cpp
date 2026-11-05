#include "gfx/native/native_pipeline.h"

#include "native_view.frag.spv.h"
#include "native_view.vert.spv.h"

#include <algorithm>
#include <filesystem>
#include <tuple>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <unordered_map>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <type_traits>

namespace dq8::gfx::native
{
namespace
{
std::atomic<bool (*)()> g_outer{nullptr};

struct Queue
{
    std::mutex mutex;
    std::vector<Record> records;
    std::vector<Id> evicted;
    std::function<std::string()> producerStats;
};
Queue &queue()
{
    static Queue instance;
    return instance;
}

// The scene target until a producer declares one (the game's frame size).
constexpr uint32_t kDefaultWidth = 640u;
constexpr uint32_t kDefaultHeight = 448u;
// The textures held, past which the oldest are let go (and their producers
// told, takeEvicted).
constexpr uint64_t kImageBudget = 256ull << 20;
// The debug view's grid.
constexpr uint32_t kViewColumns = 8u;
constexpr uint32_t kViewRows = 6u;

constexpr const char *kRecordNames[] = {"texture", "palette", "target",     "camera",   "mesh",      "sprite",
                                        "effect",  "frame",   "present",    "readback", "depth-query"};
static_assert(std::size(kRecordNames) == std::variant_size_v<Record>, "a name per record kind");

// Step 7b's depth reads: what was asked since the renderer last took them,
// and the last answer for each place in that order.
struct DepthMailbox
{
    std::mutex mutex;
    std::vector<DepthBlock> wanted;
    uint32_t asked = 0; // reads since the renderer last took them
    struct Answer
    {
        DepthBlock block;
        std::vector<float> values;
        bool valid = false;
    };
    Answer answers[kMaxDepthReads];
};
DepthMailbox &depthMailbox()
{
    static DepthMailbox instance;
    return instance;
}

std::vector<DepthBlock> takeDepthReads()
{
    DepthMailbox &m = depthMailbox();
    std::lock_guard lock(m.mutex);
    m.asked = 0u;
    std::vector<DepthBlock> out;
    out.swap(m.wanted);
    return out;
}

void publishDepth(uint32_t index, const DepthBlock &block, std::vector<float> &&values)
{
    if (index >= kMaxDepthReads)
        return;
    DepthMailbox &m = depthMailbox();
    std::lock_guard lock(m.mutex);
    m.answers[index].block = block;
    m.answers[index].values = std::move(values);
    m.answers[index].valid = true;
}
// Step 7c's colour reads, by id.
struct ColorMailbox
{
    std::mutex mutex;
    std::condition_variable done;
    uint32_t next = 1;
    struct Entry
    {
        ColorRead read;
        bool finished = false, ok = false;
        std::vector<uint8_t> rgba;
    };
    std::unordered_map<uint32_t, Entry> entries;
};
ColorMailbox &colorMailbox()
{
    static ColorMailbox instance;
    return instance;
}
} // namespace

uint32_t beginColorRead(const ColorRead &read)
{
    ColorMailbox &m = colorMailbox();
    std::lock_guard lock(m.mutex);
    uint32_t id = m.next++;
    if (id == 0u) // wrapped: 0 is none
        id = m.next++;
    m.entries[id].read = read;
    return id;
}

bool colorReadsWaiting()
{
    ColorMailbox &m = colorMailbox();
    std::lock_guard lock(m.mutex);
    for (const auto &entry : m.entries)
        if (!entry.second.finished)
            return true;
    return false;
}

bool waitColorRead(uint32_t id, std::vector<uint8_t> &rgba, uint32_t timeoutMs)
{
    ColorMailbox &m = colorMailbox();
    std::unique_lock lock(m.mutex);
    const bool finished = m.done.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] {
        const auto e = m.entries.find(id);
        return e == m.entries.end() || e->second.finished;
    });
    const auto e = m.entries.find(id);
    if (e == m.entries.end())
        return false;
    const bool ok = finished && e->second.finished && e->second.ok;
    if (ok)
        rgba = std::move(e->second.rgba);
    m.entries.erase(e); // a late answer finds nothing to fill
    return ok;
}

bool readDepth(const DepthBlock &block, std::vector<float> &out)
{
    if (!enabled() || block.target == 0u || block.width == 0u || block.height == 0u ||
        block.width > kMaxDepthSide || block.height > kMaxDepthSide)
        return false;
    DepthMailbox &m = depthMailbox();
    std::lock_guard lock(m.mutex);
    const uint32_t index = m.asked++;
    if (m.wanted.size() < kMaxDepthReads)
        m.wanted.push_back(block);
    if (index >= kMaxDepthReads)
        return false;
    const DepthMailbox::Answer &a = m.answers[index];
    if (!a.valid || a.block.target != block.target || a.block.width != block.width ||
        a.block.height != block.height)
        return false;
    out = a.values;
    return true;
}

void setOuterSwitch(bool (*outer)())
{
    g_outer.store(outer, std::memory_order_release);
}

bool enabled()
{
    static const bool on = [] {
        bool (*outer)() = g_outer.load(std::memory_order_acquire);
        const char *v = std::getenv("RG_NATIVE_PIPELINE");
        const bool asked = v && *v && *v != '0';
        const bool result = asked && outer && outer();
        if (asked && !result)
            std::fprintf(stderr, "[native-pipeline] RG_NATIVE_PIPELINE ignored: the native renderer is off\n");
        else if (result)
            std::fprintf(stderr, "[native-pipeline] on\n");
        return result;
    }();
    return on;
}

void push(Record &&record)
{
    if (!enabled())
        return;
    Queue &q = queue();
    std::lock_guard lock(q.mutex);
    q.records.push_back(std::move(record));
}

std::vector<Id> takeEvicted()
{
    Queue &q = queue();
    std::lock_guard lock(q.mutex);
    std::vector<Id> out;
    out.swap(q.evicted);
    return out;
}

void setProducerStats(std::function<std::string()> line)
{
    Queue &q = queue();
    std::lock_guard lock(q.mutex);
    q.producerStats = std::move(line);
}

void Renderer::count(const MeshDraw &draw)
{
    Readiness &r = m_readiness;
    ++r.meshDraws;
    if (draw.camera != 0u && draw.hasModel && m_cameras.count(draw.camera))
        ++r.cameraAndModel;
    if (draw.object != 0u)
        ++r.objectIdentity;
    if (const auto t = m_targets.find(draw.target);
        t != m_targets.end() && t->second.role == TargetRole::SceneColor && draw.floatDepth)
        ++r.sceneFloatDepth;
    if (draw.material.known)
        ++r.materialKnown;
    if (draw.hasNormals)
        ++r.normals;
}

SDL_GPUTexture *Renderer::createTexture(SDL_GPUDevice *device, SDL_GPUTextureFormat format, uint32_t width,
                                        uint32_t height, std::string &error)
{
    SDL_GPUTextureCreateInfo info{};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = format;
    info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    info.width = width;
    info.height = height;
    info.layer_count_or_depth = 1;
    info.num_levels = 1;
    SDL_GPUTexture *texture = SDL_CreateGPUTexture(device, &info);
    if (!texture)
        error = std::string("SDL_CreateGPUTexture(native texture): ") + SDL_GetError();
    return texture;
}

void Renderer::apply(SDL_GPUDevice *device, Record &record, std::string &error)
{
    ++m_records[record.index()];
    std::visit(
        [&](auto &r) {
            using T = std::decay_t<decltype(r)>;
            if constexpr (std::is_same_v<T, Target>)
            {
                m_targets[r.id] = r;
                if (r.role == TargetRole::SceneColor)
                    m_sceneTarget = r.id;
            }
            else if constexpr (std::is_same_v<T, Camera>)
                m_cameras[r.id] = r;
            else if constexpr (std::is_same_v<T, MeshDraw>)
            {
                count(r);
                // Any target (step 5a); one not declared is counted when drawn.
                m_meshes.push_back(std::move(r));
            }
            else if constexpr (std::is_same_v<T, Present>)
            {
                // The draws since the last one are a frame; a frame not yet
                // drawn when the next closes is dropped (the newest shows).
                m_frameMeshes.swap(m_meshes);
                m_meshes.clear();
                m_frameReady = true;
                ++m_meshCounts.frames;
            }
            else if constexpr (std::is_same_v<T, Texture>)
            {
                const uint32_t bpp = r.format == PixelFormat::Rgba8 ? 4u : r.format == PixelFormat::Index8 ? 1u : 0u;
                const uint64_t bytes = uint64_t(r.width) * r.height * bpp;
                if (r.id == 0u || bpp == 0u || bytes == 0u || !r.texels || r.texels->size() < bytes)
                {
                    ++m_refused;
                    return;
                }
                SDL_GPUTexture *texture = createTexture(
                    device, bpp == 4u ? SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM : SDL_GPU_TEXTUREFORMAT_R8_UNORM,
                    r.width, r.height, error);
                if (!texture)
                    return;
                if (const auto old = m_images.find(r.id); old != m_images.end())
                {
                    SDL_ReleaseGPUTexture(device, old->second.texture);
                    m_imageBytes -= old->second.bytes;
                    m_imageOrder.erase(std::find(m_imageOrder.begin(), m_imageOrder.end(), r.id));
                }
                m_images[r.id] = Image{texture, r.format, r.width, r.height, r.palette, bytes};
                m_imageOrder.push_back(r.id);
                m_imageBytes += bytes;
                m_uploads.push_back({texture, r.width, r.height, r.texels});
                m_recent.erase(std::remove(m_recent.begin(), m_recent.end(), r.id), m_recent.end());
                m_recent.push_front(r.id);
                if (m_recent.size() > kViewColumns * kViewRows)
                    m_recent.pop_back();
            }
            else if constexpr (std::is_same_v<T, Palette>)
            {
                if (r.id == 0u || (r.entries != 16u && r.entries != 256u) || !r.colors ||
                    r.colors->size() < r.entries * 4u)
                {
                    ++m_refused;
                    return;
                }
                SDL_GPUTexture *texture =
                    createTexture(device, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, r.entries, 1u, error);
                if (!texture)
                    return;
                if (const auto old = m_palettes.find(r.id); old != m_palettes.end())
                    SDL_ReleaseGPUTexture(device, old->second.texture);
                m_palettes[r.id] = Image{texture, PixelFormat::Rgba8, r.entries, 1u, 0u, r.entries * 4u};
                m_uploads.push_back({texture, r.entries, 1u, r.colors});
            }
            // The rest is drawn from step 4 on; counted above.
        },
        record);
}

bool Renderer::upload(SDL_GPUDevice *device, SDL_GPUCommandBuffer *commands, std::string &error)
{
    if (m_uploads.empty())
        return true;
    uint64_t total = 0u;
    for (const PendingUpload &u : m_uploads)
        total += u.bytes->size();
    SDL_GPUTransferBufferCreateInfo info{};
    info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    info.size = static_cast<uint32_t>(total);
    SDL_GPUTransferBuffer *buffer = SDL_CreateGPUTransferBuffer(device, &info);
    if (!buffer)
    {
        error = std::string("SDL_CreateGPUTransferBuffer(native textures): ") + SDL_GetError();
        return false;
    }
    auto *mapped = static_cast<uint8_t *>(SDL_MapGPUTransferBuffer(device, buffer, false));
    if (!mapped)
    {
        error = std::string("SDL_MapGPUTransferBuffer(native textures): ") + SDL_GetError();
        SDL_ReleaseGPUTransferBuffer(device, buffer);
        return false;
    }
    uint32_t offset = 0u;
    for (const PendingUpload &u : m_uploads)
    {
        std::memcpy(mapped + offset, u.bytes->data(), u.bytes->size());
        offset += static_cast<uint32_t>(u.bytes->size());
    }
    SDL_UnmapGPUTransferBuffer(device, buffer);
    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(commands);
    offset = 0u;
    for (const PendingUpload &u : m_uploads)
    {
        SDL_GPUTextureTransferInfo source{};
        source.transfer_buffer = buffer;
        source.offset = offset;
        source.pixels_per_row = u.width;
        source.rows_per_layer = u.height;
        SDL_GPUTextureRegion destination{};
        destination.texture = u.texture;
        destination.w = u.width;
        destination.h = u.height;
        destination.d = 1u;
        SDL_UploadToGPUTexture(copy, &source, &destination, false);
        offset += static_cast<uint32_t>(u.bytes->size());
    }
    SDL_EndGPUCopyPass(copy);
    // Freed once the GPU is done with it.
    SDL_ReleaseGPUTransferBuffer(device, buffer);
    m_uploads.clear();
    return true;
}

void Renderer::evict(SDL_GPUDevice *device)
{
    std::vector<Id> gone;
    while (m_imageBytes > kImageBudget && !m_imageOrder.empty())
    {
        const Id id = m_imageOrder.front();
        m_imageOrder.pop_front();
        const auto it = m_images.find(id);
        if (it == m_images.end())
            continue;
        SDL_ReleaseGPUTexture(device, it->second.texture);
        m_imageBytes -= it->second.bytes;
        m_images.erase(it);
        m_recent.erase(std::remove(m_recent.begin(), m_recent.end(), id), m_recent.end());
        gone.push_back(id);
        ++m_evicted;
    }
    if (gone.empty())
        return;
    Queue &q = queue();
    std::lock_guard lock(q.mutex);
    q.evicted.insert(q.evicted.end(), gone.begin(), gone.end());
}

bool Renderer::ensureView(SDL_GPUDevice *device, std::string &error)
{
    if (m_viewPipeline)
        return true;
    if (m_viewFailed)
        return false;
    m_viewFailed = true; // until it is made
    // SPIR-V only so far (no MSL build of these shaders): no view elsewhere.
    if (!(SDL_GetGPUShaderFormats(device) & SDL_GPU_SHADERFORMAT_SPIRV))
        return false;
    SDL_GPUSamplerCreateInfo samplerInfo{};
    samplerInfo.min_filter = SDL_GPU_FILTER_NEAREST;
    samplerInfo.mag_filter = SDL_GPU_FILTER_NEAREST;
    samplerInfo.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    samplerInfo.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    samplerInfo.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    samplerInfo.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    m_viewSampler = SDL_CreateGPUSampler(device, &samplerInfo);
    if (!m_viewSampler)
    {
        error = std::string("SDL_CreateGPUSampler(native view): ") + SDL_GetError();
        return false;
    }
    SDL_GPUShaderCreateInfo vertexInfo{};
    vertexInfo.code = reinterpret_cast<const uint8_t *>(kNativeViewVertSpirv);
    vertexInfo.code_size = sizeof(kNativeViewVertSpirv);
    vertexInfo.entrypoint = "main";
    vertexInfo.format = SDL_GPU_SHADERFORMAT_SPIRV;
    vertexInfo.stage = SDL_GPU_SHADERSTAGE_VERTEX;
    vertexInfo.num_uniform_buffers = 1u;
    SDL_GPUShaderCreateInfo fragmentInfo{};
    fragmentInfo.code = reinterpret_cast<const uint8_t *>(kNativeViewFragSpirv);
    fragmentInfo.code_size = sizeof(kNativeViewFragSpirv);
    fragmentInfo.entrypoint = "main";
    fragmentInfo.format = SDL_GPU_SHADERFORMAT_SPIRV;
    fragmentInfo.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
    fragmentInfo.num_samplers = 2u;
    fragmentInfo.num_uniform_buffers = 1u;
    SDL_GPUShader *vertex = SDL_CreateGPUShader(device, &vertexInfo);
    SDL_GPUShader *fragment = vertex ? SDL_CreateGPUShader(device, &fragmentInfo) : nullptr;
    if (!vertex || !fragment)
    {
        error = std::string("SDL_CreateGPUShader(native view): ") + SDL_GetError();
        if (vertex)
            SDL_ReleaseGPUShader(device, vertex);
        return false;
    }
    SDL_GPUColorTargetDescription colorTarget{};
    colorTarget.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    SDL_GPUGraphicsPipelineCreateInfo info{};
    info.vertex_shader = vertex;
    info.fragment_shader = fragment;
    info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLESTRIP;
    info.target_info.color_target_descriptions = &colorTarget;
    info.target_info.num_color_targets = 1u;
    m_viewPipeline = SDL_CreateGPUGraphicsPipeline(device, &info);
    SDL_ReleaseGPUShader(device, vertex);
    SDL_ReleaseGPUShader(device, fragment);
    if (!m_viewPipeline)
    {
        error = std::string("SDL_CreateGPUGraphicsPipeline(native view): ") + SDL_GetError();
        return false;
    }
    m_viewFailed = false;
    return true;
}

// The newest textures, newest first, each fitted into a cell of the grid.
void Renderer::drawView(SDL_GPUCommandBuffer *commands, SDL_GPURenderPass *pass)
{
    SDL_BindGPUGraphicsPipeline(pass, m_viewPipeline);
    const float cellW = 2.0f / kViewColumns, cellH = 2.0f / kViewRows;
    const float aspect = float(m_sceneWidth) / float(std::max(m_sceneHeight, 1u));
    uint32_t cell = 0u;
    for (const Id id : m_recent)
    {
        const auto it = m_images.find(id);
        if (it == m_images.end())
            continue;
        const Image &image = it->second;
        uint32_t mode[4] = {0u, 0u, 0u, 0u};
        SDL_GPUTexture *palette = image.texture;
        if (image.format == PixelFormat::Index8)
        {
            const auto p = m_palettes.find(image.palette);
            if (p != m_palettes.end())
            {
                mode[0] = 1u;
                mode[1] = p->second.width;
                palette = p->second.texture;
            }
            else
                mode[0] = 2u;
        }
        // The cell, less a margin, then the texture's shape inside it (in
        // clip space, whose x and y units differ by the scene's aspect).
        const float x0 = -1.0f + (cell % kViewColumns) * cellW, top = 1.0f - (cell / kViewColumns) * cellH;
        const float margin = 0.08f;
        float w = cellW * (1.0f - margin), h = cellH * (1.0f - margin);
        const float shape = float(image.width) / float(std::max(image.height, 1u)); // width over height
        if (shape * h / aspect > w)
            h = w * aspect / shape;
        else
            w = shape * h / aspect;
        const float cx = x0 + cellW * 0.5f, cy = top - cellH * 0.5f;
        const float rect[4] = {cx - w * 0.5f, cy + h * 0.5f, cx + w * 0.5f, cy - h * 0.5f};
        SDL_PushGPUVertexUniformData(commands, 0, rect, sizeof(rect));
        SDL_PushGPUFragmentUniformData(commands, 0, mode, sizeof(mode));
        const SDL_GPUTextureSamplerBinding bindings[2] = {{image.texture, m_viewSampler}, {palette, m_viewSampler}};
        SDL_BindGPUFragmentSamplers(pass, 0, bindings, 2);
        SDL_DrawGPUPrimitives(pass, 4, 1, 0, 0);
        if (++cell == kViewColumns * kViewRows)
            break;
    }
}

bool Renderer::render(SDL_GPUDevice *device, SDL_GPUCommandBuffer *commands, uint32_t scale, Frame &out,
                      std::string &error)
{
    {
        Queue &q = queue();
        std::lock_guard lock(q.mutex);
        m_drained.swap(q.records);
    }
    // A texture the GPU refuses is counted and left out, not the frame lost.
    static bool told = false;
    for (Record &record : m_drained)
    {
        std::string refused;
        apply(device, record, refused);
        if (!refused.empty())
        {
            ++m_refused;
            if (!told)
                std::fprintf(stderr, "[native-pipeline] refused: %s\n", refused.c_str());
            told = true;
        }
    }
    m_drained.clear();
    std::string meshError;
    const bool meshes = ensureMeshes(device, meshError);
    static bool toldMeshes = false;
    if (!meshes && !toldMeshes)
    {
        std::fprintf(stderr, "[native-pipeline] no mesh draws: %s\n", meshError.c_str());
        toldMeshes = true;
    }
    if (std::string failed; !upload(device, commands, failed))
    {
        std::fprintf(stderr, "[native-pipeline] uploads lost: %s\n", failed.c_str());
        m_uploads.clear();
    }
    evict(device);
    // The depth reads, from the last frame drawn (submitted ahead of this
    // one's draws).
    serveDepth(device);

    uint32_t width = kDefaultWidth, height = kDefaultHeight;
    if (const auto t = m_targets.find(m_sceneTarget); t != m_targets.end() && t->second.width && t->second.height)
    {
        width = t->second.width;
        height = t->second.height;
    }
    scale = scale ? scale : 1u;
    width *= scale;
    height *= scale;
    bool fresh = false; // the scene target holds nothing yet
    if (!m_scene || m_sceneWidth != width || m_sceneHeight != height)
    {
        fresh = true;
        if (m_scene)
            SDL_ReleaseGPUTexture(device, m_scene);
        SDL_GPUTextureCreateInfo info{};
        info.type = SDL_GPU_TEXTURETYPE_2D;
        info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
        info.width = width;
        info.height = height;
        info.layer_count_or_depth = 1;
        info.num_levels = 1;
        m_scene = SDL_CreateGPUTexture(device, &info);
        if (!m_scene)
        {
            error = std::string("SDL_CreateGPUTexture(native scene): ") + SDL_GetError();
            return false;
        }
        m_sceneWidth = width;
        m_sceneHeight = height;
    }

    // The scene: the meshes of the last frame a Present record closed, over
    // black, with a reversed depth target of its own. Kept as it is until
    // the next frame closes (the host presents more often than the game).
    static const bool grid = [] {
        const char *v = std::getenv("RG_NATIVE_TEXTURE_GRID");
        return v && *v && *v != '0';
    }();
    const bool draw = meshes && m_frameReady;
    if (!draw && !fresh && !grid)
    {
        out.texture = m_scene;
        out.width = m_sceneWidth;
        out.height = m_sceneHeight;
        return true;
    }
    bool drawn = false;
    if (draw)
    {
        std::string failed;
        drawn = drawFrame(device, commands, scale, failed);
        if (drawn)
            m_frameScale = scale;
        if (!drawn)
            std::fprintf(stderr, "[native-pipeline] frame lost: %s\n", failed.c_str());
    }
    m_frameReady = false;
    std::string viewError;
    const bool view = grid && ensureView(device, viewError);
    if (!viewError.empty())
        std::fprintf(stderr, "[native-pipeline] no texture view: %s\n", viewError.c_str());
    if (!drawn || view)
    {
        // A new scene target with no frame for it yet is cleared; the grid
        // goes over whatever the scene holds.
        SDL_GPUColorTargetInfo target{};
        target.texture = m_scene;
        target.load_op = drawn ? SDL_GPU_LOADOP_LOAD : SDL_GPU_LOADOP_CLEAR;
        target.store_op = SDL_GPU_STOREOP_STORE;
        target.clear_color = SDL_FColor{0.0f, 0.0f, 0.0f, 1.0f};
        SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(commands, &target, 1, nullptr);
        if (!pass)
        {
            error = std::string("SDL_BeginGPURenderPass(native scene): ") + SDL_GetError();
            return false;
        }
        if (view)
            drawView(commands, pass);
        SDL_EndGPURenderPass(pass);
    }

    out.texture = m_scene;
    out.width = m_sceneWidth;
    out.height = m_sceneHeight;
    return true;
}

std::string Renderer::statsLine()
{
    std::string line = "[native-pipeline] records:";
    for (size_t i = 0; i < std::size(kRecordNames); ++i)
    {
        line += std::string(" ") + kRecordNames[i] + " " + std::to_string(m_records[i] - m_reported[i]);
        m_reported[i] = m_records[i];
    }
    const auto d = [](uint64_t a, uint64_t b) { return std::to_string(a - b); };
    line += " | held: textures " + std::to_string(m_images.size()) + " (" + std::to_string(m_imageBytes >> 20) +
            " MiB), palettes " + std::to_string(m_palettes.size()) + ", evicted " + d(m_evicted, m_evictedReported) +
            ", refused " + d(m_refused, m_refusedReported);
    m_evictedReported = m_evicted;
    m_refusedReported = m_refused;
    const Readiness &now = m_readiness, &was = m_readinessReported;
    line += " | readiness of " + d(now.meshDraws, was.meshDraws) + " mesh draws: R1 camera+model " +
            d(now.cameraAndModel, was.cameraAndModel) + ", R2 object " + d(now.objectIdentity, was.objectIdentity) +
            ", R3 scene float depth " + d(now.sceneFloatDepth, was.sceneFloatDepth) + ", R4 material " +
            d(now.materialKnown, was.materialKnown) + ", R5 normals " + d(now.normals, was.normals);
    const MeshCounts &mc = m_meshCounts, &mr = m_meshReported;
    line += " | meshes drawn " + d(mc.drawn, mr.drawn) + " (offscreen " + d(mc.offscreen, mr.offscreen) + ") over " +
            d(mc.frames, mr.frames) + " frames in " + d(mc.passes, mr.passes) +
            " passes; not drawn: target not declared " + d(mc.noTarget, mr.noTarget) + ", a depth buffer as color " +
            d(mc.depthAsColor, mr.depthAsColor) + ", no camera " + d(mc.noCamera, mr.noCamera) +
            ", texture not held " + d(mc.noTexture, mr.noTexture) + ", no pipeline " +
            d(mc.noPipeline, mr.noPipeline) + ", source target never drawn " +
            d(mc.sourceNotDrawn, mr.sourceNotDrawn) + ", reading its own target uncopied " + d(mc.feedback, mr.feedback) + ", copies for those that do " +
            d(mc.copies, mr.copies) + " (destination alpha tests " + d(mc.destinationAlpha, mr.destinationAlpha) + ")" +
            "; window space " + d(mc.window, mr.window) + ", from a target " + d(mc.fromTarget, mr.fromTarget) +
            "; a second depth target in a pass " +
            d(mc.depthMismatch, mr.depthMismatch) + "\n";
    const DepthCounts &dc = m_depthCounts, &dr = m_depthReported;
    if (dc.asked != dr.asked)
        line += "  [native-pipeline] depth reads: asked " + d(dc.asked, dr.asked) + ", copied " + d(dc.copied, dr.copied) +
                ", published " + d(dc.published, dr.published) + ", no such depth target (the clear's) " +
                d(dc.noTarget, dr.noTarget) + ", left for a copy in flight " + d(dc.waiting, dr.waiting) +
                ", unserved " + d(dc.unserved, dr.unserved) + "\n";
    m_depthReported = m_depthCounts;
    if (m_colorReads != m_colorReadsReported || m_colorReadsFailed != m_colorFailedReported)
        line += "  [native-pipeline] colour reads: served " + d(m_colorReads, m_colorReadsReported) + ", failed " +
                d(m_colorReadsFailed, m_colorFailedReported) + "\n";
    m_colorReadsReported = m_colorReads;
    m_colorFailedReported = m_colorReadsFailed;
    m_meshReported = m_meshCounts;
    m_readinessReported = m_readiness;
    std::function<std::string()> producer;
    {
        Queue &q = queue();
        std::lock_guard lock(q.mutex);
        producer = q.producerStats;
    }
    if (producer)
        line += "  " + producer() + "\n";
    return line;
}

void Renderer::serveDepth(SDL_GPUDevice *device)
{
    // The copy the GPU has finished: each block brought to the game's
    // pixels (the nearest of the pixels each covers) and published.
    if (m_depthFence)
    {
        if (!SDL_QueryGPUFence(device, m_depthFence))
        {
            m_depthCounts.waiting += takeDepthReads().size(); // asked again next frame
            return;
        }
        SDL_ReleaseGPUFence(device, m_depthFence);
        m_depthFence = nullptr;
        const auto *mapped = static_cast<const float *>(SDL_MapGPUTransferBuffer(device, m_depthDownload, false));
        if (mapped)
        {
            const uint32_t s = m_depthCopyScale;
            size_t offset = 0u;
            for (const auto &[index, block] : m_depthCopied)
            {
                const uint32_t w = block.width * s, h = block.height * s;
                std::vector<float> values(size_t(block.width) * block.height, 0.0f);
                for (uint32_t y = 0; y < h; ++y)
                    for (uint32_t x = 0; x < w; ++x)
                    {
                        float &v = values[size_t(y / s) * block.width + x / s];
                        v = std::max(v, mapped[offset + size_t(y) * w + x]);
                    }
                offset += size_t(w) * h;
                publishDepth(index, block, std::move(values));
                ++m_depthCounts.published;
            }
            SDL_UnmapGPUTransferBuffer(device, m_depthDownload);
        }
        m_depthCopied.clear();
    }

    const std::vector<DepthBlock> wanted = takeDepthReads();
    if (wanted.empty())
        return;
    m_depthCounts.asked += wanted.size();
    // Only a float depth is copied out as it is.
    if (m_depthFailed || m_depthFormat != SDL_GPU_TEXTUREFORMAT_D32_FLOAT || m_frameScale == 0u)
    {
        m_depthCounts.unserved += wanted.size();
        return;
    }
    const uint32_t s = m_frameScale;
    std::vector<std::pair<uint32_t, DepthBlock>> copies;
    size_t floats = 0u;
    for (uint32_t index = 0; index < wanted.size(); ++index)
    {
        const DepthBlock &b = wanted[index];
        const auto t = m_gpuTargets.find(b.target);
        if (t == m_gpuTargets.end() || !t->second.texture || b.x < 0 || b.y < 0 ||
            (uint32_t(b.x) + b.width) * s > t->second.width || (uint32_t(b.y) + b.height) * s > t->second.height)
        {
            // Never drawn here: the clear's depth, as the GS's memory held.
            publishDepth(index, b, std::vector<float>(size_t(b.width) * b.height, 0.0f));
            ++m_depthCounts.noTarget;
            continue;
        }
        copies.emplace_back(index, b);
        floats += size_t(b.width) * s * b.height * s;
    }
    if (copies.empty())
        return;
    const auto fail = [&](const char *what) {
        std::fprintf(stderr, "[native-pipeline] depth reads off: %s: %s\n", what, SDL_GetError());
        m_depthFailed = true;
        m_depthCounts.unserved += copies.size();
    };
    const uint32_t bytes = uint32_t(floats * sizeof(float));
    if (m_depthDownloadBytes < bytes)
    {
        if (m_depthDownload)
            SDL_ReleaseGPUTransferBuffer(device, m_depthDownload);
        SDL_GPUTransferBufferCreateInfo info{};
        info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
        info.size = bytes;
        m_depthDownload = SDL_CreateGPUTransferBuffer(device, &info);
        m_depthDownloadBytes = m_depthDownload ? bytes : 0u;
        if (!m_depthDownload)
            return fail("SDL_CreateGPUTransferBuffer");
    }
    SDL_GPUCommandBuffer *commands = SDL_AcquireGPUCommandBuffer(device);
    if (!commands)
        return fail("SDL_AcquireGPUCommandBuffer");
    SDL_GPUCopyPass *pass = SDL_BeginGPUCopyPass(commands);
    if (!pass)
    {
        SDL_CancelGPUCommandBuffer(commands);
        return fail("SDL_BeginGPUCopyPass");
    }
    uint32_t offset = 0u;
    for (const auto &[index, b] : copies)
    {
        SDL_GPUTextureRegion region{};
        region.texture = m_gpuTargets[b.target].texture;
        region.x = uint32_t(b.x) * s;
        region.y = uint32_t(b.y) * s;
        region.w = b.width * s;
        region.h = b.height * s;
        region.d = 1u;
        SDL_GPUTextureTransferInfo destination{};
        destination.transfer_buffer = m_depthDownload;
        destination.offset = offset;
        destination.pixels_per_row = region.w;
        destination.rows_per_layer = region.h;
        SDL_DownloadFromGPUTexture(pass, &region, &destination);
        offset += region.w * region.h * uint32_t(sizeof(float));
    }
    SDL_EndGPUCopyPass(pass);
    m_depthFence = SDL_SubmitGPUCommandBufferAndAcquireFence(commands);
    if (!m_depthFence)
        return fail("SDL_SubmitGPUCommandBufferAndAcquireFence");
    m_depthCopied = std::move(copies);
    m_depthCopyScale = s;
    m_depthCounts.copied += m_depthCopied.size();
}

void Renderer::dumpTargets(SDL_GPUDevice *device)
{
    m_dumpTargets = false;
    std::vector<std::tuple<Id, SDL_GPUTexture *, uint32_t, uint32_t>> all;
    all.emplace_back(m_sceneTarget, m_scene, m_sceneWidth, m_sceneHeight);
    for (const auto &[id, t] : m_gpuTargets)
    {
        const auto declared = m_targets.find(id);
        if (!t.texture || declared == m_targets.end() || declared->second.format == PixelFormat::Depth32 ||
            id == m_sceneTarget)
            continue;
        all.emplace_back(id, t.texture, t.width, t.height);
    }
    std::error_code ec;
    std::filesystem::create_directories("build/run/targets", ec);
    for (const auto &[id, texture, w, h] : all)
    {
        if (!texture || w == 0u || h == 0u)
            continue;
        const uint32_t bytes = w * h * 4u;
        SDL_GPUTransferBufferCreateInfo info{};
        info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
        info.size = bytes;
        SDL_GPUTransferBuffer *download = SDL_CreateGPUTransferBuffer(device, &info);
        SDL_GPUCommandBuffer *commands = download ? SDL_AcquireGPUCommandBuffer(device) : nullptr;
        SDL_GPUCopyPass *pass = commands ? SDL_BeginGPUCopyPass(commands) : nullptr;
        SDL_GPUFence *fence = nullptr;
        if (pass)
        {
            SDL_GPUTextureRegion region{};
            region.texture = texture;
            region.w = w;
            region.h = h;
            region.d = 1u;
            SDL_GPUTextureTransferInfo destination{};
            destination.transfer_buffer = download;
            destination.pixels_per_row = w;
            destination.rows_per_layer = h;
            SDL_DownloadFromGPUTexture(pass, &region, &destination);
            SDL_EndGPUCopyPass(pass);
            fence = SDL_SubmitGPUCommandBufferAndAcquireFence(commands);
        }
        else if (commands)
            SDL_CancelGPUCommandBuffer(commands);
        if (fence)
        {
            SDL_WaitForGPUFences(device, true, &fence, 1u);
            SDL_ReleaseGPUFence(device, fence);
            if (const auto *p = static_cast<const uint8_t *>(SDL_MapGPUTransferBuffer(device, download, false)))
            {
                char name[96];
                std::snprintf(name, sizeof(name), "build/run/targets/target%u_%ux%u.ppm", id, w, h);
                if (FILE *f = std::fopen(name, "wb"))
                {
                    std::fprintf(f, "P6\n%u %u\n255\n", w * 2u, h);
                    std::vector<uint8_t> row(size_t(w) * 6u);
                    for (uint32_t y = 0; y < h; ++y)
                    {
                        for (uint32_t x = 0; x < w; ++x)
                        {
                            const uint8_t *px = p + (size_t(y) * w + x) * 4u;
                            std::memcpy(row.data() + x * 3u, px, 3u);
                            row[(w + x) * 3u] = row[(w + x) * 3u + 1u] = row[(w + x) * 3u + 2u] = px[3];
                        }
                        std::fwrite(row.data(), 1, row.size(), f);
                    }
                    std::fclose(f);
                    std::fprintf(stderr, "[target-dump] %s\n", name);
                }
                SDL_UnmapGPUTransferBuffer(device, download);
            }
        }
        if (download)
            SDL_ReleaseGPUTransferBuffer(device, download);
    }
}

void Renderer::serveColorReads(SDL_GPUDevice *device)
{
    if (m_dumpTargets)
        dumpTargets(device);
    std::vector<std::pair<uint32_t, ColorRead>> waiting;
    ColorMailbox &m = colorMailbox();
    {
        std::lock_guard lock(m.mutex);
        for (const auto &[id, e] : m.entries)
            if (!e.finished)
                waiting.emplace_back(id, e.read);
    }
    if (waiting.empty())
        return;
    const auto finish = [&](uint32_t id, std::vector<uint8_t> &&rgba, bool ok) {
        ++(ok ? m_colorReads : m_colorReadsFailed);
        std::lock_guard lock(m.mutex);
        const auto e = m.entries.find(id);
        if (e == m.entries.end())
            return;
        e->second.finished = true;
        e->second.ok = ok;
        e->second.rgba = std::move(rgba);
    };
    const uint32_t s = std::max<uint32_t>(m_frameScale, 1u);
    for (const auto &[id, read] : waiting)
    {
        // The scene, or an offscreen target, as the last frame left it.
        SDL_GPUTexture *texture = nullptr;
        uint32_t texW = 0, texH = 0;
        if (read.target == m_sceneTarget || read.target == 0u)
        {
            texture = m_scene;
            texW = m_sceneWidth;
            texH = m_sceneHeight;
        }
        else if (const auto t = m_gpuTargets.find(read.target); t != m_gpuTargets.end())
        {
            texture = t->second.texture;
            texW = t->second.width;
            texH = t->second.height;
        }
        const Rect &r = read.rect;
        const uint32_t w = uint32_t(std::max(r.x1 - r.x0, 0)), h = uint32_t(std::max(r.y1 - r.y0, 0));
        if (!texture || m_frameScale == 0u || w == 0u || h == 0u || r.x0 < 0 || r.y0 < 0 ||
            uint32_t(r.x1) * s > texW || uint32_t(r.y1) * s > texH)
        {
            finish(id, {}, false);
            continue;
        }
        const uint32_t hostW = w * s, hostH = h * s, bytes = hostW * hostH * 4u;
        SDL_GPUTransferBufferCreateInfo info{};
        info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
        info.size = bytes;
        SDL_GPUTransferBuffer *download = SDL_CreateGPUTransferBuffer(device, &info);
        SDL_GPUCommandBuffer *commands = download ? SDL_AcquireGPUCommandBuffer(device) : nullptr;
        SDL_GPUCopyPass *pass = commands ? SDL_BeginGPUCopyPass(commands) : nullptr;
        SDL_GPUFence *fence = nullptr;
        if (pass)
        {
            SDL_GPUTextureRegion region{};
            region.texture = texture;
            region.x = uint32_t(r.x0) * s;
            region.y = uint32_t(r.y0) * s;
            region.w = hostW;
            region.h = hostH;
            region.d = 1u;
            SDL_GPUTextureTransferInfo destination{};
            destination.transfer_buffer = download;
            destination.pixels_per_row = hostW;
            destination.rows_per_layer = hostH;
            SDL_DownloadFromGPUTexture(pass, &region, &destination);
            SDL_EndGPUCopyPass(pass);
            fence = SDL_SubmitGPUCommandBufferAndAcquireFence(commands);
        }
        else if (commands)
            SDL_CancelGPUCommandBuffer(commands);
        std::vector<uint8_t> rgba;
        bool ok = false;
        if (fence)
        {
            SDL_WaitForGPUFences(device, true, &fence, 1u);
            SDL_ReleaseGPUFence(device, fence);
            if (const auto *p = static_cast<const uint8_t *>(SDL_MapGPUTransferBuffer(device, download, false)))
            {
                // Each game pixel the mean of the s x s it covers; alpha
                // from the stored A / 0x80 back to the game's.
                rgba.resize(size_t(w) * h * 4u);
                for (uint32_t y = 0; y < h; ++y)
                    for (uint32_t x = 0; x < w; ++x)
                        for (uint32_t c = 0; c < 4u; ++c)
                        {
                            uint32_t sum = 0;
                            for (uint32_t dy = 0; dy < s; ++dy)
                                for (uint32_t dx = 0; dx < s; ++dx)
                                    sum += p[(size_t(y * s + dy) * hostW + x * s + dx) * 4u + c];
                            float v = float(sum) / float(s * s);
                            if (c == 3u)
                                v = v * 128.0f / 255.0f;
                            rgba[(size_t(y) * w + x) * 4u + c] = uint8_t(std::min(v + 0.5f, 255.0f));
                        }
                SDL_UnmapGPUTransferBuffer(device, download);
                ok = true;
            }
        }
        if (download)
            SDL_ReleaseGPUTransferBuffer(device, download);
        if (!ok)
            std::fprintf(stderr, "[native-pipeline] colour read %u failed: %s\n", id, SDL_GetError());
        finish(id, std::move(rgba), ok);
    }
    m.done.notify_all();
}

void Renderer::releaseDepth(SDL_GPUDevice *device)
{
    if (m_depthFence)
    {
        SDL_WaitForGPUFences(device, true, &m_depthFence, 1u);
        SDL_ReleaseGPUFence(device, m_depthFence);
    }
    if (m_depthDownload)
        SDL_ReleaseGPUTransferBuffer(device, m_depthDownload);
    m_depthFence = nullptr;
    m_depthDownload = nullptr;
    m_depthDownloadBytes = 0u;
    m_depthCopied.clear();
}

void Renderer::release(SDL_GPUDevice *device)
{
    if (!device)
        return;
    releaseMeshes(device);
    releaseDepth(device);
    m_meshes.clear();
    m_frameMeshes.clear();
    if (m_scene)
        SDL_ReleaseGPUTexture(device, m_scene);
    for (auto &[id, image] : m_images)
        SDL_ReleaseGPUTexture(device, image.texture);
    for (auto &[id, image] : m_palettes)
        SDL_ReleaseGPUTexture(device, image.texture);
    if (m_viewPipeline)
        SDL_ReleaseGPUGraphicsPipeline(device, m_viewPipeline);
    if (m_viewSampler)
        SDL_ReleaseGPUSampler(device, m_viewSampler);
    m_images.clear();
    m_palettes.clear();
    m_imageOrder.clear();
    m_recent.clear();
    m_imageBytes = 0u;
    m_scene = nullptr;
    m_viewPipeline = nullptr;
    m_viewSampler = nullptr;
    m_sceneWidth = m_sceneHeight = 0;
}
} // namespace dq8::gfx::native
