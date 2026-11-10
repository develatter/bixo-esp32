"""Bixo's screen backgrounds: pixel art on the same 13 px grid as his sprites, lightly animated.

    uv run python sprites/backgrounds.py [--out sprites/out/backgrounds] [--mockups]

Each background is a loop of 412x412 frames for the round screen. Its grid lines up with the
sprite's, so Bixo's pixels and the scenery's are the same size and sit on the same lattice.
Outdoors the horizon is high and bulges a little, like a small planet, so Bixo stands on the
ground in front of it instead of on a flat line. The ground shadow under Bixo belongs to the
background (the sprites are transparent over it), kept apart so the firmware can leave it out
when Bixo is hidden (choosing a background).

pack_assets.py stores each one for the firmware as its cells, not its pixels (see `pack`): the
Watcher paints the 13 px squares itself and only repaints the ones that change.
"""

import argparse
import math
import random
import struct
from collections.abc import Callable
from dataclasses import dataclass
from pathlib import Path

from PIL import Image, ImageDraw

import bixo

SCREEN = 412
CELL = 13
ORIGIN = -2                        # pixel of cell 0, chosen so the sprite lands on cells
COLS = ROWS = 33
SPRITE_COL, SPRITE_ROW = 5, 3      # cell of the sprite's top-left corner on screen
GROUND = bixo.GROUND + SPRITE_ROW  # Bixo's bottom outline
SHADOW = GROUND + 1
MID = 15.5                         # the screen's vertical centre line, between cells 15 and 16
HORIZON = 14                       # horizon row in the middle of the screen
CURVE = 120                        # how much it drops towards the edges (higher = flatter)

RGB = tuple[int, int, int]
WHITE: RGB = (255, 255, 255)


def horizon(x: int) -> int:
    return HORIZON + round((x - MID) ** 2 / CURVE)


def mirror_x(x: int, width: int = 1) -> int:
    """Left edge of the copy of a `width`-wide thing at x, mirrored across the screen centre."""
    return COLS - 2 - x - width + 1


class Scene:
    def __init__(self, fill: RGB) -> None:
        self.px: dict[tuple[int, int], RGB] = {(x, y): fill for x in range(COLS) for y in range(ROWS)}
        self.shade: tuple[int, int, RGB] | None = None  # Bixo's shadow: first and end column, colour

    def put(self, x: int, y: int, color: RGB) -> None:
        if 0 <= x < COLS and 0 <= y < ROWS:
            self.px[(x, y)] = color

    def get(self, x: int, y: int) -> RGB | None:
        return self.px.get((x, y))

    def bands(self, colors: list[RGB], height: int, top: int = 0) -> None:
        for i, color in enumerate(colors):
            for y in range(top + i * height, top + (i + 1) * height):
                for x in range(COLS):
                    self.put(x, y, color)

    def ground(self, layers: list[tuple[int, RGB]]) -> None:
        """Fill below the curved horizon; each layer starts `depth` rows under it."""
        for x in range(COLS):
            h = horizon(x)
            for y in range(h, ROWS):
                color = layers[0][1]
                for depth, c in layers:
                    if y - h >= depth:
                        color = c
                self.put(x, y, color)

    def shape(self, art: list[str], x: int, y: int, colors: dict[str, RGB], flip: bool = False) -> None:
        for j, row in enumerate(art):
            for i, ch in enumerate(row[::-1] if flip else row):
                if ch != ".":
                    self.put(x + i, y + j, colors[ch])

    def pair(self, art: list[str], x: int, y: int, colors: dict[str, RGB]) -> None:
        """The same motif on both sides, mirrored."""
        self.shape(art, x, y, colors)
        self.shape(art, mirror_x(x, len(art[0])), y, colors, flip=True)

    def shadow(self, color: RGB, half: int = 6) -> None:
        """Bixo's shadow on row SHADOW, drawn over the scene when he is on screen."""
        cx = bixo.CX + SPRITE_COL
        self.shade = (cx - half, cx + half, color)

    def cells(self, shadow: bool) -> dict[tuple[int, int], RGB]:
        px = dict(self.px)
        if shadow and self.shade:
            x0, x1, color = self.shade
            for x in range(x0, x1):
                px[(x, SHADOW)] = color
        return px

    def image(self, shadow: bool = True) -> Image.Image:
        img = Image.new("RGB", (SCREEN, SCREEN))
        draw = ImageDraw.Draw(img)
        for (x, y), color in self.cells(shadow).items():
            px, py = ORIGIN + x * CELL, ORIGIN + y * CELL
            draw.rectangle((px, py, px + CELL - 1, py + CELL - 1), fill=color)
        return img


