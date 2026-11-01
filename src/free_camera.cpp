#include "free_camera.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "ps2_runtime.h"
#include "return_hook.h"
#include "scene_timing.h"

namespace rg::freecamera
{
namespace
{

constexpr uint32_t kCameraModeUpdate = 0x00309770u;
// CameraModeDebugFreeUpdate, which tail-jumps to CameraDebugFreeUpdate
// (0x305C60): a `j` the generated code makes a direct call, past the table.
constexpr uint32_t kCameraModeDebugFreeUpdate = 0x00307AF0u;
constexpr uint32_t kScePadRead = 0x0012B800u;
constexpr uint32_t kPadUpdate = 0x001843A0u;
// FieldHudBtlEscOpen: the battle's "escape?" window, opened when the leader
// is outside the battle's circle (pushed out by a hit while the camera flies).
constexpr uint32_t kBattleEscapeOpen = 0x00203D20u;
constexpr uint32_t kModeOverride = 0x26Cu;
constexpr int8_t kFreeMode = 13;
constexpr int8_t kNoOverride = -1;
// The game's mode: exploring (6) or fighting (7); its menus are others.
constexpr uint32_t kGameMode = 0x004B51A0u;
constexpr uint32_t kModeField = 6u;
constexpr uint32_t kModeBattle = 7u;
// Start in the pad report's first button byte, active low.
constexpr uint8_t kReportStart = 0x08u;
// The pad objects, and in each the state PadUpdate (0x1843A0) keeps: this
// frame's and the last (buttons, then the sticks at +0x8 to +0x14, ...).
constexpr uint32_t kPad1 = 0x004B2570u;
constexpr uint32_t kPad2 = 0x004B27E0u;
constexpr uint32_t kPadState = 0x4u;
constexpr uint32_t kPadStateSize = 0xB0u;
constexpr uint32_t kPadSticks = 0x8u;
// PadGetAnalog*Norm's dead zone: 50 either side of the centre counts 0.
constexpr int32_t kStickCentre = 0x80;
constexpr int32_t kDeadZone = 49;

PS2Runtime::RecompiledFunction g_cameraModeUpdate = nullptr;
PS2Runtime::RecompiledFunction g_cameraModeDebugFreeUpdate = nullptr;
PS2Runtime::RecompiledFunction g_scePadRead = nullptr;
PS2Runtime::RecompiledFunction g_padUpdate = nullptr;
PS2Runtime::RecompiledFunction g_battleEscapeOpen = nullptr;
std::atomic<bool> g_enabled{false};
// The camera whose override is ours (one at a time: the live one when it
// was last updated while exploring or fighting), and whether it held 13 at
// its last update; then whether pad 1 stays untouched
// until the controller lets go of every button, so what ended the free
// camera does not reach the game, and Start's state at the last read (the
// game thread's alone).
uint32_t g_camera = 0u;
bool g_active = false;
bool g_holdPad = false;
bool g_startDown = false;
// Pad 1's state while the camera's update has pad 2's.
uint8_t g_pad1[kPadStateSize];
// Pad 2's state as PadUpdate left it, while the game reads pad 2 idle: the
// runtime answers port 1 with the same controller as port 0, and the event
// scripts' button tests (EvCmdPadTestTriggered) read pad 2 whatever
// IsPad2Enabled says, so a button pressed for the camera reached them.
uint8_t g_pad2[kPadStateSize];
bool g_pad2Idle = false;

// The controller is the camera's, or is let go of after it ended.
bool holding()
{
    return (g_active && g_enabled.load(std::memory_order_relaxed)) || g_holdPad;
}

int8_t modeOverride(const uint8_t *rdram, uint32_t camera)
{
    camera &= 0x1FFFFFFFu;
    if (camera == 0u || camera + kModeOverride >= PS2_RAM_SIZE)
    {
        return kNoOverride;
    }
    return static_cast<int8_t>(rdram[camera + kModeOverride]);
}

// The camera's own mode back, if its override is still the 13 we wrote.
void release(uint8_t *rdram, uint32_t camera)
{
    camera &= 0x1FFFFFFFu;
    if (modeOverride(rdram, camera) == kFreeMode)
    {
        rdram[camera + kModeOverride] = static_cast<uint8_t>(kNoOverride);
    }
}

void cameraModeUpdate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t camera = getRegU32(ctx, 4) & 0x1FFFFFFFu;
    const bool on = g_enabled.load(std::memory_order_relaxed);
    uint32_t gameMode = 0u;
    std::memcpy(&gameMode, rdram + kGameMode, sizeof(gameMode));
    const bool playing = gameMode == kModeField || gameMode == kModeBattle;
    // The game updates the live camera alone, and makes another one live
    // while exploring or fighting: R3's first-person view (camera 3), and
    // the follow camera again when it ends, by R3 or, in battle, by itself
    // (a hit, the battle won). So with the switch off ours gets its own mode
    // back at whichever camera's update comes next, and with it on, the one
    // before gets its own back when another is taken: nothing of the game's
    // would clear the 13, and CameraModeSetTop, which applies a mode only to
    // a camera without an override, could not set one on it again (the
    // first-person view would stay the free camera, on the real pad 1).
    if (!on && g_camera != 0u)
    {
        release(rdram, g_camera);
        g_camera = 0u;
    }
    int8_t mode = modeOverride(rdram, camera);
    if (on && playing && mode != kFreeMode && camera != 0u && camera + kModeOverride < PS2_RAM_SIZE)
    {
        // The override written as CameraSetModeOverride writes it, without
        // the setup it runs then: mode 13's only snaps the camera where it
        // is, and the follow camera, back, catches up with the party on its
        // own.
        if (camera != g_camera)
        {
            release(rdram, g_camera);
        }
        mode = kFreeMode;
        g_camera = camera;
        rdram[camera + kModeOverride] = static_cast<uint8_t>(mode);
    }
    const bool wasActive = g_active;
    g_active = on && playing && camera == g_camera && mode == kFreeMode;
    if (wasActive && !g_active)
    {
        g_holdPad = true;
    }
    g_cameraModeUpdate(rdram, ctx, runtime);
}

// Past the dead zone, `scale` of the stick's push.
int32_t scaleStick(int32_t raw, float scale)
{
    const int32_t push = raw - kStickCentre;
    if (push > kDeadZone)
    {
        return kStickCentre + kDeadZone + static_cast<int32_t>(static_cast<float>(push - kDeadZone) * scale);
    }
    if (push < -kDeadZone - 1)
    {
        return kStickCentre - kDeadZone - 1 + static_cast<int32_t>(static_cast<float>(push + kDeadZone + 1) * scale);
    }
    return raw;
}

void restorePad1(uint8_t *rdram, R5900Context *, PS2Runtime *, uint32_t)
{
    std::memcpy(rdram + kPad1 + kPadState, g_pad1, kPadStateSize);
}

void cameraModeDebugFreeUpdate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (!g_active || (getRegU32(ctx, 4) & 0x1FFFFFFFu) != g_camera)
    {
        g_cameraModeDebugFreeUpdate(rdram, ctx, runtime);
        return;
    }
    std::memcpy(g_pad1, rdram + kPad1 + kPadState, kPadStateSize);
    std::memcpy(rdram + kPad1 + kPadState, g_pad2Idle ? g_pad2 : rdram + kPad2 + kPadState, kPadStateSize);
    // The frame's share of a step (half at 60 frames a second, more when
    // the host is slower), as the camera's turn by the stick takes it (step
    // fix "camera"); not the game's slow motion, which the console's free
    // camera does not follow either.
    const float share = rg::timing::sixtyFps() && rg::timing::stepFix(rg::timing::StepFix::Camera)
                            ? std::min(rg::timing::sceneScale(rdram, ctx), 1.0f)
                            : 1.0f;
    if (share > 0.0f && share < 1.0f)
    {
        for (uint32_t stick = 0u; stick < 4u; ++stick)
        {
            int32_t raw = 0;
            uint8_t *at = rdram + kPad1 + kPadSticks + stick * 4u;
            std::memcpy(&raw, at, sizeof(raw));
            raw = scaleStick(raw, share);
            std::memcpy(at, &raw, sizeof(raw));
        }
    }
    rg::hooks::callThen(g_cameraModeDebugFreeUpdate, rdram, ctx, runtime, &restorePad1, 0u);
}

