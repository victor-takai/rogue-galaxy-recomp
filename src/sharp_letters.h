#pragma once

class PS2Runtime;

namespace dq8::gfx
{
class SdlGpuBackend;
struct RegionRequest;
}

#include <cstdint>
#include <vector>

// The game's letters drawn sharp at any scale (the menu's "Sharp letters").
//
// Its fonts are sheets of small glyphs, 16 levels of white over transparent
// (or grey within a dark border drawn in), drawn about one texel to a pixel:
// enlarged, they are blocks. Each glyph a sprite draws from one of them is
// traced into smooth outlines (font_trace.h) on threads of their own the
// first time it shows, and drawn from then on at the internal resolution
// (dq8-gfx's region replacements, sdlgpu_regions.h); meanwhile, and at 1x,
// it is drawn as it is.
//
// The game borders its dialogue letters with four black copies of each, a
// pixel above, below, right and left (FontDrawGlyphOutline's mode 1). On
// smooth letters those leave notches where an edge faces a diagonal, so
// they are marked (their y a sixteenth of a pixel up) and the backend draws
// one round border, a pen a pixel wide run around the outline, instead.
namespace rg::letters
{

// After loadELF().
void install(PS2Runtime &runtime);

// Before the game runs.
void installRenderer(dq8::gfx::SdlGpuBackend &backend);

void setEnabled(bool on);

// The region replacement's two halves, which texture_pack.cpp chains in front
// of the pack's own (the backend takes one lookup): whether a palette is a
// font's, and the lookup for the regions drawn with one.
bool wantsRegion(uint64_t palette);
int lookupRegion(const dq8::gfx::RegionRequest &request, uint32_t &width, uint32_t &height,
                 std::vector<uint8_t> &rgba);

} // namespace rg::letters
