#pragma once

#include <cstdint>
#include <string>
#include <vector>

// RetroAchievements: the game's 130 achievements, earned while playing.
//
// The set is RetroAchievements' own (game 2671, Rogue Galaxy on PS2): each
// achievement is a condition over the PS2's memory, which the official
// library rcheevos (thirdparty/rcheevos, `rc_client`) downloads for the
// signed-in account, checks after every frame against the game's memory, and
// reports back when one is earned. This module is the host side of it:
//
// - memory: the guest's RAM as the console's (an address is an offset into
//   `rdram`, as RetroAchievements counts them);
// - the network: one `curl` per request, on a worker thread, answered on the
//   frame thread (rc_client is not thread safe);
// - the account: `ra/login.json` in the settings folder holds the user name
//   and the connection token that `tools/ra_fetch.py --save-login` writes
//   (never the password), mode 600;
// - the game: its hash is the md5 of "SCUS_974.90" and the executable, as
//   RetroAchievements hashes a PS2 disc;
// - what the player sees: toasts, handed to the menu by takeToast().
//
// It plays in softcore (no hardcore): the game's options change it too much.
// Off by default; the menu's "RetroAchievements", or RG_ACHIEVEMENTS=1.
namespace rg::achievements
{

void setEnabled(bool on);
bool enabled();

// Whether an account is saved (ra/login.json): the menu's option starts on then.
bool signedIn();

// The game's executable (disc/SCUS_974.90), for the hash. Before the first
// frame().
void setExecutable(const std::string &path);

// Once per presented frame, on the frame thread; `rdram` is the guest's RAM.
void frame(const uint8_t *rdram);

struct Toast
{
    std::string text;
    bool earned = false; // an achievement unlocked, rather than a notice
    std::string badge;   // its badge's PNG (may still be downloading), else empty
};
// The next toast waiting, false when none.
bool takeToast(Toast &toast);

// The set, for the menu's Achievements window, in RetroAchievements' order
// (the earned ones first, then the others, each in the set's own order).
// Empty until the game is loaded. Any thread.
struct Row
{
    std::string title;
    std::string description;
    uint32_t points = 0;
    bool earned = false;
    std::string badge; // the badge's PNG once downloaded, else empty
};
std::vector<Row> list();

// The account, for the menu's Achievements page. Signing in asks
// retroachievements.org for a connection token with the user and password
// (neither is kept), saves the token as ra/login.json and starts the
// achievements by itself; signing out deletes the file and stops them. Any
// thread; the work is done on its own thread and on the next frame.
struct Account
{
    bool signedIn = false;
    std::string user;
    std::string message; // what the last attempt said
    bool busy = false;   // a sign-in is under way
    bool failed = false; // `message` is a refusal
};
Account account();
void signIn(const std::string &user, const std::string &password);
void signOut();

// One line for the menu: why it is not running, or the game's progress.
std::string status();

} // namespace rg::achievements
