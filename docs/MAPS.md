# Function maps

ps2_recomp needs to know where every function starts and ends. This project
gives it one map per image, as a Ghidra-style CSV (`Name,Start,End,Size`, End
exclusive) named by `ghidra_output` in that image's recompiler config:

| Image | Map | Recompiler config | Functions |
|---|---|---|---|
| `SCUS_974.90` | [`functions_main.csv`](../config/SCUS_974.90/functions_main.csv) | [`recomp/main.toml`](../config/SCUS_974.90/recomp/main.toml) | 17,733 |
| `BIN/TITLE.BIN` | [`functions_title.csv`](../config/SCUS_974.90/functions_title.csv) | [`recomp/title.toml`](../config/SCUS_974.90/recomp/title.toml) | 114 |
| `BIN/INSMENU.BIN` | [`functions_insmenu.csv`](../config/SCUS_974.90/functions_insmenu.csv) | [`recomp/insmenu.toml`](../config/SCUS_974.90/recomp/insmenu.toml) | 367 |
| `BIN/NORMAL.BIN` | [`functions_normal.csv`](../config/SCUS_974.90/functions_normal.csv) | [`recomp/normal.toml`](../config/SCUS_974.90/recomp/normal.toml) | 676 |
| `BIN/FACTORY.BIN` | [`functions_factory.csv`](../config/SCUS_974.90/functions_factory.csv) | [`recomp/factory.toml`](../config/SCUS_974.90/recomp/factory.toml) | 431 |
| `BIN/INSECT.BIN` | [`functions_insect.csv`](../config/SCUS_974.90/functions_insect.csv) | [`recomp/insect.toml`](../config/SCUS_974.90/recomp/insect.toml) | 761 |

VIEWER.BIN and LAUNCHER.BIN hold no code and get no map.

## Where they come from

