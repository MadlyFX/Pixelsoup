#!/usr/bin/env python3
"""Generates the Hardware Map's isometric pixel-art sprite sheet.

Writes engine/editor/assets/hardware_sprites.png and the matching
engine/editor/src/hardware_sprites_atlas.inc (sprite rectangles, ground
anchors and the screen columns where each device's ports sit). The art is
built from shaded isometric boxes plus hand-placed detail, in a 2:1
projection where one map cell is 16 units: +x runs down-right (2, 1), +y
down-left (-2, 1) and +z straight up (0, -1). A device's left face (+y) faces
the viewer's left, its right face (+x) the viewer's right.

The PNG is a source asset: an artist may repaint it, keeping each sprite's
rectangle, anchor and port columns. Re-run this script only to regenerate it
from scratch. Standard library only.

usage: python scripts/generate-hardware-sprites.py [--preview out.png]
"""
from __future__ import annotations

import argparse
import struct
import zlib
from dataclasses import dataclass, field
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PNG_PATH = ROOT / 'engine' / 'editor' / 'assets' / 'hardware_sprites.png'
INC_PATH = ROOT / 'engine' / 'editor' / 'src' / 'hardware_sprites_atlas.inc'


def hex_color(text: str, alpha: int = 255) -> tuple[int, int, int, int]:
    text = text.lstrip('#')
    return (int(text[0:2], 16), int(text[2:4], 16), int(text[4:6], 16), alpha)


# A cool slate palette: dark navy bodies, lighter top faces, LED accents.
OUTLINE = hex_color('11141d')
SLATE = {
    'top': hex_color('6b7a99'),
    'top_edge': hex_color('9aa8c4'),
    'left': hex_color('414d68'),
    'left_dark': hex_color('333d54'),
    'right': hex_color('2b3347'),
    'right_dark': hex_color('222839'),
}
CHARCOAL = {
    'top': hex_color('4a4f5e'),
    'top_edge': hex_color('767d90'),
    'left': hex_color('2c3039'),
    'left_dark': hex_color('22252d'),
    'right': hex_color('1c1f26'),
    'right_dark': hex_color('16181e'),
}
METAL = {
    'top': hex_color('8a94a8'),
    'top_edge': hex_color('b9c2d4'),
    'left': hex_color('5d6679'),
    'left_dark': hex_color('4a5263'),
    'right': hex_color('454c5c'),
    'right_dark': hex_color('383e4b'),
}
GUNMETAL = {
    'top': hex_color('565d6b'),
    'top_edge': hex_color('8a93a5'),
    'left': hex_color('353a45'),
    'left_dark': hex_color('2a2e37'),
    'right': hex_color('262a32'),
    'right_dark': hex_color('1d2027'),
}
BLUE = hex_color('3d8bff')
CYAN = hex_color('52e5ff')
CYAN_DIM = hex_color('2b8ea6')
GREEN = hex_color('5ce38f')
RED = hex_color('ff5d6c')
AMBER = hex_color('ffb24a')
ORANGE = hex_color('ff7a3d')
PURPLE = hex_color('b37dff')
WHITE = hex_color('e9f1ff')
SCREEN_DARK = hex_color('0c1a2e')
SCREEN_MID = hex_color('163a5f')
PORT = hex_color('0b0d12')
PORT_RIM = hex_color('7d879c')
SHADOW = (8, 10, 16, 110)


