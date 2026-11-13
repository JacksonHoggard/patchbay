#!/usr/bin/env python3
"""Generate PatchBay meta images (icon + TV/DRC splashes) using only the stdlib.

Usage:  python tools/make_meta.py
Writes: meta/icon.png (128x128), meta/tv-splash.png (1280x720), meta/drc-splash.png (854x480)
"""
import math
import os
import struct
import zlib

BG = (18, 20, 26)
AMBER = (255, 167, 38)
TEAL = (77, 182, 172)
TEXT = (235, 236, 240)
DIM = (150, 155, 168)
PANEL = (32, 36, 46)
PANEL_EDGE = (52, 57, 70)
JACK_RING = (96, 102, 118)
JACK_HOLE = (8, 9, 12)


def clamp01(v):
    return 0.0 if v < 0.0 else (1.0 if v > 1.0 else v)


def shade(c, f):
    return tuple(max(0, min(255, int(round(x * f)))) for x in c)


class Canvas:
    def __init__(self, w, h, color):
        self.w, self.h = w, h
        self.px = bytearray(bytes(color) * (w * h))

    def blend(self, x, y, c, a):
        if a <= 0.0 or x < 0 or y < 0 or x >= self.w or y >= self.h:
            return
        i = (y * self.w + x) * 3
        p = self.px
        if a >= 1.0:
            p[i], p[i + 1], p[i + 2] = c
        else:
            p[i] = int(p[i] + (c[0] - p[i]) * a + 0.5)
            p[i + 1] = int(p[i + 1] + (c[1] - p[i + 1]) * a + 0.5)
            p[i + 2] = int(p[i + 2] + (c[2] - p[i + 2]) * a + 0.5)

    def _bbox(self, x0, y0, x1, y1):
        return (max(0, int(math.floor(x0))), max(0, int(math.floor(y0))),
                min(self.w, int(math.ceil(x1)) + 1), min(self.h, int(math.ceil(y1)) + 1))

    # Analytic anti-aliased shapes (coverage from signed distance).
    def round_rect(self, x, y, w, h, r, c, alpha=1.0):
        cx, cy = x + w / 2.0, y + h / 2.0
        bx, by = w / 2.0 - r, h / 2.0 - r
        X0, Y0, X1, Y1 = self._bbox(x - 1, y - 1, x + w + 1, y + h + 1)
        for py in range(Y0, Y1):
            qy = abs(py + 0.5 - cy) - by
            for px in range(X0, X1):
                qx = abs(px + 0.5 - cx) - bx
                d = math.hypot(max(qx, 0.0), max(qy, 0.0)) + min(max(qx, qy), 0.0) - r
                self.blend(px, py, c, clamp01(0.5 - d) * alpha)

    def circle(self, cx, cy, r, c, alpha=1.0):
        X0, Y0, X1, Y1 = self._bbox(cx - r - 1, cy - r - 1, cx + r + 1, cy + r + 1)
        for py in range(Y0, Y1):
            dy = py + 0.5 - cy
            for px in range(X0, X1):
                d = math.hypot(px + 0.5 - cx, dy)
                self.blend(px, py, c, clamp01(r - d + 0.5) * alpha)

    def stroke(self, pts, r, c, alpha=1.0):
        """Thick polyline; per-pixel max coverage so overlaps don't double-blend."""
        cov = {}
        for (ax, ay), (bx, by) in zip(pts, pts[1:]):
            dx, dy = bx - ax, by - ay
            L2 = dx * dx + dy * dy or 1e-9
            X0, Y0, X1, Y1 = self._bbox(min(ax, bx) - r - 1, min(ay, by) - r - 1,
                                        max(ax, bx) + r + 1, max(ay, by) + r + 1)
            for py in range(Y0, Y1):
                fy = py + 0.5
                for px in range(X0, X1):
                    fx = px + 0.5
                    t = ((fx - ax) * dx + (fy - ay) * dy) / L2
                    t = 0.0 if t < 0 else (1.0 if t > 1 else t)
                    d = math.hypot(fx - ax - t * dx, fy - ay - t * dy)
                    a = clamp01(r - d + 0.5)
                    if a > 0:
                        k = (px, py)
                        if a > cov.get(k, 0.0):
                            cov[k] = a
        for (px, py), a in cov.items():
            self.blend(px, py, c, a * alpha)

    def rect(self, x, y, w, h, c):
        for py in range(max(0, y), min(self.h, y + h)):
            for px in range(max(0, x), min(self.w, x + w)):
                self.blend(px, py, c, 1.0)

    def save(self, path):
        raw = bytearray()
        row = self.w * 3
        for y in range(self.h):
            raw.append(0)
            raw += self.px[y * row:(y + 1) * row]

        def chunk(tag, data):
            return (struct.pack(">I", len(data)) + tag + data +
                    struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

        ihdr = struct.pack(">IIBBBBB", self.w, self.h, 8, 2, 0, 0, 0)
        data = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) +
                chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b""))
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as f:
            f.write(data)


