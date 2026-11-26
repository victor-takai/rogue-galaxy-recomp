"""Rogue Letters' glyphs drawn as pen strokes (skeletons.py), the strokes'
places and the pen's width fitted to the console's pixels.

A pen is an ellipse (wx across, wy down, cell pixels); strokes end square
(butt), turn at corners with a miter (a bevel past the limit) and bend
smoothly elsewhere. The fit renders the strokes into the 18x22 cell at 1x
(4x4 samples a pixel, a signed distance per piece) and moves the nodes and
the pen until the render gives the console's pixels; each glyph fitted is
written into strokes.json (its nodes and pen), the table the font is built
from. Assembling strokes the table into outlines with skia (the same caps
and joins), merges them, converts them to TrueType curves and puts them in a
copy of Andika in place of the glyphs the typeface table drew from Andika
(andika_fit.json), into build/typeface/strokes/Fitted-Regular.ttf, which
build_font.py makes the repository's font from.

    python -I tools/typeface/strokes/stroke_fit.py [--chars 4M9] [--part 0/4] [--pen 1.75,1.65]
    python -I tools/typeface/strokes/stroke_fit.py --assemble
    python -I tools/typeface/strokes/stroke_fit.py --base

A fit of every glyph (no --chars nor --part) assembles after it. --base
writes andika_fit.json again, from make_table.py's fit of Andika Regular
(with its alternates) to the message font. Every step reads the console's
glyphs from the disc (disc/); the previews of a fit, beside the console's
pixels, go to build/typeface/strokes/glyphs.
"""
import argparse
import json
import math
import os
import sys
import time
from contextlib import contextmanager
from pathlib import Path

import numpy as np
import pathops
import torch
from fontTools.pens.areaPen import AreaPen
from fontTools.pens.cu2quPen import Cu2QuPen
from fontTools.pens.reverseContourPen import ReverseContourPen
from fontTools.pens.ttGlyphPen import TTGlyphPen
from fontTools.ttLib import TTFont
from fontTools.ttLib.tables import ttProgram

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import raster as fg  # noqa: E402
from skeletons import GLYPH_FOR, LIMIT_FOR, PULL_FOR, SKELETONS, parse  # noqa: E402

TABLE = HERE / "strokes.json"
BASE = HERE / "andika_fit.json"
ANDIKA = fg.ROOT / "thirdparty/fonts/andika/Andika-Regular.ttf"
torch.set_default_dtype(torch.float64)
S = fg.S
K = 10  # line pieces per curve
H = 0.5  # the pen's half width in its own (scaled) space
# The comma as drawn (the apostrophe's and the semicolon's too; the user
# chose it, 2026-11-25): a head as round as the period's dot and a tail
# leaving its right side, narrowing down and to the left to a point below
# the head's left half, a "9". Its outline's nodes in head radii about the
# head's centre (y down), clockwise from the top, and their kinds: the tip
# and the notch where the tail leaves the head are corners.
COMMA = [((0.0, -1.0), "s"), ((0.707, -0.707), "s"), ((1.0, 0.0), "s"), ((0.93, 0.85), "s"),
         ((0.5, 1.6), "s"), ((-0.7, 2.35), "c"), ((-0.08, 1.62), "s"), ((0.08, 0.97), "c"),
         ((-0.38, 0.925), "s"), ((-0.8, 0.6), "s"), ((-1.0, 0.0), "s"), ((-0.707, -0.707), "s")]


def unit(v):
    return v / torch.sqrt((v * v).sum(-1, keepdim=True) + 1e-12)


_ANDIKA = {}


def andika_font():
    """Andika Regular and the table's scale for it (cell pixels per unit)."""
    if "tt" not in _ANDIKA:
        _ANDIKA["tt"] = TTFont(str(ANDIKA))
        p = json.loads(BASE.read_text(encoding="utf-8"))
        upm = _ANDIKA["tt"]["head"].unitsPerEm
        _ANDIKA["scale"] = (p["em_x"] / upm, p["em_y"] / upm)
    return _ANDIKA["tt"], _ANDIKA["scale"]


