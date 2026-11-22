#include "typeface_letters.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <initializer_list>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "gfx/backends/sdlgpu/sdlgpu_backend.h"
#include "return_hook.h"
#include "runtime/ps2_memory.h"
#include "scene_timing.h"
#include "texture_pack.h"

// imgui's copy of stb_truetype, compiled here on its own (imgui keeps its
// own static).
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "../thirdparty/imgui/imstb_truetype.h"
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

// raylib's stb_image_write, which the runtime links in (the review sheets).
extern "C" int stbi_write_png(const char *filename, int w, int h, int comp, const void *data, int stride_in_bytes);

// The typefaces, inside the executable (cmake/embed_file.cmake).
extern const unsigned char rg_typeface_andika_regular[];
extern const size_t rg_typeface_andika_regular_size;
extern const unsigned char rg_typeface_andika_bold[];
extern const size_t rg_typeface_andika_bold_size;

namespace rg::typeface
{
namespace
{

// typeface_table.inc's rows.
struct TypefaceGlyph
{
    char character;
    uint16_t glyph;
    // Cell pixels from the console glyph's ink centre to the typeface's.
    float shift;
    // Cell pixels below the baseline (a sign set where the console's is).
    float lift;
};
struct TypefaceFont
{
    // The game font: the middle of its pages' names (FontTex_normal_EN_0.tm2).
    const char *name;
    uint32_t cellWidth, cellHeight;
    const char *file;
    // Cell pixels per em, across and down.
    float emX, emY;
    // The baseline's row, in cell pixels from the cell's top edge.
    float baseline;
    // The stroke growth, in the fit's steps (negative: thinner), and a step
    // in eighths of a cell pixel across and down (the fit grew the evenly
    // scaled render, which the cell then narrowed).
    int grow;
    float penX, penY;
    const TypefaceGlyph *glyphs;
    size_t count;
};

#include "typeface_table.inc"

constexpr const TypefaceFont *kFonts[] = {&kNormalFont, &kSystemFont, &kNameFont};

struct Typeface
{
    const char *file;
    const unsigned char *data;
    const size_t *size;
    stbtt_fontinfo info{};
    bool tried = false, ready = false;
};
Typeface g_typefaces[] = {
    {"Andika-Regular.ttf", rg_typeface_andika_regular, &rg_typeface_andika_regular_size},
    {"Andika-Bold.ttf", rg_typeface_andika_bold, &rg_typeface_andika_bold_size},
};

// A draw past 0x80 alpha (dq8::gfx::RegionRequest::bold; the game's menus
// draw their letters so) doubles the alpha: the console's glyph comes out
// bolder at its texels, the picture here only at its fine edge. A step more
// of growth (about an eighth of a pixel) matches best, for the three fonts,
// the doubled picture at 4x seen at 1x against the console's doubled glyph
// (with the measure of tools/typeface: 0.24 against 0.25 at two steps, 0.28
// at none).
constexpr int kBolderSteps = 1;

// Render pixels a cell pixel: growth in sixteenths of a pixel or finer, then
// averaged down to the target's scale.
constexpr int kRender = 32;

// FontGetCharWidth's table: 4 bytes a glyph code, the character first.
constexpr uint32_t kFontTable = 0x0044E850u;
// The codes it names: ASCII and the game's symbols (the accents of the
// European fonts follow, from 107).
constexpr uint32_t kTableCodes = 106u;
constexpr uint32_t kArchiveFindEntryByName = 0x00181E20u; // (pack, name), an entry of a pack
constexpr uint32_t kRdramSize = 0x02000000u;

std::atomic<bool> g_enabled{false};
// RG_TYPEFACE_LETTERS, over the menu: -1 unset. Set in install(), after the
// menu has started (its thread calls setEnabled).
std::atomic<int> g_forced{-1};
std::atomic<bool> g_trace{false};
// RG_TYPEFACE_TRACE=palettes: every palette a glyph is drawn with is logged,
// and all stay asked about (finding the palettes of coloured text).
std::atomic<bool> g_tracePalettes{false};
PS2Runtime::RecompiledFunction g_originalFindEntry = nullptr;

// Where a console glyph has ink in its cell, in cell pixels from the cell's
// left and top edges: its first and past-the-last column, and the row past
// its lowest.
struct Ink
{
    uint8_t left, right, bottom;
    float centre() const { return (left + right) * 0.5f; }
};

// A glyph of a font page the table redraws, by its pixels.
struct Cell
{
    uint8_t font;
    // Its row in the font's table; -1 when two glyphs of the pages look alike.
    int16_t glyph;
    Ink ink;
};

// RG_TYPEFACE_SHEET=DIR: as each page is read, every glyph the table redraws,
// the console's (enlarged) beside the typeface's plain and bold, and the
// plain over its border and over its shadow, into
// DIR/typeface_<font>_<scale>x.png, at RG_TYPEFACE_SHEET_SCALE (4). Made
// from the game's pages: it stays on the machine.
std::string g_sheetDirectory;
uint32_t g_sheetScale = 4u;
struct SheetGlyph
{
    int16_t glyph;
    Ink ink;
    std::vector<uint8_t> alpha;
};
void writeSheet(uint8_t fontIndex, std::vector<SheetGlyph> glyphs);
std::mutex g_cellsMutex;
std::unordered_map<uint64_t, Cell> g_cells;
// The palettes whose glyphs are redrawn; the backend hashes the texels of
// those only (and of every palette when a pack or a dump asks: the others
// still stay the console's). The ones known: dq8-gfx's hashes of the three
// fonts' palettes, the same since Sharp letters in October, and the only
// ones the glyphs were seen drawn with (RG_TYPEFACE_TRACE=palettes). In
// case the hash ever changes, every palette is asked about from the first
// page read until a glyph matches with a known palette (a few seconds of
// boot), or, if none does, until a glyph of each font read has matched;
// meanwhile a palette a glyph turns up with is learned, when it gives all
// its levels one colour (the picture has one), up to kLearnedPalettes. A
// known palette matched drops the learned ones: the same letters redrawn
// at every run.
constexpr uint64_t kKnownPalettes[] = {0x33A2CFEFAEC9F88Bull, 0xCFBAE4E31E471D6Eull, 0x75B5F042466BE787ull};
constexpr size_t kLearnedPalettes = 16u;
std::vector<uint64_t> g_palettes(std::begin(kKnownPalettes), std::end(kKnownPalettes));
bool g_pageRead[std::size(kFonts)] = {};
bool g_matched[std::size(kFonts)] = {};
// A glyph has matched with a known palette: the hashes held.
bool g_knownHold = false;
std::atomic<bool> g_learning{false};

// Under g_cellsMutex.
void updateLearning()
{
    bool learning = g_tracePalettes;
    for (size_t i = 0; i < std::size(kFonts) && !g_knownHold; ++i)
    {
        learning = learning || (g_pageRead[i] && !g_matched[i]);
    }
    g_learning.store(learning, std::memory_order_relaxed);
}

uint64_t hashAlpha(const uint8_t *rgba, uint32_t texels)
{
    uint64_t hash = 0xcbf29ce484222325ull;
    for (uint32_t i = 0; i < texels; ++i)
    {
        hash ^= rgba[i * 4u + 3u];
        hash *= 0x100000001b3ull;
    }
    return hash;
}

uint32_t read32(const uint8_t *p)
{
    uint32_t value = 0;
    std::memcpy(&value, p, sizeof(value));
    return value;
}

uint16_t read16(const uint8_t *p)
{
    uint16_t value = 0;
    std::memcpy(&value, p, sizeof(value));
    return value;
}

// The glyphs of a page the table knows, from a 4-bit TIM2 (white over 16
// alpha levels): each one's alpha, cell by cell, code n the nth cell.
// Whether it added any (the page was read before, at another load, or not).
bool readPage(const uint8_t *rdram, const TypefaceFont &font, uint8_t fontIndex, const uint8_t *tim, uint32_t size,
              const char *name)
{
    if (size < 0x40u || std::memcmp(tim, "TIM2", 4) != 0)
    {
        return false;
    }
    const uint32_t at = tim[5] == 1u ? 0x80u : 0x10u; // the pictures 128-byte aligned, or 16
    if (at + 0x18u > size)
    {
        return false;
    }
    const uint32_t clutSize = read32(tim + at + 4u), imageSize = read32(tim + at + 8u);
    const uint32_t header = read16(tim + at + 12u), colours = read16(tim + at + 14u);
    const uint32_t width = read16(tim + at + 0x14u), height = read16(tim + at + 0x16u);
    if (tim[at + 0x13u] != 4u || colours < 16u || width < font.cellWidth ||
        uint64_t(at) + header + imageSize + clutSize > size || uint64_t(width) * height / 2u > imageSize ||
        clutSize < 64u)
    {
        if (g_trace)
        {
            std::fprintf(stderr, "[typeface] %s: not a 4-bit page\n", name);
        }
        return false;
    }
    const uint8_t *pixels = tim + at + header;
    const uint8_t *clut = pixels + imageSize;
    const uint32_t perRow = width / font.cellWidth;
    const uint32_t cw = font.cellWidth, ch = font.cellHeight;
    std::vector<uint8_t> rgba(size_t(cw) * ch * 4u, 0u);
    size_t added = 0;
    std::vector<SheetGlyph> sheet;
    std::lock_guard<std::mutex> lock(g_cellsMutex);
    for (uint32_t code = 0; code < kTableCodes; ++code)
    {
        const uint8_t *entry = getConstMemPtr(rdram, kFontTable + 4u * code);
        if (!entry)
        {
            break;
        }
        const char character = static_cast<char>(entry[0]);
        int16_t glyph = -1;
        for (size_t i = 0; i < font.count; ++i)
        {
            if (font.glyphs[i].character == character && character != 0)
            {
                glyph = static_cast<int16_t>(i);
            }
        }
        const uint32_t x0 = (code % perRow) * cw, y0 = (code / perRow) * ch;
        if (glyph < 0 || y0 + ch > height)
        {
            continue;
        }
        int inkLeft = int(cw), inkRight = -1, inkBottom = 0;
        for (uint32_t y = 0; y < ch; ++y)
        {
            for (uint32_t x = 0; x < cw; ++x)
            {
                const size_t texel = size_t(y0 + y) * width + x0 + x;
                const uint8_t index = (pixels[texel / 2u] >> ((texel & 1u) * 4u)) & 15u;
                const uint8_t alpha = clut[index * 4u + 3u];
                rgba[(size_t(y) * cw + x) * 4u + 3u] = alpha;
                if (alpha)
                {
                    inkLeft = std::min(inkLeft, int(x));
                    inkRight = std::max(inkRight, int(x));
                    inkBottom = int(y) + 1;
                }
            }
        }
        if (inkRight < 0)
        {
            continue;
        }
        const uint64_t key = hashAlpha(rgba.data(), cw * ch);
        const Cell cell{fontIndex, glyph,
                        Ink{static_cast<uint8_t>(inkLeft), static_cast<uint8_t>(inkRight + 1),
                            static_cast<uint8_t>(inkBottom)}};
        const auto [it, inserted] = g_cells.emplace(key, cell);
        if (!inserted && (it->second.font != cell.font || it->second.glyph != cell.glyph))
        {
            it->second.glyph = -1;
        }
        added += inserted ? 1u : 0u;
        if (inserted && !g_sheetDirectory.empty())
        {
            SheetGlyph entry{glyph, cell.ink, std::vector<uint8_t>(size_t(cw) * ch)};
            for (size_t i = 0; i < entry.alpha.size(); ++i)
            {
                entry.alpha[i] = rgba[i * 4u + 3u];
            }
            sheet.push_back(std::move(entry));
        }
    }
    if (added != 0u)
    {
        g_pageRead[fontIndex] = true;
        updateLearning();
    }
    if (!sheet.empty())
    {
        std::thread(writeSheet, fontIndex, std::move(sheet)).detach();
    }
    if (g_trace)
    {
        std::fprintf(stderr, "[typeface] %s: %zu glyphs of %s (%ux%u)\n", name, added, font.name, width, height);
    }
    return added != 0u;
}

// After ArchiveFindEntryByName: a font's first page (FontTex_normal_EN_0.tm2),
// read where the pack lies. The English pages only: with the European text
// (src/text_language.cpp asks for the _EUR_ pages) the accented glyphs are
// not in the table yet and the names' cells are 16x22, so the letters stay
// the console's there (the menu says so).
void afterFindEntry(uint8_t *rdram, R5900Context *ctx, PS2Runtime *, uint32_t)
{
    const uint32_t entry = getRegU32(ctx, 2);
    const uint32_t offset = entry & 0x1FFFFFFFu;
    if (entry == 0u || offset + 0x50u > kRdramSize)
    {
        return;
    }
    const uint8_t *e = getConstMemPtr(rdram, entry);
    if (!e)
    {
        return;
    }
    char name[0x41] = {};
    std::memcpy(name, e, 0x40);
    std::string lower;
    for (const char *p = name; *p; ++p)
    {
        lower += static_cast<char>(std::tolower(static_cast<unsigned char>(*p)));
    }
    constexpr const char kPrefix[] = "fonttex_";
    constexpr const char kSuffix[] = "_en_0.tm2";
    if (lower.rfind(kPrefix, 0) != 0 || lower.size() < sizeof(kSuffix) ||
        lower.compare(lower.size() - (sizeof(kSuffix) - 1u), sizeof(kSuffix) - 1u, kSuffix) != 0)
    {
        return;
    }
    const std::string font = lower.substr(sizeof(kPrefix) - 1u, lower.find('_', sizeof(kPrefix) - 1u) -
                                                                    (sizeof(kPrefix) - 1u));
    const uint32_t dataOffset = read32(e + 0x40u), size = read32(e + 0x44u);
    if (uint64_t(offset) + dataOffset + size > kRdramSize)
    {
        return;
    }
    for (uint8_t i = 0; i < std::size(kFonts); ++i)
    {
        if (font == kFonts[i]->name)
        {
            const uint8_t *tim = getConstMemPtr(rdram, entry + dataOffset);
            // Regions answered before the page was read are asked again. Only
            // when it brought glyphs and the letters are on: every larger
            // picture is let go (a pack's too), and the game looks the pages
            // up again at every overlay load.
            if (tim && readPage(rdram, *kFonts[i], i, tim, size, name) &&
                g_enabled.load(std::memory_order_relaxed))
            {
                rg::texturePack::refresh();
            }
        }
    }
}

void archiveFindEntryByName(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    rg::hooks::callThen(g_originalFindEntry, rdram, ctx, runtime, &afterFindEntry, 0u);
}

// FontDrawGlyphOutline(style, packet, rect, uv, alpha) draws a glyph's
// border before the glyph, as copies of it, by the style's first byte:
// 1, four black copies a pixel up, down, right and left; 4, twelve (those,
// the diagonals and two pixels out); 5, black on the four diagonals, then
// dark grey (0x20) on the four sides (the dialogue); 6, black on all
// eight; 2, a shadow: a grey copy a pixel down and right, then a black one
// two; 3, the black one alone. Copies of a smooth glyph a pixel apart show
// each its own outline (the dialogue's came out a grey ring with black
// corners), so they are marked for the backend (setRegionReplacements): a
// border's copies a sixteenth of a pixel up (three when the first is a
// diagonal), mode 2's shadow copies two. The backend then draws the border
// from one picture, the glyph ringed (lookup's border version), and leaves
// the other copies out; a shadow copy from the glyph swept back toward the
// one before it. Mode 3's lone copy two pixels away stays as it is: swept,
// it would fill the pixel between it and the glyph that the console leaves
// clear. At a pixel's centre, where the GS samples, the marks move no
// pixel and no texel (Sharp letters marked them so, October 2026,
// src/sharp_letters.cpp at b55f0ea).
constexpr uint32_t kFontDrawGlyphOutline = 0x0026E450u;
constexpr uint32_t kPacketOffsetY = 0x124u; // the packet's y offset, the GS's 12.4
PS2Runtime::RecompiledFunction g_originalOutline = nullptr;

// Once FontDrawGlyphOutline has returned, even when unwound and resumed:
// the marks taken off the packet's offset are put back. `packetAndMarks` is
// the packet's address (a multiple of 4) with the marks in its two low
// bits, so each call undoes its own, whatever ran between.
void afterOutline(uint8_t *rdram, R5900Context *, PS2Runtime *, uint32_t packetAndMarks)
{
    if (uint8_t *offset = getMemPtr(rdram, (packetAndMarks & ~3u) + kPacketOffsetY))
    {
        int32_t value = 0;
        std::memcpy(&value, offset, sizeof(value));
        value += static_cast<int32_t>(packetAndMarks & 3u);
        std::memcpy(offset, &value, sizeof(value));
    }
}

void drawGlyphOutline(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint8_t *style = getConstMemPtr(rdram, getRegU32(ctx, 4));
    uint8_t *offset = getMemPtr(rdram, getRegU32(ctx, 5) + kPacketOffsetY);
    const int8_t mode = style ? static_cast<int8_t>(style[0]) : 0;
    if (g_trace && style)
    {
        static std::mutex seenMutex;
        static std::unordered_set<uint64_t> seen;
        std::lock_guard<std::mutex> lock(seenMutex);
        if (seen.insert(uint64_t(getRegU32(ctx, 4)) << 8u | uint8_t(mode)).second)
        {
            std::fprintf(stderr, "[typeface] style 0x%08X border mode %d (caller 0x%06X)\n", getRegU32(ctx, 4),
                         mode, getRegU32(ctx, 31));
        }
    }
    const uint32_t marks = mode == 1 || mode == 4 ? 1u : mode == 5 || mode == 6 ? 3u : mode == 2 ? 2u : 0u;
    const uint32_t packet = getRegU32(ctx, 5);
    if (!g_enabled.load(std::memory_order_relaxed) || !offset || marks == 0u || (packet & 3u) != 0u)
    {
        g_originalOutline(rdram, ctx, runtime);
        return;
    }
    int32_t value = 0;
    std::memcpy(&value, offset, sizeof(value));
    value -= static_cast<int32_t>(marks);
    std::memcpy(offset, &value, sizeof(value));
    rg::hooks::callThen(g_originalOutline, rdram, ctx, runtime, &afterOutline, packet | marks);
}

// --- Drawing, on a thread of its own ---

// The pictures of a glyph: itself, its border (the glyph ringed by a round
// pen a cell pixel wide, drawn where the glyph goes, in the colour of the
// border's first copy), its shadow (the glyph swept a cell pixel back up and
// left, drawn at each shadow copy's place, so the copies join as the
// console's blocks do). The border's and the shadow's have
// dq8::gfx::kRegionBorderMargin cell pixels of margin each side.
enum class Version : uint8_t
{
    Glyph,
    Border,
    Shadow,
};

struct Job
{
    uint64_t key;
    uint8_t font;
    int16_t glyph;
    Ink ink;
    uint32_t scale;
    bool bold;
    Version version;
    uint8_t colour[3];
};
struct Picture
{
    uint32_t width = 0u, height = 0u;
    std::vector<uint8_t> rgba;
};

std::mutex g_jobsMutex;
std::condition_variable g_wake;
std::deque<Job> g_queue;
std::unordered_set<uint64_t> g_asked;
// Made and not yet taken. A picture the renderer never asks for again (the
// scale changed while it was queued, text gone before it was made) would
// stay: they go when the letters are switched and past kKeptBytes (about 650
// glyphs at 8x; the renderer asks each target's scale, so pictures of
// another scale are not dropped sooner).
std::unordered_map<uint64_t, Picture> g_ready;
size_t g_readyBytes = 0u;
constexpr size_t kKeptBytes = size_t(64u) << 20u;

// Every picture made or asked for is forgotten: what is asked again is made
// again (under g_jobsMutex).
void forgetPicturesLocked()
{
    g_queue.clear();
    g_asked.clear();
    g_ready.clear();
    g_readyBytes = 0u;
}

void forgetPictures()
{
    std::lock_guard<std::mutex> lock(g_jobsMutex);
    forgetPicturesLocked();
}

// The drawer's thread and the sheets' open the typefaces.
std::mutex g_typefacesMutex;

Typeface *typefaceFor(const char *file)
{
    std::lock_guard<std::mutex> lock(g_typefacesMutex);
    for (Typeface &face : g_typefaces)
    {
        if (std::strcmp(face.file, file) != 0)
        {
            continue;
        }
        if (!face.tried)
        {
            face.tried = true;
            const int start = stbtt_GetFontOffsetForIndex(face.data, 0);
            face.ready = start >= 0 && stbtt_InitFont(&face.info, face.data, start) != 0;
            if (!face.ready)
            {
                std::fprintf(stderr, "[rg] typeface letters: %s did not open\n", face.file);
            }
        }
        return face.ready ? &face : nullptr;
    }
    return nullptr;
}

// Along one axis (alongY: down the columns), each texel takes the largest
// (keepLeast: the smallest) value within `radius` texels of it, by a running
// window whatever the radius. Across then down, it is the rectangular pen the
// fit grew strokes with (a square one in the evenly scaled render).
void spread(std::vector<uint8_t> &image, int width, int height, bool alongY, int radius, bool keepLeast)
{
    if (radius <= 0)
    {
        return;
    }
    const int lines = alongY ? width : height, length = alongY ? height : width;
    const size_t step = alongY ? size_t(width) : 1u, stride = alongY ? 1u : size_t(width);
    std::vector<uint8_t> line(static_cast<size_t>(length));
    std::deque<int> window; // indices into line, their values from best to worst
    for (int n = 0; n < lines; ++n)
    {
        uint8_t *start = image.data() + size_t(n) * stride;
        for (int i = 0; i < length; ++i)
        {
            line[size_t(i)] = start[size_t(i) * step];
        }
        window.clear();
        int next = 0;
        for (int i = 0; i < length; ++i)
        {
            for (; next <= std::min(length - 1, i + radius); ++next)
            {
                const uint8_t value = line[size_t(next)];
                while (!window.empty() &&
                       (keepLeast ? value <= line[size_t(window.back())] : value >= line[size_t(window.back())]))
                {
                    window.pop_back();
                }
                window.push_back(next);
            }
            while (window.front() < i - radius)
            {
                window.pop_front();
            }
            start[size_t(i) * step] = line[size_t(window.front())];
        }
    }
}

// A box filter from `from` texels down to `to`: each output texel's first
// source texel and the share of each source texel it covers.
struct Taps
{
    int first;
    std::vector<float> weights;
};
std::vector<Taps> boxTaps(int from, int to)
{
    const double span = double(from) / to;
    std::vector<Taps> taps(static_cast<size_t>(to));
    for (int o = 0; o < to; ++o)
    {
        const double a = o * span, b = a + span;
        taps[size_t(o)].first = int(std::floor(a));
        for (int s = int(std::floor(a)); s < std::min(from, int(std::ceil(b))); ++s)
        {
            taps[size_t(o)].weights.push_back(float((std::min(b, double(s + 1)) - std::max(a, double(s))) / span));
        }
    }
    return taps;
}

// The lines [from, to) of an image along y (rows) or x (columns), squeezed
// toward `pivot` by `factor` (below 1): the line at pivot + d takes what lay
// from pivot + d / factor to pivot + (d + 1) / factor, each source line by
// its share. The other lines stay.
void squeeze(std::vector<uint8_t> &image, int width, int height, bool alongY, double pivot, double factor, int from,
             int to)
{
    const std::vector<uint8_t> source = image;
    const int lines = alongY ? height : width, across = alongY ? width : height;
    auto at = [&](int line, int i) -> size_t {
        return alongY ? size_t(line) * width + i : size_t(i) * width + line;
    };
    for (int line = std::max(from, 0); line < std::min(to, lines); ++line)
    {
        const double start = pivot + (line - pivot) / factor, end = pivot + (line + 1 - pivot) / factor;
        for (int i = 0; i < across; ++i)
        {
            double sum = 0.0;
            for (int s = std::max(0, int(std::floor(start))); s < std::min(lines, int(std::ceil(end))); ++s)
            {
                sum += (std::min(end, double(s + 1)) - std::max(start, double(s))) * source[at(s, i)];
            }
            image[at(line, i)] = static_cast<uint8_t>(std::min(255.0, sum * factor + 0.5));
        }
    }
}

// The squared distance of each texel of a line to the nearest of `f`'s
// zeros (f holds 0 or kFar), by the lower envelope of parabolas
// (Felzenszwalb and Huttenlocher's distance transform).
constexpr float kFar = 1e20f;
void distanceLine(const float *f, int n, float *d, int *v, float *z)
{
    int k = 0;
    v[0] = 0;
    z[0] = -kFar;
    z[1] = kFar;
    for (int q = 1; q < n; ++q)
    {
        float s = ((f[q] + float(q) * q) - (f[v[k]] + float(v[k]) * v[k])) / (2.0f * (q - v[k]));
        while (s <= z[k])
        {
            --k;
            s = ((f[q] + float(q) * q) - (f[v[k]] + float(v[k]) * v[k])) / (2.0f * (q - v[k]));
        }
        ++k;
        v[k] = q;
        z[k] = s;
        z[k + 1] = kFar;
    }
    k = 0;
    for (int q = 0; q < n; ++q)
    {
        while (z[k + 1] < q)
        {
            ++k;
        }
        d[q] = float(q - v[k]) * (q - v[k]) + f[v[k]];
    }
}

// The glyph ringed: every texel within `radius` of its half-covered shape
// covered, the ring's edge smoothed over a texel.
void ring(std::vector<uint8_t> &image, int width, int height, float radius)
{
    std::vector<float> distance(image.size());
    for (size_t i = 0; i < image.size(); ++i)
    {
        distance[i] = image[i] >= 128u ? 0.0f : kFar;
    }
    const int longest = std::max(width, height);
    std::vector<float> f(static_cast<size_t>(longest)), d(static_cast<size_t>(longest));
    std::vector<float> z(static_cast<size_t>(longest) + 1u);
    std::vector<int> v(static_cast<size_t>(longest));
    for (int x = 0; x < width; ++x)
    {
        for (int y = 0; y < height; ++y)
        {
            f[size_t(y)] = distance[size_t(y) * width + x];
        }
        distanceLine(f.data(), height, d.data(), v.data(), z.data());
        for (int y = 0; y < height; ++y)
        {
            distance[size_t(y) * width + x] = d[size_t(y)];
        }
    }
    for (int y = 0; y < height; ++y)
    {
        float *line = distance.data() + size_t(y) * width;
        std::copy(line, line + width, f.begin());
        distanceLine(f.data(), width, d.data(), v.data(), z.data());
        std::copy(d.begin(), d.begin() + width, line);
    }
    for (size_t i = 0; i < image.size(); ++i)
    {
        const float cover = std::clamp(radius + 0.5f - std::sqrt(distance[i]), 0.0f, 1.0f);
        image[i] = std::max(image[i], static_cast<uint8_t>(cover * 255.0f + 0.5f));
    }
}

// The glyph swept `length` texels back up and left: each texel takes the
// most covered of those from it to `length` down and right of it.
void sweep(std::vector<uint8_t> &image, int width, int height, int length)
{
    const std::vector<uint8_t> source = image;
    for (int t = 1; t <= length; ++t)
    {
        for (int y = 0; y + t < height; ++y)
        {
            const uint8_t *from = source.data() + size_t(y + t) * width + t;
            uint8_t *to = image.data() + size_t(y) * width;
            for (int x = 0; x + t < width; ++x)
            {
                to[x] = std::max(to[x], from[x]);
            }
        }
    }
}

// The glyph at `scale` host pixels a cell pixel, as the fit placed it: drawn
// at kRender pixels a cell pixel, grown, and averaged down (or its border or
// shadow, with their margin).
Picture draw(const Job &job)
{
    Picture picture;
    const TypefaceFont &font = *kFonts[job.font];
    const TypefaceGlyph &glyph = font.glyphs[job.glyph];
    Typeface *face = typefaceFor(font.file);
    if (!face)
    {
        return picture;
    }
    const int r = kRender;
    // The pen, in render pixels across and down: the fit's steps of growth
    // (and the bold draw's), each penX and penY eighths of a cell pixel.
    const int steps = font.grow + (job.bold ? kBolderSteps : 0);
    const int growX = int(std::lround(steps * font.penX * r / 8.0f));
    const int growY = int(std::lround(steps * font.penY * r / 8.0f));
    const float scaleX = stbtt_ScaleForMappingEmToPixels(&face->info, font.emX * float(r));
    const float scaleY = stbtt_ScaleForMappingEmToPixels(&face->info, font.emY * float(r));
    int bx0 = 0, by0 = 0, bx1 = 0, by1 = 0;
    stbtt_GetGlyphBitmapBox(&face->info, glyph.glyph, scaleX, scaleY, &bx0, &by0, &bx1, &by1);
    const int gw = bx1 - bx0, gh = by1 - by0;
    if (gw <= 0 || gh <= 0)
    {
        return picture;
    }
    std::vector<uint8_t> bitmap(size_t(gw) * gh, 0u);
    stbtt_MakeGlyphBitmap(&face->info, bitmap.data(), gw, gh, gw, scaleX, scaleY, glyph.glyph);
    int inkLeft = gw, inkRight = -1, inkTop = gh, inkEnd = 0;
    for (int y = 0; y < gh; ++y)
    {
        for (int x = 0; x < gw; ++x)
        {
            if (bitmap[size_t(y) * gw + x])
            {
                inkLeft = std::min(inkLeft, x);
                inkRight = std::max(inkRight, x);
                inkTop = std::min(inkTop, y);
                inkEnd = y + 1;
            }
        }
    }
    if (inkRight < 0)
    {
        return picture;
    }
    // The cell at r, with room for the pen past its edges, and for a border
    // or shadow version its margin; the cell's corner at (offX, offY).
    const int marginX = std::abs(growX), marginY = std::abs(growY);
    const int pad = job.version == Version::Glyph ? 0 : int(dq8::gfx::kRegionBorderMargin) * r;
    const int offX = marginX + pad, offY = marginY + pad;
    const int cw = int(font.cellWidth) * r, ch = int(font.cellHeight) * r;
    const int width = cw + 2 * offX, height = ch + 2 * offY;
    std::vector<uint8_t> canvas(size_t(width) * height, 0u);
    // The ink centre on the console glyph's (and the table's shift), the
    // baseline on the table's row (and the sign's lift).
    const int left =
        int(std::lround((job.ink.centre() + glyph.shift) * float(r) - (inkLeft + inkRight + 1) * 0.5f)) + offX;
    const int top = int(std::lround((font.baseline + glyph.lift) * float(r))) + by0 + offY;
    // What reaches past the console glyph is brought in, so nothing is cut
    // flat at the cell's edge and nothing runs into the next letter (in
    // bitmap rows and columns; canvas = bitmap + left or top):
    // - a descender longer than the console's (the typeface's g, j, p, q, y)
    //   ends where the console's does, the rows below the baseline squeezed,
    //   as the console's short descenders are;
    // - a glyph wider than the console's by more than half a pixel a side, or
    //   past the cell's last shown column (the W), is narrowed about its
    //   centre;
    // - one past the cell's top (the tall signs: / [ ] { }) has the rows
    //   above the baseline squeezed down to it.
    // Half a pixel past the console's ink is let be (a round letter's
    // overshoot); the console's ascenders are lower than the typeface's, and
    // those are let be too.
    const int baseRow = -by0;
    const int endRow = int(job.ink.bottom) * r + offY - top;
    if (baseRow >= 0 && inkEnd > endRow + r / 2 && endRow > baseRow + 1)
    {
        squeeze(bitmap, gw, gh, true, baseRow, double(endRow - baseRow) / double(inkEnd - baseRow), baseRow, gh);
    }
    const double centre = (inkLeft + inkRight + 1) * 0.5;
    const double half = (inkRight + 1 - inkLeft) * 0.5;
    const double canvasCentre = left + centre;
    // The cell's last column is never shown (a glyph's sprite spans a texel
    // past its pixels, and the backend clears that one in the picture): the
    // right edge is the column before it, the stroke's growth inside it.
    const double leftLimit = std::max(0.0, job.ink.left - 0.5) * r + offX;
    const double rightLimit =
        std::min((double(font.cellWidth) - 1.0) * r - std::max(0, growX), (job.ink.right + 0.5) * r) + offX;
    const double narrow = std::min({1.0, (canvasCentre - leftLimit) / half, (rightLimit - canvasCentre) / half});
    if (narrow < 1.0 - 0.5 / half && narrow > 0.0)
    {
        squeeze(bitmap, gw, gh, false, centre, narrow, 0, gw);
    }
    const int topRow = offY - top;
    if (baseRow > topRow && inkTop < topRow - r / 4)
    {
        squeeze(bitmap, gw, gh, true, baseRow, double(baseRow - topRow) / double(baseRow - inkTop), 0, baseRow);
    }
    for (int y = 0; y < gh; ++y)
    {
        const int cy = top + y;
        if (cy < 0 || cy >= height)
        {
            continue;
        }
        for (int x = 0; x < gw; ++x)
        {
            const int cx = left + x;
            if (cx >= 0 && cx < width)
            {
                canvas[size_t(cy) * width + cx] = bitmap[size_t(y) * gw + x];
            }
        }
    }
    spread(canvas, width, height, false, marginX, growX < 0);
    spread(canvas, width, height, true, marginY, growY < 0);
    if (job.version == Version::Border)
    {
        ring(canvas, width, height, float(r));
    }
    else if (job.version == Version::Shadow)
    {
        sweep(canvas, width, height, r);
    }
    // Averaged down to the scale, across then down, each host pixel the mean
    // of the render pixels it covers: the cell, and the margin a version has.
    const uint32_t margin = job.version == Version::Glyph ? 0u : 2u * dq8::gfx::kRegionBorderMargin;
    picture.width = (font.cellWidth + margin) * job.scale;
    picture.height = (font.cellHeight + margin) * job.scale;
    const int ow = int(picture.width), oh = int(picture.height);
    const int sw = cw + 2 * pad, sh = ch + 2 * pad; // the render pixels averaged, from (marginX, marginY)
    const std::vector<Taps> across = boxTaps(sw, ow), down = boxTaps(sh, oh);
    std::vector<float> rows(size_t(ow) * sh);
    for (int y = 0; y < sh; ++y)
    {
        const uint8_t *line = canvas.data() + size_t(y + marginY) * width + marginX;
        for (int x = 0; x < ow; ++x)
        {
            const Taps &t = across[size_t(x)];
            float sum = 0.0f;
            for (size_t i = 0; i < t.weights.size(); ++i)
            {
                sum += t.weights[i] * line[t.first + int(i)];
            }
            rows[size_t(y) * ow + x] = sum;
        }
    }
    picture.rgba.assign(size_t(ow) * oh * 4u, 0u);
    for (int y = 0; y < oh; ++y)
    {
        const Taps &t = down[size_t(y)];
        for (int x = 0; x < ow; ++x)
        {
            float sum = 0.0f;
            for (size_t i = 0; i < t.weights.size(); ++i)
            {
                sum += t.weights[i] * rows[size_t(t.first + int(i)) * ow + x];
            }
            uint8_t *out = &picture.rgba[(size_t(y) * ow + x) * 4u];
            out[0] = job.colour[0];
            out[1] = job.colour[1];
            out[2] = job.colour[2];
            // Coverage 0-255 to the GS's alpha, 0x80 opaque.
            out[3] = static_cast<uint8_t>(std::min(128.0f, sum * 128.0f / 255.0f + 0.5f));
        }
    }
    return picture;
}

void writeSheet(uint8_t fontIndex, std::vector<SheetGlyph> glyphs)
{
    const auto started = std::chrono::steady_clock::now();
    const TypefaceFont &font = *kFonts[fontIndex];
    const uint32_t s = g_sheetScale, cw = font.cellWidth * s, ch = font.cellHeight * s;
    constexpr uint32_t kColumns = 8u, kGap = 6u;
    // A glyph's pictures side by side: the console's, plain, bold, then the
    // plain over its border and over its shadow (two copies, grey then
    // black, as the field's prompts draw them), with the versions' margin.
    const uint32_t m = dq8::gfx::kRegionBorderMargin * s, vw = cw + 2u * m;
    const uint32_t entryWidth = 3u * cw + 2u * vw + 4u * kGap + 4u * kGap, entryHeight = ch + 2u * m + 2u * kGap;
    const uint32_t rows = uint32_t((glyphs.size() + kColumns - 1u) / kColumns);
    const uint32_t width = kColumns * entryWidth + kGap, height = rows * entryHeight + kGap;
    std::vector<uint8_t> image(size_t(width) * height * 3u);
    for (size_t i = 0; i < image.size(); i += 3u)
    {
        image[i] = 28u, image[i + 1u] = 38u, image[i + 2u] = 66u;
    }
    auto paint = [&](uint32_t x0, uint32_t y0, auto alphaAt) {
        for (uint32_t y = 0; y < ch; ++y)
        {
            for (uint32_t x = 0; x < cw; ++x)
            {
                const float a = std::min(1.0f, alphaAt(x, y) / 128.0f);
                uint8_t *p = &image[(size_t(y0 + y) * width + x0 + x) * 3u];
                p[0] = static_cast<uint8_t>(20.0f + (255.0f - 20.0f) * a);
                p[1] = static_cast<uint8_t>(28.0f + (255.0f - 28.0f) * a);
                p[2] = static_cast<uint8_t>(52.0f + (255.0f - 52.0f) * a);
            }
        }
    };
    for (size_t n = 0; n < glyphs.size(); ++n)
    {
        const SheetGlyph &g = glyphs[n];
        const uint32_t x0 = kGap + uint32_t(n % kColumns) * entryWidth, y0 = kGap + uint32_t(n / kColumns) * entryHeight;
        paint(x0, y0 + m, [&](uint32_t x, uint32_t y) { return float(g.alpha[(y / s) * font.cellWidth + x / s]); });
        Picture plain;
        for (int bold = 0; bold < 2; ++bold)
        {
            Picture picture =
                draw(Job{0u, fontIndex, g.glyph, g.ink, s, bold != 0, Version::Glyph, {255u, 255u, 255u}});
            paint(x0 + (1u + uint32_t(bold)) * (cw + kGap), y0 + m, [&](uint32_t x, uint32_t y) {
                return picture.rgba.empty() ? 0.0f : float(picture.rgba[(size_t(y) * picture.width + x) * 4u + 3u]);
            });
            if (bold == 0)
            {
                plain = std::move(picture);
            }
        }
        // Layers of (picture, its offset in sheet pixels, grey level) over
        // the background, the glyph last, white.
        auto alphaOf = [](const Picture &p, int x, int y) {
            return p.rgba.empty() || x < 0 || y < 0 || x >= int(p.width) || y >= int(p.height)
                       ? 0.0f
                       : std::min(1.0f, p.rgba[(size_t(y) * p.width + x) * 4u + 3u] / 128.0f);
        };
        auto layered = [&](uint32_t tx, std::initializer_list<std::tuple<const Picture *, int, float>> layers) {
            for (uint32_t y = 0; y < ch + 2u * m; ++y)
            {
                for (uint32_t x = 0; x < vw; ++x)
                {
                    float c[3] = {20.0f, 28.0f, 52.0f};
                    for (const auto &[p, at, grey] : layers)
                    {
                        const float a = alphaOf(*p, int(x) - at, int(y) - at);
                        for (float &channel : c)
                        {
                            channel += (grey - channel) * a;
                        }
                    }
                    const float a = alphaOf(plain, int(x) - int(m), int(y) - int(m));
                    uint8_t *px = &image[(size_t(y0 + y) * width + tx + x) * 3u];
                    for (int i = 0; i < 3; ++i)
                    {
                        px[i] = static_cast<uint8_t>(c[i] + (255.0f - c[i]) * a);
                    }
                }
            }
        };
        const Picture border =
            draw(Job{0u, fontIndex, g.glyph, g.ink, s, false, Version::Border, {255u, 255u, 255u}});
        const Picture shadow =
            draw(Job{0u, fontIndex, g.glyph, g.ink, s, false, Version::Shadow, {255u, 255u, 255u}});
        const uint32_t bx = x0 + 3u * (cw + kGap) + kGap;
        layered(bx, {{&border, 0, 0.0f}});
        layered(bx + vw + kGap, {{&shadow, int(s), 128.0f}, {&shadow, 2 * int(s), 0.0f}});
    }
    const std::string path = g_sheetDirectory + "/typeface_" + font.name + "_" + std::to_string(s) + "x.png";
    const bool written = stbi_write_png(path.c_str(), int(width), int(height), 3, image.data(), int(width * 3u)) != 0;
    const long long ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
    std::fprintf(stderr, "[typeface] sheet of %s at %ux: %s%s (%zu glyphs, four pictures each, in %lld ms)\n", font.name,
                 s, path.c_str(), written ? "" : " (not written)", glyphs.size(), ms);
}

void drawer()
{
    for (;;)
    {
        Job job;
        {
            std::unique_lock<std::mutex> lock(g_jobsMutex);
            g_wake.wait(lock, [] { return !g_queue.empty(); });
            job = g_queue.front();
            g_queue.pop_front();
        }
        Picture picture = draw(job);
        if (g_trace)
        {
            const TypefaceFont &font = *kFonts[job.font];
            std::fprintf(stderr, "[typeface] made %s '%c' at %ux%s%s%s\n", font.name,
                         font.glyphs[job.glyph].character, job.scale, job.bold ? ", bold" : "",
                         job.version == Version::Border   ? ", border"
                         : job.version == Version::Shadow ? ", shadow"
                                                          : "",
                         picture.rgba.empty() ? " (empty)" : "");
        }
        std::lock_guard<std::mutex> lock(g_jobsMutex);
        if (g_readyBytes + picture.rgba.size() > kKeptBytes)
        {
            forgetPicturesLocked();
        }
        Picture &kept = g_ready[job.key];
        g_readyBytes -= kept.rgba.size();
        g_readyBytes += picture.rgba.size();
        kept = std::move(picture);
    }
}

// --- What the renderer asks ---

bool wantedPalette(uint64_t clut)
{
    if (!g_enabled.load(std::memory_order_relaxed))
    {
        return false;
    }
    if (g_learning.load(std::memory_order_relaxed))
    {
        return true;
    }
    std::lock_guard<std::mutex> lock(g_cellsMutex);
    return std::find(g_palettes.begin(), g_palettes.end(), clut) != g_palettes.end();
}

int lookup(const dq8::gfx::RegionRequest &request, uint32_t &width, uint32_t &height, std::vector<uint8_t> &rgba)
{
    if (!g_enabled.load(std::memory_order_relaxed) || request.whole || !request.full || request.scale < 2u ||
        request.scale > 8u)
    {
        return dq8::gfx::kRegionNone;
    }
    // The border's version for the first of a glyph's marked border copies,
    // the shadow's for a marked shadow copy (drawGlyphOutline).
    const Version version = request.border   ? Version::Border
                            : request.shadow ? Version::Shadow
                                             : Version::Glyph;
    bool sized = false;
    for (const TypefaceFont *font : kFonts)
    {
        sized = sized || (request.width == font->cellWidth && request.height == font->cellHeight);
    }
    if (!sized)
    {
        return dq8::gfx::kRegionNone;
    }
    const uint32_t texels = request.width * request.height;
    const uint64_t alpha = hashAlpha(request.full, texels);
    // Its colour: the most opaque texel's; and whether every texel with ink
    // has it (the picture is drawn in one).
    const uint8_t *colour = request.full;
    for (uint32_t i = 0; i < texels; ++i)
    {
        if (request.full[i * 4u + 3u] > colour[3])
        {
            colour = request.full + i * 4u;
        }
    }
    bool oneColour = true;
    for (uint32_t i = 0; i < texels && oneColour; ++i)
    {
        const uint8_t *texel = request.full + i * 4u;
        for (int c = 0; c < 3 && texel[3] != 0u; ++c)
        {
            oneColour = oneColour && std::abs(int(texel[c]) - int(colour[c])) <= 8;
        }
    }
    Cell cell{};
    {
        std::lock_guard<std::mutex> lock(g_cellsMutex);
        const auto found = g_cells.find(alpha);
        if (found == g_cells.end() || found->second.glyph < 0 ||
            kFonts[found->second.font]->cellWidth != request.width ||
            kFonts[found->second.font]->cellHeight != request.height)
        {
            return dq8::gfx::kRegionNone;
        }
        cell = found->second;
        const bool seed = std::find(std::begin(kKnownPalettes), std::end(kKnownPalettes), request.clut) !=
                          std::end(kKnownPalettes);
        bool listed = std::find(g_palettes.begin(), g_palettes.end(), request.clut) != g_palettes.end();
        const char *note = "";
        if (!listed)
        {
            // Reached through a pack's or a dump's asking, or while learning.
            if (!g_learning.load(std::memory_order_relaxed))
            {
                note = " (not redrawn)";
            }
            else if (!oneColour)
            {
                note = " (not of one colour: not redrawn)";
            }
            else if (g_palettes.size() >= std::size(kKnownPalettes) + kLearnedPalettes)
            {
                note = " (too many palettes: not redrawn)";
            }
            else
            {
                g_palettes.push_back(request.clut);
                listed = true;
                note = " (new)";
            }
        }
        if (g_tracePalettes || (g_trace && (!listed || !g_matched[cell.font])))
        {
            // Every time with the palettes trace (it is for finding them);
            // otherwise a font's first match, and those the letters skip.
            static std::unordered_set<uint64_t> logged;
            if (logged.insert(request.clut ^ (uint64_t(cell.font) << 62u)).second)
            {
                std::fprintf(stderr, "[typeface] %s glyphs drawn with palette %016llX%s\n", kFonts[cell.font]->name,
                             static_cast<unsigned long long>(request.clut), note);
            }
        }
        if (!listed)
        {
            return dq8::gfx::kRegionNone;
        }
        if (!g_matched[cell.font] || (seed && !g_knownHold))
        {
            g_matched[cell.font] = true;
            if (seed && !g_knownHold)
            {
                g_knownHold = true;
                if (!g_tracePalettes)
                {
                    g_palettes.assign(std::begin(kKnownPalettes), std::end(kKnownPalettes));
                }
            }
            updateLearning();
        }
    }
    uint64_t key = alpha;
    for (const uint64_t value : {uint64_t(request.scale) << 3u | uint64_t(version) << 1u | (request.bold ? 1u : 0u),
                                 uint64_t(colour[0]) << 16u | uint64_t(colour[1]) << 8u | colour[2]})
    {
        key ^= value;
        key *= 0x100000001b3ull;
    }
    std::lock_guard<std::mutex> lock(g_jobsMutex);
    if (const auto ready = g_ready.find(key); ready != g_ready.end())
    {
        Picture picture = std::move(ready->second);
        g_readyBytes -= picture.rgba.size();
        g_ready.erase(ready);
        // Asked again if the GPU's copy is ever let go.
        g_asked.erase(key);
        if (picture.rgba.empty())
        {
            return dq8::gfx::kRegionNone;
        }
        width = picture.width;
        height = picture.height;
        rgba = std::move(picture.rgba);
        return dq8::gfx::kRegionReady;
    }
    if (g_asked.insert(key).second)
    {
        g_queue.push_back(Job{key, cell.font, cell.glyph, cell.ink, request.scale, request.bold, version,
                              {colour[0], colour[1], colour[2]}});
        g_wake.notify_one();
    }
    return dq8::gfx::kRegionPending;
}

} // namespace

void install(PS2Runtime &runtime)
{
    if (const char *trace = std::getenv("RG_TYPEFACE_TRACE"); trace && *trace && *trace != '0')
    {
        g_trace = true;
        g_tracePalettes = std::strcmp(trace, "palettes") == 0;
        g_learning.store(g_tracePalettes, std::memory_order_relaxed);
    }
    if (const char *sheet = std::getenv("RG_TYPEFACE_SHEET"); sheet && *sheet)
    {
        g_sheetDirectory = sheet;
        std::error_code error;
        std::filesystem::create_directories(g_sheetDirectory, error);
        if (const char *scale = std::getenv("RG_TYPEFACE_SHEET_SCALE"); scale && *scale)
        {
            g_sheetScale = std::clamp(uint32_t(std::strtoul(scale, nullptr, 10)), 2u, 8u);
        }
    }
    if (const char *enabled = std::getenv("RG_TYPEFACE_LETTERS"); enabled && *enabled)
    {
        g_forced = *enabled != '0' ? 1 : 0;
        g_enabled.store(g_forced == 1, std::memory_order_relaxed);
    }
    if (!rg::timing::hookFunction(runtime, kArchiveFindEntryByName, &archiveFindEntryByName, g_originalFindEntry))
    {
        std::fprintf(stderr, "[rg] typeface letters: the font pages cannot be read, the letters stay the game's\n");
    }
    if (!rg::timing::hookFunction(runtime, kFontDrawGlyphOutline, &drawGlyphOutline, g_originalOutline))
    {
        std::fprintf(stderr, "[rg] typeface letters: the borders stay copies of the glyph\n");
    }
}

void installRenderer(dq8::gfx::SdlGpuBackend &)
{
    std::thread(drawer).detach();
    rg::texturePack::setFirstSource(&lookup, &wantedPalette);
}

void setEnabled(bool on)
{
    if (g_forced >= 0)
    {
        on = g_forced == 1;
    }
    if (g_enabled.exchange(on, std::memory_order_relaxed) != on)
    {
        forgetPictures();
        rg::texturePack::refresh();
    }
}

} // namespace rg::typeface
