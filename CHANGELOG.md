# Changelog

## Unreleased

## 0.2.0 (pre-release)

`Update.bat`, the Japanese voices (undub), texture packs, the F1 menu with
only a controller, and new game options. No game data is included.

**Requires:** Windows 10/11 (64-bit), about 16 GB free, and your own image
of Rogue Galaxy (USA, SCUS-97490).

### Install

1. Download **RogueGalaxy-0.2.0.zip** (not "Source code").
2. Extract it to a short folder, such as `C:\RogueGalaxy`.
3. Run `Install.bat` and pick your disc image.
4. Wait about 20 minutes on a fast PC (up to an hour on a slower one), then
   play with `Rogue Galaxy.exe`.

### Update from 0.1.0

0.1.0 has no `Update.bat`, so this once by hand: extract the zip where your
game folder is, so that its `RogueGalaxy` folder merges into yours (let
Windows replace the files), then run `Install.bat` again. It does not ask
for the disc image, and your saves (`disc\mc0`) and settings stay. From
0.2.0 on, `Update.bat` does it
([README](README.md#update)).

### New

- `Update.bat`: brings an installed folder to the newest release and rebuilds
  only what changed, a few minutes in most cases. A rebuild also no longer
  redoes the compiled 3D programs when nothing changed (every rebuild used
  to spend ten minutes on them).
- Undub: the Japanese voices and movies from your own Japanese Director's Cut disc
  (F1 > Settings > Language, from the next start). Run `Install-Undub.bat` first
  and pick the disc image. The text stays English.
- With an Xbox controller, the game's text shows its buttons (A, B, X, Y,
  LB, RB, LT, RT) instead of the PlayStation's (Game > Controller's
  buttons, on by default).
- The game says when a newer version is out, and the menu's About window
  links to it (Game > Check for new versions, on by default).
- A 3D program the game uses later in Salgin is compiled: it ran on the
  slow path every frame, and the walk there was choppy.
- The field's minimap and compass can be made smaller (Game > Minimap
  size, Compass size, 50% to 100%).
- Weapons can grow faster in battle (Game > Weapon growth, up to 1000%).
- The loading screen's "Press X" shows the Xbox controller's A too.
- Texture packs: above 1x, the 2D art (icons, window pieces, menu
  pictures) can be drawn from larger versions in packs under
  `textures\packs` (folders or zips of PNG or DDS files), each with a switch
  in the menu (F1 > Settings > Textures). The game ships none; the format is
  in [docs/TEXTURE_PACKS.md](docs/TEXTURE_PACKS.md), and
  `debug\RogueGalaxy-collect.bat` collects what a pack can replace from your
  own play.
  A pack made for PCSX2 (`TEX0HASH-CLUTHASH-BITS.dds`) works too, at 1x as
  well, and replaces scenery and character textures; the menus, portraits
  and icons stay as the game draws them. Its pictures stay compressed on the
  GPU, a quarter of the memory.
- The free camera no longer lets a button pressed to move it reach a scene
  waiting for one, and gives the controller back at once when turned off.
- Full bag to storage: an item found when the bag is full goes to the
  transporter's item storage instead of the overflow, or of being lost
  (Game > Full bag to storage).
- Item limit 99: every item that stacks can be carried up to 99 in the bag
  (Game > Item limit 99).
- A seventh bag page: 280 items instead of 240 (Game > Seventh bag page,
  from the next start). Saves made with it still load without it.
- The free camera no longer leaves the game stuck at the battle's "escape?"
  question (a hit can push the leader out of the battle's area while the
  camera flies): the question turns it off, and it can be answered.
- Faster at high internal resolutions: blended draws take one pass instead
  of two where the graphics card allows it, and blurs and draws from
  upscaled pictures cost the graphics card less.
- A Native renderer switch (Game > Native renderer, off by default): the
  work on drawing the game with the PC's renderer goes behind it. It
  changes nothing yet.
- The F1 menu works with only a controller: A (✕) goes into a page and B
  (○) comes back, one level at a time and at last to the game; Y (△)
  reaches the menu bar, L1 and R1 turn the pages, the right stick scrolls.
  A strip along the bottom names the buttons for where the cursor is.
  "Quit game", at the end of the page list, asks first. On Controls, X (▢)
  clears a binding, and holding B cancels one being set.
- F1 sometimes paused the game without showing the menu.
- When the sound driver stops (the black screen of
  [#17](https://github.com/victor-takai/rogue-galaxy-recomp/issues/17)),
  the log says where and why, and no longer fills with the same line.

### Known issues

- Only the USA version is supported.
- It has not been played from start to finish; most of the testing covers
  the early game.
- The heaviest scenes can dip to 50 fps. After the boss at the end of
  Juraika, the frame rate halves at high internal resolutions (about 30 fps
  at 4x).
- A black screen after that boss was reported once
  ([#17](https://github.com/victor-takai/rogue-galaxy-recomp/issues/17)) and
  has not been seen again. If it happens to you, the log
  (`build\last-run.log`) helps.
- In widescreen, menus and text are stretched.
- The Steam Deck, through Proton, is prepared but untested.
- An antivirus may block the compiler during the install: restore it and
  run `Install.bat` again.

## 0.1.0 (pre-release)

First release: the source plus an installer that builds the game on your PC
from your own disc image. No game data is included.

**Requires:** Windows 10/11 (64-bit), about 16 GB free, and your own image
of Rogue Galaxy (USA, SCUS-97490).

### Install

1. Download **RogueGalaxy-0.1.0.zip** (not "Source code").
2. Extract it to a short folder, such as `C:\RogueGalaxy`.
3. Run `Install.bat` and pick your disc image.
4. Wait about 20 minutes on a fast PC (up to an hour on a slower one), then
   play with `Rogue Galaxy.exe`.

### Highlights

- 60 fps, or the original 30
- Widescreen, up to 32:9
- Up to 8x internal resolution
- In-game menu (F1): graphics, sound, controls
- Game options: camera, field of view, draw distance, instant text,
  encounter rate, experience, money, item drops, shop prices and more

### Known issues

- Only the USA version is supported.
- The heaviest scenes can dip to 50 fps.
- In widescreen, menus and text are stretched.
- An antivirus may block the compiler during the install: restore it and
  run `Install.bat` again.
