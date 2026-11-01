#include "gfx/backends/sdlgpu/sdlgpu_device.h"

#include "gs_draw.frag.spv.h"
#include "gs_draw.frag.dstcopy.spv.h"
#include "gs_draw.frag.dual.spv.h"
#include "gs_draw.vert.spv.h"
#include "gs_native.vert.spv.h"
#include "gs_index8.frag.spv.h"
#include "gs_local_copy.frag.spv.h"
#include "gs_reinterpret.frag.spv.h"
#include "gs_reinterpret.vert.spv.h"
#ifdef DQ8_GFX_HAS_MSL
#include "gs_draw.frag.msl.h"
#include "gs_draw.frag.fetch.msl.h"
#include "gs_draw.vert.msl.h"
#include "gs_native.vert.msl.h"
#include "gs_index8.frag.msl.h"
#include "gs_local_copy.frag.msl.h"
#include "gs_reinterpret.frag.msl.h"
#include "gs_reinterpret.vert.msl.h"
#endif
#include "runtime/gs/ps2_gs_psmct32.h"
#include "runtime/gs/ps2_gs_psmt8.h"

#include <chrono>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace dq8::gfx {

namespace {

// The GS blend equation is linear in Cs and Cd, so each operand's coefficient
// is `constant + factor * C` with constant in {0, 1} and factor in {-1, 0, 1}.
struct Coefficient {
    int constant = 0;
    int factor = 0;

    bool isZero() const { return constant == 0 && factor == 0; }
    bool isNegative() const { return constant == 0 && factor < 0; }
    // (1 + C) has no blend-factor equivalent.
    bool isExpressible() const { return !(constant == 1 && factor > 0); }
};

Coefficient coefficientFor(const GsAlphaState &alpha, GsBlendColor operand) {
    Coefficient result{};
    if (alpha.d == operand)
        result.constant = 1;
    if (alpha.a == operand)
        result.factor += 1;
    if (alpha.b == operand)
        result.factor -= 1;
    return result;
}

// Maps |coefficient| onto a blend factor. The sign is carried by the blend
// operation, so only the magnitude is encoded here.
SDL_GPUBlendFactor factorFor(const Coefficient &coefficient, GsBlendAlpha c) {
    if (coefficient.isZero())
        return SDL_GPU_BLENDFACTOR_ZERO;
    if (coefficient.factor == 0)
        return SDL_GPU_BLENDFACTOR_ONE;

    const bool oneMinus = coefficient.constant == 1;
    switch (c) {
    case kBlendAlphaSource:
        return oneMinus ? SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA
                        : SDL_GPU_BLENDFACTOR_SRC_ALPHA;
    case kBlendAlphaDest:
        return oneMinus ? SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_ALPHA
                        : SDL_GPU_BLENDFACTOR_DST_ALPHA;
    case kBlendAlphaFixed:
    default:
        return oneMinus ? SDL_GPU_BLENDFACTOR_ONE_MINUS_CONSTANT_COLOR
                        : SDL_GPU_BLENDFACTOR_CONSTANT_COLOR;
    }
}

} // namespace

GsBlendTranslation gsTranslateBlend(const GsAlphaState &alpha, bool blendEnabled) {
    GsBlendTranslation result{};
    if (!blendEnabled) {
        result.enabled = false;
        return result;
    }
    result.enabled = true;

    Coefficient source = coefficientFor(alpha, kBlendSource);
    Coefficient dest = coefficientFor(alpha, kBlendDest);

    if (!source.isExpressible() || !dest.isExpressible()) {
        // Cs*(1+C) and friends. Drop the constant term, which keeps the
        // factor-weighted part right and is the closer of the two truncations.
        result.exact = false;
        if (!source.isExpressible())
            source.constant = 0;
        if (!dest.isExpressible())
            dest.constant = 0;
    }

    const bool sourceNegative = source.isNegative();
    const bool destNegative = dest.isNegative();
    if (sourceNegative && destNegative) {
        // -(C*Cs) - (C*Cd) clamps to black for any non-negative input, so the
        // result is exactly what a ZERO/ZERO blend produces.
        result.sourceFactor = SDL_GPU_BLENDFACTOR_ZERO;
        result.destinationFactor = SDL_GPU_BLENDFACTOR_ZERO;
        result.operation = SDL_GPU_BLENDOP_ADD;
        return result;
    }

    result.sourceFactor = factorFor(source, alpha.c);
    result.destinationFactor = factorFor(dest, alpha.c);
    if (destNegative)
        result.operation = SDL_GPU_BLENDOP_SUBTRACT;
    else if (sourceNegative)
        result.operation = SDL_GPU_BLENDOP_REVERSE_SUBTRACT;
    else
        result.operation = SDL_GPU_BLENDOP_ADD;

    if (alpha.c == kBlendAlphaFixed &&
        (result.sourceFactor == SDL_GPU_BLENDFACTOR_CONSTANT_COLOR ||
         result.sourceFactor == SDL_GPU_BLENDFACTOR_ONE_MINUS_CONSTANT_COLOR ||
         result.destinationFactor == SDL_GPU_BLENDFACTOR_CONSTANT_COLOR ||
         result.destinationFactor == SDL_GPU_BLENDFACTOR_ONE_MINUS_CONSTANT_COLOR)) {
        result.usesConstant = true;
        // FIX is a /128 fraction, so it reaches almost 2. A UNORM target
        // clamps the constant at 1, which is the one place this mapping loses
        // range rather than precision.
        result.constant = static_cast<float>(alpha.fix) / 128.0f;
        if (result.constant > 1.0f)
            result.exact = false;
    }
    return result;
}

SDL_GPUCompareOp gsTranslateDepthCompare(GsDepthTest test, bool enabled) {
    if (!enabled)
        return SDL_GPU_COMPAREOP_ALWAYS;
    switch (test) {
    case kDepthNever:
        return SDL_GPU_COMPAREOP_NEVER;
    case kDepthGEqual:
        return SDL_GPU_COMPAREOP_GREATER_OR_EQUAL;
    case kDepthGreater:
        return SDL_GPU_COMPAREOP_GREATER;
    case kDepthAlways:
    default:
        return SDL_GPU_COMPAREOP_ALWAYS;
    }
}

size_t GsPipelineKeyHash::operator()(const GsPipelineKey &key) const {
    uint64_t hash = 1469598103934665603ull;
    auto mix = [&hash](uint64_t value) {
        hash ^= value + 0x9e3779b97f4a7c15ull + (hash << 6u) + (hash >> 2u);
    };
    mix(key.colorFormat);
    mix(key.depthFormat);
    mix(static_cast<uint64_t>(key.hasDepth) | (static_cast<uint64_t>(key.depthCompare) << 8u) |
        (static_cast<uint64_t>(key.depthWrite) << 16u) |
        (static_cast<uint64_t>(key.colorWriteMask) << 24u) |
        (static_cast<uint64_t>(key.blendEnabled) << 32u) |
        (static_cast<uint64_t>(key.sourceFactor) << 40u) |
        (static_cast<uint64_t>(key.destinationFactor) << 48u) |
        (static_cast<uint64_t>(key.blendOperation) << 56u));
    mix(key.destinationCopy | (static_cast<uint64_t>(key.nativeMesh) << 8u) |
        (static_cast<uint64_t>(key.cullMode) << 16u) | (static_cast<uint64_t>(key.dualSource) << 24u));
    return static_cast<size_t>(hash);
}