class Part:
    """One contour of an Andika glyph, its own drawing kept: the fit only
    places it, sizes it and narrows or widens it."""

    def __init__(self, name, index, uniform=False):
        tt, (self.sx, self.sy) = andika_font()
        self.uniform = uniform  # sized only, never narrowed nor widened
        o = fg.Outline(tt, name)
        idx = list(o.contours())[index]
        self.coords = torch.tensor(o.coords[idx])  # font units, on and off points
        self.on = [bool(v) for v in o.on[idx]]
        sub = fg.Outline.__new__(fg.Outline)
        sub.coords, sub.on, sub.ends = o.coords[idx], o.on[idx], [len(idx) - 1]
        self.flat, _ = fg.flatten(self.coords, fg.plan_tensors(*fg.segment_plan(sub)))
        self.ref = (self.coords.min(0).values + self.coords.max(0).values) / 2

    def place(self, Q, P):
        """Font-unit points Q to cell pixels: P[0] is where the part's centre
        goes, P[1] = (size, extra width)."""
        kx = 1.0 if self.uniform else P[1][1]
        rel = torch.stack([(Q[:, 0] - self.ref[0]) * self.sx * kx,
                           -(Q[:, 1] - self.ref[1]) * self.sy], -1)
        return P[0] + P[1][0] * rel

    def path(self, P, scale):
        """The part's own curves, placed, in the pen's space, as a skia path."""
        from fontTools.pens.pointPen import PointToSegmentPen
        pts = self.place(self.coords, P) / scale
        path = pathops.Path()
        pen = PointToSegmentPen(path.getPen())
        pen.beginPath()
        n = len(pts)
        for i in range(n):
            kind = None
            if self.on[i]:
                kind = "qcurve" if not self.on[i - 1] else "line"
            pen.addPoint((float(pts[i][0]), float(pts[i][1])), segmentType=kind)
        pen.endPath()
        return path


class Stroke:
    def __init__(self, spec):
        sp = parse(spec)
        nodes, self.closed, self.round, self.fill = sp["nodes"], sp["closed"], sp["round"], sp["fill"]
        self.dot, self.comma = sp["shape"] == "dot", sp["shape"] == "comma"
        self.part = Part(*sp["part"], uniform=sp.get("uniform", False)) if sp["shape"] == "part" else None
        self.held = self.comma  # drawn, not fitted: the fit leaves its nodes as they start
        self.xy0 = [(x, y) for x, y, _ in nodes]
        kinds = [k for _, _, k in nodes]
        # An end marked h is cut level (along the baseline or the top), not
        # square to the stroke: a slanted leg stands flat.
        self.hcut = (not self.closed and not self.round and kinds[0] == "h",
                     not self.closed and not self.round and kinds[-1] == "h")
        self.kinds = ["c" if k == "h" else k for k in kinds]

    def level_ends(self, Q):
        """For each end cut level: (index, the end, the way into the stroke,
        how far the stroke runs on past the end to reach the level line)."""
        out = []
        for flag, i, j in ((self.hcut[0], 0, 1), (self.hcut[1], -1, -2)):
            if flag:
                u = unit(Q[j] - Q[i])  # into the stroke
                ext = H * u[0].abs() / u[1].abs().clamp_min(0.2)
                out.append((i, Q[i], u, ext))
        return out

    def cubics(self, P):
        """The stroke's cubic pieces (p0, c1, c2, p3) from its nodes P (n, 2):
        at a smooth node the curve runs along (next - previous); at a corner,
        straight at the neighbour; handles a third of the piece long. A dot's
        and a comma's two "nodes" are the centre and radii of the dot or the
        comma's head: an ellipse through 8 nodes, or COMMA's outline."""
        kinds = self.kinds
        if self.dot:
            a = torch.arange(8, dtype=torch.float64) * (math.pi / 4)
            P = P[0] + P[1] * torch.stack([torch.cos(a), torch.sin(a)], -1)
            kinds = ["s"] * 8
        elif self.comma:
            P = P[0] + P[1] * torch.tensor([xy for xy, _ in COMMA])
            kinds = [k for _, k in COMMA]
        n = len(P)
        closed = self.closed
        d_in, d_out = [None] * n, [None] * n
        for i in range(n):
            has_prev, has_next = closed or i > 0, closed or i < n - 1
            if kinds[i] in "tu" and has_prev and has_next:
                # Tangent to the straight piece before (t) or after (u).
                d = unit(P[i] - P[i - 1]) if kinds[i] == "t" else unit(P[(i + 1) % n] - P[i])
                d_in[i] = d_out[i] = d
            elif kinds[i] == "s" and has_prev and has_next:
                d = unit(P[(i + 1) % n] - P[i - 1])
                d_in[i] = d_out[i] = d
            else:
                if has_next:
                    d_out[i] = unit(P[(i + 1) % n] - P[i])
                if has_prev:
                    d_in[i] = unit(P[i] - P[i - 1])
        out = []
        for i in range(n if closed else n - 1):
            j = (i + 1) % n
            L = torch.sqrt(((P[j] - P[i]) ** 2).sum() + 1e-12)
            out.append((P[i], P[i] + d_out[i] * L / 3, P[j] - d_in[j] * L / 3, P[j]))
        return out

    def corner_at(self, i, n):
        """Whether the join at node i is a corner (a miter or bevel)."""
        if self.dot or self.comma or (not self.closed and (i == 0 or i == n - 1)):
            return False
        return self.kinds[i] == "c"


