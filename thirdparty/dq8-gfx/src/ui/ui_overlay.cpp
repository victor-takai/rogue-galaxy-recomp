#include "ui/ui_overlay.h"

#include "gfx/backends/sdlgpu/sdlgpu_input.h"
#include "ui/ui_style.h"
#include "ui/ui_widgets.h"

#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlgpu3.h>
#include <imgui_internal.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>

namespace dq8::ui {
namespace {
constexpr double kCaptureSeconds = 6.0;
#if defined(__APPLE__)
constexpr const char *kQuitShortcut = "Cmd+Q";
#else
constexpr const char *kQuitShortcut = "Alt+F4";
#endif
constexpr const char *kProjectUrl = "https://github.com/victor-takai/rogue-galaxy-recomp";
constexpr const char *kIssuesUrl = "https://github.com/victor-takai/rogue-galaxy-recomp/issues";

// Function keys the menu owns, and F6, the game's free camera (Rogue Galaxy:
// added); binding one would fight the shortcut.
bool reservedKey(SDL_Scancode scancode) {
    return scancode == SDL_SCANCODE_F1 || scancode == SDL_SCANCODE_F2 || scancode == SDL_SCANCODE_F3 ||
           scancode == SDL_SCANCODE_F4 || scancode == SDL_SCANCODE_F6 || scancode == SDL_SCANCODE_F11 ||
           scancode == SDL_SCANCODE_F12;
}

std::string timestamp() {
    const std::time_t time = std::time(nullptr);
    char text[32] = {};
    std::strftime(text, sizeof(text), "%Y-%m-%d_%H-%M-%S", std::localtime(&time));
    return text;
}

float easeOut(float t) { return 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t); }
} // namespace

Overlay::Overlay(gfx::SdlGpuBackend &backend, Settings settings, std::string settingsPath, HostServices host)
    : m_backend(backend), m_settings(std::move(settings)), m_settingsPath(std::move(settingsPath)),
      m_host(std::move(host)) {}

Overlay::~Overlay() {
    for (auto &saver : m_savers)
        saver.thread.join();
    saveIfDirty(true);
    if (!m_context)
        return;
    ImGui::SetCurrentContext(m_context);
    if (m_rendererReady)
        ImGui_ImplSDLGPU3_Shutdown();
    if (m_platformReady)
        ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext(m_context);
}

