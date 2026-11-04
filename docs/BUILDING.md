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
downloads raylib and SDL3, glslang (for the GPU backend's shaders) when no
GLSL compiler is installed, and, on Windows, a prebuilt LGPL FFmpeg whose
hash the framework pins. `-DRG_GS_SDLGPU=OFF` at configure leaves out the
GPU backend, and with it the menu: the game draws with the software GS.

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

## Rebuilding after a change

Every step keeps what it already made, so a change is rebuilt from the step
it touches on down. From the repository root:

| What changed | Run |
|---|---|
| `src/`, `CMakeLists.txt`, `thirdparty/dq8-gfx`, `thirdparty/imgui` | `cmake --build build/game` |
| `config/SCUS_974.90/` (function maps, recompiler configs, `vu1-programs.txt`, `vu-blocks.txt`), `tools/mwo3/`, `tools/funcmap/` | `python setup.py recompile`, then `python setup.py build` |
| `patches/ps2recomp/`, or the commit `thirdparty/PS2Recomp` is pinned to | `python setup.py tools`, then `recompile`, then `build` |
| A new version of a git checkout (`git pull`), or in doubt | `python setup.py install` (on Windows, `Install.bat`) |
| A new release, in a folder `Install.bat` built | `Update.bat` ([`tools/install/update.ps1`](../tools/install/update.ps1): downloads the zip, writes only the files whose bytes differ, deletes from the code folders what the new version no longer has, then runs the installer) |

What each step keeps:

- `tools` applies only the patches not yet applied (the ones applied are
  listed in `thirdparty/PS2Recomp/.rg-patches-applied`; a checkout that lost
  them is noticed and patched again) and rebuilds the recompiler
  incrementally.
- `recompile` runs the recompiler on all six images (about a minute) into
  a staging folder and copies into `build/generated/` only the files whose
  bytes changed; the VU manifests (`build/vu1-manifest`,
  `build/vu-blocks-manifest`) are rewritten only when they changed too,
  since CMake recompiles every VU shard after a newer one. So the next
  build compiles just what changed: with nothing changed, `recompile`
  takes 35 s and the build 1 s. A change to what the recompiler generates
  (a patch in `patches/ps2recomp/` that changes its output) touches most
  of the generated files, and that build takes about as long as the first.
- `build` configures `build/game` the first time only (CMake reconfigures by
  itself when a `CMakeLists.txt` or a patch changes) and lets Ninja rebuild
  what depends on the changed files.
- `install` skips the disc once `disc/manifest.json` and the ELF are there
  (delete the manifest to extract again), then runs `tools`, `recompile` and
  `build` as above. `Install.bat` also keeps the toolchain it downloaded
  while its hash matches the version pinned in `tools/install/install.ps1`.

## Linux

The game builds on Linux too, but it is less tested than on Windows: it was
built and played on Arch Linux, with clang 23 and GNU ld (pull request #2).
The steps are the ones above, with these differences:

- clang is required: `setup.py` configures with `clang` and `clang++`.
- FFmpeg comes from the system, through pkg-config. Install pkg-config and
  the development packages of libavcodec, libavformat, libavutil,
  libswresample and libswscale.
- There is no `Rogue Galaxy.exe`. Run the game from the repository's folder:
  `build/game/src/roguegalaxy disc/SCUS_974.90`.
- Some diagnosis tools do nothing there: the profiler (`RG_PROFILE`), the
  hardware watch (`RG_HWWATCH`), the host stacks in the stall report and
  the crash report. The check for a newer release is Windows only too.

## Steam Deck (untested)

The installer needs Windows, but the game it builds can go to a Steam Deck
and run through Proton. On the Windows PC, after the install:

    python setup.py portable --out D:\RogueGalaxyDeck --saves

copies the game, its DLLs, `disc/` and the borders into that folder (about
6.5 GB; `--saves` adds your memory cards, which then go their own way on
the Deck). Texture packs are not copied: copy `textures\packs` (and
`textures\hd`) into the new folder's `textures\` by hand. Copy the folder
to the Deck, add `Rogue Galaxy.exe` to Steam as a non-Steam game, and
force a Proton version in its Properties > Compatibility. It starts in
fullscreen there; Back + Start opens the menu (F1).

## Layout

| Path | What |
|---|---|
| `Install.bat`, `tools/install/` | The Windows installer: build tools into `toolchain\`, then `setup.py install` |
| `Update.bat`, `tools/install/update.ps1` | Brings a folder `Install.bat` built to the newest release and rebuilds what changed (a git checkout uses `git pull` instead) |
| `Install-Undub.bat`, `tools/install/install-undub.ps1` | The Japanese voices from your own Japanese disc, into `disc/JA` (`setup.py extract-japanese`) |
| `CHANGELOG.md` | What each release brings, and its known issues |
| `setup.py` | `verify`, `extract`, `extract-japanese`, `info`, `overlays`, `check`, `recompile`, `tools`, `build`, `install`, `portable` (and `make-hashes`, `maps`, `vu1-manifest`, `vu-blocks-manifest` for maintainers) |
| `CMakeLists.txt`, `src/` | The game executable: `main.cpp`, the hooks, one file per feature ([HOOKS.md](HOOKS.md)), the overlay manager (`src/overlay/`), the diagnosis tools (`src/debug/`: traces, watches, profiler, stall watch, GS census, self-test) and the Windows launcher, `Rogue Galaxy.exe` (`src/launcher/`) |
| `cmake/framework_patches.cmake` | Applies `patches/ps2recomp/` to the framework, at configure and from `setup.py tools` |
| `debug/` | Launchers for diagnosis: `RogueGalaxy.bat` (the game in a terminal), `-profile`, `-hitches`, `-capture` and `-collect` ([DEVELOPMENT.md](DEVELOPMENT.md)) |
| `borders/` | An image you put here fills the window beside the 4:3 picture (`borders/README.txt`) |
| `config/SCUS_974.90/` | `hashes.json` (known-good dump), `overlays.toml` (MWo3 table, loader hook), `funcmap.toml` (map sources and corrections), `functions_<image>.csv` (function maps), `recomp/<image>.toml` (ps2_recomp configs, SDK bindings), `vu1-programs.txt` and `vu-blocks.txt` (VU1 and VU0 microcode locations) |
| `tools/disc/iso9660.py` | Read-only ISO9660 reader for dual-layer PS2 DVDs |
| `tools/mwo3/mwo3.py` | MWo3 overlay header parser, ELF cross-check, overlay-to-ELF wrapper |
| `tools/funcmap/` | Function-map builder and the checks behind `setup.py check` |
| `tools/bench/` | `walk.sh NAME [SCALE] [FPS]`: the scripted Salgin walk on the pad clock, for speed (3x, 60 fps mode, the `[gfx]` intervals before the logged frame) and for regressions (1x, 30 fps mode, `cmpvram.py` against a reference frame made with the same save); `PROFILE=1` samples the threads as well. It needs the maintainer's input scripts in `build/run/` and saves in `disc/mc0`, which are not in the repository |
| `tools/mipsdis.py` | Disassembler for the main program, with the map's names ([FINDING-THINGS.md](FINDING-THINGS.md)) |
| `tools/hang/` | A frozen game's memory dumped (`dumpmem.ps1`), a thread's call chain read from it (`stackwalk.py`), an AI script decoded (`aiscript.py`) |
| `tools/profile/report.py` | Reads the profiles `RG_PROFILE` writes ([DEVELOPMENT.md](DEVELOPMENT.md)) |
| `tools/vu1/` | Study of the VU1 programs: disassembler, census, captures, and the oracle the native ones are checked against ([VU1-NATIVE.md](VU1-NATIVE.md)) |
| `tools/texture_pack/` | Making the larger 2D art from what `debug\RogueGalaxy-collect.bat` collected (`make-pack.bat`), and checks of the game's zip and DDS readers ([TEXTURE_PACKS.md](TEXTURE_PACKS.md)) |
| `tools/release/` | `source_zip.py`: the release's source archive, with the submodules; `release_notes.py VERSION`: the release page's text, from that version's section of the CHANGELOG |
| `thirdparty/PS2Recomp` | Framework submodule; `patches/ps2recomp/` holds our changes to it, applied on configure ([`patches/README.md`](../patches/README.md)) |
| `thirdparty/dq8-gfx` | SDL3 GPU GS backend and in-game menu from DQ8-Recompiled (GPL-3.0), see its README |
| `thirdparty/imgui` | Dear ImGui submodule (MIT), which the menu draws with |
| `docs/` | Every page, by who it is for: [`docs/README.md`](README.md) |
| `disc/` | Your extracted disc (gitignored) |
