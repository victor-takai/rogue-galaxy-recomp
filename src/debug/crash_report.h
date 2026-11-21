#pragma once

namespace rg::debug
{

// On an unhandled exception (an access violation, say), prints its code, the
// faulting address, the thread and its call stack to stderr before the
// process dies, as module+offset for each frame; addresses in the executable
// symbolize with `llvm-symbolizer --obj=build/game/src/roguegalaxy.exe`.
// On Linux the fatal signals (SIGSEGV and the like) print the same with
// glibc's backtrace. A no-op elsewhere.
void installCrashReport();

} // namespace rg::debug
