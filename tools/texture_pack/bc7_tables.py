"""Writes src/bc7_tables.h: usage bc7_tables.py OUT.h. Needs Pillow 10 or later.

Reads BC7's partition and anchor tables out of Pillow's decoder.

A block of mode 3 (two subsets) or mode 2 (three subsets) is built for each
partition, with every subset's first endpoint at 0 and its second at the top,
and every index bit set: a pixel's red then says its subset (by its
endpoints) and whether it is an anchor (an anchor's index has its top bit
implied 0, so it reads 1 and not the largest).
"""
import io
import struct
import sys

from PIL import Image


class Bits:
    def __init__(self):
        self.value = 0
        self.length = 0

    def put(self, value, count):
        self.value |= (value & ((1 << count) - 1)) << self.length
        self.length += count

    def block(self):
        assert self.length == 128, self.length
        return self.value.to_bytes(16, "little")


def dds_bc7(data, blocks_w=1, blocks_h=1):
    w, h = blocks_w * 4, blocks_h * 4
    hdr = b"DDS " + struct.pack("<7I", 124, 0x1 | 0x2 | 0x4 | 0x1000 | 0x80000, h, w, len(data), 0, 0) + b"\0" * 44
    hdr += struct.pack("<2I4s5I", 32, 4, b"DX10", 0, 0, 0, 0, 0) + struct.pack("<5I", 0x1000, 0, 0, 0, 0)
    hdr += struct.pack("<5I", 98, 3, 0, 1, 0)
    return hdr + data


def decode(block):
    im = Image.open(io.BytesIO(dds_bc7(block)))
    im.load()
    return [im.getpixel((x, y)) for y in range(4) for x in range(4)]


def probe2(partition):
    b = Bits()
    b.put(1 << 3, 4)  # mode 3
    b.put(partition, 6)
    for _ in range(3):  # R, G, B: 4 endpoints of 7 bits
        for value in (0, 127, 0, 127) if _ == 0 else (0, 0, 0, 0):
            b.put(value, 7)
    b.put(0, 4)  # p-bits
    b.put((1 << 30) - 1, 30)
    pixels = decode(b.block())
    return pixels


def probe3(partition):
    b = Bits()
    b.put(1 << 2, 3)  # mode 2
    b.put(partition, 6)
    for channel in range(3):
        for value in (0, 31, 0, 31, 0, 31) if channel == 0 else (0, 0, 0, 0, 0, 0):
            b.put(value, 5)
    b.put((1 << 29) - 1, 29)
    return decode(b.block())


def subset_probe(partition, mode):
    """Subset of each pixel: separate the subsets by their endpoints' red."""
    b = Bits()
    if mode == 3:
        b.put(1 << 3, 4)
        b.put(partition, 6)
        # subset 0: R 0..8, subset 1: 100..127, all indices 0: the pixel is e0
        reds = (0, 8, 100, 127)
        for r in reds:
            b.put(r, 7)
        for _ in range(8):
            b.put(0, 7)
        b.put(0, 4)
        b.put(0, 30)
        return [p[0] for p in decode(b.block())]
    b.put(1 << 2, 3)
    b.put(partition, 6)
    reds = (0, 3, 12, 15, 28, 31)
    for r in reds:
        b.put(r, 5)
    for _ in range(12):
        b.put(0, 5)
    b.put(0, 29)
    return [p[0] for p in decode(b.block())]


def write_header(t, path):


    def rows(table, width):
        lines = []
        for entry in table:
            lines.append("    {" + ", ".join(str(v) for v in entry) + "},")
        return "\n".join(lines)


    def row(values):
        out, line = [], "   "
        for v in values:
            item = " %d," % v
            if len(line) + len(item) > 110:
                out.append(line)
                line = "   "
            line += item
        out.append(line)
        return "\n".join(out)


    text = """// BC7's partition and anchor tables (the format's, as every decoder has them):
// which of two or three subsets each of a block's 16 pixels is in, and the
// pixel whose index drops its top bit in each subset beyond the first
// (the first subset's is pixel 0). Read out of an independent decoder,
// pixel by pixel, by tools/texture_pack/bc7_tables.py.
#pragma once

#include <cstdint>

namespace rg::bc7
{

constexpr uint8_t kPartition2[64][16] = {
%s
};

constexpr uint8_t kAnchor2[64] = {
%s
};

constexpr uint8_t kPartition3[64][16] = {
%s
};

constexpr uint8_t kAnchor3a[64] = {
%s
};

constexpr uint8_t kAnchor3b[64] = {
%s
};

} // namespace rg::bc7
    """ % (
        rows(t["partition2"], 16),
        row(t["anchor2"]),
        rows(t["partition3"], 16),
        row(t["anchor3a"]),
        row(t["anchor3b"]),
    )
    open(path, "w", newline="\n").write(text.rstrip() + "\n")



def main():
    out = {"partition2": [], "anchor2": [], "partition3": [], "anchor3a": [], "anchor3b": []}
    for p in range(64):
        reds = subset_probe(p, 3)
        low = min(reds)
        subset = [0 if r == low else 1 for r in reds]
        assert set(reds) <= {low, max(reds)}, (p, reds)
        out["partition2"].append(subset)
        pixels = probe2(p)
        top = max(px[0] for px in pixels)
        anchors = [i for i, px in enumerate(pixels) if px[0] != top and subset[i] == 1]
        # the pixel that is not at the top in subset 1; subset 0's is always 0
        a0 = [i for i, px in enumerate(pixels) if px[0] != top and subset[i] == 0]
        assert a0 == [0] or a0 == [], (p, a0)
        assert len(anchors) == 1, (p, anchors, pixels)
        out["anchor2"].append(anchors[0])
    for p in range(64):
        reds = subset_probe(p, 2)
        values = sorted(set(reds))
        assert len(values) <= 3, (p, reds)
        index = {v: i for i, v in enumerate(values)}
        subset = [index[r] for r in reds]
        out["partition3"].append(subset)
        pixels = probe3(p)
        # a pixel at its subset's top is not an anchor
        tops = {}
        for i, px in enumerate(pixels):
            tops[subset[i]] = max(tops.get(subset[i], 0), px[0])
        anchors = {}
        for i, px in enumerate(pixels):
            if px[0] != tops[subset[i]]:
                anchors.setdefault(subset[i], []).append(i)
        assert all(len(v) == 1 for v in anchors.values()), (p, anchors)
        out["anchor3a"].append(anchors.get(1, [None])[0])
        out["anchor3b"].append(anchors.get(2, [None])[0])
    write_header(out, sys.argv[1])
    print("ok", sum(1 for a in out["anchor3a"] if a is None), "partitions missing a subset")


main()
