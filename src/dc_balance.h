#pragma once

class PS2Runtime;

// What the Japanese Director's Cut (SCPS-17013) changed in battles, which the
// US release did not get, in three parts the menu turns on one by one:
//
// - the balance: smaller groups of enemies, the Rosencaster and Starship
//   Factory enemies and three story fights weaker, the Area Masters stronger,
//   more money from many enemies, bigger enemies harder to knock down, three
//   boss attacks without their poison or paralysis, and bosses and boss parts
//   never knocked down. The damage formula is the same in both;
// - the enemies' behavior: their attacks and AI, and CATCH (below);
// - the party's AI: an ally low on health falls back to the leader and guards.
//
// The numbers: `setup.py director-cut-balance` (Install-Undub.bat runs it)
// takes four tables of the Japanese disc's param pack (monster_param,
// encount_group_param, btl_eff_param3, fixed_encount_param), gives each the
// US table's strings (names stay English), and writes them with a list,
// tables.txt, to disc/JA/balance. Every table goes through DataTableRunCfg
// (0x3683E0, the data tables' virtual Run, with the file in the param pack:
// a1 its data, a2 its size); a file whose size and CRC-32 are the US table's
// named in the list is overwritten there with the Japanese numbers before
// it runs (with its size: never larger than the US one). Run again from the
// same buffer, a table is already the Japanese one; the file's own header
// says where it ends. A European text (src/text_language.h) has tables of its
// own, the US numbers with its names; the same step makes the Japanese numbers
// with those names for each language in disc/EU, listed in tables.txt under
// the language's names (monster_param_FR.cfg.bin) and matched the same way. A
// text without them keeps the US numbers, with a line in the log (its code at
// 0x481528); one with a table missing or not whole keeps them in all of its
// tables, so its battles never mix the two releases' numbers.
//
// The rule in code: the Director's Cut's CharaResolveActionHit (JP 0x3B7068)
// sets the target's knockdown resistance to 10, never knocked down, for a
// boss (class 2501) or boss part (2502). In the US it is read only at
// 0x3B37BC, through the target's virtual +0x160, CharaGetStatF3C (0x3B02F0)
// with index 1, which the classes 2501 (function table 0x474B20) and 2502
// (0x4746C0) share with the others; it returns 10 for those two.
//
// The enemies' files and the AI scripts: the Director's Cut changed 76
// enemies' chara\mob\*.chr (their attacks' reach, timing and kind in act.cfg,
// a few models) and 98 chara\spt\*.stb, the AI: 89 enemies' and the party's
// nine (c001-c009). They hold no text and the US executable reads them as
// they are (the script machine and its native calls are the same), but most
// are larger than the US files. The same setup step packs them into
// files.dat, each on sectors of its own, listed in files.idx (archive path,
// first sector, size). DirTableFindFile (0x182F70, one archive's index: a0
// table, a1 the entry to fill, a2 the path) answers a listed path of a part
// that is on with its place in files.dat (+4 sector, +8 size), whose sector
// number comes from the runtime's sceCdSearchFile at the first lookup; the
// game reads it like an archive's file, into a buffer of its size.
//
// The one flag the US executable lacks: CATCH, which two jellyfish (e482,
// e484) get in their act.cfg, and which the Director's Cut also gives every
// ENEMY_FLY: the low half's 0x8000 of the character flags word (chr+0x3D0,
// rebuilt every frame), a bit the US leaves free. Its lock-on jump
// (CharaJumpToTarget, 0x211070, the party's air combo) homes in on a target
// with FLY; the Director's Cut's also on one with CATCH. Here
// ParseFlagNames (0x1CA700) adds 0x8000 for those two names in the low half's
// table (0x44C1F0), and a target with CATCH but not FLY gets FLY from the
// return of the jump's lookup of it (ActionMgrFindById, 0x233B50, back at
// 0x211150; the next instruction reads the flags) until the jump returns.
// It goes with the enemies' behavior.
//
// The menu's game options `director_cut_balance`, `director_cut_enemies` and
// `director_cut_party_ai`, or RG_DIRECTOR_CUT_BALANCE, RG_DIRECTOR_CUT_ENEMIES
// and RG_DIRECTOR_CUT_PARTY_AI (1 on, 0 off), read at start. A part whose
// files are not in disc/JA/balance stays off, with a line in the log. The log
// names each table and file the first time it is used.
namespace rg::dcbalance
{

// After loadELF().
void install(PS2Runtime &runtime);

// The menu's switches; install() reads them once.
void setBalance(bool on);
void setEnemyBehavior(bool on);
void setPartyAi(bool on);

// What Install-Undub.bat put in disc/JA/balance, for the menu (it reads the
// disc folder each call): the balance's tables.txt, and the files.idx and
// files.dat the enemy behavior and the party AI are served from.
bool balanceInstalled();
bool filesInstalled();

} // namespace rg::dcbalance
