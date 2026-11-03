# Finding things in the game

**In short:** before changing something in the game, you have to find the
code that does it and where it keeps its data. This page shows the tools the
project has for that and walks through a real case. It is for people who
read code; [HOW-IT-WORKS.md](HOW-IT-WORKS.md) explains the words used here.

## Start from the names

[`config/SCUS_974.90/functions_main.csv`](../config/SCUS_974.90/functions_main.csv)
lists the main program's 17,733 functions with their names and addresses
(`Name,Start,End,Size`). The names come from
[RGDecomp](https://github.com/Dorgengoa/RGDecomp) ([MAPS.md](MAPS.md)), so
searching the file for a word is the quickest way in:

```
MesWinAdvanceTypeout_275f00,0x00275F00,0x00276188,648
MesWinSetTextSpeed_277580,0x00277580,0x002775DC,92
```

Some names end in `Maybe`: a guess, worth checking. The overlays in
`disc/BIN/` have maps of their own (`functions_title.csv` and the rest).

RGDecomp also has the C source of every function, in
`src/main/<area>/<Name>_<address>.c` of its repository. Reading the C is
faster than reading the machine code. Its global variables are mostly
unnamed, `D_` followed by their address (`D_0047E00C`).

## Read the code without running it

`tools/mipsdis.py` reads the game's program directly:

```
python tools/mipsdis.py disc/SCUS_974.90 config/SCUS_974.90/functions_main.csv calls 0x277600
python tools/mipsdis.py disc/SCUS_974.90 config/SCUS_974.90/functions_main.csv dis 0x277600 0x277610
```

| Command | What it gives |
|---|---|
| `calls ADDR` | Every `jal` and `j` to a function, with the caller's name |
| `refs ADDR [ADDR...]` | The code that loads an address (a global variable, a table) |
| `dis START END` | The machine code between two addresses, with names |
| `find HEX` | A byte pattern in the program |
| `sym ADDR` | The function an address belongs to |
| `scan VALUE MASK [START END N]` | The instructions whose bits, masked, equal VALUE, each with the N after it |
| `gp` | The value of the gp register, which the game's globals are read through |

It reads the main program only, not the overlays in `disc/BIN/`.

The C++ our build runs is in `build/generated/main/`, one file per function
(`MesWinGetInstant_277600_0x277600.cpp`), after `python setup.py recompile`.

## Watch it run

These variables go in `debug\RogueGalaxy.bat`, which has examples, and print
to `build/last-run.log`. Their details, defaults and limits are in
[DEVELOPMENT.md](DEVELOPMENT.md#diagnosis), with the rest of the tools.

| Variable | What it shows |
|---|---|
| `RG_TRACE=0xADDR,...` | The first calls of those functions: arguments, return address and result. `RG_TRACE_ARG0=0xOFF` adds the word at a0+OFF, handy when a0 points at a structure |
| `RG_WATCH=0xADDR,...` | Those memory words, every second |
| `RG_HWWATCH=0xADDR` | Who writes a memory word, with the writer's call stack. Add `RG_HWWATCH_RANGE=0-0xFFFFFFFF` to catch every write: by default it reports only values that point into the main stack. Windows only |
| `RG_SCENE_TRACE=1` | Each change of the game's mode and phase, and the main menu's state |

`RG_TRACE`, like a hook, sees only the calls that go through the function
table: a function reached by a tail `j` does not show
([HOOKS.md](HOOKS.md)).

## Look at memory from outside

The game's 32 MB of memory is one block inside `roguegalaxy.exe`, which can
sit at a different place on each run. `tools\hang\dumpmem.ps1` finds it, prints
`rdram at <address>`, and saves it to `rdram.bin`:

```
powershell -File tools\hang\dumpmem.ps1 -ProcId <pid> -OutDir build\run\hang -Gp 0x00485170
```

A game address A is at that host address plus A, which is also what a tool
such as Cheat Engine needs as its base for the run. In `rdram.bin`, A is
simply the offset.

## A real case: instant text

The instant text option shows each page of a message at once. This is how
its hook was found.

1. **Names.** The message windows' functions are called `MesWin...`.
   `MesWinSetTextSpeed` (0x277580) is what a scene calls with a text speed of
   0 to show text at once, so the game already had an instant mode.
2. **The getter.** `MesWinGetInstant` (0x277600) is two instructions:

   ```
   00277600  03e00008  jr ra
   00277604  848200b4  lh v0,0xb4(a0)
   ```

   It returns the 16-bit value at +0xB4 of the window passed in a0: the
   instant flag.
3. **Who reads it.** `calls 0x277600` lists five calls, all in
   `MesWinAdvanceTypeout` (0x275F00), the function that types the text out.
   Nothing else reads the flag, so answering 1 there turns instant text on
   everywhere, and the pauses written into the text still work.
4. **The hook.** `src/instant_text.cpp` replaces `MesWinGetInstant`: with
   the option on it returns 1, otherwise it calls the game's own. That is the
   first shape in [HOOKS.md](HOOKS.md).

`RG_TRACE=0x277600` with `RG_TRACE_ARG0=0xB4` shows it live: the first 16
calls while text types out, the window in a0, and the flag (the low half of
the word printed).

## Data files

The game's files are inside the `DATA*.DAT` archives, with a `.HD6` index
each. [dialogue.md](dialogue.md) describes the index's layout and where the
text, fonts and voices are.