void scePadRead(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t port = getRegU32(ctx, 4);
    const uint32_t data = getRegU32(ctx, 6) & 0x1FFFFFFFu;
    g_scePadRead(rdram, ctx, runtime);
    if (port != 0u || data == 0u || data + 8u > PS2_RAM_SIZE)
    {
        return;
    }
    // The pad report: buttons, active low, then the four sticks.
    uint8_t *report = rdram + data;
    const bool startDown = (report[2] & kReportStart) == 0u;
    if (g_active && startDown && !g_startDown)
    {
        // Start leaves the free camera.
        setEnabled(false);
        g_holdPad = true;
    }
    g_startDown = startDown;
    // Turned off by the menu, F6 or Start: held until the controller lets go
    // too, without waiting for the camera's next update to see it off (a
    // frame of an idle pad otherwise, and no end while a window of the game's
    // pauses that update, as the battle's "escape?" does).
    if (g_active && !g_enabled.load(std::memory_order_relaxed))
    {
        g_active = false;
        g_holdPad = true;
    }
    if (g_holdPad && report[2] == 0xFFu && report[3] == 0xFFu)
    {
        g_holdPad = false;
    }
    if (holding())
    {
        std::memset(report + 2u, 0xFF, 2u);
        std::memset(report + 4u, kStickCentre, 4u);
    }
}

