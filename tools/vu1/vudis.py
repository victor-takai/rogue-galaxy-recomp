#!/usr/bin/env python3
"""Disassemble a VU1 code image, with operands, as far as a run reached it.

  vudis.py CODE [--coverage FILE] [--all] [--from PC] [--to PC]
  vudis.py RECORDING.vu1r --image HASH [...]

CODE is 16 KiB of micro memory (a code image from build/vu1-manifest), or the
image HASH (as rg_vu1_oracle prints it) out of an RG_VU1_REPLAY recording. With --coverage (rg_vu1_oracle
--coverage) only the pairs that ran are listed, each with how often; blocks
that did not run are elided. PCs are in bytes, as the oracle prints them;
pairs are 8 bytes, lower word first.
"""
import argparse
import struct

BC = "xyzw"


def sx(v, bits):
    return v - (1 << bits) if v & (1 << (bits - 1)) else v


def dest(w):
    return "".join(c for i, c in enumerate(BC) if w >> (24 - i) & 1)


def f32(w):
    return struct.unpack("<f", struct.pack("<I", w))[0]


UP_BC = ["ADD", "SUB", "MADD", "MSUB", "MAX", "MINI", "MUL"]
UP_MAIN = {0x1C: ("MUL", "q"), 0x1D: ("MAX", "i"), 0x1E: ("MUL", "i"), 0x1F: ("MINI", "i"),
           0x20: ("ADD", "q"), 0x21: ("MADD", "q"), 0x22: ("ADD", "i"), 0x23: ("MADD", "i"),
           0x24: ("SUB", "q"), 0x25: ("MSUB", "q"), 0x26: ("SUB", "i"), 0x27: ("MSUB", "i"),
           0x28: ("ADD", ""), 0x29: ("MADD", ""), 0x2A: ("MUL", ""), 0x2B: ("MAX", ""),
           0x2C: ("SUB", ""), 0x2D: ("MSUB", ""), 0x2E: ("OPMSUB", ""), 0x2F: ("MINI", "")}
UP_ACC = {0x1C: ("MULA", "q"), 0x1E: ("MULA", "i"), 0x20: ("ADDA", "q"), 0x21: ("MADDA", "q"),
          0x22: ("ADDA", "i"), 0x23: ("MADDA", "i"), 0x24: ("SUBA", "q"), 0x25: ("MSUBA", "q"),
          0x26: ("SUBA", "i"), 0x27: ("MSUBA", "i"), 0x28: ("ADDA", ""), 0x29: ("MADDA", ""),
          0x2A: ("MULA", ""), 0x2C: ("SUBA", ""), 0x2D: ("MSUBA", ""), 0x2E: ("OPMULA", "")}


def upper(u):
    op = u & 0x3F
    d = dest(u)
    ft, fs, fd = u >> 16 & 31, u >> 11 & 31, u >> 6 & 31
    if op < 0x1C:
        return "%s%s.%s vf%d, vf%d, vf%d%s" % (UP_BC[op >> 2], BC[op & 3], d, fd, fs, ft, BC[op & 3])
    if op < 0x30:
        name, src = UP_MAIN[op]
        if name == "OPMSUB":
            return "OPMSUB.xyz vf%d, vf%d, vf%d" % (fd, fs, ft)
        return "%s%s.%s vf%d, vf%d, %s" % (name, src, d, fd, fs, src or "vf%d" % ft)
    if op < 0x3C:
        return "UP?%02x" % op
    sp = (u & 3) | (u >> 4 & 0x7C)
    if sp < 0x10:
        name = ["ADDA", "SUBA", "MADDA", "MSUBA"][sp >> 2]
        return "%s%s.%s ACC, vf%d, vf%d%s" % (name, BC[sp & 3], d, fs, ft, BC[sp & 3])
    if 0x10 <= sp < 0x18:
        name = ("ITOF" if sp < 0x14 else "FTOI") + ["0", "4", "12", "15"][sp & 3]
        return "%s.%s vf%d, vf%d" % (name, d, ft, fs)
    if 0x18 <= sp < 0x1C:
        return "MULA%s.%s ACC, vf%d, vf%d%s" % (BC[sp & 3], d, fs, ft, BC[sp & 3])
    if sp == 0x1D:
        return "ABS.%s vf%d, vf%d" % (d, ft, fs)
    if sp == 0x1F:
        return "CLIPw.xyz vf%d, vf%dw" % (fs, ft)
    if sp == 0x2F:
        return "NOP"
    if sp in UP_ACC:
        name, src = UP_ACC[sp]
        if name == "OPMULA":
            return "OPMULA.xyz ACC, vf%d, vf%d" % (fs, ft)
        return "%s%s.%s ACC, vf%d, %s" % (name, src, d, fs, src or "vf%d" % ft)
    return "US?%02x" % sp