class Canvas:
    def __init__(self, width: int, height: int) -> None:
        self.width = width
        self.height = height
        self.pixels = [[(0, 0, 0, 0)] * width for _ in range(height)]

    def set(self, x: int, y: int, color) -> None:
        if 0 <= x < self.width and 0 <= y < self.height:
            if len(color) == 4 and color[3] < 255:
                below = self.pixels[y][x]
                a = color[3] / 255.0
                if below[3] == 0:
                    self.pixels[y][x] = color
                else:
                    self.pixels[y][x] = (
                        round(color[0] * a + below[0] * (1 - a)),
                        round(color[1] * a + below[1] * (1 - a)),
                        round(color[2] * a + below[2] * (1 - a)),
                        max(below[3], color[3]))
            else:
                self.pixels[y][x] = color

    def get(self, x: int, y: int):
        if 0 <= x < self.width and 0 <= y < self.height:
            return self.pixels[y][x]
        return (0, 0, 0, 0)

    def rect(self, x: int, y: int, w: int, h: int, color) -> None:
        for yy in range(y, y + h):
            for xx in range(x, x + w):
                self.set(xx, yy, color)

    def polygon(self, points, color) -> None:
        xs = [p[0] for p in points]
        ys = [p[1] for p in points]
        for py in range(int(min(ys)) - 1, int(max(ys)) + 2):
            for px in range(int(min(xs)) - 1, int(max(xs)) + 2):
                if inside(points, px + 0.5, py + 0.5):
                    self.set(px, py, color)

    def outline(self, color=OUTLINE) -> None:
        """Draws a one-pixel outline around everything opaque."""
        solid = [[self.pixels[y][x][3] > 0 for x in range(self.width)]
                 for y in range(self.height)]
        for y in range(self.height):
            for x in range(self.width):
                if solid[y][x]:
                    continue
                for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                    nx, ny = x + dx, y + dy
                    if 0 <= nx < self.width and 0 <= ny < self.height and \
                            solid[ny][nx] and self.pixels[ny][nx][3] == 255:
                        self.pixels[y][x] = color
                        break


def inside(points, x: float, y: float) -> bool:
    result = False
    count = len(points)
    j = count - 1
    for i in range(count):
        xi, yi = points[i]
        xj, yj = points[j]
        if (yi > y) != (yj > y):
            cross = (xj - xi) * (y - yi) / (yj - yi) + xi
            if x < cross:
                result = not result
        j = i
    return result


@dataclass
class Iso:
    """Projects iso units onto one sprite's pixels from its ground anchor."""
    ox: float
    oy: float

    def __call__(self, x: float, y: float, z: float = 0.0) -> tuple[float, float]:
        return (self.ox + (x - y) * 2.0, self.oy + (x + y) * 1.0 - z)


def box(canvas: Canvas, iso: Iso, x0, y0, z0, a, b, h, shade, edges=True):
    """A shaded box from (x0, y0, z0), a along x, b along y, h high.
    Returns the screen polygons of its top, left (+y) and right (+x) faces."""
    top = [iso(x0, y0, z0 + h), iso(x0 + a, y0, z0 + h),
           iso(x0 + a, y0 + b, z0 + h), iso(x0, y0 + b, z0 + h)]
    left = [iso(x0, y0 + b, z0 + h), iso(x0 + a, y0 + b, z0 + h),
            iso(x0 + a, y0 + b, z0), iso(x0, y0 + b, z0)]
    right = [iso(x0 + a, y0, z0 + h), iso(x0 + a, y0 + b, z0 + h),
             iso(x0 + a, y0 + b, z0), iso(x0 + a, y0, z0)]
    canvas.polygon(left, shade['left'])
    canvas.polygon(right, shade['right'])
    canvas.polygon(top, shade['top'])
    if edges:
        # Lit front edges of the top face, a pixel-art staple.
        line(canvas, top[3], top[2], shade['top_edge'])
        line(canvas, top[2], top[1], shade['top_edge'])
        # The vertical corner toward the viewer.
        cx, cy = iso(x0 + a, y0 + b, z0 + h)
        for yy in range(int(cy), int(iso(x0 + a, y0 + b, z0)[1])):
            canvas.set(int(cx), yy, shade['left_dark'])
    return top, left, right


def line(canvas: Canvas, p0, p1, color) -> None:
    x0, y0 = p0
    x1, y1 = p1
    steps = int(max(abs(x1 - x0), abs(y1 - y0))) + 1
    for i in range(steps + 1):
        t = i / steps if steps else 0.0
        canvas.set(int(round(x0 + (x1 - x0) * t - 0.5)),
                   int(round(y0 + (y1 - y0) * t - 0.5)), color)


