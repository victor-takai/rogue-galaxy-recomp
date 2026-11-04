#include "ui/ui_glyphs.h"

#include "ui/ui_style.h"

#include <imgui_internal.h> // ImTextCharFromUtf8

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <utility>
#include <vector>

namespace dq8::ui {
namespace {
using gfx::PadBinding;
using gfx::PadInput;

constexpr ImU32 kTriangleColor = IM_COL32(80, 214, 178, 255);
constexpr ImU32 kCircleColor = IM_COL32(255, 112, 112, 255);
constexpr ImU32 kCrossColor = IM_COL32(140, 172, 255, 255);
constexpr ImU32 kSquareColor = IM_COL32(242, 150, 214, 255);
constexpr ImU32 kDim = IM_COL32(255, 255, 255, 64);

enum Direction { kUp, kDown, kLeft, kRight };

float stroke(float size) { return std::max(1.5f, size * 0.09f); }

// Where the ink of a coverage profile starts and ends, in texels from its
// first: the half-coverage crossings, between texel centres, so a flat top
// a row only partly covered lands where it really is.
std::pair<float, float> inkSpan(const std::vector<float> &profile) {
    const float peak = *std::max_element(profile.begin(), profile.end());
    const float half = peak * 0.5f;
    size_t lo = 0, hi = profile.size() - 1;
    while (profile[lo] < half)
        ++lo;
    while (profile[hi] < half)
        --hi;
    const float before = lo > 0 ? profile[lo - 1] : 0.0f;
    const float after = hi + 1 < profile.size() ? profile[hi + 1] : 0.0f;
    const float start = static_cast<float>(lo) - 0.5f + (half - before) / std::max(1e-6f, profile[lo] - before);
    const float end = static_cast<float>(hi) + 0.5f + (profile[hi] - half) / std::max(1e-6f, profile[hi] - after);
    return {start, end};
}

// A glyph's ink in its quad's coordinates, read from the atlas; its quad
// is the rasterised box rounded out to whole pixels, up to half a pixel
// larger on one side. False when the atlas keeps no pixels.
bool glyphInk(const ImFontGlyph &glyph, ImVec4 &ink) {
    const ImTextureData *texture = ImGui::GetFont()->OwnerAtlas->TexData;
    if (!texture || !texture->Pixels || texture->Width <= 0 || texture->Height <= 0)
        return false;
    const int x0 = static_cast<int>(std::lround(glyph.U0 * texture->Width));
    const int x1 = static_cast<int>(std::lround(glyph.U1 * texture->Width));
    const int y0 = static_cast<int>(std::lround(glyph.V0 * texture->Height));
    const int y1 = static_cast<int>(std::lround(glyph.V1 * texture->Height));
    if (x1 <= x0 || y1 <= y0 || x1 > texture->Width || y1 > texture->Height)
        return false;
    const int alpha = texture->Format == ImTextureFormat_Alpha8 ? 0 : 3;
    std::vector<float> columns(static_cast<size_t>(x1 - x0)), rows(static_cast<size_t>(y1 - y0));
    float total = 0.0f;
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) {
            // The strongest coverage of each row and column, not their sum: an
            // A's point or a Y's stem is narrow but as solid as the rest.
            const float a = texture->Pixels[(static_cast<size_t>(y) * texture->Width + x) * texture->BytesPerPixel + alpha];
            float &column = columns[static_cast<size_t>(x - x0)];
            float &row = rows[static_cast<size_t>(y - y0)];
            column = std::max(column, a);
            row = std::max(row, a);
            total += a;
        }
    if (total <= 0.0f)
        return false;
    const auto [left, right] = inkSpan(columns);
    const auto [top, bottom] = inkSpan(rows);
    const float sx = (glyph.X1 - glyph.X0) / static_cast<float>(x1 - x0);
    const float sy = (glyph.Y1 - glyph.Y0) / static_cast<float>(y1 - y0);
    ink = ImVec4(glyph.X0 + left * sx, glyph.Y0 + top * sy, glyph.X0 + right * sx, glyph.Y0 + bottom * sy);
    return true;
}

// Centred by the letters' own ink, not by the line box: that one carries
// room for descenders and side bearings, which left a Y high and off to a
// side in its circle. Descenders are left out, so "Options" sits like "Menu".
struct Placed {
    ImVec2 at;     // where the text is drawn, on whole pixels as ImGui draws it
    ImVec2 shift;  // how far that moved it from the exact centre
    bool visible = false;
};

Placed place(ImVec2 center, float fontSize, const char *text) {
    ImFont *font = ImGui::GetFont();
    ImFontBaked *baked = font->GetFontBaked(fontSize);
    const float scale = fontSize / baked->Size;
    const float baseline = IM_ROUND(baked->Ascent) * scale;
    float pen = 0.0f, left = FLT_MAX, right = -FLT_MAX, top = FLT_MAX, bottom = -FLT_MAX;
    for (const char *at = text, *end = text + std::strlen(text); at < end;) {
        unsigned int code = 0;
        at += ImTextCharFromUtf8(&code, at, end);
        const ImFontGlyph *glyph = baked->FindGlyph(static_cast<ImWchar>(code));
        if (!glyph)
            continue;
        if (glyph->Visible) {
            ImVec4 ink(glyph->X0, glyph->Y0, glyph->X1, glyph->Y1);
            glyphInk(*glyph, ink);
            left = std::min(left, pen + ink.x * scale);
            right = std::max(right, pen + ink.z * scale);
            top = std::min(top, ink.y * scale);
            // A descender counts to the baseline; the overshoot of a round
            // or flat letter's foot counts as it is.
            const float foot = ink.w * scale;
            bottom = std::max(bottom, foot > baseline + fontSize * 0.1f ? baseline : foot);
        }
        pen += glyph->AdvanceX * scale;
    }
    Placed placed;
    if (left > right)
        return placed; // nothing to see, a space
    const ImVec2 exact(center.x - (left + right) * 0.5f, center.y - (top + bottom) * 0.5f);
    placed.at = ImVec2(std::round(exact.x), std::round(exact.y));
    placed.shift = ImVec2(placed.at.x - exact.x, placed.at.y - exact.y);
    placed.visible = true;
    return placed;
}

void label(ImDrawList *list, ImVec2 center, float fontSize, ImU32 color, const char *text) {
    const Placed placed = place(center, fontSize, text);
    if (placed.visible)
        list->AddText(ImGui::GetFont(), fontSize, placed.at, color, text);
}

float labelWidth(float fontSize, const char *text) {
    return ImGui::GetFont()->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, text).x;
}