BRANCHES = {0x20: "B", 0x21: "BAL", 0x28: "IBEQ", 0x29: "IBNE", 0x2C: "IBLTZ", 0x2D: "IBGTZ",
            0x2E: "IBLEZ", 0x2F: "IBGEZ"}
FLAGS24 = {0x10: "FCEQ", 0x11: "FCSET", 0x12: "FCAND", 0x13: "FCOR"}
FLAGS12 = {0x14: "FSEQ", 0x15: "FSSET", 0x16: "FSAND", 0x17: "FSOR"}
EFU = {0x70: "ESADD", 0x71: "ERSADD", 0x72: "ELENG", 0x73: "ERLENG", 0x74: "EATANxy", 0x75: "EATANxz",
       0x76: "ESUM", 0x78: "ESQRT", 0x79: "ERSQRT", 0x7A: "ERCPR", 0x7C: "ESIN", 0x7D: "EATAN", 0x7E: "EEXP"}


def lower(l, pc):
    """Text, and the branch target in bytes (or None)."""
    op = l >> 25
    d = dest(l)
    ft, fs, fd = l >> 16 & 31, l >> 11 & 31, l >> 6 & 31
    imm11 = sx(l & 0x7FF, 11)
    target = pc + 8 + imm11 * 8
    if op == 0x00:
        return "LQ.%s vf%d, %d(vi%d)" % (d, ft, imm11, fs), None
    if op == 0x01:
        return "SQ.%s vf%d, %d(vi%d)" % (d, fs, imm11, ft), None
    if op == 0x04:
        return "ILW.%s vi%d, %d(vi%d)" % (d, ft, imm11, fs), None
    if op == 0x05:
        return "ISW.%s vi%d, %d(vi%d)" % (d, ft, imm11, fs), None
    if op in (0x08, 0x09):
        imm = (l >> 21 & 15) << 11 | (l & 0x7FF)
        return "%s vi%d, vi%d, %d" % ("IADDIU" if op == 0x08 else "ISUBIU", ft, fs, imm), None
    if op in FLAGS24:
        return "%s vi1, 0x%06x" % (FLAGS24[op], l & 0xFFFFFF), None
    if op in FLAGS12:
        return "%s vi%d, 0x%03x" % (FLAGS12[op], ft, (l >> 10 & 0x800) | (l & 0x7FF)), None
    if op in (0x18, 0x1A, 0x1B):
        return "%s vi%d, vi%d" % ({0x18: "FMEQ", 0x1A: "FMAND", 0x1B: "FMOR"}[op], ft, fs), None
    if op == 0x1C:
        return "FCGET vi%d" % ft, None
    if op == 0x20:
        return "B 0x%x" % target, target
    if op == 0x21:
        return "BAL vi%d, 0x%x" % (ft, target), target
    if op == 0x24:
        return "JR vi%d" % fs, None
    if op == 0x25:
        return "JALR vi%d, vi%d" % (ft, fs), None
    if op in (0x28, 0x29):
        return "%s vi%d, vi%d, 0x%x" % (BRANCHES[op], ft, fs, target), target
    if op in (0x2C, 0x2D, 0x2E, 0x2F):
        return "%s vi%d, 0x%x" % (BRANCHES[op], fs, target), target
    if op != 0x40:
        return "LO?%02x" % op, None
    low = l & 0x3F
    if low < 0x3C:
        if low == 0x30:
            return "IADD vi%d, vi%d, vi%d" % (fd, fs, ft), None
        if low == 0x31:
            return "ISUB vi%d, vi%d, vi%d" % (fd, fs, ft), None
        if low == 0x32:
            return "IADDI vi%d, vi%d, %d" % (ft, fs, sx(fd, 5)), None
        if low == 0x34:
            return "IAND vi%d, vi%d, vi%d" % (fd, fs, ft), None
        if low == 0x35:
            return "IOR vi%d, vi%d, vi%d" % (fd, fs, ft), None
        return "LD?%02x" % low, None
    sp = (l & 3) | (l >> 4 & 0x7C)
    fsf, ftf = BC[l >> 21 & 3], BC[l >> 23 & 3]
    table = {
        0x30: lambda: "MOVE.%s vf%d, vf%d" % (d, ft, fs),
        0x31: lambda: "MR32.%s vf%d, vf%d" % (d, ft, fs),
        0x34: lambda: "LQI.%s vf%d, (vi%d++)" % (d, ft, fs),
        0x35: lambda: "SQI.%s vf%d, (vi%d++)" % (d, fs, ft),
        0x36: lambda: "LQD.%s vf%d, (--vi%d)" % (d, ft, fs),
        0x37: lambda: "SQD.%s vf%d, (--vi%d)" % (d, fs, ft),
        0x38: lambda: "DIV Q, vf%d%s, vf%d%s" % (fs, fsf, ft, ftf),
        0x39: lambda: "SQRT Q, vf%d%s" % (ft, ftf),
        0x3A: lambda: "RSQRT Q, vf%d%s, vf%d%s" % (fs, fsf, ft, ftf),
        0x3B: lambda: "WAITQ",
        0x3C: lambda: "MTIR vi%d, vf%d%s" % (ft, fs, fsf),
        0x3D: lambda: "MFIR.%s vf%d, vi%d" % (d, ft, fs),
        0x3E: lambda: "ILWR.%s vi%d, (vi%d)" % (d, ft, fs),
        0x3F: lambda: "ISWR.%s vi%d, (vi%d)" % (d, ft, fs),
        0x40: lambda: "RNEXT.%s vf%d, R" % (d, ft),
        0x41: lambda: "RGET.%s vf%d, R" % (d, ft),
        0x42: lambda: "RINIT R, vf%d%s" % (fs, fsf),
        0x43: lambda: "RXOR R, vf%d%s" % (fs, fsf),
        0x64: lambda: "MFP.%s vf%d, P" % (d, ft),
        0x68: lambda: "XTOP vi%d" % ft,
        0x69: lambda: "XITOP vi%d" % ft,
        0x6C: lambda: "XGKICK vi%d" % fs,
        0x7B: lambda: "WAITP",
    }
    if sp in table:
        text = table[sp]()
        return ("NOP" if text == "MOVE. vf0, vf0" else text), None
    if sp in EFU:
        if sp in (0x7C, 0x7D, 0x7E, 0x78, 0x79, 0x7A):
            return "%s P, vf%d%s" % (EFU[sp], fs, fsf), None
        return "%s P, vf%d" % (EFU[sp], fs), None
    return "LS?%02x" % sp, None