SdlGpuDevice::~SdlGpuDevice() {
    destroy();
}

bool SdlGpuDevice::create(std::string &error) {
    SDL_GPUShaderFormat formats = SDL_GPU_SHADERFORMAT_SPIRV;
#ifdef DQ8_GFX_HAS_MSL
    formats |= SDL_GPU_SHADERFORMAT_MSL;
#endif
    m_device = SDL_CreateGPUDevice(formats, false, nullptr);
    if (!m_device) {
        error = std::string("SDL_CreateGPUDevice: ") + SDL_GetError();
        return false;
    }

    for (SDL_GPUTextureFormat candidate : {SDL_GPU_TEXTUREFORMAT_D32_FLOAT,
                                           SDL_GPU_TEXTUREFORMAT_D24_UNORM,
                                           SDL_GPU_TEXTUREFORMAT_D16_UNORM}) {
        if (SDL_GPUTextureSupportsFormat(m_device, candidate, SDL_GPU_TEXTURETYPE_2D,
                                         SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET)) {
            m_depthFormat = candidate;
            break;
        }
    }

    SDL_GPUShaderCreateInfo vertexInfo{};
    vertexInfo.code = reinterpret_cast<const uint8_t *>(kGsDrawVertSpirv);
    vertexInfo.code_size = sizeof(kGsDrawVertSpirv);
    vertexInfo.entrypoint = "main";
    vertexInfo.format = SDL_GPU_SHADERFORMAT_SPIRV;
    vertexInfo.stage = SDL_GPU_SHADERSTAGE_VERTEX;
    vertexInfo.num_uniform_buffers = 1u;
#ifdef DQ8_GFX_HAS_MSL
    const bool metal = (SDL_GetGPUShaderFormats(m_device) & SDL_GPU_SHADERFORMAT_MSL) != 0;
    if (metal) {
        vertexInfo.code = reinterpret_cast<const uint8_t *>(kGsDrawVertSpirvMsl);
        vertexInfo.code_size = sizeof(kGsDrawVertSpirvMsl);
        vertexInfo.entrypoint = "main0";
        vertexInfo.format = SDL_GPU_SHADERFORMAT_MSL;
    }
#endif
    m_vertexShader = SDL_CreateGPUShader(m_device, &vertexInfo);
    if (!m_vertexShader) {
        error = std::string("SDL_CreateGPUShader(vertex): ") + SDL_GetError();
        destroy();
        return false;
    }
    // The native strips' (Rogue Galaxy: added), with the same bindings and
    // their records' storage buffer.
    vertexInfo.num_storage_buffers = 1u;
    vertexInfo.code = reinterpret_cast<const uint8_t *>(kGsNativeVertSpirv);
    vertexInfo.code_size = sizeof(kGsNativeVertSpirv);
#ifdef DQ8_GFX_HAS_MSL
    if (metal) {
        vertexInfo.code = reinterpret_cast<const uint8_t *>(kGsNativeVertSpirvMsl);
        vertexInfo.code_size = sizeof(kGsNativeVertSpirvMsl);
    }
#endif
    m_nativeVertexShader = SDL_CreateGPUShader(m_device, &vertexInfo);
    if (!m_nativeVertexShader) {
        error = std::string("SDL_CreateGPUShader(native vertex): ") + SDL_GetError();
        destroy();
        return false;
    }

    SDL_GPUShaderCreateInfo fragmentInfo{};
    fragmentInfo.code = reinterpret_cast<const uint8_t *>(kGsDrawFragSpirv);
    fragmentInfo.code_size = sizeof(kGsDrawFragSpirv);
    fragmentInfo.entrypoint = "main";
    fragmentInfo.format = SDL_GPU_SHADERFORMAT_SPIRV;
    fragmentInfo.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
    fragmentInfo.num_samplers = 1u;
    fragmentInfo.num_uniform_buffers = 1u;
#ifdef DQ8_GFX_HAS_MSL
    if (metal) {
        fragmentInfo.code = reinterpret_cast<const uint8_t *>(kGsDrawFragSpirvMsl);
        fragmentInfo.code_size = sizeof(kGsDrawFragSpirvMsl);
        fragmentInfo.entrypoint = "main0";
        fragmentInfo.format = SDL_GPU_SHADERFORMAT_MSL;
#if defined(__aarch64__)
        if (!std::getenv("DQ8_GFX_DISABLE_FRAMEBUFFER_FETCH")) {
            fragmentInfo.code = reinterpret_cast<const uint8_t *>(kGsDrawFetchSpirvMsl);
            fragmentInfo.code_size = sizeof(kGsDrawFetchSpirvMsl);
            m_framebufferFetch = true;
        }
#endif
    }
#endif
    m_fragmentShader = SDL_CreateGPUShader(m_device, &fragmentInfo);
    if (!m_fragmentShader) {
        error = std::string("SDL_CreateGPUShader(fragment): ") + SDL_GetError();
        destroy();
        return false;
    }
    // Without framebuffer fetch, the variant that reads the destination from
    // a snapshot (a second sampler); SPIR-V only, and the draws that would
    // use it keep their approximation if it cannot be made, or with
    // DQ8_GFX_NO_DESTINATION_COPY=1 (Rogue Galaxy: added).
    const char *noDestinationCopy = std::getenv("DQ8_GFX_NO_DESTINATION_COPY");
    if (!m_framebufferFetch && fragmentInfo.format == SDL_GPU_SHADERFORMAT_SPIRV &&
        !(noDestinationCopy && *noDestinationCopy && *noDestinationCopy != '0')) {
        SDL_GPUShaderCreateInfo destinationInfo = fragmentInfo;
        destinationInfo.code = reinterpret_cast<const uint8_t *>(kGsDrawDstCopyFragSpirv);
        destinationInfo.code_size = sizeof(kGsDrawDstCopyFragSpirv);
        destinationInfo.num_samplers = 2u;
        m_destinationShader = SDL_CreateGPUShader(m_device, &destinationInfo);
        if (!m_destinationShader)
            std::fprintf(stderr, "[gfx] no destination-copy shader: %s\n", SDL_GetError());
    }

    // Dual-source blending (Rogue Galaxy: added): SPIR-V only, and only
    // where a pipeline using it can be made (the device's dualSrcBlend); the
    // draws fall back to an alpha replay otherwise, or with
    // DQ8_GFX_NO_DUAL_SOURCE=1.
    const char *noDualSource = std::getenv("DQ8_GFX_NO_DUAL_SOURCE");
    if (!m_framebufferFetch && fragmentInfo.format == SDL_GPU_SHADERFORMAT_SPIRV &&
        !(noDualSource && *noDualSource && *noDualSource != '0')) {
        SDL_GPUShaderCreateInfo dualInfo = fragmentInfo;
        dualInfo.code = reinterpret_cast<const uint8_t *>(kGsDrawDualFragSpirv);
        dualInfo.code_size = sizeof(kGsDrawDualFragSpirv);
        m_dualSourceShader = SDL_CreateGPUShader(m_device, &dualInfo);
        if (m_dualSourceShader) {
            GsPipelineKey probe{};
            probe.colorFormat = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
            probe.blendEnabled = 1u;
            probe.sourceFactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
            probe.destinationFactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
            probe.blendOperation = SDL_GPU_BLENDOP_ADD;
            probe.dualSource = 1u;
            m_dualSourceBlend = true;
            std::string probeError;
            if (!pipeline(probe, probeError)) {
                m_dualSourceBlend = false;
                std::fprintf(stderr, "[gfx] no dual-source blending: %s\n", probeError.c_str());
            }
        }
    }

    SDL_GPUSamplerCreateInfo samplerInfo{};
    samplerInfo.min_filter = SDL_GPU_FILTER_NEAREST;
    samplerInfo.mag_filter = SDL_GPU_FILTER_NEAREST;
    samplerInfo.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    samplerInfo.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    samplerInfo.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    samplerInfo.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    m_sampler = SDL_CreateGPUSampler(m_device, &samplerInfo);
    if (!m_sampler) {
        error = std::string("SDL_CreateGPUSampler: ") + SDL_GetError();
        destroy();
        return false;
    }
    return true;
}

