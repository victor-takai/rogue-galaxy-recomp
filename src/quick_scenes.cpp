#include "quick_scenes.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "ps2_runtime.h"

namespace rg::quickscenes
{
namespace
{

constexpr uint32_t kGameMode = 0x004B51A0u;
constexpr uint32_t kGamePhase = 0x004B51E0u;
constexpr uint32_t kModeTransporterWarp = 11u;
// Twice: the pace the 60 fps mode played such scenes at before its step
// fixes, which the user liked; three times was too fast.
constexpr double kFast = 2.0;
// Mode 11 runs the area script its warp request named (s32 at 0x4821E0):
// the transporter's on leaving (0x320) and arriving (0x321); the doors' and
// lifts' (0x226) and story scripts too, which keep their pace.
constexpr uint32_t kWarpScript = 0x004821E0u;
constexpr int32_t kScriptTransporterLeave = 0x320;
constexpr int32_t kScriptTransporterArrive = 0x321;

// The main menu's state machine (a pointer): +4 the state running, an entry
// of its table whose first word is the state's id.
constexpr uint32_t kMenuMachine = 0x0048235Cu;

std::atomic<bool> g_fastTransporter{false};
bool g_trace = false;
uint32_t g_lastMode = ~0u;
int32_t g_lastPhase = 0;
uint32_t g_lastMenuState = ~0u;

double seconds()
{
    static const auto start = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

uint32_t word(const uint8_t *rdram, uint32_t address)
{
    address &= 0x1FFFFFFFu;
    if (address == 0u || address + 4u > PS2_RAM_SIZE)
    {
        return 0u;
    }
    uint32_t value = 0u;
    std::memcpy(&value, rdram + address, sizeof(value));
    return value;
}

void traceMenu(const uint8_t *rdram)
{
    const uint32_t entry = word(rdram, word(rdram, kMenuMachine) + 4u);
    const uint32_t state = entry ? word(rdram, entry) : 0u;
    if (state != g_lastMenuState)
    {
        std::fprintf(stderr, "[scene] %.2f s: menu state %#x\n", seconds(), state);
        g_lastMenuState = state;
    }
}

} // namespace

void install()
{
    const char *trace = std::getenv("RG_SCENE_TRACE");
    g_trace = trace && *trace && *trace != '0';
}

void setFastTransporter(bool on)
{
    g_fastTransporter.store(on, std::memory_order_relaxed);
}

double speed(const uint8_t *rdram)
{
    uint32_t mode = 0u;
    int32_t phase = 0;
    std::memcpy(&mode, rdram + kGameMode, sizeof(mode));
    std::memcpy(&phase, rdram + kGamePhase, sizeof(phase));
    if (g_trace && (mode != g_lastMode || phase != g_lastPhase))
    {
        std::fprintf(stderr, "[scene] %.2f s: mode %u phase %d\n", seconds(), mode, phase);
        g_lastMode = mode;
        g_lastPhase = phase;
    }
    if (g_trace)
    {
        traceMenu(rdram);
    }
    bool hurry = false;
    if (mode == kModeTransporterWarp)
    {
        int32_t script = 0;
        std::memcpy(&script, rdram + kWarpScript, sizeof(script));
        hurry = (script == kScriptTransporterLeave || script == kScriptTransporterArrive) &&
                g_fastTransporter.load(std::memory_order_relaxed);
    }
    return hurry ? kFast : 1.0;
}

} // namespace rg::quickscenes
