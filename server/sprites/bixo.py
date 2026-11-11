"""Bixo, the slime: procedural pixel-art sprites, one animated GIF per emotion.

    uv run python sprites/bixo.py [--out sprites/out] [--scale 13]

The body is a stepped dome 14 px wide and 11 px tall, rim light and a
diagonal highlight on the left, 1x3 eyes three columns apart and a 2 px mouth. Everything is
drawn on a small logical canvas and scaled with nearest neighbour, so it stays crisp.

The body colour shows the battery: every animation also comes in greyer sets (`<name>_g1`,
`_g2`, `_g3`) that the firmware picks below half charge. `no_wifi` and `exhausted` are the
faces for no connection and an almost flat battery; `listening` and `error` replace the
"Escuchando…" and "Error" texts.

The GIFs are transparent around Bixo and have no ground shadow: the firmware draws them over a
background (backgrounds.py), which brings the shadow.
"""

import argparse
import math
from collections.abc import Callable
from dataclasses import dataclass, field, replace
from pathlib import Path

from PIL import Image

W, H = 22, 20          # logical canvas
CX = 11                # body centre: columns CX-1 | CX are the middle pair
GROUND = 17            # row of the bottom outline when resting

PALETTE = {
    "bg": (8, 12, 30),
    "ground": (16, 24, 56),
    "outline": (24, 44, 120),
    "shade": (38, 82, 200),
    "body": (64, 128, 250),
    "light": (110, 176, 255),
    "highlight": (226, 244, 255),
    "ink": (8, 14, 46),
    "white": (255, 255, 255),
    "pink": (255, 110, 168),
    "blush": (255, 150, 200),
    "tear": (140, 228, 255),
    "puff": (200, 214, 255),
    "gold": (255, 214, 92),
}

# Battery tiers: suffix -> how far the body colours fade to grey (the firmware picks one).
GREY_TIERS = {"": 0.0, "_g1": 0.4, "_g2": 0.7, "_g3": 0.92}
BODY_COLORS = ("outline", "shade", "body", "light", "highlight")


def greyed(amount: float, dim: float = 1.0) -> dict[str, tuple[int, int, int]]:
    """PALETTE with the body colours mixed towards their own grey (and optionally darkened)."""
    palette = dict(PALETTE)
    for name in BODY_COLORS:
        r, g, b = PALETTE[name]
        grey = 0.299 * r + 0.587 * g + 0.114 * b
        palette[name] = tuple(round(dim * (c + (grey - c) * amount)) for c in (r, g, b))
    return palette


# Half widths of each body row, top to bottom (row width = 2 * hw, centred on CX).
SHAPES = {
    "normal": [3, 4, 5, 6, 6, 7, 7, 7, 7, 6, 5],
    "squash": [4, 5, 6, 7, 8, 8, 8, 8, 7, 6],
    "flat": [5, 6, 7, 8, 9, 9, 9, 8, 7],
    "stretch": [2, 3, 4, 5, 6, 6, 6, 6, 6, 6, 5, 4],
}


@dataclass
class Pose:
    shape: str = "normal"
    dy: int = 0                 # vertical offset (negative = up)
    dx: int = 0
    turn: int = 0               # face (eyes and mouth) shifted sideways, as if turned (+ = right)
    eyes: str = "open"
    eyes_r: str | None = None   # right eye style when it differs
    look: tuple[int, int] = (0, 0)
    mouth: str = "small"
    blush: bool = False
    brows: str | None = None
    extras: list[tuple[str, int, int]] = field(default_factory=list)  # (glyph, x, y)
    ground: bool = True


class Canvas:
    def __init__(self) -> None:
        self.px: dict[tuple[int, int], str] = {}

    def put(self, x: int, y: int, color: str) -> None:
        if 0 <= x < W and 0 <= y < H:
            self.px[(x, y)] = color

    def image(self, scale: int, transparent: bool = False, palette: dict = PALETTE) -> Image.Image:
        img = Image.new("RGBA", (W, H), (0, 0, 0, 0)) if transparent else Image.new("RGB", (W, H), palette["bg"])
        for (x, y), color in self.px.items():
            img.putpixel((x, y), palette[color])
        return img.resize((W * scale, H * scale), Image.NEAREST)


# --- glyphs: tiny sprites for effects, drawn from their top-left corner ---------------------