double Overlay::now() const {
    static const auto start = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

bool Overlay::initialize(SDL_Window *window, SDL_GPUTextureFormat format, std::string &error) {
    m_window = window;
    IMGUI_CHECKVERSION();
    m_context = ImGui::CreateContext();
    ImGui::SetCurrentContext(m_context);
    ImGuiIO &io = ImGui::GetIO();
    // Settings are ours; ImGui keeps no files.
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    // Back never leaves the cursor nowhere: a controller has no mouse to
    // find it again with.
    io.ConfigNavEscapeClearFocusItem = false;
    // Holding X would bring up ImGui's window switcher.
    m_context->ConfigNavWindowingWithGamepad = false;
    loadFonts();
    if (!ImGui_ImplSDL3_InitForSDLGPU(window)) {
        error = "Dear ImGui could not attach to the window";
        return false;
    }
    m_platformReady = true;
    // No controllers for the backend: feedGamepad() reads them instead.
    ImGui_ImplSDL3_SetGamepadMode(ImGui_ImplSDL3_GamepadMode_Manual, nullptr, 0);
    ImGui_ImplSDLGPU3_InitInfo info;
    info.Device = m_backend.gpuDevice();
    info.ColorTargetFormat = format;
    info.MSAASamples = SDL_GPU_SAMPLECOUNT_1;
    if (!ImGui_ImplSDLGPU3_Init(&info)) {
        error = "Dear ImGui could not create its GPU resources";
        return false;
    }
    m_rendererReady = true;
    updateScale();
    loadScript();
    // Guide is often the system's (Steam, the Xbox Game Bar), so Back + Start
    // is named first.
    if (m_host.steamDeck)
        notify("Press View + Menu for the game's menu", Icon::Info, 6.0);
    else if (m_settings.backStartOpensMenu)
        notify("Press F1, or Back + Start on a controller, for the menu", Icon::Info, 6.0);
    else
        notify("Press F1, or Guide on a controller, for the menu", Icon::Info, 6.0);
    return true;
}

void Overlay::applySettings() {
    m_backend.setDisplayOptions(m_settings.display);
    m_backend.padInput().setConfig(m_settings.pad);
    if (m_window && m_window == m_backend.window() && m_settings.fullscreen)
        SDL_SetWindowFullscreen(m_window, true);
    applyVolume();
    applyPause();
    for (const GameOption &option : m_host.gameOptions) {
        if (option.number()) {
            if (m_host.setGameNumber)
                m_host.setGameNumber(option.key, gameNumber(option));
        } else if (m_host.setGameOption && option.remembered) {
            m_host.setGameOption(option.key, gameOption(option));
        }
        if (!m_startCaptured && !option.number())
            m_startOptions[option.key] = gameOption(option);
    }
    m_startCaptured = true;
}

bool Overlay::waitsForRestart(const GameOption &option) const {
    const auto found = m_startOptions.find(option.key);
    return option.restart && found != m_startOptions.end() && found->second != gameOption(option);
}

bool Overlay::gameOption(const GameOption &option) const {
    if (!option.remembered)
        return m_host.gameOptionState ? m_host.gameOptionState(option.key) : option.defaultValue;
    const auto found = m_settings.gameOptions.find(option.key);
    return found != m_settings.gameOptions.end() ? found->second : option.defaultValue;
}

void Overlay::setGameOptionValue(const std::string &key, bool on) {
    for (const GameOption &option : m_host.gameOptions) {
        if (option.key != key || option.number())
            continue;
        if (option.remembered) {
            m_settings.gameOptions[key] = on;
            m_settings.gameNumbers.erase(key);
            // Saved now: the menu, closed, draws no frame to save it in.
            markDirty();
            saveIfDirty(true);
        }
        if (m_host.setGameOption)
            m_host.setGameOption(key, on);
    }
}

void Overlay::setGameNumberValue(const std::string &key, int value) {
    for (const GameOption &option : m_host.gameOptions) {
        if (option.key != key || !option.number())
            continue;
        value = std::clamp(value, option.minimum, option.maximum);
        m_settings.gameNumbers[key] = value;
        m_settings.gameOptions.erase(key);
        markDirty();
        saveIfDirty(true);
        if (m_host.setGameNumber)
            m_host.setGameNumber(key, value);
    }
}

void Overlay::setSides(gfx::SdlGpuSides sides) {
    if (m_settings.display.sides == sides)
        return;
    m_settings.display.sides = sides;
    m_backend.setDisplayOptions(m_settings.display);
    markDirty();
    saveIfDirty(true);
}

int Overlay::gameNumber(const GameOption &option) const {
    const auto found = m_settings.gameNumbers.find(option.key);
    const int value = found != m_settings.gameNumbers.end() ? found->second : option.defaultNumber;
    return std::clamp(value, option.minimum, option.maximum);
}

bool Overlay::capturesInput() const { return m_menuOpen || m_capture.active; }

bool Overlay::wantsFrames() {
    if (!m_context)
        return false;
    // Queued input has to be consumed by a frame, or it replays later.
    const bool scriptWaiting = std::any_of(m_script.begin(), m_script.end(),
                                           [](const ScriptStep &step) { return !step.done && step.seconds >= 0.0; });
    return m_menuOpen || m_capture.active || !m_toasts.empty() || m_settings.showFps || m_statsOpen ||
           pausing() || scriptWaiting || !m_context->InputEventsQueue.empty();
}

void Overlay::setMenuOpen(bool open) {
    if (m_menuOpen == open)
        return;
    m_menuOpen = open;
    if (!open) {
        m_settingsOpen = m_aboutOpen = m_shortcutsOpen = false;
        m_capture.active = false;
        m_closePopups = true;
    }
    applyPause();
}

void Overlay::openSettings(SettingsPage page) {
    setMenuOpen(true);
    m_settingsOpen = true;
    m_focusSettings = true;
    m_focusList = true;
    m_page = page;
}

void Overlay::showMenu(const char *label) {
    setMenuOpen(true);
    m_showMenu = label;
}

void Overlay::openAbout() {
    setMenuOpen(true);
    m_aboutOpen = true;
}

void Overlay::openShortcuts() {
    setMenuOpen(true);
    m_shortcutsOpen = true;
}

void Overlay::showBindings(bool keyboard) { m_selectBindings = keyboard ? 1 : 0; }

void Overlay::toggleMenu(bool fromController) {
    if (m_menuOpen) {
        setMenuOpen(false);
        return;
    }
    setMenuOpen(true);
    if (fromController) {
        // A controller starts in the window, on the page list.
        m_settingsOpen = true;
        m_focusSettings = true;
        m_focusList = true;
        m_gamepadNavHeld = true;
    }
}

// The top bar is a window of its own, which a controller reaches only by
// holding X and cycling windows; this takes it there, and back.
void Overlay::toggleBarFocus() {
    ImGuiContext &g = *ImGui::GetCurrentContext();
    ImGuiWindow *bar = ImGui::FindWindowByName("##MainMenuBar");
    if (!bar)
        return;
    const bool inBar = g.NavWindow && g.NavWindow->RootWindowForNav == bar;
    const bool inPopup = ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    if (inBar || inPopup) {
        ImGui::ClosePopupsExceptModals();
        g.NavLayer = ImGuiNavLayer_Main;
        if (m_settingsOpen) {
            m_focusSettings = true;
            m_focusList = true;
        } else {
            ImGui::FocusWindow(nullptr);
        }
        return;
    }
    ImGui::FocusWindow(bar);
    g.NavLayer = ImGuiNavLayer_Menu;
    ImGui::NavInitWindow(bar, true);
    ImGui::SetNavCursorVisible(true);
}

void Overlay::notify(std::string text, Icon kind, double seconds) {
    if (!m_settings.notifications)
        return;
    for (Toast &toast : m_toasts)
        if (toast.text == text) {
            toast.startedAt = std::max(toast.startedAt, now() - 0.2);
            toast.duration = seconds;
            return;
        }
    if (m_toasts.size() >= 4u)
        m_toasts.erase(m_toasts.begin());
    m_toasts.push_back({std::move(text), kind, now(), seconds});
}

void Overlay::beginBindingCapture(gfx::PadInput input, bool keyboard, int slot) {
    m_capture = {true, input, keyboard, slot, now()};
}

void Overlay::setUserPaused(bool paused) {
    if (!canPause())
        return;
    m_userPaused = paused;
    applyPause();
}

std::string Overlay::speedLabel(float speed) const {
    if (speed <= 0.0f)
        return "Unlimited";
    char text[16];
    std::snprintf(text, sizeof(text), "%g\xC3\x97", static_cast<double>(speed));
    return text;
}

void Overlay::setFastForward(bool on) {
    if (!canChangeSpeed() || m_fastForward == on)
        return;
    m_fastForward = on;
    m_host.setSpeed(on ? m_settings.fastForwardSpeed : 1.0);
    notify(on ? "Fast-forward: " + speedLabel(m_settings.fastForwardSpeed) : std::string("Normal speed"),
           on ? Icon::Speed : Icon::Play, 1.5);
}

bool Overlay::handleEvent(const SDL_Event &event) {
    if (!m_context)
        return false;
    ImGui::SetCurrentContext(m_context);
    if (m_capture.active && captureEvent(event))
        return true;
    switch (event.type) {
    case SDL_EVENT_GAMEPAD_ADDED: {
        const char *name = SDL_GetGamepadNameForID(event.gdevice.which);
        m_controllerNames.emplace_back(event.gdevice.which, name ? name : "Controller");
        notify(std::string("Connected: ") + m_controllerNames.back().second, Icon::Controls);
        break;
    }
    case SDL_EVENT_GAMEPAD_REMOVED: {
        const auto it = std::find_if(m_controllerNames.begin(), m_controllerNames.end(),
                                     [&](const auto &entry) { return entry.first == event.gdevice.which; });
        if (it != m_controllerNames.end()) {
            notify("Disconnected: " + it->second, Icon::Warning);
            m_controllerNames.erase(it);
        }
        break;
    }
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        m_windowFocused = event.type == SDL_EVENT_WINDOW_FOCUS_GAINED;
        applyVolume();
        applyPause();
        break;
    case SDL_EVENT_WINDOW_RESIZED:
    case SDL_EVENT_WINDOW_MOVED:
    case SDL_EVENT_WINDOW_MAXIMIZED:
    case SDL_EVENT_WINDOW_RESTORED:
        if (m_window && event.window.windowID == SDL_GetWindowID(m_window))
            rememberWindow(event.window);
        break;
    case SDL_EVENT_WINDOW_ENTER_FULLSCREEN:
    case SDL_EVENT_WINDOW_LEAVE_FULLSCREEN: {
        const bool fullscreen = event.type == SDL_EVENT_WINDOW_ENTER_FULLSCREEN;
        if (m_settings.fullscreen != fullscreen) {
            m_settings.fullscreen = fullscreen;
            markDirty();
        }
        break;
    }
    case SDL_EVENT_MOUSE_MOTION:
        m_lastMouseMotion = now();
        m_lastInputPad = false;
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        m_lastInputPad = false;
        break;
    // Back (Esc, B) reaches ImGui like any key, and drawFrame() takes it
    // from there, knowing where the cursor was.
    case SDL_EVENT_KEY_DOWN:
        m_lastInputPad = false;
        if (!event.key.repeat && hotkey(event.key))
            return true;
        break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        m_lastInputPad = true;
        if (gamepadShortcut(event.gbutton))
            return true;
        break;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        if (std::abs(static_cast<int>(event.gaxis.value)) > 16000)
            m_lastInputPad = true;
        break;
    default:
        break;
    }
    ImGui_ImplSDL3_ProcessEvent(&event);
    return false;
}

bool Overlay::hotkey(const SDL_KeyboardEvent &key) {
    const bool shift = (key.mod & SDL_KMOD_SHIFT) != 0;
    const bool alt = (key.mod & SDL_KMOD_ALT) != 0;
    switch (key.scancode) {
    case SDL_SCANCODE_F1:
        toggleMenu(false);
        return true;
    case SDL_SCANCODE_F2:
        if (!canPause())
            return false;
        setUserPaused(!m_userPaused);
        return true;
    case SDL_SCANCODE_F4:
        if (!canChangeSpeed())
            return false;
        setFastForward(!m_fastForward);
        return true;
    case SDL_SCANCODE_F3:
        m_settings.showFps = !m_settings.showFps;
        markDirty();
        return true;
    case SDL_SCANCODE_F11:
        setFullscreen(!m_settings.fullscreen);
        return true;
    case SDL_SCANCODE_F12:
        takeScreenshot(shift);
        return true;
    case SDL_SCANCODE_RETURN:
        if (!alt)
            return false;
        // Enter is Start in game; keep the held key from reaching it.
        m_backend.padInput().suppressHeld();
        setFullscreen(!m_settings.fullscreen);
        return true;
    default:
        return false;
    }
}

bool Overlay::gamepadShortcut(const SDL_GamepadButtonEvent &event) {
    const auto button = static_cast<SDL_GamepadButton>(event.button);
    if (button == SDL_GAMEPAD_BUTTON_GUIDE) {
        toggleMenu(true);
        return true;
    }
    if (!m_settings.backStartOpensMenu ||
        (button != SDL_GAMEPAD_BUTTON_BACK && button != SDL_GAMEPAD_BUTTON_START))
        return false;
    SDL_Gamepad *gamepad = SDL_GetGamepadFromID(event.which);
    const SDL_GamepadButton other = button == SDL_GAMEPAD_BUTTON_BACK ? SDL_GAMEPAD_BUTTON_START
                                                                      : SDL_GAMEPAD_BUTTON_BACK;
    if (!gamepad || !SDL_GetGamepadButton(gamepad, other))
        return false;
    toggleMenu(true);
    return true;
}

bool Overlay::captureEvent(const SDL_Event &event) {
    const auto finish = [&](gfx::PadBinding binding) {
        gfx::PadConfig config = m_settings.pad;
        config.slots(m_capture.input, m_capture.keyboard)[m_capture.slot] = binding;
        setPadConfig(config);
        m_capture.active = false;
        m_gamepadNavHeld = true;
    };
    if (m_capture.keyboard) {
        if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat) {
            if (reservedKey(event.key.scancode)) {
                notify(std::string(SDL_GetScancodeName(event.key.scancode)) + " is kept for the menu",
                       Icon::Warning);
                return true;
            }
            finish(gfx::PadBinding::key(event.key.scancode));
            return true;
        }
        return event.type == SDL_EVENT_KEY_UP;
    }
    switch (event.type) {
    case SDL_EVENT_KEY_DOWN:
        if (event.key.scancode == SDL_SCANCODE_ESCAPE)
            m_capture.active = false;
        return true;
    case SDL_EVENT_KEY_UP:
        return true;
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
        // A tap of B binds it; a hold cancels (drawCapturePrompt()).
        if (event.gbutton.button == SDL_GAMEPAD_BUTTON_EAST && m_capture.backDownAt >= 0.0) {
            m_capture.backDownAt = -1.0;
            finish(gfx::PadBinding::button(SDL_GAMEPAD_BUTTON_EAST));
        }
        return true;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        if (event.gbutton.button == SDL_GAMEPAD_BUTTON_EAST)
            m_capture.backDownAt = now();
        else if (event.gbutton.button != SDL_GAMEPAD_BUTTON_GUIDE)
            finish(gfx::PadBinding::button(static_cast<SDL_GamepadButton>(event.gbutton.button)));
        return true;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
        const auto axis = static_cast<SDL_GamepadAxis>(event.gaxis.axis);
        const bool trigger = axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER;
        // A deliberate push, well past drift and resting triggers.
        if (std::abs(static_cast<int>(event.gaxis.value)) > (trigger ? 16000 : 22000))
            finish(gfx::PadBinding::axis(axis, event.gaxis.value < 0 ? -1 : 1));
        return true;
    }
    default:
        return false;
    }
}

