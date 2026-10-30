# Changelog

## Unreleased

- With an Xbox controller, the game's text shows its buttons (A, B, X, Y,
  LB, RB, LT, RT) instead of the PlayStation's (Game > Controller's
  buttons, on by default).
- The game says when a newer version is out, and the menu's About window
  links to it (Game > Check for new versions, on by default).
- Above 1x, the letters of the dialogue, menus and story are traced into
  smooth outlines and drawn at the internal resolution, with a round black
  border or a solid shadow, instead of enlarged as blocks; the Xbox
  buttons in the text and the ® are drawn sharp too (Game > Sharp
  letters, on by default).
- A 3D program the game uses later in Salgin is compiled: it ran on the
  slow path every frame, and the walk there was choppy.
- The field's minimap and compass can be made smaller (Game > Minimap
  size, Compass size, 50% to 100%).
- Weapons can grow faster in battle (Game > Weapon growth, up to 1000%).
- The loading screen's "Press X" shows the Xbox controller's A too.
- Save anywhere: in the field, R1 + Triangle opens the transporter's menu
  (Game > Save anywhere, off by default). Such a save loads at the area's
  transporter.
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
