#pragma once

#include "ps2_runtime.h"

namespace dq8::gfx
{
class SdlGpuBackend;
}

// The game's Latin letters drawn from a typeface above 1x, instead of its
// glyphs enlarged: each glyph the renderer is about to draw from the font
// sheet is redrawn from a free typeface made to look like the console's
// (Andika, SIL Open Font License, thirdparty/fonts), at the internal
// resolution, in the glyph's own cell, so the game's layout, widths and
// borders stay. The glyphs are told apart by their pixels, which the font
// pages hold when the game opens them (ArchiveFindEntryByName); which glyph
// of the typeface, its size and its place come from typeface_table.inc,
// which tools/typeface/make_table.py writes. Glyphs the table leaves out
// (kana, kanji, the game's symbols, the menus' grey font) stay the
// console's. RG_TYPEFACE_LETTERS=0/1 over the menu; RG_TYPEFACE_TRACE=1
// logs the pages read and the glyphs made; RG_TYPEFACE_SHEET=DIR writes
// every glyph beside the console's (docs/DEVELOPMENT.md).
namespace rg::typeface
{

// After loadELF(): reads the font pages as the game opens them.
void install(PS2Runtime &runtime);

// Before the game runs, after rg::texturePack::install(): the glyphs are
// asked for before the packs.
void installRenderer(dq8::gfx::SdlGpuBackend &backend);

void setEnabled(bool on);

} // namespace rg::typeface
