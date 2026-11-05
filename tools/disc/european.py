"""The European texts for the US game, built from the player's own PAL disc.

The US executable already asks for its text files through a language token
(`menu\\cmd_static3_?.txt`, `?` = EN). Asked for FR it wants files with the US
disc's layout: the same entries in each pack, the same ids in each table, the
same script in each scene. The PAL disc (SCES-54552) has the texts, but in a
revised build: scene scripts compiled for a newer script engine, menu packs
with other entries, game tables with the Director's Cut balance. So each file
is built from both discs, with the US file as the frame and only the words
taken from the PAL one:

- scene packs (event\\...\\*.pac): the US pack, its `_EN.txt` lines replaced by
  the PAL language's (a line the PAL text lacks stays English), the trio
  renamed `_FR`;
- language packs and loose language files: the US file; in it, labelled texts
  (`@N`) take the PAL lines, tables in the cfg format (.cfg.bin, .res.bin) the
  PAL strings for the US records, through the PAL English file as the bridge
  (US English and PAL English align record by record; PAL English and PAL
  French are the same build), and pictures the PAL picture;
- the menus' font setting: the PAL glyph list, each accent tag written as one
  byte 0xA0 + its index (src/text_language.cpp, kAccentTags);
- the message font: the PAL pack with the US pack's fifth normal page.

The result is <disc>/EU/<LANG>.DAT, every file on sectors of its own, and
<LANG>.idx, a line a file: its archive path, first sector, size.
"""

from __future__ import annotations

import difflib
import re
import struct
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable, Iterator

SECTOR = 2048
LANGUAGES = ("fr", "de", "es", "it")

# The PAL executable's 64 accent tags in its own order (0x45C420); the menus'
# glyph list writes the nth as the byte 0xA0 + n.
ACCENT_TAGS = (
    "<9>", "<6>", "<66>", "<99>", "<99G>", "<->", "<-->", "<--->", "<^!>", "<^?>", "<<<>", "<>>>",
    "<degree>", "<OE>", "<oe>", "<`A>", "<'A>", "<^A>", "<:A>", "<,C>", "<`E>", "<'E>", "<^E>", "<:E>",
    "<`I>", "<'I>", "<^I>", "<:I>", "<~N>", "<`O>", "<'O>", "<^O>", "<:O>", "<`U>", "<'U>", "<^U>",
    "<:U>", "<`a>", "<'a>", "<^a>", "<:a>", "<,c>", "<`e>", "<'e>", "<^e>", "<:e>", "<`i>", "<'i>",
    "<^i>", "<:i>", "<~n>", "<`o>", "<'o>", "<^o>", "<:o>", "<~O>", "<~o>", "<`u>", "<'u>", "<^u>",
    "<:u>", "<ss>", "<a>", "<0>",
)
ACCENT_FIRST_BYTE = 0xA0


# ---------------------------------------------------------------- archives

def normalize(name: str) -> str:
    return name.replace("/", "\\").lower().lstrip("\\")


@dataclass
class Archive:
    """One DATA<layer>_0 index (.HD6) and a reader of its .DAT."""

    read_at: Callable[[int, int], bytes]
    files: dict[str, tuple[int, int]] = field(default_factory=dict)  # name -> (sector, size)

    @classmethod
    def parse(cls, hd6: bytes, read_at: Callable[[int, int], bytes]) -> "Archive":
        if hd6[:4] != b"HD6\0":
            raise ValueError("not an HD6 index")
        w = lambda at: struct.unpack_from("<I", hd6, at)[0]
        tok_off, tok_size, tok_count = w(4), w(8), w(0xC)
        name_off, unit, count, table = w(0x14), w(0x20), w(0x24), w(0x28)
        tokens = []
        at = tok_off
        for _ in range(tok_count):
            end = hd6.index(b"\0", at)
            tokens.append(hd6[at:end].decode("latin-1"))
            at = end + 1
        archive = cls(read_at)
        for i in range(count):
            v = struct.unpack_from("<Q", hd6, table + 8 * i)[0]
            p = name_off + (v & 0x3FFFF)
            name = []
            while hd6[p]:
                ident = shift = 0
                while True:
                    b = hd6[p]
                    p += 1
                    ident |= (b & 0x7F) << shift
                    shift += 7
                    if not b & 0x80:
                        break
                name.append(tokens[ident])
            archive.files[normalize("".join(name))] = ((v >> 18) & 0x3FFFFF, (v >> 40) * unit)
        return archive

    def read(self, name: str) -> bytes:
        sector, size = self.files[name]
        return self.read_at(sector * SECTOR, size)


