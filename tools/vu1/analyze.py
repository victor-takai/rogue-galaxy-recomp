#!/usr/bin/env python3
"""Classify the game's VU1 microprograms: what each entry point reads, writes and computes.

Reads the recording in build/vu1-manifest (16 KiB code images and entries.txt) and
prints one line per entry: whether it transforms vertices (matrix multiply), divides
for perspective, clips, normalizes (lighting), unpacks compressed data, and which fixed
VU memory quadwords it loads and stores. With --dump HASH:PC it prints a disassembly.
The output is a map for finding the matrix and vertex-format slots; it decodes only
what the classifier needs.
"""
import argparse
import struct
from collections import Counter, defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PAIRS = 2048

BC = "xyzw"
UP_MAIN = {0x1C: "MULq", 0x1D: "MAXi", 0x1E: "MULi", 0x1F: "MINIi", 0x20: "ADDq", 0x21: "MADDq",
           0x22: "ADDi", 0x23: "MADDi", 0x24: "SUBq", 0x25: "MSUBq", 0x26: "SUBi", 0x27: "MSUBi",
           0x28: "ADD", 0x29: "MADD", 0x2A: "MUL", 0x2B: "MAX", 0x2C: "SUB", 0x2D: "MSUB",
           0x2E: "OPMSUB", 0x2F: "MINI"}
UP_BC = ["ADD", "SUB", "MADD", "MSUB", "MAX", "MINI", "MUL"]
UP_SPECIAL = {0x10: "ITOF0", 0x11: "ITOF4", 0x12: "ITOF12", 0x13: "ITOF15", 0x14: "FTOI0",
              0x15: "FTOI4", 0x16: "FTOI12", 0x17: "FTOI15", 0x1C: "MULAq", 0x1D: "ABS",
              0x1E: "MULAi", 0x1F: "CLIP", 0x20: "ADDAq", 0x21: "MADDAq", 0x22: "ADDAi",
              0x23: "MADDAi", 0x24: "SUBAq", 0x25: "MSUBAq", 0x26: "SUBAi", 0x27: "MSUBAi",
              0x28: "ADDA", 0x29: "MADDA", 0x2A: "MULA", 0x2C: "SUBA", 0x2D: "MSUBA",
              0x2E: "OPMULA", 0x2F: "NOP"}
LO_MAIN = {0x00: "LQ", 0x01: "SQ", 0x04: "ILW", 0x05: "ISW", 0x08: "IADDIU", 0x09: "ISUBIU",
           0x10: "FCEQ", 0x11: "FCSET", 0x12: "FCAND", 0x13: "FCOR", 0x14: "FSEQ",
           0x15: "FSSET", 0x16: "FSAND", 0x17: "FSOR", 0x18: "FMEQ", 0x1A: "FMAND",
           0x1B: "FMOR", 0x1C: "FCGET", 0x20: "B", 0x21: "BAL", 0x24: "JR", 0x25: "JALR",
           0x28: "IBEQ", 0x29: "IBNE", 0x2C: "IBLTZ", 0x2D: "IBGTZ", 0x2E: "IBLEZ",
           0x2F: "IBGEZ"}
LO_DIRECT = {0x30: "IADD", 0x31: "ISUB", 0x32: "IADDI", 0x34: "IAND", 0x35: "IOR"}
LO_SPECIAL = {0x30: "MOVE", 0x31: "MR32", 0x34: "LQI", 0x35: "SQI", 0x36: "LQD", 0x37: "SQD",
              0x38: "DIV", 0x39: "SQRT", 0x3A: "RSQRT", 0x3B: "WAITQ", 0x3C: "MTIR",
              0x3D: "MFIR", 0x3E: "ILWR", 0x3F: "ISWR", 0x40: "RNEXT", 0x41: "RGET",
              0x42: "RINIT", 0x43: "RXOR", 0x64: "MFP", 0x68: "XTOP", 0x69: "XITOP",
              0x6C: "XGKICK"}
BRANCH = {0x20, 0x21, 0x28, 0x29, 0x2C, 0x2D, 0x2E, 0x2F}


def sx(v, bits):
    return v - (1 << bits) if v & (1 << (bits - 1)) else v


def dest(w):
    return "".join(c for i, c in enumerate(BC) if w >> (24 - i) & 1)


def decode_upper(u):
    op = u & 0x3F
    fd, fs, ft = u >> 6 & 31, u >> 11 & 31, u >> 16 & 31
    d = dest(u)
    if op < 0x1C:
        return UP_BC[op >> 2] + BC[op & 3], d, fd, fs, ft
    if op < 0x3C:
        return UP_MAIN[op], d, fd, fs, ft
    sp = (u & 3) | (u >> 4 & 0x7C)
    if sp < 0x10:
        name = ["ADDA", "SUBA", "MADDA", "MSUBA"][sp >> 2] + BC[sp & 3]
    elif 0x18 <= sp < 0x1C:
        name = "MULA" + BC[sp & 3]
    else:
        name = UP_SPECIAL.get(sp, "UP?%02x" % sp)
    return name, d, 0, fs, ft


