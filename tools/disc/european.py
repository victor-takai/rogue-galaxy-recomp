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
- the message font: the PAL pack with the US pack's fifth normal page;
- long lines: the PAL executable breaks a line too long for its window, the
  US one does not, so scene, talk and menu message lines wider than the US
  English's get breaks (message_font_widths, wrap_text_file, wrap_table).

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
                         every: bool, words: set[bytes] = frozenset(),
                         keep: frozenset[int] = frozenset()) -> tuple[list[CfgEntry], int]:
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
        if n >= len(options) or wid in keep:
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


# The transporter's save screen: its Slot buttons' highlight (group 30911),
# drawn with btl_st01, a picture the PAL language has otherwise, is 44 wide in
# the PAL layout, but the US code (SavePortSelectDraw, 0x37B6E0) keeps the
# US buttons 64 wide and never sizes the highlight, which the PAL code
# fits to them: the PAL one left a strip of the button bare. It stays US,
# as the title's Load screen's own has.
TRANSPORT_LAYOUT = re.compile(r"transport_menu_def_", re.IGNORECASE)
TRANSPORT_KEEP = frozenset({30911})

TITLE_LAYOUT = re.compile(r"title_menu_def2_", re.IGNORECASE)
TITLE_LEGAL = 30070                               # the legal text under the menu
TITLE_MENU = (30050, 30020, 30030, 30045, 30040)  # the highlight, NEW GAME, CONTINUE, the extra row, OPTIONS
TITLE_LIFT = 24


def lift_title_menu(entries: list[CfgEntry]) -> tuple[list[CfgEntry], int]:
    """The title screen's menu 24 higher where the layout has the PAL legal text.

    The PAL legal text is three lines, its picture's top 22 above the US two
    lines' (its sprite's y below 0), and the PAL title overlay places the menu's
    widgets 24 higher than the US one, which the US executable keeps: its
    OPTIONS ran into the text. A widget is drawn at its place plus its
    sprites', and nothing reads these sprites' y, so the menu's go up here."""
    groups = layout_groups(entries)
    legal = next((a for wid, a, _ in groups if wid == TITLE_LEGAL), None)
    if (legal is None or legal + 1 >= len(entries) or entries[legal + 1].ident != LAYOUT_SPRITE
            or signed(entries[legal + 1].params[1]) >= 0):
        return entries, 0
    moved = 0
    for wid, a, b in groups:
        if wid in TITLE_MENU:
            for e in entries[a + 1:b]:
                if e.ident == LAYOUT_SPRITE:
                    e.params[1] = (signed(e.params[1]) - TITLE_LIFT) & 0xFFFFFFFF
                    moved += 1
    return entries, moved


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


# ---------------------------------------------------------------- line breaks

# The PAL texts leave a long line to the PAL executable, whose message windows
# and subtitles break their text to fit (its rewritten text code wraps through
# the font's FontWrapText); the US executable wraps only menu widgets and draws a
# message line as written, past the screen's edges. The Spanish texts write
# almost no breaks, the others some lines wider than the US windows. So a line
# wider than its window gets breaks here, at the spaces where the next word would
# not fit (FontWrapText's rule), measured with the US message font's widths.

FONT_TABLE = 0x44E850  # US: a glyph code each, its character and widths in the three fonts
MESSAGE_FONT = 1  # names 0, messages 1, system 2 (TextStyleGetFontType)
# The accent tags' widths in the message font (src/text_language.cpp kAccentWidths).
ACCENT_WIDTHS = (6, 6, 6, 6, 6, 10, 12, 16, 4, 10, 10, 10, 6, 16, 16, 16, 16, 16, 16, 14, 12, 12, 12, 12,
                 8, 8, 10, 10, 12, 14, 14, 14, 14, 12, 12, 12, 12, 10, 10, 10, 10, 10, 10, 10, 10, 10, 8, 8,
                 10, 10, 10, 10, 10, 10, 10, 14, 10, 10, 10, 10, 10, 10, 10, 10)
SPACE_WIDTH = 8  # the text style's (TextStyleGetSpaceWidth); a guess, the same for every line
NAME_WIDTH = 60  # a %s, a name filled in at run time
# The windows: the widest line the US English writes in them, measured the same
# way (they fit). Scene subtitles span the screen (512); talk windows are narrower.
SCENE_WIDTH = 490
TALK_WIDTH = 430
TALK_LINES = 3  # lines on a talk window page, the most US English puts there
TALK_TABLE = re.compile(r"npctt|ptalk_text", re.IGNORECASE)
# Another message file's window is as wide as its widest US line; a line just
# past it (a few pixels) is left alone rather than broken.
WIDTH_SLACK = 8