void SdlGpuDevice::destroy() {
    m_framebufferFetch = false;
    if (!m_device)
        return;
    cancelPending();
    SDL_WaitForGPUIdle(m_device);
    for (const Submission &submission : m_inFlight)
        SDL_ReleaseGPUFence(m_device, submission.fence);
    m_inFlight.clear();
    for (auto &[key, pipeline] : m_pipelines) {
        (void)key;
        if (pipeline)
            SDL_ReleaseGPUGraphicsPipeline(m_device, pipeline);
    }
    m_pipelines.clear();
    if (m_reinterpretPipeline)
        SDL_ReleaseGPUGraphicsPipeline(m_device, m_reinterpretPipeline);
    m_reinterpretPipeline = nullptr;
    if (m_localCopyPipeline)
        SDL_ReleaseGPUGraphicsPipeline(m_device, m_localCopyPipeline);
    m_localCopyPipeline = nullptr;
    if (m_displayPipeline)
        SDL_ReleaseGPUGraphicsPipeline(m_device, m_displayPipeline);
    m_displayPipeline = nullptr;
    if (m_sidesPipeline)
        SDL_ReleaseGPUGraphicsPipeline(m_device, m_sidesPipeline);
    m_sidesPipeline = nullptr;
    m_sidesFormat = SDL_GPU_TEXTUREFORMAT_INVALID;
    if (m_linearSampler)
        SDL_ReleaseGPUSampler(m_device, m_linearSampler);
    m_linearSampler = nullptr;
    releaseIndex8();
    releaseLocalCopy();
    if (m_sampler)
        SDL_ReleaseGPUSampler(m_device, m_sampler);
    if (m_fragmentShader)
        SDL_ReleaseGPUShader(m_device, m_fragmentShader);
    if (m_destinationShader)
        SDL_ReleaseGPUShader(m_device, m_destinationShader);
    m_destinationShader = nullptr;
    if (m_dualSourceShader)
        SDL_ReleaseGPUShader(m_device, m_dualSourceShader);
    m_dualSourceShader = nullptr;
    m_dualSourceBlend = false;
    if (m_vertexShader)
        SDL_ReleaseGPUShader(m_device, m_vertexShader);
    if (m_nativeVertexShader)
        SDL_ReleaseGPUShader(m_device, m_nativeVertexShader);
    m_nativeVertexShader = nullptr;
    m_sampler = nullptr;
    m_fragmentShader = nullptr;
    m_vertexShader = nullptr;
    SDL_DestroyGPUDevice(m_device);
    m_device = nullptr;
}

std::string SdlGpuDevice::driverName() const {
    if (!m_device)
        return {};
    const char *name = SDL_GetGPUDeviceDriver(m_device);
    return name ? name : std::string{};
}

bool SdlGpuDevice::reinterpretColor(SDL_GPUTexture *source, SDL_GPUTexture *destination,
                                    uint32_t width, uint32_t height, bool destination16, uint32_t scale,
                                    std::string &error) {
    SDL_GPUCommandBuffer *commands = pendingCommands(error, "reinterpret (format view)");
    if (!commands)
        return false;
    if (!recordReinterpret(commands, source, destination, width, height, destination16, scale, nullptr, error)) {
        cancelPending();
        return false;
    }
    return true;
}

