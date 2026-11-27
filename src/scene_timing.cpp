#include "scene_timing.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "ps2_runtime.h"
#include "return_hook.h"
#include "loading_screen.h"
#include "step_fixes.h"

namespace rg::timing
{
namespace
{

constexpr uint32_t kGameSceneTimeScaleUpdate = 0x001CEBE0u;
// GameScene fields the update writes (RGDecomp T1cebe0).
constexpr uint32_t kStepCount = 0x5E04u; // s32: logic steps this frame
constexpr uint32_t kBaseScale = 0x5E08u; // f32: the game's slow-motion knob (GameSceneSetTimeScale)
constexpr uint32_t kTimeScale = 0x5E0Cu; // f32: time scale this frame
constexpr uint32_t kLocked = 0x5E14u;    // u8: the game's overlay mode holds the scale
// GameSceneGetTimeStep (0x1CEBA0) is scale x base, and the only way the game
// reads them. The scene is static; GetCurrentScene (0x1F5BE0) reads its
// pointer off gp, MoviePlayModal (0x267C60) another copy.
constexpr int32_t kSceneOffGp = -0x37D8;
constexpr int32_t kMovieSceneOffGp = -0x31F8;
// The draw context (GetDrawCtx, 0x15DE80, reads it off gp), its environment
// at +0x270 with the vsync interval the flip waits for at +0x24
// (DrawEnvSetVsyncInterval, 0x144140), and the last frame's time in vblanks
// at +0xF10, which GfxEndFrame clamps to between one and two intervals.
constexpr int32_t kDrawCtxOffGp = -0x40CC;
constexpr uint32_t kDrawEnv = 0x270u;
constexpr uint32_t kVsyncInterval = 0x24u;
constexpr uint32_t kFrameTime = 0xF10u;
// DrawEnvGetVsyncInterval (0x144130), where the game reads the interval for
// the vblanks a logic frame spans rather than for the wait: PlayTimeStep
// (0x2F7570: tick = interval x step, the play clock and the per-second
// event triggers), MinimapUpdate (0x3295A0: halves the step at interval 1,
// the game's own 60 Hz menus) and TextEntrySetupState (0x4359F0). The
// return addresses of those calls. PauseStepUpdate (the quit-to-title
// prompt, one update per vblank of the interval) and the insect cage's own
// halving are right with the wait's value.
constexpr uint32_t kDrawEnvGetVsyncInterval = 0x00144130u;
constexpr uint32_t kIntervalLogicReaders[] = {0x002F75BCu, 0x0032963Cu, 0x00435A28u};
// A movie plays between these two; the game's own flag (MovieIsStarted,
// 0x1D5640) stays set until the next movie opens.
constexpr uint32_t kMoviePlayerStart = 0x001D5B50u;
constexpr uint32_t kMoviePlayerShutdown = 0x001D56C0u;
// The modal loop a movie plays in: its own GfxEndFrame and PadUpdate, never
// the time-scale update. It reads the step once on entry for its sound
// fades (0x267CB0), and EventCueStep's fades read it every frame.
constexpr uint32_t kMoviePlayModal = 0x00267C60u;
// GameData (0x4B8B30) + 0xC: the block PlayTimeTick advances; the play time
// is a double at its +0x8, in vblanks.
constexpr uint32_t kPlayTimeBlock = 0x004B8B3Cu;
constexpr uint32_t kPlayTime = 0x8u;
// DrawEnvSetVsyncInterval (0x144140, a1 = interval): the game's own writes,
// 2 at boot (SystemHeapsInit) and from SceneOverlayModeOff, 1 from
// SceneOverlayModeOn (its 60 Hz menus).
constexpr uint32_t kDrawEnvSetVsyncInterval = 0x00144140u;
// GetGameMode's word. The modes that lock the scale while the game's
// interval is still 2 (TransporterWarp 11, CharaInteract 12, TreasureBox
// 13: whole steps at 30 Hz for a scripted sequence) are field modes, and
// get the host's half steps like the unlocked field; a lock in any other
// mode (the title's own overlay, in GameStart) is the game's.
constexpr uint32_t kGameMode = 0x004B51A0u;
constexpr uint32_t kFieldModes[] = {5u, 6u, 7u, 9u, 10u, 11u, 12u, 13u};
// The Factory (FactoryModeUpdate, 0x27DB40; FACTORY.BIN) runs at the
// game's interval of 2 with the scale unlocked, so it would take the
// host's half steps, but its tasks count frames, all of them: the floor's
// step (TaskStepFactory, 0x367960: a working machine's pulse by a count of
// its calls, its sparks one chance in 60 a call, a part's drop 15 calls,
// the pad's repeats), its menus (TaskStepMenuFactory, 0x204880) and its
// camera (CameraModeFactoryUpdate, 0x3679C0: the floor cursor moved by the
// stick a call, the camera eased toward it and shaken by fixed amounts a
// call). At 60 fps the machines, the menus and the cursor went at twice
// the speed (the user, 2026-11-27). The whole mode flips at the console's
// two vblanks instead, as the movies do, with whole steps (while the scale
// is ours: the game only enters it after SceneOverlayModeOff).
constexpr uint32_t kFactoryMode = 16u;
// The pad PadUpdate (0x1843A0) keeps: buttons held now at +0x4, held the
// frame before at +0x5C, input disabled at +0x24C (PadIsTriggered, 0x1839F0).
constexpr uint32_t kPad = 0x004B2570u;
constexpr uint32_t kPadHeld = 0x4u;
constexpr uint32_t kPadHeldBefore = 0x5Cu;
constexpr uint32_t kPadDisabled = 0x24Cu;

PS2Runtime::RecompiledFunction g_original = nullptr;
PS2Runtime::RecompiledFunction g_movieStartOriginal = nullptr;
PS2Runtime::RecompiledFunction g_movieShutdownOriginal = nullptr;
PS2Runtime::RecompiledFunction g_movieModalOriginal = nullptr;
PS2Runtime::RecompiledFunction g_intervalOriginal = nullptr;
bool g_moviePlaying = false;
bool g_inMovieModal = false;
bool g_sixty = false;     // RG_FPS=60
// The menu's switch, for install() (written by the menu's thread).
std::atomic<bool> g_sixtyPreferred{true};
bool g_adaptive = false;  // RG_TIME_SCALE=game
unsigned g_stepFixes = ~0u; // RG_STEP_FIXES: which frame-counted systems take the step
bool g_trace = false;     // RG_TIME_TRACE=1
// What beforeFlip set this frame's flip to wait for, in vblanks, and whether
// a movie or the loading screen asked for the 2 (the Factory's comes from
// g_factory, decided at each update).
uint32_t g_interval = 2u;
bool g_movieFlip = true;
// The vblanks a logic frame spans for the readers above: 2 while the
// scene's scale is ours (half steps at interval 1), 0 to leave the field's
// value alone (the game's overlays, or the 30 fps mode).
uint32_t g_logicInterval = 0u;
uint32_t g_intervalRewrites = 0u;
PS2Runtime::RecompiledFunction g_setIntervalOriginal = nullptr;
// The interval the game itself last set (the hook, or a write seen at the
// flip): 2 outside its own overlays.
uint32_t g_gameInterval = 2u;
// The scale written after this frame's update, and whether it was ours.
float g_lastScale = 1.0f;
bool g_ours = false;
// The scale is ours and the game is in the Factory: this frame flips at two
// vblanks (kFactoryMode). Honoured only by a flip with an update before it:
// the title, where a quit from the Factory's pause leads, runs no update.
bool g_factory = false;
uint32_t g_flipUpdateCount = 0u;
uint32_t g_updateCount = 0u;
// The systems that count frames rather than step: whole steps accumulated
// from the host's, decided once per update by the first asker.
float g_logicAccumulator = 0.0f;
uint32_t g_logicDecided = ~0u;
bool g_logicDue = false;
uint32_t g_logicRuns = 0u, g_logicSkips = 0u, g_logicEarly = 0u;

template <typename T>
bool readGuest(const uint8_t *rdram, uint32_t address, T &out)
{
    const uint32_t physical = address & 0x1FFFFFFFu;
    if (physical + sizeof(T) > PS2_RAM_SIZE)
    {
        return false;
    }
    std::memcpy(&out, rdram + physical, sizeof(T));
    return true;
}

template <typename T>
void writeGuest(uint8_t *rdram, uint32_t address, T value)
{
    const uint32_t physical = address & 0x1FFFFFFFu;
    if (physical + sizeof(T) <= PS2_RAM_SIZE)
    {
        std::memcpy(rdram + physical, &value, sizeof(T));
    }
}

void trace(const uint8_t *rdram, float frameTime, float scale)
{
    using Clock = std::chrono::steady_clock;
    static uint32_t count = 0u;
    static float low = 0.0f, high = 0.0f, sum = 0.0f;
    static Clock::time_point wallStart{};
    static double clockStart = 0.0;
    uint32_t block = 0u;
    double playTime = 0.0;
    const bool haveClock =
        readGuest(rdram, kPlayTimeBlock, block) && block != 0u && readGuest(rdram, block + kPlayTime, playTime);
    if (count == 0u)
    {
        low = high = frameTime;
        wallStart = Clock::now();
        clockStart = haveClock ? playTime : 0.0;
    }
    low = std::min(low, frameTime);
    high = std::max(high, frameTime);
    sum += frameTime;
    if (++count == 300u)
    {
        const double wall = std::chrono::duration<double>(Clock::now() - wallStart).count();
        std::printf("[timing] 300 updates: frame time %.2f..%.2f vblanks, mean %.2f; interval %u (game %u), scale "
                    "%.2f; play clock +%.0f vblanks in %.2f s; interval rewrites %u; logic frames %u run, %u "
                    "skipped, %u early\n",
                    low, high, sum / 300.0f, g_interval, g_gameInterval, scale,
                    haveClock ? playTime - clockStart : 0.0, wall, g_intervalRewrites, g_logicRuns, g_logicSkips,
                    g_logicEarly);
        count = 0u;
        sum = 0.0f;
        g_intervalRewrites = 0u;
        g_logicRuns = g_logicSkips = g_logicEarly = 0u;
        rg::steps::trace();
    }
}

std::atomic<bool> g_reportRequested{false};
uint32_t g_reportedRuns = 0u, g_reportedSkips = 0u, g_reportedEarly = 0u;

void reportState(const uint8_t *rdram, uint32_t scene, float scale)
{
    if (!g_reportRequested.exchange(false, std::memory_order_relaxed))
    {
        return;
    }
    uint32_t mode = 0u;
    uint8_t locked = 0u;
    readGuest(rdram, kGameMode, mode);
    readGuest(rdram, scene + kLocked, locked);
    std::printf("[timing] state: game mode %u, interval %u (game %u), locked %u, %s step %.2f; logic frames %u run, "
                "%u skipped, %u early since the last report%s\n",
                mode, g_interval, g_gameInterval, locked, g_ours ? "our" : "the game's", scale,
                g_logicRuns - g_reportedRuns, g_logicSkips - g_reportedSkips, g_logicEarly - g_reportedEarly,
                g_moviePlaying || g_inMovieModal ? ", movie" : g_factory ? ", factory" : "");
    g_reportedRuns = g_logicRuns;
    g_reportedSkips = g_logicSkips;
    g_reportedEarly = g_logicEarly;
}

bool fieldMode(const uint8_t *rdram)
{
    uint32_t mode = 0u;
    if (!readGuest(rdram, kGameMode, mode))
    {
        return false;
    }
    for (const uint32_t field : kFieldModes)
    {
        if (mode == field)
        {
            return true;
        }
    }
    return false;
}

// What the update itself leaves when the frame was on time, or at 60 fps
// the half step.
void afterUpdate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *, uint32_t scene)
{
    float scale = 1.0f;
    g_logicInterval = 0u;
    g_ours = false;
    // Set below once the scale is ours: under the game's own overlay mode
    // (its UI made for 60 Hz) the Factory's frames are the game's too.
    g_factory = false;
    ++g_updateCount;
    if (g_sixty)
    {
        // The game's own overlays (SceneOverlayModeOn: interval 1 with the
        // scale held, its menus) run at 60 Hz by design: theirs. A lock
        // while the game's interval is still 2 is a field mode that wants
        // whole steps at 30 Hz for a scripted sequence (chest, talk, warp):
        // ours, at half steps like the unlocked field. A lock anywhere else
        // (the title's own overlay) is left alone as well.
        uint8_t locked = 0u;
        const bool haveLock = readGuest(rdram, scene + kLocked, locked);
        if (g_trace)
        {
            static uint32_t shown = 0u;
            if (shown < 8u)
            {
                ++shown;
                std::printf("[timing] update: scene 0x%08X locked %u (read %d)\n", scene, locked, haveLock ? 1 : 0);
            }
        }
        if (g_gameInterval != 2u || (haveLock && locked != 0u && !fieldMode(rdram)))
        {
            reportState(rdram, scene, 1.0f);
            return;
        }
        g_ours = true;
        g_logicInterval = 2u;
        // Decided here, where the step is, for this frame's flip too: the
        // update runs before the frame's mode dispatch, which may enter or
        // leave the Factory, and the flip decided from the mode after it
        // would give the Factory's first frame a half step in two vblanks.
        uint32_t mode = 0u;
        g_factory = stepFix(StepFix::Factory) && readGuest(rdram, kGameMode, mode) && mode == kFactoryMode;
        // A frame flipped at two vblanks (a movie, the loading screen, the
        // Factory) keeps the 30 fps step; the last flip's 2 counts only
        // for a movie or the loading screen, the Factory's own being
        // decided just above (the frame out of it flips at one).
        const float base = g_movieFlip || g_factory ? 1.0f : 0.5f;
        scale = base;
        uint32_t drawCtx = 0u;
        float frameTime = 0.0f;
        const bool measured = readGuest(rdram, getRegU32(ctx, 28) + kDrawCtxOffGp, drawCtx) &&
                              readGuest(rdram, drawCtx + kFrameTime, frameTime) && std::isfinite(frameTime);
        if (g_adaptive && measured)
        {
            scale = std::clamp(frameTime * 0.5f, base, 2.0f);
        }
        if (g_trace)
        {
            trace(rdram, measured ? frameTime : 0.0f, scale);
        }
    }
    const int32_t one = 1;
    writeGuest(rdram, scene + kStepCount, one);
    writeGuest(rdram, scene + kTimeScale, scale);
    g_lastScale = scale;
    reportState(rdram, scene, scale);
}

void timeScaleUpdate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    // After the update has really returned: a safe point inside it would
    // otherwise let it finish, and store its own scale, after these writes.
    rg::hooks::callThen(g_original, rdram, ctx, runtime, &afterUpdate, getRegU32(ctx, 4));
}