def face_point(face, u: float, v: float):
    """A point on a face polygon [tl, tr, br, bl] at fractions u across and
    v down."""
    tl, tr, br, bl = face
    top = (tl[0] + (tr[0] - tl[0]) * u, tl[1] + (tr[1] - tl[1]) * u)
    bottom = (bl[0] + (br[0] - bl[0]) * u, bl[1] + (br[1] - bl[1]) * u)
    return (top[0] + (bottom[0] - top[0]) * v, top[1] + (bottom[1] - top[1]) * v)


def face_quad(canvas: Canvas, face, u0, v0, u1, v1, color) -> None:
    canvas.polygon([face_point(face, u0, v0), face_point(face, u1, v0),
                    face_point(face, u1, v1), face_point(face, u0, v1)], color)


def face_pixel(canvas: Canvas, face, u, v, color) -> None:
    x, y = face_point(face, u, v)
    canvas.set(int(x), int(y), color)


def ground_shadow(canvas: Canvas, iso: Iso, x0, y0, a, b, grow=2) -> None:
    canvas.polygon([iso(x0 - grow, y0 - grow), iso(x0 + a + grow, y0 - grow),
                    iso(x0 + a + grow, y0 + b + grow), iso(x0 - grow, y0 + b + grow)],
                   SHADOW)


@dataclass
class Sprite:
    name: str
    canvas: Canvas
    anchor: tuple[int, int]
    # Screen columns (x, top, bottom) in sprite pixels where ports sit.
    inputs: tuple[int, int, int]
    outputs: tuple[int, int, int]
    atlas: tuple[int, int] = (0, 0)
    extra: dict = field(default_factory=dict)


def render_server() -> Sprite:
    c = Canvas(52, 76)
    iso = Iso(26, 62)
    a, b, h = 12, 10, 38
    x0, y0 = -a / 2, -b / 2
    ground_shadow(c, iso, x0, y0, a, b)
    # Feet, then the tower.
    for fx, fy in ((x0 + 1, y0 + 1), (x0 + a - 2, y0 + b - 2)):
        box(c, iso, fx, fy, 0, 1, 1, 2, CHARCOAL, edges=False)
    top, left, right = box(c, iso, x0, y0, 2, a, b, h, SLATE)
    # Front (left face): a cyan light bar, two drive bays, a mesh grille and
    # a power button.
    face_quad(c, left, 0.10, 0.07, 0.17, 0.82, CYAN_DIM)
    face_quad(c, left, 0.11, 0.09, 0.16, 0.80, CYAN)
    for row, v in enumerate((0.10, 0.20)):
        face_quad(c, left, 0.28, v, 0.88, v + 0.07, SLATE['left_dark'])
        face_pixel(c, left, 0.82, v + 0.035, GREEN if row == 0 else AMBER)
    for gv in range(8):
        for gu in range(6):
            if (gu + gv) % 2 == 0:
                face_pixel(c, left, 0.32 + gu * 0.09, 0.40 + gv * 0.05,
                           SLATE['left_dark'])
    face_quad(c, left, 0.70, 0.86, 0.80, 0.92, WHITE)
    # Side (right face): a glass panel with the GPUs glowing behind it.
    face_quad(c, right, 0.12, 0.08, 0.88, 0.88, SCREEN_DARK)
    for gpu, v in enumerate((0.20, 0.42, 0.64)):
        face_quad(c, right, 0.20, v, 0.80, v + 0.10, hex_color('25304a'))
        for fan in (0.32, 0.62):
            face_pixel(c, right, fan, v + 0.05, PURPLE if gpu % 2 else CYAN)
            face_pixel(c, right, fan + 0.05, v + 0.05, PURPLE if gpu % 2 else CYAN)
    # Top vents.
    for i in range(4):
        line(c, iso(x0 + 2 + i * 2.2, y0 + 2, 2 + h),
             iso(x0 + 2 + i * 2.2, y0 + b - 2, 2 + h), SLATE['left'])
    c.outline()
    # Outputs down the right face, inputs (control) on the front.
    out_x = int(round(face_point(right, 0.78, 0.0)[0]))
    out_top = int(round(face_point(right, 0.78, 0.12)[1]))
    out_bottom = int(round(face_point(right, 0.78, 0.92)[1]))
    in_x = int(round(face_point(left, 0.05, 0.0)[0]))
    in_top = int(round(face_point(left, 0.05, 0.55)[1]))
    return Sprite('render_server', c, (iso.ox, iso.oy),
                  (in_x, in_top, in_top + 8), (out_x, out_top, out_bottom))


