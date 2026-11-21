#pragma once

#include "ps2_runtime.h"

namespace rg::debug
{

// RG_FORCE_RETURN=RULE[,RULE...]: main-ELF functions answer what a test
// needs, to open a screen no save reaches yet (a menu row locked until a key
// item, a page shown only from a later chapter on). A rule is
//   ADDR=VALUE     the function returns VALUE
//   ADDR>=VALUE    it returns at least VALUE (signed 32-bit, as the
//   ADDR<=VALUE    it returns at most VALUE       comparison)
// and ADDR@RA... keeps it to the calls that return to RA (the caller's
// address after the call's delay slot, the jal's own + 8), so the rest of
// the game still gets the true answer; at a call, a rule with @RA comes
// before one without. The function runs as usual and only v0 changes once
// it has returned, so whatever it does besides answering still happens; a
// call unwound at a safe point gets its answer when its caller resumes
// (src/return_hook.h). At most 16 functions a run.
//
// Example, the Frog Log and the Rare Items tab's "Ancient Key Pieces"
// heading on the bench's early save: TopMenuRow5Unlocked (0x29AC50, the main
// menu's sixth row, open with the key item "Toady") says yes, and
// StateGetChapter (0x3CD5B0) says chapter 8 or later to
// MenuItemTabsAvailUpdate's call only (returning to 0x2B0090):
//   RG_FORCE_RETURN='0x29AC50=1,0x3CD5B0@0x2B0090>=8'
//
// Like any hook it sees only the calls through the function table, not a
// function reached by a tail j (docs/HOOKS.md). Each rule and its first uses
// are logged as [force-return] lines, and an RA that does not follow a call
// in the main ELF is warned about (a caller in an overlay is not loaded yet
// to look at).
void installForceReturn(PS2Runtime &runtime);

} // namespace rg::debug
