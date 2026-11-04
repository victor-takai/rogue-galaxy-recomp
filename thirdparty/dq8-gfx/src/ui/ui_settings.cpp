#include "ui/ui_settings.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace dq8::ui {
namespace {
using gfx::ControllerSelection;
using gfx::PadInput;
using gfx::SdlGpuAspect;
using gfx::SdlGpuFilter;

template <typename T>
struct Named {
    T value;
    const char *name;
};
constexpr Named<SdlGpuAspect> kAspects[] = {
    {SdlGpuAspect::Auto, "auto"}, {SdlGpuAspect::Standard, "4:3"}, {SdlGpuAspect::Wide, "16:9"},
    {SdlGpuAspect::Native, "native"}, {SdlGpuAspect::Stretch, "stretch"}};
constexpr Named<SdlGpuFilter> kFilters[] = {
    {SdlGpuFilter::Sharp, "sharp"}, {SdlGpuFilter::Smooth, "smooth"}, {SdlGpuFilter::Nearest, "nearest"},
    {SdlGpuFilter::Fsr, "fsr"}};
constexpr Named<gfx::SdlGpuSides> kSides[] = {
    {gfx::SdlGpuSides::Black, "black"}, {gfx::SdlGpuSides::Blurred, "blurred"}, {gfx::SdlGpuSides::Image, "image"}};
constexpr Named<SDL_GPUPresentMode> kPresentModes[] = {
    {SDL_GPU_PRESENTMODE_VSYNC, "vsync"}, {SDL_GPU_PRESENTMODE_MAILBOX, "mailbox"},
    {SDL_GPU_PRESENTMODE_IMMEDIATE, "immediate"}};
constexpr Named<ControllerSelection> kSelections[] = {
    {ControllerSelection::Any, "any"}, {ControllerSelection::One, "one"}, {ControllerSelection::None, "none"}};
constexpr Named<Corner> kCorners[] = {
    {Corner::TopLeft, "top-left"}, {Corner::TopRight, "top-right"},
    {Corner::BottomLeft, "bottom-left"}, {Corner::BottomRight, "bottom-right"}};

template <typename T, size_t N>
const char *nameOf(const Named<T> (&table)[N], T value) {
    for (const auto &entry : table)
        if (entry.value == value)
            return entry.name;
    return table[0].name;
}

template <typename T, size_t N>
void parseNamed(const Named<T> (&table)[N], const std::string &text, T &out) {
    for (const auto &entry : table)
        if (text == entry.name)
            out = entry.value;
}

std::string trim(const std::string &text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return {};
    return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1u);
}

void parseBool(const std::string &text, bool &out) {
    if (text == "1" || text == "true" || text == "yes")
        out = true;
    else if (text == "0" || text == "false" || text == "no")
        out = false;
}

template <typename T>
void parseNumber(const std::string &text, T &out, T low, T high) {
    char *end = nullptr;
    const double value = std::clamp(std::strtod(text.c_str(), &end), double(low), double(high));
    // NaN comes through the clamp unchanged; the range check turns it away.
    if (end != text.c_str() && *end == '\0' && value >= double(low) && value <= double(high))
        out = static_cast<T>(value);
}

// "cross.1" in [controller] or [keyboard]: slot 1 or 2 of one pad input.
void parseBinding(gfx::PadConfig &pad, bool keyboard, const std::string &key, const std::string &value) {
    const auto dot = key.rfind('.');
    if (dot == std::string::npos || dot + 2u != key.size() || (key[dot + 1u] != '1' && key[dot + 1u] != '2'))
        return;
    const std::string name = key.substr(0, dot);
    const auto binding = gfx::padBindingFromString(value);
    if (!binding)
        return; // unreadable: the binding stays as it was
    for (size_t input = 0; input < gfx::kPadInputCount; ++input)
        if (name == gfx::padInputKey(static_cast<PadInput>(input)))
            pad.slots(static_cast<PadInput>(input), keyboard)[key[dot + 1u] - '1'] = *binding;
}

