"""A page to review a font drawn with the stroke tools, a round at a time:
every glyph numbered, in three pictures (the console's at 4x, ours as the
game draws it at 4x, ours big where its curves are judged), with the notes
of check_glyphs.py and a mark on the glyphs changed since the round before;
and names set in the console's letters and in ours, a dark ring around as
the game draws names. The glyphs are placed as preview_sheet.py places
them, so a mark can sit up to half a pixel off the game's.

Into build/typeface/strokes[/name]/review/round-N/index.html, opened in a
browser (the console's glyphs are in it: never into the repository). Run
stroke_fit.py --assemble and check_glyphs.py first.

    python -I tools/typeface/strokes/review_page.py [--font name] [--round N] [--names "Jaster,Kisala"]
"""
import argparse
import html
import json
import shutil
import sys
from pathlib import Path

from fontTools.ttLib import TTFont
from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import raster  # noqa: E402  (puts tools/typeface on the path)
import fit  # noqa: E402
import pages  # noqa: E402
import stroke_fit as sf  # noqa: E402
from preview_sheet import cell_at, tint  # noqa: E402

NAMES = ("Jaster Rogue,Kisala,Zegram Ghart,Simon,Steve,Lilika Rhyza,Jupis Tooki McGanagan,Deego Aldebaran,"
         "Canaan,Dorgengoa,Monica,Valkog,Captain Hawkins")
BG = (20, 28, 52)
# The page's words, in English and in Portuguese (--lang).
WORDS = {
    "en": {"html": "en", "round": "round", "how": "Each glyph: the console's at 4x, ours as the game draws it at 4x, "
           "and ours big. Answer with the numbers of the bad ones.",
           "checks": "Checks: {n} with notes", "changed": "Changed since round {r}: {c}.", "none": "none",
           "first": "First round.", "names": "Names (4x; the console's above, ours below; places approximate)",
           "glyphs": "Letters, digits and signs", "tag": "changed"},
    "pt": {"html": "pt-BR", "round": "rodada", "how": "Em cada letra: o console em 4x, a nossa como o jogo desenha "
           "em 4x, e a nossa grande. Responda com os números das que estão ruins.",
           "checks": "Verificação automática: {n} com aviso", "changed": "Mudaram desde a rodada {r}: {c}.",
           "none": "nenhuma", "first": "Primeira rodada.",
           "names": "Nomes (4x; console em cima, a nossa embaixo; posição aproximada)",
           "glyphs": "Letras, números e sinais", "tag": "mudou"},
}
# Cell pixels the game sets each letter of a font closer than its width in
# the executable's table: the names' letters stand 3 closer (Simon in a 4x
# capture of the console's: advances 8, 5, 13 and 9 for widths 12, 8, 16
# and 12, the S's within the half pixel the capture's edge measures).
TRACK = {"name": -3}
SCALE = 4  # the game's, as the user plays it
BIG = 170  # pixels a big glyph's em


def outline_bytes(tt, name):
    g = tt["glyf"][name]
    return g.compile(tt["glyf"]) if g.numberOfContours else b""


def big_glyph(font, c):
    w, h = int(BIG * 0.9), int(BIG * 1.2)
    im = Image.new("RGB", (w, h), BG)
    ImageDraw.Draw(im).text((w * 0.08, h * 0.86), c, fill=(255, 255, 255), font=font, anchor="ls")
    return im


