#!/usr/bin/env python3
"""Summarize an RG_VU1_CAPTURE recording: what each VU1 program starts with.

  RG_VU1_CAPTURE=build/vu1.cap build/game/src/roguegalaxy disc/SCUS_974.90
  python3 tools/vu1/capture_report.py build/vu1.cap [--quads 4-11,64-75] [--sample N]

Each record is one microprogram start: its tag (0x8 | entry << 16 | microcode hash, as
the draw tables name it), entry, TOP, ITOP, the number of the DMA list, the EE address
the model matrix (quads 60-63) was uploaded from, and all 16 KiB of VU1 data memory.

Prints, per program: the starts, which quadwords changed between starts (per-object
inputs) and which never did, and one sample's floats. Then a check of the matrix
layout (model 60-63 x view 24-27 x projection 8-11 = 72-75) and how well the model
matrix's source address identifies an object from one list to the next.
"""
import argparse
import struct
import sys
from collections import defaultdict
from pathlib import Path

FULL = 0x44315556   # "VU1D": all of VU1 data memory
LIGHT = 0x4C315556  # "VU1L": only the matrices (quads 8-11, 24-27, 60-63, 72-75)
DATA = 16384
HEAD = 28
LIGHT_QUADS = (8, 24, 60, 72)


def ranges(spec):
    out = []
    for part in spec.split(","):
        a, _, b = part.partition("-")
        out += range(int(a), int(b or a) + 1)
    return out


def runs(items):
    out, start, prev = [], None, None
    for i in items:
        if start is None:
            start = prev = i
        elif i == prev + 1:
            prev = i
        else:
            out.append((start, prev))
            start = prev = i
    if start is not None:
        out.append((start, prev))
    return ["%d" % a if a == b else "%d-%d" % (a, b) for a, b in out]


def quad(mem, q):
    return struct.unpack_from("<4f", mem, q * 16)


def matrix(mem, q):
    return [list(quad(mem, q + i)) for i in range(4)]


def mul(a, b):
    return [[sum(a[i][k] * b[k][j] for k in range(4)) for j in range(4)] for i in range(4)]


class Start:
    """One record, read in place from the file's bytes (a capture can hold millions)."""
    __slots__ = ("raw", "at", "tag", "pc", "top", "itop", "list", "source", "full", "shape", "whole", "size")

    def __init__(self, raw, at):
        self.raw, self.at = raw, at
        magic, self.tag, self.pc, self.top, self.itop, self.list, self.source = struct.unpack_from("<7I", raw, at)
        self.full = magic == FULL
        if magic == FULL:
            self.size = HEAD + DATA
            self.shape = self.whole = None
        elif magic == LIGHT:
            self.shape, self.whole = struct.unpack_from("<2I", raw, at + HEAD)
            self.size = HEAD + 8 + 64 * len(LIGHT_QUADS)
        else:
            sys.exit("bad record at %d (an old capture? record it again)" % at)

    @property
    def mem(self):
        return self.raw[self.at + HEAD:self.at + HEAD + DATA]  # full records only

    def q(self, q):
        """Quadword q as four floats; a light record has only 8-11, 24-27, 60-63 and 72-75."""
        if self.full:
            return struct.unpack_from("<4f", self.raw, self.at + HEAD + q * 16)
        for i, base in enumerate(LIGHT_QUADS):
            if base <= q < base + 4:
                return struct.unpack_from("<4f", self.raw, self.at + HEAD + 8 + i * 64 + (q - base) * 16)
        raise KeyError(q)

    def m(self, q):
        return [list(self.q(q + i)) for i in range(4)]


def read_starts(raw):
    starts, at = [], 0
    while at + HEAD <= len(raw):
        magic = struct.unpack_from("<I", raw, at)[0]
        size = HEAD + (DATA if magic == FULL else 8 + 64 * len(LIGHT_QUADS))
        if at + size > len(raw):
            break  # a record cut short when the game closed
        s = Start(raw, at)
        starts.append(s)
        at += size
    return starts


