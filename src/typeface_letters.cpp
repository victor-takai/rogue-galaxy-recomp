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
extern const unsigned char rg_typeface_rogue_letters_regular[];
extern const size_t rg_typeface_rogue_letters_regular_size;
extern const unsigned char rg_typeface_rogue_letters_names_regular[];
extern const size_t rg_typeface_rogue_letters_names_regular_size;

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
    // Cell pixels opened between a sign's two parts (the clock's colon:
    // the console's dots are farther apart than the typeface's).
    float gap = 0.0f;
    // The glyph's code where the character alone does not find it: a
    // symbol's code in the game's width table (character 0 there: the
    // plus-minus, the times sign...); every glyph's code on a European page
    // (TypefaceFont::byCode); an accent's cell in the European menu font
    // (character 0: the table's characters are ASCII). 0 otherwise.
    uint8_t code = 0u;
};

// A row's name in the traces and the sheets' file names: its character, or
// for one the table has as 0 (a symbol, an accent) its code.
std::string glyphLabel(const TypefaceGlyph &glyph, bool forFile)
{
    char text[8];
    if (glyph.character != 0)
    {
        std::snprintf(text, sizeof(text), forFile ? "%02X" : "%c", static_cast<unsigned char>(glyph.character));
    }
    else
    {
        std::snprintf(text, sizeof(text), forFile ? "c%u" : "#%u", unsigned(glyph.code));
    }
    return text;
}
struct TypefaceFont
{
    // The game font: the middle of its pages' names (FontTex_normal_EN_0.tm2),
    // with "_eur" for the European text's (FontTex_normal_EUR_0.tm2).
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
    // The lean of a slanted set (the money's digits): cell pixels across per
    // cell pixel up from the baseline.
    float slant;
    const TypefaceGlyph *glyphs;
    size_t count;
    // Its glyphs found by their code (TypefaceGlyph::code), not by the US
    // width table's characters: a European page, whose codes past 95 hold
    // other symbols and the accents (107-170).
    bool byCode = false;
};
// A digit of the menus' pictures, or a letter of the menus' own font
// (tools/typeface/pieces.py): the game draws a menu's numbers a digit at a
// time, each from its cell of a row of digits in a picture, and the menus'
// font a letter at a time, its width wide. Known by the hash of its texels
// as the renderer hands them over (hashRgba), at the size the draw samples;
// its set (kPieceFonts), its row in the set's glyphs, the console glyph's
// ink in the sprite drawn as Ink has it, and the row of the cell the sprite
// starts at (a sign drawn from a sprite inside its cell: the small slash).
struct Piece
{
    uint64_t hash;
    uint8_t set, glyph, width, height, inkLeft, inkRight, inkBottom;
    uint8_t top = 0u;
};
// A word of the menus' pictures (tools/typeface/pieces.py WORDS), drawn
// whole: known by its picture's palette and the hash of its box's texels;
// the box's place and size in the picture, the console word's ink in the
// box (as Ink has it), the shift of the drawn word's ink centre from the
// console's, its fit (a TypefaceFont whose cell is the box), and its glyphs,
// each at its pen position (cell pixels from the word's start).
struct WordGlyph
{
    uint16_t glyph;
    float pen;
};
struct PieceWord
{
    uint64_t palette, hash;
    uint16_t x, y;
    uint8_t width, height, inkLeft, inkRight, inkBottom;
    float shift;
    const TypefaceFont *style;
    const WordGlyph *glyphs;
    size_t count;
    // A soft dark shadow under the word instead of an edge (the battle's
    // end: Exp, Skills): the core blurred (sigma, cell pixels), moved
    // (cell pixels across and down) and faded; no shadow at strength 0.
    uint8_t shadowX = 0u, shadowY = 0u;
    float shadowSigma = 0.0f, shadowStrength = 0.0f;
    // Printed on a button's fill (the framed Player, kOnButton): the box's
    // texels are the word's and the fill's; the fill is put back under the
    // new word, each row from its colour left of the box to its colour right
    // of it. Printed on a band of colour across the box (the items' MAX,
    // kOnBar): a row of the box opaque at both ends is the band's, its
    // texels but the word's core the band; the band is put back, each such
    // row from its colour at one end to its colour at the other. Printed on
    // art (the Revelation Flow's title on the frame's wood, kOnArt): the box
    // is the word's texels', told from the art's by colour; the art is put
    // back, each run of the word's texels in a row from the art left of it
    // to the art right of it.
    uint8_t backing = 0u;
};
constexpr uint8_t kOnButton = 1u, kOnBar = 2u, kOnArt = 3u;

// A texel of a word on a button (buttonWord): the word's grey core, its dark
// edge, or the button's blue fill. tools/typeface/pieces.py button_kind is
// the same.
enum class ButtonTexel
{
    Core,
    Edge,
    Fill
};
ButtonTexel buttonTexel(const uint8_t *t)
{
    const float light = (3.0f * t[0] + 6.0f * t[1] + float(t[2])) / 10.0f;
    if (light < 45.0f)
    {
        return ButtonTexel::Edge;
    }
    return int(t[2]) - int(t[0]) < 0x38 && light > 90.0f ? ButtonTexel::Core : ButtonTexel::Fill;
}

// A texel of a word on a band (the items' MAX): its pale blue core, its dark
// blue edge (and the band's), or the band's saturated blue.
// tools/typeface/pieces.py bar_kind is the same.
ButtonTexel barTexel(const uint8_t *t)
{
    const float light = (3.0f * t[0] + 6.0f * t[1] + float(t[2])) / 10.0f;
    if (light < 70.0f)
    {
        return ButtonTexel::Edge;
    }
    return int(t[2]) - int(t[0]) > 110 && light < 115.0f ? ButtonTexel::Fill : ButtonTexel::Core;
}

// A texel of a word printed on art (the Revelation Flow's title): the
// word's (its lavender core, grey bevel, black edge: blue as much as red or
// more), or the art's (the wood and the frame, red well above blue).
// tools/typeface/pieces.py art_kind is the same.
bool artWord(const uint8_t *t)
{
    return t[3] >= 0x40u && int(t[2]) + 4 >= int(t[0]);
}

// A row of a word's box (its texels, `width` of them) that is a band's:
// opaque at both ends.
bool bandRow(const uint8_t *row, uint32_t width)
{
    return width != 0u && row[3] >= 0x40u && row[(width - 1u) * 4u + 3u] >= 0x40u;
}

// Whether a texel of a word's box, in `row` of it, is its button's or band's
// (left out of the word's colours, put back under the new word).
bool backingAt(uint8_t backing, const uint8_t *row, uint32_t width, uint32_t x)
{
    const uint8_t *t = row + size_t(x) * 4u;
    return backing == kOnBar   ? bandRow(row, width) && barTexel(t) != ButtonTexel::Core
           : backing == kOnArt ? !artWord(t)
                               : buttonTexel(t) == ButtonTexel::Fill;
}

// The kind of a texel of a word on a button, a band or art (on art, a dark
// texel of the word is its edge).
ButtonTexel backingKind(uint8_t backing, const uint8_t *t)
{
    if (backing == kOnArt)
    {
        const float light = (3.0f * t[0] + 6.0f * t[1] + float(t[2])) / 10.0f;
        return !artWord(t) ? ButtonTexel::Fill : light < 45.0f ? ButtonTexel::Edge : ButtonTexel::Core;
    }
    return backing == kOnBar ? barTexel(t) : buttonTexel(t);
}

#include "typeface_table.inc"

// The English pages' fonts, then the European text's (their pages named EUR).
constexpr const TypefaceFont *kFonts[] = {&kNormalFont,    &kSystemFont,    &kNameFont,
                                          &kNormalEurFont, &kSystemEurFont, &kNameEurFont};

struct Typeface
{
    const char *file;
    const unsigned char *data;
    const size_t *size;
    // Drawn after the console's own glyphs (Rogue Letters): see
    // drawnAfterConsole.
    bool afterConsole = false;
    stbtt_fontinfo info{};
    bool tried = false, ready = false;
};
Typeface g_typefaces[] = {
    {"Andika-Regular.ttf", rg_typeface_andika_regular, &rg_typeface_andika_regular_size},
    {"Andika-Bold.ttf", rg_typeface_andika_bold, &rg_typeface_andika_bold_size},
    {"RogueLetters-Regular.ttf", rg_typeface_rogue_letters_regular, &rg_typeface_rogue_letters_regular_size, true},
    {"RogueLettersNames-Regular.ttf", rg_typeface_rogue_letters_names_regular,
     &rg_typeface_rogue_letters_names_regular_size, true},
};

// A draw past 0x80 alpha (dq8::gfx::RegionRequest::bold; the game's menus
// draw their letters so) doubles the alpha: the console's glyph comes out
// bolder at its texels, the picture here only at its fine edge. A step more
// of growth (about an eighth of a pixel) matches best, for the three fonts
// in Andika (measured before the message font became Rogue Letters), the
// doubled picture at 4x seen at 1x against the console's doubled glyph
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
// A European page's codes: the US page's and its accents and marks (107-170,
// text_language.cpp's kAccentTags).
constexpr uint32_t kEuropeanCodes = 171u;
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
// RG_TYPEFACE_TRACE=timing: how long a glyph takes from the renderer's first
// asking to its picture handed over, a line every two seconds with work.
std::atomic<bool> g_traceTiming{false};
// How much narrower than the console's ink each glyph is drawn (each Andika
// glyph: not one drawn after the console's, see drawnAfterConsole), in
// tenths of a console pixel, before its font's stroke growth (draw): half a
// pixel, picked by the user from the game's screens against one and one and
// a half (2026-11-24). RG_TYPEFACE_GAP=N, 0 to 40, for comparing.
constexpr int kGapOpening = 5;
std::atomic<int> g_gapOpening{kGapOpening};
// The same for the names' fonts in Andika (name_eur, the European text's
// speaker's name over the dialogue and names over heads; the English one,
// name, is Rogue Letters Names, drawn after the console's), drawn with a
// border ring a pixel wide that took the half pixel's gaps: the name's
// letters touched (Simon's cores 0.75, 0.25, 0 and 0.5 pixels apart, the
// console's 2, 2, 1 and 1; with this, 1.5, 1.5, 1 and 1). One and a half,
// picked by the user against one (2026-11-25). The menus' and the load
// screen's letters keep half a pixel, which the user kept there;
// RG_TYPEFACE_GAP does not change this one.
constexpr int kNameGapOpening = 15;
// The "+" (the settings pages' - and + buttons, the texts' bonuses) is drawn
// with arms that come out equal on the screen, whatever its shape
// (setPixelShape): not with the even gaps' width above, nor the message
// font's own. Andika's "+", narrow in the cell as the fit draws every glyph,
// came out wider than tall once those gaps stretched it to the console's
// ink, and half as wide again on a 16:9 screen; the message font's, its arms
// equal on a 4:3 screen, a third longer across on 16:9 (the user,
// 2026-11-25). The shape a frame pixel is shown at, width over height, in
// kShapeSteps: 37 a 4:3 picture's (7/6), 50 a 16:9 one's.
constexpr int kShapeSteps = 32;
std::atomic<int> g_shapeStep{37};
// A new shape is taken once the window has shown it this many frames in a
// row: a window's edge dragged under Auto with Widescreen (or Stretch) goes
// through a shape a frame, and each would let every larger picture go
// (setPixelShape).
constexpr int kShapeSettleFrames = 10;

// A typeface drawn after the console's own glyphs (Rogue Letters, the English
// message font's, and Rogue Letters Names, the English names';
// Typeface::afterConsole): each glyph is already as wide as the console's
// ink, so it keeps its width and the table's place, and the gaps are the
// console's less the font's stroke growth, as the user saw and kept them in
// the game (2026-11-25). The names' gaps come out so a little wider than the
// console's (Simon's cores 2.5, 2.25, 1.75 and 2 pixels apart, the console's
// 2, 2, 1 and 1, as a 4x capture's warm fill shows them). The even gaps
// above are for Andika's (the European message and names fonts' too). Only
// fields never written after start are read here: no lock.
bool drawnAfterConsole(const TypefaceFont &font)
{
    for (const Typeface &face : g_typefaces)
    {
        if (std::strcmp(face.file, font.file) == 0)
        {
            return face.afterConsole;
        }
    }
    return false;
}
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
    // The rows a draw takes when fewer than the cell's: the names over heads
    // (monsters', other people's) take the name font's cells a row short.
    uint8_t rows = 0u;
};