bool SdlGpuDevice::recordReinterpret(SDL_GPUCommandBuffer *commands, SDL_GPUTexture *source,
                                     SDL_GPUTexture *destination, uint32_t width, uint32_t height,
                                     bool destination16, uint32_t scale, const SDL_Rect *scissor,
                                     std::string &error) {
    if (!m_reinterpretPipeline) {
        SDL_GPUShaderCreateInfo vertexInfo{};
        vertexInfo.code = reinterpret_cast<const uint8_t *>(kGsReinterpretVertSpirv);
        vertexInfo.code_size = sizeof(kGsReinterpretVertSpirv);
        vertexInfo.entrypoint = "main";
        vertexInfo.format = SDL_GPU_SHADERFORMAT_SPIRV;
        vertexInfo.stage = SDL_GPU_SHADERSTAGE_VERTEX;
        SDL_GPUShaderCreateInfo fragmentInfo{};
        fragmentInfo.code = reinterpret_cast<const uint8_t *>(kGsReinterpretFragSpirv);
        fragmentInfo.code_size = sizeof(kGsReinterpretFragSpirv);
        fragmentInfo.entrypoint = "main";
        fragmentInfo.format = SDL_GPU_SHADERFORMAT_SPIRV;
        fragmentInfo.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
        fragmentInfo.num_samplers = 1u;
        fragmentInfo.num_uniform_buffers = 1u;
#ifdef DQ8_GFX_HAS_MSL
        if ((SDL_GetGPUShaderFormats(m_device) & SDL_GPU_SHADERFORMAT_MSL) != 0) {
            vertexInfo.code = reinterpret_cast<const uint8_t *>(kGsReinterpretVertSpirvMsl);
            vertexInfo.code_size = sizeof(kGsReinterpretVertSpirvMsl);
            vertexInfo.entrypoint = "main0";
            vertexInfo.format = SDL_GPU_SHADERFORMAT_MSL;
            fragmentInfo.code = reinterpret_cast<const uint8_t *>(kGsReinterpretFragSpirvMsl);
            fragmentInfo.code_size = sizeof(kGsReinterpretFragSpirvMsl);
            fragmentInfo.entrypoint = "main0";
            fragmentInfo.format = SDL_GPU_SHADERFORMAT_MSL;
        }
#endif
        SDL_GPUShader *vertex = SDL_CreateGPUShader(m_device, &vertexInfo);
        if (!vertex) {
            error = std::string("SDL_CreateGPUShader(reinterpret vertex): ") + SDL_GetError();
            return false;
        }
        SDL_GPUShader *fragment = SDL_CreateGPUShader(m_device, &fragmentInfo);
        if (!fragment) {
            error = std::string("SDL_CreateGPUShader(reinterpret fragment): ") + SDL_GetError();
            SDL_ReleaseGPUShader(m_device, vertex);
            return false;
        }
        SDL_GPUColorTargetDescription colorTarget{};
        colorTarget.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        SDL_GPUGraphicsPipelineCreateInfo info{};
        info.vertex_shader = vertex;
        info.fragment_shader = fragment;
        info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
        info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
        info.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
        info.target_info.color_target_descriptions = &colorTarget;
        info.target_info.num_color_targets = 1u;
        m_reinterpretPipeline = SDL_CreateGPUGraphicsPipeline(m_device, &info);
        SDL_ReleaseGPUShader(m_device, fragment);
        SDL_ReleaseGPUShader(m_device, vertex);
        if (!m_reinterpretPipeline) {
            error = std::string("SDL_CreateGPUGraphicsPipeline(reinterpret): ") + SDL_GetError();
            return false;
        }
    }

    SDL_GPUColorTargetInfo target{};
    target.texture = destination;
    target.load_op = scissor ? SDL_GPU_LOADOP_LOAD : SDL_GPU_LOADOP_DONT_CARE;
    target.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(commands, &target, 1u, nullptr);
    if (!pass) {
        error = std::string("SDL_BeginGPURenderPass(reinterpret): ") + SDL_GetError();
        return false;
    }
    SDL_BindGPUGraphicsPipeline(pass, m_reinterpretPipeline);
    scale = std::max(scale, 1u);
    SDL_GPUViewport viewport{0.0f, 0.0f, float(width * scale), float(height * scale), 0.0f, 1.0f};
    SDL_SetGPUViewport(pass, &viewport);
    if (scissor)
        SDL_SetGPUScissor(pass, scissor);
    SDL_GPUTextureSamplerBinding binding{source, m_sampler};
    SDL_BindGPUFragmentSamplers(pass, 0u, &binding, 1u);
    const uint32_t control[4] = {uint32_t(destination16), scale, 0u, 0u};
    SDL_PushGPUFragmentUniformData(commands, 0u, control, sizeof(control));
    SDL_DrawGPUPrimitives(pass, 3u, 1u, 0u, 0u);
    SDL_EndGPURenderPass(pass);
    return true;
}

bool SdlGpuDevice::localCopy(SDL_GPUTexture *source, SDL_GPUTexture *destination, uint32_t width,
                             uint32_t height, uint32_t scale, const SDL_Rect &box,
                             const std::vector<uint32_t> &mapping, std::string &error) {
    if (box.w <= 0 || box.h <= 0 || mapping.size() != size_t(box.w) * size_t(box.h) * 4u) {
        error = "localCopy: mapping does not match its box";
        return false;
    }
    if (!m_localCopyPipeline) {
        SDL_GPUShaderCreateInfo vertexInfo{};
        vertexInfo.code = reinterpret_cast<const uint8_t *>(kGsReinterpretVertSpirv);
        vertexInfo.code_size = sizeof(kGsReinterpretVertSpirv);
        vertexInfo.entrypoint = "main";
        vertexInfo.format = SDL_GPU_SHADERFORMAT_SPIRV;
        vertexInfo.stage = SDL_GPU_SHADERSTAGE_VERTEX;
        SDL_GPUShaderCreateInfo fragmentInfo{};
        fragmentInfo.code = reinterpret_cast<const uint8_t *>(kGsLocalCopyFragSpirv);
        fragmentInfo.code_size = sizeof(kGsLocalCopyFragSpirv);
        fragmentInfo.entrypoint = "main";
        fragmentInfo.format = SDL_GPU_SHADERFORMAT_SPIRV;
        fragmentInfo.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
        fragmentInfo.num_samplers = 3u;
        fragmentInfo.num_uniform_buffers = 1u;
#ifdef DQ8_GFX_HAS_MSL
        if ((SDL_GetGPUShaderFormats(m_device) & SDL_GPU_SHADERFORMAT_MSL) != 0) {
            vertexInfo.code = reinterpret_cast<const uint8_t *>(kGsReinterpretVertSpirvMsl);
            vertexInfo.code_size = sizeof(kGsReinterpretVertSpirvMsl);
            vertexInfo.entrypoint = "main0";
            vertexInfo.format = SDL_GPU_SHADERFORMAT_MSL;
            fragmentInfo.code = reinterpret_cast<const uint8_t *>(kGsLocalCopyFragSpirvMsl);
            fragmentInfo.code_size = sizeof(kGsLocalCopyFragSpirvMsl);
            fragmentInfo.entrypoint = "main0";
            fragmentInfo.format = SDL_GPU_SHADERFORMAT_MSL;
        }
#endif
        SDL_GPUShader *vertex = SDL_CreateGPUShader(m_device, &vertexInfo);
        if (!vertex) {
            error = std::string("SDL_CreateGPUShader(local copy vertex): ") + SDL_GetError();
            return false;
        }
        SDL_GPUShader *fragment = SDL_CreateGPUShader(m_device, &fragmentInfo);
        if (!fragment) {
            error = std::string("SDL_CreateGPUShader(local copy fragment): ") + SDL_GetError();
            SDL_ReleaseGPUShader(m_device, vertex);
            return false;
        }
        SDL_GPUColorTargetDescription colorTarget{};
        colorTarget.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        SDL_GPUGraphicsPipelineCreateInfo info{};
        info.vertex_shader = vertex;
        info.fragment_shader = fragment;
        info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
        info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
        info.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
        info.target_info.color_target_descriptions = &colorTarget;
        info.target_info.num_color_targets = 1u;
        m_localCopyPipeline = SDL_CreateGPUGraphicsPipeline(m_device, &info);
        SDL_ReleaseGPUShader(m_device, fragment);
        SDL_ReleaseGPUShader(m_device, vertex);
        if (!m_localCopyPipeline) {
            error = std::string("SDL_CreateGPUGraphicsPipeline(local copy): ") + SDL_GetError();
            return false;
        }
    }

    scale = std::max(scale, 1u);
    if (!reserveLocalCopy(width * scale, height * scale, static_cast<uint32_t>(box.w),
                          static_cast<uint32_t>(box.h), error))
        return false;
    const auto fail = [&](const char *what) {
        error = std::string(what) + ": " + SDL_GetError();
        return false;
    };
    // Cycled: a copy the GPU has not finished keeps the old contents. Each
    // copy recorded ahead of a submission holds its own snapshot and
    // mapping, so their size is reserved against the pending budget.
    if (!reservePending(uint64_t(width) * scale * height * scale * 4u + mapping.size() * sizeof(uint32_t) * 2u,
                        error))
        return false;
    void *mapped = SDL_MapGPUTransferBuffer(m_device, m_localCopyUpload, true);
    if (!mapped)
        return fail("SDL_MapGPUTransferBuffer(local copy)");
    std::memcpy(mapped, mapping.data(), mapping.size() * sizeof(uint32_t));
    SDL_UnmapGPUTransferBuffer(m_device, m_localCopyUpload);

    SDL_GPUCommandBuffer *commands = pendingCommands(error, "local copy");
    if (!commands)
        return false;
    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(commands);
    if (!copy) {
        cancelPending();
        return fail("SDL_BeginGPUCopyPass(local copy)");
    }
    SDL_GPUTextureLocation from{};
    from.texture = destination;
    SDL_GPUTextureLocation to{};
    to.texture = m_localCopySnapshot;
    SDL_CopyGPUTextureToTexture(copy, &from, &to, width * scale, height * scale, 1u, true);
    const SDL_GPUTextureTransferInfo mappingFrom{m_localCopyUpload, 0u, static_cast<uint32_t>(box.w),
                                                 static_cast<uint32_t>(box.h)};
    SDL_GPUTextureRegion mappingTo{};
    mappingTo.texture = m_localCopyMapping;
    mappingTo.w = static_cast<uint32_t>(box.w);
    mappingTo.h = static_cast<uint32_t>(box.h);
    mappingTo.d = 1u;
    SDL_UploadToGPUTexture(copy, &mappingFrom, &mappingTo, true);
    SDL_EndGPUCopyPass(copy);

    SDL_GPUColorTargetInfo target{};
    target.texture = destination;
    target.load_op = SDL_GPU_LOADOP_LOAD;
    target.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(commands, &target, 1u, nullptr);
    if (!pass) {
        cancelPending();
        return fail("SDL_BeginGPURenderPass(local copy)");
    }
    SDL_BindGPUGraphicsPipeline(pass, m_localCopyPipeline);
    const SDL_GPUViewport viewport{float(box.x * int(scale)), float(box.y * int(scale)),
                                   float(box.w * int(scale)), float(box.h * int(scale)), 0.0f, 1.0f};
    SDL_SetGPUViewport(pass, &viewport);
    const SDL_Rect scissor{box.x * int(scale), box.y * int(scale), box.w * int(scale), box.h * int(scale)};
    SDL_SetGPUScissor(pass, &scissor);
    const SDL_GPUTextureSamplerBinding bindings[3] = {
        {source == destination ? m_localCopySnapshot : source, m_sampler},
        {m_localCopySnapshot, m_sampler},
        {m_localCopyMapping, m_sampler},
    };
    SDL_BindGPUFragmentSamplers(pass, 0u, bindings, 3u);
    const uint32_t control[4] = {scale, static_cast<uint32_t>(box.x), static_cast<uint32_t>(box.y), 0u};
    SDL_PushGPUFragmentUniformData(commands, 0u, control, sizeof(control));
    SDL_DrawGPUPrimitives(pass, 3u, 1u, 0u, 0u);
    SDL_EndGPURenderPass(pass);
    return true;
}