namespace {
bool inside(const ImGuiWindow *window, const ImGuiWindow *pane) {
    for (; window && pane; window = window->ParentWindow)
        if (window == pane)
            return true;
    return false;
}
} // namespace

// ImGui has already done its part of Back by now (ended a slider's
// adjustment, closed a combo or a menu, left the top bar's layer); this is
// the rest, judged by where the cursor was before it.
void Overlay::backOut(bool pad) {
    const NavBefore &before = m_navBefore;
    if (before.itemActive)
        return;
    if (before.modalOpen) {
        m_cancelQuit = true; // the only modal Back reaches: the capture prompt takes its own input
        return;
    }
    if (before.popupOpen)
        return;
    if (before.menuLayer) {
        // Out of the top bar, back to the window.
        if (m_settingsOpen) {
            m_focusSettings = true;
            m_focusList = true;
        }
        return;
    }
    if (m_aboutOpen) {
        m_aboutOpen = false;
        return;
    }
    if (m_shortcutsOpen) {
        m_shortcutsOpen = false;
        return;
    }
    if (m_settingsOpen && inside(before.window, m_pagePane)) {
        m_focusList = true;
        return;
    }
    // From the page list: a controller goes back to the game; Esc closes
    // the window first, leaving the top bar, as it always has.
    if (m_settingsOpen && !pad)
        m_settingsOpen = false;
    else
        setMenuOpen(false);
}

void Overlay::rememberNavigation() {
    const ImGuiContext &g = *m_context;
    m_navBefore.window = g.NavWindow;
    m_navBefore.menuLayer = g.NavLayer == ImGuiNavLayer_Menu;
    m_navBefore.itemActive = g.ActiveId != 0;
    m_navBefore.popupOpen = !g.OpenPopupStack.empty();
    m_navBefore.modalOpen = false;
    for (const ImGuiPopupData &popup : g.OpenPopupStack)
        if (popup.Window && (popup.Window->Flags & ImGuiWindowFlags_Modal))
            m_navBefore.modalOpen = true;

    // Past a slider, a list and the top bar, Back is backOut()'s alone.
    // ImGui's own, inside a child window, puts the cursor on the child as a
    // whole for a frame (a bright frame around the page or the list) before
    // backOut() moves it on. Owned keys still read as pressed with
    // IsKeyPressed(key, repeat), which asks for any owner.
    if (m_menuOpen && !m_capture.active && !m_navBefore.itemActive && !m_navBefore.popupOpen &&
        !m_navBefore.menuLayer) {
        static const ImGuiID kBackOwner = ImHashStr("dq8 back");
        ImGui::SetKeyOwner(ImGuiKey_Escape, kBackOwner);
        ImGui::SetKeyOwner(ImGuiKey_GamepadFaceRight, kBackOwner);
    }
}