def visible(x: int, y: int, margin: float = 0.0) -> bool:
    """Whether a cell's centre falls inside the round screen."""
    px, py = ORIGIN + x * CELL + CELL / 2, ORIGIN + y * CELL + CELL / 2
    return math.hypot(px - SCREEN / 2, py - SCREEN / 2) < SCREEN / 2 - margin * CELL


def scatter(rng: random.Random, count: int, x0: int, x1: int, y0: int, y1: int, gap: int = 3,
            keep: Callable[[int, int], bool] = lambda x, y: True) -> list[tuple[int, int]]:
    """Spots inside the round screen, apart from each other."""
    spots: list[tuple[int, int]] = []
    for _ in range(count * 60):
        if len(spots) == count:
            break
        x, y = rng.randrange(x0, x1), rng.randrange(y0, y1)
        if visible(x, y, 0.8) and keep(x, y) and all(abs(x - a) + abs(y - b) >= gap for a, b in spots):
            spots.append((x, y))
    return spots


def off_bixo(x: int, y: int) -> bool:
    left, right = bixo.CX - 8 + SPRITE_COL, bixo.CX + 7 + SPRITE_COL
    return not (left <= x <= right and SPRITE_ROW + 5 <= y <= SHADOW + 1)


# --- the backgrounds --------------------------------------------------------------------------

@dataclass
class Background:
    draw: Callable[[int], Scene]
    label: str                     # as the menu shows it
    frames: int = 1
    ms: int = 200
    text: RGB = bixo.PALETTE["highlight"]  # subtitles, over a translucent band of `panel`
    panel: RGB = bixo.PALETTE["bg"]


def bg_noche(t: int) -> Scene:
    s = Scene(bixo.PALETTE["bg"])
    s.shadow(bixo.PALETTE["ground"])
    return s


def bg_blanco(t: int) -> Scene:
    s = Scene((236, 234, 226))
    s.shadow((204, 202, 194))
    return s


def bg_prado(t: int) -> Scene:
    s = Scene((140, 208, 255))
    s.bands([(140, 208, 255), (160, 218, 255), (180, 228, 255), (200, 236, 255)], 3, top=2)
    s.shape(["..ww...", ".wwwww.", "wwwwwww"], 5, 4, {"w": WHITE})
    s.shape([".ww.", "wwww"], 22, 7, {"w": WHITE})
    far, grass, near, edge = (128, 204, 112), (96, 184, 84), (80, 164, 72), (150, 220, 124)
    s.ground([(0, edge), (1, far), (4, grass), (11, near)])
    for x in (3, 8, 23, 27):  # little trees on the horizon, the far ones small
        h = horizon(x)
        s.shape([".t.", "ttt", ".k."], x - 1, h - 3, {"t": (60, 140, 70), "k": (110, 80, 50)})
    s.shadow((64, 140, 60))
    rng = random.Random(4)
    sway = t < 4
    for x, y in scatter(rng, 9, 1, COLS - 1, SHADOW + 1, ROWS - 1, gap=4, keep=off_bixo):
        tuft = [".g", "g."] if sway else ["g.", ".g"]
        s.shape(tuft, x, y, {"g": (60, 140, 56)})
    flowers = [(255, 220, 90), (255, 130, 180), WHITE]
    for i, (x, y) in enumerate(scatter(rng, 7, 1, COLS - 1, SHADOW + 1, ROWS - 1, gap=4, keep=off_bixo)):
        s.put(x, y, flowers[i % 3])
        s.put(x, y + 1, (60, 140, 56))
    # a butterfly looping above the meadow, wings open and shut
    path = [(6, 12), (7, 11), (8, 11), (9, 12), (9, 13), (8, 14), (7, 14), (6, 13)]
    bx, by = path[t % 8]
    wing = (255, 160, 60)
    if t % 2:
        s.put(bx - 1, by, wing)
        s.put(bx + 1, by, wing)
    else:
        s.put(bx - 1, by - 1, wing)
        s.put(bx + 1, by - 1, wing)
    s.put(bx, by, (60, 40, 30))
    return s