def set_name(cells, widths, text, scale):
    """A name at the game's scale: each glyph's cell (alpha) at its advance,
    white with a dark ring a console pixel wide, on a plain ground."""
    cw, ch = next(iter(cells.values())).size
    total = sum(widths.get(c, 8) for c in text) * scale + cw + 2 * scale
    alpha = Image.new("L", (total, ch), 0)
    x = scale
    for c in text:
        if c in cells:
            one = Image.new("L", alpha.size, 0)
            one.paste(cells[c], (x, 0))
            alpha = ImageChops.lighter(alpha, one)
        x += widths.get(c, 8) * scale
    ring = alpha.filter(ImageFilter.MaxFilter(2 * scale + 1))
    out = Image.new("RGB", alpha.size, (96, 104, 120))
    out.paste((0, 0, 0), (0, 0), ring)
    out.paste((255, 255, 255), (0, 0), alpha)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--font", default="normal", choices=sorted(sf.PROFILES), help="as stroke_fit.py's")
    ap.add_argument("--round", type=int, default=None, help="default: the one after the last")
    ap.add_argument("--names", default=NAMES)
    ap.add_argument("--note", default="", help="a line on what changed this round")
    ap.add_argument("--lang", default="en", choices=sorted(WORDS), help="the page's language")
    a = ap.parse_args()
    sf.use(a.font)
    params = json.loads(sf.BASE.read_text(encoding="utf-8"))
    rows = {r["char"]: r for r in params["rows"]}
    report = json.loads((sf.OUT / "fit_report.json").read_text(encoding="utf-8"))
    checks_path = sf.OUT / "check.json"
    checks = json.loads(checks_path.read_text(encoding="utf-8")) if checks_path.exists() else {}
    src = sf.OUT / "Fitted-Regular.ttf"
    base = sf.OUT / "review"
    done = sorted(int(p.name[6:]) for p in base.glob("round-*") if p.name[6:].isdigit()) if base.exists() else []
    n = a.round if a.round is not None else ((done[-1] + 1) if done else 1)
    out = base / f"round-{n}"
    if out.exists():
        shutil.rmtree(out)
    (out / "img").mkdir(parents=True)
    shutil.copy(src, out / "font.ttf")
    tt = TTFont(str(src))
    prev = [r for r in done if r < n]
    old = TTFont(str(base / f"round-{prev[-1]}" / "font.ttf")) if prev else None

    fonts = pages.load_fonts(raster.ROOT / "disc")
    console = {g.char: g for g in fonts[a.font] if g.char}
    widths = {g.char: g.width + TRACK.get(a.font, 0) for g in fonts[a.font] if g.char}
    cw, ch = params["cw"], params["ch"]
    cap = console["H"].ink[3] - console["H"].ink[1] + 1
    size = int(round(cap * fit.R * 1.4))
    sigma = params["em_y"] * fit.R / size
    k = params["em_x"] / params["em_y"]
    bl = params["baseline"]
    # As the game grows it (preview_sheet.py's way).
    grow = sf.table_growth(a.font)
    grow = params.get("extra_grow", 0) if grow is None else grow
    face = fit.open_font(fit.Instance(str(src)), size)
    big = ImageFont.truetype(str(src), BIG)

    chars = [c for c in sf.SKELETONS if c in report]
    ours_cells, con_cells, cards = {}, {}, []
    for i, c in enumerate(chars, 1):
        g = console[c]
        oc = (g.ink[0] + g.ink[2] + 1) / 2 + rows[c]["shift"]
        hr = fit.grown(fit.render_glyph(face, c, 4 * fit.R, None), grow)
        mine = cell_at(hr, sigma, k, bl, cw, ch, oc, SCALE).point(lambda v: min(255, v))
        theirs = g.cell.resize((cw * SCALE, ch * SCALE), Image.NEAREST)
        ours_cells[c], con_cells[c] = mine, theirs
        bigim = big_glyph(big, c)
        pad = 10
        card = Image.new("RGB", (2 * cw * SCALE + bigim.width + 4 * pad, max(ch * SCALE, bigim.height) + 2 * pad),
                         (12, 16, 30))
        card.paste(tint(theirs), (pad, pad))
        card.paste(tint(mine), (2 * pad + cw * SCALE, pad))
        card.paste(bigim, (3 * pad + 2 * cw * SCALE, pad))
        fn = f"img/{i:02d}_{ord(c):04X}.png"
        card.save(out / fn)
        name = report[c]["glyph"]
        changed = old is None or name not in old["glyf"] or outline_bytes(old, name) != outline_bytes(tt, name)
        cards.append((i, c, fn, checks.get(c, []), changed and old is not None))

    specimen = []
    for j, text in enumerate(t.strip() for t in a.names.split(",") if t.strip()):
        fn_c, fn_o = f"img/name_{j:02d}_console.png", f"img/name_{j:02d}_ours.png"
        set_name(con_cells, widths, text, SCALE).save(out / fn_c)
        set_name(ours_cells, widths, text, SCALE).save(out / fn_o)
        specimen.append((text, fn_c, fn_o))

    flagged = [c for _, c, _, notes, _ in cards if notes]
    changed = [c for _, c, _, _, ch_ in cards if ch_]
    h = html.escape
    w = WORDS[a.lang]
    title = f"{h(sf.PROFILES[a.font].family)}: {w['round']} {n}"
    checks = w["checks"].format(n=len(flagged)) + ((": " + h("".join(flagged))) if flagged else "") + "."
    since = w["changed"].format(r=prev[-1], c=h("".join(changed)) or w["none"]) if prev else w["first"]
    parts = [f"""<!doctype html><html lang="{w['html']}"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>{title}</title>
<style>
body{{background:#0c101e;color:#dde;font:15px system-ui,sans-serif;margin:24px}}
h1{{font-size:22px;margin:0 0 4px}} h2{{font-size:17px;margin:28px 0 10px}}
.sub{{color:#99a;margin-bottom:6px}}
.names div{{margin:6px 0}} .names img{{display:block;margin:2px 0}}
.grid{{display:flex;flex-wrap:wrap;gap:12px}}
.card{{background:#141a2e;border:2px solid #232b45;border-radius:6px;padding:6px}}
.card.changed{{border-color:#d9a400}} .card.flag{{border-color:#d33}}
.head{{display:flex;gap:8px;align-items:baseline;margin-bottom:4px}}
.num{{font-weight:700;font-size:17px}} .ch{{font-size:17px;color:#fd8}}
.tag{{font-size:12px;padding:1px 6px;border-radius:9px;background:#d9a400;color:#000}}
.note{{color:#f88;font-size:12px;max-width:520px}}
</style></head><body>
<h1>{title}</h1>
<div class="sub">{h(w['how'])}</div>
<div class="sub">{checks}
{since}</div>
{('<div class="sub">' + h(a.note) + '</div>') if a.note else ''}
<h2>{h(w['names'])}</h2><div class="names">"""]
    for text, fc, fo in specimen:
        parts.append(f'<div><img src="{fc}" alt="{h(text)} console"><img src="{fo}" alt="{h(text)}"></div>')
    parts.append(f'</div><h2>{h(w["glyphs"])}</h2><div class="grid">')
    for i, c, fn, notes, ch_ in cards:
        cls = "card" + (" flag" if notes else "") + (" changed" if ch_ else "")
        tag = f'<span class="tag">{h(w["tag"])}</span>' if ch_ else ""
        note = f'<div class="note">{h("; ".join(notes))}</div>' if notes else ""
        parts.append(f'<div class="{cls}"><div class="head"><span class="num">{i}</span>'
                     f'<span class="ch">{h(c)}</span>{tag}</div><img src="{fn}" alt="{h(c)}">{note}</div>')
    parts.append("</div></body></html>")
    (out / "index.html").write_text("\n".join(parts), encoding="utf-8")
    print(out / "index.html")


if __name__ == "__main__":
    main()
