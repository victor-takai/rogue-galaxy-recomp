# Rogue Galaxy Recompiled

**Rogue Galaxy** (Level-5 / SCE, PlayStation 2) as a native Windows game: a
static recompilation of the USA release (SCUS-97490), built on
[PS2Recomp](https://github.com/ran-j/PS2Recomp)
([how it works](docs/HOW-IT-WORKS.md)). You build it from your own disc
image; nothing of the game is distributed.

<p align="center">
  <img src="docs/images/salgin-plaza.jpg" width="49%" alt="Salgin in widescreen">
  <img src="docs/images/battle.jpg" width="49%" alt="A battle in Salgin">
</p>

- 60 fps
- Widescreen
- Up to 8x internal resolution
- In-game menu (F1)
- [New game settings](docs/OPTIONS.md)
- Japanese voices and movies (undub)
- The Japanese Director's Cut's battle balance, enemy behavior and party
  AI, as options
- Text in French, German, Spanish or Italian (experimental)
- [Texture packs](docs/TEXTURE_PACKS.md), PCSX2's included

## What to expect

This is an early pre-release (0.2.0). The whole game plays, but:

- **It has not been played from start to finish.** Most testing covers the
  early game; later chapters, the Factory and Insectron are unchecked.
- **Expect bugs**: graphics glitches, slowdowns, maybe a freeze. Save often,
  in more than one slot, and back up `disc\mc0` now and then.
- **Only the USA disc** (SCUS-97490) works.

Known issues are in the [CHANGELOG](CHANGELOG.md); what is being worked on
is in [docs/STATUS.md](docs/STATUS.md).

**Found a problem?** Open an [issue](../../issues), say what you were doing
and attach `build\last-run.log` (`install.log` for the install). The log of
the run before is `build\last-run.prev.log`.

## Install (Windows)

You need Windows 10 or 11 (64-bit), about 16 GB free, an internet
connection and your own uncompressed `.iso` of Rogue Galaxy USA
(SCUS-97490).

1. Download `RogueGalaxy-<version>.zip` from the [releases](../../releases)
   page (not "Source code").
2. Extract it to a short path, such as `C:\`.
3. Double-click `Install.bat` and choose your disc image.
4. Wait about 20 minutes on a fast PC, up to an hour on a slower one.
5. Play with `Rogue Galaxy.exe`.

To update later, double-click `Update.bat`.

The full guide, with the undub, the European texts, updating, saves from
PCSX2 and uninstalling, is in [docs/INSTALL.md](docs/INSTALL.md).

## Controls

A gamepad, or the keyboard and mouse: the sword on the left click, the
guard on the right, jump on Space, the gun on Q, the menus on Tab. All of
it can be changed in F1 > Settings > Controls.

| Pad | Keyboard and mouse |
|---|---|
| Left stick | W A S D (hold Left Ctrl to walk) |
| Right stick | The mouse, or I J K L |
| D-pad | Arrow keys; the wheel is left and right |
| ✕ / ○ / ▢ / △ | Left click or E / Space or Esc / Q / Tab |
| L1 / R1 / L2 / R2 | 1 / Right click / 2 / C |
| L3 / R3 | 3 / V |
| Start / Select | Enter / M |

F11 or Alt+Enter toggles fullscreen. F12 takes a screenshot.

### The F1 menu

**F1** (or the Guide button, or Back+Start) opens the menu: display, sound,
controls, the game options, Language (the text's language, the undub) and
Textures. Most game options are also on the game's own Settings screen
(System > Settings). Every option and where it is:
[docs/OPTIONS.md](docs/OPTIONS.md).

With a controller: ✕ opens a page, ○ goes back, △ reaches the menu bar,
L1 and R1 turn the pages.

<p align="center"><img src="docs/images/menu-game.jpg" width="70%" alt="The F1 menu's Game page"></p>

## More

Every page, by who it is for: [docs/README.md](docs/README.md). The main
ones:

- [docs/INSTALL.md](docs/INSTALL.md): installing, the undub, the European
  texts, updating
- [CHANGELOG.md](CHANGELOG.md): each release, and its known issues
- [docs/OPTIONS.md](docs/OPTIONS.md): every option
- [docs/HOW-IT-WORKS.md](docs/HOW-IT-WORKS.md): the project in plain words
- [docs/STATUS.md](docs/STATUS.md): where things stand, and the work log
- [docs/BUILDING.md](docs/BUILDING.md): building by hand
- [docs/HOOKS.md](docs/HOOKS.md): changing the game's behaviour

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
