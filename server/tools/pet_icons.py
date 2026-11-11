"""Generate the circular menu's pixel art as LVGL alpha-only (A8) images, coloured at runtime.

    uv run python tools/pet_icons.py [--out ../main/boards/sensecap-watcher] [--preview p.png]

Writes pet_icons.c and pet_icons.h: one icon per menu item (11 x 11 pixel art, 3 px blocks), the
disc behind each icon and the twenty segments of the level ring along the edge of the 412 px
screen, with where each segment goes. Every image is opaque (255) where it draws and clear
elsewhere; LVGL paints A8 images in their image_recolor colour.
"""

import argparse
import math
from pathlib import Path

from PIL import Image

SCREEN = 412
BLOCK = 3            # px per art pixel, as on the rest of Bixo's screen
DISC_BLOCKS = 10     # disc radius in blocks (30 px)
RING_INNER, RING_OUTER = 186, 200
RING_SEGMENTS = 20
RING_GAP_DEG = 3

ICONS = {
    "salir": ["xx.......xx", "xxx.....xxx", ".xxx...xxx.", "..xxx.xxx..", "...xxxxx...", "....xxx....",
              "...xxxxx...", "..xxx.xxx..", ".xxx...xxx.", "xxx.....xxx", "xx.......xx"],
    "voz_on": ["....x......", "...xx....x.", "..xxx..x..x", "xxxxx...x.x", "xxxxx.x.x.x", "xxxxx.x.x.x",
               "xxxxx...x.x", "..xxx..x..x", "...xx....x.", "....x......"],
    "voz_off": ["....x......", "...xx......", "..xxx......", "xxxxx.x...x", "xxxxx..x.x.", "xxxxx...x..",
                "xxxxx..x.x.", "..xxx.x...x", "...xx......", "....x......"],
    "brillo": [".....x.....", ".x...x...x.", "..x.....x..", "....xxx....", "...xxxxx...", "xx.xxxxx.xx",
               "...xxxxx...", "....xxx....", "..x.....x..", ".x...x...x.", ".....x....."],
    "fondo": ["xxxxxxxxxxx", "x.........x", "x.......x.x", "x......xxxx", "x...x...x.x", "x..xxx....x",
              "x.xxxxx.x.x", "xxxxxxxxxxx", "x.........x", "xxxxxxxxxxx"],
    "charla": [".xxxxxxxxx.", "x.........x", "x....x....x", "x....x....x", "x..xxxxx..x", "x....x....x",
               "x....x....x", "x.........x", ".xxxxxxxxx.", "..xx.......", "..x........"],
    "reconectar": ["..xxxxxxx..", ".x.......x.", "x..xxxxx..x", "..x.....x..", "....xxx....", ".....x.....",
                   "....xxxx...", "...x....x.x", "...x.....xx", "....x...xxx", ".....xxx..."],
    "cambiar_wifi": ["..xxxxxxx..", ".x.......x.", "x..xxxxx..x", "..x.....x..", "....xxx....", ".....x.....",
                     "...........", ".x.......x.", "xxxxxxxxxxx", ".x.......x."],
    "info": ["...xxxxx...", ".xx.....xx.", ".x...x...x.", "x.........x", "x...xx....x", "x....x....x",
             "x....x....x", ".x..xxx..x.", ".xx.....xx.", "...xxxxx..."],
    "restablecer": [".....x.....", "....x.x....", "....x.x....", "...x.x.x...", "...x.x.x...", "..x..x..x..",
                    "..x.....x..", ".x...x...x.", ".x.......x.", "xxxxxxxxxxx"],
    "reiniciar": ["...xxxx.x..", "..x....xx..", ".x....xxx..", "x..........", "x.........x", "x.........x",
                  "x.........x", ".x.......x.", "..x.....x..", "...xxxxx..."],
    "apagar": [".....x.....", "..x..x..x..", ".x...x...x.", "x....x....x", "x....x....x", "x.........x",
               "x.........x", ".x.......x.", "..x.....x..", "...xxxxx..."],
}
# Art pixels to draw an icon higher than its centre: a triangle's weight sits low.
OPTICAL_LIFT = {"restablecer": 1}


def icon_image(rows: list[str], lift: int) -> Image.Image:
    """The drawn pixels only (so placing it by its centre centres the drawing), lift padded below."""
    cells = [(x, y) for y, row in enumerate(rows) for x, c in enumerate(row) if c == "x"]
    x0, y0 = min(x for x, _ in cells), min(y for _, y in cells)
    w = max(x for x, _ in cells) - x0 + 1
    h = max(y for _, y in cells) - y0 + 1
    img = Image.new("L", (w * BLOCK, (h + 2 * lift) * BLOCK), 0)
    for x, y in cells:
        img.paste(255, ((x - x0) * BLOCK, (y - y0) * BLOCK, (x - x0 + 1) * BLOCK, (y - y0 + 1) * BLOCK))
    return img