// RG_TYPEFACE_SHEET=DIR: as each page is read, every glyph the table redraws,
// the console's (enlarged) beside the typeface's plain and bold, and the
// plain over its border and over its shadow, into
// DIR/typeface_<font>_<scale>x.png, at RG_TYPEFACE_SHEET_SCALE (4), and
// beside it typeface_<font>_<scale>x.txt, which glyph each entry is. Made
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
// ones the glyphs were seen drawn with (RG_TYPEFACE_TRACE=palettes); then
// the European pages' three (other alpha levels; the pages are the same in
// the four languages' packs), seen with the text in French. In
// case the hash ever changes, every palette is asked about from the first
// page read until a glyph matches with a known palette (a few seconds of
// boot), or, if none does, until a glyph of each font read has matched;
// meanwhile a palette a glyph turns up with is learned, when it gives all
// its levels one colour (the picture has one), up to kLearnedPalettes. A
// known palette matched drops the learned ones: the same letters redrawn
// at every run.
constexpr uint64_t kKnownPalettes[] = {0x33A2CFEFAEC9F88Bull, 0xCFBAE4E31E471D6Eull, 0x75B5F042466BE787ull,
                                       0x9DF81CD921347BF4ull, 0x397678EBC6867E16ull, 0x0CBE16E636C4943Bull};
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

// Every byte of the texels: a piece's identity (tools/typeface/pieces.py's
// hash_rgba).
uint64_t hashRgba(const uint8_t *rgba, uint32_t texels)
{
    uint64_t hash = 0xcbf29ce484222325ull;
    for (uint32_t i = 0; i < texels * 4u; ++i)
    {
        hash ^= rgba[i];
        hash *= 0x100000001b3ull;
    }
    return hash;
}

// A palette of the menus' pictures the table knows. The European text's
// pictures hold the same digits in palettes of their own (each language's)
// and a menu font of their own, which the table has apart; its picture words
// match no English one and stay the game's.
bool piecePalette(uint64_t clut)
{
    return std::find(std::begin(kPiecePalettes), std::end(kPiecePalettes), clut) != std::end(kPiecePalettes);
}

// Palettes our own code changed (notePaletteAlias): changed, original.
std::mutex g_aliasesMutex;
std::vector<std::pair<uint64_t, uint64_t>> g_aliases;

