"""The game's archives: the DATA index (.HD6) and the packs (.pac) inside.

A layer's files sit in DATA<layer>_<n>.DAT, listed by DATA<layer>_<n>.HD6:
token-compressed names, each with its offset in sectors and its size in units.
A .pac is a run of files, each a 0x50-byte header (name, header size, data
size, stride to the next) and its data.
"""

from __future__ import annotations

import struct


def hd6_index(data: bytes, sector: int = 2048) -> dict[str, tuple[int, int]]:
    """name -> (offset in bytes, size in bytes); names lowercase, with backslashes.

    The size is rounded up to the index's unit; the file itself says how much
    of it is used (a .pac by its entries).
    """
    if data[:4] != b"HD6\0":
        raise ValueError("not an HD6 index")
    word = lambda o: struct.unpack_from("<I", data, o)[0]
    tok_off, tok_size, tok_count = word(4), word(8), word(0xC)
    name_off, unit, count, table = word(0x14), word(0x20), word(0x24), word(0x28)
    tokens = data[tok_off:tok_off + tok_size].split(b"\0")[:tok_count]
    files = {}
    for i in range(count):
        v = struct.unpack_from("<Q", data, table + 8 * i)[0]
        p = name_off + (v & 0x3FFFF)
        name = b""
        while data[p]:
            token = shift = 0
            while True:
                b = data[p]
                p += 1
                token |= (b & 0x7F) << shift
                shift += 7
                if not b & 0x80:
                    break
            name += tokens[token] if token < len(tokens) else b"?"
        key = name.decode("latin-1").replace("/", "\\").lower()
        files[key] = (((v >> 18) & 0x3FFFFF) * sector, (v >> 40) * unit)
    return files


def pac_entries(data: bytes) -> list[tuple[str, bytes]]:
    """The files of a .pac, in order: (name, data)."""
    out = []
    off = 0
    while off + 0x50 <= len(data):
        name = data[off:off + 0x40].split(b"\0")[0].decode("latin-1")
        header, size, stride = struct.unpack_from("<III", data, off + 0x40)
        if size == 0xFFFFFFFF or header == 0xFFFFFFFF:
            break
        out.append((name, data[off + header:off + header + size]))
        if stride == 0:
            break
        off += stride
    return out
