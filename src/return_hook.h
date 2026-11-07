#pragma once

#include <cstdint>
#include <cstdio>

#include "ps2_runtime.h"

// Code to run once a hooked guest function has returned to its caller.
//
// A hook that replaces a function's table entry and calls the original sees
// it return only when nothing unwound it. At a safe point (any call, or a
// loop's back edge, once the scheduler is due) the generated code returns to
// the run loop instead, which later resumes the function through the table
// entry of the address it stopped at, never the hooked one, and the rest of
// the function runs without the hook. callThen() runs `after` right away when
// the original returned, and otherwise when the run loop resumes the caller at
// its return address.
namespace rg::hooks
{

using After = void (*)(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t data);

// From a hook, on the game's thread, in place of original(rdram, ctx, runtime).
void callThen(PS2Runtime::RecompiledFunction original, uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime,
              After after, uint32_t data);

// The calls unwound and not yet resumed, one line each (the stall watch;
// read from its thread without a lock, for the log only).
void describePending(std::FILE *out);

// Runs the guest function at `entry` (`function`, its code: a hook's saved
// original) to its end from host code, on `local`, a copy of the game
// thread's context with the arguments set. A scheduler checkpoint due inside
// it unwinds it as it does any guest code, here into this loop instead of
// the run loop, which goes on where it stopped, through the table, as the run
// loop would (a host function that returns with pc at its entry has
// returned, as in the dispatcher); the checkpoint stays pending for the
// game's own code. For functions that never wait: a wait, which unwinds by
// an exception, would leave it half done. False if it could not finish (a
// stop request, an address without code), with local.pc where it stopped.
bool runToEnd(PS2Runtime::RecompiledFunction function, uint32_t entry, uint8_t *rdram, R5900Context &local,
              PS2Runtime *runtime);

} // namespace rg::hooks
