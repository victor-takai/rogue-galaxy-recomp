# Rogue Galaxy Recompiled: Steam Deck fork

A static recompilation of Rogue Galaxy (PS2, USA, SCUS-97490) into a native
game, on [PS2Recomp](https://github.com/ran-j/PS2Recomp). This fork
(`touyoo0/deck-galaxy`, from `victor-takai/rogue-galaxy-recomp`, remote
`upstream`) targets the **Steam Deck (SteamOS) only**: Windows, macOS and
other platforms are out of scope, so add no code, scripts or docs for them.
The Deck work lives on the branch `steamdeck-only`. The rules for issues and
pull requests are in [CONTRIBUTING.md](CONTRIBUTING.md); follow them.

## The product, in three parts

1. **`installer/RogueGalaxy-Setup.py`**, run once in desktop mode (GTK 4 window):
   disc image, folder, extras, then it fetches the sources (a release's
   archive, else `git clone` of `steamdeck-only`), runs `install.sh
   --container` (a distrobox `rg-build`, Ubuntu 24.04) and adds the game to
   Steam through `steamos-add-to-steam`.
2. **The game's home screen** (Play, Update, Quit), drawn by the in-game
   menu (`Overlay::drawHome`, `thirdparty/dq8-gfx/src/ui/ui_overlay.cpp`) with
   the guest paused until Play. `RG_HOME=0` skips it; a run with `RG_INPUT`
   set skips it too.
3. **In-game update** (`src/self_update.cpp`, `update.sh`, `src/update_check.cpp`):
   Update downloads the release's source archive, checks its `.sha256`,
   writes only the files that changed (a submodule whose commit has not
   moved is left alone), runs `install.sh --container`, and restarts through
   `rogue-galaxy.sh` with `RG_RELAUNCH=1`. Progress is
   `build/update.progress`; the log `build/update.log`.

## Working on the Deck

Build and test over SSH (`deck@192.168.1.123`; the Mac has no toolchain and
`thirdparty/` submodules are empty locally).

- **Never touch the player's game**: `/run/media/SD/Recomp/RogueGalaxy` (his
  saves are in it). Use a copy: `DeckGalaxy` (a copy of it, on the branch) or
  `InstallTest` (a fresh install). A copied `build/` is useless (CMake caches
  absolute paths): configure again.
- Send the branch with `git bundle create` + `scp`, then `git fetch` in the
  test folder (no push needed). Build in the container:
  `distrobox enter rg-build -- sh -c "cd /run/media/deck/SD/Recomp/DeckGalaxy && cmake --build build/game" < /dev/null`.
- `install.sh` rewrites `~/.local/share/applications/rogue-galaxy.desktop` to
  its own folder: put it back to `RogueGalaxy` after a test. Adding to Steam
  edits the user's library (`steamos-add-to-steam`): ask first, and say how to
  remove the entry.
- A process started over SSH dies with the session (systemd kills the
  scope): for a long test use `systemd-run --user --unit=NAME ...`. Stop a
  test by its id; `pkill -f "x"` matches the ssh command itself (write
  `"[x]"`).

- [docs/HOW-IT-WORKS.md](docs/HOW-IT-WORKS.md): the project in plain words,
  with a glossary.
- [docs/README.md](docs/README.md): every doc, by who it is for.
- [docs/BUILDING.md](docs/BUILDING.md): building, what to rebuild after a
  change, the layout.
- [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md): the launcher, diagnosis, every
  environment variable.
- [docs/HOOKS.md](docs/HOOKS.md): how the game's behaviour is changed.
- [docs/STATUS.md](docs/STATUS.md): the work log; search it before
  re-investigating something.

## Never game data

Nothing read from the disc goes into git, an issue or a pull request:
`disc/`, disc images, BIOS files, texture packs and art collected from the
screen (`textures/`), VU1 recordings, frame dumps. The one exception: a
reporter may attach a save to an issue, as the `disc/mc0` folder zipped;
saves never go into git. The repository holds only checksums and addresses
(`config/`).

**The one other exception: the installer's theme.** The owner chose to ship
one official promotional artwork of the game (Level-5 / Sony's, not read from
the disc) in `installer/art/`, for the installer's background; the AppImage and
the release's source zip carry it. Nothing else: no file, texture, screenshot
or frame taken from the disc or the running game. The artwork stays its
owners' property: if they ask for it to be removed, remove it (the installer
works without it, on a drawn sky).

## Build and run

From the repository root (details and the rebuild table in BUILDING.md):

```bash
./install.sh --iso "/path/to/image.iso"            # first time: extract, tools, recompile, build (in a distrobox)
cmake --build build/game                           # after a change in src/ or thirdparty/dq8-gfx
python setup.py recompile && python setup.py build # after a change in config/
build/game/src/roguegalaxy disc/SCUS_974.90        # run (or ./rogue-galaxy.sh)
```