void arrow(ImDrawList *list, ImVec2 c, float size, int direction, ImU32 color) {
    const float h = size * 0.5f, w = size * 0.42f;
    switch (direction) {
    case kUp:
        list->AddTriangleFilled(ImVec2(c.x, c.y - h), ImVec2(c.x + w, c.y + h * 0.6f), ImVec2(c.x - w, c.y + h * 0.6f), color);
        break;
    case kDown:
        list->AddTriangleFilled(ImVec2(c.x - w, c.y - h * 0.6f), ImVec2(c.x + w, c.y - h * 0.6f), ImVec2(c.x, c.y + h), color);
        break;
    case kLeft:
        list->AddTriangleFilled(ImVec2(c.x - h, c.y), ImVec2(c.x + h * 0.6f, c.y - w), ImVec2(c.x + h * 0.6f, c.y + w), color);
        break;
    default:
        list->AddTriangleFilled(ImVec2(c.x - h * 0.6f, c.y - w), ImVec2(c.x + h, c.y), ImVec2(c.x - h * 0.6f, c.y + w), color);
        break;
    }
}

ImU32 faceColor(PadInput input) {
    switch (input) {
    case PadInput::Triangle: return kTriangleColor;
    case PadInput::Circle: return kCircleColor;
    case PadInput::Cross: return kCrossColor;
    default: return kSquareColor;
    }
}

void faceSymbol(ImDrawList *list, PadInput input, ImVec2 c, float size, ImU32 color) {
    const float t = stroke(size);
    const float r = size * 0.5f;
    switch (input) {
    case PadInput::Triangle:
        list->AddTriangle(ImVec2(c.x, c.y - r * 0.48f), ImVec2(c.x - r * 0.46f, c.y + r * 0.32f),
                          ImVec2(c.x + r * 0.46f, c.y + r * 0.32f), color, t);
        break;
    case PadInput::Circle:
        list->AddCircle(c, r * 0.42f, color, 0, t);
        break;
    case PadInput::Cross: {
        const float d = r * 0.36f;
        list->AddLine(ImVec2(c.x - d, c.y - d), ImVec2(c.x + d, c.y + d), color, t);
        list->AddLine(ImVec2(c.x - d, c.y + d), ImVec2(c.x + d, c.y - d), color, t);
        break;
    }
    case PadInput::Square: {
        const float d = r * 0.34f;
        list->AddRect(ImVec2(c.x - d, c.y - d), ImVec2(c.x + d, c.y + d), color, 0.0f, t);
        break;
    }
    default:
        break;
    }
}

// A plus-shaped d-pad; `lit` is the arm drawn bright, or -1.
void dpad(ImDrawList *list, ImVec2 c, float size, int lit, ImU32 on, ImU32 off) {
    const float inner = size * 0.15f, reach = size * 0.48f;
    const ImVec2 arms[4][2] = {
        {{c.x - inner, c.y - reach}, {c.x + inner, c.y - inner}},
        {{c.x - inner, c.y + inner}, {c.x + inner, c.y + reach}},
        {{c.x - reach, c.y - inner}, {c.x - inner, c.y + inner}},
        {{c.x + inner, c.y - inner}, {c.x + reach, c.y + inner}},
    };
    list->AddRectFilled(ImVec2(c.x - inner, c.y - inner), ImVec2(c.x + inner, c.y + inner), off);
    for (int arm = 0; arm < 4; ++arm)
        list->AddRectFilled(arms[arm][0], arms[arm][1], arm == lit ? on : off, size * 0.05f);
}

void pill(ImDrawList *list, ImVec2 min, ImVec2 max, ImU32 fill, ImU32 border) {
    const float rounding = (max.y - min.y) * 0.5f;
    list->AddRectFilled(min, max, fill, rounding);
    list->AddRect(min, max, border, rounding, 1.0f);
}

// Stick directions: a stick seen from above with an arrow on the side pushed.
void stick(ImDrawList *list, ImVec2 c, float size, char side, int direction, ImU32 color) {
    const float r = size * 0.36f;
    list->AddCircle(c, r, color, 0, stroke(size) * 0.8f);
    const char text[2] = {side, '\0'};
    label(list, c, size * 0.42f, color, text);
    const float reach = r + size * 0.11f;
    const ImVec2 offsets[4] = {{0.0f, -reach}, {0.0f, reach}, {-reach, 0.0f}, {reach, 0.0f}};
    arrow(list, ImVec2(c.x + offsets[direction].x, c.y + offsets[direction].y), size * 0.2f, direction, color);
}

enum class Family { Xbox, Xbox360, PlayStation, PlayStation5, Nintendo, GameCube };

Family familyOf(SDL_GamepadType type) {
    switch (type) {
    case SDL_GAMEPAD_TYPE_XBOX360: return Family::Xbox360;
    case SDL_GAMEPAD_TYPE_PS3:
    case SDL_GAMEPAD_TYPE_PS4: return Family::PlayStation;
    case SDL_GAMEPAD_TYPE_PS5: return Family::PlayStation5;
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR: return Family::Nintendo;
#if SDL_VERSION_ATLEAST(3, 4, 0) // SDL 3.2 has no GameCube type.
    case SDL_GAMEPAD_TYPE_GAMECUBE: return Family::GameCube;
#endif
    default: return Family::Xbox;
    }
}

bool playStation(Family family) { return family == Family::PlayStation || family == Family::PlayStation5; }
bool nintendo(Family family) { return family == Family::Nintendo || family == Family::GameCube; }

