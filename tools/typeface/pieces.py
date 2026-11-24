"""The digits of the menus' pictures, read from the extracted disc: the game
draws a menu's numbers (the clock, the files' numbers, the money, the
levels) a digit at a time, each from its cell of a row of digits in one of
the menus' pictures (TIM2s in menu\\rc\\*_en.pac). Each set here is one such
row: where it lies, its cell, its characters and whether it slants.

A digit is drawn as a light core inside a dark edge (a pixel wide), the
core often shaded from top to bottom. For the fit, each cell's core is
taken as coverage: how far each opaque texel's lightness goes from the
edge's to its row's lightest (the edge's own texels count none).

For the game to know a digit when it is drawn, each cell's texels are
hashed as the renderer hands them over (RGBA, the GS's alpha), at the
sizes a draw may sample (up to two texels less or one more across, a texel
less or more down: the battle's small slash is drawn 8x12 of its 10x13),
with the
hash of the picture's palette as dq8-gfx makes it. Everything is read into
memory; only numbers go into the table.
"""
import re
import struct
import sys
from dataclasses import dataclass, field
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "disc"))
import european  # noqa: E402

import pages  # noqa: E402
from pages import Glyph  # noqa: E402


@dataclass
class SetSpec:
    pack: str
    entry: str
    tim: int  # which TIM2 of the entry, from 0
    x0: int
    y0: int
    pitch: int
    cw: int
    ch: int
    chars: str
    italic: bool = False
    # A font of the menus laid out by a setting text (pack, entry): line 1
    # its characters, line 2 its cell width and height, the page's width,
    # then each character's width, as UiGlyphTableBuild reads it. Character
    # n is then at its cell (cells row by row) and drawn its width wide.
    setting: tuple | None = None
    ref: str = "0"  # the character the fit takes the height from
    # Signs in the cells after `chars` (the attributes' slash): placed on
    # the console's, without weighing in the set's fit.
    signs: str = ""
    # Which text's pictures hold it: "all" (the digits: the European text's
    # pictures hold them in the same cells, in palettes of their own), "en"
    # (the English menu font) or "eur" (the European one).
    text: str = "all"


# The European languages: each has its copy of the menus' pictures (the same
# cells, its own palettes), disc/EU/<LANG>.DAT.
EUR_LANGS = ("FR", "DE", "ES", "IT")

# The sets, by the name the table gives them.
SETS = {
    # The menus' own font (item commands, Allies, shops): letters drawn one
    # by one from menu_font_EN.img, as the digits are from their pictures.
    "menu": SetSpec("menu\\rc\\menu_static_img_en.pac", "menu_font_EN.img", 0, 0, 0, 0, 0, 0, "",
                    setting=("menu\\rc\\menu_static_res_en.pac", "menu_font_setting_EN.txt"), ref="H", text="en"),
    # The European text's menu font: the same page in the four languages,
    # its setting listing the accents as bytes 0xA0 on (pages.EUR_ACCENTS).
    "menu_eur": SetSpec("menu\\rc\\menu_static_img_en.pac", "menu_font_EN.img", 0, 0, 0, 0, 0, 0, "",
                        setting=("menu\\rc\\menu_static_res_en.pac", "menu_font_setting_EN.txt"), ref="H",
                        text="eur"),
    # The clock and the play time (the main and load menus, the transporter),
    # and the row's letters after it: the Battle Recorder's points ("P");
    # the dot between (a space here) is left as it is.
    "clock": SetSpec("menu\\rc\\menu_static_img_en.pac", "main_menu_top_a2_EN.img", 2, 0, 0, 12, 12, 16,
                     "0123456789/: zPX"),
    # The members' levels and attributes (Allies), large and small, and the
    # slash between the HP and AP and their most (the small one drawn from
    # a sprite inside its cell, 8x10 two texels in).
    "status": SetSpec("menu\\rc\\menu_static_img_en.pac", "main_menu_top_a2_EN.img", 2, 0, 16, 12, 12, 15,
                      "0123456789", signs="/:"),
    "statussmall": SetSpec("menu\\rc\\menu_static_img_en.pac", "main_menu_top_a2_EN.img", 2, 0, 32, 10, 10, 12,
                           "0123456789", signs="/"),
    # The small digits beside "Lv" (the transporter's list of members).
    "level": SetSpec("menu\\rc\\transport_menu_rc_en.pac", "tp_menu_top_EN.img", 2, 0, 0, 12, 12, 14,
                     "0123456789"),
    # The files' numbers (FILE 01 in the load and save menus).
    "file": SetSpec("menu\\rc\\transport_menu_rc_en.pac", "tp_menu_save_EN.img", 1, 0, 0, 14, 14, 20,
                    "0123456789"),
    # The money, slanted (the items menu).
    "money": SetSpec("menu\\rc\\menu_static_img_en.pac", "menu_item_b_EN.img", 3, 0, 106, 14, 14, 22,
                     "0123456789", italic=True),
    # In battle: the members' HP and AP, large and small.
    "battle": SetSpec("menu\\rc\\menu_static_img_en.pac", "battle_menu_fix_b_EN.img", 0, 0, 83, 12, 12, 15,
                      "0123456789", italic=True),
    "battlesmall": SetSpec("menu\\rc\\menu_static_img_en.pac", "battle_menu_fix_b_EN.img", 0, 0, 98, 10, 10, 13,
                           "0123456789/", italic=True),
    # In battle: the damage dealt, in chrome.
    "damage": SetSpec("menu\\rc\\menu_static_img_en.pac", "battle_menu_fix_b_EN.img", 5, 1, 0, 20, 19, 25,
                      "0123456789/", italic=True),
    # After a battle: the experience and money won, and the levels (gold).
    "result": SetSpec("menu\\rc\\battle_endinfo_menu_rc_en.pac", "battle_menu_ed_EN.img", 0, 0, 4, 14, 14, 18,
                      "0123456789/", italic=True),
    "resultgold": SetSpec("menu\\rc\\battle_endinfo_menu_rc_en.pac", "battle_menu_ed_EN.img", 0, 0, 22, 14, 14, 18,
                          "0123456789", italic=True),
    # The registered mark of the memory card's messages ("PlayStation®2"): a
    # special character of the text, its picture the smaller of the two in
    # gaiji2.img (the buttons' picture, beside the fonts' pages), drawn 15x20.
    # The European text's own gaiji2.img has it in the same cell.
    "registered": SetSpec("meswin\\fontset_main_en.pak", "gaiji2.img", 0, 18, 236, 15, 15, 20, "®", ref="®"),
}


