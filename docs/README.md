# The docs

What each page here is for, by who it is for. New to the project? Start with
[HOW-IT-WORKS.md](HOW-IT-WORKS.md).

## Playing

- [The main README](../README.md): what the project is, the short
  install and the controls.
- [INSTALL.md](INSTALL.md): installing, the undub, the European texts,
  updating, saves from PCSX2 and uninstalling.
- [OPTIONS.md](OPTIONS.md): the F1 menu and every game option, with where to
  find each one.
- [DISC.md](DISC.md): which disc images work, and how they are checked and
  extracted.
- [TEXTURE_PACKS.md](TEXTURE_PACKS.md): putting texture packs in, and making
  them.
- [CHANGELOG.md](../CHANGELOG.md): what each release brings, and its known
  issues.

## Understanding the project

- [HOW-IT-WORKS.md](HOW-IT-WORKS.md): how the project works, in plain words,
  with a glossary.
- [STATUS.md](STATUS.md): the work log, from the first build to now, and what
  the game needs.

## Building and changing the game

- [BUILDING.md](BUILDING.md): building by hand, on Windows or Linux, and the
  project's layout.
- [DEVELOPMENT.md](DEVELOPMENT.md): the debug launchers, profiling, and the
  environment variables.
- [HOOKS.md](HOOKS.md): writing hooks, the way changes to the game's
  behaviour go in.
- [ADDING-OPTIONS.md](ADDING-OPTIONS.md): adding an option to the F1 menu
  and the game's Settings screen, with a checklist.
- [FINDING-THINGS.md](FINDING-THINGS.md): finding the code and data behind
  something in the game.
- [MAPS.md](MAPS.md): the function maps the recompiler reads, and how they
  are checked.

## How the game works

- [sixty-fps-logic.md](sixty-fps-logic.md): what the game counts in frames,
  and how the 60 fps mode keeps its speed.
- [dialogue.md](dialogue.md): how the game picks and loads a line of
  dialogue, the base of the undub and the European texts.

## The renderer

Start with [NATIVE-RENDERER.md](NATIVE-RENDERER.md): it says where things
stand and what each of the others is for.

- [VU1-NATIVE.md](VU1-NATIVE.md): the VU1's 3D programs run as native code.
- [VU1-RECON.md](VU1-RECON.md): groundwork for temporal upscaling (object
  identity, jitter, cameras).
- [GS-AUDIT.md](GS-AUDIT.md): where a frame's GPU time goes, and what was
  cut.
- [GS-PASS-GRAPH.md](GS-PASS-GRAPH.md): one frame of Salgin, pass by pass.
- [GS-PERFORMANCE.md](GS-PERFORMANCE.md): the GS side's measurements and
  ideas, with what came of each.