class Disc:
    """Both layers' DATA<layer>_0 archives; a name both hold has the same bytes in each."""

    def __init__(self, layers: list[Archive]):
        self.layers = layers

    def __contains__(self, name: str) -> bool:
        return any(name in a.files for a in self.layers)

    def names(self) -> set[str]:
        return set().union(*(a.files for a in self.layers))

    def read(self, name: str) -> bytes:
        for a in self.layers:
            if name in a.files:
                return a.read(name)
        raise KeyError(name)

    def size(self, name: str) -> int:
        for a in self.layers:
            if name in a.files:
                return a.files[name][1]
        raise KeyError(name)


def disc_from_folder(folder: Path) -> Disc:
    layers = []
    for layer in (0, 1):
        dat = folder / f"DATA{layer}_0.DAT"
        handle = open(dat, "rb")

        def read_at(offset: int, size: int, handle=handle) -> bytes:
            handle.seek(offset)
            return handle.read(size)

        layers.append(Archive.parse((folder / f"DATA{layer}_0.HD6").read_bytes(), read_at))
    return Disc(layers)


def disc_from_iso(iso) -> Disc:
    """From an open tools/disc/iso9660.IsoImage."""
    by_path = {}
    for entry in iso.entries():
        by_path.setdefault((entry.layer, entry.path.upper()), entry)
    layers = []
    for layer in (0, 1):
        dat = by_path.get((layer, f"DATA{layer}_0.DAT"))
        hd6 = by_path.get((layer, f"DATA{layer}_0.HD6"))
        if dat is None or hd6 is None:
            raise ValueError(f"the image has no DATA{layer}_0 archive on layer {layer}")

        def read_at(offset: int, size: int, base=dat.lba * SECTOR) -> bytes:
            iso._f.seek(base + offset)
            return iso._f.read(size)

        layers.append(Archive.parse(iso.read_bytes(hd6), read_at))
    return Disc(layers)


# ---------------------------------------------------------------- packs

@dataclass
class PacEntry:
    name: str
    data: bytes
    raw_header: bytes = b""  # the source's 0x50 bytes, kept for their padding


def pac_read(data: bytes) -> list[PacEntry]:
    out = []
    at = 0
    while at + 0x50 <= len(data):
        head, size, stride = struct.unpack_from("<III", data, at + 0x40)
        if size == 0xFFFFFFFF:
            break
        name = data[at:at + 0x40].split(b"\0")[0].decode("latin-1")
        out.append(PacEntry(name, data[at + head:at + head + size], data[at:at + 0x50]))
        if stride == 0:
            break
        at += stride
    return out


def pac_write(entries: list[PacEntry]) -> bytes:
    out = bytearray()
    marker = entries[0].raw_header[0x4C:0x50] if entries and entries[0].raw_header else b"\x00ABC"
    for e in entries:
        name = e.name.encode("latin-1")
        if len(name) >= 0x40:
            raise ValueError(f"pack entry name too long: {e.name}")
        stride = (0x50 + len(e.data) + 15) & ~15
        header = bytearray(e.raw_header) if len(e.raw_header) == 0x50 else bytearray(0x50)
        header[0:0x40] = name.ljust(0x40, b"\0")
        struct.pack_into("<III", header, 0x40, 0x50, len(e.data), stride)
        header[0x4C:0x50] = header[0x4C:0x50] if e.raw_header else marker
        out += header + e.data + b"\0" * (stride - 0x50 - len(e.data))
    end = bytearray(0x50)
    struct.pack_into("<III", end, 0x40, 0x50, 0xFFFFFFFF, 0xFFFFFFFF)
    end[0x4C:0x50] = marker
    return bytes(out + end)


# ---------------------------------------------------------------- labelled texts

LABEL = re.compile(rb"^@(\d+)[ \t]*$")
OFFSET = re.compile(rb"^@ID_OFS=(-?\d+)")


def text_blocks(data: bytes) -> tuple[bytes, list[tuple[int, bytes]]]:
    """The part before the first label, and each label's block (its @ line and its
    lines), keyed by the label's id with @ID_OFS added."""
    lines = data.splitlines(keepends=True)
    head = bytearray()
    blocks: list[tuple[int, bytes]] = []
    offset = 0
    current = None
    for line in lines:
        bare = line.rstrip(b"\r\n")
        m = OFFSET.match(bare)
        if m:
            offset = int(m.group(1))
            if current is None:
                head += line
            else:
                blocks.append((None, line))
            continue
        m = LABEL.match(bare)
        if m:
            current = offset + int(m.group(1))
            blocks.append((current, line))
            continue
        if current is None:
            head += line
        else:
            ident, text = blocks[-1]
            blocks[-1] = (ident, text + line)
    return bytes(head), blocks


