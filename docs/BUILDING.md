# Building by hand

For developers and other systems: these are the steps
[`Install.bat`](../README.md#install-windows) runs on Windows.

## Setup

Requirements: Git, Python 3.11+, CMake 3.24+ with Ninja, clang with
C++20, and your own dump of the game ([`DISC.md`](DISC.md) lists
the one that works). The versions tested are the ones `Install.bat`
downloads, pinned in [`tools/install/install.ps1`](../tools/install/install.ps1):
llvm-mingw 20260616 (LLVM 22.1.8), CMake 3.31.12, Ninja 1.13.2, Python 3.12.

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
# 1. The recompiler: applies patches/ps2recomp to the pinned framework, then
#    builds ps2_recomp and ps2_analyzer in build/tools (Ninja, clang)
python setup.py tools

# 2. Recompile your disc's code to C++ in build/generated/<image>/ (about a
#    minute): runs `check`, wraps the overlays, runs ps2_recomp on all six images
python setup.py recompile

# 3. Build the game against the framework runtime in build/game
python setup.py build

# 4. Run it on your extracted disc (on Windows, double-click "Rogue Galaxy.exe")
build/game/src/roguegalaxy disc/SCUS_974.90
```

`python setup.py install --iso "/path/to/Rogue Galaxy (USA).iso"` does it
all in one go: verify, extract (skipped once `disc/` holds the game), tools,
recompile and build. The patches must be in before the recompiler is built:
some change the code it generates, and `tools` applies them first.

Run `setup.py recompile` again after changing a map or config, then rebuild.

**VU1 programs.** 3D scenes need the game's VU1 microprograms compiled ahead
of time to run at full speed. `setup.py recompile` rebuilds them from
[`config/SCUS_974.90/vu1-programs.txt`](../config/SCUS_974.90/vu1-programs.txt)
(where each one sits in the game's files, plus a checksum: no game data) and
your disc into `build/vu1-manifest`, which CMake compiles into the runtime.
Programs the manifest lacks run on the interpreter; record them with
`PS2_VU_PROGRAM_PROFILE=build/vu1-profile` (CMake compiles that folder too),
then configure and build again (maintainers: `python setup.py vu1-manifest`
folds a recording into the manifest). The VU0 micro programs, which run on
the game thread, are compiled as blocks instead, the same way:
`setup.py recompile` rebuilds them from
[`config/SCUS_974.90/vu-blocks.txt`](../config/SCUS_974.90/vu-blocks.txt) and
your disc into `build/vu-blocks-manifest`. A run with
`PS2_VU_AOT_PROFILE=build/vu-blocks/NAME.txt` records the blocks the
interpreter executes (both units; keep the lines starting with `0` for
VU0 alone), and CMake compiles every `build/vu-blocks/*.txt` it finds too
(as many shards at a time as memory allows: each takes 2.2 GB to compile); maintainers:
`python setup.py vu-blocks-manifest` folds the recordings into the manifest.

## Linux and Steam Deck

`./install.sh [--iso FILE] [--container | --native]` does what `Install.bat`
does: it builds on the machine when the tools are there, otherwise (SteamOS,
whose root is read-only and has no compiler) in a distrobox container named
`rg-build`, an Ubuntu 24.04 that it creates. `./rogue-galaxy.sh` is the
launcher (the counterpart of `Rogue Galaxy.exe`): it starts the game, keeps
the log in `build/last-run.log` and allows a single instance. The game it
builds needs only the system's Vulkan, SDL's own dependencies (X11 or
Wayland, ALSA or PulseAudio) and glibc 2.38 or later; the container is only
for building.

By hand, the steps above work as they are, with these needs:

- clang with lld, CMake 3.24+, Ninja, Python 3.11+, git, GNU make, nasm,
  xz, pkg-config and `glslc` (or let the build fetch glslang), plus the
  development packages of Vulkan, X11 (Xext, Xrandr, Xinerama, Xcursor, Xi,
  Xfixes, Xss), xkbcommon, Wayland, libdecor, udev, GL/EGL, ALSA, PulseAudio
  and D-Bus.
- `CC=clang CXX=clang++` in the environment if clang is not your default
  compiler (`setup.py` passes them on to CMake).
- FFmpeg is built from source inside the game (a minimal static build with
  the MPEG-2 decoder and swscale: `PS2X_FFMPEG_BUNDLE`, ON by default on
  Linux, patch 0019), so no FFmpeg package is needed. `-DPS2X_FFMPEG_BUNDLE=OFF`
  uses the system's through pkg-config instead.
- The GNU linker reads each archive once; the game links the generated
  libraries as a rescanned group (`src/CMakeLists.txt`).

On a Steam Deck play in game mode, with `rogue-galaxy.sh` added to Steam as
a non-Steam game: a game started from desktop mode gets Steam's desktop
layout, where the controller acts as a mouse and keyboard. The game starts
in fullscreen when Steam sets `SteamDeck=1` (game mode) and `aspect = auto`
follows the window's shape. In desktop mode the CPU can drop to 400 MHz
during battles (amd-pstate, governor `powersave`); `echo performance | sudo
tee /sys/devices/system/cpu/cpu*/cpufreq/energy_performance_preference`
avoids it until the next reboot.

## Steam Deck through Proton (untested)

The installer needs Windows, but the game it builds can go to a Steam Deck
and run through Proton. On the Windows PC, after the install:

    python setup.py portable --out D:\RogueGalaxyDeck --saves

copies the game, its DLLs, `disc/` and the borders into that folder (about
6.5 GB; `--saves` adds your memory cards, which then go their own way on
the Deck). Copy the folder to the Deck, add `Rogue Galaxy.exe` to Steam as
a non-Steam game, and force a Proton version in its Properties >
Compatibility. It starts in fullscreen there; Back + Start opens the menu
(F1).

## Layout

| Path | What |
|---|---|
| `Install.bat`, `tools/install/` | The Windows installer: build tools into `toolchain\`, then `setup.py install` |
| `CHANGELOG.md` | What each release brings, and its known issues |
| `setup.py` | `verify`, `extract`, `info`, `overlays`, `check`, `recompile`, `tools`, `build`, `install`, `portable` (and `make-hashes`, `maps`, `vu1-manifest`, `vu-blocks-manifest` for maintainers) |
| `CMakeLists.txt`, `src/` | The game executable: build of the generated trees, overlay manager (`src/overlay/`), call tracing (`src/debug/`), `main.cpp`; the Windows launcher, `Rogue Galaxy.exe` (`src/launcher/`) |
| `config/SCUS_974.90/` | `hashes.json` (known-good dump), `overlays.toml` (MWo3 table, loader hook), `funcmap.toml` (map sources and corrections), `functions_<image>.csv` (function maps), `recomp/<image>.toml` (ps2_recomp configs, SDK bindings), `vu1-programs.txt` and `vu-blocks.txt` (VU1 and VU0 microcode locations) |
| `tools/disc/iso9660.py` | Read-only ISO9660 reader for dual-layer PS2 DVDs |
| `tools/mwo3/mwo3.py` | MWo3 overlay header parser, ELF cross-check, overlay-to-ELF wrapper |
| `tools/funcmap/` | Function-map builder and the checks behind `setup.py check` |
| `tools/bench/` | `walk.sh NAME [SCALE] [FPS]`: the scripted Salgin walk on the pad clock, for speed (3x, 60 fps mode, the `[gfx]` intervals before the logged frame) and for regressions (1x, 30 fps mode, `cmpvram.py` against a reference frame made with the same save); `PROFILE=1` samples the threads as well |
| `thirdparty/PS2Recomp` | Framework submodule; `patches/ps2recomp/` holds our changes to it, applied on configure ([`patches/README.md`](../patches/README.md)) |
| `thirdparty/dq8-gfx` | SDL3 GPU GS backend and in-game menu from DQ8-Recompiled (GPL-3.0), see its README |
| `thirdparty/imgui` | Dear ImGui submodule (MIT), which the menu draws with |
| `docs/` | [`STATUS.md`](STATUS.md) (state, plan, what the game needs), [`DISC.md`](DISC.md), [`MAPS.md`](MAPS.md) (function maps, SDK bindings) |
| `disc/` | Your extracted disc (gitignored) |