const char *buttonText(SDL_GamepadButton button, Family family) {
    const bool ps = playStation(family), nin = nintendo(family);
    switch (button) {
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return ps ? "L1" : nin ? "L" : "LB";
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return ps ? "R1" : nin ? "R" : "RB";
    case SDL_GAMEPAD_BUTTON_LEFT_STICK: return ps ? "L3" : "LS";
    case SDL_GAMEPAD_BUTTON_RIGHT_STICK: return ps ? "R3" : "RS";
    case SDL_GAMEPAD_BUTTON_BACK:
        return family == Family::PlayStation5 ? "Create" : ps ? "Share" : nin ? "-" : family == Family::Xbox360 ? "Back" : "View";
    case SDL_GAMEPAD_BUTTON_START:
        return ps ? "Options" : nin ? "+" : family == Family::Xbox360 ? "Start" : "Menu";
    case SDL_GAMEPAD_BUTTON_GUIDE: return ps ? "PS" : nin ? "Home" : "Guide";
    case SDL_GAMEPAD_BUTTON_MISC1: return family == Family::PlayStation5 ? "Mic" : nin ? "Capture" : "Share";
    case SDL_GAMEPAD_BUTTON_TOUCHPAD: return "Touchpad";
    case SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1: return "P1";
    case SDL_GAMEPAD_BUTTON_LEFT_PADDLE1: return "P2";
    case SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2: return "P3";
    case SDL_GAMEPAD_BUTTON_LEFT_PADDLE2: return "P4";
    default: break;
    }
    const char *name = SDL_GetGamepadStringForButton(button);
    return name ? name : "?";
}

const char *triggerText(SDL_GamepadAxis axis, Family family) {
    const bool left = axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER;
    if (playStation(family))
        return left ? "L2" : "R2";
    if (nintendo(family))
        return left ? "ZL" : "ZR";
    return left ? "LT" : "RT";
}

std::string keyText(SDL_Scancode scancode) {
    struct Short { SDL_Scancode code; const char *text; };
    static constexpr Short kShort[] = {
        {SDL_SCANCODE_LSHIFT, "L Shift"}, {SDL_SCANCODE_RSHIFT, "R Shift"},
        {SDL_SCANCODE_LCTRL, "L Ctrl"}, {SDL_SCANCODE_RCTRL, "R Ctrl"},
        {SDL_SCANCODE_LALT, "L Alt"}, {SDL_SCANCODE_RALT, "R Alt"},
#if defined(__APPLE__)
        {SDL_SCANCODE_LGUI, "L Cmd"}, {SDL_SCANCODE_RGUI, "R Cmd"},
#else
        {SDL_SCANCODE_LGUI, "L Win"}, {SDL_SCANCODE_RGUI, "R Win"},
#endif
        {SDL_SCANCODE_RETURN, "Enter"}, {SDL_SCANCODE_ESCAPE, "Esc"},
        {SDL_SCANCODE_BACKSPACE, "Bksp"}, {SDL_SCANCODE_CAPSLOCK, "Caps"},
    };
    for (const auto &entry : kShort)
        if (entry.code == scancode)
            return entry.text;
    std::string name = SDL_GetScancodeName(scancode);
    if (name.rfind("Keypad ", 0) == 0)
        name = "Num " + name.substr(7);
    return name.empty() ? std::string("?") : name;
}

int arrowKey(SDL_Scancode scancode) {
    switch (scancode) {
    case SDL_SCANCODE_UP: return kUp;
    case SDL_SCANCODE_DOWN: return kDown;
    case SDL_SCANCODE_LEFT: return kLeft;
    case SDL_SCANCODE_RIGHT: return kRight;
    default: return -1;
    }
}

int dpadDirection(SDL_GamepadButton button) {
    switch (button) {
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return kUp;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return kDown;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return kLeft;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return kRight;
    default: return -1;
    }
}

bool isFace(SDL_GamepadButton button) {
    return button == SDL_GAMEPAD_BUTTON_SOUTH || button == SDL_GAMEPAD_BUTTON_EAST ||
           button == SDL_GAMEPAD_BUTTON_WEST || button == SDL_GAMEPAD_BUTTON_NORTH;
}

// The face button's label, positional (Xbox) when SDL does not know the type.
SDL_GamepadButtonLabel faceLabel(SDL_GamepadButton button, SDL_GamepadType type) {
    const SDL_GamepadButtonLabel known = SDL_GetGamepadButtonLabelForType(type, button);
    if (known != SDL_GAMEPAD_BUTTON_LABEL_UNKNOWN)
        return known;
    switch (button) {
    case SDL_GAMEPAD_BUTTON_SOUTH: return SDL_GAMEPAD_BUTTON_LABEL_A;
    case SDL_GAMEPAD_BUTTON_EAST: return SDL_GAMEPAD_BUTTON_LABEL_B;
    case SDL_GAMEPAD_BUTTON_WEST: return SDL_GAMEPAD_BUTTON_LABEL_X;
    default: return SDL_GAMEPAD_BUTTON_LABEL_Y;
    }
}

PadInput psFace(SDL_GamepadButtonLabel label) {
    switch (label) {
    case SDL_GAMEPAD_BUTTON_LABEL_CROSS: return PadInput::Cross;
    case SDL_GAMEPAD_BUTTON_LABEL_CIRCLE: return PadInput::Circle;
    case SDL_GAMEPAD_BUTTON_LABEL_SQUARE: return PadInput::Square;
    case SDL_GAMEPAD_BUTTON_LABEL_TRIANGLE: return PadInput::Triangle;
    default: return PadInput::Count;
    }
}

const char *letter(SDL_GamepadButtonLabel label) {
    switch (label) {
    case SDL_GAMEPAD_BUTTON_LABEL_A: return "A";
    case SDL_GAMEPAD_BUTTON_LABEL_B: return "B";
    case SDL_GAMEPAD_BUTTON_LABEL_X: return "X";
    default: return "Y";
    }
}

