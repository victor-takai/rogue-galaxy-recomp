#include "button_glyphs.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>

#include "button_art.h"
#include "ps2_runtime.h"
#include "return_hook.h"
#include "runtime/ps2_memory.h"
#include "scene_timing.h"

namespace rg::buttons
{
namespace
{

// GaijiLoadImage(archive) copies the picture to 0x4C3180 afresh (at boot
// and with every map's resources; the texture registered stays in use while
// a loading screen shows), and GaijiGetTex answers its texture for each
// special character drawn, which the game sends from that memory.
constexpr uint32_t kGaijiLoadImage = 0x002705A0u;
constexpr uint32_t kGaijiGetTex = 0x00270510u;
constexpr uint32_t kGaijiImage = 0x004C3180u;
constexpr uint32_t kGaijiImageSize = 0x10800u;
// gaiji2.img: an IM3 archive holding one TIM2 picture, its header at +0x60,
// 256x256 8-bit indexed (stored in the GS's PSMT8 order), its 256-colour
// CLUT (CSM1 order) after the pixels.
constexpr uint32_t kTim2 = 0x50u;
constexpr int kWidth = 256;
constexpr int kHeight = 256;
constexpr uint32_t kClutBytes = 256u * 4u;

// The buttons' corner of the picture: L2, L1, R1, R2 along the top, the
// face buttons below.
constexpr int kRegionWidth = 128;
constexpr int kRegionHeight = 42;
using art::Bitmap;
using art::Cell;
using art::kCells;
using art::kFaceSize;
using art::kPlateHeight;
using art::kPlateWidth;
using art::Rgb;
// The templates: the cross's circle and L1's plate, emptied.
constexpr Cell kCircle = kCells[0];
constexpr Cell kPlate = kCells[4];

// The loading screen's "Press X" (LoadingScreenFrame, 0x3435D0) is a
// picture of its own, Loading_01 of rc/loading2_?.img, which
// LoadingScreenLoadImages reads into its arena (0x4E8970, its size after)
// and registers with NameTableRegisterImArchive(table, archive, ...), the
// call returning to 0x344774, which sends its CLUT then: an IM3 archive, its
// count at +0x8 and 0x40-byte entries from +0x10, each a name of 0x20 bytes
// then its TIM2's offset at +0x24. Loading_01 is 128x64, 8-bit; the cross's
// circle, 20x20 at (81, 42), is redrawn before that call as the face buttons
// are, its centre a pixel nearer.
constexpr uint32_t kNameTableRegisterImArchive = 0x001594D0u;
constexpr uint32_t kLoadingArchiveReturn = 0x00344774u;
constexpr uint32_t kLoadingArena = 0x004E8970u;
constexpr char kLoadingPicture[] = "Loading_01";
constexpr int kLoadingWidth = 128;
constexpr int kLoadingHeight = 64;
// FNV-1a of the cross's 400 indices as loaded, row by row: drawn over only
// when it is that one.
constexpr uint64_t kLoadingCrossHash = 0x7B28227F944320B0ull;

PS2Runtime::RecompiledFunction g_loadOriginal = nullptr;
PS2Runtime::RecompiledFunction g_getTexOriginal = nullptr;
PS2Runtime::RecompiledFunction g_registerArchiveOriginal = nullptr;
const char *g_dumpPath = nullptr;

// Packed Glyphs, four bits a button: what the presenting thread wants, and
// what the picture has (the game's thread).
std::atomic<uint32_t> g_wanted{0u};
// RG_BUTTONS set what is drawn: the menu's choice does not change it.
bool g_forced = false;
uint32_t g_applied = 0u;
bool g_loaded = false;
// The buttons' corner as loaded, unswizzled.
std::vector<uint8_t> g_original;

uint32_t pack(const Glyphs &glyphs)
{
    uint32_t packed = 0u;
    for (size_t i = 0; i < kButtons; ++i)
    {
        packed |= static_cast<uint32_t>(glyphs[i]) << (i * 4u);
    }
    return packed;
}

Glyph glyphAt(uint32_t packed, size_t i)
{
    return static_cast<Glyph>((packed >> (i * 4u)) & 0xFu);
}

// Where pixel (x, y) of a PSMT8 picture `width` wide stored in the GS's
// order is.
uint32_t swizzled(int x, int y, int width = kWidth)
{
    const int block = (y & ~0xF) * width + (x & ~0xF) * 2;
    const int swap = (((y + 2) >> 2) & 1) * 4;
    const int row = (((y & ~3) >> 1) + (y & 1)) & 7;
    const int column = row * width * 2 + ((x + swap) & 7) * 4;
    const int byte = ((y >> 1) & 1) + ((x >> 2) & 2);
    return static_cast<uint32_t>(block + column + byte);
}

// CSM1 keeps a CLUT's entries 8-15 and 16-23 of every 32 swapped.
int clutEntry(int index)
{
    const int row = index & 31;
    if (row >= 8 && row < 16)
    {
        return index + 8;
    }
    if (row >= 16 && row < 24)
    {
        return index - 8;
    }
    return index;
}

struct Picture
{
    uint8_t *pixels = nullptr;
    uint8_t *clut = nullptr;
};

// The 8-bit TIM2 picture at `tim2`, 256 colours, if it is `width` by
// `height` and lies within `limit` bytes.
bool tim2Picture(uint8_t *rdram, uint32_t tim2, int width, int height, uint32_t limit, Picture &out)
{
    uint8_t *image = getMemPtr(rdram, tim2);
    if (!image || limit < 0x40u || std::memcmp(image, "TIM2", 4) != 0)
    {
        return false;
    }
    const uint8_t *header = image + 0x10;
    uint32_t clutBytes = 0u, pixelBytes = 0u;
    uint16_t headerBytes = 0u, colours = 0u, w = 0u, h = 0u;
    std::memcpy(&clutBytes, header + 0x04, 4);
    std::memcpy(&pixelBytes, header + 0x08, 4);
    std::memcpy(&headerBytes, header + 0x0C, 2);
    std::memcpy(&colours, header + 0x0E, 2);
    std::memcpy(&w, header + 0x14, 2);
    std::memcpy(&h, header + 0x16, 2);
    const uint8_t imageType = header[0x13];
    const uint32_t pixels = 0x10u + headerBytes;
    const uint32_t size = static_cast<uint32_t>(width * height);
    if (clutBytes != kClutBytes || pixelBytes != size || colours != 256u || imageType != 5u || w != width ||
        h != height || pixels + size + kClutBytes > limit)
    {
        return false;
    }
    out.pixels = image + pixels;
    out.clut = image + pixels + size;
    return true;
}

// The loaded picture, if it is the one this knows.
bool picture(uint8_t *rdram, Picture &out)
{
    return tim2Picture(rdram, kGaijiImage + kTim2, kWidth, kHeight, kGaijiImageSize - kTim2, out);
}

// The picture's colour nearest to `rgb` among its opaque ones.
uint8_t nearest(const Picture &pic, Rgb rgb)
{
    int best = 0;
    long bestError = -1;
    for (int index = 0; index < 256; ++index)
    {
        const uint8_t *c = pic.clut + clutEntry(index) * 4;
        if (c[3] < 0x40)
        {
            continue;
        }
        const long dr = c[0] - rgb.r, dg = c[1] - rgb.g, db = c[2] - rgb.b;
        const long error = dr * dr + dg * dg + db * db;
        if (bestError < 0 || error < bestError)
        {
            best = index;
            bestError = error;
        }
    }
    return static_cast<uint8_t>(best);
}

// `rgb` itself, in the last of the picture's colours no pixel of its
// `pixelCount` uses (opaque as `like`), or the nearest when all are in use.
uint8_t exact(const Picture &pic, uint32_t pixelCount, Rgb rgb, uint8_t like)
{
    bool used[256] = {};
    for (uint32_t i = 0; i < pixelCount; ++i)
    {
        used[pic.pixels[i]] = true;
    }
    for (int index = 255; index > 0; --index)
    {
        if (!used[index])
        {
            uint8_t *c = pic.clut + clutEntry(index) * 4;
            c[0] = rgb.r;
            c[1] = rgb.g;
            c[2] = rgb.b;
            c[3] = pic.clut[clutEntry(like) * 4 + 3];
            return static_cast<uint8_t>(index);
        }
    }
    return nearest(pic, rgb);
}

// The region, unswizzled, to draw in.
struct Region
{
    uint8_t at[kRegionHeight][kRegionWidth];
};

uint8_t originalAt(int x, int y)
{
    return g_original[static_cast<size_t>(y) * kRegionWidth + x];
}

// The most common index of the template's pixels that `keep` accepts.
template <typename Keep>
uint8_t commonest(const Cell &cell, int width, int height, Keep keep)
{
    std::map<uint8_t, int> counts;
    for (int y = 0; y < height; ++y)
    {
        for (int x = 0; x < width; ++x)
        {
            if (keep(x, y))
            {
                ++counts[originalAt(cell.x + x, cell.y + y)];
            }
        }
    }
    uint8_t best = 0u;
    int bestCount = -1;
    for (const auto &[index, count] : counts)
    {
        if (count > bestCount)
        {
            best = index;
            bestCount = count;
        }
    }
    return best;
}

// A letter's cells at (x0, y0) of `at`, with a one-pixel edge around them.
template <int Height, int Width>
void stamp(uint8_t (&at)[Height][Width], const Bitmap &letter, int x0, int y0, uint8_t colour, uint8_t edge)
{
    const int width = letter.width();
    const auto on = [&](int x, int y) {
        return x >= 0 && y >= 0 && x < width && y < letter.count && letter.rows[y][x] == 'X';
    };
    for (int y = -1; y <= letter.count; ++y)
    {
        for (int x = -1; x <= width; ++x)
        {
            const int px = x0 + x, py = y0 + y;
            if (px < 0 || py < 0 || px >= Width || py >= Height)
            {
                continue;
            }
            if (on(x, y))
            {
                at[py][px] = colour;
                continue;
            }
            bool near = false;
            for (int dy = -1; dy <= 1 && !near; ++dy)
            {
                for (int dx = -1; dx <= 1 && !near; ++dx)
                {
                    near = on(x + dx, y + dy);
                }
            }
            if (near)
            {
                at[py][px] = edge;
            }
        }
    }
}

void drawFace(Region &region, const Picture &pic, const Cell &cell, Glyph glyph)
{
    const auto distance = [](int x, int y) { return std::hypot(x - 10.5, y - 10.5); };
    const uint8_t fill = commonest(kCircle, kFaceSize, kFaceSize, [&](int x, int y) {
        const double d = distance(x, y);
        return d > 5.5 && d <= 7.6;
    });
    for (int y = 0; y < kFaceSize; ++y)
    {
        for (int x = 0; x < kFaceSize; ++x)
        {
            region.at[cell.y + y][cell.x + x] =
                distance(x, y) <= 7.6 ? fill : originalAt(kCircle.x + x, kCircle.y + y);
        }
    }
    const Bitmap letter = art::faceLetter(glyph);
    stamp(region.at, letter, cell.x + art::faceLetterX(letter), cell.y + art::kFaceLetterY,
          nearest(pic, art::faceColour(glyph)), nearest(pic, art::kLetterEdge));
}

void drawPlate(Region &region, const Picture &pic, const Cell &cell, Glyph glyph)
{
    const auto onFace = [](int x, int y) {
        return x >= art::kPlateFaceLeft && x <= art::kPlateFaceRight && y >= art::kPlateFaceTop &&
               y <= art::kPlateFaceBottom;
    };
    const uint8_t body = commonest(kPlate, kPlateWidth, kPlateHeight, onFace);
    const uint8_t shade = nearest(pic, art::kPlateShade);
    for (int y = 0; y < kPlateHeight; ++y)
    {
        for (int x = 0; x < kPlateWidth; ++x)
        {
            uint8_t index = originalAt(kPlate.x + x, kPlate.y + y);
            if (onFace(x, y))
            {
                index = body;
            }
            else if (x >= art::kPlateFaceLeft && x <= art::kPlateFaceRight && y == art::kPlateFaceBottom + 1)
            {
                index = shade;
            }
            region.at[cell.y + y][cell.x + x] = index;
        }
    }
    const uint8_t white = nearest(pic, art::kPlateWhite), edge = nearest(pic, art::kPlateEdge);
    for (const bool second : {false, true})
    {
        stamp(region.at, art::plateLetter(glyph, second), cell.x + art::plateLetterX(glyph, second),
              cell.y + art::kPlateLetterY, white, edge);
    }
}

bool faceGlyph(Glyph glyph)
{
    return glyph == Glyph::A || glyph == Glyph::B || glyph == Glyph::X || glyph == Glyph::Y;
}

bool plateGlyph(Glyph glyph)
{
    return glyph == Glyph::LB || glyph == Glyph::RB || glyph == Glyph::LT || glyph == Glyph::RT;
}

void apply(uint8_t *rdram, uint32_t packed)
{
    Picture pic;
    if (!picture(rdram, pic))
    {
        return;
    }
    Region region;
    for (int y = 0; y < kRegionHeight; ++y)
    {
        for (int x = 0; x < kRegionWidth; ++x)
        {
            region.at[y][x] = originalAt(x, y);
        }
    }
    for (size_t i = 0; i < kButtons; ++i)
    {
        const Glyph glyph = glyphAt(packed, i);
        const Cell &cell = kCells[i];
        if (cell.face && faceGlyph(glyph))
        {
            drawFace(region, pic, cell, glyph);
        }
        else if (!cell.face && plateGlyph(glyph))
        {
            drawPlate(region, pic, cell, glyph);
        }
    }
    for (int y = 0; y < kRegionHeight; ++y)
    {
        for (int x = 0; x < kRegionWidth; ++x)
        {
            pic.pixels[swizzled(x, y)] = region.at[y][x];
        }
    }
    g_applied = packed;
}

void afterLoad(uint8_t *rdram, R5900Context *, PS2Runtime *, uint32_t)
{
    Picture pic;
    if (!picture(rdram, pic))
    {
        std::fprintf(stderr, "[buttons] gaiji2.img is not the picture expected, left as it is\n");
        g_loaded = false;
        return;
    }
    if (g_dumpPath)
    {
        if (FILE *out = std::fopen(g_dumpPath, "wb"))
        {
            std::fwrite(getConstMemPtr(rdram, kGaijiImage), 1, kGaijiImageSize, out);
            std::fclose(out);
        }
    }
    // Loaded afresh: the PlayStation's buttons.
    g_original.resize(static_cast<size_t>(kRegionWidth) * kRegionHeight);
    for (int y = 0; y < kRegionHeight; ++y)
    {
        for (int x = 0; x < kRegionWidth; ++x)
        {
            g_original[static_cast<size_t>(y) * kRegionWidth + x] = pic.pixels[swizzled(x, y)];
        }
    }
    g_loaded = true;
    g_applied = 0u;
    if (const uint32_t wanted = g_wanted.load(std::memory_order_relaxed); wanted != 0u)
    {
        apply(rdram, wanted);
    }
}

void gaijiLoadImage(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    rg::hooks::callThen(g_loadOriginal, rdram, ctx, runtime, &afterLoad, 0u);
}

void gaijiGetTex(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (g_loaded)
    {
        if (const uint32_t wanted = g_wanted.load(std::memory_order_relaxed); wanted != g_applied)
        {
            apply(rdram, wanted);
        }
    }
    g_getTexOriginal(rdram, ctx, runtime);
}

// Loading_01 of the loading screen's archive, if it is there.
bool loadingPicture(uint8_t *rdram, uint32_t archive, Picture &out)
{
    uint32_t arena = 0u, arenaSize = 0u, count = 0u;
    const uint8_t *limits = getConstMemPtr(rdram, kLoadingArena);
    if (!limits)
    {
        return false;
    }
    std::memcpy(&arena, limits, 4);
    std::memcpy(&arenaSize, limits + 4, 4);
    const uint8_t *im3 = getConstMemPtr(rdram, archive);
    if (!im3 || archive < arena || archive + 0x10u > arena + arenaSize || std::memcmp(im3, "IM3", 4) != 0)
    {
        return false;
    }
    const uint32_t size = arena + arenaSize - archive;
    std::memcpy(&count, im3 + 8, 4);
    for (uint32_t i = 0; i < count && 0x10u + (i + 1u) * 0x40u <= size; ++i)
    {
        const uint8_t *entry = im3 + 0x10u + i * 0x40u;
        uint32_t offset = 0u;
        std::memcpy(&offset, entry + 0x24, 4);
        if (std::strncmp(reinterpret_cast<const char *>(entry), kLoadingPicture, 0x20) == 0)
        {
            return offset < size &&
                   tim2Picture(rdram, archive + offset, kLoadingWidth, kLoadingHeight, size - offset, out);
        }
    }
    return false;
}

// The loading screen's pictures, before they are registered: its cross as
// the button bound to Cross shows.
void drawLoadingCross(uint8_t *rdram, uint32_t archive)
{
    const Glyph glyph = glyphAt(g_wanted.load(std::memory_order_relaxed), 0);
    Picture pic;
    if (!faceGlyph(glyph) || !loadingPicture(rdram, archive, pic))
    {
        return;
    }
    uint8_t cell[kLoadingCross][kLoadingCross];
    uint64_t hash = 0xCBF29CE484222325ull;
    for (int y = 0; y < kLoadingCross; ++y)
    {
        for (int x = 0; x < kLoadingCross; ++x)
        {
            cell[y][x] = pic.pixels[swizzled(kLoadingCrossX + x, kLoadingCrossY + y, kLoadingWidth)];
            hash = (hash ^ cell[y][x]) * 0x100000001B3ull;
        }
    }
    if (hash != kLoadingCrossHash)
    {
        std::fprintf(stderr, "[buttons] the loading screen's cross is not the one expected, left as it is\n");
        return;
    }
    const auto distance = [](int x, int y) { return std::hypot(x - 9.5, y - 9.5); };
    std::map<uint8_t, int> counts;
    for (int y = 0; y < kLoadingCross; ++y)
    {
        for (int x = 0; x < kLoadingCross; ++x)
        {
            if (const double d = distance(x, y); d > 5.5 && d <= 7.6)
            {
                ++counts[cell[y][x]];
            }
        }
    }
    const uint8_t fill =
        std::max_element(counts.begin(), counts.end(), [](const auto &a, const auto &b) { return a.second < b.second; })
            ->first;
    for (int y = 0; y < kLoadingCross; ++y)
    {
        for (int x = 0; x < kLoadingCross; ++x)
        {
            if (distance(x, y) <= 7.6)
            {
                cell[y][x] = fill;
            }
        }
    }
    const auto put = [&] {
        for (int y = 0; y < kLoadingCross; ++y)
        {
            for (int x = 0; x < kLoadingCross; ++x)
            {
                pic.pixels[swizzled(kLoadingCrossX + x, kLoadingCrossY + y, kLoadingWidth)] = cell[y][x];
            }
        }
    };
    // Its 256 colours are all in use, the cross's blues by the cross alone:
    // emptied, one of them takes the letter's colour.
    put();
    const uint8_t colour = exact(pic, kLoadingWidth * kLoadingHeight, art::faceColour(glyph), fill);
    const Bitmap letter = art::faceLetter(glyph);
    stamp(cell, letter, art::faceLetterX(letter) - 1, art::kFaceLetterY - 1, colour, nearest(pic, art::kLetterEdge));
    put();
}

void nameTableRegisterImArchive(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (getRegU32(ctx, 31) == kLoadingArchiveReturn)
    {
        drawLoadingCross(rdram, getRegU32(ctx, 5));
    }
    g_registerArchiveOriginal(rdram, ctx, runtime);
}

} // namespace

void install(PS2Runtime &runtime)
{
    if (const char *dump = std::getenv("RG_GAIJI_DUMP"); dump && *dump)
    {
        g_dumpPath = dump;
    }
    // RG_BUTTONS=xbox draws an Xbox pad's from the start, for tests.
    if (const char *buttons = std::getenv("RG_BUTTONS"); buttons && std::strcmp(buttons, "xbox") == 0)
    {
        setGlyphs({Glyph::A, Glyph::B, Glyph::X, Glyph::Y, Glyph::LB, Glyph::RB, Glyph::LT, Glyph::RT});
        g_forced = true;
    }
    if (!rg::timing::hookFunction(runtime, kGaijiLoadImage, &gaijiLoadImage, g_loadOriginal) ||
        !rg::timing::hookFunction(runtime, kGaijiGetTex, &gaijiGetTex, g_getTexOriginal) ||
        !rg::timing::hookFunction(runtime, kNameTableRegisterImArchive, &nameTableRegisterImArchive,
                                  g_registerArchiveOriginal))
    {
        std::fprintf(stderr, "[rg] the button symbols stay the PlayStation's\n");
    }
}

void setGlyphs(const Glyphs &glyphs)
{
    if (!g_forced)
    {
        g_wanted.store(pack(glyphs), std::memory_order_relaxed);
    }
}

} // namespace rg::buttons
