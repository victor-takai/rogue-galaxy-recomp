# Adding an option to the menu

**In short:** an option is a module in `src/` (most often a hook, see
[HOOKS.md](HOOKS.md)) with a switch or a number the F1 menu sets. A new one
touches four places: the module, the menu's table in `src/graphics.cpp`, the
game's own Settings screen in `src/settings_pages.cpp` (optional), and the
docs. This page walks through them with "Instant text", the smallest option
there is, and ends with a checklist.

## 1. The module

`src/instant_text.h` and `src/instant_text.cpp`. The menu's value lives in an
atomic, since the menu runs on the window's thread and the hook on the
game's:

```cpp
namespace rg::instanttext
{
namespace
{
std::atomic<bool> g_enabled{false};

void mesWinGetInstant(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (!g_enabled.load(std::memory_order_relaxed))
    {
        g_original(rdram, ctx, runtime);
        return;
    }
    // ... the option's work
}
} // namespace

void install(PS2Runtime &runtime)
{
    const char *enabled = std::getenv("RG_INSTANT_TEXT");
    if (enabled && *enabled && *enabled != '0')
    {
        setEnabled(true);
    }
    if (!rg::timing::hookFunction(runtime, kMesWinGetInstant, &mesWinGetInstant, g_original))
    {
        std::fprintf(stderr, "[rg] the text stays letter by letter\n");
    }
}

void setEnabled(bool on)
{
    g_enabled.store(on, std::memory_order_relaxed);
}
} // namespace rg::instanttext
```

- **`setEnabled` only stores the value.** The menu calls it with the saved
  setting before `install` runs, before the game is even loaded, and again
  each time the player flips it.
- **The environment variable is read in `install`.** It comes after the
  menu, so `RG_INSTANT_TEXT=1` wins over `settings.ini` for that run. Most
  are named `RG_` and the key in capitals.
- **Add the `.cpp` to `add_executable(roguegalaxy ...)`** in
  `src/CMakeLists.txt`; the list is written out by hand.
- **Call `install` from `src/main.cpp`**, after `runtime.loadELF()`, next to
  the others, with its `#include`.

**An option read once at start** (the game sets something up at boot that
cannot change later) keeps the menu's value as "preferred" and decides in
`install`: `g_biggerBagPreferred` and `g_biggerBag` in `src/inventory.cpp`.
Give it `restart` in the menu (below); the menu then says in red that it
takes effect at the next start.

## 2. The F1 menu

Two edits in `src/graphics.cpp`.

**The table.** `host.gameOptions` in `createMenu()` holds a
`dq8::ui::GameOption` per option (`thirdparty/dq8-gfx/src/ui/ui_overlay.h`):

```cpp
{"instant_text", "Instant text",
 "Dialogue and messages appear a page at a time instead of letter by letter, as the game itself does "
 "where a scene asks for it; the pauses written into the text stay.",
 false},
```

The fields, in order:

| Field | |
|---|---|
| `key` | The name everywhere: `applyGameOption`, the `[game]` section of `settings.ini` (`instant_text = 1`), the Settings screen's row |
| `label` | What the menu shows |
| `help` | The tooltip: what it does, and what it leaves alone |
| `defaultValue` | A switch's value until the player changes it |
| `minimum`, `maximum`, `defaultNumber`, `format`, `step` | A number instead of a switch, when `maximum` is larger than `minimum`: a slider labelled by `format`, - and + buttons that move it by `step`, and a Default button. `format` is `kTimes` for a number set against the game's own (in hundredths, 100 the game's, shown as 1.0x), otherwise printf's (`"%d degrees"`) |
| `remembered` | `false` for a switch that is off at every start and that something besides the menu flips (the free camera, F6): the menu shows what `host.gameOptionState` says |
| `restart` | `true` when it takes effect at the next start |
| `page` | Empty for Game, `"language"` for Language |

A number looks like this:

```cpp
{"camera_distance", "Camera distance",
 "How far the camera stays from the party, against the game's (1.0x).",
 false, 70, 160, 100, kTimes, 5},
```

