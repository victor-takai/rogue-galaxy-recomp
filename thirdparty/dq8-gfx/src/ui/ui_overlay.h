// The in-game menu, drawn with Dear ImGui over the SDL GPU backend's window:
// a menu bar (F1, or Guide / Back+Start on a controller), the settings window,
// notifications and the frame-rate counter.
#pragma once

#include "gfx/backends/sdlgpu/sdlgpu_backend.h"
#include "ui/ui_glyphs.h"
#include "ui/ui_settings.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

struct ImGuiContext;
struct ImGuiWindow;

namespace dq8::ui {

// Screenshots are PNG. SDL_SavePNG arrived in SDL 3.4; with SDL 3.2 they are BMP.
#if SDL_VERSION_ATLEAST(3, 4, 0)
inline constexpr const char *kImageExtension = ".png";
inline bool saveImage(SDL_Surface *surface, const char *path) { return SDL_SavePNG(surface, path); }
#else
inline constexpr const char *kImageExtension = ".bmp";
inline bool saveImage(SDL_Surface *surface, const char *path) { return SDL_SaveBMP(surface, path); }
#endif

// What the menu can ask of the game around it. A control whose function is
// empty is not shown.
// A switch of the game's own, shown on the Game page and kept in the
// settings file's [game] section under `key`, unless not `remembered`
// (Rogue Galaxy: added).
struct GameOption {
    std::string key;
    std::string label;
    std::string help;
    bool defaultValue = false;
    // A number from minimum to maximum instead of a switch, when maximum is
    // the larger, shown as a slider labelled by `format` (printf, its %d the
    // value), between - and + buttons that move it by `step`, and a Default
    // button.
    int minimum = 0;
    int maximum = 0;
    int defaultNumber = 0;
    std::string format;
    int step = 1;
    // A switch the file does not keep: it is off at every start, and the
    // menu shows what HostServices::gameOptionState says, as something
    // besides the menu (a key) can turn it on or off.
    bool remembered = true;
    // Takes effect at the next start: once it differs from what this run
    // started with, the menu says so in red.
    bool restart = false;
    // The page it is on: empty for Game's, "language" for Language's,
    // "textures" for Textures' (Rogue Galaxy: added).
    std::string page;
    bool number() const { return maximum > minimum; }
};

struct HostServices {
    std::string version;
    // The commit it was built from, beside the version in the About window
    // (Rogue Galaxy: added).
    std::string revision;
    std::function<void(float)> setVolume; // 0 to 1
    std::function<void(bool)> setPaused;
    // The game's speed against real time; 0 runs it as fast as it can go.
    std::function<void(double)> setSpeed;
    // Frames the game has finished drawing, for the counter; presents are
    // counted instead when it is empty.
    std::function<uint64_t()> completedRenders;
    // The guest's vsync clock, which DQ8_UI_SCRIPT is timed against, as
    // DQ8_PAD_SCRIPT is; presents are counted instead when it is empty.
    std::function<uint64_t()> guestFrame;
    // The game's own switches, and what turns one on or off (Rogue Galaxy:
    // added).
    std::vector<GameOption> gameOptions;
    // The Game page's sections: a title each, before the option of its key.
    std::vector<std::pair<std::string, std::string>> gameSections;
    std::function<void(const std::string &key, bool on)> setGameOption;
    std::function<void(const std::string &key, int value)> setGameNumber;
    std::function<bool(const std::string &key)> gameOptionState;
    // A newer release than this build, its version and page, empty while
    // none is known: the About window says so (Rogue Galaxy: added).
    std::function<std::pair<std::string, std::string>()> newerRelease;
    // A Steam Deck, whose Steam button belongs to Steam: Back + Start is the
    // only way to the menu there, so it cannot be turned off, and the menu
    // names the Deck's buttons (Rogue Galaxy: added).
    bool steamDeck = false;
};

// Game: Rogue Galaxy's own switches (added).
// Language and Textures: the options a host puts there (GameOption::page),
// apart from Game's (Rogue Galaxy: added).
enum class SettingsPage : uint8_t { Display, Sound, Controls, Game, Language, Textures, Interface, Count };

class Overlay final : public gfx::SdlGpuOverlay {
public:
    Overlay(gfx::SdlGpuBackend &backend, Settings settings, std::string settingsPath, HostServices host);
    ~Overlay() override;
    Overlay(const Overlay &) = delete;
    Overlay &operator=(const Overlay &) = delete;

