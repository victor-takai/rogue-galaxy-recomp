# Options

<p align="center"><img src="images/menu-game.jpg" width="70%" alt="The F1 menu's Game page"></p>

**F1** opens the top bar, along the top of the window: Rogue Galaxy
(About, Shortcuts, Quit), Game (pause, fast-forward, mute, screenshots),
Settings, View (fullscreen, the frame-rate counter, the renderer
statistics) and Help. The Guide button, or Back + Start, opens the
Settings window at once; L1 and R1 turn its pages.

With only a controller (the menu names its buttons as the controller does:
A and ✕ are the same button, B and ○, X and ▢, Y and △):

- In the page list, A or Right goes into the page; moving along the list
  shows each page.
- B comes back one level at a time: out of a slider, a list or the top
  bar, then from the page to the page list, and from the page list to the
  game.
- Y reaches the top bar. L1 and R1 turn the pages, the left stick moves
  like the D-pad and the right stick scrolls.
- A strip along the bottom of the window names the buttons for where the
  cursor is.
- "Quit game", at the end of the page list, asks first, as does Quit in
  the top bar.
- On Controls, X clears a binding. While one is being set, a tap of B binds
  B and holding it cancels.

Settings has seven pages, written here as F1 > Settings > Game and so on:

- **Display**: window or fullscreen, vertical sync, internal resolution
  (2x by default), aspect ratio, scaling filter, **Borders**, what fills
  the sides of a 4:3 picture (black, the picture blurred, or your own image
  from the `borders` folder), integer scaling, and **Sharper picture**,
  which drops the PS2's line blending (it only blurs on a monitor).
- **Sound**: volume and muting.
- **Controls**: controller and keyboard remapping (a key, a mouse button
  or the wheel), dead zones, and **Walk (hold)**, which makes the keys,
  which always push the stick all the way, walk instead of run. In the
  game's menus, talks and scenes the right click and Esc are Circle (back)
  and the wheel the D-pad's up and down, whatever they are bound to.
- **Game**: the game options below.
- **Language**: the text's language and the undub, [below](#language).
- **Textures**: a switch for each texture pack found
  ([TEXTURE_PACKS.md](TEXTURE_PACKS.md)).
- **Interface**: the menu's size, notifications, pause and fast-forward,
  the frame-rate counter.

An option that does nothing as things stand stays on its page, greyed
out, with a line under it saying what would make it work: Mouse
sensitivity and Invert mouse up and down until Mouse camera is on, the
undub until `Install-Undub.bat`
has read your Japanese disc, Borders while the picture fills the window,
a language of Text whose files are missing.

| Key | |
|---|---|
| F2 | Pause |
| F3 | Frame-rate counter |
| F4 | Fast-forward |
| F6 | Free camera |
| F11 or Alt+Enter | Fullscreen |
| F12 | Take screenshot |
| Shift+F12 | Screenshot with the menu |

## Game options

F1 > Settings > Game, in these sections:

**Camera and view**

