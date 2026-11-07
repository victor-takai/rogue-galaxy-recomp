// The native pipeline's mesh draws (Rogue Galaxy: added; the game's
// docs/NATIVE-PIPELINE.md steps 4b and 5a): the frame's MeshDraw records,
// in order, into the targets they name. Depth targets are float, reversed
// (cleared to 0 at their first use in a frame, nearer is greater, as the
// game compares). Nothing of the GS here.

#include "gfx/native/native_pipeline.h"

#include "native_mesh.frag.spv.h"
#include "native_mesh.vert.spv.h"

#include <algorithm>
#include <cstring>
#include <cstdio>
#include <cstdlib>

namespace dq8::gfx::native
{
namespace
{
// A vertex as the mesh shader takes it.
struct GpuVertex
{
    float position[3];
    float uv[2];
    float color[4];
    float fog;
};
static_assert(sizeof(GpuVertex) == 40u, "tightly packed");

struct DrawUniforms
{
    float clip[16];
    float viewport[16];
    float target[4];
    float fog[4];
};

struct MaterialUniforms
{
    uint32_t mode[4];
    uint32_t test[4];
    float size[4];
    float blend[4];
    float fogColor[4];
    float texa[4];
    uint32_t region[4]; // min u, max u, min v, max v (the GS's CLAMP)
    uint32_t wrap[4];   // u, v: 0 repeat, 1 clamp, 2 region clamp, 3 region repeat
};

void multiply(const float a[16], const float b[16], float out[16])
{
    float r[16];
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            r[i * 4 + j] = a[i * 4 + 0] * b[0 * 4 + j] + a[i * 4 + 1] * b[1 * 4 + j] + a[i * 4 + 2] * b[2 * 4 + j] +
                           a[i * 4 + 3] * b[3 * 4 + j];
    std::memcpy(out, r, sizeof(r));
}

// The pipeline's state as one key: depth compare (3 bits), depth write (1),
// depth used at all (1), color write mask (4), a depth target attached to
// the pass (1), blending (1), its source and destination factors (3 each)
// and op (2).
uint32_t pipelineKey(const Material &m, bool attached)
{
    const bool depth = m.depthTest != Compare::Always || m.depthWrite;
    uint32_t key = static_cast<uint32_t>(m.depthTest) | (m.depthWrite ? 1u << 3 : 0u) | (depth ? 1u << 4 : 0u) |
                   (static_cast<uint32_t>(m.colorWrite & 0xFu) << 5) | (attached ? 1u << 9 : 0u);
    if (m.blending)
        key |= (1u << 10) | (static_cast<uint32_t>(m.srcFactor) << 11) | (static_cast<uint32_t>(m.dstFactor) << 14) |
               (static_cast<uint32_t>(m.blendOp) << 17);
    return key;
}

SDL_GPUBlendFactor blendFactor(BlendFactor f)
{
    switch (f)
    {
    case BlendFactor::Zero: return SDL_GPU_BLENDFACTOR_ZERO;
    case BlendFactor::One: return SDL_GPU_BLENDFACTOR_ONE;
    case BlendFactor::SrcAlpha: return SDL_GPU_BLENDFACTOR_SRC_ALPHA;
    case BlendFactor::InvSrcAlpha: return SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    case BlendFactor::DstAlpha: return SDL_GPU_BLENDFACTOR_DST_ALPHA;
    case BlendFactor::InvDstAlpha: return SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_ALPHA;
    case BlendFactor::Fixed: return SDL_GPU_BLENDFACTOR_CONSTANT_COLOR;
    case BlendFactor::InvFixed: return SDL_GPU_BLENDFACTOR_ONE_MINUS_CONSTANT_COLOR;
    }
    return SDL_GPU_BLENDFACTOR_ONE;
}

SDL_GPUCompareOp compareOp(Compare c)
{
    switch (c)
    {
    case Compare::Always: return SDL_GPU_COMPAREOP_ALWAYS;
    case Compare::Never: return SDL_GPU_COMPAREOP_NEVER;
    case Compare::Less: return SDL_GPU_COMPAREOP_LESS;
    case Compare::LessEqual: return SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
    case Compare::Equal: return SDL_GPU_COMPAREOP_EQUAL;
    case Compare::GreaterEqual: return SDL_GPU_COMPAREOP_GREATER_OR_EQUAL;
    case Compare::Greater: return SDL_GPU_COMPAREOP_GREATER;
    case Compare::NotEqual: return SDL_GPU_COMPAREOP_NOT_EQUAL;
    }
    return SDL_GPU_COMPAREOP_ALWAYS;
}

bool growBuffer(SDL_GPUDevice *device, SDL_GPUBuffer *&buffer, uint32_t &capacity, uint32_t needed,
                SDL_GPUBufferUsageFlags usage, std::string &error)
{
    if (buffer && capacity >= needed)
        return true;
    if (buffer)
        SDL_ReleaseGPUBuffer(device, buffer);
    uint32_t size = std::max<uint32_t>(capacity, 1u << 20);
    while (size < needed)
        size *= 2u;
    SDL_GPUBufferCreateInfo info{};
    info.usage = usage;
    info.size = size;
    buffer = SDL_CreateGPUBuffer(device, &info);
    capacity = buffer ? size : 0u;
    if (!buffer)
        error = std::string("SDL_CreateGPUBuffer(native meshes): ") + SDL_GetError();
    return buffer != nullptr;
}
} // namespace

bool Renderer::ensureMeshes(SDL_GPUDevice *device, std::string &error)
{
    if (m_meshVertex && m_meshFragment)
        return true;
    if (m_meshFailed)
        return false;
    m_meshFailed = true; // until made
    if (!(SDL_GetGPUShaderFormats(device) & SDL_GPU_SHADERFORMAT_SPIRV))
    {
        error = "no SPIR-V: the native mesh shaders have no other build yet";
        return false;
    }
    SDL_GPUShaderCreateInfo vertexInfo{};
    vertexInfo.code = reinterpret_cast<const uint8_t *>(kNativeMeshVertSpirv);
    vertexInfo.code_size = sizeof(kNativeMeshVertSpirv);
    vertexInfo.entrypoint = "main";
    vertexInfo.format = SDL_GPU_SHADERFORMAT_SPIRV;
    vertexInfo.stage = SDL_GPU_SHADERSTAGE_VERTEX;
    vertexInfo.num_uniform_buffers = 1u;
    SDL_GPUShaderCreateInfo fragmentInfo{};
    fragmentInfo.code = reinterpret_cast<const uint8_t *>(kNativeMeshFragSpirv);
    fragmentInfo.code_size = sizeof(kNativeMeshFragSpirv);
    fragmentInfo.entrypoint = "main";
    fragmentInfo.format = SDL_GPU_SHADERFORMAT_SPIRV;
    fragmentInfo.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
    fragmentInfo.num_samplers = 2u;
    fragmentInfo.num_uniform_buffers = 1u;
    m_meshVertex = SDL_CreateGPUShader(device, &vertexInfo);
    m_meshFragment = m_meshVertex ? SDL_CreateGPUShader(device, &fragmentInfo) : nullptr;
    if (!m_meshVertex || !m_meshFragment)
    {
        error = std::string("SDL_CreateGPUShader(native mesh): ") + SDL_GetError();
        return false;
    }
    m_depthFormat = SDL_GPUTextureSupportsFormat(device, SDL_GPU_TEXTUREFORMAT_D32_FLOAT, SDL_GPU_TEXTURETYPE_2D,
                                                 SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET)
                        ? SDL_GPU_TEXTUREFORMAT_D32_FLOAT
                        : SDL_GPU_TEXTUREFORMAT_D24_UNORM;
    // A white texel for untextured draws: the shader ignores it, but a
    // sampler binding must be made.
    m_white = createTexture(device, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, 1u, 1u, error);
    if (!m_white)
        return false;
    static const Bytes white = std::make_shared<const std::vector<uint8_t>>(std::vector<uint8_t>{255, 255, 255, 255});
    m_uploads.push_back({m_white, 1u, 1u, white});
    m_meshFailed = false;
    return true;
}

SDL_GPUTexture *Renderer::targetTexture(SDL_GPUDevice *device, Id id, bool depth, uint32_t minW, uint32_t minH,
                                        bool &fresh, std::string &error)
{
    GpuTarget &t = m_gpuTargets[id];
    fresh = false;
    if (t.texture && t.width >= minW && t.height >= minH)
        return t.texture;
    const uint32_t width = std::max(t.width, minW), height = std::max(t.height, minH);
    if (t.texture)
        SDL_ReleaseGPUTexture(device, t.texture);
    SDL_GPUTextureCreateInfo info{};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = depth ? m_depthFormat : SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    info.usage = depth ? SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET
                       : (SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER);
    info.width = width;
    info.height = height;
    info.layer_count_or_depth = 1;
    info.num_levels = 1;
    t.texture = SDL_CreateGPUTexture(device, &info);
    t.width = t.texture ? width : 0u;
    t.height = t.texture ? height : 0u;
    if (!t.texture)
        error = std::string("SDL_CreateGPUTexture(native target): ") + SDL_GetError();
    fresh = t.texture != nullptr;
    return t.texture;
}

SDL_GPUSampler *Renderer::meshSampler(SDL_GPUDevice *device, uint32_t key)
{
    SDL_GPUSampler *&sampler = m_meshSamplers[key & 7u];
    if (sampler)
        return sampler;
    SDL_GPUSamplerCreateInfo info{};
    const bool linear = key & 1u;
    info.min_filter = linear ? SDL_GPU_FILTER_LINEAR : SDL_GPU_FILTER_NEAREST;
    info.mag_filter = info.min_filter;
    info.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    info.address_mode_u = (key & 2u) ? SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE : SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    info.address_mode_v = (key & 4u) ? SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE : SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    info.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler = SDL_CreateGPUSampler(device, &info);
    return sampler;
}

SDL_GPUGraphicsPipeline *Renderer::meshPipeline(SDL_GPUDevice *device, uint32_t key)
{
    if (const auto it = m_meshPipelines.find(key); it != m_meshPipelines.end())
        return it->second;
    const Compare depthTest = static_cast<Compare>(key & 7u);
    const bool depthWrite = (key >> 3) & 1u;
    const bool attached = (key >> 9) & 1u;
    const bool depth = ((key >> 4) & 1u) && attached;

    SDL_GPUColorTargetDescription color{};
    color.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    SDL_GPUColorTargetBlendState &b = color.blend_state;
    b.enable_color_write_mask = true;
    b.color_write_mask = static_cast<SDL_GPUColorComponentFlags>((key >> 5) & 0xFu);
    b.enable_blend = (key >> 10) & 1u;
    b.src_color_blendfactor = blendFactor(static_cast<BlendFactor>((key >> 11) & 7u));
    b.dst_color_blendfactor = blendFactor(static_cast<BlendFactor>((key >> 14) & 7u));
    const BlendOp op = static_cast<BlendOp>((key >> 17) & 3u);
    b.color_blend_op = op == BlendOp::Subtract          ? SDL_GPU_BLENDOP_SUBTRACT
                       : op == BlendOp::ReverseSubtract ? SDL_GPU_BLENDOP_REVERSE_SUBTRACT
                                                        : SDL_GPU_BLENDOP_ADD;
    // The stored alpha is the source's, as the game's blend leaves it.
    b.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    b.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
    b.alpha_blend_op = SDL_GPU_BLENDOP_ADD;

    SDL_GPUVertexBufferDescription buffer{};
    buffer.slot = 0u;
    buffer.pitch = sizeof(GpuVertex);
    buffer.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
    SDL_GPUVertexAttribute attributes[4]{};
    attributes[0] = {0u, 0u, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, offsetof(GpuVertex, position)};
    attributes[1] = {1u, 0u, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(GpuVertex, uv)};
    attributes[2] = {2u, 0u, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, offsetof(GpuVertex, color)};
    attributes[3] = {3u, 0u, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT, offsetof(GpuVertex, fog)};

    SDL_GPUGraphicsPipelineCreateInfo info{};
    info.vertex_shader = m_meshVertex;
    info.fragment_shader = m_meshFragment;
    info.vertex_input_state.vertex_buffer_descriptions = &buffer;
    info.vertex_input_state.num_vertex_buffers = 1u;
    info.vertex_input_state.vertex_attributes = attributes;
    info.vertex_input_state.num_vertex_attributes = 4u;
    info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE; // the producer's indices are already culled
    // Clamped, not clipped, at the far plane: the game's sky sits past it.
    info.rasterizer_state.enable_depth_clip = false;
    info.depth_stencil_state.enable_depth_test = depth;
    info.depth_stencil_state.enable_depth_write = depth && depthWrite;
    info.depth_stencil_state.compare_op = compareOp(depthTest);
    info.target_info.color_target_descriptions = &color;
    info.target_info.num_color_targets = 1u;
    info.target_info.has_depth_stencil_target = attached;
    info.target_info.depth_stencil_format = attached ? m_depthFormat : SDL_GPU_TEXTUREFORMAT_INVALID;
    SDL_GPUGraphicsPipeline *pipeline = SDL_CreateGPUGraphicsPipeline(device, &info);
    if (!pipeline)
        std::fprintf(stderr, "[native-pipeline] mesh pipeline %x refused: %s\n", key, SDL_GetError());
    m_meshPipelines.emplace(key, pipeline); // a refusal is kept too: not retried every draw
    return pipeline;
}

bool Renderer::uploadMeshes(SDL_GPUDevice *device, SDL_GPUCommandBuffer *commands, std::string &error)
{
    m_firstIndex.clear();
    m_vertexOffset.clear();
    uint64_t vertices = 0u, indices = 0u;
    for (const MeshDraw &d : m_frameMeshes)
    {
        m_firstIndex.push_back(static_cast<uint32_t>(indices));
        m_vertexOffset.push_back(static_cast<int32_t>(vertices));
        vertices += d.vertices.size();
        indices += d.indices.size();
    }
    if (indices == 0u)
        return true;
    const uint64_t vertexBytes = vertices * sizeof(GpuVertex), indexBytes = indices * sizeof(uint32_t);
    if (vertexBytes + indexBytes > (1ull << 31))
    {
        error = "native meshes: a frame over 2 GiB of vertices";
        return false;
    }
    if (!growBuffer(device, m_vertexBuffer, m_vertexCapacity, static_cast<uint32_t>(vertexBytes),
                    SDL_GPU_BUFFERUSAGE_VERTEX, error) ||
        !growBuffer(device, m_indexBuffer, m_indexCapacity, static_cast<uint32_t>(indexBytes),
                    SDL_GPU_BUFFERUSAGE_INDEX, error))
        return false;
    const uint32_t total = static_cast<uint32_t>(vertexBytes + indexBytes);
    if (!m_meshTransfer || m_transferCapacity < total)
    {
        if (m_meshTransfer)
            SDL_ReleaseGPUTransferBuffer(device, m_meshTransfer);
        SDL_GPUTransferBufferCreateInfo info{};
        info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        info.size = std::max(total, m_transferCapacity * 2u);
        m_meshTransfer = SDL_CreateGPUTransferBuffer(device, &info);
        m_transferCapacity = m_meshTransfer ? info.size : 0u;
        if (!m_meshTransfer)
        {
            error = std::string("SDL_CreateGPUTransferBuffer(native meshes): ") + SDL_GetError();
            return false;
        }
    }
    auto *mapped = static_cast<uint8_t *>(SDL_MapGPUTransferBuffer(device, m_meshTransfer, true));
    if (!mapped)
    {
        error = std::string("SDL_MapGPUTransferBuffer(native meshes): ") + SDL_GetError();
        return false;
    }
    auto *v = reinterpret_cast<GpuVertex *>(mapped);
    auto *ix = reinterpret_cast<uint32_t *>(mapped + vertexBytes);
    for (const MeshDraw &d : m_frameMeshes)
    {
        for (const Vertex &in : d.vertices)
        {
            std::memcpy(v->position, in.position, sizeof(v->position));
            std::memcpy(v->uv, in.uv, sizeof(v->uv));
            std::memcpy(v->color, in.color, sizeof(v->color));
            v->fog = in.fog;
            ++v;
        }
        std::memcpy(ix, d.indices.data(), d.indices.size() * sizeof(uint32_t));
        ix += d.indices.size();
    }
    SDL_UnmapGPUTransferBuffer(device, m_meshTransfer);
    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(commands);
    SDL_GPUTransferBufferLocation source{m_meshTransfer, 0u};
    SDL_GPUBufferRegion destination{m_vertexBuffer, 0u, static_cast<uint32_t>(vertexBytes)};
    SDL_UploadToGPUBuffer(copy, &source, &destination, true);
    source.offset = static_cast<uint32_t>(vertexBytes);
    destination = {m_indexBuffer, 0u, static_cast<uint32_t>(indexBytes)};
    SDL_UploadToGPUBuffer(copy, &source, &destination, true);
    SDL_EndGPUCopyPass(copy);
    return true;
}

void Renderer::drawMeshes(SDL_GPUDevice *device, SDL_GPUCommandBuffer *commands, SDL_GPURenderPass *pass,
                          size_t first, size_t last, Id target, bool depthAttached, float targetW, float targetH,
                          uint32_t scale, SDL_GPUTexture *copy, uint32_t copyW, uint32_t copyH)
{
    if (!m_vertexBuffer || !m_indexBuffer)
        return;
    const SDL_GPUBufferBinding vertexBinding{m_vertexBuffer, 0u};
    SDL_BindGPUVertexBuffers(pass, 0, &vertexBinding, 1);
    const SDL_GPUBufferBinding indexBinding{m_indexBuffer, 0u};
    SDL_BindGPUIndexBuffer(pass, &indexBinding, SDL_GPU_INDEXELEMENTSIZE_32BIT);
    SDL_GPUGraphicsPipeline *bound = nullptr;
    for (size_t n = first; n < last; ++n)
    {
        const MeshDraw &d = m_frameMeshes[n];
        if (d.indices.empty())
            continue;
        const Camera *camera = nullptr;
        if (const auto it = m_cameras.find(d.camera); d.camera != 0u && it != m_cameras.end())
            camera = &it->second;
        const Material &m = d.material;
        // A 3D draw needs its camera; a window-space one only to map its
        // depth when it tests it. One that only writes depth (test ALWAYS:
        // the menus' text, with no 3D drawn yet) writes a constant without
        // one.
        if (d.hasModel ? !camera : (m.depthTest != Compare::Always && !camera))
        {
            ++m_meshCounts.noCamera;
            continue;
        }
        MaterialUniforms mu{};
        mu.texa[0] = m.fixedTextureAlpha;
        mu.texa[1] = m.alphaZeroOnBlack ? 1.0f : 0.0f;
        SDL_GPUTexture *image = m_white, *palette = m_white;
        float ownW = 1.0f, ownH = 1.0f;
        if (m.sourceTarget != 0u)
        {
            // A target drawn earlier this frame (or kept from before), read
            // in game pixels: its texture may be larger and is upscaled.
            const float s = float(std::max(scale, 1u));
            if (m.sourceTarget == target)
            {
                // The copy drawFrame took just before (only the run's
                // first draw is given one).
                if (!copy || n != first)
                {
                    ++m_meshCounts.feedback;
                    continue;
                }
                image = palette = copy;
                ownW = float(copyW) / s;
                ownH = float(copyH) / s;
            }
            else if (m.sourceTarget == m_sceneTarget)
            {
                image = palette = m_scene;
                ownW = float(m_sceneWidth) / s;
                ownH = float(m_sceneHeight) / s;
            }
            else
            {
                const auto it = m_gpuTargets.find(m.sourceTarget);
                if (it == m_gpuTargets.end() || !it->second.texture)
                {
                    ++m_meshCounts.sourceNotDrawn;
                    continue;
                }
                image = palette = it->second.texture;
                ownW = float(it->second.width) / s;
                ownH = float(it->second.height) / s;
            }
            mu.mode[0] = 3u;
            mu.mode[1] = static_cast<uint32_t>(m.function);
            mu.mode[2] = m.textureAlpha ? 1u : 0u;
            ++m_meshCounts.fromTarget;
        }
        else if (m.texture != 0u)
        {
            const auto it = m_images.find(m.texture);
            if (it == m_images.end())
            {
                ++m_meshCounts.noTexture;
                continue;
            }
            const Image &img = it->second;
            image = palette = img.texture;
            ownW = float(img.width);
            ownH = float(img.height);
            mu.mode[0] = 1u;
            if (img.format == PixelFormat::Index8)
            {
                const auto p = m_palettes.find(m.palette ? m.palette : img.palette);
                if (p == m_palettes.end())
                {
                    ++m_meshCounts.noTexture;
                    continue;
                }
                palette = p->second.texture;
                mu.mode[0] = 2u;
                mu.mode[3] = p->second.width;
            }
            mu.mode[1] = static_cast<uint32_t>(m.function);
            mu.mode[2] = m.textureAlpha ? 1u : 0u;
        }
        if (m.destinationAlphaTest >= 0)
        {
            // The target's alpha, from the copy, at the pixel (the palette
            // slot, which an Index8 texture would need too).
            if (!copy || n != first || mu.mode[0] == 2u)
            {
                ++m_meshCounts.feedback;
                continue;
            }
            palette = copy;
            mu.texa[2] = 1.0f + float(m.destinationAlphaTest);
            ++m_meshCounts.destinationAlpha;
        }
        SDL_GPUGraphicsPipeline *pipeline = meshPipeline(device, pipelineKey(m, depthAttached));
        if (!pipeline)
        {
            ++m_meshCounts.noPipeline;
            continue;
        }
        if (pipeline != bound)
        {
            SDL_BindGPUGraphicsPipeline(pass, pipeline);
            bound = pipeline;
        }
        mu.test[0] = static_cast<uint32_t>(m.alphaTest);
        mu.test[1] = m.alphaRef;
        mu.test[2] = m.alphaFail == AlphaFail::Nothing ? 1u : 0u;
        mu.test[3] = m.fog ? 1u : 0u;
        mu.size[0] = m.textureWidth ? float(m.textureWidth) : ownW;
        mu.size[1] = m.textureHeight ? float(m.textureHeight) : ownH;
        mu.size[2] = ownW;
        mu.size[3] = ownH;
        mu.blend[0] = m.shuffle == ChannelShuffle::GreenToAlpha ? 1.0f : 0.0f;
        mu.blend[2] = m.clampU ? 1.0f : 0.0f;
        mu.blend[3] = m.clampV ? 1.0f : 0.0f;
        mu.wrap[0] = m.wrapU;
        mu.wrap[1] = m.wrapV;
        mu.region[0] = m.regionU[0];
        mu.region[1] = m.regionU[1];
        mu.region[2] = m.regionV[0];
        mu.region[3] = m.regionV[1];
        for (int i = 0; i < 3; ++i)
            mu.fogColor[i] = float(d.fogColor[i]) / 255.0f;

        DrawUniforms du{};
        if (d.hasModel)
        {
            float modelView[16];
            multiply(d.model, camera->view, modelView);
            multiply(modelView, camera->projection, du.clip);
            std::memcpy(du.viewport, camera->viewport, sizeof(du.viewport));
        }
        else
        {
            // Window space: x and y are the target's pixels, w is 1. The
            // game's window depth is linear in the projection's z / w
            // (Zgs = V22 * z / w + V32, V the camera's viewport), so the
            // clip z the vertex shader turns into reversed depth is
            // (Zgs - V32) / V22. With no camera, depth is unused.
            for (int i = 0; i < 4; ++i)
                du.clip[i * 5] = du.viewport[i * 5] = 1.0f;
            du.clip[10] = 0.0f;
            if (camera && camera->viewport[10] != 0.0f)
            {
                du.clip[10] = 1.0f / camera->viewport[10];
                du.clip[14] = -camera->viewport[14] / camera->viewport[10];
            }
            ++m_meshCounts.window;
        }
        du.target[0] = targetW;
        du.target[1] = targetH;
        du.target[2] = d.hasModel ? 0.0f : 1.0f; // fog per vertex
        std::memcpy(du.fog, d.fog, sizeof(du.fog));

        if (m.blending && (m.srcFactor == BlendFactor::Fixed || m.srcFactor == BlendFactor::InvFixed ||
                           m.dstFactor == BlendFactor::Fixed || m.dstFactor == BlendFactor::InvFixed))
        {
            const float f = std::min(float(m.blendFactor) / 128.0f, 1.0f);
            SDL_SetGPUBlendConstants(pass, SDL_FColor{f, f, f, f});
        }
        SDL_PushGPUVertexUniformData(commands, 0, &du, sizeof(du));
        SDL_PushGPUFragmentUniformData(commands, 0, &mu, sizeof(mu));
        const uint32_t samplerKey = (m.linear ? 1u : 0u) | (m.clampU ? 2u : 0u) | (m.clampV ? 4u : 0u);
        SDL_GPUSampler *sampler = meshSampler(device, samplerKey);
        const SDL_GPUTextureSamplerBinding bindings[2] = {{image, sampler}, {palette, sampler}};
        SDL_BindGPUFragmentSamplers(pass, 0, bindings, 2);

        const int32_t sx0 = std::clamp(d.scissor.x0, 0, int32_t(targetW));
        const int32_t sy0 = std::clamp(d.scissor.y0, 0, int32_t(targetH));
        const int32_t sx1 = std::clamp(d.scissor.x1, sx0, int32_t(targetW));
        const int32_t sy1 = std::clamp(d.scissor.y1, sy0, int32_t(targetH));
        const int32_t s = int32_t(std::max(scale, 1u));
        const SDL_Rect scissor{sx0 * s, sy0 * s, (sx1 - sx0) * s, (sy1 - sy0) * s};
        SDL_SetGPUScissor(pass, &scissor);
        SDL_DrawGPUIndexedPrimitives(pass, static_cast<uint32_t>(d.indices.size()), 1, m_firstIndex[n],
                                     m_vertexOffset[n], 0);
        ++m_meshCounts.drawn;
    }
}

bool Renderer::drawFrame(SDL_GPUDevice *device, SDL_GPUCommandBuffer *commands, uint32_t scale, std::string &error)
{
    scale = std::max(scale, 1u);
    // The scene starts each frame cleared (the game's own clears are not
    // records yet); every other target keeps what it holds.
    {
        SDL_GPUColorTargetInfo clear{};
        clear.texture = m_scene;
        clear.load_op = SDL_GPU_LOADOP_CLEAR;
        clear.store_op = SDL_GPU_STOREOP_STORE;
        clear.clear_color = SDL_FColor{0.0f, 0.0f, 0.0f, 1.0f};
        SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(commands, &clear, 1, nullptr);
        if (!pass)
        {
            error = std::string("SDL_BeginGPURenderPass(native scene clear): ") + SDL_GetError();
            return false;
        }
        SDL_EndGPURenderPass(pass);
    }
    if (!uploadMeshes(device, commands, error))
        return false;
    // RG_NATIVE_FRAME_DUMP=N: the N-th frame's draws, one line each.
    static const uint64_t dumpFrame = [] {
        const char *v = std::getenv("RG_NATIVE_FRAME_DUMP");
        return v ? std::strtoull(v, nullptr, 10) : 0ull;
    }();
    // Or the frame after build/run/dump-gpu appears (the producer's trigger).
    const bool triggered = std::remove("build/run/dump-gpu") == 0;
    if (triggered)
    {
        std::fprintf(stderr, "[frame-dump] frame %llu (%zu draws)\n", (unsigned long long)m_meshCounts.frames,
                     m_frameMeshes.size());
        m_dumpTargets = true;
    }
    if ((dumpFrame != 0u && m_meshCounts.frames == dumpFrame) || triggered)
    {
        for (size_t n = 0; n < m_frameMeshes.size(); ++n)
        {
            const MeshDraw &d = m_frameMeshes[n];
            float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
            for (const Vertex &v : d.vertices)
            {
                x0 = std::min(x0, v.position[0]);
                y0 = std::min(y0, v.position[1]);
                x1 = std::max(x1, v.position[0]);
                y1 = std::max(y1, v.position[1]);
            }
            const Material &m = d.material;
            const float *c = d.vertices.empty() ? nullptr : d.vertices[0].color;
            std::fprintf(stderr,
                         "[frame-dump] %4zu target %u depth %u %s src %s%u fn %d tcc %d blend %d/%d/%d fix %u write %x "
                         "atest %d/%u ztest %d zw %d tris %zu box %.0f,%.0f-%.0f,%.0f rgba %.0f,%.0f,%.0f,%.0f z %.0f fog %.0f cam %u%s\n",
                         n, d.target, d.depthTarget, d.hasModel ? "3d" : "2d", m.sourceTarget ? "target " : "tex ",
                         m.sourceTarget ? m.sourceTarget : m.texture, int(m.function), int(m.textureAlpha),
                         m.blending ? int(m.srcFactor) : -1, int(m.dstFactor), int(m.blendOp), m.blendFactor, m.colorWrite, int(m.alphaTest), m.alphaRef, int(m.depthTest),
                         int(m.depthWrite), d.indices.size() / 3, d.hasModel ? 0.f : x0, d.hasModel ? 0.f : y0,
                         d.hasModel ? 0.f : x1, d.hasModel ? 0.f : y1, c ? c[0] : 0.f, c ? c[1] : 0.f,
                         c ? c[2] : 0.f, c ? c[3] : 0.f, d.vertices.empty() ? 0.f : d.vertices[0].position[2],
                         d.vertices.empty() ? 0.f : d.vertices[0].fog, d.camera, m.known ? "" : " (unknown)");
        }
    }
    std::vector<Id> depthsCleared;
    const size_t count = m_frameMeshes.size();
    size_t i = 0;
    while (i < count)
    {
        const Id target = m_frameMeshes[i].target;
        size_t j = i;
        Id depthId = 0u;
        while (j < count && m_frameMeshes[j].target == target)
        {
            const Id d = m_frameMeshes[j].depthTarget;
            if (d != 0u && depthId == 0u)
                depthId = d;
            else if (d != 0u && d != depthId)
                ++m_meshCounts.depthMismatch;
            ++j;
        }
        const size_t first = i;
        i = j;
        const auto declared = m_targets.find(target);
        if (declared == m_targets.end() || declared->second.width == 0u || declared->second.height == 0u)
        {
            m_meshCounts.noTarget += j - first;
            continue;
        }
        if (declared->second.format == PixelFormat::Depth32)
        {
            m_meshCounts.depthAsColor += j - first;
            continue;
        }
        const uint32_t gameW = declared->second.width, gameH = declared->second.height;
        const bool scene = target == m_sceneTarget;
        if (!scene)
            m_meshCounts.offscreen += j - first;
        SDL_GPUTexture *color = m_scene;
        uint32_t colorW = m_sceneWidth, colorH = m_sceneHeight;
        bool fresh = false;
        if (!scene)
        {
            color = targetTexture(device, target, false, gameW * scale, gameH * scale, fresh, error);
            if (!color)
                return false;
            colorW = m_gpuTargets[target].width;
            colorH = m_gpuTargets[target].height;
        }
        SDL_GPUColorTargetInfo colorInfo{};
        colorInfo.texture = color;
        colorInfo.load_op = fresh ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;
        colorInfo.store_op = SDL_GPU_STOREOP_STORE;
        colorInfo.clear_color = SDL_FColor{0.0f, 0.0f, 0.0f, 0.0f};
        SDL_GPUDepthStencilTargetInfo depthInfo{};
        if (depthId != 0u)
        {
            bool depthFresh = false;
            depthInfo.texture = targetTexture(device, depthId, true, colorW, colorH, depthFresh, error);
            if (!depthInfo.texture)
                return false;
            const bool firstUse = std::find(depthsCleared.begin(), depthsCleared.end(), depthId) == depthsCleared.end();
            if (firstUse)
                depthsCleared.push_back(depthId);
            depthInfo.load_op = firstUse || depthFresh ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;
            depthInfo.store_op = SDL_GPU_STOREOP_STORE;
            depthInfo.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
            depthInfo.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
            depthInfo.clear_depth = 0.0f;
        }
        // A draw that reads the target it draws into (its texels, or its
        // alpha for the destination alpha test) reads a copy taken just
        // before it: the pass ends there, and the next one loads.
        const auto readsTarget = [&](const MeshDraw &d) {
            return d.material.sourceTarget == target || d.material.destinationAlphaTest >= 0;
        };
        size_t a = first;
        while (a < j)
        {
            const bool reads = readsTarget(m_frameMeshes[a]);
            size_t b = a + 1;
            while (b < j && !readsTarget(m_frameMeshes[b]))
                ++b;
            SDL_GPUTexture *copy = nullptr;
            if (reads)
            {
                bool copyFresh = false;
                copy = targetTexture(device, kFeedbackCopy, false, colorW, colorH, copyFresh, error);
                if (!copy)
                    return false;
                SDL_GPUCopyPass *pass = SDL_BeginGPUCopyPass(commands);
                SDL_GPUTextureLocation from{}, to{};
                from.texture = color;
                to.texture = copy;
                SDL_CopyGPUTextureToTexture(pass, &from, &to, colorW, colorH, 1, false);
                SDL_EndGPUCopyPass(pass);
                ++m_meshCounts.copies;
            }
            SDL_GPURenderPass *pass =
                SDL_BeginGPURenderPass(commands, &colorInfo, 1, depthId ? &depthInfo : nullptr);
            if (!pass)
            {
                error = std::string("SDL_BeginGPURenderPass(native target): ") + SDL_GetError();
                return false;
            }
            ++m_meshCounts.passes;
            const SDL_GPUViewport viewport{0.0f, 0.0f, float(gameW * scale), float(gameH * scale), 0.0f, 1.0f};
            SDL_SetGPUViewport(pass, &viewport);
            const GpuTarget &copied = m_gpuTargets[kFeedbackCopy]; // its texture may be the larger
            drawMeshes(device, commands, pass, a, b, target, depthId != 0u, float(gameW), float(gameH), scale,
                       copy, copied.width, copied.height);
            SDL_EndGPURenderPass(pass);
            colorInfo.load_op = SDL_GPU_LOADOP_LOAD;
            depthInfo.load_op = SDL_GPU_LOADOP_LOAD;
            a = b;
        }
    }
    return true;
}

void Renderer::releaseMeshes(SDL_GPUDevice *device)
{
    for (auto &[key, pipeline] : m_meshPipelines)
        if (pipeline)
            SDL_ReleaseGPUGraphicsPipeline(device, pipeline);
    m_meshPipelines.clear();
    for (SDL_GPUSampler *&sampler : m_meshSamplers)
    {
        if (sampler)
            SDL_ReleaseGPUSampler(device, sampler);
        sampler = nullptr;
    }
    if (m_meshVertex)
        SDL_ReleaseGPUShader(device, m_meshVertex);
    if (m_meshFragment)
        SDL_ReleaseGPUShader(device, m_meshFragment);
    for (auto &[id, target] : m_gpuTargets)
        if (target.texture)
            SDL_ReleaseGPUTexture(device, target.texture);
    m_gpuTargets.clear();
    if (m_white)
        SDL_ReleaseGPUTexture(device, m_white);
    if (m_vertexBuffer)
        SDL_ReleaseGPUBuffer(device, m_vertexBuffer);
    if (m_indexBuffer)
        SDL_ReleaseGPUBuffer(device, m_indexBuffer);
    if (m_meshTransfer)
        SDL_ReleaseGPUTransferBuffer(device, m_meshTransfer);
    m_meshVertex = m_meshFragment = nullptr;
    m_white = nullptr;
    m_vertexBuffer = m_indexBuffer = nullptr;
    m_meshTransfer = nullptr;
    m_vertexCapacity = m_indexCapacity = m_transferCapacity = 0u;
    m_meshFailed = false;
}
} // namespace dq8::gfx::native