bool SdlGpuDevice::reserveLocalCopy(uint32_t snapshotWidth, uint32_t snapshotHeight, uint32_t mappingWidth,
                                    uint32_t mappingHeight, std::string &error) {
    // Larger than a copy needs is fine: the shader fetches the snapshot at the
    // destination's own coordinates and the mapping at box-relative ones.
    if (m_localCopySnapshot &&
        (snapshotWidth > m_localCopySnapshotWidth || snapshotHeight > m_localCopySnapshotHeight)) {
        SDL_ReleaseGPUTexture(m_device, m_localCopySnapshot);
        m_localCopySnapshot = nullptr;
    }
    if (!m_localCopySnapshot) {
        m_localCopySnapshotWidth = std::max(snapshotWidth, m_localCopySnapshotWidth);
        m_localCopySnapshotHeight = std::max(snapshotHeight, m_localCopySnapshotHeight);
        SDL_GPUTextureCreateInfo info{};
        info.type = SDL_GPU_TEXTURETYPE_2D;
        info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        info.width = m_localCopySnapshotWidth;
        info.height = m_localCopySnapshotHeight;
        info.layer_count_or_depth = 1u;
        info.num_levels = 1u;
        info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
        m_localCopySnapshot = SDL_CreateGPUTexture(m_device, &info);
        if (!m_localCopySnapshot) {
            error = std::string("SDL_CreateGPUTexture(local copy snapshot): ") + SDL_GetError();
            return false;
        }
    }
    if (m_localCopyMapping &&
        (mappingWidth > m_localCopyMappingWidth || mappingHeight > m_localCopyMappingHeight)) {
        SDL_ReleaseGPUTexture(m_device, m_localCopyMapping);
        SDL_ReleaseGPUTransferBuffer(m_device, m_localCopyUpload);
        m_localCopyMapping = nullptr;
        m_localCopyUpload = nullptr;
    }
    if (!m_localCopyMapping) {
        m_localCopyMappingWidth = std::max(mappingWidth, m_localCopyMappingWidth);
        m_localCopyMappingHeight = std::max(mappingHeight, m_localCopyMappingHeight);
        SDL_GPUTextureCreateInfo info{};
        info.type = SDL_GPU_TEXTURETYPE_2D;
        info.format = SDL_GPU_TEXTUREFORMAT_R32G32B32A32_UINT;
        info.width = m_localCopyMappingWidth;
        info.height = m_localCopyMappingHeight;
        info.layer_count_or_depth = 1u;
        info.num_levels = 1u;
        info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
        m_localCopyMapping = SDL_CreateGPUTexture(m_device, &info);
        SDL_GPUTransferBufferCreateInfo upload{};
        upload.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        upload.size = m_localCopyMappingWidth * m_localCopyMappingHeight * 4u * sizeof(uint32_t);
        m_localCopyUpload = SDL_CreateGPUTransferBuffer(m_device, &upload);
        if (!m_localCopyMapping || !m_localCopyUpload) {
            error = std::string("SDL_CreateGPU*(local copy mapping): ") + SDL_GetError();
            releaseLocalCopy();
            return false;
        }
    }
    return true;
}

void SdlGpuDevice::releaseLocalCopy() {
    if (m_localCopySnapshot)
        SDL_ReleaseGPUTexture(m_device, m_localCopySnapshot);
    if (m_localCopyMapping)
        SDL_ReleaseGPUTexture(m_device, m_localCopyMapping);
    if (m_localCopyUpload)
        SDL_ReleaseGPUTransferBuffer(m_device, m_localCopyUpload);
    m_localCopySnapshot = m_localCopyMapping = nullptr;
    m_localCopyUpload = nullptr;
    m_localCopySnapshotWidth = m_localCopySnapshotHeight = 0u;
    m_localCopyMappingWidth = m_localCopyMappingHeight = 0u;
}

