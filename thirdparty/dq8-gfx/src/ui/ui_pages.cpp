// The settings window's pages and the menu's other windows.
#include "ui/ui_overlay.h"

#include "gfx/backends/sdlgpu/sdlgpu_input.h"
#include "ui/ui_style.h"
#include "ui/ui_widgets.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <string>

namespace dq8::ui {
namespace {
using gfx::PadInput;

constexpr const char *kTimes = "\xC3\x97"; // U+00D7, spelled out for compilers without /utf-8
constexpr const char *kPageNames[] = {"Display", "Sound", "Controls", "Game", "Language", "Textures", "Interface"};
constexpr Icon kPageIcons[] = {Icon::Display, Icon::Sound, Icon::Controls, Icon::Disc, Icon::Info, Icon::Folder,
                               Icon::Interface};
constexpr const char *kProjectUrl = "https://github.com/victor-takai/rogue-galaxy-recomp";
constexpr const char *kIssuesUrl = "https://github.com/victor-takai/rogue-galaxy-recomp/issues";

constexpr ImGuiWindowFlags kDialogFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                                          ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings |
                                          ImGuiWindowFlags_AlwaysAutoResize;

ImVec4 color(ImU32 packed) { return ImGui::ColorConvertU32ToFloat4(packed); }

// A round "x" in a window's corner, for the mouse: keyboard and controller
// close with Back, and the cursor would otherwise start on it.
bool closeButton(const char *id) {
    const float size = ImGui::GetFrameHeight();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton(id, ImVec2(size, size));
    const ImVec2 center(pos.x + size * 0.5f, pos.y + size * 0.5f);
    ImDrawList *list = ImGui::GetWindowDrawList();
    if (ImGui::IsItemHovered())
        list->AddCircleFilled(center, size * 0.5f, palette::kHover);
    drawIcon(list, Icon::Close, center, size * 0.62f,
             ImGui::IsItemHovered() ? palette::kText : palette::kTextMuted);
    return pressed;
}

// A window's title row: icon, title, and a close button at the right end.
void dialogHeader(Icon kind, const char *title, bool *open) {
    icon(kind, em(1.4f), palette::kGold);
    ImGui::SameLine(0.0f, em(0.6f));
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.3f);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(title);
    ImGui::PopFont();
    if (open) {
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x - ImGui::GetFrameHeight());
        if (closeButton("##close"))
            *open = false;
    }
    const ImVec2 at = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddLine(ImVec2(at.x, at.y + em(0.15f)),
                                        ImVec2(at.x + ImGui::GetContentRegionAvail().x, at.y + em(0.15f)),
                                        withAlpha(palette::kGold, 0.35f), 1.0f);
    ImGui::Dummy(ImVec2(0.0f, em(0.5f)));
}

void centerNextWindow() {
    const ImGuiViewport *viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                   viewport->WorkPos.y + viewport->WorkSize.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
}

// `joiner` sits between the keys: "+" for a chord, "/" for either of them.
void shortcutRow(const char *const *keys, int count, const char *what, const char *joiner = "+") {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    for (int key = 0; key < count; ++key) {
        if (key != 0) {
            ImGui::SameLine(0.0f, em(0.3f));
            ImGui::AlignTextToFramePadding();
            mutedText("%s", joiner);
            ImGui::SameLine(0.0f, em(0.3f));
        }
        keycap(keys[key]);
    }
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(what);
}

std::string conflictWith(const gfx::PadConfig &config, PadInput input, bool keyboard,
                         const gfx::PadBinding &binding) {
    if (!binding.bound())
        return {};
    for (size_t other = 0; other < gfx::kPadInputCount; ++other) {
        if (static_cast<PadInput>(other) == input)
            continue;
        for (const auto &candidate : config.slots(static_cast<PadInput>(other), keyboard))
            if (candidate == binding)
                return gfx::padInputName(static_cast<PadInput>(other));
    }
    return {};
}
} // namespace

