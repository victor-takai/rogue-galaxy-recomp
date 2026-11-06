"""The game's compiled configuration files (*.cfg.bin), as its CfgVm reads them.

Header: u32 op count, u32 string pool offset, u32 pool size, u32 string count.
The ops follow from 0x10: u16 id, u8 argument count, the arguments' types two
bits each (0 a string, its offset in the pool; 1 an int; 2 a float; 3 none)
packed four to a byte, then the arguments, 4 bytes each, from the next 4-byte
boundary. In the game's own files the pool starts at the next 16-byte boundary
after the ops; in the European texts' (tools/disc/european.py, cfg_write) right
after them. The file ends at the 16-byte boundary after the pool; the gaps are
0xFF. The header's pool offset says where it is, whichever the layout.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass

STRING, INT, FLOAT, NONE = 0, 1, 2, 3


def _align(x: int, n: int) -> int:
    return (x + n - 1) // n * n


@dataclass(frozen=True)
class Op:
    start: int
    end: int
    oid: int
    types: tuple[int, ...]
    slots: tuple[int, ...]  # each argument's offset in the file (none: -1)


def parse(data: bytes) -> tuple[tuple[int, int, int, int], list[Op]]:
    """(header, ops)."""
    count, pool_off, pool_size, strings = header = struct.unpack_from("<4I", data, 0)
    if pool_off > len(data) or pool_off + pool_size > len(data):
        raise ValueError("not a cfg.bin: the pool passes the end")
    ops = []
    off = 0x10
    for _ in range(count):
        oid, n = struct.unpack_from("<HB", data, off)
        type_bytes = (n * 2 + 7) // 8
        types = tuple((data[off + 3 + i // 4] >> (2 * (i % 4))) & 3 for i in range(n))
        slot = off + _align(type_bytes + 3, 4)
        slots = []
        for t in types:
            if t == NONE:
                slots.append(-1)
            else:
                slots.append(slot)
                slot += 4
        end = off + _align(_align(type_bytes + 3, 4) + n * 4, 4)
        ops.append(Op(off, end, oid, types, tuple(slots)))
        off = end
    if off > pool_off:
        raise ValueError("not a cfg.bin: the ops run into the pool")
    return header, ops


def values(data: bytes, op: Op) -> list:
    """An op's arguments: str for strings, int, float, None."""
    count, pool_off, pool_size, _ = struct.unpack_from("<4I", data, 0)
    pool = data[pool_off:pool_off + pool_size]
    out = []
    for t, s in zip(op.types, op.slots):
        if t == NONE:
            out.append(None)
            continue
        raw = data[s:s + 4]
        if t == FLOAT:
            out.append(struct.unpack("<f", raw)[0])
            continue
        v = struct.unpack("<i", raw)[0]
        if t == INT:
            out.append(v)
        else:
            out.append(pool[v:pool.index(b"\0", v)].decode("cp932", "replace") if 0 <= v < len(pool) else None)
    return out


def merge_numbers(base: bytes, numbers: bytes) -> tuple[bytes, int, int]:
    """`base` with the numbers of `numbers`: (file, ops changed, ops kept as base).

    Both must hold the same ops in the same order (the same table of two
    releases). Each op takes `numbers`' arguments, ints and floats, and keeps
    `base`'s strings, so names and texts stay `base`'s. An op whose strings sit
    in other places in the two stays `base`'s. The first two ops (the build's
    date and time) stay `base`'s.
    """
    base_header, base_ops = parse(base)
    _, num_ops = parse(numbers)
    if [o.oid for o in base_ops] != [o.oid for o in num_ops]:
        raise ValueError("the two tables do not hold the same ops")
    out = bytearray()
    changed = kept = 0
    for i, (b, n) in enumerate(zip(base_ops, num_ops)):
        b_bytes = base[b.start:b.end]
        strings_b = [j for j, t in enumerate(b.types) if t == STRING]
        strings_n = [j for j, t in enumerate(n.types) if t == STRING]
        if i < 2 or b.oid in (1, 2) or strings_b != strings_n:
            out += b_bytes
            if base[b.start:b.end] != numbers[n.start:n.end] and strings_b != strings_n:
                kept += 1
            continue
        merged = bytearray(numbers[n.start:n.end])
        for j in strings_n:
            dst = n.slots[j] - n.start
            merged[dst:dst + 4] = base[b.slots[j]:b.slots[j] + 4]
        if bytes(merged) != b_bytes:
            changed += 1
        out += merged
    count, pool_off, pool_size, strings = base_header
    pool = base[pool_off:pool_off + pool_size]
    # The strings where `base` puts them: on a 16-byte boundary after the ops, as the
    # game's own tables do, or right after them, as the European texts' (tools/disc/
    # european.py, cfg_write) do; aligning those could make the file larger than the
    # one it replaces. No gap after the ops is taken for the second way; a game table
    # whose ops end on the boundary has none either, and its pool then follows the ops
    # as they come, which the game reads by the header's offset as it does the
    # European texts'.
    packed = pool_off == 0x10 + sum(b.end - b.start for b in base_ops)
    new_pool_off = 0x10 + len(out) if packed else _align(0x10 + len(out), 16)
    head = struct.pack("<4I", count, new_pool_off, pool_size, strings)
    body = head + bytes(out)
    body += b"\xff" * (new_pool_off - len(body))
    body += pool
    body += b"\xff" * (_align(len(body), 16) - len(body))
    return body, changed, kept


def numbers_view(data: bytes) -> list[tuple]:
    """Each op's id and arguments with the strings left out, for comparing."""
    _, ops = parse(data)
    return [(o.oid, tuple(v for t, v in zip(o.types, values(data, o)) if t != STRING)) for o in ops]


def strings_view(data: bytes) -> list[tuple]:
    """Each op's id and its strings only, for comparing."""
    _, ops = parse(data)
    return [(o.oid, tuple(v for t, v in zip(o.types, values(data, o)) if t == STRING)) for o in ops]