def bezier(p0, p1, p2, p3, n=48):
    out = []
    for i in range(n + 1):
        t = i / n
        u = 1 - t
        out.append((u * u * u * p0[0] + 3 * u * u * t * p1[0] + 3 * u * t * t * p2[0] + t * t * t * p3[0],
                    u * u * u * p0[1] + 3 * u * u * t * p1[1] + 3 * u * t * t * p2[1] + t * t * t * p3[1]))
    return out


def draw_motif(cv, x, y, w):
    """Patch-bay panel with a 3x2 jack grid and two patch cables. Height = 0.72*w."""
    h = w * 0.72
    s = w / 100.0  # design units: panel is 100 x 72
    # drop shadow + panel
    cv.round_rect(x + 1.5 * s, y + 2.5 * s, w, h, 9 * s, (6, 7, 10), 0.6)
    cv.round_rect(x, y, w, h, 9 * s, PANEL_EDGE)
    cv.round_rect(x + 1.2 * s, y + 1.2 * s, w - 2.4 * s, h - 2.4 * s, 8 * s, PANEL)
    # screws
    for sx, sy in ((6, 6), (94, 6), (6, 66), (94, 66)):
        cv.circle(x + sx * s, y + sy * s, 1.6 * s, PANEL_EDGE)
    # jacks
    cols = [x + (22 + 28 * i) * s for i in range(3)]
    rows = [y + (22 + 28 * j) * s for j in range(2)]
    jr = 9 * s
    for jy in rows:
        for jx in cols:
            cv.circle(jx, jy + 0.8 * s, jr, (12, 13, 17))
            cv.circle(jx, jy, jr, JACK_RING)
            cv.circle(jx, jy, jr * 0.78, shade(JACK_RING, 0.7))
            cv.circle(jx, jy, jr * 0.48, JACK_HOLE)

    # Two cables that never cross: amber links the outer top jacks and hangs
    # in the gap between the rows; teal links the outer bottom jacks and
    # hangs below the bottom row. The middle jacks stay free.
    # A bezier with both control points `sag` below its ends dips 0.75*sag.
    cables = [
        ((cols[0], rows[0]), (cols[2], rows[0]), 18.7, AMBER),  # lowest point y=36: centre of the gap
        ((cols[0], rows[1]), (cols[2], rows[1]), 18.0, TEAL),   # lowest point y=63.5: clears the jacks
    ]
    cr = 3.4 * s
    paths = []
    for (ax, ay), (bx, by), sag, color in cables:
        pts = bezier((ax, ay), (ax, ay + sag * s), (bx, by + sag * s), (bx, by), n=64)
        paths.append((pts, color))

    # Draw each layer for every cable before the next layer, so no cable's
    # shadow or outline lands on top of another cable or plug.
    for pts, _ in paths:
        cv.stroke([(px + 1.2 * s, py + 2.4 * s) for px, py in pts], cr + 0.6 * s, (4, 5, 8), 0.45)
    for pts, color in paths:
        cv.stroke(pts, cr + 1.0 * s, shade(color, 0.45))
        cv.stroke(pts, cr, color)
        cv.stroke([(px - 0.9 * s, py - 0.9 * s) for px, py in pts[6:-6]], cr * 0.28,
                  tuple(min(255, int(v + (255 - v) * 0.55)) for v in color), 0.7)
    for (a, b, _, color) in cables:  # plug heads sit on top of everything
        for px, py in (a, b):
            cv.circle(px + 0.8 * s, py + 1.4 * s, 6.2 * s, (4, 5, 8), 0.4)
            cv.circle(px, py, 6.2 * s, shade(color, 0.45))
            cv.circle(px, py, 5.2 * s, color)
            cv.circle(px, py, 2.4 * s, shade(color, 0.7))
            cv.circle(px - 1.3 * s, py - 1.3 * s, 1.6 * s, (255, 255, 255), 0.45)
    return h