def labels(data: bytes) -> list[int]:
    return [i for i, _ in text_blocks(data)[1] if i is not None]


def merge_text(pal: bytes, us: bytes, pal_en: bytes | None = None) -> tuple[bytes, int]:
    """The PAL text, and after it each US label it lacks (under an @ID_OFS=0 so the ids
    stay): the PAL line PAL English has the same words under another label (a line the
    PAL build renumbered), or else the US English one. Returns the text and how many
    lines stayed English."""
    have = set(labels(pal))
    missing = [(i, b) for i, b in text_blocks(us)[1] if i is not None and i not in have]
    if not missing:
        return pal, 0

    def body(block: bytes) -> bytes:
        return block.split(b"\n", 1)[1].strip() if b"\n" in block else b""

    renumbered = {}
    if pal_en:
        lang_blocks = {i: b for i, b in text_blocks(pal)[1] if i is not None}
        by_words: dict[bytes, int] = {}
        for i, b in text_blocks(pal_en)[1]:
            if i is not None and i in lang_blocks:
                by_words.setdefault(body(b), i)
        for i, b in missing:
            twin = by_words.get(body(b))
            if twin is not None and body(b):
                renumbered[i] = lang_blocks[twin]
    english = 0
    resolved = []
    for i, b in missing:
        if i in renumbered:
            resolved.append((i, renumbered[i]))
        else:
            resolved.append((i, b))
            english += 1
    missing = resolved
    newline = b"\r\n" if b"\r\n" in pal else b"\n"
    out = bytearray(pal)
    if not out.endswith(newline):
        out += newline
    if OFFSET.search(pal) or b"\n@ID_OFS=" in pal or pal.startswith(b"@ID_OFS="):
        out += b"@ID_OFS=0" + newline
    for ident, block in missing:
        # The block's own @ line carried the US file's offset; write the plain id.
        first, _, rest = block.partition(b"\n")
        out += b"@%d" % ident + newline + rest
        if not out.endswith(b"\n"):
            out += newline
    return bytes(out), english


# ---------------------------------------------------------------- cfg tables

@dataclass
class CfgEntry:
    ident: int
    types: tuple[int, ...]
    params: list  # int for numbers, bytes (or None) for strings


