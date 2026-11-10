"""Put Bixo's GIFs into a xiaozhi assets partition image.

    uv run python sprites/pack_assets.py BASE_ASSETS.bin [--gifs sprites/out] [--out sprites/assets.bin]

BASE_ASSETS is the assets image produced by the firmware build (build/generated_assets.bin):
its text font is kept, the stock emoji collection is replaced by the GIFs, a skin sets the
screen background to the sprites' own background colour, and the speech models are dropped:
Bixo has no wake word (the stock "Hi, ESP" one, and its startup hint, go with them).

The screen backgrounds (backgrounds.py) go in as `bg_<name>.bin`, listed in index.json under
"backgrounds" in menu order with their label and subtitle colours.
"""

import argparse
import json
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from backgrounds import BACKGROUNDS, pack  # noqa: E402
from bixo import PALETTE  # noqa: E402

NAME_LEN = 32
ENTRY = struct.Struct(f"<{NAME_LEN}sIIHH")  # name, size, offset, width, height
PARTITION_SIZE = 16 * 1024 * 1024


def checksum(data: bytes) -> int:
    return sum(data) & 0xFFFF


def read_assets(path: Path) -> dict[str, tuple[bytes, int, int]]:
    data = path.read_bytes()
    count, check, length = struct.unpack_from("<III", data)
    payload = data[12:12 + length]
    if checksum(payload) != check:
        raise ValueError(f"{path}: checksum mismatch")
    table = count * ENTRY.size
    files = {}
    for offset in range(0, table, ENTRY.size):
        name, size, start, width, height = ENTRY.unpack_from(payload, offset)
        start += table
        if payload[start:start + 2] != b"ZZ":
            raise ValueError(f"{path}: bad entry marker")
        files[name.split(b"\0", 1)[0].decode()] = (payload[start + 2:start + 2 + size], width, height)
    return files


def write_assets(files: dict[str, tuple[bytes, int, int]]) -> bytes:
    table, payload = bytearray(), bytearray()
    for name, (data, width, height) in files.items():
        table += ENTRY.pack(name.encode(), len(data), len(payload), width, height)
        payload += b"ZZ" + data
    blob = bytes(table + payload)
    return struct.pack("<III", len(files), checksum(blob), len(blob)) + blob


def hex_color(rgb: tuple[int, int, int]) -> str:
    return "#{:02X}{:02X}{:02X}".format(*rgb)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("base", type=Path)
    ap.add_argument("--gifs", type=Path, default=Path(__file__).parent / "out")
    ap.add_argument("--out", type=Path, default=Path(__file__).parent / "assets.bin")
    args = ap.parse_args()

    files = read_assets(args.base)
    index = json.loads(files["index.json"][0])
    for emoji in index.pop("emoji_collection", []):
        files.pop(emoji["file"], None)

    gifs = sorted(args.gifs.glob("*.gif"))
    if not gifs:
        raise SystemExit(f"no GIFs in {args.gifs}")
    index["emoji_collection"] = [{"name": gif.stem, "file": gif.name} for gif in gifs]
    for gif in gifs:
        files[gif.name] = (gif.read_bytes(), 0, 0)

    for old in index.pop("backgrounds", []):
        files.pop(old["file"], None)
    index["backgrounds"] = []
    for name, bg in BACKGROUNDS.items():
        file = f"bg_{name}.bin"
        files[file] = (pack(bg), 0, 0)
        index["backgrounds"].append({"name": name, "label": bg.label, "file": file,
                                     "text": hex_color(bg.text), "panel": hex_color(bg.panel)})

    skin = {"background_color": hex_color(PALETTE["bg"]), "text_color": hex_color(PALETTE["highlight"])}
    index["skin"] = {"light": skin, "dark": skin}
    if "srmodels" in index:
        files.pop(index.pop("srmodels"), None)
    index.pop("multinet_model", None)
    files["index.json"] = (json.dumps(index, ensure_ascii=False).encode(), 0, 0)

    blob = write_assets(files)
    if len(blob) > PARTITION_SIZE:
        raise SystemExit(f"assets image is {len(blob)} bytes, partition holds {PARTITION_SIZE}")
    args.out.write_bytes(blob)
    read_assets(args.out)  # round-trip check
    print(f"{args.out}: {len(blob) / 1e6:.1f} MB, {len(gifs)} emotions, {len(BACKGROUNDS)} backgrounds, "
          f"skin {skin}")


if __name__ == "__main__":
    main()
