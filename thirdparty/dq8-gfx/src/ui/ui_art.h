// The home screen's pictures (Steam Deck fork: added): the ones the build
// embeds from installer/art (home.jpg, logo-home.png), each decoded once into
// a texture. Without one (a source tree that lacks the file) the home screen
// draws a sky, or the title in text.
#pragma once

#include <imgui.h>

namespace dq8::ui {

enum class HomePicture { Background, Logo };

// A picture as a texture and its size in pixels; false when there is none.
bool homeArt(HomePicture which, ImTextureRef &texture, ImVec2 &size);

// The embedded CHANGELOG.md as text; empty when the build has none.
const char *homeChangelog(unsigned int &size);

} // namespace dq8::ui