// The same buttons as ImGui's SDL backend gives it, from every controller,
// with the left stick pressing the D-pad (past half way, let go under a
// third) and the right stick in the left stick's place, which ImGui scrolls
// with.
void Overlay::feedGamepad() {
    ImGuiIO &io = ImGui::GetIO();
    std::vector<SDL_Gamepad *> gamepads;
    for (const auto &controller : m_backend.padInput().controllers())
        if (SDL_Gamepad *gamepad = SDL_GetGamepadFromID(controller.id))
            gamepads.push_back(gamepad);
    if (gamepads.empty()) {
        io.BackendFlags &= ~ImGuiBackendFlags_HasGamepad;
        m_stickHeld = {};
        return;
    }
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    const auto button = [&](SDL_GamepadButton which) {
        return std::any_of(gamepads.begin(), gamepads.end(),
                           [&](SDL_Gamepad *gamepad) { return SDL_GetGamepadButton(gamepad, which); });
    };
    const auto axis = [&](SDL_GamepadAxis which) {
        // The one pushed furthest.
        float value = 0.0f;
        for (SDL_Gamepad *gamepad : gamepads) {
            const float v = static_cast<float>(SDL_GetGamepadAxis(gamepad, which)) / 32767.0f;
            if (std::fabs(v) > std::fabs(value))
                value = v;
        }
        return std::clamp(value, -1.0f, 1.0f);
    };
    static constexpr std::pair<ImGuiKey, SDL_GamepadButton> kButtons[] = {
        {ImGuiKey_GamepadStart, SDL_GAMEPAD_BUTTON_START},
        {ImGuiKey_GamepadBack, SDL_GAMEPAD_BUTTON_BACK},
        {ImGuiKey_GamepadFaceLeft, SDL_GAMEPAD_BUTTON_WEST},
        {ImGuiKey_GamepadFaceRight, SDL_GAMEPAD_BUTTON_EAST},
        {ImGuiKey_GamepadFaceUp, SDL_GAMEPAD_BUTTON_NORTH},
        {ImGuiKey_GamepadFaceDown, SDL_GAMEPAD_BUTTON_SOUTH},
        {ImGuiKey_GamepadL1, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER},
        {ImGuiKey_GamepadR1, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER},
        {ImGuiKey_GamepadL3, SDL_GAMEPAD_BUTTON_LEFT_STICK},
        {ImGuiKey_GamepadR3, SDL_GAMEPAD_BUTTON_RIGHT_STICK},
    };
    for (const auto &[key, which] : kButtons)
        io.AddKeyEvent(key, button(which));

    // The left stick by its larger axis, so a diagonal push moves one way.
    const float x = axis(SDL_GAMEPAD_AXIS_LEFTX), y = axis(SDL_GAMEPAD_AXIS_LEFTY);
    const bool horizontal = std::fabs(x) >= std::fabs(y);
    const float push[4] = {horizontal ? -x : 0.0f, horizontal ? x : 0.0f, horizontal ? 0.0f : -y,
                           horizontal ? 0.0f : y};
    for (size_t direction = 0; direction < 4; ++direction)
        m_stickHeld[direction] = push[direction] > (m_stickHeld[direction] ? 0.33f : 0.5f);
    static constexpr std::pair<ImGuiKey, SDL_GamepadButton> kDpad[] = {
        {ImGuiKey_GamepadDpadLeft, SDL_GAMEPAD_BUTTON_DPAD_LEFT},
        {ImGuiKey_GamepadDpadRight, SDL_GAMEPAD_BUTTON_DPAD_RIGHT},
        {ImGuiKey_GamepadDpadUp, SDL_GAMEPAD_BUTTON_DPAD_UP},
        {ImGuiKey_GamepadDpadDown, SDL_GAMEPAD_BUTTON_DPAD_DOWN},
    };
    for (size_t direction = 0; direction < 4; ++direction)
        io.AddKeyEvent(kDpad[direction].first, button(kDpad[direction].second) || m_stickHeld[direction]);

    const auto analog = [&](ImGuiKey key, float value) {
        constexpr float kDeadZone = 0.25f;
        const float past = std::max(0.0f, value - kDeadZone) / (1.0f - kDeadZone);
        io.AddKeyAnalogEvent(key, past > 0.0f, past);
    };
    analog(ImGuiKey_GamepadL2, axis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER));
    analog(ImGuiKey_GamepadR2, axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER));
    const float rx = axis(SDL_GAMEPAD_AXIS_RIGHTX), ry = axis(SDL_GAMEPAD_AXIS_RIGHTY);
    analog(ImGuiKey_GamepadLStickLeft, -rx);
    analog(ImGuiKey_GamepadLStickRight, rx);
    analog(ImGuiKey_GamepadLStickUp, -ry);
    analog(ImGuiKey_GamepadLStickDown, ry);
}

std::string Overlay::padButtonName(SDL_GamepadButton button) const {
    return bindingLabel(gfx::PadBinding::button(button), displayedControllerType());
}

void Overlay::render(SDL_GPUCommandBuffer *commands, SDL_GPUTexture *target, uint32_t, uint32_t) {
    if (!m_rendererReady)
        return;
    ImGui::SetCurrentContext(m_context);
    ++m_presents;
    runScript();
    updateScale();
    updateNavigation();
    rememberNavigation();
    ImGui_ImplSDLGPU3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    feedGamepad();
    ImGui::NewFrame();
    drawFrame();
    ImGui::Render();
    ImDrawData *data = ImGui::GetDrawData();
    // Always: this is also where the font atlas reaches the GPU.
    ImGui_ImplSDLGPU3_PrepareDrawData(data, commands);
    if (data->TotalVtxCount == 0)
        return;
    SDL_GPUColorTargetInfo info{};
    info.texture = target;
    info.load_op = SDL_GPU_LOADOP_LOAD;
    info.store_op = SDL_GPU_STOREOP_STORE;
    if (SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(commands, &info, 1u, nullptr)) {
        ImGui_ImplSDLGPU3_RenderDrawData(data, commands, pass);
        SDL_EndGPURenderPass(pass);
    }
}

void Overlay::updateScale() {
    float display = 1.0f;
    if (m_window) {
        // Points already carry a Retina display's density; a scaled Windows or
        // Linux desktop shows up as the display scale instead.
        const float density = SDL_GetWindowPixelDensity(m_window);
        const float scale = SDL_GetWindowDisplayScale(m_window);
        if (density > 0.0f && scale > 0.0f)
            display = scale / density;
    }
    const float scale = std::clamp(display * static_cast<float>(m_settings.uiScale) / 100.0f, 0.5f, 4.0f);
    if (std::fabs(scale - m_appliedScale) < 0.001f)
        return;
    applyStyle(scale);
    m_appliedScale = scale;
}

