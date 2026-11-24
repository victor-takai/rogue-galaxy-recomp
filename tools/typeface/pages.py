"""The game's three Latin fonts, read from the extracted disc: each glyph's
cell (its alpha), character, width and ink box.

The pages are FontTex_<font>_EN_0.tm2 in meswin\\fontset_main_EN.pak (4-bit
TIM2, white over 16 alpha levels 0-0x80); glyph code n is cell n, row by row.
The characters and widths are the executable's table at 0x44E850, 4 bytes a
code: the character, then its width in the name, message and system fonts.
Everything is read into memory: these are the game's data, never written
into the repository.
"""
import struct
import sys
from dataclasses import dataclass
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "disc"))
import european  # noqa: E402

FONT_PACK = "meswin\\fontset_main_en.pak"
# The game's symbols (the width table names them 0) by their code, as the
# characters a typeface draws them by: plus-minus, times, middle dot and
# reference mark. Its stars, circles, squares and triangles stay out (Andika
# has none), and its arrows (Andika's are hairlines beside the console's bold
# ones).
SYMBOLS = {93: "±", 94: "×", 98: "·", 105: "※"}
# game font -> (its first page, cell width, cell height, its column of widths)
FONTS = {
    "name": ("FontTex_name_EN_0.tm2", 16, 18, 0),
    "normal": ("FontTex_normal_EN_0.tm2", 18, 22, 1),
    "system": ("FontTex_system_EN_0.tm2", 16, 20, 2),
}

# The European text's pages (src/text_language.cpp asks for them): one pack for
# the four languages, meswin\fontset_main_<lang>.pak in disc/EU/<LANG>.DAT. The
# glyphs are redrawn (other alpha ramps), the names' rows 22 apart. Codes 0-95
# are the US pages' (the US width table's characters, the symbols at 89-94),
# 96-106 the symbols in another order (a filled circle came in; the middle
# dot is at 100, the reference mark at 106), 107-170 the accents and marks of
# text_language.cpp's kAccentTags, in its order.
EUR_FONTS = {
    "name_eur": ("FontTex_name_EUR_0.tm2", 16, 22, 0),
    "normal_eur": ("FontTex_normal_EUR_0.tm2", 18, 22, 1),
    "system_eur": ("FontTex_system_EUR_0.tm2", 16, 20, 2),
}
# (The reference mark at 106 stays out: the US width table, which the game
# reads below the accents, ends before it.)
EUR_SYMBOLS = {93: "±", 94: "×", 100: "·"}
EUR_ACCENT_FIRST = 107
EUR_ACCENTS = ("’‘“”„–—―¡¿«»°Œœ"
               "ÀÁÂÄÇÈÉÊËÌÍÎÏÑÒÓÔÖÙÚÛÜàáâäçèéêëìíîïñòóôöÕõùúûüßªº")
# Their widths in the name, message and system fonts (kAccentWidths; the name
# font's a pixel less, as text_language.cpp answers them).
EUR_ACCENT_WIDTHS = (
    (6, 6, 4), (6, 6, 4), (6, 6, 6), (6, 6, 6), (6, 6, 6), (10, 10, 8), (14, 12, 12), (16, 16, 14), (6, 4, 4),
    (12, 10, 8), (12, 10, 8), (12, 10, 8), (6, 6, 6), (16, 16, 12), (16, 16, 12), (14, 16, 12), (14, 16, 12),
    (14, 16, 12), (14, 16, 12), (14, 14, 10), (12, 12, 8), (12, 12, 8), (12, 12, 8), (12, 12, 8), (10, 8, 8),
    (10, 8, 8), (12, 10, 8), (12, 10, 8), (12, 12, 10), (14, 14, 10), (14, 14, 10), (14, 14, 10), (14, 14, 10),
    (14, 12, 10), (14, 12, 10), (14, 12, 10), (14, 12, 10), (12, 10, 8), (12, 10, 8), (12, 10, 8), (12, 10, 8),
    (12, 10, 8), (12, 10, 8), (12, 10, 8), (12, 10, 8), (12, 10, 8), (10, 8, 8), (10, 8, 8), (12, 10, 8),
    (12, 10, 8), (12, 10, 8), (12, 10, 8), (12, 10, 8), (12, 10, 8), (12, 10, 8), (14, 14, 10), (12, 10, 8),
    (12, 10, 8), (12, 10, 8), (12, 10, 8), (12, 10, 8), (12, 10, 8), (10, 10, 8), (10, 10, 8))
assert len(EUR_ACCENTS) == len(EUR_ACCENT_WIDTHS) == 64


@dataclass
class Glyph:
    code: int
    char: str | None  # a symbol's from SYMBOLS; None for the others (the table names them 0)
    width: int  # the advance the game lays text out with
    cell: Image.Image  # "L", the alpha doubled (the game's 0x80 opaque is 256, clamped to 255)
    ink: tuple | None  # (x0, y0, x1, y1) inclusive, or None when empty


