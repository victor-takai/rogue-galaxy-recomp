"""A typeface's alternate glyphs for the game's characters (OpenType
character variants cvNN, stylistic sets ssNN, salt and aalt), made drawable
without a shaping engine: a copy of the font maps each alternate to a
private-use code point.
"""
import re
from pathlib import Path

from fontTools.ttLib import TTFont

CHARS = [chr(c) for c in range(0x21, 0x7F)]
FEATURE = re.compile(r"^(cv\d\d|ss\d\d|salt|aalt)$")


def alternates_of(font) -> dict:
    """{base glyph: {alternate glyph: feature tag}} from single and alternate
    substitutions."""
    if "GSUB" not in font:
        return {}
    gsub = font["GSUB"].table
    if not gsub.FeatureList or not gsub.LookupList:
        return {}
    out = {}
    for rec in gsub.FeatureList.FeatureRecord:
        if not FEATURE.match(rec.FeatureTag):
            continue
        for li in rec.Feature.LookupListIndex:
            lookup = gsub.LookupList.Lookup[li]
            for st in lookup.SubTable:
                kind = lookup.LookupType
                if kind == 7:
                    kind, st = st.ExtensionLookupType, st.ExtSubTable
                if kind == 1:
                    for base, alt in st.mapping.items():
                        out.setdefault(base, {}).setdefault(alt, rec.FeatureTag)
                elif kind == 3:
                    for base, alts in st.alternates.items():
                        for alt in alts:
                            out.setdefault(base, {}).setdefault(alt, rec.FeatureTag)
    return out


def drawable_copy(path: Path, dest: Path) -> dict:
    """Writes `dest`, the font with each alternate of a game character at a
    private-use code point from U+E000; returns {char: [[code point, glyph,
    tag], ...]} (empty, and nothing written, when it has none)."""
    font = TTFont(str(path))
    cmap = font.getBestCmap() or {}
    alts = alternates_of(font)
    used, pua, table, mapping = set(cmap), 0xE000, {}, {}
    for ch in CHARS:
        base = cmap.get(ord(ch))
        for glyph, tag in (alts.get(base, {}).items() if base else []):
            if glyph == base:
                continue
            while pua in used:
                pua += 1
            used.add(pua)
            mapping[pua] = glyph
            table.setdefault(ch, []).append([pua, glyph, tag])
    if not table:
        return {}
    for sub in font["cmap"].tables:
        if sub.isUnicode() and sub.format in (4, 12):
            sub.cmap.update(mapping)
    dest.parent.mkdir(parents=True, exist_ok=True)
    font.save(str(dest))
    return table
