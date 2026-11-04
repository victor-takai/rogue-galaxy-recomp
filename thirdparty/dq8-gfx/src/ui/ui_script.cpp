// RG_UI_SCRIPT drives the menu as RG_INPUT drives the pad: a file or text of
// "<guest-frame> <command> [arguments]", '#' comments, ';' between entries.
// "<seconds>s" times an entry by the wall clock, which pauses do not stop.
// The commands are the names runStep() matches below.
#include "ui/ui_overlay.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace dq8::ui {
namespace {
std::string joined(const std::vector<std::string> &words) {
    std::string text;
    for (const auto &word : words)
        text += (text.empty() ? "" : " ") + word;
    return text;
}

template <typename T, size_t N>
bool pick(const std::string &name, const char *const (&names)[N], const T (&values)[N], T &out) {
    for (size_t index = 0; index < N; ++index)
        if (name == names[index]) {
            out = values[index];
            return true;
        }
    return false;
}
} // namespace

void Overlay::loadScript() {
    const char *value = std::getenv("RG_UI_SCRIPT");
    if (!value || !*value)
        return;
    std::string text = value;
    if (std::ifstream file(value); file) {
        std::ostringstream contents;
        contents << file.rdbuf();
        text = contents.str();
    }
    std::replace(text.begin(), text.end(), ';', '\n');
    std::istringstream lines(text);
    for (std::string line; std::getline(lines, line);) {
        line = line.substr(0, line.find('#'));
        std::istringstream words(line);
        std::string when;
        ScriptStep step;
        if (!(words >> when >> step.command))
            continue;
        if (when.back() == 's')
            step.seconds = std::atof(when.c_str());
        else
            step.frame = std::strtoull(when.c_str(), nullptr, 10);
        for (std::string word; words >> word;)
            step.arguments.push_back(word);
        m_script.push_back(std::move(step));
    }
    m_scriptStart = now();
    std::fprintf(stderr, "[ui] RG_UI_SCRIPT: %zu steps\n", m_script.size());
}

void Overlay::runScript() {
    if (m_script.empty())
        return;
    const uint64_t frame = m_host.guestFrame ? m_host.guestFrame() : m_presents;
    const double seconds = now() - m_scriptStart;
    // In written order among those due, so a script reads top to bottom.
    for (ScriptStep &step : m_script) {
        if (step.done || (step.seconds >= 0.0 ? step.seconds > seconds : step.frame > frame))
            continue;
        step.done = true;
        runStep(step, frame);
    }
}