void Overlay::updateNavigation() {
    ImGuiIO &io = ImGui::GetIO();
    if (m_gamepadNavHeld) {
        // The button that opened the menu or ended a capture is still down;
        // navigation waits for its release so it does not activate anything.
        bool down = false;
        for (const auto &controller : m_backend.padInput().controllers())
            if (SDL_Gamepad *gamepad = SDL_GetGamepadFromID(controller.id))
                for (int button = 0; button < SDL_GAMEPAD_BUTTON_COUNT && !down; ++button)
                    down = SDL_GetGamepadButton(gamepad, static_cast<SDL_GamepadButton>(button));
        m_gamepadNavHeld = down;
    }
    io.ConfigFlags &= ~(ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad);
    if (m_menuOpen && !m_capture.active) {
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        if (!m_gamepadNavHeld)
            io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    }
}

void Overlay::drawFrame() {
    if (m_closePopups) {
        // All of them: a Quit question left open would stay on ImGui's
        // popup stack and take Back's next press.
        if (!ImGui::GetCurrentContext()->OpenPopupStack.empty())
            ImGui::ClosePopupToLevel(0, false);
        m_closePopups = false;
    }
    const ImGuiIO &io = ImGui::GetIO();
    if (m_menuOpen && !m_capture.active) {
        const bool padBack = (io.ConfigFlags & ImGuiConfigFlags_NavEnableGamepad) &&
                             ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false);
        const bool keyBack = (io.ConfigFlags & ImGuiConfigFlags_NavEnableKeyboard) &&
                             ImGui::IsKeyPressed(ImGuiKey_Escape, false);
        if (padBack || keyBack)
            backOut(padBack);
    }
    collectSavedScreenshots();
    sampleFrameRate();
    if (m_pendingScale != 0u) {
        // A new scale is applied as a frame is prepared, and a paused game
        // prepares none, so the wait only counts while it runs.
        if (pausing())
            m_pendingScaleSince = now();
        if (m_backend.activeResolutionScale() == m_pendingScale) {
            m_pendingScale = 0u;
        } else if (now() - m_pendingScaleSince > 3.0) {
            notify("The internal resolution could not be changed", Icon::Warning);
            m_pendingScale = 0u;
        }
    }
    // Y (the pad's top face button) moves between the top bar and the window.
    if (m_menuOpen && !m_capture.active && !m_navBefore.modalOpen &&
        (io.ConfigFlags & ImGuiConfigFlags_NavEnableGamepad) && ImGui::IsKeyPressed(ImGuiKey_GamepadFaceUp, false))
        toggleBarFocus();
    if (m_menuOpen)
        drawMenuBar();
    if (m_settingsOpen)
        drawSettingsWindow();
    if (m_aboutOpen)
        drawAbout();
    if (m_shortcutsOpen)
        drawShortcuts();
    if (m_statsOpen)
        drawStats();
    drawQuitPrompt();
    drawCapturePrompt();
    drawFps();
    drawSpeedBadge();
    drawPauseBanner();
    drawToasts();
    updateCursor();
    saveIfDirty(false);
}

void Overlay::drawMenuBar() {
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(em(0.75f), em(0.5f)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(em(1.35f), em(0.6f)));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(em(0.9f), em(0.45f)));
    if (ImGui::BeginMainMenuBar()) {
        ImDrawList *list = ImGui::GetWindowDrawList();
        const ImVec2 barMin = ImGui::GetWindowPos(), barSize = ImGui::GetWindowSize();
        // A white rule under the bar, like the edge of the game's windows.
        list->PushClipRect(barMin, ImVec2(barMin.x + barSize.x, barMin.y + barSize.y + 3.0f), false);
        list->AddLine(ImVec2(barMin.x, barMin.y + barSize.y - 1.0f),
                      ImVec2(barMin.x + barSize.x, barMin.y + barSize.y - 1.0f), withAlpha(palette::kBorder, 0.85f),
                      2.0f);
        list->PopClipRect();

        const float crown = em(1.15f);
        const ImVec2 cursor = ImGui::GetCursorScreenPos();
        drawIcon(list, Icon::Disc, ImVec2(cursor.x + crown * 0.5f, barMin.y + barSize.y * 0.5f), crown,
                 palette::kGold);
        ImGui::Dummy(ImVec2(crown, ImGui::GetTextLineHeight()));

        const auto beginMenu = [&](const char *label) {
            if (m_showMenu == label) {
                ImGui::OpenPopup(label);
                m_showMenu.clear();
            }
            return ImGui::BeginMenu(label);
        };
        if (beginMenu("Rogue Galaxy")) {
            if (menuItem("About Rogue Galaxy Recompiled"))
                m_aboutOpen = true;
            if (menuItem("Keyboard shortcuts"))
                m_shortcutsOpen = true;
            ImGui::Separator();
            if (menuItem("Quit", kQuitShortcut))
                m_quitAsk = true;
            ImGui::EndMenu();
        }
        if (beginMenu("Game")) {
            if (canPause() && menuItem("Pause", "F2", m_userPaused))
                setUserPaused(!m_userPaused);
            if (canChangeSpeed()) {
                if (menuItem("Fast-forward", "F4", m_fastForward))
                    setFastForward(!m_fastForward);
                if (beginSubmenu("Fast-forward speed")) {
                    for (const float speed : {2.0f, 3.0f, 4.0f, 0.0f}) {
                        if (menuItem(speedLabel(speed).c_str(), nullptr, m_settings.fastForwardSpeed == speed)) {
                            m_settings.fastForwardSpeed = speed;
                            markDirty();
                            if (m_fastForward)
                                m_host.setSpeed(speed);
                        }
                    }
                    ImGui::EndMenu();
                }
            }
            if (canPause() || canChangeSpeed())
                ImGui::Separator();
            if (menuItem("Mute sound", nullptr, m_settings.muted)) {
                m_settings.muted = !m_settings.muted;
                applyVolume();
                markDirty();
            }
            ImGui::Separator();
            if (menuItem("Take screenshot", "F12"))
                takeScreenshot(false);
            if (menuItem("Screenshot with menu", "Shift+F12"))
                takeScreenshot(true);
            if (menuItem("Open screenshots folder")) {
                std::error_code error;
                std::filesystem::create_directories(screenshotFolder(), error);
                SDL_OpenURL(("file://" + screenshotFolder()).c_str());
            }
            ImGui::EndMenu();
        }
        if (beginMenu("Settings")) {
            static constexpr const char *kPages[] = {"Display", "Sound", "Controls", "Game",
                                                     "Language", "Textures", "Interface"};
            for (int page = 0; page < static_cast<int>(SettingsPage::Count); ++page)
                if (menuItem(kPages[page], nullptr, m_settingsOpen && m_page == static_cast<SettingsPage>(page)))
                    openSettings(static_cast<SettingsPage>(page));
            ImGui::EndMenu();
        }
        if (beginMenu("View")) {
            if (menuItem("Fullscreen", "F11", m_settings.fullscreen))
                setFullscreen(!m_settings.fullscreen);
            if (menuItem("Frame rate", "F3", m_settings.showFps)) {
                m_settings.showFps = !m_settings.showFps;
                markDirty();
            }
            if (menuItem("Renderer statistics", nullptr, m_statsOpen))
                m_statsOpen = !m_statsOpen;
            ImGui::EndMenu();
        }
        if (beginMenu("Help")) {
            if (menuItem("Keyboard shortcuts"))
                m_shortcutsOpen = true;
            if (menuItem("Project page"))
                SDL_OpenURL(kProjectUrl);
            if (menuItem("Report a problem"))
                SDL_OpenURL(kIssuesUrl);
            ImGui::Separator();
            if (menuItem("About Rogue Galaxy Recompiled"))
                m_aboutOpen = true;
            ImGui::EndMenu();
        }
        drawStatus();
        ImGui::EndMainMenuBar();
    }
    ImGui::PopStyleVar(3);
}

