#pragma once

#include <cstdint>

#include "ps2_runtime.h"

// The game's text in a European language, read from the player's own PAL disc
// (SCES-54552) kept in disc/EU (DATA0_0 and DATA1_0, .DAT and .HD6).
//
// The US executable already has the text-language token (`?` in a path,
// PathLocalize); it only ever holds EN. With another language it asks for
// `..._fr.txt`, `menu\help_board\fr\...` and so on, which the US archives lack.
// The file lookup (DirTableFindFile, 0x182F70) is hooked: a file the PAL
// archive has for that language is served from there, and one neither disc has
// for it (the font, the menus' pictures) falls back to its English name.
namespace rg::textlanguage
{

// RG_TEXT_LANG for the run (a two-letter code: ES FR IT DE), from the hook of
// StaticInitLangCodeTables, after the game filled the tables. Sets the token
// at 0x481528 when the disc/EU files exist.
void chooseAtBoot(uint8_t *rdram, PS2Runtime &runtime);

// After loadELF(): hooks the file lookup.
void install(PS2Runtime &runtime);

} // namespace rg::textlanguage
