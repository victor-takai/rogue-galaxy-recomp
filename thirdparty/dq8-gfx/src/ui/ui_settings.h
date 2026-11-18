// Everything the in-game menu lets the player change, and its settings file.
#pragma once

#include "gfx/backends/sdlgpu/sdlgpu_input.h"
#include "gfx/backends/sdlgpu/sdlgpu_window.h"

#include <cstdint>
#include <map>
#include <string>

namespace dq8::ui {

struct Settings {
    // Window
    bool fullscreen = false;
    int windowWidth = 0; // points; 0 picks a size from the display
    int windowHeight = 0;
    // Where the plain window was on the desktop, and whether it was
    // maximized (Rogue Galaxy: added); without a place it opens centred.
    bool windowPlaced = false;
    int windowX = 0;
    int windowY = 0;
    bool maximized = false;

    // Picture
    // Mailbox: vsync makes each present wait for the refresh while holding
    // SDL's submission lock, which the GS worker then waits on. The line blend
    // stays as the game draws it (the option only acts on DQ8's pattern).
    gfx::SdlGpuDisplayOptions display{.presentMode = SDL_GPU_PRESENTMODE_MAILBOX};
    uint32_t resolutionScale = 2u;

    // Sound
    int volume = 100; // percent
    bool muted = false;

    // Controls
    gfx::PadConfig pad = gfx::PadConfig::defaults();
    bool backStartOpensMenu = true;

    // Interface
    int uiScale = 100; // percent, on top of the display's own scale
    bool showFps = false;
    bool notifications = true;
    bool pauseInMenu = true;
    bool pauseInBackground = false;

    // Game
    float fastForwardSpeed = 2.0f; // 0 runs unpaced
    // The game's own switches (HostServices::gameOptions), by key, as the
    // file has them; one it does not have takes the option's default (Rogue
    // Galaxy: added).
    std::map<std::string, bool> gameOptions;
    // And the game's numbers. A key whose value reads as both (0 or 1) is in
    // both maps until the menu sets one of them.
    std::map<std::string, int> gameNumbers;
};

std::string serializeSettings(const Settings &settings);
// Unknown or malformed entries keep their current value in `settings`.
void parseSettings(const std::string &text, Settings &settings);

// RG_SETTINGS_FILE, or settings.ini in SDL's per-user preferences folder
// (on Windows %APPDATA%\RogueGalaxy\Recompiled\).
std::string defaultSettingsPath();
// A missing file leaves the defaults; returns false only when it cannot be read.
bool loadSettings(const std::string &path, Settings &settings);
bool saveSettings(const std::string &path, const Settings &settings);

} // namespace dq8::ui