**Where it shows.** The Game page lists the options in the table's order.
`host.gameSections` starts a section at a key (`{"skip_opening",
"Convenience"}`), so an option belongs to the section above it in the table.

**What it calls.** One line in `applyGameOption` (switches) or
`applyGameNumber` (numbers), above the table:

```cpp
else if (key == "instant_text")
    rg::instanttext::setEnabled(on);
```

**When it does nothing.** An option that has no effect as things stand
(another option is off, a disc is not installed) stays visible, greyed
out, with one line under it saying what would make it work. A case in
`gameOptionBlocked`, which the menu asks each frame it draws the option
(keep it cheap: the installed files are read through `installed()`, again
only when a page is opened), returns that line, or null when the option
works:

```cpp
if ((key == "mouse_sensitivity" || key == "mouse_invert_vertical") && !rg::mousecamera::enabled())
    return "Turn on Mouse camera for this.";
```

Short and plain: what to turn on, set or run ("Needs your Japanese disc:
run Install-Undub.bat."). The help keeps saying what the option needs. A
value of an option with `choices` that would do nothing (a language not
installed) is greyed out alone, by `gameChoiceBlocked`.

## 3. The game's own Settings screen (optional)

The game's System > Settings has three extra pages of the menu's options
(L1 and R1 turn them; `src/settings_pages.h` says how). A row in one of the
lists in `src/settings_pages.cpp` puts the option there too:

```cpp
{"instant_text", "Instant Text", Kind::Switch},
```

- `kSwitches` takes switches, `kNumbers` numbers, `kScenes` the rest
  (quick scenes, the bag and the HUD sizes).
- An option greyed out at times (`gameOptionBlocked`, above) stays in F1
  alone: this screen has no room for the line that says why.
- A page shows ten rows at most (`kRows`), and the build stops when a list
  holds more. `kNumbers` is full.
- The label is in Title Case, short, and says "(Next Start)" for a
  `restart` option.
- `kLocal` names it in French, German, Spanish and Italian, for the text's
  language (Language > Text): the European release's words where it has
  them, its accent tags (`<'e>`), and no longer than "Bag Page 7 (Next
  Start)" with the value. A row it lacks shows the English label.
- A number's range, step, default and the way it reads (1.0x for `kTimes`)
  come from the menu's table; nothing to repeat. Its choices read - and +.
- Changes made there go back through the F1 menu, which saves and applies
  them as its own.

An option that is not there is listed in [OPTIONS.md](OPTIONS.md), "In the
game's own menu", among the ones in F1 alone.

## 4. The docs

- [OPTIONS.md](OPTIONS.md): a row in its section's table, in the menu's
  order, saying what it does and its default when that is on.
- [DEVELOPMENT.md](DEVELOPMENT.md): the environment variable, in the
  "Player options" table, with the menu's label in brackets ("the menu's
  "Instant text"") and the module's header.
- [CHANGELOG.md](../CHANGELOG.md): a line under Unreleased.

## Checklist

1. `src/<module>.h` and `.cpp`: `install`, `setEnabled` (or a setter for a
   number) on an atomic, the `RG_` variable read in `install`.
2. `src/CMakeLists.txt`: the `.cpp` in `add_executable(roguegalaxy ...)`.
3. `src/main.cpp`: the `#include` and the `install` call after `loadELF`.
4. `src/graphics.cpp`: the `#include`, the `GameOption` in
   `host.gameOptions`, the line in `applyGameOption` or `applyGameNumber`,
   and a case in `gameOptionBlocked` if something else can leave it doing
   nothing.
5. `src/settings_pages.cpp` (optional): the row in its page, and its names
   in `kLocal`.
6. `docs/OPTIONS.md`, `docs/DEVELOPMENT.md`, `CHANGELOG.md`.
7. Try it: flip it in F1 and see the game change; restart and see it kept
   (`%APPDATA%\RogueGalaxy\Recompiled\settings.ini`, `[game]`); run once
   with the `RG_` variable; flip it on the game's Settings screen if it is
   there.
