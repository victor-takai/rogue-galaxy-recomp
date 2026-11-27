"""Writes src/typeface_table.inc, what src/typeface_letters.cpp draws the
game's Latin letters from a typeface with: per game font the typeface's
size, baseline and stroke growth that bring it closest to the console's
glyphs, and per character the glyph drawn (the default one, one of the
typeface's alternates, or a curly quote) with its place. The same for the
menus' digits (pieces.py), with the hashes the game knows each one by.

usage (from the repository's root, with the disc extracted):
  python -I tools/typeface/make_table.py \\
      normal=thirdparty/fonts/rogue-letters/RogueLetters-Regular.ttf \\
      system=thirdparty/fonts/andika/Andika-Bold.ttf+alt \\
      name=thirdparty/fonts/rogue-letters/RogueLettersNames-Regular.ttf \\
      normal_eur=thirdparty/fonts/andika/Andika-Regular.ttf+alt \\
      system_eur=thirdparty/fonts/andika/Andika-Bold.ttf+alt \\
      name_eur=thirdparty/fonts/andika/Andika-Regular.ttf+alt \\
      menu=thirdparty/fonts/andika/Andika-Bold.ttf+alt \\
      menu_eur=thirdparty/fonts/andika/Andika-Bold.ttf+alt \\
      clock=thirdparty/fonts/andika/Andika-Bold.ttf+alt \\
      status=thirdparty/fonts/andika/Andika-Bold.ttf+alt \\
      statussmall=thirdparty/fonts/andika/Andika-Bold.ttf+alt \\
      level=thirdparty/fonts/andika/Andika-Bold.ttf+alt \\
      file=thirdparty/fonts/andika/Andika-Bold.ttf+alt \\
      money=thirdparty/fonts/andika/Andika-Bold.ttf+alt \\
      battle=thirdparty/fonts/andika/Andika-Bold.ttf+alt \\
      battlesmall=thirdparty/fonts/andika/Andika-Bold.ttf+alt \\
      damage=thirdparty/fonts/andika/Andika-Bold.ttf+alt \\
      result=thirdparty/fonts/andika/Andika-Bold.ttf+alt \\
      resultgold=thirdparty/fonts/andika/Andika-Bold.ttf+alt \\
      registered=thirdparty/fonts/andika/Andika-Bold.ttf+alt \\
      words=thirdparty/fonts/andika/Andika-Bold.ttf+alt
  [--disc disc] [--out src/typeface_table.inc] [--work build/typeface]

`words=` fits each word of the menus' pictures (pieces.WORDS) whole, with
the menu font's glyph choices when `menu=` comes before it.

`+alt` lets each character take one of the typeface's alternates where it
is closer to the console's. Needs Pillow and fontTools (pip install pillow
fonttools). The console glyphs are read from the disc into memory; only
numbers, hashes and glyph indices are written.
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
import pieces  # noqa: E402

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
# The console's times sign is a large x: the typeface's x, raised, is a
# candidate for it (Andika's times sign is half the console's height).
STAND_INS = {"×": [ord("x")]}
# Alternates that are another kind of glyph, not another drawing of the same
# character: small capitals, superiors and inferiors, fractions, ordinals,
# full-width forms, a letter with a mark.
NOT_A_DRAWING = ("uniFF", ".sc", "smcp", "c2sc", ".sups", "sinf", ".subs", "numr", "dnom", "ordn",
                 "superior", "inferior", ".sinf", ".supr", "0301", "0300", "0308")
# Drawings a word's letter never takes: the tailed i (it read as a j upside
# down in Allies; the pictures' i is straight, and no font took it).
NOT_FOR_WORDS = (".TailI",)
# Steps of growth over a font's fit: the message font drawn bolder than it
# fits at 1x. Enlarged, the console's glyph shows its soft edge texels as
# whole blocks and reads fuller; +2 matched it in the dialogue (picked by eye
# against the console, 2026-11-23).
EXTRA_GROW = {"normal": 2, "normal_eur": 2}


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
    # A picture's row may hold a small capital (the clock's row's P, after
    # the Battle Recorder's points, its top a pixel or more below the 0's):
    # the typeface's small capitals are candidates for such a letter, and
    # only for it (the menus' narrow W took the small w).
    small_caps = tuple(k for k in NOT_A_DRAWING if k not in (".sc", "smcp", "c2sc"))
    top = target.by_char[target.ref].ink[1]
    for c in chars:
        o = target.by_char[c]
        excluded = (small_caps if target.quant is fit.SMOOTH and c.isupper() and o.ink[1] >= top + 1
                    else NOT_A_DRAWING)
        options = [(c, None)] + [(chr(cp), f"{glyph} ({tag})") for cp, glyph, tag in table.get(c, [])
                                 if not any(k in glyph for k in excluded)]
        if len(options) == 1:
            continue
        o = target.by_char[c]
        oc = (o.ink[0] + o.ink[2] + 1) / 2
        best = None
        # A symbol's candidates are weighed each at its best height (the x
        # standing in for the times sign sits raised); the rest on the line.
        lifts = LIFTS if c in pages.SYMBOLS.values() else [0.0]
        for drawn, name in options:
            g = fit.grown(fit.render_glyph(font, drawn, 4 * fit.R, notdef), p.grow)
            if g.missing:
                continue
            for lift in lifts:
                q = fit.Params(p.sigma, p.k, p.bl + lift, p.grow, p.slant)
                for s in (-0.5, -0.25, 0, 0.25, 0.5):
                    e = blurred_error(fit.cell_image(g, q, target.cw, target.ch, oc + s, target.quant), o)
                    if best is None or e < best[0] - 0.005:
                        best = (e, drawn, name)
        if best and best[2]:
            chosen[c] = best[1]
            tags.append(f"{c}={best[2]}")
    return chosen


def table_for(font, spec, glyphs, work: Path, target=None, placed=""):
    """A font's (or a set of pieces') fit and rows. `placed`: its signs that
    are only placed, without weighing in the fit (a set's added signs)."""
    use_alt = spec.endswith("+alt")
    path = Path(spec[:-4] if use_alt else spec)
    target = target or fit.Target(font, glyphs)
    cw, ch = target.cw, target.ch
    for o in target.glyphs:
        o.blur = o.cell.filter(BLUR)
        o.blur_sum = ImageStat.Stat(o.blur).sum[0]
    chars = sorted(target.by_char)
    inst = fit.Instance(str(path))
    # The font is fitted on its ASCII characters; the game's symbols (times,
    # plus-minus...) and a European font's accents and marks are only placed,
    # so the letters stay as they were without them. A set of symbols alone
    # (the registered mark) is fitted on them.
    fitted = [c for c in chars if c.isascii() and c not in placed] or [c for c in chars if c not in placed]
    _, p = fit.fit(inst, target, fitted)
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
    for tag, extra in (("curly", CURLY), ("stand-in", STAND_INS)):
        for c, cps in extra.items():
            for cp in cps:
                if cp in cmap and c in chars:
                    table = {**table, c: list(table.get(c, [])) + [[cp, cmap[cp], tag]]}
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
        lifts = [0.0] if c.isalnum() else LIFTS
        gap = 0.0
        if not c.isalnum() and target.quant is fit.SMOOTH:
            # A sign among a picture's digits (the clock's colon) sits centred
            # on the console's: its dots, a pixel each and farther apart than
            # the typeface's, match nowhere, and the least error would push
            # the sign out of the cell. A colon's dots are moved apart to the
            # console's height (the gap the game's code opens between them).
            drawn = fit.cell_image(g, p, cw, ch, oc, target.quant).point(lambda v: 255 if v >= 64 else 0).getbbox()
            if drawn:
                lifts = [round(((o.ink[1] + o.ink[3]) - (drawn[1] + drawn[3] - 1)) / 2 * 4) / 4]
                if c in ":;":
                    gap = max(0.0, round(((o.ink[3] - o.ink[1] + 1) - (drawn[3] - drawn[1])) * 4) / 4)
        for lift in lifts:
            q = fit.Params(p.sigma, p.k, p.bl + lift, p.grow, p.slant)
            for s in SHIFTS:
                e = blurred_error(fit.cell_image(g, q, cw, ch, oc + s, target.quant), o)
                if best is None or e < best[0]:
                    best = (e, s, lift)
        rows.append((c, tt.getGlyphID(glyph), glyph, best[1], best[2], best[0], gap))
    em_y = int(round(target.cap * fit.R * 1.4)) * p.sigma / fit.R
    return {"font": font, "spec": spec, "cw": cw, "ch": ch, "file": path.name, "em_x": em_y * p.k,
            "em_y": em_y, "baseline": p.bl, "grow": p.grow,
            # A step of the fit's growth is a pixel of the evenly scaled render:
            # sigma eighths of a cell pixel down, sigma * k across.
            "pen_x": p.sigma * p.k, "pen_y": p.sigma,
            # The lean in cell pixels across per cell pixel up (fit.Params).
            "slant": p.slant * p.k, "rows": rows, "tags": tags, "target": target,
            "chosen": chosen, "names": names, "render_inst": render_inst, "tt": tt, "table": table}


def word_table(word, path: Path, menu):
    """A word of the menus' pictures (pieces.WORDS) fitted whole: the
    typeface's size, horizontal scale, baseline and stroke growth that bring
    the word, drawn with the menus' font's choice of glyphs (`menu`, its
    table_for result, or None), closest to the console word's core (fitted
    again when a letter then takes another drawing); its place, and each
    glyph with its pen position from the word's start."""
    o = word.glyph
    target = fit.Target(word.text, [o], cell=word.box[2:], ref=word.text, quant=fit.SMOOTH, italic=word.italic,
                        grow=word.grow, descends=True, k=word.k, hold_size=word.spaced)
    o.blur = o.cell.filter(BLUR)
    o.blur_sum = ImageStat.Stat(o.blur).sum[0]
    chosen = dict(menu["chosen"]) if menu else {}
    names = menu["names"] if menu else {}
    render_inst = menu["render_inst"] if menu else fit.Instance(str(path))
    tt = menu["tt"] if menu else TTFont(str(path))
    cmap = tt.getBestCmap()
    if word.small_caps:
        # A small-capital word's lower-case letters as the typeface's small
        # capitals: each letter's alternate that is one (the alternates copy
        # holds them at private code points; Andika's single-storey a has
        # a.SngStory.sc).
        table = menu["table"] if menu else {}
        for c in set(word.text):
            if not c.islower():
                continue
            small = [cp for cp, glyph, _ in table.get(c, []) if glyph.endswith(".sc")]
            if not small:
                raise SystemExit(f"{word.text!r}: no small capital for {c!r}")
            chosen[c] = chr(small[0])
    for c, glyph_name in word.alternates.items():
        # A drawing the list names for a character: an alternate (the names'
        # J), or the default one where the menu font took an alternate (the
        # story title's y with its tail).
        if glyph_name == cmap.get(ord(c)):
            chosen.pop(c, None)
            continue
        named = [cp for cp, glyph, _ in (menu["table"] if menu else {}).get(c, []) if glyph == glyph_name]
        if not named:
            raise SystemExit(f"{word.text!r}: no alternate {glyph_name} for {c!r}")
        chosen[c] = chr(named[0])
    def fitted(text):
        """The fit of the word drawn as `text`, with the alternates copy (it
        holds the menu font's choices, the small capitals and the named
        drawings at private code points)."""
        target.by_char = {text: o}
        target.ref = text
        _, q = fit.fit(render_inst, target, [text])
        if q is None:
            raise SystemExit(f"{word.text!r} cannot be drawn")
        return q

    drawn = "".join(chosen.get(c, c) for c in word.text)
    p = fitted(drawn)
    size = int(round(target.cap * fit.R * 1.4))
    oc = (o.ink[0] + o.ink[2] + 1) / 2
    if word.spaced:
        # Sized and placed on its first letter (its letters stand apart, and
        # the console's descenders are shorter than the typeface's, so the
        # whole word's height would shrink it): that letter's height and
        # bottom (the baseline) the console's.
        inked = [x for x in range(o.cell.width) if any(o.cell.getpixel((x, y)) >= 64 for y in range(o.cell.height))]
        end = inked[0]
        while end + 1 in inked:
            end += 1
        rows = [y for y in range(o.cell.height) if any(o.cell.getpixel((x, y)) >= 64 for x in range(inked[0], end + 1))]
        g0 = fit.render_set(render_inst, target, [drawn[0]])[drawn[0]]
        sigma = (rows[-1] + 1 - rows[0]) * fit.R / max(1, g0.ink[3] - g0.ink[1])
        p = fit.Params(sigma, p.k, float(rows[-1] + 1), p.grow, p.slant)

    def placed(text):
        """(error, shift) of the word drawn as `text` at its best place."""
        g = fit.grown(fit.render_set(render_inst, target, [word.text], {word.text: text})[word.text], p.grow)
        return min((blurred_error(fit.cell_image(g, p, target.cw, target.ch, oc + s, target.quant), o), s)
                   for s in SHIFTS)

    best = placed(drawn)
    first = drawn
    # Each letter may take another of the typeface's drawings where the word
    # comes closer (by 0.005): an alternate (a name's J with its bar on top),
    # or its default drawing where the menu font took an alternate (the y
    # with its tail, closer to some words' y than the menu font's straight one).
    table = menu["table"] if menu else {}
    # (Not for a spaced word: its letters stand elsewhere than the drawing
    # weighed, which would pick another drawing than its plain twin's.)
    for c in sorted(set(word.text)) if not word.spaced else []:
        if c == " " or (word.small_caps and c.islower()) or c in word.alternates:
            continue
        default = [(ord(c), cmap[ord(c)], "default")] if c in chosen and ord(c) in cmap else []
        for cp, glyph, tag in default + table.get(c, []):
            if any(k in glyph for k in NOT_A_DRAWING + NOT_FOR_WORDS):
                continue
            trial = "".join(chr(cp) if ch == c else chosen.get(ch, ch) for ch in word.text)
            e = placed(trial)
            if e[0] < best[0] - 0.005:
                best = e
                if cp == ord(c):
                    del chosen[c]
                else:
                    chosen[c] = chr(cp)
        drawn = "".join(chosen.get(ch, ch) for ch in word.text)
    if drawn != first:
        # The fit again, on the letters taken.
        p = fitted(drawn)
        best = placed(drawn)
    font = fit.open_font(render_inst, size)
    # Render pixels to cell pixels across (fit.cell_image's sx).
    to_cell = p.sigma * p.k / fit.R
    glyphs = []
    for i, c in enumerate(word.text):
        if c == " ":
            continue
        name = names[chosen[c]] if c in chosen else cmap.get(ord(c))
        if name is None:
            raise SystemExit(f"{word.text!r}: no glyph for {c!r}")
        glyphs.append((tt.getGlyphID(name), font.getlength(drawn[:i]) * to_cell, c))
    if word.spaced and len(glyphs) > 1:
        # Letters standing far apart (the framed Player): the pens spread
        # evenly until the word is as wide as the console's, centred on it.
        g = fit.render_set(render_inst, target, [word.text], {word.text: drawn})[word.text]
        natural = (g.ink[2] - g.ink[0]) * to_cell
        step = ((o.ink[2] - o.ink[0] + 1) - natural) / (len(glyphs) - 1)
        glyphs = [(gid, pen + n * step, c) for n, (gid, pen, c) in enumerate(glyphs)]
        best = (best[0], 0.0)
    em_y = size * p.sigma / fit.R
    return {"word": word, "em_x": em_y * p.k, "em_y": em_y, "baseline": p.bl, "grow": p.grow,
            "pen_x": p.sigma * p.k, "pen_y": p.sigma, "slant": p.slant * p.k, "shift": best[1],
            "error": best[0], "glyphs": glyphs, "file": path.name,
            "drawings": sorted({names[chosen[c]] for c in word.text if c in chosen})}


def c_char(c):
    """The C character for c; 0 for a symbol beyond ASCII (found by its code)."""
    if not c.isascii():
        return "0"
    return "'\\''" if c == "'" else "'\\\\'" if c == "\\" else f"'{c}'"


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("fonts", nargs="+", help="GAME_FONT=TYPEFACE_FILE[+alt]")
    ap.add_argument("--disc", default="disc")
    ap.add_argument("--out", default="src/typeface_table.inc")
    ap.add_argument("--work", default="build/typeface", help="where the alternates' copies go")
    ap.add_argument("--eur-lang", default="FR", help="the European language whose pack holds the EUR pages")
    a = ap.parse_args()
    asked = [arg.split("=", 1)[0] for arg in a.fonts]
    glyphs = pages.load_fonts(Path(a.disc)) if any(f in pages.FONTS for f in asked) else {}
    if any(f in pages.EUR_FONTS for f in asked):
        glyphs.update(pages.load_fonts_eur(Path(a.disc), a.eur_lang))
    asked_sets = [f for f in asked if f in pieces.SETS]
    sets = pieces.load_pieces(Path(a.disc), asked_sets) if asked_sets else {}
    # The European text's copies of the sets' pictures: the same cells in
    # palettes of their own, each language's, so more hashes of the same glyphs.
    eur_sets = {}
    eur_asked = [s for s in asked_sets if pieces.SETS[s].text != "en"]
    if eur_asked:
        if not (Path(a.disc) / "EU").is_dir():
            # Without them the table would lose the European text's digits.
            raise SystemExit(f"{Path(a.disc) / 'EU'} is missing: the European copies of the menus' pictures "
                             f"come from it (python setup.py extract-european)")
        eur_sets = {lang: pieces.load_pieces(Path(a.disc), eur_asked, lang) for lang in pieces.EUR_LANGS}
    extra_palettes, eur_palettes = set(), set()
    lines = ["// Generated by tools/typeface/make_table.py (tools/typeface/README.md): do not edit by hand.",
             "// " + " ".join(a.fonts),
             "// Per game font: the typeface, cell pixels per em across and down, the baseline's",
             "// row from the cell's top, the stroke growth in steps, a step in eighths of a cell",
             "// pixel across and down, and the lean (cell pixels across per pixel up); per",
             "// character the glyph index drawn, its shift (cell pixels) from the console glyph's",
             "// ink centre and how far below the baseline it sits (a sign put where the",
             "// console's is)."]
    piece_fonts, piece_rows = [], []
    menu, words_spec = None, None
    for arg in a.fonts:
        font, spec = arg.split("=", 1)
        if font == "words":
            words_spec = spec
            continue
        ps = sets.get(font)
        if ps:
            target = fit.Target(font, ps.glyphs, cell=(ps.spec.cw, ps.spec.ch), ref=ps.spec.ref, quant=fit.SMOOTH,
                                italic=ps.spec.italic)
            t = table_for(font, spec, ps.glyphs, Path(a.work), target, placed=ps.spec.signs)
        elif font in pages.EUR_FONTS and font in glyphs:
            # A European font: its own page's cell (the names' 16x22) and
            # alpha levels.
            target = fit.Target(font, glyphs[font], cell=pages.EUR_FONTS[font][1:3], quant=pages.EUR_QUANT[font])
            t = table_for(font, spec, glyphs[font], Path(a.work), target)
        elif font in glyphs:
            t = table_for(font, spec, glyphs[font], Path(a.work))
        else:
            raise SystemExit(f"{font}: not a game font ({', '.join([*pages.FONTS, *pages.EUR_FONTS])}) nor a set "
                             f"of digits ({', '.join(pieces.SETS)})")
        t["grow"] += EXTRA_GROW.get(font, 0)
        # A European font's glyphs are found by their code (its page's map,
        # not the US width table's characters): every row holds it.
        by_code = font in pages.EUR_FONTS
        name = "".join(part.capitalize() for part in font.split("_"))
        tags = f"; {' '.join(t['tags'])}" if t["tags"] else ""
        lines += ["", f"// {font}: {t['file']} ({len(t['rows'])} glyphs{tags})",
                  f"constexpr TypefaceGlyph k{name}Glyphs[] = {{"]
        for c, gid, glyph, shift, lift, err, gap in t["rows"]:
            extra = f", {gap:.2f}f" if gap or not c.isascii() or by_code else ""
            if not c.isascii() or by_code:
                # A symbol (pages.SYMBOLS) or a European font's glyph: the game's code for it.
                extra += f", {t['target'].by_char[c].code}"
            lines.append(f"    {{{c_char(c)}, {gid}, {shift:.3f}f, {lift:.2f}f{extra}}}, // {glyph}, {err:.2f}")
        lines.append("};")
        lines.append(f"constexpr TypefaceFont k{name}Font{{\"{font}\", {t['cw']}, {t['ch']}, \"{t['file']}\", "
                     f"{t['em_x']:.4f}f, {t['em_y']:.4f}f, {t['baseline']:.4f}f, {t['grow']}, "
                     f"{t['pen_x']:.4f}f, {t['pen_y']:.4f}f, {t['slant']:.4f}f, k{name}Glyphs, "
                     f"std::size(k{name}Glyphs){', true' if by_code else ''}}};")
        print(f"{font}: {t['file']}, {len(t['rows'])} glyphs, em {t['em_x']:.3f} x {t['em_y']:.3f}, "
              f"baseline {t['baseline']}, grow {t['grow']}, slant {t['slant']:.3f}{tags}")
        if font == "menu":
            menu = t
        if ps:
            row_of = {c: i for i, (c, *_rest) in enumerate(t["rows"])}
            # The set's own picture, then each European language's copy (its
            # glyphs' ink as that copy has it).
            copies = [ps] + [lang_sets[font] for lang_sets in eur_sets.values() if font in lang_sets]
            eur_palettes |= {copy.palette for copy in copies[1:]}
            for copy in copies:
                inks = {g.char: g.ink for g in copy.glyphs}
                for c, variants in copy.variants.items():
                    if c not in row_of or not inks.get(c):
                        continue
                    x0, _, x1, y1 = inks[c]
                    for w, h, value, ox, oy in variants:
                        # The console's ink in the sprite drawn (its corner ox, oy
                        # in the cell), and the row it starts at; a sprite cutting
                        # into the ink is no sprite the game draws.
                        if x0 < ox or y1 < oy:
                            continue
                        piece_rows.append((value, len(piece_fonts), row_of[c], w, h, x0 - ox, x1 + 1 - ox,
                                           y1 + 1 - oy, oy, font, c))
                extra_palettes.add(copy.palette)
            piece_fonts.append((name, ps.palette))
    words = []
    if words_spec:
        path = Path(words_spec[:-4] if words_spec.endswith("+alt") else words_spec)
        if menu and path.name != menu["file"]:
            # The words take the menu font's glyph choices and indices.
            raise SystemExit(f"words={path.name}: the words are drawn with the menu font's typeface "
                             f"({menu['file']})")
        for n, word in enumerate(pieces.load_words(Path(a.disc))):
            w = word_table(word, path, menu)
            words.append(w)
            lines += ["", f"// word {word.text!r} ({word.picture}, error {w['error']:.2f})",
                      f"constexpr WordGlyph kWord{n}Glyphs[] = {{"
                      + ", ".join(f"{{{gid}, {pen:.3f}f}}" for gid, pen, _ in w["glyphs"]) + "};",
                      f"constexpr TypefaceFont kWord{n}Font{{\"{word.text}\", {word.box[2]}, {word.box[3]}, "
                      f"\"{w['file']}\", {w['em_x']:.4f}f, {w['em_y']:.4f}f, {w['baseline']:.4f}f, {w['grow']}, "
                      f"{w['pen_x']:.4f}f, {w['pen_y']:.4f}f, {w['slant']:.4f}f, nullptr, 0}};"]
            print(f"word {word.text!r}: em {w['em_x']:.2f} x {w['em_y']:.2f}, grow {w['grow']}, "
                  f"error {w['error']:.2f}; {' '.join(w['drawings']) or 'default glyphs'}")
    # The game takes a picture's words by its palette: a European picture
    # with an English one's palette could have an English word redrawn among
    # its own, which stay the game's (half redrawn).
    eur_pictures = {p: "a European copy of a digit set's picture" for p in eur_palettes}
    if words and (Path(a.disc) / "EU").is_dir():
        for lang in pieces.EUR_LANGS:
            eur_pictures.update(pieces.word_picture_palettes(Path(a.disc), lang))
    shared = sorted(set(eur_pictures) & {w["word"].palette for w in words})
    if shared:
        raise SystemExit("a European picture has an English word's palette: "
                         + ", ".join(f"{p:016X} ({eur_pictures[p]})" for p in shared))
    if piece_fonts or words:
        palettes = sorted({p for _, p in piece_fonts} | extra_palettes | {w["word"].palette for w in words})
        piece_rows = sorted(set(piece_rows))
        lines += ["", "// The menus' digits and font (tools/typeface/pieces.py): the palettes of their",
                  "// pictures as dq8-gfx hashes them, the sets' fonts, and each digit or letter by the",
                  "// hash of its texels at each size a draw samples (sorted): its set, its row in the",
                  "// set's glyphs, the size, and the console glyph's ink (first and past-the-last",
                  "// column, the row past its lowest) in the sprite drawn, and the row of the cell a",
                  "// sprite starting inside it starts at.",
                  "constexpr uint64_t kPiecePalettes[] = {" + ", ".join(f"0x{p:016X}ull" for p in palettes) + "};",
                  "constexpr const TypefaceFont *kPieceFonts[] = {"
                  + (", ".join(f"&k{name}Font" for name, _ in piece_fonts) or "nullptr") + "};",
                  "constexpr Piece kPieces[] = {"]
        # The game finds a piece by its texels' hash and size alone: two
        # characters with the same ones would be drawn as one of them.
        seen = {}
        for value, s, row, w, h, left, right, bottom, top, font, c in piece_rows:
            other = seen.setdefault((value, w, h), (font, c))
            if other[1] != c:
                raise SystemExit(f"{font} {c!r} and {other[0]} {other[1]!r} at {w}x{h}: the same texels")
        for value, s, row, w, h, left, right, bottom, top, font, c in sorted(piece_rows):
            extra = f", {top}" if top else ""
            lines.append(f"    {{0x{value:016X}ull, {s}, {row}, {w}, {h}, {left}, {right}, {bottom}{extra}}}, "
                         f"// {font} {c_char(c)} {w}x{h}")
        if not piece_rows:
            lines.append("    {0u, 0, 0, 0, 0, 0, 0, 0},")
        lines.append("};")
    else:
        lines += ["", "constexpr uint64_t kPiecePalettes[] = {0u};",
                  "constexpr const TypefaceFont *kPieceFonts[] = {nullptr};",
                  "constexpr Piece kPieces[] = {{0u, 0, 0, 0, 0, 0, 0, 0}};"]
    lines += ["", "// The menus' words (tools/typeface/pieces.py WORDS): each by its picture's palette",
              "// and the hash of its box's texels, the box's place and size in the picture, the",
              "// console word's ink in the box, its shift from the console's ink centre, its fit",
              "// (above) and its glyphs; a shadowed word its shadow's offset (cell pixels across",
              "// and down), blur (sigma, cell pixels) and strength; a word on a button 1 last, on",
              "// a band 2, on art 3.",
              "constexpr PieceWord kWords[] = {"]
    for n, w in enumerate(words):
        word = w["word"]
        x0, _, x1, y1 = word.glyph.ink
        shadow = ""
        if word.shadow:
            dx, dy, sigma, strength = word.shadow
            shadow = f", {dx}, {dy}, {sigma:.2f}f, {strength:.3f}f"
        if word.button or word.bar or word.art:
            # On a button, a band or art: no shadow, and its backing (PieceWord).
            shadow = (shadow or ", 0, 0, 0.00f, 0.000f") + (", 3" if word.art else ", 2" if word.bar else ", 1")
        lines.append(f"    {{0x{word.palette:016X}ull, 0x{word.hash:016X}ull, {word.box[0]}, {word.box[1]}, "
                     f"{word.box[2]}, {word.box[3]}, {x0}, {x1 + 1}, {y1 + 1}, {w['shift']:.3f}f, &kWord{n}Font, "
                     f"kWord{n}Glyphs, std::size(kWord{n}Glyphs){shadow}}}, // {word.text}")
    if not words:
        lines.append("    {0u, 0u, 0, 0, 0, 0, 0, 0, 0, 0.0f, nullptr, nullptr, 0},")
    lines.append("};")
    Path(a.out).write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    print(a.out)


if __name__ == "__main__":
    main()
