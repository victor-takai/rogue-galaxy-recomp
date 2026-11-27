#pragma once

#include "ps2_runtime.h"

// Fixed time step, and the 60 fps mode.
//
// The game measures each frame's time in vblanks (GfxEndFrame, clamped to
// between one and two vsync intervals) and, when a frame ran long, scales
// the next frame's logic up to match: up to twice the step
// (GameSceneTimeScaleUpdate, 0x001CEBE0). On the console only an overloaded
// frame triggers that. Here timer 0 follows the host clock, so frames the
// host is too slow to finish count as long ones too: in the first battle
// about half the frames took three vblanks or more. An emulator's guest
// clock slows down with the host instead, so the game never sees slow frames
// and runs in slow motion; this does the same, by leaving the scene's step
// and time scale at 1 after the game's update. It was first blamed for
// characters' turning swinging back and forth, which was the host's rounding
// (patches/ps2recomp/0003). The first play with the bigger steps back on
// lost the leader in battle, cause not yet pinned down, so it stays.
//
// The 60 fps mode (the menu's "60 frames per second", on by default; RG_FPS
// for one run): the flip waits one vblank instead of two (the vsync interval
// the game sets at boot and keeps outside its own 60 Hz menus), and the
// scene's time scale follows the measured frame time: 0.5 at full rate, 1.0
// when a frame took two vblanks, so the logic advances at the same rate over
// however many frames the host manages. RG_TIME_SCALE=fixed makes it 0.5
// always (slow motion below 60), 1.0 where the flip waits two vblanks.
// Movies keep two vblanks, and so does the Factory (game mode 16), where
// everything counts frames. The game's own overlay mode (menus at 60 Hz
// with the scale locked) is left alone.
//
// Only the scene: music still advances by the measured frame time, so it
// keeps its tempo. RG_TIME_SCALE=game at 30 fps leaves the game's own
// compensation on.
//
// Not everything in the game takes the step (docs/sixty-fps-logic.md). The
// play clock and the field minimap take the vsync interval for the vblanks
// a frame spans: for them it reads 2 while the scale is ours. A movie's
// modal loop never runs the update, so its frames get scale 1 on entry.
// NPCs, projectiles and gravity count frames: step_fixes.cpp. RG_STEP_FIXES=0
// leaves all of these as the game has them.
namespace rg::timing
{

// After loadELF(): hooks GameSceneTimeScaleUpdate.
void install(PS2Runtime &runtime);

// From the flip hook, before the flip runs: sets the vsync interval the flip
// waits for this frame (RG_FPS=60).
void beforeFlip(uint8_t *rdram, R5900Context *ctx);

// RG_FPS=60 is on, with its hooks in place.
bool sixtyFps();

// The menu's "60 frames per second" switch (on its Game page), which
// install() reads: set at start-up as the menu applies the saved settings, so
// a change takes effect the next time the game starts. RG_FPS, when set,
// decides instead for the run (60 for the mode, any other value for the
// game's own 30).
void setSixtyFpsPreferred(bool on);

// Has the game thread print, at the scene's next update, the state the
// mode runs in ([timing] state: game mode, intervals, lock, whose step,
// logic frames run and skipped since the last report): for an F7 capture
// of something that plays at the wrong speed.
void requestStateReport();

// The systems made to take the step (RG_STEP_FIXES: unset, 1 or "all" for
// every one, 0 or "none" for none, or a comma list of the names below, e.g.
// RG_STEP_FIXES=clock,gravity).
enum class StepFix : unsigned
{
    Clock = 1u << 0,    // "clock": play clock, minimap, name entry (the interval readers)
    Movie = 1u << 1,    // "movie": scale 1.0 inside movies
    Npc = 1u << 2,      // "npc": NPC walk and turn rates
    Judge = 1u << 3,    // "judge": projectiles and thrown objects
    Gravity = 1u << 4,  // "gravity": character gravity, terminal velocity and the air move's step
    Event = 1u << 5,    // "event": scripts, the event player and message windows at the console's rate
    Menu = 1u << 6,     // "menu": the field's menus and HUD windows (the battle results among them), the chests, the doors' triangles
    Ai = 1u << 7,       // "ai": the characters' AI (its scripts and timers) at the console's rate
    Texture = 1u << 8,  // "texture": texture animations (scrolling layers, flip-books) at the console's rate
    Camera = 1u << 9,   // "camera": the camera's turn by the stick, and the free camera, at the console's rate (camera_view.cpp)
    Factory = 1u << 10, // "factory": the Factory at the console's 30 frames a second
};
bool stepFix(StepFix which);
// Any of them.
bool stepFixes();
// RG_TIME_TRACE is set.
bool traceEnabled();

// GameSceneGetTimeStep of the current scene (its per-frame scale times its
// base scale); 1 before the scene exists.
float sceneStep(const uint8_t *rdram, const R5900Context *ctx);
// The scene's per-frame scale on its own, and writing it through both
// pointers the game reads it by.
float sceneScale(const uint8_t *rdram, const R5900Context *ctx);
void setSceneScale(uint8_t *rdram, const R5900Context *ctx, float scale);
// RG_FPS=60: how a system that counts frames instead of taking the step
// should run this frame. AsIs while the scale is the game's (its own 60 Hz
// overlays, movies, the 30 fps mode). Otherwise the host's steps are
// accumulated and the system runs, with the scale at 1.0 for the call, on
// the frames that complete a whole one, and is skipped in between; a button
// pressed this frame runs it at once, so no press is lost.
enum class LogicFrame
{
    AsIs,
    Skip,
    WholeStep,
};
LogicFrame logicFrame(const uint8_t *rdram, const R5900Context *ctx);

// Puts `replacement` in the function table at `address` and the entry it
// replaced in `original`; false, with a message, when there is no entry.
bool hookFunction(PS2Runtime &runtime, uint32_t address, PS2Runtime::RecompiledFunction replacement,
                  PS2Runtime::RecompiledFunction &original);

} // namespace rg::timing