GLYPHS = {
    "heart": ["pp.pp", "ppppp", ".ppp.", "..p.."],
    "heart_s": ["p.p", "ppp", ".p."],
    "z": ["www", "..w", ".w.", "w..", "www"],
    "q": [".ww.", "w..w", "..w.", ".w..", "....", ".w.."],
    "excl": ["g", "g", "g", ".", "g"],
    "dot": ["w"],
    "tear": ["t", "t"],
    "drop": [".t", "tt"],
    "puff": [".u.", "uuu"],
    "spark": [".w.", "www", ".w."],
    "note": [".ww", ".w.", "ww."],
    "wifi": [".uuuuu.", "u.....u", "..uuu..", ".u...u.", "...u..."],
    "wave_s": [".uu.", "u..u"],
    "wave": ["..uuuu..", ".u....u.", "u......u"],
    "slash": [".....p", "....p.", "...p..", "..p...", ".p...."],
    "cross": ["p...p", ".p.p.", "..p..", ".p.p.", "p...p"],
}
GLYPH_COLORS = {"p": "pink", "w": "white", "g": "gold", "t": "tear", "u": "puff"}
# Saturated enough to read on any background; an outline only makes them heavy.
UNOUTLINED = {"heart", "heart_s"}


def glyph(c: Canvas, name: str, x: int, y: int) -> None:
    """Off the body a glyph gets a dark outline, so it shows on light backgrounds too. Keep the
    glyph a pixel inside the canvas, or that side of the outline is cut off."""
    cells = {(x + i, y + j): GLYPH_COLORS[ch] for j, row in enumerate(GLYPHS[name])
             for i, ch in enumerate(row) if ch != "."}
    for (gx, gy) in cells if name not in UNOUTLINED else ():
        for ox in (-1, 0, 1):
            for oy in (-1, 0, 1):
                around = (gx + ox, gy + oy)
                if around not in cells and around not in c.px:
                    c.put(*around, "ink")
    for (gx, gy), color in cells.items():
        c.put(gx, gy, color)


# --- body and face -------------------------------------------------------------------------

def body_cells(pose: Pose) -> tuple[set[tuple[int, int]], int, int]:
    """Cells of the body, plus its top and bottom rows."""
    hws = SHAPES[pose.shape]
    bottom = GROUND + pose.dy
    top = bottom - len(hws) + 1
    cells = set()
    for i, hw in enumerate(hws):
        y = top + i
        for x in range(CX - hw + pose.dx, CX + hw + pose.dx):
            cells.add((x, y))
    return cells, top, bottom


def draw_body(c: Canvas, pose: Pose) -> tuple[int, int]:
    cells, top, bottom = body_cells(pose)
    if pose.ground:
        hw = SHAPES[pose.shape][-1] + 1 - min(2, max(0, -pose.dy))
        for x in range(CX - hw + pose.dx, CX + hw + pose.dx):
            c.put(x, GROUND + 1, "ground")
    for (x, y) in cells:
        edge = any((x + ox, y + oy) not in cells for ox, oy in ((1, 0), (-1, 0), (0, 1), (0, -1)))
        if edge:
            c.put(x, y, "outline")
            continue
        right_edge = (x + 1, y) not in cells or (x + 2, y) not in cells
        low_edge = (x, y + 1) not in cells or (x, y + 2) not in cells and y > top + 3
        left_edge = (x - 1, y) not in cells
        top_edge = (x, y - 1) not in cells
        if right_edge or low_edge:
            c.put(x, y, "shade")
        elif left_edge or top_edge:
            c.put(x, y, "light")
        else:
            c.put(x, y, "body")
    # A diagonal highlight: second interior pixel from the left, rows 2..7.
    for y in range(top + 2, min(top + 8, bottom - 2)):
        row = sorted(x for (x, yy) in cells if yy == y)
        if len(row) > 6:
            c.put(row[2], y, "highlight")
    return top, bottom


def shades(c: Canvas, lx: int, rx: int, ey: int) -> None:
    for x in range(lx - 2, rx + 3):
        c.put(x, ey + 1, "ink")
    for x in (lx - 1, lx, rx, rx + 1):
        c.put(x, ey + 2, "ink")
    c.put(lx - 1, ey + 1, "white")
    c.put(rx, ey + 1, "white")


