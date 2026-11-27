"""A close look at fitted glyphs: per glyph, the console's cell enlarged
(nearest, a grid per pixel) with the outline as assembled drawn over it in
red, and the glyph alone filled at the same scale beside it. Into
build/typeface/strokes (the console's glyphs are in it: never into the
repository); run stroke_fit.py --assemble first.

    python -I tools/typeface/strokes/overlay_strokes.py --chars ",;j" [--font name] [--scale 24] [--name overlay.png]
"""
import argparse
import json
import sys
from pathlib import Path

import numpy as np
import torch
from fontTools.ttLib import TTFont
from PIL import Image, ImageDraw

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import raster as fg  # noqa: E402
import stroke_fit as sf  # noqa: E402
from preview_sheet import label_font  # noqa: E402


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--font", default="normal", choices=sorted(sf.PROFILES), help="as stroke_fit.py's")
    ap.add_argument("--dir", type=Path, default=None, help="stroke_fit.py's output (default: the font's)")
    ap.add_argument("--chars", required=True)
    ap.add_argument("--scale", type=int, default=24)
    ap.add_argument("--name", default="overlay.png")
    args = ap.parse_args()
    sf.use(args.font)
    args.dir = args.dir or sf.OUT
    setup = sf.Setup(2.0)
    table = sf.load_table()
    tt = TTFont(str(args.dir / "Fitted-Regular.ttf"))
    report = json.loads((args.dir / "fit_report.json").read_text(encoding="utf-8"))
    s = args.scale
    cw, ch = setup.params["cw"], setup.params["ch"]
    pw, ph = cw * s, ch * s
    pad = 10
    lf = label_font()
    unknown = [c for c in args.chars if c not in sf.SKELETONS]
    if unknown:
        raise SystemExit(f"no skeleton for {''.join(unknown)!a}")
    stale = [c for c in args.chars if c not in report or c not in table or sf.table_strokes(table, c) is None]
    if stale:
        raise SystemExit(f"{''.join(stale)!a}: to fit first (missing, or the skeleton changed), then assemble")
    tiles = []
    for c in args.chars:
        rep = report[c]
        strokes, Ps, _ = sf.table_strokes(table, c)
        # The place assemble_step gave the glyph.
        o_and, _ = setup.andika_today(c)
        allx = torch.cat([sf.flatten_stroke(st, P)[0][:, 0] for st, P in zip(strokes, Ps)])
        cx = float((allx.min() + allx.max()) / 2)
        cA = (o_and.coords[:, 0].min() + o_and.coords[:, 0].max()) / 2
        tx = cx - cA * setup.oras.sx
        o = fg.Outline(tt, rep["glyph"])
        polys = []
        for idx in o.contours():
            sub = fg.Outline.__new__(fg.Outline)
            sub.coords, sub.on = o.coords[idx], o.on[idx]
            sub.ends = [len(idx) - 1]
            plan = fg.plan_tensors(*fg.segment_plan(sub))
            p0, _ = fg.flatten(setup.oras.to_cell(torch.tensor(sub.coords), torch.tensor(tx)), plan)
            polys.append([(float(x) * s, float(y) * s) for x, y in p0])
        cell = np.asarray(setup.console[c].cell, dtype=np.float64) / 255.0
        bg = np.array([20, 28, 52], float)
        rgb = bg + (255 - bg) * cell[..., None]
        base = Image.fromarray(rgb.astype(np.uint8), "RGB").resize((pw, ph), Image.NEAREST)
        d = ImageDraw.Draw(base)
        for x in range(cw + 1):
            d.line([(x * s, 0), (x * s, ph)], fill=(45, 55, 80))
        for y in range(ch + 1):
            d.line([(0, y * s), (pw, y * s)], fill=(45, 55, 80))
        for poly in polys:
            d.line(poly + [poly[0]], fill=(255, 60, 60), width=2)
        ours = Image.new("RGB", (pw, ph), tuple(int(v) for v in bg))
        do = ImageDraw.Draw(ours)
        for poly in polys:  # even-odd is enough to look at
            do.polygon(poly, fill=(255, 255, 255))
        tile = Image.new("RGB", (2 * pw + 3 * pad, ph + 2 * pad + 30), (12, 16, 30))
        ImageDraw.Draw(tile).text((pad, 2), f"{c}  {rep['before']:.2f} -> {rep['after']:.2f}",
                                  fill=(255, 220, 120), font=lf)
        tile.paste(base, (pad, 30 + pad))
        tile.paste(ours, (2 * pad + pw, 30 + pad))
        tiles.append(tile)
    out = Image.new("RGB", (sum(t.width for t in tiles), tiles[0].height), (12, 16, 30))
    x = 0
    for t in tiles:
        out.paste(t, (x, 0))
        x += t.width
    out.save(args.dir / args.name)
    print(args.dir / args.name)


if __name__ == "__main__":
    main()