// The scene's per-frame scale, through both pointers the game reads it by.
void writeScale(uint8_t *rdram, uint32_t gp, float scale)
{
    uint32_t scene = 0u, movieScene = 0u;
    readGuest(rdram, gp + kSceneOffGp, scene);
    readGuest(rdram, gp + kMovieSceneOffGp, movieScene);
    if (scene != 0u)
    {
        writeGuest(rdram, scene + kTimeScale, scale);
    }
    if (movieScene != 0u && movieScene != scene)
    {
        writeGuest(rdram, movieScene + kTimeScale, scale);
    }
}

void movieStart(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    g_moviePlaying = true;
    if (g_sixty && stepFix(StepFix::Movie))
    {
        // Nothing runs the update while a movie plays, and its frames flip
        // every two vblanks: whole steps, or the sound fades crawl.
        writeScale(rdram, getRegU32(ctx, 28), 1.0f);
    }
    g_movieStartOriginal(rdram, ctx, runtime);
}

void movieShutdown(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    // Inside the modal loop the flag holds until the loop returns: its
    // fade-out after the shutdown flips at the movie's rate too.
    if (!g_inMovieModal)
    {
        g_moviePlaying = false;
    }
    g_movieShutdownOriginal(rdram, ctx, runtime);
}

void afterMovieModal(uint8_t *, R5900Context *, PS2Runtime *, uint32_t)
{
    g_inMovieModal = false;
    g_moviePlaying = false;
}

