#pragma once

#include <memory>

class GSRasterBackend;

namespace rg::graphics
{

// Step 7's structural check (docs/NATIVE-PIPELINE.md): under the native
// pipeline the GS backend is not there for the game. Every call into it is
// a crossing the pipeline missed; the first of each kind is printed with a
// backtrace, and the counts at exit.
//
// RG_NATIVE_GS selects what a crossing does:
//   drop  (the default) draws, transfers, flushes and syncs go nowhere;
//         reads and presentation still reach the backend, logged
//   abort the first crossing of any kind stops the run with its trace
//   pass  everything reaches the backend as before, logged
//
// With the pipeline off it is a plain pass-through.
std::unique_ptr<GSRasterBackend> tripOnGs(std::unique_ptr<GSRasterBackend> backend);

} // namespace rg::graphics
