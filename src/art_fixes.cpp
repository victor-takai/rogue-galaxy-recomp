#include "art_fixes.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "ps2_runtime.h"
#include "runtime/ps2_memory.h"
#include "scene_timing.h"

namespace rg::artfix
{
namespace
{

// NameTableRegisterImArchive(table, archive, ...) registers an IM3
// archive's pictures as textures: its count at +0x8 and 0x40-byte entries
// from +0x10, each a name of 0x20 bytes then its TIM2's offset at +0x24 (as
// rg::buttons reads the loading screen's). NameTableRegisterImArchiveFwd
// (for the effects' and the model configs' callers) ends in a `j` to it,
// past the function table, with the archive in a1 as well: both are hooked.
constexpr uint32_t kNameTableRegisterImArchive = 0x001594D0u;
constexpr uint32_t kNameTableRegisterImArchiveFwd = 0x001594C0u;
constexpr uint32_t kEntries = 0x10u;
constexpr uint32_t kEntryBytes = 0x40u;
constexpr uint32_t kMaxEntries = 256u;
constexpr char kPicture[] = "Pty_02";
// Its TIM2: the picture's header at +0x10, the pixels after it (8-bit,
// in the GS's order), then a 256-colour CLUT, in the GS's CSM1 order unless
// the CLUT type's bit 0x80 says CSM2 (in order).
constexpr uint32_t kPictureHeader = 0x10u;
constexpr uint16_t kSide = 128u;
constexpr uint32_t kPixels = uint32_t(kSide) * kSide;
constexpr uint32_t kClutBytes = 256u * 4u;
// FNV-1a of its pixels as on the disc (the US and the PAL English), and as
// fixed here.
constexpr uint64_t kOriginalHash = 0x435130808B7FBC22ull;
constexpr uint64_t kFixedHash = 0xA37BB3D508A44B39ull;
constexpr uint8_t kMagenta[] = {74u, 83u, 90u};
constexpr uint8_t kClear = 98u;
constexpr uint8_t kClutCsm2 = 0x80u;

// The CLUT position of colour `index`: CSM1 keeps a CLUT's entries 8-15 and
// 16-23 of every 32 swapped.
uint32_t clutEntry(uint32_t index, bool csm1)
{
    if (!csm1)
    {
        return index;
    }
    const uint32_t row = index & 31u;
    return row >= 8u && row < 16u ? index + 8u : row >= 16u && row < 24u ? index - 8u : index;
}

PS2Runtime::RecompiledFunction g_registerOriginal = nullptr;
PS2Runtime::RecompiledFunction g_registerFwdOriginal = nullptr;
bool g_fixed = false;
bool g_unknown = false;

// `size` bytes of guest memory at `address`, if they are one run of it.
uint8_t *guestRange(uint8_t *rdram, uint32_t address, uint32_t size)
{
    uint8_t *first = getMemPtr(rdram, address);
    if (!first || size == 0u || address + (size - 1u) < address)
    {
        return nullptr;
    }
    return getMemPtr(rdram, address + (size - 1u)) == first + (size - 1u) ? first : nullptr;
}

uint64_t fnv1a(const uint8_t *bytes, uint32_t size)
{
    uint64_t hash = 0xCBF29CE484222325ull;
    for (uint32_t i = 0; i < size; ++i)
    {
        hash = (hash ^ bytes[i]) * 0x100000001B3ull;
    }
    return hash;
}

// Pty_02's pixels in the archive at `archive`, if it holds it as the disc
// has it (128x128, 8-bit, 256 colours), and its CLUT after them (`csm1`, its
// order).
uint8_t *labelPixels(uint8_t *rdram, uint32_t archive, const uint8_t *&clut, bool &csm1)
{
    const uint8_t *im3 = guestRange(rdram, archive, kEntries);
    if (!im3 || std::memcmp(im3, "IM3", 4) != 0)
    {
        return nullptr;
    }
    uint32_t count = 0u;
    std::memcpy(&count, im3 + 8, 4);
    if (count == 0u || count > kMaxEntries)
    {
        return nullptr;
    }
    const uint8_t *entries = guestRange(rdram, archive + kEntries, count * kEntryBytes);
    if (!entries)
    {
        return nullptr;
    }
    for (uint32_t i = 0; i < count; ++i)
    {
        const uint8_t *entry = entries + i * kEntryBytes;
        if (std::strncmp(reinterpret_cast<const char *>(entry), kPicture, 0x20) != 0)
        {
            continue;
        }
        uint32_t offset = 0u;
        std::memcpy(&offset, entry + 0x24, 4);
        const uint32_t tim2 = archive + offset;
        const uint8_t *image = guestRange(rdram, tim2, kPictureHeader + 0x18u);
        if (!image || std::memcmp(image, "TIM2", 4) != 0)
        {
            return nullptr;
        }
        const uint8_t *header = image + kPictureHeader;
        uint32_t clutBytes = 0u, pixelBytes = 0u;
        uint16_t headerBytes = 0u, colours = 0u, width = 0u, height = 0u;
        std::memcpy(&clutBytes, header + 0x04, 4);
        std::memcpy(&pixelBytes, header + 0x08, 4);
        std::memcpy(&headerBytes, header + 0x0C, 2);
        std::memcpy(&colours, header + 0x0E, 2);
        std::memcpy(&width, header + 0x14, 2);
        std::memcpy(&height, header + 0x16, 2);
        if (clutBytes != kClutBytes || pixelBytes != kPixels || colours != 256u || header[0x13] != 5u ||
            width != kSide || height != kSide)
        {
            return nullptr;
        }
        uint8_t *pixels = guestRange(rdram, tim2 + kPictureHeader + headerBytes, kPixels + kClutBytes);
        clut = pixels ? pixels + kPixels : nullptr;
        csm1 = (header[0x12] & kClutCsm2) == 0u;
        return pixels;
    }
    return nullptr;
}

// The CLUT's entries kMagenta are opaque magenta: the picture with the slip.
bool hasMagenta(const uint8_t *clut, bool csm1)
{
    for (const uint8_t index : kMagenta)
    {
        const uint8_t *rgba = clut + clutEntry(index, csm1) * 4u;
        if (rgba[0] < 0xC0u || rgba[1] > 0x10u || rgba[2] < 0xC0u || rgba[3] == 0u)
        {
            return false;
        }
    }
    return true;
}

void fixLabels(uint8_t *rdram, uint32_t archive)
{
    const uint8_t *clut = nullptr;
    bool csm1 = true;
    uint8_t *pixels = labelPixels(rdram, archive, clut, csm1);
    if (!pixels)
    {
        return;
    }
    const uint64_t hash = fnv1a(pixels, kPixels);
    if (hash == kFixedHash)
    {
        return;
    }
    if (hash != kOriginalHash)
    {
        // Another Pty_02 (the European text's, its own words): left quietly,
        // unless its palette has the slip's colours.
        if (!g_unknown && hasMagenta(clut, csm1))
        {
            std::fprintf(stderr, "[art] %s is not the picture expected (%016llX), left as it is\n", kPicture,
                         static_cast<unsigned long long>(hash));
            g_unknown = true;
        }
        return;
    }
    for (uint32_t i = 0; i < kPixels; ++i)
    {
        for (const uint8_t magenta : kMagenta)
        {
            if (pixels[i] == magenta)
            {
                pixels[i] = kClear;
            }
        }
    }
    if (!g_fixed)
    {
        std::fprintf(stderr, "[art] the Player and Party labels' magenta corners cleared (%s at 0x%08X)\n", kPicture,
                     archive);
        g_fixed = true;
    }
}

void nameTableRegisterImArchive(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    fixLabels(rdram, getRegU32(ctx, 5));
    g_registerOriginal(rdram, ctx, runtime);
}

void nameTableRegisterImArchiveFwd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    fixLabels(rdram, getRegU32(ctx, 5));
    g_registerFwdOriginal(rdram, ctx, runtime);
}

} // namespace

void install(PS2Runtime &runtime)
{
    if (const char *fixes = std::getenv("RG_ART_FIXES"); fixes && std::strcmp(fixes, "0") == 0)
    {
        std::fprintf(stderr, "[rg] the game's pictures as the disc has them (RG_ART_FIXES=0)\n");
        return;
    }
    if (!rg::timing::hookFunction(runtime, kNameTableRegisterImArchive, &nameTableRegisterImArchive,
                                  g_registerOriginal))
    {
        std::fprintf(stderr, "[rg] the Player and Party labels keep their magenta corners\n");
        return;
    }
    if (!rg::timing::hookFunction(runtime, kNameTableRegisterImArchiveFwd, &nameTableRegisterImArchiveFwd,
                                  g_registerFwdOriginal))
    {
        std::fprintf(stderr, "[rg] the Player and Party labels keep their magenta corners where the "
                             "effects register pictures\n");
    }
}

} // namespace rg::artfix