def flatten_stroke(st: Stroke, P):
    """The stroke as a polyline: points (M, 2), and per inner vertex whether
    it is a corner. Closed strokes wrap (the last point is not repeated)."""
    if st.part:
        Q = st.part.place(st.part.flat, P)
        return Q, [False] * len(Q)
    t = torch.linspace(0, 1, K + 1)[:, None]
    pts, corner = [], []
    cub = st.cubics(P)
    n = len(P)
    for s, (p0, c1, c2, p3) in enumerate(cub):
        q = (1 - t) ** 3 * p0 + 3 * t * (1 - t) ** 2 * c1 + 3 * t ** 2 * (1 - t) * c2 + t ** 3 * p3
        if s == 0:
            pts.append(q[0:1]); corner.append(st.corner_at(0, n))
        pts.append(q[1:])
        corner += [False] * (K - 1) + [st.corner_at((s + 1) % n, n)]
    Q = torch.cat(pts)
    if st.closed:
        Q = Q[:-1]
        corner = corner[:-1]
        corner[0] = st.corner_at(0, n)
    return Q, corner


def sd_pieces(p, A, B):
    """Signed distance from samples p (N,2) to butt-ended bars A->B (E,2)
    of half width H: (N, E)."""
    d = B - A
    L = torch.sqrt((d * d).sum(-1) + 1e-12)
    u = d / L[:, None]
    n = torch.stack([-u[:, 1], u[:, 0]], -1)
    rel = p[:, None, :] - A[None]
    t = (rel * u[None]).sum(-1)
    s = (rel * n[None]).sum(-1)
    qx = (t - L / 2).abs() - L / 2
    qy = s.abs() - H
    out = torch.sqrt(qx.clamp_min(0) ** 2 + qy.clamp_min(0) ** 2 + 1e-12) - 1e-6
    return out + torch.clamp(torch.maximum(qx, qy), max=0)


def sd_discs(p, C):
    return torch.sqrt(((p[:, None, :] - C[None]) ** 2).sum(-1) + 1e-12) - H


def sd_polys(p, V):
    """Signed distance to convex polygons V (C, k, 2)."""
    e = torch.roll(V, -1, 1) - V  # (C,k,2)
    rel = p[:, None, None, :] - V[None]  # (N,C,k,2)
    L2 = (e * e).sum(-1).clamp_min(1e-12)
    t = ((rel * e[None]).sum(-1) / L2[None]).clamp(0, 1)
    proj = rel - t[..., None] * e[None]
    dist = torch.sqrt((proj * proj).sum(-1) + 1e-12).min(-1).values  # (N,C)
    with torch.no_grad():
        cross = e[None, ..., 0] * rel[..., 1] - e[None, ..., 1] * rel[..., 0]  # (N,C,k)
        area = (V[..., 0] * torch.roll(V, -1, 1)[..., 1] - torch.roll(V, -1, 1)[..., 0] * V[..., 1]).sum(-1)
        sgn = torch.sign(area)[None, :, None]
        inside = ((cross * sgn) >= 0).all(-1)
    return torch.where(inside, -dist, dist)


def sd_fill(p, Q):
    """Signed distance to the filled closed polygon Q (M, 2), any shape."""
    e = torch.roll(Q, -1, 0) - Q
    rel = p[:, None, :] - Q[None]  # (N,M,2)
    L2 = (e * e).sum(-1).clamp_min(1e-12)
    t = ((rel * e[None]).sum(-1) / L2[None]).clamp(0, 1)
    proj = rel - t[..., None] * e[None]
    dist = torch.sqrt((proj * proj).sum(-1) + 1e-12).min(-1).values
    with torch.no_grad():
        y0, y1 = Q[None, :, 1], torch.roll(Q, -1, 0)[None, :, 1]
        y = p[:, 1:2]
        span = ((y0 <= y) & (y < y1)) | ((y1 <= y) & (y < y0))
        xc = Q[None, :, 0] + (y - y0) / torch.where(y1 == y0, torch.ones_like(y1), y1 - y0) * e[None, :, 0]
        inside = ((span & (xc > p[:, 0:1])).sum(-1) % 2) == 1
    return torch.where(inside, -dist, dist)


def corner_polys(Q, i_list, closed, limit):
    """Miter (or bevel past the limit) pieces at the polyline's corners."""
    polys = []
    M = len(Q)
    for i in i_list:
        a, v, b = Q[i - 1] if (i > 0 or closed) else None, Q[i], Q[(i + 1) % M]
        u1, u2 = unit(v - a), unit(b - v)
        n1 = torch.stack([-u1[1], u1[0]])
        n2 = torch.stack([-u2[1], u2[0]])
        with torch.no_grad():
            cr = float(u1[0] * u2[1] - u1[1] * u2[0])
        if abs(cr) < 1e-6:
            continue
        sigma = -1.0 if cr > 0 else 1.0  # the outer side
        A1, A2 = v + sigma * H * n1, v + sigma * H * n2
        m = n1 + n2
        m2 = (m * m).sum()
        with torch.no_grad():
            ratio = float(2 / torch.sqrt(m2 + 1e-12))
        if ratio <= limit:
            Mi = v + sigma * H * 2 * m / m2
        else:
            Mi = (A1 + A2) / 2
        polys.append(torch.stack([v, A1, Mi, A2]))
    return polys


