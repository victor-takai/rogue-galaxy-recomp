# Running and diagnosis

## Launchers

On Windows, double-click `Rogue Galaxy.exe`, which the build puts in the
repository's folder: it starts the built game on `disc/SCUS_974.90`, writes
the game's log to `build/last-run.log` (the run before survives as
`build/last-run.prev.log`) and opens no terminal; a game that ends with an
error shows the log's last lines in a message box. `debug\RogueGalaxy.bat`
does the same in a terminal, which it keeps open until the game closes (run
it from one to see the log as it is written). The build copies the FFmpeg and clang
runtime DLLs next to `roguegalaxy.exe`, so it needs nothing on PATH. To set
any of the variables below, set them in Windows (they reach the game through
either launcher), add a `set` line to the batch file (it has examples), or set
them in the terminal before running it. Both launchers also set
`VK_LOADER_LAYERS_DISABLE=~implicit~`, which keeps other programs' Vulkan
layers (OBS's capture hook, ReShade, the Steam, GOG and Epic overlays) out
of the game's process; set it yourself when running from a terminal.
`Rogue Galaxy.exe` leaves it unset under Wine or Proton, where it would
reach the host's loader and keep out the layers the picture needs there.
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

`debug\RogueGalaxy-hitches.bat` is the profile launcher with a stats line every 10 frames, for short hitches: each line also gives the longest frame of its interval (the longest gap between two presents), the longest the main thread was held up ("main stall"), the GPU pipelines created in it and what creating them cost (a first-time state compiles one), and the VU1 pairs that ran on the interpreter (a program the build lacks; the session records it into `build/vu1-profile`, see VU1 programs in [BUILDING.md](BUILDING.md)). Its profiler samples all the time (`RG_PROFILE_BEFORE=10`; about 9% of the VIF1 thread's time at the boss gate, so it is for hunting hitches, not for playing): it writes a profile by itself at a hitch nobody marked (`RG_PROFILE_AUTO_MS=50`: a frame of 50 ms or more and three times the usual, or the frame rate halved for a few frames), of the hitch and half a second either side, up to 30 a session, and the log says so in a `[profile] window N: a hitch` line; an F9 window holds the 10 seconds before the press and the 5 after. Press F9 at each hitch felt, then read the lines around the `[mark]` (`python tools/bench/hitches.py build/last-run.log`, one row per interval); a profile window still open when the game is closed is cut short and written.

## Variables

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
| `RG_FPS=60` | The 60 fps mode for this run whatever the menu's 60 frames per second says (on by default; `RG_FPS=30`, or any other value, the game's own 30): the game flips every vblank and the scene's step follows the measured frame time (half at 60, whole at 30), so it plays at its normal speed at whatever rate the host reaches (movies and the loading screen keep 30); `RG_TIME_SCALE=fixed` keeps half steps regardless, `RG_TIME_TRACE=1` logs the frame times and the play clock, `RG_STEP_FIXES=0` leaves the systems that count frames (play clock, minimap, NPCs, projectiles, gravity and the jump's flight, movie fades) as the game has them, or a comma list of `clock`, `movie`, `npc`, `judge`, `gravity`, `event`, `menu`, `ai`, `texture`, `camera` picks which take the step (`event`: the scripts, the event player and the message windows count frames, so they run on the frames that complete a whole step, and at once when a button is pressed; the field modes that hold the scale for a scripted sequence, chests, talks and warps, take the half steps too; `menu`: the field's menus and HUD windows, the battle results among them, the chests' opening and the doors' orange triangles count frames as well and run on the same frames; `ai`: the characters' AI, enemies' and allies', counts its timers in frames and moves them by a step a run, so it runs on those frames too, each move shared with the next frame; `texture`: the texture animations, most of them stepped a whole frame a draw, take the frame's share, and the layers the data holds still stay still; `camera`: the camera's turn by the right stick (by both sticks in the first-person view), a frame's worth each frame, takes the frame's share, and so does the free camera's move) |
| `RG_TIME_SCALE=game` | Let the game take bigger logic steps after frames the host was too slow for, as on the console (off by default: slow frames play in slow motion, as in an emulator) |
| `RG_EE_CVT=round` | Diagnostic: float-to-int conversions round, as the runtime used to, instead of truncating as the EE does |
| `RG_MOVIE_AUDIO=game` | The game's own movie PCM callback, which drops packets the runtime's demux delivers too early (noise) |
| `PS2X_EE_ROUNDING=nearest` | Diagnostic: guest float math rounds to nearest, the host default, instead of toward zero as the EE does (patch 0003) |
| `RG_GS_DEPTH_READS=queue` | Serve the game's depth readbacks through the GS worker queue |
| `RG_MUTE=1` | No sound output (the IOP still runs) |
| `RG_INPUT="30:CROSS:150,..."` | Scripted pad input: seconds, buttons (`+`-joined; `LUP` etc. for the left stick, `RUP` etc. for the right), milliseconds; with `RG_INPUT_CLOCK=pad` both numbers count the game's pad reads, so a script replays alike at any host speed |
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
| `RG_MINIMAP_SIZE=N`, `RG_COMPASS_SIZE=N` | The field's minimap and compass, percent of the game's size, 50 to 100 (the menu's "Minimap size" and "Compass size"; `src/hud_size.h`) |
| `RG_BUTTONS=xbox` | The game's text shows an Xbox controller's buttons whatever is connected (`src/button_glyphs.h`; the menu's "Controller's buttons" follows the controller in use); `RG_GAIJI_DUMP=FILE` writes the picture of special characters they are drawn in (gaiji2.img) as loaded |
| `RG_UPDATE_AS=VERSION` | The check for a newer release (`src/update_check.h`, the menu's "Check for new versions") takes this build for that version, to see the notice; scripted runs (`RG_INPUT`) never check |
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
| `RG_SCENE_TRACE=1` | Prints each change of the game's mode and phase, the main menu's state and the Revelation Flow's placing step (`[scene]` lines; the quick scenes go by the mode, the warp's script and the placing step) |
| `RG_SHARP_LETTERS=0` | The letters as the game draws them, enlarged, whatever the menu's "Sharp letters" says (`=1` traced; `src/sharp_letters.h`, `src/font_trace.h`); the `[gfx]` stats line counts `larger regions` (glyphs drawn traced) and `borders` (round borders drawn for their copies); `RG_SHARP_LETTERS_TRACE=1` logs each text style's border mode and each glyph traced, `=DIR` also writes each glyph as found and as made there |
| `RG_INSTANT_TEXT=1` | Text in the message windows a page at a time (the menu's "Instant text"; `src/instant_text.h`) |
| `RG_MAP_CHESTS=1` | The chests not yet opened on the maps before a planet is complete (the menu's "Chests on the map"; `src/map_chests.h`) |
| `RG_SAVE_ANYWHERE=1` | In the field, R1 + Triangle opens the transporter's menu (the menu's "Save anywhere"; `src/save_anywhere.h`) |
| `RG_FULL_BAG_TO_STORAGE=1` | An item the bag has no room for goes to the transporter's item storage (the menu's "Full bag to storage"; `src/inventory.h`) |
| `RG_STACK_LIMIT_99=1` | Every item that stacks can be carried up to 99 in the bag (the menu's "Item limit 99"; `src/inventory.h`) |
| `RG_BIGGER_BAG=1` | The bag has a seventh page, 280 spaces, from the start (the menu's "Seventh bag page"; `0` turns it off; `src/inventory.h`) |
| `RG_DEV_TOOLS=1` | The developers' tools left in the game come back (the menu's "Developer tools"; `src/dev_tools.h`) |
| `RG_GS_CAPTURE=DIR` | F7 writes the next frame's batches, GS memory and the renderer's target trace (`RG_GS_FRAMELOG`'s files, as `DIR/capture-N.txt`); `debug\RogueGalaxy-capture.bat` sets it to `build\captures`, for a picture that looks wrong |
| `RG_CROP=N` | GS pixels left off each edge of the picture (default 2: the border the game leaves black or half drawn, which a television's overscan hid; 0 shows the whole frame) |
| `RG_INSTANT_RECAP=1` | The loading screen's story so far appears whole at once, then Press X (the menu's "Show the loading recap at once") |
| `RG_STICK_SQUARE=0` | Hand the game the host's round analog stick range as it is. By default `src/stick_response.cpp` reshapes both sticks onto the DualShock 2's square range (the direction and the pushed distance stay, the distance counts as the longer axis, so a full diagonal reads full on both), which the game's run threshold was tuned for: with a round stick the party only runs within about 20 degrees of each of the four directions and walks in between |
| `RG_GS_TIMING=1` | Time every GS backend call on both sides of the worker queue, every 2 s |
| `RG_GS_CENSUS=1` | Every 2 s, the draw states the GPU backend only approximates (destination alpha test and the like), and draws whose scissor is wider than their buffer; `RG_GS_SKIP=MASK:VALUE` drops draws by TEST register |
| `RG_GS_FRAMELOG=file@N` | Every batch, transfer and clear of one frame, once the game's pad read count reaches N, with GS memory as `file.vram` (`@N#K` also before batch K); `RG_GS_FRAMESKIP=A-B,...` leaves batches of that frame out (`A-` to the end); `RG_GS_TRACE_BEFORE=M` turns the backend's target trace (`DQ8_GFX_TRACE_TARGETS`, a line per transfer and ownership move) on from M pad reads before that frame to its end |
| `RG_SELFTEST=gs` | Check the code changed in vendored dependencies, then exit |
| `DQ8_GFX_SCREENSHOT_DIR`, `DQ8_GFX_SCREENSHOT_EVERY` | Framework: write every Nth frame as PPM |
| `PS2X_AUDIO_DUMP=file` | Framework: copy of the audio output, raw 48 kHz s16 stereo |
