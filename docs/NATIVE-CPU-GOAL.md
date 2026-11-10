# Native CPU: the goal

**We own the recompiler and everything it emits.** PS2Recomp's output is the
oracle we check against, never the thing we ship or the shape we copy. No
shortcuts that keep us coupled to CPU emulation because it is easier.

The spec is *what the game computes*, not *what the PS2 did*. Only the game's
**data** keeps its PS2 form: the heap, globals, model/DMA data the native
renderer reads, overlays, saves. That is the game's own state and its
pointers are 32-bit addresses into it. Everything else is ours:

- **Registers**: host locals. Never a context struct in the hot path.
- **Stack**: native frames. Spills and saved registers are C++ locals; guest
  stack memory only for a slot whose address really escapes.
- **Calls**: direct native calls with per-function signatures from
  whole-program dataflow; inlined across functions; hooks point at native
  code, not guards around it.
- **Memory**: direct host access to the data. No bus emulation per access;
  hardware registers handled out of band (fault handler / known addresses).
- **Threads/scheduling**: fibers; nothing unwinds; checkpoints cheap.
- **VU0**: its registers as locals too, not context fields.
- **FPU**: exact PS2 results where the game depends on them, native speed
  everywhere else.

Target: the game runs ~3.6M guest instructions per frame. At ~1 host cycle
per guest instruction that is ~1 ms of game code per frame (today ~4.4 ms).
Anything that leaves us far from that is not done.

How we work:
- Correctness by differential testing against the oracle
  (tools/rgcpu/check_trees.sh, rg_harness.cpp), whole call trees.
- Performance measured in the game (unpaced walk, hardware counters on the
  game thread), not in the harness.
- Feature flag `RG_NATIVE_CPU=1`, hard cutover like the native renderer.
- When something is slow, fix the structure, don't keep the emulated path.

Status and plan: docs/NATIVE-CPU.md.