| Option | |
|---|---|
| 60 frames per second | On by default; off, the game's own 30. From the next start. |
| Widescreen | Off by default. A wider view, from 4:3 to 32:9; with Display > Aspect ratio on Auto (the default) it fills the window. With the picture shown at 4:3 (Aspect ratio on 4:3 or Native, or a window no wider than 4:3) it does nothing, and the menu greys it out. |
| Minimap size | 0.5x to 1.0x the game's, toward its corner. |
| Compass size | 0.5x to 1.0x the game's, toward its corner. |
| Field of view | 40 to 90 degrees (the game's is 52). |
| Camera distance | 0.7x to 1.6x the game's. |
| Faster camera rotation | The camera turns twice as fast. |
| Mouse camera | On by default. The mouse turns and tilts the camera, exploring, in battle and in the first-person view: to the right looks right and up looks up, whatever the game's camera options. The tilt stays where the mouse leaves it; locked on a target in battle, the camera only turns, as with the stick. Once you press a key of the game's or click, the mouse stays in the window with the cursor hidden; F1, the pause (F2), Alt+Tab or five seconds on the controller alone let it go. Playing on a controller only, the cursor is never taken. |
| Mouse sensitivity | 0.25x to 4.0x; at 1.0x an inch at 1,600 dpi turns about 100 degrees. |
| Invert mouse up and down | Off by default. Moving the mouse up looks down. |
| Draw distance | 1.0x to 4.0x the game's, from the next map loaded. |
| Free camera (F6) | The party stands still and the controller moves the camera. Start or F6 to leave. |

**Convenience**

| Option | |
|---|---|
| Buttons in the game's text | The buttons in the game's text follow what you play with: an Xbox controller's A, B, X, Y, LB, RB, LT and RT, or on the keyboard and mouse the keys (a letter or digit, TAB, ESC and the like, the space bar) and mouse buttons, as bound in Controls. On by default. |
| Skip the opening | Straight to the title screen. |
| Quick loading recap | The story so far appears whole, and you can go on as soon as it has loaded. |
| Instant text | Dialogue a page at a time instead of letter by letter. |
| Chests on the map | Unopened chests show on the map in the parts explored. |
| Fast transporters | Transporter warps play up to twice as fast. |
| Check for new versions | At each start, asks GitHub for this project's releases and says when a newer one is out (Rogue Galaxy > About links to it). On by default. |
| Full bag to storage | An item found when the bag has no room for it goes to the transporter's item storage, instead of the overflow you have to sort out (or losing it when that is full too). Shops and the Factory still need room in the bag. |
| Item limit 99 | Every item that stacks can be carried up to 99 in the bag; the game holds some (healing items, boosts) to 20, 30 or 50. |
| Seventh bag page | The bag holds 280 items instead of 240; the transporter's storage keeps its 320. A save made with it loads without it too, the seventh page's items kept out of sight until it is back on. From the next start. |

**Battles and rewards**

The numbers are against the game's own, 1.0x.

| Option | |
|---|---|
| Encounter rate | 0.0x (no random battles) to 3.0x. |
| Experience from battles | 0.0x to 5.0x. |
| Money from battles | 0.0x to 5.0x. |
| Item drops | 0.0x to 10.0x. |
| Weapon growth | 1.0x to 10.0x: each battle fills a weapon's gauge (15 to full), then its attributes, by 1 point, or more. |
| Shop prices | 0.1x to 2.0x. |
| Action gauge speed | 0.25x to 4.0x. |
| Director's Cut balance | Off by default. Battles with the numbers of the Japanese Director's Cut (SCPS-17013): 85 of the 374 enemy groups are smaller (the Cactulus group in Rosa comes 2 to 3 instead of 4 to 5), 75 enemies leave about 1.5 times the money, some enemies are weaker and the 10 Area Masters stronger, bigger enemies are harder to knock down and bosses never are. Experience, the party, the weapons and the damage formula stay the same. Works with the European texts too. Needs your Japanese disc: run `Install-Undub.bat` (it serves all three Director's Cut options). Takes effect at the next start. |
| Director's Cut enemy behavior | Off by default. The enemies' attacks and AI of the same release: enemies face you before they attack, some attack from farther away, and the party's jump attacks home in on flying enemies knocked out of the air. Works with or without the balance. Needs the same disc. Takes effect at the next start. |
| Director's Cut party AI | Off by default. The allies' AI of the same release: an ally low on health sometimes falls back to the leader and guards. Needs the same disc. Takes effect at the next start. |

**Rendering**

| Option | |
|---|---|
| Native renderer | Off by default. For development: the work on drawing the game with the PC's renderer, instead of through the emulation of the PS2's graphics chip, goes behind it. For now, most of the work the PS2's vector unit did on the models is done on the PC and its graphics card; the models are still drawn through the emulation. Takes effect at the next start. |

**Developers**

| Option | |
|---|---|
| Developer tools | What the developers left in the game. In the main menu: R3 shows every item (R1 + R3 as a list, Cross gives one). On the help pages and the info list, R1 + Select unlocks every entry; in the bestiary, Select fills it. R2 + Triangle on the Revelation Flow gives the items a cell needs. Triangle + R1 + R2, paused in a scene, skips it (this can break the story's order). |

<p align="center">
  <img src="images/salgin-above.jpg" width="49%" alt="Salgin from above">
  <img src="images/dev-items.jpg" width="49%" alt="Developer tools: the list of every item">
</p>

## Language

F1 > Settings > Language.

| Option | |
|---|---|
| Text | English (the game's), Français, Deutsch, Español or Italiano: the menus, scenes and battles in that language of the European release, from your own European disc (SCES-54552). Needs that disc's texts in `disc/EU`: double-click `Install-European.bat` (after `Install.bat`) and pick your European disc image, or drop it onto the file; by hand, `python setup.py extract-european --iso "<your European disc>"`. A few lines the European disc has no words for stay English. Takes effect at the next start. |
| Undub (Japanese voices) | The voices in scenes and the movies come from your own Japanese Director's Cut (SCPS-17013) instead of the English ones; the text stays as it is. Needs that disc's files in `disc/JA`: double-click `Install-Undub.bat` (after `Install.bat`) and pick your Japanese disc image, or drop it onto the file; by hand, `python setup.py extract-japanese --iso "<your Japanese disc>"`. A few lines (63) have no Japanese voice and play silent. Takes effect at the next start. |

## In the game's own menu

Most of the same options are on the game's Settings screen (System >
Settings in the main menu): L1 and R1 turn the page, and a number's - and
+ buttons lower and raise it. With the text in French, German, Spanish or
Italian, the options are named in that language there too. Widescreen,
Buttons in the game's text, the mouse camera's three options, Check for new
versions, the three Director's Cut options, Native renderer, the text's
language, the undub, the texture packs and Display > Borders are in F1
alone.

<p align="center">
  <img src="images/settings-switches.jpg" width="49%" alt="The game's Settings screen: the switches">
  <img src="images/settings-numbers.jpg" width="49%" alt="The game's Settings screen: the numbers">
</p>

## Good to know

- The window opens as you left it. Settings are saved in
  `%APPDATA%\RogueGalaxy\Recompiled\settings.ini`, screenshots in
  `%APPDATA%\RogueGalaxy\Recompiled\screenshots` (F1 > Game > Open
  screenshots folder).
- On a 144 Hz monitor 60 fps judders a little; 120 Hz or variable refresh
  looks smoother.
- Each option also has an environment variable, for testing:
  [DEVELOPMENT.md](DEVELOPMENT.md).
