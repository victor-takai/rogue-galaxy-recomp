# RetroAchievements

**In short:** the game can earn the 130 achievements of
[RetroAchievements](https://retroachievements.org/game/2671) (Rogue Galaxy,
PS2). The library rcheevos (`thirdparty/rcheevos`) downloads each one's
condition, a test over the PS2's memory, for your account, checks them after
every frame, and tells RetroAchievements when one is earned. `src/achievements.cpp`
is the game's side of it.

## Using it

1. Sign in once. The installer's Options has "Earn the game's 130
   achievements": ticking it shows the user, the password and Sign in; with the game already installed, run this where
   you can type a password (a terminal, on the Deck in desktop mode):

   ```bash
   python3 tools/ra_fetch.py --user YOUR_NAME --save-login
   ```

   The password is asked, sent to retroachievements.org and forgotten. What is
   kept is a connection token, in `~/.local/share/RogueGalaxy/ra/login.json`
   (mode 600): delete the file to sign out. The same run writes the set to
   `ra/game_2671.json`, which is only for reading the conditions: the game
   downloads its own.
2. The F1 menu's "RetroAchievements" starts on once an account is saved (or
   start with `RG_ACHIEVEMENTS=1`). A notice says how many you have, and another shows
   at each one earned.

## What you see

- **A notice** at each one earned, with its badge, and one at the start with
  how many you have.
- **The Achievements page**: F1 menu, Settings, Achievements (also Rogue
  Galaxy, Achievements). At the top the account: who is signed in with a
  Sign out button, or a user and a password with Sign in (typed with the
  keyboard, or the Steam keyboard on the Deck); signing in turns the option
  on and starts the achievements at once, signing out turns it off. Then the
  option, and the list: each achievement has its badge (grey until earned,
  gold frame once earned), its points and its description, the earned ones
  first as on RetroAchievements' page. The badges are downloaded from
  RetroAchievements' media server into `ra/badges/` the first time.

## Testing

An achievement is a change, not a state: most conditions are "this bit goes
from 0 to 1" while the game is running. Loading a save triggers nothing; the
action has to happen. `RG_ACHIEVEMENTS_TEST=1` runs the checks without
sending anything (spectator and encore modes) and is for reading conditions
against a running game, with `RG_WATCH` on the addresses
([DEVELOPMENT.md](DEVELOPMENT.md)). Note that RetroAchievements sees this
client as an unknown emulator (its "Warning: Unknown Emulator" achievement
shows in the log): unlocks in softcore are still accepted, as the first one
(Teleporting to Rosa) was.

## How it fits

- **Memory.** An address in a condition is an offset in the PS2's 32 MB of
  RAM: the same as an offset in the guest's `rdram`.
- **The game's hash.** RetroAchievements knows a PS2 disc by the md5 of the
  boot file's name and contents (`SCUS_974.90`): `disc/SCUS_974.90` gives the
  hash of the USA disc, set 2671.
- **Softcore only.** Hardcore forbids what this fork's options do (speed,
  widescreen, save states in the menu), so unlocks count as softcore.
- **The network.** One `curl` per request, on a worker thread, with the
  answer handed back on the frame thread. The body goes through a file only
  you can read, since it holds the token.
- **Not in git.** The set is its authors' work: the repository holds neither
  the conditions nor the titles.
