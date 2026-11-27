"""Checks the glyphs of a font drawn with the stroke tools as they come out
large, where the game shows them, not only at the console's 1x: each glyph
assembled (stroke_fit.py --assemble) against the console's glyph and its own
skeleton.

- blot: a piece of the outline outside the console's ink grown by
  BLOT_GRACE pixels (a bulge, a point standing out), its area in square cell
  pixels and where;
- counters: the closed counters (the holes of a, e, o, 8, @...) are as
  many as the console's;
- curve: a stroke whose curvature changes unevenly along it (stroke_fit's
  smoothness above CURVE: bends gathered at the nodes, flats between them,
  a wobble);
- bend: a stroke bending tighter than SPIKE radians per cell pixel (a
  radius under 1/SPIKE pixels, tighter than the pen: a kink);
- join: an end marked j off the centre line of the stroke it joins by more
  than JOIN_OFF pixels, its end standing out of that stroke;
- end: an end not marked j within NEAR pixels of another stroke's centre
  line (a join to mark, or an end that stands out of the stroke it touches).

A check the skeletons' module lets a glyph off (its ACCEPT: the user chose a
drawing the console's does not have) is not reported. Into
build/typeface/strokes[/name]/check.json, and printed: the glyphs with
something to look at. Reads the console's glyphs from the disc.

    python -I tools/typeface/strokes/check_glyphs.py [--font name] [--chars ...]
"""
import argparse
import importlib
import json
import sys
from collections import deque
from pathlib import Path

import numpy as np
import torch
from fontTools.ttLib import TTFont
from PIL import Image, ImageDraw

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import raster as fg  # noqa: E402
import stroke_fit as sf  # noqa: E402

K = 16  # samples a cell pixel, each way, for the outline's checks
BLOT_ALPHA = 2 / 15  # the console's ink: its faintest levels too
BLOT_GRACE = 0.5  # cell pixels the outline may stand past that ink
BLOT_AREA = 0.15  # square cell pixels: smaller pieces are let be
COUNTER_ALPHA = 0.5  # the console's counters: within ink this dark
COUNTER_AREA = 0.3  # square cell pixels: smaller holes are not counters
CURVE = 4.0  # smoothness of a stroke: the names' uneven curves 17-40 before --smooth; s and braces 2.4-3.4 by design
SPIKE = 1.6
JOIN_OFF = 0.25
NEAR = 0.5


def outline_polys(setup, table, tt, c, glyph):
    """The glyph's outline in cell pixels, placed as assemble_step placed
    it (overlay_strokes.py's way): a polygon a contour."""
    strokes, Ps, _ = sf.table_strokes(table, c)
    o_and, _ = setup.andika_today(c)
    allx = torch.cat([sf.flatten_stroke(st, P)[0][:, 0] for st, P in zip(strokes, Ps)])
    cx = float((allx.min() + allx.max()) / 2)
    cA = (o_and.coords[:, 0].min() + o_and.coords[:, 0].max()) / 2
    tx = cx - cA * setup.oras.sx
    o = fg.Outline(tt, glyph)
    polys = []
    for idx in o.contours():
        sub = fg.Outline.__new__(fg.Outline)
        sub.coords, sub.on, sub.ends = o.coords[idx], o.on[idx], [len(idx) - 1]
        plan = fg.plan_tensors(*fg.segment_plan(sub))
        p0, _ = fg.flatten(setup.oras.to_cell(torch.tensor(sub.coords), torch.tensor(tx)), plan)
        polys.append([(float(x), float(y)) for x, y in p0])
    return polys


def fill(polys, cw, ch, k):
    """The outline filled (even-odd: the merged contours do not cross), k
    samples a pixel."""
    acc = np.zeros((ch * k, cw * k), dtype=bool)
    for poly in polys:
        im = Image.new("1", (cw * k, ch * k), 0)
        ImageDraw.Draw(im).polygon([(x * k, y * k) for x, y in poly], fill=1)
        acc ^= np.asarray(im, dtype=bool)
    return acc


def dilate(mask, r):
    """Grown by a disc of r samples."""
    h, w = mask.shape
    pad = np.pad(mask, r)
    out = np.zeros_like(mask)
    for dy in range(-r, r + 1):
        for dx in range(-r, r + 1):
            if dx * dx + dy * dy <= r * r:
                out |= pad[r + dy:r + dy + h, r + dx:r + dx + w]
    return out


def components(mask):
    """Its 4-connected pieces: (size, (row, col) of their middle)."""
    seen = np.zeros_like(mask)
    h, w = mask.shape
    out = []
    for y0, x0 in zip(*np.nonzero(mask)):
        if seen[y0, x0]:
            continue
        seen[y0, x0] = True
        q, n, sy, sx = deque([(y0, x0)]), 0, 0, 0
        while q:
            y, x = q.popleft()
            n += 1
            sy += y
            sx += x
            for yy, xx in ((y + 1, x), (y - 1, x), (y, x + 1), (y, x - 1)):
                if 0 <= yy < h and 0 <= xx < w and mask[yy, xx] and not seen[yy, xx]:
                    seen[yy, xx] = True
                    q.append((yy, xx))
        out.append((n, (sy / n, sx / n)))
    return out