`build/generated/` is the recompiler's output: change the maps, the
configs or the patches, never the generated files. The game's log is
`build/last-run.log` (the run before, `build/last-run.prev.log`).

## Testing

- Run your own copy of the game, not the player's: set `RG_SETTINGS_FILE` to
  a file of your own (never the player's, `~/.local/share/RogueGalaxy/`),
  `RG_MUTE=1` and `DQ8_GFX_HIDDEN_WINDOW=1` for scripted runs, and stop only
  your process, by its id.
- **A hidden window draws no menu.** The home screen and the F1 menu are only
  rendered with a visible window: use `RG_WINDOWED=1` and
  `RG_UI_SCRIPT="0 screenshot menu"` (steps are timed by guest frame, or
  `3s` by wall clock, since a paused guest has no frames; commands `play`,
  `update`...), and read the `.bmp` in the settings folder's `screenshots/`.
  `RG_HOME=0` for runs that only need the game.
- To test the update without a release: a source zip from
  `tools/release/source_zip.py --worktree`, its `.sha256`, a `releases.json`,
  and `RG_UPDATE_API=file:///...` (see DEVELOPMENT.md). The run restarts the
  game after the update, and the script runs again: stop it yourself.
- Scripted input: `RG_INPUT` with `RG_INPUT_CLOCK=pad` replays alike at any
  speed (DEVELOPMENT.md).
- The frame-compare gate for renderer changes was `tools/bench/walk.sh`,
  which only worked on Windows and is gone from this fork; compare frames by
  hand with `RG_UI_SCRIPT` screenshots and `tools/bench/cmpvram.py` until a
  Deck replacement exists. Say in the pull request what was compared.
- Speed: compare runs taken alike on an idle machine, with frames per second
  over wall time (`[gfx]` lines), not presents.
- `RG_SELFTEST=gs` checks the code changed in vendored dependencies.

## PS2Recomp patches

`thirdparty/PS2Recomp` stays at its upstream commit; changes to it are
patches in `patches/ps2recomp/`, applied in name order at configure. To
change one, follow [patches/README.md](patches/README.md): regenerate it from
a base holding the earlier patches, check that it reverse-applies, add its
row to the table, then run `cmake -S . -B build/game` once. A patch that
changes the generated code needs `setup.py tools`, `recompile` and `build`.

## Branches and pull requests

- Work on a branch (English kebab-case, one topic each); the Deck work is on
  `steamdeck-only`, and the fork's `main` still equals upstream's until it is
  merged.
- Commit, push, open a pull request or post on GitHub only when your human
  asks. `gh` is logged in as `touyoo0`, the owner of the fork.
- Open a pull request once the change is built, tested on the Deck and
  reviewed. Fill in the template; name issues with "Refs #N".
- A new option follows [docs/ADDING-OPTIONS.md](docs/ADDING-OPTIONS.md) and
  goes in [docs/OPTIONS.md](docs/OPTIONS.md); a new environment variable gets
  a row in DEVELOPMENT.md; a change players notice gets a line in
  CHANGELOG.md under "Unreleased", linking its issue by the full URL:
  `([#N](https://github.com/touyoo0/deck-galaxy/issues/N))`.
- Answer review with new commits; no force-push unless asked.
- Line endings: LF.

## Gotchas

- **Hooks see only calls through the function table.** A guest `j` (a tail
  jump) bypasses it: hook the caller. Code under a hook can be unwound at any
  safe point, so write guest state directly instead of calling game functions
  that take a lock or a file slot (HOOKS.md, "Traps").
- **The game counts frames.** At 60 fps, logic that counts frames runs at
  half steps (`src/step_fixes.cpp`,
  [docs/sixty-fps-logic.md](docs/sixty-fps-logic.md)). Never skip a
  logic-frame function called from a loop inside one frame: the loop hangs
  (read its callers with `tools/mipsdis.py ... calls ADDR` first).
- **Never lower `kGuestHeapEnd`** (0x01F80000, `src/main.cpp`): the game's
  big `memalign` fails and the boot dies on a black screen.
- **The runtime's libc stubs can be wrong** (the stub list in
  `config/SCUS_974.90/recomp/main.toml`): the old freezes were its 15-bit
  `rand` (`src/libc_rand.cpp`).
- **A table hook that skips the original sets `ctx->pc = ra`.**
- **A release is source only.** The game's generated code comes from the
  player's disc, so no binary is distributed: a release is
  `tools/release/source_zip.py`'s zip (with `FILES`, `REVISION`,
  `SUBMODULES`) and its `.sha256`, plus the installer: `RogueGalaxy-Setup-x86_64.AppImage` and its `.sha256`
  (`tools/release/build_appimage.sh`, run on the Deck) and
  `installer/RogueGalaxy-Setup.py`.
- **The emulated GS/VU1 path stays intact.** Native rendering work goes
  behind the native renderer switch, off by default
  ([docs/NATIVE-RENDERER.md](docs/NATIVE-RENDERER.md)).