def operator_machine() -> Sprite:
    c = Canvas(84, 64)
    iso = Iso(42, 50)
    a, b = 26, 12
    x0, y0 = -a / 2, -b / 2
    ground_shadow(c, iso, x0, y0, a, b)
    # Desk legs and top.
    for fx, fy in ((x0, y0), (x0 + a - 1, y0), (x0, y0 + b - 1),
                   (x0 + a - 1, y0 + b - 1)):
        box(c, iso, fx, fy, 0, 1, 1, 11, CHARCOAL, edges=False)
    box(c, iso, x0, y0, 11, a, b, 2, METAL)
    # Two monitors at the back, screens toward the viewer (+y).
    for mx in (x0 + 1, x0 + 13):
        box(c, iso, mx + 4, y0 + 2, 13, 3, 1, 3, CHARCOAL, edges=False)
        _, screen, _ = box(c, iso, mx, y0 + 1, 15, 11, 2, 10, CHARCOAL)
        face_quad(c, screen, 0.06, 0.10, 0.94, 0.86, SCREEN_MID)
        # A timeline and a picture on the screens.
        face_quad(c, screen, 0.12, 0.20, 0.60, 0.50, hex_color('2c6e9e'))
        face_quad(c, screen, 0.66, 0.20, 0.88, 0.32, AMBER)
        face_quad(c, screen, 0.12, 0.62, 0.88, 0.70, CYAN_DIM)
        face_quad(c, screen, 0.30, 0.60, 0.34, 0.74, WHITE)
    # Keyboard and a fader box.
    box(c, iso, x0 + 6, y0 + 7, 13, 12, 3, 1, SLATE, edges=False)
    box(c, iso, x0 + 20, y0 + 6, 13, 4, 4, 1, CHARCOAL, edges=False)
    c.set(int(iso(x0 + 22, y0 + 8, 14)[0]), int(iso(x0 + 22, y0 + 8, 14)[1]), GREEN)
    c.outline()
    right_x = int(round(iso(x0 + a, y0 + b / 2, 12)[0])) - 2
    right_y = int(round(iso(x0 + a, y0 + b / 2, 12)[1]))
    return Sprite('operator_machine', c, (iso.ox, iso.oy),
                  (int(iso(x0, y0 + b, 12)[0]) + 2, right_y, right_y + 8),
                  (right_x, right_y - 4, right_y + 6))


