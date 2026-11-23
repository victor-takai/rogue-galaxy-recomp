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
