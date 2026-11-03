#pragma once

class PS2Runtime;

// Which language the voices play in. Paths carry two tokens that PathLocalize
// (0x1C28C0) replaces: `?` with the string at 0x481528 (text) and `!` with the
// one at 0x48152C (voice, as in wav/!/EV01-0060/...). StaticInitLangCodeTables
// (0x4698B0) points both at EN. With RG_VOICE_LANG=JA (or EN, ES, FR, IT, DE,
// KO, ZH) the voice token is pointed at that entry of the game's own table
// (0x44AF38) instead. The menu's Undub switch means JA: the voice archives
// (wav/ja/) and the movies that differ then come from disc/JA, which
// `setup.py extract-japanese` (Install-Undub.bat) fills.
namespace rg::voicelanguage
{

// The menu's switch: Japanese voices and movies at the next start.
void setEnabled(bool on);

// After loadELF().
void install(PS2Runtime &runtime);

} // namespace rg::voicelanguage
