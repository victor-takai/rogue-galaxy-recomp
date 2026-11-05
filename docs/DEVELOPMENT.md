# Running and diagnosis

To change what the game does, see [HOOKS.md](HOOKS.md). To find the code
and data behind something in the game, see
[FINDING-THINGS.md](FINDING-THINGS.md).

## Launchers

On Windows, double-click `Rogue Galaxy.exe`, which the build puts in the
repository's folder: it starts the built game on `disc/SCUS_974.90`, writes
the game's log to `build/last-run.log` (the run before survives as
`build/last-run.prev.log`) and opens no terminal; a game that ends with an
error shows the log's last lines in a message box. `debug\RogueGalaxy.bat`
does the same in a terminal, which it keeps open until the game closes. The
log still goes to `build/last-run.log`, not to the terminal: to follow it as
it is written, run `Get-Content -Wait build\last-run.log` in a second one.
On an error the terminal shows the log's last lines. The build copies the FFmpeg and clang
runtime DLLs next to `roguegalaxy.exe`, so it needs nothing on PATH. To set
any of the variables below, set them in Windows (they reach the game through
either launcher), add a `set` line to the batch file (it has examples), or set
them in the terminal before running it. Both launchers also set
`VK_LOADER_LAYERS_DISABLE=~implicit~`, which keeps other programs' Vulkan
layers (OBS's capture hook, ReShade, the Steam, GOG and Epic overlays) out
of the game's process; set it yourself when running from a terminal.
`Rogue Galaxy.exe` leaves it unset under Wine or Proton, where it would
reach the host's loader and keep out the layers the picture needs there.
Both launchers also set `DQ8_GFX_STATS_EVERY=60` unless it is set already:
a `[gfx]` line a second in `build/last-run.log` (the worker's share of the
wall is "backend", the VIF1 thread's is the "vif1 busy" line, the game
thread's load the "ee-dispatches" per frame).
The launchers in `debug\` are for diagnosis, not for playing.
`debug\RogueGalaxy-profile.bat` runs the same game with the profiler ready
(Windows only): each F9 samples every thread for the next 20 seconds
(`RG_PROFILE_ON_MARK=1`, into `build/run/play-profile-1.prof`, `-2`, ...,
numbered on from the files already there), so a profile holds the moment
the player marked; report it with `tools/profile/report.py` against
`build/run/exe-profile.exe`, the copy of the build it was recorded with,
which the launcher makes at each start (a report of an older session's
profile needs that session's build). `RG_PROFILE_BEFORE` makes the windows
reach back before the F9 (`debug\RogueGalaxy-hitches.bat`, below); without
`RG_PROFILE_ON_MARK` the window is fixed (`RG_PROFILE_START`,
`RG_PROFILE_SECONDS`). F9 also writes a `[mark]` line with the seconds
since launch (any launcher), so pressing it when a drop is felt puts the
drop next to the `[gfx]` lines of that moment. F10 writes the stall
watchdog's report at once (the guest
threads and what each waits on, the semaphores' counts, the host
stacks), for a freeze the watchdog does not see by itself because the
screen keeps flipping; the watchdog also reports on its own when the
main thread has not moved for 15 s, and the runtime logs a `[ee]` line
when a guest thread ends with pc 0 or the main thread exits. A frozen
process can be dumped with `tools\hang\dumpmem.ps1` (its guest RAM and
thread contexts) and a thread's call chain read from the dump with
`python tools/hang/stackwalk.py DUMPDIR SP RA --exe build/game/src/roguegalaxy.exe`
(the build that froze; the script runs `llvm-nm`, from the toolchain's
`bin`, so that folder must be on PATH).

`debug\RogueGalaxy-hitches.bat` is the profile launcher with a stats line every 10 frames, for short hitches: each line also gives the longest frame of its interval (the longest gap between two presents), the longest the main thread was held up ("main stall"), the GPU pipelines created in it and what creating them cost (a first-time state compiles one), and the VU1 pairs that ran on the interpreter (a program the build lacks; the session records it into `build/vu1-profile`, see VU1 programs in [BUILDING.md](BUILDING.md)). Its profiler samples all the time (`RG_PROFILE_BEFORE=10`; about 9% of the VIF1 thread's time at the boss gate, so it is for hunting hitches, not for playing): it writes a profile by itself at a hitch nobody marked (`RG_PROFILE_AUTO_MS=50`: a frame of 50 ms or more and three times the usual, or the frame rate halved for a few frames), of the hitch and half a second either side, up to 30 a session, and the log says so in a `[profile] window N: a hitch` line; an F9 window holds the 10 seconds before the press and the 5 after. Press F9 at each hitch felt, then read the lines around the `[mark]` (`python tools/bench/hitches.py build/last-run.log`, one row per interval); a profile window still open when the game is closed is cut short and written.

`debug\RogueGalaxy-capture.bat` sets `RG_GS_CAPTURE` to `build\captures`:
F7 then logs the next frame, for a picture that looks wrong.
`debug\RogueGalaxy-collect.bat` sets `RG_TEXTURE_DUMP` to
`textures\collected`, for making a texture pack
([TEXTURE_PACKS.md](TEXTURE_PACKS.md)).

## Variables

They come in three groups: the menu's options for one run, the tools for
diagnosis, and the renderer's internals.

### Player options

Most of these set an option of the F1 menu for one run, without saving it.

| Variable | Effect |
|---|---|
| `RG_SETTINGS_FILE=path` | The menu's settings file (default `settings.ini` in `%APPDATA%\RogueGalaxy\Recompiled`) |
| `RG_GS_SCALE=N` | GPU internal resolution multiplier for this run (the menu's saved setting otherwise, default 2) |
| `RG_SHOW_FPS=1` | The menu's frame-rate counter on from the start (in the window title when the runtime presents) |
| `RG_PRESENT_MODE=vsync\|immediate` | Present mode for this run (the menu's setting otherwise, default mailbox) |
| `RG_FPS=60` | The 60 fps mode for this run whatever the menu's 60 frames per second says (on by default; `RG_FPS=30`, or any other value, the game's own 30): the game flips every vblank and the scene's step follows the measured frame time (half at 60, whole at 30), so it plays at its normal speed at whatever rate the host reaches (movies and the loading screen keep 30); `RG_TIME_SCALE=fixed` keeps half steps regardless, `RG_TIME_TRACE=1` logs the frame times and the play clock, `RG_STEP_FIXES=0` leaves the systems that count frames (play clock, minimap, NPCs, projectiles, gravity and the jump's flight, movie fades) as the game has them, or a comma list of `clock`, `movie`, `npc`, `judge`, `gravity`, `event`, `menu`, `ai`, `texture`, `camera` picks which take the step (`event`: the scripts, the event player and the message windows count frames, so they run on the frames that complete a whole step, and at once when a button is pressed; the field modes that hold the scale for a scripted sequence, chests, talks and warps, take the half steps too; `menu`: the field's menus and HUD windows, the battle results among them, the chests' opening and the doors' orange triangles count frames as well and run on the same frames; `ai`: the characters' AI, enemies' and allies', counts its timers in frames and moves them by a step a run, so it runs on those frames too, each move shared with the next frame; `texture`: the texture animations, most of them stepped a whole frame a draw, take the frame's share, and the layers the data holds still stay still; `camera`: the camera's turn by the right stick (by both sticks in the first-person view), a frame's worth each frame, takes the frame's share, and so does the free camera's move) |
| `RG_TIME_SCALE=game` | Let the game take bigger logic steps after frames the host was too slow for, as on the console (off by default: slow frames play in slow motion, as in an emulator) |
| `RG_MUTE=1` | No sound output (the IOP still runs) |
| `RG_WIDESCREEN=1` | The game drawn for a wide screen of the window's shape (the menu's "Widescreen"; `src/widescreen.cpp`) |
| `RG_MINIMAP_SIZE=N`, `RG_COMPASS_SIZE=N` | The field's minimap and compass, percent of the game's size, 50 to 100 (the menu's "Minimap size" and "Compass size"; `src/hud_size.h`) |
| `RG_BUTTONS=xbox` | The game's text shows an Xbox controller's buttons whatever is connected (`src/button_glyphs.h`; the menu's "Controller's buttons" follows the controller in use); `RG_GAIJI_DUMP=FILE` writes the picture of special characters they are drawn in (gaiji2.img) as loaded |
| `RG_SKIP_OPENING=1` | The logos at boot give way to the title screen (the menu's "Skip the opening") |
| `RG_FIELD_OF_VIEW=N` | The camera's field of view in the field and in battle, degrees across a 4:3 picture (the menu's "Field of view"; the game's is 52) |
| `RG_CAMERA_DISTANCE=N` | The camera's distance from the party, percent of the game's (the menu's "Camera distance") |
| `RG_FASTER_CAMERA=1` | The right stick turns the camera twice as fast (the menu's "Faster camera rotation") |
| `RG_DRAW_DISTANCE=N` | How far the scenery and objects are drawn, percent of the game's (the menu's "Draw distance") |
| `RG_ENCOUNTER_RATE=N` | Random battles, percent of the game's rate; 0 turns them off (the menu's "Encounter rate") |
| `RG_BATTLE_EXPERIENCE=N` | A won battle's experience, percent of the game's (the menu's "Experience from battles") |
| `RG_BATTLE_MONEY=N` | The defeated enemies' money, percent of the game's (the menu's "Money from battles") |
| `RG_ITEM_DROPS=N` | The chance of each item defeated enemies leave, percent of the game's (the menu's "Item drops") |
| `RG_WEAPON_GROWTH=N` | What a won battle adds to each weapon's gauge and then attributes, percent of the game's 1, in whole steps (the menu's "Weapon growth") |
| `RG_SHOP_PRICES=N` | What shops ask for their items, percent of the game's (the menu's "Shop prices") |
| `RG_ACTION_GAUGE=N` | How fast the party's action gauge fills, percent of the game's (the menu's "Action gauge speed") |
| `RG_FREE_CAMERA=1` | Starts with the free camera on (the menu's "Free camera"; `src/free_camera.h`) |
| `RG_INSTANT_TEXT=1` | Text in the message windows a page at a time (the menu's "Instant text"; `src/instant_text.h`) |
| `RG_INSTANT_RECAP=1` | The loading screen's story so far appears whole at once, then Press X (the menu's "Show the loading recap at once") |
| `RG_VOICE_LANG=JA` | Voices (and the movies that differ) from `disc/JA`, whatever the menu's "Undub (Japanese voices)" says; any language of the game's table works for the voice token (`src/voice_language.h`). Logs `[rg] voice <path>` for each voice asked |
| `RG_MAP_CHESTS=1` | The chests not yet opened on the maps before a planet is complete (the menu's "Chests on the map"; `src/map_chests.h`) |
| `RG_FULL_BAG_TO_STORAGE=1` | An item the bag has no room for goes to the transporter's item storage (the menu's "Full bag to storage"; `src/inventory.h`) |
| `RG_STACK_LIMIT_99=1` | Every item that stacks can be carried up to 99 in the bag (the menu's "Item limit 99"; `src/inventory.h`) |
| `RG_BIGGER_BAG=1` | The bag has a seventh page, 280 spaces, from the start (the menu's "Seventh bag page"; `0` turns it off; `src/inventory.h`) |
| `RG_NATIVE_RENDERER=1` | The native renderer for this run, whatever the menu's "Native renderer" says (`0` turns it off; `src/native_renderer.h`, [NATIVE-RENDERER.md](NATIVE-RENDERER.md)): the switch the native rendering work goes behind as it lands, off by default; nothing reads it yet. Logs `[rg] native renderer on` |
| `RG_DEV_TOOLS=1` | The developers' tools left in the game come back (the menu's "Developer tools"; `src/dev_tools.h`) |
| `RG_TEXTURE_PACK=DIR` | Read that one texture pack, a folder or a `.zip`, instead of `textures\hd` and `textures\packs` ([TEXTURE_PACKS.md](TEXTURE_PACKS.md)) |

### Diagnosis

Scripted input, traces, watches, the profiler, and switches that turn one of
the project's fixes off to compare.

| Variable | Effect |
|---|---|
| `RG_UI_SCRIPT=file\|text` | Drive the menu for tests and screenshots: `<guest frame> <command> [args]` entries, `;`-separated (see `thirdparty/dq8-gfx/src/ui/ui_script.cpp`) |
| `RG_INPUT="30:CROSS:150,..."` | Scripted pad input: seconds, buttons (`+`-joined; `LUP` etc. for the left stick, `RUP` etc. for the right), milliseconds; with `RG_INPUT_CLOCK=pad` both numbers count the game's pad reads, so a script replays alike at any host speed |
| `RG_MC0_DIR=path` | Memory card folder instead of `disc/mc0` (`mc1` is its sibling). The benchmarks point it at `build/run/mc0-bench`, a copy holding only the save they load, since a second save changes what the scripted load menu picks |
| `RG_FORCE_ENCOUNTER=1` | A battle at the first steps wherever the field has enemies: the encounter countdown is cut before each tick (`src/debug/encounter.cpp`), and each battle's start is logged with its pad read; the battle bench (`walk.sh BATTLE=1`) sets it |
| `RG_TRACE=0xADDR,...` | Log calls of main-ELF functions with guest thread, sp, ra and a0-a3 (`RG_TRACE_PEEK` words, `RG_TRACE_ARG0` offsets from a0, `RG_TRACE_LIMIT` calls each, default 16), and v0 on return. Like a hook, it sees only the calls that go through the function table: not a function reached by a tail `j` ([HOOKS.md](HOOKS.md)). A return line whose pc is not ra was unwound at a safe point, and its v0 is not the result |
| `RG_DUMP=0xFUNC:file` | Append each call of a `FUNC(dst, src, size)` to file: dst, size and the bytes at src |
| `RG_WATCH=0xADDR,...` | Print guest words, I/O registers included, every `RG_WATCH_MS` (default 1000) |
| `RG_HWWATCH=0xADDR` | x86 debug register on a guest word: writes leaving a value in `RG_HWWATCH_RANGE=lo-hi` (default the main stack) print the writer's stack (`RG_HWWATCH_READS=1`, `RG_HWWATCH_READER=0xFUNC`, `RG_HWWATCH_DELAY=s`); at most 64 reports a run. The stack is the host's, as `roguegalaxy.exe+0xOFFSET`: 0x140000000, the executable's image base, plus OFFSET is the address `llvm-symbolizer --obj=build/game/src/roguegalaxy.exe` takes. Windows only |
| `RG_PROFILE=out.txt` | Sample all threads for `RG_PROFILE_SECONDS` (default 20) from `RG_PROFILE_START` seconds in (default 0); report with `tools/profile/report.py` (`RG_PROFILE_STACKS=1` also records the callers of samples inside the executable, which the report skips). Windows only, as are the `RG_PROFILE_*` rows below |
| `RG_PROFILE_THREAD=NAME` | With `RG_PROFILE`: sample only the threads whose name starts with NAME (`VIF1`, `GIF`, `GSWorker`, `GameThread`), for many more samples of one |
| `RG_PROFILE_ON_MARK=1` | With `RG_PROFILE`: a window of `RG_PROFILE_SECONDS` at each F9 instead, into `out-1.txt`, `out-2.txt`, ... (numbered on from the last file already there; an F9 during an open window is folded into it) |
| `RG_PROFILE_BEFORE=S` | With `RG_PROFILE_ON_MARK=1`: sample all the time (every 5 ms; about 9% of the VIF1 thread at the boss gate) and put the S seconds before each F9 into its window too |
| `RG_PROFILE_AUTO_MS=N` | With `RG_PROFILE_BEFORE`: a hitch opens a window by itself (a frame N ms long and three times the usual, or the frame rate halved for a few frames), of the hitch and half a second either side, at most 30 a run |
| `RG_PROFILE_INTERVAL_MS=N` | The pause between two sampling rounds (default 1 for a window, 5 when sampling all the time with `RG_PROFILE_BEFORE`) |
| `RG_STALL_SECONDS=15` | When no frame has been flipped for that long while the game is not paused, the log gets every guest thread's state (what it waits on; the running one's registers and the addresses it passes over the next second) and every host thread's stack (Windows only; `exe!0x...` frames resolve with `llvm-symbolizer --obj=build/game/src/roguegalaxy.exe`, from the toolchain's `bin`), again every minute; `RG_STALL_WATCH=0` turns the watchdog off |
| `RG_POS_TRACE=1` | Log every call that puts a character somewhere it did not walk to (`CharaSetPos`, `CharaSetPosXYZ`, `CharaPlaceAt`: character, id, old and new position, caller), and any character still falling after 90 movement updates, with where it fell from |
| `RG_JUMP_TRACE=1` | Log the party leader's movement updates while off the ground (host time, position, step-scaled velocity, the motion's displacement, the action mode and the gravity flags, one line per `CharaMoveCollideMaybe`; `=2` logs the moves on the ground too, for the speed ramps; `=3` every character's, tagged with address and id, and for enemies every move with what drives it and every move request with its caller; `=4` every character's moves on the ground too, for a party following its leader): compares a jump's arc and timing between the 30 and 60 fps modes |
| `RG_SCENE_TRACE=1` | Prints each change of the game's mode and phase and of the main menu's state (`[scene]` lines; the quick scenes go by the mode and the warp's script) |
| `RG_UPDATE_AS=VERSION` | The check for a newer release (`src/update_check.h`, the menu's "Check for new versions") takes this build for that version, to see the notice; scripted runs (`RG_INPUT`) never check |
| `RG_SELFTEST=gs` | Check the code changed in vendored dependencies, then exit (`all` does the same; another value checks nothing) |
| `RG_EE_CVT=round` | Diagnostic: float-to-int conversions round, as the runtime used to, instead of truncating as the EE does |
| `PS2X_EE_ROUNDING=nearest` | Diagnostic: guest float math rounds to nearest, the host default, instead of toward zero as the EE does (patch 0003) |
| `RG_VU0_FLAGS=0` | Keep the generated code for the game's three VU0 macro-mode bounding-box tests (`AabbOverlap`, `AabbContains`, `AabbContainsPoint`) instead of the host versions in `src/vu0_flags.cpp`; the recompiler updates the VU0 status and MAC flags in the functions that read them (`patches/ps2recomp/0017`), so both answer alike. `=check` runs the generated code and counts where it disagrees with the host versions |
| `RG_LIBC_RAND=0` | Leave the game's `rand` (0x133710) to the runtime's libc stub, which returns 15 bits, instead of newlib's generator in `src/libc_rand.cpp`: the game divides every random number by 2^31, so with the stub they all come out 0, and a party member's AI froze the game that way ([STATUS.md](STATUS.md), "The freezes"). For comparing only |
| `RG_MOVIE_AUDIO=game` | The game's own movie PCM callback, which drops packets the runtime's demux delivers too early (noise) |
| `RG_STICK_SQUARE=0` | Hand the game the host's round analog stick range as it is. By default `src/stick_response.cpp` reshapes both sticks onto the DualShock 2's square range (the direction and the pushed distance stay, the distance counts as the longer axis, so a full diagonal reads full on both), which the game's run threshold was tuned for: with a round stick the party only runs within about 20 degrees of each of the four directions and walks in between |
| `RG_TEXTURE_DUMP=DIR` | Write each piece of 2D art drawn (font glyphs, icons, window pieces) once into DIR, named by its content; `debug\RogueGalaxy-collect.bat` sets it to `textures\collected`, for making a pack |
| `RG_TEXTURE_DUMP_WHOLE=DIR` | Write each texture the game builds, whole, as `TEX0HASH-CLUTHASH-BITS.pam` ([TEXTURE_PACKS.md](TEXTURE_PACKS.md)) |
| `PS2X_AUDIO_DUMP=file` | Framework: copy of the audio output, raw 48 kHz s16 stereo |

### Renderer internals

The GS backend, the VIF1 and VU1 threads, and the native VU1 work.

| Variable | Effect |
|---|---|
| `RG_GS=sw` | Software GS rasterizer instead of the SDL3 GPU backend |
| `RG_GS_NO_WINDOW=1` | Present through the runtime's window instead of SDL's |
| `RG_GS_WORKER=0` | Run the GS backend on the game's thread instead of its own worker thread |
| `RG_PRESENT=vblank` | Present at host vblanks instead of on the game's flips |
| `RG_VIF1_THREAD=0` | Run VIF1 lists (VU1) on the game's thread instead of their own |
| `RG_GIF_THREAD=0` | Run the GS packets of the VIF1 lists on the VIF1 thread instead of a GIF stage of their own |
| `RG_GS_DEPTH_READS=queue` | Serve the game's depth readbacks through the GS worker queue |
| `RG_CROP=N` | GS pixels left off each edge of the picture (default 2: the border the game leaves black or half drawn, which a television's overscan hid; 0 shows the whole frame) |
| `RG_GS_CAPTURE=DIR` | F7 writes the next frame's batches, GS memory and the renderer's target trace (`RG_GS_FRAMELOG`'s files, as `DIR/capture-N.txt`); `debug\RogueGalaxy-capture.bat` sets it to `build\captures`, for a picture that looks wrong |
| `RG_GS_TIMING=1` | Time every GS backend call on both sides of the worker queue, every 2 s |
| `RG_GS_CENSUS=1` | Every 2 s, the draw states the GPU backend only approximates (destination alpha test and the like), and draws whose scissor is wider than their buffer; `RG_GS_SKIP=MASK:VALUE` drops draws by TEST register |
| `RG_GS_FRAMELOG=file@N` | Every batch, transfer and clear of one frame, once the game's pad read count reaches N, with GS memory as `file.vram` (`@N#K` also before batch K); `RG_GS_FRAMESKIP=A-B,...` leaves batches of that frame out (`A-` to the end); `RG_GS_TRACE_BEFORE=M` turns the backend's target trace (`DQ8_GFX_TRACE_TARGETS`, a line per transfer and ownership move) on from M pad reads before that frame to its end; `RG_GS_FRAMELOG_COUNT=K` logs the K-1 frames after it too (120 in all at most), as `file.1.txt`, `file.2.txt`, ... (the path without its extension), each with its `.vram` |
| `RG_VU1_REPLAY=file` | Every VU1 program run in full (code, data memory and registers it starts with, the packets it kicks), for `tools/vu1/oracle`; `RG_VU1_REPLAY_FROM=N` and `RG_VU1_REPLAY_LISTS=M` choose the DMA lists (`src/debug/vu1_replay.h`) |
| `RG_VU1_REPLAY_AFTER=1` | With `RG_VU1_REPLAY`: also the data memory each run ends with, the state a following run reads (`RG_VU1_REPLAY_FROM` is 1 and `RG_VU1_REPLAY_LISTS` 30 unless set) |
| `RG_VU1_HARVEST=DIR` | A few samples of every VU1 program DIR has no file for yet, a file each in `RG_VU1_REPLAY`'s format, to leave on while playing (`tools/vu1/oracle/harvest-play.sh`); `RG_VU1_HARVEST_SAMPLES` (4), `RG_VU1_HARVEST_CHUNKS` (64 MSCNTs a sample), `RG_VU1_HARVEST_SPACING` (300 lists between samples); see [VU1-NATIVE.md](VU1-NATIVE.md) |
| `RG_VU1_NATIVE=MODE` | The VU1 programs done natively (`src/vu1_native.h`): `check` compares them with the VU1 as the game runs, `1` sends their packets in place of the VU1's, `skip` does so without running the VU1, `gpu` sends the chunks as meshes the GPU transforms (`gs_native.vert`); a `[vu1native]` line every 2 s. `RG_VU1_NATIVE_CHECK_EVERY=N` checks one chunk in N (every check costs the VU1's run, the native build and two decodes on the VIF1 thread; checking all halved a boss fight's frame rate). `DQ8_GFX_NATIVE_CPU=1` expands the meshes on the CPU instead, `DQ8_GFX_NATIVE_CULL_FLIP=1` culls the other face |
| `DQ8_GFX_NATIVE_NO_CULL=1` | With `RG_VU1_NATIVE=gpu`: draw the meshes wholly off screen too |
| `RG_VU1_CAPTURE=FILE` | The VU1 data memory each program starts with (16 KiB a start), for `tools/vu1/capture_report.py`: `RG_VU1_CAPTURE_MAX` starts (4000), then only the matrices for `RG_VU1_CAPTURE_LISTS` more DMA lists (1500) |
| `PS2X_VU_FMAC_FLAGS=1` | Compiled VU1 programs queue the flags of every FMAC result, not only the ones a flag reader can still see (patch 0006); for comparisons |
| `RG_JITTER=1` | Experimental: the 3D scene's draws move by a Halton(2,3) offset of up to half a host pixel each frame, the first step of a temporal upscaler; `RG_JITTER_SCALE=F` multiplies it (1 unless set; 4 makes it easy to see) |
| `RG_CAMERA_LOG=N` | Every N presents, the cameras the scene's draws ran under since the last line, busiest first, with their projection; needs `RG_JITTER` |
| `DQ8_GFX_PROFILE_CYCLE` | Tags the scene's draws with the VU1 program that made them, as `RG_JITTER` does (`src/vif1_thread.cpp`); the backend's profile cycle it is named for is no longer in the code |
| `RG_TEXTURE_PACK_DECODE=1` | Decode every picture of the texture packs on the CPU, instead of sending the BC1, BC2, BC3 and BC7 ones to the GPU compressed; to compare |
| `DQ8_GFX_STATS_EVERY=N` | A `[gfx]` stats line every N presented frames (off unless set; both launchers set 60, `debug\RogueGalaxy-hitches.bat` 10) |
| `DQ8_GFX_SHOW_FPS=1` | The backend's frame rate in the window title; `RG_SHOW_FPS=1` sets it with `RG_GS_NO_WINDOW=1` |
| `DQ8_GFX_HIDDEN_WINDOW=1` | The window is never shown or raised, for scripted runs; the GS work is the same, and frames still reach `DQ8_GFX_SCREENSHOT_DIR` (`tools/bench/walk.sh` sets it) |
| `DQ8_GFX_REMOVE_LINE_BLEND` | Set: no PS2 one-line display blend, for runs without the menu (the trace replay). In the game the menu's setting replaces it at start: use `RG_SETTINGS_FILE` with `remove_line_blend = 1` instead |
| `DQ8_GFX_NATIVE_CAPTURE=FILE` | When the file `FILE.trigger` exists, the next presented frame is saved as a BMP at FILE, once; `DQ8_GFX_NATIVE_CAPTURE_SERIES=1` deletes each trigger and numbers the frames `FILE.N.bmp` |
| `DQ8_GFX_EFFECT_SCALE=N` | Scale of the blur buffers (0x2C40 and 0x3800, at most 256 pixels wide; `DQ8_GFX_EFFECT_BASES` for others): 2 unless set, 0 for the scene's scale. At 8x they would be 1536x1536; at 2 the frame differs from the scene's scale by 814 of 229,376 pixels at 3x, none by more than 8 levels |
| `DQ8_GFX_GPU_TIMES=1` | Label the backend's GPU work and print `[gputime]` lines every two seconds (GPU ms and count a second per label, heaviest first); Vulkan only, about a tenth of the frame at 8x |
| `DQ8_GFX_NO_DUAL_SOURCE=1` | No dual-source blending: the source-alpha blends go back to the alpha replay (two passes), for comparing frames or a driver without `dualSrcBlend` |
| `DQ8_GFX_NO_DESTINATION_COPY=1` | No shader that reads the destination from a snapshot: the draws that need it keep their approximation |
| `DQ8_GFX_SUBMIT_DRAWS=N` | A flush of the queued draws submits them once this many are recorded (96 unless set; 0 submits at every flush) |
| `DQ8_GFX_PRESENT_FROM_MEMORY=1`, `DQ8_GFX_DISABLE_GPU_COMPOSE=1` | Present from GS local memory, as the software backend reads it; compose the display on the CPU. Both tell a wrong picture from a wrong readback |
| `DQ8_GFX_TRACE_BATCHES=1` | A line per draw call: its target, pipeline state and geometry |
| `DQ8_GFX_DUMP_TEXTURES=DIR` | Every texture as expanded, named by identity |
| `DQ8_GFX_CHECK_GPU_TEXTURES=1` | Rebuild each texture taken from a render target the old way, by reading the target back, and report texels that differ; it stalls on every readback, so it is for checking changes |
| `DQ8_GS_UPLOAD_TRACE=N` | Every N uploads to GS memory, how they are fed (the payload a call) |
| `DQ8_GFX_FLUSH_ON_TRANSFER`, `DQ8_GFX_NO_CLAIM`, `DQ8_GFX_NO_PASSIVE_DEPTH`, `DQ8_GFX_PATCH_CT32_ONLY`, `DQ8_GFX_NO_COVERED_RELEASE`, `DQ8_GFX_NO_GPU_PADDING`, `DQ8_GFX_CYCLE_SNAPSHOTS`, `DQ8_GFX_NO_ROUND_TRIP`, `DQ8_GFX_CPU_LOCAL_COPY`, `DQ8_GFX_NO_UNCHANGED_SKIP` | Each, when set, puts one of the backend's optimizations back to its old behaviour, for comparing frames (the comment by each `getenv` in `thirdparty/dq8-gfx/src/gfx/backends/sdlgpu/` says which) |
| `DQ8_GFX_SCREENSHOT_DIR`, `DQ8_GFX_SCREENSHOT_EVERY` | Framework: write every Nth frame as PPM |
