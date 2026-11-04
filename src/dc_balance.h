#pragma once

class PS2Runtime;

// The battle balance of the Japanese Director's Cut (SCPS-17013), which the
// US release did not get: smaller groups of enemies, the Rosencaster and
// Starship Factory enemies and three story fights weaker, the Area Masters
// stronger, more money from many enemies, bigger enemies harder to knock down,
// three boss attacks without their poison or paralysis, and bosses and boss
// parts never knocked down. The damage formula is the same in both.
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
// says where it ends.
//
// The rule in code: the Director's Cut's CharaResolveActionHit (JP 0x3B7068)
// sets the target's knockdown resistance to 10, never knocked down, for a
// boss (class 2501) or boss part (2502). In the US it is read only at
// 0x3B37BC, through the target's virtual +0x160, CharaGetStatF3C (0x3B02F0)
// with index 1, which the classes 2501 (function table 0x474B20) and 2502
// (0x4746C0) share with the others; it returns 10 for those two.
//
// The menu's game option `director_cut_balance`, or RG_DIRECTOR_CUT_BALANCE=1,
// read at start. Without disc/JA/balance it stays off, with a line in the log.
// Not included: the enemies' own attacks (their act.cfg in chara\mob\*.chr)
// and their AI scripts, which the Director's Cut also changed.
namespace rg::dcbalance
{

// After loadELF().
void install(PS2Runtime &runtime);

// The menu's switch; install() reads it once.
void setEnabled(bool on);

} // namespace rg::dcbalance
