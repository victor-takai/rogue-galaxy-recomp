#include "mouse_camera.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#if defined(RG_HAS_SDLGPU)
#include <SDL3/SDL.h>
#endif

#include "Stubs/Pad.h"
#include "runtime/ps2_pad_host.h"

namespace rg::mousecamera
{
namespace
{

// Radians a count at 1.0x: 0.063 degrees.
constexpr float kRadiansPerCount = 0.0011f;
// Motion is kept for a camera while it takes it as often as it has been
// (twice its usual gap between takes, in the game's frames, vsyncs, and at
// least kMinGap), and never longer than kStaleFrames, half a second: a camera
// that stops for longer (a menu of the game's, a scene, a battle's pause)
// gets none of what the mouse did meanwhile, and the usual gap follows the
// frame rate down to 4 frames a second within a few takes.
constexpr uint64_t kStaleFrames = 30u;
constexpr uint64_t kMinGap = 4u;
// How much of a new gap the usual one takes in.
constexpr float kGapFollow = 0.25f;
// Counts are kept in 1/1024ths.
constexpr float kFixed = 1024.0f;
constexpr int kSensitivityMin = 25;
constexpr int kSensitivityMax = 400;

std::atomic<bool> g_enabled{false};
std::atomic<int> g_sensitivity{100};
std::atomic<bool> g_invert{false};

// One axis's counts not taken yet, the game's frame of its last take, and its
// usual gap between takes.
struct Axis
{
    std::atomic<int64_t> moved{0};
    std::atomic<uint64_t> takenAt{0};
    std::atomic<float> usualGap{static_cast<float>(kMinGap)};
};
Axis g_x; // right
Axis g_y; // down, as SDL counts it

// Whether the camera has stopped taking this axis, `now`.
bool stale(const Axis &axis, uint64_t now)
{
    const float usual = axis.usualGap.load(std::memory_order_relaxed);
    const uint64_t allowed = std::clamp(static_cast<uint64_t>(std::lround(2.0f * usual)), kMinGap, kStaleFrames);
    return now - axis.takenAt.load(std::memory_order_relaxed) > allowed;
}

void add(Axis &axis, float counts)
{
    if (stale(axis, ps2PadCurrentGuestFrame()))
    {
        axis.moved.store(0, std::memory_order_relaxed);
    }
    axis.moved.fetch_add(std::llround(counts * kFixed), std::memory_order_relaxed);
}

float take(Axis &axis)
{
    const uint64_t now = ps2PadCurrentGuestFrame();
    const bool old = stale(axis, now);
    const uint64_t gap = now - axis.takenAt.exchange(now, std::memory_order_relaxed);
    if (gap > 0u)
    {
        const float usual = axis.usualGap.load(std::memory_order_relaxed);
        const float latest = static_cast<float>(std::min(gap, kStaleFrames));
        axis.usualGap.store(usual + kGapFollow * (latest - usual), std::memory_order_relaxed);
    }
    const int64_t moved = axis.moved.exchange(0, std::memory_order_relaxed);
    return old ? 0.0f : static_cast<float>(moved) / kFixed;
}

// RG_MOUSE_TEST=DX,DY,FROM,TO: DX and DY counts at each take while the game's
// reads of pad 1 are from FROM to TO (the input script's clock with
// RG_INPUT_CLOCK=pad), for a bench run without a mouse.
struct Test
{
    bool on = false;
    float dx = 0.0f, dy = 0.0f;
    double from = 0.0, to = 0.0;
};

const Test &test()
{
    static const Test parsed = [] {
        Test t;
        if (const char *value = std::getenv("RG_MOUSE_TEST"))
        {
            t.on = std::sscanf(value, "%f,%f,%lf,%lf", &t.dx, &t.dy, &t.from, &t.to) == 4;
            if (t.on)
            {
                std::fprintf(stderr, "[rg] mouse camera test: %g, %g counts a take from pad read %g to %g\n", t.dx,
                             t.dy, t.from, t.to);
            }
        }
        return t;
    }();
    return parsed;
}

float testCounts(float Test::*axis)
{
    const Test &t = test();
    if (!t.on)
    {
        return 0.0f;
    }
    const double reads = ps2_stubs::getPadDebugSnapshot().ports[0][0].readCount;
    return reads >= t.from && reads < t.to ? t.*axis : 0.0f;
}

float scale()
{
    return kRadiansPerCount * static_cast<float>(g_sensitivity.load(std::memory_order_relaxed)) / 100.0f;
}

} // namespace

void install()
{
    if (const char *value = std::getenv("RG_MOUSE_CAMERA"))
    {
        setEnabled(*value && *value != '0');
    }
    // A percentage (25 to 400), or the multiplier the menu shows (0.25 to 4);
    // anything else keeps the menu's.
    if (const char *value = std::getenv("RG_MOUSE_SENSITIVITY"))
    {
        const double number = std::strtod(value, nullptr);
        if (number > 0.0)
        {
            setSensitivity(static_cast<int>(std::lround(number <= 4.0 ? number * 100.0 : number)));
        }
    }
    if (const char *value = std::getenv("RG_MOUSE_INVERT"))
    {
        setInvertVertical(*value && *value != '0');
    }
}

#if defined(RG_HAS_SDLGPU)
void pump(SDL_Window *window, bool letGo)
{
    if (window == nullptr)
    {
        return;
    }
    const bool want = g_enabled.load(std::memory_order_relaxed) && SDL_GetKeyboardFocus() == window && !letGo;
    // Set whenever SDL's state is not what is wanted, a second apart after a
    // failure, which the log says once.
    static uint64_t nextTry = 0u;
    if (SDL_GetWindowRelativeMouseMode(window) != want && SDL_GetTicks() >= nextTry)
    {
        if (!SDL_SetWindowRelativeMouseMode(window, want))
        {
            nextTry = SDL_GetTicks() + 1000u;
            static bool told = false;
            if (!told)
            {
                told = true;
                std::fprintf(stderr, "[rg] mouse camera: the mouse cannot be %s: %s\n",
                             want ? "captured" : "let go", SDL_GetError());
            }
        }
    }
    static bool wasCaptured = false;
    const bool captured = want && SDL_GetWindowRelativeMouseMode(window);
    if (captured && !wasCaptured)
    {
        static bool told = false;
        if (!told)
        {
            told = true;
            std::fprintf(stderr, "[rg] mouse camera: the mouse is in the window (F1 or Alt+Tab lets it go)\n");
        }
    }
    // Read every frame, so that motion from before the capture (the cursor's,
    // in the frame it starts too) never reaches the camera.
    float dx = 0.0f, dy = 0.0f;
    SDL_GetRelativeMouseState(&dx, &dy);
    if (captured && wasCaptured)
    {
        if (dx != 0.0f)
        {
            add(g_x, dx);
        }
        if (dy != 0.0f)
        {
            add(g_y, dy);
        }
    }
    wasCaptured = captured;
}
#else
// Without the SDL window (the software GS) there is no mouse to read.
void pump(SDL_Window *, bool)
{
}
#endif

bool enabled()
{
    return g_enabled.load(std::memory_order_relaxed);
}

float takeTurn()
{
    return (take(g_x) + testCounts(&Test::dx)) * scale();
}

float takeTilt()
{
    const float up = -(take(g_y) + testCounts(&Test::dy)) * scale();
    return g_invert.load(std::memory_order_relaxed) ? -up : up;
}

void setEnabled(bool on)
{
    g_enabled.store(on, std::memory_order_relaxed);
}

void setSensitivity(int percent)
{
    g_sensitivity.store(std::clamp(percent, kSensitivityMin, kSensitivityMax), std::memory_order_relaxed);
}

void setInvertVertical(bool on)
{
    g_invert.store(on, std::memory_order_relaxed);
}

} // namespace rg::mousecamera