def eye(c: Canvas, style: str, ex: int, ey: int, side: int) -> None:
    """Eye whose column is ex and top row ey; side is -1 for left, +1 for right."""
    ink = "ink"
    if style == "shades":
        if side < 0:  # one pair of glasses covers both eyes
            shades(c, ex, ex + 5, ey)
    elif style == "open":
        for y in range(ey, ey + 3):
            c.put(ex, y, ink)
    elif style == "half":
        c.put(ex, ey + 1, ink)
        c.put(ex, ey + 2, ink)
    elif style == "closed":
        c.put(ex, ey + 2, ink)
        c.put(ex + side, ey + 2, ink)
    elif style == "happy":       # ^
        c.put(ex, ey + 1, ink)
        c.put(ex + side, ey + 2, ink)
        c.put(ex - side, ey + 2, ink)
    elif style == "sad":         # lowered, inner corner raised
        c.put(ex, ey + 1, ink)
        c.put(ex, ey + 2, ink)
        c.put(ex - side, ey, ink)
    elif style == "big":
        for y in range(ey - 1, ey + 3):
            c.put(ex, y, ink)
            c.put(ex + side, y, ink)
        c.put(ex + side, ey - 1, "white")
    elif style == "dot":
        c.put(ex, ey + 1, ink)
    elif style == "x":           # > <
        c.put(ex + side, ey, ink)
        c.put(ex, ey + 1, ink)
        c.put(ex + side, ey + 2, ink)
    elif style == "heart":
        c.put(ex - 1, ey, "pink")
        c.put(ex + 1, ey, "pink")
        for x in range(ex - 1, ex + 2):
            c.put(x, ey + 1, "pink")
        c.put(ex, ey + 2, "pink")
    elif style == "spiral":
        c.put(ex, ey + 1, ink)
        c.put(ex + side, ey, ink)
        c.put(ex - side, ey + 2, ink)


def mouth(c: Canvas, style: str, mx: int, my: int) -> None:
    """mx is the left pixel of the 2 px mouth, my its row."""
    ink = "ink"
    shapes = {
        "small": [(0, 0), (1, 0)],
        "dot": [(1, 0)],
        "smile": [(-1, -1), (0, 0), (1, 0), (2, -1)],
        "frown": [(-1, 0), (0, -1), (1, -1), (2, 0)],
        "smirk": [(0, 0), (1, 0), (2, -1)],
        "wavy": [(-1, 0), (0, -1), (1, 0), (2, -1)],
        "kiss": [(1, -1), (2, 0), (1, 0)],
    }
    if style in shapes:
        for ox, oy in shapes[style]:
            c.put(mx + ox, my + oy, ink)
    elif style == "open":
        for ox in (0, 1):
            c.put(mx + ox, my - 1, ink)
            c.put(mx + ox, my, ink)
    elif style == "o":
        for ox in (0, 1):
            for oy in (-2, -1, 0):
                c.put(mx + ox, my + oy, ink)
    elif style == "grin":
        for ox in range(-1, 3):
            c.put(mx + ox, my - 1, ink)
        c.put(mx, my, ink)
        c.put(mx + 1, my, "pink")
    elif style == "laugh":
        for ox in range(-1, 3):
            c.put(mx + ox, my - 1, ink)
        for ox in range(0, 2):
            c.put(mx + ox, my, "pink")
        c.put(mx - 1, my, ink)
        c.put(mx + 2, my, ink)
    elif style == "tongue":
        c.put(mx, my, ink)
        c.put(mx + 1, my, ink)
        c.put(mx + 1, my + 1, "pink")
    elif style == "yum":
        for ox, oy in [(-1, -1), (0, 0), (1, 0), (2, -1)]:
            c.put(mx + ox, my + oy, ink)
        c.put(mx + 2, my, "pink")
        c.put(mx + 2, my + 1, "pink")


