"""The fidelity measure and fit: how close a typeface, drawn at the game's 1x,
comes to the game's own glyphs, and the size and place that bring it
closest.

A candidate glyph is drawn at R times the game's size (FreeType through
Pillow), box-filtered into the game's cell (each cell pixel the mean
coverage of its area) and quantized to the font's 16 alpha levels. Its
difference with the console glyph, summed and divided by the glyphs' ink,
is the score: 0 is identical, 1 as far off as the ink itself.

The fit is global per typeface: the vertical scale (the size), an extra
horizontal scale, the baseline row and a growth or thinning of the strokes
(in 1/R pixel steps); per glyph only its horizontal place.
"""
import math
from dataclasses import asdict, dataclass, field
from pathlib import Path

from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont, ImageStat

from pages import FONTS

R = 8  # render scale
MISSING_PROBE = "\U0010FFFD"
# The 16 alpha levels of the font pages (0-0x80), doubled as pages.py keeps them.
QUANT = bytes(min(255, 2 * round(round(c * 15 / 255) * 0x80 / 15)) for c in range(256))


@dataclass
class Instance:
    path: str
    axes: dict = field(default_factory=dict)  # axis name -> value, for a variable font


@dataclass
class Params:
    sigma: float  # cell pixels per R render pixels, down (times R)
    k: float  # extra horizontal scale
    bl: float  # the baseline, in cell rows from the top
    grow: int  # render pixels added (+) or taken (-) on each side of a stroke


def open_font(inst: Instance, size: int):
    f = ImageFont.truetype(inst.path, size, layout_engine=ImageFont.Layout.BASIC)
    if inst.axes:
        values = []
        for a in f.get_variation_axes():
            name = a["name"].decode() if isinstance(a["name"], bytes) else a["name"]
            values.append(inst.axes.get(name, a["default"]))
        f.set_variation_by_axes(values)
    return f


@dataclass
class HrGlyph:
    img: Image.Image  # "L", coverage 0-255
    ax: int  # the anchor (left, baseline) in img
    ay: int
    ink: tuple  # getbbox of img (x0, y0, x1, y1), exclusive ends
    missing: bool


def render_glyph(font, ch: str, pad: int, notdef: bytes | None) -> HrGlyph:
    l, t, r, b = font.getbbox(ch, anchor="ls")
    w, h = (r - l) + 2 * pad, (b - t) + 2 * pad
    img = Image.new("L", (max(w, 1), max(h, 1)), 0)
    ax, ay = pad - l, pad - t
    ImageDraw.Draw(img).text((ax, ay), ch, font=font, fill=255, anchor="ls")
    ink = img.getbbox()
    missing = ink is None or (notdef is not None and img.crop(ink).tobytes() == notdef)
    return HrGlyph(img, ax, ay, ink or (0, 0, 0, 0), missing)


def grown(g: HrGlyph, grow: int) -> HrGlyph:
    if grow == 0 or g.missing:
        return g
    f = ImageFilter.MaxFilter(2 * grow + 1) if grow > 0 else ImageFilter.MinFilter(-2 * grow + 1)
    img = g.img.filter(f)
    return HrGlyph(img, g.ax, g.ay, img.getbbox() or (0, 0, 0, 0), g.missing)


def cell_image(g: HrGlyph, p: Params, cw: int, ch: int, center: float) -> Image.Image:
    """The glyph in a cw x ch cell at 1x: its ink centred on column `center`."""
    sx = R / (p.sigma * p.k)  # render pixels per cell column
    sy = R / p.sigma
    cx = (g.ink[0] + g.ink[2]) / 2
    x0 = cx - center * sx
    y0 = g.ay - p.bl * sy
    box = (x0, y0, x0 + cw * sx, y0 + ch * sy)
    W, H = g.img.size
    src = g.img
    if box[0] < 0 or box[1] < 0 or box[2] > W or box[3] > H:
        px = int(math.ceil(max(0, -box[0], box[2] - W))) + 2
        py = int(math.ceil(max(0, -box[1], box[3] - H))) + 2
        src = Image.new("L", (W + 2 * px, H + 2 * py), 0)
        src.paste(g.img, (px, py))
        box = (box[0] + px, box[1] + py, box[2] + px, box[3] + py)
    return src.resize((cw, ch), Image.BOX, box=box).point(QUANT)