// The bar's right end: pause state, frame rate, what is playing, the way out.
void Overlay::drawStatus() {
    const auto controllers = m_backend.padInput().controllers();
    const auto active = std::find_if(controllers.begin(), controllers.end(),
                                     [](const gfx::ControllerInfo &info) { return info.active; });
    std::string device = active != controllers.end() ? controllerFamilyName(active->type)
                         : m_settings.pad.keyboardEnabled ? "Keyboard" : "No input";
    if (active != controllers.end() && active->batteryPercent >= 0 &&
        (active->power == SDL_POWERSTATE_ON_BATTERY || active->power == SDL_POWERSTATE_CHARGING))
        device += " " + std::to_string(active->batteryPercent) + "%";
    char fps[32] = "- FPS";
    if (!pausing())
        std::snprintf(fps, sizeof(fps), "%.0f FPS", m_fps);
    std::string state = pausing() ? "Paused" : "";
    if (m_fastForward)
        state += (state.empty() ? "" : "  ") + std::string("Fast ") + speedLabel(m_settings.fastForwardSpeed);

    const ImGuiStyle &style = ImGui::GetStyle();
    const float iconSize = em(1.05f);
    const float gap = style.ItemSpacing.x;
    float width = iconSize + em(0.4f) + ImGui::CalcTextSize(device.c_str()).x + gap +
                  ImGui::CalcTextSize(fps).x + gap + ImGui::CalcTextSize("F1").x + em(0.8f);
    if (!state.empty())
        width += ImGui::CalcTextSize(state.c_str()).x + gap;
    const float x = ImGui::GetWindowWidth() - width - style.WindowPadding.x - em(0.4f);
    if (x <= ImGui::GetCursorPosX())
        return;
    ImGui::SetCursorPosX(x);
    if (!state.empty())
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(palette::kGold), "%s", state.c_str());
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(palette::kTextMuted), "%s", fps);
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float middle = ImGui::GetWindowPos().y + ImGui::GetWindowHeight() * 0.5f;
    drawIcon(ImGui::GetWindowDrawList(), active != controllers.end() ? Icon::Controls : Icon::Keyboard,
             ImVec2(at.x + iconSize * 0.5f, middle), iconSize, palette::kText);
    ImGui::Dummy(ImVec2(iconSize, ImGui::GetTextLineHeight()));
    ImGui::SameLine(0.0f, em(0.4f));
    ImGui::TextUnformatted(device.c_str());
    keycap("F1");
}

// Quit, from the page list or the top bar, asked first: a stray press would
// lose what was not saved. B or Esc answers no.
void Overlay::drawQuitPrompt() {
    constexpr const char *kName = "Quit?##quit";
    if (m_quitAsk) {
        ImGui::OpenPopup(kName);
        m_quitAsk = false;
    }
    const ImGuiViewport *viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x + viewport->Size.x * 0.5f, viewport->Pos.y + viewport->Size.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(em(1.6f), em(1.2f)));
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove;
    if (ImGui::BeginPopupModal(kName, nullptr, flags)) {
        decorateWindow();
        ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.2f);
        ImGui::TextUnformatted("Quit Rogue Galaxy?");
        ImGui::PopFont();
        mutedText("Progress since your last save is lost.");
        ImGui::Dummy(ImVec2(0.0f, em(0.6f)));
        // Cancel first: it is where the cursor starts.
        const ImVec2 buttonSize(em(7.0f), 0.0f);
        if (ImGui::Button("Cancel", buttonSize) || m_cancelQuit)
            ImGui::CloseCurrentPopup();
        ImGui::SetItemDefaultFocus();
        ImGui::SameLine();
        if (ImGui::Button("Quit", buttonSize)) {
            SDL_Event quit{};
            quit.type = SDL_EVENT_QUIT;
            SDL_PushEvent(&quit);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    m_cancelQuit = false;
    ImGui::PopStyleVar();
}

