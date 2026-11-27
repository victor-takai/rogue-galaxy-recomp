"""The game fonts the stroke tools draw (pages.FONTS), each into a typeface
of its own: the message font's, Rogue Letters, and the names', Rogue
Letters Names. stroke_fit.use() points the tools at one."""
from dataclasses import dataclass


@dataclass(frozen=True)
class Profile:
    skeletons: str  # the module of its skeletons (SKELETONS, LIMIT_FOR, PULL_FOR, GLYPH_FOR)
    table: str  # where the fit leaves its glyphs, beside the tools
    base: str  # make_table.py's fit of Andika Regular to it, beside the tools
    folder: str  # its files' folder under build/typeface/strokes ("" that folder itself)
    family: str
    ps_name: str  # the PostScript name, and the file's
    modified: int  # the font's modified date, fixed (seconds since 1904): the same table builds the same file
    after: str  # what its letters follow, for the font's description
    what: str  # the font's description
    # The fit's terms (stroke_fit.py's --fair, --smooth, --join) by default:
    # the message font's table was fitted before them, so its glyphs fit
    # again as their neighbours were.
    fair: float = 0.0
    smooth: float = 0.0
    join: float = 0.0


PROFILES = {
    "normal": Profile("skeletons", "strokes.json", "andika_fit.json", "", "Rogue Letters", "RogueLetters-Regular",
                      # the first build's, 2026-11-25 01:27:29
                      3878414849, "after the message font of the PlayStation 2 game",
                      "The letters of Rogue Galaxy Recompiled's message font, drawn as pen strokes"),
    "name": Profile("skeletons_name", "strokes_name.json", "andika_fit_name.json", "name", "Rogue Letters Names",
                    "RogueLettersNames-Regular",
                    # 2026-11-26 12:00:00
                    3878539200, "after the names' font of the PlayStation 2 game",
                    "The letters of Rogue Galaxy Recompiled's names font, drawn as pen strokes",
                    fair=1.0, smooth=0.2, join=5.0),
}
