#!/usr/bin/env python3
"""vram2png.py OUT.png A.vram [B.vram ...]: the display buffer (PSMCT32 at
block 0, FBW 8: 512 x 448) of each GS memory dump, side by side, and with two
a difference image (red: any channel differs by more than 8 levels)."""
import struct, sys, zlib

W, H, FBW = 512, 448, 8
BLOCKS = [[0, 1, 4, 5, 16, 17, 20, 21], [2, 3, 6, 7, 18, 19, 22, 23],
          [8, 9, 12, 13, 24, 25, 28, 29], [10, 11, 14, 15, 26, 27, 30, 31]]
COLUMN = [[0, 1, 4, 5, 8, 9, 12, 13], [2, 3, 6, 7, 10, 11, 14, 15]]

def offsets():
    out = []
    for y in range(H):
        for x in range(W):
            page = (y // 32) * FBW + x // 64
            px, py = x % 64, y % 32
            block = BLOCKS[py // 8][px // 8]
            bx, by = px % 8, py % 8
            word = page * 2048 + block * 64 + (by // 2) * 16 + COLUMN[by % 2][bx]
            out.append(word * 4)
    return out

def image(path, where):
    vram = open(path, "rb").read()
    rgb = bytearray(W * H * 3)
    for i, o in enumerate(where):
        rgb[i * 3:i * 3 + 3] = vram[o:o + 3]
    return bytes(rgb)

def write_png(path, w, h, rgb):
    raw = b"".join(b"\0" + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))
    def chunk(t, d): return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    open(path, "wb").write(png + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))

where = offsets()
images = [image(p, where) for p in sys.argv[2:]]
if len(images) == 2:
    a, b = images
    diff, n = bytearray(len(a)), 0
    for k in range(0, len(a), 3):
        d = max(abs(a[k] - b[k]), abs(a[k + 1] - b[k + 1]), abs(a[k + 2] - b[k + 2]))
        if d > 8:
            n += 1
            diff[k] = 255
        else:
            g = (a[k] + a[k + 1] + a[k + 2]) // 12
            diff[k] = diff[k + 1] = diff[k + 2] = g
    images.append(bytes(diff))
    print("%s: %d of %d pixels differ by more than 8 levels (%.2f%%)" % (sys.argv[1], n, W * H, 100.0 * n / (W * H)))
rows = [b"".join(img[y * W * 3:(y + 1) * W * 3] for img in images) for y in range(H)]
write_png(sys.argv[1], W * len(images), H, b"".join(rows))
