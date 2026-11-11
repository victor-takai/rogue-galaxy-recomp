#pragma once

#include <cstdint>

class PS2Runtime;
struct R5900Context;

// Dispatch for the MWo3 code overlays (config/SCUS_974.90/overlays.toml).
//
// All five code overlays are linked for the same arena, so an arena address
// means different code depending on which one is resident. The game's loader
// calls a `jr ra` stub with a0 = the overlay header after every read and
// before .bss and the constructors; the manager replaces that stub, checks
// the header against the one each tree was generated from, and points the
// runtime's FunctionRegion resolver at that overlay's table.
namespace rg::overlay
{

constexpr uint32_t kArenaStart = 0x004E9E80u;
constexpr uint32_t kArenaEnd = 0x00528200u;
constexpr uint32_t kHeaderSize = 0x40u;
constexpr uint32_t kImageLoadedHook = 0x00100400u; // OverlayLoadNotifyStubMaybe_100400

// Call after PS2Runtime::loadELF(), which fills the table the hook lives in.
bool install(PS2Runtime &runtime);

// Name of the resident overlay ("none" before the first load).
const char *residentName();

// Puts `replacement` in the function table of the overlay called `name`
// ("insmenu", "insect"...: the names in overlays.toml) at `address`, one of
// that overlay's functions, and hands back the entry it replaced in
// `original`, as rg::timing::hookFunction does for the main executable; false,
// with a message and `original` null, when that overlay has no function
// starting there (a resume point inside one has an entry too, and is refused).
// The table answers only while that overlay is resident, so the hook runs only
// then. Function is PS2Runtime::RecompiledFunction (checked where it is
// defined). Call it at boot, before the game runs.
using Function = void (*)(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
bool hookFunction(const char *name, uint32_t address, Function replacement, Function &original);

// Overlay loads seen so far.
uint32_t loadCount();

} // namespace rg::overlay
