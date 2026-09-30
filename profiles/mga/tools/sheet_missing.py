#!/usr/bin/env python3
"""Contact sheet of textures the game drew that the pack has no replacement for.

    sheet_missing.py <data>/textures <out.png>

Reads <data>/textures/dump (everything drawn while MGA_DUMP_TEXTURES=1 was set)
and <data>/textures (the pack), and renders what is in the first and not the
second. A key on its own says nothing; the picture says whether it is scenery
that should have been packed or something the game composed at run time.
"""
import sys, os
from PIL import Image, ImageDraw

def main(argv):
    if len(argv) < 3:
        print(__doc__.strip()); return 2
    root, out = argv[1], argv[2]
    dump = os.path.join(root, "dump")
    have = {f[:-4] for f in os.listdir(root) if f.endswith(".png")}
    drawn = [f[:-4] for f in sorted(os.listdir(dump)) if f.endswith(".png")]
    missing = [k for k in drawn if k not in have]
    if not missing:
        print("nothing drawn is missing a replacement"); return 0
    tiles = []
    for k in missing:
        im = Image.open(os.path.join(dump, k + ".png")).convert("RGBA")
        z = max(1, min(4, 256 // max(im.width, im.height, 1)))
        tiles.append((k, im.size, im.resize((im.width * z, im.height * z), Image.NEAREST)))
    cols = 6
    cw = max(t[2].width for t in tiles) + 8
    rh = max(t[2].height for t in tiles) + 22
    rows = (len(tiles) + cols - 1) // cols
    sheet = Image.new("RGBA", (cols * cw, rows * rh), (32, 32, 32, 255))
    d = ImageDraw.Draw(sheet)
    for i, (k, size, im) in enumerate(tiles):
        x, y = (i % cols) * cw, (i // cols) * rh
        sheet.paste(im, (x + 4, y + 4))
        d.text((x + 4, y + im.height + 6), f"{k[:8]} {size[0]}x{size[1]}", fill=(230, 230, 230, 255))
    sheet.save(out)
    print(f"{len(missing)} of {len(drawn)} drawn textures have no replacement -> {out}")
    return 0

if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
