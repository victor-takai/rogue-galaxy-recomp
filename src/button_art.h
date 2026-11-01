#pragma once

#include <cstdint>
#include <vector>

#include "button_glyphs.h"

// What an Xbox pad's buttons look like in the special-character picture
// (gaiji2.img, 256x256): the letters as the PS2-sized picture has them,
// which src/button_glyphs.cpp draws into it. Nothing here touches the
// game's memory.
namespace rg::buttons::art
{

// The picture's corner of buttons, in Glyphs' order (Cross, Circle, Square,
// Triangle, L1, R1, L2, R2): 22x22 circles below 32x20 plates.
struct Cell
{
    int x, y;
    bool face;
};
constexpr Cell kCells[kButtons] = {
    {66, 20, true}, {0, 20, true}, {44, 20, true}, {22, 20, true},
    {32, 0, false}, {64, 0, false}, {0, 0, false}, {96, 0, false},
};
constexpr int kFaceSize = 22;
constexpr int kPlateWidth = 32;
constexpr int kPlateHeight = 20;

// A letter as the PS2-sized picture has it: rows of 'X' (on) and '.'.
struct Bitmap
{
    const char *const *rows;
    int count;
    int width() const;
};
// A face button's letter (A, B, X or Y), with its top-left corner in the
// cell; null rows for any other glyph.
Bitmap faceLetter(Glyph glyph);
int faceLetterX(const Bitmap &letter);
constexpr int kFaceLetterY = 7;
// A plate's two letters (LB, RB, LT or RT; `second` for the right one).
Bitmap plateLetter(Glyph glyph, bool second);
int plateLetterX(Glyph glyph, bool second);
constexpr int kPlateLetterY = 5;

struct Rgb
{
    int r, g, b;
};
// The face buttons' letters in their colours, as the menu's controller
// pictures have them, each in a dark edge; the plates' white.
Rgb faceColour(Glyph glyph);
constexpr Rgb kLetterEdge{46, 46, 47};
constexpr Rgb kPlateWhite{230, 230, 230};
constexpr Rgb kPlateEdge{35, 35, 35};
constexpr Rgb kPlateShade{51, 50, 48};
// The plate's face, cleared of the PlayStation's letters (and the shade
// below it), in the cell.
constexpr int kPlateFaceLeft = 4, kPlateFaceRight = 27, kPlateFaceTop = 4, kPlateFaceBottom = 15;

} // namespace rg::buttons::art