def draw(pose: Pose, scale: int, transparent: bool = False, palette: dict = PALETTE) -> Image.Image:
    c = Canvas()
    top, bottom = draw_body(c, pose)
    face_y = top + round(0.4 * (bottom - top))
    ey = face_y + pose.look[1]
    lx, rx = CX - 3 + pose.dx + pose.turn + pose.look[0], CX + 2 + pose.dx + pose.turn + pose.look[0]
    eye(c, pose.eyes, lx, ey, -1)
    eye(c, pose.eyes_r or pose.eyes, rx, ey, +1)
    if pose.brows == "angry":
        c.put(lx - 1, ey - 2, "ink"); c.put(lx, ey - 1, "ink")
        c.put(rx + 1, ey - 2, "ink"); c.put(rx, ey - 1, "ink")
    elif pose.brows == "worried":
        c.put(lx, ey - 2, "ink"); c.put(lx - 1, ey - 1, "ink")
        c.put(rx, ey - 2, "ink"); c.put(rx + 1, ey - 1, "ink")
    if pose.blush:
        for x in (lx - 2, lx - 1, rx + 1, rx + 2):
            c.put(x, ey + 3, "blush")
    my = min(face_y + 4, bottom - 2)
    mouth(c, pose.mouth, CX - 1 + pose.dx + pose.turn, my)
    for name, x, y in pose.extras:
        glyph(c, name, x, y)
    return c.image(scale, transparent, palette)


# --- animations: list of (pose, milliseconds) per emotion ----------------------------------

Frames = list[tuple[Pose, int]]


def breathe(base: Pose, beats: int = 2, blink: bool = True, ms: int = 420) -> Frames:
    frames: Frames = []
    for i in range(beats):
        frames.append((Pose(**{**base.__dict__}), ms * 2))
        frames.append((Pose(**{**base.__dict__, "shape": "squash"}), ms))
    if blink:
        frames.insert(1, (Pose(**{**base.__dict__, "eyes": "closed", "eyes_r": None}), 110))
    return frames


def p(**kw) -> Pose:
    return Pose(**kw)


def anim_neutral() -> Frames:
    base = p()
    return [
        (base, 1400), (p(eyes="closed"), 120), (base, 900), (p(shape="squash"), 450),
        (base, 900), (p(look=(1, 0)), 700), (p(look=(-1, 0)), 700), (base, 500), (p(shape="squash"), 450),
    ]


def hop(face: dict, height: int = 3, ms: int = 70, rest: int = 300) -> Frames:
    up = [p(shape="squash", **face), p(shape="stretch", dy=-1, **face)]
    up += [p(dy=-d, **face) for d in range(2, height + 1)]
    down = list(reversed(up[2:])) + [p(shape="stretch", dy=-1, **face), p(shape="flat", **face), p(shape="squash", **face)]
    return [(f, ms) for f in up + down] + [(p(**face), rest)]


def anim_happy() -> Frames:
    face = dict(eyes="happy", mouth="smile")
    return hop(face) + hop(face) + [(p(**face), 600)]


def anim_laughing() -> Frames:
    frames: Frames = []
    for i in range(6):
        frames.append((p(eyes="x", mouth="laugh", shape="squash" if i % 2 else "normal", dy=-(i % 2)), 110))
    return frames + [(p(eyes="happy", mouth="laugh"), 300)]


def anim_funny() -> Frames:
    frames: Frames = []
    for t in (0, 1, 0, -1) * 2:
        frames.append((p(eyes="happy", eyes_r="closed", mouth="grin", dx=t, shape="squash" if t else "normal"), 180))
    return frames


def anim_sad() -> Frames:
    face = dict(eyes="half", mouth="frown", brows="worried")
    return [(p(shape="squash", **face), 900), (p(shape="flat", **face), 1100), (p(shape="squash", **face), 900),
            (p(shape="squash", eyes="closed", mouth="frown"), 140)]


def anim_crying() -> Frames:
    frames: Frames = []
    lx, rx = CX - 3, CX + 2
    for i in range(6):
        y = GROUND - 3 + i % 3
        extras = [("tear", lx - 1, y), ("tear", rx + 1, y + 1 if i % 3 < 2 else y - 2)]
        frames.append((p(shape="squash" if i % 2 else "flat", eyes="closed", mouth="frown", extras=extras), 160))
    return frames


def anim_angry() -> Frames:
    frames: Frames = []
    face = dict(eyes="open", mouth="frown", brows="angry")
    for i in range(8):
        puffs = [("puff", CX - 6 + (i % 2), 5 - i % 3), ("puff", CX + 4 - (i % 2), 5 - (i + 1) % 3)]
        frames.append((p(dx=(1 if i % 2 else -1), shape="squash", extras=puffs, **face), 130))
    return frames + [(p(shape="squash", **face), 500)]


