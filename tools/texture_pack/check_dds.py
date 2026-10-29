"""Checks the game's DDS reader (src/dds.h, src/bc7.h) against Pillow's.

check_dds.py CHECKER_EXE [BLOCKS=2000]: writes random BC7 blocks of every mode,
and BC1, BC3 and uncompressed pictures, as DDS files, has the checker (built from
check_dds.cpp) decode them, and compares with what Pillow makes of the same
files. BC7 and BC1/BC3 must match exactly (Pillow's BC1 has the same
rounding); a block Pillow and the format both call reserved (mode 8+) is left out.
Random blocks reach every mode and partition; a picture a real encoder made is
only a more ordinary case of them.
"""
import io
import os
import random
import struct
import subprocess
import sys
import tempfile

from PIL import Image


def header(width, height, four_cc=None, dxgi=None, flags_extra=0, rgb=None):
    pixel = struct.pack("<2I4s5I", 32, 4, four_cc or b"\0\0\0\0", 0, 0, 0, 0, 0)
    if rgb:
        pixel = struct.pack("<4I4I", 32, 0x41, 0, 32, *rgb)
    out = b"DDS " + struct.pack("<7I", 124, 0x1 | 0x2 | 0x4 | 0x1000 | flags_extra, height, width, 0, 0, 0) + b"\0" * 44
    out += pixel + struct.pack("<5I", 0x1000, 0, 0, 0, 0)
    if dxgi:
        out += struct.pack("<5I", dxgi, 3, 0, 1, 0)
    return out


def main():
    checker = sys.argv[1]
    count = int(sys.argv[2]) if len(sys.argv) > 2 else 2000
    rng = random.Random(7)
    work = tempfile.mkdtemp()
    cases = []

    # BC7: every mode, many partitions, 4x4 blocks 16 to a picture
    modes_seen = set()
    blocks = []
    while len(blocks) < count:
        block = bytearray(rng.getrandbits(8) for _ in range(16))
        mode = 8
        for m in range(8):
            if block[0] >> m & 1:
                mode = m
                break
        # even out the modes: force the mode bits
        want = len(blocks) % 8
        block[0] = (block[0] & ~((1 << (want + 1)) - 1) | (1 << want)) & 255
        modes_seen.add(want)
        blocks.append(bytes(block))
    for n in range(0, len(blocks), 16):
        chunk = blocks[n:n + 16]
        if len(chunk) < 16:
            break
        data = header(16, 16, b"DX10", 98) + b"".join(chunk)
        cases.append(("bc7_%d" % n, data))

    # BC1 and BC3 from Pillow's own encoder
    rnd = Image.new("RGBA", (32, 32))
    rnd.putdata([(rng.randrange(256), rng.randrange(256), rng.randrange(256), rng.randrange(256))
                 for _ in range(32 * 32)])
    for fmt in ("DXT1", "DXT3", "DXT5"):
        try:
            buf = io.BytesIO()
            rnd.save(buf, format="DDS", pixel_format=fmt)
            cases.append((fmt.lower(), buf.getvalue()))
        except Exception as error:  # an encoder Pillow lacks
            print("skip", fmt, error)
    # uncompressed 8-bit RGBA in two byte orders
    pixels = [(rng.randrange(256), rng.randrange(256), rng.randrange(256), rng.randrange(256)) for _ in range(64)]
    bgra = b"".join(bytes((b, g, r, a)) for r, g, b, a in pixels)
    cases.append(("bgra", header(8, 8, rgb=(0xFF0000, 0xFF00, 0xFF, 0xFF000000)) + bgra))
    rgba = b"".join(bytes(p) for p in pixels)
    cases.append(("rgba", header(8, 8, rgb=(0xFF, 0xFF00, 0xFF0000, 0xFF000000)) + rgba))

    paths = []
    for name, data in cases:
        path = os.path.join(work, name + ".dds")
        open(path, "wb").write(data)
        paths.append(path)
    subprocess.run([checker] + paths, check=True)

    bad = 0
    for (name, data), path in zip(cases, paths):
        ours = open(path + ".rgba", "rb").read()
        theirs = Image.open(io.BytesIO(data)).convert("RGBA")
        expected = theirs.tobytes()
        if ours != expected:
            diff = sum(1 for a, b in zip(ours, expected) if a != b)
            worst = max(abs(a - b) for a, b in zip(ours, expected))
            print("%s: %d of %d bytes differ (worst %d)" % (name, diff, len(ours), worst))
            bad += 1
    print("%d pictures, %d differ" % (len(cases), bad))
    return 1 if bad else 0


sys.exit(main())
