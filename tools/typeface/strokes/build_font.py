"""The repository's font file from stroke_fit.py's assembly: only the
game font's characters, no hinting nor Andika's layout tables, named anew
(Andika's licence reserves "Andika" and "SIL"), the OFL kept. The message
font's is Rogue Letters; --font name makes the names' (Rogue Letters
Names; profiles.py).

    python -I tools/typeface/strokes/build_font.py [--font name] [--src build/typeface/strokes]
        [--dest thirdparty/fonts/rogue-letters]
"""
import argparse
import json
import sys
from pathlib import Path

from fontTools import subset
from fontTools.ttLib import TTFont

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
sys.path.insert(0, str(HERE))
from profiles import PROFILES  # noqa: E402

VERSION = "0.1"
OURS = "the Rogue Galaxy Recompiled contributors"


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--font", default="normal", choices=sorted(PROFILES), help="as stroke_fit.py's")
    ap.add_argument("--src", type=Path, default=None,
                    help="stroke_fit.py's output (Fitted-Regular.ttf, fit_report.json; default: the font's)")
    ap.add_argument("--dest", type=Path, default=ROOT / "thirdparty/fonts/rogue-letters")
    args = ap.parse_args()
    prof = PROFILES[args.font]
    FAMILY, PS_NAME = prof.family, prof.ps_name
    args.src = args.src or ROOT / "build/typeface/strokes" / prof.folder
    src = args.src / "Fitted-Regular.ttf"
    report = json.loads((args.src / "fit_report.json").read_text(encoding="utf-8"))
    tt = TTFont(str(src), recalcTimestamp=False)
    cmap = {}
    for table in tt["cmap"].tables:
        if table.isUnicode():
            cmap = table.cmap
            break
    # The curly quotes the table weighs against the straight ones (make_table's
    # CURLY) are ours too, so it never picks Andika's.
    extra = {0x2019: cmap[ord("'")], 0x201D: cmap[ord('"')]}
    for table in tt["cmap"].tables:
        if table.isUnicode():
            table.cmap.update(extra)
    unicodes = sorted({ord(c) for c in report} | set(extra))
    opts = subset.Options()
    opts.hinting = False
    opts.layout_features = []
    opts.name_IDs = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14]
    opts.drop_tables += ["Silt", "GDEF", "GPOS", "GSUB"]
    opts.glyph_names = True
    sub = subset.Subsetter(opts)
    sub.populate(unicodes=unicodes)
    sub.subset(tt)

    andika_copyright = "Copyright (c) 2004-2022 SIL International (http://www.sil.org/)"
    names = {
        0: f"{andika_copyright}, with Reserved Font Names \"Andika\" and \"SIL\". "
           f"Copyright (c) 2026, {OURS}.",
        1: FAMILY,
        2: "Regular",
        3: f"{FAMILY} Regular {VERSION}",
        4: f"{FAMILY} Regular",
        5: f"Version {VERSION}",
        6: PS_NAME,
        8: OURS,
        9: f"{OURS}, {prof.after}; from Andika "
           f"(Victor Gaultney, Annie Olsen, Julie Remington, Don Collingsworth, Eric Hays, Becca Hirsbrunner)",
        10: prof.what,
    }
    name = tt["name"]
    for rec in list(name.names):
        if rec.nameID in (7, 11, 12):  # Andika's trademark line and SIL's addresses
            name.names.remove(rec)
        elif rec.nameID in names:
            rec.string = names[rec.nameID]
    for rec in name.names:
        if rec.nameID == 13:
            text = rec.toUnicode()
            rec.string = text.replace(
                "Copyright (c) 2004-2022 SIL International (http://www.sil.org/) with Reserved Font Names "
                "\"Andika\" and \"SIL\".",
                names[0])
    tt["OS/2"].achVendID = "NONE"
    tt["head"].fontRevision = float(VERSION)
    tt["head"].modified = prof.modified
    args.dest.mkdir(parents=True, exist_ok=True)
    out = args.dest / f"{PS_NAME}.ttf"
    tt.save(str(out))
    # The licence goes with the font: Andika's, with our line added.
    ofl = (ROOT / "thirdparty/fonts/andika/OFL.txt").read_text(encoding="utf-8")
    head = ("Copyright (c) 2004-2022 SIL International  (http://www.sil.org/)\n"
            "with Reserved Font Names \"Andika\" and \"SIL\".\n")
    assert ofl.startswith(head), ofl[:200]
    ofl = head + f"Copyright (c) 2026, {OURS}\n(the Rogue Letters fonts, made from Andika).\n" + ofl[len(head):]
    (args.dest / "OFL.txt").write_text(ofl, encoding="utf-8", newline="\n")
    check = TTFont(str(out))
    print(out, out.stat().st_size, "bytes,", len(check.getGlyphOrder()), "glyphs,",
          len(check.getBestCmap()), "characters")


if __name__ == "__main__":
    main()