uint64_t unaliased(uint64_t clut)
{
    std::lock_guard<std::mutex> lock(g_aliasesMutex);
    for (const auto &[changed, original] : g_aliases)
    {
        if (changed == clut)
        {
            return original;
        }
    }
    return clut;
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
    for (uint32_t code = 0; code < (font.byCode ? kEuropeanCodes : kTableCodes); ++code)
    {
        // A character by itself, a symbol (named 0) by its code; on a
        // European page every glyph by its code.
        char character = 0;
        if (!font.byCode)
        {
            const uint8_t *entry = getConstMemPtr(rdram, kFontTable + 4u * code);
            if (!entry)
            {
                break;
            }
            character = static_cast<char>(entry[0]);
        }
        int16_t glyph = -1;
        for (size_t i = 0; i < font.count; ++i)
        {
            if (font.byCode            ? font.glyphs[i].code == code
                : character != 0       ? font.glyphs[i].character == character
                                       : font.glyphs[i].character == 0 && font.glyphs[i].code == code)
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
        // The cell a row short too, when its last row is clear (the console's
        // glyphs leave it so): the names over heads are drawn that way.
        if (inkBottom < int(ch))
        {
            Cell shorter = cell;
            shorter.rows = static_cast<uint8_t>(ch - 1u);
            const auto [at, fresh] = g_cells.emplace(hashAlpha(rgba.data(), cw * (ch - 1u)), shorter);
            if (!fresh && (at->second.font != shorter.font || at->second.glyph != shorter.glyph ||
                           at->second.rows != shorter.rows))
            {
                at->second.glyph = -1;
            }
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

// Every larger picture the renderer holds let go (rg::texturePack::refresh),
// and so the "+" pictures it was handed (g_shapesHanded): a shape change
// after it has none to refresh.
void refreshPictures();

// After ArchiveFindEntryByName: a font's first page (FontTex_normal_EN_0.tm2),
// read where the pack lies; with the European text (src/text_language.cpp
// asks for the EUR pages: FontTex_normal_EUR_0.tm2) the European font's.
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
    auto endsWith = [&](const std::string &suffix) {
        return lower.size() > suffix.size() && lower.compare(lower.size() - suffix.size(), suffix.size(), suffix) == 0;
    };
    const bool european = endsWith("_eur_0.tm2");
    if (lower.rfind(kPrefix, 0) != 0 || !(european || endsWith("_en_0.tm2")))
    {
        return;
    }
    const std::string font = lower.substr(sizeof(kPrefix) - 1u, lower.find('_', sizeof(kPrefix) - 1u) -
                                                                    (sizeof(kPrefix) - 1u)) +
                             (european ? "_eur" : "");
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
                refreshPictures();
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

// A piece's colours, from its own texels (pieceColours): its edge's, how
// opaque the edge is, and its core's row by row (a digit is often shaded
// from top to bottom). The tallest piece: the Revelation Flow's title is
// drawn with its frame's wood, 46 rows.
constexpr uint32_t kPieceRows = 64u;
// A word is redrawn in a piece up to this tall, but on art (the title's).
constexpr uint32_t kWordRows = 32u;
struct PieceColours
{
    uint8_t edge[3] = {};
    uint8_t edgeAlpha = 0u; // 0x80 opaque
    uint8_t rows[kPieceRows][3] = {};
};

struct Job
{
    uint64_t key;
    const TypefaceFont *font;
    int16_t glyph;
    Ink ink;
    uint32_t scale;
    bool bold;
    Version version;
    uint8_t colour[3];
    // A piece of a menu picture: the size its draw samples, and its colours.
    bool piece = false;
    uint8_t regionWidth = 0u, regionHeight = 0u;
    PieceColours colours{};
    // With RG_TYPEFACE_SHEET, the piece's console texels (writePieceSheet).
    std::vector<uint8_t> console;
    // A word: drawn over its whole piece (regionWidth x regionHeight), the
    // console word's box at this place in it (ink holds the console word's
    // ink in the piece).
    const PieceWord *word = nullptr;
    uint8_t offsetX = 0u, offsetY = 0u;
    // A piece drawn from a sprite starting this many rows into its cell
    // (Piece::top): the baseline, a cell's row, is that much higher in it.
    uint8_t spriteTop = 0u;
    // The piece holds other art away from the word (console holds the
    // piece): kept as the game draws it, under the word.
    bool keep = false;
    // With the timing trace: when it was queued.
    std::chrono::steady_clock::time_point asked{};
    // g_generation when it was queued: made after the pictures were
    // forgotten, it is dropped.
    uint64_t generation = 0u;
    // With RG_TYPEFACE_SHEET, the piece's texels as the game has them, where
    // console has a word's backing put back (writePieceSheet).
    std::vector<uint8_t> sheetConsole;
    // A font's "+": the screen shape it is drawn for (g_shapeStep's steps);
    // 0 for every other glyph.
    int shapeStep = 0;
};

// The screen shape a font's glyph is drawn for: the current one for the "+"
// (g_shapeStep), 0 for the others.
int shapeStepFor(const TypefaceFont &font, int16_t glyph)
{
    return glyph >= 0 && size_t(glyph) < font.count && font.glyphs[glyph].character == '+'
               ? g_shapeStep.load(std::memory_order_relaxed)
               : 0;
}
struct Picture
{
    uint32_t width = 0u, height = 0u;
    // RGBA, or for a font's glyph its coverage alone (the GS's alpha, a byte
    // a texel), its colour given when it is handed over.
    std::vector<uint8_t> rgba;
    bool coverage = false;
    // When it was last handed over or made (g_readyClock).
    uint64_t used = 0u;
    // Handed over at least once (drawn ahead, it may never be).
    bool handed = false;
};

// The timing trace's sums since its last line (under g_jobsMutex): the jobs
// drawn, their time in the queue and drawing, the pictures handed over and
// their time from asking to handover, the longest queue.
struct Timing
{
    std::chrono::steady_clock::time_point since = std::chrono::steady_clock::now();
    unsigned drawn = 0u, ahead = 0u, handed = 0u;
    double queuedMs = 0.0, queuedMaxMs = 0.0, drawMs = 0.0, drawMaxMs = 0.0, handedMs = 0.0, handedMaxMs = 0.0;
    size_t queueMax = 0u;
};
Timing g_timing;

double millisecondsSince(std::chrono::steady_clock::time_point from, std::chrono::steady_clock::time_point to)
{
    return std::chrono::duration<double, std::milli>(to - from).count();
}

// Under g_jobsMutex: the line, every two seconds with work, then the sums
// started again.
void reportTiming(std::chrono::steady_clock::time_point now)
{
    if (!g_traceTiming || now - g_timing.since < std::chrono::seconds(2))
    {
        return;
    }
    if (g_timing.drawn != 0u || g_timing.handed != 0u)
    {
        std::fprintf(stderr,
                     "[typeface] timing %.1f s: %u drawn, %u of them ahead (the others queued %.1f ms mean, %.1f "
                     "most; drawing %.2f ms mean, %.2f most), %u handed over %.1f ms mean after the asking, %.1f "
                     "most; queue up to %zu\n",
                     millisecondsSince(g_timing.since, now) / 1000.0, g_timing.drawn, g_timing.ahead,
                     g_timing.drawn > g_timing.ahead ? g_timing.queuedMs / (g_timing.drawn - g_timing.ahead) : 0.0,
                     g_timing.queuedMaxMs,
                     g_timing.drawn ? g_timing.drawMs / g_timing.drawn : 0.0, g_timing.drawMaxMs, g_timing.handed,
                     g_timing.handed ? g_timing.handedMs / g_timing.handed : 0.0, g_timing.handedMaxMs,
                     g_timing.queueMax);
    }
    g_timing = Timing{};
    g_timing.since = now;
}

// The jobs, drawn by a few threads (drawer): the renderer's first, then the
// glyphs drawn ahead (drawAhead). Until a picture is made the renderer draws
// the console's glyph, so a new screen's letters changed a moment after it
// showed: with one thread, a third of a second on average and up to 0.6 s
// (93 glyphs at 4x, 7 to 11 ms each, RG_TYPEFACE_TRACE=timing, 2026-11-24).
std::mutex g_jobsMutex;
std::condition_variable g_wake;
std::deque<Job> g_queue, g_ahead;
// Queued or being drawn; and of them, those still in g_ahead.
std::unordered_set<uint64_t> g_asked, g_aheadKeys;
// Made, and kept once handed over: the renderer asks again when its copy is
// let go (its budget, a switch of the letters), and the same shape in another
// colour is the same picture. Up to kKeptBytes (some 1,800 glyphs with their
// border's margin at 8x), the least recently used going first.
std::unordered_map<uint64_t, Picture> g_ready;
size_t g_readyBytes = 0u;
uint64_t g_readyClock = 0u;
constexpr size_t kKeptBytes = size_t(64u) << 20u;
// The fonts' styles drawn ahead (drawAhead's style: the font, scale,
// version, bold draw and rows).
std::unordered_set<uint64_t> g_aheadStyles;
// Pictures drawn ahead and let go by the budget before they were ever handed
// over: made again only when asked, or past the budget each eviction would
// have every style asked draw its unused glyphs again, evicting others'.
std::unordered_set<uint64_t> g_notAhead;
// The screen shapes of the "+" pictures handed over since the renderer last
// let its copies go: one drawn for another shape asks it to (setPixelShape).
std::unordered_set<int> g_shapesHanded;
// Counts forgetPicturesLocked: a job queued before it is dropped when made.
uint64_t g_generation = 0u;
// With the timing trace: when each picture not ready was first and last
// asked for (the renderer asks again at each draw while it waits).
struct Waiting
{
    std::chrono::steady_clock::time_point first, last;
};
std::unordered_map<uint64_t, Waiting> g_waiting;
// RG_TYPEFACE_AHEAD=0 (for the timing trace's comparisons): no glyphs drawn
// ahead.
std::atomic<bool> g_drawAhead{true};

// Every picture made or asked for is forgotten: what is asked again is made
// again (under g_jobsMutex).
void forgetPicturesLocked()
{
    g_queue.clear();
    g_ahead.clear();
    g_asked.clear();
    g_aheadKeys.clear();
    g_ready.clear();
    g_readyBytes = 0u;
    g_aheadStyles.clear();
    g_notAhead.clear();
    g_waiting.clear();
    g_shapesHanded.clear();
    ++g_generation;
}

// Room for `adding` bytes more within kKeptBytes: the least recently used
// pictures dropped, down to three quarters of it (under g_jobsMutex).
void keepWithinBudget(size_t adding)
{
    if (g_readyBytes + adding <= kKeptBytes)
    {
        return;
    }
    std::vector<std::pair<uint64_t, uint64_t>> byUse; // when used, key
    byUse.reserve(g_ready.size());
    for (const auto &[key, picture] : g_ready)
    {
        byUse.emplace_back(picture.used, key);
    }
    std::sort(byUse.begin(), byUse.end());
    for (const auto &[used, key] : byUse)
    {
        if (g_readyBytes + adding <= kKeptBytes / 4u * 3u)
        {
            break;
        }
        const auto found = g_ready.find(key);
        g_readyBytes -= found->second.rgba.size();
        if (!found->second.handed)
        {
            g_notAhead.insert(key);
        }
        g_ready.erase(found);
    }
    // Some styles lost pictures: drawn ahead again when next asked
    // (drawAhead passes over the pictures still kept, and those never shown).
    g_aheadStyles.clear();
}

void forgetPictures()
{
    std::lock_guard<std::mutex> lock(g_jobsMutex);
    forgetPicturesLocked();
}

void refreshPictures()
{
    // Cleared first: a "+" handed between the two is let go too, and at
    // worst asks one refresh more of a later shape change.
    {
        std::lock_guard<std::mutex> lock(g_jobsMutex);
        g_shapesHanded.clear();
    }
    rg::texturePack::refresh();
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

// The width of a glyph's strokes across, in render pixels: the median of its
// rows' runs of ink at least half covered; alongY, their height down: the
// median of its columns' runs in the rows from `from` on.
int strokeWidth(const std::vector<uint8_t> &image, int width, int height, bool alongY = false, int from = 0)
{
    std::vector<int> runs;
    const int lines = alongY ? width : height, length = alongY ? height : width;
    for (int n = 0; n < lines; ++n)
    {
        int run = 0;
        for (int i = alongY ? std::max(0, from) : 0; i <= length; ++i)
        {
            if (i < length && image[alongY ? size_t(i) * width + n : size_t(n) * width + i] >= 128u)
            {
                ++run;
            }
            else if (run > 0)
            {
                runs.push_back(run);
                run = 0;
            }
        }
    }
    if (runs.empty())
    {
        return 0;
    }
    std::nth_element(runs.begin(), runs.begin() + runs.size() / 2u, runs.end());
    return runs[runs.size() / 2u];
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

// A glyph's bitmap stretched (factor above 1) or squeezed across about its
// ink centre, in room made each side first; then `back` render pixels taken
// off each side of its strokes (stretched) or put back on (squeezed), so they
// keep their width. The ink's columns found again, and its place on the
// canvas (`left`) kept. False when no ink is left.
bool scaleAcross(std::vector<uint8_t> &bitmap, int &gw, int gh, int &inkLeft, int &inkRight, int &left,
                 double factor, int back, int r)
{
    const double inkWidth = inkRight + 1 - inkLeft;
    const int extra = int(std::ceil(std::max(0.0, inkWidth * (factor - 1.0)) / 2.0)) + r;
    std::vector<uint8_t> wider(size_t(gw + 2 * extra) * gh, 0u);
    for (int y = 0; y < gh; ++y)
    {
        std::copy_n(bitmap.begin() + size_t(y) * gw, gw, wider.begin() + size_t(y) * (gw + 2 * extra) + extra);
    }
    bitmap = std::move(wider);
    gw += 2 * extra;
    inkLeft += extra;
    inkRight += extra;
    squeeze(bitmap, gw, gh, false, (inkLeft + inkRight + 1) * 0.5, factor, 0, gw);
    spread(bitmap, gw, gh, false, back, factor > 1.0);
    inkLeft = gw, inkRight = -1;
    for (int y = 0; y < gh; ++y)
    {
        for (int x = 0; x < gw; ++x)
        {
            if (bitmap[size_t(y) * gw + x])
            {
                inkLeft = std::min(inkLeft, x);
                inkRight = std::max(inkRight, x);
            }
        }
    }
    if (inkRight < 0)
    {
        return false;
    }
    left -= extra;
    return true;
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

// A shadowed word's shadow at the picture's scale: its core (coverage
// 0-255, ow x oh) blurred by a Gaussian, moved down and right and faded,
// 0-255, as tools/typeface/pieces.py's shadow_core fitted it.
std::vector<float> softShadow(const std::vector<float> &cover, int ow, int oh, const PieceWord &word,
                              uint32_t scale)
{
    const float sigma = std::max(0.3f, word.shadowSigma * float(scale));
    const int radius = int(std::ceil(sigma * 3.0f));
    std::vector<float> kernel(size_t(2 * radius + 1));
    float total = 0.0f;
    for (int k = -radius; k <= radius; ++k)
    {
        kernel[size_t(k + radius)] = std::exp(-0.5f * float(k * k) / (sigma * sigma));
        total += kernel[size_t(k + radius)];
    }
    for (float &k : kernel)
    {
        k /= total;
    }
    std::vector<float> across(cover.size(), 0.0f), down(cover.size(), 0.0f);
    for (int y = 0; y < oh; ++y)
    {
        for (int x = 0; x < ow; ++x)
        {
            float sum = 0.0f;
            for (int k = -radius; k <= radius; ++k)
            {
                if (x + k >= 0 && x + k < ow)
                {
                    sum += kernel[size_t(k + radius)] * cover[size_t(y) * ow + x + k];
                }
            }
            across[size_t(y) * ow + x] = sum;
        }
    }
    for (int y = 0; y < oh; ++y)
    {
        for (int x = 0; x < ow; ++x)
        {
            float sum = 0.0f;
            for (int k = -radius; k <= radius; ++k)
            {
                if (y + k >= 0 && y + k < oh)
                {
                    sum += kernel[size_t(k + radius)] * across[size_t(y + k) * ow + x];
                }
            }
            down[size_t(y) * ow + x] = sum;
        }
    }
    const int dx = int(word.shadowX) * int(scale), dy = int(word.shadowY) * int(scale);
    std::vector<float> out(cover.size(), 0.0f);
    for (int y = dy; y < oh; ++y)
    {
        for (int x = dx; x < ow; ++x)
        {
            out[size_t(y) * ow + x] = std::min(255.0f, down[size_t(y - dy) * ow + x - dx] * word.shadowStrength);
        }
    }
    return out;
}

// The glyph at `scale` host pixels a cell pixel, as the fit placed it: drawn
// at kRender pixels a cell pixel, grown, and averaged down (or its border or
// shadow, with their margin; or a word, its glyphs drawn together, its box
// as the cell).
Picture draw(const Job &job)
{
    Picture picture;
    const TypefaceFont &font = *job.font;
    // A word is drawn over its whole piece (its edge may reach past the
    // console word's box into the clear around it): its baseline, a row of
    // its box, moved down to the box's place.
    const TypefaceGlyph wordGlyph{' ', 0u, job.word ? job.word->shift : 0.0f, float(job.offsetY)};
    const TypefaceGlyph &glyph = job.word ? wordGlyph : font.glyphs[job.glyph];
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
    // The glyphs drawn, each at its pen position (cell pixels; one at 0 for
    // a letter), on one baseline: the bitmap spans them all, its top row
    // `by0` render pixels from the baseline.
    std::vector<std::pair<int, int>> drawn; // glyph, render pixels across
    if (job.word)
    {
        for (size_t i = 0; i < job.word->count; ++i)
        {
            drawn.emplace_back(job.word->glyphs[i].glyph, int(std::lround(job.word->glyphs[i].pen * float(r))));
        }
    }
    else
    {
        drawn.emplace_back(glyph.glyph, 0);
    }
    int minX = INT32_MAX, minY = INT32_MAX, maxX = INT32_MIN, maxY = INT32_MIN;
    for (const auto &[index, at] : drawn)
    {
        int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        stbtt_GetGlyphBitmapBox(&face->info, index, scaleX, scaleY, &x0, &y0, &x1, &y1);
        if (x1 > x0 && y1 > y0)
        {
            minX = std::min(minX, at + x0), maxX = std::max(maxX, at + x1);
            minY = std::min(minY, y0), maxY = std::max(maxY, y1);
        }
    }
    if (maxX <= minX || maxY <= minY)
    {
        return picture;
    }
    int gw = maxX - minX, gh = maxY - minY, by0 = minY;
    std::vector<uint8_t> bitmap(size_t(gw) * gh, 0u);
    std::vector<uint8_t> one;
    for (const auto &[index, at] : drawn)
    {
        int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        stbtt_GetGlyphBitmapBox(&face->info, index, scaleX, scaleY, &x0, &y0, &x1, &y1);
        const int w = x1 - x0, h = y1 - y0;
        if (w <= 0 || h <= 0)
        {
            continue;
        }
        one.assign(size_t(w) * h, 0u);
        stbtt_MakeGlyphBitmap(&face->info, one.data(), w, h, w, scaleX, scaleY, index);
        for (int y = 0; y < h; ++y)
        {
            uint8_t *to = bitmap.data() + size_t(y + y0 - minY) * gw + (at + x0 - minX);
            for (int x = 0; x < w; ++x)
            {
                to[x] = std::max(to[x], one[size_t(y) * w + x]);
            }
        }
    }
    if (glyph.gap > 0.0f)
    {
        // The rows below the middle of the widest clear band between the
        // sign's parts moved down by the gap, those above it up by half, so
        // its centre stays.
        std::vector<bool> inked(static_cast<size_t>(gh), false);
        for (int y = 0; y < gh; ++y)
        {
            for (int x = 0; x < gw && !inked[size_t(y)]; ++x)
            {
                inked[size_t(y)] = bitmap[size_t(y) * gw + x] != 0u;
            }
        }
        int split = -1, widest = 0;
        for (int y = 0; y < gh;)
        {
            if (inked[size_t(y)])
            {
                ++y;
                continue;
            }
            int end = y;
            while (end < gh && !inked[size_t(end)])
            {
                ++end;
            }
            if (y > 0 && end < gh && end - y > widest)
            {
                widest = end - y, split = (y + end) / 2;
            }
            y = end;
        }
        const int add = int(std::lround(glyph.gap * float(r)));
        if (split > 0 && add > 0)
        {
            std::vector<uint8_t> opened(size_t(gw) * (gh + add), 0u);
            std::copy(bitmap.begin(), bitmap.begin() + size_t(split) * gw, opened.begin());
            std::copy(bitmap.begin() + size_t(split) * gw, bitmap.end(), opened.begin() + size_t(split + add) * gw);
            bitmap = std::move(opened);
            gh += add;
            by0 -= add / 2;
        }
    }
    if (font.slant != 0.0f)
    {
        // Leant: each row moved across by the slant times its height above
        // the baseline (row -by0), split between the two pixels it falls on.
        const int base = -by0;
        const int extra = int(std::ceil(std::abs(font.slant) * float(std::max(base, gh - base)))) + 1;
        const int wide = gw + 2 * extra;
        std::vector<float> leant(size_t(wide) * gh, 0.0f);
        for (int y = 0; y < gh; ++y)
        {
            const float shift = font.slant * float(base - y) + float(extra);
            const int whole = int(std::floor(shift));
            const float part = shift - float(whole);
            for (int x = 0; x < gw; ++x)
            {
                const float value = bitmap[size_t(y) * gw + x];
                const int to = x + whole;
                if (value == 0.0f || to < 0 || to + 1 >= wide)
                {
                    continue;
                }
                leant[size_t(y) * wide + to] += value * (1.0f - part);
                leant[size_t(y) * wide + to + 1] += value * part;
            }
        }
        bitmap.assign(size_t(wide) * gh, 0u);
        for (size_t i = 0; i < bitmap.size(); ++i)
        {
            bitmap[i] = static_cast<uint8_t>(std::min(255.0f, leant[i] + 0.5f));
        }
        gw = wide;
    }
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
    // The picture's texels: the font's cell (a row short for the names over
    // heads), or the size a piece's draw samples (its cell, up to two texels
    // narrower or one wider, a row less or more; a word's whole piece).
    const uint32_t outW = job.regionWidth ? job.regionWidth : font.cellWidth;
    const uint32_t outH = job.regionHeight ? job.regionHeight : font.cellHeight;
    const int cw = int(std::max(font.cellWidth, outW)) * r, ch = int(std::max(font.cellHeight, outH)) * r;
    const int width = cw + 2 * offX, height = ch + 2 * offY;
    std::vector<uint8_t> canvas(size_t(width) * height, 0u);
    // The ink centre on the console glyph's (and the table's shift), the
    // baseline on the table's row (and the sign's lift).
    int left =
        int(std::lround((job.ink.centre() + glyph.shift) * float(r) - (inkLeft + inkRight + 1) * 0.5f)) + offX;
    int top = int(std::lround((font.baseline + glyph.lift - float(job.spriteTop)) * float(r))) + by0 + offY;
    // What reaches past the console glyph is brought in, so nothing is cut
    // flat at the cell's edge and nothing runs into the next letter (in
    // bitmap rows and columns; canvas = bitmap + left or top):
    // - a descender longer than the console's (the typeface's g, j, p, q, y)
    //   ends where the console's does, the rows below the baseline squeezed,
    //   as the console's short descenders are;
    // - a glyph past the console's ink by more than half a pixel a side, or
    //   past the cell's last shown column, is moved back inside when it is
    //   no wider than that room; one wider (the W) is centred in the room
    //   and narrowed to it, its strokes keeping their width (narrowed where
    //   the fit's shift put it, the W came out a pixel off, a gap after it);
    // - one past the cell's top (the tall signs: / [ ] { }) has the rows
    //   above the baseline squeezed down to it.
    // Half a pixel past the console's ink is let be (a round letter's
    // overshoot); the console's ascenders are lower than the typeface's, and
    // those are let be too.
    const int baseRow = -by0;
    // A glyph's last row is never shown, as its last column (below): drawn a
    // row short (the names over heads), the console's descenders reach it,
    // and the typeface's end a row above, with no overshoot let be. The name
    // font is drawn so over heads while its edge's copies take whole cells:
    // its descenders end there in every draw, so the glyph, its edge and its
    // shadow agree (an edge ringing a longer g left a black band below it).
    const uint32_t shownEnd = job.piece                ? 0u
                              : &font == &kNameFont || &font == &kNameEurFont ? font.cellHeight - 2u
                              : job.ink.bottom >= outH ? outH - 1u
                                                       : 0u;
    const bool lastRow = shownEnd != 0u && job.ink.bottom > shownEnd;
    const int endRow = int(lastRow ? shownEnd : job.ink.bottom) * r + offY - top;
    if (baseRow >= 0 && inkEnd > endRow + (lastRow ? 0 : r / 2) && endRow > baseRow + 1)
    {
        // The whole glyph squeezed from its top, as the console's pixel fonts
        // draw a descender letter's bowl shorter than the others' (the names'
        // g ends its bowl two rows above the a's): the top stays on the
        // letters', and the bowl, the gap and the tail keep their proportions
        // (squeezed below the baseline alone, the names' tail touched the
        // bowl). Its strokes across are then widened back by what the
        // squeeze took, as the narrowed W's: squeezed a little further first,
        // so it still ends at endRow. A word (a whole word is one glyph here)
        // has only its rows below the baseline squeezed, its letters' tops
        // and bottoms staying on the console word's.
        const int from = job.word ? baseRow : inkTop;
        const double room = endRow - from, depth = inkEnd - from;
        const double stroke = std::min(double(strokeWidth(bitmap, gw, gh, true, from)), room / 2.0);
        const double factor = (room - stroke / 2.0) / (depth - stroke / 2.0);
        squeeze(bitmap, gw, gh, true, from, factor, from, gh);
        // Widened from the squeezed rows only: a word's letters above its
        // baseline would otherwise reach down past it.
        std::vector<uint8_t> thick = bitmap;
        std::fill(thick.begin(), thick.begin() + size_t(from) * gw, uint8_t(0));
        spread(thick, gw, gh, true, int(std::lround(stroke * (1.0 - factor) / 2.0)), false);
        std::copy(thick.begin() + size_t(from) * gw, thick.end(), bitmap.begin() + size_t(from) * gw);
    }
    // The cell's last column is never shown (a glyph's sprite spans a texel
    // past its pixels, and the backend clears that one in the picture): the
    // right edge is the column before it, the stroke's growth inside it. A
    // word's piece is clear around the word: its edges are the console
    // word's ink, the growth inside them too.
    const double leftLimit = std::max(0.0, job.ink.left - 0.5) * r + (job.word ? std::max(0, growX) : 0) + offX;
    const double rightLimit =
        (job.word ? (job.ink.right + 0.5) * r - std::max(0, growX)
                  : std::min((double(std::min(font.cellWidth, outW)) - 1.0) * r - std::max(0, growX),
                             (job.ink.right + 0.5) * r)) +
        offX;
    const double room = rightLimit - leftLimit;
    if (job.shapeStep != 0)
    {
        // The "+" (g_shapeStep): its ink as wide as it is tall once shown,
        // each with the growth both ways (below), stretched or squeezed
        // across about its ink centre, its stem with it (scaled with the
        // arms, it keeps to the bar on the screen what it has in the font).
        // The table's shift stays: it puts the stem on the console's.
        const double shape = double(job.shapeStep) / kShapeSteps;
        const double inkWidth = inkRight + 1 - inkLeft;
        double wanted = std::max(double(r), double(inkEnd - inkTop + 2 * growY) / shape - 2.0 * growX);
        if (room > double(r) && wanted > room)
        {
            // Wider than its room (a pixel shown narrow: Stretch in a window
            // near square), it is as wide as the room and as much shorter,
            // squeezed down about its bar: narrowed across alone, by the
            // room's fit below, it stood taller than wide.
            const double height = inkEnd - inkTop;
            const double fits = std::max(double(r), (room + 2.0 * growX) * shape - 2.0 * growY);
            if (fits < height)
            {
                squeeze(bitmap, gw, gh, true, (inkTop + inkEnd) * 0.5, fits / height, 0, gh);
                inkTop = gh, inkEnd = 0;
                for (int y = 0; y < gh; ++y)
                {
                    if (std::any_of(bitmap.begin() + size_t(y) * gw, bitmap.begin() + size_t(y + 1) * gw,
                                    [](uint8_t v) { return v != 0u; }))
                    {
                        inkTop = std::min(inkTop, y);
                        inkEnd = y + 1;
                    }
                }
            }
            wanted = room;
        }
        if (std::abs(wanted - inkWidth) > r / 8.0 &&
            !scaleAcross(bitmap, gw, gh, inkLeft, inkRight, left, wanted / inkWidth, 0, r))
        {
            return picture;
        }
    }
    else if (!job.word && !drawnAfterConsole(font))
    {
        // Every Andika glyph (not one drawn after the console's:
        // drawnAfterConsole) as wide as the console's ink less kGapOpening
        // (the names' fonts kNameGapOpening), so the gaps between letters
        // are the console's, all opened by it: placed
        // on the console's ink centres with the typeface's own widths, the
        // gaps came out uneven (the menus' digits, narrower than the
        // console's, stood apart). Stretched or squeezed about its ink
        // centre, a little further first and its strokes then thinned or
        // widened back across, so they keep their width. The width is the
        // ink's before the font's stroke growth (below), which widens every
        // glyph of the font alike. Its ink centre is the console's: the
        // table's shift was fitted at the typeface's own width.
        left -= int(std::lround(glyph.shift * float(r)));
        const double inkWidth = inkRight + 1 - inkLeft;
        const double consoleWidth = double(job.ink.right - job.ink.left) * r;
        // The names' fonts open by kNameGapOpening: today only the European
        // one comes here, the English names' (Rogue Letters Names) being
        // drawn after the console's (drawnAfterConsole).
        const bool names = job.font == &kNameFont || job.font == &kNameEurFont;
        const int opening = names ? kNameGapOpening : g_gapOpening.load(std::memory_order_relaxed);
        const double wanted = consoleWidth - opening / 10.0 * r;
        const double rawStroke = strokeWidth(bitmap, gw, gh);
        const double stroke = std::min(rawStroke, inkWidth / 2.0);
        // A lone stem or dot (l, i, 1, the colon's dots) keeps its width:
        // stretched, it would stand bolder than the other letters' stems.
        const bool stem = rawStroke >= inkWidth * 0.75;
        if (!stem && std::abs(wanted - inkWidth) > r / 4.0 && inkWidth > stroke && wanted > stroke)
        {
            // Stretched by the factor, then its strokes thinned back.
            const double factor = (wanted - stroke) / (inkWidth - stroke);
            if (!scaleAcross(bitmap, gw, gh, inkLeft, inkRight, left, factor,
                             int(std::lround(stroke * std::abs(factor - 1.0) / 2.0)), r))
            {
                return picture;
            }
        }
    }
    const double centre = (inkLeft + inkRight + 1) * 0.5;
    const double half = (inkRight + 1 - inkLeft) * 0.5;
    const double canvasCentre = left + centre;
    if (room > 0.0 && (canvasCentre - half < leftLimit - 0.5 || canvasCentre + half > rightLimit + 0.5))
    {
        const double wanted = 2.0 * half <= room ? std::clamp(canvasCentre, leftLimit + half, rightLimit - half)
                                                 : (leftLimit + rightLimit) * 0.5;
        left += int(std::lround(wanted - canvasCentre));
        if (2.0 * half > room + 1.0)
        {
            // Narrowed to the room, its strokes would come out thinner than
            // the other letters' (the menus' W): narrowed a little further,
            // its strokes then widened back across by what that took, so they
            // keep their width and the glyph its room.
            const double stroke = std::min(double(strokeWidth(bitmap, gw, gh)), room / 2.0);
            const double factor = (room - stroke) / (2.0 * half - stroke);
            squeeze(bitmap, gw, gh, false, centre, factor, 0, gw);
            spread(bitmap, gw, gh, false, int(std::lround(stroke * (1.0 - factor) / 2.0)), false);
        }
    }
    const int topRow = offY - top;
    if (baseRow > topRow && inkTop < topRow - r / 4)
    {
        squeeze(bitmap, gw, gh, true, baseRow, double(baseRow - topRow) / double(baseRow - inkTop), 0, baseRow);
    }
    // A piece's edge rings the glyph inside the piece: the glyph and its
    // growth stay a cell pixel inside the piece's top and bottom, or the edge
    // is cut flat there (Slot 1's bottom, the battle's slash). Moved into the
    // room the other side has, and squeezed down its rows if that is short.
    if (job.piece && !(job.word && job.word->shadowStrength > 0.0f))
    {
        int first = gh, end = 0;
        for (int y = 0; y < gh; ++y)
        {
            for (int x = 0; x < gw; ++x)
            {
                if (bitmap[size_t(y) * gw + x] >= 128u)
                {
                    first = std::min(first, y);
                    end = y + 1;
                    break;
                }
            }
        }
        const int lowest = offY + r + std::max(0, growY), highest = offY + int(outH) * r - r - std::max(0, growY);
        if (end > first && highest > lowest)
        {
            const int over = top + end - highest, under = lowest - (top + first);
            if (over > 0 && under < 0)
            {
                top -= std::min(over, -under);
            }
            else if (under > 0 && over < 0)
            {
                top += std::min(under, -over);
            }
            const int from = lowest - top, to = highest - top;
            if (first < from || end > to)
            {
                if (job.word)
                {
                    const double factor = double(to - from) / double(end - first);
                    squeeze(bitmap, gw, gh, true, (from - first * factor) / (1.0 - factor), factor, 0, gh);
                }
                else
                {
                    // A single glyph (the attributes' slash) squeezed whole,
                    // across as well about its ink's centre, so it keeps its
                    // slope (squeezed down only, the slash lay flatter than
                    // the console's); its strokes then widened back by what
                    // the squeeze took, as the narrowed W's, so a little more
                    // squeezed first to end within the room.
                    int left0 = gw, right0 = 0;
                    for (int y = 0; y < gh; ++y)
                    {
                        for (int x = 0; x < gw; ++x)
                        {
                            if (bitmap[size_t(y) * gw + x] >= 128u)
                            {
                                left0 = std::min(left0, x);
                                right0 = std::max(right0, x + 1);
                            }
                        }
                    }
                    const double room = to - from, depth = end - first;
                    const double stroke = std::min(double(strokeWidth(bitmap, gw, gh)), room / 2.0);
                    const double factor = (room - stroke) / (depth - stroke);
                    squeeze(bitmap, gw, gh, true, (from + stroke * (1.0 - factor) / 2.0 - first * factor) / (1.0 - factor),
                            factor, 0, gh);
                    if (right0 > left0)
                    {
                        squeeze(bitmap, gw, gh, false, (left0 + right0) * 0.5, factor, 0, gw);
                    }
                    const int back = int(std::lround(stroke * (1.0 - factor) / 2.0));
                    spread(bitmap, gw, gh, false, back, false);
                    spread(bitmap, gw, gh, true, back, false);
                }
            }
        }
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
    // A piece's dark edge: the glyph ringed by a round pen a cell pixel wide,
    // under the glyph (the console's digits have the same).
    std::vector<uint8_t> edge;
    const bool shadowed = job.word && job.word->shadowStrength > 0.0f;
    if (job.piece && !shadowed)
    {
        edge = canvas;
        ring(edge, width, height, float(r));
    }
    else if (job.version == Version::Border)
    {
        ring(canvas, width, height, float(r));
    }
    else if (job.version == Version::Shadow)
    {
        sweep(canvas, width, height, r);
    }
    // Averaged down to the scale, across then down, each host pixel the mean
    // of the render pixels it covers: the texels drawn, and the margin a
    // version has.
    const uint32_t margin = job.version == Version::Glyph ? 0u : 2u * dq8::gfx::kRegionBorderMargin;
    picture.width = (outW + margin) * job.scale;
    picture.height = (outH + margin) * job.scale;
    const int ow = int(picture.width), oh = int(picture.height);
    // The render pixels averaged, from (marginX, marginY).
    const int sw = int(outW) * r + 2 * pad, sh = int(outH) * r + 2 * pad;
    const std::vector<Taps> across = boxTaps(sw, ow), down = boxTaps(sh, oh);
    auto average = [&](const std::vector<uint8_t> &image) {
        std::vector<float> rows(size_t(ow) * sh), out(size_t(ow) * oh);
        for (int y = 0; y < sh; ++y)
        {
            const uint8_t *line = image.data() + size_t(y + marginY) * width + marginX;
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
                out[size_t(y) * ow + x] = sum;
            }
        }
        return out;
    };
    const std::vector<float> cover = average(canvas);
    picture.rgba.assign(size_t(ow) * oh * 4u, 0u);
    if (!job.piece)
    {
        for (size_t i = 0; i < cover.size(); ++i)
        {
            uint8_t *out = &picture.rgba[i * 4u];
            out[0] = job.colour[0];
            out[1] = job.colour[1];
            out[2] = job.colour[2];
            // Coverage 0-255 to the GS's alpha, 0x80 opaque.
            out[3] = static_cast<uint8_t>(std::min(128.0f, cover[i] * 128.0f / 255.0f + 0.5f));
        }
        return picture;
    }
    // A piece: the core in its row's colour over the edge (or over a
    // shadowed word's shadow: the core blurred, moved and faded).
    const std::vector<float> edgeCover = shadowed ? softShadow(cover, ow, oh, *job.word, job.scale) : average(edge);
    const float strength = shadowed ? 1.0f : job.colours.edgeAlpha / 128.0f;
    for (int y = 0; y < oh; ++y)
    {
        const float at = std::clamp((float(y) + 0.5f) / float(job.scale) - 0.5f, 0.0f, float(outH - 1u));
        const int row = std::min(int(at), int(outH) - 1), next = std::min(row + 1, int(outH) - 1);
        const float part = at - float(row);
        float core[3];
        for (int c = 0; c < 3; ++c)
        {
            core[c] = job.colours.rows[row][c] * (1.0f - part) + job.colours.rows[next][c] * part;
        }
        for (int x = 0; x < ow; ++x)
        {
            const size_t i = size_t(y) * ow + x;
            const float c = std::min(1.0f, cover[i] / 255.0f);
            const float e = std::min(1.0f, edgeCover[i] / 255.0f) * strength * (1.0f - c);
            const float a = c + e;
            if (a <= 0.0f)
            {
                continue;
            }
            uint8_t *out = &picture.rgba[i * 4u];
            for (int k = 0; k < 3; ++k)
            {
                out[k] = static_cast<uint8_t>(
                    std::min(255.0f, (core[k] * c + float(job.colours.edge[k]) * e) / a + 0.5f));
            }
            out[3] = static_cast<uint8_t>(std::min(128.0f, a * 128.0f + 0.5f));
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
        auto sheetJob = [&](bool bold, Version version) {
            Job job{0u, &font, g.glyph, g.ink, s, bold, version, {255u, 255u, 255u}};
            job.shapeStep = shapeStepFor(font, g.glyph);
            return job;
        };
        Picture plain;
        for (int bold = 0; bold < 2; ++bold)
        {
            Picture picture = draw(sheetJob(bold != 0, Version::Glyph));
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
        const Picture border = draw(sheetJob(false, Version::Border));
        const Picture shadow = draw(sheetJob(false, Version::Shadow));
        const uint32_t bx = x0 + 3u * (cw + kGap) + kGap;
        layered(bx, {{&border, 0, 0.0f}});
        layered(bx + vw + kGap, {{&shadow, int(s), 128.0f}, {&shadow, 2 * int(s), 0.0f}});
    }
    const std::string path = g_sheetDirectory + "/typeface_" + font.name + "_" + std::to_string(s) + "x.png";
    const bool written = stbi_write_png(path.c_str(), int(width), int(height), 3, image.data(), int(width * 3u)) != 0;
    // Beside it, which glyph each entry is (row by row, kColumns a row), for
    // tools that read the sheet: the character's hex code or c and its code.
    // Only with the sheet written: a list beside an older sheet would misname it.
    std::FILE *list = written ? std::fopen((path.substr(0, path.size() - 4u) + ".txt").c_str(), "w") : nullptr;
    if (list)
    {
        std::fprintf(list, "# entry %ux%u, columns %u, gap %u, margin %u, scale %u\n", entryWidth, entryHeight,
                     kColumns, kGap, m, s);
        for (size_t n = 0; n < glyphs.size(); ++n)
        {
            std::fprintf(list, "%zu %s\n", n, glyphLabel(font.glyphs[glyphs[n].glyph], true).c_str());
        }
        std::fclose(list);
    }
    const long long ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
    std::fprintf(stderr, "[typeface] sheet of %s at %ux: %s%s (%zu glyphs, four pictures each, in %lld ms)\n", font.name,
                 s, path.c_str(), written ? "" : " (not written)", glyphs.size(), ms);
}

// RG_TYPEFACE_SHEET, for the menus' pieces (they are known as they are
// drawn, not from pages read): each piece as it is first made, the
// console's enlarged beside the picture made, over the menus' blue, into
// DIR/piece_<set>_<character code>_<size>_<scale>x.png. From the game's
// pictures: it stays on the machine.
void writePieceSheet(const Job &job, const Picture &picture)
{
    const uint32_t w = job.regionWidth, h = job.regionHeight, s = job.scale;
    const std::vector<uint8_t> &console = job.sheetConsole.empty() ? job.console : job.sheetConsole;
    if (picture.rgba.empty() || console.size() != size_t(w) * h * 4u || picture.width != w * s ||
        picture.height != h * s)
    {
        return;
    }
    constexpr uint32_t kGap = 4u;
    const uint32_t width = 2u * w * s + 3u * kGap, height = h * s + 2u * kGap;
    std::vector<uint8_t> image(size_t(width) * height * 3u);
    for (size_t i = 0; i < image.size(); i += 3u)
    {
        image[i] = 63u, image[i + 1u] = 104u, image[i + 2u] = 160u;
    }
    auto put = [&](uint32_t x0, auto texel) {
        for (uint32_t y = 0; y < h * s; ++y)
        {
            for (uint32_t x = 0; x < w * s; ++x)
            {
                const uint8_t *t = texel(x, y);
                const float a = std::min(1.0f, t[3] / 128.0f);
                uint8_t *p = &image[(size_t(kGap + y) * width + x0 + x) * 3u];
                for (int c = 0; c < 3; ++c)
                {
                    p[c] = static_cast<uint8_t>(p[c] + (float(t[c]) - p[c]) * a);
                }
            }
        }
    };
    put(kGap, [&](uint32_t x, uint32_t y) { return &console[(size_t(y / s) * w + x / s) * 4u]; });
    put(2u * kGap + w * s, [&](uint32_t x, uint32_t y) { return &picture.rgba[(size_t(y) * picture.width + x) * 4u]; });
    char name[128];
    if (job.word)
    {
        std::string text = job.font->name;
        std::replace_if(text.begin(), text.end(), [](char c) { return !std::isalnum(static_cast<unsigned char>(c)); },
                        '-');
        // Named by its place in its picture too: a picture may hold a word
        // twice (main_menu_top's AP and Lv).
        std::snprintf(name, sizeof(name), "/piece_word_%s_%u-%u_%ux%u_%ux.png", text.c_str(), unsigned(job.word->x),
                      unsigned(job.word->y), w, h, s);
    }
    else
    {
        // With its key's low bits: each European language's copy of a digit
        // is the same set, character and size.
        std::snprintf(name, sizeof(name), "/piece_%s_%s_%ux%u_%ux_%08X.png", job.font->name,
                      glyphLabel(job.font->glyphs[job.glyph], true).c_str(), w, h, s, uint32_t(job.key));
    }
    // One at a time: two drawers can make the same word with other art beside
    // it (two keys, one file).
    static std::mutex writing;
    std::lock_guard<std::mutex> lock(writing);
    stbi_write_png((g_sheetDirectory + name).c_str(), int(width), int(height), 3, image.data(), int(width * 3u));
}

void drawer()
{
    for (;;)
    {
        Job job;
        {
            std::unique_lock<std::mutex> lock(g_jobsMutex);
            g_wake.wait(lock, [] { return !g_queue.empty() || !g_ahead.empty(); });
            std::deque<Job> &from = g_queue.empty() ? g_ahead : g_queue;
            job = std::move(from.front());
            from.pop_front();
            if (&from == &g_ahead)
            {
                g_aheadKeys.erase(job.key);
            }
        }
        const auto started = g_traceTiming ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        Picture picture = draw(job);
        if (job.keep && job.word && !picture.rgba.empty() &&
            job.console.size() == size_t(job.regionWidth) * job.regionHeight * 4u)
        {
            // The piece's other art, enlarged as the game shows it (each
            // texel a block), under the word: the word's box cleared of the
            // game's word (on a button, its fill put back there).
            const uint32_t s = job.scale;
            for (uint32_t y = 0; y < picture.height; ++y)
            {
                for (uint32_t x = 0; x < picture.width; ++x)
                {
                    const uint32_t tx = x / s, ty = y / s;
                    if (!job.word->backing && tx >= job.offsetX && tx < uint32_t(job.offsetX) + job.word->width &&
                        ty >= job.offsetY && ty < uint32_t(job.offsetY) + job.word->height)
                    {
                        continue;
                    }
                    const uint8_t *under = &job.console[(size_t(ty) * job.regionWidth + tx) * 4u];
                    uint8_t *over = &picture.rgba[(size_t(y) * picture.width + x) * 4u];
                    const float a = over[3] / 128.0f, b = under[3] / 128.0f * (1.0f - a), sum = a + b;
                    if (sum <= 0.0f)
                    {
                        continue;
                    }
                    for (int c = 0; c < 3; ++c)
                    {
                        over[c] = static_cast<uint8_t>((over[c] * a + under[c] * b) / sum + 0.5f);
                    }
                    over[3] = static_cast<uint8_t>(std::min(128.0f, sum * 128.0f + 0.5f));
                }
            }
        }
        if (job.piece && !job.console.empty() && !g_sheetDirectory.empty())
        {
            writePieceSheet(job, picture);
        }
        if (g_trace)
        {
            const TypefaceFont &font = *job.font;
            // A symbol or an accent (character 0) by its code; a word's style
            // has no glyphs.
            std::string shown = " ";
            if (!job.word)
            {
                shown = glyphLabel(font.glyphs[job.glyph], false);
            }
            if (!job.piece && job.regionHeight)
            {
                // A font's glyph drawn short (the names over heads).
                shown += " " + std::to_string(job.regionWidth) + "x" + std::to_string(job.regionHeight);
            }
            std::fprintf(stderr, "[typeface] made %s '%s' at %ux%s%s%s\n", font.name, shown.c_str(), job.scale,
                         job.bold ? ", bold" : "",
                         job.version == Version::Border   ? ", border"
                         : job.version == Version::Shadow ? ", shadow"
                                                          : "",
                         picture.rgba.empty() ? " (empty)" : "");
        }
        if (!job.piece)
        {
            // A font's glyph is drawn in one colour: its coverage is kept.
            for (size_t i = 0; i * 4u < picture.rgba.size(); ++i)
            {
                picture.rgba[i] = picture.rgba[i * 4u + 3u];
            }
            picture.rgba.resize(picture.rgba.size() / 4u);
            picture.rgba.shrink_to_fit();
            picture.coverage = true;
        }
        std::lock_guard<std::mutex> lock(g_jobsMutex);
        if (job.generation != g_generation)
        {
            // The pictures were forgotten while it was drawn (the letters
            // turned off or on): what is asked again is made again.
            continue;
        }
        if (g_traceTiming)
        {
            // A glyph drawn ahead has no asking: its wait is not counted.
            const auto now = std::chrono::steady_clock::now();
            const bool asked = job.asked != std::chrono::steady_clock::time_point{};
            const double queued = asked ? millisecondsSince(job.asked, started) : 0.0;
            const double drawing = millisecondsSince(started, now);
            ++g_timing.drawn;
            g_timing.ahead += asked ? 0u : 1u;
            g_timing.queuedMs += queued;
            g_timing.queuedMaxMs = std::max(g_timing.queuedMaxMs, queued);
            g_timing.drawMs += drawing;
            g_timing.drawMaxMs = std::max(g_timing.drawMaxMs, drawing);
            reportTiming(now);
        }
        g_asked.erase(job.key);
        keepWithinBudget(picture.rgba.size());
        picture.used = ++g_readyClock;
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
    if (g_learning.load(std::memory_order_relaxed) || piecePalette(unaliased(clut)))
    {
        return true;
    }
    std::lock_guard<std::mutex> lock(g_cellsMutex);
    return std::find(g_palettes.begin(), g_palettes.end(), clut) != g_palettes.end();
}

// The picture made for a job, once a drawer has it (a font's glyph in the
// job's colour), or the job queued once (one drawn ahead and not yet made
// moved to the front).
int answer(const Job &job, uint32_t &width, uint32_t &height, std::vector<uint8_t> &rgba)
{
    std::lock_guard<std::mutex> lock(g_jobsMutex);
    if (!g_enabled.load(std::memory_order_relaxed))
    {
        // Turned off since the renderer asked (after forgetPictures): as
        // lookup answers then, nothing queued.
        return dq8::gfx::kRegionNone;
    }
    if (job.shapeStep != 0 && job.shapeStep != g_shapeStep.load(std::memory_order_relaxed))
    {
        // A "+" for the screen's shape before (setPixelShape changed it since
        // lookup read it): the renderer asks again at its next draw, for
        // the shape now. Never handed over, so no copy of it is left drawn.
        return dq8::gfx::kRegionPending;
    }
    const auto now = g_traceTiming ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    if (const auto ready = g_ready.find(job.key); ready != g_ready.end())
    {
        Picture &picture = ready->second;
        picture.used = ++g_readyClock;
        if (!picture.handed)
        {
            picture.handed = true;
            g_notAhead.erase(job.key);
        }
        if (g_traceTiming)
        {
            // Every first handover of a glyph on the screen: at once (drawn
            // ahead, or the same shape in another colour), or after the wait
            // since its first asking. One whose text went before it was made
            // (not asked for the last quarter second) is not counted.
            double handed = 0.0;
            bool shown = true;
            if (const auto waited = g_waiting.find(job.key); waited != g_waiting.end())
            {
                handed = millisecondsSince(waited->second.first, now);
                shown = millisecondsSince(waited->second.last, now) < 250.0;
                g_waiting.erase(waited);
            }
            if (shown)
            {
                ++g_timing.handed;
                g_timing.handedMs += handed;
                g_timing.handedMaxMs = std::max(g_timing.handedMaxMs, handed);
            }
            reportTiming(now);
        }
        if (picture.rgba.empty())
        {
            return dq8::gfx::kRegionNone;
        }
        if (job.shapeStep != 0)
        {
            g_shapesHanded.insert(job.shapeStep);
        }
        width = picture.width;
        height = picture.height;
        if (picture.coverage)
        {
            rgba.resize(picture.rgba.size() * 4u);
            for (size_t i = 0; i < picture.rgba.size(); ++i)
            {
                uint8_t *out = &rgba[i * 4u];
                out[0] = job.colour[0];
                out[1] = job.colour[1];
                out[2] = job.colour[2];
                out[3] = picture.rgba[i];
            }
        }
        else
        {
            rgba = picture.rgba;
        }
        return dq8::gfx::kRegionReady;
    }
    if (g_traceTiming)
    {
        g_waiting.try_emplace(job.key, Waiting{now, now}).first->second.last = now;
    }
    if (g_asked.insert(job.key).second)
    {
        g_queue.push_back(job);
        g_queue.back().asked = now;
        g_queue.back().generation = g_generation;
        g_timing.queueMax = std::max(g_timing.queueMax, g_queue.size() + g_ahead.size());
        g_wake.notify_one();
    }
    else if (g_aheadKeys.erase(job.key) != 0u)
    {
        const auto ahead = std::find_if(g_ahead.begin(), g_ahead.end(), [&](const Job &queued) { return queued.key == job.key; });
        if (ahead != g_ahead.end())
        {
            g_queue.push_back(std::move(*ahead));
            g_queue.back().asked = now;
            g_ahead.erase(ahead);
        }
    }
    return dq8::gfx::kRegionPending;
}

// A font glyph's job: its picture known by the cell's alpha (its hash), the
// scale, the version, the bold draw and for the "+" the screen's shape, not
// by its colour (given at the handover).
Job glyphJob(uint64_t alpha, const Cell &cell, uint32_t scale, Version version, bool bold, const uint8_t *colour)
{
    const int shape = shapeStepFor(*kFonts[cell.font], cell.glyph);
    uint64_t key = alpha;
    key ^= uint64_t(scale) << 3u | uint64_t(version) << 1u | (bold ? 1u : 0u) | uint64_t(shape) << 40u;
    key *= 0x100000001b3ull;
    Job job{key, kFonts[cell.font], cell.glyph, cell.ink, scale, bold, version, {colour[0], colour[1], colour[2]}};
    job.shapeStep = shape;
    if (cell.rows)
    {
        // Drawn a row short, as the draw takes it.
        job.regionWidth = static_cast<uint8_t>(kFonts[cell.font]->cellWidth);
        job.regionHeight = cell.rows;
    }
    return job;
}

// The first time a glyph of a font is asked in a style (its scale, version,
// bold draw, the cell a row short or whole), the font's other glyphs in that
// style are queued behind the renderer's jobs: the rest of the screen's text,
// and the dialogue's next letters, are made by the time they show.
void drawAhead(const Cell &asked, uint32_t scale, Version version, bool bold)
{
    const uint64_t style = uint64_t(asked.font) | uint64_t(scale) << 8u | uint64_t(version) << 16u |
                           uint64_t(bold ? 1u : 0u) << 24u | uint64_t(asked.rows) << 32u;
    if (!g_drawAhead)
    {
        return;
    }
    uint64_t generation = 0u;
    {
        std::lock_guard<std::mutex> lock(g_jobsMutex);
        if (!g_aheadStyles.insert(style).second)
        {
            return;
        }
        generation = g_generation;
    }
    std::vector<Job> jobs;
    {
        std::lock_guard<std::mutex> lock(g_cellsMutex);
        const uint8_t white[3] = {0x80u, 0x80u, 0x80u};
        for (const auto &[alpha, cell] : g_cells)
        {
            if (cell.font == asked.font && cell.glyph >= 0 && cell.rows == asked.rows)
            {
                jobs.push_back(glyphJob(alpha, cell, scale, version, bold, white));
            }
        }
    }
    std::lock_guard<std::mutex> lock(g_jobsMutex);
    if (generation != g_generation || !g_enabled.load(std::memory_order_relaxed))
    {
        // The pictures were forgotten meanwhile (the letters turned off).
        return;
    }
    // In front of the glyphs still waiting to be drawn ahead, which may be of
    // another scale (the internal resolution changed; an effect's target is
    // drawn at a scale of its own) and would only delay these.
    std::vector<Job> queued;
    queued.reserve(jobs.size());
    for (Job &job : jobs)
    {
        if (g_ready.count(job.key) == 0u && g_notAhead.count(job.key) == 0u && g_asked.insert(job.key).second)
        {
            g_aheadKeys.insert(job.key);
            job.generation = generation;
            queued.push_back(std::move(job));
        }
    }
    g_ahead.insert(g_ahead.begin(), std::make_move_iterator(queued.begin()), std::make_move_iterator(queued.end()));
    g_wake.notify_all();
}

// A piece's colours from its texels (the GS's alpha): the edge's, the mean
// of the texels with ink next to a clear one (a shadowed word's shadow's,
// of its dark texels, black without any); the core's per row, the mean of
// the opaque texels at least two thirds of the way from the edge's
// lightness to the row's lightest. Rows without core (and a bevel's darker
// first or last row) take the nearest row's. False when it has no edge or no
// core.
bool pieceColours(const uint8_t *rgba, uint32_t width, uint32_t height, PieceColours &out, bool shadowed = false)
{
    auto at = [&](uint32_t x, uint32_t y) { return rgba + (size_t(y) * width + x) * 4u; };
    auto light = [](const uint8_t *t) { return (3.0f * t[0] + 6.0f * t[1] + float(t[2])) / 10.0f; };
    float edge[3] = {};
    uint32_t edges = 0u;
    uint8_t edgeAlpha = 0u;
    float top = 0.0f;
    for (uint32_t i = 0; i < width * height && shadowed; ++i)
    {
        if (rgba[i * 4u + 3u] != 0u)
        {
            top = std::max(top, light(rgba + i * 4u));
        }
    }
    for (uint32_t y = 0; y < height; ++y)
    {
        for (uint32_t x = 0; x < width; ++x)
        {
            const uint8_t *t = at(x, y);
            // A shadowed word's shadow: its dark texels, however clear. An
            // edged one's edge: the texels with ink next to a clear one.
            const bool counted =
                shadowed ? t[3] != 0u && light(t) < 0.35f * top
                         : t[3] >= 0x40u && (x == 0 || y == 0 || x + 1 == width || y + 1 == height ||
                                             at(x - 1, y)[3] < 0x20u || at(x + 1, y)[3] < 0x20u ||
                                             at(x, y - 1)[3] < 0x20u || at(x, y + 1)[3] < 0x20u);
            if (counted)
            {
                for (int c = 0; c < 3; ++c)
                {
                    edge[c] += t[c];
                }
                ++edges;
                edgeAlpha = std::max(edgeAlpha, t[3]);
            }
        }
    }
    if (edges == 0u && !shadowed)
    {
        return false;
    }
    edges = std::max(edges, 1u);
    for (int c = 0; c < 3; ++c)
    {
        out.edge[c] = static_cast<uint8_t>(edge[c] / float(edges) + 0.5f);
    }
    out.edgeAlpha = std::min<uint8_t>(edgeAlpha, 0x80u);
    const float dark = light(out.edge);
    const uint32_t rows = std::min(height, kPieceRows);
    float rise[kPieceRows] = {};
    int first = -1, last = -1;
    for (uint32_t y = 0; y < rows; ++y)
    {
        for (uint32_t x = 0; x < width; ++x)
        {
            if (at(x, y)[3] >= 0x70u)
            {
                rise[y] = std::max(rise[y], light(at(x, y)) - dark);
            }
        }
        if (rise[y] >= 24.0f)
        {
            first = first < 0 ? int(y) : first;
            last = int(y);
        }
    }
    // A bevel: the core's first or last row much darker than the next row
    // in (the menus' words' tops and bottoms). The typeface's letters, taller
    // or lower than the console's, would carry it over more rows (a shade
    // on Allies' l): those rows take the colour inside. A gradient (Press,
    // Story) darkens row by row and stays.
    auto inner = [&](int y, int step) {
        for (int i = y + step; i >= first && i <= last; i += step)
        {
            if (rise[i] >= 24.0f)
            {
                return i;
            }
        }
        return -1;
    };
    if (const int next = first >= 0 ? inner(first, 1) : -1; next >= 0 && rise[first] < 0.6f * rise[next])
    {
        rise[first] = 0.0f;
    }
    if (const int next = last >= 0 ? inner(last, -1) : -1; next >= 0 && rise[last] < 0.6f * rise[next])
    {
        rise[last] = 0.0f;
    }
    bool has[kPieceRows] = {};
    bool any = false;
    for (uint32_t y = 0; y < rows; ++y)
    {
        if (rise[y] < 24.0f)
        {
            continue;
        }
        const float lightest = dark + rise[y];
        float sum[3] = {};
        uint32_t count = 0u;
        for (uint32_t x = 0; x < width; ++x)
        {
            const uint8_t *t = at(x, y);
            if (t[3] >= 0x70u && light(t) >= dark + (lightest - dark) * 0.65f)
            {
                for (int c = 0; c < 3; ++c)
                {
                    sum[c] += t[c];
                }
                ++count;
            }
        }
        for (int c = 0; c < 3; ++c)
        {
            out.rows[y][c] = static_cast<uint8_t>(sum[c] / float(count) + 0.5f);
        }
        has[y] = any = true;
    }
    if (!any)
    {
        return false;
    }
    for (uint32_t y = 0; y < rows; ++y)
    {
        if (has[y])
        {
            continue;
        }
        int nearest = -1;
        for (uint32_t d = 1; d < rows && nearest < 0; ++d)
        {
            if (y >= d && has[y - d])
            {
                nearest = int(y - d);
            }
            else if (y + d < rows && has[y + d])
            {
                nearest = int(y + d);
            }
        }
        std::memcpy(out.rows[y], out.rows[nearest], 3u);
    }
    return true;
}

// A word of the menus' pictures in the piece drawn: its box inside the
// piece and its texels the table's. Redrawn when the piece holds the word on
// a clear background, other art two texels or more away kept; a word the
// table marks as printed on a button (the framed Player) with the button's
// fill put back under it; any other word printed on art stays the game's.
// kNoWord when no word of the table is there.
constexpr int kNoWord = -1;
int lookupWord(const dq8::gfx::RegionRequest &request, uint64_t clut, uint32_t &width, uint32_t &height,
               std::vector<uint8_t> &rgba)
{
    if (request.width > 255u)
    {
        return kNoWord;
    }
    // A word is redrawn in a piece up to kWordRows tall, but on art: the
    // Revelation Flow's title is drawn with its frame's wood, 46 rows.
    for (const PieceWord &word : kWords)
    {
        if (!word.style || word.palette != clut || word.x < request.x || word.y < request.y ||
            word.x + word.width > request.x + request.width || word.y + word.height > request.y + request.height ||
            (request.height > kWordRows && word.backing != kOnArt))
        {
            continue;
        }
        const uint32_t ox = word.x - request.x, oy = word.y - request.y;
        std::vector<uint8_t> box(size_t(word.width) * word.height * 4u);
        for (uint32_t y = 0; y < word.height; ++y)
        {
            std::memcpy(&box[size_t(y) * word.width * 4u], request.full + (size_t(oy + y) * request.width + ox) * 4u,
                        size_t(word.width) * 4u);
        }
        if (hashRgba(box.data(), uint32_t(word.width) * word.height) != word.hash)
        {
            continue;
        }
        // The piece holds the word alone (clear around its box), or other art
        // too, kWordRoom texels or more from the box (the story screen's
        // "Press" and its button): that art is kept as the game draws it.
        constexpr uint32_t kWordRoom = 2u;
        bool alone = true, roomy = true;
        for (uint32_t y = 0; y < request.height && roomy; ++y)
        {
            for (uint32_t x = 0; x < request.width && roomy; ++x)
            {
                const bool inside = x >= ox && x < ox + word.width && y >= oy && y < oy + word.height;
                const bool near = x + kWordRoom >= ox && x < ox + word.width + kWordRoom && y + kWordRoom >= oy &&
                                  y < oy + word.height + kWordRoom;
                if (!inside && request.full[(size_t(y) * request.width + x) * 4u + 3u] != 0u)
                {
                    alone = false;
                    roomy = !near;
                }
            }
        }
        // A word on a button: the button around it is kept, its fill put
        // back under the word from the texels left and right of the box. On
        // a band, the band put back from its own texels in the box; on art,
        // the art from beside each run of the word's texels.
        const bool onButton = word.backing == kOnButton && ox >= 1u && ox + word.width < request.width;
        const bool backed = onButton || word.backing == kOnBar || word.backing == kOnArt;
        if (word.backing == kOnButton && !onButton)
        {
            // No fill beside the box to put back under it: left the game's
            // (redrawn plainly, the fill would be read as its colours and the
            // button left with a hole).
            return dq8::gfx::kRegionNone;
        }
        if (backed)
        {
            alone = false;
            roomy = true;
        }
        // On a button, a band or art the core meets the fill here and there,
        // and the texels next to the cleared fill are not all edge: the
        // edge's colour is its dark texels' (those of the word's own).
        float sum[3] = {};
        uint32_t count = 0u;
        for (uint32_t y = 0; y < word.height && backed; ++y)
        {
            const uint8_t *row = &box[size_t(y) * word.width * 4u];
            for (uint32_t x = 0; x < word.width; ++x)
            {
                const uint8_t *t = row + size_t(x) * 4u;
                if (backingKind(word.backing, t) == ButtonTexel::Edge && t[3] >= 0x40u &&
                    !backingAt(word.backing, row, word.width, x))
                {
                    for (int c = 0; c < 3; ++c)
                    {
                        sum[c] += t[c];
                    }
                    ++count;
                }
            }
        }
        uint8_t edge[4] = {0u, 0u, 0u, 0x7Fu};
        for (int c = 0; c < 3 && count; ++c)
        {
            edge[c] = static_cast<uint8_t>(sum[c] / float(count) + 0.5f);
        }
        // The colours from the word alone, its rows the piece's: on a button
        // or art, the fill's texels left out; on a band, they take the edge's
        // colour, so the core is read against its own edge as the table's fit
        // read it (pieces.py load_words).
        std::vector<uint8_t> only(size_t(request.width) * request.height * 4u, 0u);
        for (uint32_t y = 0; y < word.height; ++y)
        {
            std::memcpy(&only[(size_t(oy + y) * request.width + ox) * 4u], &box[size_t(y) * word.width * 4u],
                        size_t(word.width) * 4u);
            for (uint32_t x = 0; x < word.width && backed; ++x)
            {
                if (backingAt(word.backing, &box[size_t(y) * word.width * 4u], word.width, x))
                {
                    uint8_t *t = &only[(size_t(oy + y) * request.width + ox + x) * 4u];
                    if (word.backing == kOnBar && count)
                    {
                        std::memcpy(t, edge, 4u);
                    }
                    else
                    {
                        std::memset(t, 0, 4u);
                    }
                }
            }
        }
        PieceColours colours;
        const bool coloured = roomy && pieceColours(only.data(), request.width, request.height, colours,
                                                    word.shadowStrength > 0.0f);
        if (coloured && backed && count)
        {
            std::memcpy(colours.edge, edge, 3u);
        }
        if (g_trace)
        {
            static std::mutex loggedMutex;
            static std::unordered_set<uint64_t> logged;
            std::lock_guard<std::mutex> lock(loggedMutex);
            if (logged.insert(word.hash ^ (uint64_t(request.width) << 48u | uint64_t(request.height) << 40u |
                                           (request.bold ? 1u : 0u)))
                    .second)
            {
                std::fprintf(stderr, "[typeface] word '%s' (%ux%u at %u,%u%s)%s\n", word.style->name, request.width,
                             request.height, request.x, request.y, request.bold ? ", bold" : "",
                             !roomy     ? ": art next to it in its piece, left as it is"
                             : !coloured ? ": no colours read"
                             : !alone    ? ", the rest of its piece kept"
                                         : "");
            }
        }
        if (!coloured)
        {
            return dq8::gfx::kRegionNone;
        }
        // The picture is the word's, and with other art in the piece that
        // art's too (the button beside "Press" changes with the controller).
        uint64_t key = word.hash;
        for (const uint64_t value :
             {uint64_t(request.scale) << 3u | 0x200u,
              uint64_t(ox) << 48u | uint64_t(oy) << 32u | uint64_t(request.width) << 16u | request.height,
              alone ? 0u : hashRgba(request.full, request.width * request.height)})
        {
            key ^= value;
            key *= 0x100000001b3ull;
        }
        // The console word's ink in the piece.
        const Ink ink{static_cast<uint8_t>(word.inkLeft + ox), static_cast<uint8_t>(word.inkRight + ox),
                      static_cast<uint8_t>(word.inkBottom + oy)};
        Job job{key, word.style, 0, ink, request.scale, false, Version::Glyph, {0u, 0u, 0u}};
        job.piece = true;
        job.word = &word;
        job.regionWidth = static_cast<uint8_t>(request.width);
        job.regionHeight = static_cast<uint8_t>(request.height);
        job.offsetX = static_cast<uint8_t>(ox);
        job.offsetY = static_cast<uint8_t>(oy);
        job.colours = colours;
        job.keep = !alone;
        if (job.keep || !g_sheetDirectory.empty())
        {
            job.console.assign(request.full, request.full + size_t(request.width) * request.height * 4u);
        }
        if (backed && !g_sheetDirectory.empty())
        {
            // The sheet shows the game's piece, not the backing put back.
            job.sheetConsole = job.console;
        }
        if (onButton)
        {
            // The kept art's box: each row the fill from the nearest fill
            // texel left of the box to the nearest right of it (the fill
            // shades across; a texel of the word's edge just outside the box
            // is passed over), within three texels, or the one next to it.
            auto fillAt = [&](uint32_t y, uint32_t from, int step) {
                for (int n = 0; n < 3; ++n)
                {
                    const int x = int(from) + n * step;
                    if (x < 0 || x >= int(request.width))
                    {
                        break;
                    }
                    const uint8_t *t = &request.full[(size_t(y) * request.width + uint32_t(x)) * 4u];
                    if (buttonTexel(t) == ButtonTexel::Fill)
                    {
                        return t;
                    }
                }
                return &request.full[(size_t(y) * request.width + from) * 4u];
            };
            for (uint32_t y = oy; y < oy + word.height; ++y)
            {
                const uint8_t *left = fillAt(y, ox - 1u, -1);
                const uint8_t *right = fillAt(y, ox + word.width, 1);
                for (uint32_t x = 0; x < word.width; ++x)
                {
                    const float t = float(x + 1u) / float(word.width + 1u);
                    uint8_t *to = &job.console[(size_t(y) * request.width + ox + x) * 4u];
                    for (int c = 0; c < 4; ++c)
                    {
                        to[c] = static_cast<uint8_t>(float(left[c]) + (float(right[c]) - float(left[c])) * t + 0.5f);
                    }
                }
            }
        }
        else if (word.backing == kOnArt)
        {
            // What is taken away: the word's texels, and its shadow, the dark
            // texels (lightness below 40, darker than the wood ever is away
            // from the letters) within two texels of them, in the box's rows
            // and the row below it (the shadow's alone there). Each run of it
            // in a row is filled from the texel left of it to the one right of
            // it (the wood's grain runs along the rows), or from the one side
            // that has art in the piece.
            const uint32_t rows = std::min(request.height, oy + word.height + 1u);
            const auto lightAt = [&](uint32_t x, uint32_t y) {
                const uint8_t *t = &request.full[(size_t(y) * request.width + x) * 4u];
                return (3.0f * t[0] + 6.0f * t[1] + float(t[2])) / 10.0f;
            };
            const auto wordAt = [&](uint32_t x, uint32_t y) {
                return artWord(&request.full[(size_t(y) * request.width + x) * 4u]);
            };
            std::vector<uint8_t> taken(size_t(request.width) * rows, 0u);
            for (uint32_t y = oy; y < rows; ++y)
            {
                for (uint32_t x = 0; x < request.width; ++x)
                {
                    const bool inBox = y < oy + word.height && x >= ox && x < ox + word.width;
                    if (wordAt(x, y))
                    {
                        taken[size_t(y) * request.width + x] = inBox ? 1u : 0u;
                        continue;
                    }
                    if (lightAt(x, y) >= 40.0f)
                    {
                        continue;
                    }
                    bool near = false;
                    for (int dy = -2; dy <= 2 && !near; ++dy)
                    {
                        for (int dx = -2; dx <= 2 && !near; ++dx)
                        {
                            const int nx = int(x) + dx, ny = int(y) + dy;
                            near = nx >= int(ox) && nx < int(ox + word.width) && ny >= int(oy) &&
                                   ny < int(oy + word.height) && wordAt(uint32_t(nx), uint32_t(ny));
                        }
                    }
                    taken[size_t(y) * request.width + x] = near ? 2u : 0u;
                }
            }
            for (uint32_t y = oy; y < rows; ++y)
            {
                const uint8_t *row = &request.full[size_t(y) * request.width * 4u];
                const uint8_t *flags = &taken[size_t(y) * request.width];
                for (uint32_t x = 0; x < request.width;)
                {
                    if (!flags[x])
                    {
                        ++x;
                        continue;
                    }
                    uint32_t end = x;
                    while (end < request.width && flags[end])
                    {
                        ++end;
                    }
                    // Not from another word's texels (the frame's outline):
                    // with no art either side, left as the game draws it.
                    const uint8_t *left = x > 0u && !wordAt(x - 1u, y) ? row + size_t(x - 1u) * 4u : nullptr;
                    const uint8_t *right = end < request.width && !wordAt(end, y) ? row + size_t(end) * 4u : nullptr;
                    for (uint32_t at = x; at < end && (left || right); ++at)
                    {
                        uint8_t *to = &job.console[(size_t(y) * request.width + at) * 4u];
                        const uint8_t *from = left ? left : right, *towards = right ? right : left;
                        const float t = float(at + 1u - x) / float(end + 1u - x);
                        for (int c = 0; c < 4; ++c)
                        {
                            to[c] = static_cast<uint8_t>(float(from[c]) + (float(towards[c]) - float(from[c])) * t +
                                                         0.5f);
                        }
                    }
                    x = end;
                }
            }
        }
        else if (word.backing == kOnBar)
        {
            // The kept art's box: each of the band's rows its colour from one
            // end of the box to the other, the rest clear (the word's edge
            // above and below the band).
            for (uint32_t y = 0; y < word.height; ++y)
            {
                const uint8_t *row = &box[size_t(y) * word.width * 4u];
                const bool band = bandRow(row, word.width);
                const uint8_t *left = row, *right = row + size_t(word.width - 1u) * 4u;
                for (uint32_t x = 0; x < word.width; ++x)
                {
                    uint8_t *to = &job.console[(size_t(oy + y) * request.width + ox + x) * 4u];
                    if (!band)
                    {
                        std::memset(to, 0, 4u);
                        continue;
                    }
                    const float t = word.width > 1u ? float(x) / float(word.width - 1u) : 0.0f;
                    for (int c = 0; c < 4; ++c)
                    {
                        to[c] = static_cast<uint8_t>(float(left[c]) + (float(right[c]) - float(left[c])) * t + 0.5f);
                    }
                }
            }
        }
        return answer(job, width, height, rgba);
    }
    return kNoWord;
}

void traceUnknown(const dq8::gfx::RegionRequest &request);

// A region drawn with a palette of the menus' digit pictures or font: a
// digit or letter the table knows, redrawn in its own colours, or else a
// word of the table (lookupWord).
int lookupPiece(const dq8::gfx::RegionRequest &request, uint64_t clut, uint32_t &width, uint32_t &height,
                std::vector<uint8_t> &rgba)
{
    if (request.border || request.shadow || request.height > kPieceRows)
    {
        if (g_tracePalettes && request.height > kWordRows)
        {
            // Taller than a piece: logged too when looking for text the
            // letters miss (a word drawn with the art around it).
            traceUnknown(request);
        }
        return dq8::gfx::kRegionNone;
    }
    const uint64_t hash = hashRgba(request.full, request.width * request.height);
    const Piece *piece = std::lower_bound(std::begin(kPieces), std::end(kPieces), hash,
                                          [](const Piece &p, uint64_t value) { return p.hash < value; });
    const bool known = piece != std::end(kPieces) && piece->hash == hash && piece->width == request.width &&
                       piece->height == request.height;
    if (!known)
    {
        if (const int answered = lookupWord(request, clut, width, height, rgba); answered != kNoWord)
        {
            return answered;
        }
        if (g_tracePalettes && request.height > kWordRows)
        {
            // Taller than a word's piece and no word of the table's art.
            traceUnknown(request);
        }
    }
    PieceColours colours;
    const bool coloured = known && pieceColours(request.full, request.width, request.height, colours);
    if (g_trace)
    {
        static std::mutex loggedMutex;
        static std::unordered_set<uint64_t> logged;
        std::lock_guard<std::mutex> lock(loggedMutex);
        if (logged.insert(hash ^ (request.bold ? 1u : 0u)).second)
        {
            if (coloured)
            {
                const TypefaceFont &font = *kPieceFonts[piece->set];
                std::fprintf(stderr, "[typeface] %s piece '%s' (%ux%u at %u,%u%s)\n", font.name,
                             glyphLabel(font.glyphs[piece->glyph], false).c_str(), request.width, request.height,
                             request.x, request.y, request.bold ? ", bold" : "");
            }
            else
            {
                std::fprintf(stderr, "[typeface] piece %ux%u at %u,%u of palette %016llX: %s\n", request.width,
                             request.height, request.x, request.y, static_cast<unsigned long long>(request.clut),
                             known ? "no colours read" : "not one the table knows");
            }
        }
    }
    if (!coloured)
    {
        return dq8::gfx::kRegionNone;
    }
    // A menu font's "+" is drawn for the screen's shape too (setPixelShape).
    const int shape = shapeStepFor(*kPieceFonts[piece->set], piece->glyph);
    uint64_t key = hash;
    key ^= uint64_t(request.scale) << 3u | 0x100u | uint64_t(shape) << 40u;
    key *= 0x100000001b3ull;
    Job job{key, kPieceFonts[piece->set], piece->glyph, Ink{piece->inkLeft, piece->inkRight, piece->inkBottom},
            request.scale, false, Version::Glyph, {0u, 0u, 0u}};
    job.shapeStep = shape;
    job.piece = true;
    job.regionWidth = static_cast<uint8_t>(request.width);
    job.regionHeight = static_cast<uint8_t>(request.height);
    job.spriteTop = piece->top;
    job.colours = colours;
    if (!g_sheetDirectory.empty())
    {
        job.console.assign(request.full, request.full + size_t(request.width) * request.height * 4u);
    }
    return answer(job, width, height, rgba);
}

// With the palettes trace (for finding text the letters miss): a region of
// a text's height that is no glyph of the pages read, or of a piece picture
// taller than a piece, its palette and alpha levels logged, and with
// RG_TYPEFACE_SHEET its texels written as
// DIR/nocell_<palette>_<x>-<y>_<size>.png. The first 400.
void traceUnknown(const dq8::gfx::RegionRequest &request)
{
    static std::mutex loggedMutex;
    static std::unordered_set<uint64_t> logged;
    std::lock_guard<std::mutex> lock(loggedMutex);
    if (logged.size() >= 400u ||
        !logged.insert(request.clut ^ (uint64_t(request.width) << 56u | uint64_t(request.height) << 48u |
                                       uint64_t(request.x) << 16u | request.y))
             .second)
    {
        return;
    }
    const uint32_t texels = request.width * request.height;
    if (!g_sheetDirectory.empty())
    {
        char file[160];
        std::snprintf(file, sizeof(file), "/nocell_%016llX_%u-%u_%ux%u.png",
                      static_cast<unsigned long long>(request.clut), request.x, request.y, request.width,
                      request.height);
        std::vector<uint8_t> shown(request.full, request.full + size_t(texels) * 4u);
        for (uint32_t i = 0; i < texels; ++i)
        {
            shown[i * 4u + 3u] = static_cast<uint8_t>(std::min(255, shown[i * 4u + 3u] * 2));
        }
        stbi_write_png((g_sheetDirectory + file).c_str(), int(request.width), int(request.height), 4, shown.data(),
                       int(request.width * 4u));
    }
    bool seen[256] = {};
    std::string levels;
    for (uint32_t i = 0; i < texels; ++i)
    {
        const uint8_t a = request.full[i * 4u + 3u];
        if (!seen[a])
        {
            seen[a] = true;
            levels += " " + std::to_string(a);
        }
    }
    std::fprintf(stderr, "[typeface] %ux%u at %u,%u of palette %016llX: no glyph (alpha%s)\n", request.width,
                 request.height, request.x, request.y, static_cast<unsigned long long>(request.clut), levels.c_str());
}

int lookup(const dq8::gfx::RegionRequest &request, uint32_t &width, uint32_t &height, std::vector<uint8_t> &rgba)
{
    if (!g_enabled.load(std::memory_order_relaxed) || request.whole || !request.full || request.scale < 2u ||
        request.scale > 8u)
    {
        return dq8::gfx::kRegionNone;
    }
    if (const uint64_t clut = unaliased(request.clut); piecePalette(clut))
    {
        return lookupPiece(request, clut, width, height, rgba);
    }
    // The border's version for the first of a glyph's marked border copies,
    // the shadow's for a marked shadow copy (drawGlyphOutline).
    const Version version = request.border   ? Version::Border
                            : request.shadow ? Version::Shadow
                                             : Version::Glyph;
    // A font's cell, or its cell a row short (the names over heads).
    bool sized = false;
    for (const TypefaceFont *font : kFonts)
    {
        sized = sized || (request.width == font->cellWidth &&
                          (request.height == font->cellHeight || request.height + 1u == font->cellHeight));
    }
    if (!sized)
    {
        if (g_tracePalettes && request.height >= 8u && request.height <= 32u)
        {
            traceUnknown(request);
        }
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
            (found->second.rows ? found->second.rows : kFonts[found->second.font]->cellHeight) != request.height)
        {
            if (g_tracePalettes && found == g_cells.end())
            {
                traceUnknown(request);
            }
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
    // The glyph asked for queued first, ahead of its font's others.
    const Job job = glyphJob(alpha, cell, request.scale, version, request.bold, colour);
    const int answered = answer(job, width, height, rgba);
    drawAhead(cell, request.scale, version, request.bold);
    return answered;
}

} // namespace

void install(PS2Runtime &runtime)
{
    if (const char *trace = std::getenv("RG_TYPEFACE_TRACE"); trace && *trace && *trace != '0')
    {
        // "timing" alone: the timing lines, not the glyphs.
        g_traceTiming = std::strcmp(trace, "timing") == 0;
        g_trace = !g_traceTiming;
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
    if (const char *ahead = std::getenv("RG_TYPEFACE_AHEAD"); ahead && *ahead)
    {
        g_drawAhead = *ahead != '0';
    }
    if (const char *gap = std::getenv("RG_TYPEFACE_GAP"); gap && *gap)
    {
        g_gapOpening = std::clamp(int(std::strtol(gap, nullptr, 10)), 0, 40);
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
    // A quarter of the processors, two to four: the game's own threads keep
    // theirs (four here on 16, two on a Steam Deck's 8).
    // RG_TYPEFACE_DRAWERS=N, for the timing trace's comparisons. (Read
    // here: the renderer is set up before install().)
    unsigned drawers = std::clamp(std::thread::hardware_concurrency() / 4u, 2u, 4u);
    if (const char *count = std::getenv("RG_TYPEFACE_DRAWERS"); count && *count)
    {
        drawers = std::clamp(unsigned(std::strtoul(count, nullptr, 10)), 1u, 16u);
    }
    for (unsigned n = 0; n < drawers; ++n)
    {
        std::thread(drawer).detach();
    }
    rg::texturePack::setFirstSource(&lookup, &wantedPalette);
}

uint64_t paletteHash256(const uint8_t *clut)
{
    // GsTextureCache::hashClutWindow: the palette's format and storage (CT32,
    // CSM1: 0, 0), then each colour in index order, eight bytes each.
    uint64_t hash = 1469598103934665603ull;
    const auto step = [&](uint64_t value) {
        for (int byte = 0; byte < 8; ++byte)
        {
            hash ^= (value >> (byte * 8)) & 0xffull;
            hash *= 1099511628211ull;
        }
    };
    step(0u);
    step(0u);
    for (uint32_t index = 0; index < 256u; ++index)
    {
        // CSM1 keeps entries 8-15 and 16-23 of every 32 swapped.
        const uint32_t stored = (index & ~0x18u) | ((index & 0x08u) << 1u) | ((index & 0x10u) >> 1u);
        uint32_t colour = 0;
        std::memcpy(&colour, clut + stored * 4u, sizeof(colour));
        step(colour);
    }
    return hash;
}

void notePaletteAlias(uint64_t changed, uint64_t original)
{
    if (changed == original)
    {
        return;
    }
    std::lock_guard<std::mutex> lock(g_aliasesMutex);
    for (auto &alias : g_aliases)
    {
        if (alias.first == changed)
        {
            alias.second = original;
            return;
        }
    }
    g_aliases.emplace_back(changed, original);
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

bool enabled()
{
    return g_enabled.load(std::memory_order_relaxed);
}

void setPixelShape(double widthOverHeight)
{
    const int step =
        std::clamp(int(std::lround(widthOverHeight * kShapeSteps)), kShapeSteps / 2, kShapeSteps * 4);
    // Called by the window's thread alone (graphics.cpp's pump, a frame).
    static int pending = 0, frames = 0;
    if (step == g_shapeStep.load(std::memory_order_relaxed))
    {
        pending = 0;
        return;
    }
    frames = step == pending ? frames + 1 : 1;
    pending = step;
    if (frames < kShapeSettleFrames)
    {
        return;
    }
    g_shapeStep.store(step, std::memory_order_relaxed);
    pending = 0;
    // A "+" the renderer holds was drawn for another shape: its copies let
    // go, the "+" is asked again (the others come back from g_ready). Only
    // then: a change with no "+" handed since the last refresh refreshes
    // nothing. Going to the loading screen's 4:3 after a "+" was shown at
    // 16:9 refreshes, unless the load reading the font pages did first
    // (readPage, refreshPictures); coming back only if a "+" was drawn
    // meanwhile.
    bool stale = false;
    {
        std::lock_guard<std::mutex> lock(g_jobsMutex);
        for (const int handed : g_shapesHanded)
        {
            stale = stale || handed != step;
        }
        if (stale)
        {
            g_shapesHanded.clear();
        }
    }
    if (stale)
    {
        rg::texturePack::refresh();
    }
}

bool forced()
{
    return g_forced >= 0;
}

} // namespace rg::typeface