# 5-wide bitmap font; rows 0-6 sit on the baseline, rows 7-8 are descenders.
GLYPHS = {
    "P": ["####.", "#...#", "#...#", "####.", "#....", "#....", "#...."],
    "B": ["####.", "#...#", "#...#", "####.", "#...#", "#...#", "####."],
    "S": [".####", "#....", "#....", ".###.", "....#", "....#", "####."],
    "D": ["###..", "#..#.", "#...#", "#...#", "#...#", "#..#.", "###.."],
    "C": [".###.", "#...#", "#....", "#....", "#....", "#...#", ".###."],
    "a": [".....", ".....", ".###.", "....#", ".####", "#...#", ".####"],
    "t": [".#...", ".#...", "###..", ".#...", ".#...", ".#..#", "..##."],
    "c": [".....", ".....", ".###.", "#....", "#....", "#...#", ".###."],
    "h": ["#....", "#....", "#.##.", "##..#", "#...#", "#...#", "#...#"],
    "y": [".....", ".....", "#...#", "#...#", "#...#", "#..##", ".##.#", "....#", ".###."],
    "g": [".....", ".....", ".####", "#...#", "#...#", "#...#", ".####", "....#", ".###."],
    "f": ["..##.", ".#..#", ".#...", "###..", ".#...", ".#...", ".#..."],
    "i": ["..#..", ".....", ".##..", "..#..", "..#..", "..#..", ".###."],
    "n": [".....", ".....", "#.##.", "##..#", "#...#", "#...#", "#...#"],
    "e": [".....", ".....", ".###.", "#...#", "#####", "#....", ".###."],
    "m": [".....", ".....", "##.#.", "#.#.#", "#.#.#", "#...#", "#...#"],
    "o": [".....", ".....", ".###.", "#...#", "#...#", "#...#", ".###."],
    "d": ["....#", "....#", ".##.#", "#..##", "#...#", "#...#", ".####"],
    "r": [".....", ".....", "#.##.", "##..#", "#....", "#....", "#...."],
    " ": ["....."] * 7,
}


def text_width(s, scale):
    return (len(s) * 6 - 1) * scale


def draw_text(cv, x, y, s, scale, color, shadow=None):
    for ch in s:
        for r, line in enumerate(GLYPHS[ch]):
            for col, bit in enumerate(line):
                if bit == "#":
                    if shadow:
                        cv.rect(x + col * scale + max(1, scale // 4), y + r * scale + max(1, scale // 4),
                                scale, scale, shadow)
                    cv.rect(x + col * scale, y + r * scale, scale, scale, color)
        x += 6 * scale
    return x


def make_icon(path):
    cv = Canvas(128, 128, BG)
    draw_motif(cv, 8, 18, 112)
    cv.save(path)


def make_splash(path, W, H):
    k = W / 1280.0
    cv = Canvas(W, H, BG)
    # subtle radial glow behind the logo
    gx, gy, gr = W * 0.32, H * 0.5, H * 0.55
    for y in range(H):
        for x in range(int(gx - gr), int(gx + gr)):
            d = math.hypot(x - gx, y - gy) / gr
            if d < 1:
                cv.blend(x, y, (40, 44, 56), (1 - d) ** 2 * 0.6)

    big = max(2, int(round(14 * k)))
    small = max(2, int(round(4 * k)))
    mw = 300 * k
    gap = 64 * k
    tw = text_width("PatchBay", big)
    total = mw + gap + tw
    x0 = (W - total) / 2.0
    mh = mw * 0.72
    draw_motif(cv, x0, (H - mh) / 2.0, mw)

    tx = int(round(x0 + mw + gap))
    block_h = 7 * big + int(30 * k) + 7 * small
    ty = int(round((H - block_h) / 2.0 - 2 * big))  # nudge up: descender of "y" hangs below
    sh = (8, 9, 12)
    nx = draw_text(cv, tx, ty, "Patch", big, TEXT, sh)
    draw_text(cv, nx, ty, "Bay", big, AMBER, sh)
    # accent underline + tagline
    ly = ty + 9 * big + int(14 * k)
    cv.rect(tx, ly, int(48 * k), max(2, int(4 * k)), TEAL)
    draw_text(cv, tx, ly + int(18 * k), "SDCafiine mod manager", small, DIM)
    cv.save(path)


def main():
    root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "meta")
    root = os.path.normpath(root)
    make_icon(os.path.join(root, "icon.png"))
    make_splash(os.path.join(root, "tv-splash.png"), 1280, 720)
    make_splash(os.path.join(root, "drc-splash.png"), 854, 480)
    print("wrote", root)


if __name__ == "__main__":
    main()
