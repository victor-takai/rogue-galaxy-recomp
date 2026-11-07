r"""The stats intervals of a run as a table, for short hitches: hitches.py LOG [FROM TO] [--over MS]

LOG is a log written with DQ8_GFX_STATS_EVERY (build/last-run.log of
debug\RogueGalaxy-hitches.bat, which prints every 10 frames, or a bench's
build/run/NAME.log with STATS_EVERY=10). Each row is one interval: its end
in seconds from the first stats line (summed from the frame rates, so it
drifts a little against the [mark] times), the frame rate, the longest
frame (between two presents), the longest the main thread was held up, the
VIF1 thread's, the GIF stage's and the GS worker's busy shares of the wall,
the draws, the texture builds, the readback wait a frame, the GPU pipelines
created and the VU1 pairs that ran on the interpreter. F9 marks and overlay
loads and profile windows are rows of their own. --over MS keeps only the intervals whose
longest frame exceeds MS (and the marks).

A bench's frame capture (RG_GS_FRAMELOG, "[gs-census] frame logged") is a
hitch of its own, a quarter to half a second: not the game's. A hitch that
holds the main thread as an interval's line is printed shows its longest
frame in that row and its main stall in the next.
"""
import re
import sys


def main() -> int:
    args = sys.argv[1:]
    over = None
    if "--over" in args:
        i = args.index("--over")
        if i + 1 >= len(args):
            print(__doc__)
            return 2
        over = float(args[i + 1])
        del args[i:i + 2]
    if not args:
        print(__doc__)
        return 2
    lo = float(args[1]) if len(args) > 1 else 0.0
    hi = float(args[2]) if len(args) > 2 else 1e9
    t = 0.0
    busy = {}
    rows = []
    with open(args[0], encoding="utf-8", errors="replace") as log:
        for line in log:
            m = re.search(r"vif1 busy=(\d+)% gif busy=(\d+)% of wall(.*)", line)
            if m:
                busy = {"vif1": int(m.group(1)), "gif": int(m.group(2))}
                stall = re.search(r"main stall ([\d.]+) ms", m.group(3))
                busy["stall"] = float(stall.group(1)) if stall else 0.0
                vu1 = re.search(r"vu1 interpreted ([0-9.]+k pairs/s|\d+ pairs) \((\d+)%", m.group(3))
                busy["interp"] = "%s (%s%%)" % vu1.groups() if vu1 else ""
                continue
            m = re.match(r"\[gfx\] ([\d.]+) fps \(backend (\d+)% of wall\) over (\d+) frames .*? draws=(\d+)", line)
            if m:
                fps = float(m.group(1))
                t += int(m.group(3)) / fps if fps > 0 else 0.0
                built = re.search(r"tex builds=(\d+) \((\d+) Ktexels\)", line)
                rows.append(dict(busy, t=t, fps=fps, worker=int(m.group(2)), draws=int(m.group(4)),
                                 built="%s/%sK" % built.groups() if built else "", longest=0.0, wait=0.0, pipes=""))
                continue
            m = re.search(r"resolves by reason:.*readback wait ([\d.]+) ms/frame(.*)", line)
            # The interval's last line; a mark printed by another thread may
            # have come between it and the fps line.
            interval = next((row for row in reversed(rows) if "fps" in row), None)
            if m and interval is not None:
                interval["wait"] = float(m.group(1))
                pipes = re.search(r"pipelines \+(\d+) \(([\d.]+) ms\)", m.group(2))
                interval["pipes"] = "+%s (%s ms)" % pipes.groups() if pipes else ""
                longest = re.search(r"longest frame ([\d.]+) ms", m.group(2))
                interval["longest"] = float(longest.group(1)) if longest else 0.0
                continue
            m = re.match(r"\[(mark|overlay|gs-census|profile)\](.*)", line)
            if m:
                rows.append({"t": t, "note": line.strip()[:120]})
    print("%7s %6s %8s %7s %5s %5s %6s %6s %10s %6s %14s %s" % (
        "t", "fps", "longest", "stall", "vif1", "gif", "worker", "draws", "texbuild", "wait", "pipelines", "vu1 interp"))
    for row in rows:
        if not lo <= row["t"] <= hi:
            continue
        if "note" in row:
            print("        %s" % row["note"])
        elif over is None or row["longest"] > over:
            print("%7.2f %6.1f %8.1f %7.1f %4d%% %4d%% %5d%% %6d %10s %6.2f %14s %s" % (
                row["t"], row["fps"], row["longest"], row.get("stall", 0.0), row.get("vif1", 0), row.get("gif", 0),
                row["worker"], row["draws"], row["built"], row["wait"], row["pipes"], row.get("interp", "")))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