void Overlay::drawSettingsWindow() {
    ImGuiContext &g = *ImGui::GetCurrentContext();
    const ImGuiViewport *viewport = ImGui::GetMainViewport();
    const ImVec2 area = viewport->WorkSize;
    const ImVec2 size(std::min(area.x - em(2.0f), em(54.0f)), std::min(area.y - em(2.0f), em(37.0f)));
    centerNextWindow();
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    if (m_focusSettings) {
        ImGui::SetNextWindowFocus();
        m_focusSettings = false;
    }
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(em(1.2f), em(1.0f)));
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                                   ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar |
                                   ImGuiWindowFlags_NoScrollWithMouse;
    if (beginWindow("Settings##dq8", nullptr, flags)) {
        dialogHeader(Icon::Gear, "Settings", &m_settingsOpen);
        // The controller's buttons along the bottom edge while one is in use.
        const float hints = padInUse() ? ImGui::GetFrameHeight() + em(1.2f) : 0.0f;
        const int count = static_cast<int>(SettingsPage::Count);

        // The page list and the page are apart for keyboard and controller:
        // the cursor stays in one until Right or A takes it into the page, and
        // Left at the page's edge or Back brings it out. Moving along the
        // list shows each page.
        const float navWidth = em(12.5f);
        ImGui::BeginChild("##nav", ImVec2(navWidth, -hints), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
        m_listPane = ImGui::GetCurrentWindow();
        const bool inList = g.NavWindow == m_listPane;
        const bool follow = inList && g.NavCursorVisible && !m_focusList;
        for (int page = 0; page < count; ++page) {
            const auto which = static_cast<SettingsPage>(page);
            if (navItem(kPageNames[page], kPageIcons[page], m_page == which)) {
                m_page = which;
                // Chosen with A or Enter rather than a click: on into it.
                if (g.NavCursorVisible)
                    m_focusPage = true;
            } else if (follow && m_page != which && ImGui::IsItemFocused()) {
                m_page = which;
            }
            if (m_focusList && m_page == which) {
                ImGui::FocusItem();
                if (padInUse())
                    ImGui::SetNavCursorVisible(true);
                m_focusList = false;
            }
        }
        // A way out for a player with only a controller, asked first.
        if (navItem("Quit game", Icon::Close, false))
            m_quitAsk = true;
        if (inList && !m_focusPage &&
            (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight, false) || ImGui::IsKeyPressed(ImGuiKey_RightArrow, false))) {
            // Nothing lies right of the list in it; the request would only
            // find the page's items once the page has the focus.
            ImGui::NavMoveRequestCancel();
            m_focusPage = true;
        }
        const float footer = ImGui::GetTextLineHeightWithSpacing() * 2.0f;
        if (ImGui::GetContentRegionAvail().y > footer) {
            ImGui::SetCursorPosY(ImGui::GetWindowHeight() - footer);
            ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 0.84f);
            mutedText("Rogue Galaxy Recompiled %s", m_host.version.c_str());
            mutedText("Changes save automatically");
            ImGui::PopFont();
        }
        ImGui::EndChild();

        ImGui::SameLine(0.0f, em(0.8f));
        const ImVec2 rule = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddLine(ImVec2(rule.x, rule.y),
                                            ImVec2(rule.x, rule.y + ImGui::GetContentRegionAvail().y - hints),
                                            IM_COL32(255, 255, 255, 34), 1.0f);
        ImGui::SetCursorScreenPos(ImVec2(rule.x + em(0.9f), rule.y));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        // One window a page, so each keeps its own scroll and cursor.
        ImGui::PushID(static_cast<int>(m_page));
        ImGui::BeginChild("##page", ImVec2(0.0f, -hints), ImGuiChildFlags_None);
        ImGui::PopStyleVar();
        m_pagePane = ImGui::GetCurrentWindow();
        if (m_focusPage) {
            // Back where it was in this page, or on its first item.
            ImGui::FocusWindow(m_pagePane);
            ImGui::NavInitWindow(m_pagePane, false);
            ImGui::SetNavCursorVisible(true);
            m_focusPage = false;
        }
        // Leaves room for the scrollbar without the page touching it.
        ImGui::PushItemWidth(-em(0.6f));
        ImGui::Indent(em(0.1f));
        switch (m_page) {
        case SettingsPage::Display: drawDisplayPage(); break;
        case SettingsPage::Sound: drawSoundPage(); break;
        case SettingsPage::Controls: drawControlsPage(); break;
        case SettingsPage::Game: drawGamePage(); break;
        case SettingsPage::Language: drawLanguagePage(); break;
        case SettingsPage::Textures: drawTexturesPage(); break;
        case SettingsPage::Interface: drawInterfacePage(); break;
        case SettingsPage::Count: break;
        }
        ImGui::Unindent(em(0.1f));
        ImGui::PopItemWidth();
        ImGui::Dummy(ImVec2(0.0f, em(0.5f)));
        ImGui::EndChild();
        ImGui::PopID(); // the ID stack is the settings window's again
        const bool inPage = g.NavWindow == m_pagePane;
        // Left with nothing further left in the page: back to the list.
        if (inPage && g.NavMoveDir == ImGuiDir_Left && ImGui::NavMoveRequestButNoResultYet()) {
            ImGui::NavMoveRequestCancel();
            m_focusList = true;
        }

        // L1 and R1 turn the pages, as in the game's own menus; not while a
        // slider is held (they speed it up and slow it down) or a list is open.
        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::IsAnyItemActive() &&
            !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) {
            const int turn = ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false)   ? count - 1
                             : ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false) ? 1
                                                                              : 0;
            if (turn != 0) {
                m_page = static_cast<SettingsPage>((static_cast<int>(m_page) + turn) % count);
                // The cursor stays on its side: in the new page, or on its entry.
                if (inPage)
                    m_focusPage = true;
                else
                    m_focusList = true;
            }
        }
        if (hints > 0.0f)
            drawPadHints(hints);
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

void Overlay::drawPadHints(float height) {
    const ImGuiContext &g = *ImGui::GetCurrentContext();
    const SDL_GamepadType type = displayedControllerType();
    struct Hint {
        SDL_GamepadButton first;
        SDL_GamepadButton second; // SDL_GAMEPAD_BUTTON_INVALID for none
        const char *text;
    };
    constexpr SDL_GamepadButton kNone = SDL_GAMEPAD_BUTTON_INVALID;
    std::vector<Hint> hints;
    const bool inPage = g.NavWindow && m_pagePane && g.NavWindow->RootWindowForNav == m_pagePane;
    if (ImGui::IsAnyItemActive()) {
        hints = {{SDL_GAMEPAD_BUTTON_DPAD_LEFT, SDL_GAMEPAD_BUTTON_DPAD_RIGHT, "Adjust"},
                 {SDL_GAMEPAD_BUTTON_SOUTH, kNone, "Done"}};
    } else if (ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) {
        hints = {{SDL_GAMEPAD_BUTTON_SOUTH, kNone, "Choose"}, {SDL_GAMEPAD_BUTTON_EAST, kNone, "Close"}};
    } else if (g.NavLayer == ImGuiNavLayer_Menu) {
        hints = {{SDL_GAMEPAD_BUTTON_SOUTH, kNone, "Open"}, {SDL_GAMEPAD_BUTTON_EAST, kNone, "Back"}};
    } else if (inPage) {
        hints = {{SDL_GAMEPAD_BUTTON_SOUTH, kNone, "Select"}, {SDL_GAMEPAD_BUTTON_EAST, kNone, "Back"}};
        if (m_page == SettingsPage::Controls)
            hints.push_back({SDL_GAMEPAD_BUTTON_WEST, kNone, "Clear a binding"});
        hints.push_back({SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, "Page"});
        hints.push_back({SDL_GAMEPAD_BUTTON_NORTH, kNone, "Top bar"});
    } else {
        hints = {{SDL_GAMEPAD_BUTTON_SOUTH, kNone, "Open"},
                 {SDL_GAMEPAD_BUTTON_EAST, kNone, "Back to the game"},
                 {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, "Page"},
                 {SDL_GAMEPAD_BUTTON_NORTH, kNone, "Top bar"}};
    }

    ImDrawList *list = ImGui::GetWindowDrawList();
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float right = at.x + ImGui::GetContentRegionAvail().x;
    list->AddLine(ImVec2(at.x, at.y), ImVec2(right, at.y), IM_COL32(255, 255, 255, 34), 1.0f);
    const float chip = std::floor(ImGui::GetFrameHeight() * 0.8f);
    // A clear gap under the rule; what is left goes below the buttons.
    const float y = std::floor(at.y + em(0.8f));
    float x = at.x;
    for (const Hint &hint : hints) {
        float width = 0.0f;
        for (const SDL_GamepadButton button : {hint.first, hint.second})
            if (button != kNone)
                width += bindingChipWidth(gfx::PadBinding::button(button), type, chip) + em(0.25f);
        const ImVec2 text = ImGui::CalcTextSize(hint.text);
        if (x + width + text.x > right)
            break;
        for (const SDL_GamepadButton button : {hint.first, hint.second}) {
            if (button == kNone)
                continue;
            const gfx::PadBinding binding = gfx::PadBinding::button(button);
            drawBindingChip(list, binding, type, ImVec2(x, y), chip);
            x += bindingChipWidth(binding, type, chip) + em(0.25f);
        }
        list->AddText(ImVec2(x + em(0.15f), std::floor(y + (chip - text.y) * 0.5f)), palette::kTextMuted, hint.text);
        x += em(0.15f) + text.x + em(1.3f);
    }
    ImGui::Dummy(ImVec2(0.0f, height - ImGui::GetStyle().ItemSpacing.y));
}

