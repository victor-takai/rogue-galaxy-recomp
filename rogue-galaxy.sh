#!/bin/sh
# Rogue Galaxy on Linux and the Steam Deck: what "Rogue Galaxy.exe"
# (src/launcher/launcher.c) does on Windows. Starts the game from this
# folder, with the log in build/last-run.log (the previous run's in
# build/last-run.prev.log). Extra arguments go to the game.
cd "$(dirname "$(readlink -f "$0")")" || exit 1

fail() {
    echo "Rogue Galaxy: $1" >&2
    if command -v kdialog >/dev/null 2>&1; then
        kdialog --error "$1" --title "Rogue Galaxy" 2>/dev/null
    elif command -v zenity >/dev/null 2>&1; then
        zenity --error --text="$1" --title="Rogue Galaxy" 2>/dev/null
    elif command -v notify-send >/dev/null 2>&1; then
        notify-send -u critical "Rogue Galaxy" "$1" 2>/dev/null
    elif command -v xmessage >/dev/null 2>&1; then
        xmessage -center "Rogue Galaxy: $1" 2>/dev/null
    fi
    exit 1
}

game=build/game/src/roguegalaxy
disc=disc/SCUS_974.90
# An install or update running in this folder holds build/install.lock, and
# its build in the container build/container.lock (install.sh's busy): the
# game would start half built.
if command -v flock >/dev/null 2>&1; then
    for lock in build/install.lock build/container.lock; do
        if [ -e "$lock" ] && ! flock -n "$lock" true; then
            fail "An install or update is running in this folder. Let it end, then start the game again."
        fi
    done
fi
# Not built yet, the disc not out whole (disc/manifest.json comes last), or
# a release archive unpacked over an older build (its REVISION is not the one
# built); the same test as install.sh's ready: the installer builds it, in a
# terminal it opens when there is none, then starts the game. Through sh: an
# archive unpacked without its modes leaves install.sh not runnable.
if ! [ -x "$game" ] || ! [ -e "$disc" ] || ! [ -e disc/manifest.json ] \
    || { [ -f REVISION ] && [ "$(cat REVISION)" != "$(cat build/installed-revision 2>/dev/null)" ]; }; then
    [ -f install.sh ] && exec sh ./install.sh --play
    [ -x "$game" ] || fail "The game is not built yet: see docs/BUILDING.md."
    [ -e "$disc" ] || fail "No extracted disc in disc/: run  python3 setup.py extract  first."
fi

# A second start while the game is up would start a second game. The game
# that restarts itself after an update (RG_RELAUNCH) is not a second one: the
# script that started it still holds the lock.
if [ -z "$RG_RELAUNCH" ] && command -v flock >/dev/null 2>&1; then
    exec 9>build/.launcher.lock
    flock -n 9 || fail "Rogue Galaxy is already running."
fi

# The [gfx] line a second unless one is set already. The Vulkan layers are
# left alone: gamescope's and Steam's overlay are among them.
export DQ8_GFX_STATS_EVERY="${DQ8_GFX_STATS_EVERY:-60}"
# The window's app id, so the desktop matches it to rogue-galaxy.desktop (icon).
export SDL_APP_ID="${SDL_APP_ID:-rogue-galaxy}"

[ -f build/last-run.log ] && cp -f build/last-run.log build/last-run.prev.log
"$game" "$disc" "$@" >build/last-run.log 2>&1 </dev/null
code=$?
if [ "$code" -ne 0 ]; then
    fail "The game stopped (code $code). The end of build/last-run.log:
$(tail -n 8 build/last-run.log)"
fi
