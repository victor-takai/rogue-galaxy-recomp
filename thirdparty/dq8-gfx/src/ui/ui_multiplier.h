#pragma once

#include <cstddef>
#include <cstdio>

namespace dq8::ui {

// A GameOption's `format` for a number in hundredths shown as a multiplier
// (Rogue Galaxy: added).
inline constexpr const char *kMultiplierFormat = "multiplier";

// Hundredths as kMultiplierFormat shows them: 100 as "1.0x", 125 as "1.25x"
// (Rogue Galaxy: added; the game's Settings screen writes its numbers with
// it too).
inline void multiplierText(int hundredths, char *out, std::size_t size) {
    const int whole = hundredths / 100, rest = hundredths % 100;
    if (rest % 10 == 0)
        std::snprintf(out, size, "%d.%dx", whole, rest / 10);
    else
        std::snprintf(out, size, "%d.%02dx", whole, rest);
}

} // namespace dq8::ui
