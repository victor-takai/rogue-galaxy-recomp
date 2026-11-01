# Status

**The game runs.** `roguegalaxy` boots to the title screen, starts a new
game, plays the opening movie and the real-time cutscenes in Salgin, and
fights battles (HUD, damage, allies, results screen) at 60 frames per
second with `Rogue Galaxy.exe` (the scene's step follows the frame time, so
a slow frame does not play in slow motion; the game's own 30 without
`RG_FPS=60`), with the GS on the GPU, VU1 compiled ahead of time and on a
thread of its own, music and sound effects, keyboard or gamepad. The
heaviest field scene measured, a boss gate, holds 50 to 60, limited by the
VU1 programs.
Known gaps: a rare hang after the Story loading screen (below). Release
0.1.0 is the source with an installer that builds the game from the
player's disc (`Install.bat`; [`CHANGELOG.md`](../CHANGELOG.md)).

| Phase | Goal | State |
|---|---|---|
| 0 | Repo, framework pin, disc verify/extract, overlay table | done |
| 1 | Function maps for the ELF and the 5 real overlays | done |
| 2 | Build ELF + overlays into one executable; overlay-aware dispatch | done |
| 3 | Boot to the title screen | done |
| 4 | Field, battle, movies, sound | done |
| 5 | Performance, widescreen, 60 fps, launcher | 60 fps mode by default (50-60 in the heaviest scenes), launcher, in-game menu, widescreen option, installer (0.1.0) |

## Phase 1 in short

Details in [MAPS.md](MAPS.md).

- One map per image (`config/SCUS_974.90/functions_<image>.csv`), from
  RGDecomp's symbol files with 62 overlay constructors, 4 main constructors
  and 17 other functions added, 13 removed and two functions split, each
  justified in [`funcmap.toml`](../config/SCUS_974.90/funcmap.toml). The
  big one: every overlay's static constructors live in its .data, where no
  map had them, so the first overlay load would have failed. (The main ELF's
  constructor fixes came from the first boot, in phase 2.)
- One ps2_recomp config per image (`config/SCUS_974.90/recomp/`). The main
  one binds 208 SDK functions (205 since phase 4) to runtime handlers, seeded from
  DQ8-Recompiled's list for the same framework. (It also carried
  ps2_analyzer's MMIO table until phase 3 showed that table was wrong.)
- `python setup.py check` proves coverage, bounds, cross-image control flow,
  constructors, names and bindings against the retail bytes; it passes.
- Smoke test: the pinned `ps2_recomp` recompiles all 20,072 functions with no
  decode failure and no unhandled instruction. 186 functions (183 in the main
  ELF) have computed jumps it could not resolve and fall back to making every
  instruction an entry point.
- `$gp` is settled: 0x00485170, from `.reginfo` and the entry code.

## Framework