void movieModal(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    g_inMovieModal = true;
    g_moviePlaying = true;
    writeScale(rdram, getRegU32(ctx, 28), 1.0f);
    rg::hooks::callThen(g_movieModalOriginal, rdram, ctx, runtime, &afterMovieModal, 0u);
}

void vsyncInterval(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t returnPc = getRegU32(ctx, 31);
    g_intervalOriginal(rdram, ctx, runtime);
    if (g_logicInterval == 0u)
    {
        return;
    }
    for (uint32_t reader : kIntervalLogicReaders)
    {
        if (returnPc == reader)
        {
            setReturnU32(ctx, g_logicInterval);
            ++g_intervalRewrites;
            return;
        }
    }
}

// The game's own interval writes: what it believes the flip waits for.
void setVsyncInterval(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t interval = getRegU32(ctx, 5);
    if (interval == 1u || interval == 2u)
    {
        g_gameInterval = interval;
    }
    g_setIntervalOriginal(rdram, ctx, runtime);
}

// A button went down at this frame's PadUpdate.
bool padEdge(const uint8_t *rdram)
{
    uint32_t held = 0u, before = 0u, disabled = 0u;
    if (!readGuest(rdram, kPad + kPadHeld, held) || !readGuest(rdram, kPad + kPadHeldBefore, before) ||
        !readGuest(rdram, kPad + kPadDisabled, disabled) || disabled != 0u)
    {
        return false;
    }
    return (held & ~before) != 0u;
}

} // namespace

