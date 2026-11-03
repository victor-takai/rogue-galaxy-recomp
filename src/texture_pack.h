#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace dq8::gfx
{
class SdlGpuBackend;
}

// The game's art at a higher resolution: replacement packs kept on the
// player's machine (made from their own disc or by others, never in the
// repository).
//
// Collecting (RG_TEXTURE_DUMP=DIR, debug\RogueGalaxy-collect.bat): every
// region a UV sprite draws from a texture -- a glyph of the font, a menu
// label drawn as a picture, an icon, a piece of a window -- is written once
// as DIR\TEXELS_CLUT_WxH.png, named by its content (from the
// lookup dq8-gfx asks for each new region, which this file answers), alpha doubled so the GS's 0x80 is opaque.
// The font goes glyph by glyph: its 512x512 texture covers rows of other
// data, whose changes would change a name taken from the whole texture.
//
// Reading (textures\hd, RG_TEXTURE_PACK=DIR): a PNG there by the same name,
// a whole 2 to 8 times larger (tools\texture_pack\make_pack.py writes them),
// is drawn in place of that region above 1x (SdlGpuBackend::
// setRegionReplacements). A pack made for PCSX2 (TEX0HASH-CLUTHASH-BITS.dds)
// replaces whole textures, the scenery's and the characters', through the
// same lookup (RegionRequest::whole); docs/TEXTURE_PACKS.md.
namespace rg::texturePack
{

// Before the game runs.
void install(dq8::gfx::SdlGpuBackend &backend);

// The packs found, in the order they are read (a later one over an earlier
// one): `key` names the pack's switch in the settings (texture_pack_KEY),
// `enabled` is where it starts.
struct PackInfo
{
    std::string key, name;
    size_t regions;
    bool enabled;
};
std::vector<PackInfo> packs();

void setPackEnabled(const std::string &key, bool on);

// What dq8-gfx kept of the answers goes, so the regions and textures are
// asked about again (a pack switched).
void refresh();

} // namespace rg::texturePack
