// Vector drawings for the menu: icons, the PS2 pad's button marks, what a
// binding asks the player to press, and a DualShock 2 lit by live input.
// Drawn rather than loaded, so they stay sharp at any scale and ship nothing.
#pragma once

#include "gfx/backends/sdlgpu/sdlgpu_input.h"

#include <imgui.h>
#include <string>

namespace dq8::ui {

enum class Icon : uint8_t {
    Display, Sound, Controls, Interface, Camera, Pause, Play, Keyboard, Info, Chart, Close, Folder, Link,
    Speed, Check, Warning, Reset, Disc, Gear, Copy,
    Count // how many there are; not an icon
};

// How many icons there are, and each one's name (RG_UI_SCRIPT's notify
// icon=, its icon sheet).
constexpr int kIconCount = static_cast<int>(Icon::Count);
const char *iconName(Icon icon);

void drawIcon(ImDrawList *list, Icon icon, ImVec2 center, float size, ImU32 color);
// A point moved to the centre of the screen pixel it is in, where drawIcon
// puts an icon's centre.
ImVec2 pixelCentre(ImVec2 point);
// An icon before text on the same line, centred on that text: iconRoom lays
// out the icon's room and returns it; once the text (or a group of lines) is
// submitted beside it, drawIconBeside draws the icon there, centred on that
// last item's height. The text decides the height: framed text
// (AlignTextToFramePadding) sits FramePadding.y below the line's top, and an
// icon drawn at the top looked too high beside it.
struct IconRoom {
    ImVec2 pos;
    float size = 0.0f;
};
IconRoom iconRoom(float size);
void drawIconBeside(const IconRoom &room, Icon icon, ImU32 color);
// The vertical middle of the item just submitted, in screen space.
float lastItemMiddle();

// A PS2 input as the pad itself marks it.
void drawPadGlyph(ImDrawList *list, gfx::PadInput input, ImVec2 center, float size);

// What a binding asks the player to press, as their keyboard or controller
// type labels it. Chips are `height` tall; the width depends on the label.
float bindingChipWidth(const gfx::PadBinding &binding, SDL_GamepadType type, float height);
void drawBindingChip(ImDrawList *list, const gfx::PadBinding &binding, SDL_GamepadType type,
                     ImVec2 min, float height, float alpha = 1.0f);
std::string bindingLabel(const gfx::PadBinding &binding, SDL_GamepadType type);
const char *controllerFamilyName(SDL_GamepadType type);

// A DualShock 2 fitted into the box, lit by `pad`. `highlight` rings one
// input, or none when it is PadInput::Count.
void drawPadDiagram(ImDrawList *list, ImVec2 min, ImVec2 size, const gfx::PadSnapshot &pad,
                    gfx::PadInput highlight);

} // namespace dq8::ui