def pair_text(code, pc):
    lo, up = struct.unpack_from("<II", code, pc)
    bits = "".join(c for c, b in zip("IEMDT", (31, 30, 29, 28, 27)) if up >> b & 1)
    ut = upper(up)
    if up >> 31 & 1:
        lt, target = "LOI %r" % f32(lo), None
    else:
        lt, target = lower(lo, pc)
    return bits, ut, lt, target


def image_from_recording(path, wanted):
    """The CODE record `wanted` (hex) of an RG_VU1_REPLAY file (src/debug/vu1_replay.h)."""
    want = int(wanted, 16)
    with open(path, "rb") as f:
        state_size = struct.unpack("<5I", f.read(20))[2]
        while True:
            kind = f.read(4)
            if len(kind) < 4:
                break
            kind = struct.unpack("<I", kind)[0]
            if kind == 0x45444F43:    # CODE
                image_hash = struct.unpack("<Q", f.read(8))[0]
                code = f.read(16384)
                if image_hash == want:
                    return code
            elif kind == 0x204E5552:  # RUN
                f.seek(40 + state_size + 16384, 1)
            elif kind == 0x4B43494B:  # KICK
                f.seek(struct.unpack("<I", f.read(4))[0], 1)
            elif kind == 0x20444E45:  # END
                flags = struct.unpack("<I", f.read(4))[0]
                f.seek(state_size + (16384 if flags & 1 else 0), 1)
            else:
                break
    raise SystemExit("image %s is not in %s" % (wanted, path))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("code")
    ap.add_argument("--image", help="with a recording: the code image's hash (hex)")
    ap.add_argument("--coverage")
    ap.add_argument("--all", action="store_true", help="every pair, not only those that ran")
    ap.add_argument("--from", dest="start", default="0")
    ap.add_argument("--to", dest="end", default="4000")
    a = ap.parse_args()
    code = image_from_recording(a.code, a.image) if a.image else open(a.code, "rb").read()
    hits = {}
    if a.coverage:
        for line in open(a.coverage):
            pc, count = line.split()
            hits[int(pc, 16)] = int(count)
    start, end = int(a.start, 16), int(a.end, 16)
    gap = False
    for pc in range(start, min(end, len(code)), 8):
        if hits and not a.all and pc not in hits:
            if not gap:
                print("        ...")
            gap = True
            continue
        gap = False
        bits, ut, lt, target = pair_text(code, pc)
        count = ("%9d" % hits[pc]) if pc in hits else " " * 9
        print("%04x %s %-5s %-38s | %s" % (pc, count, bits, ut, lt))


if __name__ == "__main__":
    main()