TOKEN = re.compile(rb"<[^<>\r\n]{1,8}>|%[0-9]*[sd]|.", re.S)
BREAK = re.compile(rb"(\r?\n|\\n|<page>)")


def message_font_widths(elf: bytes) -> dict[int, int]:
    """Each character's width in the US message font, from the executable's table
    (FontMapAsciiChar's characters, FontGetCharWidth's widths)."""
    phoff = struct.unpack_from("<I", elf, 0x1C)[0]
    phentsize, phnum = struct.unpack_from("<HH", elf, 0x2A)
    widths = {}
    for i in range(phnum):
        kind, offset, vaddr, _, size = struct.unpack_from("<5I", elf, phoff + i * phentsize)
        if kind == 1 and vaddr <= FONT_TABLE < vaddr + size:
            at = offset + FONT_TABLE - vaddr
            for code in range(1, 256):
                char, *fonts = elf[at + 4 * code:at + 4 * code + 4]
                if fonts[0] == 0:  # the table's end (FontMapAsciiChar stops there)
                    break
                if char:
                    widths[char] = fonts[MESSAGE_FONT]
    return widths


def line_width(line: bytes, widths: dict[int, int]) -> int:
    total = 0
    for token in TOKEN.findall(line):
        if token == b" ":
            total += SPACE_WIDTH
        elif token.startswith(b"<") and len(token) > 1:
            tag = token.decode("latin-1")
            total += ACCENT_WIDTHS[ACCENT_TAGS.index(tag)] if tag in ACCENT_TAGS else 0
        elif token.startswith(b"%") and len(token) > 1:
            total += NAME_WIDTH
        else:
            total += widths.get(token[0], 12)
    return total


def text_width(text: bytes, widths: dict[int, int]) -> int:
    """The widest line of a text (lines end at a newline, a written \\n or <page>)."""
    return max((line_width(part, widths) for part in BREAK.split(text) if not BREAK.fullmatch(part)), default=0)


def wrap_line(line: bytes, limit: int, newline: bytes, widths: dict[int, int]) -> tuple[bytes, int]:
    """The line with `newline` in place of each space where the next word would
    not fit in `limit`; and how many breaks went in."""
    if line_width(line, widths) <= limit:
        return line, 0
    out = []
    current = None
    for word in line.split(b" "):
        candidate = word if current is None else current + b" " + word
        if current is not None and word and line_width(candidate, widths) > limit:
            out.append(current)
            current = word
        else:
            current = candidate
    out.append(current)
    return newline.join(out), len(out) - 1


def wrap_text(text: bytes, limit: int, newline: bytes, widths: dict[int, int]) -> tuple[bytes, int]:
    parts = BREAK.split(text)
    breaks = 0
    for i, part in enumerate(parts):
        if part and not BREAK.fullmatch(part):
            parts[i], n = wrap_line(part, limit, newline, widths)
            breaks += n
    return b"".join(parts), breaks


def wrap_text_file(data: bytes, limit: int, widths: dict[int, int]) -> tuple[bytes, int]:
    """A labelled text file with its long lines broken (a newline inside a line's
    text, as the files write their own breaks)."""
    head, blocks = text_blocks(data)
    out = bytearray(head)
    breaks = 0
    for ident, block in blocks:
        if ident is None:
            out += block
            continue
        first, sep, rest = block.partition(b"\n")
        out += first + sep
        lines = rest.splitlines(keepends=True)
        k = 0
        while k < len(lines):
            line = lines[k]
            body = line.rstrip(b"\r\n")
            if body.startswith(b"//") or body.startswith(b"@"):
                out += line
                k += 1
                continue
            # A paragraph: this line and those after it that go on with its
            # sentence. One too long is laid out again whole, so its rest does
            # not stand alone on a line ("(para" / "PlayStation®2)" / "de la
            # ranura...").
            end = k + 1
            while body and end < len(lines) and continues(lines[end - 1].rstrip(b"\r\n"),
                                                          lines[end].rstrip(b"\r\n")):
                end += 1
            bodies = [x.rstrip(b"\r\n") for x in lines[k:end]]
            if (end - k > 1 and any(line_width(b, widths) > limit for b in bodies)
                    and not any(BREAK.search(b) for b in bodies)):
                each = [[w for w in b.split(b" ") if w] for b in bodies]
                laid = balanced_lines([w for line_words in each for w in line_words], limit, widths)
                out += b"\n".join(laid) + lines[end - 1][len(bodies[-1]):]
                breaks += len(line_ends([len(x.split(b" ")) for x in laid]) - line_ends([len(x) for x in each]))
                k = end
                continue
            wrapped, n = wrap_text(body, limit, b"\n", widths)
            out += wrapped + line[len(body):]
            breaks += n
            k += 1
    return bytes(out), breaks