    // `format` is the texture format render() draws into.
    bool initialize(SDL_Window *window, SDL_GPUTextureFormat format, std::string &error);
    // Pushes every setting to the backend, the window and the host (a game
    // switch not `remembered` is the host's own and is not pushed), and the
    // first time notes the switches this run starts with.
    void applySettings();

    bool handleEvent(const SDL_Event &event) override;
    void render(SDL_GPUCommandBuffer *commands, SDL_GPUTexture *target, uint32_t width,
                uint32_t height) override;
    bool wantsFrames() override;
    bool capturesInput() const override;

    const Settings &settings() const { return m_settings; }
    // A game option changed elsewhere, such as the game's own settings
    // screen (Rogue Galaxy: added): passed to the host and kept as the
    // menu's own changes are.
    void setGameOptionValue(const std::string &key, bool on);
    void setGameNumberValue(const std::string &key, int value);
    // Display > Borders, changed there too (Rogue Galaxy: added).
    void setSides(gfx::SdlGpuSides sides);
    void setMenuOpen(bool open);
    void openSettings(SettingsPage page);
    // Opens one of the menu bar's menus at the next frame, by its label.
    void showMenu(const char *label);
    void openAbout();
    void openShortcuts();
    // Picks the Controls page's controller or keyboard tab.
    void showBindings(bool keyboard);
    void notify(std::string text, Icon icon = Icon::Info, double seconds = 3.0);
    void setUserPaused(bool paused);
    void setFastForward(bool on);
    // Waits for the next key or controller input and binds it.
    void beginBindingCapture(gfx::PadInput input, bool keyboard, int slot);
    // Waiting for one, so a key of the host's own is not taken for it too
    // (Rogue Galaxy: added).
    bool capturingBinding() const { return m_capture.active; }
    void takeScreenshot(bool withMenu);
    std::string screenshotFolder() const;

private:
    struct Toast {
        std::string text;
        Icon icon = Icon::Info;
        double startedAt = 0.0;
        double duration = 3.0;
    };
    struct Capture {
        bool active = false;
        gfx::PadInput input = gfx::PadInput::Count;
        bool keyboard = false;
        int slot = 0;
        double startedAt = 0.0;
        // B on a controller binds B when tapped and cancels when held: the
        // pad has no Esc.
        double backDownAt = -1.0;
    };
    // Where keyboard and controller navigation stood before this frame's
    // input, which Back (B, Esc) is judged by: ImGui's own handling of it
    // has already moved things by the time the frame is drawn.
    struct NavBefore {
        ImGuiWindow *window = nullptr;
        bool menuLayer = false;
        bool itemActive = false;
        bool popupOpen = false;
        bool modalOpen = false;
    };
    struct ScriptStep {
        uint64_t frame = 0u;
        double seconds = -1.0; // wall time instead, for steps a pause would hold back
        std::string command;
        std::vector<std::string> arguments;
        bool done = false;
    };

    // DQ8_UI_SCRIPT (ui_script.cpp): menu actions on the guest frame clock,
    // for tests and screenshots of the menu over the running game.
    void loadScript();
    void runScript();
    void runStep(const ScriptStep &step, uint64_t frame);

    // Event side.
    bool hotkey(const SDL_KeyboardEvent &key);
    bool gamepadShortcut(const SDL_GamepadButtonEvent &button);
    bool captureEvent(const SDL_Event &event);
    // Back, one level at a time; `pad` when it came from the controller's B.
    void backOut(bool pad);
    void toggleMenu(bool fromController);
    void toggleBarFocus();
    // The controllers' state for ImGui, read here instead of by its SDL
    // backend: the left stick moves the cursor as the D-pad does, and the
    // right stick scrolls.
    void feedGamepad();
    void rememberNavigation();
    bool padInUse() const { return m_lastInputPad; }
    // A controller button as the controller in use labels it ("B", "Circle").
    std::string padButtonName(SDL_GamepadButton button) const;
    // The row of button hints along the settings window's bottom edge.
    void drawPadHints(float height);
    void drawQuitPrompt();

    // Frame side.
    void updateScale();
    void updateNavigation();
    void drawFrame();
    void drawMenuBar();
    void drawStatus();
    void drawSettingsWindow();
    void drawDisplayPage();
    void drawSoundPage();
    void drawControlsPage();
    void drawBindingTable(bool keyboard);
    void drawGamePage();
    void drawLanguagePage();
    void drawTexturesPage();
    // The host's switches whose page is `page`, in its sections.
    void drawGameOptions(const std::string &page, const char *emptyText, const char *firstTitle);
    void drawInterfacePage();
    void drawCapturePrompt();
    void drawAbout();
    void drawShortcuts();
    void drawStats();
    void pictureBounds(ImVec2 &min, ImVec2 &max) const;
    void drawFps();
    void drawSpeedBadge();
    void drawPauseBanner();
    void drawToasts();
    void updateCursor();
    void sampleFrameRate();
    void collectSavedScreenshots();