The seed is [RGDecomp](https://github.com/Dorgengoa/RGDecomp) (CC0), a matching
decompilation of this exact build: `config/<image>.symbols.txt`, functions only,
pinned by commit and SHA-256 in
[`funcmap.toml`](../config/SCUS_974.90/funcmap.toml). That file also lists every
correction, each with the reason. `python setup.py maps --rgdecomp <checkout>`
applies them and rewrites the CSVs; the CSVs are committed, so nobody needs
RGDecomp to build.

`python setup.py check` verifies the maps and configs against your extracted
disc (and the pinned framework): it reads, never writes, and takes under a
second.

## Main ELF layout

The ELF has one RWX section, 0x00100000..0x00481080, with code and data mixed.
From the map, the code that runs and the cross-references:

| Range | What |
|---|---|
| 0x00100000..0x00436958 | EE code |
| 0x00436980..0x00446470 | VU microcode (VIF MPG packets), rodata |
| 0x00446470..0x00447768 | Kernel-mode payloads (TLB, ExecPS2, alarm patches), copied into kernel RAM by `InitTLBFunctions`, `InitExecPS2`, `InitAlarm` |
| 0x00447768..0x00469180 | rodata and data (the CD library's state is at 0x4479xx) |
| 0x00469180..0x0046F9BC | Static constructors |
| 0x0046F9C0..0x00481080 | .data, starting with the constructor table (0x46F9C0..0x46FD64, 233 entries) |
| 0x00481080..0x004E9E80 | .bss (no section header) |
| 0x004E9E80..0x00528200 | Overlay arena (see [`overlays.toml`](../config/SCUS_974.90/overlays.toml)) |

`$gp` is 0x00485170: the value in `.reginfo`, the one the entry code builds
(`lui a0, 0x48; addiu a0, a0, 0x5170` at 0x100198/0x1001AC), passes to
`SetupThread` and moves to `gp` at 0x1001C0.

## Corrections to RGDecomp

Found by the checks below, then confirmed by reading the code.

- **Overlay static constructors.** All five overlays keep theirs in .data,
  just before the ctor table that the loader walks after each load (62 in
  total). RGDecomp clamps its overlay maps to the header's .text, so none was
  mapped, and the first overlay load would have called unmapped code.
- **Main, missing functions (17).** SDK and libc functions reached only
  through pointers (callbacks, function tables), which Ghidra never saw
  called: `sceMpegAddBs`, `sceCdCallback`, `_sceCdSC`,
  `sceMcChangeThreadPriority`, `mceIntrReadFixAlign`, the memory card's RPC
  end callback (registered at 12 sites), two `kCopy`s, newlib's `_cleanup`,
  `_init_signal`, `__sigtramp`, and others.
- **Main, merged pair.** `TaskLoadSound_207a80` is a thunk plus the real
  function at 0x207a90, which TITLE.BIN calls directly. Split in two.
- **Main, constructors.** The CRT calls 233 constructors from the table at
  0x46F9C0. The first two (0x469180, 0x469190) sit before the first one
  RGDecomp maps, and `StaticInitTexAnimeCfg_4693a0` is four of them. The
  first boot found this: its very first constructor call had no function.
- **Callbacks and getters reached through pointers.** Three `return 1` menu
  callbacks in TITLE.BIN that only the main ELF's widget tables point at, and
  eight 8-byte getters a vtable points at, which RGDecomp had merged into the
  functions before them (`split = [...]` in funcmap.toml). Found by the boot,
  each as a call to an address with no function.
- **Main, removed.** Two 8-byte "functions" that are VU microcode, and the
  11 kernel-mode payload functions, which never run at their EE address.

One gap with code stays unmapped on purpose: a lone `jr ra` at 0x119b5c whose
delay slot is the next function's first instruction.

## Checks

`setup.py check` (tools/funcmap/check.py) proves, against the retail bytes:

- **Coverage.** No non-zero word between functions in the code ranges (main)
  or in .text (overlays), except the known gaps listed in funcmap.toml.
- **Bounds.** Functions are word-aligned, inside the code ranges, and do not
  overlap.
- **Control flow.** Every J/JAL lands somewhere the recompiled code can be
  entered. Inside one image, a jump into the middle of a function is fine
  (ps2_recomp adds an `entry_<addr>` for it; the main ELF has 28). Across
  images it is not, since each image is recompiled alone: overlay calls into
  the main ELF must hit main function starts, and the main ELF's 263 direct
  calls into the arena must each hit a start in at least one overlay.
- **Constructors.** Every entry of the main ELF's constructor table and of
  each overlay's is a function start.
- **Pointers.** No code address taken as data (a word in any image's data, or
  a lui/addiu pair) lands right after a `jr ra` and its padding inside a
  mapped function: that shape is a merged function. Overlays are matched
  against themselves and the main ELF only, as they are never resident
  together.
- **Names.** Unique across all images (after ps2_recomp's identifier
  sanitizing), no prefix ps2_recomp reserves (`sub_`, `FUN_`, `LAB_`, `DAT_`,
  `entry_`), and none that would bind to a runtime handler by name.
- **Configs.** Each `stubs` entry is `handler@address` at a function start,
  and the pinned runtime implements that handler (a stub whose body is only
  `TODO_NAMED` throws when called, so binding to it is refused). Any `[mmio]`
  site would have to be inside a function.

## Naming

Names are RGDecomp's, and every one ends in its own address
(`sceCdRead_12a728`; overlay names also start with the overlay's name,
`title_...`). ps2_recomp binds any function whose name equals a runtime
handler to that handler without being asked; with the suffix, nothing binds
unless the config's `stubs` says so, so the bindings are one reviewed list.
RGDecomp's `entry` and `entry_002d94b0` become `rg_entry_...` because
`entry_` is the recompiler's own prefix for synthesized entry points.

## SDK bindings

[`recomp/main.toml`](../config/SCUS_974.90/recomp/main.toml) binds 205 SDK
functions to runtime handlers. The seed is the list DQ8-Recompiled uses with
the same framework (its pin is the parent commit of ours) and the same SDK
generation, located in this ELF by ps2_analyzer's SDK signatures and
RGDecomp's names. Where the two put an SDK function at different places, the
analyzer's choice sits at the same offset as in DQ8's copy of the library
(checked for `sceMpegInit`, 0x111050, which RGDecomp calls
`MpegStopDmaMaybe`; `sceCdDiskReady`; `sceCdGetDiskType`, where both entries
are bound because the game calls the inner one). Handlers
that are TODOs in our pin are left out (sceDmaRecv*, sceDmaWatch/Pause,
sceDeci2*, sceTty*, sceMpegGetPictureRAW8*), so the game's own code runs
there. Like DQ8, the allocator, sceGifPk*, syscall wrappers and private SDK
entry points stay translated, and so do `fflush`, `fseek` and `ftell` (DQ8 binds
them, but the runtime's versions only know handles from its own `fopen`, and
this game opens files with its own newlib).

Not bound: `sceCdLayerSearchFile` (0x1297E0) and `sceCdReadDvdDualInfo`
(0x12ACD0). Phase 3 found they need no handler: they run as the game's own
code, the dual-layer query gets no answer, so the game takes the disc for
single-layer, and the extracted folder holds both layers' files
([DISC.md](DISC.md)).

There is no `[mmio]` table. ps2_analyzer's recorded only the page base
(0x10000000) for 137 of its 190 sites, and an `[mmio]` entry overrides the
address ps2_recomp resolves on its own, so every listed timer access went to
Timer 0's count and the game's timers stopped (see STATUS.md, phase 3).
ps2_recomp already sends statically resolved hardware addresses through the
runtime, and READ/WRITE check at run time otherwise.

ps2_analyzer's eight suggested instruction patches ("potential self-modifying
code") are not used: they would turn stores to the CD library's variables at
0x4479xx into nops. The analyzer takes them for code writes because the ELF's
one section is executable throughout.

## Overlays as ELF

`python setup.py overlays` writes `build/overlays/<name>.elf` for each code
overlay: .text at 0x004E9EC0 (load address + the 0x40-byte header), .data
right after it, .bss as NOBITS. .data is marked executable, because the
constructors live there and ps2_recomp only takes map functions from
executable sections (the main ELF is one RWX section for the same reason).

## Smoke test (phase 1)

With the maps and configs above, the pinned `ps2_recomp` recompiles all six
images with no decode failure and no unhandled instruction (main: 25 s,
236 MB of C++ in 17,729 files):

Counts as of phase 2 (later map fixes and binding changes moved them slightly):

| Image | Functions | Recompiled | Bound to the runtime | Extra entry points | Functions with an unresolved JR |
|---|---|---|---|---|---|
| main | 17,726 | 17,518 | 208 | 84,067 | 183 (277 jumps) |
| title | 111 | 111 | 0 | 1,170 | 1 |
| insmenu | 367 | 367 | 0 | 2,046 | 0 |
| normal | 676 | 676 | 0 | 6,800 | 0 |
| factory | 431 | 431 | 0 | 3,930 | 1 |
| insect | 761 | 761 | 0 | 7,486 | 1 |

"Extra entry points" are resume points ps2_recomp adds inside functions (after
calls, branch targets). An unresolved JR is a computed jump whose targets it
could not prove: every instruction of that function then becomes an entry
point, which is correct but larger and slower. In the main ELF 271 of the 277
are not `switch` tables but indirect tail calls through a pointer loaded from
memory (`lw t9, ...; jr t9`, C++ virtual calls and callbacks), and only 4 are
tables, so `[jump_tables]` would not help much. Treating such a `jr` as an
indirect call is a recompiler change, left for the performance phase. (Not
done since: no patch in `patches/ps2recomp/` makes it, and such a `jr` still
goes through the runtime's dispatcher.)

The main ELF has no .bss section header, so ps2_recomp's bootstrap clears no
.bss; the game's own entry code clears 0x00481080..0x00528200 before `main`,
which covers it. Each overlay's table also registers the overlay's ELF entry
(0x004E9EC0); that is just its first function (`<name>_entry`), so it is
harmless.
