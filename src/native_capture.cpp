#include "native_capture.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "native_producer.h"
#include "ps2_runtime.h"
#include "return_hook.h"
#include "scene_timing.h"

namespace rg::nativecapture
{
namespace
{
constexpr uint32_t kTexBuildUploadPkt = 0x00158610u;
// DrawEnvSetFrameFromDesc_144fe0(env, desc, flags).
constexpr uint32_t kSetFrameFromDesc = 0x00144FE0u;
// The getters that fill the frame's and the display buffer's descriptors.
constexpr uint32_t kGetFrameTexDesc = 0x00144CC0u;
constexpr uint32_t kGetDispFbTexDesc = 0x00144A00u;

// The kinds rg::nativeproducer::target takes.
constexpr int kAuto = 0;
constexpr int kFrame = 1;
constexpr int kDisplay = 2;

uint8_t *g_rdram = nullptr;
PS2Runtime::RecompiledFunction g_texBuildUploadOriginal = nullptr;
PS2Runtime::RecompiledFunction g_setFrameFromDescOriginal = nullptr;
PS2Runtime::RecompiledFunction g_getFrameTexDescOriginal = nullptr;
PS2Runtime::RecompiledFunction g_getDispFbTexDescOriginal = nullptr;

uint32_t physical(uint32_t address)
{
    return address & (PS2_RAM_SIZE - 1u);
}

template <typename T> T readAt(uint32_t address)
{
    T value{};
    std::memcpy(&value, g_rdram + physical(address), sizeof(T));
    return value;
}

// A descriptor's TEX0 (+0x38: TBP0 in 14 bits, TBW in 6, PSM in 6), with the
// width and height given, as a target of `kind`.
void declare(uint64_t tex0, int32_t width, int32_t height, int kind)
{
    const uint32_t base = static_cast<uint32_t>(tex0 & 0x3FFFu) & ~31u;
    const uint32_t bufferWidth = static_cast<uint32_t>((tex0 >> 14) & 0x3Fu);
    const uint32_t psm = static_cast<uint32_t>((tex0 >> 20) & 0x3Fu);
    rg::nativeproducer::target(base, bufferWidth, width > 0 ? static_cast<uint32_t>(width) : bufferWidth * 64u,
                               height > 0 ? static_cast<uint32_t>(height) : 0u, psm, kind);
}

// DrawEnvSetFrameFromDesc_144fe0(env, desc, flags): when desc is set, the
// draw environment's FRAME becomes the descriptor's TEX0 with its height
// (+4, s16). Every offscreen buffer the effects draw into (CBlur's, the
// bloom's, the glare's, the shadow map, the per-object composites, the
// minimap, the mob preview) comes through here.
void setFrameFromDesc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t desc = getRegU32(ctx, 5);
    if (desc != 0u)
        declare(readAt<uint64_t>(desc + 0x38u), 0, readAt<int16_t>(desc + 4u), kAuto);
    g_setFrameFromDescOriginal(rdram, ctx, runtime);
}

// The descriptor a getter filled (its node: w at +2, h at +4, TEX0 at
// +0x38), declared as the frame or the display buffer.
void declareScreen(uint32_t node, int kind)
{
    if (node != 0u)
        declare(readAt<uint64_t>(node + 0x38u), readAt<int16_t>(node + 2u), readAt<int16_t>(node + 4u), kind);
}

void afterGetFrameTexDesc(uint8_t *, R5900Context *, PS2Runtime *, uint32_t node)
{
    declareScreen(node, kFrame);
}

void afterGetDispFbTexDesc(uint8_t *, R5900Context *, PS2Runtime *, uint32_t node)
{
    declareScreen(node, kDisplay);
}

void getFrameTexDesc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    rg::hooks::callThen(g_getFrameTexDescOriginal, rdram, ctx, runtime, &afterGetFrameTexDesc, getRegU32(ctx, 5));
}

void getDispFbTexDesc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    rg::hooks::callThen(g_getDispFbTexDescOriginal, rdram, ctx, runtime, &afterGetDispFbTexDesc, getRegU32(ctx, 5));
}

