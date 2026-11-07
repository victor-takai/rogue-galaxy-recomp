#include "native_effects.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>

#include "gif_events.h"
#include "native_producer.h"
#include "ps2_runtime.h"
#include "return_hook.h"
#include "scene_timing.h"

namespace rg::nativefx
{
namespace
{

constexpr uint32_t kGlareFilterDraw = 0x0017A0D0u; // GlareFilterDrawMaybe
constexpr uint32_t kPlaneShadowEnd = 0x00146CC0u;   // DrawListEndPlaneShadowPass
constexpr uint32_t kPlaneShadowBegin = 0x00147B80u; // DrawListBeginPlaneShadowPass
// ModelDrawOffscreenCompositeAtMaybe: the composite's only way in (it jumps
// into ModelDrawOffscreenCompositeMaybe_16aac0 past the function table).
constexpr uint32_t kCompositeAt = 0x0016B8A0u;
constexpr uint32_t kModelEmitDrawPackets = 0x0014EC20u; // ModelEmitDrawPacketsMaybe
constexpr uint32_t kDrawListFrameBegin = 0x001425D0u;
constexpr uint32_t kCBlurTexBlur = 0x001789E0u; // CBlurTexBlurMaybe
constexpr uint32_t kBloom = 0x00179360u;        // BloomExtractBlurMaybe
constexpr uint32_t kDof = 0x0017D150u;          // DofFilterDrawMaybe
constexpr uint32_t kCBlurDraw = 0x001784E0u;    // CBlurDrawMaybe
constexpr uint32_t kRefraction = 0x0013FFD0u;   // RefractionCaptureFrame

// The draw context (GetDrawCtx_15de80 reads it at gp-0x40cc): the frame's
// buffer index at +0 and its memory stacks at +8, each 8 bytes into the
// pointer there, with its base at +0x28 and the quadwords used at +0x2C.
// Every packet of the frame is written in the stack of the buffer being
// built.
constexpr uint32_t kDrawCtxPointer = 0x004810A4u;

uint8_t *g_rdram = nullptr;
bool g_log = false;

uint32_t physical(uint32_t address)
{
    return address & (PS2_RAM_SIZE - 1u);
}

uint32_t read32(uint32_t address)
{
    uint32_t value = 0u;
    if (g_rdram)
    {
        std::memcpy(&value, g_rdram + (physical(address) & ~3u), sizeof(value));
    }
    return value;
}

// The memory stack of the buffer being built, 0 when there is none.
uint32_t frameStack()
{
    const uint32_t ctx = read32(kDrawCtxPointer);
    if (ctx == 0u)
    {
        return 0u;
    }
    const uint32_t stack = read32(ctx + 8u + read32(ctx) * 4u);
    return stack == 0u ? 0u : stack + 8u;
}

uint32_t stackTop()
{
    const uint32_t stack = frameStack();
    return stack == 0u ? 0u : read32(stack + 0x28u) + read32(stack + 0x2Cu) * 16u;
}

// The packet memory each effect's calls wrote, by start: a few frames'
// worth, the VIF1 thread reading a frame or two behind the game.
struct Range
{
    uint32_t start = 0u;
    uint32_t end = 0u;
    Place place;
};
std::mutex g_mutex;
std::vector<Range> g_ranges;

// The ranges `start`-`end` overlaps go: the memory is something else's now.
void dropRangesLocked(uint32_t start, uint32_t end)
{
    g_ranges.erase(std::remove_if(g_ranges.begin(), g_ranges.end(),
                                  [&](const Range &range) { return range.start < end && start < range.end; }),
                   g_ranges.end());
}

void addRange(uint32_t start, uint32_t end, Effect effect, uint32_t unit = 0u)
{
    start = physical(start);
    end = physical(end);
    if (end <= start || end - start > (256u << 10))
    {
        return;
    }
    std::lock_guard lock(g_mutex);
    dropRangesLocked(start, end);
    const Range range{start, end, Place{effect, unit}};
    g_ranges.insert(std::upper_bound(g_ranges.begin(), g_ranges.end(), range,
                                     [](const Range &a, const Range &b) { return a.start < b.start; }),
                    range);
}

// A frame begins in a buffer: what the effects wrote there before is some
// other packet's now. (The game waits for the lists of a buffer to finish
// before it builds in it again.)
void dropRanges(uint32_t start, uint32_t end)
{
    start = physical(start);
    end = physical(end);
    std::lock_guard lock(g_mutex);
    dropRangesLocked(start, end);
}

PS2Runtime::RecompiledFunction g_frameBeginOriginal = nullptr;

void afterFrameBegin(uint8_t *, R5900Context *, PS2Runtime *, uint32_t)
{
    // The stack's base and its capacity in quadwords.
    const uint32_t stack = frameStack();
    if (stack != 0u)
    {
        const uint32_t base = read32(stack + 0x28u);
        dropRanges(base, base + read32(stack + 0x34u) * 16u);
    }
}

void drawListFrameBegin(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    rg::hooks::callThen(g_frameBeginOriginal, rdram, ctx, runtime, &afterFrameBegin, 0u);
}

PS2Runtime::RecompiledFunction g_glareOriginal = nullptr;

void afterGlare(uint8_t *, R5900Context *, PS2Runtime *, uint32_t top)
{
    addRange(top, stackTop(), Effect::Glare);
}

void glareFilterDraw(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    rg::hooks::callThen(g_glareOriginal, rdram, ctx, runtime, &afterGlare, stackTop());
}

PS2Runtime::RecompiledFunction g_shadowOriginal = nullptr;

void afterShadow(uint8_t *, R5900Context *, PS2Runtime *, uint32_t top)
{
    addRange(top, stackTop(), Effect::Shadow);
}

void planeShadowEnd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    rg::hooks::callThen(g_shadowOriginal, rdram, ctx, runtime, &afterShadow, stackTop());
}

PS2Runtime::RecompiledFunction g_shadowBeginOriginal = nullptr;

void afterShadowBegin(uint8_t *, R5900Context *, PS2Runtime *, uint32_t top)
{
    addRange(top, stackTop(), Effect::ShadowSetup);
}

void planeShadowBegin(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    rg::hooks::callThen(g_shadowBeginOriginal, rdram, ctx, runtime, &afterShadowBegin, stackTop());
}

PS2Runtime::RecompiledFunction g_cblurOriginal = nullptr;

void afterCBlur(uint8_t *, R5900Context *, PS2Runtime *, uint32_t top)
{
    addRange(top, stackTop(), Effect::CBlur);
}

void cblurTexBlur(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    rg::hooks::callThen(g_cblurOriginal, rdram, ctx, runtime, &afterCBlur, stackTop());
}

PS2Runtime::RecompiledFunction g_bloomOriginal = nullptr;

void afterBloom(uint8_t *, R5900Context *, PS2Runtime *, uint32_t top)
{
    addRange(top, stackTop(), Effect::Bloom);
}

void bloomExtractBlur(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    rg::hooks::callThen(g_bloomOriginal, rdram, ctx, runtime, &afterBloom, stackTop());
}

// ScreenColorFilterMaybe(r, g, b) (RGDecomp's C; GameSceneTintApply's, for
// the grayscale filter task): the frame's alpha from its green, through a
// CT16 view of it drawn onto itself 8 pixels over, then the frame Cs Ad:
// (r, g, b) times the frame's green / 128. The colour is the range's unit
// (and bit 24, so none is 0).
constexpr uint32_t kColorFilter = 0x00179C60u;
PS2Runtime::RecompiledFunction g_colorFilterOriginal = nullptr;
uint32_t g_colorFilterUnit = 0u; // game thread only

void afterColorFilter(uint8_t *, R5900Context *, PS2Runtime *, uint32_t top)
{
    addRange(top, stackTop(), Effect::ColorFilter, g_colorFilterUnit);
}

void colorFilter(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    g_colorFilterUnit = (getRegU32(ctx, 4) & 0xFFu) | ((getRegU32(ctx, 5) & 0xFFu) << 8) |
                        ((getRegU32(ctx, 6) & 0xFFu) << 16) | (1u << 24);
    rg::hooks::callThen(g_colorFilterOriginal, rdram, ctx, runtime, &afterColorFilter, stackTop());
}

// RG_DEBUG_COLOR_FILTER=RRGGBB: GameSceneTintApply (the grayscale filter
// task's, each frame the task runs) calls the filter with this colour as if
// the scene's tint were on, to see it where no scene tints. It calls the
// filter directly and marks its packets itself: callThen's return hook is
// for calls the game makes, not for one made from native code.
uint32_t debugColorFilter()
{
    static const uint32_t color = [] {
        const char *value = std::getenv("RG_DEBUG_COLOR_FILTER");
        return value && *value ? (static_cast<uint32_t>(std::strtoul(value, nullptr, 16)) | (1u << 24)) : 0u;
    }();
    return color;
}

constexpr uint32_t kGameSceneTintApply = 0x001CE8F0u;
PS2Runtime::RecompiledFunction g_tintApplyOriginal = nullptr;

void gameSceneTintApply(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (const uint32_t color = debugColorFilter(); color != 0u)
    {
        static bool told = false;
        if (!told)
        {
            told = true;
            std::fprintf(stderr, "[nativefx] debug colour filter %06x from GameSceneTintApply\n", color & 0xFFFFFFu);
        }
        // The call GameSceneTintApply makes when the scene's tint is on.
        const R5900Context saved = *ctx;
        const auto set = [&](int reg, uint32_t value) {
            ctx->r[reg] = _mm_set_epi64x(0, static_cast<int64_t>(static_cast<int32_t>(value)));
        };
        set(4, (color >> 16) & 0xFFu);
        set(5, (color >> 8) & 0xFFu);
        set(6, color & 0xFFu);
        if (g_colorFilterOriginal)
        {
            const uint32_t top = stackTop();
            g_colorFilterOriginal(rdram, ctx, runtime);
            // The unit as colorFilter makes it: r in the low byte.
            const uint32_t unit =
                ((color >> 16) & 0xFFu) | (color & 0xFF00u) | ((color & 0xFFu) << 16) | (1u << 24);
            addRange(top, stackTop(), Effect::ColorFilter, unit);
        }
        else if (PS2Runtime::RecompiledFunction filter = runtime->lookupFunction(kColorFilter))
        {
            filter(rdram, ctx, runtime);
        }
        // GameSceneTintApply itself is not run: return to its caller, as a
        // replaced function must (a hook that left pc where it was froze
        // the game before).
        *ctx = saved;
        ctx->pc = getRegU32(ctx, 31);
        return;
    }
    g_tintApplyOriginal(rdram, ctx, runtime);
}

PS2Runtime::RecompiledFunction g_cblurDrawOriginal = nullptr;

void afterCBlurDraw(uint8_t *, R5900Context *, PS2Runtime *, uint32_t top)
{
    addRange(top, stackTop(), Effect::CBlurDraw);
}

void cblurDraw(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    rg::hooks::callThen(g_cblurDrawOriginal, rdram, ctx, runtime, &afterCBlurDraw, stackTop());
}

// FocusFilterDrawMaybe and ShadowMapBlur (RG_NATIVE_FOCUS, RG_NATIVE_SHADOW_MAP):
// sprite effects through the builder, as the bloom; both use the CT16 view of
// a buffer the colour filter does (kGreenToAlpha where its masks are those).
// ShadowMapRender is not hooked: its models are VU1 draws (native already) and
// its sprites use the destination alpha test, which the builder refuses.
constexpr uint32_t kFocusFilter = 0x0017C000u;
constexpr uint32_t kShadowMapBlur = 0x00145B60u;
PS2Runtime::RecompiledFunction g_focusOriginal = nullptr;
PS2Runtime::RecompiledFunction g_shadowMapBlurOriginal = nullptr;

void afterFocus(uint8_t *, R5900Context *, PS2Runtime *, uint32_t top)
{
    addRange(top, stackTop(), Effect::Focus);
}

void focusFilterDraw(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    rg::hooks::callThen(g_focusOriginal, rdram, ctx, runtime, &afterFocus, stackTop());
}

void afterShadowMapBlur(uint8_t *, R5900Context *, PS2Runtime *, uint32_t top)
{
    addRange(top, stackTop(), Effect::ShadowMapBlur);
}

void shadowMapBlur(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    rg::hooks::callThen(g_shadowMapBlurOriginal, rdram, ctx, runtime, &afterShadowMapBlur, stackTop());
}

PS2Runtime::RecompiledFunction g_refractionOriginal = nullptr;

void afterRefraction(uint8_t *, R5900Context *, PS2Runtime *, uint32_t top)
{
    addRange(top, stackTop(), Effect::Refraction);
}

void refractionCaptureFrame(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    rg::hooks::callThen(g_refractionOriginal, rdram, ctx, runtime, &afterRefraction, stackTop());
}

PS2Runtime::RecompiledFunction g_dofOriginal = nullptr;

void afterDof(uint8_t *, R5900Context *, PS2Runtime *, uint32_t top)
{
    addRange(top, stackTop(), Effect::Dof);
}

void dofFilterDraw(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    rg::hooks::callThen(g_dofOriginal, rdram, ctx, runtime, &afterDof, stackTop());
}

// RG_NATIVE_EFFECTS_PROBE: the effects still GS packets, hooked to see
// Stage E, an object at a time. The composite draws offscreen when its
// settings (a0) have +0x50 and +0x5C, a buffer at +0x20 and a strength at
// +0x24 not negative (the archive branch's docs/NATIVE-RENDERER.md, "Stage E, why offscreen"):
// the packets up to the end of its last ModelEmitDrawPackets are the head,
// the rest the tail. (Game thread only.)
struct Composite
{
    uint32_t unit = 0u;
    uint32_t start = 0u;
    uint32_t headEnd = 0u;
};
Composite g_composite;
uint32_t g_units = 0u;
PS2Runtime::RecompiledFunction g_compositeOriginal = nullptr;
PS2Runtime::RecompiledFunction g_emitOriginal = nullptr;

float readFloat(uint32_t address)
{
    const uint32_t bits = read32(address);
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

// The fixed DMA chain each composite's tail calls (GsPktAddDmaCallTag in
// ModelDrawOffscreenCompositeMaybe at 0x16b180): the memory it sends from,
// any tail's (unit 0). Walked once, when the first composite is marked.
constexpr uint32_t kCompositeTailCall = 0x00445C60u;

void markTailCall()
{
    static bool marked = false;
    if (marked)
        return;
    marked = true;
    uint32_t address = kCompositeTailCall;
    std::vector<uint32_t> returns;
    for (int step = 0; step < 256; ++step)
    {
        const uint32_t tag = read32(address);
        const uint32_t qwc = tag & 0xFFFFu;
        const uint32_t id = (tag >> 28) & 7u;
        const uint32_t target = read32(address + 4u) & 0x7FFFFFF0u;
        // The data after the tag (cnt, next, call, ret, end) or at its
        // address (ref, refe, refs).
        if (g_log)
            std::fprintf(stderr, "[nativefx] tail call: tag at %08x id %u qwc %u address %08x\n", address, id, qwc, target);
        if (id == 0u || id == 3u || id == 4u)
            addRange(target, target + qwc * 16u, Effect::CompositeTail, 0u);
        else
            addRange(address, address + 16u + qwc * 16u, Effect::CompositeTail, 0u);
        if (id == 0u || id == 7u)
            break;
        if (id == 1u)
            address += 16u + qwc * 16u;
        else if (id == 2u)
            address = target;
        else if (id == 3u || id == 4u)
            address += 16u;
        else if (id == 5u)
        {
            returns.push_back(address + 16u + qwc * 16u);
            address = target;
        }
        else
        {
            if (returns.empty())
                break;
            address = returns.back();
            returns.pop_back();
        }
    }
}

void afterComposite(uint8_t *, R5900Context *, PS2Runtime *, uint32_t unit)
{
    if (g_composite.unit != unit)
        return;
    markTailCall();
    const uint32_t end = stackTop();
    if (g_composite.headEnd > g_composite.start && g_composite.headEnd < end)
    {
        addRange(g_composite.start, g_composite.headEnd, Effect::CompositeHead, unit);
        addRange(g_composite.headEnd, end, Effect::CompositeTail, unit);
    }
    g_composite = Composite{};
}

void compositeAt(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t settings = getRegU32(ctx, 4);
    const bool offscreen = read32(settings + 0x5Cu) != 0u && read32(settings + 0x50u) != 0u &&
                           read32(settings + 0x20u) != 0u && readFloat(settings + 0x24u) >= 0.0f;
    if (!offscreen || g_composite.unit != 0u)
    {
        g_compositeOriginal(rdram, ctx, runtime);
        return;
    }
    g_units = g_units + 1u == 0u ? 1u : g_units + 1u;
    g_composite = Composite{g_units, stackTop(), 0u};
    rg::hooks::callThen(g_compositeOriginal, rdram, ctx, runtime, &afterComposite, g_units);
}

void afterEmit(uint8_t *, R5900Context *, PS2Runtime *, uint32_t)
{
    if (g_composite.unit != 0u)
        g_composite.headEnd = stackTop();
}

void modelEmitDrawPackets(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (g_composite.unit == 0u)
    {
        g_emitOriginal(rdram, ctx, runtime);
        return;
    }
    rg::hooks::callThen(g_emitOriginal, rdram, ctx, runtime, &afterEmit, 0u);
}

bool g_glare = false;
bool g_composites = false;
bool g_shadow = false;
bool g_cblur = false;
bool g_bloom = false;
bool g_dof = false;
bool g_refraction = false;
bool g_flip = false;
bool g_colorFilter = false;
bool g_focus = false;
bool g_shadowMap = false;
// The flip's draw environment (below, with flipPacket).
constexpr uint32_t kSceGsPutDrawEnv = 0x00108BB8u;
PS2Runtime::RecompiledFunction g_putDrawEnvOriginal = nullptr;
void putDrawEnv(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);

// --- Packets -----------------------------------------------------------

constexpr uint8_t kXyzf2 = 0x04u, kXyz2 = 0x05u, kXyzf3 = 0x0Cu, kXyz3 = 0x0Du, kAd = 0x0Eu;

// The packet with every drawing kick made a plain vertex write: XYZ2 and
// XYZF2 as XYZ3 and XYZF3, PACKED ones with ADC set.
void withholdKicks(std::vector<uint8_t> &bytes)
{
    uint8_t *data = bytes.data();
    const uint32_t size = static_cast<uint32_t>(bytes.size());
    uint32_t at = 0u;
    while (at + 16u <= size)
    {
        uint64_t tag, regs;
        std::memcpy(&tag, data + at, sizeof(tag));
        std::memcpy(&regs, data + at + 8u, sizeof(regs));
        const uint32_t tagAt = at;
        at += 16u;
        const uint32_t nloop = static_cast<uint32_t>(tag & 0x7FFFu);
        const uint32_t flg = static_cast<uint32_t>((tag >> 58) & 3u);
        uint32_t nreg = static_cast<uint32_t>(tag >> 60);
        nreg = nreg == 0u ? 16u : nreg;
        if (flg == 0u)
        {
            for (uint32_t loop = 0; loop < nloop; ++loop)
            {
                for (uint32_t r = 0; r < nreg && at + 16u <= size; ++r, at += 16u)
                {
                    const uint8_t descriptor = static_cast<uint8_t>((regs >> (4u * r)) & 0xFu);
                    if (descriptor == kXyz2 || descriptor == kXyzf2)
                    {
                        data[at + 13u] |= 0x80u; // ADC, bit 111
                    }
                    else if (descriptor == kAd)
                    {
                        uint8_t &reg = data[at + 8u];
                        reg = reg == kXyz2 ? kXyz3 : (reg == kXyzf2 ? kXyzf3 : reg);
                    }
                }
            }
        }
        else if (flg == 1u)
        {
            for (uint32_t r = 0; r < nreg; ++r)
            {
                const uint64_t shift = 4u * r;
                const uint64_t descriptor = (regs >> shift) & 0xFu;
                const uint64_t replaced = descriptor == kXyz2 ? kXyz3 : (descriptor == kXyzf2 ? kXyzf3 : descriptor);
                regs = (regs & ~(0xFull << shift)) | (replaced << shift);
            }
            std::memcpy(data + tagAt + 8u, &regs, sizeof(regs));
            at += ((nloop * nreg * 8u) + 15u) & ~15u;
        }
        else
        {
            at += nloop * 16u;
        }
    }
}

// --- Sprites -------------------------------------------------------------

// The GS registers the sprites depend on, and whether the effect's packets
// set them (anything before the effect is not seen here).
struct Registers
{
    std::array<uint64_t, 0x64> value{};
    std::array<bool, 0x64> set{};
    void write(uint8_t reg, uint64_t v)
    {
        if (reg < value.size())
        {
            value[reg] = v;
            set[reg] = true;
        }
    }
};

// The VIF1 thread's: what the last shadow setup's packets wrote.
thread_local Registers t_shadowSetup;

constexpr uint8_t kTex0 = 0x06u, kXyoffset = 0x18u, kPrmodecont = 0x1Au, kPrmode = 0x1Bu,
                  kTex1 = 0x14u, kScissor = 0x40u, kAlpha = 0x42u, kDthe = 0x45u, kColclamp = 0x46u,
                  kTest = 0x47u, kPabe = 0x49u, kTexa = 0x3Bu, kFba = 0x4Au, kFrame = 0x4Cu, kZbuf = 0x4Eu;

struct BufferKey
{
    uint32_t base, fbw, psm;
    bool operator==(const BufferKey &other) const
    {
        return base == other.base && fbw == other.fbw && psm == other.psm;
    }
};

struct Builder
{
    GSNativeEffect &out;
    std::string &why;
    std::array<BufferKey, GSNativeEffect::kMaxBuffers> keys{};
    // Per buffer: drawn or read yet, in op order.
    std::array<int, GSNativeEffect::kMaxBuffers> firstWrite{};
    std::array<int, GSNativeEffect::kMaxBuffers> firstRead{};
    std::array<float, GSNativeEffect::kMaxBuffers> readBottom{};
    std::string missing;
    bool failed = false;
    // A composite's tail: sprites from the object's buffer (its layer, not
    // the GS's) over the frame, alpha tested.
    bool tail = false;
    // The flip's copy: into the display buffer, which is CT24.
    bool flip = false;
    // Per op: how many CT16 strips a kGreenToAlpha op stands for.
    std::array<uint32_t, GSNativeEffect::kMaxOps> strips{};

    bool fail(const char *reason)
    {
        if (!failed)
            why = reason;
        failed = true;
        if (trace)
            std::fprintf(stderr, "[nativefx]   (refused: %s)\n", reason);
        return trace; // tracing goes on past a refusal, to show every sprite
    }

    int buffer(const BufferKey &key)
    {
        for (uint32_t i = 0; i < out.bufferCount; ++i)
        {
            if (keys[i] == key)
            {
                return static_cast<int>(i);
            }
        }
        if (out.bufferCount == GSNativeEffect::kMaxBuffers)
        {
            return -1;
        }
        const uint32_t i = out.bufferCount++;
        keys[i] = key;
        out.buffers[i].base = key.base;
        out.buffers[i].fbw = key.fbw;
        out.buffers[i].psm = key.psm;
        firstWrite[i] = -1;
        firstRead[i] = -1;
        readBottom[i] = 0.0f;
        return static_cast<int>(i);
    }

    // A sprite from v0 to v1 under `r`; with quadZ, a strip's quad, its Z
    // v0's at v0's x and v1's at v1's.
    bool trace = false;
    bool quadZ = false;
    bool sprite(const Registers &r, uint16_t prim, const rg::gif::GifEvent &v0, const rg::gif::GifEvent &v1)
    {
        const uint32_t ctxt = (prim >> 9) & 1u;
        if (trace)
            std::fprintf(stderr,
                         "[nativefx]   sprite prim %03x frame %llx tex0 %llx tex1 %llx alpha %llx test %llx texa %llx "
                         "xyoff %llx scissor %llx xy %u,%u-%u,%u uv %u,%u-%u,%u rgba %02x%02x%02x%02x\n",
                         prim, (unsigned long long)r.value[kFrame + ctxt], (unsigned long long)r.value[kTex0 + ctxt],
                         (unsigned long long)r.value[kTex1 + ctxt], (unsigned long long)r.value[kAlpha + ctxt],
                         (unsigned long long)r.value[kTest + ctxt], (unsigned long long)r.value[kTexa],
                         (unsigned long long)r.value[kXyoffset + ctxt], (unsigned long long)r.value[kScissor + ctxt],
                         v0.x, v0.y, v1.x, v1.y, v0.u, v0.v, v1.u, v1.v, v1.rgba[0], v1.rgba[1], v1.rgba[2],
                         v1.rgba[3]);
        const bool tme = (prim >> 4) & 1u;
        const bool abe = (prim >> 6) & 1u;
        if ((prim >> 5) & 1u || (prim >> 7) & 1u)
            return fail("a sprite with fog or antialiasing");
        if (tme && !((prim >> 8) & 1u))
            return fail("a sprite with STQ");
        const auto need = [&](uint8_t reg) { return r.set[reg]; };
        if (!need(kFrame + ctxt) || !need(kScissor + ctxt) || !need(kXyoffset + ctxt) || !need(kTest + ctxt) ||
            !need(kZbuf + ctxt))
        {
            missing = "a sprite under a draw environment set before the effect (context " + std::to_string(ctxt) + ":";
            for (uint8_t reg : {kFrame, kScissor, kXyoffset, kTest, kZbuf})
                if (!need(reg + ctxt))
                    missing += " " + std::to_string(reg + ctxt);
            missing += ")";
            return fail(missing.c_str());
        }
        if (tme && (!need(kTex0 + ctxt) || !need(kTex1 + ctxt)))
            return fail("a sprite under a texture set before the effect");
        if (abe && !need(kAlpha + ctxt))
            return fail("a sprite under a blend set before the effect");
        const uint64_t frame = r.value[kFrame + ctxt];
        const uint64_t test = r.value[kTest + ctxt];
        const uint64_t zbuf = r.value[kZbuf + ctxt];
        if (((frame >> 24) & 0x3Fu) == 2u && (frame >> 32) == 0xFFFFu)
            return greenToAlpha(r, prim, ctxt, v0, v1);
        // CT24 for the flip alone: the display buffer, a buffer of its own
        // format (the GS writes its colour and keeps no alpha; what a CT24
        // surface holds there is the draws').
        const uint32_t framePsm = static_cast<uint32_t>((frame >> 24) & 0x3Fu);
        if ((framePsm != 0u && !(flip && framePsm == 1u)) ||
            ((frame >> 32) != 0u && (frame >> 32) != 0xFF000000u && (frame >> 32) != 0x00FFFFFFu))
        {
            missing = "a sprite into a target not CT32, or masked (FRAME " + std::to_string(frame) + ")";
            return fail(missing.c_str());
        }
        if ((test >> 14) & 1u)
            return fail("a sprite with the destination alpha test");
        // A depth test against the scene (the depth of field's lay back,
        // GEQUAL at one Z): drawn into a GS target only (finish).
        const uint32_t ztst = static_cast<uint32_t>((test >> 17) & 3u);
        const bool depthTest = ((test >> 16) & 1u) && ztst != 1u;
        if (depthTest && ztst == 0u)
            return fail("a sprite with the depth test NEVER");
        if (((zbuf >> 32) & 1u) == 0u)
            return fail("a sprite that writes depth");
        if ((r.value[kDthe] & 1u) || (r.value[kFba + ctxt] & 1u) || (abe && (r.value[kPabe] & 1u)))
            return fail("a sprite with dithering, FBA or PABE");

        GSNativeEffect::Op op;
        if (depthTest)
        {
            op.zbuf = zbuf;
            op.z[0] = v0.x <= v1.x ? v0.z : v1.z;
            op.z[1] = v0.x <= v1.x ? v1.z : v0.z;
            // A sprite has v1's Z throughout; a quad's ramps in x (the
            // caller passes its corners with Z by column).
            if ((prim & 7u) == 6u && !quadZ)
                op.z[0] = op.z[1] = v1.z;
        }
        op.flags |= (frame >> 32) == 0xFF000000u ? GSNativeEffect::kKeepAlpha
                    : (frame >> 32) == 0x00FFFFFFu ? GSNativeEffect::kKeepColor
                                                   : 0u;
        const BufferKey target{static_cast<uint32_t>(frame & 0x1FFu) << 5u, static_cast<uint32_t>((frame >> 16) & 0x3Fu),
                               framePsm};
        const int targetIndex = buffer(target);
        if (targetIndex < 0)
            return fail("more buffers than an effect has");
        op.target = static_cast<uint32_t>(targetIndex);
        if (tme)
        {
            const uint64_t tex0 = r.value[kTex0 + ctxt];
            const uint32_t tfx = static_cast<uint32_t>((tex0 >> 35) & 3u);
            const uint32_t psm = static_cast<uint32_t>((tex0 >> 20) & 0x3Fu);
            if (psm == 1u)
            {
                // TEXA set before the effect: the GS's when the effect
                // reaches it (kTexaFromGs).
                const uint64_t texa = r.value[kTexa];
                op.flags |= GSNativeEffect::kCt24 | (r.set[kTexa] ? 0u : GSNativeEffect::kTexaFromGs);
                op.texa = static_cast<uint32_t>(texa & 0xFFu) | static_cast<uint32_t>(texa & 0x8000u);
            }
            if (psm > 1u)
            {
                missing = "a texture not CT32 or CT24 (TEX0 " + std::to_string(tex0) + ", psm " +
                          std::to_string((tex0 >> 20) & 0x3Fu) + ", tfx " + std::to_string(tfx) + ")";
                return fail(missing.c_str());
            }
            const BufferKey source{static_cast<uint32_t>(tex0 & 0x3FFFu), static_cast<uint32_t>((tex0 >> 14) & 0x3Fu),
                                   0u};
            const int sourceIndex = buffer(source);
            if (sourceIndex < 0)
                return fail("more buffers than an effect has");
            op.source = sourceIndex;
            const uint64_t tex1 = r.value[kTex1 + ctxt];
            const uint32_t mmag = static_cast<uint32_t>((tex1 >> 5) & 1u);
            const uint32_t mmin = static_cast<uint32_t>((tex1 >> 6) & 7u);
            op.flags |= (mmag != 0u || mmin == 1u || (mmin & 4u) != 0u) ? GSNativeEffect::kLinear : 0u;
            op.flags |= tfx == 1u ? GSNativeEffect::kDecal
                        : tfx == 2u ? GSNativeEffect::kHighlight
                        : tfx == 3u ? GSNativeEffect::kHighlight2
                                    : 0u;
            op.flags |= ((tex0 >> 34) & 1u) ? GSNativeEffect::kTextureAlpha : 0u;
        }
        // The GS blends the colour alone and writes As as the alpha: with
        // the colour masked (FBMSK 0x00FFFFFF) a blend changes nothing, so
        // such a sprite is drawn as the unblended alpha write it is (a
        // weapon effect's tail, 2026-11-01, refused every frame otherwise).
        if (abe)
        {
            op.flags |= (op.flags & GSNativeEffect::kKeepColor) ? 0u : GSNativeEffect::kBlend;
            op.alpha = r.value[kAlpha + ctxt];
        }
        // COLCLAMP is 1 unless the game says otherwise; it never does here.
        op.flags |= (!r.set[kColclamp] || (r.value[kColclamp] & 1u)) ? GSNativeEffect::kColClamp : 0u;
        op.test = test;
        const uint64_t offset = r.value[kXyoffset + ctxt];
        const float ox = static_cast<float>(offset & 0xFFFFu) / 16.0f;
        const float oy = static_cast<float>((offset >> 32) & 0xFFFFu) / 16.0f;
        op.rect[0] = v0.x / 16.0f - ox;
        op.rect[1] = v0.y / 16.0f - oy;
        op.rect[2] = v1.x / 16.0f - ox;
        op.rect[3] = v1.y / 16.0f - oy;
        op.uv[0] = v0.u / 16.0f;
        op.uv[1] = v0.v / 16.0f;
        op.uv[2] = v1.u / 16.0f;
        op.uv[3] = v1.v / 16.0f;
        if (op.rect[2] < op.rect[0])
        {
            std::swap(op.rect[0], op.rect[2]);
            std::swap(op.uv[0], op.uv[2]);
        }
        if (op.rect[3] < op.rect[1])
        {
            std::swap(op.rect[1], op.rect[3]);
            std::swap(op.uv[1], op.uv[3]);
        }
        const uint64_t scissor = r.value[kScissor + ctxt];
        op.scissor[0] = static_cast<uint32_t>(scissor & 0x7FFu);
        op.scissor[1] = static_cast<uint32_t>((scissor >> 32) & 0x7FFu);
        op.scissor[2] = static_cast<uint32_t>((scissor >> 16) & 0x7FFu);
        op.scissor[3] = static_cast<uint32_t>((scissor >> 48) & 0x7FFu);
        std::memcpy(op.rgba, v1.rgba, sizeof(op.rgba));
        return add(op);
    }

    // A strip of the CT16 view of a CT32 buffer (192 x 384 for 192 x 192):
    // in each 16 pixels of a row, the 8 that hold the BA halves of 8 CT32
    // pixels drawn from the 8 that hold their RG halves, the same row, with
    // R and G masked, so CT16's B5 and A1 take G's bits 2-7 into A's. The
    // strips together, every BA half of the buffer, are one kGreenToAlpha.
    bool greenToAlpha(const Registers &r, uint16_t prim, uint32_t ctxt, const rg::gif::GifEvent &v0,
                      const rg::gif::GifEvent &v1)
    {
        const uint64_t frame = r.value[kFrame + ctxt];
        const uint64_t tex0 = r.value[kTex0 + ctxt];
        const uint32_t base = static_cast<uint32_t>(frame & 0x1FFu) << 5u;
        const uint32_t fbw = static_cast<uint32_t>((frame >> 16) & 0x3Fu);
        const bool tme = (prim >> 4) & 1u, abe = (prim >> 6) & 1u;
        if (!tme || abe || static_cast<uint32_t>(tex0 & 0x3FFFu) != base ||
            static_cast<uint32_t>((tex0 >> 14) & 0x3Fu) != fbw || ((tex0 >> 20) & 0x3Fu) != 2u ||
            ((tex0 >> 35) & 3u) > 1u)
            return fail("a CT16 sprite other than a channel copy");
        if (((tex0 >> 35) & 3u) == 0u && (v1.rgba[0] != 0x80u || v1.rgba[1] != 0x80u || v1.rgba[2] != 0x80u))
            return fail("a CT16 channel copy with a colour");
        if (r.value[kTest + ctxt] & 1u)
            return fail("a CT16 channel copy with the alpha test");
        const uint64_t offset = r.value[kXyoffset + ctxt];
        const int32_t ox = static_cast<int32_t>(offset & 0xFFFFu), oy = static_cast<int32_t>((offset >> 32) & 0xFFFFu);
        const int32_t x0 = static_cast<int32_t>(v0.x) - ox, x1 = static_cast<int32_t>(v1.x) - ox;
        const int32_t y0 = static_cast<int32_t>(v0.y) - oy, y1 = static_cast<int32_t>(v1.y) - oy;
        // In 1/16 pixel: 8 wide at the second 8 of 16; the texels the first
        // 8 of the same rows (at their centres).
        if (x1 - x0 != 128 || (x0 & 255) != 128 || y0 != 0 || static_cast<int32_t>(v0.u) != x0 - 128 + 8 ||
            static_cast<int32_t>(v0.v) != y0 + 8 || static_cast<int32_t>(v1.u) != x1 - 128 + 8)
            return fail("a CT16 channel copy of another shape");
        const uint64_t scissor = r.value[kScissor + ctxt];
        const uint32_t rows = static_cast<uint32_t>(y1 / 16);
        if ((scissor & 0x7FFu) != 0u || ((scissor >> 32) & 0x7FFu) != 0u ||
            ((scissor >> 16) & 0x7FFu) + 1u < fbw * 64u || ((scissor >> 48) & 0x7FFu) + 1u < rows ||
            y1 % 16 != 0 || rows % 2u != 0u)
            return fail("a CT16 channel copy under a scissor");
        const int index = buffer(BufferKey{base, fbw, 0u});
        if (index < 0)
            return fail("more buffers than an effect has");
        if (out.opCount != 0u)
        {
            GSNativeEffect::Op &last = out.ops[out.opCount - 1u];
            if ((last.flags & GSNativeEffect::kGreenToAlpha) && last.target == static_cast<uint32_t>(index) &&
                last.rect[3] == static_cast<float>(rows / 2u))
            {
                ++strips[out.opCount - 1u];
                return true;
            }
        }
        GSNativeEffect::Op op;
        op.target = static_cast<uint32_t>(index);
        op.source = index;
        op.flags = GSNativeEffect::kGreenToAlpha;
        op.rect[2] = static_cast<float>(fbw * 64u);
        op.rect[3] = static_cast<float>(rows / 2u);
        op.uv[2] = op.rect[2];
        op.uv[3] = op.rect[3];
        op.scissor[2] = fbw * 64u - 1u;
        op.scissor[3] = rows / 2u - 1u;
        op.test = r.value[kTest + ctxt];
        if (!add(op))
            return false;
        strips[out.opCount - 1u] = 1u;
        return true;
    }

    static bool sameDrawing(const GSNativeEffect::Op &a, const GSNativeEffect::Op &b)
    {
        return a.target == b.target && a.source == b.source && a.flags == b.flags && a.alpha == b.alpha &&
               a.test == b.test && a.zbuf == b.zbuf && a.z[0] == b.z[0] && a.z[1] == b.z[1] && std::memcmp(a.rgba, b.rgba, sizeof(a.rgba)) == 0 &&
               std::memcmp(a.scissor, b.scissor, sizeof(a.scissor)) == 0;
    }

    // The next strip of the last sprite joins it: same drawing, the same rows
    // and texture rows, starting where it ended at nearly the same texels a
    // pixel (the strips' UVs are rounded to 1/16 texel each).
    bool join(GSNativeEffect::Op &last, const GSNativeEffect::Op &op)
    {
        if (!sameDrawing(last, op) || last.rect[2] != op.rect[0] || last.rect[1] != op.rect[1] ||
            last.rect[3] != op.rect[3])
            return false;
        if (op.source < 0)
        {
            last.rect[2] = op.rect[2];
            return true;
        }
        if (last.uv[2] != op.uv[0] || last.uv[1] != op.uv[1] || last.uv[3] != op.uv[3])
            return false;
        const float rateLast = (last.uv[2] - last.uv[0]) / (last.rect[2] - last.rect[0]);
        const float rate = (op.uv[2] - op.uv[0]) / (op.rect[2] - op.rect[0]);
        if (std::fabs(rate - rateLast) > 0.02f * std::max(std::fabs(rateLast), 1e-3f))
            return false;
        last.rect[2] = op.rect[2];
        last.uv[2] = op.uv[2];
        return true;
    }

    bool add(const GSNativeEffect::Op &op)
    {
        if (out.opCount != 0u && !(op.flags & GSNativeEffect::kGreenToAlpha) &&
            !(out.ops[out.opCount - 1u].flags & GSNativeEffect::kGreenToAlpha) && join(out.ops[out.opCount - 1u], op))
            return true;
        if (out.opCount == GSNativeEffect::kMaxOps)
            return fail("more sprites than an effect has");
        const int index = static_cast<int>(out.opCount);
        out.ops[out.opCount++] = op;
        if (op.source >= 0)
        {
            int &read = firstRead[op.source];
            read = read < 0 ? index : read;
            readBottom[op.source] = std::max(readBottom[op.source], std::max(op.uv[1], op.uv[3]));
        }
        int &write = firstWrite[op.target];
        write = write < 0 ? index : write;
        return true;
    }

    static bool readsDestination(const GSNativeEffect::Op &op)
    {
        // An alpha-only sprite keeps its ALPHA though it is not blended:
        // whether it names Cd is judged as before.
        if (!(op.flags & (GSNativeEffect::kBlend | GSNativeEffect::kKeepColor)))
            return false;
        const uint32_t a = op.alpha & 3u, b = (op.alpha >> 2) & 3u, c = (op.alpha >> 4) & 3u,
                       d = (op.alpha >> 6) & 3u;
        return a == 1u || b == 1u || c == 1u || d == 1u;
    }

    // Which buffers are the GS's and the extent of each; and whether the
    // effect's own buffers are wholly drawn before anything reads them, and
    // the GS's drawn only in ways a fixed-function blend can.
    bool finish()
    {
        if (out.opCount == 0u)
            return fail("no sprites");
        for (uint32_t i = 0; i < out.bufferCount; ++i)
        {
            GSNativeEffect::Buffer &buffer = out.buffers[i];
            // The GS's: never drawn here, read first, first drawn over
            // what it holds, or drawn and never read here (its result is
            // for later: the refraction's copy of the frame).
            const bool external = firstWrite[i] < 0 || firstRead[i] < 0 || firstRead[i] <= firstWrite[i] ||
                                  readsDestination(out.ops[firstWrite[i]]);
            buffer.external = external ? 1u : 0u;
            uint32_t width = 0u, height = 0u;
            for (uint32_t n = 0; n < out.opCount; ++n)
            {
                const GSNativeEffect::Op &op = out.ops[n];
                if (op.target != i)
                    continue;
                width = std::max(width, std::min(op.scissor[2] + 1u, static_cast<uint32_t>(std::ceil(op.rect[2]))));
                height = std::max(height, std::min(op.scissor[3] + 1u, static_cast<uint32_t>(std::ceil(op.rect[3]))));
            }
            if (external)
            {
                buffer.width = std::max(width, buffer.fbw * 64u);
                buffer.height = std::max(height, static_cast<uint32_t>(std::ceil(readBottom[i])));
                buffer.height = std::min(buffer.height, 2048u);
                continue;
            }
            buffer.width = width;
            buffer.height = height;
            const GSNativeEffect::Op &first = out.ops[firstWrite[i]];
            if (readsDestination(first) || first.rect[0] > 0.0f || first.rect[1] > 0.0f || first.scissor[0] != 0u ||
                first.scissor[1] != 0u || std::ceil(first.rect[2]) < static_cast<float>(width) ||
                std::ceil(first.rect[3]) < static_cast<float>(height) || (first.test & 1u))
                return fail("an effect buffer read before it is wholly drawn");
        }
        // A GS buffer drawn in a way the pipeline cannot is taken over
        // (kTakenOver): drawn as the effect's own from what it holds, and
        // what it ends with put back.
        for (uint32_t n = 0; n < out.opCount && !tail; ++n)
        {
            GSNativeEffect::Buffer &target = out.buffers[out.ops[n].target];
            if (target.external && intoGs(out.ops[n]) != nullptr)
                target.external = GSNativeEffect::kTakenOver;
        }
        for (uint32_t n = 0; n < out.opCount; ++n)
        {
            const GSNativeEffect::Op &op = out.ops[n];
            const GSNativeEffect::Buffer &buffer = out.buffers[op.target];
            if (op.zbuf != 0u && buffer.external != 1u)
                return fail("a depth test into an effect's own buffer");
            if (op.flags & GSNativeEffect::kGreenToAlpha)
            {
                if (buffer.external == 1u || strips[n] != buffer.fbw * 64u / 16u ||
                    op.rect[3] < static_cast<float>(buffer.height))
                    return fail("a CT16 channel copy of part of a buffer");
                continue;
            }
            if (buffer.external != 1u)
                continue;
            if (const char *problem = intoGs(op))
                return fail(problem);
        }
        return !failed;
    }

    // Why the pipeline cannot draw a sprite into a GS target, or null.
    const char *intoGs(const GSNativeEffect::Op &op) const
    {
        if (op.flags & GSNativeEffect::kGreenToAlpha)
            return "a CT16 channel copy of a GS buffer";
        if (op.source >= 0 && static_cast<uint32_t>(op.source) == op.target)
            return "a sprite into a GS target from itself";
        if ((op.test & 1u) && (!tail || ((op.test >> 12) & 3u) != 0u))
            return "a sprite into a GS target with the alpha test";
        if (!(op.flags & GSNativeEffect::kBlend))
            return nullptr;
        // (A - B) C + D with D = Cd and C not Ad: A = Cs and B = 0 (an
        // add), A = 0 and B = Cs (a subtract), A = Cs and B = Cd (laid
        // over, its alpha masked: the pipeline blends by the shader's).
        const uint32_t ab = static_cast<uint32_t>(op.alpha & 0xFu);
        const uint32_t c = static_cast<uint32_t>((op.alpha >> 4) & 3u);
        if (((op.alpha >> 6) & 3u) != 1u || c == 1u || (ab != 0x8u && ab != 0x2u && ab != 0x4u))
            return "a sprite into a GS target with a blend it cannot draw";
        if (ab == 0x4u && !(op.flags & GSNativeEffect::kKeepAlpha) && !tail)
            return "a sprite laid over a GS target that writes its alpha";
        return nullptr;
    }
};

} // namespace


const char *name(Effect effect)
{
    switch (effect)
    {
    case Effect::Glare: return "glare";
    case Effect::Shadow: return "shadow";
    case Effect::CBlur: return "cblur";
    case Effect::Bloom: return "bloom";
    case Effect::Dof: return "dof";
    case Effect::CBlurDraw: return "cblur draw";
    case Effect::Refraction: return "refraction";
    case Effect::ShadowSetup: return "shadow setup";
    case Effect::CompositeHead: return "composite head";
    case Effect::CompositeTail: return "composite tail";
    case Effect::Flip: return "flip";
    case Effect::ColorFilter: return "color filter";
    case Effect::Focus: return "focus";
    case Effect::ShadowMapBlur: return "shadow map blur";
    default: return "none";
    }
}

bool compositesBatched()
{
    return enabled() && g_composites;
}

bool enabled()
{
    static const bool on = [] {
        if (!rg::nativeproducer::pipelineOn())
            return false;
        const auto flag = [](const char *name) {
            const char *value = std::getenv(name);
            return !(value && std::strcmp(value, "0") == 0);
        };
        g_log = std::getenv("RG_NATIVE_EFFECTS_LOG") != nullptr;
        g_glare = flag("RG_NATIVE_GLARE");
        g_shadow = flag("RG_NATIVE_SHADOW");
        g_cblur = flag("RG_NATIVE_CBLUR");
        g_bloom = flag("RG_NATIVE_BLOOM");
        g_dof = flag("RG_NATIVE_DOF");
        g_refraction = flag("RG_NATIVE_REFRACTION");
        g_flip = flag("RG_NATIVE_FLIP");
        g_colorFilter = flag("RG_NATIVE_COLOR_FILTER");
        g_focus = flag("RG_NATIVE_FOCUS");
        g_shadowMap = flag("RG_NATIVE_SHADOW_MAP");
        g_composites = flag("RG_NATIVE_COMPOSITE");
        return g_composites || g_glare || g_shadow || g_cblur || g_bloom || g_dof || g_refraction || g_flip || g_colorFilter ||
               g_focus || g_shadowMap;
    }();
    return on;
}

void install(PS2Runtime &runtime)
{
    if (!enabled())
    {
        return;
    }
    g_rdram = runtime.memory().getRDRAM();
    if (!rg::timing::hookFunction(runtime, kDrawListFrameBegin, &drawListFrameBegin, g_frameBeginOriginal))
    {
        std::fprintf(stderr, "[nativefx] the effects stay GS packets\n");
        return;
    }
    if (g_glare && !rg::timing::hookFunction(runtime, kGlareFilterDraw, &glareFilterDraw, g_glareOriginal))
    {
        std::fprintf(stderr, "[nativefx] the glare stays GS packets\n");
    }
    if (g_shadow && (!rg::timing::hookFunction(runtime, kPlaneShadowBegin, &planeShadowBegin, g_shadowBeginOriginal) ||
                     !rg::timing::hookFunction(runtime, kPlaneShadowEnd, &planeShadowEnd, g_shadowOriginal)))
    {
        std::fprintf(stderr, "[nativefx] the plane shadow stays GS packets\n");
    }
    if (g_cblur && (!rg::timing::hookFunction(runtime, kCBlurTexBlur, &cblurTexBlur, g_cblurOriginal) ||
                    !rg::timing::hookFunction(runtime, kCBlurDraw, &cblurDraw, g_cblurDrawOriginal)))
    {
        std::fprintf(stderr, "[nativefx] the CBlur stays GS packets\n");
    }
    if (g_bloom && !rg::timing::hookFunction(runtime, kBloom, &bloomExtractBlur, g_bloomOriginal))
    {
        std::fprintf(stderr, "[nativefx] the bloom stays GS packets\n");
    }
    if (g_dof && !rg::timing::hookFunction(runtime, kDof, &dofFilterDraw, g_dofOriginal))
    {
        std::fprintf(stderr, "[nativefx] the depth of field stays GS packets\n");
    }
    if (g_refraction && !rg::timing::hookFunction(runtime, kRefraction, &refractionCaptureFrame, g_refractionOriginal))
    {
        std::fprintf(stderr, "[nativefx] the refraction's capture stays GS packets\n");
    }
    if (g_focus && !rg::timing::hookFunction(runtime, kFocusFilter, &focusFilterDraw, g_focusOriginal))
    {
        std::fprintf(stderr, "[nativefx] the focus filter stays GS packets\n");
    }
    if (g_shadowMap && !rg::timing::hookFunction(runtime, kShadowMapBlur, &shadowMapBlur, g_shadowMapBlurOriginal))
    {
        std::fprintf(stderr, "[nativefx] the shadow map's blur stays GS packets\n");
    }
    if (debugColorFilter() != 0u)
        rg::timing::hookFunction(runtime, kGameSceneTintApply, &gameSceneTintApply, g_tintApplyOriginal);
    if (g_composites &&
        (!rg::timing::hookFunction(runtime, kCompositeAt, &compositeAt, g_compositeOriginal) ||
         !rg::timing::hookFunction(runtime, kModelEmitDrawPackets, &modelEmitDrawPackets, g_emitOriginal)))
    {
        g_composites = false;
        std::fprintf(stderr, "[nativefx] the composites stay GS packets\n");
    }
    if (g_colorFilter &&
        !rg::timing::hookFunction(runtime, kColorFilter, &colorFilter, g_colorFilterOriginal))
    {
        std::fprintf(stderr, "[nativefx] the colour filter stays GS packets\n");
    }
    if (g_flip && !rg::timing::hookFunction(runtime, kSceGsPutDrawEnv, &putDrawEnv, g_putDrawEnvOriginal))
    {
        g_flip = false;
        std::fprintf(stderr, "[nativefx] the flip's copy stays GS packets\n");
    }
}

bool flipNative()
{
    return enabled() && g_flip;
}

namespace
{
// Game thread: inside the flip, and the last packet it sent there (the draw
// environment the copy's sprites are drawn under).
thread_local bool t_inFlip = false;
thread_local std::vector<uint8_t> t_flipEnvironment;

// GfxFlipBuffersMaybe's sprite packet: its blend, test and PRMODE, and
// sprites; nothing else the game sends from its thread looks so.
bool isFlipCopy(const uint8_t *data, uint32_t size)
{
    rg::gif::GifState state;
    std::vector<rg::gif::GifEvent> events;
    rg::gif::decodeGifPacket(data, size, state, events);
    bool alpha = false, test = false, prmode = false, sprite = false;
    for (const rg::gif::GifEvent &event : events)
    {
        if (event.kind == rg::gif::GifEvent::Kind::Register)
        {
            alpha |= event.reg == kAlpha && event.value == 0x8000000064ull;
            test |= event.reg == kTest && event.value == 0x20002ull;
            prmode |= event.reg == kPrmode && event.value == 0x110ull;
            continue;
        }
        sprite |= (event.prim & 7u) == 6u && event.drawing;
    }
    return alpha && test && prmode && sprite;
}

bool writesFrame(const std::vector<uint8_t> &packet)
{
    rg::gif::GifState state;
    std::vector<rg::gif::GifEvent> events;
    rg::gif::decodeGifPacket(packet.data(), static_cast<uint32_t>(packet.size()), state, events);
    for (const rg::gif::GifEvent &event : events)
        if (event.kind == rg::gif::GifEvent::Kind::Register && (event.reg == kFrame || event.reg == kFrame + 1u))
            return true;
    return false;
}
} // namespace

// sceGsPutDrawEnv is the runtime's (Kernel/Stubs/GS.cpp): it writes its eight
// register pairs into the GS and sends no packet. Inside the flip, the pairs
// as an A+D packet stand for the draw environment the copy is drawn under:
// the context registers alone (the first "pair" is the environment's GIF
// tag, which the stub reads as one).
namespace
{
void putDrawEnv(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (t_inFlip && g_rdram)
    {
        const uint32_t address = getRegU32(ctx, 4);
        std::vector<uint8_t> packet(16u);
        uint32_t count = 0u;
        for (uint32_t i = 0u; i < 8u; ++i)
        {
            uint64_t pair[2];
            std::memcpy(pair, g_rdram + physical(address + i * 16u), sizeof(pair));
            const uint64_t reg = pair[1];
            if (reg < 0x14u || reg > 0x63u)
                continue;
            const uint8_t *bytes = reinterpret_cast<const uint8_t *>(pair);
            packet.insert(packet.end(), bytes, bytes + 16u);
            ++count;
        }
        // NLOOP pairs, EOP, PACKED, one register: A+D.
        const uint64_t tag[2] = {static_cast<uint64_t>(count) | (1ull << 15) | (1ull << 60), 0xEull};
        std::memcpy(packet.data(), tag, sizeof(tag));
        if (count != 0u)
            t_flipEnvironment = std::move(packet);
    }
    g_putDrawEnvOriginal(rdram, ctx, runtime);
}
} // namespace

void beginFlip()
{
    t_inFlip = flipNative();
    t_flipEnvironment.clear();
}

void endFlip()
{
    t_inFlip = false;
    t_flipEnvironment.clear();
}

bool flipPacket(const uint8_t *data, uint32_t size, GSNativeEffect &out,
                std::vector<std::vector<uint8_t>> &withheld)
{
    if (!t_inFlip)
        return false;
    if (!isFlipCopy(data, size))
    {
        t_flipEnvironment.assign(data, data + size);
        return false;
    }
    std::string why;
    bool built = false;
    if (t_flipEnvironment.empty() || !writesFrame(t_flipEnvironment))
    {
        why = "no draw environment before it";
    }
    else
    {
        // The environment went on already: its registers are what the
        // builder needs, and it sends the sprites' packet alone.
        const std::vector<std::vector<uint8_t>> packets{t_flipEnvironment, std::vector<uint8_t>(data, data + size)};
        built = build(Effect::Flip, packets, out, withheld, why);
        if (built)
            withheld.erase(withheld.begin());
    }
    t_flipEnvironment.clear();
    if (!built)
    {
        static bool told = false;
        if (!told)
        {
            told = true;
            std::fprintf(stderr, "[nativefx] the flip's copy stays GS packets: %s\n", why.c_str());
        }
    }
    return built;
}

Place placeAt(uint32_t address)
{
    address = physical(address);
    std::lock_guard lock(g_mutex);
    auto it = std::upper_bound(g_ranges.begin(), g_ranges.end(), address,
                               [](uint32_t value, const Range &range) { return value < range.start; });
    if (it == g_ranges.begin())
        return Place{};
    --it;
    return address < it->end ? it->place : Place{};
}

Effect effectAt(uint32_t address)
{
    return placeAt(address).effect;
}

bool clearBox(const std::vector<std::vector<uint8_t>> &packets, Box &box)
{
    Registers registers;
    rg::gif::GifState state;
    std::vector<rg::gif::GifEvent> events;
    bool found = false;
    uint32_t queued = 0u;
    rg::gif::GifEvent first;
    for (const std::vector<uint8_t> &packet : packets)
    {
        events.clear();
        rg::gif::eachPacket(packet.data(), static_cast<uint32_t>(packet.size()),
                   [&](uint32_t at, uint32_t size) { rg::gif::decodeGifPacket(packet.data() + at, size, state, events); });
        for (const rg::gif::GifEvent &event : events)
        {
            if (event.kind == rg::gif::GifEvent::Kind::Register)
            {
                if (event.reg == 0x00u)
                    queued = 0u;
                registers.write(event.reg, event.value);
                continue;
            }
            if ((event.prim & 7u) != 6u || ((event.prim >> 4) & 1u))
                continue;
            if (++queued < 2u)
            {
                first = event;
                continue;
            }
            queued = 0u;
            const uint32_t ctxt = (event.prim >> 9) & 1u;
            const uint64_t frame = registers.value[kFrame + ctxt];
            const uint64_t offset = registers.value[kXyoffset + ctxt];
            const uint32_t base = static_cast<uint32_t>(frame & 0x1FFu) << 5u;
            const uint32_t fbw = static_cast<uint32_t>((frame >> 16) & 0x3Fu);
            if (((frame >> 24) & 0x3Fu) != 0u || (found && (base != box.base || fbw != box.fbw)))
                return false;
            const int32_t ox = static_cast<int32_t>(offset & 0xFFFFu), oy = static_cast<int32_t>((offset >> 32) & 0xFFFFu);
            const int32_t x0 = (std::min<int32_t>(first.x, event.x) - ox) >> 4;
            const int32_t y0 = (std::min<int32_t>(first.y, event.y) - oy) >> 4;
            const int32_t x1 = (std::max<int32_t>(first.x, event.x) - ox + 15) >> 4;
            const int32_t y1 = (std::max<int32_t>(first.y, event.y) - oy + 15) >> 4;
            if (!found)
                box = Box{base, fbw, x0, y0, x1, y1};
            box.x0 = std::min(box.x0, x0);
            box.y0 = std::min(box.y0, y0);
            box.x1 = std::max(box.x1, x1);
            box.y1 = std::max(box.y1, y1);
            found = true;
        }
    }
    return found && box.fbw != 0u;
}

bool uploadPages(const uint8_t *data, uint32_t size, uint32_t &first, uint32_t &last)
{
    // An upload's registers come as one A+D packet: BITBLTBUF, TRXPOS,
    // TRXREG, TRXDIR.
    if (size < 32u)
        return false;
    uint64_t tag, regs;
    std::memcpy(&tag, data, sizeof(tag));
    std::memcpy(&regs, data + 8, sizeof(regs));
    if ((tag >> 58) & 3u || (tag >> 60) != 1u || (regs & 0xFu) != kAd)
        return false;
    const uint32_t nloop = static_cast<uint32_t>(tag & 0x7FFFu);
    uint64_t bitblt = 0u, pos = 0u, reg = 0u;
    bool host = false;
    for (uint32_t i = 0; i < nloop && 32u + i * 16u <= size; ++i)
    {
        uint64_t value, address;
        std::memcpy(&value, data + 16u + i * 16u, sizeof(value));
        std::memcpy(&address, data + 24u + i * 16u, sizeof(address));
        switch (address & 0xFFu)
        {
        case 0x50u: bitblt = value; break;
        case 0x51u: pos = value; break;
        case 0x52u: reg = value; break;
        case 0x53u: host = (value & 3u) == 0u; break;
        default: break;
        }
    }
    if (!host)
        return false;
    const uint32_t dbp = static_cast<uint32_t>((bitblt >> 32) & 0x3FFFu);
    const uint32_t dbw = std::max<uint32_t>(static_cast<uint32_t>((bitblt >> 48) & 0x3Fu), 1u);
    const uint32_t dpsm = static_cast<uint32_t>((bitblt >> 56) & 0x3Fu);
    const uint32_t dsay = static_cast<uint32_t>((pos >> 48) & 0x7FFu);
    const uint32_t rrh = std::max<uint32_t>(static_cast<uint32_t>((reg >> 32) & 0xFFFu), 1u);
    // A page's size in pixels by format: 64 x 32 (32 and 24 bits), 64 x 64
    // (16), 128 x 64 (8), 128 x 128 (4); a row of pages spans the width.
    const bool four = dpsm == 0x14u || dpsm == 0x24u || dpsm == 0x2Cu;
    const bool eight = dpsm == 0x13u || dpsm == 0x1Bu;
    const bool sixteen = dpsm == 0x02u || dpsm == 0x0Au || dpsm == 0x30u || dpsm == 0x31u || dpsm == 0x32u ||
                         dpsm == 0x3Au;
    const uint32_t pageW = (four || eight) ? 128u : 64u;
    const uint32_t pageH = four ? 128u : (eight || sixteen) ? 64u : 32u;
    const uint32_t perRow = std::max<uint32_t>(dbw * 64u / pageW, 1u);
    first = dbp / 32u + (dsay / pageH) * perRow;
    last = dbp / 32u + ((dsay + rrh - 1u) / pageH) * perRow + perRow - 1u;
    return true;
}

void dumpPackets(const char *what, const std::vector<std::vector<uint8_t>> &packets)
{
    rg::gif::GifState state;
    std::vector<rg::gif::GifEvent> events;
    for (const std::vector<uint8_t> &packet : packets)
    {
        events.clear();
        rg::gif::eachPacket(packet.data(), static_cast<uint32_t>(packet.size()),
                   [&](uint32_t at, uint32_t size) { rg::gif::decodeGifPacket(packet.data() + at, size, state, events); });
        std::fprintf(stderr, "[%s] packet %zu bytes:", what, packet.size());
        for (const rg::gif::GifEvent &event : events)
        {
            if (event.kind == rg::gif::GifEvent::Kind::Register)
                std::fprintf(stderr, " %02x=%llx", event.reg, static_cast<unsigned long long>(event.value));
            else
                std::fprintf(stderr, " V(prim %x %d,%d z %u uv %d,%d rgba %02x%02x%02x%02x%s)", event.prim, event.x, event.y, event.z,
                             event.u, event.v, event.rgba[0], event.rgba[1], event.rgba[2], event.rgba[3],
                             event.drawing ? " kick" : "");
        }
        std::fprintf(stderr, "\n");
    }
}

void observe(Effect effect, const uint8_t *data, uint32_t size)
{
    if (effect != Effect::ShadowSetup)
        return;
    thread_local std::vector<rg::gif::GifEvent> events;
    rg::gif::GifState state;
    events.clear();
    rg::gif::eachPacket(data, size, [&](uint32_t at, uint32_t piece) { rg::gif::decodeGifPacket(data + at, piece, state, events); });
    for (const rg::gif::GifEvent &event : events)
        if (event.kind == rg::gif::GifEvent::Kind::Register)
            t_shadowSetup.write(event.reg, event.value);
    if (g_log)
    {
        static int shown = 0;
        if (shown++ < 3)
        {
            std::fprintf(stderr, "[nativefx] shadow setup packet, %u bytes:", size);
            for (const rg::gif::GifEvent &event : events)
                if (event.kind == rg::gif::GifEvent::Kind::Register)
                    std::fprintf(stderr, " %02x=%llx", event.reg, static_cast<unsigned long long>(event.value));
            std::fprintf(stderr, "\n");
        }
    }
}

namespace
{
// The colour filter from its arguments (the unit), not its sprites: the
// packets give only the frame they end on (FRAME and SCISSOR, context 1,
// last written: the draw environment put back for the second pass). Two
// ops into the frame, taken over: its alpha from its green (kGreenToAlpha,
// as the CT16 view's copy leaves it), then (r, g, b, 0x80) with ALPHA
// A = Cs, B = 0, C = Ad, D = 0.
bool buildColorFilter(const std::vector<std::vector<uint8_t>> &packets, uint32_t unit, GSNativeEffect &out,
                      std::vector<std::vector<uint8_t>> &withheld, std::string &why)
{
    uint64_t frame = 0u, scissor = 0u;
    bool haveFrame = false, haveScissor = false;
    rg::gif::GifState state;
    std::vector<rg::gif::GifEvent> events;
    for (const std::vector<uint8_t> &packet : packets)
    {
        rg::gif::eachPacket(packet.data(), static_cast<uint32_t>(packet.size()), [&](uint32_t at, uint32_t size) {
            events.clear();
            rg::gif::decodeGifPacket(packet.data() + at, size, state, events);
            for (const rg::gif::GifEvent &event : events)
            {
                if (event.kind != rg::gif::GifEvent::Kind::Register)
                    continue;
                if (event.reg == kFrame)
                {
                    frame = event.value;
                    haveFrame = true;
                }
                else if (event.reg == kScissor)
                {
                    scissor = event.value;
                    haveScissor = true;
                }
            }
        });
    }
    if (!haveFrame || !haveScissor)
    {
        why = "no draw environment among its packets";
        return false;
    }
    if (((frame >> 24) & 0x3Fu) != 0u || (frame >> 32) != 0u)
    {
        why = "a frame not CT32, or masked";
        return false;
    }
    const uint32_t width = static_cast<uint32_t>((scissor >> 16) & 0x7FFu) + 1u;
    const uint32_t height = static_cast<uint32_t>((scissor >> 48) & 0x7FFu) + 1u;
    out.bufferCount = 1u;
    GSNativeEffect::Buffer &buffer = out.buffers[0];
    buffer.base = static_cast<uint32_t>(frame & 0x1FFu) << 5u;
    buffer.fbw = static_cast<uint32_t>((frame >> 16) & 0x3Fu);
    buffer.psm = 0u;
    buffer.width = std::max(width, buffer.fbw * 64u);
    buffer.height = height;
    buffer.external = GSNativeEffect::kTakenOver;
    const auto whole = [&](GSNativeEffect::Op &op) {
        op.target = 0u;
        op.source = -1;
        op.rect[2] = static_cast<float>(width);
        op.rect[3] = static_cast<float>(height);
        op.scissor[2] = width - 1u;
        op.scissor[3] = height - 1u;
    };
    out.opCount = 2u;
    GSNativeEffect::Op &alpha = out.ops[0];
    whole(alpha);
    alpha.flags = GSNativeEffect::kGreenToAlpha | GSNativeEffect::kColClamp;
    GSNativeEffect::Op &tint = out.ops[1];
    whole(tint);
    tint.flags = GSNativeEffect::kBlend | GSNativeEffect::kColClamp;
    tint.alpha = 0x98u; // A = Cs, B = 0, C = Ad, D = 0; FIX 0
    tint.rgba[0] = static_cast<uint8_t>(unit & 0xFFu);
    tint.rgba[1] = static_cast<uint8_t>((unit >> 8) & 0xFFu);
    tint.rgba[2] = static_cast<uint8_t>((unit >> 16) & 0xFFu);
    tint.rgba[3] = 0x80u;
    for (const std::vector<uint8_t> &packet : packets)
    {
        withheld.push_back(packet);
        withholdKicks(withheld.back());
    }
    if (g_log)
    {
        static bool shown = false;
        if (!shown)
        {
            shown = true;
            std::fprintf(stderr, "[nativefx] color filter: %02x%02x%02x over %05x fbw %u %ux%u\n", tint.rgba[0],
                         tint.rgba[1], tint.rgba[2], buffer.base, buffer.fbw, width, height);
        }
    }
    return true;
}
} // namespace

bool build(Effect effect, const std::vector<std::vector<uint8_t>> &packets, GSNativeEffect &out,
           std::vector<std::vector<uint8_t>> &withheld, std::string &why, uint32_t unit)
{
    out = GSNativeEffect{};
    withheld.clear();
    if (effect == Effect::ColorFilter)
        return buildColorFilter(packets, unit, out, withheld, why);
    if (effect != Effect::Glare && effect != Effect::Shadow && effect != Effect::CBlur && effect != Effect::Bloom &&
        effect != Effect::Dof && effect != Effect::CBlurDraw && effect != Effect::Refraction &&
        effect != Effect::CompositeTail && effect != Effect::Flip && effect != Effect::Focus &&
        effect != Effect::ShadowMapBlur)
    {
        why = "no such effect";
        return false;
    }
    Builder builder{out, why};
    builder.tail = effect == Effect::CompositeTail;
    builder.flip = effect == Effect::Flip;
    static const bool traceShadow = std::getenv("RG_NATIVE_EFFECTS_TRACE") != nullptr;
    static int traced[16] = {};
    builder.trace = traceShadow &&
                    (effect == Effect::Shadow || effect == Effect::Bloom || effect == Effect::Dof || effect == Effect::CBlurDraw || effect == Effect::Refraction ||
                     effect == Effect::CompositeTail) &&
                    traced[static_cast<int>(effect)]++ == 0;
    Registers registers = effect == Effect::Shadow ? t_shadowSetup : Registers{};
    rg::gif::GifState state;
    std::vector<rg::gif::GifEvent> events;
    uint32_t queued = 0u;
    rg::gif::GifEvent first;
    std::array<rg::gif::GifEvent, 4> strip{};
    bool pendingQuad = false;
    bool ok = true;
    // Each DIRECT on its own, as GS::processGIFPacket takes them: a tag cut
    // at a DIRECT's end loses the rest of its data (the glare's first A+D
    // tag counts 14 and its DIRECT holds 8), and the next starts at a tag.
    uint32_t cut = 0u;
    for (const std::vector<uint8_t> &packet : packets)
    {
        if (!rg::gif::eachPacket(packet.data(), static_cast<uint32_t>(packet.size()), [&](uint32_t at, uint32_t size) {
                events.clear();
                rg::gif::decodeGifPacket(packet.data() + at, size, state, events);
                for (const rg::gif::GifEvent &event : events)
                {
                    if (!ok)
                        return;
                    if (event.kind == rg::gif::GifEvent::Kind::Register)
                    {
                        if (event.reg == 0x00u)
                        {
                            if (pendingQuad)
                                ok = builder.fail("a triangle strip ending in half a quad");
                            queued = 0u;
                            pendingQuad = false;
                        }
                        else if (event.reg == 0x53u || event.reg == 0x50u)
                            ok = builder.fail("a transfer among the sprites");
                        registers.write(event.reg, event.value);
                        continue;
                    }
                    const bool attributes = !registers.set[kPrmodecont] || (registers.value[kPrmodecont] & 1u);
                    const uint16_t prim =
                        attributes ? event.prim
                                   : static_cast<uint16_t>((event.prim & 7u) | (registers.value[kPrmode] & 0x7F8u));
                    if ((prim & 7u) == 4u)
                    {
                        // A triangle strip of axis-aligned quads, two
                        // triangles each, top and bottom vertices in turn
                        // (the depth of field's lay back): each quad a sprite.
                        if (queued < 4u)
                            strip[queued] = event;
                        else
                        {
                            strip[0] = strip[1];
                            strip[1] = strip[2];
                            strip[2] = strip[3];
                            strip[3] = event;
                        }
                        ++queued;
                        // Odd counts from three on end a quad's first triangle.
                        pendingQuad = queued >= 3u && queued % 2u == 1u;
                        if (queued < 4u || pendingQuad)
                            continue;
                        const rg::gif::GifEvent &a = strip[0], &b = strip[1], &c = strip[2], &d = strip[3];
                        // Z by column: a quad's two triangles then
                        // interpolate it in x alone.
                        const bool quad = a.x == b.x && c.x == d.x && a.y == c.y && b.y == d.y && a.u == b.u &&
                                          c.u == d.u && a.v == c.v && b.v == d.v && a.z == b.z && c.z == d.z &&
                                          std::memcmp(a.rgba, d.rgba, 4) == 0 &&
                                          std::memcmp(b.rgba, d.rgba, 4) == 0 && std::memcmp(c.rgba, d.rgba, 4) == 0;
                        if (!quad)
                        {
                            ok = builder.fail("a triangle strip other than axis-aligned quads");
                            return;
                        }
                        builder.quadZ = true;
                        ok = builder.sprite(registers, static_cast<uint16_t>((prim & ~7u) | 6u), a, d);
                        builder.quadZ = false;
                        continue;
                    }
                    if ((prim & 7u) != 6u)
                    {
                        ok = builder.fail("a primitive other than a sprite");
                        return;
                    }
                    if (++queued < 2u)
                    {
                        first = event;
                        continue;
                    }
                    queued = 0u;
                    if (event.drawing)
                        ok = builder.sprite(registers, prim, first, event);
                }
            }))
        {
            ++cut;
        }
        if (!ok)
            return false;
    }
    if (pendingQuad)
        builder.fail("a triangle strip ending in half a quad");
    for (const std::vector<uint8_t> &packet : packets)
    {
        withheld.push_back(packet);
        withholdKicks(withheld.back());
    }
    if (!builder.finish() || builder.failed)
        return false;
    if (g_log)
    {
        static bool shown[16] = {};
        if (!shown[static_cast<int>(effect)])
        {
            shown[static_cast<int>(effect)] = true;
            std::fprintf(stderr, "[nativefx] %s: %u buffers, %u sprites, %u DIRECTs ending inside a GIF tag\n",
                         name(effect), out.bufferCount, out.opCount, cut);
            for (uint32_t i = 0; i < out.bufferCount; ++i)
            {
                const auto &b = out.buffers[i];
                std::fprintf(stderr, "[nativefx]   buffer %u: %05x fbw %u %ux%u%s\n", i, b.base, b.fbw, b.width,
                             b.height,
                             b.external == GSNativeEffect::kTakenOver ? " (the GS's, taken over)"
                             : b.external                             ? " (the GS's)"
                                                                      : "");
            }
            for (uint32_t n = 0; n < out.opCount; ++n)
            {
                const auto &op = out.ops[n];
                std::fprintf(stderr,
                             "[nativefx]   %2u: %u <- %d rect %g,%g-%g,%g uv %g,%g-%g,%g rgba %02x%02x%02x%02x "
                             "flags %x alpha %llx test %llx\n",
                             n, op.target, op.source, op.rect[0], op.rect[1], op.rect[2], op.rect[3], op.uv[0],
                             op.uv[1], op.uv[2], op.uv[3], op.rgba[0], op.rgba[1], op.rgba[2], op.rgba[3], op.flags,
                             static_cast<unsigned long long>(op.alpha), static_cast<unsigned long long>(op.test));
            }
        }
    }
    return true;
}



} // namespace rg::nativefx
