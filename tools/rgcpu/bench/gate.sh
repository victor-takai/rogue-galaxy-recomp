#!/bin/bash
# Frame compare for a change that must not move the default path (Linux).
#
#   tools/rgcpu/bench/gate.sh REF_BIN TEST_BIN [ENV ...]
#
# Runs the scripted walk (File 01 of build/run/mc0-bench, inputs on the pad
# clock, hidden and muted, paced) and logs the frame at pad read 3000 to
# build/run/gate-*.txt(.vram):
#   ref 1x twice (the walk must match itself, or nothing below means
#   anything), test 1x, ref 3x, test 3x; then the same test runs with each
#   ENV (e.g. RG_NATIVE_CPU=1), which are compared too but need not match.
# Prints tools/bench/cmpvram.py for each pair. Everything else stays at the
# game's defaults (no native renderer): each run starts from no settings file.
cd "$(dirname "$0")/../../.." || exit 1
REF=$1; TEST=$2; shift 2
if [ -z "$REF" ] || [ -z "$TEST" ]; then echo "usage: gate.sh REF_BIN TEST_BIN [ENV ...]"; exit 2; fi
if pgrep -x roguegalaxy > /dev/null; then echo "a game is running; not starting"; exit 2; fi
if [ ! -d build/run/mc0-bench/BASCUS-97490LV5RG_00 ]; then echo "needs build/run/mc0-bench (a card with File 01)"; exit 2; fi
export XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/$(id -u)}
export WAYLAND_DISPLAY=${WAYLAND_DISPLAY:-$(basename "$(ls "$XDG_RUNTIME_DIR"/wayland-? 2>/dev/null | head -1)")}
run() { # NAME BIN SCALE [ENV...]
    local name=$1 bin=$2 scale=$3; shift 3
    (
        export RG_INPUT="909:CROSS:5,999:CROSS:5,1059:DOWN:5,1118:CROSS:5,1238:CROSS:5,1358:CROSS:5,1478:CROSS:5,1569:CROSS:5,1629:CROSS:5,2019:CROSS:5,2069:CROSS:5,2223:CIRCLE:5,2277:CIRCLE:5,2900:LUP:240"
        export RG_INPUT_CLOCK=pad RG_MUTE=1 DQ8_GFX_HIDDEN_WINDOW=1 RG_FPS=30 RG_GS_SCALE=$scale
        export RG_MC0_DIR=build/run/mc0-bench RG_SETTINGS_FILE="build/run/gate-$name.ini" RG_STALL_WATCH=0
        export RG_GS_FRAMELOG="build/run/gate-$name.txt@3000"
        for e in "$@"; do export "$e"; done
        rm -f "build/run/gate-$name.txt" "build/run/gate-$name.txt.vram" "build/run/gate-$name.ini"
        "$bin" disc/SCUS_974.90 > "build/run/gate-$name.log" 2>&1 &
        pid=$!
        for _ in $(seq 1 240); do sleep 1; [ -s "build/run/gate-$name.txt.vram" ] && grep -q "frame logged" "build/run/gate-$name.log" && break; kill -0 $pid 2>/dev/null || break; done
        sleep 2; kill -INT $pid 2>/dev/null; sleep 3; kill -9 $pid 2>/dev/null
        grep -m1 -E "\[crash\]|fatal" "build/run/gate-$name.log"
    )
}
cmp() { python3 tools/bench/cmpvram.py "gate-$1" "gate-$2" | tail -1 | sed "s/^/$1 vs $2: /"; }
run ref1a "$REF" 1; run ref1b "$REF" 1; run test1 "$TEST" 1
run ref3 "$REF" 3; run test3 "$TEST" 3
i=0; for e in "$@"; do i=$((i+1)); run "env$i" "$TEST" 1 "$e"; done
echo "== gate"
cmp ref1a ref1b; cmp ref1a test1; cmp ref3 test3
i=0; for e in "$@"; do i=$((i+1)); echo "env$i = $e"; cmp ref1a "env$i"; done