void apply(Settings &s, const std::string &section, const std::string &key, const std::string &value) {
    if (section == "window") {
        if (key == "fullscreen") parseBool(value, s.fullscreen);
        else if (key == "width") parseNumber(value, s.windowWidth, 0, 16384);
        else if (key == "height") parseNumber(value, s.windowHeight, 0, 16384);
        else if (key == "maximized") parseBool(value, s.maximized);
        else if (key == "x" || key == "y") {
            parseNumber(value, key == "x" ? s.windowX : s.windowY, -65536, 65536);
            s.windowPlaced = true;
        }
    } else if (section == "display") {
        if (key == "scale") parseNumber(value, s.resolutionScale, 1u, 8u);
        else if (key == "aspect") parseNamed(kAspects, value, s.display.aspect);
        else if (key == "filter") parseNamed(kFilters, value, s.display.filter);
        else if (key == "sides") parseNamed(kSides, value, s.display.sides);
        else if (key == "integer_scale") parseBool(value, s.display.integerScale);
        else if (key == "remove_line_blend") parseBool(value, s.display.removeLineBlend);
        else if (key == "present") parseNamed(kPresentModes, value, s.display.presentMode);
    } else if (section == "sound") {
        if (key == "volume") parseNumber(value, s.volume, 0, 100);
        else if (key == "muted") parseBool(value, s.muted);
        else if (key == "mute_in_background") parseBool(value, s.muteInBackground);
    } else if (section == "controls") {
        if (key == "keyboard") parseBool(value, s.pad.keyboardEnabled);
        else if (key == "controller") parseNamed(kSelections, value, s.pad.controllers);
        else if (key == "controller_guid") s.pad.controllerGuid = value;
        else if (key == "controller_serial") s.pad.controllerSerial = value;
        else if (key == "controller_name") s.pad.controllerName = value;
        else if (key == "stick_dead_zone") {
            float percent = s.pad.stickDeadZone * 100.0f;
            parseNumber(value, percent, 0.0f, 50.0f);
            s.pad.stickDeadZone = percent / 100.0f;
        } else if (key == "trigger_threshold") {
            float percent = s.pad.triggerThreshold * 100.0f;
            parseNumber(value, percent, 5.0f, 95.0f);
            s.pad.triggerThreshold = percent / 100.0f;
        }
        else if (key == "invert_camera_x") parseBool(value, s.pad.invertCameraX);
        else if (key == "invert_camera_y") parseBool(value, s.pad.invertCameraY);
        else if (key == "back_start_menu") parseBool(value, s.backStartOpensMenu);
    } else if (section == "controller" || section == "keyboard") {
        parseBinding(s.pad, section == "keyboard", key, value);
    } else if (section == "interface") {
        if (key == "ui_scale") parseNumber(value, s.uiScale, 50, 250);
        else if (key == "show_fps") parseBool(value, s.showFps);
        else if (key == "fps_corner") parseNamed(kCorners, value, s.fpsCorner);
        else if (key == "notifications") parseBool(value, s.notifications);
        else if (key == "pause_in_menu") parseBool(value, s.pauseInMenu);
        else if (key == "pause_in_background") parseBool(value, s.pauseInBackground);
        else if (key == "hide_idle_cursor") parseBool(value, s.hideIdleCursor);
    } else if (section == "game") {
        if (key == "fast_forward_speed") parseNumber(value, s.fastForwardSpeed, 0.0f, 16.0f);
        else {
            // Any other key is one of the game's switches or numbers (Rogue
            // Galaxy: added).
            if (value == "1" || value == "true" || value == "yes" || value == "0" || value == "false" ||
                value == "no") {
                bool on = false;
                parseBool(value, on);
                s.gameOptions[key] = on;
            }
            char *end = nullptr;
            const long number = std::strtol(value.c_str(), &end, 10);
            if (end != value.c_str() && *end == '\0' && number >= INT32_MIN && number <= INT32_MAX)
                s.gameNumbers[key] = static_cast<int>(number);
        }
    }
}
} // namespace