@dataclass
class PieceSet:
    name: str
    spec: SetSpec
    glyphs: list  # [Glyph], the core as the cell
    palette: int  # dq8-gfx's hash of the picture's palette
    # char -> [(width, height, hash of the RGBA, x and y of the sprite in the cell)]
    variants: dict = field(default_factory=dict)


# The words of the menus' pictures (step 3 of the menus' letters): a picture
# (pack, entry, TIM2), and per word its text and a window around it in the
# picture; the word's box is the texels with ink inside the window. The game
# draws a word as a piece holding it and nothing else (a clear background
# around it), and redraws are made for such pieces, other art two texels or
# more away kept; a word printed on a button's fill only when marked
# "button" (its fill put back under it). Typed by hand from the
# pictures: only places and words, no image. A sixth field, when there,
# holds the word's style: "i" slanted, "sc" its lower-case letters drawn
# as the typeface's small capitals (the members' names), "C=GLYPH" the
# character C drawn as the typeface's glyph GLYPH: an alternate (the names'
# J with its bar on top, which the fit alone does not pick) or C's default
# glyph where the menu font took an alternate (the story title's y with its
# tail, like the console's italic y), "shadow" a soft dark
# shadow under it instead of an edge (the battle's end: Exp, Skills),
# "grow=N" its strokes' growth held at N eighths of a texel a side (the fit
# alone drew Acquired Items bolder than Exp and Skills beside it), "button"
# printed on a button's fill (the framed Player): the word told from the
# fill by colour (button_kind), the fill put back under the new word,
# "bar" printed on a band of colour across its whole box (the items' MAX on
# its blue bar): a row of the box opaque at both ends is the band's, and its
# texels but the word's core (bar_kind) are the band, put back under the new
# word from one end of the row to the other, "art" printed on art (the
# Revelation Flow's title on the frame's wood): its texels told from the
# art's by colour (art_kind), its box theirs, and the art put back under the
# new word, each run of the word's texels in a row filled from the art left
# and right of it,
# "scale=X" its letters' horizontal scale held at X (a word whose letters
# stand far apart would be stretched to them), "spaced" its size held at the
# console letters' height and its letters spread to the console word's width.
WORDS = [
    ("menu\\rc\\menu_static_img_en.pac", "main_menu_top_a2_EN.img", 2, [
        ("Items", 0, 45, 40, 18), ("Allies", 0, 65, 41, 18), ("Strategy", 0, 84, 60, 20),
        ("Revelation", 0, 105, 73, 18), ("Battle Recorder", 86, 45, 86, 18), ("Frog Log", 86, 64, 62, 20),
        ("Factory", 86, 84, 53, 20), ("System", 86, 104, 51, 20), ("Settings", 172, 84, 59, 20),
        ("Help", 172, 104, 33, 20), ("Sold-out", 204, 64, 48, 16), ("?", 172, 64, 20, 20), ("AP", 205, 2, 18, 13),
        ("EXP", 232, 0, 24, 13), ("HP", 238, 14, 18, 13), ("Lv", 204, 16, 19, 16), ("AP", 238, 28, 18, 13),
        ("Lv", 238, 42, 15, 13)]),
    ("menu\\rc\\transport_menu_rc_en.pac", "tp_menu_top_EN.img", 2, [
        ("Save", 0, 14, 86, 20), ("Teleport", 0, 34, 86, 20), ("Exchange Points", 86, 14, 86, 20),
        ("Load", 172, 14, 84, 20), ("Item Storage", 86, 34, 86, 20), ("Lv", 120, 0, 18, 14)]),
    ("menu\\rc\\title_menu_rc_en.pac", "tp_menu_load_EN.img", 2, [
        ("Settings", 72, 0, 56, 18), ("On", 87, 38, 25, 18), ("High", 12, 56, 36, 19), ("Low", 75, 56, 31, 18),
        ("Normal", 4, 74, 52, 18), ("Reversed", 60, 74, 60, 18), ("Stereo", 4, 92, 52, 18),
        ("Mono", 68, 92, 43, 18), ("Active", 5, 110, 50, 18), ("Off", 76, 110, 28, 18)]),
    ("menu\\rc\\transport_menu_rc_en.pac", "tp_menu_save_EN.img", 1, [
        ("FILE", 140, 0, 41, 19), ("Slot 1", 0, 40, 56, 16), ("Slot 2", 56, 40, 56, 16)]),
    ("menu\\rc\\menu_static_img_en.pac", "menu_pt_b_EN.img", 1, [
        ("Player", 2, 0, 44, 16), ("Party", 5, 17, 39, 15),
        # The framed button with Player printed on its fill (Allies, Strategy):
        # the box is the word and its edge, the fill left and right of it. Its
        # letters stand far apart: the plain Player's proportions, strokes and
        # double-storey a held, the size its P's, the pens spread to its width
        # (the fit alone stretched the letters 1.67 times as wide, or shrank
        # and thinned them).
        ("Player", 19, 70, 58, 12, "button scale=1.12 spaced grow=1 a=a")]),
    ("menu\\rc\\menu_static_img_en.pac", "menu_pt_b_EN.img", 2, [
        ("Jaster", 0, 0, 64, 18, "i sc J=J.BarTop"), ("Lilika", 76, 0, 56, 18, "i sc"),
        ("Deego", 0, 18, 60, 18, "i sc"), ("Jupis", 76, 18, 53, 18, "i sc J=J.BarTop"), ("Simon", 0, 36, 58, 18, "i sc"), ("Zegram", 76, 36, 73, 18, "i sc"),
        ("Kisala", 0, 54, 60, 18, "i sc"), ("Hooded Man", 76, 53, 75, 19, "i sc"),
        ("Steve", 0, 72, 57, 18, "i sc"), ("Hooded Man", 76, 71, 76, 19, "i sc"),
        ("'s Weapons", 0, 90, 81, 18, "i"),
        ("Equipment", 152, 0, 92, 18), ("Base", 152, 18, 92, 18), ("Defense Status", 152, 36, 104, 18),
        ("Ability List", 152, 53, 80, 18), ("Equipped", 152, 72, 92, 18)]),
    ("menu\\rc\\main_menu_img_en.pac", "menu_tac_a_EN.img", 0, [
        ("Suggestion Settings", 0, 60, 128, 17)]),
    # The Revelation Flow's title, printed on the wood of its frame.
    ("menu\\rc\\main_menu_img_en.pac", "menu_imgboard_EN.img", 0, [
        ("Revelation Flow", 20, 16, 156, 18, "art")]),
    ("menu\\rc\\main_menu_img_en.pac", "menu_br_a_EN.img", 0, [
        ("Hunter", 164, 0, 49, 16), ("Name", 164, 16, 41, 15), ("Points", 164, 32, 46, 15),
        ("Monster", 164, 48, 57, 16), ("Target", 164, 64, 48, 16), ("List", 164, 80, 29, 16),
        ("Total", 164, 96, 39, 16), ("Rank", 164, 112, 37, 15), ("Hunted", 164, 128, 51, 16),
        ("Quarries", 13, 129, 64, 19), ("Hunting Record", 2, 149, 86, 19), ("Ranking", 15, 169, 60, 19),
        ("Hunter Points", 162, 184, 94, 19)]),
    ("menu\\rc\\menu_static_img_en.pac", "menu_item_b_EN.img", 3, [
        ("Power", 0, 0, 47, 16), ("Skills", 0, 16, 40, 16), ("S", 40, 16, 12, 16), ("P", 52, 16, 12, 16)]),
    # A weapon's Skills when they are all learned (MAX, its letters apart on
    # a blue band) and when mastered.
    ("menu\\rc\\menu_static_img_en.pac", "menu_item_b_EN.img", 2, [
        ("MAX", 0, 48, 54, 16, "bar spaced"), ("MASTERED", 55, 48, 55, 16)]),
    ("menu\\rc\\main_menu_img_en.pac", "menu_item_sp_EN.img", 0, [
        ("Rare Items", 160, 70, 75, 16, "shadow"), ("Ancient Key Pieces", 0, 86, 132, 17, "shadow"),
        ("Power of the Heart", 1, 104, 132, 16, "shadow")]),
    # The story screen's title is a serif italic: drawn in Andika, slanted.
    ("rc\\loading2_en.img", "loading2_en.img", 0, [
        ("Story", 26, 0, 102, 27, "i y=y"), ("Press", 25, 41, 54, 22)]),
    ("menu\\rc\\battle_endinfo_menu_rc_en.pac", "battle_menu_ed_EN.img", 0, [
        ("Skills", 169, 0, 45, 22, "shadow"), ("Exp", 217, 0, 34, 24, "shadow"),
        ("Acquired Items", 0, 64, 112, 23, "shadow grow=2"),
        ("Player Bonus!", 56, 108, 107, 20), ("MASTERED", 160, 22, 70, 20, "i"), ("Lv", 139, 22, 21, 20, "i"),
        ("LEVEL UP !", 97, 42, 106, 22), ("Lv UP!", 0, 42, 40, 15)]),
    ("menu\\rc\\menu_static_img_en.pac", "battle_menu_fix_b_EN.img", 0, [
        ("Continue", 127, 76, 61, 17), ("Go back to title", 127, 96, 107, 17), ("AP", 215, 197, 20, 13),
        ("HP", 237, 197, 18, 13), ("SKIP:", 216, 209, 40, 16)]),
]