def anim_loving() -> Frames:
    frames: Frames = []
    for i in range(8):
        hearts = [("heart", CX - 7, 6 - i // 2), ("heart_s", CX + 4, 4 - (i + 3) // 3)]
        shape = "squash" if i % 4 == 3 else "normal"
        frames.append((p(shape=shape, eyes="happy", mouth="smile", blush=True, extras=hearts), 180))
    return frames


def anim_embarrassed() -> Frames:
    sweat = [("drop", CX + 6, 8)]
    return [
        (p(eyes="half", look=(-1, 1), mouth="wavy", blush=True, extras=sweat), 700),
        (p(shape="squash", eyes="closed", mouth="wavy", blush=True, extras=sweat), 160),
        (p(eyes="half", look=(1, 1), mouth="wavy", blush=True, extras=sweat), 700),
        (p(shape="squash", eyes="half", look=(0, 1), mouth="small", blush=True, extras=sweat), 500),
    ]


def anim_surprised() -> Frames:
    face = dict(eyes="big", mouth="o")
    return [(p(shape="squash", eyes="open"), 120), (p(shape="stretch", dy=-2, extras=[("excl", CX, 1)], **face), 140),
            (p(dy=-1, extras=[("excl", CX, 1)], **face), 120), (p(extras=[("excl", CX, 2)], **face), 900),
            (p(eyes="closed", mouth="o"), 120), (p(**face), 700)]


def anim_shocked() -> Frames:
    face = dict(eyes="dot", mouth="o")
    frames = [(p(shape="squash", eyes="open"), 100), (p(shape="stretch", dy=-3, extras=[("excl", CX - 2, 1), ("excl", CX + 1, 1)], **face), 120)]
    for i in range(6):
        frames.append((p(shape="stretch", dx=(1 if i % 2 else -1), dy=-1, extras=[("excl", CX - 2, 1), ("excl", CX + 1, 1)], **face), 120))
    return frames + [(p(**face), 800)]


def anim_thinking() -> Frames:
    frames: Frames = []
    for n in range(4):
        dots = [("dot", CX + 4 + 2 * i, 5 - i) for i in range(n)]
        frames.append((p(look=(1, -1), mouth="smirk", extras=dots), 450))
    frames.append((p(look=(1, -1), mouth="smirk", eyes="closed", extras=[("dot", CX + 4 + 2 * i, 5 - i) for i in range(3)]), 120))
    frames.append((p(shape="squash", look=(1, -1), mouth="smirk", extras=[("dot", CX + 4 + 2 * i, 5 - i) for i in range(3)]), 600))
    return frames


def anim_winking() -> Frames:
    return [(p(mouth="smile"), 600), (p(eyes_r="closed", mouth="smile", extras=[("spark", CX + 6, 6)]), 120),
            (p(eyes="open", eyes_r="happy", mouth="smile", extras=[("spark", CX + 6, 5)]), 900),
            (p(shape="squash", mouth="smile"), 400)]


def anim_cool() -> Frames:
    frames: Frames = []
    for i in range(6):
        extras = [("note", CX + 6, 5 - i % 3)] if i % 3 != 2 else []
        frames.append((p(eyes="shades", mouth="smirk", shape="squash" if i % 2 else "normal", extras=extras), 260))
    return frames


def anim_relaxed() -> Frames:
    return breathe(p(eyes="closed", mouth="smile"), beats=3, blink=False, ms=650)


def anim_delicious() -> Frames:
    return [(p(eyes="happy", mouth="yum"), 300), (p(shape="squash", eyes="happy", mouth="yum"), 200)] * 3 + \
           [(p(eyes="happy", mouth="smile", extras=[("spark", CX - 7, 6)]), 600)]


def anim_kissy() -> Frames:
    frames: Frames = []
    for i in range(6):
        extras = [("heart_s", CX + 3 + i // 2, 9 - i)] if i else []
        frames.append((p(eyes="closed", mouth="kiss", blush=True, extras=extras), 200))
    return frames + [(p(eyes="happy", mouth="smile", blush=True), 500)]


def anim_confident() -> Frames:
    return [(p(eyes="half", mouth="smirk"), 900), (p(eyes="half", mouth="smirk", extras=[("spark", CX - 6, 8)]), 160),
            (p(eyes="half", mouth="smirk", extras=[("spark", CX - 6, 7)]), 160), (p(shape="squash", eyes="half", mouth="smirk"), 600)]


def anim_sleepy() -> Frames:
    """Breathing slowly, with two z's over his head bobbing a pixel, out of step."""
    frames: Frames = []
    bob = (0, 0, 1, 1)
    for i in range(8):
        zs = [("z", CX + 1, 3 + bob[i % 4]), ("z", CX + 7, 1 + bob[(i + 2) % 4])]
        frames.append((p(shape="flat" if i % 4 >= 2 else "squash", eyes="closed", mouth="dot", extras=zs), 450))
    return frames


def anim_silly() -> Frames:
    frames: Frames = []
    for t in (-1, 0, 1, 0):
        frames.append((p(eyes="open", eyes_r="dot", mouth="tongue", dx=t), 180))
        frames.append((p(eyes="dot", eyes_r="open", mouth="tongue", dx=t, shape="squash"), 180))
    return frames


def anim_confused() -> Frames:
    q = [("q", CX + 5, 2)]
    return [(p(eyes="open", eyes_r="half", mouth="wavy", dx=1, extras=q), 600),
            (p(eyes="open", eyes_r="half", mouth="wavy", extras=q), 300),
            (p(eyes="half", eyes_r="open", mouth="wavy", dx=-1, extras=[("q", CX + 5, 1)]), 600),
            (p(eyes="spiral", mouth="wavy", extras=q), 500)]


def anim_no_wifi() -> Frames:
    """Lost: looks around for the signal, which blinks above its head."""
    face = dict(mouth="wavy", brows="worried")
    sign = [("wifi", CX - 4, 1), ("slash", CX - 4, 1)]
    return [(p(look=(-1, 0), extras=sign, **face), 700), (p(look=(-1, 0), **face), 300),
            (p(look=(1, 0), extras=sign, **face), 700), (p(look=(1, 0), **face), 300),
            (p(look=(0, -1), extras=sign, **face), 900), (p(shape="squash", eyes="closed", extras=sign, **face), 140),
            (p(shape="squash", look=(0, -1), **face), 500)]


def anim_listening() -> Frames:
    """Lending an ear: the face turned to one side, a small "o", and dots of sound arriving."""
    face = dict(turn=2, mouth="open")
    dots = [("dot", CX - 9 + 2 * i, 3) for i in range(3)]
    return [(p(extras=dots[:1], **face), 350), (p(extras=dots[:2], **face), 350),
            (p(extras=dots, **face), 700), (p(extras=dots, eyes="closed", **face), 110),
            (p(extras=dots, **face), 500), (p(**face), 400)]


def anim_speaking() -> Frames:
    """Talking: the mouth opens and closes at an uneven pace, with a small nod on the stresses."""
    beats = [("open", 140), ("small", 110), ("o", 160), ("small", 90), ("open", 120), ("dot", 130),
             ("o", 140), ("small", 100), ("open", 170), ("small", 220)]
    frames: Frames = []
    for i, (m, ms) in enumerate(beats):
        frames.append((p(mouth=m, shape="squash" if m == "o" else "normal"), ms))
        if i == 5:
            frames.append((p(mouth=m, eyes="closed"), 110))
    return frames


def anim_connecting() -> Frames:
    """Reaching out to the server: eyes up, and waves rising from his head one after the other."""
    face = dict(look=(0, -1), mouth="dot")
    near, far = ("wave_s", CX - 2, 4), ("wave", CX - 4, 1)
    return [(p(**face), 300), (p(extras=[near], **face), 300), (p(extras=[near, far], **face), 300),
            (p(extras=[far], **face), 300), (p(**face), 200), (p(extras=[near], **face), 300),
            (p(extras=[near, far], **face), 300), (p(extras=[far], eyes="closed", **face), 110),
            (p(extras=[far], **face), 190)]


def anim_error() -> Frames:
    """Something failed: a shiver, a red cross over its head, then a sorry droop."""
    face = dict(eyes="sad", mouth="frown", brows="worried")
    cross = [("cross", CX - 3, 1)]
    frames: Frames = [(p(shape="squash", eyes="open", mouth="o"), 120)]
    for i in range(6):
        frames.append((p(dx=(1 if i % 2 else -1), extras=cross, **face), 90))
    return frames + [(p(extras=cross, **face), 900), (p(shape="squash", **face), 500),
                     (p(shape="squash", extras=cross, **face), 900), (p(shape="flat", eyes="closed", mouth="frown"), 160)]


def anim_exhausted() -> Frames:
    """Battery almost flat: melted, eyelids heavy, slow breaths and a sigh now and then."""
    face = dict(mouth="frown")
    return [(p(shape="flat", eyes="half", **face), 1400), (p(shape="flat", eyes="closed", **face), 700),
            (p(shape="flat", eyes="half", look=(0, 1), **face), 1200), (p(shape="squash", eyes="half", **face), 900),
            (p(shape="flat", eyes="closed", mouth="dot", extras=[("puff", CX + 6, 8)]), 600),
            (p(shape="flat", eyes="closed", mouth="dot", extras=[("puff", CX + 7, 6)]), 500)]


ANIMATIONS: dict[str, Callable[[], Frames]] = {
    "neutral": anim_neutral, "happy": anim_happy, "laughing": anim_laughing, "funny": anim_funny,
    "sad": anim_sad, "crying": anim_crying, "angry": anim_angry, "loving": anim_loving,
    "embarrassed": anim_embarrassed, "surprised": anim_surprised, "shocked": anim_shocked,
    "thinking": anim_thinking, "winking": anim_winking, "cool": anim_cool, "relaxed": anim_relaxed,
    "delicious": anim_delicious, "kissy": anim_kissy, "confident": anim_confident,
    "sleepy": anim_sleepy, "silly": anim_silly, "confused": anim_confused, "no_wifi": anim_no_wifi,
    "listening": anim_listening, "speaking": anim_speaking, "connecting": anim_connecting, "error": anim_error,
}
EXHAUSTED_PALETTE = greyed(1.0, dim=0.8)


def save_gif(frames: Frames, path: Path, scale: int, palette: dict = PALETTE) -> None:
    """Index 0 is the "bg" colour, saved as transparent; each frame clears the previous one."""
    images = [draw(replace(pose, ground=False), scale, palette=palette) for pose, _ in frames]
    palette_img = Image.new("P", (1, 1))
    flat = [v for rgb in palette.values() for v in rgb]
    palette_img.putpalette(flat + [0] * (768 - len(flat)))
    quantized = [im.quantize(palette=palette_img, dither=Image.Dither.NONE) for im in images]
    quantized[0].save(path, save_all=True, append_images=quantized[1:], duration=[ms for _, ms in frames],
                      loop=0, disposal=2, transparency=0, optimize=False)


def contact_sheet(out: Path, scale: int) -> None:
    cells = [(make, PALETTE) for make in ANIMATIONS.values()]
    cells += [(anim_neutral, greyed(amount)) for amount in list(GREY_TIERS.values())[1:]]
    cells += [(anim_exhausted, EXHAUSTED_PALETTE)]
    cols = 7
    rows = math.ceil(len(cells) / cols)
    sheet = Image.new("RGB", (cols * W * scale, rows * H * scale), PALETTE["bg"])
    for i, (make, palette) in enumerate(cells):
        frames = make()
        key = max(frames, key=lambda f: f[1])[0]  # the longest-held frame is the "pose"
        sheet.paste(draw(key, scale, palette=palette), ((i % cols) * W * scale, (i // cols) * H * scale))
    sheet.save(out / "sheet.png")


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=str(Path(__file__).parent / "out"))
    ap.add_argument("--scale", type=int, default=13)
    args = ap.parse_args()
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    for stale in out.glob("*.gif"):
        stale.unlink()
    for name, make in ANIMATIONS.items():
        frames = make()
        for suffix, amount in GREY_TIERS.items():
            save_gif(frames, out / f"{name}{suffix}.gif", args.scale, greyed(amount))
    save_gif(anim_exhausted(), out / "exhausted.gif", args.scale, EXHAUSTED_PALETTE)
    contact_sheet(out, 6)
    count = len(list(out.glob("*.gif")))
    print(f"{count} GIFs + sheet.png in {out} ({W * args.scale}x{H * args.scale})")


if __name__ == "__main__":
    main()
