# Writing hooks

**In short:** the game's code was translated into C++, one C++ function for
each function of the game. A hook is a function of ours put in place of one
of them, so the game runs ours when it calls that one. Most of the options
that change the game, the 60 fps mode and widescreen work that way; the
display options live in the renderer instead. This page is for people who
write C++ for the project.

Changes to how the game behaves go into `src/` as hooks: host C++ functions
that take the place of a game function. A change there only needs
`cmake --build build/game` ([BUILDING.md](BUILDING.md#rebuilding-after-a-change)).
This page covers how the generated code calls functions, the three shapes a
hook takes, and the traps the project already fell into.

## How the generated code calls functions

Every function of the main executable is a C++ function in
`build/generated/main/`, named after its map entry and address
(`FileMakeDevicePath_181980_0x181980`). They all take
`(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)`: `ctx` holds the
registers (`getRegU32(ctx, 4)` is a0, 5 is a1, 2 is v0, 29 is sp, 31 is ra),
and guest memory is `rdram + (address & 0x1FFFFFFF)`.

About 200 SDK and libc functions are bound to the runtime instead (the
`stubs` list in `config/SCUS_974.90/recomp/main.toml`). Their file calls
the runtime's version, not the game's code: `Rand_133710_0x133710.cpp` is
`ctx->pc = ra` and a call to `ps2_stubs::rand`. The runtime's version can
behave unlike the game's: rand did (`src/libc_rand.h`).

**Calls go through a table.** A `jal` or `jalr` becomes a call to
`runtime->dispatchGuestBranch`, which finds the target in the function table.
A hook replaces the target's table entry, so it sees every call made that way.

**Tail jumps don't.** A `j` to another function is generated as a direct C++
call, past the table. `CameraModeDebugFreeUpdate_307af0` ends with
`CameraDebugFreeUpdate_305c60_0x305c60(rdram, ctx, runtime); return;`, so a
hook on 0x305C60 never runs on that path; hook the caller instead.

**Safe points.** Every call through `dispatchGuestBranch` and every loop's
back edge is a safe point. When the scheduler is due (another thread to run,
a wait), the generated code returns without finishing
(`if (!runtime->dispatchGuestBranch(...)) return;`), all the way up to the
run loop, with `ctx->pc` where it stopped. The run loop later resumes the
thread at that pc, through that address's table entry: a resume label inside
the function, not its entry, so a hook on the function is not called again.
The host functions that were above it are gone; the guest's return addresses
lead back to their guest code.

## Installing a hook

A module has an `install` function, run once at startup:

```cpp
namespace
{
constexpr uint32_t kMesWinGetInstant = 0x00277600u;
PS2Runtime::RecompiledFunction g_original = nullptr;

void mesWinGetInstant(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    // ...
}
} // namespace

void install(PS2Runtime &runtime)
{
    if (!rg::timing::hookFunction(runtime, kMesWinGetInstant, &mesWinGetInstant, g_original))
    {
        std::fprintf(stderr, "[rg] the text stays letter by letter\n");
    }
}
```

- `rg::timing::hookFunction` (`src/scene_timing.h`) puts your function in the
  table at the address and hands back the entry it replaced in `original`. It
  returns false, with a message, when the address has no entry.
- Call your `install` from `src/main.cpp` after `runtime.loadELF()`, which
  fills the table, next to the others. Add the `.cpp` to `add_executable` in
  `src/CMakeLists.txt`.
- Function names and addresses are in
  `config/SCUS_974.90/functions_main.csv`. RGDecomp's C source tells what a
  function does ([MAPS.md](MAPS.md)).
- The overlay arena (0x004E9E80 to 0x00528200) holds whichever `BIN/*.BIN`
  overlay is resident, and its table entries are that overlay's. This page is
  about the main executable's functions.

## The three shapes of a hook

**Replace the function.** Set the return value and end with the guest's
`jr ra`:

```cpp
setReturnS32(ctx, 1);
ctx->pc = getRegU32(ctx, 31);
```

Without the last line the game freezes sooner or later: a thread unwound at
the call is resumed at its pc, and with pc left at the hook's entry it calls
the hook forever. See `src/instant_text.cpp` and `src/libc_rand.cpp`.

**Change something, then run the original.** Write the registers or guest
memory, then call `g_original(rdram, ctx, runtime)` as the last thing the
hook does.

**Do something after the original.** Use `rg::hooks::callThen`
(`src/return_hook.h`), never code placed after `g_original(...)`. If a safe
point unwinds the original, `g_original` returns early: the code after it
runs on half-done work, and the rest of the original runs later without the
hook. `callThen` notes the return address and calls the original. If the
original really returned, it runs `after` at once. Otherwise it puts its own
entry in the table at the return address until the caller resumes there,
which happens only once the original has finished.

```cpp
void afterMakePath(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t dst)
{
    // The path is complete here.
}

void fileMakeDevicePath(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    rg::hooks::callThen(g_original, rdram, ctx, runtime, &afterMakePath, getRegU32(ctx, 5));
}
```

By the time `after` runs, the argument registers are gone, so pass what it
needs as the last argument (`data`). v0 still holds the original's return
value (`toSavedSlots` in `src/inventory.cpp` reads it). Other examples:
`gaijiLoadImage` in `src/button_glyphs.cpp`, and `src/free_camera.cpp`, which
changes the pad's sticks before the original and puts them back in `after`.

## Traps

**Calling game functions from a hook.** A guest function called from host
code stops at the first safe point that unwinds, and nothing finishes it in
the hook's place: a file slot or semaphore it took stays taken. Do the work on
the host (check a file with `std::filesystem`, not the game's
`FileSlotOpen`), or change guest memory directly (the free camera writes the
camera's override byte instead of calling `CameraSetModeOverride`).

**Skipping a call a loop waits on.** Before a hook skips or delays a
function, read its callers:

```
python tools/mipsdis.py disc/SCUS_974.90 config/SCUS_974.90/functions_main.csv calls 0x181980
```

lists every `jal` and `j` to an address (a `j` means hooking that caller
instead), and `build/generated/main/` has their code. Calls through a
register (`jalr`: function pointers, task tables) are not listed, though
they go through the table; `RG_TRACE` shows them at run time by their `ra`
([DEVELOPMENT.md](DEVELOPMENT.md#diagnosis)). A caller that loops on
the result within one frame hangs when the hook skips it: the 60 fps mode
froze on powers that way, at `do {} while (SystemVmUpdate() != 1)` in
AbilityModeRun ([STATUS.md](STATUS.md), "Two freezes after round four", and
`kSystemVmLoopReturns` in `src/step_fixes.cpp`).

**Threads.** Guest code, and so every hook, runs on the game's thread. An
option the menu changes reaches a hook through an atomic (`g_enabled` in
`src/instant_text.cpp`); [ADDING-OPTIONS.md](ADDING-OPTIONS.md) has the
whole path of an option.

**Check that it fires.** A temporary `std::fprintf(stderr, ...)` or counter
in the hook, read in `build/last-run.log`, before relying on it.
