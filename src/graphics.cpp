#include "graphics.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>

#include "debug/gs_census.h"
#include "debug/gs_timing.h"
#include "debug/sampler.h"
#include "debug/stall_watch.h"
#include "depth_reads.h"
#include "flip_pacing.h"
#include "battle_rates.h"
#include "button_glyphs.h"
#include "update_check.h"
#include "camera_view.h"
#include "dev_tools.h"
#include "draw_distance.h"
#include "free_camera.h"
#include "instant_text.h"
#include "voice_language.h"
#include "map_chests.h"
#include "native_renderer.h"
#include "loading_screen.h"
#include "opening.h"
#include "quick_scenes.h"
#include "scene_timing.h"
#include "settings_pages.h"
#include "shop_prices.h"
#include "vif1_thread.h"
#include "hud_size.h"
#include "inventory.h"
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

#include <cctype>
#include <filesystem>
#include <vector>

// stb_image, as the runtime's raylib builds it.
extern "C" unsigned char *stbi_load(const char *filename, int *x, int *y, int *comp, int req_comp);
extern "C" void stbi_image_free(void *retval_from_stbi_load);
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

// A Steam Deck: Steam sets SteamDeck=1 in Game Mode (natively and under
// Proton); desktop mode does not, but the machine says what it is.
bool onSteamDeck()
{
    if (env("SteamDeck", "0") == "1")
    {
        return true;
    }
#if defined(__linux__)
    std::FILE *file = std::fopen("/sys/class/dmi/id/product_name", "r");
    if (file)
    {
        char name[64] = {};
        std::fgets(name, sizeof name, file);
        std::fclose(file);
        return std::strncmp(name, "Jupiter", 7) == 0 || std::strncmp(name, "Galileo", 7) == 0;
    }
#endif
    return false;
}

#if defined(RG_HAS_SDLGPU)
bool g_hasMenu = false;
dq8::ui::Overlay *g_menu = nullptr;
PS2Runtime *g_runtime = nullptr;

// The game's speed: the menu's (fast-forward, 0 unpaced) times a quick
// scene's (src/quick_scenes.h); the main thread's alone.
double g_menuSpeed = 1.0;
double g_sceneSpeed = 1.0;

void applySpeed()
{
    if (g_runtime)
    {
        g_runtime->eeScheduler().setHostSpeed(g_menuSpeed > 0.0 ? g_menuSpeed * g_sceneSpeed : 0.0);
    }
}

void followQuickScenes()
{
    if (!g_runtime)
    {
        return;
    }
    const double scene = rg::quickscenes::speed(g_runtime->memory().getRDRAM());
    if (scene != g_sceneSpeed)
    {
        g_sceneSpeed = scene;
        applySpeed();
    }
}

// The game's text shows the buttons of the controller driving it (the menu's
// "Controller's buttons").
std::atomic<bool> g_controllerButtons{true};

// What a controller calls the button a PlayStation button is bound to, as a
// picture the game's text can show: an Xbox pad's (or a pad SDL does not
// know, taken as one); a PlayStation or Nintendo pad keeps the game's.
rg::buttons::Glyph glyphFor(const dq8::gfx::PadBinding &binding, SDL_GamepadType type)
{
    using rg::buttons::Glyph;
    if (binding.kind == dq8::gfx::PadBinding::Kind::Axis)
    {
        if (binding.code == SDL_GAMEPAD_AXIS_LEFT_TRIGGER)
            return Glyph::LT;
        if (binding.code == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)
            return Glyph::RT;
        return Glyph::PlayStation;
    }
    if (binding.kind != dq8::gfx::PadBinding::Kind::Button)
        return Glyph::PlayStation;
    const auto button = static_cast<SDL_GamepadButton>(binding.code);
    switch (button)
    {
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER:
        return Glyph::LB;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER:
        return Glyph::RB;
    case SDL_GAMEPAD_BUTTON_SOUTH:
        return Glyph::A;
    case SDL_GAMEPAD_BUTTON_EAST:
        return Glyph::B;
    case SDL_GAMEPAD_BUTTON_WEST:
        return Glyph::X;
    case SDL_GAMEPAD_BUTTON_NORTH:
        return Glyph::Y;
    default:
        return Glyph::PlayStation;
    }
}

