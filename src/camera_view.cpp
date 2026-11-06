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
// (0x1619E0, which CameraFollowLeaderStep alone calls, every frame, with the
// player's stick), returns here. Every camera that follows the party takes
// it: exploring, and in battle the free camera. The field camera's
// +0x214 has 2 while it may not turn, +0x250 the frames it may not yet (made
// one fewer before the call), +0x244 its turn a frame at full stick, +0x248
// its tilt (a height the camera rises by), +0xE0 the point it looks at and
// +0xF0 its eye; its nearest distance is the first float of its parameters
// (+0x15C, kDistanceBlock), and nearer than that (a wall) the turn is
// multiplied by how much nearer. The height is the parameters' +0x10, kept
// from +0x18 (the lowest; reaching it sets 0x481120) to +0x14; with no tilt
// in a frame, it eases back into +0x20 to +0x1C by an nth of the way
// (0x481118's float, 20). The ground and ceilings under and over the eye
// move it too, and set 0x481120 while the ground holds the camera up. Its
// own read of the stick's up and down returns to kOrbitTiltReturn (s4 the
// camera): there, with 0x481120 set, holding it down brings the camera in to
// look up, and anything else clears 0x481120 again.
constexpr uint32_t kCameraApplyInput = 0x001619E0u;
constexpr uint32_t kOrbitFromInput = 0x00161B30u;
constexpr uint32_t kOrbitTiltReturn = 0x0016154Cu;
constexpr uint32_t kOrbitLowest = 0x00481120u;
constexpr uint32_t kOrbitEase = 0x00481118u;
constexpr uint32_t kFieldTarget = 0xE0u;
constexpr uint32_t kFieldEye = 0xF0u;
constexpr uint32_t kHeight = 0x10u;
constexpr uint32_t kHighest = 0x14u;
constexpr uint32_t kLowest = 0x18u;
// A tilt too small to move the camera, which keeps it from easing back.
constexpr float kHoldTilt = 1e-7f;
// Every change of a camera's mode (CameraApplyMode: a battle, a scene, a
// map, the first-person view and back).
constexpr uint32_t kCameraApplyMode = 0x00309490u;
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
// down times +0x1C: the mouse moves a tilt it holds. Each frame it moves
// +0x34 of the way (a quarter) to where that turn puts it and starts again
// from where it is, so the stick turns it that share of +0x18 a frame.
constexpr uint32_t kFollowParams = 0x004B6800u;
constexpr uint32_t kFollowTurn = 0x18u;
constexpr uint32_t kFollowTilt = 0x1Cu;
constexpr uint32_t kFollowCatchUp = 0x34u;
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
PS2Runtime::RecompiledFunction g_cameraApplyMode = nullptr;
// The game thread's: whether the field camera may turn this frame, as
// CameraObjApplyInput found it before it ran; whether the mouse holds the
// field camera's height, and which; the look up from its lowest the mouse
// holds (a push of the stick, 0 to 1), with what goes to the camera's own
// read this frame; the tilt the mouse holds for the following battle camera.
bool g_fieldTurnBlocked = false;
bool g_fieldHold = false;
float g_heldHeight = 0.0f;
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

// What the mouse holds, let go: the camera's heights and tilts are the
// game's again.
void letGo()
{
    g_fieldHold = false;
    g_fieldLookUp = 0.0f;
    g_fieldTiltPush = 0.0f;
    g_followTilt = 0.0f;
}

// The battle camera's: 150 to 190 up, 120 to 160 down. Also when a camera
// that follows the party finds a ceiling over it or loses it, with heights
// of their own.
void cameraSetPresetParams(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t block = getRegU32(ctx, 4);
    letGo();
    g_cameraSetPresetParams(rdram, ctx, runtime);
    distancesWritten(rdram, block);
}

// Every camera's first, the field's for good: 65 to 120.
void cameraSetDefaultParams(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t camera = getRegU32(ctx, 4);
    letGo();
    g_cameraSetDefaultParams(rdram, ctx, runtime);
    distancesWritten(rdram, camera + kDistanceBlock);
}

