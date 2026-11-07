#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

class PS2Runtime;

// The controller's own buttons in the game's text, or the keys.
//
// The game writes its buttons into text as special characters, drawn from
// one 256x256 picture (gaiji2.img, loaded to 0x4C3180 and registered as a
// texture by GaijiRegister): the PlayStation's face buttons, 22x22 circles,
// and L1, R1, L2 and R2, 32x20 plates. With another controller in use they
// are redrawn in the picture, in the game's style, as what that controller
// calls the button each one is bound to (A, B, X and Y in their colours, LB,
// RB, LT and RT), and on the keyboard and mouse as a keycap with the key's
// name or a mouse with its button pressed, back to the PlayStation's
// otherwise. The picture is
// 8-bit indexed, stored as the GS keeps it, so the new pixels take the
// nearest of its 256 colours. The loading screen's "Press X" is a picture
// of its own, its cross redrawn the same way as it is registered (its
// letter's colour, or a mouse's pressed button, in one of the cross's blues,
// which nothing else uses; a keycap on the screen's own background).
//
// RG_GAIJI_DUMP=FILE writes the picture as loaded, and FILE.applied as drawn.
namespace rg::buttons
{

enum class Glyph : uint8_t
{
    PlayStation,
    A,
    B,
    X,
    Y,
    LB,
    RB,
    LT,
    RT,
    // A key or mouse button, as its KeyLabel says: a keycap with the key's
    // name, or a mouse with its button pressed, on the picture's clear
    // background in place of the button's circle or plate.
    Key,
};

// Cross, Circle, Square, Triangle, L1, R1, L2, R2.
constexpr size_t kButtons = 8;
using Glyphs = std::array<Glyph, kButtons>;

// What a Key glyph shows: a picture, or up to three letters and digits
// (A to Z, 0 to 9, ?), one large or two or three small in a circle, up to
// three large on a plate.
enum class KeyIcon : uint8_t
{
    None,
    MouseLeft,
    MouseRight,
    MouseMiddle,
    Space,
    Up,
    Down,
    Left,
    Right,
};
struct KeyLabel
{
    KeyIcon icon = KeyIcon::None;
    char text[4] = {};
    bool operator==(const KeyLabel &) const = default;
};
using KeyLabels = std::array<KeyLabel, kButtons>;

// After loadELF().
void install(PS2Runtime &runtime);

// What each button shows from now on, and the Key glyphs' labels; the
// game's thread redraws the picture before its next special character.
void setGlyphs(const Glyphs &glyphs, const KeyLabels &labels = {});

// The loading screen's picture with "Press X" (Loading_01, 128x64) and its
// cross's 20x20 circle there.
constexpr int kLoadingCrossX = 81;
constexpr int kLoadingCrossY = 42;
constexpr int kLoadingCross = 20;

} // namespace rg::buttons
