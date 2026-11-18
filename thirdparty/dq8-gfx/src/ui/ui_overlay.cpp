#include "ui/ui_overlay.h"

#include "gfx/backends/sdlgpu/sdlgpu_input.h"
#include "ui/ui_art.h"
#include "ui/ui_style.h"
#include "ui/ui_widgets.h"

#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlgpu3.h>
#include <imgui_internal.h>

#include <algorithm>
#include <chrono>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <ctime>
#include <filesystem>

namespace dq8::ui {
namespace {
constexpr double kCaptureSeconds = 6.0;
constexpr const char *kQuitShortcut = "Alt+F4";
constexpr const char *kProjectUrl = "https://github.com/victor-takai/rogue-galaxy-recomp";
constexpr const char *kIssuesUrl = "https://github.com/victor-takai/rogue-galaxy-recomp/issues";

// Function keys the menu owns; with the game's own (HostServices::
// gameShortcuts: Rogue Galaxy's F6, the free camera), never bound: binding
// one would fight the shortcut.
constexpr SDL_Scancode kMenuKeys[] = {SDL_SCANCODE_F1, SDL_SCANCODE_F2,  SDL_SCANCODE_F3,
                                      SDL_SCANCODE_F4, SDL_SCANCODE_F11, SDL_SCANCODE_F12};

bool reservedKey(SDL_Scancode scancode, const std::vector<HostServices::Shortcut> &game) {
    return std::find(std::begin(kMenuKeys), std::end(kMenuKeys), scancode) != std::end(kMenuKeys) ||
           std::any_of(game.begin(), game.end(),
                       [&](const HostServices::Shortcut &shortcut) { return shortcut.key == scancode; });
}

// The reserved keys as the binding prompt names them, runs of function keys
// shortened: "F1 to F4, F6, F11 and F12".
std::string reservedKeysText(const std::vector<HostServices::Shortcut> &game) {
    std::vector<int> functions;
    std::vector<std::string> others;
    const auto add = [&](SDL_Scancode key) {
        if (key >= SDL_SCANCODE_F1 && key <= SDL_SCANCODE_F12)
            functions.push_back(static_cast<int>(key - SDL_SCANCODE_F1) + 1);
        else
            others.push_back(SDL_GetScancodeName(key));
    };
    for (const SDL_Scancode key : kMenuKeys)
        add(key);
    for (const HostServices::Shortcut &shortcut : game)
        add(shortcut.key);
    std::sort(functions.begin(), functions.end());
    functions.erase(std::unique(functions.begin(), functions.end()), functions.end());
    std::vector<std::string> parts;
    for (size_t first = 0; first < functions.size();) {
        size_t last = first;
        while (last + 1 < functions.size() && functions[last + 1] == functions[last] + 1)
            ++last;
        if (last - first >= 2) {
            parts.push_back("F" + std::to_string(functions[first]) + " to F" + std::to_string(functions[last]));
        } else {
            for (size_t index = first; index <= last; ++index)
                parts.push_back("F" + std::to_string(functions[index]));
        }
        first = last + 1;
    }
    parts.insert(parts.end(), others.begin(), others.end());
    std::string text;
    for (size_t index = 0; index < parts.size(); ++index)
        text += (index == 0 ? "" : index + 1 == parts.size() ? " and " : ", ") + parts[index];
    return text;
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
        notify("Press View + Menu for the menu", Icon::Info, 6.0);
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
        if (!m_startCaptured && option.number())
            m_startNumbers[option.key] = gameNumber(option);
    }
    m_startCaptured = true;
}

bool Overlay::waitsForRestart(const GameOption &option) const {
    if (option.number()) {
        const auto found = m_startNumbers.find(option.key);
        return option.restart && found != m_startNumbers.end() && found->second != gameNumber(option);
    }
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

int Overlay::gameNumber(const GameOption &option) const {
    const auto found = m_settings.gameNumbers.find(option.key);
    const int value = found != m_settings.gameNumbers.end() ? found->second : option.defaultNumber;
    return std::clamp(value, option.minimum, option.maximum);
}

bool Overlay::capturesInput() const { return m_menuOpen || m_capture.active || m_homeOpen; }

bool Overlay::wantsFrames() {
    if (!m_context)
        return false;
    // Queued input has to be consumed by a frame, or it replays later.
    const bool scriptWaiting = std::any_of(m_script.begin(), m_script.end(),
                                           [](const ScriptStep &step) { return !step.done && step.seconds >= 0.0; });
    return m_menuOpen || m_homeOpen || m_capture.active || !m_toasts.empty() || m_settings.showFps || m_statsOpen ||
           m_iconSheet || pausing() || scriptWaiting || !m_context->InputEventsQueue.empty();
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

void Overlay::openHome() {
    if (!canPause())
        return;
    m_homeOpen = true;
    m_toasts.clear(); // the menu's hint does not apply here
    // The button that started the game is still down: it must not press Play.
    m_gamepadNavHeld = true;
    applyPause();
}

void Overlay::closeHome() {
    if (!m_homeOpen)
        return;
    m_homeOpen = false;
    m_gamepadNavHeld = true;
    applyPause();
    // The hint that went unseen behind the home screen.
    if (m_host.steamDeck)
        notify("Press View + Menu for the menu", Icon::Info, 6.0);
}

void Overlay::toggleMenu(bool fromController) {
    if (m_homeOpen)
        return;
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

void Overlay::notify(std::string text, Icon kind, double seconds, std::string picture) {
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
    m_toasts.push_back({std::move(text), kind, now(), seconds, std::move(picture)});
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
    if (m_captureMouseDown != 0u) {
        if (event.type == SDL_EVENT_MOUSE_BUTTON_UP && event.button.button == m_captureMouseDown) {
            m_captureMouseDown = 0u;
            return true;
        }
        // Its release went elsewhere (the window lost the focus): nothing
        // to keep from the menu any more.
        if ((SDL_GetMouseState(nullptr, nullptr) & SDL_BUTTON_MASK(m_captureMouseDown)) == 0u)
            m_captureMouseDown = 0u;
    }
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
            if (reservedKey(event.key.scancode, m_host.gameShortcuts)) {
                notify(std::string(SDL_GetScancodeName(event.key.scancode)) + " is kept for a shortcut",
                       Icon::Warning);
                return true;
            }
            finish(gfx::PadBinding::key(event.key.scancode));
            return true;
        }
        // A mouse button (the five the pad reads), outside the prompt (its
        // own buttons take theirs), not a touch; or a whole notch of the
        // wheel, which only presses a button (a stick or Walk is held).
        if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && !m_capturePromptHovered &&
            event.button.which != SDL_TOUCH_MOUSEID && event.button.button >= SDL_BUTTON_LEFT &&
            event.button.button <= SDL_BUTTON_X2) {
            m_captureMouseDown = event.button.button;
            finish(gfx::PadBinding::mouse(event.button.button));
            return true;
        }
        if (event.type == SDL_EVENT_MOUSE_WHEEL && event.wheel.integer_y != 0) {
            if (gfx::padButtonMask(m_capture.input) == 0u) {
                notify("The wheel can only press a button", Icon::Warning);
                return true;
            }
            const bool flipped = event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED;
            finish(gfx::PadBinding::wheel((event.wheel.integer_y > 0) != flipped ? 1 : -1));
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

void Overlay::render(SDL_GPUCommandBuffer *commands, SDL_GPUTexture *target, uint32_t width, uint32_t height) {
    if (!m_rendererReady)
        return;
    m_targetWidth = width;
    m_targetHeight = height;
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
    if ((m_menuOpen || m_homeOpen) && !m_capture.active) {
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
    if (m_menuOpen && !m_homeOpen && !m_capture.active && !m_navBefore.modalOpen &&
        (io.ConfigFlags & ImGuiConfigFlags_NavEnableGamepad) && ImGui::IsKeyPressed(ImGuiKey_GamepadFaceUp, false))
        toggleBarFocus();
    // Over the home screen the settings stand alone: no top bar, and closing
    // them closes the menu with them.
    if (m_homeOpen && m_menuOpen && !m_settingsOpen)
        setMenuOpen(false);
    if (m_homeOpen)
        drawHome();
    if (m_menuOpen && !m_homeOpen)
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
    drawIconSheet();
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
            if (m_host.achievements && menuItem("Achievements"))
                openSettings(SettingsPage::Achievements);
            // Named as the window it opens, which lists the controller too.
            if (menuItem("Shortcuts"))
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
            if (menuItem("Screenshot with the menu", "Shift+F12"))
                takeScreenshot(true);
            if (menuItem("Open screenshots folder")) {
                std::error_code error;
                std::filesystem::create_directories(screenshotFolder(), error);
                SDL_OpenURL(("file://" + screenshotFolder()).c_str());
            }
            ImGui::EndMenu();
        }
        if (beginMenu("Settings")) {
            static constexpr const char *kPages[] = {"Display",  "Sound",        "Controls",  "Game",
                                                     "Language", "Achievements", "Interface"};
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
            if (menuItem("Shortcuts"))
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

namespace {
// The home screen's pieces (Steam Deck fork). All of them draw on the
// window's own list, so a hidden window or a missing artwork costs nothing.
float hash01(uint32_t n) {
    n = (n ^ 61u) ^ (n >> 16);
    n *= 9u;
    n ^= n >> 4;
    n *= 0x27d4eb2du;
    n ^= n >> 15;
    return static_cast<float>(n & 0xFFFFu) / 65535.0f;
}

float smooth(float x) {
    x = std::clamp(x, 0.0f, 1.0f);
    return x * x * (3.0f - 2.0f * x);
}

ImU32 mix(ImU32 a, ImU32 b, float t) {
    const auto lerp = [&](int shift) {
        const float x = static_cast<float>((a >> shift) & 0xFFu);
        const float y = static_cast<float>((b >> shift) & 0xFFu);
        return static_cast<ImU32>(x + (y - x) * t) << shift;
    };
    return lerp(IM_COL32_R_SHIFT) | lerp(IM_COL32_G_SHIFT) | lerp(IM_COL32_B_SHIFT) | lerp(IM_COL32_A_SHIFT);
}

// Text with tracking and a soft shadow; `size` in pixels. Returns its width.
float trackedText(ImDrawList *list, ImVec2 at, float size, float tracking, ImU32 from, ImU32 to, const char *text,
                  float fade) {
    ImFont *font = ImGui::GetFont();
    const size_t count = std::strlen(text);
    float x = at.x;
    for (size_t i = 0; i < count; ++i) {
        const char glyph[2] = {text[i], 0};
        const float t = count > 1 ? static_cast<float>(i) / static_cast<float>(count - 1) : 0.0f;
        const float advance = font->CalcTextSizeA(size, FLT_MAX, 0.0f, glyph).x;
        list->AddText(font, size, ImVec2(x + size * 0.04f, at.y + size * 0.05f), IM_COL32(0, 0, 10, static_cast<int>(150 * fade)),
                      glyph);
        list->AddText(font, size, ImVec2(x, at.y), withAlpha(mix(from, to, t), fade), glyph);
        x += advance + tracking;
    }
    return x - at.x - tracking;
}

// A spinning arc, for work in progress.
void spinner(ImDrawList *list, ImVec2 centre, float radius, ImU32 color) {
    const float t = static_cast<float>(ImGui::GetTime());
    list->PathClear();
    list->PathArcTo(centre, radius, t * 5.0f, t * 5.0f + 4.2f, 24);
    list->PathStroke(color, std::max(2.0f, radius * 0.28f));
}

// One button of the home screen: a glass plate that lights up under the
// cursor, with a gold bar and a sliding label; the first (Play) is gold.
// True while the changelog panel has the d-pad: the buttons then show no
// selection (the panel's border does).
bool gHomeReading = false;

bool homeButton(const char *id, const char *label, ImVec2 size, bool enabled, float fade,
                int adornment) {
    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_NavCursor, IM_COL32(0, 0, 0, 0));
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::Button(id, size);
    ImGui::PopStyleColor(4);
    const bool lit = enabled && !gHomeReading && (ImGui::IsItemHovered() || ImGui::IsItemFocused());
    const bool held = enabled && ImGui::IsItemActive();
    const ImVec2 max(min.x + size.x, min.y + size.y);

    float &glow = *ImGui::GetStateStorage()->GetFloatRef(ImGui::GetID(id), 0.0f);
    glow += ((lit ? 1.0f : 0.0f) - glow) * std::min(1.0f, ImGui::GetIO().DeltaTime * 14.0f);
    const float dim = (enabled ? 1.0f : 0.72f) * fade;
    const float round = size.y * 0.3f;
    ImDrawList *list = ImGui::GetWindowDrawList();

    // The plate.
    if (glow > 0.02f)
        drawShadow(list, min, max, round, glow * 0.7f * fade);
    // Glass at rest; the selected button turns gold-orange, whichever it is.
    const ImU32 top = mix(IM_COL32(16, 24, 60, 215), IM_COL32(255, 222, 118, 255), glow);
    const ImU32 bottom = mix(IM_COL32(8, 12, 36, 215), IM_COL32(244, 150, 56, 255), glow);
    const float press = held ? 1.0f : 0.0f;
    list->AddRectFilled(min, max, withAlpha(mix(top, bottom, 0.5f), dim), round);
    list->AddRectFilled(ImVec2(min.x + 2.0f, min.y + 2.0f), ImVec2(max.x - 2.0f, min.y + size.y * 0.5f),
                        withAlpha(IM_COL32(255, 255, 255, static_cast<int>(26 + 44 * glow)), dim), round - 2.0f, ImDrawFlags_RoundCornersTop);
    // A glint that crosses a lit button.
    list->PushClipRect(ImVec2(min.x + round, min.y + 3.0f), ImVec2(max.x - round, max.y - 3.0f), true);
    if (glow > 0.05f) {
        const float sweep = std::fmod(static_cast<float>(ImGui::GetTime()) * 0.7f, 1.8f) - 0.4f;
        const float gx = min.x + size.x * sweep;
        list->AddRectFilledMultiColor(ImVec2(gx - size.y, min.y), ImVec2(gx, max.y), IM_COL32(255, 255, 255, 0),
                                      IM_COL32(255, 255, 255, static_cast<int>(46 * glow * dim)),
                                      IM_COL32(255, 255, 255, static_cast<int>(46 * glow * dim)), IM_COL32(255, 255, 255, 0));
    }
    list->PopClipRect();
    list->AddRectFilled(min, max, IM_COL32(0, 0, 0, static_cast<int>(70 * press)), round);
    const ImU32 edge = mix(IM_COL32(205, 218, 255, 190), IM_COL32(255, 244, 200, 255), glow);
    list->AddRect(min, max, withAlpha(edge, dim), round, 2.0f + glow);

    // The label, nudged right as the plate lights up, and a chevron.
    ImFont *font = ImGui::GetFont();
    const float textSize = em(1.2f);
    const ImVec2 measured = font->CalcTextSizeA(textSize, FLT_MAX, 0.0f, label);
    const float slide = glow * em(0.35f);
    const ImVec2 at(min.x + em(1.5f) + slide, min.y + (size.y - measured.y) * 0.5f);
    const ImU32 ink = mix(palette::kText, IM_COL32(42, 22, 8, 255), glow);
    if (glow < 0.5f)
        list->AddText(font, textSize, ImVec2(at.x + 1.0f, at.y + 1.5f), IM_COL32(0, 0, 8, static_cast<int>(160 * dim)), label);
    list->AddText(font, textSize, at, withAlpha(ink, dim), label);
    if (glow > 0.05f && enabled) {
        const ImVec2 tip(max.x - em(1.1f) + slide * 0.5f, min.y + size.y * 0.5f);
        const ImU32 chevron = IM_COL32(42, 22, 8, 255);
        list->AddTriangleFilled(ImVec2(tip.x - em(0.55f), tip.y - em(0.4f)), ImVec2(tip.x, tip.y),
                                ImVec2(tip.x - em(0.55f), tip.y + em(0.4f)), withAlpha(chevron, glow * dim));
    }
    // Right side: a spinner while work goes on, or a pulsing gold dot.
    const ImVec2 mark(max.x - em(1.3f), min.y + size.y * 0.5f);
    if (adornment == 1)
        spinner(list, mark, em(0.45f), withAlpha(palette::kSky, dim));
    else if (adornment == 2 && glow < 0.05f) {
        const float pulse = 0.5f + 0.5f * std::sin(static_cast<float>(ImGui::GetTime()) * 3.5f);
        list->AddCircleFilled(mark, em(0.55f) + pulse * em(0.25f), withAlpha(palette::kGold, 0.18f * dim));
        list->AddCircleFilled(mark, em(0.3f), withAlpha(palette::kGold, dim));
    }
    return pressed;
}

// The changelog on the home screen (Steam Deck fork: added): the embedded
// CHANGELOG.md cut into versions, sections and bullets, the last few versions
// only, without the install notes and known issues.
struct ChangeLine {
    enum Kind { Version, Section, Bullet } kind;
    std::string text;
    std::string date; // Version only: the release's date, from "## X.Y.Z - DATE"
};

std::string plainMarkdown(const std::string &in) {
    std::string out;
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '`' || in[i] == '*')
            continue;
        if (in[i] == '[') {
            const size_t close = in.find("](", i);
            const size_t end = close == std::string::npos ? close : in.find(')', close);
            if (end != std::string::npos) {
                out += in.substr(i + 1, close - i - 1);
                i = end;
                continue;
            }
        }
        out += in[i];
    }
    return out;
}

// "2026-11-18" -> "18 nov. 2026"; anything else is shown as written.
std::string shortDate(const std::string &iso) {
    static const char *const kMonths[] = {"jan.", "feb.", "mar.", "apr.", "may", "jun.",
                                          "jul.", "aug.", "sep.", "oct.", "nov.", "dec."};
    int y = 0, m = 0, d = 0;
    char extra = 0;
    if (std::sscanf(iso.c_str(), "%d-%d-%d%c", &y, &m, &d, &extra) != 3 || m < 1 || m > 12 || d < 1 || d > 31)
        return iso;
    return std::to_string(d) + " " + kMonths[m - 1] + " " + std::to_string(y);
}

const std::vector<ChangeLine> &changelogLines() {
    static std::vector<ChangeLine> lines;
    static bool parsed = false;
    if (parsed)
        return lines;
    parsed = true;
    unsigned int size = 0;
    const char *data = homeChangelog(size);
    if (size <= 1u)
        return lines;
    const std::string text(data, size);
    constexpr int kVersions = 3;
    int versions = 0;
    bool skip = false;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos)
            end = text.size();
        std::string line = text.substr(pos, end - pos);
        pos = end + 1;
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.rfind("## ", 0) == 0) {
            if (++versions > kVersions)
                break;
            skip = false;
            std::string head = plainMarkdown(line.substr(3));
            std::string date;
            const size_t dash = head.find(" - ");
            if (dash != std::string::npos) {
                date = head.substr(dash + 3);
                head.resize(dash);
            }
            const size_t paren = head.find(" (");
            if (paren != std::string::npos)
                head.resize(paren);
            lines.push_back({ChangeLine::Version, head, shortDate(date)});
        } else if (line.rfind("### ", 0) == 0) {
            const std::string name = line.substr(4);
            skip = name.rfind("Install", 0) == 0 || name.rfind("Update from", 0) == 0 ||
                   name.rfind("Known issues", 0) == 0 || name.rfind("Highlights", 0) == 0;
            if (!skip && versions > 0)
                lines.push_back({ChangeLine::Section, name});
        } else if (!skip && versions > 0) {
            if (line.rfind("- ", 0) == 0)
                lines.push_back({ChangeLine::Bullet, plainMarkdown(line.substr(2))});
            else if (line.rfind("  ", 0) == 0 && !lines.empty() && lines.back().kind == ChangeLine::Bullet) {
                size_t first = line.find_first_not_of(' ');
                if (first != std::string::npos)
                    lines.back().text += " " + plainMarkdown(line.substr(first));
            }
        }
    }
    return lines;
}
} // namespace

