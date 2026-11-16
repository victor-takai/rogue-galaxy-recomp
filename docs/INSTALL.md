# Installing and updating

Everything about getting the game on your PC and keeping it up to date. The
short version is on the [README](../README.md#install-windows).

- [Install (Windows)](#install-windows)
- [Undub (optional)](#undub-optional)
- [European texts (optional)](#european-texts-optional)
- [Update](#update)
- [Saves from PCSX2](#saves-from-pcsx2)
- [Uninstall](#uninstall)

## Install (Windows)

You need:

- Windows 10 or 11 (64-bit), about 16 GB free and an internet connection
- Your own image of Rogue Galaxy USA (SCUS-97490). It must be an
  uncompressed `.iso`, as Redump's (6,967,721,984 bytes); turn a `.chd` or
  `.cso` back into an `.iso` first. Which images work: [DISC.md](DISC.md).

Then:

1. Download `RogueGalaxy-<version>.zip` from the
   [releases](https://github.com/victor-takai/rogue-galaxy-recomp/releases)
   page (not "Source code").
2. Extract it to a folder on this PC, outside OneDrive, such as `C:\`.
   You get `C:\RogueGalaxy`, your game folder.
3. Double-click `Install.bat` and choose your disc image.
4. Wait about 20 minutes on a fast PC, up to an hour on a slower one. It
   downloads the build tools (about 300 MB) into the folder, extracts the
   disc and builds the game. Nothing is installed in Windows.
5. Play with `Rogue Galaxy.exe`.

If it stops, `install.log` says why; run `Install.bat` again to continue
where it stopped (`Rogue Galaxy.exe` offers it too). If an antivirus
removes the compiler, restore it and run `Install.bat` again.

A long path, or one with spaces or accented letters, is fine: while it
builds, the build tools see the folder by a short name in
`C:\RogueGalaxyBuild` (on the drive Windows is on), which leads to the
folder and is removed when the install ends; an install closed before then
leaves it until the next one.

In a launcher that installs into its own folder, such as QuiverLauncher,
Play installs the game the first time, in a window of its own, and starts
it from then on. If the install stops halfway, Play offers to go on with
it; if it stops in its first minutes, before `Rogue Galaxy.exe` is made,
Play may open a build tool instead: run `Install.bat` from the game's
folder then.

## Undub (optional)

The Japanese voices and movies. You need your own image of the Japanese
Director's Cut (SCPS-17013).

1. After installing, double-click `Install-Undub.bat` and choose that image
   (or drop it onto the file). It copies about 3 GB of voices and movies
   into the folder.
2. Turn the undub on in
   [F1 > Settings > Language](OPTIONS.md#language).
3. Start the game again.

The text stays in its language: English, or one of the
[European texts](#european-texts-optional) with the Japanese voices.

`Install-Undub.bat` also reads what the Director's Cut changed in
battles, which the US release did not get. Three options in
F1 > Settings > Game turn it on, each part on its own
([DIRECTORS-CUT.md](DIRECTORS-CUT.md)). The voices can stay English.

## European texts (optional)

The text in French, German, Spanish or Italian. You need your own image of
the European disc (SCES-54552).

1. After installing, double-click `Install-European.bat` and choose that
   image (or drop it onto the file). It builds the four languages' texts,
   about 320 MB, into the folder in a minute.
2. Pick the language in
   [F1 > Settings > Language > Text](OPTIONS.md#language).
3. Start the game again.

The voices stay as they are (English, or Japanese with the undub).

It is experimental: played in Spanish and tested in all four languages up to
the first scenes, but the rest of the story has not been seen in them yet. If
something looks wrong (text, a menu, the picture), open an
[issue](https://github.com/victor-takai/rogue-galaxy-recomp/issues) with a
screenshot and the game's log. Picking English again brings the game back as
it was.

## Update

The game tells you at start when a new version is out, and F1 > Rogue
Galaxy > About links to it. Nothing updates by itself.

To update, double-click `Update.bat`. It downloads the newest release,
replaces only the files that changed and rebuilds what depends on them.
That takes a few minutes, or about as long as the first install when the
new version changes how the game's code is translated. The build tools
stay unless the new version needs newer ones.

What is kept: your disc, saves (`disc\mc0`), settings
(`%APPDATA%\RogueGalaxy\Recompiled`), textures, the undub's files
(`disc\JA`) and the European texts (`disc\EU`). `update.log` and
`install.log` say what happened.

A zip downloaded by hand from the
[releases](https://github.com/victor-takai/rogue-galaxy-recomp/releases)
page can be dropped onto `Update.bat` instead.

### From 0.1.0

Version 0.1.0 has no `Update.bat`, so the first update from it is by hand:

1. Download the new `RogueGalaxy-<version>.zip`.
2. Extract it where your game folder is (for `C:\RogueGalaxy`, into `C:\`),
   so that the zip's `RogueGalaxy` folder merges into yours. Let Windows
   replace the files it asks about.
3. Double-click `Install.bat` again. It does not ask for the disc image.

From then on, `Update.bat` is in the folder.

### Into a new folder

This works too, but the tools are downloaded, the disc extracted and
everything built again, as the first time. Copy `disc\mc0` (and `disc\mc1`)
from the old folder to keep your saves, `disc\JA` to keep the undub and
`disc\EU` to keep the European texts.

## Saves from PCSX2

The memory card, `disc\mc0`, has the same layout as a PCSX2 folder memory
card, so saves copy over as they are.

1. In PCSX2's memory card settings, convert your card to a folder card
   (keep the original).
2. Back up `disc\mc0`: a save with the same folder name replaces the one in
   that slot.
3. Copy the card's `BASCUS-97490LV5RG_xx` folders, one per Rogue Galaxy
   save, into `disc\mc0`.

Only saves from the USA release work (their folders start with
`BASCUS-97490`).

## Uninstall

Delete the game folder (your saves are in `disc\mc0`) and
`%APPDATA%\RogueGalaxy\Recompiled`, which holds the settings and
screenshots.