// Pad 2 read idle, as pad 1 is, while the controller is the camera's: its
// state kept aside for the camera's update, and given back before the next
// update, which takes the last frame's from it.
void idlePad2(uint8_t *rdram, R5900Context *, PS2Runtime *, uint32_t)
{
    if (!holding())
    {
        return;
    }
    std::memcpy(g_pad2, rdram + kPad2 + kPadState, kPadStateSize);
    std::memcpy(rdram + kPad2 + kPadState, rdram + kPad1 + kPadState, kPadStateSize);
    g_pad2Idle = true;
}

void padUpdate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if ((getRegU32(ctx, 4) & 0x1FFFFFFFu) != kPad2)
    {
        g_padUpdate(rdram, ctx, runtime);
        return;
    }
    if (g_pad2Idle)
    {
        std::memcpy(rdram + kPad2 + kPadState, g_pad2, kPadStateSize);
        g_pad2Idle = false;
    }
    rg::hooks::callThen(g_padUpdate, rdram, ctx, runtime, &idlePad2, 0u);
}

// The window reads pad 1 alone, and the game pauses the camera's update
// while it is open: the free camera ends, as Start ends it, so the window
// gets the controller once it lets go of every button.
void battleEscapeOpen(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (holding())
    {
        setEnabled(false);
        g_holdPad = true;
    }
    g_battleEscapeOpen(rdram, ctx, runtime);
}

} // namespace

void install(PS2Runtime &runtime)
{
    // RG_FREE_CAMERA=1 starts with it on.
    const char *on = std::getenv("RG_FREE_CAMERA");
    if (on && *on && *on != '0')
    {
        setEnabled(true);
    }
    const bool complete =
        rg::timing::hookFunction(runtime, kCameraModeUpdate, &cameraModeUpdate, g_cameraModeUpdate) &&
        rg::timing::hookFunction(runtime, kCameraModeDebugFreeUpdate, &cameraModeDebugFreeUpdate,
                                 g_cameraModeDebugFreeUpdate) &&
        rg::timing::hookFunction(runtime, kScePadRead, &scePadRead, g_scePadRead) &&
        rg::timing::hookFunction(runtime, kPadUpdate, &padUpdate, g_padUpdate) &&
        rg::timing::hookFunction(runtime, kBattleEscapeOpen, &battleEscapeOpen, g_battleEscapeOpen);
    if (!complete)
    {
        std::fprintf(stderr, "[rg] free camera: some hooks are missing, see above\n");
    }
}

void setEnabled(bool on)
{
    g_enabled.store(on, std::memory_order_relaxed);
}

bool enabled()
{
    return g_enabled.load(std::memory_order_relaxed);
}

} // namespace rg::freecamera
