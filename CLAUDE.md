# Rogue Galaxy Recompiled

A static recompilation of Rogue Galaxy (PS2, USA, SCUS-97490) into a native
game, on [PS2Recomp](https://github.com/ran-j/PS2Recomp). The rules for
issues and pull requests are in [CONTRIBUTING.md](CONTRIBUTING.md); follow
them.

- [docs/HOW-IT-WORKS.md](docs/HOW-IT-WORKS.md): the project in plain words,
  with a glossary.
- [docs/README.md](docs/README.md): every doc, by who it is for.
- [docs/BUILDING.md](docs/BUILDING.md): building, what to rebuild after a
  change, the layout.
- [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md): launchers, profiling, every
  environment variable.
- [docs/HOOKS.md](docs/HOOKS.md): how the game's behaviour is changed.
- [docs/STATUS.md](docs/STATUS.md): the work log; search it before
  re-investigating something.

## Never game data

Nothing read from the disc goes into git, an issue or a pull request:
`disc/`, disc images, BIOS files, texture packs and art collected from the
screen (`textures/`), VU1 recordings, frame dumps. The one exception: a
reporter may attach a save to an issue, as the `disc\mc0` folder zipped;
saves never go into git. The repository holds only checksums and addresses
(`config/`).

## Build and run

From the repository root (details and the rebuild table in BUILDING.md):

```bash
python setup.py install --iso "/path/to/image.iso"  # first time: extract, tools, recompile, build
cmake --build build/game                           # after a change in src/ or thirdparty/dq8-gfx
python setup.py recompile && python setup.py build # after a change in config/
build/game/src/roguegalaxy disc/SCUS_974.90        # run ("Rogue Galaxy.exe" on Windows)
```

`build/generated/` is the recompiler's output: change the maps, the
configs or the patches, never the generated files. The game's log is
`build/last-run.log` (the run before, `build/last-run.prev.log`).

## Testing

- Run your own copy of the game, not the player's: set `RG_SETTINGS_FILE` to
  a file of your own (never the one in `%APPDATA%`), `RG_MUTE=1` and
  `DQ8_GFX_HIDDEN_WINDOW=1` for scripted runs, and stop only your process, by
  its id.
- Scripted input: `RG_INPUT` with `RG_INPUT_CLOCK=pad` replays alike at any
  speed (DEVELOPMENT.md).
- The frame-compare gate for renderer changes: `tools/bench/walk.sh NAME 1 30`
  (scale 1, the 30 fps mode; then scale 3), then
  `tools/bench/cmpvram.py NAME REF` against a reference made with the same
  save. Only the 30 fps walk is
  deterministic; 60 fps frames follow the frame time and never compare. The
  input scripts and saves it needs are the maintainer's (`build/run/`, not in
  the repository): say in the pull request that the gate is still to run.
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

- `main` takes pull requests only. Branch names in English kebab-case, one
  topic each.
- Commit, push, open a pull request or post on GitHub only when your human
  asks.
- Open a pull request once the change is built, tested in the game and
  reviewed. Fill in the template; name issues with "Refs #N".
- A new option follows [docs/ADDING-OPTIONS.md](docs/ADDING-OPTIONS.md) and
  goes in [docs/OPTIONS.md](docs/OPTIONS.md); a new environment variable gets
  a row in DEVELOPMENT.md; a change players notice gets a line in
  CHANGELOG.md under "Unreleased", linking its issue by the full URL, as
  the others do:
  `([#N](https://github.com/victor-takai/rogue-galaxy-recomp/issues/N))`.
- Answer review with new commits; no force-push unless asked.
- Line endings: LF, except `.bat` and `.ps1` (CRLF; `.gitattributes`).

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
- **The emulated GS/VU1 path stays intact.** Native rendering work goes
  behind the native renderer switch, off by default
  ([docs/NATIVE-RENDERER.md](docs/NATIVE-RENDERER.md)).