def counters(ink, k, min_area):
    """The holes of a filled shape (background pieces off its border), each
    its area in square cell pixels."""
    outside = np.pad(~ink, 1, constant_values=True)
    seen = np.zeros_like(outside)
    h, w = outside.shape
    q = deque([(0, 0)])
    seen[0, 0] = True
    while q:
        y, x = q.popleft()
        for yy, xx in ((y + 1, x), (y - 1, x), (y, x + 1), (y, x - 1)):
            if 0 <= yy < h and 0 <= xx < w and outside[yy, xx] and not seen[yy, xx]:
                seen[yy, xx] = True
                q.append((yy, xx))
    return [round(n / (k * k), 2) for n, _ in components(outside & ~seen) if n / (k * k) >= min_area]


def stroke_notes(strokes, Ps):
    """The checks on the strokes themselves: curves, bends, joins, ends."""
    notes = []
    lines = [None if st.dot or st.comma or st.part else sf.flatten_stroke(st, P)[0] for st, P in zip(strokes, Ps)]
    joined = {(k, i): o for k, i, o in sf.join_targets(strokes)}
    for k, (st, P) in enumerate(zip(strokes, Ps)):
        if st.dot or st.comma or st.part:
            continue
        if not st.fill and len(P) > 1:
            e = float(sf.smoothness([st], [P]))
            if e > CURVE:
                notes.append(f"curve: stroke {k} uneven, {e:.1f}")
            peak = max(float(sf.piece_curvature(*piece)[0].abs().max()) for piece in st.cubics(P))
            if peak > SPIKE:
                notes.append(f"bend: stroke {k} tight, {peak:.1f}")
        if st.closed:
            continue
        for i in (0, len(P) - 1):
            if (k, i) in joined:
                o = joined[(k, i)]
                d = float(sf.segment_distance(P[i], lines[o], strokes[o].closed))
                if d > JOIN_OFF:
                    notes.append(f"join: stroke {k} {d:.2f} off stroke {o}")
                continue
            near = [(float(sf.segment_distance(P[i], Q, strokes[o].closed)), o) for o, Q in enumerate(lines)
                    if o != k and Q is not None]
            if near and min(near)[0] < NEAR:
                notes.append(f"end: stroke {k} end {i} on stroke {min(near)[1]} (j?)")
    return notes


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--font", default="normal", choices=sorted(sf.PROFILES), help="as stroke_fit.py's")
    ap.add_argument("--chars", default=None)
    ap.add_argument("--quiet", action="store_true", help="only the summary line")
    a = ap.parse_args()
    sf.use(a.font)
    setup = sf.Setup(2.0)
    table = sf.load_table()
    tt = TTFont(str(sf.OUT / "Fitted-Regular.ttf"))
    report = json.loads((sf.OUT / "fit_report.json").read_text(encoding="utf-8"))
    cw, ch = setup.params["cw"], setup.params["ch"]
    chars = a.chars or "".join(c for c in sf.SKELETONS if c in report)
    unknown = [c for c in chars if c not in sf.SKELETONS]
    if unknown:
        raise SystemExit(f"no skeleton for {''.join(unknown)!a}")
    stale = [c for c in chars if c not in report or c not in table or sf.table_strokes(table, c) is None]
    if stale:
        raise SystemExit(f"{''.join(stale)!a}: to fit first (missing, or the skeleton changed), then assemble")
    grace = int(round(BLOT_GRACE * K))
    accept = getattr(importlib.import_module(sf.PROFILES[a.font].skeletons), "ACCEPT", {})
    result = {}
    for c in chars:
        notes = []
        ours = fill(outline_polys(setup, table, tt, c, report[c]["glyph"]), cw, ch, K)
        con = np.asarray(setup.console[c].cell, dtype=np.float64) / 255.0
        up = np.kron(con, np.ones((K, K)))
        for n, (y, x) in components(ours & ~dilate(up >= BLOT_ALPHA, grace)):
            if n / (K * K) >= BLOT_AREA:
                notes.append(f"blot: {n / (K * K):.2f} at {x / K:.1f},{y / K:.1f}")
        mine, theirs = counters(ours, K, COUNTER_AREA), counters(up >= COUNTER_ALPHA, K, COUNTER_AREA)
        if len(mine) != len(theirs):
            notes.append(f"counters: {len(mine)} (console {len(theirs)})")
        strokes, Ps, _ = sf.table_strokes(table, c)
        with torch.no_grad():
            notes += stroke_notes(strokes, Ps)
        # The user's choices against the console's glyph (the skeletons'
        # ACCEPT: a check's kind, before its note's colon) let be.
        accepted = accept.get(c, set())
        notes = [n for n in notes if n.split(":")[0] not in accepted]
        result[c] = notes
        if notes and not a.quiet:
            print(f"{c!a}: " + "; ".join(notes))
    path = sf.OUT / "check.json"
    kept = json.loads(path.read_text(encoding="utf-8")) if a.chars and path.exists() else {}
    kept.update(result)
    path.write_text(json.dumps(kept, indent=1, ensure_ascii=False), encoding="utf-8")
    bad = [c for c in chars if result[c]]
    print(f"{len(chars)} glyphs checked, {len(bad)} to look at: {''.join(bad)!a}")


if __name__ == "__main__":
    main()
