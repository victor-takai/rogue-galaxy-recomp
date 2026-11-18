#include "widescreen.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "loading_screen.h"
#include "overlay/overlay_manager.h"
#include "ps2_runtime.h"
#include "scene_timing.h"

namespace rg::widescreen
{
namespace
{

constexpr uint32_t kGetAspectMode = 0x001C6AA0u;
constexpr uint32_t kIsWidescreen = 0x001C6AB0u;
constexpr uint32_t kViewSetAspect = 0x00142F50u;
constexpr uint32_t kViewSetPerspective = 0x001564A0u;
constexpr uint32_t kGsPktSetXyzPixel = 0x0013F110u;

// The wide mode's camera: ViewSetAspect(ctx, f12 9/16) from
// SceneCameraApplyAspect (0x1C6A58), whatever the picture's shape.
constexpr uint32_t kCameraAspectReturn = 0x001C6A60u;
constexpr float kWideAspect = 0.5625f;
// ViewSetPerspective(view, f12 fov, f13 aspect, ...) from CompassHudRender
// (lui v0,0x3f40 at 0x37112C) and DrawModelWithCamera (0x28C070, the
// insect menus' previews).
constexpr uint32_t kCompassPerspectiveReturn = 0x0037114Cu;
constexpr uint32_t kModelPerspectiveReturn = 0x0028C0A0u;
constexpr float kStandardAspect = 0.75f;

// MovieDrawFrame's picture, centred in the 512x448 frame:
// GsPktSetXYZPixel(packet, x, y, z) for its top left (x = offset + s5,
// y = offset + s6, the bars beside and above it) and its bottom right
// (x + s1 - 1, y + s0 - 1, s1 x s0 its size).
constexpr uint32_t kMovieTopLeftReturn = 0x001D5A80u;
constexpr uint32_t kMovieBottomRightReturn = 0x001D5ADCu;
constexpr int kLeftRegister = 21;
constexpr int kTopRegister = 22;
constexpr int kWidthRegister = 17;
constexpr int kHeightRegister = 16;
constexpr int32_t kFrameWidth = 0x200;
constexpr int32_t kFrameHeight = 0x1C0;

// The patch's fifth word, an event's letterbox left open (0x2611D4), is not
// taken: the letterbox's fade also places the text over the scene
// (LetterboxFadeStep moves the map captions by it), and without it an
// event's messages ran off the bottom and the bar's edge stayed drawn at
// the top (2026-10-21).

PS2Runtime::RecompiledFunction g_getAspectMode = nullptr;
PS2Runtime::RecompiledFunction g_isWidescreen = nullptr;
PS2Runtime::RecompiledFunction g_viewSetAspect = nullptr;
PS2Runtime::RecompiledFunction g_viewSetPerspective = nullptr;
PS2Runtime::RecompiledFunction g_gsPktSetXyzPixel = nullptr;
// The picture's width over its height: 16:10, the Steam Deck's screen, the
// only shape the game is shown at.
constexpr float kAspect = 16.0f / 10.0f;

// What the game's views take for the shape: its height over its width.
float projection()
{
    return 1.0f / kAspect;
}

int32_t argument(const R5900Context *ctx, int reg)
{
    return static_cast<int32_t>(getRegU32(ctx, reg));
}

void setArgument(R5900Context *ctx, int reg, int32_t value)
{
    ctx->r[reg] = _mm_set_epi64x(0, static_cast<int64_t>(value));
}

struct Box
{
    int32_t left, top, width, height;
};

// A movie's place in the frame shown wide: the largest box of the shape it
// had on a 4:3 screen. Movies made for 16:9 come 512x336, between bars on
// the 4:3 screen, and fill a 16:9 frame; the 4:3 ones (512x448, as the
// title overlay's are) keep bars at their sides.
Box widePicture(int32_t width, int32_t height)
{
    const double kWide = kAspect;
    const double shape = (4.0 / 3.0) * (double(width) / kFrameWidth) / (double(height) / kFrameHeight);
    int32_t w = kFrameWidth;
    int32_t h = kFrameHeight;
    if (shape >= kWide)
    {
        h = static_cast<int32_t>(std::lround(kFrameHeight * kWide / shape));
    }
    else
    {
        w = static_cast<int32_t>(std::lround(kFrameWidth * shape / kWide));
    }
    return {(kFrameWidth - w) / 2, (kFrameHeight - h) / 2, w, h};
}

void getAspectMode(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    g_getAspectMode(rdram, ctx, runtime);
    if (wide())
    {
        setReturnS32(ctx, 1);
    }
}

void isWidescreen(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    g_isWidescreen(rdram, ctx, runtime);
    if (wide())
    {
        setReturnS32(ctx, 1);
    }
}

void viewSetAspect(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (getRegU32(ctx, 31) == kCameraAspectReturn && ctx->f[12] == kWideAspect && wide())
    {
        ctx->f[12] = projection();
    }
    g_viewSetAspect(rdram, ctx, runtime);
}

void viewSetPerspective(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t ra = getRegU32(ctx, 31);
    if (wide() && (ra == kCompassPerspectiveReturn || ra == kModelPerspectiveReturn) &&
        ctx->f[13] == kStandardAspect)
    {
        ctx->f[13] = projection();
    }
    g_viewSetPerspective(rdram, ctx, runtime);
}

void gsPktSetXyzPixel(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t ra = getRegU32(ctx, 31);
    const int32_t width = argument(ctx, kWidthRegister);
    const int32_t height = argument(ctx, kHeightRegister);
    if ((ra == kMovieTopLeftReturn || ra == kMovieBottomRightReturn) && wide() && width > 0 && height > 0)
    {
        // The frame's offset stays: x and y less the bar, and less the size
        // for the bottom right.
        const bool bottomRight = ra == kMovieBottomRightReturn;
        const Box box = widePicture(width, height);
        const int32_t x = argument(ctx, 5) - argument(ctx, kLeftRegister) - (bottomRight ? width - 1 : 0);
        const int32_t y = argument(ctx, 6) - argument(ctx, kTopRegister) - (bottomRight ? height - 1 : 0);
        setArgument(ctx, 5, x + box.left + (bottomRight ? box.width - 1 : 0));
        setArgument(ctx, 6, y + box.top + (bottomRight ? box.height - 1 : 0));
    }
    g_gsPktSetXyzPixel(rdram, ctx, runtime);
}

} // namespace

void install(PS2Runtime &runtime)
{
    const bool complete =
        rg::timing::hookFunction(runtime, kGetAspectMode, &getAspectMode, g_getAspectMode) &&
        rg::timing::hookFunction(runtime, kIsWidescreen, &isWidescreen, g_isWidescreen) &&
        rg::timing::hookFunction(runtime, kViewSetAspect, &viewSetAspect, g_viewSetAspect) &&
        rg::timing::hookFunction(runtime, kViewSetPerspective, &viewSetPerspective, g_viewSetPerspective) &&
        rg::timing::hookFunction(runtime, kGsPktSetXyzPixel, &gsPktSetXyzPixel, g_gsPktSetXyzPixel);
    if (!complete)
    {
        std::fprintf(stderr, "[rg] widescreen: some hooks are missing, see above\n");
    }
}

bool wide()
{
    return !rg::loading::active() && std::strcmp(rg::overlay::residentName(), "title") != 0;
}

} // namespace rg::widescreen
