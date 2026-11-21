r"""Frame pacing from a present log: present_log.py LOG [FROM TO] [--hz HZ]

LOG is the CSV a run writes with DQ8_GFX_PRESENT_LOG=FILE, a line per
present to the window. FROM and TO are seconds from its first line. --hz is
the display's refresh rate (60 unless given; the Steam Deck OLED's is 90
unless set lower).

For the frames the game flipped (kind "flip") it prints:
- how many a second, and the frames shown at a vsync with no flip (kind
  "vblank": loading, movies, and every frame with RG_PRESENT=vblank or
  RG_GS_WORKER=0) and the refreshes of the last frame, which are left out of
  the rest;
- the time between two presents going out, and the same in display
  refreshes, rounded: 0 is a present less than half a refresh after the one
  before, which a mailbox or immediate swapchain may replace before it is
  seen;
- the delays on a frame's way, from its flip: until the presenter took it
  (up to a guest vsync tick with RG_PRESENT_TICK=1), the GS worker reaching
  it (under the native pipeline, its composing), its drawing on the GPU
  (fence), a swapchain image (acquire), and the
  whole way to the present going out. A spread of the whole wider than
  about half a refresh moves frames across refreshes, which shows as 1 and
  3 refreshes in turn at 30 fps.

What happens after the present (the compositor, the display) is not in the
log. With the window hidden (DQ8_GFX_HIDDEN_WINDOW, walk.sh) nothing is
presented: the moments the presenter took the frames stand in, and the
acquire and the whole way are left out.
"""
import csv
import sys


def percentiles(values, points=(5, 50, 95)):
    if not values:
        return "none"
    ordered = sorted(values)
    parts = []
    for p in points:
        index = min(len(ordered) - 1, round(p / 100 * (len(ordered) - 1)))
        parts.append(f"p{p} {ordered[index]:.1f}")
    parts.append(f"max {ordered[-1]:.1f}")
    return ", ".join(parts)


def histogram(steps):
    counts = {}
    for step in steps:
        key = min(step, 5)
        counts[key] = counts.get(key, 0) + 1
    total = len(steps) or 1
    return "  ".join(f"{'5+' if key == 5 else key}: {counts[key]} ({counts[key] * 100 / total:.0f}%)"
                     for key in sorted(counts))


def main() -> int:
    args = sys.argv[1:]
    hz = 60.0
    if "--hz" in args:
        i = args.index("--hz")
        if i + 1 >= len(args):
            print(__doc__)
            return 2
        hz = float(args[i + 1])
        del args[i:i + 2]
    if not args:
        print(__doc__)
        return 2

    with open(args[0], newline="") as f:
        rows = list(csv.DictReader(f))
    if not rows:
        print("the log is empty")
        return 1
    origin = float(rows[0]["taken_ms"])
    lo = origin + (float(args[1]) * 1000 if len(args) > 1 else 0.0)
    hi = origin + (float(args[2]) * 1000 if len(args) > 2 else 1e12)

    flips = []
    vblanks = refreshes = 0
    for row in rows:
        taken = float(row["taken_ms"])
        if taken < lo or taken > hi:
            continue
        if row["kind"] == "vblank":
            vblanks += 1
            continue
        if row["kind"] != "flip":
            refreshes += 1
            continue
        flips.append({key: float(row[key]) for key in
                      ("flip_ms", "taken_ms", "worker_ms", "fence_ms", "acquire_ms", "submitted_ms")})
    if len(flips) < 2:
        print("fewer than two flipped frames in the log")
        return 1

    # A visible run times the presents; a hidden one only the presenter's
    # takes. Frames that went nowhere in a visible run (minimised, no
    # picture) are left out rather than timed on the other clock.
    presented = [f for f in flips if f["submitted_ms"] >= 0]
    hidden = not presented
    timed = flips if hidden else presented
    out = "taken_ms" if hidden else "submitted_ms"
    span = (timed[-1][out] - timed[0][out]) / 1000 if len(timed) > 1 else 0.0
    if span <= 0:
        print(f"{len(flips)} flipped frames, {len(presented)} of them presented: too few to time"
              " (the window minimised most of the run?)")
        return 1

    refresh = 1000.0 / hz
    print(f"{len(flips)} flipped frames over {span:.1f} s: {(len(timed) - 1) / span:.1f} a second"
          f" (besides: {vblanks} shown at a vsync with no flip, {refreshes} refreshes)")
    if hidden:
        print("window hidden: nothing was presented; the times are the presenter's takes")
    elif len(presented) < len(flips):
        print(f"{len(flips) - len(presented)} flipped frames went to no window (minimised, no picture): left out")
    gaps = [b[out] - a[out] for a, b in zip(timed, timed[1:])]
    print(f"between presents, ms: {percentiles(gaps)}")
    print(f"between presents, in {hz:g} Hz refreshes: {histogram([round(g / refresh) for g in gaps])}")
    print(f"from the flip, ms (half a refresh: {refresh / 2:.1f}):")
    print(f"  until the presenter took it: {percentiles([f['taken_ms'] - f['flip_ms'] for f in timed])}")
    print(f"  the GS worker reaching it (native pipeline: its composing): "
          f"{percentiles([f['worker_ms'] for f in timed])}")
    print(f"  its drawing on the GPU (fence): {percentiles([f['fence_ms'] for f in timed])}")
    if not hidden:
        print(f"  a swapchain image (acquire): {percentiles([f['acquire_ms'] for f in timed])}")
        print(f"  the whole way, to the present going out: "
              f"{percentiles([f['submitted_ms'] - f['flip_ms'] for f in timed])}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