std::string serializeSettings(const Settings &s) {
    std::ostringstream out;
    const auto flag = [](bool value) { return value ? "1" : "0"; };
    out << "; Rogue Galaxy Recompiled settings, written by the in-game menu (F1).\n\n"
        << "[window]\nfullscreen = " << flag(s.fullscreen) << "\nmaximized = " << flag(s.maximized)
        << "\nwidth = " << s.windowWidth << "\nheight = " << s.windowHeight << "\n";
    if (s.windowPlaced)
        out << "x = " << s.windowX << "\ny = " << s.windowY << "\n";
    out << "\n[display]\nscale = " << s.resolutionScale << "\naspect = " << nameOf(kAspects, s.display.aspect)
        << "\nfilter = " << nameOf(kFilters, s.display.filter)
        << "\nsides = " << nameOf(kSides, s.display.sides)
        << "\ninteger_scale = " << flag(s.display.integerScale)
        << "\nremove_line_blend = " << flag(s.display.removeLineBlend)
        << "\npresent = " << nameOf(kPresentModes, s.display.presentMode) << "\n\n"
        << "[sound]\nvolume = " << s.volume << "\nmuted = " << flag(s.muted)
        << "\nmute_in_background = " << flag(s.muteInBackground) << "\n\n"
        << "[controls]\nkeyboard = " << flag(s.pad.keyboardEnabled)
        << "\ncontroller = " << nameOf(kSelections, s.pad.controllers)
        << "\ncontroller_guid = " << s.pad.controllerGuid << "\ncontroller_serial = " << s.pad.controllerSerial
        << "\ncontroller_name = " << s.pad.controllerName
        << "\nstick_dead_zone = " << s.pad.stickDeadZone * 100.0f
        << "\ntrigger_threshold = " << s.pad.triggerThreshold * 100.0f
        << "\ninvert_camera_x = " << flag(s.pad.invertCameraX)
        << "\ninvert_camera_y = " << flag(s.pad.invertCameraY)
        << "\nback_start_menu = " << flag(s.backStartOpensMenu) << "\n";
    for (const bool keyboard : {false, true}) {
        out << "\n[" << (keyboard ? "keyboard" : "controller") << "]\n";
        for (size_t input = 0; input < gfx::kPadInputCount; ++input) {
            const auto &slots = s.pad.slots(static_cast<PadInput>(input), keyboard);
            for (size_t slot = 0; slot < slots.size(); ++slot)
                out << gfx::padInputKey(static_cast<PadInput>(input)) << '.' << slot + 1u << " = "
                    << gfx::padBindingToString(slots[slot]) << '\n';
        }
    }
    out << "\n[interface]\nui_scale = " << s.uiScale << "\nshow_fps = " << flag(s.showFps)
        << "\nfps_corner = " << nameOf(kCorners, s.fpsCorner) << "\nnotifications = " << flag(s.notifications)
        << "\npause_in_menu = " << flag(s.pauseInMenu)
        << "\npause_in_background = " << flag(s.pauseInBackground)
        << "\nhide_idle_cursor = " << flag(s.hideIdleCursor) << '\n'
        << "\n[game]\nfast_forward_speed = " << s.fastForwardSpeed << '\n';
    for (const auto &[key, on] : s.gameOptions)
        out << key << " = " << flag(on) << '\n';
    for (const auto &[key, value] : s.gameNumbers)
        if (!s.gameOptions.count(key))
            out << key << " = " << value << '\n';
    return out.str();
}

void parseSettings(const std::string &text, Settings &settings) {
    std::istringstream in(text);
    std::string section;
    // No inline comments: key names such as ";" are valid binding values.
    for (std::string line; std::getline(in, line);) {
        line = trim(line);
        if (line.empty() || line[0] == ';' || line[0] == '#')
            continue;
        if (line.front() == '[' && line.back() == ']') {
            section = trim(line.substr(1u, line.size() - 2u));
            continue;
        }
        const auto equals = line.find('=');
        if (equals != std::string::npos)
            apply(settings, section, trim(line.substr(0, equals)), trim(line.substr(equals + 1u)));
    }
}

std::string defaultSettingsPath() {
    if (const char *path = std::getenv("RG_SETTINGS_FILE"); path && *path)
        return path;
    char *folder = SDL_GetPrefPath("RogueGalaxy", "Recompiled");
    if (!folder)
        return "roguegalaxy-settings.ini";
    std::string path = std::string(folder) + "settings.ini";
    SDL_free(folder);
    return path;
}

bool loadSettings(const std::string &path, Settings &settings) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        std::error_code error;
        return !std::filesystem::exists(path, error);
    }
    std::ostringstream text;
    text << file.rdbuf();
    parseSettings(text.str(), settings);
    return true;
}

bool saveSettings(const std::string &path, const Settings &settings) {
    // Written beside the old file and renamed over it, so a crash mid-write
    // cannot leave a truncated settings file.
    std::error_code error;
    if (const auto folder = std::filesystem::path(path).parent_path(); !folder.empty())
        std::filesystem::create_directories(folder, error);
    const std::string temporary = path + ".tmp";
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file)
            return false;
        file << serializeSettings(settings);
        if (!file.flush())
            return false;
    }
    // Unlike std::rename on Windows, SDL_RenamePath replaces the old file.
    if (SDL_RenamePath(temporary.c_str(), path.c_str()))
        return true;
    SDL_RemovePath(temporary.c_str());
    return false;
}

} // namespace dq8::ui