void Overlay::drawDisplayPage() {
    sectionHeader("Window");
    if (beginSettings("window")) {
        settingRow("Mode");
        int mode = m_settings.fullscreen ? 1 : 0;
        static constexpr const char *kModes[] = {"Windowed", "Fullscreen"};
        if (segmented("mode", &mode, kModes, 2))
            setFullscreen(mode == 1);

        settingRow("Vertical sync", "Fast avoids tearing with less delay, where the system offers it.");
        static constexpr SDL_GPUPresentMode kPresent[] = {SDL_GPU_PRESENTMODE_VSYNC, SDL_GPU_PRESENTMODE_MAILBOX,
                                                         SDL_GPU_PRESENTMODE_IMMEDIATE};
        static constexpr const char *kSync[] = {"On", "Fast", "Off"};
        bool supported[3] = {true, m_backend.supportsPresentMode(kPresent[1]), m_backend.supportsPresentMode(kPresent[2])};
        int sync = 0;
        for (int index = 0; index < 3; ++index)
            if (m_settings.display.presentMode == kPresent[index])
                sync = index;
        if (segmented("sync", &sync, kSync, 3, supported)) {
            m_settings.display.presentMode = kPresent[sync];
            m_backend.setDisplayOptions(m_settings.display);
            markDirty();
        }
        endSettings();
    }

    sectionHeader("Picture");
    if (beginSettings("picture")) {
        settingRow("Internal resolution",
                   "Draws the game at a multiple of the PS2's own resolution. Above 1\xC3\x97 is slower for now.",
                   "Experimental");
        const uint32_t active = m_backend.activeResolutionScale();
        const uint32_t shown = m_pendingScale != 0u ? m_pendingScale : active;
        char preview[96];
        std::snprintf(preview, sizeof(preview), "%u%s   %u %s %u%s", shown, kTimes, 512u * shown, kTimes, 448u * shown,
                      m_pendingScale == 0u ? "" : pausing() ? "   when the game resumes" : "   applying...");
        if (ImGui::BeginCombo("##scale", preview)) {
            for (uint32_t scale = 1u; scale <= 8u; ++scale) {
                char item[64];
                std::snprintf(item, sizeof(item), "%u%s   %u %s %u%s", scale, kTimes, 512u * scale, kTimes,
                              448u * scale, scale == 1u ? "   (PS2)" : "");
                if (ImGui::Selectable(item, scale == shown))
                    requestScale(scale);
            }
            ImGui::EndCombo();
        }

        const char *aspectHelp = "";
        switch (m_settings.display.aspect) {
        case gfx::SdlGpuAspect::Auto: {
            // Rogue Galaxy's is on the Game page.
            const int wide = m_backend.gameWidescreen();
            aspectHelp = wide == 1   ? "Follows Game > Widescreen: the window's shape."
                         : wide == 0 ? "Follows Game > Widescreen: 4:3."
                                     : "4:3, the shape the game draws for; it has no wide-screen option.";
            break;
        }
        case gfx::SdlGpuAspect::Standard: aspectHelp = "The shape a television gave the game."; break;
        case gfx::SdlGpuAspect::Wide:
            aspectHelp = m_backend.gameWidescreen() == 1 ? "16:9 whatever the window; the game draws for it."
                                                         : "Stretches the 4:3 picture to 16:9; the game draws for 4:3.";
            break;
        case gfx::SdlGpuAspect::Native: aspectHelp = "Square pixels, as drawn; narrower than on a television."; break;
        case gfx::SdlGpuAspect::Stretch: aspectHelp = "Fills the window, whatever its shape."; break;
        }
        settingRow("Aspect ratio", aspectHelp, "Experimental");
        static constexpr const char *kAspects[] = {"Auto", "4:3", "16:9", "Native", "Stretch"};
        int aspect = static_cast<int>(m_settings.display.aspect);
        if (segmented("aspect", &aspect, kAspects, 5)) {
            m_settings.display.aspect = static_cast<gfx::SdlGpuAspect>(aspect);
            m_backend.setDisplayOptions(m_settings.display);
            markDirty();
        }

        static constexpr const char *kFilterHelp[] = {"Crisp pixels with no uneven columns, at any size.",
                                                      "Soft, smooth scaling.",
                                                      "Raw pixels; uneven unless the size is a whole multiple.",
                                                      "AMD FSR 1: edge-aware upscaling with sharpening. Best with a "
                                                      "lower internal resolution; smooth when the picture is not enlarged."};
        settingRow("Scaling filter", kFilterHelp[static_cast<int>(m_settings.display.filter)]);
        static constexpr const char *kFilters[] = {"Sharp", "Smooth", "Nearest", "FSR 1"};
        int filter = static_cast<int>(m_settings.display.filter);
        if (segmented("filter", &filter, kFilters, 4)) {
            m_settings.display.filter = static_cast<gfx::SdlGpuFilter>(filter);
            m_backend.setDisplayOptions(m_settings.display);
            markDirty();
        }

        // Rogue Galaxy: added (`fsr_sharpness` in the file).
        if (m_settings.display.filter == gfx::SdlGpuFilter::Fsr) {
            settingRow("FSR sharpness", "How much FSR 1 sharpens the enlarged picture.");
            if (ImGui::SliderInt("##fsrsharpness", &m_settings.display.fsrSharpness, 0, 100, "%d%%")) {
                m_backend.setDisplayOptions(m_settings.display);
                markDirty();
            }
        }

        // Rogue Galaxy: added (`sides` in the file).
        static constexpr const char *kSidesHelp[] = {
            "What fills the window beside a 4:3 picture: black bars.",
            "What fills the window beside a 4:3 picture: the picture itself, softly blurred and a little darker.",
            "What fills the window beside a 4:3 picture: your image, the first PNG or JPG in the game's "
            "\"borders\" folder (the blurred picture without one)."};
        settingRow("Borders", kSidesHelp[static_cast<int>(m_settings.display.sides)]);
        static constexpr const char *kSides[] = {"Black", "Blurred", "Image"};
        int sides = static_cast<int>(m_settings.display.sides);
        if (segmented("sides", &sides, kSides, 3)) {
            m_settings.display.sides = static_cast<gfx::SdlGpuSides>(sides);
            m_backend.setDisplayOptions(m_settings.display);
            markDirty();
        }

        settingRow("Integer scaling", "Whole multiples of the picture's height: even lines, with borders.");
        if (toggle("integer", &m_settings.display.integerScale)) {
            m_backend.setDisplayOptions(m_settings.display);
            markDirty();
        }

        settingRow("Sharper picture", "Drops the PS2's line blending, a deflicker for TVs that only blurs on a monitor.");
        if (toggle("sharper", &m_settings.display.removeLineBlend)) {
            m_backend.setDisplayOptions(m_settings.display);
            markDirty();
        }
        endSettings();
    }
}

