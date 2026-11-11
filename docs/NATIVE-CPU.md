# Native CPU

The goal is in [NATIVE-CPU-GOAL.md](NATIVE-CPU-GOAL.md). Everything here is behind
`RG_NATIVE_CPU=1`, off by default.

## Why

The CPU-bound walk (2026-11-07) runs ~3.6M EE instructions
per frame. That is one real PS2 frame of work, so nothing is being wasted on
the guest side. What costs us is the price of each guest instruction:
~11 host instructions and ~5.5 cycles in game code, IPC ~2. The profile is
flat (no guest function above 0.5%), which points at the shape of every
function, not at a hot spot.

That shape comes from the contract ps2_recomp's output works under, not from
any one macro:

- **Guest registers live in `R5900Context`**, as 128-bit `__m128i` slots. A
  32-bit `addiu` extracts the low lane, adds, sign-extends, rebuilds a vector
  and merges it back into the slot.
- **Every runtime entry takes `ctx`.** A load's slow path
  (`runtime->Load32(rdram, ctx, a)`), a call (`directGuestCall`), a
  checkpoint (`eeCheckpointDue`) and a syscall can each read or write any
  register. So the compiler stores every live register before the call and
  reloads it afterwards. A guest register never stays in a host register
  across one.
- **Any checkpoint can unwind.** `eeCheckpointDue()` returning true makes the
  function `return` with `ctx->pc` set. The scheduler then re-enters the
  function at `ctx->pc` through the `switch` at its top. That only works if
  `ctx` is exact at every back edge and every call, which is also why every
  memory access stores `ctx->pc` first.

Earlier work removed these one at a time: lean PC, `__restrict` rdram, the
plain-RAM fast path, and regcache (registers in locals, but still writing
them back to `ctx` at every call). Each gained a few percent at most,
because the other parts of the contract still forced the round-trips. The
fix has to replace the whole contract at once. Like the native renderer, it
goes behind a flag.

## The contract

1. **Guest GPRs are `uint64_t` locals.** Upper halves get their own locals,
   touched only by `lq`/`sq` and the MMI ops. FPU registers, `fcr31`, HI/LO
   and `sa` are locals too.
2. **Memory stays one compare and one `mov`.** The plain-RAM check is
   inline; the slow path is a cold out-of-line call that gets the guest
   context (for exceptions and tracing) but not the locals, so it clobbers
   nothing. Exceptions raised there see stale registers; the game raises
   none.
3. **Checkpoints never unwind.** Guest threads already have their own stacks
   (`EeFiber`): blocking syscalls suspend the fiber in place today. A
   checkpoint that is due should do the same thing, preempting the thread by
   switching fibers and continuing when the scheduler resumes it. The
   locals stay on the fiber's stack. The `switch (ctx->pc)` re-entry table,
   the per-instruction `ctx->pc` stores and `in_delay_slot`/`branch_pc` all
   go away.
4. **Calls are C calls.** A `jal` to a function on the same contract becomes
   a direct call that passes the registers the callee reads and returns the
   ones it writes. The register sets come from analysing the callee, not
   from assuming the MIPS ABI, since hand-written code breaks the ABI. Only
   `jalr` through a table and calls into old-contract code go through
   `ctx`.
5. **`ctx` is the boundary format.** Registers are copied into `ctx` and
   back only where the native contract meets the old one: syscalls, host
   stubs and hooks, VU0 `vcallms`, functions not yet converted, and thread
   entry and exit.

### Every place the runtime reads `ctx` in the middle of a function

| Reader | What it needs | Under the contract |
|---|---|---|
| `directGuestCall` / `dispatchGuestBranch` | all GPRs, pc, sp (for the unwind check) | native callee: a direct call with arguments. Old-contract callee: write back the callee's input set, reload its output set |
| `eeCheckpointDue` (back edges) | exact `ctx` for re-entry | yield the fiber in place; no `ctx` needed |
| `Load*`/`Store*` slow path | `ctx` only for `SignalException` and `ps2TraceGuestWrite` | the guest `ctx`, stale registers (documented) |
| `handleSyscall`, `ps2_syscalls::*`, `ps2_stubs::*` | a0–a3, v0/v1, sp, sometimes more | boundary: write back the declared argument registers, reload v0/v1 (and whatever the stub table says it writes) |
| `executeVU0Microprogram` / VU0 hook | `vu0_vf`, `vi`, `acc`, `q` | VU0 registers stay in the guest context for now; a later step makes them locals too |
| `handleBreak`, `SignalException`, COP0 | pc, registers for the report | boundary write-back before the call (cold) |
| Scheduler debug snapshot (`m_debugPc`, ra, sp) | pc/ra/sp between turns | written at the boundary points; debug only |
| DMAC handlers drained from a store | none (they run on their own context) | unchanged |

## The plan

1. **A leaf prototype** (2026-11-07). The first test took PS2Recomp's own
   output for every leaf function and recompiled it under the contract,
   using macros only. It produced 0 mismatches against the generated code
   over 5,609 leaves, and ran 1.57x faster over all of them. Weighted by
   the walk's samples that is only 4.4% of guest time: the cost sits in
   functions that make calls and in the call and checkpoint machinery.
   That made the case for doing the whole contract at once, with a
   translator of our own instead of a text pass over PS2Recomp's output.
2. **Checkpoints, waits and callbacks suspend in place** (patches 0036 and
   0037). A due checkpoint (`eeCheckpointDue`, `directGuestCall`,
   `dispatchGuestBranch`) suspends the running fiber
   (`EeScheduler::yieldInPlace`) and returns to the run loop. The loop
   handles events and preemption as before, and when the thread runs again
   the fiber resumes at the checkpoint, which then returns "not due".
   `blockCurrent`, `waitVSync`, `waitExternal` and `invokeCurrent` do the
   same. Each invocation level (interrupt handler, callback) has its own
   fiber, so an interrupt can run while the code it interrupted is
   suspended in a call; before, the unwind made room for it. The unwind
   stays off a fiber, inside an interrupt handler (the EE does not switch
   threads there) and on a stop.
   Measured on its own (unpaced walk, hardware counters on the game
   thread, off/on twice: 30.4/30.2 against 32.1/31.3 Mcycles per frame,
   with VIF1, which runs none of this code, moving as much): **no gain on
   its own**. At ~100 unwinds a frame a fiber switch costs about what the
   unwind did. What it buys is native code that never has to be re-entered
   in the middle of a function. The 30 fps walk's frames match.
3. **Our own translator** (tools/rgcpu). It reads the game's MIPS code,
   turns it into C++ under the contract, and is checked against
   PS2Recomp's output, which stays the oracle.
4. **In the game.** `RG_NATIVE_CPU=1` puts the translated functions into the
   function table, and the remaining runtime boundary (checkpoint and calls
   out of native code) moves inline.

Measure each step in the game. The metric is the main guest thread's ms per
frame from `RG_EE_WAITLOG=1` (its share of wall time over the frame rate),
taken over several runs a side, alternating.
