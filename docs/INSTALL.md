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

- A Steam Deck (SteamOS), about 20 GB free (the SD card works) and an
  internet connection.
- Your own image of Rogue Galaxy USA (SCUS-97490). It must be an
  uncompressed `.iso`, as Redump's (6,967,721,984 bytes); turn a `.chd` or
  `.cso` back into an `.iso` first. Which images work: [DISC.md](DISC.md).

Then, in desktop mode, in Konsole:

```bash
git clone --recurse-submodules https://github.com/thibault-lgm/deck-galaxy.git
cd deck-galaxy
./install.sh --iso /path/to/rogue-galaxy.iso
```

SteamOS has a read-only root and no compiler, so `install.sh` creates a
distrobox container named `rg-build` (Ubuntu 24.04), installs the build tools
in it and builds there. It takes 10 to 30 minutes. The game it builds runs on
SteamOS itself and needs nothing from the container. Then:

1. Add `rogue-galaxy.sh` to Steam as a non-Steam game (Games > Add a
   Non-Steam Game > Browse). A game started from desktop mode gets Steam's
   desktop layout, where the controller acts as a mouse and keyboard, so
   switch to game mode to play.
2. The game starts in fullscreen when Steam sets `SteamDeck=1` (game mode).
   View + Menu opens its menu.

If it stops, run `./install.sh` again: it goes on where it stopped. The log
of the game is `build/last-run.log` (the run before: `build/last-run.prev.log`).

## Undub (optional)

The Japanese voices and movies. You need your own image of the Japanese
Director's Cut (SCPS-17013).

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

```bash
python3 setup.py extract-european --iso /path/to/european.iso
```

It builds the four languages' texts, about 320 MB, into `disc/EU`. Pick the
language in [F1 > Settings > Language > Text](OPTIONS.md#language). The voices
stay as they are. It is experimental: report problems with a screenshot and
`build/last-run.log`.

## Update

The game tells you at start when a new version is out; nothing updates by
itself. To update:

```bash
git pull --recurse-submodules
./install.sh
```

It keeps the extracted disc and rebuilds only what changed.

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
