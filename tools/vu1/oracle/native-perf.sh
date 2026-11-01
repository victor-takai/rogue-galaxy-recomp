#!/bin/bash
# The speed of RG_VU1_NATIVE=MODE on the field after File 01 loads, in the
# 60 fps mode at SCALE: the last 25 s of SECONDS (the walk is over by then and
# the party stands in the street), from the [gfx] lines (frame rate, the GS
# backend's share of the wall, the VIF1 thread's and the GIF stage's) and the
# GPU's busy share and power draw, sampled every half second.
#   tools/vu1/oracle/native-perf.sh NAME MODE SCALE [SECONDS=110]
cd "$(dirname "$0")/../../.." || exit 1
NAME=$1; MODE=$2; SCALE=${3:-3}; SECS=${4:-110}
if [ -z "$MODE" ]; then echo "usage: native-perf.sh NAME MODE SCALE [SECONDS]"; exit 2; fi
if pgrep -x roguegalaxy > /dev/null; then echo "a game is running; not starting"; exit 2; fi
if [ ! -d build/run/mc0-bench/BASCUS-97490LV5RG_00 ]; then
    mkdir -p build/run/mc0-bench && cp -r disc/mc0/BASCUS-97490LV5RG_00 build/run/mc0-bench/
fi
export XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/$(id -u)}
export WAYLAND_DISPLAY=${WAYLAND_DISPLAY:-$(basename "$(ls "$XDG_RUNTIME_DIR"/wayland-? 2>/dev/null | head -1)")}
export RG_INPUT="909:CROSS:5,999:CROSS:5,1059:DOWN:5,1118:CROSS:5,1238:CROSS:5,1358:CROSS:5,1478:CROSS:5,1569:CROSS:5,1629:CROSS:5,2019:CROSS:5,2069:CROSS:5,2223:CIRCLE:5,2277:CIRCLE:5,2900:LUP:240,3200:LLEFT:120,3400:LDOWN:200"
export RG_INPUT_CLOCK=pad RG_MUTE=1 DQ8_GFX_HIDDEN_WINDOW=1 RG_FPS=60 RG_GS_SCALE=$SCALE
export RG_MC0_DIR=build/run/mc0-bench RG_SETTINGS_FILE=build/run/bench-settings.ini DQ8_GFX_STATS_EVERY=60
export RG_VU1_NATIVE=$MODE
unset RG_VU1_REPLAY RG_GS_FRAMELOG DQ8_GFX_SCREENSHOT_DIR
card=$(dirname "$(ls /sys/class/drm/card*/device/gpu_busy_percent | head -1)")
cat disc/*.DAT disc/*.HD6 > /dev/null 2>&1
build/game/src/roguegalaxy disc/SCUS_974.90 > "build/run/$NAME.log" 2>&1 &
pid=$!
sleep $((SECS - 25))
: > "build/run/$NAME.gpu"
for i in $(seq 1 50); do
    echo "$(cat "$card/gpu_busy_percent") $(cat "$card"/hwmon/hwmon*/power1_average)" >> "build/run/$NAME.gpu"
    sleep 0.5
done
kill $pid 2>/dev/null; sleep 2; kill -9 $pid 2>/dev/null
python3 - "$NAME" "$MODE" "$SCALE" <<'PY'
import re, sys
name, mode, scale = sys.argv[1:4]
lines = open(f"build/run/{name}.log", errors="replace").read().splitlines()
gfx = [l for l in lines if re.match(r"\[gfx\] [0-9.]+ fps", l)][-26:-1]
busy = [l for l in lines if "vif1 busy=" in l][-26:-1]
avg = lambda xs: sum(xs) / len(xs) if xs else float("nan")
fps = avg([float(re.search(r"([0-9.]+) fps", l).group(1)) for l in gfx])
backend = avg([float(re.search(r"backend ([0-9]+)%", l).group(1)) for l in gfx])
vif1 = avg([float(re.search(r"vif1 busy=([0-9]+)%", l).group(1)) for l in busy])
gif = avg([float(re.search(r"gif busy=([0-9]+)%", l).group(1)) for l in busy])
draws = avg([float(re.search(r"draws=([0-9]+)", l).group(1)) for l in gfx])
samples = [l.split() for l in open(f"build/run/{name}.gpu") if l.strip()]
gpu = avg([float(s[0]) for s in samples]); watts = avg([float(s[1]) / 1e6 for s in samples])
print(f"{name:10s} mode={mode:5s} {scale}x  fps {fps:5.1f}  GS backend {backend:4.0f}% of wall  "
      f"VIF1 {vif1:3.0f}%  GIF {gif:3.0f}%  draws/s {draws:7.0f}  GPU busy {gpu:3.0f}%  {watts:5.1f} W")
PY
