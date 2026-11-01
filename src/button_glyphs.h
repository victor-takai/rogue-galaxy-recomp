#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

class PS2Runtime;

// The controller's own buttons in the game's text.
//
// The game writes its buttons into text as special characters, drawn from
// one 256x256 picture (gaiji2.img, loaded to 0x4C3180 and registered as a
// texture by GaijiRegister): the PlayStation's face buttons, 22x22 circles,
// and L1, R1, L2 and R2, 32x20 plates. With another controller in use they
// are redrawn in the picture, in the game's style, as what that controller
// calls the button each one is bound to (A, B, X and Y in their colours, LB,
// RB, LT and RT), back to the PlayStation's otherwise. The picture is
// 8-bit indexed, stored as the GS keeps it, so the new pixels take the
// nearest of its 256 colours. The loading screen's "Press X" is a picture
// of its own, its cross redrawn the same way as it is registered (its
// letter's colour in one of the cross's blues, which nothing else uses).
//
// RG_GAIJI_DUMP=FILE writes the picture as loaded.
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
};

// Cross, Circle, Square, Triangle, L1, R1, L2, R2.
constexpr size_t kButtons = 8;
using Glyphs = std::array<Glyph, kButtons>;

// After loadELF().
void install(PS2Runtime &runtime);

// What each button shows from now on; the game's thread redraws the
// picture before its next special character.
void setGlyphs(const Glyphs &glyphs);

// The loading screen's picture with "Press X" (Loading_01, 128x64) and its
// cross's 20x20 circle there.
constexpr int kLoadingCrossX = 81;
constexpr int kLoadingCrossY = 42;
constexpr int kLoadingCross = 20;

} // namespace rg::buttons
