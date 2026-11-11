#pragma once

#include <cstdint>

#include "ps2_runtime.h"

// The game's text in French, German, Spanish or Italian, from the player's own
// European disc (SCES-54552).
//
// The US executable already has the text-language token (`?` in a path,
// PathLocalize); it only ever holds EN. Set to FR it asks for `..._fr.txt`,
// `menu\rc\..._fr.pac` and so on. Install-European.bat (setup.py
// extract-european, tools/disc/european.py) builds those files from both discs
// into disc/EU/<LANG>.DAT and .idx: the US files with the European words in them,
// since the European disc is a revised build whose own files the US executable
// cannot use as they are. The file lookup (DirTableFindFile, 0x182F70) serves
// them, and a file the language has none of falls back to its English name.
//
// Besides the files: the accent tags of the European texts (<'e>) get glyph codes
// in the message font and one-byte codes in the menus' font, and the menus' and
// game data's memory gets the European executable's sizes.
namespace rg::textlanguage
{

// The menu's choice (Language > Text): 0 English, 1 French, 2 German, 3 Spanish,
// 4 Italian. Read once, at boot.
void setPreferred(int choice);

// The language the game's text is in, numbered as that choice: English (0) until
// chooseAtBoot has set another. Writing to its menus: the accents as the European
// texts spell them (<'e>), which the menus' font then draws.
int language();

// Install-European.bat has made that language's files (numbered as the choice;
// disc/EU/<LANG>.idx, which the boot looks for too), for the menu; English is
// always there. It reads the disc folder each call.
bool installed(int language);

// From the hook of StaticInitLangCodeTables, after the game filled its tables:
// sets the text token (0x481528) to the language chosen (RG_TEXT_LANG over the
// menu) when its files exist in disc/EU.
void chooseAtBoot(uint8_t *rdram, PS2Runtime &runtime);

// The European language the text is in, as a column of the tables of European
// sizes: 0 French, 1 German, 2 Spanish, 3 Italian; -1 in English (and while
// the language's files are not known usable, when the game reads the English
// ones).
int europeanColumn();

// After loadELF(): the hooks.
void install(PS2Runtime &runtime);

// The European sizes of the Insectron screens (insectron_layouts.cpp): the
// capture notices and the overlays' menus. install() calls it, last; a second
// call would hook its own hooks.
void installInsectronLayouts(PS2Runtime &runtime);

} // namespace rg::textlanguage