void Overlay::drawSoundPage() {
    sectionHeader("Volume");
    if (beginSettings("sound")) {
        settingRow("Master volume");
        if (ImGui::SliderInt("##volume", &m_settings.volume, 0, 100, "%d%%")) {
            applyVolume();
            markDirty();
        }
        settingRow("Mute");
        if (toggle("mute", &m_settings.muted)) {
            applyVolume();
            markDirty();
        }
        settingRow("Mute in the background", "Silences the game while its window is not in front.");
        if (toggle("background", &m_settings.muteInBackground)) {
            applyVolume();
            markDirty();
        }
        endSettings();
    }
    if (!m_host.setVolume)
        mutedText("Sound is not available in this build.");
}

void Overlay::drawInterfacePage() {
    sectionHeader("Menu");
    if (beginSettings("menu")) {
        settingRow("Menu size", "Scales this menu and its messages.");
        // Applied on release: the menu resizing under the cursor mid-drag
        // would move the slider away from it.
        if (!ImGui::IsAnyItemActive() || m_uiScaleEdit == 0)
            m_uiScaleEdit = m_settings.uiScale;
        ImGui::SliderInt("##uiscale", &m_uiScaleEdit, 75, 200, "%d%%");
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            m_settings.uiScale = m_uiScaleEdit;
            markDirty();
        }
        settingRow("Notifications", "Short messages about controllers, screenshots and changes.");
        if (toggle("notifications", &m_settings.notifications))
            markDirty();
        settingRow("Hide the idle mouse cursor");
        if (toggle("cursor", &m_settings.hideIdleCursor))
            markDirty();
        // Not on a Steam Deck: Steam keeps its own button, so View + Menu is
        // the only way to the menu there.
        if (!m_host.steamDeck) {
            settingRow("Back + Start opens the menu", "The Guide button (Xbox, PS, Home) always does.");
            if (toggle("backstart", &m_settings.backStartOpensMenu))
                markDirty();
        }
        endSettings();
    }

    if (canPause() || canChangeSpeed()) {
        sectionHeader("Pause and speed");
        if (beginSettings("pausing")) {
            if (canPause()) {
                settingRow("Pause while the menu is open");
                if (toggle("pausemenu", &m_settings.pauseInMenu)) {
                    applyPause();
                    markDirty();
                }
                settingRow("Pause in the background", "Stops the game while its window is not in front.");
                if (toggle("pausebackground", &m_settings.pauseInBackground)) {
                    applyPause();
                    markDirty();
                }
            }
            if (canChangeSpeed()) {
                settingRow("Fast-forward speed",
                           "F4, or Game > Fast-forward in the top bar, switches between it and normal speed. "
                           "Music keeps its tempo.",
                           "Experimental");
                static constexpr float kSpeeds[] = {2.0f, 3.0f, 4.0f, 0.0f};
                static constexpr const char *kSpeedNames[] = {"2\xC3\x97", "3\xC3\x97", "4\xC3\x97", "Unlimited"};
                int speed = 0;
                for (int index = 0; index < 4; ++index)
                    if (m_settings.fastForwardSpeed == kSpeeds[index])
                        speed = index;
                if (segmented("speed", &speed, kSpeedNames, 4)) {
                    m_settings.fastForwardSpeed = kSpeeds[speed];
                    if (m_fastForward)
                        m_host.setSpeed(m_settings.fastForwardSpeed);
                    markDirty();
                }
            }
            endSettings();
        }
    }

    sectionHeader("Frame rate");
    if (beginSettings("framerate")) {
        settingRow("Show the frame rate", "Frames the game draws each second. F3 toggles it.");
        if (toggle("fps", &m_settings.showFps))
            markDirty();
        settingRow("Position");
        static constexpr const char *kCorners[] = {"Top left", "Top right", "Bottom left", "Bottom right"};
        int corner = static_cast<int>(m_settings.fpsCorner);
        if (segmented("corner", &corner, kCorners, 4)) {
            m_settings.fpsCorner = static_cast<Corner>(corner);
            markDirty();
        }
        endSettings();
    }
}

