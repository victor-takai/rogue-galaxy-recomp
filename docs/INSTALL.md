# Installing and updating (Steam Deck)

Everything about getting the game on your Steam Deck and keeping it up to
date. The short version is on the [README](../README.md#install-steam-deck).

- [Install](#install)
- [Undub (optional)](#undub-optional)
- [European texts (optional)](#european-texts-optional)
- [Update](#update)
- [Saves from PCSX2](#saves-from-pcsx2)
- [Uninstall](#uninstall)

## Install

You need:

- A Steam Deck (SteamOS), about 24 GB free (the SD card works) and an
  internet connection.
- Your own image of Rogue Galaxy USA (SCUS-97490). It must be an
  uncompressed `.iso`, as Redump's (6,967,721,984 bytes); turn a `.chd` or
  `.cso` back into an `.iso` first. Which images work: [DISC.md](DISC.md).

Then, in desktop mode:

1. Download `RogueGalaxy-Setup-x86_64.AppImage` from the
   [releases](https://github.com/touyoo0/deck-galaxy/releases) page and
   double-click it (Execute). It needs only what SteamOS has: Python with
   GTK 4, curl, git and distrobox. (`RogueGalaxy-Setup.py` is the same
   installer as a script: `python3 RogueGalaxy-Setup.py`.)
2. One window asks for everything: your disc image, where to install (the
   internal storage, the SD card or another folder) and the options. Keep
   "Add Rogue Galaxy to Steam" on; tick "Add the European image" to choose
   that disc too (see below). The Japanese voices are added afterwards (see
   below). Then press Install.
3. Wait while the game is built (10 to 30 minutes; the window shows the
   steps, and "Details" the log). If it stops, run the installer again with
   the same folder: it goes on where it stopped. `build/setup.log` in the
   game's folder keeps the whole log.
4. Switch to game mode: Rogue Galaxy is in the library, under Non-Steam.

SteamOS has a read-only root and no compiler, so the installer creates a
distrobox container named `rg-build` (Ubuntu 24.04), installs the build tools
in it and builds there. The game it builds runs on SteamOS itself and needs
nothing from the container. The game opens on its home screen (Play, Update);
View + Menu opens its menu. The log of the game is `build/last-run.log`.

Without the installer, from a terminal: `git clone --recurse-submodules
https://github.com/touyoo0/deck-galaxy.git`, then `./install.sh --iso
FILE` in the folder.

## Undub (optional)

The Japanese voices and movies. You need your own image of the Japanese
Director's Cut (SCPS-17013).

The installer offers it on its Options page. Later, from a terminal in the
game's folder:

```bash
python3 setup.py extract-japanese --iso /path/to/japanese.iso
```

It copies about 3 GB of voices and movies into `disc/JA`. Turn the undub on
in [F1 > Settings > Language](OPTIONS.md#language) and start the game again.
The Director's Cut's battle changes
([DIRECTORS-CUT.md](DIRECTORS-CUT.md)) come with it.

## European texts (optional)

The text in French, German, Spanish or Italian. You need your own image of
the European disc (SCES-54552).

Also on the installer's Options page, or later:

```bash
python3 setup.py extract-european --iso /path/to/european.iso
```

It builds the four languages' texts, about 320 MB, into `disc/EU`. Pick the
language in [F1 > Settings > Language > Text](OPTIONS.md#language). The voices
stay as they are. It is experimental: report problems with a screenshot and
`build/last-run.log`.

## Update

The game opens on a home screen with **Play**, **Update** and **Quit**. When
a newer release is out, Update names it ("Update to 0.4.0"). Press it and the
game does the rest in its own window, with a progress bar: it downloads the
release's source archive, checks its checksum, unpacks it over the folder and
builds it (`update.sh`, the same build as `install.sh`; the first update
after a new version of the game's code takes as long as the install, later
ones less). When it is done the game starts again by itself. Your disc,
saves and settings are kept.

If the update fails, the message says why (`build/update.log` has the
detail) and the game you had still plays; Update tries again.

Without a controller, or from a terminal, the same thing is:

```bash
git pull --recurse-submodules
./install.sh
```

## Saves from PCSX2

The memory card, `disc/mc0`, has the same layout as a PCSX2 folder memory
card, so saves copy over as they are.

1. In PCSX2's memory card settings, convert your card to a folder card
   (keep the original).
2. Back up `disc/mc0`: a save with the same folder name replaces the one in
   that slot.
3. Copy the card's `BASCUS-97490LV5RG_xx` folders, one per Rogue Galaxy
   save, into `disc/mc0`.

Only saves from the USA release work (their folders start with `BASCUS-97490`).

## Uninstall

Delete the game folder (your saves are in `disc/mc0`), the `rg-build`
container (`distrobox rm rg-build`) and the menu entry
`~/.local/share/applications/rogue-galaxy.desktop`.