@dataclass
class Word:
    text: str
    picture: str
    box: tuple  # (x, y, w, h) in the picture
    palette: int
    hash: int  # hash_rgba of the box's texels
    glyph: Glyph  # the core, the box as its cell
    italic: bool = False
    small_caps: bool = False
    alternates: dict = field(default_factory=dict)  # character -> the typeface's glyph name
    # A soft shadow's offset (cell pixels across and down), blur (its
    # Gaussian's sigma in cell pixels) and strength (0-1); None: an edge.
    shadow: tuple | None = None
    grow: int | None = None  # the strokes' growth held (fit.Params.grow); None: fitted
    button: bool = False  # printed on a button's fill (its box is the word's and the fill's)
    k: float | None = None  # the extra horizontal scale held (fit.Params.k); None: fitted
    spaced: bool = False  # its letters stand far apart: size held, pens spread to the console word's width
    bar: bool = False  # printed on a flat band of colour (its box is the word's and the band's)
    art: bool = False  # printed on art (its box is its own texels', the art's in between)


def unswizzle8(buf: bytes, width: int, height: int) -> bytearray:
    """PSMT8 texels from the PSMCT32-swizzled order some TIM2s keep."""
    out = bytearray(width * height)
    for y in range(height):
        for x in range(width):
            block = (y & ~0xF) * width + (x & ~0xF) * 2
            swap = (((y + 2) >> 2) & 1) * 4
            row = (((y & ~3) >> 1) + (y & 1)) & 7
            column = row * width * 2 + ((x + swap) & 7) * 4
            at = block + column + ((y >> 1) & 1) + ((x >> 2) & 2)
            out[y * width + x] = buf[at] if at < len(buf) else 0
    return out