def disc_image() -> Image.Image:
    """A filled circle of blocks, centred on the image's middle pixel."""
    n = DISC_BLOCKS
    size = (2 * n + 1) * BLOCK
    img = Image.new("L", (size, size), 0)
    for gy in range(-n, n + 1):
        for gx in range(-n, n + 1):
            if math.hypot(gx, gy) < n + 0.2:
                x, y = (gx + n) * BLOCK, (gy + n) * BLOCK
                img.paste(255, (x, y, x + BLOCK, y + BLOCK))
    return img


def ring_segments() -> list[tuple[int, int, Image.Image]]:
    """(x, y, image) per segment, clockwise from the top, on the screen's block grid."""
    span = 360 / RING_SEGMENTS
    blocks: list[list[tuple[int, int]]] = [[] for _ in range(RING_SEGMENTS)]
    for gy in range(SCREEN // BLOCK + 1):
        for gx in range(SCREEN // BLOCK + 1):
            dx, dy = gx * BLOCK + 1 - SCREEN / 2, gy * BLOCK + 1 - SCREEN / 2
            if not RING_INNER <= math.hypot(dx, dy) <= RING_OUTER:
                continue
            k = ((math.degrees(math.atan2(dy, dx)) + 90) % 360) / span
            if k - math.floor(k) >= RING_GAP_DEG / span:
                blocks[math.floor(k)].append((gx, gy))
    segments = []
    for cells in blocks:
        gx0, gy0 = min(g for g, _ in cells), min(g for _, g in cells)
        w = max(g for g, _ in cells) - gx0 + 1
        h = max(g for _, g in cells) - gy0 + 1
        img = Image.new("L", (w * BLOCK, h * BLOCK), 0)
        for gx, gy in cells:
            x, y = (gx - gx0) * BLOCK, (gy - gy0) * BLOCK
            img.paste(255, (x, y, x + BLOCK, y + BLOCK))
        segments.append((gx0 * BLOCK, gy0 * BLOCK, img))
    return segments


def c_image(name: str, img: Image.Image) -> list[str]:
    data = img.tobytes()
    out = [f"static LV_ATTRIBUTE_LARGE_CONST const uint8_t {name}_map[] = {{"]
    for i in range(0, len(data), 24):
        out.append("    " + ", ".join(f"0x{v:02x}" for v in data[i:i + 24]) + ",")
    out += ["};", f"const lv_image_dsc_t {name} = {{",
            f"    .header = {{.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_A8, .w = {img.width}, "
            f".h = {img.height}, .stride = {img.width}}},",
            f"    .data_size = sizeof({name}_map),", f"    .data = {name}_map,", "};", ""]
    return out


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", type=Path, default=Path(__file__).parents[2] / "main/boards/sensecap-watcher")
    ap.add_argument("--preview", type=Path, help="every image on one sheet, to eyeball them")
    args = ap.parse_args()

    images = {f"pet_icon_{key}": icon_image(rows, OPTICAL_LIFT.get(key, 0)) for key, rows in ICONS.items()}
    images["pet_disc"] = disc_image()
    segments = ring_segments()

    source = ["/* The circular menu's pixel art (A8). Generated by server/tools/pet_icons.py. */",
              '#include "pet_icons.h"', ""]
    for name, img in images.items():
        source += c_image(name, img)
    for i, (_, _, img) in enumerate(segments):
        source += c_image(f"pet_ring_segment_{i}", img)
    source.append("const lv_image_dsc_t* const pet_ring_segments[PET_RING_SEGMENTS] = {")
    source += [f"    &pet_ring_segment_{i}," for i in range(len(segments))]
    source += ["};", "const lv_point_t pet_ring_segment_pos[PET_RING_SEGMENTS] = {"]
    source += [f"    {{{x}, {y}}}," for x, y, _ in segments]
    source += ["};", ""]

    header = ["/* The circular menu's pixel art (A8). Generated by server/tools/pet_icons.py. */",
              "#ifndef PET_ICONS_H", "#define PET_ICONS_H", "", '#include "lvgl.h"', "",
              "#ifdef __cplusplus", 'extern "C" {', "#endif", ""]
    header += [f"extern const lv_image_dsc_t {name};" for name in images]
    header += ["", "// The level ring along the screen edge, clockwise from the top: each segment and the",
               "// screen position of its top-left corner.",
               f"#define PET_RING_SEGMENTS {len(segments)}",
               "extern const lv_image_dsc_t* const pet_ring_segments[PET_RING_SEGMENTS];",
               "extern const lv_point_t pet_ring_segment_pos[PET_RING_SEGMENTS];", "",
               "#ifdef __cplusplus", "}", "#endif", "", "#endif  // PET_ICONS_H", ""]

    (args.out / "pet_icons.c").write_text("\n".join(source))
    (args.out / "pet_icons.h").write_text("\n".join(header))
    print(f"{args.out}/pet_icons.c: {len(images)} images, {len(segments)} ring segments")

    if args.preview:
        sheet = Image.new("L", (SCREEN, SCREEN), 40)
        for x, y, img in segments:
            sheet.paste(img, (x, y), img)
        for i, img in enumerate(images.values()):
            sheet.paste(img, (60 + (i % 5) * 64, 90 + (i // 5) * 70), img)
        sheet.save(args.preview)


if __name__ == "__main__":
    main()