class StrokeRaster:
    def __init__(self, cw, ch, limit):
        self.cw, self.ch, self.limit = cw, ch, limit
        ys, xs = torch.meshgrid(torch.arange(ch * S, dtype=torch.float64),
                                torch.arange(cw * S, dtype=torch.float64), indexing="ij")
        self.samples = torch.stack([(xs + 0.5) / S, (ys + 0.5) / S], -1).reshape(-1, 2)

    def render(self, strokes, Ps, pen, sharp=1.0, limit=None):
        limit = limit or self.limit
        scale = pen  # (2,): wx, wy
        p = self.samples / scale
        A, B, discs, polys, cuts, pclip, fills = [], [], [], [], [], [], []
        g = torch.sqrt(pen[0] * pen[1])
        for st, P in zip(strokes, Ps):
            Q, corner = flatten_stroke(st, P)
            if st.fill:  # a filled shape: no pen (its distance in the pen's units)
                fills.append(sd_fill(self.samples, Q) / g)
                continue
            Q = Q / scale
            Mq = len(Q)
            level = st.level_ends(Q)
            if st.closed:
                A.append(Q); B.append(torch.roll(Q, -1, 0))
                pclip.append((torch.zeros(Mq, 2), torch.zeros(Mq, 2), torch.zeros(Mq)))
                inner = range(Mq)
            else:
                a, b = Q[:-1], Q[1:]
                po, pn, pon = torch.zeros(Mq - 1, 2), torch.zeros(Mq - 1, 2), torch.zeros(Mq - 1)
                for i, E, u, ext in level:
                    # The end piece runs on past the end, then the level line
                    # through the end cuts it.
                    lvl = torch.stack([torch.zeros(()), torch.sign(u[1].detach())])
                    if i == 0:
                        a = torch.cat([(E - u * ext)[None], a[1:]])
                        po = torch.cat([E[None], po[1:]])
                        pn[0], pon[0] = lvl, 1.0
                    else:
                        b = torch.cat([b[:-1], (E - u * ext)[None]])
                        po = torch.cat([po[:-1], E[None]])
                        pn[-1], pon[-1] = lvl, 1.0
                A.append(a); B.append(b)
                pclip.append((po, pn, pon))
                inner = range(1, Mq - 1)
            smooth_i = [i for i in inner if not corner[i]]
            corner_i = [i for i in inner if corner[i]]
            if smooth_i:
                C = Q[smooth_i]
                discs.append(C)
                # A disc near an open stroke's end is cut at the end's line,
                # or it would round the square end (the pieces are short).
                o = torch.zeros(len(smooth_i), 2, 2)
                nrm = torch.zeros(len(smooth_i), 2, 2)
                on = torch.zeros(len(smooth_i), 2)
                if not st.closed and not st.round:
                    with torch.no_grad():
                        near0 = ((C - Q[0]) ** 2).sum(-1) < 1.5 ** 2
                        near1 = ((C - Q[-1]) ** 2).sum(-1) < 1.5 ** 2
                    u0, u1 = unit(Q[1] - Q[0]), unit(Q[-2] - Q[-1])
                    # A level end cuts its discs at the level line too.
                    if st.hcut[0]:
                        u0 = torch.stack([torch.zeros(()), torch.sign(u0[1].detach())])
                    if st.hcut[1]:
                        u1 = torch.stack([torch.zeros(()), torch.sign(u1[1].detach())])
                    o = torch.stack([Q[0].expand(len(C), 2), Q[-1].expand(len(C), 2)], 1)
                    nrm = torch.stack([u0.expand(len(C), 2), u1.expand(len(C), 2)], 1)
                    on = torch.stack([near0, near1], 1).to(torch.float64)
                cuts.append((o, nrm, on))
            if st.round and not st.closed:  # round ends: a disc on each
                discs.append(torch.stack([Q[0], Q[-1]]))
                cuts.append((torch.zeros(2, 2, 2), torch.zeros(2, 2, 2), torch.zeros(2, 2)))
            polys += corner_polys(Q, corner_i, st.closed, limit)
        sd = list(fills)
        if A:
            sp = sd_pieces(p, torch.cat(A), torch.cat(B))  # (N, E)
            po = torch.cat([c[0] for c in pclip])
            pn = torch.cat([c[1] for c in pclip])
            pon = torch.cat([c[2] for c in pclip])
            if bool((pon > 0).any()):
                behind = -((p[:, None, :] - po[None]) * pn[None]).sum(-1)  # (N, E)
                sp = torch.where(pon[None] > 0, torch.maximum(sp, behind), sp)
            sd.append(sp.min(1).values)
        if discs:
            dd = sd_discs(p, torch.cat(discs))  # (N, D)
            o = torch.cat([c[0] for c in cuts])  # (D, 2, 2)
            nrm = torch.cat([c[1] for c in cuts])
            on = torch.cat([c[2] for c in cuts])  # (D, 2)
            # Outside the end's line: (p - o) . n < 0.
            behind = -((p[:, None, None, :] - o[None]) * nrm[None]).sum(-1)  # (N, D, 2)
            behind = torch.where(on[None] > 0, behind, torch.full_like(behind, -1e9))
            dd = torch.maximum(dd, behind.max(-1).values)
            sd.append(dd.min(1).values)
        if polys:
            sd.append(sd_polys(p, torch.stack(polys)).min(1).values)
        sdf = torch.stack(sd).min(0).values * g
        cov = (0.5 - sdf / (sharp / S)).clamp(0, 1)
        return cov.reshape(self.ch, S, self.cw, S).mean((1, 3))