bool SdlGpuDevice::createIndex8(std::string &error) {
    SDL_GPUShaderCreateInfo vertexInfo{};
    vertexInfo.code = reinterpret_cast<const uint8_t *>(kGsReinterpretVertSpirv);
    vertexInfo.code_size = sizeof(kGsReinterpretVertSpirv);
    vertexInfo.entrypoint = "main";
    vertexInfo.format = SDL_GPU_SHADERFORMAT_SPIRV;
    vertexInfo.stage = SDL_GPU_SHADERSTAGE_VERTEX;
    SDL_GPUShaderCreateInfo fragmentInfo{};
    fragmentInfo.code = reinterpret_cast<const uint8_t *>(kGsIndex8FragSpirv);
    fragmentInfo.code_size = sizeof(kGsIndex8FragSpirv);
    fragmentInfo.entrypoint = "main";
    fragmentInfo.format = SDL_GPU_SHADERFORMAT_SPIRV;
    fragmentInfo.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
    fragmentInfo.num_samplers = 3u;
    fragmentInfo.num_uniform_buffers = 1u;
#ifdef DQ8_GFX_HAS_MSL
    if ((SDL_GetGPUShaderFormats(m_device) & SDL_GPU_SHADERFORMAT_MSL) != 0) {
        vertexInfo.code = reinterpret_cast<const uint8_t *>(kGsReinterpretVertSpirvMsl);
        vertexInfo.code_size = sizeof(kGsReinterpretVertSpirvMsl);
        vertexInfo.entrypoint = "main0";
        vertexInfo.format = SDL_GPU_SHADERFORMAT_MSL;
        fragmentInfo.code = reinterpret_cast<const uint8_t *>(kGsIndex8FragSpirvMsl);
        fragmentInfo.code_size = sizeof(kGsIndex8FragSpirvMsl);
        fragmentInfo.entrypoint = "main0";
        fragmentInfo.format = SDL_GPU_SHADERFORMAT_MSL;
    }
#endif
    SDL_GPUShader *vertex = SDL_CreateGPUShader(m_device, &vertexInfo);
    if (!vertex) {
        error = std::string("SDL_CreateGPUShader(index8 vertex): ") + SDL_GetError();
        return false;
    }
    SDL_GPUShader *fragment = SDL_CreateGPUShader(m_device, &fragmentInfo);
    if (!fragment) {
        error = std::string("SDL_CreateGPUShader(index8 fragment): ") + SDL_GetError();
        SDL_ReleaseGPUShader(m_device, vertex);
        return false;
    }
    SDL_GPUColorTargetDescription colorTarget{};
    colorTarget.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    SDL_GPUGraphicsPipelineCreateInfo info{};
    info.vertex_shader = vertex;
    info.fragment_shader = fragment;
    info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    info.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
    info.target_info.color_target_descriptions = &colorTarget;
    info.target_info.num_color_targets = 1u;
    m_index8Pipeline = SDL_CreateGPUGraphicsPipeline(m_device, &info);
    SDL_ReleaseGPUShader(m_device, fragment);
    SDL_ReleaseGPUShader(m_device, vertex);
    // Failing part-way leaves nothing behind, so the next texture tries again.
    const auto fail = [&](const char *what) {
        error = std::string(what) + ": " + SDL_GetError();
        releaseIndex8();
        return false;
    };
    if (!m_index8Pipeline)
        return fail("SDL_CreateGPUGraphicsPipeline(index8)");

    SDL_GPUTextureCreateInfo texture{};
    texture.type = SDL_GPU_TEXTURETYPE_2D;
    texture.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    texture.layer_count_or_depth = 1u;
    texture.num_levels = 1u;
    texture.sample_count = SDL_GPU_SAMPLECOUNT_1;
    texture.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    texture.width = 256u;
    texture.height = 1u;
    m_index8Palette = SDL_CreateGPUTexture(m_device, &texture);
    texture.width = 128u;
    texture.height = 64u;
    m_index8Placement = SDL_CreateGPUTexture(m_device, &texture);
    SDL_GPUTransferBufferCreateInfo buffer{};
    buffer.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    buffer.size = 128u * 64u * 4u;
    m_index8Upload = SDL_CreateGPUTransferBuffer(m_device, &buffer);
    if (!m_index8Palette || !m_index8Placement || !m_index8Upload)
        return fail("SDL_CreateGPU*(index8 resources)");

    // Where each byte of a PSMT8 page lives in the same page read as CT32.
    std::array<uint16_t, 2048> wordPixel{};
    for (uint32_t y = 0u; y < 32u; ++y)
        for (uint32_t x = 0u; x < 64u; ++x)
            wordPixel[GSPSMCT32::addrPSMCT32(0u, 1u, x, y) >> 2u] = static_cast<uint16_t>(x | (y << 8u));
    auto *mapped = static_cast<uint8_t *>(SDL_MapGPUTransferBuffer(m_device, m_index8Upload, false));
    if (!mapped)
        return fail("SDL_MapGPUTransferBuffer(index8 placement)");
    for (uint32_t y = 0u; y < 64u; ++y) {
        for (uint32_t x = 0u; x < 128u; ++x) {
            const uint32_t address = GSPSMT8::addrPSMT8(0u, 2u, x, y);
            const uint16_t pixel = wordPixel[address >> 2u];
            uint8_t *out = mapped + (y * 128u + x) * 4u;
            out[0] = static_cast<uint8_t>(pixel & 0xffu);
            out[1] = static_cast<uint8_t>(pixel >> 8u);
            out[2] = static_cast<uint8_t>(address & 3u);
            out[3] = 0u;
        }
    }
    SDL_UnmapGPUTransferBuffer(m_device, m_index8Upload);
    SDL_GPUCommandBuffer *commands = pendingCommands(error);
    if (!commands)
        return false;
    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(commands);
    if (!copy) {
        cancelPending();
        return fail("SDL_BeginGPUCopyPass(index8 placement)");
    }
    const SDL_GPUTextureTransferInfo from{m_index8Upload, 0u, 128u, 64u};
    SDL_GPUTextureRegion to{};
    to.texture = m_index8Placement;
    to.w = 128u;
    to.h = 64u;
    to.d = 1u;
    SDL_UploadToGPUTexture(copy, &from, &to, false);
    SDL_EndGPUCopyPass(copy);
    return true;
}

void SdlGpuDevice::releaseIndex8() {
    if (m_index8Pipeline)
        SDL_ReleaseGPUGraphicsPipeline(m_device, m_index8Pipeline);
    if (m_index8Palette)
        SDL_ReleaseGPUTexture(m_device, m_index8Palette);
    if (m_index8Placement)
        SDL_ReleaseGPUTexture(m_device, m_index8Placement);
    if (m_index8Upload)
        SDL_ReleaseGPUTransferBuffer(m_device, m_index8Upload);
    m_index8Pipeline = nullptr;
    m_index8Palette = m_index8Placement = nullptr;
    m_index8Upload = nullptr;
}