bool hookFunction(PS2Runtime &runtime, uint32_t address, PS2Runtime::RecompiledFunction replacement,
                  PS2Runtime::RecompiledFunction &original)
{
    original = runtime.lookupFunction(address);
    if (!original || !runtime.replaceFunction(address, replacement))
    {
        std::fprintf(stderr, "[rg] 0x%08X is not in the function table\n", address);
        original = nullptr;
        return false;
    }
    return true;
}

bool sixtyFps()
{
    return g_sixty;
}

void setSixtyFpsPreferred(bool on)
{
    g_sixtyPreferred.store(on, std::memory_order_relaxed);
}

bool stepFix(StepFix which)
{
    return (g_stepFixes & static_cast<unsigned>(which)) != 0u;
}

bool stepFixes()
{
    return g_stepFixes != 0u;
}

unsigned parseStepFixes(const char *value)
{
    if (!value || !*value || std::strcmp(value, "1") == 0 || std::strcmp(value, "all") == 0)
    {
        return ~0u;
    }
    if (std::strcmp(value, "0") == 0 || std::strcmp(value, "none") == 0)
    {
        return 0u;
    }
    struct Name
    {
        const char *text;
        StepFix fix;
    };
    static const Name names[] = {{"clock", StepFix::Clock},
                                 {"movie", StepFix::Movie},
                                 {"npc", StepFix::Npc},
                                 {"judge", StepFix::Judge},
                                 {"gravity", StepFix::Gravity},
                                 {"event", StepFix::Event},
                                 {"menu", StepFix::Menu},
                                 {"ai", StepFix::Ai},
                                 {"texture", StepFix::Texture},
                                 {"camera", StepFix::Camera},
                                 {"factory", StepFix::Factory}};
    unsigned fixes = 0u;
    const char *p = value;
    while (*p)
    {
        const char *end = std::strchr(p, ',');
        const size_t length = end ? static_cast<size_t>(end - p) : std::strlen(p);
        bool known = false;
        for (const Name &name : names)
        {
            if (length == std::strlen(name.text) && std::strncmp(p, name.text, length) == 0)
            {
                fixes |= static_cast<unsigned>(name.fix);
                known = true;
            }
        }
        if (!known && length > 0u)
        {
            std::fprintf(stderr,
                         "[rg] RG_STEP_FIXES: unknown name \"%.*s\" (clock, movie, npc, judge, gravity, event, menu, ai, texture, camera, factory)\n",
                         static_cast<int>(length), p);
        }
        p = end ? end + 1 : p + length;
    }
    return fixes;
}

