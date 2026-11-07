#include "native_capture.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

#if defined(RG_HAS_SDLGPU)
#include "gfx/native/native_pipeline.h"
#endif

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
// Steps 7b and 7c of docs/NATIVE-PIPELINE.md: the game's reads of GS memory
// (libgraph's sceGsSetDefStoreImage(si, sbp, sbw, spsm, x, y, w, h), then
// sceGsExecStoreImage(si, buf) and sceGsSyncPath(0, 0)) answered from the
// renderer instead: a depth read (the lens flare's 8x8 block of Z at the
// sun) from its depth a frame or two late, a colour read (the fades' and
// the menus' frame captures) from its targets as the last frame drawn left
// them. Straight into buf, with no GS transfer. The sceGsSyncPath after it
// is left alone: the game drains VIF1 before these reads, so it has nothing
// to wait for. A colour read the renderer does not answer in time gets the
// last answer of its size, or black (under the pipeline the GS has nothing
// to give); a depth read it cannot place goes to the GS as before.
constexpr uint32_t kSceGsSetDefStoreImage = 0x00109348u;
constexpr uint32_t kSceGsExecStoreImage = 0x00109488u;
PS2Runtime::RecompiledFunction g_setDefStoreImageOriginal = nullptr;
PS2Runtime::RecompiledFunction g_execStoreImageOriginal = nullptr;

struct StoreImageCall
{
    uint32_t si = 0u;
    uint32_t base = 0u, psm = 0u, x = 0u, y = 0u, width = 0u, height = 0u;
};
thread_local StoreImageCall t_storeImage;

void setDefStoreImage(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    StoreImageCall &r = t_storeImage;
    r.si = getRegU32(ctx, 4);
    r.base = getRegU32(ctx, 5) & 0x3FFFu;
    r.psm = getRegU32(ctx, 7) & 0x3Fu;
    r.x = getRegU32(ctx, 8) & 0x7FFu;
    r.y = getRegU32(ctx, 9) & 0x7FFu;
    r.width = getRegU32(ctx, 10) & 0xFFFFu;
    r.height = getRegU32(ctx, 11) & 0xFFFFu;
    g_setDefStoreImageOriginal(rdram, ctx, runtime);
}

// A colour read's RGBA pixels in the read's format (CT32, CT24, CT16).
void toFormat(const std::vector<uint8_t> &rgba, uint32_t psm, size_t pixels, std::vector<uint8_t> &out)
{
    if (psm == 0x00u)
    {
        out = rgba;
        return;
    }
    if (psm == 0x01u)
    {
        out.resize(pixels * 3u);
        for (size_t i = 0; i < pixels; ++i)
            std::memcpy(out.data() + i * 3u, rgba.data() + i * 4u, 3u);
        return;
    }
    out.resize(pixels * 2u);
    for (size_t i = 0; i < pixels; ++i)
    {
        const uint8_t *c = rgba.data() + i * 4u;
        const uint16_t v = static_cast<uint16_t>((c[0] >> 3) | ((c[1] >> 3) << 5) | ((c[2] >> 3) << 10) |
                                                 (c[3] >= 0x80u ? 0x8000u : 0u));
        out[i * 2u] = static_cast<uint8_t>(v);
        out[i * 2u + 1u] = static_cast<uint8_t>(v >> 8);
    }
}

// The read's bytes, or false to leave it to the GS.
bool answer(const StoreImageCall &q, std::vector<uint8_t> &bytes)
{
    const uint32_t psm = q.psm;
    const bool depth = psm == 0x30u || psm == 0x31u || psm == 0x32u || psm == 0x3Au;
    if (depth)
        return rg::nativeproducer::readDepth(q.base, q.psm, q.x, q.y, q.width, q.height, bytes) && !bytes.empty();
#if defined(RG_HAS_SDLGPU)
    const bool colour = psm == 0x00u || psm == 0x01u || psm == 0x02u || psm == 0x0Au;
    if (!colour)
        return false;
    namespace native = dq8::gfx::native;
    native::ColorRead read;
    read.target = rg::nativeproducer::captureTarget(q.base);
    read.rect = {static_cast<int32_t>(q.x), static_cast<int32_t>(q.y), static_cast<int32_t>(q.x + q.width),
                 static_cast<int32_t>(q.y + q.height)};
    // The presenting thread answers it (step 7d), nothing on the GS queue.
    const uint32_t id = native::beginColorRead(read);
    thread_local std::vector<uint8_t> rgba;
    // The last answer, by the read's size and format, for a read not served.
    static std::mutex lastMutex;
    static std::vector<uint8_t> last;
    static uint32_t lastWidth = 0u, lastHeight = 0u, lastPsm = ~0u;
    if (native::waitColorRead(id, rgba, 2000u))
    {
        toFormat(rgba, psm, static_cast<size_t>(q.width) * q.height, bytes);
        if (!bytes.empty())
        {
            std::lock_guard lock(lastMutex);
            last = bytes;
            lastWidth = q.width;
            lastHeight = q.height;
            lastPsm = psm;
            return true;
        }
    }
    {
        std::lock_guard lock(lastMutex);
        const bool same = !last.empty() && lastWidth == q.width && lastHeight == q.height && lastPsm == psm;
        static int told = 0;
        if (told < 8)
        {
            ++told;
            std::fprintf(stderr, "[native-capture] a colour read (%ux%u) was not served; %s\n", q.width, q.height,
                         same ? "the last one of its size is given" : "black is given");
        }
        if (same)
            bytes = last;
        else
            bytes.assign(static_cast<size_t>(q.width) * q.height * (psm == 0x00u ? 4u : psm == 0x01u ? 3u : 2u),
                         0u);
    }
    return !bytes.empty();
#else
    return false;
#endif
}

void execStoreImage(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t si = getRegU32(ctx, 4);
    const uint32_t buffer = getRegU32(ctx, 5);
    thread_local std::vector<uint8_t> bytes;
    if (si == t_storeImage.si && answer(t_storeImage, bytes))
    {
        std::memcpy(g_rdram + physical(buffer), bytes.data(), std::min<size_t>(bytes.size(), PS2_RAM_SIZE - physical(buffer)));
        t_storeImage.si = 0u;
        setReturnS32(ctx, 0);
        ctx->pc = getRegU32(ctx, 31);
        return;
    }
    t_storeImage.si = 0u;
    g_execStoreImageOriginal(rdram, ctx, runtime);
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
    if (!rg::timing::hookFunction(runtime, kSceGsSetDefStoreImage, &setDefStoreImage, g_setDefStoreImageOriginal) ||
        !rg::timing::hookFunction(runtime, kSceGsExecStoreImage, &execStoreImage, g_execStoreImageOriginal))
        std::fprintf(stderr, "[native-capture] the GS memory reads could not be hooked: they stay GS transfers\n");
}

} // namespace rg::nativecapture