ImU32 letterColor(SDL_GamepadButtonLabel label, Family family) {
    if (nintendo(family))
        return palette::kText;
    switch (label) {
    case SDL_GAMEPAD_BUTTON_LABEL_A: return IM_COL32(110, 204, 104, 255);
    case SDL_GAMEPAD_BUTTON_LABEL_B: return IM_COL32(232, 92, 80, 255);
    case SDL_GAMEPAD_BUTTON_LABEL_X: return IM_COL32(84, 148, 244, 255);
    default: return IM_COL32(244, 204, 76, 255);
    }
}

enum class ChipKind { Empty, Text, Key, ArrowKey, Face, Letter, Dpad, Stick };
struct Chip {
    ChipKind kind = ChipKind::Empty;
    std::string text;
    int direction = 0;
    PadInput face = PadInput::Count;
    ImU32 color = palette::kText;
    char side = 'L';
};

Chip describe(const PadBinding &binding, SDL_GamepadType type) {
    const Family family = familyOf(type);
    Chip chip;
    switch (binding.kind) {
    case PadBinding::Kind::Key: {
        const auto scancode = static_cast<SDL_Scancode>(binding.code);
        chip.direction = arrowKey(scancode);
        chip.kind = chip.direction >= 0 ? ChipKind::ArrowKey : ChipKind::Key;
        chip.text = keyText(scancode);
        break;
    }
    case PadBinding::Kind::Button: {
        const auto button = static_cast<SDL_GamepadButton>(binding.code);
        if (isFace(button)) {
            const SDL_GamepadButtonLabel faceName = faceLabel(button, type);
            chip.face = psFace(faceName);
            chip.kind = chip.face != PadInput::Count ? ChipKind::Face : ChipKind::Letter;
            if (chip.kind == ChipKind::Letter) {
                chip.text = letter(faceName);
                chip.color = letterColor(faceName, family);
            }
        } else if ((chip.direction = dpadDirection(button)) >= 0) {
            chip.kind = ChipKind::Dpad;
        } else {
            chip.kind = ChipKind::Text;
            chip.text = buttonText(button, family);
        }
        break;
    }
    case PadBinding::Kind::Axis: {
        const auto axis = static_cast<SDL_GamepadAxis>(binding.code);
        if (axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) {
            chip.kind = ChipKind::Text;
            chip.text = triggerText(axis, family);
        } else {
            chip.kind = ChipKind::Stick;
            chip.side = axis == SDL_GAMEPAD_AXIS_LEFTX || axis == SDL_GAMEPAD_AXIS_LEFTY ? 'L' : 'R';
            const bool horizontal = axis == SDL_GAMEPAD_AXIS_LEFTX || axis == SDL_GAMEPAD_AXIS_RIGHTX;
            chip.direction = horizontal ? (binding.direction < 0 ? kLeft : kRight)
                                        : (binding.direction < 0 ? kUp : kDown);
        }
        break;
    }
    case PadBinding::Kind::None:
        chip.text = "-";
        break;
    }
    return chip;
}

float chipFont(float height) { return std::max(10.0f, height * 0.52f); }
} // namespace

