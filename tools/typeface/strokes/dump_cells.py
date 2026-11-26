"""The console's cells printed as alpha levels (0-F, "." clear), a row a
line under the columns' numbers: where a skeleton's nodes start
(skeletons.py). Read from the disc, printed only.

    python -I tools/typeface/strokes/dump_cells.py GMPSy94 [--font normal]
"""
import argparse
import sys
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
import pages  # noqa: E402


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("chars")
    ap.add_argument("--font", default="normal", choices=sorted(pages.FONTS))
    args = ap.parse_args()
    fonts = pages.load_fonts(HERE.parents[2] / "disc")
    con = {g.char: g for g in fonts[args.font] if g.char}
    for c in args.chars:
        a = np.asarray(con[c].cell, dtype=float) / 255 * 15
        print(ascii(c), "ink", con[c].ink)
        print("   " + "".join(f"{x % 10}" for x in range(a.shape[1])))
        for y, row in enumerate(a):
            print(f"{y:2d} " + "".join("0123456789ABCDEF"[int(round(v))] if v > 0.5 else "." for v in row))


if __name__ == "__main__":
    main()
