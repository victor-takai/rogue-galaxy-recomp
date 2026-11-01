#include "button_art.h"

#include <cstring>

namespace rg::buttons::art
{
namespace
{

// Letters two pixels thick: the face buttons' 8 rows high, the plates' 10.
const char *const kFaceA[] = {"..XXXX..", ".XXXXXX.", "XXX..XXX", "XX....XX",
                              "XXXXXXXX", "XXXXXXXX", "XX....XX", "XX....XX"};
const char *const kFaceB[] = {"XXXXXX.", "XX...XX", "XX...XX", "XXXXXX.",
                              "XXXXXXX", "XX...XX", "XX...XX", "XXXXXX."};
const char *const kFaceX[] = {"XX....XX", "XXX..XXX", ".XXXXXX.", "..XXXX..",
                              "..XXXX..", ".XXXXXX.", "XXX..XXX", "XX....XX"};
const char *const kFaceY[] = {"XX....XX", "XXX..XXX", ".XXXXXX.", "..XXXX..",
                              "...XX...", "...XX...", "...XX...", "...XX..."};
constexpr int kFaceRows = 8;
const char *const kTallL[] = {"XX....", "XX....", "XX....", "XX....", "XX....",
                              "XX....", "XX....", "XX....", "XXXXXX", "XXXXXX"};
const char *const kTallR[] = {"XXXXXX.", "XXXXXXX", "XX...XX", "XX...XX", "XXXXXXX",
                              "XXXXXX.", "XX.XX..", "XX..XX.", "XX...XX", "XX...XX"};
const char *const kTallB[] = {"XXXXXX.", "XXXXXXX", "XX...XX", "XX...XX", "XXXXXX.",
                              "XXXXXXX", "XX...XX", "XX...XX", "XXXXXXX", "XXXXXX."};
const char *const kTallT[] = {"XXXXXXXX", "XXXXXXXX", "...XX...", "...XX...", "...XX...",
                              "...XX...", "...XX...", "...XX...", "...XX...", "...XX..."};
constexpr int kTallRows = 10;
constexpr int kPlateGap = 3;

constexpr Rgb kGreen{110, 204, 104};
constexpr Rgb kRed{232, 92, 80};
constexpr Rgb kBlue{84, 148, 244};
constexpr Rgb kYellow{244, 204, 76};

// The first letter of a plate's two, and the second.
char plateChar(Glyph glyph, bool second)
{
    if (!second)
    {
        return glyph == Glyph::LB || glyph == Glyph::LT ? 'L' : 'R';
    }
    return glyph == Glyph::LB || glyph == Glyph::RB ? 'B' : 'T';
}

char faceChar(Glyph glyph)
{
    switch (glyph)
    {
    case Glyph::A:
        return 'A';
    case Glyph::B:
        return 'B';
    case Glyph::X:
        return 'X';
    case Glyph::Y:
        return 'Y';
    default:
        return 0;
    }
}

} // namespace

int Bitmap::width() const
{
    return rows ? static_cast<int>(std::strlen(rows[0])) : 0;
}

Bitmap faceLetter(Glyph glyph)
{
    switch (glyph)
    {
    case Glyph::A:
        return {kFaceA, kFaceRows};
    case Glyph::B:
        return {kFaceB, kFaceRows};
    case Glyph::X:
        return {kFaceX, kFaceRows};
    case Glyph::Y:
        return {kFaceY, kFaceRows};
    default:
        return {nullptr, 0};
    }
}

int faceLetterX(const Bitmap &letter)
{
    return (kFaceSize - letter.width()) / 2;
}

Bitmap plateLetter(Glyph glyph, bool second)
{
    switch (plateChar(glyph, second))
    {
    case 'L':
        return {kTallL, kTallRows};
    case 'R':
        return {kTallR, kTallRows};
    case 'B':
        return {kTallB, kTallRows};
    default:
        return {kTallT, kTallRows};
    }
}

int plateLetterX(Glyph glyph, bool second)
{
    const int first = plateLetter(glyph, false).width();
    const int left = (kPlateWidth - (first + kPlateGap + plateLetter(glyph, true).width())) / 2;
    return second ? left + first + kPlateGap : left;
}

Rgb faceColour(Glyph glyph)
{
    switch (glyph)
    {
    case Glyph::B:
        return kRed;
    case Glyph::X:
        return kBlue;
    case Glyph::Y:
        return kYellow;
    default:
        return kGreen;
    }
}

} // namespace rg::buttons::art