void drawIcon(ImDrawList *list, Icon kind, ImVec2 c, float size, ImU32 color) {
    const float s = size, t = stroke(size);
    const auto at = [&](float x, float y) { return ImVec2(c.x + x * s, c.y + y * s); };
    switch (kind) {
    case Icon::Display:
        list->AddRect(at(-0.45f, -0.36f), at(0.45f, 0.18f), color, s * 0.08f, t);
        list->AddLine(at(0.0f, 0.18f), at(0.0f, 0.33f), color, t);
        list->AddLine(at(-0.2f, 0.35f), at(0.2f, 0.35f), color, t);
        break;
    case Icon::Sound:
    case Icon::SoundOff: {
        const ImVec2 body[] = {at(-0.44f, -0.12f), at(-0.26f, -0.12f), at(-0.04f, -0.32f),
                               at(-0.04f, 0.32f), at(-0.26f, 0.12f), at(-0.44f, 0.12f)};
        list->AddConvexPolyFilled(body, 6, color);
        if (kind == Icon::Sound) {
            list->PathArcTo(at(-0.04f, 0.0f), s * 0.2f, -0.8f, 0.8f);
            list->PathStroke(color, t);
            list->PathArcTo(at(-0.04f, 0.0f), s * 0.36f, -0.8f, 0.8f);
            list->PathStroke(color, t);
        } else {
            list->AddLine(at(0.12f, -0.14f), at(0.4f, 0.14f), color, t);
            list->AddLine(at(0.12f, 0.14f), at(0.4f, -0.14f), color, t);
        }
        break;
    }
    case Icon::Controls:
        list->AddRect(at(-0.46f, -0.22f), at(0.46f, 0.2f), color, s * 0.2f, t);
        list->AddLine(at(-0.3f, -0.01f), at(-0.12f, -0.01f), color, t);
        list->AddLine(at(-0.21f, -0.1f), at(-0.21f, 0.08f), color, t);
        list->AddCircleFilled(at(0.16f, 0.04f), s * 0.055f, color);
        list->AddCircleFilled(at(0.28f, -0.07f), s * 0.055f, color);
        break;
    case Icon::Interface:
        for (int row = 0; row < 3; ++row) {
            const float y = -0.26f + row * 0.26f;
            const float knob = row == 0 ? -0.14f : row == 1 ? 0.2f : -0.02f;
            list->AddLine(at(-0.42f, y), at(0.42f, y), color, t);
            list->AddCircleFilled(at(knob, y), s * 0.09f, color);
        }
        break;
    case Icon::Crown: {
        const ImVec2 spikes[3][3] = {{at(-0.4f, 0.14f), at(-0.42f, -0.26f), at(-0.1f, 0.14f)},
                                     {at(-0.2f, 0.14f), at(0.0f, -0.36f), at(0.2f, 0.14f)},
                                     {at(0.1f, 0.14f), at(0.42f, -0.26f), at(0.4f, 0.14f)}};
        for (const auto &spike : spikes)
            list->AddTriangleFilled(spike[0], spike[1], spike[2], color);
        list->AddRectFilled(at(-0.4f, 0.08f), at(0.4f, 0.3f), color, s * 0.04f);
        for (const ImVec2 tip : {at(-0.42f, -0.28f), at(0.0f, -0.38f), at(0.42f, -0.28f)})
            list->AddCircleFilled(tip, s * 0.065f, color);
        list->AddCircleFilled(at(0.0f, 0.19f), s * 0.05f, IM_COL32(200, 40, 60, 255));
        break;
    }
    case Icon::Camera:
        list->AddRect(at(-0.44f, -0.2f), at(0.44f, 0.3f), color, s * 0.08f, t);
        list->AddRectFilled(at(-0.16f, -0.32f), at(0.14f, -0.2f), color, s * 0.04f);
        list->AddCircle(at(0.0f, 0.05f), s * 0.15f, color, 0, t);
        break;
    case Icon::Pause:
        list->AddRectFilled(at(-0.28f, -0.32f), at(-0.08f, 0.32f), color, s * 0.04f);
        list->AddRectFilled(at(0.08f, -0.32f), at(0.28f, 0.32f), color, s * 0.04f);
        break;
    case Icon::Play:
        list->AddTriangleFilled(at(-0.24f, -0.34f), at(0.34f, 0.0f), at(-0.24f, 0.34f), color);
        break;
    case Icon::Speed:
        list->AddTriangleFilled(at(-0.42f, -0.28f), at(0.0f, 0.0f), at(-0.42f, 0.28f), color);
        list->AddTriangleFilled(at(0.0f, -0.28f), at(0.42f, 0.0f), at(0.0f, 0.28f), color);
        break;
    case Icon::Keyboard:
        list->AddRect(at(-0.46f, -0.26f), at(0.46f, 0.26f), color, s * 0.08f, t);
        for (int row = 0; row < 2; ++row)
            for (int key = 0; key < 5; ++key)
                list->AddRectFilled(at(-0.33f + key * 0.165f - 0.04f, -0.13f + row * 0.13f - 0.035f),
                                    at(-0.33f + key * 0.165f + 0.04f, -0.13f + row * 0.13f + 0.035f), color);
        list->AddLine(at(-0.2f, 0.14f), at(0.2f, 0.14f), color, t);
        break;
    case Icon::Info:
        list->AddCircle(c, s * 0.4f, color, 0, t);
        list->AddCircleFilled(at(0.0f, -0.17f), s * 0.05f, color);
        list->AddLine(at(0.0f, -0.05f), at(0.0f, 0.22f), color, t);
        break;
    case Icon::Chart:
        list->AddRectFilled(at(-0.38f, 0.0f), at(-0.18f, 0.34f), color, s * 0.03f);
        list->AddRectFilled(at(-0.1f, -0.3f), at(0.1f, 0.34f), color, s * 0.03f);
        list->AddRectFilled(at(0.18f, -0.12f), at(0.38f, 0.34f), color, s * 0.03f);
        break;
    case Icon::Close:
        list->AddLine(at(-0.26f, -0.26f), at(0.26f, 0.26f), color, t * 1.2f);
        list->AddLine(at(-0.26f, 0.26f), at(0.26f, -0.26f), color, t * 1.2f);
        break;
    case Icon::Folder:
        list->AddRectFilled(at(-0.44f, -0.3f), at(-0.08f, -0.14f), color, s * 0.05f);
        list->AddRect(at(-0.44f, -0.2f), at(0.44f, 0.3f), color, s * 0.06f, t);
        break;
    case Icon::Link:
        list->AddRect(at(-0.38f, -0.26f), at(0.24f, 0.36f), color, s * 0.05f, t);
        list->AddLine(at(-0.04f, 0.04f), at(0.4f, -0.4f), color, t);
        list->AddLine(at(0.12f, -0.4f), at(0.4f, -0.4f), color, t);
        list->AddLine(at(0.4f, -0.4f), at(0.4f, -0.12f), color, t);
        break;
    case Icon::Check:
        list->AddLine(at(-0.32f, 0.02f), at(-0.08f, 0.26f), color, t * 1.3f);
        list->AddLine(at(-0.08f, 0.26f), at(0.34f, -0.24f), color, t * 1.3f);
        break;
    case Icon::Warning:
        list->AddTriangle(at(0.0f, -0.4f), at(0.42f, 0.34f), at(-0.42f, 0.34f), color, t);
        list->AddLine(at(0.0f, -0.12f), at(0.0f, 0.12f), color, t);
        list->AddCircleFilled(at(0.0f, 0.23f), s * 0.045f, color);
        break;
    case Icon::Reset:
        list->PathArcTo(c, s * 0.32f, -2.6f, 2.1f);
        list->PathStroke(color, t);
        list->AddTriangleFilled(at(-0.42f, -0.36f), at(-0.12f, -0.26f), at(-0.36f, -0.06f), color);
        break;
    case Icon::Disc:
        list->AddCircle(c, s * 0.42f, color, 40, t);
        list->AddCircle(c, s * 0.11f, color, 16, t);
        list->PathArcTo(c, s * 0.27f, -2.5f, -1.4f);
        list->PathStroke(color, t * 0.8f);
        break;
    case Icon::Wrench:
        list->AddLine(at(-0.36f, 0.36f), at(0.06f, -0.06f), color, t * 1.7f);
        list->PathArcTo(at(0.17f, -0.17f), s * 0.2f, 0.4f, 5.1f);
        list->PathStroke(color, t * 1.3f);
        break;
    case Icon::Gear:
        list->AddCircle(c, s * 0.24f, color, 28, t * 1.2f);
        list->AddCircle(c, s * 0.08f, color, 12, t);
        for (int tooth = 0; tooth < 8; ++tooth) {
            const float a = static_cast<float>(tooth) * 0.785398f;
            list->AddLine(ImVec2(c.x + std::cos(a) * s * 0.3f, c.y + std::sin(a) * s * 0.3f),
                          ImVec2(c.x + std::cos(a) * s * 0.43f, c.y + std::sin(a) * s * 0.43f), color, t * 1.7f);
        }
        break;
    case Icon::Copy:
        // Two sheets; the back one's edges stop where the front one hides them.
        list->AddRect(at(-0.12f, -0.12f), at(0.38f, 0.4f), color, s * 0.06f, t);
        list->PathLineTo(at(-0.12f, 0.14f));
        list->PathLineTo(at(-0.38f, 0.14f));
        list->PathLineTo(at(-0.38f, -0.38f));
        list->PathLineTo(at(0.12f, -0.38f));
        list->PathLineTo(at(0.12f, -0.12f));
        list->PathStroke(color, t);
        break;
    }
}