// The game's own switches, in the host's sections (Rogue Galaxy: added).
void Overlay::drawGamePage() {
    drawGameOptions(std::string(), "The game has no options of its own.", "Game");
}

void Overlay::drawLanguagePage() {
    drawGameOptions("language", "There are no language options.", "Language");
}

// The host's switches for packs of art (Rogue Galaxy: added).
void Overlay::drawTexturesPage() {
    drawGameOptions("textures",
                    "No texture packs found: put one (a folder or a .zip) in textures\\packs.\n"
                    "They are drawn above 1x. The game ships none (docs\\TEXTURE_PACKS.md).",
                    "Texture packs");
}

void Overlay::drawGameOptions(const std::string &page, const char *emptyText, const char *firstTitle) {
    if (std::none_of(m_host.gameOptions.begin(), m_host.gameOptions.end(),
                     [&](const GameOption &option) { return option.page == page; })) {
        mutedText("%s", emptyText);
        return;
    }
    bool started = false, table = false;
    for (const GameOption &option : m_host.gameOptions) {
        if (option.page != page)
            continue;
        const auto section = std::find_if(m_host.gameSections.begin(), m_host.gameSections.end(),
                                          [&](const auto &entry) { return entry.first == option.key; });
        if (!started || section != m_host.gameSections.end()) {
            if (table)
                endSettings();
            sectionHeader(section != m_host.gameSections.end() ? section->second.c_str() : firstTitle);
            table = beginSettings(("game." + option.key).c_str());
            started = true;
        }
        if (!table)
            continue;
        settingRow(option.label.c_str(), option.help.empty() ? nullptr : option.help.c_str());
        if (option.number()) {
            const int value = gameNumber(option);
            int next = value;
            const ImGuiStyle &style = ImGui::GetStyle();
            const float button = ImGui::GetFrameHeight();
            const float reset = ImGui::CalcTextSize("Default").x + style.FramePadding.x * 2.0f;
            ImGui::PushID(option.key.c_str());
            if (ImGui::Button("-", ImVec2(button, 0.0f)))
                next = value - option.step;
            ImGui::SameLine();
            ImGui::SetNextItemWidth(std::max(button * 3.0f, ImGui::GetContentRegionAvail().x - button -
                                                                reset - style.ItemSpacing.x * 2.0f));
            ImGui::SliderInt("##value", &next, option.minimum, option.maximum,
                             option.format.empty() ? "%d" : option.format.c_str());
            ImGui::SameLine();
            if (ImGui::Button("+", ImVec2(button, 0.0f)))
                next = value + option.step;
            ImGui::SameLine();
            ImGui::BeginDisabled(value == option.defaultNumber);
            if (ImGui::Button("Default"))
                next = option.defaultNumber;
            ImGui::EndDisabled();
            ImGui::PopID();
            next = std::clamp(next, option.minimum, option.maximum);
            if (next != value) {
                m_settings.gameNumbers[option.key] = next;
                // A "0" or "1" in the file reads as a switch too.
                m_settings.gameOptions.erase(option.key);
                if (m_host.setGameNumber)
                    m_host.setGameNumber(option.key, next);
                markDirty();
            }
            continue;
        }
        bool on = gameOption(option);
        if (!option.remembered) {
            if (toggle(option.key.c_str(), &on) && m_host.setGameOption)
                m_host.setGameOption(option.key, on);
            continue;
        }
        if (toggle(option.key.c_str(), &on)) {
            m_settings.gameOptions[option.key] = on;
            m_settings.gameNumbers.erase(option.key);
            if (m_host.setGameOption)
                m_host.setGameOption(option.key, on);
            markDirty();
        }
        if (waitsForRestart(option))
            ImGui::TextColored(color(palette::kBad), "Restart the game for this to take effect.");
    }
    if (table)
        endSettings();
}

