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

// The keys' letters, 5x7 and 3x5, A to Z, 0 to 9 and ?.
constexpr char kKeyChars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789?";
const char *const kLarge[][7] = {
    {".XXX.", "X...X", "X...X", "XXXXX", "X...X", "X...X", "X...X"}, // A
    {"XXXX.", "X...X", "X...X", "XXXX.", "X...X", "X...X", "XXXX."}, // B
    {".XXX.", "X...X", "X....", "X....", "X....", "X...X", ".XXX."}, // C
    {"XXXX.", "X...X", "X...X", "X...X", "X...X", "X...X", "XXXX."}, // D
    {"XXXXX", "X....", "X....", "XXXX.", "X....", "X....", "XXXXX"}, // E
    {"XXXXX", "X....", "X....", "XXXX.", "X....", "X....", "X...."}, // F
    {".XXX.", "X...X", "X....", "X.XXX", "X...X", "X...X", ".XXXX"}, // G
    {"X...X", "X...X", "X...X", "XXXXX", "X...X", "X...X", "X...X"}, // H
    {".XXX.", "..X..", "..X..", "..X..", "..X..", "..X..", ".XXX."}, // I
    {"..XXX", "...X.", "...X.", "...X.", "...X.", "X..X.", ".XX.."}, // J
    {"X...X", "X..X.", "X.X..", "XX...", "X.X..", "X..X.", "X...X"}, // K
    {"X....", "X....", "X....", "X....", "X....", "X....", "XXXXX"}, // L
    {"X...X", "XX.XX", "X.X.X", "X.X.X", "X...X", "X...X", "X...X"}, // M
    {"X...X", "X...X", "XX..X", "X.X.X", "X..XX", "X...X", "X...X"}, // N
    {".XXX.", "X...X", "X...X", "X...X", "X...X", "X...X", ".XXX."}, // O
    {"XXXX.", "X...X", "X...X", "XXXX.", "X....", "X....", "X...."}, // P
    {".XXX.", "X...X", "X...X", "X...X", "X.X.X", "X..X.", ".XX.X"}, // Q
    {"XXXX.", "X...X", "X...X", "XXXX.", "X.X..", "X..X.", "X...X"}, // R
    {".XXXX", "X....", "X....", ".XXX.", "....X", "....X", "XXXX."}, // S
    {"XXXXX", "..X..", "..X..", "..X..", "..X..", "..X..", "..X.."}, // T
    {"X...X", "X...X", "X...X", "X...X", "X...X", "X...X", ".XXX."}, // U
    {"X...X", "X...X", "X...X", "X...X", "X...X", ".X.X.", "..X.."}, // V
    {"X...X", "X...X", "X...X", "X.X.X", "X.X.X", "X.X.X", ".X.X."}, // W
    {"X...X", "X...X", ".X.X.", "..X..", ".X.X.", "X...X", "X...X"}, // X
    {"X...X", "X...X", ".X.X.", "..X..", "..X..", "..X..", "..X.."}, // Y
    {"XXXXX", "....X", "...X.", "..X..", ".X...", "X....", "XXXXX"}, // Z
    {".XXX.", "X...X", "X..XX", "X.X.X", "XX..X", "X...X", ".XXX."}, // 0
    {"..X..", ".XX..", "..X..", "..X..", "..X..", "..X..", ".XXX."}, // 1
    {".XXX.", "X...X", "....X", "...X.", "..X..", ".X...", "XXXXX"}, // 2
    {"XXXXX", "...X.", "..X..", "...X.", "....X", "X...X", ".XXX."}, // 3
    {"...X.", "..XX.", ".X.X.", "X..X.", "XXXXX", "...X.", "...X."}, // 4
    {"XXXXX", "X....", "XXXX.", "....X", "....X", "X...X", ".XXX."}, // 5
    {"..XX.", ".X...", "X....", "XXXX.", "X...X", "X...X", ".XXX."}, // 6
    {"XXXXX", "....X", "...X.", "..X..", ".X...", ".X...", ".X..."}, // 7
    {".XXX.", "X...X", "X...X", ".XXX.", "X...X", "X...X", ".XXX."}, // 8
    {".XXX.", "X...X", "X...X", ".XXXX", "....X", "...X.", ".XX.."}, // 9
    {".XXX.", "X...X", "....X", "...X.", "..X..", ".....", "..X.."}, // ?
};
const char *const kSmall[][5] = {
    {".X.", "X.X", "XXX", "X.X", "X.X"}, // A
    {"XX.", "X.X", "XX.", "X.X", "XX."}, // B
    {".XX", "X..", "X..", "X..", ".XX"}, // C
    {"XX.", "X.X", "X.X", "X.X", "XX."}, // D
    {"XXX", "X..", "XX.", "X..", "XXX"}, // E
    {"XXX", "X..", "XX.", "X..", "X.."}, // F
    {".XX", "X..", "X.X", "X.X", ".XX"}, // G
    {"X.X", "X.X", "XXX", "X.X", "X.X"}, // H
    {"XXX", ".X.", ".X.", ".X.", "XXX"}, // I
    {"..X", "..X", "..X", "X.X", ".X."}, // J
    {"X.X", "X.X", "XX.", "X.X", "X.X"}, // K
    {"X..", "X..", "X..", "X..", "XXX"}, // L
    {"X.X", "XXX", "XXX", "X.X", "X.X"}, // M
    {"XX.", "X.X", "X.X", "X.X", "X.X"}, // N
    {".X.", "X.X", "X.X", "X.X", ".X."}, // O
    {"XX.", "X.X", "XX.", "X..", "X.."}, // P
    {".X.", "X.X", "X.X", "XX.", ".XX"}, // Q
    {"XX.", "X.X", "XX.", "X.X", "X.X"}, // R
    {".XX", "X..", ".X.", "..X", "XX."}, // S
    {"XXX", ".X.", ".X.", ".X.", ".X."}, // T
    {"X.X", "X.X", "X.X", "X.X", "XXX"}, // U
    {"X.X", "X.X", "X.X", "X.X", ".X."}, // V
    {"X.X", "X.X", "XXX", "XXX", "X.X"}, // W
    {"X.X", "X.X", ".X.", "X.X", "X.X"}, // X
    {"X.X", "X.X", ".X.", ".X.", ".X."}, // Y
    {"XXX", "..X", ".X.", "X..", "XXX"}, // Z
    {"XXX", "X.X", "X.X", "X.X", "XXX"}, // 0
    {".X.", "XX.", ".X.", ".X.", "XXX"}, // 1
    {"XX.", "..X", ".X.", "X..", "XXX"}, // 2
    {"XX.", "..X", ".X.", "..X", "XX."}, // 3
    {"X.X", "X.X", "XXX", "..X", "..X"}, // 4
    {"XXX", "X..", "XX.", "..X", "XX."}, // 5
    {".XX", "X..", "XXX", "X.X", "XXX"}, // 6
    {"XXX", "..X", ".X.", ".X.", ".X."}, // 7
    {"XXX", "X.X", "XXX", "X.X", "XXX"}, // 8
    {"XXX", "X.X", "XXX", "..X", "XX."}, // 9
    {"XX.", "..X", ".X.", "...", ".X."}, // ?
};