def processor(novastar: bool) -> Sprite:
    c = Canvas(64, 58)
    iso = Iso(32, 44)
    a, b = 18, 12
    x0, y0 = -a / 2, -b / 2
    ground_shadow(c, iso, x0, y0, a, b)
    # A small rack stand, then the 2U chassis on it.
    for fx, fy in ((x0, y0), (x0 + a - 1, y0), (x0, y0 + b - 1),
                   (x0 + a - 1, y0 + b - 1)):
        box(c, iso, fx, fy, 0, 1, 1, 9, CHARCOAL, edges=False)
    box(c, iso, x0, y0, 9, a, b, 1, METAL, edges=False)
    shade = CHARCOAL if novastar else SLATE
    top, front, side = box(c, iso, x0, y0, 10, a, b, 9, shade)
    # Rack ears.
    face_quad(c, front, 0.0, 0.0, 0.05, 1.0, shade['top'])
    face_quad(c, front, 0.95, 0.0, 1.0, 1.0, shade['top'])
    if novastar:
        # MX-style front: a colour LCD, a knob, buttons and an orange key.
        face_quad(c, front, 0.09, 0.18, 0.38, 0.76, SCREEN_DARK)
        face_quad(c, front, 0.12, 0.28, 0.34, 0.40, CYAN)
        face_quad(c, front, 0.12, 0.48, 0.26, 0.56, WHITE)
        face_quad(c, front, 0.12, 0.60, 0.30, 0.66, CYAN_DIM)
        face_quad(c, front, 0.44, 0.26, 0.52, 0.70, METAL['left'])
        face_pixel(c, front, 0.48, 0.46, WHITE)
        for i in range(4):
            face_quad(c, front, 0.58 + i * 0.07, 0.30, 0.62 + i * 0.07, 0.44,
                      METAL['left'])
        face_quad(c, front, 0.58, 0.58, 0.66, 0.70, ORANGE)
        face_pixel(c, front, 0.86, 0.30, GREEN)
        face_pixel(c, front, 0.86, 0.52, CYAN)
    else:
        # A generic processor: input connectors, status lights.
        for i in range(4):
            face_quad(c, front, 0.10 + i * 0.11, 0.30, 0.17 + i * 0.11, 0.62, PORT)
        face_quad(c, front, 0.62, 0.30, 0.82, 0.48, SCREEN_DARK)
        face_quad(c, front, 0.64, 0.34, 0.74, 0.40, GREEN)
        face_pixel(c, front, 0.88, 0.40, GREEN)
        face_pixel(c, front, 0.88, 0.62, AMBER)
    # Network outputs to the LED wall, a grid of ports on the side.
    for row in range(2):
        for col in range(4):
            face_quad(c, side, 0.14 + col * 0.19, 0.24 + row * 0.34,
                      0.26 + col * 0.19, 0.48 + row * 0.34, PORT)
            face_pixel(c, side, 0.20 + col * 0.19, 0.26 + row * 0.34,
                       GREEN if (row + col) % 3 else AMBER)
    c.outline()
    in_x = int(round(face_point(front, 0.04, 0.0)[0]))
    in_top = int(round(face_point(front, 0.04, 0.0)[1]))
    in_bottom = int(round(face_point(front, 0.04, 1.0)[1])) + 14
    out_x = int(round(face_point(side, 0.90, 0.0)[0]))
    out_top = int(round(face_point(side, 0.90, 0.30)[1]))
    return Sprite('processor_novastar' if novastar else 'processor', c,
                  (iso.ox, iso.oy), (in_x, in_top, in_bottom),
                  (out_x, out_top, out_top + 8))


def brompton_processor() -> Sprite:
    """A Tessera-style 1U processor: a slim gunmetal chassis with a wide front
    display, a column of keys, a rotary encoder and a blue light bar."""
    c = Canvas(64, 58)
    iso = Iso(32, 44)
    a, b = 18, 12
    x0, y0 = -a / 2, -b / 2
    ground_shadow(c, iso, x0, y0, a, b)
    # The same rack stand as the other processors, a 1U box on it.
    for fx, fy in ((x0, y0), (x0 + a - 1, y0), (x0, y0 + b - 1),
                   (x0 + a - 1, y0 + b - 1)):
        box(c, iso, fx, fy, 0, 1, 1, 9, CHARCOAL, edges=False)
    box(c, iso, x0, y0, 9, a, b, 1, METAL, edges=False)
    top, front, side = box(c, iso, x0, y0, 10, a, b, 6, GUNMETAL)
    # Rack ears.
    face_quad(c, front, 0.0, 0.0, 0.05, 1.0, GUNMETAL['top'])
    face_quad(c, front, 0.95, 0.0, 1.0, 1.0, GUNMETAL['top'])
    # A wide display showing a picture and a status line.
    face_quad(c, front, 0.08, 0.16, 0.50, 0.70, SCREEN_DARK)
    face_quad(c, front, 0.10, 0.22, 0.30, 0.46, hex_color('2c6e9e'))
    face_quad(c, front, 0.32, 0.22, 0.48, 0.34, WHITE)
    face_quad(c, front, 0.10, 0.52, 0.46, 0.60, BLUE)
    # Keys and the encoder.
    for i in range(3):
        face_quad(c, front, 0.56 + i * 0.07, 0.22, 0.60 + i * 0.07, 0.42,
                  hex_color('c9d2e3'))
    face_quad(c, front, 0.80, 0.18, 0.88, 0.62, METAL['left'])
    face_pixel(c, front, 0.84, 0.36, WHITE)
    # The blue light bar along the bottom of the front.
    face_quad(c, front, 0.06, 0.80, 0.94, 0.92, BLUE)
    # Outputs to the wall on the side: fibre and copper ports.
    for col in range(4):
        face_quad(c, side, 0.14 + col * 0.19, 0.24, 0.26 + col * 0.19, 0.66,
                  PORT)
        face_pixel(c, side, 0.20 + col * 0.19, 0.30, GREEN if col else BLUE)
    c.outline()
    in_x = int(round(face_point(front, 0.04, 0.0)[0]))
    in_top = int(round(face_point(front, 0.04, 0.0)[1]))
    in_bottom = int(round(face_point(front, 0.04, 1.0)[1])) + 14
    out_x = int(round(face_point(side, 0.90, 0.0)[0]))
    out_top = int(round(face_point(side, 0.90, 0.30)[1]))
    return Sprite('processor_brompton', c, (iso.ox, iso.oy),
                  (in_x, in_top, in_bottom), (out_x, out_top, out_top + 8))


