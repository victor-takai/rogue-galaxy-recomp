# How the project works

This page explains the project in plain words, for players, modders and
anyone curious. No programming needed. The other pages in `docs/` go into
the details.

## A translation, not an emulator

A PlayStation 2 game is a program written for the PS2's main processor, the
Emotion Engine (EE). An emulator such as PCSX2 imitates the whole PS2 and
translates the game's code bit by bit while you play.

This project works differently. Before the game is built, a tool called a
recompiler ([PS2Recomp](https://github.com/ran-j/PS2Recomp)) reads the game's
program once and translates every one of its functions into C++, the
language Windows programs are written in. That C++ is then compiled into
`roguegalaxy.exe`, an ordinary Windows program (it builds on Linux too,
[BUILDING.md](BUILDING.md)). The game's own logic runs as
it did on the PS2, but as native code.

That is why it is called a static recompilation: "static" because the
translation happens ahead of time, not while you play.

## Why you need your own disc

Nothing of the game is in this repository. When you install, the setup
reads your disc image and copies its files into the `disc/` folder (about
6.3 GiB). Two things come from there:

- **The code.** The recompiler translates the game's program
  (`disc/SCUS_974.90`, plus five smaller code files in `disc/BIN/` that the
  game loads for some parts, such as the title screen) on your machine.
- **The data.** While you play, the game reads its models, textures, text,
  voices and movies from `disc/`, the same files it read from the DVD.

## The runtime: a stand-in for the PS2

The translated code still expects a PS2 around it: its memory (32 MB), the
threads the game starts, a DVD drive, memory cards, a controller, a sound
chip and a graphics chip. The runtime is the part that provides all of
that on your PC:

- File requests go to `disc/`, and memory card saves to `disc/mc0`.
- Your controller or keyboard stands in for the DualShock 2.
- The PS2's graphics chip (the GS) is imitated on your graphics card by
  `thirdparty/dq8-gfx`, taken from the Dragon Quest VIII recompilation. That
  is what lets the game draw at up to 8 times its original resolution.
- The 3D models go through a second processor on the PS2, the VU1, which
  runs small programs of its own. Those programs are compiled ahead of time
  too, so 3D scenes run at full speed.

The runtime comes with PS2Recomp. Our changes to it live in
`patches/ps2recomp/` and are applied when the game is built.

The translated game is called the **guest** (its memory, its threads, its
addresses), and your PC the **host**. You will see those two words all over
the code and the issues.

## Hooks: how the game gets changed

Most of what this project changes in the game, from the 60 fps mode and
widescreen to the game options in the F1 menu, is a **hook**: a function of
ours put in place of one of the game's functions. When the game calls that
function, ours runs instead, and it can change the result, do something
extra, or call the game's own version. The hooks live in `src/`, one file
per feature (`src/instant_text.cpp` is the instant text option, for
example). The display options, such as the resolution, are not hooks: they
belong to the part that draws the picture.

The game's files are never modified, and an option can be turned off at any
time (a few take effect at the next start). [HOOKS.md](HOOKS.md) is the
guide to writing a hook.

## What happens when you press play

1. `Rogue Galaxy.exe` starts `roguegalaxy.exe` and writes its log to
   `build/last-run.log`.
2. The runtime loads the game's program from `disc/`, sets up the PS2's
   memory and installs the hooks.
3. The translated code starts running, exactly where the PS2 would start it.
4. Whenever the game asks for a file, the runtime reads it from `disc/`.
   Whenever it draws, the graphics chip's work goes to your graphics card
   and then to the window.
5. Your settings from the F1 menu are kept in
   `%APPDATA%\RogueGalaxy\Recompiled\settings.ini`.

## Words you will run into

| Word | Meaning |
|---|---|
| EE | The PS2's main processor, the one whose code was translated |
| ELF | The game's program file, `SCUS_974.90` |
| Overlay | One of the code files in `disc/BIN/`, loaded only for some parts of the game |
| Recompiler | The tool that translates the game's code into C++ (PS2Recomp) |
| Runtime | The part that stands in for the PS2's hardware and system |
| Guest / host | The game and its PS2 world / your PC |
| Hook | Our function put in place of one of the game's |
| Function map | The list of where each of the game's functions starts and ends, with its name ([MAPS.md](MAPS.md)) |
| RGDecomp | A separate project that turned the game's code into readable C; the function names come from it |
| GS | The PS2's graphics chip, imitated on your graphics card |
| VU1 | The PS2's processor for 3D models |
| `disc/` | Your extracted copy of the game's disc |

## Where to go next

- [OPTIONS.md](OPTIONS.md): every option and what it does
- [BUILDING.md](BUILDING.md): building by hand, and where everything is
- [FINDING-THINGS.md](FINDING-THINGS.md): finding where the game keeps something
- [HOOKS.md](HOOKS.md): writing hooks
- [STATUS.md](STATUS.md): how the work went