def cfg_read(data: bytes) -> list[CfgEntry] | None:
    if len(data) < 16:
        return None
    count, st, ssize, scount = struct.unpack_from("<4I", data, 0)
    if st > len(data) or st + ssize > len(data) or count > len(data) // 4:
        return None
    entries = []
    at = 0x10
    try:
        for _ in range(count):
            ident, pc = struct.unpack_from("<HB", data, at)
            tb = (pc + 3) // 4
            types = []
            for k in range(pc):
                types.append((data[at + 3 + k // 4] >> (2 * (k % 4))) & 3)
            at += (3 + tb + 3) & ~3
            params = []
            for k in range(pc):
                v = struct.unpack_from("<I", data, at + 4 * k)[0]
                if types[k] == 0:
                    if v == 0xFFFFFFFF:
                        params.append(None)
                    else:
                        if v >= ssize:
                            return None
                        end = data.index(b"\0", st + v)
                        params.append(data[st + v:end])
                else:
                    params.append(v)
            at += 4 * pc
            entries.append(CfgEntry(ident, tuple(types), params))
    except (struct.error, ValueError, IndexError):
        return None
    if at > st:
        return None
    return entries


def cfg_write(entries: list[CfgEntry]) -> bytes:
    strings = bytearray()
    offsets: dict[bytes, int] = {}
    body = bytearray()
    for e in entries:
        pc = len(e.types)
        head = bytearray(struct.pack("<HB", e.ident, pc))
        tb = bytearray((pc + 3) // 4)
        for k, t in enumerate(e.types):
            tb[k // 4] |= t << (2 * (k % 4))
        head += tb
        head += b"\xff" * (((3 + len(tb) + 3) & ~3) - len(head))
        body += head
        for t, p in zip(e.types, e.params):
            if t == 0:
                if p is None:
                    body += struct.pack("<I", 0xFFFFFFFF)
                else:
                    if p not in offsets:
                        offsets[p] = len(strings)
                        strings += p + b"\0"
                    body += struct.pack("<I", offsets[p])
            else:
                body += struct.pack("<I", p)
    st = 0x10 + len(body)
    out = bytearray(struct.pack("<4I", len(entries), st, len(strings), len(offsets)))
    out += body + strings
    out += b"\xff" * (-len(out) % 16)
    return bytes(out)


def merge_cfg(us: bytes, pal_en: bytes, pal_lang: bytes) -> tuple[bytes, int] | None:
    """The US table with the PAL language's strings, where PAL English and the PAL
    language differ (a name or a line, not an asset name). None if a table does
    not parse or the PAL pair does not have one structure."""
    u, e, l = cfg_read(us), cfg_read(pal_en), cfg_read(pal_lang)
    if u is None or e is None or l is None or len(e) != len(l):
        return None
    if any(a.ident != b.ident for a, b in zip(e, l)):
        return None
    # A record of another shape in the language (one that lists no names there, as
    # 16 of insector_name_param's in French) keeps the US words; the rest is merged.

    def key(x: CfgEntry):
        first = next((p for t, p in zip(x.types, x.params) if t == 0 and p), None)
        return (x.ident, x.types, first)

    pairs = []
    same_shape = [(x.ident, x.types) for x in u] == [(x.ident, x.types) for x in e]
    if same_shape:
        # The same records in the same order, unless the names say otherwise (a
        # record moved): then search as below.
        named = [(a, b) for a, b in zip(u, e) if key(a)[2] is not None]
        moved = sum(1 for a, b in named if key(a)[2] != key(b)[2])
        same_shape = moved <= max(2, len(named) // 50)
    if same_shape:
        opcodes = [("equal", 0, len(u), 0, len(e))]
    else:
        opcodes = difflib.SequenceMatcher(None, [key(x) for x in u], [key(x) for x in e],
                                          autojunk=False).get_opcodes()
    for tag, i1, i2, j1, j2 in opcodes:
        if tag == "equal":
            pairs += zip(range(i1, i2), range(j1, j2))
        elif tag == "replace" and i2 - i1 == j2 - j1:
            # Same shape, other first string (a spelling): pair them when ids and types agree.
            pairs += [(i, j) for i, j in zip(range(i1, i2), range(j1, j2))
                      if u[i].ident == e[j].ident and u[i].types == e[j].types]
    changed = 0
    for i, j in pairs:
        if e[j].types != l[j].types or u[i].types != e[j].types:
            continue
        for k, t in enumerate(u[i].types):
            if t == 0 and e[j].params[k] != l[j].params[k] and l[j].params[k] is not None:
                u[i].params[k] = l[j].params[k]
                changed += 1
    if not changed:
        return us, 0
    return cfg_write(u), changed


LAYOUT_GROUP = 13   # [widget id, sprite count], then that many LAYOUT_SPRITE records
LAYOUT_SPRITE = 14  # [x, y, w, h, u, v, texture w, texture h, ...]: a rectangle of a picture


def layout_groups(entries: list[CfgEntry]) -> list[tuple[int, int, int]]:
    """(widget id, first record, end record) of each sprite group of a layout."""
    out = []
    i = 0
    while i < len(entries):
        e = entries[i]
        if e.ident == LAYOUT_GROUP and len(e.params) >= 2 and e.types[:2] == (1, 1):
            n = e.params[1]
            end = i + 1
            while end < len(entries) and end - i - 1 < n and entries[end].ident == LAYOUT_SPRITE:
                end += 1
            out.append((e.params[0], i, end))
            i = end
        else:
            i += 1
    return out


LAYOUT_TEXT_GRID = 19   # [widget id, n], then n LAYOUT_TEXT_CELL records: where a text list's rows go
LAYOUT_TEXT_CELL = 20   # [x, y, ...]
LAYOUT_TEXT_WIDTH = 34  # [widget id, width]: how wide a text box is


def merge_layout_text(us: list[CfgEntry], pal_lang: list[CfgEntry]) -> tuple[list[CfgEntry], int]:
    """The text geometry of the PAL layout (its text lists' rows and its text boxes'
    widths, made for the European words) on the US layout, by widget id (nth
    occurrence), when the records have the same shape."""
    def keyed(entries):
        out: dict[int, list[tuple[int, int]]] = {}
        i = 0
        while i < len(entries):
            e = entries[i]
            if e.ident == LAYOUT_TEXT_GRID and e.types[:2] == (1, 1):
                end = i + 1
                while end < len(entries) and end - i - 1 < e.params[1] and entries[end].ident == LAYOUT_TEXT_CELL:
                    end += 1
                out.setdefault((LAYOUT_TEXT_GRID, e.params[0]), []).append((i, end))
                i = end
                continue
            if e.ident == LAYOUT_TEXT_WIDTH and e.types[:1] == (1,):
                out.setdefault((LAYOUT_TEXT_WIDTH, e.params[0]), []).append((i, i + 1))
            i += 1
        return out

    pal_blocks = keyed(pal_lang)
    us_blocks = sorted(((a, b, key, n) for key, spans in keyed(us).items() for n, (a, b) in enumerate(spans)))
    out: list[CfgEntry] = []
    at = 0
    changed = 0
    for a, b, key, n in us_blocks:
        options = pal_blocks.get(key, [])
        if n >= len(options):
            continue
        pa, pb = options[n]
        mine, theirs = us[a:b], pal_lang[pa:pb]
        if len(mine) != len(theirs) or any(x.ident != y.ident or x.types != y.types for x, y in zip(mine, theirs)):
            continue
        if all(x.params == y.params for x, y in zip(mine, theirs)):
            continue
        out += us[at:a]
        out += [CfgEntry(y.ident, y.types, list(y.params)) for y in theirs]
        at = b
        changed += 1
    out += us[at:]
    return out, changed


def widen_text_cells(us: list[CfgEntry], pal_lang: list[CfgEntry]) -> tuple[list[CfgEntry], int]:
    """A text line drawn with the menus' font is a sprite group of identical cells, one a
    letter (UiLayoutStringGlyphs fills them); a longer word overruns the group, onto the
    next ones' cells. Where the PAL layout gives such a line more letters (a text record
    [widget id, letters, y] or a bigger group), the US group gets that many cells."""
    letters: dict[int, int] = {}
    for e in pal_lang:
        if e.ident == LAYOUT_TEXT_WIDTH and len(e.params) >= 2 and e.types[:2] == (1, 1):
            letters[e.params[0]] = max(letters.get(e.params[0], 0), e.params[1])
    for wid, a, b in layout_groups(pal_lang):
        letters[wid] = max(letters.get(wid, 0), b - a - 1)
    out: list[CfgEntry] = []
    at = 0
    widened = 0
    for wid, a, b in layout_groups(us):
        cells = us[a + 1:b]
        want = letters.get(wid, 0)
        if len(cells) < 2 or want <= len(cells) or any(c.params != cells[0].params for c in cells):
            continue
        out += us[at:a]
        head = us[a]
        out.append(CfgEntry(head.ident, head.types, [wid, want] + list(head.params[2:])))
        out += [CfgEntry(cells[0].ident, cells[0].types, list(cells[0].params)) for _ in range(want)]
        at = b
        widened += 1
    out += us[at:]
    return out, widened


LAYOUT_OBJECT = 2     # [index]: an object starts
LAYOUT_TEXTURE = 6    # [name]: the object's texture, a picture of an IM3 pack
LAYOUT_TEMPLATE = 3   # [widget id]: the sprite group the object is drawn as


def im3_pictures(data: bytes) -> dict[bytes, bytes]:
    """The pictures of an IM3 picture pack (.img) by texture name: 0x40-byte entries
    from 0x10, a name of 0x20 bytes, the picture's TIM2 at the offset at +0x24 (up to
    the next one)."""
    if data[:4] != b"IM3\0" or len(data) < 16:
        return {}
    count = min(struct.unpack_from("<I", data, 8)[0], (len(data) - 0x10) // 0x40)
    entries = []
    for i in range(count):
        at = 0x10 + 0x40 * i
        name = data[at:at + 0x20].split(b"\0")[0]
        offset = struct.unpack_from("<I", data, at + 0x24)[0]
        if name and offset < len(data):
            entries.append((offset, name))
    entries.sort()
    pictures = {}
    for k, (offset, name) in enumerate(entries):
        end = entries[k + 1][0] if k + 1 < len(entries) else len(data)
        pictures[name] = data[offset:end]
    return pictures


def im3_names(data: bytes) -> list[bytes]:
    """The picture names of an IM3 pack, in entry order."""
    if data[:4] != b"IM3\0" or len(data) < 16:
        return []
    count = min(struct.unpack_from("<I", data, 8)[0], (len(data) - 0x10) // 0x40)
    return [data[0x10 + 0x40 * i:0x30 + 0x40 * i].split(b"\0")[0] for i in range(count)]


def keep_picture_names(us_im3: bytes, pal_im3: bytes) -> bytes:
    """The PAL IM3 with the US names on pictures the PAL disc renamed: the executable
    asks for a picture by its US name (e031_bn_en.img holds "e031" in the US, "e031_bn"
    in PAL). A renamed picture takes, in order, a US name the PAL pack lacks."""
    us_names, pal_names = im3_names(us_im3), im3_names(pal_im3)
    missing = [n for n in us_names if n not in pal_names]
    if not missing or len(us_names) != len(pal_names):
        return pal_im3
    out = bytearray(pal_im3)
    for i, name in enumerate(pal_names):
        if name not in us_names and missing:
            out[0x10 + 0x40 * i:0x30 + 0x40 * i] = missing.pop(0).ljust(0x20, b"\0")[:0x20]
    return bytes(out)


def layout_textures(entries: list[CfgEntry]) -> dict[int, set[bytes]]:
    """The textures each sprite group (widget id) is drawn with by the layout's objects."""
    uses: dict[int, set[bytes]] = {}
    texture = None
    for e in entries:
        if e.ident == LAYOUT_OBJECT:
            texture = None
        elif e.ident == LAYOUT_TEXTURE and e.types == (0,):
            texture = e.params[0]
        elif e.ident == LAYOUT_TEMPLATE and e.types[:1] == (1,) and texture:
            uses.setdefault(e.params[0], set()).add(texture)
    return uses


def signed(value: int) -> int:
    return value - (1 << 32) if value >= 1 << 31 else value


def sprite_extent(group: list[CfgEntry]) -> tuple[int, int, int, int] | None:
    """The box (left, top, right, bottom) a sprite group's records 14 [x, y, w, h, ...] cover."""
    boxes = [(signed(e.params[0]), signed(e.params[1]), signed(e.params[2]), signed(e.params[3]))
             for e in group if e.ident == LAYOUT_SPRITE and len(e.params) >= 4]
    if not boxes:
        return None
    return (min(x for x, _, _, _ in boxes), min(y for _, y, _, _ in boxes),
            max(x + w for x, _, w, _ in boxes), max(y + h for _, y, _, h in boxes))


def roomier(pal: list[CfgEntry], us: list[CfgEntry]) -> bool:
    """The PAL group has the US one's pieces and covers at least its box: room the PAL
    layout gives longer words (a frame or a panel made wider)."""
    pieces = [e for e in pal if e.ident == LAYOUT_SPRITE]
    if len(pieces) != sum(1 for e in us if e.ident == LAYOUT_SPRITE):
        return False
    a, b = sprite_extent(pal), sprite_extent(us)
    return a is not None and b is not None and a[0] <= b[0] and a[1] <= b[1] and a[2] >= b[2] and a[3] >= b[3]


def merge_layout_sprites(us: list[CfgEntry], pal_lang: list[CfgEntry], pal_en: list[CfgEntry] | None,
                         every: bool, words: set[bytes] = frozenset()) -> tuple[list[CfgEntry], int]:
    """The US layout with sprite groups of the PAL layout (same widget id, nth
    occurrence): the rectangles of the PAL pictures, the US ids. Taken: those drawn
    with a texture of `words` (the pictures the PAL language has otherwise, taken from
    the PAL disc); with `every` (its pack's pictures moved in the atlas), those drawn
    with no texture the layout names; those the PAL language's layout changes against
    PAL English (a word's picture); and those that give the words more room. Other
    groups of a picture both discs have alike stay the US ones: there the PAL layout
    was redrawn (its load menu's slot buttons, narrowed and cut otherwise for words it
    writes as text where the US layout has a picture of them), and the US layout's
    other pieces still go with the US groups."""
    textures = layout_textures(us)
    def groups_of(entries):
        out: dict[int, list[tuple[int, int]]] = {}
        for wid, a, b in layout_groups(entries):
            out.setdefault(wid, []).append((a, b))
        return out

    def flat(entries):
        return [(x.ident, x.types, x.params) for x in entries]

    pal_groups = groups_of(pal_lang)
    en_groups = groups_of(pal_en) if pal_en is not None else {}
    seen: dict[int, int] = {}
    out: list[CfgEntry] = []
    at = 0
    replaced = 0
    for wid, a, b in layout_groups(us):
        n = seen.get(wid, 0)
        seen[wid] = n + 1
        options = pal_groups.get(wid, [])
        if n >= len(options):
            continue
        pa, pb = options[n]
        group = pal_lang[pa:pb]
        if flat(group) == flat(us[a:b]):
            continue
        drawn_with = textures.get(wid, set())
        if not (every and not drawn_with) and not (drawn_with & words) and not roomier(group, us[a:b]):
            english = en_groups.get(wid, [])
            if n >= len(english) or flat(pal_en[english[n][0]:english[n][1]]) == flat(group):
                continue
        out += us[at:a]
        out += [CfgEntry(x.ident, x.types, list(x.params)) for x in group]
        at = b
        replaced += 1
    out += us[at:]
    return out, replaced


# ---------------------------------------------------------------- the menus' glyph list

def encode_glyph_list(text: bytes) -> bytes:
    """menu_font_setting: the first line lists the glyphs; each accent tag becomes one byte."""
    first, sep, rest = text.partition(b"\n")
    out = bytearray()
    i = 0
    while i < len(first):
        for n, tag in enumerate(ACCENT_TAGS):
            t = tag.encode("ascii")
            if first.startswith(t, i):
                out.append(ACCENT_FIRST_BYTE + n)
                i += len(t)
                break
        else:
            out.append(first[i])
            i += 1
    return bytes(out) + sep + rest


# ---------------------------------------------------------------- building

LANG_TOKEN = re.compile(r"(_|\\)(en)(?=[._\\])")


def to_language(name: str, code: str) -> str:
    """A US name with the English token(s) as the language's: `_en.`, `\\en\\`, `_en_`."""
    return LANG_TOKEN.sub(lambda m: m.group(1) + code, name)


def entry_to_language(name: str, code: str) -> str:
    return re.sub(r"_EN(?=[._])", "_" + code.upper(), name, flags=re.IGNORECASE)


def is_text(name: str) -> bool:
    return name.lower().endswith(".txt")


def is_table(name: str) -> bool:
    return name.lower().endswith((".cfg.bin", ".res.bin"))


@dataclass
class Report:
    files: int = 0
    english_lines: int = 0
    strings: int = 0
    sprite_groups: int = 0
    text_boxes: int = 0
    kept_us: list[str] = field(default_factory=list)


def changed_pictures(us_im3: bytes, pal_im3: bytes) -> set[bytes]:
    """The textures of an IM3 pack whose picture the PAL one has otherwise."""
    pal_pictures = im3_pictures(pal_im3)
    return {name for name, picture in im3_pictures(us_im3).items() if pal_pictures.get(name) != picture}


def word_textures(us: Disc, pal: Disc, code: str) -> set[bytes]:
    """The textures of the pictures the language takes from the PAL disc (the IM3
    packs of the language files) that differ from the US ones: pictures with words,
    or moved in their atlas. Wherever a layout draws them, it takes the PAL cut."""
    pal_names = pal.names()
    words: set[bytes] = set()
    for name in us.names():
        target = to_language(name, code)
        if target == name or target not in pal_names:
            continue
        if name.endswith(".img"):
            words |= changed_pictures(us.read(name), pal.read(target))
        elif name.endswith(".pac"):
            pal_entries = {re.sub(r"_" + code + r"(?=[._])", "_en", e.name, flags=re.IGNORECASE).lower(): e
                           for e in pac_read(pal.read(target))}
            for e in pac_read(us.read(name)):
                lower = e.name.lower()
                if lower.endswith(".img") and re.search(r"_en[._]", lower) and lower in pal_entries:
                    words |= changed_pictures(e.data, pal_entries[lower].data)
    return words


def build_pack(us_pack: bytes, pal_pack: bytes, pal_en_pack: bytes | None, code: str, report: Report,
               where: str, words: set[bytes] = frozenset()) -> bytes:
    """A language pack: the US entries, renamed to the language, with the PAL words."""
    # The PAL language pack's entries under their US (English) names.
    pal = {re.sub(r"_" + code + r"(?=[._])", "_en", e.name, flags=re.IGNORECASE).lower(): e
           for e in pac_read(pal_pack)}
    pal_en = {e.name.lower(): e for e in pac_read(pal_en_pack)} if pal_en_pack else {}
    us_entries = pac_read(us_pack)
    language = [e for e in us_entries if re.search(r"_en[._]", e.name.lower()) and e.name.lower() in pal]
    # A PAL picture is cut up by the PAL layout. Where the US picture differs from the
    # PAL English one, the atlas moved: the layouts then take the PAL sprite groups.
    atlas_moved = any(
        not (is_text(e.name) or is_table(e.name)) and not e.name.lower().startswith("menu_font_setting")
        and (e.name.lower() not in pal_en or pal_en[e.name.lower()].data != e.data)
        for e in language)
    out = []
    for e in us_entries:
        lower = e.name.lower()
        p = pal.get(lower)
        data = e.data
        if p is not None and re.search(r"_en[._]", lower):
            if lower.startswith("menu_font_setting"):
                data = encode_glyph_list(p.data)
            elif is_text(lower):
                bridge = pal_en.get(lower)
                data, n = merge_text(p.data, e.data, bridge.data if bridge else None)
                report.english_lines += n
            elif is_table(lower):
                bridge = pal_en.get(lower)
                merged = merge_cfg(e.data, bridge.data, p.data) if bridge else None
                if merged is None:
                    report.kept_us.append(f"{where}:{e.name}")
                else:
                    data, n = merged
                    report.strings += n
                if lower.endswith(".res.bin"):
                    layout, pal_layout = cfg_read(data), cfg_read(p.data)
                    english = cfg_read(bridge.data) if bridge else None
                    if layout is not None and pal_layout is not None:
                        layout, groups = merge_layout_sprites(layout, pal_layout, english, atlas_moved, words)
                        layout, texts = merge_layout_text(layout, pal_layout)
                        layout, cells = widen_text_cells(layout, pal_layout)
                        texts += cells
                        if groups or texts:
                            data = cfg_write(layout)
                            report.sprite_groups += groups
                            report.text_boxes += texts
            else:
                data = keep_picture_names(e.data, p.data)  # a picture with words, a font page
        out.append(PacEntry(entry_to_language(e.name, code), data, e.raw_header))
    return pac_write(out)


def build_scene(us_pack: bytes, pal_pack: bytes, code: str, report: Report) -> bytes:
    pal = {e.name.lower(): e for e in pac_read(pal_pack)}
    out = []
    for e in pac_read(us_pack):
        data = e.data
        lower = e.name.lower()
        if lower.endswith("_en.txt"):
            p = pal.get(lower[:-len("_en.txt")] + f"_{code}.txt")
            bridge = pal.get(lower)
            if p is not None:
                data, n = merge_text(p.data, e.data, bridge.data if bridge else None)
                report.english_lines += n
        out.append(PacEntry(entry_to_language(e.name, code), data, e.raw_header))
    return pac_write(out)


def build_font(us_pack: bytes, pal_pack: bytes) -> bytes:
    """PAL's message font (one for its five languages, named EUR) with the US pack's
    fifth normal page: the US executable loads five, PAL's pack has four."""
    us = {e.name: e for e in pac_read(us_pack)}
    extra = us.get("FontTex_normal_EN_4.tm2")
    out = []
    for e in pac_read(pal_pack):
        if e.name == "gaiji2.img" and extra is not None:
            out.append(PacEntry("FontTex_normal_EUR_4.tm2", extra.data, extra.raw_header))
        out.append(e)
    return pac_write(out)


def build_language(us: Disc, pal: Disc, code: str, report: Report) -> Iterator[tuple[str, bytes]]:
    """Every file the US executable asks for in the language that the PAL disc has words for."""
    us_names = us.names()
    pal_names = pal.names()
    words = word_textures(us, pal, code)
    for name in sorted(us_names):
        if name.startswith("event\\") and name.endswith(".pac"):
            if name not in pal_names:
                continue
            us_pack = us.read(name)
            if not any(e.name.lower().endswith("_en.txt") for e in pac_read(us_pack)):
                continue
            yield name, build_scene(us_pack, pal.read(name), code, report)
            continue
        target = to_language(name, code)
        if target == name:
            continue
        if re.match(r"meswin\\fontset_\w+_en\.pak$", name):
            # One font for the PAL disc's five languages, named EUR (its entries too:
            # src/text_language.cpp asks for the _EUR entries).
            european = name.replace("_en.", "_eur.")
            if european in pal_names:
                if name.startswith("meswin\\fontset_main_"):
                    yield target, build_font(us.read(name), pal.read(european))
                else:
                    yield target, pal.read(european)
            continue
        if target not in pal_names:
            continue
        pal_en = name if name in pal_names else None  # the PAL disc's English, the bridge
        lower = name.lower()
        if lower.endswith(".pac"):
            yield target, build_pack(us.read(name), pal.read(target),
                                     pal.read(pal_en) if pal_en else None, code, report, name, words)
        elif is_text(lower):
            data, n = merge_text(pal.read(target), us.read(name), pal.read(pal_en) if pal_en else None)
            report.english_lines += n
            yield target, data
        elif is_table(lower):
            merged = merge_cfg(us.read(name), pal.read(pal_en), pal.read(target)) if pal_en else None
            if merged is None:
                report.kept_us.append(name)
                continue
            report.strings += merged[1]
            yield target, merged[0]
        else:
            yield target, keep_picture_names(us.read(name), pal.read(target))  # pictures with words


def write_language(us: Disc, pal: Disc, code: str, out_dir: Path,
                   progress: Callable[[str], None] | None = None) -> Report:
    report = Report()
    out_dir.mkdir(parents=True, exist_ok=True)
    dat = out_dir / f"{code.upper()}.DAT"
    idx = out_dir / f"{code.upper()}.idx"
    part_dat = dat.with_name(dat.name + ".part")
    lines = [f"# {code.upper()}: archive path, first sector in {dat.name}, size (setup.py extract-european)"]
    sector = 0
    with open(part_dat, "wb") as f:
        for name, data in build_language(us, pal, code, report):
            f.write(data)
            f.write(b"\0" * (-len(data) % SECTOR))
            lines.append(f"{name}\t{sector}\t{len(data)}")
            sector += (len(data) + SECTOR - 1) // SECTOR
            report.files += 1
            if progress and report.files % 100 == 0:
                progress(f"{code.upper()}: {report.files} files")
    part_dat.replace(dat)
    idx.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return report
