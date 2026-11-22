"""Writes src/typeface_table.inc, what src/typeface_letters.cpp draws the
game's Latin letters from a typeface with: per game font the typeface's
size, baseline and stroke growth that bring it closest to the console's
glyphs, and per character the glyph drawn (the default one, one of the
typeface's alternates, or a curly quote) with its place.

usage (from the repository's root, with the disc extracted):
  python -I tools/typeface/make_table.py \\
      normal=thirdparty/fonts/andika/Andika-Regular.ttf+alt \\
      system=thirdparty/fonts/andika/Andika-Bold.ttf+alt \\
      name=thirdparty/fonts/andika/Andika-Regular.ttf+alt
  [--disc disc] [--out src/typeface_table.inc] [--work build/typeface]

`+alt` lets each character take one of the typeface's alternates where it
is closer to the console's. Needs Pillow and fontTools (pip install pillow
fonttools). The console glyphs are read from the disc into memory; only
numbers and glyph indices are written.
"""
import argparse
import site
import sys
from pathlib import Path

site.addsitedir(site.getusersitepackages())  # a pip --user install, which -I hides
sys.path.insert(0, str(Path(__file__).resolve().parent))

from fontTools.ttLib import TTFont  # noqa: E402
from PIL import ImageChops, ImageFilter, ImageStat  # noqa: E402

import alternates  # noqa: E402
import fit  # noqa: E402
import pages  # noqa: E402

# The error per glyph: L1 after a small blur, which forgives a fraction of a
# pixel of place and anti-aliasing, so the shape counts more.
BLUR = ImageFilter.GaussianBlur(0.6)
# A glyph's horizontal place, in cell pixels from the console glyph's ink centre.
SHIFTS = [s / 8 for s in range(-6, 7)]
# A sign's vertical place, in cell pixels below the baseline: the typeface's
# colon, asterisk or underscore sit elsewhere than the console's.
LIFTS = [d / 4 for d in range(-28, 29)]
# The console's apostrophe and quotes are curly: the typeface's curly ones
# are candidates too.
CURLY = {"'": [0x2019], '"': [0x201D], "`": [0x2018]}
# Alternates that are another kind of glyph, not another drawing of the same
# character: small capitals, superiors and inferiors, fractions, ordinals,
# full-width forms, a letter with a mark.
NOT_A_DRAWING = ("uniFF", ".sc", "smcp", "c2sc", ".sups", "sinf", ".subs", "numr", "dnom", "ordn",
                 "superior", "inferior", ".sinf", ".supr", "0301", "0300", "0308")


def blurred_error(img, o) -> float:
    return ImageStat.Stat(ImageChops.difference(img.filter(BLUR), o.blur)).sum[0] / o.blur_sum


def choose_variants(inst, target, p, table, chars, tags):
    """Per character, the default glyph or an alternate, whichever is closer
    to the console's with the fit (an alternate must win by 0.005).
    Returns {char: the string drawn}."""
    size = int(round(target.cap * fit.R * 1.4))
    font = fit.open_font(inst, size)
    probe = fit.render_glyph(font, fit.MISSING_PROBE, 4, None)
    notdef = probe.img.crop(probe.ink).tobytes() if not probe.missing else None
    chosen = {}
    for c in chars:
        options = [(c, None)] + [(chr(cp), f"{glyph} ({tag})") for cp, glyph, tag in table.get(c, [])
                                 if not any(k in glyph for k in NOT_A_DRAWING)]
        if len(options) == 1:
            continue
        o = target.by_char[c]
        oc = (o.ink[0] + o.ink[2] + 1) / 2
        best = None
        for drawn, name in options:
            g = fit.grown(fit.render_glyph(font, drawn, 4 * fit.R, notdef), p.grow)
            if g.missing:
                continue
            for s in (-0.5, -0.25, 0, 0.25, 0.5):
                e = blurred_error(fit.cell_image(g, p, target.cw, target.ch, oc + s), o)
                if best is None or e < best[0] - 0.005:
                    best = (e, drawn, name)
        if best and best[2]:
            chosen[c] = best[1]
            tags.append(f"{c}={best[2]}")
    return chosen