void Overlay::drawControlsPage() {
    const auto controllers = m_backend.padInput().controllers();
    sectionHeader("Devices");
    if (beginSettings("devices")) {
        gfx::PadConfig config = m_settings.pad;
        bool changed = false;
        settingRow("Keyboard", "Plays alongside the controller.");
        changed |= toggle("keyboard", &config.keyboardEnabled);

        settingRow("Controller", "Which controller plays the game.");
        std::string preview = "Any connected controller";
        if (config.controllers == gfx::ControllerSelection::None) {
            preview = "No controller";
        } else if (config.controllers == gfx::ControllerSelection::One) {
            const bool connected = std::any_of(controllers.begin(), controllers.end(),
                                               [](const auto &c) { return c.active; });
            preview = config.controllerName + (connected ? "" : "  (not connected)");
        }
        if (ImGui::BeginCombo("##controller", preview.c_str())) {
            if (ImGui::Selectable("Any connected controller", config.controllers == gfx::ControllerSelection::Any)) {
                config.controllers = gfx::ControllerSelection::Any;
                changed = true;
            }
            for (size_t index = 0; index < controllers.size(); ++index) {
                const auto &controller = controllers[index];
                ImGui::PushID(static_cast<int>(index));
                // Identical controllers share a GUID, so the device in use is
                // what marks the choice, and the pick remembers the device.
                const bool chosen = config.controllers == gfx::ControllerSelection::One && controller.active;
                if (ImGui::Selectable(controller.name.c_str(), chosen)) {
                    config.controllers = gfx::ControllerSelection::One;
                    config.controllerGuid = controller.guid;
                    config.controllerSerial = controller.serial;
                    config.controllerName = controller.name;
                    config.controllerInstance = controller.id;
                    changed = true;
                }
                ImGui::PopID();
            }
            if (ImGui::Selectable("No controller", config.controllers == gfx::ControllerSelection::None)) {
                config.controllers = gfx::ControllerSelection::None;
                changed = true;
            }
            ImGui::EndCombo();
        }
        endSettings();
        if (changed)
            setPadConfig(config);
    }

    // What is plugged in, and what of it plays.
    if (controllers.empty()) {
        mutedText("No controller connected. Plug one in or pair it; it is picked up at once.");
    } else {
        for (const auto &controller : controllers) {
            ImGui::PushID(&controller);
            icon(Icon::Controls, em(1.1f), controller.active ? palette::kGold : palette::kTextMuted);
            ImGui::SameLine(0.0f, em(0.5f));
            ImGui::TextUnformatted(controller.name.c_str());
            ImGui::SameLine(0.0f, em(0.6f));
            mutedText("%s", controllerFamilyName(controller.type));
            if (controller.batteryPercent >= 0 && (controller.power == SDL_POWERSTATE_ON_BATTERY ||
                                                   controller.power == SDL_POWERSTATE_CHARGING)) {
                ImGui::SameLine(0.0f, em(0.6f));
                mutedText("%d%%%s", controller.batteryPercent,
                          controller.power == SDL_POWERSTATE_CHARGING ? " charging" : "");
            }
            ImGui::SameLine(0.0f, em(0.6f));
            ImGui::TextColored(color(controller.active ? palette::kGood : palette::kTextMuted), "%s",
                               controller.active ? "In use" : "Not used");
            ImGui::PopID();
        }
    }

    sectionHeader("Buttons");
    {
        const float width = ImGui::GetContentRegionAvail().x;
        const ImVec2 size(std::min(width * 0.56f, em(16.0f)), std::min(width * 0.56f, em(16.0f)) * 0.62f);
        const ImVec2 at = ImGui::GetCursorScreenPos();
        ImGui::Dummy(size);
        drawPadDiagram(ImGui::GetWindowDrawList(), at, size, m_backend.padInput().preview(), m_hoveredInput);
        ImGui::SameLine(0.0f, em(1.2f));
        ImGui::BeginGroup();
        ImGui::Dummy(ImVec2(0.0f, em(0.6f)));
        ImGui::TextUnformatted("Test your bindings");
        ImGui::PushTextWrapPos(0.0f);
        mutedText("%s", "Press anything: the pad lights up with what the game would read.");
        if (padInUse())
            mutedText("%s on a binding changes it; %s clears it.", padButtonName(SDL_GAMEPAD_BUTTON_SOUTH).c_str(),
                      padButtonName(SDL_GAMEPAD_BUTTON_WEST).c_str());
        else
            mutedText("%s", "Click a binding to change it; right-click to clear it.");
        ImGui::PopTextWrapPos();
        ImGui::EndGroup();
    }
    ImGui::Dummy(ImVec2(0.0f, em(0.2f)));
    if (ImGui::BeginTabBar("##bindings")) {
        const auto pick = [&](int tab) {
            return m_selectBindings == tab ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
        };
        if (ImGui::BeginTabItem("Controller", nullptr, pick(0))) {
            m_keyboardBindings = false;
            drawBindingTable(false);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Keyboard", nullptr, pick(1))) {
            m_keyboardBindings = true;
            drawBindingTable(true);
            ImGui::EndTabItem();
        }
        m_selectBindings = -1;
        ImGui::EndTabBar();
    }

    sectionHeader("Analog sticks");
    if (beginSettings("analog")) {
        gfx::PadConfig config = m_settings.pad;
        bool changed = false;
        settingRow("Dead zone", "Ignores the small drift of a worn stick.");
        float deadZone = config.stickDeadZone * 100.0f;
        if (ImGui::SliderFloat("##deadzone", &deadZone, 0.0f, 40.0f, "%.0f%%")) {
            config.stickDeadZone = deadZone / 100.0f;
            changed = true;
        }
        settingRow("Trigger threshold", "How far L2 and R2 travel before they count as pressed.");
        float threshold = config.triggerThreshold * 100.0f;
        if (ImGui::SliderFloat("##trigger", &threshold, 5.0f, 90.0f, "%.0f%%")) {
            config.triggerThreshold = threshold / 100.0f;
            changed = true;
        }
        settingRow("Invert camera left and right");
        changed |= toggle("invertx", &config.invertCameraX);
        settingRow("Invert camera up and down");
        changed |= toggle("inverty", &config.invertCameraY);
        endSettings();
        if (changed)
            setPadConfig(config);
    }
}