def loss_img(img, T):
    return ((img - T) ** 2).sum() + ((fg.blur(img) - fg.blur(T)) ** 2).sum() * 2


def fit_char(strokes, T, ras, args, pen0, limit, pull_w=None):
    pull_w = args.pull if pull_w is None else pull_w
    Ps = [torch.tensor(st.xy0, requires_grad=not st.held) for st in strokes]
    P0 = [P.detach().clone() for P in Ps]
    # The pen keeps its shape (pen0); only its size is the glyph's own.
    base = torch.tensor(pen0)
    lsize = torch.zeros((), requires_grad=not args.fixed_size)
    params = []
    free = [P for P in Ps if P.requires_grad]
    if free:
        params.append({"params": free, "lr": args.lr})
    if not args.fixed_size:
        params.append({"params": [lsize], "lr": 0.005})
    opt = torch.optim.Adam(params) if params else None
    for it in range(args.steps if opt else 0):
        opt.zero_grad()
        sharp = 3.0 if it < args.steps // 3 else (2.0 if it < 2 * args.steps // 3 else 1.0)
        img = ras.render(strokes, Ps, base * torch.exp(lsize), sharp, limit)
        pull = sum(((P - Q) ** 2).sum() for P, Q in zip(Ps, P0))
        loss = loss_img(img, T) + pull_w * pull
        loss.backward()
        opt.step()
    with torch.no_grad():
        pen = base * torch.exp(lsize)
        img = ras.render(strokes, Ps, pen, limit=limit)
    return [P.detach() for P in Ps], pen, img


# ---------------------------------------------------------------- outlines

def outline_glyph(strokes, Ps, pen, to_font, limit):
    """Stroke the skeleton with skia (in the pen's space, where it is round),
    merge, and draw as a TrueType glyph in font units."""
    wx, wy = float(pen[0]), float(pen[1])
    merged = pathops.Path()
    sc = torch.tensor([wx, wy])
    for st, P in zip(strokes, Ps):
        if st.part:  # Andika's own curves, placed
            path = st.part.path(P, sc)
            path.simplify(fix_winding=True, keep_starting_points=False)
            merged = pathops.op(merged, path, pathops.PathOp.UNION)
            continue
        path = pathops.Path()
        cub = [tuple(q / sc for q in c) for c in st.cubics(P)]  # in the pen's space
        # A level end: the stroke runs on past it along its way, then the
        # level line through the end cuts it (a box just past the end).
        level = []
        for flag, E, toward in ((st.hcut[0], cub[0][0], cub[0][1]), (st.hcut[1], cub[-1][3], cub[-1][2])):
            if flag:
                u = unit(toward - E)
                ext = float(H * u[0].abs() / u[1].abs().clamp_min(0.2))
                level.append((E, u, ext))
            else:
                level.append(None)
        p0 = cub[0][0]
        if level[0]:
            E, u, ext = level[0]
            path.moveTo(float(E[0] - u[0] * ext), float(E[1] - u[1] * ext))
            path.lineTo(float(p0[0]), float(p0[1]))
        else:
            path.moveTo(float(p0[0]), float(p0[1]))
        for _, c1, c2, p3 in cub:
            path.cubicTo(float(c1[0]), float(c1[1]), float(c2[0]), float(c2[1]), float(p3[0]), float(p3[1]))
        if level[1]:
            E, u, ext = level[1]
            path.lineTo(float(E[0] - u[0] * ext), float(E[1] - u[1] * ext))
        if st.closed:
            path.close()
        if not st.fill:  # a filled shape is its own outline
            cap = pathops.LineCap.ROUND_CAP if st.round else pathops.LineCap.BUTT_CAP
            path.stroke(2 * H, cap, pathops.LineJoin.MITER_JOIN, limit)
            path.convertConicsToQuads(0.002)  # round ends come as conics (the pen is 1 wide here)
        for lv in level:
            if lv is None:
                continue
            E, u, ext = lv
            ex, ey = float(E[0]), float(E[1])
            half = H / max(abs(float(u[1])), 0.2) + 0.2
            y0, y1 = (ey - 3, ey) if float(u[1]) > 0 else (ey, ey + 3)
            box = pathops.Path()
            box.moveTo(ex - half, y0); box.lineTo(ex + half, y0); box.lineTo(ex + half, y1)
            box.lineTo(ex - half, y1); box.close()
            path = pathops.op(path, box, pathops.PathOp.DIFFERENCE)
        # Each piece made whole on its own, then merged: a filled shape and a
        # stroke may run opposite ways, which would cut holes in one path.
        path.simplify(fix_winding=True, keep_starting_points=False)
        merged = pathops.op(merged, path, pathops.PathOp.UNION)
    # Back to cell pixels, then to font units (y up).
    sx, sy, tx, bl = to_font
    merged = merged.transform(wx / sx, 0, 0, -wy / sy, -tx / sx, bl / sy)
    merged.simplify(fix_winding=True, keep_starting_points=False)
    area = AreaPen()
    merged.draw(area)
    tt_pen = TTGlyphPen(None)
    # TrueType outer contours run clockwise (negative area in AreaPen).
    target = ReverseContourPen(tt_pen) if area.value > 0 else tt_pen
    merged.draw(Cu2QuPen(target, max_err=1.0, reverse_direction=False))
    return tt_pen.glyph()