    // Settings side.
    void markDirty();
    void saveIfDirty(bool force);
    void rememberWindow(const SDL_WindowEvent &event);
    void applyVolume();
    void applyPause();
    // A game switch's value: the settings file's, or the option's default;
    // for one not `remembered`, what HostServices::gameOptionState says.
    bool gameOption(const GameOption &option) const;
    // A `restart` switch changed since this run started.
    bool waitsForRestart(const GameOption &option) const;
    // A game number's value, the same way, within the option's range.
    int gameNumber(const GameOption &option) const;
    void setFullscreen(bool fullscreen);
    void setPadConfig(const gfx::PadConfig &config);
    // Asks for an internal resolution and saves it. Asking for the active one
    // replaces a request still queued, which cancels it.
    void requestScale(uint32_t scale);
    bool pausing() const;
    bool canPause() const { return static_cast<bool>(m_host.setPaused); }
    bool canChangeSpeed() const { return static_cast<bool>(m_host.setSpeed); }
    std::string speedLabel(float speed) const;
    SDL_GamepadType displayedControllerType() const;
    double now() const;

    gfx::SdlGpuBackend &m_backend;
    Settings m_settings;
    std::string m_settingsPath;
    HostServices m_host;
    // The switches as this run started with them, for those that wait for a
    // restart (Rogue Galaxy: added).
    std::map<std::string, bool> m_startOptions;
    bool m_startCaptured = false;
    SDL_Window *m_window = nullptr;
    ImGuiContext *m_context = nullptr;
    bool m_platformReady = false;
    bool m_rendererReady = false;
    float m_appliedScale = 0.0f;

    bool m_menuOpen = false;
    bool m_settingsOpen = false;
    bool m_focusSettings = false;
    SettingsPage m_page = SettingsPage::Display;
    bool m_aboutOpen = false;
    bool m_shortcutsOpen = false;
    bool m_statsOpen = false;
    std::string m_showMenu;
    bool m_windowFocused = true;
    bool m_userPaused = false;
    bool m_hostPaused = false;
    bool m_fastForward = false;
    bool m_gamepadNavHeld = false;
    // The settings window's two halves, the page list and the page, which
    // the cursor moves between by Right / A and Left / B (the windows
    // themselves; ImGui keeps them for the life of the context).
    ImGuiWindow *m_listPane = nullptr;
    ImGuiWindow *m_pagePane = nullptr;
    bool m_focusList = false;
    bool m_focusPage = false;
    bool m_quitAsk = false;
    bool m_cancelQuit = false;
    NavBefore m_navBefore;
    // The last input was a controller's, so the hints show its buttons.
    bool m_lastInputPad = false;
    // The left stick's directions as D-pad presses: left, right, up, down.
    std::array<bool, 4> m_stickHeld{};
    bool m_keyboardBindings = false;
    int m_selectBindings = -1;
    gfx::PadInput m_hoveredInput = gfx::PadInput::Count;
    Capture m_capture;
    uint32_t m_pendingScale = 0u;
    double m_pendingScaleSince = 0.0;

    std::vector<Toast> m_toasts;
    std::vector<std::pair<SDL_JoystickID, std::string>> m_controllerNames;

    double m_rateStart = 0.0;
    uint64_t m_rateRenders = 0u;
    uint64_t m_presents = 0u;
    float m_fps = 0.0f;
    // Below the counter when they share a corner.
    float m_toastTop = 0.0f;
    float m_fpsLeftBottom = 0.0f;
    std::array<float, 64> m_fpsHistory{};
    size_t m_fpsHistoryNext = 0u;

    gfx::SdlGpuStats m_stats{};
    gfx::SdlGpuStats m_statsPrevious{};
    double m_statsAt = -1.0;
    double m_statsSpan = 0.0;
    double m_lastMouseMotion = 0.0;
    int m_uiScaleEdit = 0;

    bool m_dirty = false;
    double m_dirtyAt = 0.0;

    bool m_closePopups = false;
    std::vector<ScriptStep> m_script;
    double m_scriptStart = 0.0;

    struct Saver {
        std::thread thread;
        std::shared_ptr<std::atomic<bool>> done;
    };
    std::mutex m_savedMutex;
    std::vector<std::pair<bool, std::string>> m_saved;
    std::vector<Saver> m_savers;
};

} // namespace dq8::ui
