"""2xBR (Hyllian's xBR, level 2) for pixel art: xbr.py IN.png OUT.png [TIMES].

Doubles a picture TIMES times (default 2, so 4x): each pixel becomes four,
and where the neighbourhood says an edge crosses a corner (a diagonal, or a
shallow or steep slope), that corner is blended toward the colour across the
edge; everywhere else the pixel stays as it was. The art keeps its pixels and
colours and loses its staircases. Colour distance is xBR's YUV weighting with
alpha added, and "equal" a small distance rather than exact equality, as the
game's pictures are anti-aliased. Blending is premultiplied, so transparent
pixels do not darken an edge. Pure Python: fine for icons and portraits.
"""
import sys

from PIL import Image

EQUAL = 40.0


def yuv(p):
    r, g, b, a = p
    return (0.299 * r + 0.587 * g + 0.114 * b, -0.169 * r - 0.331 * g + 0.5 * b, 0.5 * r - 0.419 * g - 0.081 * b, a)


def distance(p, q):
    return 48.0 * abs(p[0] - q[0]) + 7.0 * abs(p[1] - q[1]) + 6.0 * abs(p[2] - q[2]) + 32.0 * abs(p[3] - q[3])


def double(image):
    w, h = image.size
    src = list(image.getdata())
    pre = [(r * a / 255.0, g * a / 255.0, b * a / 255.0, float(a)) for r, g, b, a in src]
    keys = [yuv(p) for p in src]

    def at(x, y):
        return min(max(x, 0), w - 1) + min(max(y, 0), h - 1) * w

    out = [None] * (w * 2 * h * 2)
    # The corners of a pixel and, for each, how its 5x5 neighbourhood turns:
    # (dx, dy) of "right" and "down" in that corner's frame.
    frames = ((1, 0, 0, 1, 1, 1), (-1, 0, 0, 1, 0, 1), (1, 0, 0, -1, 1, 0), (-1, 0, 0, -1, 0, 0))
    for y in range(h):
        for x in range(w):
            e = at(x, y)
            corners = {}
            for rx, ry, dx, dy, cx, cy in frames:
                def n(i, j):
                    # i steps right, j steps down, in this corner's frame
                    return at(x + i * rx + j * dx, y + i * ry + j * dy)
                E, F, H, I = n(0, 0), n(1, 0), n(0, 1), n(1, 1)
                B, D, C, G = n(0, -1), n(-1, 0), n(1, -1), n(-1, 1)
                F4, H5, I4, I5 = n(2, 0), n(0, 2), n(2, 1), n(1, 2)
                k = keys
                d = distance
                if d(k[E], k[F]) < EQUAL or d(k[E], k[H]) < EQUAL:
                    continue
                across = d(k[E], k[C]) + d(k[E], k[G]) + d(k[I], k[F4]) + d(k[I], k[H5]) + 4 * d(k[H], k[F])
                along = d(k[H], k[D]) + d(k[H], k[I5]) + d(k[F], k[I4]) + d(k[F], k[B]) + 4 * d(k[E], k[I])
                if across >= along:
                    continue
                px = F if d(k[E], k[F]) <= d(k[E], k[H]) else H
                ke, ki = d(k[F], k[G]), d(k[H], k[C])
                left = ke * 2 <= ki and d(k[E], k[G]) >= EQUAL and d(k[D], k[G]) >= EQUAL
                up = ki * 2 <= ke and d(k[E], k[C]) >= EQUAL and d(k[B], k[C]) >= EQUAL
                # Output subpixels in this frame: the corner, its neighbour
                # along the slope sideways, and up.
                corner = (cx, cy)
                side = (1 - cx, cy)
                over = (cx, 1 - cy)
                if left or up:
                    corners[corner] = (px, 0.75)
                    if left:
                        corners.setdefault(side, (px, 0.25))
                    if up:
                        corners.setdefault(over, (px, 0.25))
                else:
                    corners.setdefault(corner, (px, 0.5))
            for sy in (0, 1):
                for sx in (0, 1):
                    p = pre[e]
                    blend = corners.get((sx, sy))
                    if blend:
                        q, t = pre[blend[0]], blend[1]
                        p = tuple(p[c] + (q[c] - p[c]) * t for c in range(4))
                    out[(x * 2 + sx) + (y * 2 + sy) * w * 2] = p
    result = []
    for r, g, b, a in out:
        if a <= 0.0:
            result.append((0, 0, 0, 0))
        else:
            result.append(tuple(min(255, int(round(c * 255.0 / a))) for c in (r, g, b)) + (int(round(a)),))
    doubled = Image.new("RGBA", (w * 2, h * 2))
    doubled.putdata(result)
    return doubled


def xbr(image, times=2):
    image = image.convert("RGBA")
    for _ in range(times):
        image = double(image)
    return image


if __name__ == "__main__":
    times = int(sys.argv[3]) if len(sys.argv) > 3 else 2
    xbr(Image.open(sys.argv[1]), times).save(sys.argv[2])