// The keys' pictures: a mouse with its button (the left, the right, the
// wheel) filled, the space bar, the arrows.
const char *const kMouseLeft[] = {".XXXXXXX.", "XXXXX...X", "XXXXX...X", "XXXXX...X", "XXXXXXXXX", "X.......X",
                                  "X.......X", "X.......X", "X.......X", ".X.....X.", "..XXXXX.."};
const char *const kMouseRight[] = {".XXXXXXX.", "X...XXXXX", "X...XXXXX", "X...XXXXX", "XXXXXXXXX", "X.......X",
                                   "X.......X", "X.......X", "X.......X", ".X.....X.", "..XXXXX.."};
const char *const kMouseMiddle[] = {".XXXXXXX.", "X..XXX..X", "X..XXX..X", "X..XXX..X", "XXXXXXXXX", "X.......X",
                                    "X.......X", "X.......X", "X.......X", ".X.....X.", "..XXXXX.."};
constexpr int kMouseRows = 11;
const char *const kSmallMouseLeft[] = {".XXXXX.", "XXXX..X", "XXXX..X", "XXXXXXX", "X.....X",
                                       "X.....X", "X.....X", "X.....X", ".X...X.", "..XXX.."};
const char *const kSmallMouseRight[] = {".XXXXX.", "X..XXXX", "X..XXXX", "XXXXXXX", "X.....X",
                                        "X.....X", "X.....X", "X.....X", ".X...X.", "..XXX.."};
const char *const kSmallMouseMiddle[] = {".XXXXX.", "X.XXX.X", "X.XXX.X", "XXXXXXX", "X.....X",
                                         "X.....X", "X.....X", "X.....X", ".X...X.", "..XXX.."};
constexpr int kSmallMouseRows = 10;
const char *const kSpace[] = {"X.........X", "X.........X", "X.........X", "XXXXXXXXXXX"};
const char *const kUp[] = {"...X...", "..XXX..", ".XXXXX.", "XXXXXXX", "..XXX..", "..XXX..", "..XXX.."};
const char *const kDown[] = {"..XXX..", "..XXX..", "..XXX..", "XXXXXXX", ".XXXXX.", "..XXX..", "...X..."};
const char *const kLeft[] = {"...X...", "..XX...", ".XXXXXX", "XXXXXXX", ".XXXXXX", "..XX...", "...X..."};
const char *const kRight[] = {"...X...", "...XX..", "XXXXXX.", "XXXXXXX", "XXXXXX.", "...XX..", "...X..."};

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

Bitmap keyChar(char c, bool small)
{
    const char *found = c != 0 ? std::strchr(kKeyChars, c) : nullptr;
    if (found == nullptr)
    {
        return {nullptr, 0};
    }
    const size_t index = static_cast<size_t>(found - kKeyChars);
    return small ? Bitmap{kSmall[index], 5} : Bitmap{kLarge[index], 7};
}

Bitmap keyIcon(KeyIcon icon, bool plate)
{
    switch (icon)
    {
    case KeyIcon::MouseLeft:
        return plate ? Bitmap{kSmallMouseLeft, kSmallMouseRows} : Bitmap{kMouseLeft, kMouseRows};
    case KeyIcon::MouseRight:
        return plate ? Bitmap{kSmallMouseRight, kSmallMouseRows} : Bitmap{kMouseRight, kMouseRows};
    case KeyIcon::MouseMiddle:
        return plate ? Bitmap{kSmallMouseMiddle, kSmallMouseRows} : Bitmap{kMouseMiddle, kMouseRows};
    case KeyIcon::Space:
        return {kSpace, 4};
    case KeyIcon::Up:
        return {kUp, 7};
    case KeyIcon::Down:
        return {kDown, 7};
    case KeyIcon::Left:
        return {kLeft, 7};
    case KeyIcon::Right:
        return {kRight, 7};
    case KeyIcon::None:
        break;
    }
    return {nullptr, 0};
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
