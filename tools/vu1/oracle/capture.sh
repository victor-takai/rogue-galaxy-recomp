#!/bin/bash
# An RG_VU1_REPLAY recording for rg_vu1_oracle, on Linux: File 01 from a copy
# of the card (build/run/mc0-bench, so the player's card is never touched),
# the gate bench's load sequence, then the field until the window ends.
#
#   tools/vu1/oracle/capture.sh NAME [FROM_LIST=2700] [LISTS=40]
#
# Writes build/run/NAME.vu1r (about 32 MB a list in Salgin's streets), the log
# build/run/NAME.log and a screenshot every 300 frames in build/run/shots/NAME,
# to see what the window caught. 30 fps mode, 1x, hidden window, muted. Lists
# are counted from boot, one a frame: 2700 is a few seconds into the field.
cd "$(dirname "$0")/../../.." || exit 1
NAME=$1; FROM=${2:-2700}; LISTS=${3:-40}
if [ -z "$NAME" ]; then echo "usage: capture.sh NAME [FROM_LIST] [LISTS]"; exit 2; fi
if pgrep -x roguegalaxy > /dev/null; then echo "a game is running; not starting"; exit 2; fi
if [ ! -d build/run/mc0-bench/BASCUS-97490LV5RG_00 ]; then
    mkdir -p build/run/mc0-bench && cp -r disc/mc0/BASCUS-97490LV5RG_00 build/run/mc0-bench/
fi
export XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/$(id -u)}
export WAYLAND_DISPLAY=${WAYLAND_DISPLAY:-$(basename "$(ls "$XDG_RUNTIME_DIR"/wayland-? 2>/dev/null | head -1)")}
export RG_INPUT="909:CROSS:5,999:CROSS:5,1059:DOWN:5,1118:CROSS:5,1238:CROSS:5,1358:CROSS:5,1478:CROSS:5,1569:CROSS:5,1629:CROSS:5,2019:CROSS:5,2069:CROSS:5,2223:CIRCLE:5,2277:CIRCLE:5"
export RG_INPUT_CLOCK=pad RG_MUTE=1 DQ8_GFX_HIDDEN_WINDOW=1 RG_FPS=30 RG_GS_SCALE=1
export RG_MC0_DIR=build/run/mc0-bench RG_SETTINGS_FILE=build/run/bench-settings.ini DQ8_GFX_STATS_EVERY=60
export DQ8_GFX_SCREENSHOT_DIR=build/run/shots/$NAME DQ8_GFX_SCREENSHOT_EVERY=300
export RG_VU1_REPLAY=build/run/$NAME.vu1r RG_VU1_REPLAY_FROM=$FROM RG_VU1_REPLAY_LISTS=$LISTS
mkdir -p "$DQ8_GFX_SCREENSHOT_DIR"
cat disc/*.DAT disc/*.HD6 > /dev/null 2>&1
build/game/src/roguegalaxy disc/SCUS_974.90 > "build/run/$NAME.log" 2>&1 &
pid=$!
for i in $(seq 1 150); do
    sleep 2
    grep -q "\[vu1replay\]\|\[crash\]\|fatal" "build/run/$NAME.log" && break
    kill -0 $pid 2>/dev/null || break
done
sleep 2
kill $pid 2>/dev/null; sleep 2; kill -9 $pid 2>/dev/null
grep "\[vu1replay\]\|\[crash\]\|fatal" "build/run/$NAME.log" | head -5