# ---------------------------------------------------------------- main

class Setup:
    """What every step needs: the table's fit of Andika, the console's glyphs,
    Andika."""

    def __init__(self, limit):
        self.params = json.loads(BASE.read_text(encoding="utf-8"))
        self.rows = {r["char"]: r for r in self.params["rows"]}
        self.src = ANDIKA
        self.andika = TTFont(str(self.src))
        upm = self.andika["head"].unitsPerEm
        fonts = fg.pages.load_fonts(fg.ROOT / "disc")
        self.console = {g.char: g for g in fonts["normal"] if g.char}
        p = self.params
        self.oras = fg.Raster(p["cw"], p["ch"], p["em_x"], p["em_y"], p["baseline"], upm)
        self.ras = StrokeRaster(p["cw"], p["ch"], limit)

    def target(self, c):
        return torch.tensor(np.asarray(self.console[c].cell, dtype=np.float64) / 255.0)

    def andika_today(self, c):
        """Andika's glyph as the table drew it (andika_fit.json), placed on
        the console's ink centre and the table's shift, rendered at 1x."""
        r = self.rows[c]
        o = fg.Outline(self.andika, r["glyph"])
        ink = self.console[c].ink
        oc = (ink[0] + ink[2] + 1) / 2 + r["shift"]
        xs = o.coords[:, 0] * self.oras.sx
        tx0 = oc - (xs.min() + xs.max()) / 2
        img = self.oras.render(torch.tensor(o.coords), fg.plan_tensors(*fg.segment_plan(o)),
                               torch.tensor(tx0, dtype=torch.float64))
        return o, img


# ---------------------------------------------------------------- the table

def load_table():
    """strokes.json: per character, the pen (wx, wy), each stroke's nodes and
    the skeleton's form they were fitted to (skeleton_form)."""
    return json.loads(TABLE.read_text(encoding="utf-8")) if TABLE.exists() else {}


def skeleton_form(c):
    """A glyph's skeleton without its nodes' places, a string a stroke: its
    words as written, each node as "." or its kind letter (a dot's, a
    comma's and a part's size the same). The fitted nodes hold for the
    skeleton while this is the same (table_strokes)."""
    return [" ".join((t[-1] if t[-1] in "cshtu" else ".") if "," in t else t for t in spec.split())
            for spec in SKELETONS[c]]


def write_table(table):
    """A line a glyph, in the skeletons' order: a glyph fitted again shows in
    a diff as its own line. Written beside it, then put in its place: a fit
    stopped meanwhile leaves the table as it was."""
    order = [c for c in SKELETONS if c in table] + sorted(c for c in table if c not in SKELETONS)
    lines = [f" {json.dumps(c, ensure_ascii=False)}: {json.dumps(table[c])}" for c in order]
    new = TABLE.with_suffix(".json.new")
    new.write_text("{\n" + ",\n".join(lines) + "\n}\n", encoding="utf-8", newline="\n")
    os.replace(new, TABLE)


