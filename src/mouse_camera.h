#pragma once

#include <cstdint>

struct SDL_Window;

// The camera turned by the mouse, with the menu's game switch `mouse_camera`
// ("Mouse camera", on by default) on, or RG_MOUSE_CAMERA=1.
//
// While the switch is on, the game's window has the keyboard focus, the F1
// menu is closed and the player plays with the keyboard and mouse (from a
// press of a key bound to the pad or a click in the window, until the
// controller alone has been used for kControllerLetGo), the mouse is captured
// (SDL's relative mode: the cursor hidden and kept in the window); F1, the
// pause (F2) and Alt+Tab let it go. A player on a controller who never
// presses a bound key or clicks never has it captured. What it moves
// goes to the camera the stick turns, as the right stick would with the
// game's camera options on Normal, whatever they are: left and right turns
// it, up and down tilts it, the other way with `mouse_invert_vertical`. The
// game's cameras take it in src/camera_view.cpp. `mouse_sensitivity` (25 to
// 400, 1.0x at 100) scales both; at 1.0x a count turns 0.063 degrees, an
// inch at 1,600 counts per inch 100 degrees. Motion made while the cameras
// stop taking it (a menu of the game's, a scene, a battle's pause: twice
// their usual gap between takes, half a second at most) is dropped; while the
// game is paused (F2) the mouse is let go too. RG_MOUSE_CAMERA,
// RG_MOUSE_SENSITIVITY (25 to 400, or 0.25 to 4) and RG_MOUSE_INVERT over the
// menu. Without the SDL window (the software GS) it does nothing.
namespace rg::mousecamera
{

// At start: the environment's settings.
void install();

// What the player last used, SDL_GetTicks() times as dq8::gfx::SdlPadInput
// gives them: a press of a key or mouse button bound to the pad (or a click,
// or the wheel), mouse motion, the controller.
struct Use
{
    uint64_t keyboardMouse = 0u;
    uint64_t mouseMotion = 0u;
    uint64_t controller = 0u;
};

// The window thread, once a frame after the events. `letGo`: the F1 menu is
// open or the game paused.
void pump(SDL_Window *window, bool letGo, const Use &use);

// Whether the player is not in control of the party but in one of the game's
// menus, a scene or a talk (where the right click goes back and the wheel
// moves up and down; dq8::gfx::SdlPadInput::setInMenus). Read from the
// game's memory, any thread.
bool inGameMenu(const uint8_t *rdram);

// The game thread, at a camera's update: the turn to the right and the tilt
// up in radians the mouse made since the last take of that axis.
bool enabled();
float takeTurn();
float takeTilt();

// The menu's switches and number, at start and when changed.
void setEnabled(bool on);
void setSensitivity(int percent);
void setInvertVertical(bool on);

} // namespace rg::mousecamera
