#pragma once

#include <string>

// The update the home screen starts, all inside the game's window.
//
// The newer release comes from update_check. "Update" runs update.sh (in the
// game's folder), which downloads the release's source archive, checks its
// checksum, unpacks it over the folder and builds it in the distrobox
// container, as install.sh does; the script writes its progress to
// build/update.progress ("<fraction or -1> <text>" on one line) and the
// game shows it. When the script succeeds the game starts again through
// rogue-galaxy.sh, in the same process, so Steam sees no exit.
namespace rg::selfupdate
{

enum class State
{
    Unknown,   // the release check has not answered, or there is no network
    UpToDate,
    Available,
    Working,
    Failed,
    Done,
};

struct Status
{
    State state = State::Unknown;
    std::string version; // the newer release, when there is one
    std::string text;    // what is being done, or what went wrong
    float progress = -1.0f;
};

Status status();

// Starts the update (Available or Failed); does nothing otherwise.
void start();

} // namespace rg::selfupdate