def check_matrices(starts):
    checked = bad = 0
    worst = 0.0
    for s in starts[::max(1, len(starts) // 50000)]:  # a sample is enough
        c = s.m(72)
        if any(x != x for r in c for x in r):
            continue
        t = mul(mul(s.m(60), s.m(24)), s.m(8))
        err = max(abs(t[i][j] - c[i][j]) / (1 + abs(c[i][j])) for i in range(4) for j in range(4))
        checked += 1
        worst = max(worst, err)
        bad += err > 1e-3
    return checked, bad, worst


def identity_report(starts):
    by_source = defaultdict(list)
    found = 0
    for s in starts:
        if s.source:
            found += 1
            by_source[s.source].append(s)
    print("\nobject identity: %d of %d starts found their model matrix in the list (%d distinct EE addresses)" %
          (found, len(starts), len(by_source)))
    lists = sorted({s.list for s in starts})
    print("  %d lists in the capture" % len(lists))
    repeat = [v for v in by_source.values() if len({s.list for s in v}) > 1]
    print("  addresses seen in more than one list: %d of %d" % (len(repeat), len(by_source)))
    # does the matrix at an address move smoothly from list to list?
    jumps = steps = 0
    for v in repeat:
        last = {}
        for s in v:
            if s.list in last:
                continue
            last[s.list] = s.q(63)
        seq = [last[k] for k in sorted(last)]
        for a, b in zip(seq, seq[1:]):
            d = sum((x - y) ** 2 for x, y in zip(a[:3], b[:3])) ** 0.5
            steps += 1
            jumps += d > 200.0
    if steps:
        print("  translation steps between consecutive lists: %d, over 200 units: %d (%.0f%%)" %
              (steps, jumps, 100.0 * jumps / steps))
    per_list = defaultdict(int)
    for s in starts:
        per_list[s.list] += 1
    sizes = sorted(per_list.values())
    print("  starts per list: min %d median %d max %d" % (sizes[0], sizes[len(sizes) // 2], sizes[-1]))


def pose(s):
    return s.q(63)[:3] + s.q(60)[:3] + s.q(61)[:3] + s.q(62)[:3]


def pose_distance(p, q):
    """Translation distance plus the rotation rows' difference, weighted so 1 radian ~ 50 units."""
    t = sum((x - y) ** 2 for x, y in zip(p[:3], q[:3])) ** .5
    r = sum((x - y) ** 2 for x, y in zip(p[3:], q[3:])) ** .5
    return t + 50.0 * r


def matching_report(starts):
    """Pair each draw with itself in the next list: same program and mesh shape, then the
    nearest pose, each draw of the next list taken once."""
    per = defaultdict(list)
    for s in starts:
        if s.shape is not None:
            per[s.list].append(s)
    lists = sorted(per)
    res = defaultdict(lambda: [0, 0, 0, 0, 0])  # draws, no match, exact, confident, uncertain
    for a, b in zip(lists, lists[1:]):
        if b != a + 1:
            continue
        nxt, cur = defaultdict(list), defaultdict(list)
        for s in per[b]:
            nxt[(s.tag, s.shape)].append(pose(s))
        for s in per[a]:
            cur[(s.tag, s.shape)].append(pose(s))
        for key, prev in cur.items():
            r = res[key[0]]
            r[0] += len(prev)
            cand = nxt.get(key)
            if not cand:
                r[1] += len(prev)
                continue
            pairs = sorted((pose_distance(p, q), i, j) for i, p in enumerate(prev) for j, q in enumerate(cand))
            used_i, used_j, best = set(), set(), {}
            for d, i, j in pairs:
                if i not in used_i and j not in used_j:
                    used_i.add(i)
                    used_j.add(j)
                    best[i] = (d, j)
            for i, p in enumerate(prev):
                if i not in best:
                    r[1] += 1
                    continue
                d, j = best[i]
                if d < 0.01:
                    r[2] += 1
                    continue
                others = [(pose_distance(p, q), k) for k, q in enumerate(cand) if k != j]
                if not others:
                    r[3] += 1
                    continue
                second, k = min(others)
                # a rival at the same pose gives the same motion whichever is picked
                same = pose_distance(cand[j], cand[k]) < 2.0
                r[3 if d <= 0.5 * second or d < 2.0 or same else 4] += 1
    if not res:
        print("\nno light records to match (record with RG_VU1_CAPTURE_LISTS)")
        return
    print("\nmatching a draw to itself in the next list (same program and mesh shape, then nearest pose):")
    print("  %-9s %9s %8s %8s %10s %10s" % ("program", "draws", "nomatch", "exact", "confident", "uncertain"))
    total = [0] * 5
    rows = sorted(res.items(), key=lambda kv: -kv[1][0])
    for tag, r in rows[:8]:
        print("  %08x %9d %7.0f%% %7.0f%% %9.0f%% %9.1f%%" % (tag, r[0], *[100.0 * x / r[0] for x in r[1:]]))
    for r in res.values():
        total = [x + y for x, y in zip(total, r)]
    print("  %-9s %9d %7.0f%% %7.0f%% %9.0f%% %9.1f%%" % ("all", total[0], *[100.0 * x / total[0] for x in total[1:]]))
    print("  exact: the matrix is unchanged; confident: the best pose is clearly nearer than the next, "
          "moved under 2 units, or the rival is at the same pose; uncertain: a rival at a different pose was about as close")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("capture")
    ap.add_argument("--quads", default="4-11,64-75")
    ap.add_argument("--sample", type=int, default=0, help="which start of each program to print")
    ap.add_argument("--programs", type=int, default=6, help="programs to list, most frequent first")
    a = ap.parse_args()
    raw = Path(a.capture).read_bytes()
    starts = read_starts(raw)
    full = [s for s in starts if s.full]
    by_tag = defaultdict(list)
    for s in full:
        by_tag[s.tag].append(s)
    print("%d starts, %d programs" % (len(starts), len(by_tag)))
    n, bad, worst = check_matrices([s for s in starts if (s.tag >> 16 & 0x7FF) in (0x1900 // 8, 0xB00 // 8)])
    print("matrix check, model x view x projection = clip: %d starts, %d off by more than 1e-3, worst %.1e" %
          (n, bad, worst))
    identity_report(starts)
    matching_report(starts)
    for tag, recs in sorted(by_tag.items(), key=lambda kv: -len(kv[1]))[:a.programs]:
        varying = [q for q in range(DATA // 16) if len({r.mem[q * 16:q * 16 + 16] for r in recs}) > 1]
        print("\ntag %08x  pc 0x%x  starts %d  TOP %s" % (tag, (tag >> 16 & 0x7FF) * 8, len(recs),
                                                         ",".join(map(str, sorted({r.top for r in recs})[:4]))))
        print("  varying quads:", " ".join(runs(varying)) or "none")
        s = recs[min(a.sample, len(recs) - 1)]
        for q in ranges(a.quads):
            print("  %s q%-3d % 11.4f % 11.4f % 11.4f % 11.4f" % ("*" if q in varying else " ", q, *quad(s.mem, q)))
        ints = struct.unpack_from("<8I", s.mem, 103 * 16)
        print("  q103-104 as words:", " ".join("%x" % x for x in ints))
        print("  quads at TOP %d:" % s.top)
        for i in range(6):
            print("    ", " ".join("%12.4f" % x for x in quad(s.mem, s.top + i)),
                  " |", " ".join("%08x" % x for x in struct.unpack_from("<4I", s.mem, (s.top + i) * 16)))


if __name__ == "__main__":
    main()
