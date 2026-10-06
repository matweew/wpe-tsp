#!/usr/bin/env python3
"""Draw the app icon: WPE's isometric slab carrying TrimUI's three-hexagon emblem, on a
WPE-blue rounded square. Layout matches other TrimUI app icons (e.g. PortMaster): transparent
300x300 canvas, 170x170 tile with ~38px corners at (65, 25). Usage: make_icon.py out.png"""
import math
import sys

from PIL import Image, ImageDraw

SS = 4                      # supersampling factor
SIZE = 300 * SS


def P(x, y):
    return (x * SS, y * SS)


def vertical_gradient(w, h, top, bottom):
    img = Image.new("RGBA", (w, h))
    d = ImageDraw.Draw(img)
    for y in range(h):
        t = y / (h - 1)
        d.line([(0, y), (w, y)], fill=tuple(int(top[i] + (bottom[i] - top[i]) * t) for i in range(3)) + (255,))
    return img


img = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))

# Rounded square tile (same placement as other TrimUI app icons)
box = (65, 25, 235, 195)
mask = Image.new("L", (SIZE, SIZE), 0)
ImageDraw.Draw(mask).rounded_rectangle([P(box[0], box[1]), P(box[2], box[3])], radius=38 * SS, fill=255)
img.paste(vertical_gradient(SIZE, SIZE, (64, 156, 235), (20, 84, 170)), (0, 0), mask)

d = ImageDraw.Draw(img)

# Isometric slab (WPE logo shape): top face is a square rotated 45 deg and squashed vertically
cx, cy = 150, 98            # center of the top face (tile center is 150, 110)
half = 70                   # half diagonal (horizontal)
squash = 0.58               # isometric vertical factor
thick = 22                  # slab thickness

L, T, R, B = (cx - half, cy), (cx, cy - half * squash), (cx + half, cy), (cx, cy + half * squash)


def down(p, dy=thick):
    return (p[0], p[1] + dy)


d.polygon([P(*L), P(*B), P(*down(B)), P(*down(L))], fill=(214, 232, 250, 255))   # left side
d.polygon([P(*B), P(*R), P(*down(R)), P(*down(B))], fill=(168, 203, 240, 255))   # right side
d.polygon([P(*L), P(*T), P(*R), P(*B)], fill=(255, 255, 255, 255))               # top face
edge = (20, 84, 170, 255)
for a, b in [(L, T), (T, R), (R, B), (B, L), (L, down(L)), (B, down(B)), (R, down(R)), (down(L), down(B)), (down(B), down(R))]:
    d.line([P(*a), P(*b)], fill=edge, width=2 * SS)


def to_iso(u, v):
    """Map a point in the top-face square (u, v in [-1, 1]) onto the isometric rhombus."""
    x = (u - v) / 2 * half
    y = (u + v) / 2 * half * squash
    return (cx + x, cy + y)


def hexagon(ccx, ccy, r):
    """Pointy-top hexagon in face coordinates, rotated so it reads upright after projection."""
    pts = []
    for k in range(6):
        a = math.radians(60 * k - 90 + 45)   # +45: compensate the face's 45 deg rotation
        pts.append(to_iso(ccx + r * math.cos(a), ccy + r * math.sin(a)))
    return pts


# TrimUI emblem: three hexagons in a triangle, in face coordinates (rotated 45 deg to read upright)
r = 0.40
gap = r * 1.95
centers = []
for ang in (-90, 30, 150):                       # top, bottom-right, bottom-left (upright frame)
    a = math.radians(ang + 45)
    centers.append((gap / math.sqrt(3) * math.cos(a), gap / math.sqrt(3) * math.sin(a)))
for (u, v) in centers:
    pts = hexagon(u, v, r)
    d.polygon([P(*p) for p in pts], fill=(247, 186, 46, 255), outline=(176, 120, 18, 255), width=2 * SS)

img = img.resize((300, 300), Image.LANCZOS)
img.save(sys.argv[1] if len(sys.argv) > 1 else "icon.png")