// TexBuildUploadPkt_158610(obj, dst, tex, mode) writes, at dst, the uploads
// of one image node: its palettes (count of them, `stride` apart, to
// `base + i*n` blocks; 16x16 for 8-bit, 8x2 for 4-bit), then each mip
// level. 8-bit art with the node's flag (+0x36) set goes as CT32 at half
// the width and height, pre-swizzled. Each palette and level is handed to
// the producer from where the node keeps it. The arithmetic is the
// function's own.
void texBuildUpload(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t obj = getRegU32(ctx, 4);
    const uint32_t dst = getRegU32(ctx, 5);
    const uint32_t tex = getRegU32(ctx, 6);
    const uint32_t mode = getRegU32(ctx, 7);
    if (dst != 0u && tex != 0u)
    {
        const int32_t nodeBpp = readAt<int16_t>(tex + 6u);
        int32_t w = readAt<int16_t>(tex + 2u);
        int32_t h = readAt<int16_t>(tex + 4u);
        const int16_t flag = readAt<int16_t>(tex + 0x36u);
        const uint64_t tex0 = readAt<uint64_t>(tex + 0x38u);
        const uint32_t count = readAt<uint8_t>(tex + 0x32u);
        const uint32_t base = static_cast<uint16_t>(readAt<int16_t>(tex + 0x34u));
        const uint32_t clut = readAt<uint32_t>(tex + 0x60u);
        const uint32_t fn = readAt<uint32_t>(obj + 0x1F4u);
        uint32_t tbp = static_cast<uint32_t>(tex0 & 0x3FFFu);
        uint32_t tbw = static_cast<uint32_t>((tex0 >> 14) & 0x3Fu);
        uint32_t psm = static_cast<uint32_t>((tex0 >> 20) & 0x3Fu);
        const uint32_t cpsm = static_cast<uint32_t>((tex0 >> 51) & 0xFu);
        const uint32_t texturePsm = psm;
        uint32_t textureTbw = tbw;
        int32_t bpp = nodeBpp;
        bool preSwizzled = false;
        uint32_t n = 0u;
        if (nodeBpp == 8)
        {
            if (flag != 0)
            {
                w >>= 1;
                h >>= 1;
                psm = 0u;
                bpp = 32;
                tbw >>= 1;
                preSwizzled = true;
            }
            n = 4u;
        }
        else if (nodeBpp == 4)
        {
            n = 1u;
        }
        // The palettes, unless the node's manager uploads them through its
        // own callback (a transfer of a shape not known here): palette i
        // `stride` bytes after the first.
        if (n > 0u && clut != 0u && !(fn != 0u && mode == 0u))
        {
            const int32_t stride = readAt<int16_t>(tex + 0x30u);
            const uint32_t entries = nodeBpp == 8 ? 256u : 16u;
            const uint32_t paletteBytes = entries * ((cpsm & 0x3Fu) == 0u ? 4u : 2u);
            for (uint32_t i = 0u; i < count; ++i)
            {
                const uint32_t at = physical(clut + i * static_cast<uint32_t>(stride > 0 ? stride : int32_t(paletteBytes)));
                rg::nativeproducer::palette(g_rdram + at, PS2_RAM_SIZE - at, (base + i * n) & 0x3FFFu, cpsm, entries);
            }
        }
        for (uint32_t i = 0u; i < 4u; ++i)
        {
            const uint32_t mip = readAt<uint32_t>(tex + 0x50u + i * 4u);
            if (mip == 0u || w <= 0 || h <= 0)
                break;
            if (tbw == 0u)
                tbw = 1u;
            rg::nativeproducer::TextureLevel level;
            const uint32_t at = physical(mip);
            level.ram = g_rdram + at;
            level.ramBytes = PS2_RAM_SIZE - at;
            level.tbp = tbp & 0x3FFFu;
            level.psm = texturePsm;
            level.transferWidth = static_cast<uint32_t>(w);
            level.transferHeight = static_cast<uint32_t>(h);
            level.transferBufferWidth = tbw;
            level.transferPsm = psm;
            level.width = preSwizzled ? level.transferWidth * 2u : level.transferWidth;
            level.height = preSwizzled ? level.transferHeight * 2u : level.transferHeight;
            level.bufferWidth = preSwizzled ? (textureTbw > 1u ? textureTbw : 1u) : tbw;
            level.preSwizzled = preSwizzled;
            level.cbp = static_cast<uint32_t>((tex0 >> 37) & 0x3FFFu);
            rg::nativeproducer::texture(level);
            const int32_t k = bpp * (w * h) / 256 / 8;
            w >>= 1;
            h >>= 1;
            tbp += static_cast<uint16_t>(k);
            tbw >>= 1;
            textureTbw >>= 1;
        }
    }
    g_texBuildUploadOriginal(rdram, ctx, runtime);
}
} // namespace

void install(PS2Runtime &runtime)
{
    if (!rg::nativeproducer::pipelineOn())
        return;
    g_rdram = runtime.memory().getRDRAM();
    if (!rg::timing::hookFunction(runtime, kSetFrameFromDesc, &setFrameFromDesc, g_setFrameFromDescOriginal) ||
        !rg::timing::hookFunction(runtime, kGetFrameTexDesc, &getFrameTexDesc, g_getFrameTexDescOriginal) ||
        !rg::timing::hookFunction(runtime, kGetDispFbTexDesc, &getDispFbTexDesc, g_getDispFbTexDescOriginal))
        std::fprintf(stderr, "[native-capture] the target declarations could not be hooked\n");
    if (!rg::timing::hookFunction(runtime, kTexBuildUploadPkt, &texBuildUpload, g_texBuildUploadOriginal))
        std::fprintf(stderr, "[native-capture] TexBuildUploadPkt could not be hooked\n");
}

} // namespace rg::nativecapture
