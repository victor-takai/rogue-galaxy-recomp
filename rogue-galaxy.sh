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
    fi
    exit 1
}

game=build/game/src/roguegalaxy
disc=disc/SCUS_974.90
[ -x "$game" ] || fail "The game is not built yet: see docs/BUILDING.md."
[ -e "$disc" ] || fail "No extracted disc in disc/: run  python3 setup.py extract  first."

# A second start while the game is up would start a second game.
if command -v flock >/dev/null 2>&1; then
    exec 9>build/.launcher.lock
    flock -n 9 || fail "Rogue Galaxy is already running."
fi

# The [gfx] line a second unless one is set already. The Vulkan layers are
# left alone: gamescope's and Steam's overlay are among them.
export DQ8_GFX_STATS_EVERY="${DQ8_GFX_STATS_EVERY:-60}"

[ -f build/last-run.log ] && cp -f build/last-run.log build/last-run.prev.log
"$game" "$disc" "$@" >build/last-run.log 2>&1 </dev/null
code=$?
if [ "$code" -ne 0 ]; then
    fail "The game stopped (code $code). The end of build/last-run.log:
$(tail -n 8 build/last-run.log)"
fi
