#!/usr/bin/env python3
"""Write one microcode piece from the executable as a 16 KiB micro-memory image
(zeros elsewhere), at its load address, for vudis.py and symvu.py.

  piece.py HASHPREFIX OUT.bin [--elf disc/SCUS_974.90]
"""
import argparse, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import census

ap = argparse.ArgumentParser()
ap.add_argument("prefix"); ap.add_argument("out")
ap.add_argument("--elf", default=str(census.ROOT / "disc" / "SCUS_974.90"))
a = ap.parse_args()
for p in census.find_pieces(open(a.elf, "rb").read()):
    if "%016x" % p["hash"] == a.prefix or ("%016x" % p["hash"]).startswith(a.prefix):
        img = bytearray(16384)
        img[p["load"] * 8:p["load"] * 8 + len(p["code"])] = p["code"]
        open(a.out, "wb").write(img)
        print("load 0x%x bytes, %d pairs, %016x" % (p["load"] * 8, p["pairs"], p["hash"]))
        break
else:
    sys.exit("no such piece")
