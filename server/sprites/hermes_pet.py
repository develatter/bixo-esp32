"""Bixo as a Hermes Agent pet: a petdex/Codex atlas (8x9 cells of 192x208) plus pet.json.

    uv run python sprites/hermes_pet.py [--install PROFILE_DIR]

Six frames per Hermes state, drawn with the same code as the Watcher GIFs.
"""

import argparse
import json
import shutil
from pathlib import Path

from PIL import Image

from bixo import CX, H, W, Pose, draw

CELL_W, CELL_H = 192, 208
COLS = 8
SCALE = 8  # 22x20 px -> 176x160, inside a 192x208 cell
# Row order of current petdex atlases (see agent/pet/constants.py in Hermes).
ROWS = ["idle", "running-right", "running-left", "waving", "jumping", "failed", "waiting", "running", "review"]


def p(**kw) -> Pose:
    return Pose(**kw)


def dots(n: int) -> list[tuple[str, int, int]]:
    return [("dot", CX + 4 + 2 * i, 5 - i) for i in range(n)]


def hop(look: tuple[int, int], face: dict) -> list[Pose]:
    return [p(shape="squash", look=look, **face), p(shape="stretch", dy=-1, look=look, **face),
            p(dy=-3, look=look, **face), p(shape="stretch", dy=-2, look=look, **face),
            p(shape="flat", look=look, **face), p(shape="squash", look=look, **face)]


STATES: dict[str, list[Pose]] = {
    "idle": [p(), p(), p(shape="squash"), p(), p(eyes="closed"), p()],
    # Hops that lean the way they go: the eyes look ahead and the body drifts a pixel.
    "running-right": [Pose(**{**f.__dict__, "dx": i // 3}) for i, f in enumerate(hop((1, 0), {"mouth": "small"}))],
    "running-left": [Pose(**{**f.__dict__, "dx": -(i // 3)}) for i, f in enumerate(hop((-1, 0), {"mouth": "small"}))],
    "waving": [p(eyes="happy", mouth="smile"), p(eyes="happy", mouth="smile", extras=[("spark", CX + 6, 6)]),
               p(eyes_r="closed", mouth="smile", extras=[("spark", CX + 6, 5)]),
               p(eyes="open", eyes_r="happy", mouth="smile", extras=[("spark", CX + 6, 5)]),
               p(shape="squash", eyes="happy", mouth="smile"), p(eyes="happy", mouth="smile")],
    "jumping": hop((0, 0), {"eyes": "happy", "mouth": "smile"}),
    "failed": [p(shape="squash", eyes="half", mouth="frown", brows="worried"),
               p(shape="flat", eyes="half", mouth="frown", brows="worried"),
               p(shape="flat", eyes="closed", mouth="frown", extras=[("tear", CX - 4, 13)]),
               p(shape="flat", eyes="closed", mouth="frown", extras=[("tear", CX - 4, 14)]),
               p(shape="squash", eyes="half", mouth="frown", brows="worried"),
               p(shape="squash", eyes="closed", mouth="frown")],
    # Waiting on the user: looks up at them with a question mark that bobs.
    "waiting": [p(mouth="small", extras=[("q", CX + 5, 2)]), p(mouth="small", extras=[("q", CX + 5, 1)]),
                p(look=(0, -1), mouth="small", extras=[("q", CX + 5, 1)]), p(eyes="closed", extras=[("q", CX + 5, 2)]),
                p(mouth="small", extras=[("q", CX + 5, 2)]), p(shape="squash", mouth="small", extras=[("q", CX + 5, 2)])],
    # Using a tool: busy little bounces with loading dots.
    "running": [p(shape="squash", look=(1, 0), mouth="smirk", extras=dots(1)), p(look=(1, 0), mouth="smirk", extras=dots(2)),
                p(shape="stretch", dy=-1, look=(1, 0), mouth="smirk", extras=dots(3)),
                p(shape="squash", look=(1, 0), mouth="smirk", extras=dots(1)), p(look=(1, 0), mouth="smirk", extras=dots(2)),
                p(shape="stretch", dy=-1, look=(1, 0), mouth="smirk", extras=dots(3))],
    "review": [p(look=(1, -1), mouth="smirk", extras=dots(n)) for n in (0, 1, 2, 3, 3)]
              + [p(look=(1, -1), mouth="smirk", eyes="closed", extras=dots(3))],
}


def build_atlas() -> Image.Image:
    atlas = Image.new("RGBA", (COLS * CELL_W, len(ROWS) * CELL_H), (0, 0, 0, 0))
    ox, oy = (CELL_W - W * SCALE) // 2, CELL_H - H * SCALE
    for r, state in enumerate(ROWS):
        frames = STATES[state]
        for c in range(COLS):  # the 6 animated frames, then repeats to fill the row
            atlas.paste(draw(frames[c % len(frames)], SCALE, transparent=True), (c * CELL_W + ox, r * CELL_H + oy))
    return atlas


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=str(Path(__file__).parent / "out" / "hermes-pet"))
    ap.add_argument("--install", help="Hermes profile dir, e.g. ~/.hermes/profiles/bixo")
    args = ap.parse_args()
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    build_atlas().save(out / "spritesheet.webp", lossless=True)
    (out / "pet.json").write_text(json.dumps({
        "id": "bixo",
        "displayName": "Bixo",
        "description": "Un slime azul de pixel art, blandito y un poco descarado.",
        "spriteVersionNumber": 2,
        "spritesheetPath": "spritesheet.webp",
    }, ensure_ascii=False, indent=2))
    print(f"atlas {COLS * CELL_W}x{len(ROWS) * CELL_H} in {out}")
    if args.install:
        dest = Path(args.install).expanduser() / "pets" / "bixo"
        dest.mkdir(parents=True, exist_ok=True)
        for name in ("spritesheet.webp", "pet.json"):
            shutil.copy(out / name, dest / name)
        print(f"installed in {dest}")


if __name__ == "__main__":
    main()
