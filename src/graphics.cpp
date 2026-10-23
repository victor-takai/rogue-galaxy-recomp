#include "graphics.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>

#include "debug/gs_census.h"
#include "debug/gs_timing.h"
#include "debug/sampler.h"
#include "debug/stall_watch.h"
#include "depth_reads.h"
#include "flip_pacing.h"
#include "loading_screen.h"
#include "opening.h"
#include "scene_timing.h"
#include "texture_pack.h"
#include "vif1_thread.h"
#include "widescreen.h"
#include "ps2_runtime.h"
#include "stick_response.h"
#include "runtime/gs/gs_cpu_backend.h"
#include "runtime/gs/gs_frontend.h"
#include "runtime/gs/gs_threaded_backend.h"

#if defined(RG_HAS_SDLGPU)
#include <SDL3/SDL.h>

#include "gfx/backends/sdlgpu/sdlgpu_backend.h"
#include "runtime/ee_scheduler.h"
#include "runtime/ps2_native_iop.h"
#include "runtime/ps2_pad_host.h"
#include "ui/ui_overlay.h"
#endif

namespace rg::graphics
{
namespace
{

std::string env(const char *name, const char *fallback)
{
    const char *value = std::getenv(name);
    return (value && *value) ? value : fallback;
}

#if defined(RG_HAS_SDLGPU)
bool g_hasMenu = false;

// F9 and F10 presses, counted as SDL queues their key-down events. pump()
// read the keyboard state once a presented frame, and a quick tap between two
// of them went unseen, most of all in a drop, when frames are long: the
// player's marks of 2026-10-20 never reached the log.
std::atomic<unsigned> g_f9Presses{0u};
std::atomic<unsigned> g_f10Presses{0u};
std::atomic<unsigned> g_f7Presses{0u};
// The longest gap between two pump() calls since the stats line last printed
// it. The presenter loop calls pump several times a frame, so this is not the
// frame time (the backend's "longest frame" is) but the longest the main
// thread was held up: by a wait for the GS, a present, a load.
std::atomic<int64_t> g_longestPumpGapNanos{0};

std::string mainStallReport()
{
    const int64_t longest = g_longestPumpGapNanos.exchange(0, std::memory_order_relaxed);
    char text[48];
    std::snprintf(text, sizeof(text), " | main stall %.1f ms", static_cast<double>(longest) / 1e6);
    return text;
}

bool SDLCALL countMarkKeys(void *, SDL_Event *event)
{
    if (event->type == SDL_EVENT_KEY_DOWN && !event->key.repeat)
    {
        if (event->key.scancode == SDL_SCANCODE_F9)
        {
            g_f9Presses.fetch_add(1u, std::memory_order_relaxed);
        }
        else if (event->key.scancode == SDL_SCANCODE_F10)
        {
            g_f10Presses.fetch_add(1u, std::memory_order_relaxed);
        }
        else if (event->key.scancode == SDL_SCANCODE_F7)
        {
            g_f7Presses.fetch_add(1u, std::memory_order_relaxed);
        }
    }
    return true;
}

// Called once per frame by the runtime when the backend owns the window:
// services SDL events (the menu takes its own first) and hands the host pad
// to the runtime.
bool pump(void *userData)
{
    auto *backend = static_cast<dq8::gfx::SdlGpuBackend *>(userData);
    debug::notePresenterLoop();
    static const bool watching = SDL_AddEventWatch(&countMarkKeys, nullptr);
    (void)watching;
    {
        static auto previous = std::chrono::steady_clock::now();
        const auto now = std::chrono::steady_clock::now();
        const int64_t gap = std::chrono::duration_cast<std::chrono::nanoseconds>(now - previous).count();
        previous = now;
        int64_t longest = g_longestPumpGapNanos.load(std::memory_order_relaxed);
        while (gap > longest &&
               !g_longestPumpGapNanos.compare_exchange_weak(longest, gap, std::memory_order_relaxed))
        {
        }
    }
    if (!backend->pumpEvents())
    {
        return false;
    }
    // The shape the picture takes with Widescreen on, for the game's camera:
    // under Auto the window's.
    int windowWidth = 0, windowHeight = 0;
    if (SDL_GetWindowSizeInPixels(backend->window(), &windowWidth, &windowHeight) && windowWidth > 0 &&
        windowHeight > 0)
    {
        rg::widescreen::setAspect(static_cast<float>(dq8::gfx::sdlGpuDisplayAspect(
            backend->displayOptions(), 512u, 448u, static_cast<uint32_t>(windowWidth),
            static_cast<uint32_t>(windowHeight), 1)));
    }
    // The menu has F11 and Alt+Enter; without it, F11 still toggles.
    static bool f11WasDown = false;
    const bool f11Down = !g_hasMenu && SDL_GetKeyboardState(nullptr)[SDL_SCANCODE_F11];
    if (f11Down && !f11WasDown)
    {
        SDL_Window *window = backend->window();
        SDL_SetWindowFullscreen(window, (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) == 0);
    }
    f11WasDown = f11Down;
    // F9 writes "[mark] N s" to the log: pressed when a drop is felt, it
    // puts the moment next to the [gfx] lines of that second.
    static const auto markStart = std::chrono::steady_clock::now();
    for (unsigned n = g_f9Presses.exchange(0u, std::memory_order_relaxed); n > 0u; --n)
    {
        // On stderr with the stats lines, so a mark cannot cut one in two.
        std::fprintf(stderr, "[mark] %.1f s\n",
                     std::chrono::duration<double>(std::chrono::steady_clock::now() - markStart).count());
        // With the profile launcher, the mark also opens a profile window.
        debug::requestProfileWindow();
    }
    // F7 logs the next GS frame with RG_GS_CAPTURE (debug\RogueGalaxy-capture.bat).
    for (unsigned n = g_f7Presses.exchange(0u, std::memory_order_relaxed); n > 0u; --n)
    {
        std::fprintf(stderr, "[mark] F7: frame capture requested\n");
        debug::requestFrameCapture();
        rg::timing::requestStateReport();
    }
    // F10 writes the stall watchdog's report now (the guest threads, their
    // waits, the host stacks), for a freeze that keeps flipping.
    if (g_f10Presses.exchange(0u, std::memory_order_relaxed) != 0u)
    {
        std::fprintf(stderr, "[mark] F10: stall report requested\n");
        debug::requestStallReport();
    }
    uint32_t held = 0u, pressed = 0u, sticks = 0u;
    backend->hostPadState(held, pressed, sticks);
    ps2PadPublishHostState(held, pressed, rg::pad::shapeSticks(sticks));
    return true;
}

// The saved window size, or 80% of the display's height at 4:3, the shape
// the game is shown at, so the picture fills it.
std::pair<uint32_t, uint32_t> initialWindowSize(const dq8::ui::Settings &settings)
{
    if (settings.windowWidth >= 320 && settings.windowHeight >= 240)
    {
        return {static_cast<uint32_t>(settings.windowWidth), static_cast<uint32_t>(settings.windowHeight)};
    }
    SDL_Rect usable{0, 0, 1280, 960};
    SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &usable);
    const uint32_t height = static_cast<uint32_t>(std::max(448.0f, static_cast<float>(usable.h) * 0.8f));
    return {height * 4u / 3u, height};
}

// DQ8-Recompiled's in-game menu over the window: display, sound, controls
// and the frame-rate counter (thirdparty/dq8-gfx/src/ui). What it asks of
// the game goes through HostServices.
std::unique_ptr<dq8::ui::Overlay> createMenu(PS2Runtime &runtime, dq8::gfx::SdlGpuBackend &backend,
                                             dq8::ui::Settings settings, const std::string &settingsPath)
{
    dq8::ui::HostServices host;
    host.version = RG_REVISION;
    // RG_MUTE=1 keeps the game silent whatever the menu's volume says.
    const bool mute = env("RG_MUTE", "0") != "0";
    host.setVolume = [mute](float volume) { ps2_native_iop::setVolume(mute ? 0.0f : volume); };
    // The sound driver runs in step with the audio device, so stopping the
    // device stops the IOP along with the EE's clock.
    host.setPaused = [&runtime](bool paused) {
        runtime.eeScheduler().setHostPaused(paused);
        ps2_native_iop::setPaused(paused);
    };
    host.setSpeed = [&runtime](double speed) { runtime.eeScheduler().setHostSpeed(speed); };
    // The game's flips: frames it finished, not presents, which go on while
    // it is paused.
    host.completedRenders = [] { return guestFlips(); };
    host.guestFrame = [] { return ps2PadCurrentGuestFrame(); };
    // The game's own switches, under Interface > Game.
    host.gameOptions = {
        {"sixty_fps", "60 frames per second",
         "The game runs at 60 frames a second instead of its own 30, its logic still at the PS2's pace; movies "
         "and the loading screen keep 30. Takes effect the next time the game starts.",
         true},
        {"widescreen", "Widescreen",
         "The game draws a wider view, and the HUD over the scene and the movies fit it; with Display > Aspect "
         "ratio on Auto the picture fills the window, whatever its shape, once a game is loaded. Menus and text "
         "are drawn for 4:3 and come out stretched.",
         false},
        {"skip_opening", "Skip the opening",
         "At boot, the Sony and Level-5 logos and the opening movie give way to the title screen at once.", false},
        {"instant_loading_recap", "Show the loading recap at once",
         "When a game loads, the story so far appears whole instead of line by line, and Press X comes as soon as "
         "it has loaded.",
         false},
        {"larger_art", "Larger 2D art",
         "Letters, menus and icons drawn from their larger versions in textures\\hd, where it has them, above 1x.",
         true},
    };
    host.setGameOption = [&backend](const std::string &key, bool on) {
        if (key == "sixty_fps")
            rg::timing::setSixtyFpsPreferred(on);
        else if (key == "widescreen")
            rg::widescreen::setEnabled(on);
        else if (key == "skip_opening")
            rg::opening::setSkip(on);
        else if (key == "instant_loading_recap")
            rg::loading::setInstantRecap(on);
        else if (key == "larger_art")
            backend.setRegionReplacementsEnabled(on);
    };
    auto menu = std::make_unique<dq8::ui::Overlay>(backend, std::move(settings), settingsPath, std::move(host));
    std::string error;
    if (!menu->initialize(backend.window(), backend.windowTextureFormat(), error))
    {
        std::fprintf(stderr, "[rg] in-game menu unavailable: %s\n", error.c_str());
        return nullptr;
    }
    menu->applySettings();
    backend.setOverlay(menu.get());
    return menu;
}
#endif

} // namespace

