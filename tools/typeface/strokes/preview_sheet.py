"""A sheet to judge the glyphs by: per character, the console's (nearest)
beside Andika as the typeface table drew it and the fitted strokes, drawn as
make_table.py's fit draws them (FreeType at 8x the cell, the table's size,
horizontal scale and baseline, the stroke grown by the message font's two
steps, as in the game), box-filtered to the sheet's scale. Both are placed
as the table placed Andika (andika_fit.json's shift, on the baseline, no
lift); the game places Rogue Letters by its own rows (src/typeface_table.inc,
fitted again by make_table.py), so a mark can sit up to half a pixel off the
game's (the quotes: 0.75 across here, 0.5 across and 0.25 down in the game).
Into build/typeface/strokes (the console's glyphs are in it: never into the
repository).

    python -I tools/typeface/strokes/preview_sheet.py [--scale 4] [--grow 2] [--chars ...]
        [--face LABEL=PATH ...] [--name preview_4x.png]
"""
import argparse
import json
import math
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import raster  # noqa: E402  (puts tools/typeface on the path)
import fit  # noqa: E402
import pages  # noqa: E402

BG = (20, 28, 52)


def label_font(size=20):
    try:
        return ImageFont.truetype("segoeui.ttf", size)
    except OSError:
        return ImageFont.load_default(size)


def cell_at(g, sigma, k, bl, cw, ch, center, scale):
    sx = fit.R / (sigma * k)
    sy = fit.R / sigma
    cx = (g.ink[0] + g.ink[2]) / 2
    x0 = cx - center * sx
    y0 = g.ay - bl * sy
    box = (x0, y0, x0 + cw * sx, y0 + ch * sy)
    W, H = g.img.size
    src = g.img
    if box[0] < 0 or box[1] < 0 or box[2] > W or box[3] > H:
        px = int(math.ceil(max(0, -box[0], box[2] - W))) + 2
        py = int(math.ceil(max(0, -box[1], box[3] - H))) + 2
        src = Image.new("L", (W + 2 * px, H + 2 * py), 0)
        src.paste(g.img, (px, py))
        box = (box[0] + px, box[1] + py, box[2] + px, box[3] + py)
    return src.resize((cw * scale, ch * scale), Image.BOX, box=box)


def tint(a: Image.Image) -> Image.Image:
    out = Image.new("RGB", a.size)
    lut = [tuple(int(b + (255 - b) * v / 255) for b in BG) for v in range(256)]
    px, ap = out.load(), a.load()
    for y in range(a.height):
        for x in range(a.width):
            px[x, y] = lut[ap[x, y]]
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--dir", type=Path, default=raster.OUT, help="stroke_fit.py's output")
    ap.add_argument("--scale", type=int, default=4)
    ap.add_argument("--grow", type=int, default=2)
    ap.add_argument("--chars", default=None)
    ap.add_argument("--cols", type=int, default=6)
    ap.add_argument("--name", default=None)
    ap.add_argument("--face", action="append", default=None,
                    help="LABEL=PATH (a .ttf whose cmap maps each character to the glyph to show), "
                         "repeatable; default: Andika as the table drew it, and the fitted strokes")
    args = ap.parse_args()
    params = json.loads((HERE / "andika_fit.json").read_text(encoding="utf-8"))
    rows = {r["char"]: r for r in params["rows"]}
    report = json.loads((args.dir / "fit_report.json").read_text(encoding="utf-8"))
    chars = args.chars or "".join(report)
    fonts = pages.load_fonts(raster.ROOT / "disc")
    console = {g.char: g for g in fonts["normal"] if g.char}
    cw, ch = params["cw"], params["ch"]
    cap = console["H"].ink[3] - console["H"].ink[1] + 1
    size = int(round(cap * fit.R * 1.4))
    sigma = params["em_y"] * fit.R / size
    k = params["em_x"] / params["em_y"]
    bl = params["baseline"]
    specs = args.face or [f"Andika={args.dir / 'Andika-remapped.ttf'}",
                          f"strokes={args.dir / 'Fitted-Regular.ttf'}"]
    labels = [s.split("=", 1)[0] for s in specs]
    faces = [fit.open_font(fit.Instance(s.split("=", 1)[1]), size) for s in specs]
    nf = len(faces) + 1
    s = args.scale
    pw, ph = cw * s, ch * s
    pad, labh = 12, 28
    tile_w, tile_h = nf * pw + (nf + 1) * pad, labh + ph + 2 * pad
    n = len(chars)
    ncols = min(args.cols, n)
    nrows = (n + ncols - 1) // ncols
    out = Image.new("RGB", (ncols * tile_w + pad, 44 + nrows * tile_h), (12, 16, 30))
    d = ImageDraw.Draw(out)
    lf = label_font()
    d.text((pad, 8), f"console | {' | '.join(labels)}  ({s}x, stroke +{args.grow} as in the game; "
           f"numbers: difference with the console's, Andika -> strokes)", fill=(200, 200, 200), font=lf)
    for i, c in enumerate(chars):
        tx = pad + (i % ncols) * tile_w
        ty = 44 + (i // ncols) * tile_h
        rep = report[c]
        d.text((tx, ty), f"{c}   {rep['before']:.2f} -> {rep['after']:.2f}", fill=(255, 220, 120), font=lf)
        con = console[c].cell.resize((pw, ph), Image.NEAREST)
        out.paste(tint(con), (tx, ty + labh))
        o = console[c]
        oc = (o.ink[0] + o.ink[2] + 1) / 2 + rows[c]["shift"]
        for j, face in enumerate(faces):
            g = fit.grown(fit.render_glyph(face, c, 4 * fit.R, None), args.grow)
            img = cell_at(g, sigma, k, bl, cw, ch, oc, s)
            img = img.point(lambda v: min(255, v))
            out.paste(tint(img), (tx + (j + 1) * (pw + pad), ty + labh))
    name = args.name or f"preview_{s}x.png"
    out.save(args.dir / name)
    print(args.dir / name)


if __name__ == "__main__":
    main()