# The accent tags of lower-case letters (<`a>, <'e>, <:u>, <~n>, <oe>, <ss>...) and of
# capitals (<`A>, <,C>, <~N>, <OE>...).
LOWER_ACCENTS = tuple(t.encode("latin-1") for t in ACCENT_TAGS if len(t) == 4 and t[2].islower())
UPPER_ACCENTS = tuple(t.encode("latin-1") for t in ACCENT_TAGS if len(t) == 4 and t[2].isupper())
# A line that ends a sentence: a stop, maybe in quotes or brackets.
SENTENCE_END = re.compile(rb"[.!?:][\x22'\xbb)\]]*\s*$")


def continues(previous: bytes, line: bytes) -> bool:
    """A message's line that goes on with the sentence of the line before it: it starts
    with a lower-case letter, accented ones included, or with a capital (a name, a
    German noun) after a line that ends no sentence."""
    if not line:
        return False
    if 0x61 <= line[0] <= 0x7A or line.startswith(LOWER_ACCENTS):
        return True
    return (0x41 <= line[0] <= 0x5A or line.startswith(UPPER_ACCENTS)) and not SENTENCE_END.search(previous)


def line_ends(counts: list[int]) -> set[int]:
    """Where lines of so many words each break, counted in words."""
    ends, total = set(), 0
    for count in counts[:-1]:
        total += count
        ends.add(total)
    return ends


def balanced_lines(words: list[bytes], limit: int, widths: dict[int, int]) -> list[bytes]:
    """The words in as few lines as fit `limit`, the longest line as short as it can
    be (a word wider than `limit` alone on its line)."""
    sizes = [line_width(w, widths) for w in words]
    n = len(words)
    before = [0]
    for size in sizes:
        before.append(before[-1] + size)

    def span(i: int, j: int) -> int:
        return before[j] - before[i] + SPACE_WIDTH * (j - i - 1)

    count, start = 0, 0
    while start < n:  # the fewest lines, greedily
        end = start + 1
        while end < n and span(start, end + 1) <= limit:
            end += 1
        count, start = count + 1, end
    worst = float("inf")
    best = [[worst] * (n + 1) for _ in range(count + 1)]
    cut = [[0] * (n + 1) for _ in range(count + 1)]
    best[0][0] = 0
    for k in range(1, count + 1):
        for j in range(1, n + 1):
            for i in range(k - 1, j):
                width = span(i, j)
                if best[k - 1][i] == worst or (width > limit and j - i > 1):
                    continue
                longest = max(best[k - 1][i], width)
                if longest < best[k][j]:
                    best[k][j], cut[k][j] = longest, i
    out, j = [], n
    for k in range(count, 0, -1):
        i = cut[k][j]
        out.append(b" ".join(words[i:j]))
        j = i
    return out[::-1]


def split_pages(text: bytes, lines: int) -> bytes:
    """A talk window's text with a page wherever one would hold more than `lines`
    lines (the US talk texts never put more on a page): its next line starts a
    new page (<page> and the line's \\n)."""
    pages = text.split(b"<page>")
    out = []
    for page in pages:
        parts = page.split(b"\\n")
        lead = parts[0] == b""  # a page after <page> starts with its \n
        body = parts[1:] if lead else parts
        if len(body) <= lines:
            out.append(page)
            continue
        chunks = [b"\\n".join(body[i:i + lines]) for i in range(0, len(body), lines)]
        out.append((b"\\n" if lead else b"") + b"<page>\\n".join(chunks))
    return b"<page>".join(out)


def wrap_table(data: bytes, limit: int, widths: dict[int, int], lines: int = 0) -> tuple[bytes, int]:
    """A cfg table with its long strings broken (a written \\n, as the tables
    write their own breaks), and with `lines`, no page holding more than that."""
    entries = cfg_read(data)
    if entries is None:
        return data, 0
    breaks = 0
    for entry in entries:
        for k, param in enumerate(entry.params):
            if isinstance(param, bytes):
                wrapped, n = wrap_text(param, limit, b"\\n", widths)
                if lines:
                    paged = split_pages(wrapped, lines)
                    n += paged != wrapped
                    wrapped = paged
                if n:
                    entry.params[k] = wrapped
                    breaks += n
    return (cfg_write(entries), breaks) if breaks else (data, 0)