bool xboxLike(SDL_GamepadType type)
{
    switch (type)
    {
    case SDL_GAMEPAD_TYPE_PS3:
    case SDL_GAMEPAD_TYPE_PS4:
    case SDL_GAMEPAD_TYPE_PS5:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
        return false;
    default:
        return true;
    }
}

// Twice a second, the pictures for Cross, Circle, Square, Triangle, L1, R1,
// L2 and R2: each one's first controller binding as the controller driving
// the game names it, the PlayStation's with the keyboard alone.
void updateButtonGlyphs(dq8::gfx::SdlGpuBackend &backend)
{
    static auto last = std::chrono::steady_clock::time_point{};
    const auto now = std::chrono::steady_clock::now();
    if (now - last < std::chrono::milliseconds(500))
        return;
    last = now;
    rg::buttons::Glyphs glyphs{};
    if (g_controllerButtons.load(std::memory_order_relaxed))
    {
        dq8::gfx::SdlPadInput &pad = backend.padInput();
        bool active = false;
        SDL_GamepadType type = SDL_GAMEPAD_TYPE_UNKNOWN;
        for (const dq8::gfx::ControllerInfo &controller : pad.controllers())
        {
            if (controller.active)
            {
                active = true;
                type = controller.type;
                break;
            }
        }
        if (active && xboxLike(type))
        {
            using dq8::gfx::PadInput;
            constexpr PadInput kInputs[rg::buttons::kButtons] = {PadInput::Cross, PadInput::Circle, PadInput::Square,
                                                                 PadInput::Triangle, PadInput::L1, PadInput::R1,
                                                                 PadInput::L2, PadInput::R2};
            for (size_t i = 0; i < rg::buttons::kButtons; ++i)
            {
                for (const dq8::gfx::PadBinding &binding : pad.config().slots(kInputs[i], false))
                {
                    if (binding.bound())
                    {
                        glyphs[i] = glyphFor(binding, type);
                        break;
                    }
                }
            }
        }
    }
    rg::buttons::setGlyphs(glyphs);
}

// What the menu's game options switch, and the game's Settings screen's
// pages of them too.
void applyGameOption(const std::string &key, bool on)
{
    rg::settingspages::noteOption(key, on);
    if (key == "sixty_fps")
        rg::timing::setSixtyFpsPreferred(on);
    else if (key == "widescreen")
        rg::widescreen::setEnabled(on);
    else if (key == "controller_buttons")
        g_controllerButtons.store(on, std::memory_order_relaxed);
    else if (key == "check_updates")
        rg::updates::setEnabled(on);
    else if (key == "skip_opening")
        rg::opening::setSkip(on);
    else if (key == "instant_loading_recap")
        rg::loading::setInstantRecap(on);
    else if (key == "instant_text")
        rg::instanttext::setEnabled(on);
    else if (key == "japanese_voices")
        rg::voicelanguage::setEnabled(on);
    else if (key == "full_bag_to_storage")
        rg::inventory::setFullBagToStorage(on);
    else if (key == "stack_limit_99")
        rg::inventory::setStackLimit99(on);
    else if (key == "bigger_bag")
        rg::inventory::setBiggerBag(on);
    else if (key == "map_chests")
        rg::mapchests::setEnabled(on);
    else if (key == "dev_tools")
        rg::devtools::setEnabled(on);
    else if (key == "native_renderer")
        rg::nativerenderer::setPreferred(on);
    else if (key == "free_camera")
        rg::freecamera::setEnabled(on);
    else if (key == "faster_camera")
        rg::cameraview::setFasterTurn(on);
    else if (key == "fast_transporter")
        rg::quickscenes::setEnabled(rg::quickscenes::Scene::Transporter, on);
    else if (key == "fast_revelation")
        rg::quickscenes::setEnabled(rg::quickscenes::Scene::Revelation, on);
}