void Overlay::drawCapturePrompt() {
    constexpr const char *kName = "##capture";
    if (m_capture.active && now() - m_capture.startedAt > kCaptureSeconds)
        m_capture.active = false;
    // B held on a controller cancels; a tap binds it (captureEvent()).
    if (m_capture.active && m_capture.backDownAt >= 0.0 && now() - m_capture.backDownAt > 0.6) {
        m_capture.active = false;
        m_capture.backDownAt = -1.0;
        m_gamepadNavHeld = true;
    }
    if (m_capture.active && !ImGui::IsPopupOpen(kName))
        ImGui::OpenPopup(kName);
    const ImGuiViewport *viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x + viewport->Size.x * 0.5f, viewport->Pos.y + viewport->Size.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(em(1.6f), em(1.2f)));
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove;
    if (ImGui::BeginPopupModal(kName, nullptr, flags)) {
        if (!m_capture.active) {
            ImGui::CloseCurrentPopup();
        } else {
            decorateWindow();
            ImDrawList *list = ImGui::GetWindowDrawList();
            const float glyph = em(2.4f);
            const ImVec2 at = ImGui::GetCursorScreenPos();
            ImGui::Dummy(ImVec2(glyph, glyph));
            drawPadGlyph(list, m_capture.input, ImVec2(at.x + glyph * 0.5f, at.y + glyph * 0.5f), glyph);
            ImGui::SameLine(0.0f, em(1.0f));
            ImGui::BeginGroup();
            ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.2f);
            ImGui::Text("Press %s for %s", m_capture.keyboard ? "a key" : "a button",
                        gfx::padInputName(m_capture.input));
            ImGui::PopFont();
            if (m_capture.keyboard)
                mutedText("%s", "Any key works, even Esc.");
            else
                mutedText("On the controller. Hold %s or press Esc to cancel.",
                          padButtonName(SDL_GAMEPAD_BUTTON_EAST).c_str());
            ImGui::EndGroup();

            // Time left, as a gold line draining away.
            const float left = 1.0f - static_cast<float>((now() - m_capture.startedAt) / kCaptureSeconds);
            ImGui::Dummy(ImVec2(0.0f, em(0.4f)));
            const ImVec2 bar = ImGui::GetCursorScreenPos();
            const float width = ImGui::GetContentRegionAvail().x;
            list->AddRectFilled(bar, ImVec2(bar.x + width, bar.y + 3.0f), IM_COL32(255, 255, 255, 30), 2.0f);
            list->AddRectFilled(bar, ImVec2(bar.x + width * std::max(left, 0.0f), bar.y + 3.0f), palette::kGold, 2.0f);
            ImGui::Dummy(ImVec2(width, em(0.6f)));
            if (iconButton("Clear", Icon::Close)) {
                gfx::PadConfig config = m_settings.pad;
                config.slots(m_capture.input, m_capture.keyboard)[m_capture.slot] = {};
                setPadConfig(config);
                m_capture.active = false;
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel"))
                m_capture.active = false;
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();
}

void Overlay::sampleFrameRate() {
    const double time = now();
    const uint64_t renders = m_host.completedRenders ? m_host.completedRenders() : m_presents;
    // A paused game draws nothing; its rate is not zero, it is not running.
    if (m_rateStart == 0.0 || pausing()) {
        m_rateStart = time;
        m_rateRenders = renders;
        return;
    }
    const double elapsed = time - m_rateStart;
    if (elapsed < 0.5)
        return;
    m_fps = static_cast<float>(static_cast<double>(renders - m_rateRenders) / elapsed);
    m_fpsHistory[m_fpsHistoryNext++ % m_fpsHistory.size()] = m_fps;
    m_rateStart = time;
    m_rateRenders = renders;
}

// The game picture in points, below the menu bar; the whole window until a
// frame has been shown.
void Overlay::pictureBounds(ImVec2 &min, ImVec2 &max) const {
    const ImGuiViewport *viewport = ImGui::GetMainViewport();
    const ImVec2 scale = ImGui::GetIO().DisplayFramebufferScale;
    min = viewport->WorkPos;
    max = ImVec2(viewport->WorkPos.x + viewport->WorkSize.x, viewport->WorkPos.y + viewport->WorkSize.y);
    const gfx::SdlGpuRect rect = m_backend.displayRect();
    if (rect.width != 0u && scale.x > 0.0f && scale.y > 0.0f) {
        min = ImVec2(std::max(min.x, rect.x / scale.x), std::max(min.y, rect.y / scale.y));
        max = ImVec2(std::min(max.x, (rect.x + rect.width) / scale.x), std::min(max.y, (rect.y + rect.height) / scale.y));
    }
}

void Overlay::drawFps() {
    m_toastTop = 0.0f;
    m_fpsLeftBottom = 0.0f;
    if (!m_settings.showFps)
        return;
    // Inside the picture rather than over the bars around it.
    ImVec2 min, max;
    pictureBounds(min, max);
    const float margin = em(0.6f);
    const bool right = m_settings.fpsCorner == Corner::TopRight || m_settings.fpsCorner == Corner::BottomRight;
    const bool bottom = m_settings.fpsCorner == Corner::BottomLeft || m_settings.fpsCorner == Corner::BottomRight;
    ImGui::SetNextWindowPos(ImVec2(right ? max.x - margin : min.x + margin, bottom ? max.y - margin : min.y + margin),
                            ImGuiCond_Always, ImVec2(right ? 1.0f : 0.0f, bottom ? 1.0f : 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(em(0.7f), em(0.45f)));
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                                   ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing |
                                   ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoBringToFrontOnFocus;
    if (beginWindow("##fps", nullptr, flags)) {
        ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
        const bool paused = pausing();
        const ImU32 color = paused             ? palette::kTextMuted
                            : m_fps >= 29.0f   ? palette::kGood
                            : m_fps >= 20.0f   ? palette::kWarn
                                               : palette::kBad;
        ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.3f);
        if (paused)
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(color), "%s", "-");
        else
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(color), "%.0f", m_fps);
        ImGui::PopFont();
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        mutedText("FPS");
        float peak = 30.0f;
        for (const float value : m_fpsHistory)
            peak = std::max(peak, value);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_PlotLines, ImGui::ColorConvertU32ToFloat4(withAlpha(color, 0.85f)));
        ImGui::PlotLines("##history", m_fpsHistory.data(), static_cast<int>(m_fpsHistory.size()),
                         static_cast<int>(m_fpsHistoryNext % m_fpsHistory.size()), nullptr, 0.0f, peak * 1.1f,
                         ImVec2(em(5.5f), em(1.2f)));
        ImGui::PopStyleColor(2);
        if (m_settings.fpsCorner == Corner::TopRight)
            m_toastTop = ImGui::GetWindowPos().y + ImGui::GetWindowHeight();
        if (m_settings.fpsCorner == Corner::TopLeft)
            m_fpsLeftBottom = ImGui::GetWindowPos().y + ImGui::GetWindowHeight();
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