void icon(Icon kind, float size, ImU32 color) {
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(size, size));
    drawIcon(ImGui::GetWindowDrawList(), kind, ImVec2(pos.x + size * 0.5f, pos.y + size * 0.5f), size, color);
}

void drawPadGlyph(ImDrawList *list, PadInput input, ImVec2 c, float size) {
    const float r = size * 0.5f;
    const ImU32 fill = IM_COL32(255, 255, 255, 22);
    const ImU32 edge = IM_COL32(255, 255, 255, 70);
    switch (input) {
    case PadInput::Cross:
    case PadInput::Circle:
    case PadInput::Square:
    case PadInput::Triangle:
        list->AddCircleFilled(c, r, fill);
        list->AddCircle(c, r - 0.5f, edge, 0, 1.0f);
        faceSymbol(list, input, c, size, faceColor(input));
        break;
    case PadInput::Up: dpad(list, c, size, kUp, palette::kText, kDim); break;
    case PadInput::Down: dpad(list, c, size, kDown, palette::kText, kDim); break;
    case PadInput::Left: dpad(list, c, size, kLeft, palette::kText, kDim); break;
    case PadInput::Right: dpad(list, c, size, kRight, palette::kText, kDim); break;
    case PadInput::L1:
    case PadInput::R1:
    case PadInput::L2:
    case PadInput::R2: {
        const ImVec2 min(c.x - r, c.y - r * 0.62f), max(c.x + r, c.y + r * 0.62f);
        list->AddRectFilled(min, max, fill, r * 0.35f);
        list->AddRect(min, max, edge, r * 0.35f, 1.0f);
        label(list, c, size * 0.44f, palette::kText, gfx::padInputName(input));
        break;
    }
    case PadInput::L3:
    case PadInput::R3:
        list->AddCircleFilled(c, r, fill);
        list->AddCircle(c, r - 0.5f, edge, 0, 1.0f);
        label(list, c, size * 0.4f, palette::kText, gfx::padInputName(input));
        break;
    case PadInput::Start:
        pill(list, ImVec2(c.x - r, c.y - r * 0.36f), ImVec2(c.x + r, c.y + r * 0.36f), fill, edge);
        arrow(list, c, size * 0.3f, kRight, palette::kText);
        break;
    case PadInput::Select:
        pill(list, ImVec2(c.x - r, c.y - r * 0.36f), ImVec2(c.x + r, c.y + r * 0.36f), fill, edge);
        list->AddRectFilled(ImVec2(c.x - r * 0.38f, c.y - r * 0.1f), ImVec2(c.x + r * 0.38f, c.y + r * 0.1f),
                            palette::kText, 1.0f);
        break;
    case PadInput::LeftStickUp: stick(list, c, size, 'L', kUp, palette::kText); break;
    case PadInput::LeftStickDown: stick(list, c, size, 'L', kDown, palette::kText); break;
    case PadInput::LeftStickLeft: stick(list, c, size, 'L', kLeft, palette::kText); break;
    case PadInput::LeftStickRight: stick(list, c, size, 'L', kRight, palette::kText); break;
    case PadInput::RightStickUp: stick(list, c, size, 'R', kUp, palette::kText); break;
    case PadInput::RightStickDown: stick(list, c, size, 'R', kDown, palette::kText); break;
    case PadInput::RightStickLeft: stick(list, c, size, 'R', kLeft, palette::kText); break;
    case PadInput::RightStickRight: stick(list, c, size, 'R', kRight, palette::kText); break;
    case PadInput::Count:
        break;
    }
}

float bindingChipWidth(const PadBinding &binding, SDL_GamepadType type, float height) {
    const Chip chip = describe(binding, type);
    switch (chip.kind) {
    case ChipKind::Empty: return height * 1.6f;
    case ChipKind::Text: return std::max(height * 1.3f, labelWidth(chipFont(height), chip.text.c_str()) + height * 0.8f);
    case ChipKind::Key: return std::max(height * 1.1f, labelWidth(chipFont(height), chip.text.c_str()) + height * 0.7f);
    case ChipKind::Stick: return height * 1.3f;
    default: return height;
    }
}

