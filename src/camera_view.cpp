#include "camera_view.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <utility>

#include "mouse_camera.h"
#include "ps2_runtime.h"
#include "scene_timing.h"

namespace rg::cameraview
{
namespace
{

constexpr uint32_t kViewSetProjection = 0x00143000u;
constexpr uint32_t kCameraSetPresetParams = 0x00309100u;
constexpr uint32_t kCameraSetDefaultParams = 0x00309260u;
constexpr uint32_t kCameraOrbitControl = 0x00161260u;
// The sticks, left and right and up and down (PadGetAnalogLXNorm, LYNorm,
// RXNorm and RYNorm), and the calls to them of the cameras that turn by a
// stick a frame's worth each frame, by where they return to. The battle
// camera locked on a target (CameraModeLockTargetUpdate) turns by the right
// stick's left and right; its up and down zooms past a threshold and stays
// as it is. The one following it (CameraTargetFollowUpdate) turns by the
// same; its tilt (+0x40 of its parameters) eases toward the right stick's up
// and down times 15 degrees (+0x1C), a tilt the stick holds whatever the
// frame rate, so it stays as it is too. The first-person view
// (CameraFirstPersonUpdate, camera mode 1, R3 exploring or in battle)
// turns and tilts by both sticks added.
constexpr uint32_t kPadReads[] = {0x00183AF0u, 0x00183A50u, 0x00183C30u, 0x00183B90u};
constexpr uint32_t kLockTurnReturn = 0x003065E0u;
constexpr uint32_t kFollowTurnReturn = 0x003073ACu;
constexpr uint32_t kTurnReturns[] = {
    kLockTurnReturn,   // CameraModeLockTargetUpdate: the right stick's left and right
    kFollowTurnReturn, // CameraTargetFollowUpdate: the same
    0x00307F98u, // CameraFirstPersonUpdate: the left stick's left and right,
    0x00307FA8u, // the right stick's,
    0x00307FB8u, // the left stick's up and down
    0x00307FC8u, // and the right stick's
};
// The mouse (src/mouse_camera.h), added where each camera takes the stick,
// to the right a look to the right and up a look up, whatever the game's
// camera options: the camera's signs of those (CameraSetAxisSigns, 0x160470,
// from the options' flags, 2 up and down, 4 left and right) are +0x254 up
// and down and +0x258 left and right, 1 on Normal or -1 reversed, which the
// stick is multiplied by. On Normal the stick moves the field and battle
// cameras around the party (to the right, the view turns left; up, the
// camera rises and looks down) and turns the first-person view itself, so
// the mouse goes into the first as the stick the other way. Seen on frames
// against the stick, 2026-11-05.
constexpr uint32_t kSignVertical = 0x254u;
constexpr uint32_t kSignHorizontal = 0x258u;
// The field's: CameraObjOrbitControl's one call, from CameraObjApplyInput
// (0x1619E0, s3 its input, 0 without one), returns here. The field camera's
// +0x214 has 2 while it may not turn, +0x250 the frames it may not yet (made
// one fewer before the call), +0x244 its turn a frame at full stick, +0x248
// its tilt (a height the camera rises by), +0xE0 the point it looks at and
// +0xF0 its eye; its nearest distance is the first float of its parameters
// (+0x15C, kDistanceBlock), and nearer than that (a wall) the turn is
// multiplied by how much nearer. With no tilt in a frame, the height eases
// back toward the middle of its range. Its own read of the stick's up and
// down, which at the lowest tilt (0x481120 set) brings the camera in to look
// up, returns to kOrbitTiltReturn (s4 the camera).
constexpr uint32_t kCameraApplyInput = 0x001619E0u;
constexpr uint32_t kOrbitFromInput = 0x00161B30u;
constexpr uint32_t kOrbitTiltReturn = 0x0016154Cu;
constexpr uint32_t kOrbitLowest = 0x00481120u;
constexpr uint32_t kFieldTarget = 0xE0u;
constexpr uint32_t kFieldEye = 0xF0u;
// A tilt too small to move the camera, which keeps it from easing back.
constexpr float kHoldTilt = 1e-7f;
constexpr uint32_t kFieldFlags = 0x214u;
constexpr uint32_t kFieldTurnSpeed = 0x244u;
constexpr uint32_t kFieldTiltSpeed = 0x248u;
constexpr uint32_t kFieldNoTurnFrames = 0x250u;
// The locked-on battle camera (s1 at its read of the stick) turns by the
// stick times 0x47D8E8's float a frame, half that while 0x4E72A8's byte has
// it far.
constexpr uint32_t kLockTurnSpeed = 0x0047D8E8u;
constexpr uint32_t kLockFar = 0x004E72A8u;
// The following one (s0) by the stick times +0x18 of its parameters
// (GetCameraFollowParams, 0x4B6800), and tilts toward the stick's up and
// down times +0x1C: the mouse moves a tilt it holds.
constexpr uint32_t kFollowParams = 0x004B6800u;
constexpr uint32_t kTiltReturn = 0x003073C4u; // its read of the right stick's up and down
// The first-person view's turn and tilt (CameraFirstPersonUpdate), which it
// clamps after adding the sticks.
constexpr uint32_t kFirstPersonUpdate = 0x00307F00u;
constexpr uint32_t kFirstPersonYaw = 0x2A4u;
constexpr uint32_t kFirstPersonPitch = 0x2A8u;
constexpr uint32_t kFirstPersonPitchMax = 0x0047D8A8u; // floats, +-1.555 radians
constexpr uint32_t kFirstPersonPitchMin = 0x0047D8ACu;
// The block CameraSetDefaultParams writes the distances into, in the camera.
constexpr uint32_t kDistanceBlock = 0x15Cu;
constexpr uint32_t kGameMode = 0x004B51A0u;
constexpr uint32_t kModeField = 6u;
constexpr uint32_t kModeBattle = 7u;
constexpr int kGameFieldOfView = 52;
constexpr float kPi = 3.14159265358979f;

PS2Runtime::RecompiledFunction g_viewSetProjection = nullptr;
PS2Runtime::RecompiledFunction g_cameraSetPresetParams = nullptr;
PS2Runtime::RecompiledFunction g_cameraSetDefaultParams = nullptr;
PS2Runtime::RecompiledFunction g_cameraOrbitControl = nullptr;
PS2Runtime::RecompiledFunction g_padReads[std::size(kPadReads)] = {};
PS2Runtime::RecompiledFunction g_firstPersonUpdate = nullptr;
PS2Runtime::RecompiledFunction g_cameraApplyInput = nullptr;
// The game thread's: whether the field camera may turn this frame, as
// CameraObjApplyInput found it before it ran; whether the mouse holds the
// field camera's height, and the look up from its lowest the mouse holds (a
// push of the stick, 0 to 1), with what goes to the camera's own read this
// frame; the tilt the mouse holds for the following battle camera (out of
// battle, 0).
bool g_fieldTurnBlocked = false;
bool g_fieldHold = false;
float g_fieldLookUp = 0.0f;
float g_fieldTiltPush = 0.0f;
float g_followTilt = 0.0f;
std::atomic<int> g_fieldOfView{kGameFieldOfView};
std::atomic<int> g_distance{100};
std::atomic<bool> g_fasterTurn{false};

// The game's memory, by its addresses (any segment), left alone outside it.
bool inRam(uint32_t address, uint32_t size)
{
    return address != 0u && address <= PS2_RAM_SIZE && size <= PS2_RAM_SIZE - address;
}

template <typename T>
T read(const uint8_t *rdram, uint32_t address)
{
    T value{};
    address &= 0x1FFFFFFFu;
    if (inRam(address, sizeof(T)))
    {
        std::memcpy(&value, rdram + address, sizeof(T));
    }
    return value;
}

template <typename T>
void write(uint8_t *rdram, uint32_t address, T value)
{
    address &= 0x1FFFFFFFu;
    if (inRam(address, sizeof(T)))
    {
        std::memcpy(rdram + address, &value, sizeof(T));
    }
}

// A camera's distances as the game last wrote them, and the percentage of
// them its block holds now. The game keeps two cameras (0x4B5950 and
// 0x4B6220); the game thread's alone.
struct Distances
{
    uint32_t block = 0u;
    float base[2] = {};
    int applied = 100;
};
std::array<Distances, 4> g_cameras{};
size_t g_nextCamera = 0u;

void writeDistances(uint8_t *rdram, Distances &camera, int percent)
{
    for (uint32_t index = 0; index < 2u; ++index)
    {
        write(rdram, camera.block + index * 4u, camera.base[index] * static_cast<float>(percent) / 100.0f);
    }
    camera.applied = percent;
}

// The game has just written a camera's nearest (+0x0) and farthest (+0x4)
// distances at `block`: they are kept as its base and scaled.
void distancesWritten(uint8_t *rdram, uint32_t block)
{
    block &= 0x1FFFFFFFu;
    if (!inRam(block, 8u))
    {
        return;
    }
    Distances *camera = nullptr;
    for (Distances &known : g_cameras)
    {
        if (known.block == block)
        {
            camera = &known;
        }
    }
    if (!camera)
    {
        camera = &g_cameras[g_nextCamera];
        g_nextCamera = (g_nextCamera + 1u) % g_cameras.size();
        camera->block = block;
    }
    camera->base[0] = read<float>(rdram, block);
    camera->base[1] = read<float>(rdram, block + 4u);
    writeDistances(rdram, *camera, g_distance.load(std::memory_order_relaxed));
}

// The camera distance moved in the menu: the cameras take it now. A block
// still holding what was last written says it is the same camera; one the
// game has let go or set again is forgotten.
void followDistance(uint8_t *rdram, uint32_t mode)
{
    const int percent = g_distance.load(std::memory_order_relaxed);
    if (mode != kModeField && mode != kModeBattle)
    {
        return;
    }
    for (Distances &camera : g_cameras)
    {
        if (camera.block == 0u || camera.applied == percent)
        {
            continue;
        }
        bool same = true;
        for (uint32_t index = 0; index < 2u; ++index)
        {
            const float expected = camera.base[index] * static_cast<float>(camera.applied) / 100.0f;
            same = same && std::fabs(read<float>(rdram, camera.block + index * 4u) - expected) <= 0.01f;
        }
        if (same)
        {
            writeDistances(rdram, camera, percent);
        }
        else
        {
            camera.block = 0u;
        }
    }
}

// FovHorizToVert's: the angle up a 4:3 picture whose width spans `degrees`.
float vertical(int degrees)
{
    return 2.0f * std::atan(std::tan(static_cast<float>(degrees) * kPi / 360.0f) * 0.75f);
}

void viewSetProjection(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t mode = read<uint32_t>(rdram, kGameMode);
    const int degrees = g_fieldOfView.load(std::memory_order_relaxed);
    if (degrees != kGameFieldOfView)
    {
        static const float game = vertical(kGameFieldOfView);
        if ((mode == kModeField || mode == kModeBattle) && std::fabs(ctx->f[12] - game) < 1e-3f)
        {
            ctx->f[12] = vertical(degrees);
        }
    }
    followDistance(rdram, mode);
    g_viewSetProjection(rdram, ctx, runtime);
}

// The battle camera's: 150 to 190 up, 120 to 160 down.
void cameraSetPresetParams(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t block = getRegU32(ctx, 4);
    g_cameraSetPresetParams(rdram, ctx, runtime);
    distancesWritten(rdram, block);
}

// Every camera's first, the field's for good: 65 to 120.
void cameraSetDefaultParams(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t camera = getRegU32(ctx, 4);
    g_cameraSetDefaultParams(rdram, ctx, runtime);
    distancesWritten(rdram, camera + kDistanceBlock);
}

// What a frame's turn by the stick is multiplied by: the frame's share of a
// step in the 60 fps mode (a half at full rate), so the camera turns at the
// console's speed, and twice that with "Faster camera rotation".
float turnFactor(const uint8_t *rdram, const R5900Context *ctx)
{
    float factor = g_fasterTurn.load(std::memory_order_relaxed) ? 2.0f : 1.0f;
    if (rg::timing::sixtyFps() && rg::timing::stepFix(rg::timing::StepFix::Camera))
    {
        // The frame's share alone, as the free camera takes it: the game's
        // slow motion (the step's base) never slowed the camera's turn.
        factor *= std::min(rg::timing::sceneScale(rdram, ctx), 1.0f);
    }
    return factor;
}

// What the game's option multiplies the stick by (+1, or -1 reversed), so
// that the mouse is the stick on Normal whatever it is.
float optionSign(const uint8_t *rdram, uint32_t camera, uint32_t offset)
{
    return read<int32_t>(rdram, camera + offset) < 0 ? -1.0f : 1.0f;
}

// The stick's push that turns a camera by `radians`, at `perFrame` radians a
// frame for a full push.
float stickFor(float radians, float perFrame)
{
    return perFrame > 1e-6f ? radians / perFrame : 0.0f;
}

// The field camera's input: whether it may turn, before CameraObjApplyInput
// counts down the frames it may not.
void cameraApplyInput(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t self = getRegU32(ctx, 4);
    g_fieldTurnBlocked = (read<uint32_t>(rdram, self + kFieldFlags) & 2u) != 0u ||
                         read<int32_t>(rdram, self + kFieldNoTurnFrames) > 0;
    g_cameraApplyInput(rdram, ctx, runtime);
}

// How much nearer than its nearest distance the field camera is (a wall
// pushing it in), which the game multiplies its turn by the inverse of: 1 at
// that distance or farther.
float fieldNearness(const uint8_t *rdram, uint32_t self)
{
    const float nearest = read<float>(rdram, self + kDistanceBlock);
    const float x = read<float>(rdram, self + kFieldTarget) - read<float>(rdram, self + kFieldEye);
    const float z = read<float>(rdram, self + kFieldTarget + 8u) - read<float>(rdram, self + kFieldEye + 8u);
    const float distance = std::sqrt(x * x + z * z);
    return nearest > 0.0f && distance > 0.0f && distance < nearest ? distance / nearest : 1.0f;
}

// The field camera's turn and tilt this frame, with the mouse's: only for the
// player's input, and the turn only while the camera may turn, at the same
// angle near a wall. The tilt is a height, so the mouse's is in the camera's
// tilt for its turn, and it stays where the mouse leaves it until the stick
// tilts the camera. At the lowest tilt, moving on up looks up, as holding
// the stick does, and stays so until the mouse comes down.
void addFieldMouse(const uint8_t *rdram, const R5900Context *ctx, float (&turn)[2])
{
    g_fieldTiltPush = 0.0f;
    if (!rg::mousecamera::enabled())
    {
        g_fieldHold = false;
        g_fieldLookUp = 0.0f;
        return;
    }
    if (g_cameraApplyInput == nullptr || getRegU32(ctx, 31) != kOrbitFromInput || getRegU32(ctx, 19) == 0u)
    {
        return;
    }
    if (read<uint32_t>(rdram, kGameMode) != kModeBattle)
    {
        g_followTilt = 0.0f;
    }
    const uint32_t self = getRegU32(ctx, 4);
    const float mouseTurn = rg::mousecamera::takeTurn();
    const float mouseTilt = rg::mousecamera::takeTilt();
    if (!g_fieldTurnBlocked)
    {
        turn[0] -= mouseTurn * fieldNearness(rdram, self);
    }
    const float turnSpeed = read<float>(rdram, self + kFieldTurnSpeed);
    if (turn[1] != 0.0f)
    {
        // The stick's: the game's own again.
        g_fieldHold = false;
        g_fieldLookUp = 0.0f;
        return;
    }
    if (mouseTilt != 0.0f)
    {
        g_fieldHold = true;
        g_fieldLookUp = read<uint32_t>(rdram, kOrbitLowest) != 0u || mouseTilt < 0.0f
                            ? std::clamp(g_fieldLookUp + stickFor(mouseTilt, turnSpeed), 0.0f, 1.0f)
                            : 0.0f;
    }
    turn[1] = -mouseTilt * stickFor(read<float>(rdram, self + kFieldTiltSpeed), turnSpeed);
    if (turn[1] == 0.0f && g_fieldHold)
    {
        turn[1] = kHoldTilt;
    }
    g_fieldTiltPush = g_fieldLookUp;
}

// The field camera's turn this frame: a1 points at the stick's turn and
// tilt (two floats), which it adds as they are.
void cameraOrbitControl(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const float factor = turnFactor(rdram, ctx);
    const uint32_t input = getRegU32(ctx, 5) & 0x1FFFFFFFu;
    if ((factor != 1.0f || rg::mousecamera::enabled() || g_fieldHold) && inRam(input, 8u))
    {
        float turn[2] = {read<float>(rdram, input) * factor, read<float>(rdram, input + 4u) * factor};
        addFieldMouse(rdram, ctx, turn);
        write(rdram, input, turn[0]);
        write(rdram, input + 4u, turn[1]);
    }
    g_cameraOrbitControl(rdram, ctx, runtime);
}

// The following battle camera's tilt the mouse holds, as the stick's up and
// down on Normal (down, +1, looks up), until the field camera takes the
// mouse out of battle (game mode 7) or the option is turned off. Its turn
// goes through the camera's easing, as the stick's does.
float followTilt(float mouseTilt, float perPush)
{
    g_followTilt = std::clamp(g_followTilt + stickFor(mouseTilt, perPush), -1.0f, 1.0f);
    return g_followTilt;
}

// The mouse's push, added to the stick's (f0) where a camera reads it.
void addStickMouse(const uint8_t *rdram, R5900Context *ctx, uint32_t site)
{
    if (site == kOrbitTiltReturn)
    {
        // The field camera's look up from its lowest tilt, as the stick's down on Normal.
        if (g_fieldTiltPush != 0.0f)
        {
            ctx->f[0] = std::clamp(
                ctx->f[0] + g_fieldTiltPush * optionSign(rdram, getRegU32(ctx, 20), kSignVertical), -1.0f, 1.0f);
            g_fieldTiltPush = 0.0f;
        }
    }
    else if (site == kLockTurnReturn)
    {
        const float perFrame = read<float>(rdram, kLockTurnSpeed) * (read<uint8_t>(rdram, kLockFar) ? 0.5f : 1.0f);
        ctx->f[0] -= stickFor(rg::mousecamera::takeTurn(), perFrame) *
                     optionSign(rdram, getRegU32(ctx, 17), kSignHorizontal);
    }
    else if (site == kFollowTurnReturn)
    {
        ctx->f[0] -= stickFor(rg::mousecamera::takeTurn(), read<float>(rdram, kFollowParams + 0x18u)) *
                     optionSign(rdram, getRegU32(ctx, 16), kSignHorizontal);
    }
    else if (site == kTiltReturn)
    {
        const float held = followTilt(rg::mousecamera::takeTilt(), read<float>(rdram, kFollowParams + 0x1Cu));
        ctx->f[0] = std::clamp(ctx->f[0] + held * optionSign(rdram, getRegU32(ctx, 16), kSignVertical), -1.0f, 1.0f);
    }
}

// A stick's push (f0), when a camera reads it to turn: the reads are leaves
// and return to the hook.
template <size_t Index>
void padRead(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t returnPc = getRegU32(ctx, 31);
    g_padReads[Index](rdram, ctx, runtime);
    for (uint32_t site : kTurnReturns)
    {
        if (returnPc == site)
        {
            ctx->f[0] *= turnFactor(rdram, ctx);
            break;
        }
    }
    if (rg::mousecamera::enabled())
    {
        addStickMouse(rdram, ctx, returnPc);
    }
    else
    {
        g_followTilt = 0.0f;
    }
}

// The first-person view: the mouse's turn and tilt go into its angles before
// it adds the sticks' (a stick to the right lowers the yaw, down lowers the
// pitch). The pitch is kept in the view's limits here: the game wraps the
// angle before it clamps it, and past half a turn a quick flick up would
// wrap to the lowest.
void cameraFirstPersonUpdate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (rg::mousecamera::enabled())
    {
        const uint32_t self = getRegU32(ctx, 4);
        write(rdram, self + kFirstPersonYaw, read<float>(rdram, self + kFirstPersonYaw) - rg::mousecamera::takeTurn());
        float pitch = read<float>(rdram, self + kFirstPersonPitch) + rg::mousecamera::takeTilt();
        const float lowest = read<float>(rdram, kFirstPersonPitchMin);
        const float highest = read<float>(rdram, kFirstPersonPitchMax);
        if (lowest < highest)
        {
            pitch = std::clamp(pitch, lowest, highest);
        }
        write(rdram, self + kFirstPersonPitch, pitch);
    }
    g_firstPersonUpdate(rdram, ctx, runtime);
}

template <size_t... Index>
bool installPadReads(PS2Runtime &runtime, std::index_sequence<Index...>)
{
    bool complete = true;
    ((complete &= rg::timing::hookFunction(runtime, kPadReads[Index], &padRead<Index>, g_padReads[Index])), ...);
    return complete;
}

} // namespace

