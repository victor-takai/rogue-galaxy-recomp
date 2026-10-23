# Rogue Galaxy Recompiled

Static recompilation of **Rogue Galaxy** (Level-5 / SCE, PlayStation 2) to a
native PC executable, built on [PS2Recomp](https://github.com/ran-j/PS2Recomp).

| | |
|---|---|
| Release | NTSC-U, **SCUS-97490** v1.01 (one DVD9) |
| Framework | [Sinan-Karakaya/PS2Recomp](https://github.com/Sinan-Karakaya/PS2Recomp) fork, pinned in `thirdparty/PS2Recomp` |
| State | **Runs**: title screen, opening, field and battles at 60 fps by default (the menu's 60 frames per second; the scene's step follows the frame time, so a slow frame does not play in slow motion), 50 to 60 at a boss gate, the heaviest field scene measured, where the VU1 programs are the limit; GS on the GPU, VU1 compiled ahead of time and on its own thread, music and sound. See [`docs/STATUS.md`](docs/STATUS.md). |

## Setup

Requirements: Git, Python 3.11+, CMake 3.24+ with Ninja, clang with C++20,
and your own dump of the game ([`docs/DISC.md`](docs/DISC.md) lists the one
that works).

```bash
git clone --recurse-submodules <this repo>
cd rogue-galaxy-recomp

# Read-only check of the dump (add --full to compare the whole image with Redump)
python setup.py verify --iso "/path/to/Rogue Galaxy (USA).iso"

# Unpack both DVD layers into disc/ (about 6.3 GiB), hashing every file
python setup.py extract --iso "/path/to/Rogue Galaxy (USA).iso"

# SYSTEM.CNF and the MWo3 overlay table, cross-checked against the ELF
python setup.py info

# Check the function maps and recompiler configs against your disc
python setup.py check
```

## Build

From the repository root. The generated code is built with clang (the flags
are GNU-style; on Windows an LLVM MinGW clang works). Configuring the game
downloads raylib, SDL3 and glslang (for the GPU backend's shaders) and, on
Windows, a prebuilt LGPL FFmpeg whose hash the framework pins.

```bash
# 1. The recompiler, from the pinned framework (once)
cmake -S thirdparty/PS2Recomp -B build/tools -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DPS2X_BUILD_RUNTIME=OFF -DPS2X_BUILD_TEST=OFF -DPS2X_BUILD_STUDIO=OFF
cmake --build build/tools --target ps2_recomp ps2_analyzer

# 2. Recompile your disc's code to C++ in build/generated/<image>/ (about a
#    minute): runs `check`, wraps the overlays, runs ps2_recomp on all six images
python setup.py recompile

# 3. Build the game against the framework runtime
cmake -S . -B build/game -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build/game

# 4. Run it on your extracted disc
build/game/src/roguegalaxy disc/SCUS_974.90
```

Run `setup.py recompile` again after changing a map or config, then rebuild.

**VU1 programs.** 3D scenes need the game's VU1 microprograms compiled ahead
of time to run at full speed. `setup.py recompile` rebuilds them from
[`config/SCUS_974.90/vu1-programs.txt`](config/SCUS_974.90/vu1-programs.txt)
(where each one sits in the game's files, plus a checksum: no game data) and
your disc into `build/vu1-manifest`, which CMake compiles into the runtime.
Programs the manifest lacks run on the interpreter; record them with
`PS2_VU_PROGRAM_PROFILE=build/vu1-profile` (CMake compiles that folder too),
then configure and build again (maintainers: `python setup.py vu1-manifest`
folds a recording into the manifest). The VU0 micro programs, which run on
the game thread, are compiled as blocks instead: a run with
`PS2_VU_AOT_PROFILE=build/vu-blocks/NAME.txt` records the blocks the
interpreter executes (both units; keep the lines starting with `0` for
VU0 alone), and CMake compiles every `build/vu-blocks/*.txt` it finds
(one shard at a time: each takes gigabytes to compile).

## Controls

The game window takes a keyboard or a gamepad (PlayStation layout).

| Pad | Keyboard |
|---|---|
| Left stick | W A S D |
| D-pad | Arrow keys |
| ✕ / ○ / □ / △ | X or Space / C or Esc / Z / V |
| L1 / R1 / L2 / R2 | Q / E / Left Shift / Right Shift |
| Start / Select | Enter / Tab |
| Right stick | I J K L |

**F1** (or the Guide button, or Back+Start) opens the in-game menu, from
DQ8-Recompiled: window and fullscreen, vertical sync, internal resolution,
aspect and scaling filter, volume, controller and keyboard remapping with
dead zones, pause (F2), fast-forward (F4), a frame-rate counter (F3),
screenshots (F12) and renderer statistics, and under Interface > Game the
game's own switches:
- "Widescreen": the game draws a wider view, with the HUD over the
  scene, the compass and the movies fitted to it (events keep their
  letterbox bars, which their text is placed by); with Display > Aspect
  ratio on Auto the picture fills the window, whatever its shape from 4:3
  to 32:9, once a game is loaded (the title, load menu and loading screen
  stay 4:3). Menus and text are drawn for 4:3 and come out stretched.
- "Skip the opening": at boot the Sony, Japan Studio and Level-5 logos give
  way to the title screen at once.
- "Show the loading recap at once": the whole story so far on the loading
  screen at once, instead of line by line, and Press X comes as soon as the
  game has loaded.

F11 or Alt+Enter toggles fullscreen. The settings live in
`%APPDATA%\RogueGalaxy\Recompiled\settings.ini` (`RG_SETTINGS_FILE` names
another file).

The game runs at 60 frames per second unless the menu's Interface > Game >
60 frames per second is off (the game's own 30; a change takes effect at the
next start; `RG_FPS=30` or `RG_FPS=60` decides for one run). On a 144 Hz
monitor a frame lasts 2 or 3 refreshes at 60 and 4 or 5 at 30, which reads
as judder, so 120 Hz or variable refresh looks smoother.

Larger 2D art: the letters, menu labels drawn as pictures, icons, window
pieces and help pages can be drawn from larger versions made on the machine
(`src/texture_pack.h`). `textures\RogueGalaxy-collect.bat` plays the game
while writing each of them that appears on the screen once into
`textures\collected`, named by its content; the folder only grows with what
has not been seen yet. `textures\make-pack.bat` then enlarges them 4x into
`textures\hd`, which the game reads when it starts: the text among them (the
fonts' glyphs and the menus' labels) with an image upscaler, Upscayl's
UltraSharp model where Upscayl is installed (or Real-ESRGAN's
`realesr-animevideov3`, its Windows build from
github.com/xinntao/Real-ESRGAN/releases extracted into `textures\realesrgan`),
made ready for it in `textures\to-upscale` (colour and transparency apart,
small ones padded); the icons, portraits and window pieces with xBR, which
keeps their pixels' shapes and smooths the staircases between them. The help
pages stay as they are. `make-pack.bat xbr` enlarges the text with xBR too,
`make-pack.bat all` everything with the upscaler. The menu's Interface >
Game > "Larger 2D art" turns it off. All of it is made from the disc, so it stays on the
machine: `.gitignore` keeps `textures\*\` out of the repository.

## Running

On Windows, double-click `RogueGalaxy.bat` (or run it from a terminal): it
starts the built game on `disc/SCUS_974.90` and writes the game's log to
`build/last-run.log`. The build copies the FFmpeg and clang runtime DLLs
next to `roguegalaxy.exe`, so it needs nothing on PATH. To set any of the
variables below, add a `set` line to the batch file (it has examples) or set
them in the terminal before running it. The batch file also sets
`VK_LOADER_LAYERS_DISABLE=~implicit~`, which keeps other programs' Vulkan
layers (OBS's capture hook, ReShade, the Steam, GOG and Epic overlays) out
of the game's process; set it yourself when running from a terminal.
The launchers in `debug\` are for diagnosis, not for playing.
`debug\RogueGalaxy-profile.bat` runs the same game with the profiler ready:
each F9 samples every thread for the next 20 seconds
(`RG_PROFILE_ON_MARK=1`, into `build/run/play-profile-1.prof`, `-2`, ...,
numbered on from the files already there), so a profile holds the moment
the player marked; report it with `tools/profile/report.py` against
`build/run/exe-profile.exe`, the copy of the build it was recorded with,
which the launcher makes at each start (a report of an older session's
profile needs that session's build). `RG_PROFILE_BEFORE` makes the windows
reach back before the F9 (`debug\RogueGalaxy-hitches.bat`, below); without
`RG_PROFILE_ON_MARK` the window is fixed (`RG_PROFILE_START`,
`RG_PROFILE_SECONDS`). It also prints a `[gfx]` line a second
into `build/last-run.log` (`DQ8_GFX_STATS_EVERY=60`; the worker's share
of the wall is "backend", the VIF1 thread's is the "vif1 busy" line, the
game thread's load the "ee-dispatches" per frame), and F9 writes a
`[mark]` line with the seconds since launch (any launcher), so pressing
it when a drop is felt puts the drop next to the `[gfx]` lines of that
moment. F10 writes the stall watchdog's report at once (the guest
threads and what each waits on, the semaphores' counts, the host
stacks), for a freeze the watchdog does not see by itself because the
screen keeps flipping; the watchdog also reports on its own when the
main thread has not moved for 15 s, and the runtime logs a `[ee]` line
when a guest thread ends with pc 0 or the main thread exits. A frozen
process can be dumped with `tools\hang\dumpmem.ps1` (its guest RAM and
thread contexts) and a thread's call chain read from the dump with
`python tools/hang/stackwalk.py DUMPDIR SP RA`.

`debug\RogueGalaxy-hitches.bat` is the profile launcher with a stats line every 10 frames, for short hitches: each line also gives the longest frame of its interval (the longest gap between two presents), the longest the main thread was held up ("main stall"), the GPU pipelines created in it and what creating them cost (a first-time state compiles one), and the VU1 pairs that ran on the interpreter (a program the build lacks; the session records it into `build/vu1-profile`, see VU1 programs above). Its profiler samples all the time (`RG_PROFILE_BEFORE=10`; about 9% of the VIF1 thread's time at the boss gate, so it is for hunting hitches, not for playing): it writes a profile by itself at a hitch nobody marked (`RG_PROFILE_AUTO_MS=50`: a frame of 50 ms or more and three times the usual, or the frame rate halved for a few frames), of the hitch and half a second either side, up to 30 a session, and the log says so in a `[profile] window N: a hitch` line; an F9 window holds the 10 seconds before the press and the 5 after. Press F9 at each hitch felt, then read the lines around the `[mark]` (`python tools/bench/hitches.py build/last-run.log`, one row per interval); a profile window still open when the game is closed is cut short and written.

| Variable | Effect |
|---|---|
| `RG_GS=sw` | Software GS rasterizer instead of the SDL3 GPU backend |
| `RG_GS_SCALE=N` | GPU internal resolution multiplier for this run (the menu's saved setting otherwise, default 2) |
| `RG_GS_NO_WINDOW=1` | Present through the runtime's window instead of SDL's |
| `RG_GS_WORKER=0` | Run the GS backend on the game's thread instead of its own worker thread |
| `RG_SHOW_FPS=1` | The menu's frame-rate counter on from the start (in the window title when the runtime presents) |
| `RG_PRESENT_MODE=vsync\|immediate` | Present mode for this run (the menu's setting otherwise, default mailbox) |
| `RG_SETTINGS_FILE=path` | The menu's settings file (default `settings.ini` in `%APPDATA%\RogueGalaxy\Recompiled`) |
| `RG_UI_SCRIPT=file\|text` | Drive the menu for tests and screenshots: `<guest frame> <command> [args]` entries, `;`-separated (see `thirdparty/dq8-gfx/src/ui/ui_script.cpp`) |
| `RG_PRESENT=vblank` | Present at host vblanks instead of on the game's flips |
| `RG_VIF1_THREAD=0` | Run VIF1 lists (VU1) on the game's thread instead of their own |
| `RG_GIF_THREAD=0` | Run the GS packets of the VIF1 lists on the VIF1 thread instead of a GIF stage of their own |
| `RG_FPS=60` | The 60 fps mode for this run whatever the menu's 60 frames per second says (on by default; `RG_FPS=30`, or any other value, the game's own 30): the game flips every vblank and the scene's step follows the measured frame time (half at 60, whole at 30), so it plays at its normal speed at whatever rate the host reaches (movies and the loading screen keep 30); `RG_TIME_SCALE=fixed` keeps half steps regardless, `RG_TIME_TRACE=1` logs the frame times and the play clock, `RG_STEP_FIXES=0` leaves the systems that count frames (play clock, minimap, NPCs, projectiles, gravity and the jump's flight, movie fades) as the game has them, or a comma list of `clock`, `movie`, `npc`, `judge`, `gravity`, `event`, `menu`, `ai`, `texture` picks which take the step (`event`: the scripts, the event player and the message windows count frames, so they run on the frames that complete a whole step, and at once when a button is pressed; the field modes that hold the scale for a scripted sequence, chests, talks and warps, take the half steps too; `menu`: the field's menus and HUD windows, the battle results among them, and the chests' opening count frames as well and run on the same frames; `ai`: the characters' AI, enemies' and allies', counts its timers in frames and moves them by a step a run, so it runs on those frames too, each move shared with the next frame; `texture`: the texture animations, most of them stepped a whole frame a draw, take the frame's share, and the layers the data holds still stay still) |
| `RG_TIME_SCALE=game` | Let the game take bigger logic steps after frames the host was too slow for, as on the console (off by default: slow frames play in slow motion, as in an emulator) |
| `RG_EE_CVT=round` | Diagnostic: float-to-int conversions round, as the runtime used to, instead of truncating as the EE does |
| `RG_MOVIE_AUDIO=game` | The game's own movie PCM callback, which drops packets the runtime's demux delivers too early (noise) |
| `PS2X_EE_ROUNDING=nearest` | Diagnostic: guest float math rounds to nearest, the host default, instead of toward zero as the EE does (patch 0003) |
| `RG_GS_DEPTH_READS=queue` | Serve the game's depth readbacks through the GS worker queue |
| `RG_MUTE=1` | No sound output (the IOP still runs) |
| `RG_INPUT="30:CROSS:150,..."` | Scripted pad input: seconds, buttons (`+`-joined; `LUP` etc. for the left stick), milliseconds; with `RG_INPUT_CLOCK=pad` both numbers count the game's pad reads, so a script replays alike at any host speed |
| `RG_TRACE=0xADDR,...` | Log calls of main-ELF functions with guest thread, sp, ra and a0-a3 (`RG_TRACE_PEEK` words, `RG_TRACE_ARG0` offsets from a0, `RG_TRACE_LIMIT` calls each, default 16) |
| `RG_DUMP=0xFUNC:file` | Append each call of a `FUNC(dst, src, size)` to file: dst, size and the bytes at src |
| `RG_WATCH=0xADDR,...` | Print guest words, I/O registers included, every `RG_WATCH_MS` (default 1000) |
| `RG_HWWATCH=0xADDR` | x86 debug register on a guest word: writes leaving a value in `RG_HWWATCH_RANGE=lo-hi` (default the main stack) print the writer's stack (`RG_HWWATCH_READS=1`, `RG_HWWATCH_READER=0xFUNC`, `RG_HWWATCH_DELAY=s`); Windows only |
| `RG_PROFILE=out.txt` | Sample all threads for `RG_PROFILE_SECONDS` (default 20) from `RG_PROFILE_START` seconds in (default 0); report with `tools/profile/report.py` (`RG_PROFILE_STACKS=1` also records the callers of samples inside the executable, which the report skips) |
| `RG_PROFILE_THREAD=NAME` | With `RG_PROFILE`: sample only the threads whose name starts with NAME (`VIF1`, `GIF`, `GSWorker`, `GameThread`), for many more samples of one |
| `RG_PROFILE_ON_MARK=1` | With `RG_PROFILE`: a window of `RG_PROFILE_SECONDS` at each F9 instead, into `out-1.txt`, `out-2.txt`, ... (numbered on from the last file already there; an F9 during an open window is folded into it) |
| `RG_PROFILE_BEFORE=S` | With `RG_PROFILE_ON_MARK=1`: sample all the time (every 5 ms; about 9% of the VIF1 thread at the boss gate) and put the S seconds before each F9 into its window too |
| `RG_PROFILE_AUTO_MS=N` | With `RG_PROFILE_BEFORE`: a hitch opens a window by itself (a frame N ms long and three times the usual, or the frame rate halved for a few frames), of the hitch and half a second either side, at most 30 a run |
| `RG_PROFILE_INTERVAL_MS=N` | The pause between two sampling rounds (default 1 for a window, 5 when sampling all the time with `RG_PROFILE_BEFORE`) |
| `RG_STALL_SECONDS=15` | When no frame has been flipped for that long while the game is not paused, the log gets every guest thread's state (what it waits on; the running one's registers and the addresses it passes over the next second) and every host thread's stack (`exe!0x...` frames resolve with `llvm-symbolizer --obj=build/game/src/roguegalaxy.exe`, from the toolchain's `bin`), again every minute; `RG_STALL_WATCH=0` turns the watchdog off |
| `RG_VU0_FLAGS=0` | Keep the generated code for the game's three VU0 macro-mode bounding-box tests (`AabbOverlap`, `AabbContains`, `AabbContainsPoint`) instead of the host versions in `src/vu0_flags.cpp`; the recompiler updates the VU0 status and MAC flags in the functions that read them (`patches/ps2recomp/0017`), so both answer alike. `=check` runs the generated code and counts where it disagrees with the host versions |
| `RG_POS_TRACE=1` | Log every call that puts a character somewhere it did not walk to (`CharaSetPos`, `CharaSetPosXYZ`, `CharaPlaceAt`: character, id, old and new position, caller), and any character still falling after 90 movement updates, with where it fell from |
| `RG_MC0_DIR=path` | Memory card folder instead of `disc/mc0` (`mc1` is its sibling). The benchmarks point it at `build/run/mc0-bench`, a copy holding only the save they load, since a second save changes what the scripted load menu picks |
| `RG_FORCE_ENCOUNTER=1` | A battle at the first steps wherever the field has enemies: the encounter countdown is cut before each tick (`src/debug/encounter.cpp`), and each battle's start is logged with its pad read; the battle bench (`walk.sh BATTLE=1`) sets it |
| `RG_JUMP_TRACE=1` | Log the party leader's movement updates while off the ground (host time, position, step-scaled velocity, the motion's displacement, the action mode and the gravity flags, one line per `CharaMoveCollideMaybe`; `=2` logs the moves on the ground too, for the speed ramps; `=3` every character's, tagged with address and id, and for enemies every move with what drives it and every move request with its caller; `=4` every character's moves on the ground too, for a party following its leader): compares a jump's arc and timing between the 30 and 60 fps modes |
| `RG_WIDESCREEN=1` | The game drawn for a wide screen of the window's shape (the menu's "Widescreen"; `src/widescreen.cpp`) |
| `RG_SKIP_OPENING=1` | The logos at boot give way to the title screen (the menu's "Skip the opening") |
| `RG_GS_CAPTURE=DIR` | F7 writes the next frame's batches, GS memory and the renderer's target trace (`RG_GS_FRAMELOG`'s files, as `DIR/capture-N.txt`); `debug\RogueGalaxy-capture.bat` sets it to `build\captures`, for a picture that looks wrong |
| `RG_CROP=N` | GS pixels left off each edge of the picture (default 2: the border the game leaves black or half drawn, which a television's overscan hid; 0 shows the whole frame) |
| `RG_TEXTURE_PACK=DIR` | The larger 2D art read from `DIR` instead of `textures\hd` (`src/texture_pack.h`) |
| `RG_TEXTURE_DUMP=DIR` | Write each new region a sprite draws on the screen from a texture into `DIR` as `TEXELS_CLUT_WxH.png` (`textures\RogueGalaxy-collect.bat`; `src/texture_pack.h`) |
| `RG_INSTANT_RECAP=1` | The loading screen's story so far appears whole at once, then Press X (the menu's "Show the loading recap at once") |
| `RG_STICK_SQUARE=0` | Hand the game the host's round analog stick range as it is. By default `src/stick_response.cpp` reshapes both sticks onto the DualShock 2's square range (the direction and the pushed distance stay, the distance counts as the longer axis, so a full diagonal reads full on both), which the game's run threshold was tuned for: with a round stick the party only runs within about 20 degrees of each of the four directions and walks in between |
| `RG_GS_TIMING=1` | Time every GS backend call on both sides of the worker queue, every 2 s |
| `RG_GS_CENSUS=1` | Every 2 s, the draw states the GPU backend only approximates (destination alpha test and the like), and draws whose scissor is wider than their buffer; `RG_GS_SKIP=MASK:VALUE` drops draws by TEST register |
| `RG_GS_FRAMELOG=file@N` | Every batch, transfer and clear of one frame, once the game's pad read count reaches N, with GS memory as `file.vram` (`@N#K` also before batch K); `RG_GS_FRAMESKIP=A-B,...` leaves batches of that frame out (`A-` to the end); `RG_GS_TRACE_BEFORE=M` turns the backend's target trace (`DQ8_GFX_TRACE_TARGETS`, a line per transfer and ownership move) on from M pad reads before that frame to its end |
| `RG_SELFTEST=gs` | Check the code changed in vendored dependencies, then exit |
| `DQ8_GFX_SCREENSHOT_DIR`, `DQ8_GFX_SCREENSHOT_EVERY` | Framework: write every Nth frame as PPM |
| `PS2X_AUDIO_DUMP=file` | Framework: copy of the audio output, raw 48 kHz s16 stereo |

## Layout

| Path | What |
|---|---|
| `setup.py` | `verify`, `extract`, `info`, `overlays`, `check`, `recompile` (and `make-hashes`, `maps` for maintainers) |
| `CMakeLists.txt`, `src/` | The game executable: build of the generated trees, overlay manager (`src/overlay/`), call tracing (`src/debug/`), `main.cpp` |
| `config/SCUS_974.90/` | `hashes.json` (known-good dump), `overlays.toml` (MWo3 table, loader hook), `funcmap.toml` (map sources and corrections), `functions_<image>.csv` (function maps), `recomp/<image>.toml` (ps2_recomp configs, SDK bindings), `vu1-programs.txt` (VU1 microcode locations) |
| `tools/disc/iso9660.py` | Read-only ISO9660 reader for dual-layer PS2 DVDs |
| `tools/mwo3/mwo3.py` | MWo3 overlay header parser, ELF cross-check, overlay-to-ELF wrapper |
| `tools/funcmap/` | Function-map builder and the checks behind `setup.py check` |
| `tools/bench/` | `walk.sh NAME [SCALE] [FPS]`: the scripted Salgin walk on the pad clock, for speed (3x, 60 fps mode, the `[gfx]` intervals before the logged frame) and for regressions (1x, 30 fps mode, `cmpvram.py` against a reference frame made with the same save); `PROFILE=1` samples the threads as well |
| `thirdparty/PS2Recomp` | Framework submodule; `patches/ps2recomp/` holds our changes to it, applied on configure ([`patches/README.md`](patches/README.md)) |
| `thirdparty/dq8-gfx` | SDL3 GPU GS backend and in-game menu from DQ8-Recompiled (GPL-3.0), see its README |
| `thirdparty/imgui` | Dear ImGui submodule (MIT), which the menu draws with |
| `docs/` | [`STATUS.md`](docs/STATUS.md) (state, plan, what the game needs), [`DISC.md`](docs/DISC.md), [`MAPS.md`](docs/MAPS.md) (function maps, SDK bindings) |
| `disc/` | Your extracted disc (gitignored) |

## Legal

You must own the game. No game data, BIOS or disc-derived files are part of
this repository: `disc/` and disc images are gitignored and must never be
committed.

## Credits

- [PS2Recomp](https://github.com/ran-j/PS2Recomp) by ran-j, and the
  [fork](https://github.com/Sinan-Karakaya/PS2Recomp) by Sinan Karakaya.
- [DQ8-Recompiled](https://github.com/Sinan-Karakaya/DQ8-Recompiled) by Sinan
  Karakaya, the reference for MWo3 overlays on PS2Recomp, and the source of
  the SDL3 GPU GS backend and the in-game menu in `thirdparty/dq8-gfx`.
- [Dear ImGui](https://github.com/ocornut/imgui) by Omar Cornut (MIT), which
  the menu is drawn with, and its Roboto Medium font (Apache 2.0).
- [RGDecomp](https://github.com/Dorgengoa/RGDecomp) (CC0), the function maps
  and names for this build (`config/SCUS_974.90/functions_*.csv` derive from
  it).
- [Redump](http://redump.org/disc/6567/) and [PCSX2](https://github.com/PCSX2/pcsx2)
  (GameDB, patches) for disc and hardware notes.

## License

GPL-3.0, the same as PS2Recomp, which the built executable links. See
[`LICENSE`](LICENSE).
