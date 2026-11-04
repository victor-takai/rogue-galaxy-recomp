// What the SDL GPU backend does with a window it owns: how the game picture
// is fitted into it, and the hook that draws an overlay on top.
#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <functional>
#include <vector>

namespace dq8::gfx {

enum class SdlGpuAspect : uint8_t {
    Auto,     // the game's own Screen Size option: 4:3 or 16:9
    Standard, // 4:3, as on a television
    Wide,     // 16:9
    Native,   // square pixels, the framebuffer's own shape
    Stretch,  // fill the window
};

enum class SdlGpuFilter : uint8_t {
    Sharp,   // nearest to a whole multiple, then smooth to the final size
    Smooth,  // bilinear
    Nearest, // nearest neighbour
    Fsr,     // AMD FSR 1 (EASU then RCAS) when enlarging; Smooth otherwise (Rogue Galaxy: added)
};

// What fills the window beside a picture that does not reach its edges
// (Rogue Galaxy: added).
enum class SdlGpuSides : uint8_t {
    Black,
    Blurred, // the picture itself, blurred and darkened, covering the window
    Image,   // the player's image (SdlGpuSideImageLoader), covering the window
};

// The image for SdlGpuSides::Image, as RGBA8 rows: called on the presenting
// thread each time that choice is made; false when there is none, which
// leaves the blurred picture (Rogue Galaxy: added).
using SdlGpuSideImageLoader =
    std::function<bool(uint32_t &width, uint32_t &height, std::vector<uint8_t> &rgba)>;

struct SdlGpuDisplayOptions {
    SdlGpuAspect aspect = SdlGpuAspect::Auto;
    SdlGpuFilter filter = SdlGpuFilter::Sharp;
    // Whole multiples of the picture's height, so scanlines stay even.
    bool integerScale = false;
    SDL_GPUPresentMode presentMode = SDL_GPU_PRESENTMODE_VSYNC;
    // Shows both display circuits at one origin, which leaves DQ8's one-line
    // deflicker blend a no-op. Off is the PS2's own picture.
    bool removeLineBlend = false;
    // GS pixels left off each edge of the picture: the border a game leaves
    // undrawn or half drawn, which a television's overscan hid.
    uint32_t crop = 0u;
    SdlGpuSides sides = SdlGpuSides::Black;
    // FSR 1's sharpening, 0 to 100 (Rogue Galaxy: added): 100 is RCAS at 0
    // stops (the sharpest), 0 at 2 stops.
    int fsrSharpness = 70;
    bool operator==(const SdlGpuDisplayOptions &) const = default;
};

struct SdlGpuRect {
    uint32_t x = 0u, y = 0u, width = 0u, height = 0u;
    bool operator==(const SdlGpuRect &) const = default;
};

// Where the game picture goes in a window of the given size. gameWidescreen
// is the game's own setting for Auto: 1 for 16:9, 0 for 4:3, -1 unknown.
SdlGpuRect sdlGpuDisplayRect(const SdlGpuDisplayOptions &options, uint32_t sourceWidth,
                             uint32_t sourceHeight, uint32_t windowWidth, uint32_t windowHeight,
                             int gameWidescreen);
// The shape of a window a game drawn for a wide screen fills under Auto:
// the window's own, from 4:3 to 32:9.
double sdlGpuWideAspect(uint32_t windowWidth, uint32_t windowHeight);

// The aspect ratio the picture is shown at, before fitting.
double sdlGpuDisplayAspect(const SdlGpuDisplayOptions &options, uint32_t sourceWidth,
                           uint32_t sourceHeight, uint32_t windowWidth, uint32_t windowHeight,
                           int gameWidescreen);

// Drawn over the game in the window. Every call comes from the thread that
// owns the window.
class SdlGpuOverlay {
public:
    virtual ~SdlGpuOverlay() = default;
    // True when the event is the overlay's own; the game does not see it.
    virtual bool handleEvent(const SDL_Event &event) = 0;
    // Records the overlay into `target`, which already holds the game picture.
    virtual void render(SDL_GPUCommandBuffer *commands, SDL_GPUTexture *target, uint32_t width,
                        uint32_t height) = 0;
    // True while the overlay needs frames even when the game shows none.
    virtual bool wantsFrames() = 0;
    // True while the game must read an idle pad.
    virtual bool capturesInput() const = 0;
};

struct SdlGpuCapture {
    std::vector<uint8_t> rgba;
    uint32_t width = 0u;
    uint32_t height = 0u;
};
using SdlGpuCaptureCallback = std::function<void(SdlGpuCapture capture)>;

// Gives a window the project's icon where it has to come from the window, as
// on Linux. Windows and macOS already show the executable's or the bundle's.
void setWindowIcon(SDL_Window *window);

} // namespace dq8::gfx