bool traceEnabled()
{
    return g_trace;
}

float sceneStep(const uint8_t *rdram, const R5900Context *ctx)
{
    uint32_t scene = 0u;
    float scale = 1.0f, base = 1.0f;
    if (!readGuest(rdram, getRegU32(ctx, 28) + kSceneOffGp, scene) || scene == 0u ||
        !readGuest(rdram, scene + kTimeScale, scale) || !readGuest(rdram, scene + kBaseScale, base))
    {
        return 1.0f;
    }
    const float step = scale * base;
    return std::isfinite(step) && step > 0.0f ? step : 1.0f;
}

float sceneScale(const uint8_t *rdram, const R5900Context *ctx)
{
    uint32_t scene = 0u;
    float scale = 1.0f;
    if (!readGuest(rdram, getRegU32(ctx, 28) + kSceneOffGp, scene) || scene == 0u ||
        !readGuest(rdram, scene + kTimeScale, scale) || !std::isfinite(scale))
    {
        return 1.0f;
    }
    return scale;
}

void setSceneScale(uint8_t *rdram, const R5900Context *ctx, float scale)
{
    writeScale(rdram, getRegU32(ctx, 28), scale);
}

LogicFrame logicFrame(const uint8_t *rdram, const R5900Context *)
{
    if (!g_ours || g_moviePlaying || g_inMovieModal)
    {
        return LogicFrame::AsIs;
    }
    if (g_logicDecided != g_updateCount)
    {
        g_logicDecided = g_updateCount;
        // Capped: a run of slow frames must not turn into a burst later.
        g_logicAccumulator = std::min(g_logicAccumulator + g_lastScale, 2.0f);
        g_logicDue = g_logicAccumulator >= 1.0f;
        if (g_logicDue)
        {
            g_logicAccumulator -= 1.0f;
        }
        else if (padEdge(rdram))
        {
            g_logicDue = true;
            g_logicAccumulator = 0.0f;
            ++g_logicEarly;
        }
        if (g_logicDue)
        {
            ++g_logicRuns;
        }
        else
        {
            ++g_logicSkips;
        }
    }
    return g_logicDue ? LogicFrame::WholeStep : LogicFrame::Skip;
}

