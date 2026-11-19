#!/usr/bin/env python3
"""Makes src/btn_icons.h: the footer's button symbols (cross, circle, triangle, square, d-pad, up / down,
left / right), 30 x 30 pixels each.

1.2: drawn at the size they are shown, as vita-turbo-vpn draws its symbols: each pixel's cover is worked out
from its distance to the line (a line 3 px wide: full inside, a 1 px soft edge), so nothing is scaled down and
the edges are not blurred. The straight lines sit on whole pixels (the square, the triangle's base, the d-pad),
so they are fully sharp. The symbols are bare, in their colours, without the dark disc (as on the Vita's own
screens): bigger and with more contrast in the same 30 px.
No Pillow needed:  python3 tools/make_button_icons.py"""
import math, os

S = 30                                  # the cell
C = S / 2.0                             # its centre
SW = 3.0                                # the line width
BLUE = (124, 178, 232); RED = (255, 102, 102); GREEN = (64, 226, 160); PINK = (255, 105, 220); GREY = (200, 205, 220)


def seg_dist(px, py, ax, ay, bx, by):
    dx, dy = bx - ax, by - ay
    l2 = dx * dx + dy * dy
    t = 0.0 if l2 == 0 else max(0.0, min(1.0, ((px - ax) * dx + (py - ay) * dy) / l2))
    ex, ey = ax + t * dx - px, ay + t * dy - py
    return math.hypot(ex, ey)


def stroke(segs, w=SW):
    """cover of a stroke along the segments (round ends and joins), at each pixel's centre"""
    def cov(x, y):
        d = min(seg_dist(x, y, *s) for s in segs)
        return max(0.0, min(1.0, w / 2 - d + 0.5))
    return cov


def ring(cx, cy, r, w=SW):
    def cov(x, y):
        d = abs(math.hypot(x - cx, y - cy) - r)
        return max(0.0, min(1.0, w / 2 - d + 0.5))
    return cov


def fill(inside, n=8):
    """cover of a filled shape: n x n samples in each pixel"""
    def cov(x, y):
        k = 0
        for sy in range(n):
            for sx in range(n):
                if inside(x - 0.5 + (sx + 0.5) / n, y - 0.5 + (sy + 0.5) / n):
                    k += 1
        return k / (n * n)
    return cov


def in_rect(x0, y0, x1, y1):
    return lambda x, y: x0 <= x < x1 and y0 <= y < y1


def in_tri(p):
    def f(x, y):
        s = []
        for i in range(3):
            (ax, ay), (bx, by) = p[i], p[(i + 1) % 3]
            s.append((bx - ax) * (y - ay) - (by - ay) * (x - ax))
        return all(v >= 0 for v in s) or all(v <= 0 for v in s)
    return f


def poly_segs(pts, closed=True):
    n = len(pts)
    return [(*pts[i], *pts[(i + 1) % n]) for i in range(n if closed else n - 1)]


def glyph(kind):
    """(cover function, colour)"""
    if kind == "cross":                 # 1.3.1: 18 px high like the others (ends +-7.5, line 3, round ends)
        h = 7.5
        return stroke([(C - h, C - h, C + h, C + h), (C + h, C - h, C - h, C + h)]), BLUE
    if kind == "circle":                # 1.3.1: 18 px across (the line's outside)
        return ring(C, C, 7.5), RED
    if kind == "square":                # 1.3.1: 18 px, lines on whole pixels: 6..9, 21..24
        a = 7.5
        return stroke(poly_segs([(C - a, C - a), (C + a, C - a), (C + a, C + a), (C - a, C + a)])), PINK
    if kind == "triangle":              # 1.3.1: 20 px wide, 18 high
        top, base, hw = C - 7.5, C + 7.5, 8.5
        return stroke(poly_segs([(C, top), (C + hw, base), (C - hw, base)])), GREEN
    if kind in ("dpad", "updown"):      # bars 4 px wide (13..17), 16 long
        bars = [in_rect(C - 2, C - 8, C + 2, C + 8)]
        if kind == "dpad":
            bars.append(in_rect(C - 8, C - 2, C + 8, C + 2))
        return fill(lambda x, y: any(b(x, y) for b in bars)), GREY
    if kind == "leftright":             # two arrow heads
        l = in_tri([(C - 9, C), (C - 2, C - 6), (C - 2, C + 6)])
        r = in_tri([(C + 9, C), (C + 2, C - 6), (C + 2, C + 6)])
        return fill(lambda x, y: l(x, y) or r(x, y)), GREY
    raise ValueError(kind)


ORDER = ["cross", "circle", "triangle", "square", "dpad", "updown", "leftright"]   # (ui.c: BTN_I_*)


def render():
    """rows of (r, g, b, a) for the whole strip"""
    W = S * len(ORDER)
    px = [(0, 0, 0, 0)] * (W * S)
    for i, k in enumerate(ORDER):
        cov, (r, g, b) = glyph(k)
        for y in range(S):
            for x in range(S):
                a = int(round(cov(x + 0.5, y + 0.5) * 255))
                if a:
                    px[y * W + i * S + x] = (r, g, b, a)
    return W, px


def main():
    W, px = render()
    out = ["/* Button symbols for the footer, made by tools/make_button_icons.py (1.2: drawn at their size, sharp,",
           " * without the disc), 30 x 30 pixels each, side by side: cross, circle, triangle, square, d-pad, up / down,",
           " * left / right. RGBA8 as vita2d takes it (0xAABBGGRR), not premultiplied: the colour, the cover in alpha. */",
           "#ifndef BTN_ICONS_H", "#define BTN_ICONS_H", "#include <stdint.h>",
           "#define BTN_ICON_SIZE %d" % S, "#define BTN_ICON_COUNT %d" % len(ORDER),
           "static const uint32_t BTN_ICONS[%d] = {" % (S * W)]
    for y in range(S):
        row = px[y * W:(y + 1) * W]
        vals = ["0x%08x" % (a << 24 | b << 16 | g << 8 | r) if a else "0" for (r, g, b, a) in row]
        for i in range(0, len(vals), 14):
            out.append("    " + ",".join(vals[i:i + 14]) + ",")
    out += ["};", "#endif", ""]
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    with open(os.path.join(root, "src", "btn_icons.h"), "w") as f:
        f.write("\n".join(out))


if __name__ == "__main__":
    main()