def table_for(font, spec, glyphs, work: Path):
    use_alt = spec.endswith("+alt")
    path = Path(spec[:-4] if use_alt else spec)
    target = fit.Target(font, glyphs)
    cw, ch = target.cw, target.ch
    for o in target.glyphs:
        o.blur = o.cell.filter(BLUR)
        o.blur_sum = ImageStat.Stat(o.blur).sum[0]
    chars = sorted(target.by_char)
    inst = fit.Instance(str(path))
    _, p = fit.fit(inst, target, chars)
    if p is None:
        raise SystemExit(f"{path} has no H")
    tt = TTFont(str(path))
    cmap = tt.getBestCmap()
    table, render_inst = {}, inst
    if use_alt:
        copy = work / f"{path.stem}-alternates{path.suffix}"
        table = alternates.drawable_copy(path, copy)
        if table:
            render_inst = fit.Instance(str(copy))
    for c, cps in CURLY.items():
        for cp in cps:
            if cp in cmap and c in chars:
                table = {**table, c: list(table.get(c, [])) + [[cp, cmap[cp], "curly"]]}
    names = {chr(cp): glyph for alts in table.values() for cp, glyph, _ in alts}
    tags = []
    chosen = choose_variants(render_inst, target, p, table, chars, tags)
    hr = fit.render_set(render_inst, target, chars, chosen)
    rows = []
    for c in chars:
        o = target.by_char[c]
        g = fit.grown(hr[c], p.grow)
        glyph = names[chosen[c]] if c in chosen else cmap.get(ord(c))
        if g.missing or glyph is None:
            continue
        oc = (o.ink[0] + o.ink[2] + 1) / 2
        best = None
        for lift in ([0.0] if c.isalnum() else LIFTS):
            q = fit.Params(p.sigma, p.k, p.bl + lift, p.grow)
            for s in SHIFTS:
                e = blurred_error(fit.cell_image(g, q, cw, ch, oc + s), o)
                if best is None or e < best[0]:
                    best = (e, s, lift)
        rows.append((c, tt.getGlyphID(glyph), glyph, best[1], best[2], best[0]))
    em_y = int(round(target.cap * fit.R * 1.4)) * p.sigma / fit.R
    return {"font": font, "spec": spec, "cw": cw, "ch": ch, "file": path.name, "em_x": em_y * p.k,
            "em_y": em_y, "baseline": p.bl, "grow": p.grow,
            # A step of the fit's growth is a pixel of the evenly scaled render:
            # sigma eighths of a cell pixel down, sigma * k across.
            "pen_x": p.sigma * p.k, "pen_y": p.sigma, "rows": rows, "tags": tags}


def c_char(c):
    return "'\\''" if c == "'" else "'\\\\'" if c == "\\" else f"'{c}'"


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("fonts", nargs="+", help="GAME_FONT=TYPEFACE_FILE[+alt]")
    ap.add_argument("--disc", default="disc")
    ap.add_argument("--out", default="src/typeface_table.inc")
    ap.add_argument("--work", default="build/typeface", help="where the alternates' copies go")
    a = ap.parse_args()
    glyphs = pages.load_fonts(Path(a.disc))
    lines = ["// Generated by tools/typeface/make_table.py (tools/typeface/README.md): do not edit by hand.",
             "// " + " ".join(a.fonts),
             "// Per game font: the typeface, cell pixels per em across and down, the baseline's",
             "// row from the cell's top, the stroke growth in steps, and a step in eighths of a",
             "// cell pixel across and down; per character the glyph index drawn, its shift (cell",
             "// pixels) from the console glyph's ink centre and how far below the baseline it",
             "// sits (a sign put where the console's is)."]
    for arg in a.fonts:
        font, spec = arg.split("=", 1)
        t = table_for(font, spec, glyphs[font], Path(a.work))
        name = font.capitalize()
        lines += ["", f"// {font}: {t['file']} ({len(t['rows'])} glyphs; {' '.join(t['tags'])})",
                  f"constexpr TypefaceGlyph k{name}Glyphs[] = {{"]
        for c, gid, glyph, shift, lift, err in t["rows"]:
            lines.append(f"    {{{c_char(c)}, {gid}, {shift:.3f}f, {lift:.2f}f}}, // {glyph}, {err:.2f}")
        lines.append("};")
        lines.append(f"constexpr TypefaceFont k{name}Font{{\"{font}\", {t['cw']}, {t['ch']}, \"{t['file']}\", "
                     f"{t['em_x']:.4f}f, {t['em_y']:.4f}f, {t['baseline']:.4f}f, {t['grow']}, "
                     f"{t['pen_x']:.4f}f, {t['pen_y']:.4f}f, k{name}Glyphs, std::size(k{name}Glyphs)}};")
        print(f"{font}: {t['file']}, {len(t['rows'])} glyphs, em {t['em_x']:.3f} x {t['em_y']:.3f}, "
              f"baseline {t['baseline']}, grow {t['grow']}; {' '.join(t['tags'])}")
    Path(a.out).write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    print(a.out)


if __name__ == "__main__":
    main()
