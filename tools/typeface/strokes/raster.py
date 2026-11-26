"""What the stroke tools share: a TrueType glyph's outline, a small
differentiable rasterizer that draws it into the game's cell at 1x, and the
measure of how far a drawing is from the console's glyph (make_table.py's).

S samples a cell pixel each way; a quadratic curve is K line pieces.
"""
import sys
from pathlib import Path

import numpy as np
import torch
from fontTools.pens.recordingPen import DecomposingRecordingPen
from fontTools.pens.ttGlyphPen import TTGlyphPen
from fontTools.ttLib import TTFont

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
sys.path.insert(0, str(HERE.parent))
import pages  # noqa: E402,F401  (the tools take it from here)

torch.set_default_dtype(torch.float64)
S = 4  # samples per cell pixel, each way
K = 8  # line pieces per quadratic curve
# Where the tools write (outside the repository: the console's pixels go
# into the previews).
OUT = ROOT / "build" / "typeface" / "strokes"


class Outline:
    """A decomposed TrueType glyph: points (font units), on-curve flags,
    contour ends."""

    def __init__(self, tt: TTFont, name: str):
        gs = tt.getGlyphSet()
        rec = DecomposingRecordingPen(gs)
        gs[name].draw(rec)
        pen = TTGlyphPen(None)
        rec.replay(pen)
        g = pen.glyph()
        self.name = name
        self.coords = np.array(g.coordinates, dtype=np.float64).reshape(-1, 2)
        self.on = np.array([f & 1 for f in g.flags], dtype=bool)
        self.ends = list(g.endPtsOfContours)

    def contours(self):
        start = 0
        for e in self.ends:
            yield list(range(start, e + 1))
            start = e + 1


def segment_plan(o: Outline):
    """Per quadratic segment: start, control, end, each as (i, j, w) =
    w * P[i] + (1 - w) * P[j]; a line's control is its middle."""
    A, C, B, line = [], [], [], []
    for idx in o.contours():
        n = len(idx)
        if n < 2:
            continue
        # The cycle with the implied on-curve points put in: (expr, on).
        seq = []
        for k in range(n):
            p, q = idx[k], idx[(k + 1) % n]
            seq.append(((p, p, 1.0), bool(o.on[p])))
            if not o.on[p] and not o.on[q]:
                seq.append(((p, q, 0.5), True))
        first = next(k for k, (_, on) in enumerate(seq) if on)
        seq = seq[first:] + seq[:first]
        m = len(seq)
        k = 0
        while k < m:
            cur = seq[k][0]
            nxt, nxt_on = seq[(k + 1) % m]
            if nxt_on:
                A.append(cur); C.append(None); B.append(nxt); line.append(True)
                k += 1
            else:
                end = seq[(k + 2) % m][0]
                A.append(cur); C.append(nxt); B.append(end); line.append(False)
                k += 2
    return A, C, B, line


def plan_tensors(A, C, B, line):
    def t(exprs):
        i = torch.tensor([e[0] if e else 0 for e in exprs])
        j = torch.tensor([e[1] if e else 0 for e in exprs])
        w = torch.tensor([e[2] if e else 0.0 for e in exprs], dtype=torch.float64)
        return i, j, w
    return t(A), t(C), t(B), torch.tensor(line)


def eval_expr(P, e):
    i, j, w = e
    return w[:, None] * P[i] + (1 - w[:, None]) * P[j]


def flatten(P, plan):
    """The segments as line pieces: their starts and ends."""
    (Ai, Ci, Bi, line) = plan
    a, b = eval_expr(P, Ai), eval_expr(P, Bi)
    c = torch.where(line[:, None], (a + b) / 2, eval_expr(P, Ci))
    t = torch.linspace(0, 1, K + 1, dtype=P.dtype)[None, :, None]
    q = (1 - t) ** 2 * a[:, None] + 2 * t * (1 - t) * c[:, None] + t ** 2 * b[:, None]
    return q[:, :-1].reshape(-1, 2), q[:, 1:].reshape(-1, 2)


class Raster:
    """A glyph in font units drawn into the cell (cw x ch pixels at 1x) at
    the table's scale (em_x, em_y cell pixels per em) on its baseline."""

    def __init__(self, cw, ch, em_x, em_y, baseline, upm):
        self.cw, self.ch = cw, ch
        self.sx, self.sy = em_x / upm, em_y / upm
        self.baseline = baseline
        ys, xs = torch.meshgrid(torch.arange(ch * S, dtype=torch.float64),
                                torch.arange(cw * S, dtype=torch.float64), indexing="ij")
        self.samples = torch.stack([(xs + 0.5) / S, (ys + 0.5) / S], -1).reshape(-1, 2)

    def to_cell(self, P, tx):
        return torch.stack([P[:, 0] * self.sx + tx, self.baseline - P[:, 1] * self.sy], -1)

    def render(self, P, plan, tx, sharp=1.0):
        """Coverage per cell pixel, (ch, cw), differentiable in P and tx."""
        p0, p1 = flatten(self.to_cell(P, tx), plan)
        s = self.samples
        d = p1 - p0  # (E,2)
        rel = s[:, None, :] - p0[None]  # (N,E,2)
        L2 = (d * d).sum(-1).clamp_min(1e-12)
        t = ((rel * d[None]).sum(-1) / L2).clamp(0, 1)
        proj = rel - t[..., None] * d[None]
        dist = torch.sqrt((proj * proj).sum(-1) + 1e-12).min(1).values  # (N,)
        with torch.no_grad():
            y = s[:, 1:2]
            x = s[:, 0:1]
            y0, y1 = p0[None, :, 1], p1[None, :, 1]
            cross = (p1[None, :, 0] - p0[None, :, 0]) * (y - y0) - (x - p0[None, :, 0]) * (y1 - y0)
            up = (y0 <= y) & (y1 > y) & (cross > 0)
            down = (y1 <= y) & (y0 > y) & (cross < 0)
            wind = up.sum(1) - down.sum(1)
            sign = torch.where(wind != 0, 1.0, -1.0).to(P.dtype)
        w = sharp / S
        cov = (0.5 + sign * dist / w).clamp(0, 1)
        return cov.reshape(self.ch, S, self.cw, S).mean((1, 3))


def blur(img, sigma=0.6):
    r = 2
    x = torch.arange(-r, r + 1, dtype=img.dtype)
    k = torch.exp(-x * x / (2 * sigma * sigma)); k = k / k.sum()
    im = img[None, None]
    im = torch.nn.functional.conv2d(torch.nn.functional.pad(im, (r, r, 0, 0)), k.view(1, 1, 1, -1))
    im = torch.nn.functional.conv2d(torch.nn.functional.pad(im, (0, 0, r, r)), k.view(1, 1, -1, 1))
    return im[0, 0]


def quantize(img):
    return torch.round(img * 15) / 15


def measure(img, target):
    """make_table's measure: L1 after a 0.6 blur, over the console's blurred
    ink (0 the same, 1 as far off as the ink itself)."""
    tb = blur(target)
    return float((blur(quantize(img)) - tb).abs().sum() / tb.sum())


def remapped_copy(src: Path, dest: Path, cmap_for: dict):
    """A copy of a font whose cmap maps each character to the glyph named."""
    tt = TTFont(str(src))
    for table in tt["cmap"].tables:
        if table.isUnicode():
            for ch, name in cmap_for.items():
                table.cmap[ord(ch)] = name
    tt.save(str(dest))
