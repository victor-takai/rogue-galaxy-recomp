#pragma once

namespace dq8::gfx
{
class SdlGpuBackend;
}

// The game's 2D art at a higher resolution: a replacement pack kept on the
// player's machine (made from their own disc, never in the repository).
//
// Collecting (RG_TEXTURE_DUMP=DIR, textures\RogueGalaxy-collect.bat): every
// region a UV sprite draws from a texture -- a glyph of the font, a menu
// label drawn as a picture, an icon, a piece of a window -- is written once
// as DIR\TEXELS_CLUT_WxH.png, named by its content (dq8-gfx:
// SdlGpuBackend::setRegionDump), alpha doubled so the GS's 0x80 is opaque.
// The font goes glyph by glyph: its 512x512 texture covers rows of other
// data, whose changes would change a name taken from the whole texture.
//
// Reading (textures\hd, RG_TEXTURE_PACK=DIR): a PNG there by the same name,
// a whole 2 to 8 times larger (tools\texture_pack\make_pack.py writes them),
// is drawn in place of that region above 1x (SdlGpuBackend::
// setRegionReplacements); the menu's "Larger 2D art" turns it off.
namespace rg::texturePack
{

// Before the game runs.
void install(dq8::gfx::SdlGpuBackend &backend);

} // namespace rg::texturePack