def led_wall() -> Sprite:
    c = Canvas(112, 112)
    iso = Iso(54, 92)
    a, b, h = 44, 4, 40
    x0, y0 = -a / 2, -b / 2
    ground_shadow(c, iso, x0 - 1, y0 - 1, a + 2, b + 4)
    # A low ground-stack plinth under the cabinets, with a back brace.
    box(c, iso, x0 - 1, y0 - 3, 0, a + 2, b + 5, 3, METAL)
    for fx in (x0 + 4, x0 + a / 2 - 1, x0 + a - 6):
        box(c, iso, fx, y0 - 3, 3, 2, 2, 22, CHARCOAL, edges=False)
    top, screen, edge = box(c, iso, x0, y0, 3, a, b, h, CHARCOAL)
    # The picture: a pixel sunset across the cabinets.
    bands = [hex_color('2a1b5c'), hex_color('46247a'), hex_color('7a2f8f'),
             hex_color('c2427f'), hex_color('f0685a'), hex_color('ff9f4a'),
             hex_color('ffd166')]
    rows = len(bands)
    for i, band in enumerate(bands):
        face_quad(c, screen, 0.015, 0.03 + i * (0.94 / rows), 0.985,
                  0.03 + (i + 1) * (0.94 / rows) + 0.01, band)
    # A sun and a mountain line.
    for du in range(-3, 4):
        for dv in range(-2, 3):
            if du * du * 0.7 + dv * dv * 2.5 < 7:
                face_pixel(c, screen, 0.62 + du * 0.012, 0.66 + dv * 0.025,
                           hex_color('fff3b0'))
    ridge = [0.80, 0.74, 0.70, 0.76, 0.68, 0.62, 0.70, 0.78, 0.72, 0.66,
             0.74, 0.82, 0.78, 0.84]
    for i, v in enumerate(ridge):
        u0 = 0.015 + i * (0.97 / len(ridge))
        face_quad(c, screen, u0, v, u0 + 0.97 / len(ridge) + 0.005, 0.97,
                  hex_color('1b1036'))
    # Cabinet seams.
    for cab in range(1, 8):
        u = cab / 8.0
        p0, p1 = face_point(screen, u, 0.0), face_point(screen, u, 1.0)
        line(c, p0, p1, (10, 8, 20, 150))
    for row in range(1, 4):
        v = row / 4.0
        line(c, face_point(screen, 0.0, v), face_point(screen, 1.0, v),
             (10, 8, 20, 150))
    c.outline()
    in_x = int(round(face_point(screen, 0.0, 0.0)[0])) - 1
    in_top = int(round(face_point(screen, 0.0, 0.55)[1]))
    out_x = int(round(face_point(screen, 1.0, 0.0)[0]))
    return Sprite('led_wall', c, (iso.ox, iso.oy), (in_x, in_top, in_top + 8),
                  (out_x, in_top, in_top + 8))