void Overlay::runStep(const ScriptStep &step, uint64_t frame) {
    const std::string &command = step.command;
    const std::string first = step.arguments.empty() ? std::string() : step.arguments.front();
    const bool on = first != "off";
    std::fprintf(stderr, "[ui] script frame %llu, %.1f s: %s %s\n", static_cast<unsigned long long>(frame),
                 now() - m_scriptStart, command.c_str(), joined(step.arguments).c_str());
    static constexpr const char *kPages[] = {"display", "sound", "controls", "game",
                                             "language", "textures", "interface"};
    static constexpr SettingsPage kPageValues[] = {SettingsPage::Display, SettingsPage::Sound, SettingsPage::Controls,
                                                   SettingsPage::Game, SettingsPage::Language, SettingsPage::Textures,
                                                   SettingsPage::Interface};
    static constexpr const char *kAspects[] = {"auto", "4:3", "16:9", "native", "stretch"};
    static constexpr gfx::SdlGpuAspect kAspectValues[] = {gfx::SdlGpuAspect::Auto, gfx::SdlGpuAspect::Standard,
                                                          gfx::SdlGpuAspect::Wide, gfx::SdlGpuAspect::Native,
                                                          gfx::SdlGpuAspect::Stretch};
    static constexpr const char *kFilters[] = {"sharp", "smooth", "nearest", "fsr"};
    static constexpr gfx::SdlGpuFilter kFilterValues[] = {gfx::SdlGpuFilter::Sharp, gfx::SdlGpuFilter::Smooth,
                                                          gfx::SdlGpuFilter::Nearest, gfx::SdlGpuFilter::Fsr};
    static constexpr const char *kSides[] = {"black", "blurred", "image"};
    static constexpr gfx::SdlGpuSides kSidesValues[] = {gfx::SdlGpuSides::Black, gfx::SdlGpuSides::Blurred,
                                                        gfx::SdlGpuSides::Image};
    SettingsPage page{};
    // Settings the script changes are saved as the menu's own changes are.
    static constexpr const char *kSaved[] = {"aspect", "filter", "sides", "integer", "scale", "fps", "speed"};
    if (std::find(std::begin(kSaved), std::end(kSaved), command) != std::end(kSaved))
        markDirty();
    if (command == "menu") {
        setMenuOpen(on);
    } else if (command == "settings" && pick(first, kPages, kPageValues, page)) {
        openSettings(page);
    } else if (command == "bindings") {
        showBindings(first == "keyboard");
    } else if (command == "show") {
        showMenu(joined(step.arguments).c_str());
    } else if (command == "about") {
        openAbout();
    } else if (command == "shortcuts") {
        openShortcuts();
    } else if (command == "stats") {
        m_statsOpen = on;
    } else if (command == "pad") {
        // The menu as a controller player sees it, hints and all.
        m_lastInputPad = on;
    } else if (command == "capture") {
        for (size_t input = 0; input < gfx::kPadInputCount; ++input)
            if (first == gfx::padInputKey(static_cast<gfx::PadInput>(input)))
                beginBindingCapture(static_cast<gfx::PadInput>(input),
                                    step.arguments.size() > 1 && step.arguments[1] == "keyboard", 0);
    } else if (command == "aspect" && pick(first, kAspects, kAspectValues, m_settings.display.aspect)) {
        m_backend.setDisplayOptions(m_settings.display);
    } else if (command == "filter" && pick(first, kFilters, kFilterValues, m_settings.display.filter)) {
        m_backend.setDisplayOptions(m_settings.display);
    } else if (command == "sides" && pick(first, kSides, kSidesValues, m_settings.display.sides)) {
        m_backend.setDisplayOptions(m_settings.display);
    } else if (command == "integer") {
        m_settings.display.integerScale = on;
        m_backend.setDisplayOptions(m_settings.display);
    } else if (command == "scale") {
        requestScale(static_cast<uint32_t>(std::clamp(std::atoi(first.c_str()), 1, 8)));
    } else if (command == "fps") {
        m_settings.showFps = on;
    } else if (command == "pause") {
        setUserPaused(on);
    } else if (command == "fastforward") {
        setFastForward(on);
    } else if (command == "speed") {
        m_settings.fastForwardSpeed = static_cast<float>(std::atof(first.c_str()));
        if (m_fastForward && m_host.setSpeed)
            m_host.setSpeed(m_settings.fastForwardSpeed);
    } else if (command == "fullscreen") {
        setFullscreen(on);
    } else if (command == "key") {
        const SDL_Scancode scancode = SDL_GetScancodeFromName(joined(step.arguments).c_str());
        for (const bool down : {true, false}) {
            SDL_Event event{};
            event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
            event.key.windowID = m_window ? SDL_GetWindowID(m_window) : 0;
            event.key.scancode = scancode;
            event.key.key = SDL_GetKeyFromScancode(scancode, SDL_KMOD_NONE, false);
            event.key.down = down;
            SDL_PushEvent(&event);
        }
    } else if (command == "screenshot") {
        takeScreenshot(first == "menu");
    } else if (command == "notify") {
        notify(joined(step.arguments));
    } else if (command == "quit") {
        SDL_Event quit{};
        quit.type = SDL_EVENT_QUIT;
        SDL_PushEvent(&quit);
    } else {
        std::fprintf(stderr, "[ui] RG_UI_SCRIPT: cannot do '%s %s'\n", command.c_str(), joined(step.arguments).c_str());
    }
}

} // namespace dq8::ui
