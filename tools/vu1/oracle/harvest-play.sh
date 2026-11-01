#!/bin/bash
# Play this build normally while it collects samples of the VU1 programs it
# has not met (RG_VU1_HARVEST, into build/harvest) and checks the native ones
# against the VU1 (RG_VU1_NATIVE=check, one chunk in 16: checking every one
# halved the frame rate at a boss). Your own memory card and settings,
# window and sound as usual. A [harvest] line in the log names each new
# program; build/harvest/ is what to hand back (tools/vu1/census.py and
# rg_vu1_oracle read it).
#   tools/vu1/oracle/harvest-play.sh [extra environment, e.g. RG_FPS=30]
cd "$(dirname "$0")/../../.." || exit 1
if pgrep -x roguegalaxy > /dev/null; then echo "a game is running; not starting"; exit 2; fi
export XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/$(id -u)}
export WAYLAND_DISPLAY=${WAYLAND_DISPLAY:-$(basename "$(ls "$XDG_RUNTIME_DIR"/wayland-? 2>/dev/null | head -1)")}
export RG_VU1_HARVEST=${RG_VU1_HARVEST:-build/harvest} RG_VU1_NATIVE=${RG_VU1_NATIVE:-check}
export RG_VU1_NATIVE_CHECK_EVERY=${RG_VU1_NATIVE_CHECK_EVERY:-16}
# The [gfx] stats every 600 frames, so a slow stretch shows in the log.
export DQ8_GFX_STATS_EVERY=${DQ8_GFX_STATS_EVERY:-600}
for assignment in "$@"; do export "$assignment"; done
mkdir -p build/run
log="build/run/harvest-$(date +%Y%m%d-%H%M%S).log"
echo "playing; log in $log, samples in $RG_VU1_HARVEST"
build/game/src/roguegalaxy disc/SCUS_974.90 > "$log" 2>&1
echo "new programs this session: $(grep -c '\[harvest\] new program' "$log")"
grep '\[vu1native\]' "$log" | tail -1