@contextmanager
def table_lock():
    """Fits run side by side (--part) write the table in turn. A fit killed
    while writing may leave the lock (build/typeface/strokes/strokes.lock):
    delete it (said after a few seconds of waiting)."""
    fg.OUT.mkdir(parents=True, exist_ok=True)
    lock = fg.OUT / "strokes.lock"
    waited = 0
    while True:
        try:
            fd = os.open(lock, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
            break
        except FileExistsError:
            waited += 1
            if waited == 50:
                print(f"waiting for {lock}: another fit is writing the table, or one stopped while "
                      f"writing left it (then delete it)", file=sys.stderr, flush=True)
            time.sleep(0.1)
    try:
        yield
    finally:
        os.close(fd)
        os.unlink(lock)


def table_strokes(table, c):
    """A glyph's strokes, their nodes and its pen from the table, or None
    when its skeleton changed since its fit (a stroke or a node more or
    less, a node's kind, a stroke's kind or flags, or a stroke the fit
    holds, a comma, moved)."""
    strokes = [Stroke(s) for s in SKELETONS[c]]
    nodes = table[c]["nodes"]
    if table[c].get("form") != skeleton_form(c) or [len(st.xy0) for st in strokes] != [len(n) for n in nodes]:
        return None
    for st, n in zip(strokes, nodes):
        if st.held and any(abs(a - b) > 5e-4 for p, q in zip(st.xy0, n) for a, b in zip(p, q)):
            return None
    return strokes, [torch.tensor(n) for n in nodes], torch.tensor(table[c]["pen"])


# ---------------------------------------------------------------- the steps

def fit_step(setup, chars, args, pen0):
    """Each glyph fitted and written into the table; beside it, in
    build/typeface/strokes/glyphs, the console's cell, Andika's and the fit
    at 1x (CODE.npy, for looking at)."""
    per = fg.OUT / "glyphs"
    per.mkdir(parents=True, exist_ok=True)
    t0 = time.time()
    for c in chars:
        T = setup.target(c)
        strokes = [Stroke(s) for s in SKELETONS[c]]
        with torch.no_grad():
            start = setup.ras.render(strokes, [torch.tensor(st.xy0) for st in strokes], torch.tensor(pen0),
                                     limit=LIMIT_FOR.get(c, args.limit))
        Ps, pen, img = fit_char(strokes, T, setup.ras, args, pen0, LIMIT_FOR.get(c, args.limit),
                                PULL_FOR.get(c))
        _, before = setup.andika_today(c)
        entry = {"pen": [round(float(v), 4) for v in pen],
                 "nodes": [[[round(float(x), 3), round(float(y), 3)] for x, y in P] for P in Ps],
                 "form": skeleton_form(c)}
        with table_lock():
            table = load_table()
            table[c] = entry
            write_table(table)
        np.save(per / f"{ord(c):04X}.npy", np.stack((T.numpy(), before.numpy(), img.numpy())))
        print(f"{c!a} andika {fg.measure(before, T):.3f}  start {fg.measure(start, T):.3f}  "
              f"fit {fg.measure(img, T):.3f}  pen {entry['pen']}  ({time.time() - t0:.0f} s)", flush=True)


def assemble_step(setup, limit):
    """Every glyph of the table stroked into a copy of Andika, in place of
    the glyph the typeface table drew for its character, into
    build/typeface/strokes: Fitted-Regular.ttf, Andika-remapped.ttf (Andika
    with those glyphs, for the previews) and fit_report.json (per glyph, the
    difference with the console's: Andika's, the fit's and the outline's as
    written)."""
    table = load_table()
    stale = [c for c in SKELETONS if c not in table or table_strokes(table, c) is None]
    unknown = [c for c in table if c not in SKELETONS]
    if stale or unknown:
        raise SystemExit("strokes.json: to fit first (missing, or the skeleton changed): "
                         f"{''.join(stale)!a}; without a skeleton: {''.join(unknown)!a}")
    tt = TTFont(str(setup.src))
    glyf, hmtx = tt["glyf"], tt["hmtx"]
    order = set(tt.getGlyphOrder())
    p = setup.params
    report, cmap_new, cmap_old = {}, {}, {}
    for c in sorted(table, key=ord):
        strokes, Ps, pen = table_strokes(table, c)
        old = setup.rows[c]["glyph"]
        name = GLYPH_FOR.get(c, old)
        assert name in order, name
        o, before = setup.andika_today(c)
        # Centred where Andika's glyph was in the em.
        allx = torch.cat([flatten_stroke(st, P)[0][:, 0] for st, P in zip(strokes, Ps)])
        cx = float((allx.min() + allx.max()) / 2)
        cA = (o.coords[:, 0].min() + o.coords[:, 0].max()) / 2
        tx = cx - cA * setup.oras.sx
        g = outline_glyph(strokes, Ps, pen, (setup.oras.sx, setup.oras.sy, tx, p["baseline"]),
                          LIMIT_FOR.get(c, limit))
        g.program = ttProgram.Program(); g.program.fromBytecode(b"")
        glyf[name] = g
        g.recalcBounds(glyf)
        hmtx[name] = (hmtx[name][0], g.xMin)
        # The outline as written, rendered back: the check that it is the fit.
        T = setup.target(c)
        o2 = fg.Outline(tt, name)
        final = setup.oras.render(torch.tensor(o2.coords), fg.plan_tensors(*fg.segment_plan(o2)),
                                  torch.tensor(tx, dtype=torch.float64))
        with torch.no_grad():
            fitted = setup.ras.render(strokes, Ps, pen, limit=LIMIT_FOR.get(c, limit))
        report[c] = {"glyph": name, "before": round(fg.measure(before, T), 4),
                     "fit": round(fg.measure(fitted, T), 4), "after": round(fg.measure(final, T), 4),
                     "pen": table[c]["pen"]}
        cmap_new[c], cmap_old[c] = name, old
    for cmap in tt["cmap"].tables:
        if cmap.isUnicode():
            for c, name in cmap_new.items():
                cmap.cmap[ord(c)] = name
    for rec in tt["name"].names:
        if rec.nameID in (1, 4, 16, 18):
            rec.string = "Strokes Draft"
        elif rec.nameID == 6:
            rec.string = "StrokesDraft"
    fg.OUT.mkdir(parents=True, exist_ok=True)
    tt.save(str(fg.OUT / "Fitted-Regular.ttf"))
    fg.remapped_copy(setup.src, fg.OUT / "Andika-remapped.ttf", cmap_old)
    (fg.OUT / "fit_report.json").write_text(json.dumps(report, indent=1, ensure_ascii=False), encoding="utf-8")
    worst = sorted(report.items(), key=lambda kv: -abs(kv[1]["after"] - kv[1]["fit"]))[:5]
    print(f"{len(report)} glyphs; mean {np.mean([v['before'] for v in report.values()]):.3f} -> "
          f"{np.mean([v['after'] for v in report.values()]):.3f}; written vs fitted, the farthest: "
          + ", ".join(f"{c!a} {v['fit']:.3f}/{v['after']:.3f}" for c, v in worst))
    print(fg.OUT / "Fitted-Regular.ttf")


def base_step():
    """andika_fit.json: make_table.py's fit of Andika Regular (with its
    alternates) to the message font: the em the strokes are drawn in, and
    the glyph each character took, whose place in the font it takes."""
    import make_table  # tools/typeface
    work = fg.OUT / "work"
    work.mkdir(parents=True, exist_ok=True)
    fonts = fg.pages.load_fonts(fg.ROOT / "disc")
    t = make_table.table_for("normal", str(ANDIKA) + "+alt", fonts["normal"], work)
    out = {k: t[k] for k in ("font", "cw", "ch", "file", "em_x", "em_y", "baseline", "grow", "pen_x", "pen_y",
                             "slant")}
    out["extra_grow"] = make_table.EXTRA_GROW.get("normal", 0)
    out["rows"] = [{"char": c, "gid": gid, "glyph": name, "shift": s, "lift": lift, "err": e, "gap": gap}
                   for c, gid, name, s, lift, e, gap in t["rows"]]
    out["tags"] = t["tags"]
    BASE.write_text(json.dumps(out, indent=1, ensure_ascii=False) + "\n", encoding="utf-8", newline="\n")
    print(BASE, len(out["rows"]), "rows")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--chars", default=None, help="the characters to fit (default: every skeleton)")
    ap.add_argument("--part", default=None, help="i/n: every n-th character of those from the i-th")
    ap.add_argument("--assemble", action="store_true", help="only write the font from the table")
    ap.add_argument("--base", action="store_true", help="only write andika_fit.json again")
    ap.add_argument("--steps", type=int, default=450)
    ap.add_argument("--lr", type=float, default=0.03)
    ap.add_argument("--pull", type=float, default=0.002)
    ap.add_argument("--pen", default="1.75,1.65",
                    help="the pen's wx,wy (cell pixels): its shape, and its size unless fitted")
    ap.add_argument("--fixed-size", action="store_true", help="every glyph with the pen's size as given")
    ap.add_argument("--limit", type=float, default=2.0, help="miter limit (skia's); LIMIT_FOR overrides")
    ap.add_argument("--threads", type=int, default=8)
    args = ap.parse_args()
    if args.base:
        base_step()
        return
    torch.set_num_threads(args.threads)
    pen0 = [float(v) for v in args.pen.split(",")]
    chars = args.chars or "".join(SKELETONS)
    unknown = [c for c in chars if c not in SKELETONS]
    if unknown:
        raise SystemExit(f"no skeleton for {''.join(unknown)!a}")
    if args.part:
        i, n = (int(v) for v in args.part.split("/"))
        chars = chars[i::n]
    setup = Setup(args.limit)
    if not args.assemble:
        fit_step(setup, chars, args, pen0)
    if args.assemble or (args.chars is None and args.part is None):
        assemble_step(setup, args.limit)


if __name__ == "__main__":
    main()