Setup::Setup(PS2Runtime &runtime)
{
#if defined(RG_HAS_SDLGPU)
    if (env("RG_GS", "sdlgpu") == "sdlgpu")
    {
        const bool noWindow = env("RG_GS_NO_WINDOW", "0") != "0";
        // RG_SHOW_FPS=1 turns the menu's counter on; without a window of
        // ours the backend's readout in the title stands in, which it reads
        // under its DQ8-Recompiled name.
        const bool showFps = env("RG_SHOW_FPS", "0") != "0";
        if (showFps && noWindow)
        {
            _putenv("DQ8_GFX_SHOW_FPS=1");
        }
        std::string error;
        std::unique_ptr<dq8::gfx::SdlGpuBackend> sdl = dq8::gfx::createSdlGpuBackend(error);
        if (!sdl)
        {
            std::fprintf(stderr, "[rg] SDL GPU backend unavailable (%s); using the software rasterizer\n",
                         error.c_str());
            return;
        }
        // The menu's settings, then the variables on top for this run only:
        // they go to the backend, not into the settings the menu saves, so a
        // test run's scale does not become the player's.
        dq8::ui::Settings settings;
        const std::string settingsPath = dq8::ui::defaultSettingsPath();
        if (!dq8::ui::loadSettings(settingsPath, settings))
        {
            std::fprintf(stderr, "[rg] could not read %s; using default settings\n", settingsPath.c_str());
        }
        uint32_t runScale = settings.resolutionScale;
        if (const std::string scale = env("RG_GS_SCALE", ""); !scale.empty())
        {
            runScale = std::clamp(static_cast<uint32_t>(std::strtoul(scale.c_str(), nullptr, 10)), 1u, 8u);
        }
        // The game leaves a border of its frame black or half drawn (a column
        // each side, the top row and the last two), which the television's
        // overscan hid, and at a raised scale its half-pixel effects leave a
        // sliver beyond it: two GS pixels off each edge. RG_CROP=0 shows it.
        settings.display.crop = static_cast<uint32_t>(std::strtoul(env("RG_CROP", "2").c_str(), nullptr, 10));
        dq8::gfx::SdlGpuDisplayOptions runDisplay = settings.display;
        if (const std::string mode = env("RG_PRESENT_MODE", ""); !mode.empty())
        {
            runDisplay.presentMode = mode == "vsync"       ? SDL_GPU_PRESENTMODE_VSYNC
                                     : mode == "immediate" ? SDL_GPU_PRESENTMODE_IMMEDIATE
                                                           : SDL_GPU_PRESENTMODE_MAILBOX;
        }
        if (showFps)
        {
            settings.showFps = true;
        }
        sdl->setResolutionScale(runScale);
        // The VIF1 thread's share of the wall on every stats line, beside the
        // worker's: an F9 mark then says which thread was saturated.
        dq8::gfx::setStatsExtraLine([] { return rg::vif1::busyReport() + mainStallReport(); });
        dq8::gfx::setPresentObserver(&debug::noteFrameTime);
        // Aspect ratio Auto follows the Widescreen option.
        sdl->setGameWidescreenQuery([] { return rg::widescreen::wide() ? 1 : 0; });
        rg::texturePack::install(*sdl);
        if (noWindow)
        {
            std::printf("[rg] GS: sdlgpu, presenting through the runtime window\n");
        }
        else
        {
            const auto [width, height] = initialWindowSize(settings);
            std::string windowError;
            if (sdl->openWindow("Rogue Galaxy Recompiled", width, height, windowError))
            {
                runtime.setExternalPresenter(&pump, sdl.get());
                if (!sdl->supportsPresentMode(settings.display.presentMode))
                {
                    settings.display.presentMode = SDL_GPU_PRESENTMODE_VSYNC;
                }
                if (!sdl->supportsPresentMode(runDisplay.presentMode))
                {
                    runDisplay.presentMode = SDL_GPU_PRESENTMODE_VSYNC;
                }
                m_menu = createMenu(runtime, *sdl, settings, settingsPath);
                g_hasMenu = m_menu != nullptr;
                // After the menu applied the saved options: this run's.
                sdl->setDisplayOptions(runDisplay);
                m_window = sdl.get();
            }
            else
            {
                std::fprintf(stderr, "[rg] could not open the SDL window (%s); presenting through the runtime\n",
                             windowError.c_str());
            }
        }
        std::printf("[rg] GS: sdlgpu (%s), internal scale %ux%s\n", sdl->driverName().c_str(),
                    sdl->resolutionScale(), m_menu ? ", menu on F1" : "");
        m_backend = std::move(sdl);
        m_name = "sdlgpu";
    }
#else
    (void)runtime;
#endif
}