void Overlay::drawBindingTable(bool keyboard) {
    const SDL_GamepadType type = displayedControllerType();
    const float chip = std::floor(ImGui::GetFrameHeight() * 0.92f);
    PadInput hovered = PadInput::Count;
    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_PadOuterX;
    if (ImGui::BeginTable(keyboard ? "##keys" : "##buttons", 3, flags)) {
        ImGui::TableSetupColumn("PS2", ImGuiTableColumnFlags_WidthStretch, 1.25f);
        ImGui::TableSetupColumn(keyboard ? "Key" : "Button", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Alternate", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, color(palette::kTextMuted));
        ImGui::TableHeadersRow();
        ImGui::PopStyleColor();
        for (size_t index = 0; index < gfx::kPadInputCount; ++index) {
            const auto input = static_cast<PadInput>(index);
            ImGui::TableNextRow(ImGuiTableRowFlags_None, chip + em(0.35f));
            ImGui::TableNextColumn();
            ImDrawList *list = ImGui::GetWindowDrawList();
            const ImVec2 at = ImGui::GetCursorScreenPos();
            const ImVec2 name = ImGui::CalcTextSize(gfx::padInputName(input));
            ImGui::Dummy(ImVec2(chip + em(0.6f) + name.x, chip));
            drawPadGlyph(list, input, ImVec2(at.x + chip * 0.5f, at.y + chip * 0.5f), chip * 0.9f);
            list->AddText(ImVec2(at.x + chip + em(0.6f), std::floor(at.y + (chip - name.y) * 0.5f)), palette::kText,
                          gfx::padInputName(input));

            for (int slot = 0; slot < static_cast<int>(gfx::kBindingSlots); ++slot) {
                ImGui::TableNextColumn();
                const gfx::PadBinding binding = m_settings.pad.slots(input, keyboard)[slot];
                ImGui::PushID(static_cast<int>(index * gfx::kBindingSlots + slot));
                const ImVec2 cell = ImGui::GetCursorScreenPos();
                const float cellWidth = ImGui::GetContentRegionAvail().x;
                if (ImGui::InvisibleButton("##bind", ImVec2(cellWidth, chip), ImGuiButtonFlags_EnableNav))
                    beginBindingCapture(input, keyboard, slot);
                const bool over = ImGui::IsItemHovered() || ImGui::IsItemFocused();
                // Cleared by a right-click, or X (Square) or Delete on the cursor.
                const bool clear = ImGui::IsItemFocused() && (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceLeft, false) ||
                                                             ImGui::IsKeyPressed(ImGuiKey_Delete, false) ||
                                                             ImGui::IsKeyPressed(ImGuiKey_Backspace, false));
                if (clear || ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                    gfx::PadConfig config = m_settings.pad;
                    config.slots(input, keyboard)[slot] = {};
                    setPadConfig(config);
                }
                if (over)
                    list->AddRectFilled(ImVec2(cell.x - em(0.2f), cell.y - em(0.12f)),
                                        ImVec2(cell.x + cellWidth, cell.y + chip + em(0.12f)), palette::kHover,
                                        ImGui::GetStyle().FrameRounding);
                drawBindingChip(list, binding, type, ImVec2(cell.x + em(0.2f), cell.y), chip,
                                binding.bound() ? 1.0f : 0.7f);
                const std::string clash = conflictWith(m_settings.pad, input, keyboard, binding);
                if (!clash.empty()) {
                    const float x = cell.x + em(0.2f) + bindingChipWidth(binding, type, chip) + em(0.7f);
                    drawIcon(list, Icon::Warning, ImVec2(x, cell.y + chip * 0.5f), em(0.95f), palette::kWarn);
                }
                if (ImGui::BeginItemTooltip()) {
                    ImGui::TextUnformatted(bindingLabel(binding, type).c_str());
                    if (!clash.empty())
                        ImGui::TextColored(color(palette::kWarn), "Also bound to %s", clash.c_str());
                    mutedText("%s", "Click to change, right-click to clear");
                    ImGui::EndTooltip();
                }
                ImGui::PopID();
            }
            if (ImGui::TableGetHoveredRow() == ImGui::TableGetRowIndex())
                hovered = input;
        }
        ImGui::EndTable();
    }
    m_hoveredInput = hovered;
    ImGui::Dummy(ImVec2(0.0f, em(0.2f)));
    if (iconButton(keyboard ? "Restore default keys" : "Restore default buttons", Icon::Reset)) {
        gfx::PadConfig config = m_settings.pad;
        const gfx::PadConfig defaults = gfx::PadConfig::defaults();
        (keyboard ? config.keyboard : config.controller) = keyboard ? defaults.keyboard : defaults.controller;
        setPadConfig(config);
        notify(keyboard ? "Keys restored" : "Buttons restored", Icon::Check);
    }
}

void Overlay::drawAbout() {
    centerNextWindow();
    ImGui::SetNextWindowSize(ImVec2(std::min(ImGui::GetMainViewport()->WorkSize.x - em(2.0f), em(31.0f)), 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(em(1.4f), em(1.2f)));
    if (beginWindow("##about", nullptr, kDialogFlags & ~ImGuiWindowFlags_AlwaysAutoResize)) {
        icon(Icon::Disc, em(3.0f), palette::kGold);
        ImGui::SameLine(0.0f, em(0.9f));
        ImGui::BeginGroup();
        ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.6f);
        ImGui::TextUnformatted("Rogue Galaxy Recompiled");
        ImGui::PopFont();
        if (m_host.revision.empty())
            mutedText("Version %s", m_host.version.c_str());
        else
            mutedText("Version %s (%s)", m_host.version.c_str(), m_host.revision.c_str());
        const auto newer = m_host.newerRelease ? m_host.newerRelease() : std::pair<std::string, std::string>{};
        if (!newer.first.empty())
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(palette::kGold), "Version %s is out",
                               newer.first.c_str());
        ImGui::EndGroup();
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x - ImGui::GetFrameHeight());
        if (closeButton("##close"))
            m_aboutOpen = false;
        ImGui::Dummy(ImVec2(0.0f, em(0.3f)));
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted("A native PC port of Rogue Galaxy, made by statically recompiling the PlayStation 2 "
                               "game. It plays from your own copy of the game.");
        ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(0.0f, em(0.3f)));
        if (beginSettings("about")) {
            settingRow("Renderer");
            ImGui::Text("SDL GPU (%s)", m_backend.driverName().c_str());
            settingRow("Internal resolution");
            ImGui::Text("%u%s", m_backend.activeResolutionScale(), kTimes);
            const int sdl = SDL_GetVersion();
            settingRow("Built with");
            ImGui::Text("SDL %d.%d.%d, Dear ImGui %s", SDL_VERSIONNUM_MAJOR(sdl), SDL_VERSIONNUM_MINOR(sdl),
                        SDL_VERSIONNUM_MICRO(sdl), IMGUI_VERSION);
            settingRow("Recompiler");
            ImGui::TextUnformatted("PS2Recomp");
            endSettings();
        }
        ImGui::Dummy(ImVec2(0.0f, em(0.3f)));
        if (!newer.first.empty()) {
            if (iconButton(("Get version " + newer.first).c_str(), Icon::Link, true))
                SDL_OpenURL(newer.second.c_str());
            ImGui::SameLine();
        }
        if (iconButton("Project page", Icon::Link, newer.first.empty()))
            SDL_OpenURL(kProjectUrl);
        ImGui::SameLine();
        if (iconButton("Report a problem", Icon::Warning))
            SDL_OpenURL(kIssuesUrl);
        ImGui::Dummy(ImVec2(0.0f, em(0.2f)));
        ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 0.84f);
        ImGui::PushTextWrapPos(0.0f);
        mutedText("%s", "Rogue Galaxy is a trademark of Sony Interactive Entertainment. This project is not "
                        "affiliated with Sony or Level-5, and includes nothing from the game.");
        ImGui::PopTextWrapPos();
        ImGui::PopFont();
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