void Overlay::drawSpeedBadge() {
    if (!m_fastForward)
        return;
    ImVec2 min, max;
    pictureBounds(min, max);
    const float margin = em(0.6f);
    ImGui::SetNextWindowPos(ImVec2(min.x + margin, std::max(min.y, m_fpsLeftBottom) + margin), ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(em(0.7f), em(0.35f)));
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                                   ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing |
                                   ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings;
    if (beginWindow("##speed", nullptr, flags)) {
        ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
        icon(Icon::Speed, em(1.0f), palette::kGold);
        ImGui::SameLine(0.0f, em(0.45f));
        ImGui::TextUnformatted(speedLabel(m_settings.fastForwardSpeed).c_str());
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

void Overlay::drawPauseBanner() {
    if (!canPause() || m_menuOpen)
        return;
    const bool background = m_settings.pauseInBackground && !m_windowFocused;
    if (!m_userPaused && !background)
        return;
    const ImGuiViewport *viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x + viewport->Size.x * 0.5f, viewport->Pos.y + viewport->Size.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(em(1.6f), em(1.0f)));
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                                   ImGuiWindowFlags_NoNav | ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing;
    if (beginWindow("##paused", nullptr, flags)) {
        icon(Icon::Pause, em(1.6f), palette::kGold);
        ImGui::SameLine(0.0f, em(0.8f));
        ImGui::BeginGroup();
        ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.35f);
        ImGui::TextUnformatted("Paused");
        ImGui::PopFont();
        mutedText("%s", background && !m_userPaused ? "Until the window is focused again"
                        : padInUse()                ? "Menu > Game > Pause resumes"
                                                    : "Press F2 to resume");
        ImGui::EndGroup();
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

void Overlay::drawToasts() {
    const double time = now();
    m_toasts.erase(std::remove_if(m_toasts.begin(), m_toasts.end(),
                                  [&](const Toast &toast) { return time - toast.startedAt > toast.duration; }),
                   m_toasts.end());
    const ImGuiViewport *viewport = ImGui::GetMainViewport();
    float y = std::max(viewport->WorkPos.y, m_toastTop) + em(0.8f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                                   ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing |
                                   ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings;
    for (size_t slot = 0; slot < m_toasts.size(); ++slot) {
        const Toast &toast = m_toasts[slot];
        const float age = static_cast<float>(time - toast.startedAt);
        const float remaining = static_cast<float>(toast.duration) - age;
        const float alpha = std::clamp(std::min(age / 0.18f, remaining / 0.4f), 0.0f, 1.0f);
        const float slide = (1.0f - easeOut(std::min(age / 0.3f, 1.0f))) * em(2.5f);
        ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - em(0.8f) + slide, y),
                                ImGuiCond_Always, ImVec2(1.0f, 0.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(em(1.0f), em(0.6f)));
        char name[16];
        std::snprintf(name, sizeof(name), "##toast%zu", slot);
        if (ImGui::Begin(name, nullptr, flags)) {
            // Messages stay above the windows they may be talking about.
            ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
            decorateWindow(alpha);
            icon(toast.icon, em(1.15f), withAlpha(palette::kGold, alpha));
            ImGui::SameLine(0.0f, em(0.6f));
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(toast.text.c_str());
            y += ImGui::GetWindowHeight() + em(0.5f);
        }
        ImGui::End();
        ImGui::PopStyleVar(2);
    }
}

void Overlay::updateCursor() {
    if (m_menuOpen || !m_settings.hideIdleCursor)
        return;
    if (now() - m_lastMouseMotion > 2.5)
        ImGui::SetMouseCursor(ImGuiMouseCursor_None);
}

std::string Overlay::screenshotFolder() const {
    const std::filesystem::path settings(m_settingsPath);
    return (settings.has_parent_path() ? settings.parent_path() / "screenshots"
                                       : std::filesystem::path("screenshots"))
        .string();
}

void Overlay::takeScreenshot(bool withMenu) {
    m_backend.requestCapture(withMenu, [this](gfx::SdlGpuCapture capture) {
        if (capture.rgba.empty()) {
            notify("The screenshot could not be taken", Icon::Warning);
            return;
        }
        const std::string folder = screenshotFolder();
        std::error_code error;
        std::filesystem::create_directories(folder, error);
        std::string path = folder + "/RogueGalaxy_" + timestamp() + kImageExtension;
        for (int copy = 2; std::filesystem::exists(path, error); ++copy)
            path = folder + "/RogueGalaxy_" + timestamp() + "_" + std::to_string(copy) + kImageExtension;
        // Encoding a PNG takes longer than a frame, so it happens off thread.
        auto done = std::make_shared<std::atomic<bool>>(false);
        std::thread saver([this, capture = std::move(capture), path, done]() mutable {
            for (size_t alpha = 3u; alpha < capture.rgba.size(); alpha += 4u)
                capture.rgba[alpha] = 0xFFu;
            bool saved = false;
            if (SDL_Surface *surface = SDL_CreateSurfaceFrom(static_cast<int>(capture.width),
                                                             static_cast<int>(capture.height),
                                                             SDL_PIXELFORMAT_RGBA32, capture.rgba.data(),
                                                             static_cast<int>(capture.width * 4u))) {
                saved = saveImage(surface, path.c_str());
                SDL_DestroySurface(surface);
            }
            {
                std::lock_guard lock(m_savedMutex);
                m_saved.emplace_back(saved, path);
            }
            done->store(true, std::memory_order_release);
        });
        m_savers.push_back({std::move(saver), std::move(done)});
    });
}

void Overlay::collectSavedScreenshots() {
    std::vector<std::pair<bool, std::string>> saved;
    {
        std::lock_guard lock(m_savedMutex);
        saved.swap(m_saved);
    }
    for (const auto &[ok, path] : saved) {
        if (ok)
            notify("Screenshot saved: " + std::filesystem::path(path).filename().string(), Icon::Camera);
        else
            notify("The screenshot could not be saved", Icon::Warning);
    }
    for (auto it = m_savers.begin(); it != m_savers.end();) {
        if (!it->done->load(std::memory_order_acquire)) {
            ++it;
            continue;
        }
        it->thread.join();
        it = m_savers.erase(it);
    }
}

void Overlay::markDirty() {
    m_dirty = true;
    m_dirtyAt = now();
}

// What the window will open as next time: whether it is maximized, and a
// plain window's size and place, a maximized one keeping those it will be
// restored to. A fullscreen, minimized or hidden window says nothing new.
void Overlay::rememberWindow(const SDL_WindowEvent &event) {
    const SDL_WindowFlags flags = SDL_GetWindowFlags(m_window);
    if (flags & (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN))
        return;
    Settings &s = m_settings;
    const bool maximized = (flags & SDL_WINDOW_MAXIMIZED) != 0;
    bool changed = s.maximized != maximized;
    s.maximized = maximized;
    if (!maximized && event.type == SDL_EVENT_WINDOW_RESIZED &&
        (s.windowWidth != event.data1 || s.windowHeight != event.data2)) {
        s.windowWidth = event.data1;
        s.windowHeight = event.data2;
        changed = true;
    } else if (!maximized && event.type == SDL_EVENT_WINDOW_MOVED &&
               (!s.windowPlaced || s.windowX != event.data1 || s.windowY != event.data2)) {
        s.windowPlaced = true;
        s.windowX = event.data1;
        s.windowY = event.data2;
        changed = true;
    }
    if (changed)
        markDirty();
}

void Overlay::saveIfDirty(bool force) {
    if (!m_dirty || (!force && now() - m_dirtyAt < 0.75))
        return;
    m_dirty = false;
    if (!saveSettings(m_settingsPath, m_settings))
        std::fprintf(stderr, "[ui] could not save settings to %s\n", m_settingsPath.c_str());
}

void Overlay::applyVolume() {
    if (!m_host.setVolume)
        return;
    const bool silent = m_settings.muted || (m_settings.muteInBackground && !m_windowFocused);
    m_host.setVolume(silent ? 0.0f : static_cast<float>(m_settings.volume) / 100.0f);
}

bool Overlay::pausing() const {
    return canPause() && (m_userPaused || (m_settings.pauseInMenu && m_menuOpen) ||
                          (m_settings.pauseInBackground && !m_windowFocused));
}

void Overlay::applyPause() {
    const bool paused = pausing();
    if (!m_host.setPaused || paused == m_hostPaused)
        return;
    m_hostPaused = paused;
    m_host.setPaused(paused);
}

void Overlay::setFullscreen(bool fullscreen) {
    if (m_window && m_window == m_backend.window())
        SDL_SetWindowFullscreen(m_window, fullscreen);
    m_settings.fullscreen = fullscreen;
    markDirty();
}

void Overlay::setPadConfig(const gfx::PadConfig &config) {
    m_settings.pad = config;
    m_backend.padInput().setConfig(config);
    markDirty();
}

void Overlay::requestScale(uint32_t scale) {
    scale = std::clamp(scale, 1u, 8u);
    m_backend.requestResolutionScale(scale);
    m_pendingScale = scale != m_backend.activeResolutionScale() ? scale : 0u;
    m_pendingScaleSince = now();
    m_settings.resolutionScale = scale;
    markDirty();
}

SDL_GamepadType Overlay::displayedControllerType() const {
    const auto controllers = m_backend.padInput().controllers();
    for (const auto &controller : controllers)
        if (controller.active)
            return controller.type;
    return controllers.empty() ? SDL_GAMEPAD_TYPE_STANDARD : controllers.front().type;
}

} // namespace dq8::ui
