#!/usr/bin/env python3
"""Turn a texture dump into a replacement texture pack.

    python3 upscale_textures.py <data>/textures/dump  <data>/textures

The port writes a dump when MGA_DUMP_TEXTURES=1 is set: one PNG per texture,
named after the 64-bit key the renderer looks textures up by. This reads that
folder, enlarges what is worth enlarging, and writes the results back beside it
under the same names, which is all the pack loader needs to start using them.

Why not simply enlarge everything
---------------------------------
A PSP game's textures are two different kinds of picture that want opposite
treatment, and running one algorithm over both makes half of them worse.

The interface -- cards, text, icons, HUD panels -- is pixel art. It was drawn
texel by texel to be shown at one texel per pixel, and its hard edges are the
art rather than an artefact of its size. A neural upscaler invents plausible
detail, which on a hand-drawn edge means inventing detail that was never there:
letters grow soft haloes and flat panels acquire texture. The port already draws
these sharp by snapping their sampling to the texel grid, so the honest thing is
to leave them alone.

Character skins, environment surfaces and effect sprites are the other kind.
They are photographic in nature, they were downsampled hard to fit a UMD, and
detail really is missing from them. That is what an upscaler is for.

So each texture is classified and only the second kind is enlarged. The test is
deliberately conservative: anything ambiguous is left as it is, because a
texture left alone still looks exactly as it does today, while a texture wrongly
"improved" is a visible regression the player cannot turn off per texture.

Alpha
-----
Real-ESRGAN has no opinion about alpha -- it takes three channels. Transparency
is common in these textures (every card, every sprite), so the alpha channel is
enlarged separately with Lanczos and put back. Enlarging RGB while leaving alpha
at its old size, or letting the model hallucinate into it, is what produces the
fringing that makes a pack look worse than no pack.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

import numpy as np
from PIL import Image

# Beyond this the pack costs more video memory than the detail is worth: the
# renderer caches by texture count, not bytes, so a folder of very large
# replacements can fill a device.
MAX_EDGE = 2048
# Below this there is nothing for a model to work with, and the result is
# invention rather than restoration.
MIN_EDGE = 16


def is_pixel_art(image: Image.Image) -> tuple[bool, str]:
    """Whether this looks hand-drawn rather than photographic.

    Three signs, any of which is enough. None of them is certain on its own,
    which is why the answer is only ever used to *skip* a texture.
    """
    rgba = np.asarray(image.convert("RGBA"))
    height, width = rgba.shape[:2]
    rgb = rgba[..., :3].reshape(-1, 3)

    # A small palette. Interface art is usually drawn from a handful of colours;
    # a photograph of anything has hundreds even at 64x64.
    distinct = len(np.unique(rgb, axis=0))
    if distinct <= max(32, (width * height) // 64):
        return True, f"{distinct} colours"

    # A cut-out alpha channel was a third test here and has been removed: it
    # marked every sprite and billboard in the game as interface art. Hard alpha
    # says the texture has a masked shape, which is as true of a rendered
    # character standing against nothing as it is of a drawn icon. A signal that
    # fires on both kinds cannot tell them apart, and leaving it in cost more
    # textures than it saved.

    # Long runs of identical texels along a row: flat fills and straight rules,
    # which photographs do not have.
    row_same = np.count_nonzero(rgb.reshape(height, width, 3)[:, 1:] ==
                                rgb.reshape(height, width, 3)[:, :-1])
    if row_same / (rgb.reshape(height, width, 3)[:, 1:].size or 1) > 0.80:
        return True, "flat fills"

    return False, ""


class Upscaler:
    """Real-ESRGAN x4, loaded once."""

    def __init__(self, weights: Path):
        import torch  # imported here so --list works without it

        from rrdbnet import RRDBNet

        self.torch = torch
        state = torch.load(weights, map_location="cpu")
        state = state.get("params_ema", state.get("params", state))
        self.net = RRDBNet()
        # strict: a mismatch means these are not the weights this architecture
        # was written for, and a partly-loaded model produces confident noise.
        self.net.load_state_dict(state, strict=True)
        self.net.eval()

    def __call__(self, image: Image.Image) -> Image.Image:
        torch = self.torch
        rgba = image.convert("RGBA")
        rgb = np.asarray(rgba.convert("RGB"), dtype=np.float32) / 255.0
        tensor = torch.from_numpy(rgb).permute(2, 0, 1).unsqueeze(0)
        with torch.no_grad():
            out = self.net(tensor)
        out = out.squeeze(0).permute(1, 2, 0).clamp(0.0, 1.0).numpy()
        result = Image.fromarray((out * 255.0 + 0.5).astype(np.uint8), "RGB")

        alpha = rgba.getchannel("A")
        if alpha.getextrema() != (255, 255):
            # Lanczos, not the model: alpha is a shape, not a picture, and the
            # model has never seen one.
            result.putalpha(alpha.resize(result.size, Image.LANCZOS))
        return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("dump", type=Path, help="the folder the game wrote its dump into")
    parser.add_argument("out", type=Path, help="the textures folder the pack is read from")
    parser.add_argument("--weights", type=Path, default=Path("RealESRGAN_x4plus.pth"))
    parser.add_argument("--list", action="store_true",
                        help="classify everything and report, writing nothing")
    parser.add_argument("--all", action="store_true",
                        help="enlarge the interface art too, against the advice above")
    parser.add_argument("--limit", type=int, default=0, help="stop after this many (for a trial run)")
    args = parser.parse_args()

    sources = sorted(p for p in args.dump.glob("*.png"))
    if not sources:
        print(f"no textures in {args.dump}", file=sys.stderr)
        print("run the game once with MGA_DUMP_TEXTURES=1 first", file=sys.stderr)
        return 1

    upscaler = None
    kept = skipped = failed = 0
    for path in sources:
        if args.limit and kept + skipped >= args.limit:
            break
        try:
            image = Image.open(path)
            image.load()
        except Exception as error:  # a dump can contain a file the encoder half wrote
            print(f"  skip {path.name}: unreadable ({error})")
            failed += 1
            continue

        width, height = image.size
        reason = ""
        if max(width, height) * 4 > MAX_EDGE:
            reason = f"already {width}x{height}"
        elif min(width, height) < MIN_EDGE:
            reason = f"only {width}x{height}"
        elif not args.all:
            pixel_art, why = is_pixel_art(image)
            if pixel_art:
                reason = f"interface art ({why})"

        if reason:
            print(f"  skip {path.name}  {width}x{height}  {reason}")
            skipped += 1
            continue

        if args.list:
            print(f"  take {path.name}  {width}x{height} -> {width * 4}x{height * 4}")
            kept += 1
            continue

        if upscaler is None:
            upscaler = Upscaler(args.weights)
        try:
            result = upscaler(image)
        except Exception as error:
            print(f"  fail {path.name}: {error}")
            failed += 1
            continue
        args.out.mkdir(parents=True, exist_ok=True)
        result.save(args.out / path.name)
        print(f"  done {path.name}  {width}x{height} -> {result.width}x{result.height}")
        kept += 1

    what = "would enlarge" if args.list else "enlarged"
    print(f"\n{what} {kept}, left alone {skipped}, could not read {failed}")
    if not args.list and kept:
        print(f"pack written to {args.out}")
        print("turn on \"Replacement textures\" in the video menu")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