`thirdparty/PS2Recomp` is [Sinan-Karakaya/PS2Recomp](https://github.com/Sinan-Karakaya/PS2Recomp)
at `379b9ca` (2026-10-11), the fork that Dragon Quest VIII Recompiled builds
on (DQ8 pins `888ec47`, the parent commit). Against upstream
[ran-j/PS2Recomp](https://github.com/ran-j/PS2Recomp) `eb8688b` it is 139
commits ahead and 7 behind. What we need from it:

- dispatch of overlaid code (`1d4c4dc`) through a `FunctionRegion` resolver:
  upstream has one dense function table per ELF and no way to hold several
  overlays at the same addresses;
- the game's own IRX modules on an emulated IOP with an SPU2 path (`f16758d`,
  `2d03b8c`): upstream has no SPU2, so the game would be silent;
- whole VU1 microprograms compiled ahead of time (`97d1214`, `7d4779e`);
- PSS movie fixes (`96c2ecf`, `e3f7eb2`, `dc4bbd8`), a raster backend that
  can own the window (`438d05e`).

Upstream commits the fork lacks and we will want: `eb8688b` PS2 float
semantics for EE FPU results and DIV.S/RSQRT.S (#272), see the float patch
below; `a5d3049` resume points for standalone `entry_*` functions (#271);
`2c5fbb9` syscalls 0x79/0x7A (#274). Upstream's own IOP emulator (`75d729c`,
#244) diverges from the fork's and is not needed.

## What the game needs (known so far)

**Overlays.** Main ELF code is 0x00100000-0x00436958 plus static constructors
at 0x00469300-0x0046F9BC (layout in [MAPS.md](MAPS.md)). Five real overlays
(TITLE, INSMENU, NORMAL, FACTORY, INSECT) share the arena
0x004E9E80-0x00528200, each with its own map and generated code. Dispatch
must follow whichever one is resident: the main ELF calls into the arena
directly at 263 sites, and every overlay calls back into the main ELF. The
hook point is the `jr ra` stub at 0x00100400, called with a0 = header after
every load and before the constructors run. Details in
[`config/SCUS_974.90/overlays.toml`](../config/SCUS_974.90/overlays.toml).
This is the same Metrowerks scheme as DQ8 (its stub is at 0x00100320).

**IOP.** Per RGDecomp, the ELF reboots the IOP with `MODULES/IOPRP300.IMG`,
then loads 15 IRX modules from `cdrom0:\MODULES\` with `sceSifLoadModule`,
retrying until each succeeds. Sound goes through SDRDRV (`sceSdRemote*`) and the SCE "sk"
sound kit (SKSOUND/SKHSYNTH/SKMIDI/SKMSIN, MODHSYN/MODMIDI/MODMSIN).

**CD/DVD.** Data archives are opened per layer with `sceCdLayerSearchFile`
(0x001297E0) and `sceCdReadDvdDualInfo` (0x0012ACD0). Neither upstream's
runtime nor the fork has a handler for them; they need one (in a host-folder
layout both can resolve by name, see [DISC.md](DISC.md)). Streaming uses
CDVDSTM.

**Movies.** Sony libmpeg + IPU (`PsIIlibipu 3000`), PSS demuxed by the game
(`MpegDemuxPss` in RGDecomp). MVI files carry an XOR layer; DQ8 has a
converter (`tools/mvi/mvi_decrypt.py`) for the same container.

**FPU.** PCSX2 patches `0x00124898` to `3442fffe` (clamp to 0x7FFFFFFE
instead of 0x7FFFFFFF) for the out-of-bounds glitch in the Vedan/Myna area
([pcsx2#3544](https://github.com/PCSX2/pcsx2/issues/3544)). The runtime must
reproduce PS2 float saturation or apply the same patch.

**GS.** The PCSX2 GameDB carries only hardware-renderer fixes for this game,
which name the features it leans on: accurate blending (text/menu colour),
auto flush (lamp glow), round sprite (minimap/menu), preloaded frame data
(water), partial target invalidation (UI, subtitles), half-pixel offset
(bloom), native scaling (post-processing / DOF). No EE/VU game fixes.

**Timing.** Game logic runs at 30 fps; the PAL 60 fps pnach has to patch
gravity and timers too.

## Phase 2: one executable

- `python setup.py recompile` runs `check`, wraps the overlays as ELFs and
  runs ps2_recomp on all six images into clean `build/generated/<image>/`
  trees, refusing any skipped function, decode failure or unhandled
  instruction. For each overlay it also writes `overlay_info.h`, the MWo3
  header that tree was generated from.
- The top-level `CMakeLists.txt` builds the framework runtime only, and
  `src/CMakeLists.txt` one static library per generated tree (unity batches
  of 64, a shared PCH). The main tree's `register_functions.cpp` defines the
  runtime's own function table; each overlay's is compiled through
  `src/overlay/overlay_table.cpp.in`, which renames those globals and exposes
  the table as a `FunctionRegion`.
- `src/overlay/overlay_manager.cpp` replaces the `jr ra` stub at 0x00100400.
  On every load it reads the header at a0, refuses it unless it matches the
  header its tree was generated from field by field, and points the
  runtime's region resolver for the arena at that overlay's table (VIEWER and
  LAUNCHER, which hold no code, leave the arena without one). Arena calls
  with nothing resident fail loudly through the runtime's missing-function
  report.
- `src/main.cpp` loads the ELF from the extracted disc, sets the guest memory
  layout (below), installs the manager and runs.
- Build: clang 22 (LLVM MinGW) with Ninja; the generated code compiles in
  about 4 minutes on 16 threads into a 113 MB executable. The runtime's
  Release LTO is switched off: with clang on MinGW it drops the TLS
  initializer of an `inline thread_local` with a destructor and the link
  fails.

**Memory layout.** The game sizes its main memory stack (mgCMemory at
0x4A6FA0, one 24.7 MB memalign) to nearly all the heap, which on hardware
runs up to the main thread's stack at 0x01FD0000. The runtime caps the guest
heap at 0x01F00000, so that memalign failed, the game went on with a null
base, and its first clear wiped RAM from address 0 (the overlay path table
went with it). `main.cpp` now gives the game the heap up to 0x01F80000, keeps
the runtime's callback stacks between that and the main stack, and moves the
runtime's own small heap to 0x00080000..0x00100000 (EE kernel space, unused
here). If more than 20 callback stacks are ever needed they would grow into
the game heap; the executable reports it on exit.

**First boot.** CRT and all 233 constructors run; the IOP is rebooted and
the game's IRX modules load (CDVDSTM, the sk sound kit, PCMPLAY, ...); the CD
code finds all seven overlays; TITLE.BIN loads and the manager makes it
resident; the title code opens `cddat:menu/rc/menu_static_res_EN.pac` from
the DATA archives (via the HD6 index) and reads it with raw sector reads
(`sceCdRead` at the file's sector plus an offset, which the runtime's
pseudo-LBN mapping serves). Then nothing more is read and every frame is
black, at about 58 frames per second, with no error logged.

**Debugging aids.** `RG_TRACE=0xADDR,...` logs calls of main-ELF functions
(arguments, strings they point at, return value; `RG_TRACE_LIMIT`,
`RG_TRACE_PEEK=0xADDR,...` to print guest words at each call).
`DQ8_GFX_SCREENSHOT_DIR` + `DQ8_GFX_SCREENSHOT_EVERY=N` (framework) write
every Nth frame as PPM.

## Phase 3: to the title screen and into the game

The black screen after TITLE.BIN loaded was the main thread asleep in
`DelayThread`, waiting for a libkernl timer alarm that never fired. Three
fixes got the game from there to the title screen, NEW GAME and the opening:

- **No `[mmio]` table.** ps2_analyzer's MMIO table recorded only the page
  base, 0x10000000, for 137 instruction sites, and an `[mmio]` entry
  overrides the address ps2_recomp resolves itself. Every listed timer access
  went to Timer 0's count: `StartTimerSystemTime` read Timer 2's mode from
  there, got 0, and wrote back a mode with no clock source, so Timer 2 never
  reached its compare value. ps2_recomp needs no table (it routes statically
  resolved hardware addresses through the runtime, and READ/WRITE check at
  run time), so `recomp/main.toml` now has none.
- **Callbacks the main ELF points into overlays.** Three `return 1` menu
  callbacks in TITLE.BIN sit right after the `jr ra` of the function
  RGDecomp merged them into, and only the main ELF's widget tables point at
  them. Split out in `funcmap.toml`.
- **Vtable getters.** `rg_entry_002d94b0` was eight 8-byte getters a vtable
  points at one by one; `funcmap.toml` now supports `split = [...]`.
- `setup.py check` now also flags any code address taken as data (in any
  image's data, or built with lui/addiu) that lands after a `jr ra` and its
  padding inside a mapped function, the shape all of these had, across
  images (overlays are only matched against themselves and the main ELF).

**Result.** Memory card check, Japan Studio and Level-5 logos, title screen
(NEW GAME / CONTINUE / OPTIONS), NEW GAME, the "Story" text with its loading
bar, NORMAL.BIN loaded and made resident, then the opening in the desert of
Rosa and the town of Salgin, with subtitles. No error in the log.

**Test aids.** `RG_INPUT="SECONDS:BUTTON[+BUTTON]:MS,..."` presses pad
buttons on a schedule (unattended runs); `RG_WATCH=0xADDR,...` prints guest
words, I/O registers included, every `RG_WATCH_MS`. `setup.py recompile`
now rewrites only the generated files that changed, so a rebuild after a
map or config change takes seconds instead of minutes.

Still open from the first boot: `sceCdReadDvdDualInfo` (0x12ACD0) runs as
guest code and its S-command RPC (server 0x80000593, rpc 0x27) is unhandled,
so the game takes the disc for single-layer. That works with the extracted
tree, which holds both layers' files in one folder.

## Phase 4: sound and speed

**Sound.** The disc's sound drivers (LIBSD, SDRDRV, the Sound Kit and
PCMPLAY, the same set DQ8-Recompiled runs) now load onto the framework's
emulated IOP with an SPU2 (`ps2_native_iop::setModules` in `main.cpp`), so the
game's libsdr and sound RPCs run their real code. Music and sound effects
play (`PS2X_AUDIO_DUMP` confirms a clean music signal on the title screen).
Movie audio did not, until phase 5 (**Movie audio**, below): during the
opening movie the output was noise (a sign change on nearly every sample).
`RG_MUTE=1` silences the output.

**Speed.** In the first 3D scene (Salgin, the soldiers' cutscene) the game
ran at 2.7 frames per second. Profiling (`RG_PROFILE`, below) showed why, in
three steps:

1. The software GS rasterizer ran on the game thread: now it runs on a
   worker thread behind the framework's ordered queue (`GSThreadedBackend`).
2. That left the worker saturated by full-screen effect passes, which a
   per-pixel CPU rasterizer cannot keep up with. The GS now renders on the
   GPU with the SDL3 GPU backend from DQ8-Recompiled (vendored in
   `thirdparty/dq8-gfx/`, GPL-3.0), Vulkan on Windows, 2x internal
   resolution, in its own window: 2.7 -> 20 frames per second.
3. That left the game thread 85% inside the VU1 interpreter. VU1
   microprograms are now compiled to C++ ahead of time from a recording of
   the ones a run executes (`PS2_VU_PROGRAM_PROFILE=build/vu1-profile`,
   picked up by CMake): 20 -> about 60 presents per second. (Presents, not
   game frames: the game draws at most 30, and phase 5 below found that in
   heavy scenes it drew far fewer.)

The recorded microcode is game code, so it is not committed: a manifest,
`config/SCUS_974.90/vu1-programs.txt`, names where each program sits in the
game's files (48 footprints in the ELF and overlays, with a checksum and no
data), and `setup.py recompile` rebuilds them from the user's disc into
`build/vu1-manifest`. Programs it lacks run on the interpreter until a local
recording (`build/vu1-profile`) adds them.

**Field and battle.** With scripted input (`RG_INPUT`) the opening plays
through to Jaster's first fights in Salgin: the battle tutorial (START pages
through it), real-time combat against the Baphu with Steve and the Hooded
Man, the results screen (EXP, skills, items), then the story carries on into
the next fight and its tutorial. No error in the log, about 57 frames per
second in battle.

Bindings: `fflush`, `fseek` and `ftell` are no longer bound to the runtime.
Its versions only know handles from its own `fopen`, and this game opens its
files with its own newlib, so every call failed ("Invalid file handle").

`RG_GS=sw` returns to the software rasterizer, `RG_GS_SCALE=N` sets the GPU
internal resolution, `RG_GS_WORKER=0` puts the rasterizer back on the game
thread.

**Profiling.** `RG_PROFILE=out.txt` (with `RG_PROFILE_START`,
`RG_PROFILE_SECONDS`) samples every thread's instruction pointer about every
millisecond; `tools/profile/report.py out.txt build/game/src/roguegalaxy.exe`
prints where each thread spends its time, by function.

## Phase 5: a steady 30

Played by hand, the real-time cutscenes looked choppy: characters visibly
skipped animation frames. The game runs its logic at 30 frames per second
(GfxFlipBuffers waits two vblanks), but a recording of the window showed the
picture changing only 15 to 20 times a second in 3D scenes, and the time
went in four places.

**EE float conversion.** The runtime's `FPU_CVT_W_S` rounded to nearest;
CVT.W.S on the R5900 truncates and saturates. 563 recompiled functions use
it, among them the animation clock (`AnimKeyFindSpan`, the cvt.w.s at
0x0015600C, the defect RGDecomp's notes name as the character-animation
jerk). `src/ee_fpu.h` redefines it for the generated code, through its
precompiled header.

**Presenting.** The runtime's presenter samples the display at each host
vblank and waits for the GS worker to reach that sample; with the worker
busy those samples fell unevenly against the game's flips, so frames showed
twice or never. `src/flip_pacing.cpp` hooks GfxFlipBuffers and prepares the
presentation on the game's thread, in the GS stream right behind the flip's
copy; the window shows exactly those frames, one each (`RG_PRESENT=vblank`
for the old way). Mailbox replaces VSync as the present mode.

**The lens-flare probe.** GfxEndFrame reads an 8x8 block of Z every frame
(sceGsExecStoreImage, PSMZ32) for flare occlusion, and through the worker
that drained the whole queue: a third of a frame waiting. The SDL GPU
backend never reads depth back from the GPU, so it answered from local
memory anyway; `src/depth_reads.cpp` answers depth reads from that same
memory without the drain (`RG_GS_DEPTH_READS=queue` for the old way).

**Throughput.** With every frame presented, heavy scenes still ran at 15
to 25 frames per second: the GS worker and the game thread were both
saturated. On the GS side (changes marked "Rogue Galaxy" in
`thirdparty/dq8-gfx`): 4- and 8-bit texture uploads write runs instead of a
dispatched call per pixel, 4-bit textures expand through the resolved
palette (the general path re-read each texel's CLUT entry), and resolves
reuse one download buffer and staging texture instead of creating them each
time. `RG_SELFTEST=gs` checks the new writers and reader against GSMem.
On the game thread two thirds of the time was the VIF1 list (VU1 and its GS
packets), which the runtime runs inside sceDmaSend. The game starts each
frame's list at the end of GfxEndFrame and waits for it at the start of the
next, as the console runs VU1 beside the EE; `src/vif1_thread.cpp` now does
the same, on a thread of its own (`RG_VIF1_THREAD=0` for the old way). The
list's closing VIFcode interrupt still reaches the game's handler, which
times it, so the frame-time compensation sees real GS time. GS packets go
straight to the GS rather than through the framework's unlocked
GifArbiter, and a framework patch (`patches/ps2recomp/`) lets DMA started
through the registers wait for the lists in flight.

Over the scripted run to the first battle, frames per second for each ten
seconds went from dips of 12 to 24 in the cutscenes before the first battle
to 25 at worst, 28 to 30 elsewhere. Tools: `RG_GS_TIMING=1` times every GS
backend call on both sides of the worker queue; `RG_PROFILE_STACKS=1` adds
call stacks to `RG_PROFILE`, so samples in drivers or waits name their
caller.

At 144 Hz a 30 fps game shows each frame for 4 or 5 refreshes, which reads
as judder; 120 Hz (4 each) or variable refresh avoids it.

**Frame-time compensation.** Played by hand, the first battle then went
wrong: characters' facing flipped from frame to frame. GfxEndFrame times
each frame with EE timer 0, and from the VIF1 list's completion interrupt
(D_004810B4), and GameSceneTimeScaleUpdate scales the next frame's logic by
it, up to two steps. With the list on its own thread its real duration
became visible: in the battle about half the frames took three vblanks, and
the logic ran 1.5 to 2 times per frame, so turning overshot. Timer 0 follows
the host clock, so host slowness reads as slow frames, which an emulator's
guest clock never shows. `src/scene_timing.cpp` leaves the scene's step and
time scale at 1 after the update (slow motion, as an emulator gives, when
the host is slow); music still follows the measured time
(`RG_TIME_SCALE=game` restores the game's scaling). The flips themselves
turned out to have another cause, rounding (below), and came back by hand
with the clamp in place. With rounding fixed, the game's own scaling turns
cleanly in a scripted battle (4 single snaps in 556 updates of the leader,
none swinging back), but the first play with it lost Jaster in battle, so
the clamp stays the default until that is explained.

**Rounding.** The EE's FPU and VU0 round every result toward zero. The
host rounds to nearest, and code that rounds with the 1.5 * 2^23 trick
(add and subtract 12582912) then gets the next integer up. AngleWrap
(0x1CBF30) and VecWrapAngle (0x1CBE90) wrap an angle that way, so on the
host a small angle such as -0.23 came back as 6.05, a full turn added.
Turning code that takes the difference of two wrapped angles then turned
the leader the wrong way at full rate, and in battle, whenever the stick
was released, the leader swung 60 degrees each way around its target every
frame: 497 jumps of over a radian in 966 movement updates. Guest code now
runs toward zero, with denormals as zero, like PCSX2's default
(`patches/ps2recomp/0003`, `PS2X_EE_ROUNDING=nearest` to compare): 2 in
669, all angles within plus or minus pi. The mode belongs to the guest
fibers: the executor's own stack keeps the host's, but the host code that
guest code calls on a fiber (syscalls, stubs, the GS frontend) runs under
it too, and its float work tolerates that (exact divisions by 16, no
rounding to integers).

**Story-transition hangs.** After the Story loading bar reached 100% the
game could stop for good. One cause was the GS worker: it held every
prepared presentation until the end of its batch, each holding one of the
SDL backend's three slots, so a batch with four waited on itself
(`patches/ps2recomp/0002`; a forced worker stall reproduced it). Another,
in about one scripted run in ten, remains: the saved return address of
CfgVmRun (0x42FA20) at 0x01FFEF30 is overwritten by CfgVmFindOp and
DataTableFindByKey working through a stale VM pointer in the global data
table (gp-0x2078, its +4), left from an earlier DataTableRunCfg frame; the
main thread then returns into the stack and is put to sleep while the
loading thread keeps drawing the full bar. `RG_HWWATCH` (an x86 debug
register on a guest word) found the writers; what leaves the pointer stale
is not known yet.

**Hooks and safe points.** A hook that replaces a function's table entry
and does its work after calling the original was wrong whenever a safe
point inside the original unwound to the run loop: the original finished
later, after the hook's work. `src/return_hook.cpp` runs that work when
the call really returns, by taking over the return address's table entry
until then; flip pacing, the scene timing and the movie audio use it. A
hook that returns without calling the original has to leave `pc = ra`
itself, as `jr $ra` does: a call through dispatchGuestBranch forgives a pc
left at the entry, but a thread that a safe point unwound at that call is
resumed by the run loop at the entry, and a hook that leaves pc there is
dispatched again forever. The VIF1 hooks for sceDmaSend and sceDmaSync
did that, and the 60 fps mode's event cadence made the loading thread's
`Vif1DmaHandlerShutdown` hit it every time at 1x scale: the game froze at
the Story text with the executor spinning (`RG_PROFILE` with
`RG_PROFILE_STACKS=1` showed the game thread cycling through the hook,
`PS2X_KERNEL_TRACE_MS` the thread stuck at the hook's entry).

**Movie audio.** The opening movie's sound turned, part way through, into
full-scale noise or a garbled version of itself. Movie files (.MVI) are
program streams with every 2048-byte sector XORed by one keystream (seed
times 0x02E90EDD per word, the seed from a key string the game decodes),
which a sector's pack header gives away, so the disc's PCM could be
compared with what the game sent to the IOP (`RG_DUMP` on
SifDmaSendSync, 0x1D2C20) and with the output (`PS2X_AUDIO_DUMP`). The
game's PCM callback (0x1D4CA0) copies each packet, after its four-byte
sub-stream header, into a 48 KB ring that the read thread drains to
pcmplay's 24 KB on the IOP, and drops what does not fit: the runtime's
demux runs ahead of the IOP by wall-clock time, so whole packets went
missing, 4059, then 8132, 12191 bytes into the stream. The "SShd"/"SSbd"
stream is 16-bit stereo in 512-byte blocks per channel: an odd shift gave
noise, an even one swapped channels block by block. `src/movie_audio.cpp`
queues every packet and tops the ring up whenever the read thread has
sent some of it (`RG_MOVIE_AUDIO=game` for the game's callback); the
whole movie now matches the disc sample for sample, bar a click in its
first 0.2 s. The SPU2 plays it at full scale, as the game sets it (BVOL
0x7FFF, MVOL 0x3FFF on both cores).

**A VIF1-thread race.** The runtime's MSCAL/MSCNT callbacks read FBRST's
D/T enables from, and wrote VPU_STAT into, the context of whatever guest
thread the scheduler was running; from the VIF1 thread that context could be
an interrupt invocation being popped. `vif1_thread.cpp` installs its own,
which take the enables captured when the list was started and leave the stop
bits for the game thread. One device loss (`VK_ERROR_DEVICE_LOST`) at the
Story-to-cutscene transition remains unexplained (once in about fifty
runs); `src/debug/crash_report.cpp` now prints the exception and the call
stack of any crash into the log.

**Seeing through walls.** After some menus (the transporter's, and
something after the first battle) the canal water, characters and props
drew over walls: a glassy band across the screen by the canals, dark boxes
in mid-air. The header strip of the transporter menu is drawn with a
scissor of 0 to 512 on a 512-pixel buffer (FBW 8); the GPU backend sized
its surfaces by the scissor, so the depth buffer at page 0xE0 grew to 513
pixels and its page set took in page 0x150, one past its end. The game
uploads textures there for every object, and each upload made the backend
discard the depth buffer (7905 times by the frame examined), so whatever
came after drew on cleared depth. The surfaces now grow past FBW only for
geometry that reaches past it, and host scissors are clipped to the
surface (`thirdparty/dq8-gfx`, see its README). Found with a frame log
(`RG_GS_FRAMELOG`, every batch of one frame and GS memory beside it),
rasterizing it offline with the right depth, skipping batch ranges of that
frame (`RG_GS_FRAMESKIP`) and `DQ8_GFX_TRACE_TARGETS`; scripted runs
replay alike with `RG_INPUT_CLOCK=pad`.

**In-game menu.** DQ8-Recompiled's menu (`thirdparty/dq8-gfx/src/ui`, Dear
ImGui over the SDL GPU window) is in, on F1, the Guide button or Back+Start:
window and fullscreen, vsync, internal resolution changed live, aspect and
scaling filter, volume, controller and keyboard remapping, pause,
fast-forward, a frame-rate counter and screenshots, saved to `settings.ini`
under `%APPDATA%\RogueGalaxy\Recompiled`. Every hook it needs was already in
the vendored backend (identical headers), and the runtime had host pause and
speed, so the port is the copy with the name taken off, defaults for this
game (2x, mailbox, no line-blend change) and about sixty lines of host glue
in `src/graphics.cpp`. The counter counts the game's flips (the hook in
`src/flip_pacing.cpp` now installs even without flip pacing), not presents,
which go on while the game is paused. Widescreen and 60 fps are separate
work: the game shows no 16:9 option of its own (it has a hidden wide mode,
used since 2026-10-21, below); nemesis2000's PCSX2 patch (five words in the
ELF, verified) gives the rest, and no speed-correct 60 fps code exists for
the NTSC-U release.

**60 fps (`RG_FPS=60`, experimental).** The game paces itself with a vsync
interval the flip waits for (2 at boot, `DrawEnvSetVsyncInterval`; its own
menus switch to 1 with the scene's time scale locked) and scales the
scene's logic by the measured frame time: `GfxEndFrame` clamps the frame's
CPU/GS time to between one and two intervals, and `GameSceneTimeScaleUpdate`
divides it by two, never below 1.0, so at interval 1 the game would run
twice as fast (the PAL 60 fps patch rewrites those constants).
`src/scene_timing.cpp` writes interval 1 before each flip (2 while a movie
plays, between `MoviePlayerStart` and `MoviePlayerShutdown`; the game's own
`MovieIsStarted` flag stays set after a movie) and, after the update, a
scale of frame time over two with 0.5 as the floor, so the logic advances
at the same rate over however many frames the host manages
(`RG_TIME_SCALE=fixed` holds 0.5). The game's locked overlays are left to
themselves. It runs, at the right speed, and this machine then shows about
33 frames a second in Salgin's streets at 1x as at 2x: the GS worker is
the limit, not the GPU's resolution. Its first profile (`RG_PROFILE`) was
a quarter CPU-side texture writes (`GsVram::writeRunT4` 23%, the 4-bit
textures the game re-uploads every frame, plus 6% in 32-bit writes), a
quarter the NVIDIA driver, and the rest texture-cache and render-target
bookkeeping, including fence waits for render targets read back to the CPU
(`downloadScaled`); the game thread was idle 40% of the time, the VIF1
thread about 15%. Six changes since, all in `thirdparty/dq8-gfx` and the
framework (see the dq8-gfx README's list and `patches/README.md`): texture
writes in closed form with SSE kernels (PSMT4 uploads 40x faster; the
upload path went from 30% of the worker to 6%), host-to-local transfers
that flush the queued draws only on a real conflict, a whole-surface sprite
taking its surface over instead of importing aliased pages, texture reads
counting only the pages they sample, a passive depth attachment so a
clear and the object after it share a pass, the rounding mode read from
MXCSR instead of the CRT per command, and a 16 MiB worker queue. Every one
left the regression frame byte-identical. Salgin went from 33 to about 35
frames a second, at 2x as at 3x, and the worker is still saturated, now
almost entirely in draw submission (SubmitMany 65%): 900 draw calls, 220
render passes and 14 render-target readbacks a frame, each readback a
fence wait that drains the GPU. The CPU savings bought little because the
frame is bound by that serialisation of the GPU, not by CPU work beside
it. Reaching 60 means changing how the backend schedules the GPU: ending
the readbacks the game's VRAM reuse provokes (the character target at
page 0x150 shares pages with the bloom targets and with every object's
texture upload), and far fewer passes and command buffers per frame. The
VIF1 thread (85% busy, a sixth of it waiting on the worker before the
queue grew) and the game thread (75%) come next. For the VIF1 thread the
profile says, in order of gain per effort: batch primitives into one
SubmitMany per run instead of one queue command (with its lock, copies and
variant) per primitive (about 6%); finish an XGKICK in bulk once the
program has ended instead of one quadword per two interpreted cycles
(4-5%); UNPACK fast paths for the plain modes and one copy fewer per IMAGE
chunk (4%); and, bigger, having the VU1 compiler emit flag bookkeeping
only where a flag is read (5%). About 45% of its time is the compiled VU1
programs themselves.

**Readbacks (the first of those fronts).** The backend's stats now count
resolves by reason (`DQ8_GFX_STATS_EVERY`) and its target trace can be
switched on for a window of frames (`RG_GS_TRACE_BEFORE`). In Salgin's
streets the game provoked 7.8 readbacks a frame, 9 to 13 ms of fence
waits: three quarters of them host writes of 4-bit textures into pages a
live target owned in part (the uploads land in the character buffer's and
the bloom buffers' pages, which only CT32 writes could patch), the rest
T8 textures read from pages the bloom target still held, the padding row
of the bloom blur's feedback snapshot, and page marking that took the
page after each 16-colour palette. All in `thirdparty/dq8-gfx` (its
README lists them): exact page marking for a block base inside a page;
host writes in CT16, T8 and T4 patching a native CT32 target word by word
like CT32 ones, across payloads; texture reads consulting those words, and
leaving a target's pages that the draw reaches no texel of; the snapshot
padding copied from the owning target's texture (through the reinterpret
shader for a CT16 view). The readbacks are gone (0.01 ms a frame) and the
walk stays byte-identical to its references at 1x and at 3x. Under the
test harness (stats and census on) at the game's 30 fps the walk at 3x
went from 22.8 frames a second with the worker at 98% to a steady 29.9 at
75%, and at 1x the worker went from 74% to 70%. In the 60 fps mode at 3x
it changed nothing: 32 to 36 frames a second in the streets with the new
paths as without them (there the waits were only 2 ms a frame, and the
CPU patches the uploads now become cost about as much). What the worker
does now, from its profile: about 45% in the driver and the kernel, where
every patch application, page import, reinterpret and clear is a command
buffer submission of its own on top of the ten flushes a frame; the rest
in draw submission, the texture cache and the patches (every patch is
expanded scale-squared on the CPU and uploaded, 40,000 Kpx a minute at
3x). Next: patch uploads and page imports recorded into the flush's
command buffer instead of their own, patches uploaded at native size and
scaled on the GPU, then the pass count.

**Submissions (the second front).** Every texture upload, target patch,
page import, 8-bit expansion, reinterpret, local copy and clear was a
command buffer submission of its own, and so was each flush of the queued
draws, of which the game makes about 70 a frame (feedback snapshots,
transfers into pages the queued draws touch, aliased views), not the ten
the transfer counter showed: about 150 submissions a frame. The backend
now records all of it into one pending command buffer, in order, and
submits it once 96 draws are in it, or when a readback, the present or
the budget for resources SDL cycles per use asks (`thirdparty/dq8-gfx`,
its README: `SdlGpuDevice::pendingCommands`, `DQ8_GFX_SUBMIT_DRAWS`). The
field's frames go out in 4 to 5 submissions. Under the harness at the
game's 30 fps the worker went from 70% to 51% at 1x and from 75% to 59%
at 3x, with the logged frame byte-identical to its references at both
scales; in the 60 fps mode at 3x the streets went from 32-36 to 40-43
frames a second (38.4 to 46.4 over the six intervals before the logged
frame), the worker from 97% to 92%. Next on this front: the render pass
count (about 200 a frame, half of them the breaks around feedback
snapshots, which copy the whole 512x448 surface for draws that sample a
strip of it) and the patch uploads expanded scale-squared on the CPU.

**60 fps logic: what the PAL patch maps to.** Red-tv's PCSX2 patch for
the PAL release (SCES-54552, February 2026; the only 60 fps patch there
is, NTSC-U has none) was mapped onto our ELF by code shape
(`docs/sixty-fps-logic.md`, with the game's time-scale anatomy). Its
vsync byte and two `lui 0x3f00` are what our hook does
(`src/scene_timing.cpp`: the interval field is 0x491B54, the scale is
read only through `GameSceneGetTimeStep`, and the step count at
scene+0x5E04 we also write has no reader). The rest of his patch
compensates, through PAL-only globals, for systems that count frames
rather than the step, and those are wrong in our mode too: the play clock
and the per-second event triggers run at half speed (`PlayTimeStep`
multiplies by the vsync interval); the field minimap halves its step
itself when the interval is 1; NPCs walk and turn at twice the speed
(`NpcObjUpdate` takes no step); thrown objects and projectiles fly at
twice the speed with half the gravity (`TaskStepJudgeObject` passes a
constant 1.0); character gravity, players' and enemies' alike,
accelerates twice as fast (the velocity is scaled by the step, gravity
added per frame, the velocity unscaled: jumps reach half their height,
bosses' jumps too, which his per-boss code cave patched); and the scale
stays 0.5 inside movies (their modal loop never runs the update), so
audio fades there run at half speed. Covered already: base speed, the
title screen (its own overlay mode), FMV pacing and subtitles
(frame-counted), menu timers (overlay mode, scale locked).

**60 fps logic: the frame-counted systems hooked.** All of the above now
take the step (`RG_STEP_FIXES=0` for the previous behaviour). The
interval field itself stays 1 (the flip's wait, `GfxEndFrame`'s clamp,
the quit-to-title prompt and the insect cage want the wait's value); the
three readers that take it for the vblanks a logic frame spans
(`PlayTimeStep`, `MinimapUpdate`, `TextEntrySetupState`, told apart by
the return address of their `DrawEnvGetVsyncInterval` call) get 2 while
the scale is ours, so the clock ticks 2 x 0.5 a frame. `MinimapDrawEntry`
also calls it but overwrites the result before using it. `MoviePlayModal`
is hooked (it reads the step once on entry for its sound fades): scale
1.0 on entry, the movie flag for its whole duration, cleared when it
returns. `src/step_fixes.cpp`, hooks told apart by return address where
a function serves several callers: NPCs run every frame as before, with
the walk factor they all share (0x47D278) scaled by the step for the
duration of `TaskStepNpc` (hooked there, not at the `NpcObjTableStepMaybe`
it jumps to: the recompiler makes a `j` to a known function a direct host
call, past the table) and the two `AngleStepToward` rates
of `NpcObjUpdate` scaled at the call (so they move smoothly at 60
instead of stepping every other frame; the waits they count in frames
are now half as long, which nobody will notice; the 1.0 chase speed
inside 70 units of the player stays per frame); `HandleRegistryRefStepAll`
gets the step in f12 instead of 1.0; `GameSceneGetTimeStep` returns step
squared to the gravity site in `CharaMoveCollideMaybe` (0x234FD8) and
`GetThrowGravity` the same to the held-character site (0x234FC0), while
`GetScaledGravity` returns a whole frame's gravity to everyone (the
launch computations of throws, gun shots, cartridges and script attack
objects, whose integrators now take the step); and after
`CharaMoveCollideMaybe` returns the scaled vertical velocity is clamped
to 10 x step, the terminal velocity's share of the frame, which the
game's own clamp at 10 let reach twice (long falls). Verified in a
scripted run only (the hooks fire, the play clock advances 60 vblanks a
second of game time, no regression in the frame); jumps, throws, bosses'
leaps, NPC walks and the clock want a play-test.

**The freezes: the libc `rand` stub.** Three times in the first hour of
play in the 60 fps mode the picture froze with the music going on. A
watchdog (`src/debug/stall_watch.cpp`: no flip for `RG_STALL_SECONDS`,
15, while the window is active) logs every guest thread's state, the
running one's registers and the addresses it passes over a second: the
game thread was running, inside `ScriptVmExec` (0x173B00, the script
interpreter: 12-byte commands, a slot stack, opcode 21 calls a command
table, 23 yields the frame, 16-18 jump) and the AI commands it calls
(`AiCmdIsInBattleRadius`, `AiCmdRandInt`), the other threads waiting
normally. Reading the frozen process's memory (`tools/hang/dumpmem.ps1`
finds the guest RAM and the thread contexts; `tools/hang/aiscript.py`
decodes a VM's bytecode with the command names from the table at
0x4511E0) gave the loop: a party member's main AI routine, whose top is
reached again without a yield from a branch taken when `RandInt(100) <
10`, expecting the next pass to yield; the state it was in (linked to
itself, nothing to do) made every other branch skip to that one. It was
taken every time because the game's `rand` (0x133710) is bound to the
runtime's libc stub (`main.toml`), `std::rand() & 0x7FFF`, while the
game's own is newlib's 64-bit generator returning 31 bits, and every
caller divides by 2^31 (`RandFloat`, `RandRangeFloat`, `RandInt`): every
random number in the game had been 0 since the start of the port (damage
variance, particles, AI choices, loco selection). `src/libc_rand.cpp`
puts the game's generator back, on the seed in its reentrancy struct
(1, as on the console: the game only calls `srand` in a debug mode).
Nothing to do with the 60 fps hooks; the `RG_STEP_FIXES` toggles stay for
the play-test. The scripted canal walk boots and runs with the fix, but
its logged frame could not be compared: another game was running on the
machine at the time (GPU at 53%, 8 GB of its memory), the GS worker took
83% of wall instead of 40%, and the logged frames came out partial (HUD
and parts of the player missing, fewer draws per frame) with the fix on
or off and with the committed build too. To redo on an idle machine; the
references in `build/run` were also made with every random number 0, so
scenes with random effects may differ legitimately.

**A fourth freeze, from the fix itself.** The first session on the fix
froze again after an hour, differently: the watchdog showed the game
thread "running" at pc 0x133710 (`rand`) with ra inside
`ParticleSwirlUpdate` for 15 minutes, `v0` changing between reports and
nothing else. The scheduler's run loop dispatches a thread parked at a
safe point straight at its pc, and relies on the function leaving the pc
at its return address when it finishes (`jr ra` in generated code; the
generated stub wrappers write `ctx->pc = ra` before calling the stub). A
host function hooked into the table that returns without touching the pc
is dispatched again, forever, the moment a safe point lands on a call to
it; `src/libc_rand.cpp` did that, the other replacements in `src/`
(`movie_audio.cpp`, `overlay_manager.cpp`, `vif1_thread.cpp`) already set
the pc. Fixed by setting it. The watchdog now also prints every host
thread's stack (`src/debug/host_stacks.cpp`, module+offset, executable
frames as link-time addresses for `llvm-symbolizer`), since a guest
thread "running" with a pc that never moves is parked in host code. The
toolchain's `lldb` crashes on start here, so there is no attaching to a
frozen process from outside; the log has to carry everything.

**The party falling out of Salgin's chest house: the VU0 flags.** Four
times on 2026-10-18 the characters fell through the floor on the way to
a treasure chest inside a house in Salgin. `RG_POS_TRACE=1`
(`src/debug/pos_trace.cpp`: every `CharaSetPos`/`CharaSetPosXYZ`/
`CharaPlaceAt` with its caller, the frame the floor clamp first finds
nothing with the collision lists dumped to `build/run/fall_N.bin`, and
the sweeps of the next moves) showed nobody teleporting them: at
(-1235, 29.6, -877) the upper floor ends, a short ramp leads down to the
chest's level, and the lower floor was not in the collision list. The
list (`ActionMgrMoveAll`'s 256-record work buffer, filled once a move by
`MapEvCollectCollTrisNearMaybe`: event objects and treasure boxes first,
then every active map block whose bounds hold the character) was full,
256 of 256, with 60 of the 113 terrain triangles more than 100 units
away; the house block came last and lost its tail. Each block's model
culls by `AabbOverlap` (0x13E0E0), which, like `AabbContains` (0x13E090)
and `AabbContainsPoint` (0x13E130), is VU0 macro code: `ctc2 zero,
Status`, two `vsub.xyz`, `cfc2` of the status register, return
`(status & 0x80) == 0` (the sticky sign bit). The recompiler emits the
subtractions without any flag update, so the read is always zero and
every box test says yes: nothing is culled, the list overflows wherever
the active blocks hold more than 256 triangles, and whichever block is
late in the area's table loses its floor. The three are the only readers
of the VU0 control registers in the main ELF (the `sceVu0Clip*` readers
have no callers), and a scan of the seven overlay images (VIEWER, TITLE,
INSMENU, LAUNCHER, NORMAL, FACTORY, INSECT) for `cfc2` of registers
16-18 finds none, so the host versions cover every reader the game has. `src/vu0_flags.cpp` replaces them with host versions
with the same register side effects (`RG_VU0_FLAGS=0` keeps the
generated ones); the proper fix, status and MAC flags for macro-mode
FMAC instructions in the recompiler, is pending and would need a full
regeneration. Nothing to do with the 60 fps mode or the random numbers;
with every `AabbContainsPoint` true, `CollShapeContainsPoint` was also
always true, so trigger shapes may have fired early before this.

**Walking instead of running on diagonals: the stick's shape.** Reported
2026-10-18 with the chest fix, but older: the character drops from a run
to a walk depending on the direction the stick points. The game reads
each axis as `(byte - 128)`, ignores the first 50 of its 128 steps and
scales the rest to 0..1 (`PadGetAnalogLXNorm`, 0x183AF0), and the player
control (0x1FC4A0, constants at 0x47DC3C/0x47DC40) stands still under a
squared length of 0.0256, walks under 0.81 and runs from there, so a run
needs 0.9 of the stick. A DualShock 2 fills its square range and a full
diagonal reads near (1, 1); the Xbox 360 controller in use (and the
DualShock 4 and DualSense) is round, so a full diagonal is (0.7, 0.7),
which the game's deadzone turns into (0.5, 0.5), length 0.74: the party
ran only within about 20 degrees of each of the four directions. PCSX2's
"analog sensitivity" of 1.33 exists for the same reason. The pad bytes
come from dq8-gfx's SDL sampler through `src/graphics.cpp` (`pump`), so
`src/stick_response.cpp` reshapes them there: the direction and the
pushed distance stay, the distance is measured as the longer axis
(`RG_STICK_SQUARE=0` passes the round range through). Both sticks get it,
as on the console.

**In-game cutscenes at double speed (60 fps mode).** Reported 2026-10-18.
The event system counts frames: `SystemVmUpdate` (0x2509F0, from
`TaskStepEvent` and the battle sub-modes) resumes the system VM and runs
`EventSysUpdate` (0x26AC10: the event camera, the event player with its
cast, voices, sound fades and queued scripts; `EventPlayerUpdate` adds one
to its +0x468/+0x470 counters per call and the scripts' waits count
calls), and `TaskStepMessage` runs `MesMgrUpdateAll` (0x277AC0), whose
windows count their character and page waits in frames. At 60 updates a
second all of it ran twice as fast. `src/step_fixes.cpp` (`event`) now
runs both on the frames that complete a whole step (`rg::timing::
logicFrame`: the host's scales accumulated, capped at two; a button going
down at this frame's `PadUpdate` runs them at once, so no press is lost),
with the scene's scale at 1.0 for the call so the step takers inside (the
event camera, script commands) take a whole one. Two more things fell out
of the same reading. The field modes that lock the scale for a scripted
sequence (`TreasureBoxModeUpdate`, `CharaInteractModeUpdateMaybe`,
`TransporterWarpModeUpdate`) were left to the game's 1.0 while the flip
waited one vblank: chests, talks and warps ran at double speed too. The
lock now only protects the game's own 60 Hz overlays, told apart by the
interval the game itself last set (`DrawEnvSetVsyncInterval`, 0x144140:
1 from `SceneOverlayModeOn`, 2 from `Off` and at boot; a direct write by
an overlay's own copy shows at the flip) and, for a lock at interval 2,
by `GetGameMode` being a field mode (5-7, 9-13). And `PlayTimeStep` gets
its interval of 2 in those locked field modes now, so the clock no longer
runs double there. The frame-time trace (`RG_TIME_TRACE=1`) prints the
game's interval and the logic frames run, skipped and run early. The same
day's 40 s profile of real play (`build/run/profile-play.txt`, 60 fps
mode, 3x, Salgin): the VIF1 thread 90% busy (compiled VU1 programs 30%,
their flag bookkeeping 7%, XGKICK and GIF 7%, UNPACK 8%, one queue
command per primitive 5%), the GS worker 88% (driver 18%, draw
submission 12%, page imports 8%, CPU patches 7%, page and texture-cache
bookkeeping 12%, memcpy 6%), the game thread 54% (46% waiting for the
other two). The frame rate the player sees follows what is in view, 30
through a door to the street, 60 facing a wall, 40-45 in the streets:
both emulation threads need to lose about half their work per frame for
a steady 60 in the heavy views, so the next cuts go to both.

**Battle results at double speed, and the jump checked (60 fps mode).**
Reported 2026-10-19 morning. The battle results window is a task:
`TaskTableRun` (0x1F5340) calls the step of every enabled entry of the
table at 0x44ECF4 once a frame and ignores its return, and `TaskStepMenuWin`
(0x204350) jumps into NORMAL.BIN's `MenuBattleEndStep` (0x4EB6C0), which
counts frames: a wait counter at +0x102, widgets moved 0x14 a call,
`UiFadeWidgetAlpha` over 0x48 frames, and none of it reads the step. The
same holds for the other menu tasks that run in the field and in battle
(`TaskStepMenuBtlChg`, `BtlEsc`, `BattleMenuStep` with the HUD's party
panels, `Shop`, `Think`, `Help`, `EasyChange`, `CompleteCheck`,
`7thMystery`, the insect menus, `MenuWarp`, `Caption`; the NORMAL.BIN
steps call `GameSceneGetTimeStep` only in two Think and transporter
states). `LiveTalk` was in the list for a day: besides the party's remarks
it sequences the skill actions in step with the battle, and on logic
frames using a skill froze the game (2026-10-19, the screen still drawn,
the watchdog silent), so it runs every frame again. `src/step_fixes.cpp` (`menu`) now runs those steps
through the same `logicFrame` gate as the event system: on the frames
that complete a whole step, at once on a button press, with the scale at
1.0 for the call. The main, transporter and space menus run under the
game's own overlay mode (interval 1), where `logicFrame` leaves everything
as it is. `TaskStepStatus` (HP gauges and the screen markers that follow
the characters) stays at every frame on purpose. The player confirmed
the results window and the running jump the same morning.
The jump was reported as feeling fast, with a float at the top in the
original. Measured with the new `RG_JUMP_TRACE=1` (one line per
`CharaMoveCollideMaybe` of the party leader while off the ground;
`tools/bench/jumps.py` sums the arcs) on the scripted load, Circle at pad
read 2277 (Circle jumps in the field; Cross does nothing there): the jump
is a move motion (`CharaActMoveMotion`, 0x21B180, move ids 0xC8..0xCB
from `CharaActLoco`): a rise of 55 units driven by the motion with the
gravity flags set (bits 46 and 37 of chara+0x3D0), a hold at the top
under the same flags, then the fall under gravity (move id 1) until
`CharaHasLandedMaybe` (id 4). Its state counter at chara+0xAD0 advances by
the step the action manager passes in its context (+0x4, set from
`GameSceneGetTimeStep` at 0x232220 in `ActionMgrRunActions`), so it keeps
wall time. 30 fps mode:
rise 0.268 s (8 moves), hold 0.235 s (7), fall 0.50 s (15 moves,
-0.4356 a move), peak 55.00. 60 fps mode: rise 0.267 s (16 moves), hold
0.220 s (13), fall 0.50 s (30 moves, -0.1089 a move), peak 55.00. The
standing jump's arc, float and landing are the same to the frame. The
running jump was not: 192 units of ground against 101 in the 30 fps mode
(Circle while holding the stick; `RG_JUMP_TRACE=2` logs the moves on the
ground too). In the air CharaUpdateMovement takes another path: a loop of
whole frames (`for (i = 0; i < step; i += 1)`, 0x20B3A8..0x20B4A0) steers
and clamps the velocity, then sets the step register f23 to 1.0 (0x20B4AC)
and scales the velocity by it before the move, so the move covers one
frame's distance whatever the step (on the console a slow frame made the
jump go half as far a second; nobody noticed). The `CharaMoveCollideMaybe`
hook (`gravity`) now sees f23 == 1.0 with a step below 1 (f23 is
callee-saved and holds the step on the walking path), scales the velocity
by the step for the move and divides it back after, since the game's own
unscale divides by 1.0. With fixed half steps the running jump then lasts
24.5 logic frames against 25, falls 1.80 a move against 3.60, lands with
the same velocity, moves 1.80 a move against 3.60 in every phase, and
covers 77 units against 83 (the end points of the measurement differ by a
move and the landing is on rising ground).

**Throughput, round three (2026-10-19 afternoon): the GS worker.** A
profile of the player's own session (`RogueGalaxy-perfil.bat`, fifteen
minutes of fighting from the File 01 save, `build/run/play-perfil.prof`)
put the worker at 93% busy, the VIF1 thread waiting on its queue a fifth
of the time and the game thread idle half the time. Half of the worker
was the texture stream again: uploads into the pages the scene buffer
held a frame earlier (recording the patches 13%, applying them 7%, the
page imports that follow 4%), and three pieces of bookkeeping: the
texture cache's invalidation walking all 2048 entries for every upload
(7%), the patch finding a page's owners by a linear search per 8x8
cell, and `regionForPages` testing every cell of a target's page grid
for each batch drawn (4%). Done, in `thirdparty/dq8-gfx`: the texture
cache keeps its entries by page (`m_entriesByPage`), so an invalidation
visits only the entries over the written pages (1.3% of the worker
now); `regionForPages` and `markDrawn` walk the set bits
(`gsForEachPage`, reading the bitset's words directly: libc++'s bitset
shift and mask go through bit iterators and cost as much as the tests
they replaced, which the first profile showed as 5% in bitset
internals); `patchHostWrite` indexes its owners table by page. Then
the morning's experiment, releasing the pages an upload covers whole
instead of patching them (`releaseCoveredPages`), is on by default:
the 915 VRAM bytes it left different are the patched path's own. With
`DQ8_GFX_PATCH_CT32_ONLY=1` (a target read back before any non-CT32
write instead of patched) the VRAM is byte-identical to the released
path's, so the block patch of a T4/T8 write leaves byte 2 of some words
of a page's first block row stale (blocks 0, 2, 8 and 10 of pages
0x150-0x151, where a 128x128 CT32 upload is read as a 256x256 T8
texture); that is still to be found, and now only matters for a page a
write covers in part. The display buffer is identical in every
comparison. Numbers at 3x in the 60 fps mode: the street walk's worker
share of the wall 86% to 57-59% at the 60 cap (two runs); the long
walk's limited stretch from 55.0 (profiled) and 59.3 to 59.9 with the
worker at 61% instead of 85-90%; page refreshes 2800 to 1350 per 60
frames (40 to 16 Mpx). References remade as `build/run/ref1x30c` and
`ref3x30c` (the b ones differ only in those 915 bytes).
`DQ8_GFX_NO_COVERED_RELEASE=1` restores the patching. Play-tested the
same afternoon: the battles are much better by the player's feel, with
room left. The profile
launcher now also prints a `[gfx]` line a second into
`build/last-run.log`, so a drop in a session can be put to its minute.
A street profile of this build (`build/run/wl3p.prof`) has the three
threads within reach of each other at the 60 cap: the worker about 70%
busy (texture decode from local memory `GsTextureCache::expand` 6%,
`appendPrimitive` 4%, CT32 writes 3.5%, the CT32 patch 3%, the driver
17%), the VIF1 thread about 80% (the VU1 routines 40%, the image
continuation's copy into the worker's queue 5%, `fmacFlagsWanted`
rescanning images 2%), the game thread 70% (VU0 micro programs through
the interpreter 12%, `dispatchGuestBranch` plus `lookupFunction` 8%, the
VIF1 send 6%). Next, in that order of value for cost: a few-entry cache
for `fmacFlagsWanted`; handing the image continuation to the worker
without the copy; compiling the VU0 micro programs the way VU1's are;
a direct-mapped cache in front of `lookupFunction`; and the block patch's
stale byte 2 above, which `DQ8_GFX_CHECK_GPU_TEXTURES=1` should catch.
The first of those is done (patch 0006): the memory keeps a bit per pair
of VU1 code for the status readers as the VIF's MPG writes the code
(`PS2Memory::markVU1CodeWritten`), so `fmacFlagsWanted` looks at 32
words instead of rescanning the image after each code upload; it is
gone from the VIF1 profile, frames byte-identical. For the player's
sessions, F9 now writes a `[mark]` line to the log; for measuring
battles, `walk.sh` takes `MC0=` (another card: `build/run/mc0-battle`
holds the player's File 03 alone, two characters on a field where
walking meets a battle) and `FRAME=` (the pad read of the logged frame),
and `build/run/input_battle1.txt` loads and walks up and down.

**The battle bench, and the VU0 micro programs compiled (2026-10-19,
late afternoon).** With File 03's three characters the game thread is
the limiter, not the worker: in the field 97% busy at 50-55 frames a
second, in the battle 94% (the worker 70-85%), with the VU0 micro
programs interpreted on it (`VU1Interpreter::execUpper` and the pair
scheduler, 18%), `dispatchGuestBranch` plus `lookupFunction` (9%), the
VIF1 `send` (6%). The whole-program compile covers VU1 only (16 KB data
memory, `drained()`), but the framework's block compiler
(`compile_vu_blocks.py`, `PS2X_VU_AOT_IMAGES`) takes either unit and was
simply not wired in: a battle run with `PS2_VU_AOT_PROFILE` recorded 109
VU0 blocks (and 533 VU1 blocks the interpreter still ran, left out for
now), CMake compiles every `build/vu-blocks/*.txt` (one shard at a time:
the first build, with 32 shards two at a time and the rest of the build
in parallel, was killed for memory), and the interpreter is gone from
the game thread's profile (86% busy from 94%, `execUpper` off the list).
The link needed patch 0007: `ps2_vu_detail::regionCounters`, an inline
thread_local with a destructor, got a TLS initialization wrapper per
shard and lld on COFF refused the duplicates; it is `extern` now and
defined once. Frames byte-identical at 1x and 3x; the battle at 53-57
frames a second (62-67k draws a second) against 48-57 before, in runs
that vary. What remains on the game thread: the indirect-branch dispatch
(a direct-mapped cache in front of `lookupFunction`), the VIF1 send's
gather copy, `checkpointDue` (4%), the EE timers (2%). Two of those are
done since: the dispatcher looks the target up once (patch 0008; the
table slot is a direct index, so there was no cache to add, only the
second look in `lookupFunction` to drop: the dispatcher and the lookup
went from 10% of the game thread to 8%), and the VIF1 send walks the
chain's tags once, keeping the spans aside, instead of twice (once for
the size). Both byte-identical at 1x. The battle bench's number to
compare is now `tools/bench/battlefps.py`, the frame rate over the
intervals with 55k draws a second or more, since each run meets a
different fight: medians 57.4 before the VU0 blocks, 56.1 and 59.8 with
them, 59.3 and 57.4 with these two, in fights of different weight (a
profiled run of a heavy one sat at 41-47 with no thread above 77% busy:
the three wait on each other's queues, which is the next thing to look
at, together with the flip's wait when a frame runs late).

**Two freezes, and the tools for them (2026-10-19 evening).** Playing
File 03, the player froze twice in one evening. First, in a battle, on a
hit that poisoned and knocked the player down: the watchdog's report
listed every service thread waiting on its semaphore and the main
thread not at all, and a memory dump (`tools/hang/dumpmem.ps1`) found no
context for it either: it had ended. The scheduler has a silent path for
that: a thread whose pc reaches 0 outside a callback (a jump through a
null function pointer, or a return past the entry) goes dormant without
a word; patch 0009 now reports it with the registers and the last 64
dispatched pcs (`PS2Runtime::dispatchHistory`), and the report says so
when the main thread is gone or dormant. Second, on a return to the
title, the loading screen stuck at 99%: the main thread alive inside
`WaitSema(25)`, the vsync semaphore of `WaitVsyncCount`; the game's
vblank handler (`GfxVblankStartHandler`) still ran every frame (the
counter at gp-0x40AC kept advancing in the live process) but signals
that semaphore only while the "waiting" flag at gp-0x40B0 is set, and
the flag was 0 with the thread still blocked: the signal was given and
did not wake it. The semaphore's own state (count, waiters) is the
scheduler's and was not in the dump; the watchdog never fired because
the loading screen kept flipping. Tools since: F10 writes the watchdog's
report at once, the watchdog fires by itself when the main thread has
not moved for 15 s while the game flips, and `host_stacks` prints
link-time addresses again (the mapped PE header's ImageBase is the load
address; the file's is read instead). The next freeze of either kind
should name its cause.

**The freezes' cause (2026-10-19 night): a checkpoint unwind taken for a
return.** Five review agents (scheduler, hooks, VU compile, the day's
patches, forensics on the two dumps) ran while the player played; three
converged on one mechanism in `PS2Runtime::dispatchGuestBranch`. A
scheduler checkpoint at a `jal` makes the dispatcher set `pc` to the
callee's entry and return false, and every generated frame above it
returns too, up to the scheduler, which re-enters at that pc with the
guest's own stack intact (that is how time slices and interrupts work).
But the dispatcher also forgives a callee that comes back with `pc` still
at its entry (a host stub that never set pc) by continuing at the
fallthrough. When the function the innermost frame was about to call is
also the callee of an outer frame still in flight (indirect recursion:
the config script VM `CfgVmRun`/`CfgVmStep`/`CfgVmFindOp` during a map
load, the action dispatch `ActionMgrRunActions`/`CharaActDispatch`/
`CharaActBlownAway` on a knockdown, `ScriptCallWithArgs`/`ScriptListRun`
on a skill), that outer frame took the unwind for a return and resumed
its caller with the innermost frame's registers and stack pointer; the
next epilogue read a stale slot (0 in the battle: "ended at pc 0"; a
stack address while loading: the `[guest-branch:missing-target]` line
at 0x1fff500 that was in the log all along) and the main thread went
dormant. Both dumps' main-thread stacks show the duplicated callee and
the bogus frame, slot by slot. The VIF1 thread made it frequent: it
posts an event per finished list, each a checkpoint at the next call.
Patch 0010: the dispatcher forgives only with the stack pointer back
where the call left it (the unwind comes from a deeper frame; a stub
never leaves sp moved). A first version, a scheduler flag set by the
checkpoint and cleared on guest entry, went stale when the scheduler
resumed a thread suspended inside a stub (not a new entry): the stub's
return was taken for an unwind and the stub ran twice, a semaphore wait
twice over, and the 1x walk landed on a different frame each run
(13609, 13363 and 9613 batches) and once hung at the load. Also found:
the loading freeze's "main thread in WaitSema(25)" context was the
loading-screen thread's (the main thread's stack sits at the top of RAM
and `dumpmem.ps1` rejected its context: fixed; `tools/hang/stackwalk.py`
now walks a thread's frames in a dump from its sp and ra, through the
prologues in the recompiled sources, which is how the chains above were
read); the 30 s slow loading is
not explained yet (the report cannot tell a thread re-waiting every
vblank from one never woken: the next step is to print the vsync
counter and the waiter's count next to such a thread). Smaller findings,
to do: the covered-page release lets whole pages go at the first payload
of a split upload, before their bytes land (release only pages whose
rows have arrived); `flag_liveness` treats an FMAC upper with an empty
dest mask as publishing flags (none of the recorded routines crosses
one, but a zero word is what padding decodes to); the EE timer batch
should settle before the idle wait and drain late interrupts before the
"cued" test; `gatherChain`'s reserve belongs inside its try; a VU0 micro
program that runs out of its 4096-cycle budget is truncated silently;
the callback stack pool (20 x 16 KB) overflowed into the game heap once
(`runtime callback stacks grew into the game heap`), so it gets 32; the
scheduler drops a pending interrupt invocation's checkpoint flag in
`processPendingEvents` (bounded latency, not a loss); the missing-target
report prints once per process. Done the same night as patch 0011 and
in `thirdparty/dq8-gfx` / `src`: all of the above except the callback
pool, which cannot grow downwards (the heap end at 0x01F50000 made the
game's big memalign fail again: a black boot at 0.6 fps with every file
path empty, caught by the walk), and the interrupt-invocation checkpoint
flag, left as is. The watchdog's main-thread trigger fired on every
intro movie (the main loop sits in WaitVsyncCount each frame) and was
taught the vsync rule: stuck only once the counter has passed the
thread's count + n. One bench run of the fixed build hung silently at the
title-to-load transition (42 s in, no line after; the bench window is
never active, so the watchdog does not report there); four more runs,
two with the watchdog off, did not reproduce it. Frames byte-identical
at 1x and 3x; the street and the battle bench at the 60 cap. Later that
night the 1x walk diverged in three runs out of four with the same exe
that had matched, which first looked like the flag version of patch 0010
(it was wrong too, see above) and then was not: the disc's data had left
the Windows file cache, the pad is read during the load screen, and a
load that takes longer on the clock starts the scripted walk at another
point of the area's arrival. `walk.sh` now reads the data files first
(4 s when cold); with that, 1x and 3x match byte for byte again.

**Throughput, round four (2026-10-20 morning): the boss gate, and the
GIF stage.** The user's F9 at a boss gate (File 03, beside its save
point; 34-44 frames a second against 60 everywhere before it) opened a
20-second profile (`RogueGalaxy-perfil.bat` now samples at each F9, into
`build/run/play-perfil-N.prof`). The stats line, which now carries the
VIF1 thread's busy share, put that thread at 94% of the wall with the
draws per frame unchanged; the game thread spent half its time waiting
for it, and the doubled `ee-dispatches` a frame were its polling, not new
game logic. Inside the VIF1 thread: compiled VU1 programs 56% (one
routine, `r_a786efabfa469ada_44`, 18%; `_52` in the builds since the
recorded programs were compiled in), queueing each primitive for the GS
worker 17% (42,000 a frame, 312 bytes each), the GS front end 9%, texture
uploads into the worker's queue 6%, and a wait for the worker at every
FINISH. A microbenchmark of the worker queue alone (one producer, a
spinning stub backend) found the per-primitive cost in memory traffic,
not in the copies or the locks: a chunked queue that sends a state only
when it changes saved 8% there with the producer's caches polluted, and
was left aside. What moved instead is the GS front end: the VIF1 thread
now copies the packets it hands the GS (XGKICK, DIRECT, image
continuations) into a GIF stage, a thread of its own that runs them in
order (`src/vif1_thread.cpp`, `RG_GIF_THREAD=0` for the old way). Waiting
for the lists in flight waits for the stage too, so the game's own GS
work and the frame log's cut see what they did before; the front end
calls nothing back on the game's side, so only the GS state lock is
shared. Each packet carries the VIF1 thread's MXCSR: VU1 programs select
rounding toward zero, the front end's Q/S/T arithmetic ran under it, and
without it the logged frames differed in the last bit of those values.
Frames byte-identical at 1x and 3x (both the batch logs and VRAM). A gate
bench came with it (`GATE=1 tools/bench/walk.sh NAME 3 60`: a copy of the
user's File 03 of 2026-10-19 21:16 in `build/run/mc0-gate`, loaded and
left standing; `tools/bench/gatefps.py`). The scene there varies between
runs of the 60 fps mode (one build gave 39 to 54 frames a second;
pinning the process to one logical processor a core, `AFFINITY=5555`,
did not change that), so the steadier number is the VIF1 thread's
milliseconds a frame: 23-28 before, 16.5-22.5 with the stage (7-9 ms on
the stage), the frame rate 33-41 before and 39-54 after (means 37.5 and
45). The battle bench sits at the cap (median 59.9, it was 53-57) and the
street walk at 60. What limits the gate now is VU1 itself: three quarters
of the VIF1 thread are the compiled programs, `r_a786efabfa469ada_44` a
sixth of it, `commitFlags` and `stall` 6%.

**The loading screen at the console's pace, and a recap at once
(2026-10-20 night).** The story so far that a loaded game shows, its lines
fading in one by one before Press X, ran at twice its speed in the 60 fps
mode. A thread of its own draws it (LoadingScreenThread, 0x343510:
LoadingScreenFrame, then GfxEndFrame's flip) while GameSessionInit loads,
between LoadingScreenStart and LoadingScreenStop, and each line's alpha
gains a constant a frame (0x3445B4..0x344628, at 1.0 the next line
starts); every flip there waited one vblank. `src/loading_screen.cpp`
flags the screen and the flip waits two while it is up, as for the
movies (the `[timing]` trace shows the interval 2 there, 1 after). The
menu has a new Interface > Game section for the game's own switches
(dq8-gfx's HostServices::gameOptions, kept under [game] in settings.ini):
"Show the loading recap at once" (`RG_INSTANT_RECAP=1` too) sets every
line's alpha and the shown count before each frame, so the whole recap
and Press X come up with the screen. Only X or START ends it, as before:
in state 5 LoadingScreenFrame calls LoadingScreenTryFinish on a press,
which finishes once every line is shown (a spare X in the bench's load
script ended the instant screen at once; RG_TRACE on 0x3430C0 showed the
press path, 0x343724). Frames byte-identical at 1x.

**Widescreen, and the opening skipped (2026-10-21).** The game keeps a
wide mode nothing in this release turns on: an aspect mode at 0x481638
that the system config's "TVMODE WIDE" sets to 1 (SysCfgTvModeCmd) and two
readers, GetAspectMode and IsWidescreen. In it SceneCameraApplyAspect gives
the scene camera 9/16 for 3/4, and the HUD drawn over the scene (HP gauges,
target slots, minimap entries, the attack chain, the sprite helpers
GsDrawSpriteCentered/AnchorBR/AnchorBL and DrawRotatedQuad) narrows its
sprites by 3/4. `src/widescreen.cpp` has both readers answer 1 and adds
what nemesis2000's PCSX2 patch does besides 0x1C6A50 (the scene camera's
3/4, which the mode replaces): the compass's ViewSetPerspective (0x37112C)
takes 9/16, and so do the insect menus' model previews
(DrawModelWithCamera, 0x28C070, not in the patch; untested); a movie made for 16:9, 512x336 between bars,
is stretched over the 448 lines (MovieDrawFrame's two GsPktSetXYZPixel
corners), and one made for 4:3 (512x448, as the title overlay's are)
narrows to 384 columns with bars at its sides; an event's start leaves the letterbox
open (LetterboxSetFade from 0x2611F8 gets 0). The backend's game
wide-screen query, which DQ8 used for its Screen Size option, makes
Aspect ratio Auto 16:9. The screens before the game stay 4:3 and the game
draws for 4:3 there (the title overlay resident, or the loading screen
up): the title's 2D menu and copyright would otherwise run wider than its
movie. The menu's Interface > Game > "Widescreen (16:9)",
`RG_WIDESCREEN=1`. The town bench holds its frame rate (56.4 against
56.2 at 4:3), the GS worker and VIF1 6 points busier for the wider view.
Seen in the town and the residential gate's event:
proportions right, the compass round, no bars; NPC names, subtitles and
menus are 2D drawn for 4:3 and come out stretched. The opening
(TitleOpeningMovieTick, the title overlay's 0x4EBAF0) fades out on a
button once it has run six frames, but only once it has been seen
(0x481B90): "Skip the opening" (`RG_SKIP_OPENING=1`) marks it seen and
answers the tick's first PadIsTriggered with a press, so the title comes
up some 5 s after boot instead of 20. The battle bench no longer meets
its battle: with or without these, three runs walked up into the
residential gate's event.

**The picture's edges cropped (2026-10-21).** The user saw a stripe along
the right and bottom edges. The game leaves a border of its frame
undrawn: at 1x the first and last columns are black, the top row half
bright, the last row black and the one above it half bright (the title,
a 2D screen, has the right column and the bottom rows only). At 3x its
half-pixel effects move that border by a texel and leave a sliver of
picture past the black. Neither came from the presenter: the composed
slot ends in black and the blits read inside it. On a television the
overscan hid it. The presenter now shows the slot less `crop` GS pixels
an edge, and half a pixel more at a raised scale
(`SdlGpuDisplayOptions::crop`, applied before the Sharp filter's
nearest pass so the bilinear one reads nothing outside); `src/graphics.cpp`
sets 2 (`RG_CROP`). In the town at 3x and 16:9 the picture now runs clean
to all four edges.

**The game's own Settings screen, a free camera, quick scenes
(2026-10-25).** The user asked for the menu's options inside the game,
the developers' free camera, the action gauge's speed, a faster camera
turn (at 60 fps it turned twice as fast and they liked it) and scenes
that only make you wait played faster.
- System > Settings gained three pages, turned with L1 and R1
  (`src/settings_pages.cpp`): the switches, the numbers (Low a step off,
  High one more) and the quick scenes, every change kept in settings.ini
  through dq8-gfx's new `Overlay::setGameOptionValue`/`setGameNumberValue`
  (written at once: the menu, closed, draws no frame to save in). A RAM
  dump of the screen (the old build, scripted into it, dumped with
  `tools/hang/dumpmem.ps1`) gave its layout: ten names in one text widget,
  each choice a widget of two sprites, a button picture shared by all and
  a word cut from one picture (On at 72,38, Low at 60,56, ...), and a
  cursor grid with its last row. A page writes those fields and nothing of
  the guest's runs for it. The first try changed a texture id found before
  the sprites and nothing changed; the words are the second sprite.
- The free camera (`src/free_camera.cpp`, F6 or the menu, not kept): the
  scene camera's override written 13 before its update, pad 1 read as idle
  by the game and pad 2 copied over it for the camera's update alone,
  Start leaving it, a notice in the menu's corner when it turns on or off.
  Two traps on the way: a call into the guest from a
  hook comes back early at the first virtual call (the generated code
  leaves every indirect call to the run loop), so CameraSetModeOverride
  could not be called; and CameraModeDebugFreeUpdate reaches
  CameraDebugFreeUpdate with a `j`, which the generated code makes a direct
  call past the table, so a hook on the latter never ran. In the walk the
  camera flew off over the town while the party stood on the transporter;
  Start brought it back and the party walked on, no pause.
- Quick scenes (`src/quick_scenes.cpp`): the host's pace at up to three
  times while the game's mode says a chest or a transporter's warp script
  is on screen, or while items go into a Revelation Flow cell (the board's
  placing step, read where RevelationBoardUpdate takes the board, the step
  that waits for the player left out). Mode 11 also runs the doors' and
  lifts' scripts (0x226) and story scripts, so only the transporter's
  (0x320, 0x321) count. A Revelation
  Flow placement (scripted with the developers' giver) went from 1.03 s to
  0.35 s. A battle's start (phase 101) and end (phases 500 to 1001) were
  quick scenes too for a while (battle bench: 1.50 s to 0.76 s, 4.98 s to
  2.59 s), and so were the map changes (mode 5's fades, the doors'
  scripts); the user did not like them and they were taken out.
  `RG_SCENE_TRACE=1` prints the mode, phase, main menu state and placing
  step changes with times. The Factory runs in its own overlay and is not
  reached by the test saves; the weapon combine reads the cursor during
  its animation, so it is left alone.
- "Action gauge speed" scales CharaActionGaugeRegen's step (the battle
  bench at 300%: the step 1.73 against 0.58). The camera's turn by the
  right stick (CameraObjOrbitControl's input in the field, the battle
  cameras' reads of the stick) is added a frame's worth each frame, so the
  60 fps mode multiplies it by the frame's share of a step (step fix
  `camera`), and
  "Faster camera rotation" doubles it at either rate: the right stick for
  0.4 s turned the camera about 90 degrees at 30 fps, about 180 with the
  switch, and about 90 at 60 fps. The user first had a percentage, then
  asked for the console's speed at 60 and a "faster" switch instead. The
  F1 menu says in red when 60 frames per second waits for a restart
  (`GameOption::restart`). The Revelation Flow's giver stays at one, as
  the user preferred.
- The doors' and exits' orange triangles pulsed at twice the speed at 60
  fps (the user): DoorCursorFadePulseUpdate steps them a call at a time,
  so it runs on the logic frames (`menu`). Not their task: Step Object2
  shows every door's cursor each frame however far, and the task fades
  those out of reach to rest within the frame, which a skipped task left
  drawn every other frame.
- A review of the day's change found, and it was fixed: the following
  battle camera's tilt eases toward the stick's push (15 degrees at most,
  whatever the rate), so it is no longer scaled; the first-person view
  (R3) turns by both sticks a frame's worth each frame and now takes the
  step too; the free camera left its 13 on a camera the game stopped using
  (the first-person view's, when a hit in battle ends it), where nothing
  cleared it, and it now gives it back, and its sticks take the frame's
  share instead of a fixed half; F6 cannot be bound to a pad input; the
  Settings screen's pages are there with the F1 menu alone.

With every option at its default the 30 fps walk's frame is byte-identical
to the one of 2026-10-23 (`mods30c`). `RG_INPUT` takes the right stick
too (`RUP` ...).

**The window as it was left; item drops and shop prices (2026-10-26).**
- The user found the window opening at the same size every time. dq8-gfx
  kept a plain window's size alone, from its resize events: a maximized
  window kept nothing, and the place was never kept. The settings now
  hold the place (`x`, `y`) and `maximized` too, and the window is
  created with them (`SdlGpuWindowPlacement`, through
  SDL_CreateWindowWithProperties); a place whose top edge is on no
  display opens centred, and a hidden window (the benches') remembers
  nothing. A scripted run moved the window, maximized it and closed it;
  the next opened maximized, and restored, it went back to the moved
  place.
- "Item drops" (`src/battle_rates.h`): an enemy's item drops when 100
  times a RandFloat (the call returning to 0x3A2990) is under its chance,
  and the draw is divided by the percentage, 0% to 1000%. The battle
  bench's enemies have chances of 2 to 10%: at 100% none of 22 draws
  dropped, at 500% 11 of 40 did.
- "Shop prices" (`src/shop_prices.h`): ItemGetBuyPrice (0x3D4340) is
  behind both the price a shop shows and charges (ShopRowGetPrice, with
  the members' cards' 0.8 or 0.9) and the one before the card, so it is
  scaled there, 10% to 200%, held to the game's 32767. Below 100% an item
  never sells (ItemGetSellPrice, 0x3D4320) for more than it costs with
  the best card. No test save is beside a shop; the user tried it in
  one, and "Item drops" in their battles, and both worked.
- The quick scenes went from three times the pace to twice: a chest at
  three was too fast for the user, who had asked for the pace the 60 fps
  mode once played them at.
- The user asked what could replace the black bars beside the 4:3
  screens (the title overlay's, the 4:3 movies, or the whole game with
  Widescreen off on a wide window), as ports of older games do. Display >
  Sides: black, the picture itself blurred and darkened, or an image of
  the player's from `borders\` (the first PNG, JPG, BMP or TGA by name,
  read with the runtime's stb_image each time that choice is made; the
  folder's images are ignored by git). The presenter draws the side first
  with `present_sides.frag`, the source scaled to cover the window, and
  the picture over it. A visible scripted run at the title (1280x720,
  Widescreen on) captured all three.
- The user asked whether the map could show the chests. An agent read
  RGDecomp and the generated code: MinimapDrawEntry (0x325720), behind the
  corner map, the full map and the transporter menu's, already has a pass
  that draws the zones' chest records (an 8x8 icon; opened ones, by the
  bitset StateFlag2Test reads at the record's +8, left out), gated on
  SavePointAreaCompleteTest (0x3EC7A0, its only caller the jal at
  0x3267A8): the planet's flag, 460 to 466, set once every save point there
  is activated. "Chests on the map" answers yes for that call alone and
  writes nothing, so the save and the planet's own completion (the whole
  map revealed) are untouched; cells not explored still hide them. In the
  30 fps walk three chest icons came out on Salgin's corner map, the frame
  otherwise identical (212 bytes of the display differ); off, it is
  byte-identical to `mods30c`.
- The user found the chests too quick still at twice the pace, and of
  little worth: "Fast chests" was taken out, as the battles' starts and
  ends were. The transporters and the Revelation Flow stay.
- Sides is on the game's Settings screen too, on the numbers' page under
  Draw Distance ("Sides: Black"), Low and High going through Black,
  Blurred and Image; a change goes to the F1 menu (`Overlay::setSides`,
  kept in settings.ini), and the menu's own changes come back to the page
  (`noteNumber`, from `pump()`). In a hidden walk High twice on the row
  read "Sides: Image" and wrote `sides = image`; the next start's page
  read the same. The walks' scripts reach the screen with the cursor on
  row 8, where the save's cursor memory leaves it, not row 0.
- The user found Interface crowded: the menu's own settings and the
  game's twenty options in one list. The game's went to a page of their
  own, Game (between Controls and Interface; `SettingsPage::Game`, the
  script's `settings game`), in four sections the host names
  (`HostServices::gameSections`, each a title before an option's key):
  Camera and view, Convenience, Battles and rewards, Developers.
  Interface keeps the menu, the pause and speed, and the frame counter.
  The settings file is unchanged.
- The user found the blurred sides too blurred and too dark to make out,
  and the name "Sides" odd. The menus call it Borders now, the name of the
  images' folder (still `sides` in the file and the script). The blurred
  copy is drawn at 0.75 of its brightness instead of half, and stops at
  256 texels wide instead of 64: 128 was tried first (a simulation from a
  window capture of Salgin put 64, 128 and 256 side by side), and was
  still too blurred for the user.

At their defaults the 30 fps walk's frame is still byte-identical to
`mods30c`.

**Toward a 0.1.0: a build from a fresh clone (2026-10-26, night).** The
user asked how a first pre-release would go and chose the source with an
installer that builds the game on the player's machine from their own
disc (nothing of the game distributed). A clone of the pushed repository
(d59ebcf), built by the README's steps, found:
- A deep folder breaks the toml11 download (one of its test submodules
  passes Windows' 260-character path limit): the installer needs a short
  path or long paths on. Built again in `F:\rgclean`.
- The README built the recompiler before anything applied
  patches/ps2recomp (the game's configure did), so patch 0017 was missing
  from it: three generated functions (the VU0 box tests) came out without
  their flags. The patch loop moved to `cmake/framework_patches.cmake`,
  which the configure includes and `python setup.py tools` runs on its own
  (`cmake -P`) before it builds the recompiler.
- A fresh game build stopped at once: the runtime's FFmpeg download goes
  to `ThirdParty/ffmpeg-prefix` under the build folder, which already had a
  `thirdparty/` (the framework's and dq8-gfx's binary folders); CMake wrote
  the download's outputs with one case and the link inputs with the other,
  and Ninja found no rule for the libraries. An existing build had them
  from before. The two subprojects build in `ps2recomp/` and `dq8-gfx/` now.
- `setup.py` has `tools`, `build` and `install` (verify, extract unless
  `disc/` holds the game, tools, recompile, build). In the fresh clone,
  `install` from the ISO took 11 minutes after the extraction; the
  generated code is identical to this tree's, and its walk's frame is
  byte-identical to `mods30c`.
- Not reproducible from the repository: the VU0 blocks of
  `build/vu-blocks` (a recording, microcode included, never committed),
  so a fresh install interprets the VU0 micro programs (the battle's game
  thread was 94% busy that way against 86%). Needs a manifest like VU1's.
- The version is 0.1.0 (`project(VERSION)`, `RG_APP_VERSION`): the window
  title and the menu say it, the About window adds the commit. The
  commit was always "-dirty" (`git describe --dirty` counts the
  framework's checkout, which configure patches); the changed files are
  read with `git status --ignore-submodules=dirty` instead.
- The user chose the installer that downloads its tools into the folder
  (no Windows install, no administrator). `Install.bat` runs
  `tools/install/install.ps1` (Windows PowerShell 5.1): it refuses a folder
  whose path is over 60 characters (the build's deepest path is 185 below
  it), has spaces, accents or symbols, or is in OneDrive or on a share, and
  offers a copy at `<drive>:\RogueGalaxy`; checks the free space; asks for
  the image (or takes it dropped onto the batch file) and checks its size;
  downloads MinGit 2.56.0.2, Python 3.12.13 (python-build-standalone),
  CMake 3.31.12, Ninja 1.13.2 and llvm-mingw 20260616 into `toolchain\`,
  the versions this tree builds with, each against a SHA-256 read from
  GitHub's asset digest and, where the project publishes one, its own list;
  then runs `setup.py install` with only those tools and Windows on PATH,
  its output also in `install.log`, and offers a desktop shortcut. A run
  again skips what is done. The downloads themselves are for the user to
  try (I do not fetch executables); the rest ran end to end with the local
  tools (`-UseToolsOnPath`, PATH cut to them and Windows): the archive
  unpacked at a 66-character path, copied itself to `F:\RogueGalaxy` and
  went from the ISO to the game in 11 minutes, extraction included; its
  walk `inst1` is byte-identical to `mods30c`.
- `setup.py install` stopped ignoring a failed recompile, and redoes an
  interrupted extraction over itself (`extract` refused a non-empty
  `disc/`; the memory cards in it are not the disc's files and stay). Into
  a pipe the extraction prints a line every 5%: the line rewritten in place
  reached the installer only at the end.
- GitHub's "Source code" archives leave the submodules out (PS2Recomp,
  imgui), so the release carries `RogueGalaxy-<version>.zip` from
  `tools/release/source_zip.py`: HEAD (or the working tree with
  `--worktree`), each submodule at its recorded commit, and `REVISION`,
  which the About window reads when there is no git checkout. 7.4 MiB.
- A review of these changes (the user asked for one) found, all fixed and
  tried: the archive took the framework with this machine's line endings
  (`core.autocrlf=true` made it CRLF, having no .gitattributes), and
  outside a repository `git apply` with autocrlf=false rejected patch
  0001, so a MinGit that does not convert would have stopped the install
  at the patches; it is archived with autocrlf=false now (LF, and all 18
  patches apply under true, false and input). A failed configure leaves
  CMakeCache.txt, which `setup.py` took for "configured", so a run again
  never configured: it looks for build.ninja. `extract` wrote the manifest
  even with problems (a large file's hash mismatch, which the quick
  verify does not hash), and the next install took the disc as extracted:
  the manifest is removed first and written only when nothing is wrong.
  The patch stamp survived a framework reset and skipped every patch: when
  the first stamped patch applies again, all are applied again (the last
  one cannot tell, it applies only over the others). Smaller: a missing
  tool ends in "failed: STEP: cannot run" instead of a traceback, an
  `--iso` that is not used says so, `-UseToolsOnPath` checks Python 3.11,
  and the unused `RG_VERSION` define went. Left: the revision rule is in
  both CMake and `source_zip.py`, and the FFmpeg case fix renames our
  binary folders instead of patching the framework's `ThirdParty` name.
  The main tree's walk `def11` is byte-identical to `mods30c`.
- The user's first install with the real downloads (from Downloads, 54
  characters, so in place) downloaded and checked MinGit, then stopped:
  Windows refused to rename the folder it was unpacked into ("access
  denied"). Not reproduced afterwards, by me or there: an antivirus looking
  at programs just written holds them, Windows renames no folder with a
  file open inside, and a scanner does not look at the same files twice.
  Tools are unpacked in place now (`tar --strip-components` when the whole
  archive is in one folder, read from its listing), and deleting or
  renaming a download tries again for half a minute (`Invoke-Patiently`),
  which a test with a file held by another process passed (3 s: waits and
  goes on; 40 s: says why and stops, or goes on when the step can wait).
  The real MinGit archive unpacks in place to `cmd\git.exe`. Run again,
  the user's install built and ran (11 minutes from the tools to the game).
- Before the repository goes public, the user chose to keep their email
  out of it: every commit's author and committer is GitHub's no-reply
  address now (`git filter-branch --env-filter`, the trees unchanged), so
  every commit id before this changed (b198448 became d59ebcf; memory notes
  and old messages quote the old ones). `RogueGalaxy.bat` moved to `debug\`,
  so a player's folder shows `Install.bat` and `Rogue Galaxy.exe` only; the
  other debug launchers call it beside them.
- The VU0 blocks have a manifest now, as VU1's programs do:
  `setup.py vu-blocks-manifest` writes `config/SCUS_974.90/vu-blocks.txt`
  from `build/vu-blocks` (each block as its longest pieces found in the
  ELF and the overlays, `file:offset:pairs`, and one FNV-64 over all), and
  `recompile` rebuilds `build/vu-blocks-manifest/blocks.txt` from the
  player's disc, which CMake compiles with the local recordings. All 109
  recorded blocks are in the ELF (107 whole, 2 in two pieces); the
  expansion is the recording's set exactly. A fresh install built so ran
  the battle bench with the interpreter off the game thread
  (`execUpper`, `run` and the pair scheduler 16.7% of it before, the
  compiled blocks now; the thread 74% inside the executable against 85%;
  the lowest heavy interval 52.6 fps against 46.2), and its 30 fps walk
  `inst2` is byte-identical to `mods30c`. The 32 shards took 19 minutes
  one at a time (2.2 GB and about a minute each; the generated game code
  0.1-0.2 GB a unit): CMake now runs one per 7.5 GB of memory, up to four,
  and a fresh install's whole game build took 14.9 minutes with four here
  (32 GB, 16 threads; the compilers held 7.1 GB at most, 9.9 GB stayed
  free).
- Published as a Pre-release on a new public repository (the old one
  renamed `rogue-galaxy-recomp-old`, private, since a force push leaves
  the old commits linked from GitHub's activity view). Hours later a look
  at the upstreams found DQ8-Recompiled's #36 in our archive: zip times
  have no time zone and Explorer reads them as local, so 0.1.0's files,
  stamped 16:09 Brasília, were hours in the future west of it, and a CMake
  input dated after now keeps build.ninja stale: ninja regenerates it 100
  times (some 40 minutes) and fails. `source_zip.py` now stamps a day
  before, in UTC, and `--ref` makes a tag's archive again: 0.1.0's was
  remade from its tag (the 814 files byte-identical, only the times) and
  replaced on the release before any download. `setup.py tools` and
  `build` also give any source file dated after now now's time first.

**Menus in 4:3 on a wide screen (2026-10-27).** After 0.1.0 the user
picked five things to do next, the stretched menus first. The frame is 512
wide and the presenter stretches it to the window, so the game's 2D, laid
out for 4:3, came out a third wider at 16:9. Its drawing goes in groups,
each opened by DrawListBeginPrim and closed by DrawListEndPrim, and nearly
all of its vertices through six setters (GsPktSetXYZ*), after one of four
for texture coordinates (GsPktSetUV/ST); the group's owner, BeginPrim's
caller (or FontDrawText's, for a text), tells what it is. A trace of the
owners (`RG_UI_FIT_TRACE=1`) in the item menu, the main menu, the town's
talk and a battle showed the menus all in the widget system (UiDrawWidgets,
UiDrawWidgetText, UiDrawSpriteTiled), the field's messages in MesWinDraw
and MesWinDrawName, the battle's party panel and WARNING!! in widgets too,
and the names over heads, damage and status icons in their own functions.
`src/ui_fit.cpp` rewrites a closing group's vertices in guest memory (the
frame goes to the VIF1 thread at GfxEndFrame, which closes any group left
open first): the menus and messages about the frame's centre by 4:3 over
the shape shown, the names and damage each about its own centre, the
compass's letters each about their own. What it found on the way:
- The main menu keeps its side list at x -74..46, past the frame's left
  edge and under the column drawn over it: narrowed, it showed in the
  side. Each sprite is cut to the 4:3 frame first, its texture coordinates
  with it, and a scissor a group sets is narrowed with it (the whole
  frame's stays, for the scene after).
- At 4x, two-pixel light lines at every tile of the item panel: a sprite
  starting on a half pixel starts a pixel late in the renderer while the
  one before it ends on time. The narrowed positions are whole pixels.
- A menu over nothing (the map not drawn: MapDrawPre not called in the
  frame) showed the frame's clear at its sides, blue in the menus; it is
  made black before the frame goes out. Showing such frames 4:3 instead,
  with the presenter's Borders, was tried and dropped: the presenter reads
  the flag later than the frame was drawn, so a menu's first and last
  frames would come out stretched or narrowed twice.
- The transporter menu draws the field's map and compass in its panel. In
  the field they keep the game's wide layout (MinimapDrawEntries narrows
  the map by 3/4, the compass is round by its view, both at the frame's
  edges); with the menu's markers drawn the frame before, IsWidescreen
  answers 0 to MinimapDrawEntries, the map's view and arrow are narrowed
  with the panel (not its first group, which draws the map's tiles into a
  texture of its own: narrowed, the map showed garbage), and the compass's
  centre (ViewSetScreenMatrix from CompassHudRender, 1866.9 at 0x47D830)
  moves in with them. In the menu's opening fade the scene shows bright at
  the sides of the darkening panel.
- Owners not in the list stay stretched, as before; shops, the factory,
  Insectron and the Revelation Flow are listed or expected through the
  widgets but not seen yet.
The menu's Game page has "Menus in 4:3" under Widescreen, on by default
(`RG_UI_FIT=0`), and the game's Settings screen a row for it. Only the
groups to fit keep their vertices: with Widescreen off the 30 fps walk's
frame is byte-identical to `mods30c`, and the battle bench with Widescreen
on gave 49.1 and 48.3 fps (median of the heavy intervals) with it against
47.8 and 52.2 without, within the runs' spread.

Then (2026-10-28) the user asked for the backgrounds to reach the sides
instead of the black bars, stretched, the rest staying 4:3. A group dump
(`RG_UI_FIT_DUMP`) of the main menu showed its background as sheets wider
than the frame (576x512, the texture drawn at twice its size, each drawn
twice in the same place): one at -64,-64 and its neighbours right, above
and below, past the frame; then one the frame's size, a darker shading at
its edges. The save menu's (and the transporter menu's stone) is a group
of 256-pixel tiles scrolling a pixel a frame. Stretched, the main menu's
sheet took its hole off the circle the field shows in, a dark crescent
beside the ring: the hole is in the 576-wide sheet, not the shading. So
a group of large sprites (none under 128 pixels a side nor over the
frame's width) covering the frame is left as drawn, stretched (the tiles,
the shading), and a sprite wider than the frame is narrowed with the menu
but not cut to the frame (the hole stays on the circle, the neighbours
come in at the sides). The game draws only the sheets the 4:3 frame
needs, so at 16:9 a strip about 3% of the width stays black on one side
when the sheets sit at an edge (left in the main menu, right in the
Revelation menu); `RG_UI_FIT_BACKGROUNDS=0` brings the black sides back.

The user played with it and liked none of it, so it is gone: with
Widescreen the 2D stretches across the picture again, as in 0.1.0, and
the transporter menu's map is the game's wide one. `src/ui_fit.cpp` was
never committed; what this section describes is the record of it.

The same day the user asked for the minimap and compass to be made
smaller. A dump of the field's groups found the map drawn by two owners
alone in its corner (MinimapDrawEntry's view, a strip of the map's
texture in its frame, at 310..470 by 274..418 in the wide layout, and
the player's arrow; the tiles and chests go into the map's texture
first), and the compass as a model under its own screen matrix
(`CompassHudRender`: ViewSetScreenMatrix with ox, oy 1866.9, 2212.2 from
0x47D830, then its four letters, sprites 26 pixels a side about points
projected through that matrix). Game > Minimap size and Compass size (50
to 100%) shrink the map's view and arrow toward the view's bottom right
corner (not while the view is wider than 240 pixels, nor in the
transporter menu), and the compass's matrix (sx, sy scaled, its centre
moved toward a corner 46 pixels left and 44 down of it), its letters
each about its own centre; wide or not, the letters stay on the dial.
These stayed when the 4:3 menus went, in `src/hud_size.cpp`: the same
groups and owners, recorded only for the map and the compass.

**Weaker machines, and an Xbox controller's buttons (2026-10-27/28).**
The second of the five: how the game fares with less. The battle bench
with the instance held to fewer logical processors (`AFFINITY`): all 16,
8 (four cores), 4 cores without their second threads, and 4 (two cores)
gave 59.8, 59.8, 60.0 and 51.6 fps (median of the heavy intervals): four
cores are enough, and what limits a slower machine is the game thread's
own speed (71% busy here at 60 fps, `dispatchGuestBranch` the largest
single part of it, 9.5%, already cut once). Two pairs of runs with
Widescreen on had given 48-52 against 60 without, and a third pair 59.8
and 59.9: the machine's other load, not the wider view. Nothing changed;
a Steam Deck would run the Windows build through Proton, untried.

The third: the buttons in the game's text are special characters drawn
from one picture, gaiji2.img (an IM3 archive with a 256x256 8-bit TIM2,
stored in the GS's PSMT8 order, its CLUT in CSM1's), which GaijiLoadImage
copies to 0x4C3180 at boot and with each map's resources: L2, L1, R1 and
R2 as 32x20 plates along its top, the four face buttons as 22x22 circles
below (no Start or Select: the text spells them). `src/button_glyphs.cpp`
keeps that corner as loaded and redraws it in place, in the game's style:
the cross's circle emptied with a coloured letter (A green, B red, X
blue, Y yellow, the colours of the menu's controller pictures), L1's
plate emptied with LB, RB, LT or RT in white, each pixel the nearest of
the picture's own 256 colours. `src/graphics.cpp` picks, twice a second,
what the controller driving the game calls the button each PlayStation
button is bound to first (Controls), an Xbox pad's or one SDL does not
know; a PlayStation or Nintendo pad, or the keyboard alone, keeps the
game's. The menu's Game page has "Controller's buttons", on by default
(`RG_BUTTONS=xbox` for tests). Seen in the field ("A Use transporter").
Not changed: the loading screen after a save is picked still shows the
cross (its picture is uploaded before, and kept while it shows: redrawing
on every special character drawn changed nothing there). The 30 fps walk
with no controller is byte-identical to `mods30c`.

**A notice of new versions (2026-10-28).** The fourth: once a run, four
seconds in, unless the menu's "Check for new versions" is off by then,
`src/update_check.cpp` asks GitHub's API for the project's releases
(WinHTTP, one HTTPS GET with five-second timeouts, nothing sent but the
request; `/releases`, since `/releases/latest` leaves out pre-releases,
which 0.1.0 is) and compares the newest one's tag with `RG_APP_VERSION`,
number by number. A newer one gets a toast ("Version X is out: F1, then
Rogue Galaxy > About") and a line in the About window with a button to
its page. Scripted runs (`RG_INPUT`) never ask. Tried: GitHub answered
v0.1.0, and with `RG_UPDATE_AS=0.0.9` the check found it newer.

**Sharp letters (2026-10-28).** The fifth: the letters at 8x. A search of
how other projects do HD text found redrawn fonts (TTF) and AI-upscaled
packs, both made off the player's machine; the user had found an
UltraSharp/xBR pack of the glyphs soft (branch texture-pack). Tracing
does it from the disc's own glyphs, on the player's machine: a glyph's 16
alpha levels give its anti-aliased distance field (Gustavson and Strand),
whose zero line is a contour accurate to a fraction of a texel; corners
are found where it turns past 60 degrees and squared off, the runs
between them smoothed and fitted with cubic Beziers (Schneider), and the
outlines drawn back with exact coverage at the internal scale
(`src/font_trace.cpp`; a Python prototype on 29 collected letters first,
the C++ port matching it, 2-3 ms a glyph at 8x). The edge is moved 0.15
texel in: at the half-covered line the letters looked bolder than the
originals, whose edges are semi-transparent. The user judged the trace
good, then asked why the black border did not go all round: the game
borders its dialogue letters with four black copies a pixel up, down,
right and left (FontDrawGlyphOutline mode 1, D_0047E968), which on
smooth letters leave notches wherever an edge faces a diagonal. Shown
four copies, eight and a round border side by side, the user chose the
round one. `src/sharp_letters.cpp` marks mode 1's copies (the packet's y
offset a sixteenth of a pixel up for the call; at a pixel's centre that
changes no pixel and no texel) and dq8-gfx, given the host's pictures of
regions (`sdlgpu_regions.h`, from the texture-pack branch's region
replacement), draws the first copy where the glyph goes from a border
version, the outline grown by a pen a pixel wide, and leaves the other
three out; without one, the four are drawn as they were. Four fonts are
traced, by their palette's hash: the dialogue's rounded letters, two
narrow ones (smoothed harder and kept at full width: their strokes are a
texel or so and came out wobbly), and the menus' small letters, grey in a
dark border drawn in (traced twice, the border's edge from the alpha and
the core's from the colour). Each glyph is made on three threads the
first time it shows and drawn as it is meanwhile. Text in a gradient
draws each glyph as two triangles, which take their rectangle whole. Not
traced: the menus' labels, numbers and icons, which are pictures, some
shaded. In the bench walk at 8x the load menu, its messages, the story
recap and the strategy menu came out traced; the town held the same 48
fps (hidden window) with the option on and off. `RG_SHARP_LETTERS=0` or
`=1` overrides the menu.

The user then saw a layer over the letters, out of place, a ghost, worst
in the load menu. Three things made it. The bench walk runs without the
menu's settings, so with the PS2's line blend (the menu's "sharper"
option off): each line mixed with the one above by the frame's alpha,
which smooth glyphs show as a second outline a line up; the user's game
has it removed, and the captures since are made so (a bench settings
file with `remove_line_blend = 1`, deleted after). The load menu draws
its letters at vertex alpha 0xFF, a blend factor of two, so every texel
at half coverage or more is opaque and the glyph comes out bolder than
its half-covered line, and its black copies at 0x80, soft: traced at the
half-covered line, the letters were thinner than the game shows them and
their border took over. dq8-gfx now asks for a bold version of a region
drawn past 0x80 (traced a quarter texel out), and the border goes about
that bolder outline, 0.75 texel, which reaches as far as a pixel about
the plain one. And the pictures took a glyph's own highest alpha for
opaque: a thin glyph never reaches 0x80, and its border came out half
transparent; 0x80 is opaque now. A frame log of the menu
(`RG_GS_FRAMELOG`) showed each glyph as four marked copies then the
glyph, and `RG_SHARP_LETTERS_TRACE` that the menu's letters are the
narrow font, bold. With the line blend removed, the menu's letters are a
grey glyph in one round black border.

Then the battle (the bench's File 03) and the widescreen menus. Battle
messages and the names over enemies (in a gradient) came out traced,
squeezed with "Menus in 4:3" as the rest of the 2D, their borders in
place; "CAMERA:HIGH" and the party's numbers are pictures. But a
descender poked out under its border: a glyph's sprite spans a texel
more than its pixels, the GS never shows that last row, and the font's
descenders end in it. The trace took it in, and the draw, clamped to the
shown texels, showed the last shown row a second time below it (the
shader clamps the GS texel and keeps the position inside it). The host
is now handed the texels a draw never shows transparent (the shown ones
taken without the scissor, so a line scrolling out keeps its pictures;
the border's from the first copy, not its grown quad), and the pictures
are drawn whole. The "g" and "y" of the load menu end in their border.

The user, at 4x, still saw a light bar beside some letters (h, a, E, l,
o). A capture of the game's window showed a column a pixel wide, the
glyph's grey, just left of its border: drawn by the glyph's quad, whose
first pixel column takes a bilinear tap one physical texel left of the
picture. The shader's REGION_CLAMP clamped that tap's GS texel to the
first and kept its place inside it, three texels in at 4x, where a
stroke touching the cell's left edge already is. A tap past a clamped
edge now takes the edge's outermost physical texel (`clampedSubTexel` in
`gs_draw.frag`; CLAMP and REGION_CLAMP, an upscaled source only); the
bench's load menu at 4x has the same border both sides of the "l".

Then two of the user's screenshots: "Use transporter" over a transporter
and a townsman's dialogue in Salgin, the letters each with a grey and a
black ghost down and right. Their style's border mode is 2
(`FontDrawGlyphOutline`: a 0x40 grey copy a pixel down and right, then a
black one two; mode 3 is the black one alone; 4 to 6 are wider borders
the game was not seen to use). At 1x those copies join into a solid
shadow, as the GS's blocks touch; traced, each showed its own outline a
pixel from the next. `src/sharp_letters.cpp` now marks a shadow's copies
two sixteenths up, and dq8-gfx draws each where it goes from the host's
shadow version: the glyph swept a texel back up and left, with the
border's margin, so each copy reaches the place of the one before. The
prompt comes out with a whole black shadow, its grey only where the
copies' ends differ, as the 1x frame has it.

And the special characters (`gaiji2.img`): the ® and the Xbox buttons
(`src/button_glyphs.cpp` draws them at the PS2's 22 pixels) stayed
enlarged blocks. Its palette, 844F1E0969532C9D (found with
`RG_SHARP_LETTERS_TRACE=palettes`, which names every small region's
palette and place), is now known to `sharp_letters.cpp`, and dq8-gfx
hands the host a region's place in its texture too: `src/button_art.cpp`
tells the Xbox's buttons from the PlayStation's by their letters' texels,
exactly as stamped, and draws them from their shapes at the target's
scale (the circle's bevel taken about it from the texels, the plates'
straight bevels texel by texel inside a rounded outline, the letters as
pen strokes in their edges); the PlayStation's own and the rest of the
picture stay as they are. The ® draws its oval, ring and R the same way:
its region (18,236 15x20) ends a texel past the picture's bottom, the
texel a sprite's UVs span past its pixels, and `spriteTexelRect` turned
it down; a region a texel past its texture is now cut to it.

Later the user compared the names over heads with the console's at a low
scale: pixelated, but defined, where ours came out too round. The town
walk at 4x was captured once per version (temporary knobs, the names
cropped side by side): a border of four copies like the game's (no
visible change at that size), a thinner round border, less smoothing with
corners from a 40-degree turn (the user's pick, but wobbly: the field's
small errors show), and that with the smoothing back. The rounded font
now has its own `Params` (`kRounded`: corners at 40 degrees instead of
60, now a field of `Params`, smoothing 0.3 instead of 0.4, runs within
0.2 texels of their chord straight); the narrow and bordered fonts keep
theirs.

Then the dialogue: a grey ring between the letters and their dark border,
which came out dotted, and no border on top of an "l". The trace showed a
style in border mode 5, which the earlier logs never had: black copies on
the four diagonals, then dark grey (0x20) ones on the four sides
(D_0047E970, D_0047E978), a soft border at 1x; traced, the copies showed
apart, the grey covering the black but at the corners. Modes 4 (twelve
black copies, two pixels out on the sides) and 6 (eight black) are alike.
They are now marked as borders too, three sixteenths up for the modes
whose first copy is diagonal (the backend moves that one a pixel right as
well as down to place the border), the backend takes dark grey copies up
to 0x20 as border copies, and it leaves out every other copy from the
same texels (it stopped at three, mode 1's). The battle's ability name,
in mode 5, now has one round dark border.

**A VU1 program of later Salgin (2026-10-28).** The user found the walk
choppy: their log had the field at 38-50 fps, the VIF1 thread 94% busy,
and "vu1 interpreted" at 1-9% of the pairs from the moment the save
loaded, where the bench walks show none. Their options (Field of view 64,
widescreen, Developer tools) were not it: the town, square and waterway
walks with them held 60 with nothing interpreted. Their newest save
(File 04, 2026-10-26, copied to `build/run/mc0-file04`) was: a program
at 0x39d0, 3% of the pairs every frame, missing from the manifest.
Recorded with `PS2_VU_PROGRAM_PROFILE` (four code images, the same entry)
and compiled, the same walk interprets nothing and the VIF1 thread is
41.5% busy against 61.8% (hidden window, 4x). `setup.py vu1-manifest`
folded it into `vu1-programs.txt` (51 images, 3 new), so a fresh install
compiles it too.

**Weapon growth (2026-10-28).** Asked for by players: weapons that grow
faster, as the experience option. BattleCalcResults ends with each weapon
of the members fighting (the two slots, through the weapon's virtuals):
while its gauge (ItemWeaponGetGrowth, +0x16) is under 15 (0x465B68) it
gains 1 (ItemWeaponAddGrowth, 0x3D3640, vtable +0x6C); once full, the five
attributes are shuffled with RandFloat and the first under its maximum
gains 1 (ItemWeaponAddAttr, 0x3D3290, +0xA4; the setter clamps to the
item's maximum); all at their maximums is the complete weapon (the
results' 0x8000). Game > Weapon growth (100 to 1000% in 100s) multiplies
the two 1s at those calls, known by their return addresses (0x3A2DDC,
0x3A2EE8): the hook on BattleCalcResults has already returned there (its
`g_results` was 0 at the weapon calls, though set at the money calls), so
it is no guide. The battle bench at 300%: every attribute call gave 3
(5 to 8), the money at 200% scaled as before.

**The loading screen's cross (2026-10-28).** With an Xbox controller the
loading screen after picking a save still said "Press" with the
PlayStation's cross, though redrawing the special characters' picture at
every draw had changed nothing there: LoadingScreenFrame draws it from a
picture of its own, Loading_01 (128x64, 8-bit, with "Story", the progress
bar's pieces and "Press X" at 0,40), in rc/loading2_?.img, which
LoadingScreenLoadImages reads into its arena (0x4E8970) and registers
(NameTableRegisterImArchive, the call returning to 0x344774). The cross's
circle (20x20 at 81,42) is redrawn there before that call, as the face
buttons in gaiji2.img are, when its indices are the ones known (an FNV-1a
of them). Redrawn after the call, the letter took its shape but not its
colour: the CLUT goes out at registration. The picture uses all 256
colours, but the cross's five blues only in the cross, so once emptied
one of them is set to the letter's green. Above 1x the user found that A
coarse beside the text's sharp ones and low in its circle (the circle's
dark lower bevel): button_glyphs.cpp now also hashes the redrawn palette
as the renderer does (FNV-1a over CT32, CSM1 and the 256 entries in index
order) and keeps the special characters' Cross cell, and sharp_letters
takes that palette as a font of its own (up to 128 texels): the "Press X"
region comes back enlarged as the GS's bilinear filter shows it, the
circle cleared and the text's face button drawn over it from its shapes
(button_art.cpp's drawLoadingLarger), so both A's are alike.

**Toward Linux and the Steam Deck (2026-10-28).** An audit of the tree
(notes kept outside the repository) found the runtime already built on
Linux by its fork's CI, the renderer on Vulkan already, and in `src/` one
call that does not compile with glibc: `_putenv`, now `setenv` off
Windows. Two changes for the Steam Deck, through Proton as natively: the
first window is fullscreen when Steam says `SteamDeck=1` and no window
size is saved (Game Mode letterboxes a window), and `Rogue Galaxy.exe`
no longer sets `VK_LOADER_LAYERS_DISABLE=~implicit~` under Wine (ntdll's
`wine_get_version`), where it reaches the host's loader and would keep
out gamescope's layer and Steam's overlay. Not tried on either: there is
no Linux here (no WSL), and the installer cannot run under Proton
(Windows PowerShell 5.1, System32 tools), so a Deck takes a folder built
on Windows: `setup.py portable --out DIR [--saves]` copies what the game
runs from (the launcher, the game and its DLLs, `disc/`, the borders) in
the repository's layout, the memory cards only with `--saves`.

**Save anywhere (2026-10-28).** The game saves only at transporters, whose
menu is a game mode of its own: GameModeDispatch's 20,
TransporterMenuModeUpdate (0x27CC20), which holds the game's requests,
pushes the tasks' states, opens the panel (Save, Teleport, Exchange
Points, Item Storage), waits for it to close, and gives the field back
(or warps, for Teleport). FieldExploreModeUpdate asks for it with
GameReqQueuePushFront(20) for Cross at a transporter, and for the main
menu (17) with GameReqQueuePushBack for Triangle when the leader is free
(LeaderGetControlStateMaybe 0). In the field Select opens the full map,
L3 the leader's choice, R3 a fixed camera, and R1 nothing alone (only
with Square). Game > Save anywhere (`src/save_anywhere.cpp`) turns
Triangle with R1 held into mode 20 at the main menu's request: the
Triangle check returning to 0x283840, the request to 0x283880. Scripted
from File 04: walked off the transporter, R1 + Triangle opened its panel,
Circle closed it to the field, and Triangle alone still opened the main
menu. The user saved so and loaded the save: it loads at the area's
transporter, not where it was made (LeaderPlaceAtSavePoint places the
leader by the save point the area records). Kept so, off by default; the
exact place is left for later.

**Small ones (2026-10-29).** Left from the free camera's review of
2026-10-26, three cases, and a check from the upstreams' survey:
- The free camera turned off from the menu or F6 kept the pad idle one
  frame more: scePadRead read `g_active`, which only the camera's next
  update clears. A switch seen off while active now holds the pad as Start
  does, until the controller lets go of every button, and clears
  `g_active` there. That matters more than the frame: the battle's
  "escape?" window (FieldHudBtlEscOpen, 0x203D20; BattleModeUpdate opens it
  when the leader stands outside the battle's circle, which a hit can push
  them out of while the camera flies) reads pad 1 alone and pauses the
  update tasks, the camera's among them, so `g_active` never cleared: the
  pad stayed idle even after Start, and the game was stuck (a player's
  report). The window's opening now ends the free camera too, as Start
  does. Tested with a temporary line that turned the free camera on at the
  battle bench's start and shrank the circle's radius (0x482210) to 1: the
  window opened over the free camera, the camera ended, Cross answered it,
  and it opened and was answered nine times more; Start at the stairs as
  before.
- Pad 2 was not blocked. The runtime answers port 1 with the same
  controller as port 0, and EvCmdPadTestTriggered, the event scripts'
  button test, ORs in pad 2's triggers whatever IsPad2Enabled says (it
  tests the function's address, always set), so a button pressed for the
  camera could reach a script waiting for one. PadUpdate (0x1843A0) is
  hooked for pad 2: while the controller is the camera's, its state is
  kept aside and pad 1's idle one put in its place; the camera's update
  takes the kept one, and the next PadUpdate gets the real one back
  first, so its last-frame half stays true. Scripted from File 03
  (`RG_FREE_CAMERA=1`): the camera flew off with the stick, Start brought
  it back and the party walked to the stairs.
- Left as it is: releasing the camera writes the override -1 without
  CameraSetModeOverride's CameraModeSetTop, which applies the camera's
  mode again (its setup, CameraApplyMode, snaps it to its defaults). Taking
  it skipped mode 13's setup the same way, so nothing of it needs undoing,
  the follow camera catches up with the party on its own, and the game's
  call would need a guest call from a hook.
- GIF tags with FLG 3 (on the GS, IMAGE again), which the original
  PS2Recomp learned to read and our fork's GS::processGIFPacket skips:
  counted with a temporary line through a free camera walk (title, the
  loading recap, the field), the battle bench and the town bench. None:
  the game does not send them there, so nothing was brought over.

**Save anywhere and Sharp letters taken out (2026-10-30).** The user asked
for both to go, to be looked at again some day: `src/save_anywhere.cpp`,
`src/sharp_letters.cpp` and `src/font_trace.cpp`, their options in the F1
menu and in the game's Settings screen (`save_anywhere` and
`sharp_letters`, which an older settings.ini may still hold: the loader
keeps any key under [game] and the menu asks only for the ones it knows),
`RG_SAVE_ANYWHERE`, `RG_SHARP_LETTERS` and `RG_SHARP_LETTERS_TRACE`, and
the button art's drawing of the Xbox buttons and the registered mark from
their shapes (`drawLarger`, `drawLoadingLarger`, with the loading cross's
cell and palette `button_glyphs.cpp` kept for them), which only the
letters' region replacements called. dq8-gfx's region replacements
(`setRegionReplacements`, the round border) stay in the backend with no
user here. The Xbox buttons in the text and the loading screen's cross
are still redrawn into their pictures at the PS2's size. The sources as
they were are in the history at b55f0ea.

**A launcher without the terminal (2026-10-23).** The user asked for
RogueGalaxy.bat not to open a terminal. A batch file always opens one, and
this one stays the whole game, waiting to keep the log and show an error.
`Rogue Galaxy.exe` (`src/launcher/launcher.c`, built into the repository's
folder) does the batch file's work as a Windows program with no window: the
same variables, the previous log kept, the game started with its output in
`build\last-run.log` and no console (`CREATE_NO_WINDOW`), and for a game
that ends with an error a message box with the log's last 15 lines, each cut
at 150 characters (the `[gfx]` lines run past 300). It is plain C linked
statically, since the DLLs the build copies sit beside the game and not
beside it, and a second start while the game is up says so instead of
starting another. The name has a space so it never answers to the image
name the bench and hang tools look for (`roguegalaxy.exe`); its icon, a
ringed planet, is drawn by `src/launcher/make_icon.py`. The batch file
stays for the `debug\` launchers and terminal runs. Started from Explorer
the game ran with no console window, closed from its window the launcher
went with it, and killed it left the message box. Found on the way: the
machine's Malwarebytes quarantines llvm-mingw's windres the moment it runs
(three times, once a copy in a scratch folder with `--version`), so the
icon and manifest are a `.res` that `src/launcher/make_resources.py`
writes and lld links as it is; CMake's RC language, which an earlier
subproject had enabled with a `windres.exe` the toolchain no longer has,
is not used.

**The developers' tools back (2026-10-23).** The user asked what
RGDecomp would let us change; three read-only sweeps of it found a pile of
debug code still in the retail executable. Fifteen places ask 0x207FE0,
a function that returns 0, before they act: the main menu's item giver
(R3 shows every item's icon with its number, name and text, R1 + R3 a
list; Cross gives one), unlocking every help page and info entry, filling
the bestiary, one of each item a Revelation Flow cell asks for (the 10 in
those calls, InvAddItemCore's, is the item's kind; the user saw one come),
and ending any scene
while paused. The menu's "Developer tools" makes it answer 1
(`src/dev_tools.cpp`); the File 03 town run with R3 in the main menu
showed the grid. The other findings (a free camera, the game's own
performance meter, the field of view fixed at 52 degrees, two camera
distance presets, per-map draw distance and fog, an instant text flag,
encounters as a distance to walk, the battle rewards in one function)
wait for the mods the user picked next, in order: instant text, the field
of view and a farther camera, a longer draw distance, then multipliers.

"Instant text" came next (`src/instant_text.cpp`). A message window types
its text out in MesWinAdvanceTypeout (0x275F00), which reads the window's
instant flag (+0xB4) through MesWinGetInstant (0x277600) and nowhere
else; scripts turn it on with a text speed of 0. The option makes the
getter answer 1: the game's own instant mode, which still stops at the
pauses and waits written into the text and at each page. In the File 03
load and item menu six windows went through it; their text (the menus'
descriptions, the battle log) comes up whole either way, so a dialogue
typed letter by letter was not seen in a scripted run.

Then the camera (`src/camera_view.cpp`). The scene's projection is set
again every frame by ViewSetProjection (0x143000, f12 the vertical angle
in radians, from ViewSetAspect, ViewSetDepthRange and
ViewReapplyCameraRec): 40.2 degrees high, the 52 across a 4:3 picture
that CameraObjInit gives the field camera, in the field (game mode 6), in
battle (7) and in the transporter's menu (20) alike. "Field of view"
replaces that angle in modes 6 and 7; an event's camera sets its own
(CameraTrackUpdate) and keeps it. The camera keeps between a nearest and
a farthest distance, pushed back or pulled in past them: 65 to 120 in the
field (CameraSetDefaultParams 0x309260), 150 to 190 up or 120 to 160 down
in battle (the two settings its button switches between,
CameraSetPresetParams 0x309100). "Camera distance" scales both as they
are written, and when it moves, the cameras take it at the next
projection if their blocks still hold what was last written there. The
first version scaled the battle's alone, and the user saw no change
moving the slider end to end in the field; a trace of the block showed
65 and 120 there, and CameraSetPresetParams called only as a battle
began. The menu's Interface > Game
has sliders for numbers now. In the town at 2x, 75 degrees and 140%
showed the whole stair and the transporter where 52 and 100% showed part.

"Draw distance" (`src/draw_distance.cpp`) scales the maps' far clips and
fog as their configuration is read. A far clip record (FarClipRecParseArgs
0x18C850 for nodes and the map, MapModelCfgFarClip 0x195DC0 for models)
holds a far distance past which a thing is hidden and a near one inside
which it is, 0 or -1 for none. Salgin's streets have far ones of 4000 and
near ones of 38, 300 and 360 with no far one to pair with: things that
hide as the camera comes close, so only the far ones are scaled, with the
fog's start and end (MapSceneCfgFog 0x192BB0, from 5 to 10000 or 4500).
At 100% the 30 fps walk's logged frame is byte-identical to before; at
300% the fog is a little thinner, mostly far off, and nothing comes or
goes near the camera.

Last, the battles' rates (`src/battle_rates.cpp`). Encounters come by
distance: TaskAddEncountDist (0x1F6D90) hands the leader's step to
CharaSysEncounterDistTick (0x3A5890, f12), which takes it off a countdown
and queues a battle at 0; "Encounter rate" scales the step, 0 for none.
BattleCalcResults (0x3A2100) totals the enemies' experience in its results
block (capped at 9999, 0x465C90), adds each enemy's money with
PartyAddMoney (from 0x3A2944), then gives every member the total
(CharaAddExp, from 0x3A2D38). "Experience from battles" scales the total at
the first of those money calls, before anyone gets it, so the results
screen shows what was given; "Money from battles" scales each enemy's
money, and selling, chests and rewards (PartyAddMoney's other callers)
stay. In the battle bench at 300% every member got 27 instead of 9, the
three enemies 72, 63 and 69 (draws of 21 to 26 tripled); at 0% the forced
encounter never came.

**The larger 2D art set aside; SDL's defragmentation (2026-10-23).** The
user found the game's letters pixelated at 8x. Edge smoothing in the
shader did not please them and was undone. A pack of larger versions,
collected while playing (each sprite's texture rectangle by content) and
enlarged on the machine, followed: AI upscalers (Real-ESRGAN, then
Upscayl's models) made icons and faces look like watercolour, so the
letters went through UltraSharp and the other pictures through xBR, and
the user did not like it yet either. It waits on the `texture-pack`
branch for a better way (one idea: drawing each glyph from a similar
high-resolution typeface instead of enlarging the picture); what the
user collected stays in `textures\collected`, out of the repository.
Found with it: in about one item-menu run in twenty, digits came out
missing or as noise, with the same draws as a good run's. SDL defragments
memory at a presenting submission and moves textures while the GS
thread's recorded uploads still name the old ones (dq8-gfx's README);
the window thread's present now goes out under the backend lock after
them, which keeps any upload or draw from being lost that way.

**8x without paging (2026-10-22).** The user, playing at 8x on purpose
(scale 8 in settings.ini; 3x before), found a place where the frame rate
fell hard; two F9 windows came after the dips (35 and 30 fps for a second
or two, the GS worker past 150% of the wall). The town bench at 8x held
29.9 frames a second, with frames of up to 1.4 s, against 55.0 at 3x; the
GPU's memory went from 2.7 to 11.4 GB of 12 as the town loaded, and a
quarter of the worker was SDL binding memory under `recordSnapshot`. The
streets take about 110 feedback snapshots a frame (strips of a blur,
channel shuffles), each copy cycled its texture, and every copy recorded
ahead of a submission kept a whole texture of the surface's size until
the GPU was done with it: about 70 MiB each at 8x. The copies overwrite
the texture in place now (the GPU runs them in order after the draws that
sampled the last one; `thirdparty/dq8-gfx`, its README,
`DQ8_GFX_CYCLE_SNAPSHOTS=1` as before): the town at 8x 57.7 frames a
second with 2.3 GB, a battle at 8x a median of 57.1 against 48.3 (the
lowest interval 50 against 26), the town at 3x 58.4 against 55.0, the
worker 66% against 82%; the logged frame byte-identical at 1x and 3x.
What 8x still costs is the GPU's own drawing, busy most of the time.

**The chests' lid at the console's pace (2026-10-21).** The user saw a
chest's contents jump out faster at 60 fps. TaskStepTBox (0x338900)
runs TreasureBoxUpdate (0x3398C0) on the chests in reach once a frame,
and it counts frames: an opening chest's lid rises, drops in a parabola
and shakes in a damped sine over a counter of 112 (+0x28; the draw turns
the lid frame by +0x5C), fades over 15, and the glow pulses over 300.
It joins the `menu` step fix's list, so it runs on the logic frames. The
user found the items still fast: they are icons the chest's script bursts
out (EvCmdEventIconBurst, up at 5.0 and staggered by 10 frames), which
EventIconUpdate (0x2789B0) moves a frame at a time, velocity, gravity, a
bounce at half and 45 frames of life, from the Draw Event Icon task, which
runs every frame. It joins the list too. On 2026-10-22 the user saw the
items jump out at the PS2's pace.

**The 60 fps mode a menu switch, the launchers tidied (2026-10-21).**
Interface > Game > 60 frames per second (on by default; `sixty_fps` in
settings.ini) decides the mode at the next start: the menu applies the
saved settings before the hooks go in, and the 60 fps mode's hooks are
only installed at start-up. `RG_FPS=60` or `RG_FPS=30` still decides
for a run (the benches set it either way), and `RogueGalaxy.bat` no
longer sets it. The diagnostic launchers (captura, perfil, engasgos)
moved to `ferramentas\`; `RogueGalaxy-30fps.bat` (the switch now),
`-sem-ia.bat` (the `ai` fix confirmed) and `-pulos.bat` (the jump fixed;
`RG_JUMP_TRACE=3 RG_POS_TRACE=1` by hand) are gone. On 2026-10-22 the
folder became `debug\` and the launchers took English names like the rest
of the project: `RogueGalaxy-capture.bat`, `-profile.bat` and
`-hitches.bat`, whose profiles go to `build/run/play-profile-N.prof` with
`exe-profile.exe` (the `play-perfil-N.prof` before them stay as they were).

**Texture animations at the console's rate (2026-10-21).** The user saw
the platform the Monography Shot raises in battle drawn wrong at 60 fps:
tiles on dark blue, where a PS2 recording and the 30 fps mode show circles.
F7 captures in both modes: its 64x64 texture at 0x2B00 is redrawn every
frame from four layers scrolled across it, each as four sprites split
where it wraps, and in a 60 fps capture two of them sat half their size
off, which at 30 never move. TexAnimSeqUpdate (0x163DD0) moves a layer by
the step and wraps it at its period in frames; the draw shifts it by
position / period of its size. A period of one frame is how the data keeps
an axis still (the shift divides by the period): at whole steps the
position wraps back to 0 at every update, at half steps it read 0.5 every
other frame, and the layer jumped by half and back. Most callers also pass
a constant 1.0 a draw (DrawListAddTexLoad from the effects, the HUD, the
chests; EffTexResStepAnim), so those animations, scrolling and flip-book,
ran at twice the speed. The new `texture` step fix (`src/step_fixes.cpp`)
gives such calls the frame's share of a step, and after a call with a part
step puts the still layers (a period of one frame, or a sine over two:
sin 0 and sin pi alike) back where whole steps leave them. In the battle
bench it holds about 14,000 layer positions every 300 updates (about 46 a
frame, the map's textures among them) and rescales 23 to 202 calls; the
battle's frame rate is the same with it and without (48.4 both, with
another game open).

**The EE safe point inline (2026-10-21).** Every inter-function transfer
and back edge of the generated code charges cycles and asks the scheduler
whether to unwind: PS2Runtime::eeCheckpointDue -> EeScheduler::checkpointDue
-> accountCycles -> PS2Memory::advanceEeTimers, three calls across
translation units for a few compares; in the battle bench's profile
checkpointDue and advanceEeTimers were 6% of the game's thread on top of
the dispatcher's 7.9%. Patch 0018 has the three in the headers (the end of
a time slice and the timers' batch flush stay out of line), same logic:
the 1x bench walk's frame is byte-identical. Battle profile: the game's
thread idle 22.6% of the window against 19.0%, dispatcher plus checkpoint
11% against about 14%; the heavy intervals' minimum 55.0 fps against 51.9
(one run each; battles vary).

**Bench windows after the loading screen's pace (2026-10-21).** With the
loading screen at the console's 30 fps (2026-10-21 morning) the field
appears about 20 s later than the benches assumed: the town bench's
summary over 56-80 s took in the loading screen's 30 fps and read 56.3 fps
where the town runs at 59-60 (its heavy stretch is now 76-95 s, the GS
worker at 77-85%); the gate's field starts at about 60 s and its profile
window is 64-81 s; the battle's 68-83 s. A 0.5-0.63 s frame at the gate's
82 s, twice the same, is the bench's own frame capture, not a game hitch;
the field entry's 0.3-0.44 s is the known load. Gate: median 55.9 fps, the
VIF1 thread 89% busy, one VU1 program 16% of it. A CLIP-only fast path in
Run::commitFlags (4% of that thread: a vertex program's CLIP results,
queued one per vertex, went through the per-entry loop) gave nothing in
three alternated pairs (VIF1 16.0/16.2/16.1 against 16.0/16.2/16.4 ms a
frame) and was dropped.

**The battle bench back (2026-10-21).** The user saved a new File 03 in
Salgin near the waterway for it. Its old route walked out of the battle
area and froze on "Escape from the battle?"; the start has no encounters
either (TaskAddEncountDist returns before the countdown). Now the walk
reaches the next area, `RG_FORCE_ENCOUNTER=1` (`src/debug/encounter.cpp`)
cuts the encounter countdown so the battle starts at its first step there
(pad read 2547-2549 in three runs, about 68 s into the run, after the
loading screen's 58, to about 83 s), and Cross
every 30 pad reads attacks and answers "Keep fighting". First run: 13
intervals with 55k draws or more, median 59.8 fps, minimum 47.7. F7 now
also has the game thread print the 60 fps mode's state (`[timing] state:`
game mode, intervals, lock, whose step, logic frames run and skipped),
and captures number on from the folder's highest.

**Bench runs off the desktop (2026-10-21).** `tools/bench/walk.sh` now
starts the game with its window hidden (`DQ8_GFX_HIDDEN_WINDOW=1`;
`HIDDEN=0` shows it, for window captures), so a scripted run no longer
takes the user's screen or focus. The 1x bench walk's frame came out
byte-identical to a visible run's; one earlier hidden run logged 206
batches fewer (scenery out of view, the same picture), a run-to-run
difference that a second hidden run did not repeat.

**VU0 macro flags in the recompiler (2026-10-21).** The generated code for
VU0 macro-mode FMAC ops never updated the status or MAC flags, which broke
the game's three bounding-box tests until `src/vu0_flags.cpp` replaced
them (2026-10-18). The recompiler now emits the update
(`patches/ps2recomp/0017`: `PS2_VU0_FMAC_FLAGS` in ps2_runtime_macros.h,
zero and sign per field of the destination mask, current and sticky) after
every ADD, SUB, MUL, MADD, MSUB, OPMULA and OPMSUB form, but only in the
functions that read the flags (a `cfc2` of $vi16 or $vi17), so the rest of
the game's vector code costs what it did. In this build that is the three
box tests, two updates each, and a regeneration rewrote only their files.
With `RG_VU0_FLAGS=check` the generated tests answer and the host versions
check them: 1,769,472 tests on the bench walk, none disagreeing; with
`RG_VU0_FLAGS=0` the walk's frame is byte-identical to the default's. The
host versions stay the default, as the shorter path.

**Destination reads without framebuffer fetch (2026-10-21).** After the
Monography Shot, the renderer's fixed-function path (every GPU but
Apple's) was searched for more of the kind. `tools/bench/gsstates.py` over
397 frame logs (town, battles, the gate, menus, events, F7 captures)
lists the states a GPU blend cannot express that the game uses: FBA
(fixed), CT16 byte masks (exact as write masks), and three left: a factor
of destination alpha in 323 logs (the sun flare: a 64x64 buffer's alpha
turned into colour, Cs times Ad, which a fixed-function blend weighs by
Ad/255 against the GS's Ad/128), a destination alpha test in 134 (one
full-screen sprite writing alpha 0 only where the frame's alpha is under
0x80, before the depth of field sorts pixels by alpha; ignored, it cleared
every pixel's), and additive lines and fans with vertex alpha up to 0xB4 in
14 (clamped at 0x80). Those draws now take a snapshot of the primitive's
region of their target and blend in the shader, with the fetch path's code
(`thirdparty/dq8-gfx`, see its README). GS memory dumped around the test
shows it right (56831 pixels at 0x80 or more kept, 513 cleared). No
on-screen change in the frames at hand: the frame's alpha there is 0x80,
which the later tests treat as they did 0, and the sun was out of view
(its 64x64 visibility buffer all 0); one to four reads a frame.

**The movies smoothed (2026-10-21).** In a scene the user took for a
flashback the picture went jagged and the frame rate to 30. Four F7
captures of it hold 952 batches each and no 3D scene: 240 black
sprites, one textured sprite from 0x2E00, the subtitles, and 672 uploads of
16x16 to 0x2E00 covering 512x336, the decoder's macroblocks of the next
frame; the picture changes between the captures. It is a pre-rendered
movie, at its own 30 frames a second as on the PS2. MovieDrawFrame
(0x1D5870) draws the frame with the texture filter off; at 3x every texel
is a hard block, and with Widescreen its 336 lines are stretched over 448,
so one in three is doubled and the edges step. The menu's "Smooth the
movies" (on by default, `src/movie_filter.cpp`, `RG_SMOOTH_MOVIES=0` for
the PS2's look) answers the frame's GsPktSetTexFilter call (returning to
0x1D5A08) with 1, so GsPktSetTexture sets TEX1 to bilinear. The title
screen, a movie too, has smooth edges at 3x where it had 3x3 steps.
Removed on 2026-10-22: in play the user saw little difference, and the
movies are drawn unfiltered again, as on the PS2.

**The Monography Shot solid (2026-10-21).** The user saw the gun Jaster
is handed in an event translucent, in the event and in battle, at 60 and
then at 30 fps too; a PS2 recording shows it solid. Frame logs of the
user's save, walking at 30 fps with the gun holstered, show how the game
draws a character's weapons: each into its own layer in the 0x150 buffer
(0x158 for the sword, 0x152 for the gun), then over the frame with
sprites: the layer read as CT24 with AEM at alpha 0x3D-0x3F, twice, a
fraction of a pixel apart (a soft edge), then as CT32 with an alpha test
of 1 or more and a fixed factor of 0x80 (the copy). The gun's 328 batches use texture alpha
0 and FBA; GS memory dumped before the copy (`RG_GS_FRAMELOG=file@N#K`)
had 493 of its 503 pixels at alpha 0, so the copy skipped them. The
renderer applied FBA only where it has framebuffer fetch; now its
fixed-function path sets the bit too (`thirdparty/dq8-gfx`, see its
README). All 503 pixels then carry alpha 128 and the gun is drawn solid;
the rest of that frame is unchanged, and the 1x bench walk differs from
its reference in 10 pixels, the edge of the gun Jaster held then. The
60 fps step fixes were cleared first (with `RG_STEP_FIXES=0` the gun
was still translucent).

**Captured frames keep their scale (2026-10-21).** In an event where the
party is handed an item the user saw the whole picture, background and
characters, in blocks at 3x. Two frames captured with F7
(`RogueGalaxy-captura.bat`: `RG_GS_CAPTURE` logs the frame after the
press, with the renderer's target trace, now numbered by batch) showed
why: the item's message runs `EvCmdMenuWinShowScreenCapture`, which reads
the frame into EE memory (ScreenFadeCaptureFrame, GsDownloadImage, a
StoreImage of 512x448 CT32) and shows it in a 512x448 window under the
message, uploading it to 0x2A00 and drawing it unfiltered every frame;
read back through local memory, the frame comes at 1x. The same capture
makes the dissolves into and out of battle (BattleModeUpdate) and the
events' dissolves and wipes. The renderer now keeps a scaled read's
region on the GPU with a hash of the bytes the host got, and an upload of
the same bytes onto a surface of that size gets the scaled copy
(`thirdparty/dq8-gfx`, see its README). On the town bench's battle the
stats line counts 15 round trips at its start, and the dissolving frame
keeps its detail; frames byte-identical at 1x. On the way: the event's
blur chain (0x150 and 0x162, 192x192) shares its pages with the textures
uploaded that frame, and the first blur pass imports 0x162 from local
memory, harmless as the passes cover it whole.

**Widescreen of the window's shape (2026-10-21).** The user's window,
maximised, is 2560x1352 (1.89:1), so a 16:9 picture left bars at its sides.
Under Auto a wide game now fills the window (`sdlGpuWideAspect`, its shape
clamped to 4:3..32:9), and `src/graphics.cpp`'s pump hands the shape shown
to `rg::widescreen::setAspect` every frame: the scene camera (ViewSetAspect
from SceneCameraApplyAspect's wide call, 0x1C6A58), the compass and the
model previews take its inverse for 9/16, and the movies fit it. The HUD's
3/4 is the mode's own and right at 16:9 (6% wide at 1.89:1). At 1.89:1 in
the town the picture runs to all four edges with the right proportions;
the 2D menus are stretched the more. Frames byte-identical at 1x with the
option off. The patch's letterbox fix is dropped: in an event where the
party is handed an item the messages ran off the bottom and a dark line
stayed across the top, since the letterbox's fade also places the text
over the scene (LetterboxFadeStep moves the map captions by it); events
keep their bars.

**The party's AI holds its walk between its runs (2026-10-21).** The
user saw the party following Jaster walk in little steps and stops, from
the moment a game loaded, and walk normally with the `ai` step fix off
(`RogueGalaxy-sem-ia.bat`). RGDecomp's C for the AI shows why: its walk
and run are inputs held for one frame. CharaAiHandlePrmtRequestsMaybe
clears the character's flags (+0x568) and calls AiUpdate, whose
AiMoveWalkOrRunToTarget / AiMoveRunToTarget add 1 (walk) and 2 (run)
(AiRouteSegmentStep and AiClimbOrJumpWall 0x10002, the ladder 0x80020 and
0x80040), and the character's update reads them after; with AiUpdate on
the logic frames only, every other frame had no input and the character
stopped. The frame in between now holds again the low-word flags the
AI's last run added. A first try, the fix only while IsInBattle
(0x4821B8), did not do: SetInBattle comes with a battle and only
GameFlowReset clears it, so it stays set after the first fight (the town
bench meets one at 88 s). Measured with `RG_JUMP_TRACE=4` (new: every
character's moves on the ground too) over the town bench, the two
followers' stops of one to six frames: 30 and 22 in the walk with the
first try (20 and 19 of them after its fight), 5 and 6 now, 3 and 1 with
the fix off; the share of frames they move is the same as with it off.

**The VU1 programs again: three tries that gave nothing (2026-10-20
night).** A VIF1 profile over the gate's busy stretch (PROFILE_START=62:
the field starts near 57 s on the stats clock; an earlier window caught
the load and showed the thread idle) splits it into about 53% compiled
VU routines, 9% copying GS packets into the GIF stage, 4% commitFlags,
2.4% VIF unpack and 1.4% exactUpper. The routines read their registers
from memory at every pair: each pair's rare path (a stall, a kick) calls
out of the routine and rejoins it, and the routines are too large (the
busiest, r_a786efabfa469ada_52, 134 KB) for the compiler to prove the
registers unchanged. Three changes, each alternated with the build before
over three pairs of gate benches, frames byte-identical: the ready stamps
as the Frame's own copy (20.7 against 20.8 ms of VIF1 a frame, and 2.6 MB
more code); the VI registers as the Frame's own copy, copied in at its
construction and out in save() (19.9 -> 20.9 ms: the memory accesses went,
72 to 2 for one register in the busiest routine, but its stack reloads
doubled, the register allocator spilling in these functions); and the
stage's copies through memcpy instead of vector::insert (9.3% -> 8.8% of
VIF1: the cost is the bytes copied). All three were taken out again. A
real cut needs another shape of compiled code (small register-friendly
regions, rare paths that leave instead of rejoining, timing and flags
dropped where nothing reads them, as PCSX2's microVU does), a project of
its own. These benches ran beside another game the user had open, which
took the gate from about 56 fps to about 42 and made it noisier.

**The GS worker in the square: a kept staging texture, whole-block page
imports, and a town bench (2026-10-20 night).** `TOWN=1 walk.sh` loads
the user's File 03 at Salgin's transporter and walks the square, where
the GS worker is the limit (80-105% of the wall on its heavy stretch in
one session, 60-77% in quieter ones: alternate builds). Its profile: the
texture updates 15% (24% before the unchanged-texture skip), the draw
flushes 20%, the host writes 24%, the page imports between aliased views
7%. A scaled target's refresh (GsTargetCache::uploadNative) created a
native-size texture for each upload and released it after the blit: the
driver's texture creation was 1.7% of the worker; one is kept and grown,
cycled on upload. importColorPages copied one run of pages a row; the
same run on the next row of both surfaces now joins it, so a block of
whole rows is one copy (each copy carries SDL's layout transitions).
Together, three alternated pairs: the worker 76.7% -> 70.3% of the wall
at 59.2-59.3 fps (the stretch already near the cap that session).
DQ8_GFX_SUBMIT_DRAWS=256 instead of 96 made it worse (94% against
81-88%). Frames byte-identical at 3x. A feedback snapshot per strip of a
192x192 blur (113 a frame) stays: each strip reads two or three pixels of
the strip before, so sharing one would change those pixels.

**Textures that come back unchanged, and a menu that pauses by itself
(2026-10-20 evening).** In the user's busiest street the GS worker was 95%
busy, a quarter of it re-expanding textures (GsTextureCache::update; the
expansion alone 13.6%), some 60 a frame, 1.2 Mtexels. Two measurements
on the street walk settled what to do. Host writes that leave local
memory as it was are rare (6% of the payloads, nearly all over pages a
live target owns, where skipping is not allowed): the game streams other
textures through the same pages, so a skip at the write was tried and
taken out. But 86% of the re-expands give exactly the texels the texture
already held (98% of them whole textures): another texture passes through
its pages and this one is uploaded again. update() used to find that out
after expanding it, and skipped only the upload; expand() now hashes
without expanding (`hashOnly`, the same reads and hash), and a whole
texture whose raw texels hash to what it holds, with no palette change
waiting, skips the expansion as well (`textures unchanged N` on the
stats line; DQ8_GFX_NO_UNCHANGED_SKIP=1 for the old way). Frames
byte-identical at 1x and 3x, with and without; on the light street walk
the worker went from 33-35% to 32-34% of the wall. The first opening of
the main menu pauses the frames for 149 ms at 60 fps and at 30 alike:
the game thread waits on vblanks and alarms with every guest thread
blocked while the menu's data and sounds load (the kernel snapshots,
PS2X_KERNEL_TRACE_MS, and a game-thread profile around the hitch against
one of normal frames), the game's own behaviour and not a cost of the
port. RG_TRACE's return lines now give the call's host time.

**The characters' AI at the console's rate: a boss that left its arena
(2026-10-20 evening).** In the gate fight the boss, after a while, charged
across the arena and out of it, and hung in the air outside, half its
textures gone and black shapes where it stood in the scenery, with the
profiler off as well; at 30 fps it landed on the far side, so the game's
logic and not the drawing. RG_JUMP_TRACE=3 now logs every enemy's
moves with what drives them (velocity, the per-frame moves, the move
request) and how far something else moved it between two, and every move
request with its caller: the charge was 54 frames of (29.8, 0.9, -0.8)
written outside the movement, and a debug-register watch on the boss's x
(RG_HWWATCH, the boss at the same address in every fight) named the
writer: AiCmdCharaSetPosById, from the AI script, through AiUpdate
(0x2C05B0), which runs once a frame per character, counts its timers in
frames and moves the character a step a call (AiCmdMoveByLocalOffset by a
requested move that CharaMoveCollideMaybe applies whole and clears),
while what ended the charge took as long as at 30. The documentation had
this down as the gravity bug, which Red-tv's per-boss code cave patched
by halving a delta; it is not. The new `ai` step fix runs AiUpdate on the
logic frames as a whole step, like the scripts, and shares the move it
made, by position or by requested move, half and half with the next
frame (moves of more than 100 units are placements, made at once), so
the characters move at the console's speed without stepping at 30 Hz.
The boss now lands where it should (the user's fight); a battle on the
bench card runs with its four enemies moving every frame. The enemies'
and allies' AI had been thinking twice as fast at 60 fps.

**A profile of a hitch nobody could mark in time, and a review (2026-10-20
afternoon).** A hitch is often over before F9 is pressed. With
`RG_PROFILE_BEFORE=S` the sampler runs all the time, keeping quarter-second
slices of the last S seconds, and a window holds the S seconds before its
mark as well as the ones after; `RG_PROFILE_AUTO_MS=N` opens a window by
itself when a present comes N ms after the one before and three times the
usual gap, or when the frame rate halves for a few frames (dq8-gfx's new
`setPresentObserver` feeds the frame times to `debug::noteFrameTime`), and
such a window is the hitch and half a second either side. At the gate it
caught the boot, the area load, the field entry (376 ms; the GS worker
building textures and waiting on readbacks, the GIF stage waiting for room
in its queue) and the bench's frame capture. Sampling all the time costs:
at a round a millisecond the VIF1 thread took 16% longer a frame; at the
replay's round every 5 ms, with the sampler at time-critical priority (a
preemption between SuspendThread and ResumeThread froze the suspended
thread for a quantum), 15.7 against 17.2 ms a frame, about 9%. So only
`RogueGalaxy-engasgos.bat` samples all the time; `-perfil` keeps its 20 s
window from the F9. Profiles are numbered on from the files already there
(a new session overwrote the last one's), `-perfil` copies the build to
`build/run/exe-perfil.exe` at each start, and the `[profile]` and `[mark]`
lines go to stderr with the stats, whose lines another stream's printf cut
in two. Three review agents read the uncommitted work and the last commit
(host code, framework patches 0012-0015, documentation). No bug confirmed
in the patches; two cheap hardenings taken: the queue append of 0015 is a
range insert (a failed allocation throws before anything moves) and patch
0016 counts a jump the dispatcher hands to the scheduler as an unwind. Left
as known: the unwind serial follows the scheduler's current thread, which an
interrupt-side rotate or suspend can clear while guest code runs (Rogue
Galaxy calls neither); with the software GS and the worker, a lens-flare
depth read no longer waits for a FINISH (the GPU backend never writes depth
to local memory). The documentation still described the 30 fps game in its
summaries, and a few notes were stale; fixed. Frames byte-identical at 1x.

**Short hitches (2026-10-20 afternoon).** The user marked four in a few
minutes with `RogueGalaxy-engasgos.bat` (the profile launcher with a stats
line every 10 frames; each line now gives the longest frame between two
presents, the longest the main thread was held up, the GPU pipelines
created and what they cost, and the VU1 pairs run on the interpreter;
`tools/bench/hitches.py` turns a log into one row per interval). Two kinds.
The drops to 47-50 fps for a second or so are the VIF1 thread at 87-89%
with more on screen (12,600 draws a 10-frame interval against 8,000-9,000):
the VU1 programs' known limit. The others, at the field's first frames and
once a few seconds into it, had the GIF stage at 70-90% with the VIF1
thread and the GS worker below normal, and frames of 60-120 ms (the gate
bench: one of 629 ms after the load, then 152 ms). A profile of the GIF
thread put most of its own work in reallocating the GS worker's queue:
publishing a stage of commands behind pending ones reserved exactly the
new size, so while the worker was behind every publish of 128 commands
moved the whole queue, up to 16 MB, to a new block, and the presenter
waited on the submission lock meanwhile. Patch 0015 lets the queue grow
geometrically: the frame after the load is 372-391 ms instead of 629 ms
plus a 152 ms one, the GIF thread's time inside the executable over that
window 6% instead of 16%; steady gate numbers unchanged within noise
(VIF1 16.5 to 16.2 ms a frame). The 1x walk's frame is byte-identical.
What is left of that frame is 1,044 texture resolves (readbacks of
render-target pages a texture build reads) in its interval, 0.24-0.28 s
of waits, and 62 texture builds. A mid-field hitch of the second kind did
not come back in a bench that walks and turns the camera at the gate,
with either build, so whether the patch removes it is for the user's next
session to show. Two traps of the bench: the frame capture
(`[gs-census] frame logged`) is a quarter to half a second hitch of its
own, and a build copied under a new file name runs with the driver's
shader cache cold (the first 29 pipelines in 495 ms, not 2 ms). The
sampler now writes a profile window still open when the game is closed
(the user's first window was lost a second before its end).

**The VU1 programs, first cuts (2026-10-20 afternoon).** The sampler
listed the process's threads with a system-wide snapshot every round,
which held it to 13 rounds a second; it now lists them every quarter
second and `RG_PROFILE_THREAD=VIF1` samples one thread (8068 samples in
20 s instead of 263; `PROFILE_STACKS=0` in walk.sh for leaf pcs only). The
busiest routine, `r_a786efabfa469ada_52` (16% of the VIF1 thread at the
gate), is 134 KB of x86 with its time spread thin; its hottest instruction
followed a 16-byte read of a VF register just written as two lane stores,
a store-forwarding stall. Patch 0014 writes registers as one blended
store: 16.2 to 15.9 ms of VIF1 a frame, frames identical. Compiling the
runtime for AVX2 (VEX three-operand forms, no FMA) gave 1% and was taken
out again. A ceiling test that dropped the stall model's bookkeeping
altogether (register readiness, VI readiness; values only, not shipped)
gave 15.0 ms: that model costs about 6%. The rest is the arithmetic with
the PS2's float rules and the VF state kept in memory between pairs; a
real cut there means keeping registers in host registers across pairs,
as PCSX2's microVU does. FINISH no longer drains the GS worker queue
(patch 0013), with no change at the gate. Gate bench: 54-55 fps median
before these, 55.6-56.1 after.

**Two freezes after round four (2026-10-20 morning), and a review.**
Playing the GIF-stage build, the user met a battle whose logic stopped
after a skill while frames kept coming (every interval the same 1195 draws
and 1120 guest dispatches a frame) and, with the stage off, a power that
froze the game at 0 fps. The second was the 60 fps mode's own: the stall
report put the main thread in AbilityModeRun (0x2790E0), which after
SystemVmCall 0xBB8 runs `do {} while (SystemVmUpdate() != 1)` inside one
frame, and `logicStep` answers 0 on a frame it skips; the frame never ends
inside the loop, so a power that began on a skipped frame (half of them)
never did. Four call sites loop like that (three in AbilityModeRun, one in
AttackChainModeUpdate); `src/step_fixes.cpp` runs every call from them as
a whole step. A scripted battle on the bench card (triangle, Abilities,
Flash Sword) shows the cutscene, the battle back and 6 AP spent, with the
stage on and off. Three review agents read the stage and patch 0010 with
the symptoms. The stage: nothing the game reads from the GS changes but
time (SIGNAL and FINISH only set CSR bits, no GS interrupt exists, every
readback drains); the end-of-list interrupt now comes before the GS work,
so the frame time measured by the game follows the EE's frame alone, and a
timing trace at the gate (`TIME_TRACE=1` in walk.sh) shows the play clock at
real time with it (mean 1.08-1.10 vblanks at 54 fps, logic at 30 a second).
The callbacks from threads other than VIF1 now wait for the lists in
flight too, not only for the stage. Patch 0010: one wrong forgiveness,
StrCmpU8 (see patch 0012), which can make a file lookup come back wrong;
that may be what stopped the skill's logic, a wait on a file the lookup
missed, but no dump was taken then. Frames byte-identical at 1x and 3x.

**Throughput, round two (2026-10-19 morning): the emulation threads.**
Profiles of the street view at the player's scale (3, from the menu's
settings.ini) with the new `INPUT=`/`PROFILE_START=` overrides of
`tools/bench/walk.sh`: standing in the street at 3x the GS worker is the
limiter (88-91% of wall, 58.7 fps), with the driver 24%, the CPU patches of
uploads into pages that live targets own 14% (`patchHostWrite`,
`ReadRowCT32`, `writeRunCT32`), draw submission 7%, the caches 5%. At 1x
the same view ran 36-41 fps with the worker at 56%: there the VIF1 thread
(compiled VU1 programs 35%, their flag bookkeeping 7%, the GS queue 5%,
UNPACK 4%, 26% idle) and the game thread (85% busy: the copy of each DMA
chain in `rg::vif1::send` 14%, VU0 microprograms through the interpreter
12%, `advanceEeTimers` at every safe point 5.6%, the dispatch of every call
through `checkpointDue`/`lookupFunction` 11%) set the pace. Three cuts,
rendering byte-identical between the three builds at 1x in the 30 fps
mode (`fl1`..`fl3`):
- patch 0006, the VU1 flags: Rogue Galaxy's microcode images read MAC
  flags in a few places (FMAND 29, FMEQ 13 across all images) and status
  flags nowhere, and nothing outside VU1 can see them; a compiled run of
  an image without such readers (`VU1Interpreter::fmacFlagsWanted`, one
  scan per microcode generation) no longer queues its FMAC results, which
  is most of `commitFlags`. `PS2X_VU_FMAC_FLAGS=1` keeps the old behaviour.
- `src/vif1_thread.cpp`: the chain's tags are walked once for the size
  and the copy happens once into a reserved buffer, which comes back from
  the VIF1 thread with its capacity (a pool of eight) instead of being
  allocated and freed across threads for every list.
- patch 0006, the EE timers: `advanceEeTimers` batches its cycles to 2048
  (7 us) before walking the timers; register reads and writes settle the
  batch first, so the count they see is exact.
At 3x in that street the frame rate did not move (58.7 to 58.5, the
worker being the limiter), but the game thread's profile did: `send` 6.1
to 3.9% with its memcpy from 13% to under 1%, `advanceEeTimers` 5.6 to
1.3%. The view that matters is the player's own: standing in the street
by the save point of File 01, 46 fps at 3x, and there the VIF1 thread is
the limiter (90% busy: the compiled VU1 programs 45%, `commitFlags` still
6.8% because the map's image reads MAC flags and keeps its results
queued, the copy of textures split over DIRECT packets 5%, the GS
frontend 5%; the worker waits 22% of the time, the game thread 35%). A
texture split over DIRECTs now continues in place
(`GS::processImageContinuation`, patch 0006) instead of being copied
behind a synthesized IMAGE tag: that view went from 46 fps (44-47 over
20 intervals, three runs) to 53-58 (run to run; the street at 3x varies
by 3 fps between runs now, so a change needs three runs each way). An
experiment on the worker's side, releasing the pages an upload covers
whole instead of patching them (`DQ8_GFX_COVERED_RELEASE=1`, see the
dq8-gfx README), took the worker from 79% to 63% of the wall without
moving the frame rate and left 915 VRAM bytes different; it stays off. Then the generated VU1 code itself:
`compile_vu_programs.py` now decides pair by pair whether a MAC flag
reader can still see an FMAC's result (backwards over the routine's
blocks: a later FMAC that lands first kills it, a call, register jump or
pair left to the interpreter keeps it, the E bit ends it) and the compiled
pair queues the result only then; 138 of the game's 1775 FMAC pairs keep
their flags. The map's routine went from 14% to 9% of the VIF1 thread,
`commitFlags` from 7% to 3%, and the player's street view from 53-56 to
the 60 cap (three runs), byte-identical frames at 1x and 3x. What is left
on that thread: the pairs' cycle bookkeeping itself (the routines are
still 35% of it), the GS queue (one command per primitive, 3-4%), the
UNPACK shapes outside the plain path. A trap found on the way: the scripted
load picks a save by its position in the menu, and the player's File 02,
saved again that morning, changed what the walk loaded (3155 batches at
the logged frame against the reference's 5820, the scene drawn into
frame buffer 0 instead of the 0x150 pass). `RG_MC0_DIR` (new) points the
memory card elsewhere; walk.sh runs on `build/run/mc0-bench`, a copy
holding only File 01, so the player's card is never read or written by a
run, and the references are remade on it.

**Throughput, round one (2026-10-18 night).** Measured with
`tools/bench/walk.sh`, the scripted Salgin walk on the pad clock: speed
as the six `[gfx]` intervals before the logged frame at 3x in the 60 fps
mode, correctness as the frame at pad read 2660 at 1x in the 30 fps mode
against a reference made with the same build and save
(`build/run/ref1x30`; the save changed at 21:23, so the 10-17 references
are dead). Baseline 50.5 frames a second. Three cuts, each byte-identical
at 1x: the VIF1 thread's XGKICK finishing in bulk once the program has
ended or stalls on its next kick, plus a plain-shape UNPACK path
(`patches/ps2recomp/0005`): 54.5; the GS worker's same-state fast path
for primitives whose draw state repeats the previous one, 78% of the
streets' primitives (`thirdparty/dq8-gfx`, `Impl::submitSameState`):
56.2; page imports between targets copying runs of pages as one copy:
57.9; a second pass over the fast path (no cache call for texels already
acquired, pages by the batch's bounding box) took the worker from 96% to
90% of wall at the same 57 to 58, the walk's own noise being about one
frame a second between runs; an early-out in the texture cache's
invalidation (the union of every entry's pages, so a flush or host
write into pages no texture uses skips the walk over up to 2048 entries)
measured 58.2 in the same noise. The worker is still the limit. The
flushes of the queued draws, now counted by reason in the stats line,
are about 83 a frame in the streets: 46 for texture reads of pages
queued draws write, 17 after recorded feedback draws, 9 for transfers
into queued pages, 8 for views aliasing queued pages; each ends a render
pass, which is where the 230 passes a frame come from. A street-only
profile (`PROFILE=1 LONG=1`, pacing one stretch of street for 40 s)
puts the worker at 86% busy with a quarter of it in host writes landing
in pages live targets own (the CT32 patches: `applyCpuPatches` 11%,
`patchHostWrite` 5%, the VRAM row reads 5%), the driver at 19%, memcpy
at 10%; the VIF1 thread at 78% (compiled VU1 programs 35%, `commitFlags`
7%, the queue to the worker 5%); and the game thread at 78%, a quarter
of that waiting to hand lists to the VIF1 thread. The large CPU patches
now go to the GPU at native size and are blitted scaled (`patchNative`;
the expansion was 24 MB a frame at 3x): the worker's share of the walk
went from 93% to 84% of wall, 58.9 frames a second against a cap of 60,
and the frame at 3x in the 30 fps mode is byte-identical to a reference
made just before (`build/run/ref3x30`; 3x holds 29.9 there now, so
scale-dependent changes can be checked too). The walk's six intervals
sit at the cap now; the long walk's pacing window (`LONG=1`, printed as
intervals 12 to 36 after the marker, 44 to 60 frames a second) is the
speed gauge from here. A trace of three frames (`RG_GS_TRACE_BEFORE=3`)
put 41 of the 46 texture-read flushes a frame on one read: the 512x512
CT32 frame buffer at page 0, sampled by draws into the CT24 buffer at
page 112, with a live target for it that the draw could not bind. A
target behind only on patched host writes now gets them landed and is
sampled live (`settlePatches`): pacing window 54.6 to 56.5 frames a
second, frame identical at 3x, but the flush count did not change, so
the frame buffer is held back by something else (needsUpload, aliasing
queued draws, or the sampled rectangle not fitting); the trace line
prints those now. It was the aliasing, by half a texel: the sampled
rectangle carries bilinear's half texel past the last row, which for a
512x448 frame buffer read in 41 strips lands in the back buffer's first
page row, where that frame's draws are queued, and likewise for an object
buffer read while the next one is drawn. A draw that samples within a
live target now counts only that target's pages against queued draws:
texture-read flushes 47 to 3 a frame, all flushes 83 to 40, render passes
230 to 180, pacing window 56.5 to 58.3 frames a second, both reference
frames byte-identical. What remains of the flushes: 17 a frame after
recorded feedback draws (one snapshot texture, so each such draw lands
before the next snapshot), 10 for transfers into queued pages, 8 for
views aliasing queued pages; and the CT16 buffer at 0x3800 reading
itself in 16 strips reaches two rows past its end, one more than the
snapshot padding covers. Left on
the list: the GS worker's draw submission and texture-cache bookkeeping,
the render pass count, the VIF1 queue's per-primitive command and
copies, and the compiled VU1 programs themselves. A profile taken by the
walk (`PROFILE=1`) is diluted by the loading and menu stretches around
it; the user's play profile is the better read.

## Open questions

- What leaves the data table's VM pointer stale before the Story hang.
- The two `sceCdGetDiskType` entries (0x12A9D8, called by the game; 0x12AA70,
  the wrapper ps2_analyzer names) are both bound; phase 3 confirms which is
  the public one.

## References

- [RGDecomp](https://github.com/Dorgengoa/RGDecomp) (CC0): matching
  decompilation of this exact build. `config/*.symbols.txt` gives names,
  addresses and sizes for the main ELF and each overlay, from a Ghidra export
  of 14,620 functions: the seed for the function maps.
- [DQ8-Recompiled](https://github.com/Sinan-Karakaya/DQ8-Recompiled) (GPL-3.0):
  same overlay format, compiler and SDK generation. Overlay manager,
  `mwo3_to_elf.py`, HD6 and MVI tools, SDL GPU backend, launcher; its SDK
  binding list seeded ours.
- [DC2-PS2RECOMP](https://github.com/Red-tv141/DC2-PS2RECOMP): Dark Cloud 2
  (Level-5, same compiler family) brought to a playable state on PS2Recomp.
- PCSX2 [GameIndex.yaml](https://github.com/PCSX2/pcsx2/blob/master/bin/resources/GameIndex.yaml)
  and [pcsx2_patches](https://github.com/PCSX2/pcsx2_patches)
  (`SCUS-97490_0643F90C.pnach`).
