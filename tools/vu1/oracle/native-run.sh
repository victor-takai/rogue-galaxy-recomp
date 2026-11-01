#!/bin/bash
# RG_VU1_NATIVE=MODE on the field after File 01 loads (from a copy of the
# card, build/run/mc0-bench),
# 30 fps mode, 1x, hidden and muted; the frame at pad read FRAME logged to
# build/run/NAME.txt(.vram); SECONDS of play.
#   tools/vu1/oracle/native-run.sh NAME MODE [SECONDS=110] [FRAME=3000]
# MODE is RG_VU1_NATIVE's (0, check, 1, skip, gpu; src/vu1_native.h). The
# 30 fps walk is deterministic: two runs of one build log the same frame, so
# tools/bench/cmpvram.py and tools/bench/vram2png.py compare modes.
cd "$(dirname "$0")/../../.." || exit 1
NAME=$1; MODE=$2; SECS=${3:-110}; FRAME=${4:-3000}
if pgrep -x roguegalaxy > /dev/null; then echo "a game is running; not starting"; exit 2; fi
if [ ! -d build/run/mc0-bench/BASCUS-97490LV5RG_00 ]; then
    mkdir -p build/run/mc0-bench && cp -r disc/mc0/BASCUS-97490LV5RG_00 build/run/mc0-bench/
fi
export XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/$(id -u)}
export WAYLAND_DISPLAY=${WAYLAND_DISPLAY:-$(basename "$(ls "$XDG_RUNTIME_DIR"/wayland-? 2>/dev/null | head -1)")}
export RG_INPUT="909:CROSS:5,999:CROSS:5,1059:DOWN:5,1118:CROSS:5,1238:CROSS:5,1358:CROSS:5,1478:CROSS:5,1569:CROSS:5,1629:CROSS:5,2019:CROSS:5,2069:CROSS:5,2223:CIRCLE:5,2277:CIRCLE:5,2900:LUP:240,3200:LLEFT:120,3400:LDOWN:200"
export RG_INPUT_CLOCK=pad RG_MUTE=1 DQ8_GFX_HIDDEN_WINDOW=1 RG_FPS=30 RG_GS_SCALE=1
export RG_MC0_DIR=build/run/mc0-bench RG_SETTINGS_FILE=build/run/bench-settings.ini DQ8_GFX_STATS_EVERY=60
export RG_GS_FRAMELOG="build/run/$NAME.txt@$FRAME" RG_VU1_NATIVE=$MODE DQ8_GFX_SCREENSHOT_DIR=build/run/shots/$NAME DQ8_GFX_SCREENSHOT_EVERY=600
mkdir -p build/run/shots/$NAME
unset RG_VU1_REPLAY
cat disc/*.DAT disc/*.HD6 > /dev/null 2>&1
build/game/src/roguegalaxy disc/SCUS_974.90 > "build/run/$NAME.log" 2>&1 &
pid=$!
sleep "$SECS"
kill $pid 2>/dev/null; sleep 2; kill -9 $pid 2>/dev/null
grep -E "frame logged|\[crash\]|fatal" "build/run/$NAME.log" | head -3