Setup::~Setup()
{
#if defined(RG_HAS_SDLGPU)
    // The runtime still holds the backend (it is destroyed after this), but
    // the presenter has stopped: take the menu off it before the menu goes.
    if (m_window && m_menu)
    {
        m_window->setOverlay(nullptr);
    }
    m_menu.reset();
#endif
}

void Setup::install(PS2Runtime &runtime)
{
    std::unique_ptr<GSRasterBackend> backend = std::move(m_backend);
    if (!backend)
    {
        backend = std::make_unique<GSCpuBackend>();
    }
    // In 3D scenes the software rasterizer was more than half of the game
    // thread's time; behind the queue it runs beside the game instead.
    if (env("RG_GS_WORKER", "1") != "0")
    {
        // The queue counts an upload's bytes in full, and the game uploads
        // about 4 MB of textures a frame: at the framework's 4 MiB the VIF1
        // thread spent a sixth of its time waiting for the worker to drain.
        constexpr size_t kQueueBytes = 16u * 1024u * 1024u;
        backend = std::make_unique<GSThreadedBackend>(debug::timeGsBackend(std::move(backend), "worker"),
                                                      kQueueBytes);
    }
    backend = paceOnFlips(serveDepthReadsLocally(
        debug::censusGsBackend(debug::timeGsBackend(std::move(backend), "frontend"))));
    runtime.gs().setRasterBackend(std::move(backend));
    std::printf("[rg] GS backend: %s%s\n", m_name, env("RG_GS_WORKER", "1") != "0" ? " on its own thread" : "");
}

} // namespace rg::graphics