def bg_mazmorra(t: int) -> Scene:
    mortar, brick, brick_d = (46, 40, 54), (66, 60, 76), (58, 52, 68)
    s = Scene(mortar)
    floor_top = HORIZON + 4
    for y in range(0, floor_top + 3):
        row = y // 3
        if y % 3 == 2:
            continue
        for x in range(COLS):
            if (x + (row % 2) * 3) % 6 == 5:
                continue
            tone = brick
            if (x * 7 + row * 5) % 13 == 0:
                tone = brick_d
            s.put(x, y, tone)
    # the floor: packed stone, its edge curving like the outdoor horizons, and a red carpet
    floor, floor_l = (54, 46, 52), (66, 58, 64)
    for x in range(COLS):
        h = floor_top + round((x - MID) ** 2 / (CURVE * 1.5))
        for y in range(h, ROWS):
            s.put(x, y, floor_l if y == h else floor)
    carpet, trim = (116, 40, 52), (160, 120, 64)
    for y in range(floor_top + 1, ROWS):
        half = 4 + (y - floor_top) // 3
        for x in range(round(MID - half + 0.5), round(MID + half + 0.5)):
            edge = x in (round(MID - half + 0.5), round(MID + half - 0.5))
            s.put(x, y, trim if edge else carpet)
    s.shadow((80, 26, 36))
    # two blue banners and two torches
    banner = ["bbbb", "bggb", "bbbb", "bbbb", "b..b"]
    s.pair(banner, 6, 3, {"b": (56, 92, 180), "g": (200, 170, 80)})
    flames = [[".f.", "fyf"], ["f..", "fyf"], [".f.", "fyf"], ["..f", "fyf"], [".y.", "fyf"], [".f.", "yyf"]]
    glow = (112, 86, 72) if t % 3 else (124, 96, 78)
    for x in (2, mirror_x(2, 3)):
        for dx, dy in ((-1, 1), (3, 1), (1, -1), (-1, 3), (3, 3)):
            if s.get(x + dx, 10 + dy) != mortar:
                s.put(x + dx, 10 + dy, glow)
        s.shape(flames[(t + (x > MID) * 2) % 6], x, 10, {"f": (255, 120, 40), "y": (255, 230, 120)})
        s.shape([".h.", ".h.", "hhh"], x, 12, {"h": (110, 80, 50)})
    return s


SYNTH_SKY = [(26, 8, 56), (40, 12, 76), (64, 18, 100), (100, 26, 124), (146, 38, 138), (200, 60, 144)]
SYNTH_RISE = 12  # rows a neon line climbs before it fades out


def lerp(a: RGB, b: RGB, k: float) -> RGB:
    return tuple(round(x + (y - x) * k) for x, y in zip(a, b))


def synth_sky(y: int) -> RGB:
    k = max(0.0, min(1.0, (y / HORIZON) ** 1.2)) * (len(SYNTH_SKY) - 1)
    i = min(int(k), len(SYNTH_SKY) - 2)
    return lerp(SYNTH_SKY[i], SYNTH_SKY[i + 1], k - i)