def pack(sprites: list[Sprite]) -> Canvas:
    width = 256
    x = y = row = 0
    for sprite in sprites:
        w, h = sprite.canvas.width, sprite.canvas.height
        if x + w > width:
            x = 0
            y += row + 1
            row = 0
        sprite.atlas = (x, y)
        x += w + 1
        row = max(row, h)
    height = y + row
    height = 1 << (height - 1).bit_length()
    atlas = Canvas(width, height)
    for sprite in sprites:
        ax, ay = sprite.atlas
        for yy in range(sprite.canvas.height):
            for xx in range(sprite.canvas.width):
                px = sprite.canvas.pixels[yy][xx]
                if px[3]:
                    atlas.pixels[ay + yy][ax + xx] = px
    return atlas


def write_png(path: Path, canvas: Canvas, scale: int = 1) -> None:
    raw = bytearray()
    for y in range(canvas.height * scale):
        raw.append(0)
        row = canvas.pixels[y // scale]
        for x in range(canvas.width * scale):
            raw.extend(bytes(row[x // scale]))

    def chunk(kind: bytes, data: bytes) -> bytes:
        return (struct.pack('>I', len(data)) + kind + data +
                struct.pack('>I', zlib.crc32(kind + data) & 0xFFFFFFFF))

    header = struct.pack('>IIBBBBB', canvas.width * scale,
                         canvas.height * scale, 8, 6, 0, 0, 0)
    data = (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', header) +
            chunk(b'IDAT', zlib.compress(bytes(raw), 9)) + chunk(b'IEND', b''))
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)


def write_inc(path: Path, sprites: list[Sprite], atlas: Canvas) -> None:
    lines = [
        '// Generated by scripts/generate-hardware-sprites.py; do not edit.',
        '// One row per sprite of engine/editor/assets/hardware_sprites.png:',
        '// atlas x, y, width, height; ground anchor x, y; then the screen',
        '// columns (x, top, bottom) its input and output ports sit on, all in',
        '// sprite pixels.',
        f'inline constexpr core::u32 hardware_sprite_atlas_width = {atlas.width}U;',
        f'inline constexpr core::u32 hardware_sprite_atlas_height = {atlas.height}U;',
        'inline constexpr std::array<HardwareSpriteFrame, '
        f'{len(sprites)}U> hardware_sprite_frames{{{{',
    ]
    for sprite in sprites:
        ax, ay = sprite.atlas
        lines.append(
            f'    {{{ax}, {ay}, {sprite.canvas.width}, {sprite.canvas.height}, '
            f'{int(sprite.anchor[0])}, {int(sprite.anchor[1])}, '
            f'{{{sprite.inputs[0]}, {sprite.inputs[1]}, {sprite.inputs[2]}}}, '
            f'{{{sprite.outputs[0]}, {sprite.outputs[1]}, {sprite.outputs[2]}}}}}, '
            f'// {sprite.name}')
    lines.append('}};')
    path.write_text('\n'.join(lines) + '\n', encoding='utf-8', newline='\n')


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--preview', type=Path,
                        help='also write an enlarged copy for viewing')
    args = parser.parse_args()
    # Order matches editor::HardwareSprite.
    sprites = [render_server(), operator_machine(), processor(False),
               processor(True), led_wall(), brompton_processor()]
    atlas = pack(sprites)
    write_png(PNG_PATH, atlas)
    write_inc(INC_PATH, sprites, atlas)
    if args.preview:
        write_png(args.preview, atlas, scale=4)
    print(f'wrote {PNG_PATH.relative_to(ROOT)} ({atlas.width}x{atlas.height}) '
          f'and {INC_PATH.relative_to(ROOT)}')


if __name__ == '__main__':
    main()