void beforeFlip(uint8_t *rdram, R5900Context *ctx)
{
    if (!g_sixty)
    {
        return;
    }
    const uint32_t gp = getRegU32(ctx, 28);
    uint32_t drawCtx = 0u, env = 0u;
    if (!readGuest(rdram, gp + kDrawCtxOffGp, drawCtx) || !readGuest(rdram, drawCtx + kDrawEnv, env))
    {
        return;
    }
    // A movie, the loading screen or the Factory, which count frames: they
    // flip at the console's 30.
    const uint8_t movie = g_moviePlaying || rg::loading::active() ? 1u : 0u;
    const uint8_t factory = g_factory && g_updateCount != g_flipUpdateCount ? 1u : 0u;
    g_flipUpdateCount = g_updateCount;
    g_movieFlip = movie != 0u;
    uint32_t before = 0u;
    readGuest(rdram, env + kVsyncInterval, before);
    // A write of the game's that did not go through DrawEnvSetVsyncInterval
    // (an overlay's own copy of the overlay mode) shows here as a value
    // other than the one left at the last flip.
    if (before != g_interval && (before == 1u || before == 2u))
    {
        g_gameInterval = before;
    }
    g_interval = movie != 0u || factory != 0u ? 2u : 1u;
    if (g_trace)
    {
        static uint32_t shown = 0u;
        static uint8_t lastMovie = 0xFFu, lastFactory = 0xFFu;
        if (shown < 5u || movie != lastMovie || factory != lastFactory)
        {
            ++shown;
            lastMovie = movie;
            lastFactory = factory;
            std::printf("[timing] flip: gp 0x%08X drawctx 0x%08X env 0x%08X interval %u -> %u, movie %u, factory %u\n",
                        gp, drawCtx, env, before, g_interval, movie, factory);
        }
    }
    writeGuest(rdram, env + kVsyncInterval, g_interval);
}