void install(PS2Runtime &runtime)
{
    if (const char *value = std::getenv("RG_FIELD_OF_VIEW"))
    {
        setFieldOfView(std::atoi(value));
    }
    if (const char *value = std::getenv("RG_CAMERA_DISTANCE"))
    {
        setDistance(std::atoi(value));
    }
    if (const char *value = std::getenv("RG_FASTER_CAMERA"))
    {
        setFasterTurn(*value && *value != '0');
    }
    if (!rg::timing::hookFunction(runtime, kViewSetProjection, &viewSetProjection, g_viewSetProjection))
    {
        std::fprintf(stderr, "[rg] the field of view stays the game's\n");
    }
    if (!rg::timing::hookFunction(runtime, kCameraSetPresetParams, &cameraSetPresetParams, g_cameraSetPresetParams) ||
        !rg::timing::hookFunction(runtime, kCameraSetDefaultParams, &cameraSetDefaultParams, g_cameraSetDefaultParams))
    {
        std::fprintf(stderr, "[rg] the camera distance stays the game's\n");
    }
    // The field's turn and the mouse's there; the battle and first-person
    // views' stick reads and the mouse's in battle.
    if (!rg::timing::hookFunction(runtime, kCameraOrbitControl, &cameraOrbitControl, g_cameraOrbitControl))
    {
        std::fprintf(stderr, "[rg] exploring, the camera turns at the game's speed, and not by the mouse\n");
    }
    else if (!rg::timing::hookFunction(runtime, kCameraApplyInput, &cameraApplyInput, g_cameraApplyInput))
    {
        std::fprintf(stderr, "[rg] exploring, the mouse does not turn the camera\n");
    }
    if (!installPadReads(runtime, std::make_index_sequence<std::size(kPadReads)>{}))
    {
        std::fprintf(stderr, "[rg] in battle and in the first-person view, the camera turns at the game's speed, "
                             "and not by the mouse in battle\n");
    }
    if (!rg::timing::hookFunction(runtime, kFirstPersonUpdate, &cameraFirstPersonUpdate, g_firstPersonUpdate))
    {
        std::fprintf(stderr, "[rg] the mouse does not turn the first-person view\n");
    }
}

void setFieldOfView(int degrees)
{
    if (degrees > 0)
    {
        g_fieldOfView.store(degrees, std::memory_order_relaxed);
    }
}

void setDistance(int percent)
{
    if (percent > 0)
    {
        g_distance.store(percent, std::memory_order_relaxed);
    }
}

void setFasterTurn(bool on)
{
    g_fasterTurn.store(on, std::memory_order_relaxed);
}

} // namespace rg::cameraview