def decode_lower(l):
    op = l >> 25
    d = dest(l)
    ft, fs = l >> 16 & 31, l >> 11 & 31
    imm11 = sx(l & 0x7FF, 11)
    if op < 0x40:
        name = LO_MAIN.get(op, "LO?%02x" % op)
        imm = imm11
        if name in ("IADDIU", "ISUBIU"):
            imm = (l >> 21 & 15) << 11 | (l & 0x7FF)
        return name, d, ft, fs, imm
    low = l & 0x3F
    if low < 0x3C:
        return LO_DIRECT.get(low, "LD?%02x" % low), d, l >> 6 & 31, fs, ft
    sp = (l & 3) | (l >> 4 & 0x7C)
    return LO_SPECIAL.get(sp, "LS?%02x" % sp), d, ft, fs, 0


def pairs(img):
    for p in range(PAIRS):
        lo, up = struct.unpack_from("<II", img, p * 8)
        yield p, lo, up


def reach(words, entry):
    """Pairs reachable from entry; a branch's delay slot is included, calls fall through."""
    seen, stack = set(), [entry]
    while stack:
        p = stack.pop()
        while 0 <= p < PAIRS and p not in seen:
            seen.add(p)
            lo, up = words[p]
            name = decode_lower(lo)[0]
            if name in ("B", "BAL") or name in LO_MAIN.values() and name.startswith("IB"):
                t = p + 1 + sx(lo & 0x7FF, 11)
                stack.append(t)
                if name == "B":
                    seen.add(p + 1)
                    break
            if name in ("JR",):
                seen.add(p + 1)
                break
            if up >> 30 & 1:       # E bit: ends after the next pair
                seen.add(p + 1)
                break
            p += 1
    return seen


def analyze(words, entry):
    r = reach(words, entry)
    ops = Counter()
    lq, sq, runs, run = defaultdict(int), defaultdict(int), [], 0
    for p in sorted(r):
        lo, up = words[p]
        un, _, _, _, _ = decode_upper(up)
        ln, ld, lft, lfs, imm = decode_lower(lo)
        ops[un] += 1
        ops[ln] += 1
        if ln == "LQ" and lfs == 0:
            lq[imm] += 1
        if ln == "SQ" and lft == 0 or ln == "SQ" and lfs == 0:
            sq[imm] += 1
        # a matrix multiply: MULAx, MADDAy, MADDAz, MADDw in sequence
        run = run + 1 if un in ("MULAx", "MADDAy", "MADDAz", "MADDAw", "MADDw", "MADDx", "MADDy", "MADDz") else 0
        if run == 4:
            runs.append(p)
    return r, ops, lq, sq, runs


def disasm(words, hashed, entry):
    r = reach(words, entry)
    for p in sorted(r):
        lo, up = words[p]
        un, ud, ufd, ufs, uft = decode_upper(up)
        ln, ld, lt, ls, imm = decode_lower(lo)
        flag = "E" if up >> 30 & 1 else " "
        print("%04x %s %-8s.%-4s vf%-2d vf%-2d vf%-2d | %-7s.%-4s %2d %2d %6d" %
              (p * 8, flag, un, ud, ufd, ufs, uft, ln, ld, lt, ls, imm))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", default=str(ROOT / "build" / "vu1-manifest"))
    ap.add_argument("--dump", help="HASH:PC (hex) to disassemble")
    a = ap.parse_args()
    d = Path(a.dir)
    imgs = {}
    for f in d.glob("vu1-*.code"):
        raw = f.read_bytes()
        imgs[f.stem[4:]] = [(lo, up) for _, lo, up in pairs(raw)]
    if a.dump:
        h, pc = a.dump.split(":")
        disasm(imgs[h], h, int(pc, 16) // 8)
        return
    entries = defaultdict(list)
    for line in (d / "entries.txt").read_text().split("\n"):
        if line.strip():
            h, pc = line.split()
            entries[h].append(int(pc, 16))
    print("%-16s %5s %4s %4s %4s %4s %4s %4s %4s %4s  %s" %
          ("image", "pc", "pair", "xfm", "div", "sqrt", "clip", "ftoi", "itof", "xgk", "fixed lq / sq quadwords"))
    for h, pcs in sorted(entries.items()):
        for pc in sorted(pcs):
            if h not in imgs:
                continue
            r, ops, lq, sq, runs = analyze(imgs[h], pc // 8)
            ftoi = sum(v for k, v in ops.items() if k.startswith("FTOI"))
            itof = sum(v for k, v in ops.items() if k.startswith("ITOF"))
            sqrt = ops["SQRT"] + ops["RSQRT"]
            print("%-16s %5x %4d %4d %4d %4d %4d %4d %4d %4d  lq %s | sq %s" %
                  (h, pc, len(r), len(runs), ops["DIV"], sqrt, ops["CLIP"], ftoi, itof, ops["XGKICK"],
                   " ".join("%d" % k for k in sorted(lq)[:14]), " ".join("%d" % k for k in sorted(sq)[:8])))


if __name__ == "__main__":
    main()
