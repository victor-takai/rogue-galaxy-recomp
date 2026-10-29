#include "sharp_letters.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "button_art.h"
#include "font_trace.h"
#include "ps2_runtime.h"
#include "runtime/ps2_memory.h"
#include "scene_timing.h"

#if defined(RG_HAS_SDLGPU)
#include "gfx/backends/sdlgpu/sdlgpu_backend.h"
#endif

// raylib's stb_image_write, which the runtime links in.
extern "C" int stbi_write_png(const char *filename, int w, int h, int comp, const void *data, int stride_in_bytes);

namespace rg::letters
{
namespace
{

// FontDrawGlyphOutline(style, packet, rect, uv, alpha) draws a glyph's
// border before the glyph: mode 1, the style's first byte, is four black
// copies a pixel up, down, right and left (D_0047E968), each through
// GsPktSpriteUVRect, whose GsPktSetXYZPixel adds the packet's offsets, in
// the GS's 12.4, to the whole pixels it is given. Mode 2 is a shadow: a
// grey copy a pixel down and right, then a black one two; mode 3 the black
// one alone (the field's prompts, some towns' dialogue). The others are
// borders too: 4 twelve black copies, the four of mode 1, the diagonals and
// two pixels out (D_0044EBB0); 5 black on the four diagonals, then dark grey
// (0x20) on mode 1's four (the dialogue); 6 black on all eight. Traced, the
// copies of mode 5 showed apart, a grey ring with black dots at the
// corners: like mode 1's they are drawn as one round border.
constexpr uint32_t kFontDrawGlyphOutline = 0x0026E450u;
constexpr int8_t kFourCopies = 1;
constexpr int8_t kTwoShadows = 2;
constexpr int8_t kOneShadow = 3;
constexpr int8_t kTwelveCopies = 4;
constexpr int8_t kDiagonalsThenSides = 5;
constexpr int8_t kEightCopies = 6;
constexpr uint32_t kPacketOffsetY = 0x124u;

std::atomic<bool> g_enabled{true};
// RG_SHARP_LETTERS, over the menu: -1 unset.
int g_forced = -1;
PS2Runtime::RecompiledFunction g_original = nullptr;
// RG_SHARP_LETTERS_TRACE=1 logs each text style's border mode and each
// glyph traced; =DIR also writes the glyphs as found and as made there;
// =palettes logs the palette and place of every small region drawn from an
// indexed texture, once, and makes nothing (finding a palette to add).
bool g_trace = false;
bool g_tracePalettes = false;
std::string g_traceDirectory;

void drawGlyphOutline(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint8_t *style = getConstMemPtr(rdram, getRegU32(ctx, 4));
    uint8_t *offset = getMemPtr(rdram, getRegU32(ctx, 5) + kPacketOffsetY);
    if (g_trace && style)
    {
        static std::set<std::pair<uint32_t, int>> seen;
        if (seen.insert({getRegU32(ctx, 4), static_cast<int8_t>(style[0])}).second)
        {
            std::fprintf(stderr, "[letters] style 0x%08X border mode %d (caller 0x%06X)\n", getRegU32(ctx, 4),
                         static_cast<int8_t>(style[0]), getRegU32(ctx, 31));
        }
    }
    const int8_t mode = style ? static_cast<int8_t>(style[0]) : 0;
    // A border whose first copy is a pixel up, one whose first is a pixel up
    // and left, or a shadow.
    const int32_t marks = mode == kFourCopies || mode == kTwelveCopies                ? 1
                          : mode == kDiagonalsThenSides || mode == kEightCopies       ? 3
                          : mode == kTwoShadows || mode == kOneShadow                 ? 2
                                                                                      : 0;
    if (!g_enabled.load(std::memory_order_relaxed) || !offset || marks == 0)
    {
        g_original(rdram, ctx, runtime);
        return;
    }
    // The border's copies a sixteenth of a pixel up (three when the first is
    // diagonal), a shadow's two: the marks the backend knows them by
    // (sdlgpu_backend.h, setRegionReplacements). At a pixel's centre, as the
    // GS samples, that changes no pixel and no texel.
    int32_t saved = 0;
    std::memcpy(&saved, offset, sizeof(saved));
    const int32_t marked = saved - marks;
    std::memcpy(offset, &marked, sizeof(marked));
    g_original(rdram, ctx, runtime);
    std::memcpy(offset, &saved, sizeof(saved));
}

#if defined(RG_HAS_SDLGPU)

dq8::gfx::SdlGpuBackend *g_backend = nullptr;

// The fonts, by the palette their glyphs are drawn with (its hash, as
// dq8-gfx's texture cache takes it), and how each is traced.
enum class Kind
{
    // White over 16 levels of alpha.
    White,
    // A grey core inside a dark border drawn into the glyph, both opaque,
    // the border's edge over the alpha levels.
    Bordered,
    // The special characters' picture (gaiji2.img): the Xbox's buttons and
    // the registered mark are drawn from their shapes (button_art.cpp), the
    // rest left as they are.
    Icons,
    // The loading screen's picture with "Press X" (button_glyphs.h), its
    // palette known once redrawn: the region with the button, enlarged, the
    // button drawn as the text's.
    Loading,
};
struct Font
{
    uint64_t palette;
    Kind kind;
    rg::fonttrace::Params params;
};
// Thin strokes (the narrow fonts are a texel or so wide) come out wobbly
// from the field's small errors, so they are smoothed harder and keep their
// full width.
constexpr rg::fonttrace::Params kNarrow{0.0f, 1.0f, 0.25f};
// The rounded letters as defined as the console's at 1x: corners from a
// sharper turn (40 degrees), the runs between them smoothed less and the
// nearly straight ones straight (the user's pick of six versions; less
// smoothing alone came out wobbly).
constexpr rg::fonttrace::Params kRounded{0.15f, 0.3f, 0.2f, 40.0f};
// gaiji2.img's palette (button_glyphs.cpp redraws only its indices).
constexpr uint64_t kGaijiPalette = 0x844F1E0969532C9Dull;
constexpr Font kFonts[] = {
    // The dialogue's and the menus' rounded letters, 18x22, bordered by
    // copies.
    {0x33A2CFEFAEC9F88Bull, Kind::White, kRounded},
    // The narrow letters, 16x20 and 16x18.
    {0xCFBAE4E31E471D6Eull, Kind::White, kNarrow},
    {0x75B5F042466BE787ull, Kind::White, kNarrow},
    // The menus' small letters, grey in a dark border.
    {0x1C80044CAB6BF201ull, Kind::Bordered, kNarrow},
    {kGaijiPalette, Kind::Icons, kNarrow},
};
constexpr Font kLoadingFont{0u, Kind::Loading, {}};
// Larger than a glyph: art that shares a palette with one.
constexpr uint32_t kLargestGlyph = 32u;
// The loading picture's "Press X": 128x24.
constexpr uint32_t kLargestLoading = 128u;

const Font *fontOf(uint64_t palette)
{
    for (const Font &font : kFonts)
    {
        if (font.palette == palette)
        {
            return &font;
        }
    }
    if (palette != 0u && palette == rg::buttons::loadingPalette())
    {
        return &kLoadingFont;
    }
    return nullptr;
}

// A glyph's picture to make: the region as the GS samples it, the scale of
// the target, and which version.
struct Job
{
    uint64_t key;
    const Font *font;
    uint32_t x, y, width, height, scale;
    bool border, shadow, bold;
    std::vector<uint8_t> rgba;
};
struct Result
{
    uint32_t width = 0u, height = 0u;
    std::vector<uint8_t> rgba;
};

std::mutex g_mutex;
std::condition_variable g_wake;
std::deque<Job> g_queue;
std::unordered_set<uint64_t> g_asked;
std::unordered_map<uint64_t, Result> g_ready;

// The GS's opaque alpha. Not a glyph's own highest: a thin one never
// reaches it, and its border came out half transparent.
constexpr float kOpaque = 128.0f;

// A draw past 0x80 alpha (dq8::gfx::RegionRequest::bold) shows every texel
// at a quarter coverage or more half opaque, about a quarter texel past the
// half-covered line; the game's menus draw their letters so.
constexpr float kBolder = 0.25f;

uint8_t level(float coverage)
{
    return static_cast<uint8_t>(std::clamp(coverage, 0.0f, 1.0f) * kOpaque + 0.5f);
}

int luminance(const uint8_t *texel)
{
    return (texel[0] * 299 + texel[1] * 587 + texel[2] * 114) / 1000;
}

// The larger picture of a glyph, or an empty one for a region with nothing
// to trace (a space).
Result make(const Job &job)
{
    const uint32_t w = job.width, h = job.height, s = job.scale;
    if (job.font->kind == Kind::Icons)
    {
        Result result;
        if (!job.border && !job.shadow &&
            rg::buttons::art::drawLarger(job.x, job.y, w, h, job.rgba.data(), s, result.rgba))
        {
            result.width = w * s;
            result.height = h * s;
        }
        else
        {
            result.rgba.clear();
        }
        return result;
    }
    if (job.font->kind == Kind::Loading)
    {
        Result result;
        if (rg::buttons::art::drawLoadingLarger(job.x, job.y, w, h, job.rgba.data(), s,
                                                rg::buttons::loadingCrossCell(), result.rgba))
        {
            result.width = w * s;
            result.height = h * s;
        }
        else
        {
            result.rgba.clear();
        }
        return result;
    }
    const size_t texels = size_t(w) * h;
    // The coverage as the alpha levels say it, full at the GS's 0x80.
    std::vector<float> outer(texels);
    uint8_t full = 0u;
    // The colour of its most covered texel.
    const uint8_t *opaque = nullptr;
    for (size_t i = 0; i < texels; ++i)
    {
        const uint8_t *texel = &job.rgba[i * 4u];
        outer[i] = std::min(255, texel[3] * 2) / 255.0f;
        if (texel[3] > full)
        {
            full = texel[3];
            opaque = texel;
        }
    }
    Result result;
    if (!opaque)
    {
        return result;
    }
    // The border goes about the bolder outline: under a glyph drawn bold
    // they meet without a gap, and about one drawn as it is it reaches as
    // far as a pen a pixel wide about that one.
    rg::fonttrace::Params params = job.font->params;
    if (job.bold || job.border)
    {
        params.thin -= kBolder;
    }
    const std::vector<rg::fonttrace::Outline> outlines = rg::fonttrace::trace(outer, int(w), int(h), params);
    if (outlines.empty())
    {
        return result;
    }
    if (job.border)
    {
        // Black where the pen reaches; the copies' own colour and alpha
        // come from their vertices.
        constexpr int kMargin = static_cast<int>(dq8::gfx::kRegionBorderMargin);
        const std::vector<float> inside = rg::fonttrace::raster(outlines, int(w), int(h), int(s), kMargin);
        const std::vector<float> ring =
            rg::fonttrace::grown(outlines, inside, int(w), int(h), int(s), kMargin, 1.0f - kBolder);
        result.width = (w + 2u * kMargin) * s;
        result.height = (h + 2u * kMargin) * s;
        result.rgba.assign(ring.size() * 4u, 0u);
        for (size_t i = 0; i < ring.size(); ++i)
        {
            result.rgba[i * 4u + 3u] = level(ring[i]);
        }
        return result;
    }
    if (job.shadow)
    {
        // The glyph swept a texel back up and left: each copy reaches the
        // place of the one a pixel before it (the glyph's own for the
        // first), so the shadow comes out whole, as the GS's blocks join,
        // not as outlines a pixel apart. The colour is the glyph's, which
        // the copy's vertices tint.
        constexpr int kMargin = static_cast<int>(dq8::gfx::kRegionBorderMargin);
        const std::vector<float> cover = rg::fonttrace::raster(outlines, int(w), int(h), int(s), kMargin);
        result.width = (w + 2u * kMargin) * s;
        result.height = (h + 2u * kMargin) * s;
        result.rgba.assign(cover.size() * 4u, 0u);
        for (uint32_t y = 0; y < result.height; ++y)
        {
            for (uint32_t x = 0; x < result.width; ++x)
            {
                float swept = 0.0f;
                for (uint32_t step = 0; step <= s && x + step < result.width && y + step < result.height; ++step)
                {
                    swept = std::max(swept, cover[size_t(y + step) * result.width + x + step]);
                }
                uint8_t *pixel = &result.rgba[(size_t(y) * result.width + x) * 4u];
                std::memcpy(pixel, opaque, 3u);
                pixel[3] = level(swept);
            }
        }
        return result;
    }
    result.width = w * s;
    result.height = h * s;
    const std::vector<float> cover = rg::fonttrace::raster(outlines, int(w), int(h), int(s), 0);
    result.rgba.assign(cover.size() * 4u, 0u);
    if (job.font->kind == Kind::White)
    {
        for (size_t i = 0; i < cover.size(); ++i)
        {
            std::memcpy(&result.rgba[i * 4u], opaque, 3u);
            result.rgba[i * 4u + 3u] = level(cover[i]);
        }
        return result;
    }
    // The border's colour, the darkest seen, and the core's, the lightest
    // opaque one; the core traced from where the colour turns from one to
    // the other.
    const uint8_t *dark = opaque, *light = opaque;
    for (size_t i = 0; i < texels; ++i)
    {
        const uint8_t *texel = &job.rgba[i * 4u];
        if (texel[3] != 0u && luminance(texel) < luminance(dark))
        {
            dark = texel;
        }
        if (texel[3] + 4 >= full && luminance(texel) > luminance(light))
        {
            light = texel;
        }
    }
    const int darkLevel = luminance(dark), lightLevel = luminance(light);
    std::vector<float> core(texels, 0.0f);
    if (lightLevel > darkLevel)
    {
        for (size_t i = 0; i < texels; ++i)
        {
            const uint8_t *texel = &job.rgba[i * 4u];
            if (texel[3] + 4 >= full)
            {
                core[i] = std::clamp(float(luminance(texel) - darkLevel) / float(lightLevel - darkLevel), 0.0f, 1.0f);
            }
        }
    }
    const std::vector<float> inner =
        rg::fonttrace::raster(rg::fonttrace::trace(core, int(w), int(h), params), int(w), int(h), int(s), 0);
    for (size_t i = 0; i < cover.size(); ++i)
    {
        const float mix = std::min(inner[i], cover[i]);
        for (int c = 0; c < 3; ++c)
        {
            result.rgba[i * 4u + c] = static_cast<uint8_t>(dark[c] + (light[c] - dark[c]) * mix + 0.5f);
        }
        result.rgba[i * 4u + 3u] = level(cover[i]);
    }
    return result;
}

void worker()
{
    for (;;)
    {
        Job job;
        {
            std::unique_lock<std::mutex> lock(g_mutex);
            g_wake.wait(lock, [] { return !g_queue.empty(); });
            job = std::move(g_queue.front());
            g_queue.pop_front();
        }
        Result result = make(job);
        if (g_trace)
        {
            std::fprintf(stderr, "[letters] palette %016llX %ux%u at %ux%s%s: %s\n",
                         static_cast<unsigned long long>(job.font->palette), job.width, job.height, job.scale,
                         job.border ? ", border" : job.shadow ? ", shadow" : "", job.bold ? ", bold" : "",
                         result.rgba.empty() ? "nothing to trace" : "made");
        }
        if (!g_traceDirectory.empty() && !result.rgba.empty())
        {
            // Alpha doubled, so the GS's 0x80 is opaque.
            const auto write = [](const std::string &path, uint32_t w, uint32_t h, std::vector<uint8_t> rgba) {
                for (size_t i = 3u; i < rgba.size(); i += 4u)
                {
                    rgba[i] = static_cast<uint8_t>(std::min(255, rgba[i] * 2));
                }
                stbi_write_png(path.c_str(), int(w), int(h), 4, rgba.data(), int(w * 4u));
            };
            char name[96];
            std::snprintf(name, sizeof(name), "/%016llx_%016llx%s", static_cast<unsigned long long>(job.font->palette),
                          static_cast<unsigned long long>(job.key),
                          job.border ? "_border" : job.shadow ? "_shadow" : "");
            write(g_traceDirectory + name + "_in.png", job.width, job.height, job.rgba);
            write(g_traceDirectory + name + "_out.png", result.width, result.height, result.rgba);
        }
        std::lock_guard<std::mutex> lock(g_mutex);
        g_ready[job.key] = std::move(result);
    }
}

// On the GS thread: a glyph's picture once made, its making asked for the
// first time it is drawn.
int lookup(const dq8::gfx::RegionRequest &request, uint32_t &width, uint32_t &height, std::vector<uint8_t> &rgba)
{
    if (g_tracePalettes)
    {
        if (request.width <= kLargestLoading && request.height <= kLargestLoading)
        {
            std::fprintf(stderr, "[letters] palette %016llX region %u,%u %ux%u\n",
                         static_cast<unsigned long long>(request.clut), request.x, request.y, request.width,
                         request.height);
        }
        return dq8::gfx::kRegionNone;
    }
    const Font *font = fontOf(request.clut);
    const uint32_t largest = font && font->kind == Kind::Loading ? kLargestLoading : kLargestGlyph;
    if (!font || request.width > largest || request.height > largest || request.width < 2u || request.height < 2u)
    {
        return dq8::gfx::kRegionNone;
    }
    const uint32_t scale = std::min(request.scale, 8u);
    // A special character is what its place says as much as its texels.
    const uint64_t place = font->kind == Kind::Icons || font->kind == Kind::Loading
                               ? uint64_t(request.x) << 32u | request.y
                               : 0u;
    uint64_t key = 1469598103934665603ull;
    for (const uint64_t value : {request.texels, request.clut, uint64_t(request.width) << 32u | request.height,
                                 uint64_t(scale) << 3u | (request.shadow ? 4u : 0u) | (request.border ? 2u : 0u) |
                                     (request.bold ? 1u : 0u),
                                 place})
    {
        key ^= value;
        key *= 1099511628211ull;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    if (const auto ready = g_ready.find(key); ready != g_ready.end())
    {
        Result result = std::move(ready->second);
        g_ready.erase(ready);
        // Made again if the backend ever lets its copy go.
        g_asked.erase(key);
        if (result.rgba.empty())
        {
            return dq8::gfx::kRegionNone;
        }
        width = result.width;
        height = result.height;
        rgba = std::move(result.rgba);
        return dq8::gfx::kRegionReady;
    }
    if (g_asked.insert(key).second)
    {
        Job job{key,   font,           request.x,      request.y,    request.width, request.height,
                scale, request.border, request.shadow, request.bold, {}};
        job.rgba.assign(request.rgba, request.rgba + size_t(request.width) * request.height * 4u);
        g_queue.push_back(std::move(job));
        g_wake.notify_one();
    }
    return dq8::gfx::kRegionPending;
}

#endif

} // namespace

void install(PS2Runtime &runtime)
{
    if (const char *trace = std::getenv("RG_SHARP_LETTERS_TRACE"); trace && *trace && *trace != '0')
    {
        g_trace = true;
        if (std::strcmp(trace, "palettes") == 0)
        {
            g_tracePalettes = true;
        }
        else if (std::strcmp(trace, "1") != 0)
        {
            g_traceDirectory = trace;
        }
    }
    if (const char *enabled = std::getenv("RG_SHARP_LETTERS"); enabled && *enabled)
    {
        g_forced = *enabled != '0' ? 1 : 0;
        setEnabled(g_forced == 1);
    }
    if (!rg::timing::hookFunction(runtime, kFontDrawGlyphOutline, &drawGlyphOutline, g_original))
    {
        std::fprintf(stderr, "[rg] sharp letters keep the game's square borders\n");
    }
}

void installRenderer(dq8::gfx::SdlGpuBackend &backend)
{
#if defined(RG_HAS_SDLGPU)
    g_backend = &backend;
    // A glyph takes a few milliseconds at 8x: a page of new text, a hundred
    // or so, comes out in a frame or two over a few threads.
    const unsigned threads = std::clamp(std::thread::hardware_concurrency() / 4u, 1u, 3u);
    for (unsigned i = 0; i < threads; ++i)
    {
        std::thread(worker).detach();
    }
#else
    (void)backend;
#endif
}

void setEnabled(bool on)
{
    if (g_forced >= 0)
    {
        on = g_forced == 1;
    }
    g_enabled.store(on, std::memory_order_relaxed);
}

#if defined(RG_HAS_SDLGPU)
bool wantsRegion(uint64_t palette)
{
    return g_enabled.load(std::memory_order_relaxed) && (g_tracePalettes || fontOf(palette) != nullptr);
}

int lookupRegion(const dq8::gfx::RegionRequest &request, uint32_t &width, uint32_t &height,
                 std::vector<uint8_t> &rgba)
{
    return lookup(request, width, height, rgba);
}
#else
bool wantsRegion(uint64_t)
{
    return false;
}

int lookupRegion(const dq8::gfx::RegionRequest &, uint32_t &, uint32_t &, std::vector<uint8_t> &)
{
    return 0;
}
#endif

} // namespace rg::letters