// The screen the game starts on: Play, Update, Options, Quit. It covers the window
// with the game's artwork, slowly moving and lit, and keeps the game paused
// until Play. Everything is reached with the controller (A selects).
void Overlay::drawHome() {
    static bool logged = false;
    static double openedAt = -1.0;
    if (!logged) {
        logged = true;
        std::fprintf(stderr, "[ui] home screen drawn\n");
    }
    const double time = ImGui::GetTime();
    if (openedAt < 0.0)
        openedAt = time;
    const float fade = smooth(static_cast<float>((time - openedAt) / 0.9));
    const float t = static_cast<float>(time);

    const ImGuiViewport *viewport = ImGui::GetMainViewport();
    const ImVec2 origin = viewport->Pos;
    const ImVec2 view = viewport->Size;
    const ImVec2 corner(origin.x + view.x, origin.y + view.y);
    ImGui::SetNextWindowPos(origin);
    ImGui::SetNextWindowSize(view);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(4, 7, 20, 255));
    ImGui::Begin("##homebg", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
    {
        ImDrawList *list = ImGui::GetWindowDrawList();
        // The sky, when there is no artwork, or beneath its edges.
        list->AddRectFilledMultiColor(origin, corner, IM_COL32(10, 22, 70, 255), IM_COL32(70, 36, 40, 255),
                                      IM_COL32(8, 10, 30, 255), IM_COL32(20, 12, 34, 255));
        ImVec2 image(origin.x, origin.y), imageSize = view;
        ImTextureRef art;
        ImVec2 artPixels;
        if (homeArt(HomePicture::Background, art, artPixels)) {
            // Covers the window, breathing in and out and drifting a little,
            // the picture shown wider than the window so no edge ever shows.
            const float cover = std::max(view.x / artPixels.x, view.y / artPixels.y);
            const float zoom = 1.03f + 0.02f * std::sin(t * 0.11f) + 0.05f * (1.0f - fade);
            const float scale = cover * zoom;
            imageSize = ImVec2(artPixels.x * scale, artPixels.y * scale);
            const float spareX = imageSize.x - view.x, spareY = imageSize.y - view.y;
            image = ImVec2(origin.x - spareX * (0.5f + 0.45f * std::sin(t * 0.07f)),
                           origin.y - spareY * (0.5f + 0.38f * std::sin(t * 0.05f + 1.0f)));
            list->AddImage(art, image, ImVec2(image.x + imageSize.x, image.y + imageSize.y), ImVec2(0, 0), ImVec2(1, 1),
                           IM_COL32(255, 255, 255, 255));
            // The ship's two engines, pulsing: where the picture burns blue.
            const ImVec2 engines[2] = {ImVec2(0.565f, 0.523f), ImVec2(0.678f, 0.490f)};
            for (int i = 0; i < 2; ++i) {
                const ImVec2 p(image.x + engines[i].x * imageSize.x, image.y + engines[i].y * imageSize.y);
                const float pulse = 0.5f + 0.5f * std::sin(t * 4.3f + static_cast<float>(i) * 1.7f + 1.3f * std::sin(t * 9.0f));
                const float radius = view.y * (0.035f + 0.012f * pulse);
                for (int ring = 5; ring >= 1; --ring) {
                    const float k = static_cast<float>(ring) / 5.0f;
                    list->AddCircleFilled(p, radius * (0.4f + k * 1.8f),
                                          IM_COL32(90, 170, 255, static_cast<int>((10.0f + 26.0f * pulse) * (1.2f - k) * fade)), 28);
                }
                list->AddCircleFilled(p, radius * 0.28f, IM_COL32(220, 240, 255, static_cast<int>((90 + 120 * pulse) * fade)), 20);
            }
        }

        // Light streaks crossing the picture, like the ones it already has.
        for (uint32_t i = 0; i < 16; ++i) {
            const float speed = 0.05f + 0.07f * hash01(i * 7u + 1u);
            const float phase = std::fmod(t * speed + hash01(i * 13u + 5u), 1.0f);
            const float x = origin.x + view.x * (1.15f - phase * 1.5f + 0.2f * hash01(i * 3u + 9u));
            const float y = origin.y + view.y * (-0.1f + phase * 1.25f * (0.4f + hash01(i * 5u + 2u)));
            const float length = view.x * (0.08f + 0.14f * hash01(i * 11u + 3u));
            const float alpha = std::sin(phase * 3.14159f) * (0.10f + 0.16f * hash01(i * 17u + 4u)) * fade;
            const ImVec2 head(x, y), tail(x + length * 0.82f, y - length * 0.58f);
            list->AddLine(tail, head, i % 3u == 0u ? IM_COL32(255, 190, 110, static_cast<int>(255 * alpha))
                                                   : IM_COL32(190, 220, 255, static_cast<int>(255 * alpha)),
                          1.0f + 1.4f * hash01(i * 19u + 6u));
        }

        // Legibility: a dark veil behind the left column, the corners drawn in.
        const float veil = 0.62f;
        list->AddRectFilledMultiColor(origin, ImVec2(origin.x + view.x * veil, corner.y), IM_COL32(3, 6, 22, 215),
                                      IM_COL32(3, 6, 22, 0), IM_COL32(3, 6, 22, 0), IM_COL32(3, 6, 22, 215));
        list->AddRectFilledMultiColor(ImVec2(origin.x, corner.y - view.y * 0.30f), corner, IM_COL32(2, 4, 14, 0),
                                      IM_COL32(2, 4, 14, 0), IM_COL32(2, 4, 14, 200), IM_COL32(2, 4, 14, 200));
        list->AddRectFilledMultiColor(origin, ImVec2(corner.x, origin.y + view.y * 0.2f), IM_COL32(2, 4, 14, 150),
                                      IM_COL32(2, 4, 14, 150), IM_COL32(2, 4, 14, 0), IM_COL32(2, 4, 14, 0));

        // Stars and embers drifting up and across, twinkling.
        for (uint32_t i = 0; i < 90; ++i) {
            const float speed = 0.012f + 0.03f * hash01(i * 31u + 1u);
            const float rise = std::fmod(t * speed + hash01(i * 7u + 3u), 1.0f);
            const float x = origin.x + view.x * std::fmod(hash01(i * 5u + 2u) + rise * 0.12f, 1.0f);
            const float y = corner.y - view.y * rise * 1.05f;
            const float twinkle = 0.5f + 0.5f * std::sin(t * (1.2f + 2.5f * hash01(i * 3u + 8u)) + hash01(i) * 6.28f);
            const bool ember = i % 4u == 0u;
            const float alpha = std::sin(rise * 3.14159f) * (0.25f + 0.6f * twinkle) * fade;
            const float size = (ember ? 1.6f : 1.0f) + 1.6f * hash01(i * 23u + 4u);
            const ImU32 color = ember ? IM_COL32(255, 196, 110, static_cast<int>(255 * alpha))
                                      : IM_COL32(220, 232, 255, static_cast<int>(255 * alpha));
            list->AddCircleFilled(ImVec2(x, y), size * (view.y / 800.0f) * 1.2f, color, 8);
            if (size > 2.2f)
                list->AddCircleFilled(ImVec2(x, y), size * 2.6f * (view.y / 800.0f), withAlpha(color, 0.18f), 12);
        }
        // The foot of the screen: how to move, and the version.
        ImDrawList *foot = list;
        ImFont *font = ImGui::GetFont();
        const float small = em(0.9f);
        const std::string version = m_host.version;
        const float versionWidth = font->CalcTextSizeA(small, FLT_MAX, 0.0f, version.c_str()).x;
        const ImVec2 versionAt(corner.x - view.x * 0.035f - versionWidth, corner.y - view.y * 0.06f - small);
        foot->AddText(font, small, ImVec2(versionAt.x + 1.0f, versionAt.y + 1.0f), IM_COL32(0, 0, 8, static_cast<int>(180 * fade)),
                      version.c_str());
        foot->AddText(font, small, versionAt, IM_COL32(200, 212, 245, static_cast<int>(230 * fade)), version.c_str());
        // The first moments: up from black.
        list->AddRectFilled(origin, corner, IM_COL32(0, 0, 0, static_cast<int>(255 * (1.0f - fade))));
    }
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);

    const HostServices::HomeUpdate update = m_host.homeUpdate ? m_host.homeUpdate() : HostServices::HomeUpdate{};
    using State = HostServices::HomeUpdate::State;
    const bool working = update.state == State::Working;

    // What changed: the last versions' notes, on the right. The wheel or the
    // right stick scrolls; it takes no focus, so the buttons keep the pad.
    const std::vector<ChangeLine> &changes = changelogLines();
    // Stays visible behind the settings; it cannot be read then.
    if (m_settingsOpen)
        gHomeReading = false;
    if (!changes.empty() && view.x >= view.y * 1.3f) {
        const float panelW = std::min(view.x * 0.34f, em(30.0f));
        const float panelH = view.y * 0.72f;
        ImGui::SetNextWindowPos(ImVec2(corner.x - view.x * 0.05f, origin.y + view.y * 0.5f), ImGuiCond_Always,
                                ImVec2(1.0f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(panelW, panelH));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(em(1.0f), em(0.9f)));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, em(0.6f));
        ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(4, 8, 24, static_cast<int>(170 * fade)));
        if (ImGui::Begin("##homenews", nullptr,
                         ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBringToFrontOnFocus)) {
            ImDrawList *list = ImGui::GetWindowDrawList();
            const ImVec2 wmin = ImGui::GetWindowPos();
            if (!m_settingsOpen) // behind the settings window
                ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
            // Reading mode: while on, the panel owns the d-pad, the arrows and
            // the confirm keys, so ImGui's navigation (which only reads keys
            // nobody owns) leaves the buttons alone. Right from the buttons
            // enters it; Left or B leaves. Owning takes effect the next frame.
            bool &reading = gHomeReading;
            const ImGuiIO &io = ImGui::GetIO();
            const ImGuiID readId = ImGui::GetID("##newsread");
            const bool navOn = (io.ConfigFlags & (ImGuiConfigFlags_NavEnableGamepad | ImGuiConfigFlags_NavEnableKeyboard)) != 0;
            const auto pressed = [](ImGuiKey pad, ImGuiKey key, bool repeat) {
                return ImGui::IsKeyPressed(pad, repeat) || ImGui::IsKeyPressed(key, repeat);
            };
            if (!navOn) {
                reading = false;
            } else if (!reading) {
                reading = pressed(ImGuiKey_GamepadDpadRight, ImGuiKey_RightArrow, false);
            } else if (pressed(ImGuiKey_GamepadDpadLeft, ImGuiKey_LeftArrow, false) ||
                       ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) ||
                       ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
                reading = false;
            }
            if (reading) {
                for (const ImGuiKey key : {ImGuiKey_GamepadDpadUp, ImGuiKey_GamepadDpadDown, ImGuiKey_GamepadDpadLeft,
                                           ImGuiKey_GamepadDpadRight, ImGuiKey_GamepadFaceDown, ImGuiKey_UpArrow,
                                           ImGuiKey_DownArrow, ImGuiKey_LeftArrow, ImGuiKey_RightArrow, ImGuiKey_Enter,
                                           ImGuiKey_Space, ImGuiKey_KeypadEnter})
                    ImGui::SetKeyOwner(key, readId);
            }
            list->AddRect(wmin, ImVec2(wmin.x + panelW, wmin.y + panelH),
                          reading ? withAlpha(palette::kGold, fade) : withAlpha(IM_COL32(186, 204, 255, 90), fade), em(0.6f),
                          reading ? 2.0f : 1.0f);
            const float titleSize = em(1.0f);
            trackedText(list, ImGui::GetCursorScreenPos(), titleSize, em(0.22f), IM_COL32(186, 204, 255, 255),
                        IM_COL32(186, 204, 255, 255), "WHAT'S NEW", fade);
            ImGui::Dummy(ImVec2(0.0f, titleSize * 1.6f));
            if (navOn) {
                const char *hint = reading ? "Up / Down: scroll    Left: back" : "Right: read";
                const float hintSize = em(0.8f);
                ImFont *hintFont = ImGui::GetFont();
                const float hintWidth = hintFont->CalcTextSizeA(hintSize, FLT_MAX, 0.0f, hint).x;
                list->AddText(hintFont, hintSize, ImVec2(wmin.x + panelW - em(1.0f) - hintWidth, wmin.y + panelH - em(1.6f)),
                              reading ? withAlpha(palette::kGold, fade) : IM_COL32(200, 212, 245, static_cast<int>(200 * fade)),
                              hint);
            }
            ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(0, 0, 0, 0));
            if (ImGui::BeginChild("##homenewsbody", ImVec2(0.0f, -em(1.8f)), ImGuiChildFlags_None,
                                  ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBackground)) {
                const float stick = ImGui::GetKeyData(ImGuiKey_GamepadLStickDown)->AnalogValue -
                                    ImGui::GetKeyData(ImGuiKey_GamepadLStickUp)->AnalogValue;
                if (stick != 0.0f)
                    ImGui::SetScrollY(ImGui::GetScrollY() + stick * ImGui::GetIO().DeltaTime * em(30.0f));
                if (reading) {
                    // The d-pad (or the arrows) a line or two at a time, L1 / R1 a page.
                    float step = 0.0f;
                    if (pressed(ImGuiKey_GamepadDpadDown, ImGuiKey_DownArrow, true))
                        step += em(2.2f);
                    if (pressed(ImGuiKey_GamepadDpadUp, ImGuiKey_UpArrow, true))
                        step -= em(2.2f);
                    if (pressed(ImGuiKey_GamepadL1, ImGuiKey_PageUp, true))
                        step -= ImGui::GetWindowHeight() * 0.85f;
                    if (pressed(ImGuiKey_GamepadR1, ImGuiKey_PageDown, true))
                        step += ImGui::GetWindowHeight() * 0.85f;
                    if (step != 0.0f)
                        ImGui::SetScrollY(ImGui::GetScrollY() + step);
                }
                ImGui::PushTextWrapPos(0.0f);
                for (const ChangeLine &line : changes) {
                    if (line.kind == ChangeLine::Version) {
                        ImGui::Dummy(ImVec2(0.0f, em(0.3f)));
                        ImGui::PushStyleColor(ImGuiCol_Text, palette::kGold);
                        ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.15f);
                        ImGui::TextUnformatted(line.text.c_str());
                        ImGui::PopFont();
                        ImGui::PopStyleColor();
                        if (!line.date.empty()) {
                            ImGui::SameLine(0.0f, em(0.4f));
                            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(150, 165, 200, 255));
                            ImGui::TextUnformatted(("- " + line.date).c_str());
                            ImGui::PopStyleColor();
                        }
                    } else if (line.kind == ChangeLine::Section) {
                        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(186, 204, 255, 255));
                        ImGui::TextUnformatted(line.text.c_str());
                        ImGui::PopStyleColor();
                    } else {
                        ImGui::Bullet();
                        ImGui::TextUnformatted(line.text.c_str());
                        ImGui::Dummy(ImVec2(0.0f, em(0.15f)));
                    }
                }
                ImGui::PopTextWrapPos();
            }
            ImGui::EndChild();
            ImGui::PopStyleColor();
        }
        ImGui::End();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(2);
    }

    // The column: title, buttons, update status.
    const float column = std::min(em(21.0f), view.x * 0.46f);
    // Padding all round, so a button's edge and glow are not cut by the window.
    const float pad = em(1.0f);
    ImGui::SetNextWindowPos(ImVec2(origin.x + view.x * 0.07f - pad, origin.y + view.y * 0.5f), ImGuiCond_Always,
                            ImVec2(0.0f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(column + pad * 2.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad, pad));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, em(0.8f)));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(0, 0, 0, 0));
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBackground |
                                   ImGuiWindowFlags_AlwaysAutoResize;
    if (ImGui::Begin("##home", nullptr, flags)) {
        ImDrawList *list = ImGui::GetWindowDrawList();
        const ImVec2 top = ImGui::GetCursorScreenPos();
        const float slide = (1.0f - fade) * em(2.0f);

        // The title: the game's logo, or its name in text without the picture.
        float y = top.y;
        float titleWidth = column;
        ImTextureRef logo;
        ImVec2 logoPixels;
        if (homeArt(HomePicture::Logo, logo, logoPixels)) {
            titleWidth = column * 1.12f;
            const float height = titleWidth * logoPixels.y / logoPixels.x;
            const ImVec2 at(top.x - slide - column * 0.04f, y);
            list->AddImage(logo, at, ImVec2(at.x + titleWidth, at.y + height), ImVec2(0, 0), ImVec2(1, 1),
                           IM_COL32(255, 255, 255, static_cast<int>(255 * fade)));
            y += height + em(0.4f);
        } else {
            const float titleSize = em(3.6f);
            trackedText(list, ImVec2(top.x - slide, y), titleSize, titleSize * 0.06f, IM_COL32(255, 255, 255, 255),
                        IM_COL32(255, 232, 160, 255), "ROGUE", fade);
            y += titleSize * 1.08f;
            titleWidth = trackedText(list, ImVec2(top.x - slide, y), titleSize, titleSize * 0.06f,
                                     IM_COL32(255, 244, 205, 255), IM_COL32(255, 176, 76, 255), "GALAXY", fade);
            y += titleSize * 1.12f;
        }
        // A gold rule that draws itself in, with a spark at its end.
        const float rule = std::min(titleWidth, column) * smooth(static_cast<float>((time - openedAt - 0.2) / 0.8));
        list->AddRectFilledMultiColor(ImVec2(top.x, y), ImVec2(top.x + rule, y + 3.0f), withAlpha(palette::kGold, fade),
                                      IM_COL32(255, 168, 70, 0), IM_COL32(255, 168, 70, 0), withAlpha(palette::kGold, fade));
        list->AddCircleFilled(ImVec2(top.x + rule, y + 1.5f), 3.0f, IM_COL32(255, 244, 205, static_cast<int>(220 * fade)));
        y += em(0.7f);
        const float subSize = em(0.95f);
        trackedText(list, ImVec2(top.x, y), subSize, em(0.28f), IM_COL32(186, 204, 255, 255), IM_COL32(186, 204, 255, 255),
                    "STEAM DECK EDITION", fade);
        ImGui::Dummy(ImVec2(column, (y + subSize - top.y) + em(1.8f)));

        // The buttons.
        const ImVec2 buttonSize(column, em(3.1f));
        ImGui::BeginDisabled(working);
        if (homeButton("##play", "Play", buttonSize, !working, fade, 0))
            closeHome();
        ImGui::SetItemDefaultFocus();
        ImGui::EndDisabled();

        std::string label = "Update";
        if (update.state == State::Available)
            label = "Update to " + update.version;
        else if (update.state == State::UpToDate)
            label = "Up to date";
        else if (update.state == State::Checking)
            label = "Checking for updates...";
        else if (update.state == State::Offline)
            label = "Cannot check for updates";
        else if (working)
            label = "Updating...";
        else if (update.state == State::Failed)
            label = "Update failed: try again";
        else if (update.state == State::Done)
            label = "Restart to finish";
        const bool canUpdate = static_cast<bool>(m_host.startUpdate) &&
                               (update.state == State::Available || update.state == State::Failed);
        const int adornment = (update.state == State::Checking || working) ? 1 : update.state == State::Available ? 2 : 0;
        ImGui::BeginDisabled(!canUpdate);
        if (homeButton("##update", label.c_str(), buttonSize, canUpdate || update.state == State::UpToDate, fade,
                       adornment) && canUpdate)
            m_host.startUpdate();
        ImGui::EndDisabled();

        // Options: the settings window, over the home screen. Back returns here.
        ImGui::BeginDisabled(working);
        if (homeButton("##options", "Options", buttonSize, !working, fade, 0))
            openSettings(SettingsPage::Display);
        ImGui::EndDisabled();

        ImGui::BeginDisabled(working);
        if (homeButton("##quit", "Quit", buttonSize, !working, fade, 0)) {
            SDL_Event quit{};
            quit.type = SDL_EVENT_QUIT;
            SDL_PushEvent(&quit);
        }
        ImGui::EndDisabled();

        if (working || update.state == State::Failed || update.state == State::Done) {
            const ImVec2 at = ImGui::GetCursorScreenPos();
            if (working) {
                const float bar = em(0.55f);
                const ImVec2 end(at.x + column, at.y + bar);
                list->AddRectFilled(at, end, IM_COL32(255, 255, 255, 36), bar * 0.5f);
                list->PushClipRect(at, end, true);
                if (update.progress < 0.0f) {
                    const float run = std::fmod(t * 0.8f, 1.4f) - 0.2f;
                    const float from = at.x + column * run;
                    list->AddRectFilledMultiColor(ImVec2(from, at.y), ImVec2(from + column * 0.3f, end.y), IM_COL32(244, 203, 82, 0),
                                                  palette::kGold, palette::kGold, IM_COL32(244, 203, 82, 0));
                } else {
                    const float done = std::clamp(update.progress, 0.0f, 1.0f);
                    list->AddRectFilledMultiColor(at, ImVec2(at.x + column * done, end.y), IM_COL32(255, 168, 70, 255),
                                                  palette::kGold, palette::kGold, IM_COL32(255, 168, 70, 255));
                }
                list->PopClipRect();
                ImGui::Dummy(ImVec2(column, bar));
            }
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + column);
            ImGui::PushStyleColor(ImGuiCol_Text, update.state == State::Failed ? palette::kBad : palette::kText);
            ImGui::TextUnformatted(update.text.c_str());
            ImGui::PopStyleColor();
            ImGui::PopTextWrapPos();
        }
        ImGui::End();
    } else {
        ImGui::End();
    }
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
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
    m_capturePromptHovered = false;
    if (ImGui::BeginPopupModal(kName, nullptr, flags)) {
        if (!m_capture.active) {
            ImGui::CloseCurrentPopup();
        } else {
            m_capturePromptHovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows);
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
            if (m_capture.keyboard) {
                // Wrapped, so the prompt keeps a reading width at any menu size.
                ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + em(32.0f));
                mutedText("Any key, Esc included, but %s; a mouse button outside this box, or the wheel.",
                          reservedKeysText(m_host.gameShortcuts).c_str());
                ImGui::PopTextWrapPos();
            } else
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