def bg_synthwave(t: int) -> Scene:
    """A still sunset sky with neon lines rising from the horizon and fading as they climb."""
    s = Scene(SYNTH_SKY[0])
    for y in range(ROWS):
        for x in range(COLS):
            s.put(x, y, synth_sky(y))
    for k in range(3):
        age = (t + k * SYNTH_RISE // 3) % SYNTH_RISE
        y = HORIZON - 1 - age
        for x in range(COLS):
            if y < horizon(x):
                s.put(x, y, lerp(synth_sky(y), (255, 90, 210), 0.7 * (1 - age / SYNTH_RISE)))
    mountains = (70, 16, 100)
    for x in range(COLS):
        height = max(0, round(3 - abs((x % 11) - 5) * 0.8)) if abs(x - MID) > 7 else 0
        for y in range(horizon(x) - height, horizon(x)):
            s.put(x, y, mountains)
    s.ground([(0, (255, 70, 200)), (1, (60, 14, 90)), (4, (44, 10, 72)), (10, (30, 6, 52))])
    s.shadow((22, 4, 40))
    return s


def bg_navidad(t: int) -> Scene:
    s = Scene((14, 22, 54))
    s.bands([(14, 22, 54), (18, 28, 64), (22, 34, 74)], 4, top=4)
    s.ground([(0, (250, 252, 255)), (1, (236, 244, 255)), (8, (222, 232, 250))])
    s.shadow((190, 206, 236))
    tree = ["...s...", "..ggg..", ".gRgYg.", "..ggg..", ".ggggg.", "gYgggRg", ".ggggg.",
            "ggRgggg", "...t..."]
    lights = {"R": (240, 70, 80), "Y": (255, 220, 90)}
    if t % 4 >= 2:
        lights = {"R": (255, 220, 90), "Y": (240, 70, 80)}
    colors = {"g": (40, 120, 70), "s": (255, 230, 120), "t": (110, 70, 40), **lights}
    s.shape(tree, 3, horizon(3) - 7, colors)
    # a little house back by the horizon, its eave strung with twinkling lights
    house = ["...s...", "..sss..", ".sssss.", "LLLLLLL", ".wwwww.", ".wYwdw.", ".wwwdw."]
    hx = mirror_x(1, 7)
    hy = horizon(hx + 3) + 1 - len(house) + 1
    s.shape(house, hx, hy, {"s": WHITE, "L": WHITE, "w": (150, 92, 60), "Y": (255, 214, 92),
                            "d": (96, 56, 36)})
    bulbs = [(240, 70, 80), (255, 220, 90), (90, 200, 120)]
    for i in range(7):
        s.put(hx + i, hy + 3, bulbs[(i + t // 4) % 3])
    # two small presents under the tree
    s.shape([".y.", "ryr", "ryr"], 2, horizon(3) + 1, {"r": (220, 56, 70), "y": (255, 214, 92)})
    s.shape(["yry", "yry"], 6, horizon(3) + 2, {"y": (255, 214, 92), "r": (220, 56, 70)})
    rng = random.Random(12)
    for x, y0 in scatter(rng, 14, 0, COLS, 0, 16, gap=3):
        for band in (0, 16):
            y = (y0 + t) % 16 + band
            x_sway = x + (1 if (y // 3) % 2 else 0)
            if s.get(x_sway, y) is not None and y < horizon(x_sway) + 1:
                s.put(x_sway, y, WHITE)
    return s


def bg_verano(t: int) -> Scene:
    s = Scene((86, 186, 255))
    s.bands([(86, 186, 255), (110, 200, 255), (136, 214, 255)], 3, top=4)
    s.shape(["..yyy..", ".yyyyy.", "yyyyyyy", "yyyyyyy", "yyyyyyy", ".yyyyy.", "..yyy.."],
            20, 2, {"y": (255, 222, 90)})
    sea, sea_d, foam = (40, 150, 210), (30, 124, 190), (240, 250, 255)
    shore = 2  # rows of sea before the sand: Bixo sits well into the beach
    wave = t // 3
    for x in range(COLS):
        h = horizon(x)
        for y in range(h, ROWS):
            d = y - h
            if d == 0:
                color = sea_d
            elif d < shore:
                color = sea if (x + d * 3 + wave) % 8 else (120, 210, 240)
            elif d == shore:
                color = foam if (x + wave) % 4 else (120, 210, 240)
            else:
                color = (244, 214, 150) if d < shore + 9 else (236, 202, 134)
            s.put(x, y, color)
    s.shadow((214, 178, 112))
    rng = random.Random(3)
    for x, y in scatter(rng, 6, 1, COLS - 1, SHADOW + 1, ROWS - 1, gap=4, keep=off_bixo):
        s.put(x, y, (222, 186, 120))
    s.shape([".p.", "ppp"], 11, ROWS - 5, {"p": (255, 160, 170)})
    ball = [".rw.", "rwwb", "rwbb", ".wb."]
    s.shape(ball, 24, SHADOW - 2, {"r": (240, 70, 80), "w": WHITE, "b": (60, 120, 230)})
    # the palm bends in the breeze: the top of the trunk leans and the fronds follow
    trunk, leaf = (150, 100, 60), (60, 170, 80)
    lean = (t // 6) % 2
    base = horizon(4) + shore + 2
    for y in range(9, base):
        s.put(4 + (1 if y < 15 else 0) + (lean if y < 12 else 0), y, trunk)
    fronds = [["ll...ll", ".lllll.", "l..l..l"], [".l...l.", "llllll.", "l..l..."]]
    s.shape(fronds[lean], 2 + lean, 7, {"l": leaf})
    s.put(5 + lean, 10, (110, 70, 40)), s.put(6 + lean, 10, (110, 70, 40))  # coconuts
    return s


def bg_otono(t: int) -> Scene:
    s = Scene((246, 186, 120))
    s.bands([(246, 186, 120), (250, 200, 140), (252, 214, 160), (254, 226, 180)], 3, top=2)
    s.ground([(0, (200, 120, 60)), (1, (178, 108, 56)), (5, (156, 94, 50)), (11, (136, 82, 44))])
    s.shadow((112, 66, 36))
    trunk, trunk_d = (110, 70, 44), (84, 52, 32)
    orange, red, gold, dark = (232, 122, 40), (200, 64, 40), (240, 186, 70), (176, 72, 32)
    leaves = {"o": orange, "g": gold, "r": red, "d": dark, "k": trunk}
    # leafy, lumpy crowns with branches showing through, so they don't read as lollipops
    big = ["....oog.oo....", "..oogooooooo..", ".ogoooroooogoo", "ooooooooooooro", "ooroogoookodoo",
           "oooookooookooo", ".odooookookdo.", "..odd.okko.do.", "...d...kk..d..", "......kk......"]
    bx, by = -1, 3
    s.shape(big, bx, by, leaves)
    for y in range(by + len(big), horizon(6) + 2):
        s.put(bx + 6, y, trunk)
        s.put(bx + 7, y, trunk_d)
    small = ["..rrgr...", ".rrrrrrr.", "rrgrrdrrr", "rdrrkrrgr", ".rrkkkrd.", "..d.k.d.."]
    sx = mirror_x(2, len(small[0]))
    sy = horizon(sx + 4) - 9
    s.shape(small, sx, sy, {**leaves, "r": red, "g": orange})
    for y in range(sy + len(small), horizon(sx + 4) + 1):
        s.put(sx + 4, y, trunk)
    rng = random.Random(9)
    for i, (x, y) in enumerate(scatter(rng, 10, 1, COLS - 1, SHADOW + 1, ROWS - 1, gap=3, keep=off_bixo)):
        s.put(x, y, (orange, red, gold)[i % 3])
        s.put(x + 1, y, (orange, red, gold)[i % 3])
    # leaves falling from the trees, swaying as they go
    sway = [0, 1, 1, 0, 0, -1, -1, 0]
    for i, (x, y0) in enumerate(scatter(rng, 7, 0, COLS, 0, 16, gap=4)):
        y = 6 + (y0 + t) % 16
        x_now = x + sway[(t + i * 3) % 8]
        if y < SHADOW + 4:
            s.put(x_now, y, (orange, red, gold)[i % 3])
    return s


BACKGROUNDS: dict[str, Background] = {
    "noche": Background(bg_noche, "Noche"),
    "blanco": Background(bg_blanco, "Blanco", text=bixo.PALETTE["outline"], panel=(236, 234, 226)),
    "prado": Background(bg_prado, "Prado", frames=8, ms=260),
    "mazmorra": Background(bg_mazmorra, "Mazmorra", frames=6, ms=130),
    "synthwave": Background(bg_synthwave, "Synthwave", frames=SYNTH_RISE, ms=220),
    "navidad": Background(bg_navidad, "Navidad", frames=16, ms=180),
    "verano": Background(bg_verano, "Verano", frames=24, ms=200),
    "otono": Background(bg_otono, "Otoño", frames=16, ms=180),
}

# --- for the firmware -------------------------------------------------------------------------

# magic, version, cell, origin, cols, rows, frames, ms, shadow row, first and end column, RGB
HEADER = struct.Struct("<4sBBbBBxHHBBB3s")


def pack(bg: Background) -> bytes:
    """Header, then every frame without the shadow as rows x cols RGB888 cells, row by row."""
    x0, x1, color = bg.draw(0).shade or (0, 0, (0, 0, 0))
    out = bytearray(HEADER.pack(b"BXBG", 2, CELL, ORIGIN, COLS, ROWS, bg.frames, bg.ms,
                                SHADOW, x0, x1, bytes(color)))
    for t in range(bg.frames):
        px = bg.draw(t).cells(shadow=False)
        for y in range(ROWS):
            for x in range(COLS):
                out += bytes(px[(x, y)])
    return bytes(out)


# --- mockups: the round screen with Bixo on each background -----------------------------------

def bixo_on(background: Image.Image) -> Image.Image:
    sprite = bixo.draw(bixo.Pose(ground=False), CELL, transparent=True)
    screen = background.copy()
    screen.paste(sprite, (ORIGIN + SPRITE_COL * CELL, ORIGIN + SPRITE_ROW * CELL), sprite)
    return screen


def round_screen(img: Image.Image, bezel: int = 14) -> Image.Image:
    size = SCREEN + 2 * bezel
    out = Image.new("RGB", (size, size), WHITE)
    ImageDraw.Draw(out).ellipse((0, 0, size - 1, size - 1), fill=(30, 30, 34))
    mask = Image.new("L", (SCREEN, SCREEN), 0)
    ImageDraw.Draw(mask).ellipse((0, 0, SCREEN - 1, SCREEN - 1), fill=255)
    out.paste(img, (bezel, bezel), mask)
    return out


def mockup_sheet(frames: dict[str, list[Image.Image]], path: Path, cols: int = 4, scale: float = 0.6,
                 step_ms: int = 80) -> None:
    """An animated sheet where every background runs at its own pace."""
    loop_ms = max(len(f) * BACKGROUNDS[name].ms for name, f in frames.items())
    pad = 12
    images = []
    for now in range(0, loop_ms, step_ms):
        cells = [round_screen(bixo_on(f[(now // BACKGROUNDS[name].ms) % len(f)])) for name, f in frames.items()]
        w, h = cells[0].size
        rows = math.ceil(len(cells) / cols)
        sheet = Image.new("RGB", (cols * (w + pad) + pad, rows * (h + pad) + pad), WHITE)
        for i, cell in enumerate(cells):
            sheet.paste(cell, (pad + (i % cols) * (w + pad), pad + (i // cols) * (h + pad)))
        images.append(sheet.resize((round(sheet.width * scale), round(sheet.height * scale)), Image.NEAREST))
    images[0].save(path, save_all=True, append_images=images[1:], duration=step_ms, loop=0)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=str(Path(__file__).parent / "out" / "backgrounds"))
    ap.add_argument("--mockups", action="store_true", help="also write mockups.gif with Bixo on each")
    args = ap.parse_args()
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    frames = {name: [bg.draw(t).image() for t in range(bg.frames)] for name, bg in BACKGROUNDS.items()}
    for name, images in frames.items():
        images[0].save(out / f"{name}.png")
    if args.mockups:
        mockup_sheet(frames, out / "mockups.gif")
    print(f"{len(frames)} backgrounds in {out}")


if __name__ == "__main__":
    main()