def file_window(us_data: bytes, widths: dict[int, int]) -> int:
    """A message file's window width: its widest US English line, with the slack."""
    widest = 0
    for ident, block in text_blocks(us_data)[1]:
        if ident is not None and b"\n" in block:
            widest = max(widest, text_width(block.split(b"\n", 1)[1], widths))
    return widest + WIDTH_SLACK if widest else 0


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
    line_breaks: int = 0
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
               where: str, words: set[bytes] = frozenset(), widths: dict[int, int] | None = None) -> bytes:
    """A language pack: the US entries, renamed to the language, with the PAL words
    (and, given the message font's widths, their long lines broken)."""
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
                if widths:
                    window = file_window(e.data, widths)
                    if window:
                        data, n = wrap_text_file(data, window, widths)
                        report.line_breaks += n
            elif is_table(lower):
                bridge = pal_en.get(lower)
                merged = merge_cfg(e.data, bridge.data, p.data) if bridge else None
                if merged is None:
                    report.kept_us.append(f"{where}:{e.name}")
                else:
                    data, n = merged
                    report.strings += n
                    if widths and TALK_TABLE.search(lower):
                        data, n = wrap_table(data, TALK_WIDTH, widths, TALK_LINES)
                        report.line_breaks += n
                if lower.endswith(".res.bin"):
                    layout, pal_layout = cfg_read(data), cfg_read(p.data)
                    english = cfg_read(bridge.data) if bridge else None
                    if layout is not None and pal_layout is not None:
                        keep = TRANSPORT_KEEP if TRANSPORT_LAYOUT.search(lower) else frozenset()
                        layout, groups = merge_layout_sprites(layout, pal_layout, english, atlas_moved, words, keep)
                        layout, texts = merge_layout_text(layout, pal_layout)
                        layout, cells = widen_text_cells(layout, pal_layout)
                        texts += cells
                        if TITLE_LAYOUT.search(lower):
                            layout, lifted = lift_title_menu(layout)
                            groups += 1 if lifted else 0
                        if groups or texts:
                            data = cfg_write(layout)
                            report.sprite_groups += groups
                            report.text_boxes += texts
            else:
                data = keep_picture_names(e.data, p.data)  # a picture with words, a font page
        out.append(PacEntry(entry_to_language(e.name, code), data, e.raw_header))
    return pac_write(out)


def build_scene(us_pack: bytes, pal_pack: bytes, code: str, report: Report,
                widths: dict[int, int] | None = None) -> bytes:
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
                if widths:
                    data, n = wrap_text_file(data, SCENE_WIDTH, widths)
                    report.line_breaks += n
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


def build_language(us: Disc, pal: Disc, code: str, report: Report,
                   widths: dict[int, int] | None = None) -> Iterator[tuple[str, bytes]]:
    """Every file the US executable asks for in the language that the PAL disc has words for;
    given the US message font's widths (message_font_widths), with long lines broken."""
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
            yield name, build_scene(us_pack, pal.read(name), code, report, widths)
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
                                     pal.read(pal_en) if pal_en else None, code, report, name, words, widths)
        elif is_text(lower):
            us_data = us.read(name)
            data, n = merge_text(pal.read(target), us_data, pal.read(pal_en) if pal_en else None)
            report.english_lines += n
            window = file_window(us_data, widths) if widths else 0
            if window:
                data, n = wrap_text_file(data, window, widths)
                report.line_breaks += n
            yield target, data
        elif is_table(lower):
            merged = merge_cfg(us.read(name), pal.read(pal_en), pal.read(target)) if pal_en else None
            if merged is None:
                report.kept_us.append(name)
                continue
            report.strings += merged[1]
            data = merged[0]
            if widths and TALK_TABLE.search(lower):
                data, n = wrap_table(data, TALK_WIDTH, widths, TALK_LINES)
                report.line_breaks += n
            yield target, data
        else:
            yield target, keep_picture_names(us.read(name), pal.read(target))  # pictures with words


def write_language(us: Disc, pal: Disc, code: str, out_dir: Path,
                   progress: Callable[[str], None] | None = None,
                   widths: dict[int, int] | None = None) -> Report:
    report = Report()
    out_dir.mkdir(parents=True, exist_ok=True)
    dat = out_dir / f"{code.upper()}.DAT"
    idx = out_dir / f"{code.upper()}.idx"
    part_dat = dat.with_name(dat.name + ".part")
    lines = [f"# {code.upper()}: archive path, first sector in {dat.name}, size (setup.py extract-european)"]
    sector = 0
    with open(part_dat, "wb") as f:
        for name, data in build_language(us, pal, code, report, widths):
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
