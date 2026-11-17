#pragma once

#include <string>

// Whether a newer release of this project is out.
//
// Once a run, a few seconds after the start and unless the menu's "Check
// for new versions" is off by then, a background thread asks GitHub's API
// for the project's releases (one HTTPS GET, nothing sent but the request)
// and compares the newest one's tag with this build's version. The menu
// says so when it is newer, and its About window links to it.
namespace rg::updates
{

struct Release
{
    std::string version; // empty while none newer is known
    std::string url;
};

// Starts the check; `current` is this build's version (RG_APP_VERSION). The
// home screen asks at once (`delayed` false); in the game it waits a few
// seconds.
void start(const char *current, bool delayed = true);

// Whether the check has answered, even with nothing newer or no network.
bool checked();

// Whether GitHub answered with a release (false: offline, or none published).
bool reachable();

void setEnabled(bool on);

// The newer release found, once the check is done.
Release newer();

} // namespace rg::updates
