// Larger versions of the 2D art a sprite samples, made by the host from what
// the sprite's texture rectangle holds (Rogue Galaxy: added).
//
// A whole texture is no unit for this: Rogue Galaxy's font sheet declares
// 512x512 over rows of other data that change with the scene. The
// rectangle a UV sprite samples (a glyph's cell) is, hashed as a whole
// texture's content is: its texels (indices for an indexed format) and its
// palette window. The host is shown each new one whose palette it asked
// for, and answers with a larger picture of it, which the sprite is then
// drawn from, bilinear, at the target's scale.
#pragma once

#include <cstdint>
#include <functional>
#include <vector>

namespace dq8::gfx {

struct RegionRequest {
    // Its identity: the texels' and the palette window's hashes, and size.
    uint64_t texels = 0u;
    uint64_t clut = 0u;
    uint32_t width = 0u;
    uint32_t height = 0u;
    // Its place in the texture: its top-left texel.
    uint32_t x = 0u;
    uint32_t y = 0u;
    // Its final RGBA, row by row, alpha as the GS's (0x80 opaque).
    const uint8_t *rgba = nullptr;
    // The same, with every texel of the region as it is (`rgba` has the ones
    // this draw never shows transparent): what a picture of the region
    // should be made from, as another draw of it may show them.
    const uint8_t *full = nullptr;
    // Host pixels per GS pixel of the target it is drawn into.
    uint32_t scale = 1u;
    // The border version: see SdlGpuBackend::setRegionReplacements.
    bool border = false;
    // The shadow version, with the border's margin: the same.
    bool shadow = false;
    // Drawn with vertex alpha past 0x80, which the GS takes as a blend
    // factor past one: every texel at half coverage or more comes out
    // opaque, and the glyph looks bolder than at 0x80 (Rogue Galaxy's menus
    // draw their letters at 0xFF, their black copies at 0x80).
    bool bold = false;
    // A whole texture, not a region (3D art, a pack made for PCSX2): `texels`
    // is the hash PCSX2 names it by (XXH3-64 of its GS blocks), `clut` the
    // hash of its palette as PCSX2 makes it, `width` and `height` are the
    // texture's size, and no pixels are given.
    bool whole = false;
    // For a whole texture, the block formats the GPU samples itself
    // (kRegionBc1 and the others, or'd): the host may answer with its
    // picture's blocks as they are, which keeps them compressed on the GPU.
    int blockFormats = 0;
};

// The texels of margin a border or shadow version has past the region on
// each side.
constexpr uint32_t kRegionBorderMargin = 2u;

// The answers of a RegionLookup.
constexpr int kRegionNone = 0;
constexpr int kRegionPending = 1;
constexpr int kRegionReady = 2;
// Or'd onto kRegionReady: draw the picture from its own texels, not filtered
// across them (a pack of pixel art made at a whole multiple of the original).
constexpr int kRegionNearest = 4;
// Or'd onto kRegionReady for a whole texture whose request lists the format
// in blockFormats: `rgba` holds the picture's 4x4 blocks of that format, a
// row of them after another, its width and height multiples of 4.
constexpr int kRegionBc1 = 8;
constexpr int kRegionBc2 = 16;
constexpr int kRegionBc3 = 32;
constexpr int kRegionBc7 = 64;
constexpr int kRegionBlocks = kRegionBc1 | kRegionBc2 | kRegionBc3 | kRegionBc7;

// Fills `rgba` with the larger picture (alpha as the GS's), `width` by
// `height`, a whole multiple of the region's size (with the border's margin
// for a border or shadow version), 2 to 8, and answers kRegionReady (with
// kRegionNearest, if it is to be drawn unfiltered; with a kRegionBc* for
// blocks); kRegionPending
// while it is still making it (the region is drawn as it is meanwhile); or
// kRegionNone, after which it is not asked about that content again.
using RegionLookup = std::function<int(const RegionRequest &request, uint32_t &width, uint32_t &height,
                                       std::vector<uint8_t> &rgba)>;
// Whether the host wants the regions drawn with that palette at all: the
// rest are left before their texels are hashed.
using RegionPalettes = std::function<bool(uint64_t clut)>;

} // namespace dq8::gfx