void Overlay::drawShortcuts() {
    centerNextWindow();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(em(1.4f), em(1.2f)));
    if (beginWindow("##shortcuts", nullptr, kDialogFlags)) {
        dialogHeader(Icon::Keyboard, "Shortcuts", &m_shortcutsOpen);
        sectionHeader("Keyboard");
        if (ImGui::BeginTable("##keys", 2, ImGuiTableFlags_None)) {
            ImGui::TableSetupColumn("keys", ImGuiTableColumnFlags_WidthFixed, em(7.5f));
            ImGui::TableSetupColumn("what", ImGuiTableColumnFlags_WidthFixed, em(15.0f));
            static constexpr const char *kF1[] = {"F1"}, *kF2[] = {"F2"}, *kF3[] = {"F3"}, *kF4[] = {"F4"},
                                        *kF11[] = {"F11"}, *kAltEnter[] = {"Alt", "Enter"}, *kF12[] = {"F12"},
                                        *kShiftF12[] = {"Shift", "F12"}, *kEsc[] = {"Esc"};
            shortcutRow(kF1, 1, "Open or close this menu");
            if (canPause())
                shortcutRow(kF2, 1, "Pause or resume");
            shortcutRow(kF3, 1, "Show the frame rate");
            if (canChangeSpeed())
                shortcutRow(kF4, 1, "Fast-forward");
            shortcutRow(kF11, 1, "Fullscreen");
            shortcutRow(kAltEnter, 2, "Fullscreen");
            shortcutRow(kF12, 1, "Screenshot of the game");
            shortcutRow(kShiftF12, 2, "Screenshot with the menu");
            shortcutRow(kEsc, 1, "Back, in the menu");
            ImGui::EndTable();
        }
        sectionHeader("Controller");
        if (ImGui::BeginTable("##pad", 2, ImGuiTableFlags_None)) {
            ImGui::TableSetupColumn("keys", ImGuiTableColumnFlags_WidthFixed, em(7.5f));
            ImGui::TableSetupColumn("what", ImGuiTableColumnFlags_WidthFixed, em(15.0f));
            static constexpr const char *kGuide[] = {"Guide"}, *kBackStart[] = {"Back", "Start"},
                                        *kViewMenu[] = {"View", "Menu"}, *kShoulders[] = {"L1", "R1"},
                                        *kBack[] = {"B / Circle"}, *kTop[] = {"Y / Triangle"},
                                        *kClear[] = {"X / Square"};
            if (m_host.steamDeck)
                shortcutRow(kViewMenu, 2, "Open or close the menu");
            else if (m_settings.backStartOpensMenu)
                shortcutRow(kBackStart, 2, "Open or close the menu");
            if (!m_host.steamDeck)
                shortcutRow(kGuide, 1, "Open or close the menu");
            shortcutRow(kShoulders, 2, "Previous and next settings page", "/");
            shortcutRow(kBack, 1, "Back, in the menu");
            shortcutRow(kTop, 1, "To the top bar and back");
            shortcutRow(kClear, 1, "Clear a binding (Controls)");
            ImGui::EndTable();
        }
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

void Overlay::drawStats() {
    const double time = now();
    if (m_statsAt < 0.0 || time - m_statsAt > 0.5) {
        m_statsPrevious = m_stats;
        m_stats = m_backend.stats();
        m_statsSpan = m_statsAt < 0.0 ? 0.0 : time - m_statsAt;
        m_statsAt = time;
    }
    const ImGuiViewport *viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + em(0.8f), viewport->WorkPos.y + em(0.8f)), ImGuiCond_FirstUseEver);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                             ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_NoFocusOnAppearing;
    if (!m_menuOpen)
        flags |= ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(em(1.0f), em(0.8f)));
    if (beginWindow("##stats", nullptr, flags)) {
        dialogHeader(Icon::Chart, "Renderer", m_menuOpen ? &m_statsOpen : nullptr);
        const double span = m_statsSpan > 0.0 ? m_statsSpan : 1.0;
        const auto rate = [&](uint64_t now, uint64_t before) {
            return m_statsSpan > 0.0 && now >= before ? static_cast<double>(now - before) / span : 0.0;
        };
        if (ImGui::BeginTable("##stats", 2, ImGuiTableFlags_None)) {
            ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthFixed, em(11.0f));
            ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthFixed, em(8.0f));
            const auto row = [&](const char *name, const char *format, double value) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                mutedText("%s", name);
                ImGui::TableNextColumn();
                ImGui::Text(format, value);
            };
            row("Frames shown", "%.0f /s", rate(m_stats.presents, m_statsPrevious.presents));
            row("Draw calls", "%.0f /s", rate(m_stats.drawCalls, m_statsPrevious.drawCalls));
            row("Triangles", "%.1f K/s", rate(m_stats.trianglesDrawn, m_statsPrevious.trianglesDrawn) / 1000.0);
            row("Render passes", "%.0f /s", rate(m_stats.renderPasses, m_statsPrevious.renderPasses));
            row("Texture cache hits", "%.1f %%", m_stats.textureHitRate() * 100.0);
            row("Textures built", "%.0f /s", rate(m_stats.textureBuilds, m_statsPrevious.textureBuilds));
            row("Target resolves", "%.0f /s", rate(m_stats.colorResolves, m_statsPrevious.colorResolves));
            row("Transfers", "%.0f KiB/s", rate(m_stats.transferBytes, m_statsPrevious.transferBytes) / 1024.0);
            row("Pipelines", "%.0f", static_cast<double>(m_stats.pipelinesCreated));
            ImGui::EndTable();
        }
        sectionHeader("Approximations");
        if (ImGui::BeginTable("##approximations", 2, ImGuiTableFlags_None)) {
            ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthFixed, em(11.0f));
            ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthFixed, em(8.0f));
            const auto row = [&](const char *name, uint64_t value) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                mutedText("%s", name);
                ImGui::TableNextColumn();
                ImGui::TextColored(color(value ? palette::kWarn : palette::kGood), "%llu",
                                   static_cast<unsigned long long>(value));
            };
            row("Inexact blends", m_stats.inexactBlends);
            row("Saturated factors", m_stats.saturatedBlendFactors);
            row("Destination alpha", m_stats.destinationAlphaFactors);
            row("Partial masks", m_stats.partialChannelMasks);
            row("Alpha fail modes", m_stats.alphaFailModes);
            ImGui::EndTable();
        }
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

} // namespace dq8::ui