void drawBindingChip(ImDrawList *list, const PadBinding &binding, SDL_GamepadType type, ImVec2 min,
                     float height, float alpha) {
    // On whole pixels: a ring drawn half a pixel over with its letter
    // snapped back reads as a letter off to one side.
    min = ImVec2(std::floor(min.x), std::floor(min.y));
    const Chip chip = describe(binding, type);
    const float width = bindingChipWidth(binding, type, height);
    const ImVec2 max(min.x + width, min.y + height);
    const ImVec2 c((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f);
    const float font = chipFont(height);
    const ImU32 text = withAlpha(palette::kText, alpha);
    const ImU32 fill = IM_COL32(255, 255, 255, static_cast<int>(26 * alpha));
    const ImU32 edge = IM_COL32(255, 255, 255, static_cast<int>(84 * alpha));
    switch (chip.kind) {
    case ChipKind::Empty:
        list->AddRect(min, max, IM_COL32(255, 255, 255, static_cast<int>(40 * alpha)), height * 0.3f, 1.0f);
        label(list, c, font, withAlpha(palette::kTextMuted, alpha), "-");
        break;
    case ChipKind::Text:
        pill(list, min, max, fill, edge);
        label(list, c, font, text, chip.text.c_str());
        break;
    case ChipKind::Key:
    case ChipKind::ArrowKey: {
        // A keycap: a darker base shows under its face, like a key's side.
        const float rounding = height * 0.22f;
        list->AddRectFilled(ImVec2(min.x, min.y + 2.0f), ImVec2(max.x, max.y + 1.0f),
                            IM_COL32(0, 0, 10, static_cast<int>(120 * alpha)), rounding);
        list->AddRectFilled(min, max, IM_COL32(255, 255, 255, static_cast<int>(36 * alpha)), rounding);
        list->AddRect(min, max, edge, rounding, 1.0f);
        if (chip.kind == ChipKind::ArrowKey)
            arrow(list, c, height * 0.42f, chip.direction, text);
        else
            label(list, c, font, text, chip.text.c_str());
        break;
    }
    case ChipKind::Face:
        list->AddCircleFilled(c, height * 0.5f, fill);
        list->AddCircle(c, height * 0.5f - 0.5f, edge, 0, 1.0f);
        faceSymbol(list, chip.face, c, height, withAlpha(faceColor(chip.face), alpha));
        break;
    case ChipKind::Letter: {
        // The letter lands on whole pixels, up to half a pixel off; a ring
        // drawn smooth can follow it there, so it sits dead centre.
        const Placed placed = place(c, font, chip.text.c_str());
        const ImVec2 ring(c.x + placed.shift.x, c.y + placed.shift.y);
        list->AddCircleFilled(ring, height * 0.5f, fill);
        list->AddCircle(ring, height * 0.5f - 0.5f, withAlpha(chip.color, 0.7f * alpha), 0, 1.5f);
        if (placed.visible)
            list->AddText(ImGui::GetFont(), font, placed.at, withAlpha(chip.color, alpha), chip.text.c_str());
        break;
    }
    case ChipKind::Dpad:
        dpad(list, c, height * 0.92f, chip.direction, text, IM_COL32(255, 255, 255, static_cast<int>(52 * alpha)));
        break;
    case ChipKind::Stick:
        stick(list, c, height * 0.95f, chip.side, chip.direction, text);
        break;
    }
}

std::string bindingLabel(const PadBinding &binding, SDL_GamepadType type) {
    const Family family = familyOf(type);
    switch (binding.kind) {
    case PadBinding::Kind::Key: {
        const char *name = SDL_GetScancodeName(static_cast<SDL_Scancode>(binding.code));
        return name && *name ? name : "Unknown key";
    }
    case PadBinding::Kind::Button: {
        const auto button = static_cast<SDL_GamepadButton>(binding.code);
        if (isFace(button)) {
            const SDL_GamepadButtonLabel name = faceLabel(button, type);
            switch (name) {
            case SDL_GAMEPAD_BUTTON_LABEL_CROSS: return "Cross";
            case SDL_GAMEPAD_BUTTON_LABEL_CIRCLE: return "Circle";
            case SDL_GAMEPAD_BUTTON_LABEL_SQUARE: return "Square";
            case SDL_GAMEPAD_BUTTON_LABEL_TRIANGLE: return "Triangle";
            default: return letter(name);
            }
        }
        switch (dpadDirection(button)) {
        case kUp: return "D-pad up";
        case kDown: return "D-pad down";
        case kLeft: return "D-pad left";
        case kRight: return "D-pad right";
        default: return buttonText(button, family);
        }
    }
    case PadBinding::Kind::Axis: {
        const auto axis = static_cast<SDL_GamepadAxis>(binding.code);
        if (axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)
            return triggerText(axis, family);
        const bool left = axis == SDL_GAMEPAD_AXIS_LEFTX || axis == SDL_GAMEPAD_AXIS_LEFTY;
        const bool horizontal = axis == SDL_GAMEPAD_AXIS_LEFTX || axis == SDL_GAMEPAD_AXIS_RIGHTX;
        const char *way = horizontal ? (binding.direction < 0 ? "left" : "right")
                                     : (binding.direction < 0 ? "up" : "down");
        return std::string(left ? "Left stick " : "Right stick ") + way;
    }
    case PadBinding::Kind::None:
        break;
    }
    return "Not bound";
}

const char *controllerFamilyName(SDL_GamepadType type) {
    switch (familyOf(type)) {
    case Family::Xbox360: return "Xbox 360";
    case Family::PlayStation: return "PlayStation";
    case Family::PlayStation5: return "DualSense";
    case Family::Nintendo: return "Nintendo Switch";
    case Family::GameCube: return "GameCube";
    case Family::Xbox: break;
    }
    return type == SDL_GAMEPAD_TYPE_XBOXONE ? "Xbox" : "Controller";
}

void drawPadDiagram(ImDrawList *list, ImVec2 min, ImVec2 size, const gfx::PadSnapshot &pad,
                    PadInput highlight) {
    // Laid out on a 320x200 grid, then fitted and centred.
    const float k = std::min(size.x / 320.0f, size.y / 200.0f);
    const ImVec2 origin(min.x + (size.x - 320.0f * k) * 0.5f, min.y + (size.y - 200.0f * k) * 0.5f);
    const auto at = [&](float x, float y) { return ImVec2(origin.x + x * k, origin.y + y * k); };
    const auto held = [&](PadInput input) { return (pad.held & gfx::padButtonMask(input)) != 0u; };

    const ImU32 body = IM_COL32(30, 37, 70, 255);
    const ImU32 well = IM_COL32(18, 23, 48, 255);
    const ImU32 button = IM_COL32(52, 62, 104, 255);
    const ImU32 lit = palette::kGold;
    const ImU32 outline = palette::kBorder;
    const float pulse = 0.55f + 0.45f * std::sin(static_cast<float>(ImGui::GetTime()) * 5.0f);
    const ImU32 ring = withAlpha(palette::kGold, pulse);

    // Shoulders, behind the body.
    struct Shoulder { PadInput input; float x0, y0, x1, y1; };
    const Shoulder shoulders[] = {{PadInput::L2, 64, 12, 112, 28}, {PadInput::L1, 60, 27, 116, 42},
                                  {PadInput::R2, 208, 12, 256, 28}, {PadInput::R1, 204, 27, 260, 42}};
    for (const auto &s : shoulders) {
        const bool on = held(s.input);
        list->AddRectFilled(at(s.x0, s.y0), at(s.x1, s.y1), on ? lit : button, 6.0f * k);
        list->AddRect(at(s.x0, s.y0), at(s.x1, s.y1), withAlpha(outline, 0.85f), 6.0f * k, 1.5f);
        if (k >= 0.9f)
            label(list, at((s.x0 + s.x1) * 0.5f, (s.y0 + s.y1) * 0.5f - 1.0f), 10.0f * k,
                  on ? well : palette::kTextMuted, gfx::padInputName(s.input));
        if (highlight == s.input)
            list->AddRect(at(s.x0 - 3, s.y0 - 3), at(s.x1 + 3, s.y1 + 3), ring, 8.0f * k, 2.5f);
    }

    // The body: every shape once in white a little larger, then in navy, so
    // the union gets one clean edge like the game's windows.
    for (int pass = 0; pass < 2; ++pass) {
        const float grow = pass == 0 ? 2.5f : 0.0f;
        const ImU32 fill = pass == 0 ? outline : body;
        list->AddRectFilled(ImVec2(at(64, 38).x - grow, at(64, 38).y - grow),
                            ImVec2(at(256, 126).x + grow, at(256, 126).y + grow), fill, 34.0f * k + grow);
        for (const float side : {-1.0f, 1.0f}) {
            const ImVec2 top = at(160.0f + side * 66.0f, 98.0f), bottom = at(160.0f + side * 88.0f, 176.0f);
            list->AddLine(top, bottom, fill, 54.0f * k + grow * 2.0f);
            list->AddCircleFilled(top, 27.0f * k + grow, fill);
            list->AddCircleFilled(bottom, 27.0f * k + grow, fill);
        }
    }

    // D-pad.
    const ImVec2 dpadCenter = at(96, 82);
    list->AddCircleFilled(dpadCenter, 30.0f * k, well);
    struct Arm { PadInput input; float x0, y0, x1, y1; int direction; };
    const Arm arms[] = {{PadInput::Up, 89, 56, 103, 76, kUp}, {PadInput::Down, 89, 88, 103, 108, kDown},
                        {PadInput::Left, 70, 75, 90, 89, kLeft}, {PadInput::Right, 102, 75, 122, 89, kRight}};
    list->AddRectFilled(at(89, 75), at(103, 89), button);
    for (const auto &arm : arms) {
        const bool on = held(arm.input);
        list->AddRectFilled(at(arm.x0, arm.y0), at(arm.x1, arm.y1), on ? lit : button, 3.0f * k);
        arrow(list, at((arm.x0 + arm.x1) * 0.5f, (arm.y0 + arm.y1) * 0.5f), 7.0f * k, arm.direction,
              on ? well : IM_COL32(255, 255, 255, 110));
        if (highlight == arm.input)
            list->AddRect(at(arm.x0 - 3, arm.y0 - 3), at(arm.x1 + 3, arm.y1 + 3), ring, 4.0f * k, 2.5f);
    }

    // Face buttons.
    const ImVec2 faceCenter = at(224, 82);
    list->AddCircleFilled(faceCenter, 33.0f * k, well);
    struct Face { PadInput input; float x, y; };
    const Face faces[] = {{PadInput::Triangle, 224, 61}, {PadInput::Circle, 245, 82},
                          {PadInput::Cross, 224, 103}, {PadInput::Square, 203, 82}};
    for (const auto &face : faces) {
        const bool on = held(face.input);
        const ImVec2 c = at(face.x, face.y);
        list->AddCircleFilled(c, 10.5f * k, on ? faceColor(face.input) : button);
        faceSymbol(list, face.input, c, 21.0f * k, on ? well : faceColor(face.input));
        if (highlight == face.input)
            list->AddCircle(c, 14.0f * k, ring, 0, 2.5f);
    }

    // Select and Start, and the analog LED between the sticks.
    struct Small { PadInput input; float x0, y0, x1, y1; };
    const Small smalls[] = {{PadInput::Select, 128, 68, 150, 77}, {PadInput::Start, 170, 68, 192, 77}};
    for (const auto &small : smalls) {
        const bool on = held(small.input);
        list->AddRectFilled(at(small.x0, small.y0), at(small.x1, small.y1), on ? lit : button, 4.5f * k);
        if (highlight == small.input)
            list->AddRect(at(small.x0 - 3, small.y0 - 3), at(small.x1 + 3, small.y1 + 3), ring, 6.0f * k, 2.5f);
    }
    list->AddCircleFilled(at(160, 97), 3.2f * k, IM_COL32(236, 64, 64, 255));

    // Sticks, each knob pushed by the stick's live position.
    const auto stickByte = [&](int shift) { return (static_cast<float>((pad.sticks >> shift) & 0xFFu) - 128.0f) / 128.0f; };
    struct Stick { float x, y, dx, dy; PadInput click, first; };
    const Stick sticks[] = {{126, 130, stickByte(8), stickByte(0), PadInput::L3, PadInput::LeftStickUp},
                            {194, 130, stickByte(24), stickByte(16), PadInput::R3, PadInput::RightStickUp}};
    for (const auto &s : sticks) {
        const ImVec2 base = at(s.x, s.y);
        list->AddCircleFilled(base, 22.0f * k, well);
        const ImVec2 knob(base.x + s.dx * 9.0f * k, base.y + s.dy * 9.0f * k);
        const bool on = held(s.click);
        list->AddCircleFilled(knob, 15.0f * k, on ? lit : button);
        list->AddCircle(knob, 10.0f * k, on ? well : IM_COL32(255, 255, 255, 50), 0, 1.5f);
        const bool ringed = highlight == s.click ||
                            (highlight >= s.first && static_cast<int>(highlight) < static_cast<int>(s.first) + 4);
        if (ringed)
            list->AddCircle(base, 25.0f * k, ring, 0, 2.5f);
        if (highlight >= s.first && static_cast<int>(highlight) < static_cast<int>(s.first) + 4) {
            const int direction = static_cast<int>(highlight) - static_cast<int>(s.first);
            const ImVec2 offsets[4] = {{0, -33}, {0, 33}, {-33, 0}, {33, 0}};
            arrow(list, ImVec2(base.x + offsets[direction].x * k, base.y + offsets[direction].y * k), 9.0f * k,
                  direction, ring);
        }
    }
}

} // namespace dq8::ui
