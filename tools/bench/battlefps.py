"""Frame rate over the heavy intervals of battle-bench logs: battlefps.py NAME... [--draws N]

A battle bench run (`BATTLE=1 tools/bench/walk.sh NAME 3 60`) meets its
battle at a different moment each time, so the intervals are picked by
load instead of by time: those whose `[gfx]` line counts at least N draws
per 60 frames (default 40000, about a second; since the one-pass blends of
2026-10-30 the field runs at 20-40k and a battle at 45-65k, a fifth fewer
draws than before), scaled for other stats intervals (walk.sh STATS_EVERY).
Prints the count, median, minimum and mean of their frame rates.
"""
import argparse
import re


def heavy(name: str, threshold: int) -> list[float]:
    fps = []
    for line in open(f"build/run/{name}.log", encoding="utf-8", errors="replace"):
        m = re.search(r"\] ([0-9.]+) fps \(backend ([0-9]+)%[^)]*\) over ([0-9]+) frames.*draws=([0-9]+)", line)
        if m and int(m.group(4)) * 60 >= threshold * int(m.group(3)):
            fps.append(float(m.group(1)))
    return fps


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.strip().splitlines()[0])
    ap.add_argument("names", nargs="+")
    ap.add_argument("--draws", type=int, default=40000, help="draws per 60 frames an interval needs to count")
    args = ap.parse_args()
    for name in args.names:
        fps = sorted(heavy(name, args.draws))
        if not fps:
            print(f"{name}: no interval with {args.draws} draws per 60 frames or more")
            continue
        print(f"{name}: {len(fps)} heavy intervals, median {fps[len(fps) // 2]:.1f}, "
              f"min {fps[0]:.1f}, mean {sum(fps) / len(fps):.1f}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