void requestStateReport()
{
    g_reportRequested.store(true, std::memory_order_relaxed);
}

void install(PS2Runtime &runtime)
{
    const char *fps = std::getenv("RG_FPS");
    g_sixty = fps && *fps ? std::strcmp(fps, "60") == 0 : g_sixtyPreferred.load(std::memory_order_relaxed);
    if (!g_sixty)
    {
        std::printf("[rg] 30 fps, the game's own (%s)\n",
                    fps && *fps ? "RG_FPS" : "the menu's 60 frames per second is off");
    }
    const char *mode = std::getenv("RG_TIME_SCALE");
    // At 60 fps the step follows the frame time unless RG_TIME_SCALE=fixed:
    // a host that only reaches 35 frames a second would otherwise play at
    // 60% speed.
    g_adaptive = g_sixty ? !(mode && std::strcmp(mode, "fixed") == 0) : (mode && std::strcmp(mode, "game") == 0);
    g_stepFixes = parseStepFixes(std::getenv("RG_STEP_FIXES"));
    const char *traceMode = std::getenv("RG_TIME_TRACE");
    g_trace = traceMode && *traceMode && *traceMode != '0';
    if (!g_sixty && g_adaptive)
    {
        return;
    }
    if (!hookFunction(runtime, kGameSceneTimeScaleUpdate, &timeScaleUpdate, g_original))
    {
        std::fprintf(stderr, "[rg] the game scales its own steps\n");
        g_sixty = false;
        return;
    }
    if (g_sixty)
    {
        if (!hookFunction(runtime, kMoviePlayerStart, &movieStart, g_movieStartOriginal) ||
            !hookFunction(runtime, kMoviePlayerShutdown, &movieShutdown, g_movieShutdownOriginal))
        {
            std::fprintf(stderr, "[rg] 60 fps: movies will flip every vblank too\n");
        }
        if (stepFix(StepFix::Movie) &&
            !hookFunction(runtime, kMoviePlayModal, &movieModal, g_movieModalOriginal))
        {
            std::fprintf(stderr, "[rg] 60 fps: movies will fade their sound at half speed\n");
        }
        if (stepFix(StepFix::Clock) &&
            !hookFunction(runtime, kDrawEnvGetVsyncInterval, &vsyncInterval, g_intervalOriginal))
        {
            std::fprintf(stderr, "[rg] 60 fps: the play clock will run at half speed\n");
        }
        if (!hookFunction(runtime, kDrawEnvSetVsyncInterval, &setVsyncInterval, g_setIntervalOriginal))
        {
            std::fprintf(stderr, "[rg] 60 fps: the game's own menus may take half steps\n");
        }
        std::printf("[rg] 60 fps: the flip waits one vblank and the scene takes %s; step fixes:%s%s%s%s%s%s%s%s%s%s%s%s\n",
                    g_adaptive ? "steps sized by the frame time, half at full rate" : "half steps",
                    g_stepFixes == 0u ? " none" : "", stepFix(StepFix::Clock) ? " clock" : "",
                    stepFix(StepFix::Movie) ? " movie" : "", stepFix(StepFix::Npc) ? " npc" : "",
                    stepFix(StepFix::Judge) ? " judge" : "", stepFix(StepFix::Gravity) ? " gravity" : "",
                    stepFix(StepFix::Event) ? " event" : "", stepFix(StepFix::Menu) ? " menu" : "",
                    stepFix(StepFix::Ai) ? " ai" : "", stepFix(StepFix::Texture) ? " texture" : "",
                    stepFix(StepFix::Camera) ? " camera" : "", stepFix(StepFix::Factory) ? " factory" : "");
    }
}

} // namespace rg::timing