bool SdlGpuDevice::expandIndexed8(SDL_GPUTexture *target, const uint32_t *palette,
                                  SDL_GPUTexture *destination, uint32_t width, uint32_t height,
                                  const SDL_Rect &region, const std::array<uint32_t, 4> &pages,
                                  std::string &error) {
    if (!m_index8Pipeline && !createIndex8(error))
        return false;

    // The upload buffer cycles, so this palette cannot overwrite one still in flight.
    void *mapped = SDL_MapGPUTransferBuffer(m_device, m_index8Upload, true);
    if (!mapped) {
        error = std::string("SDL_MapGPUTransferBuffer(index8 palette): ") + SDL_GetError();
        return false;
    }
    std::memcpy(mapped, palette, 256u * 4u);
    SDL_UnmapGPUTransferBuffer(m_device, m_index8Upload);

    SDL_GPUCommandBuffer *commands = pendingCommands(error, "expand indexed texture");
    if (!commands)
        return false;
    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(commands);
    if (!copy) {
        error = std::string("SDL_BeginGPUCopyPass(index8 palette): ") + SDL_GetError();
        cancelPending();
        return false;
    }
    const SDL_GPUTextureTransferInfo from{m_index8Upload, 0u, 256u, 1u};
    SDL_GPUTextureRegion to{};
    to.texture = m_index8Palette;
    to.w = 256u;
    to.h = 1u;
    to.d = 1u;
    SDL_UploadToGPUTexture(copy, &from, &to, true);
    SDL_EndGPUCopyPass(copy);

    const bool whole = region.x == 0 && region.y == 0 && uint32_t(region.w) >= width && uint32_t(region.h) >= height;
    SDL_GPUColorTargetInfo colorTarget{};
    colorTarget.texture = destination;
    colorTarget.load_op = whole ? SDL_GPU_LOADOP_DONT_CARE : SDL_GPU_LOADOP_LOAD;
    colorTarget.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(commands, &colorTarget, 1u, nullptr);
    if (!pass) {
        error = std::string("SDL_BeginGPURenderPass(index8): ") + SDL_GetError();
        cancelPending();
        return false;
    }
    SDL_BindGPUGraphicsPipeline(pass, m_index8Pipeline);
    const SDL_GPUViewport viewport{0.0f, 0.0f, float(width), float(height), 0.0f, 1.0f};
    SDL_SetGPUViewport(pass, &viewport);
    SDL_SetGPUScissor(pass, &region);
    const SDL_GPUTextureSamplerBinding bindings[3] = {
        {target, m_sampler}, {m_index8Palette, m_sampler}, {m_index8Placement, m_sampler}};
    SDL_BindGPUFragmentSamplers(pass, 0u, bindings, 3u);
    SDL_PushGPUFragmentUniformData(commands, 0u, pages.data(), sizeof(uint32_t) * pages.size());
    SDL_DrawGPUPrimitives(pass, 3u, 1u, 0u, 0u);
    SDL_EndGPURenderPass(pass);
    return true;
}

SDL_GPUGraphicsPipeline *SdlGpuDevice::pipeline(const GsPipelineKey &key, std::string &error) {
    if (!m_device)
        return nullptr;
    if (auto found = m_pipelines.find(key); found != m_pipelines.end())
        return found->second;

    static constexpr std::array<SDL_GPUVertexAttribute, 5> kAttributes{{
        {0u, 0u, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(GsGpuVertex, x)},
        {1u, 0u, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(GsGpuVertex, z)},
        {2u, 0u, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(GsGpuVertex, s)},
        {3u, 0u, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, offsetof(GsGpuVertex, r)},
        {4u, 0u, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT, offsetof(GsGpuVertex, fog)},
    }};
    const SDL_GPUVertexBufferDescription bufferDescription{
        0u, sizeof(GsGpuVertex), SDL_GPU_VERTEXINPUTRATE_VERTEX, 0u};

    SDL_GPUColorTargetBlendState blend{};
    blend.enable_blend = key.blendEnabled != 0u;
    blend.src_color_blendfactor = static_cast<SDL_GPUBlendFactor>(key.sourceFactor);
    blend.dst_color_blendfactor = static_cast<SDL_GPUBlendFactor>(key.destinationFactor);
    // The source alpha a dual-source draw blends by is its second output's.
    if (key.dualSource) {
        auto second = [](SDL_GPUBlendFactor factor) {
            return factor == SDL_GPU_BLENDFACTOR_SRC_ALPHA             ? SDL_GPU_BLENDFACTOR_SRC1_ALPHA
                   : factor == SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA ? SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC1_ALPHA
                                                                       : factor;
        };
        blend.src_color_blendfactor = second(blend.src_color_blendfactor);
        blend.dst_color_blendfactor = second(blend.dst_color_blendfactor);
    }
    blend.color_blend_op = static_cast<SDL_GPUBlendOp>(key.blendOperation);
    // GS ALPHA blends RGB only; the source alpha is stored unchanged.
    blend.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    blend.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
    blend.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
    blend.color_write_mask = key.colorWriteMask;
    blend.enable_color_write_mask = true;

    SDL_GPUColorTargetDescription colorTarget{};
    colorTarget.format = static_cast<SDL_GPUTextureFormat>(key.colorFormat);
    colorTarget.blend_state = blend;

    SDL_GPUGraphicsPipelineCreateInfo info{};
    info.vertex_shader = key.nativeMesh ? m_nativeVertexShader : m_vertexShader;
    info.fragment_shader = key.destinationCopy ? m_destinationShader
                           : key.dualSource            ? m_dualSourceShader
                                                       : m_fragmentShader;
    info.vertex_input_state.vertex_buffer_descriptions = &bufferDescription;
    info.vertex_input_state.num_vertex_buffers = 1u;
    info.vertex_input_state.vertex_attributes = kAttributes.data();
    info.vertex_input_state.num_vertex_attributes =
        static_cast<uint32_t>(kAttributes.size());
    // Native strips (Rogue Galaxy: added) come as indexed triangle lists, and
    // cull the backfaces their program skipped.
    info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    // The GS has no backface culling; every primitive it is given is drawn.
    info.rasterizer_state.cull_mode = static_cast<SDL_GPUCullMode>(key.cullMode);
    info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    info.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
    info.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
    info.depth_stencil_state.compare_op = static_cast<SDL_GPUCompareOp>(key.depthCompare);
    info.depth_stencil_state.enable_depth_test = key.hasDepth != 0u;
    info.depth_stencil_state.enable_depth_write = key.hasDepth != 0u && key.depthWrite != 0u;
    info.target_info.color_target_descriptions = &colorTarget;
    info.target_info.num_color_targets = 1u;
    info.target_info.has_depth_stencil_target = key.hasDepth != 0u;
    info.target_info.depth_stencil_format = static_cast<SDL_GPUTextureFormat>(key.depthFormat);

    const auto started = std::chrono::steady_clock::now();
    SDL_GPUGraphicsPipeline *pipeline = SDL_CreateGPUGraphicsPipeline(m_device, &info);
    m_pipelineCreateNanos += static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started).count());
    if (!pipeline) {
        error = std::string("SDL_CreateGPUGraphicsPipeline: ") + SDL_GetError();
        return nullptr;
    }
    m_pipelines.emplace(key, pipeline);
    return pipeline;
}