void applyGameNumber(const std::string &key, int value)
{
    rg::settingspages::noteNumber(key, value);
    if (key == "field_of_view")
        rg::cameraview::setFieldOfView(value);
    else if (key == "camera_distance")
        rg::cameraview::setDistance(value);
    else if (key == "draw_distance")
        rg::drawdistance::setPercent(value);
    else if (key == "minimap_size")
        rg::hudsize::setMinimapSize(value);
    else if (key == "compass_size")
        rg::hudsize::setCompassSize(value);
    else if (key == "encounter_rate")
        rg::battlerates::setEncounterRate(value);
    else if (key == "battle_experience")
        rg::battlerates::setExperience(value);
    else if (key == "battle_money")
        rg::battlerates::setMoney(value);
    else if (key == "item_drops")
        rg::battlerates::setItemDrops(value);
    else if (key == "weapon_growth")
        rg::battlerates::setWeaponGrowth(value);
    else if (key == "shop_prices")
        rg::shopprices::setPercent(value);
    else if (key == "action_gauge")
        rg::battlerates::setActionGauge(value);
}

// F9 and F10 presses, counted as SDL queues their key-down events. pump()
// read the keyboard state once a presented frame, and a quick tap between two
// of them went unseen, most of all in a drop, when frames are long: the
// player's marks of 2026-10-20 never reached the log.
std::atomic<unsigned> g_f9Presses{0u};
std::atomic<unsigned> g_f10Presses{0u};
std::atomic<unsigned> g_f7Presses{0u};
// F6 turns the free camera on or off.
std::atomic<unsigned> g_f6Presses{0u};
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
        else if (event->key.scancode == SDL_SCANCODE_F6 && !(g_menu && g_menu->capturingBinding()))
        {
            // Not while the menu waits for a key to bind, which F6 is not
            // (ui_overlay.cpp's reservedKey).
            g_f6Presses.fetch_add(1u, std::memory_order_relaxed);
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
    updateButtonGlyphs(*backend);
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
    if (g_f6Presses.exchange(0u, std::memory_order_relaxed) % 2u != 0u)
    {
        rg::freecamera::setEnabled(!rg::freecamera::enabled());
    }
    // Changes made on the game's Settings screen (its pages are there with
    // the menu alone), kept as the menu's own.
    for (rg::settingspages::Change change; g_menu && rg::settingspages::takeChange(change);)
    {
        if (change.key == "sides")
            g_menu->setSides(static_cast<dq8::gfx::SdlGpuSides>(change.value));
        else if (change.number)
            g_menu->setGameNumberValue(change.key, change.value);
        else
            g_menu->setGameOptionValue(change.key, change.value != 0);
    }
    // Display > Borders, which the pages show too, as the menu has it.
    if (g_menu)
    {
        static int sidesWas = -1;
        if (const int sides = static_cast<int>(g_menu->settings().display.sides); sides != sidesWas)
        {
            sidesWas = sides;
            rg::settingspages::noteNumber("sides", sides);
        }
    }
    followQuickScenes();
    // The free camera turned on or off, by F6, either menu or Start: a toast.
    static bool freeCameraWas = false;
    if (const bool freeCamera = rg::freecamera::enabled(); freeCamera != freeCameraWas)
    {
        freeCameraWas = freeCamera;
        if (g_menu)
        {
            g_menu->notify(freeCamera ? "Free camera on: Start or F6 to leave" : "Free camera off",
                           dq8::ui::Icon::Camera);
        }
    }
    // Items the bag had no room for, sent to the storage.
    if (const unsigned sent = rg::inventory::takeSentToStorage(); sent != 0u && g_menu)
    {
        g_menu->notify(sent == 1u ? "The bag is full: the item went to the transporter's item storage"
                                  : "The bag is full: " + std::to_string(sent) +
                                        " items went to the transporter's item storage",
                       dq8::ui::Icon::Info);
    }
    // A newer release found by the check of this run: a toast, once.
    static bool releaseTold = false;
    static auto releaseAsked = std::chrono::steady_clock::time_point{};
    if (!releaseTold && g_menu && std::chrono::steady_clock::now() - releaseAsked > std::chrono::seconds(1))
    {
        releaseAsked = std::chrono::steady_clock::now();
        if (const rg::updates::Release newer = rg::updates::newer(); !newer.version.empty())
        {
            releaseTold = true;
            g_menu->notify("Version " + newer.version + " is out: F1, then Rogue Galaxy > About", dq8::ui::Icon::Info,
                           10.0);
        }
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

// The image the menu's Display > Borders > Image shows beside a 4:3 picture:
// the first PNG, JPG, BMP or TGA by name in borders\ (beside the disc
// folder, in the game's working directory). It is read each time that
// choice is made, so an image put there while the game runs is taken by
// choosing it again.
bool loadBorderImage(uint32_t &width, uint32_t &height, std::vector<uint8_t> &rgba)
{
    std::vector<std::filesystem::path> images;
    std::error_code error;
    for (const auto &entry : std::filesystem::directory_iterator("borders", error))
    {
        std::string extension = entry.path().extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (entry.is_regular_file(error) && (extension == ".png" || extension == ".jpg" || extension == ".jpeg" ||
                                             extension == ".bmp" || extension == ".tga"))
        {
            images.push_back(entry.path());
        }
    }
    std::sort(images.begin(), images.end());
    for (const std::filesystem::path &path : images)
    {
        int w = 0, h = 0, channels = 0;
        if (unsigned char *pixels = stbi_load(path.string().c_str(), &w, &h, &channels, 4))
        {
            width = static_cast<uint32_t>(w);
            height = static_cast<uint32_t>(h);
            rgba.assign(pixels, pixels + static_cast<size_t>(w) * static_cast<size_t>(h) * 4u);
            stbi_image_free(pixels);
            std::printf("[rg] sides: %s, %dx%d\n", path.string().c_str(), w, h);
            return true;
        }
        std::fprintf(stderr, "[rg] sides: could not read %s\n", path.string().c_str());
    }
    std::fprintf(stderr, "[rg] sides: no image in borders\\, the blurred picture instead\n");
    return false;
}

// The saved place and maximized state. A place whose top edge is on no
// display now (one unplugged since) opens centred instead.
dq8::gfx::SdlGpuWindowPlacement initialWindowPlacement(const dq8::ui::Settings &settings)
{
    dq8::gfx::SdlGpuWindowPlacement placement;
    placement.maximized = settings.maximized;
    if (!settings.windowPlaced)
    {
        return placement;
    }
    const SDL_Point titleBar{settings.windowX + 64, settings.windowY};
    int count = 0;
    if (SDL_DisplayID *displays = SDL_GetDisplays(&count))
    {
        for (int index = 0; index < count && !placement.placed; ++index)
        {
            SDL_Rect bounds{};
            placement.placed = SDL_GetDisplayBounds(displays[index], &bounds) && SDL_PointInRect(&titleBar, &bounds);
        }
        SDL_free(displays);
    }
    placement.x = settings.windowX;
    placement.y = settings.windowY;
    return placement;
}

// DQ8-Recompiled's in-game menu over the window: display, sound, controls
// and the frame-rate counter (thirdparty/dq8-gfx/src/ui). What it asks of
// the game goes through HostServices.
std::unique_ptr<dq8::ui::Overlay> createMenu(PS2Runtime &runtime, dq8::gfx::SdlGpuBackend &backend,
                                             dq8::ui::Settings settings, const std::string &settingsPath)
{
    dq8::ui::HostServices host;
    host.version = RG_APP_VERSION;
    host.revision = RG_REVISION;
    // RG_MUTE=1 keeps the game silent whatever the menu's volume says.
    const bool mute = env("RG_MUTE", "0") != "0";
    host.setVolume = [mute](float volume) { ps2_native_iop::setVolume(mute ? 0.0f : volume); };
    // The sound driver runs in step with the audio device, so stopping the
    // device stops the IOP along with the EE's clock.
    host.setPaused = [&runtime](bool paused) {
        runtime.eeScheduler().setHostPaused(paused);
        ps2_native_iop::setPaused(paused);
    };
    g_runtime = &runtime;
    host.setSpeed = [](double speed) {
        g_menuSpeed = speed;
        applySpeed();
    };
    // The game's flips: frames it finished, not presents, which go on while
    // it is paused.
    host.completedRenders = [] { return guestFlips(); };
    host.guestFrame = [] { return ps2PadCurrentGuestFrame(); };
    // The game's own switches, on the menu's Game page, in its sections.
    host.gameOptions = {
        {"sixty_fps", "60 frames per second",
         "The game runs at 60 frames a second instead of its own 30, its logic still at the PS2's pace; movies "
         "and the loading screen keep 30. Takes effect the next time the game starts.",
         true, 0, 0, 0, "", 1, true, true},
        {"widescreen", "Widescreen",
         "The game draws a wider view, and the HUD over the scene and the movies fit it; with Display > Aspect "
         "ratio on Auto the picture fills the window, whatever its shape, once a game is loaded.",
         false},
        {"minimap_size", "Minimap size",
         "The map in the field's corner, against the game's size (100%), smaller toward its corner. The "
         "transporter menu's map stays as it is.",
         false, 50, 100, 100, "%d%%", 5},
        {"compass_size", "Compass size",
         "The compass in the field's corner, against the game's size (100%), smaller toward its corner.", false, 50,
         100, 100, "%d%%", 5},
        {"field_of_view", "Field of view",
         "How wide the camera sees while exploring and in battle, in degrees across a 4:3 picture (the game's is "
         "52); a wide picture adds to it. Scenes keep their own camera.",
         false, 40, 90, 52, "%d degrees", 2},
        {"camera_distance", "Camera distance",
         "How far the camera stays from the party, against the game's (100%).",
         false, 70, 160, 100, "%d%%", 5},
        {"faster_camera", "Faster camera rotation",
         "The stick turns the camera twice as fast, exploring, in battle and in the first-person view. Otherwise "
         "it turns at the game's speed, at 60 frames a second as at 30.",
         false},
        {"draw_distance", "Draw distance",
         "How far away the scenery and the objects are still drawn, against the game's (100%), the fog thinned "
         "in step. Most of Salgin has no limit to raise; the open areas gain more. From the next map loaded.",
         false, 100, 400, 100, "%d%%", 25},
        {"free_camera", "Free camera (F6)",
         "The camera leaves the party, which stands still, and the controller moves it: the left stick moves the "
         "point it looks at (Square held, faster; L2 held, up and down), the right stick turns it around that "
         "point and tilts it (R2 held, nearer and farther). Turn it off, or press F6, to play again. Off at every "
         "start.",
         false, 0, 0, 0, "", 1, false},
        {"controller_buttons", "Controller's buttons",
         "The buttons shown in the game's text follow the controller in use: an Xbox controller's A, B, X, Y, LB, "
         "RB, LT and RT, as each button is bound in Controls. Off, or with a PlayStation or Nintendo controller "
         "or the keyboard, the game's PlayStation buttons.",
         true},
        {"skip_opening", "Skip the opening",
         "At boot, the Sony and Level-5 logos and the opening movie give way to the title screen at once.", false},
        {"instant_loading_recap", "Show the loading recap at once",
         "When a game loads, the story so far appears whole instead of line by line, and Press X comes as soon as "
         "it has loaded.",
         false},
        {"instant_text", "Instant text",
         "Dialogue and messages appear a page at a time instead of letter by letter, as the game itself does "
         "where a scene asks for it; the pauses written into the text stay.",
         false},
        {"map_chests", "Chests on the map",
         "The chests not yet opened show on the maps in the parts you have explored, as the game shows them once "
         "every save point of a planet is activated. Nothing is saved: the planet's own completion still comes.",
         false},
        {"fast_transporter", "Fast transporters",
         "A transporter's warp plays at up to twice the speed. The music and the sounds keep their pace.", false},
        {"check_updates", "Check for new versions",
         "At each start the game asks GitHub for this project's releases (nothing is sent but the request) and, "
         "when a newer one is out, says so and links to it from the menu's About window.",
         true},
        {"full_bag_to_storage", "Full bag to storage",
         "An item found when the bag has no room for it (no free space, or as many of that item as the bag "
         "holds) goes to the transporter's item storage, instead of the overflow the game makes you sort out in "
         "the item menu, or losing it when that is full too. Shops and the Factory still ask for room in the "
         "bag.",
         false},
        {"stack_limit_99", "Item limit 99",
         "Every item that stacks can be carried up to 99 in the bag. The game holds some to 20, 30 or 50, healing "
         "items and boosts among them. Off again, nothing carried is lost; more just cannot be added.",
         false},
        {"bigger_bag", "Seventh bag page",
         "The bag gets a seventh page: 280 spaces instead of 240. The transporter's storage keeps its 320. A save "
         "made with it still loads without it: the seventh page's items are kept, hidden, until it is on again. "
         "Takes effect the next time the game starts.",
         false, 0, 0, 0, "", 1, true, true},
        {"fast_revelation", "Fast Revelation Flow",
         "Items going into a Revelation Flow cell, the zoom, the glow and the ability won, play at up to twice "
         "the speed.",
         false},
        {"encounter_rate", "Encounter rate",
         "How often random battles come, against the game's (100%); 0% turns them off. The story's battles stay.",
         false, 0, 300, 100, "%d%%", 10},
        {"battle_experience", "Experience from battles",
         "The experience a won battle gives, against the game's (100%), up to the game's 9999 a battle.",
         false, 0, 500, 100, "%d%%", 25},
        {"battle_money", "Money from battles",
         "The money defeated enemies leave, against the game's (100%). Selling, chests and rewards stay as they are.",
         false, 0, 500, 100, "%d%%", 25},
        {"item_drops", "Item drops",
         "How likely defeated enemies are to leave each of their items, against the game's (100%): most have a 2 "
         "to 10% chance, which 1000% makes 20 to 100%; 0% for none. A battle still gives three kinds of items at "
         "most, as the game does.",
         false, 0, 1000, 100, "%d%%", 50},
        {"weapon_growth", "Weapon growth",
         "How fast weapons grow in won battles, against the game's (100%): each battle fills a weapon's gauge by "
         "1 (15 to full), then raises one of its attributes by 1, which 300% makes 3.",
         false, 100, 1000, 100, "%d%%", 100},
        {"shop_prices", "Shop prices",
         "What shops ask for their items, against the game's (100%). Below 100% nothing sells for more than it "
         "costs, so buying and selling back makes no money.",
         false, 10, 200, 100, "%d%%", 10},
        {"action_gauge", "Action gauge speed",
         "How fast the party's action gauge fills again in battle, against the game's (100%).",
         false, 25, 400, 100, "%d%%", 25},
        {"japanese_voices", "Undub (Japanese voices)",
         "The voices in scenes and the movies come from your Japanese Director's Cut disc instead of the English "
         "ones; the text stays as it is. It needs that disc's files: run Install-Undub.bat in the game's folder "
         "once and pick your Japanese disc image. Without them the voices stay English. A few lines have no "
         "Japanese voice and play silent. Takes effect the next time the game starts.",
         false, 0, 0, 0, "", 1, true, true, true},
        {"native_renderer", "Native renderer",
         "Experimental, for development: the models and the post effects drawn by the PC's renderer instead of "
         "through the emulation of the PS2's graphics chip, as that work lands; for now it changes nothing. Off, "
         "everything goes through the emulation. Takes effect the next time the game starts.",
         false, 0, 0, 0, "", 1, true, true},
        {"dev_tools", "Developer tools",
         "The tools the developers left in the game, switched off for release. Main menu: R3 shows every item's "
         "icon, R1 + R3 a list of them, and Cross gives one of the one picked. Help pages and info list: R1 + "
         "Select unlocks every entry. Bestiary: Select fills it. Revelation Flow: R2 + Triangle gives one of each "
         "item the picked cell asks for. Paused in a scene: Triangle + R1 + R2 ends it, also one the game would "
         "not let you skip, which can leave the story out of order.",
         false},
    };
    host.gameSections = {
        {"sixty_fps", "Camera and view"},
        {"skip_opening", "Convenience"},
        {"encounter_rate", "Battles and rewards"},
        {"native_renderer", "Rendering"},
        {"dev_tools", "Developers"},
    };
    // The game's own Settings screen shows them on pages of its own.
    for (const dq8::ui::GameOption &option : host.gameOptions)
    {
        if (option.number())
        {
            rg::settingspages::setRange(option.key, option.minimum, option.maximum, option.step,
                                        option.defaultNumber);
        }
    }
    // And Display > Borders: Black, Blurred, Image.
    rg::settingspages::setRange("sides", 0, 2, 1, 0);
    host.setGameOption = &applyGameOption;
    host.gameOptionState = [](const std::string &key) { return key == "free_camera" && rg::freecamera::enabled(); };
    host.setGameNumber = &applyGameNumber;
    host.newerRelease = [] {
        rg::updates::Release newer = rg::updates::newer();
        return std::make_pair(std::move(newer.version), std::move(newer.url));
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
    // Not in a scripted run: the benchmarks ask nothing of GitHub.
    if (env("RG_INPUT", "").empty())
    {
        rg::updates::start(RG_APP_VERSION);
    }
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
#if defined(_WIN32)
            _putenv("DQ8_GFX_SHOW_FPS=1");
#else
            setenv("DQ8_GFX_SHOW_FPS", "1", 1);
#endif
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
        // A Steam Deck starts in fullscreen (its Game Mode shows a window
        // letterboxed, and a window of another shape turns the widescreen
        // off), whatever an earlier run saved; RG_WINDOWED=1 keeps the
        // saved window. Its first run also starts at the native scale: the
        // 1280x800 screen shows nothing of a 2x one, and the APU is the
        // limit in battle.
        if (onSteamDeck())
        {
            std::error_code none;
            if (!std::filesystem::exists(settingsPath, none))
            {
                settings.resolutionScale = 1u;
            }
            if (env("RG_WINDOWED", "0") != "1")
            {
                settings.fullscreen = true;
            }
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
        sdl->setSideImageLoader(&loadBorderImage);
        if (noWindow)
        {
            std::printf("[rg] GS: sdlgpu, presenting through the runtime window\n");
        }
        else
        {
            const auto [width, height] = initialWindowSize(settings);
            std::string windowError;
            if (sdl->openWindow("Rogue Galaxy Recompiled", width, height, windowError,
                                initialWindowPlacement(settings)))
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
                g_menu = m_menu.get();
                // The game's Settings screen shows the menu's options.
                rg::settingspages::setEnabled(g_hasMenu);
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
    g_menu = nullptr;
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