def roughness(idx, width: int) -> int:
    """How many texels differ from the one to their right."""
    return sum(1 for i in range(len(idx) - 1) if (i + 1) % width and idx[i] != idx[i + 1])


def tim2(d: bytes, at: int):
    """(width, height, indices, palette as [(r, g, b, a)] in index order) of
    the 4- or 8-bit TIM2 at `at`."""
    if d[at:at + 4] != b"TIM2":
        raise ValueError("not a TIM2")
    pic = at + (0x80 if d[at + 5] == 1 else 0x10)
    _total, clut_size, image_size, head, _colours = struct.unpack_from("<IIIHH", d, pic)
    clut_type, img_type = d[pic + 0x12], d[pic + 0x13]
    w, h = struct.unpack_from("<HH", d, pic + 0x14)
    pixels = d[pic + head:pic + head + image_size]
    clut = d[pic + head + image_size:pic + head + image_size + clut_size]
    if img_type == 4:
        idx = bytearray(w * h)
        for i, b in enumerate(pixels[:w * h // 2]):
            idx[2 * i] = b & 0xF
            idx[2 * i + 1] = b >> 4
    elif img_type == 5:
        idx = bytearray(pixels[:w * h])
        # Some are kept in the GS's order (always those with format byte 5,
        # some others too, with nothing in the header to say so): the order
        # whose neighbouring texels differ less is the picture.
        swizzled = unswizzle8(idx, w, h)
        if d[pic + 0x10] == 5 or roughness(swizzled, w) < roughness(idx, w):
            idx = swizzled
    else:
        raise ValueError(f"image type {img_type}")
    pal = [tuple(clut[4 * i:4 * i + 4]) for i in range(clut_size // 4)]
    if img_type == 5 and (clut_type & 0x80) == 0 and len(pal) == 256:
        # The 8-bit palettes are stored in the GS's order (blocks of 8 swapped).
        pal = [pal[(i & ~0x18) | ((i & 0x08) << 1) | ((i & 0x10) >> 1)] for i in range(256)]
    return w, h, bytes(idx), pal


FNV_SEED, FNV_PRIME, MASK = 0xCBF29CE484222325, 0x100000001B3, (1 << 64) - 1


def hash_rgba(data: bytes) -> int:
    """FNV-1a over the bytes, as src/typeface_letters.cpp's hashRgba."""
    h = FNV_SEED
    for b in data:
        h = ((h ^ b) * FNV_PRIME) & MASK
    return h


def palette_hash(pal, entries: int) -> int:
    """dq8-gfx's GsTextureCache::hashClutWindow for a CT32 palette (CSM1)."""
    h = 1469598103934665603

    def step(h, value):
        for byte in range(8):
            h = ((h ^ ((value >> (byte * 8)) & 0xFF)) * 1099511628211) & MASK
        return h

    h = step(step(h, 0), 0)
    for i in range(entries):
        r, g, b, a = pal[i] if i < len(pal) else (0, 0, 0, 0)
        h = step(h, r | g << 8 | b << 16 | a << 24)
    return h


def lightness(r, g, b):
    return (3 * r + 6 * g + b) / 10


def core_cells(rgba, w, cw, ch, origins):
    """Each cell's core as coverage 0-1 (a list of rows), by the set's edge
    colour and each row's lightest opaque texel."""
    cells = [[[rgba[(y0 + y) * w + x0 + x] for x in range(cw)] for y in range(ch)] for x0, y0 in origins]
    # The edge: the opaque texels next to a clear one, their mean lightness.
    edge = []
    for cell in cells:
        for y, row in enumerate(cell):
            for x, (r, g, b, a) in enumerate(row):
                if a < 0x40:
                    continue
                near = [cell[yy][xx][3] for yy, xx in ((y - 1, x), (y + 1, x), (y, x - 1), (y, x + 1))
                        if 0 <= yy < ch and 0 <= xx < cw]
                if len(near) < 4 or min(near) < 0x20:
                    edge.append(lightness(r, g, b))
    dark = sum(edge) / len(edge) if edge else 0.0
    light = [max([lightness(*t[:3]) for cell in cells for t in cell[y] if t[3] >= 0x70] or [dark])
             for y in range(ch)]
    out = []
    for cell in cells:
        cov = []
        for y, row in enumerate(cell):
            span = light[y] - dark
            cov.append([0.0 if a < 0x70 or span < 24 else
                        max(0.0, min(1.0, (lightness(r, g, b) - dark) / (0.65 * span)))
                        for r, g, b, a in row])
        out.append(cov)
    return out


def pack_reader(disc_folder: Path, lang: str | None):
    """pack -> {entry: data}: the US disc's, or a European language's
    (disc/EU), its pack and entries named by their US names."""
    if lang is None:
        disc = european.disc_from_folder(disc_folder)
        return lambda pack: {e.name: e.data for e in european.pac_read(disc.read(pack))}

    def read(pack):
        name = re.sub(r"_en(?=\.pa[ck]$)", f"_{lang.lower()}", pack, flags=re.IGNORECASE)
        entries = european.pac_read(pages.eur_read(disc_folder, lang, name))
        return {re.sub(r"_" + lang + r"(?=[._])", "_EN", e.name, flags=re.IGNORECASE): e.data for e in entries}
    return read


def load_pieces(disc_folder: Path = ROOT / "disc", names=None, lang: str | None = None):
    """{set name: PieceSet} for the sets asked (all by default), from the US
    pictures, or with `lang` a European language's (the sets its text has).
    A European set alone (menu_eur) is read from the first European language."""
    readers = {}
    packs = {}
    out = {}
    for name, spec in SETS.items():
        if names is not None and name not in names:
            continue
        if lang is not None and spec.text == "en":
            continue
        used = lang if lang is not None else (EUR_LANGS[0] if spec.text == "eur" else None)
        if used not in readers:
            readers[used] = pack_reader(disc_folder, used)
        if (used, spec.pack) not in packs:
            packs[(used, spec.pack)] = readers[used](spec.pack)
        d = packs[(used, spec.pack)][spec.entry]
        at = d.find(b"TIM2")
        for _ in range(spec.tim):
            at = d.find(b"TIM2", at + 4)
        w, h, idx, pal = tim2(d, at)
        rgba = [pal[i] if i < len(pal) else (0, 0, 0, 0) for i in idx]
        chars, cw, ch = spec.chars, spec.cw, spec.ch
        if spec.setting:
            pack, entry = spec.setting
            if (used, pack) not in packs:
                packs[(used, pack)] = readers[used](pack)
            lines = packs[(used, pack)][entry].decode("latin-1").splitlines()
            numbers = [int(v) for v in lines[1].split(",") if v.strip()]
            chars, (cw, ch, page) = lines[0], numbers[:3]
            if used is not None:
                # The European setting's accents, one byte each from 0xA0.
                chars = "".join(pages.EUR_ACCENTS[ord(c) - 0xA0] if 0xA0 <= ord(c) < 0xA0 + len(pages.EUR_ACCENTS)
                                else c for c in chars)
            per_row = page // cw
            origins = [((n % per_row) * cw, (n // per_row) * ch) for n in range(len(chars))]
            widths = numbers[3:3 + len(chars)]
            spec = SetSpec(spec.pack, spec.entry, spec.tim, 0, 0, cw, cw, ch, chars, spec.italic, spec.setting,
                           spec.ref, text=spec.text)
        else:
            origins = [(spec.x0 + n * spec.pitch, spec.y0) for n in range(len(chars) + len(spec.signs))]
            widths = [cw] * len(origins)
        # The characters' cores as the set alone gives them (the edge's and
        # the rows' lightness from its cells), the signs' after.
        cores = core_cells(rgba, w, cw, ch, origins[:len(chars)])
        if spec.signs:
            cores += core_cells(rgba, w, cw, ch, origins)[len(chars):]
        chars += spec.signs
        glyphs, variants = [], {}
        for n, c in enumerate(chars):
            img = Image.new("L", (cw, ch))
            img.putdata([int(round(v * 255)) for row in cores[n] for v in row])
            box = img.point(lambda v: 255 if v >= 64 else 0).getbbox()
            ink = (box[0], box[1], box[2] - 1, box[3] - 1) if box else None
            glyphs.append(Glyph(n, c, widths[n], img, ink))
            x0, y0 = origins[n]
            # (width, height, hash, x and y of its corner in the cell): the
            # cell's corner, and for a sign a sprite starting inside the cell
            # too (the small slash, two texels in and one down).
            sizes = [(0, 0, rw, rh) for rw in (widths[n] - 2, widths[n] - 1, widths[n], widths[n] + 1)
                     for rh in (ch - 1, ch, ch + 1)]
            if c in spec.signs:
                sizes += [(ox, oy, cw - ox - dw, ch - oy - dh) for ox, oy in ((1, 0), (2, 0), (0, 1), (1, 1), (2, 1))
                          for dw in (0, 1, 2) for dh in (0, 1, 2)]
            seen = []
            for ox, oy, rw, rh in sizes:
                if rw <= 0 or rh <= 0 or x0 + ox + rw > w or y0 + oy + rh > h:
                    continue
                data = bytes(v for y in range(rh) for x in range(rw)
                             for v in rgba[(y0 + oy + y) * w + x0 + ox + x])
                seen.append((rw, rh, hash_rgba(data), ox, oy))
            variants[c] = seen
        out[name] = PieceSet(name, spec, glyphs, palette_hash(pal, 16 if len(pal) <= 16 else 256), variants)
    return out


def button_kind(t):
    """A texel of a word printed on a button: "core" (the word's grey),
    "edge" (its dark edge) or "fill" (the button's blue). typeface_letters.cpp's
    buttonTexel is the same."""
    r, g, b, _ = t
    light = lightness(r, g, b)
    if light < 45:
        return "edge"
    return "core" if b - r < 0x38 and light > 90 else "fill"


def bar_kind(t):
    """A texel of a word printed on a band (the items' MAX): "core" (the
    word's pale blue), "edge" (its dark blue edge, and the band's) or "fill"
    (the band's saturated blue). typeface_letters.cpp's barTexel is the same."""
    r, g, b, _ = t
    light = lightness(r, g, b)
    if light < 70:
        return "edge"
    return "fill" if b - r > 110 and light < 115 else "core"


def art_kind(t):
    """A texel of a word printed on art (the Revelation Flow's title on the
    frame's wood): "word" (its lavender core, grey bevel, black edge: blue as
    much as red or more) or "art" (the wood and the frame, red well above
    blue). typeface_letters.cpp's artTexel is the same."""
    r, g, b, a = t
    return "word" if a >= 0x40 and b + 4 >= r else "art"


def shadow_core(rgba, w, x0, y0, bw, bh):
    """A shadowed word's core as coverage (rows), and its shadow fitted: the
    core is its light texels (their alpha, toward their row's lightest), the
    shadow its dark ones; the shadow taken as the core blurred, moved and
    faded, (dx, dy, sigma, strength) the closest."""
    from PIL import ImageFilter
    cell = [[rgba[(y0 + y) * w + x0 + x] for x in range(bw)] for y in range(bh)]
    top = max(lightness(*t[:3]) for row in cell for t in row if t[3])
    dark = [[t[3] / 128 if t[3] and lightness(*t[:3]) < 0.35 * top else 0.0 for t in row] for row in cell]
    rows_light = [max([lightness(*t[:3]) for t in row if t[3] >= 0x70] or [top]) for row in cell]
    core = [[min(1.0, t[3] / 127) * max(0.0, min(1.0, (lightness(*t[:3]) - 0.35 * top)
                                                     / max(1.0, 0.5 * (light - 0.35 * top))))
             if t[3] else 0.0 for t in row] for row, light in zip(cell, rows_light)]
    img = Image.new("L", (bw, bh))
    img.putdata([int(round(v * 255)) for row in core for v in row])
    best = None
    for sigma in (0.5, 0.8, 1.1, 1.5, 2.0, 2.5, 3.0):
        blurred = img.filter(ImageFilter.GaussianBlur(sigma))
        b = list(blurred.getdata())
        for dy in (0, 1, 2):
            for dx in (0, 1, 2):
                pairs = []
                for y in range(bh):
                    for x in range(bw):
                        if core[y][x] >= 0.3:
                            continue
                        sx, sy = x - dx, y - dy
                        value = b[sy * bw + sx] / 255 if 0 <= sx < bw and 0 <= sy < bh else 0.0
                        pairs.append((value, dark[y][x]))
                den = sum(v * v for v, _ in pairs)
                k = max(0.0, min(2.0, sum(v * s for v, s in pairs) / den)) if den else 0.0
                err = sum((k * v - s) ** 2 for v, s in pairs)
                if best is None or err < best[0]:
                    best = (err, (dx, dy, sigma, k))
    return core, best[1]


def word_picture_palettes(disc_folder: Path, lang: str) -> dict:
    """{palette hash: picture} of a European language's copies of the WORDS
    pictures (a picture the language has no copy of left out)."""
    read = pack_reader(disc_folder, lang)
    packs = {}
    out = {}
    for pack, entry, tim, _words in WORDS:
        try:
            if pack not in packs and pack.lower().endswith(".img"):
                name = re.sub(r"_en(?=\.img$)", f"_{lang.lower()}", pack, flags=re.IGNORECASE)
                packs[pack] = {entry: pages.eur_read(disc_folder, lang, name)}
            elif pack not in packs:
                packs[pack] = read(pack)
        except ValueError:
            packs[pack] = {}
        d = packs[pack].get(entry)
        if d is None:
            continue
        at = d.find(b"TIM2")
        for _ in range(tim):
            at = d.find(b"TIM2", at + 4)
        _w, _h, _idx, pal = tim2(d, at)
        out[palette_hash(pal, 16 if len(pal) <= 16 else 256)] = f"{lang} {entry}#{tim}"
    return out


def load_words(disc_folder: Path = ROOT / "disc"):
    """[Word] of WORDS: each word's box (the texels with ink in its window),
    the hash of the box's texels, its core (as core_cells takes it, from the
    word's own edge and rows) and the picture's palette hash."""
    disc = european.disc_from_folder(disc_folder)
    packs = {}
    out = []
    for pack, entry, tim, words in WORDS:
        if pack not in packs and pack.lower().endswith(".img"):
            # A loose picture file (an IM3), its entry its own name.
            packs[pack] = {entry: disc.read(pack)}
        elif pack not in packs:
            packs[pack] = {e.name: e.data for e in european.pac_read(disc.read(pack))}
        d = packs[pack][entry]
        at = d.find(b"TIM2")
        for _ in range(tim):
            at = d.find(b"TIM2", at + 4)
        w, h, idx, pal = tim2(d, at)
        rgba = [pal[i] if i < len(pal) else (0, 0, 0, 0) for i in idx]
        palette = palette_hash(pal, 16 if len(pal) <= 16 else 256)
        for text, wx, wy, ww, wh, *style in words:
            flags = style[0].split() if style else []
            ink = [(x, y) for y in range(wy, min(h, wy + wh)) for x in range(wx, min(w, wx + ww))
                   if rgba[y * w + x][3] and ("art" not in flags or art_kind(rgba[y * w + x]) == "word")]
            if not ink:
                raise ValueError(f"{entry}#{tim}: no ink for {text!r}")
            x0, y0 = min(x for x, _ in ink), min(y for _, y in ink)
            bw, bh = max(x for x, _ in ink) - x0 + 1, max(y for _, y in ink) - y0 + 1
            data = bytes(v for y in range(bh) for x in range(bw) for v in rgba[(y0 + y) * w + x0 + x])
            shadow = None
            if "shadow" in flags:
                core, shadow = shadow_core(rgba, w, x0, y0, bw, bh)
            elif "button" in flags:
                # Printed on a button: the button's fill cleared out of the
                # box first, the word's core then read as any word's.
                cleared = list(rgba)
                for y in range(y0, y0 + bh):
                    for x in range(x0, x0 + bw):
                        if button_kind(rgba[y * w + x]) == "fill":
                            cleared[y * w + x] = (0, 0, 0, 0)
                core = core_cells(cleared, w, bw, bh, [(x0, y0)])[0]
            elif "art" in flags:
                # Printed on art: the art's texels cleared out of the box.
                cleared = list(rgba)
                for y in range(y0, y0 + bh):
                    for x in range(x0, x0 + bw):
                        if art_kind(rgba[y * w + x]) == "art":
                            cleared[y * w + x] = (0, 0, 0, 0)
                core = core_cells(cleared, w, bw, bh, [(x0, y0)])[0]
            elif "bar" in flags:
                # Printed on a band: in the band's rows (opaque at both ends
                # of the box) all but the word's core takes its edge's colour
                # (the mean of its edge in the other rows), so the core is read
                # against its own edge as any word's.
                rows = [y for y in range(y0, y0 + bh)
                        if rgba[y * w + x0][3] >= 0x40 and rgba[y * w + x0 + bw - 1][3] >= 0x40]
                edges = [rgba[y * w + x] for y in range(y0, y0 + bh) if y not in rows for x in range(x0, x0 + bw)
                         if rgba[y * w + x][3] >= 0x40 and bar_kind(rgba[y * w + x]) == "edge"]
                if not edges:
                    raise ValueError(f"{entry}#{tim}: {text!r} has no edge outside its band's rows")
                # Rounded as typeface_letters.cpp rounds it (halves up).
                edge = tuple(int(sum(t[c] for t in edges) / len(edges) + 0.5) for c in range(3)) + (0x7F,)
                cleared = list(rgba)
                for y in rows:
                    for x in range(x0, x0 + bw):
                        if bar_kind(rgba[y * w + x]) != "core":
                            cleared[y * w + x] = edge
                core = core_cells(cleared, w, bw, bh, [(x0, y0)])[0]
            else:
                core = core_cells(rgba, w, bw, bh, [(x0, y0)])[0]
            img = Image.new("L", (bw, bh))
            img.putdata([int(round(v * 255)) for row in core for v in row])
            box = img.point(lambda v: 255 if v >= 64 else 0).getbbox()
            core_ink = (box[0], box[1], box[2] - 1, box[3] - 1) if box else None
            named = dict(f.split("=", 1) for f in flags if "=" in f)
            out.append(Word(text, f"{entry}#{tim}", (x0, y0, bw, bh), palette, hash_rgba(data),
                            Glyph(0, text, bw, img, core_ink), "i" in flags, "sc" in flags,
                            {c: g for c, g in named.items() if len(c) == 1}, shadow,
                            int(named["grow"]) if "grow" in named else None, "button" in flags,
                            float(named["scale"]) if "scale" in named else None, "spaced" in flags,
                            "bar" in flags, "art" in flags))
    return out