def tim2_4bit(d: bytes):
    """(width, height, indices, alpha of the 16 colours) of a 4-bit TIM2."""
    at = 0x80 if d[5] == 1 else 0x10  # pictures 128-byte aligned, or 16
    _total, clut_size, image_size, head = struct.unpack_from("<IIIH", d, at)
    w, h = struct.unpack_from("<HH", d, at + 0x14)
    if d[at + 0x13] != 4:
        raise ValueError("not a 4-bit TIM2")
    pixels = d[at + head:at + head + image_size]
    clut = d[at + head + image_size:at + head + image_size + clut_size]
    alpha = [clut[4 * i + 3] for i in range(16)]
    idx = bytearray(w * h)
    for i, b in enumerate(pixels[:w * h // 2]):
        idx[2 * i] = b & 0xF
        idx[2 * i + 1] = b >> 4
    return w, h, bytes(idx), alpha


def width_table(disc_folder: Path):
    """{code: (character or None, [width in name, message, system])}."""
    elf = (disc_folder / "SCUS_974.90").read_bytes()
    phoff = struct.unpack_from("<I", elf, 0x1C)[0]
    phentsize, phnum = struct.unpack_from("<HH", elf, 0x2A)
    for i in range(phnum):
        kind, offset, vaddr, _, size = struct.unpack_from("<5I", elf, phoff + i * phentsize)
        if kind == 1 and vaddr <= european.FONT_TABLE < vaddr + size:
            at = offset + european.FONT_TABLE - vaddr
            rows = {}
            for code in range(256):
                char, *widths = elf[at + 4 * code:at + 4 * code + 4]
                if code and widths[0] == 0:
                    break
                rows[code] = (chr(char) if 32 <= char < 127 else None, widths)
            return rows
    raise ValueError("the width table is not in the executable")


def load_fonts(disc_folder: Path = ROOT / "disc"):
    """{game font: [Glyph]} for the codes the width table names."""
    disc = european.disc_from_folder(disc_folder)
    entries = {e.name: e for e in european.pac_read(disc.read(FONT_PACK))}
    table = width_table(disc_folder)
    fonts = {}
    for font, (page, cw, ch, column) in FONTS.items():
        w, h, idx, alpha = tim2_4bit(entries[page].data)
        doubled = bytes(min(255, 2 * alpha[i]) for i in idx)
        sheet = Image.frombytes("L", (w, h), doubled)
        per_row = w // cw
        glyphs = []
        for code, (char, widths) in sorted(table.items()):
            x, y = (code % per_row) * cw, (code // per_row) * ch
            cell = sheet.crop((x, y, x + cw, y + ch))
            box = cell.getbbox()
            ink = (box[0], box[1], box[2] - 1, box[3] - 1) if box else None
            glyphs.append(Glyph(code, char or SYMBOLS.get(code), widths[column], cell, ink))
        fonts[font] = glyphs
    return fonts


def eur_read(disc_folder: Path, lang: str, name: str) -> bytes:
    """A file of a European language (meswin\\fontset_main_fr.pak...), from
    disc/EU/<LANG>.idx and .DAT (Install-European.bat)."""
    eu = disc_folder / "EU"
    for line in (eu / f"{lang.upper()}.idx").read_text(encoding="utf-8").splitlines():
        fields = line.split("\t")
        if len(fields) == 3 and fields[0].lower() == name.lower():
            with open(eu / f"{lang.upper()}.DAT", "rb") as f:
                f.seek(int(fields[1]) * 2048)
                return f.read(int(fields[2]))
    raise ValueError(f"{name} is not in {eu / (lang.upper() + '.idx')}: run Install-European.bat")


def eur_pack(disc_folder: Path = ROOT / "disc", lang: str = "FR") -> bytes:
    """The European font pack (one for the four languages, each its copy)."""
    return eur_read(disc_folder, lang, f"meswin\\fontset_main_{lang.lower()}.pak")


def quant_for(alpha) -> bytes:
    """A coverage (0-255) to the nearest of a page's 16 alpha levels, doubled
    as the cells are (fit.QUANT is the US pages' even ramp)."""
    levels = sorted({min(255, 2 * a) for a in alpha})
    return bytes(min(levels, key=lambda v: (abs(v - c), v)) for c in range(256))


# {European font: its page's quantization}, filled by load_fonts_eur: the
# European pages' alpha levels are not the US pages' even ramp.
EUR_QUANT = {}


def load_fonts_eur(disc_folder: Path = ROOT / "disc", lang: str = "FR"):
    """{European font: [Glyph]}: the EUR pages' codes 0-170, their characters
    by EUR_FONTS' map (glyph code n in cell n, the page's cells)."""
    entries = {e.name.lower(): e for e in european.pac_read(eur_pack(disc_folder, lang))}
    table = width_table(disc_folder)
    fonts = {}
    for font, (page, cw, ch, column) in EUR_FONTS.items():
        w, h, idx, alpha = tim2_4bit(entries[page.lower()].data)
        EUR_QUANT[font] = quant_for(alpha)
        sheet = Image.frombytes("L", (w, h), bytes(min(255, 2 * alpha[i]) for i in idx))
        per_row = w // cw
        glyphs = []
        for code in range(EUR_ACCENT_FIRST + len(EUR_ACCENTS)):
            if code >= EUR_ACCENT_FIRST:
                char = EUR_ACCENTS[code - EUR_ACCENT_FIRST]
                width = EUR_ACCENT_WIDTHS[code - EUR_ACCENT_FIRST][column] - (1 if column == 0 else 0)
            elif code < 96 and code in table:
                char, widths = table[code]
                char, width = char or EUR_SYMBOLS.get(code), widths[column]
            elif code in EUR_SYMBOLS:
                char, width = EUR_SYMBOLS[code], table.get(code, (None, [0, 0, 0]))[1][column]
            else:
                continue
            x, y = (code % per_row) * cw, (code // per_row) * ch
            cell = sheet.crop((x, y, x + cw, y + ch))
            box = cell.getbbox()
            ink = (box[0], box[1], box[2] - 1, box[3] - 1) if box else None
            glyphs.append(Glyph(code, char, width, cell, ink))
        fonts[font] = glyphs
    return fonts