bool SdlGpuDevice::downloadTexture(SDL_GPUTexture *texture, uint32_t width, uint32_t height,
                                    std::vector<uint8_t> &out, std::string &error) const {
    SDL_GPUDevice *device = m_device;
    if (!device || !texture) {
        error = "render target has no texture to download";
        return false;
    }
    const size_t bytes = static_cast<size_t>(width) * height * 4u;
    out.resize(bytes);

    SDL_GPUTransferBufferCreateInfo transferInfo{};
    transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    transferInfo.size = static_cast<uint32_t>(bytes);
    SDL_GPUTransferBuffer *download = SDL_CreateGPUTransferBuffer(device, &transferInfo);
    if (!download) {
        error = std::string("SDL_CreateGPUTransferBuffer(download): ") + SDL_GetError();
        return false;
    }

    SDL_GPUCommandBuffer *commands = SDL_AcquireGPUCommandBuffer(device);
    if (!commands) {
        error = std::string("SDL_AcquireGPUCommandBuffer(download): ") + SDL_GetError();
        SDL_ReleaseGPUTransferBuffer(device, download);
        return false;
    }
    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(commands);
    if (!copy) {
        error = std::string("SDL_BeginGPUCopyPass(download): ") + SDL_GetError();
        SDL_CancelGPUCommandBuffer(commands);
        SDL_ReleaseGPUTransferBuffer(device, download);
        return false;
    }
    SDL_GPUTextureRegion region{};
    region.texture = texture;
    region.w = width;
    region.h = height;
    region.d = 1u;
    SDL_GPUTextureTransferInfo destination{};
    destination.transfer_buffer = download;
    destination.pixels_per_row = width;
    destination.rows_per_layer = height;
    SDL_DownloadFromGPUTexture(copy, &region, &destination);
    SDL_EndGPUCopyPass(copy);

    SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(commands);
    if (!fence) {
        error = std::string("SDL_SubmitGPUCommandBufferAndAcquireFence(download): ") + SDL_GetError();
        SDL_ReleaseGPUTransferBuffer(device, download);
        return false;
    }
    SDL_GPUFence *fences[] = {fence};
    const bool waited = SDL_WaitForGPUFences(device, true, fences, 1u);
    SDL_ReleaseGPUFence(device, fence);
    if (!waited) {
        error = std::string("SDL_WaitForGPUFences(download): ") + SDL_GetError();
        SDL_ReleaseGPUTransferBuffer(device, download);
        return false;
    }
    void *mapped = SDL_MapGPUTransferBuffer(device, download, false);
    if (!mapped) {
        error = std::string("SDL_MapGPUTransferBuffer(download): ") + SDL_GetError();
        SDL_ReleaseGPUTransferBuffer(device, download);
        return false;
    }
    std::memcpy(out.data(), mapped, bytes);
    SDL_UnmapGPUTransferBuffer(device, download);
    SDL_ReleaseGPUTransferBuffer(device, download);
    return true;
}

SDL_GPUCommandBuffer *SdlGpuDevice::pendingCommands(std::string &error, const char *what) {
    if (!m_pending) {
        m_pending = SDL_AcquireGPUCommandBuffer(m_device);
        if (!m_pending)
            error = std::string("SDL_AcquireGPUCommandBuffer(pending): ") + SDL_GetError();
        else if (m_gpuTimes)
            SDL_InsertGPUDebugLabel(m_pending, "other");
    }
    if (m_pending && m_gpuTimes && what)
        SDL_InsertGPUDebugLabel(m_pending, what);
    return m_pending;
}

bool SdlGpuDevice::submitPending(std::string &error) {
    if (!m_pending)
        return true;
    SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(m_pending);
    m_pending = nullptr;
    m_pendingBytes = 0u;
    const uint64_t serial = m_nextSerial++;
    ++m_submissions;
    if (!fence) {
        error = std::string("SDL_SubmitGPUCommandBufferAndAcquireFence(pending): ") + SDL_GetError();
        return false;
    }
    m_inFlight.push_back(Submission{serial, fence});
    completedSerial();
    return true;
}

bool SdlGpuDevice::submitPendingAndWait(std::string &error) {
    if (!m_pending)
        return true;
    const uint64_t serial = m_nextSerial;
    if (!submitPending(error))
        return false;
    for (auto it = m_inFlight.begin(); it != m_inFlight.end(); ++it) {
        if (it->serial != serial)
            continue;
        SDL_GPUFence *fences[] = {it->fence};
        const bool waited = SDL_WaitForGPUFences(m_device, true, fences, 1u);
        if (!waited)
            error = std::string("SDL_WaitForGPUFences(pending): ") + SDL_GetError();
        SDL_ReleaseGPUFence(m_device, it->fence);
        m_inFlight.erase(it);
        return waited;
    }
    return true;
}

void SdlGpuDevice::cancelPending() {
    if (!m_pending)
        return;
    SDL_CancelGPUCommandBuffer(m_pending);
    m_pending = nullptr;
    m_pendingBytes = 0u;
    // Its serial is spent: whatever was marked used in it is free again.
    ++m_nextSerial;
}

uint64_t SdlGpuDevice::completedSerial() {
    while (!m_inFlight.empty() && SDL_QueryGPUFence(m_device, m_inFlight.front().fence)) {
        SDL_ReleaseGPUFence(m_device, m_inFlight.front().fence);
        m_inFlight.pop_front();
    }
    return m_inFlight.empty() ? m_nextSerial - 1u : m_inFlight.front().serial - 1u;
}

bool SdlGpuDevice::reservePending(uint64_t bytes, std::string &error) {
    constexpr uint64_t kBudget = 96u * 1024u * 1024u;
    if (m_pending && m_pendingBytes + bytes > kBudget && !submitPending(error))
        return false;
    m_pendingBytes += bytes;
    return true;
}

SDL_GPUTransferBuffer *SdlGpuUploadPool::take(SdlGpuDevice &device, size_t bytes, std::string &error) {
    const uint64_t completed = device.completedSerial();
    Slot *best = nullptr;
    for (Slot &slot : m_slots) {
        if (slot.serial != 0u && slot.serial <= completed)
            slot.serial = 0u;
        if (slot.serial == 0u && slot.bytes >= bytes && (!best || slot.bytes < best->bytes))
            best = &slot;
    }
    if (best)
        return best->buffer;
    size_t capacity = 64u * 1024u;
    while (capacity < bytes)
        capacity *= 2u;
    SDL_GPUTransferBufferCreateInfo info{};
    info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    info.size = static_cast<uint32_t>(capacity);
    SDL_GPUTransferBuffer *buffer = SDL_CreateGPUTransferBuffer(device.handle(), &info);
    if (!buffer) {
        error = std::string("SDL_CreateGPUTransferBuffer(upload pool): ") + SDL_GetError();
        return nullptr;
    }
    m_slots.push_back(Slot{buffer, 0u, capacity});
    return buffer;
}

void SdlGpuUploadPool::used(SDL_GPUTransferBuffer *buffer, uint64_t serial) {
    for (Slot &slot : m_slots) {
        if (slot.buffer == buffer) {
            slot.serial = serial;
            return;
        }
    }
}

void SdlGpuUploadPool::release(SdlGpuDevice &device) {
    if (device.valid()) {
        for (Slot &slot : m_slots)
            SDL_ReleaseGPUTransferBuffer(device.handle(), slot.buffer);
    }
    m_slots.clear();
}

} // namespace dq8::gfx
