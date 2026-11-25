#pragma once

#include "ps2_runtime.h"

namespace dq8::gfx
{
class SdlGpuBackend;
}

// The game's Latin letters drawn from a typeface above 1x, instead of its
// glyphs enlarged: each glyph the renderer is about to draw from the font
// sheet is redrawn from a free typeface made to look like the console's
// (SIL Open Font License, thirdparty/fonts: Rogue Letters for the English
// dialogue, drawn after the console's own glyphs; Andika for the rest), at
// the internal resolution, in the glyph's own cell, so the game's layout,
// widths and borders stay. An Andika glyph is drawn as wide as the console's
// ink less half a pixel (before its strokes' growth), so the gaps between
// letters are even; a Rogue Letters glyph keeps its own width. The glyphs
// are told apart by their pixels, which the font pages hold when the game
// opens them (ArchiveFindEntryByName); which glyph of the typeface, its
// size and its place come from typeface_table.inc,
// which tools/typeface/make_table.py writes. The menus' own font and the
// digits and words of their pictures are redrawn the same way, known by
// the hashes of their texels the table holds; of the game's symbols, the
// plus-minus, times, middle dot and reference mark, found by their code; and
// the registered mark, a special character drawn from the buttons' picture,
// known as the digits are.
// With the European text the European pages' fonts, the European menu font
// and the digits of each language's pictures are redrawn the same way,
// accents included (the pages' glyphs found by their codes); the words of
// the pictures stay the console's. Glyphs the table leaves out (kana, kanji,
// the arrows and the other symbols) stay the console's.
// RG_TYPEFACE_LETTERS=0/1 over the menu; RG_TYPEFACE_TRACE=1 logs the pages
// read and the glyphs made; RG_TYPEFACE_SHEET=DIR writes every glyph beside
// the console's (docs/DEVELOPMENT.md).
namespace rg::typeface
{

// After loadELF(): reads the font pages as the game opens them.
void install(PS2Runtime &runtime);

// Before the game runs, after rg::texturePack::install(): the glyphs are
// asked for before the packs.
void installRenderer(dq8::gfx::SdlGpuBackend &backend);

void setEnabled(bool on);
bool enabled();
// RG_TYPEFACE_LETTERS holds the letters on or off, whatever the menu says.
bool forced();

// dq8-gfx's hash of a 256-colour CT32 palette kept in the GS's CSM1 order
// (as a TIM2 and the game's memory hold it).
uint64_t paletteHash256(const uint8_t *clut);

// A picture's palette changed by our own code (button_glyphs redraws the
// loading screen's button in colours of its own): its pieces drawn with
// `changed` are known as those of `original`, the palette the table has.
void notePaletteAlias(uint64_t changed, uint64_t original);

} // namespace rg::typeface