def glyph_error(g: HrGlyph, orig, p: Params, cw: int, ch: int, shifts=(-0.5, -0.25, 0, 0.25, 0.5)):
    """(error, best shift) for one glyph: the L1 difference at its best place."""
    if g.missing:
        return orig.ink_sum, 0.0
    oc = (orig.ink[0] + orig.ink[2] + 1) / 2
    best = None
    for s in shifts:
        e = ImageStat.Stat(ImageChops.difference(cell_image(g, p, cw, ch, oc + s), orig.cell)).sum[0]
        if best is None or e < best[0]:
            best = (e, s)
    return best


class Target:
    """A game font's printable glyphs, with what the fit needs."""

    def __init__(self, font: str, glyphs):
        self.font = font
        _, self.cw, self.ch, _ = FONTS[font]
        self.glyphs = [g for g in glyphs if g.char and g.char != " " and g.ink]
        for g in self.glyphs:
            g.ink_sum = ImageStat.Stat(g.cell).sum[0]
        self.by_char = {g.char: g for g in self.glyphs}
        H = self.by_char["H"]
        self.cap = H.ink[3] - H.ink[1] + 1
        self.baseline = H.ink[3] + 1


def score(hr: dict, target: Target, p: Params, chars) -> float:
    err = ink = 0.0
    for c in chars:
        o = target.by_char[c]
        err += glyph_error(grown(hr[c], p.grow), o, p, target.cw, target.ch)[0]
        ink += o.ink_sum
    return err / ink


def render_set(inst: Instance, target: Target, chars, render_map=None):
    """The glyphs at R times a size whose cap height is near the target's.
    render_map: char -> the string drawn for it (an alternate's code point)."""
    size = int(round(target.cap * R * 1.4))
    font = open_font(inst, size)
    probe = render_glyph(font, MISSING_PROBE, 4, None)
    notdef = probe.img.crop(probe.ink).tobytes() if not probe.missing else None
    return {c: render_glyph(font, (render_map or {}).get(c, c), 4 * R, notdef) for c in chars}


def initial(hr, target: Target) -> Params | None:
    H = hr["H"]
    if H.missing:
        return None
    sigma = target.cap * R / max(H.ink[3] - H.ink[1], 1)
    wo = wc = 0.0
    for c, o in target.by_char.items():
        g = hr.get(c)
        if g is None or g.missing:
            continue
        wo += o.ink[2] - o.ink[0] + 1
        wc += (g.ink[2] - g.ink[0]) * sigma / R
    return Params(sigma, wo / wc if wc else 1.0, float(target.baseline), 0)


def descend(hr, target: Target, chars, p: Params, steps, sweeps):
    best = score(hr, target, p, chars)
    for _ in range(sweeps):
        improved = False
        for name, opts in steps.items():
            for o in opts:
                if name == "height":  # taller or shorter, as wide
                    q = Params(p.sigma * o, p.k / o, p.bl, p.grow)
                else:
                    base = getattr(p, name)
                    v = base * o if name in ("sigma", "k") else base + o
                    if name == "grow" and not -2 <= v <= 6:
                        continue
                    q = Params(**{**asdict(p), name: v})
                s = score(hr, target, q, chars)
                if s < best - 1e-9:
                    best, p, improved = s, q, True
        if not improved:
            break
    return best, p


def fit(inst: Instance, target: Target, chars):
    """Coarse then fine coordinate descent over the global parameters, the
    coarse one from each stroke growth of -1 to 3 (the size and the growth
    trade off, and one start stops short). Returns (score, Params), or
    (9.0, None) without an H."""
    hr = render_set(inst, target, sorted(set(chars) | {"H"}))
    p0 = initial(hr, target)
    if p0 is None:
        return 9.0, None
    best = None
    for grow in (-1, 0, 1, 2, 3):
        start = Params(p0.sigma, p0.k, p0.bl, grow)
        s, p = descend(hr, target, chars, start, {
            "sigma": [0.94, 0.97, 1.03, 1.06], "k": [0.92, 0.96, 1.04, 1.08],
            "bl": [-0.5, -0.25, 0.25, 0.5], "grow": [-1, 1]}, 4)
        s, p = descend(hr, target, chars, p, {
            "sigma": [0.985, 0.995, 1.005, 1.015], "k": [0.985, 0.995, 1.005, 1.015],
            "height": [0.985, 0.995, 1.005, 1.015], "bl": [-0.125, 0.125], "grow": [-1, 1]}, 4)
        if best is None or s < best[0]:
            best = (s, p)
    return best


def label(inst: Instance) -> str:
    name = Path(inst.path).stem
    if inst.axes:
        name += " " + " ".join(f"{k}={v:g}" for k, v in inst.axes.items())
    return name