// The window in points, below the menu bar. The frame-rate counter and the
// speed badge sit in its corners, as the notifications do, rather than in
// the picture's: with bars at the sides (4:3 in a wide window) they moved
// in with the picture (Rogue Galaxy: changed).
void Overlay::windowBounds(ImVec2 &min, ImVec2 &max) const {
    const ImGuiViewport *viewport = ImGui::GetMainViewport();
    min = viewport->WorkPos;
    max = ImVec2(viewport->WorkPos.x + viewport->WorkSize.x, viewport->WorkPos.y + viewport->WorkSize.y);
}

void Overlay::drawFps() {
    m_toastTop = 0.0f;
    if (!m_settings.showFps)
        return;
    ImVec2 min, max;
    windowBounds(min, max);
    const float margin = em(0.6f);
    // Always the top right corner.
    ImGui::SetNextWindowPos(ImVec2(max.x - margin, min.y + margin), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
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
        m_toastTop = ImGui::GetWindowPos().y + ImGui::GetWindowHeight();
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

void Overlay::drawSpeedBadge() {
    if (!m_fastForward)
        return;
    ImVec2 min, max;
    windowBounds(min, max);
    const float margin = em(0.6f);
    ImGui::SetNextWindowPos(ImVec2(min.x + margin, min.y + margin), ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(em(0.7f), em(0.35f)));
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                                   ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing |
                                   ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings;
    if (beginWindow("##speed", nullptr, flags)) {
        ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
        const IconRoom room = iconRoom(em(1.0f));
        ImGui::SameLine(0.0f, em(0.45f));
        ImGui::TextUnformatted(speedLabel(m_settings.fastForwardSpeed).c_str());
        drawIconBeside(room, Icon::Speed, palette::kGold);
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

void Overlay::drawPauseBanner() {
    if (!canPause() || m_menuOpen || m_homeOpen)
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
        const IconRoom room = iconRoom(em(1.6f));
        ImGui::SameLine(0.0f, em(0.8f));
        ImGui::BeginGroup();
        ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.35f);
        ImGui::TextUnformatted("Paused");
        ImGui::PopFont();
        mutedText("%s", background && !m_userPaused ? "Until the game's window is active again"
                        : padInUse()                ? "Open the menu: Game > Pause in the top bar resumes"
                                                    : "Press F2 to resume");
        ImGui::EndGroup();
        drawIconBeside(room, Icon::Pause, palette::kGold);
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
            ImTextureRef picture;
            ImVec2 pictureSize;
            if (!toast.picture.empty() && fileTexture(toast.picture.c_str(), picture, pictureSize)) {
                // An achievement's badge, with the text beside it.
                const float side = em(2.6f);
                ImGui::Image(picture, ImVec2(side, side));
                ImGui::SameLine(0.0f, em(0.7f));
                ImGui::BeginGroup();
                ImGui::Dummy(ImVec2(0.0f, std::max(0.0f, (side - ImGui::GetTextLineHeight()) * 0.5f - em(0.2f))));
                ImGui::TextUnformatted(toast.text.c_str());
                ImGui::EndGroup();
            } else {
                const IconRoom room = iconRoom(em(1.15f));
                ImGui::SameLine(0.0f, em(0.6f));
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(toast.text.c_str());
                drawIconBeside(room, toast.icon, withAlpha(palette::kGold, alpha));
            }
            y += ImGui::GetWindowHeight() + em(0.5f);
        }
        ImGui::End();
        ImGui::PopStyleVar(2);
    }
}

// Every icon at the sizes the menu draws them (a button's, a notice's, a
// window title's, the About window's), with its name: RG_UI_SCRIPT's
// "icons", for a screenshot when an icon changes.
void Overlay::drawIconSheet() {
    if (!m_iconSheet)
        return;
    const ImGuiViewport *viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x + viewport->Size.x * 0.5f, viewport->Pos.y + viewport->Size.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(em(1.2f), em(1.0f)));
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                                   ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoFocusOnAppearing;
    if (beginWindow("##icons", nullptr, flags)) {
        ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
        constexpr int kColumns = 6;
        if (ImGui::BeginTable("##sheet", kColumns, ImGuiTableFlags_None)) {
            ImDrawList *list = ImGui::GetWindowDrawList();
            for (int index = 0; index < kIconCount; ++index) {
                ImGui::TableNextColumn();
                const Icon kind = static_cast<Icon>(index);
                const ImVec2 at = ImGui::GetCursorScreenPos();
                const float row = em(3.0f);
                float x = at.x;
                for (const float size : {em(0.95f), em(1.15f), em(1.4f), em(3.0f)}) {
                    drawIcon(list, kind, ImVec2(x + size * 0.5f, at.y + row * 0.5f), size, palette::kGold);
                    x += size + em(0.5f);
                }
                ImGui::Dummy(ImVec2(x - at.x, row));
                mutedText("%s", iconName(kind));
            }
            ImGui::EndTable();
        }
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

void Overlay::updateCursor() {
    if (m_menuOpen)
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
    const bool silent = m_settings.muted;
    m_host.setVolume(silent ? 0.0f : static_cast<float>(m_settings.volume) / 100.0f);
}

bool Overlay::pausing() const {
    return canPause() && (m_homeOpen || m_userPaused || (m_settings.pauseInMenu && m_menuOpen) ||
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
