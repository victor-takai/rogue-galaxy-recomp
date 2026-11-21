#!/bin/bash
# The native pipeline's pass criteria (docs/NATIVE-PIPELINE.md, steps 5-7)
# from a run's log: build/run/NAME.log. Prints one line per check.
#   tools/native-check.sh NAME
set -u
LOG="build/run/${1:?usage: native-check.sh NAME}.log"
[ -f "$LOG" ] || { echo "no $LOG"; exit 1; }

# The sum of a "label N" count in the producer's lines.
count() { grep -ao "$1 [0-9]*" "$LOG" | awk '{s+=$NF} END {print s+0}'; }
verdict() { if [ "$2" = "0" ]; then echo "PASS  $1"; else echo "FAIL  $1 ($2)"; fi; }

echo "== $LOG"
verdict "7: GS backend never entered (tripwire)" "$(grep -ac '^\[native-gs\] the GS backend was entered' "$LOG")"
grep -a '^\[native-gs\] GS backend calls' "$LOG" | tail -1 | sed 's/^/INFO  7 tripwire: /'
verdict "5: no effect left as packets" "$(grep -ac '^\[nativefx\] an effect stays packets' "$LOG")"
verdict "5b: no packet left unconverted" "$(grep -ac '^\[native2d\] a packet stays a packet' "$LOG")"
verdict "5c: no composite tail left as packets" "$(grep -ac '^\[composite\] a tail stays packets' "$LOG")"
verdict "7c: every colour read served" "$(grep -ac 'a colour read was not served' "$LOG")"
grep -a 'source is unknown' "$LOG" | sort | uniq -c | sed 's/^/INFO  6: /'
echo "INFO  6: local copies mirrored: $(count 'local copies mirrored'), other format: $(count 'other format'), no source: $(count 'no source'), no destination: $(count 'no destination')"
grep -ao "pieces made [0-9]*, to the GS [0-9]*, dropped [0-9]*" "$LOG" | tail -1 | sed 's/^/INFO  5b refused pieces (dropped, not the tripwire): /'
echo "INFO  5b: green-to-alpha ops made: $(count 'made green to alpha')"
echo "INFO  5c: objects routed: $(count 'object layers: routed'), tails made: $(count 'tails made'), put back: $(count 'put back for packets')"
grep -ao "depth reads: asked.*" "$LOG" | tail -1 | sed 's/^/INFO  7b renderer: /'
grep -ao "colour reads: served.*" "$LOG" | tail -1 | sed 's/^/INFO  7c renderer: /'
echo "-- errors:"
grep -a "depth reads off\|colour read .* failed\|frame lost\|\[native-pipeline\] refused:" "$LOG" | sort | uniq -c | head