void cameraApplyMode(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    letGo();
    g_cameraApplyMode(rdram, ctx, runtime);
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

// The field camera's turn and tilt this frame, with the mouse's: the turn
// only while the camera may turn, at the same angle near a wall. The tilt is
// a height, so the mouse's is in the camera's tilt for its turn, and the
// height stays where the mouse leaves it (the ground or a ceiling moving it,
// it eases back there as the game eases it into its range) until the stick
// tilts the camera or the camera changes (letGo). At the lowest tilt, moving
// on up looks up, as holding the stick does, and stays so until the mouse
// comes down.
void addFieldMouse(const uint8_t *rdram, const R5900Context *ctx, float (&turn)[2])
{
    g_fieldTiltPush = 0.0f;
    if (!rg::mousecamera::enabled())
    {
        g_fieldHold = false;
        g_fieldLookUp = 0.0f;
        return;
    }
    if (g_cameraApplyInput == nullptr || getRegU32(ctx, 31) != kOrbitFromInput)
    {
        return;
    }
    const uint32_t self = getRegU32(ctx, 4);
    const float mouseTurn = rg::mousecamera::takeTurn();
    const float mouseTilt = rg::mousecamera::takeTilt();
    if (!g_fieldTurnBlocked)
    {
        turn[0] -= mouseTurn * fieldNearness(rdram, self);
    }
    if (turn[1] != 0.0f)
    {
        // The stick's: the game's own again.
        g_fieldHold = false;
        g_fieldLookUp = 0.0f;
        return;
    }
    const uint32_t heights = self + kDistanceBlock;
    const float height = read<float>(rdram, heights + kHeight);
    const float lowest = read<float>(rdram, heights + kLowest);
    const float highest = read<float>(rdram, heights + kHighest);
    if (mouseTilt != 0.0f)
    {
        const float turnSpeed = read<float>(rdram, self + kFieldTurnSpeed);
        const float rise = -mouseTilt * stickFor(read<float>(rdram, self + kFieldTiltSpeed), turnSpeed);
        // At the lowest as the game will find it in this call: its flag from
        // the last one lasts only while the ground holds the camera up.
        const bool atLowest = read<uint32_t>(rdram, kOrbitLowest) != 0u || height + rise < lowest;
        g_fieldLookUp = atLowest || mouseTilt < 0.0f
                            ? std::clamp(g_fieldLookUp + stickFor(mouseTilt, turnSpeed), 0.0f, 1.0f)
                            : 0.0f;
        g_fieldHold = true;
        g_heldHeight = lowest <= highest ? std::clamp(height + rise, lowest, highest) : height + rise;
        turn[1] = rise;
    }
    else if (g_fieldHold)
    {
        const float ease = read<float>(rdram, kOrbitEase);
        const float back = (g_heldHeight - height) / (ease > 1.0f ? ease : 1.0f);
        turn[1] = std::fabs(back) > kHoldTilt ? back : kHoldTilt;
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
// down on Normal (down, +1, looks up), until the camera changes (letGo) or
// the option is turned off. Its turn goes through the camera's easing, as
// the stick's does.
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
        // The whole angle, as exploring: of a frame's turn the camera moves
        // only a share, and drops the rest.
        const float catchUp = read<float>(rdram, kFollowParams + kFollowCatchUp);
        const float perFrame =
            read<float>(rdram, kFollowParams + kFollowTurn) * (catchUp > 0.0f && catchUp < 1.0f ? catchUp : 1.0f);
        ctx->f[0] -= stickFor(rg::mousecamera::takeTurn(), perFrame) *
                     optionSign(rdram, getRegU32(ctx, 16), kSignHorizontal);
    }
    else if (site == kTiltReturn)
    {
        const float held = followTilt(rg::mousecamera::takeTilt(), read<float>(rdram, kFollowParams + kFollowTilt));
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
    if (!rg::timing::hookFunction(runtime, kCameraApplyMode, &cameraApplyMode, g_cameraApplyMode))
    {
        std::fprintf(stderr, "[rg] a tilt held by the mouse lasts into the next scene or battle\n");
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
