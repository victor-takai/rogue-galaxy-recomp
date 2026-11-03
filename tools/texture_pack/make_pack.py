"""The larger 2D art put back together: make_pack.py [--no-pictures] [--pages] [UPSCALED] [PACK].

UPSCALED (default textures/upscaled) holds what an upscaler made of prepare.py's
pictures: for each NAME, its colour and its NAME.alpha, larger by the same whole
factor (2 to 8). Upscalers often add to a name ("NAME_upscayl_4x_model.png");
anything after NAME is ignored. Each pair becomes PACK/NAME.png (default
textures/hd), RGBA, the alpha back from grey, which the game reads at start
(src/texture_pack.h), cropped of prepare.py's padding to MINIMUM a side.
Without UPSCALED, every picture goes the xBR way below, the text too.

The rest of textures/collected, the pictures prepare.py leaves to their
pixels (icons, portraits, window pieces), goes into PACK enlarged 4x by xBR
(xbr.py), which keeps the art and smooths its staircases: the user found the
AI upscalers' icons and faces like watercolour. Not the help pages (512x448)
unless --pages: xbr.py in pure Python takes about 40 seconds for one.
--no-pictures leaves them all out.
"""
import os
import re
import sys

from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from xbr import xbr  # noqa: E402

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
MINIMUM = 32  # as prepare.py pads
NAME = re.compile(r"^([0-9a-f]{16}_[0-9a-f]{16}_(\d+)x(\d+))(\.alpha)?")


def main():
    arguments = [a for a in sys.argv[1:] if not a.startswith("--")]
    pictures = "--no-pictures" not in sys.argv[1:]
    pages = "--pages" in sys.argv[1:]
    source = arguments[0] if len(arguments) > 0 else os.path.join(ROOT, "textures", "upscaled")
    target = arguments[1] if len(arguments) > 1 else os.path.join(ROOT, "textures", "hd")
    os.makedirs(target, exist_ok=True)
    pairs = {}
    for file in os.listdir(source) if os.path.isdir(source) else []:
        match = NAME.match(file)
        if not match or not file.lower().endswith(".png"):
            continue
        pairs.setdefault(match.group(1), {})["alpha" if match.group(4) else "colour"] = file
    made = incomplete = wrong = 0
    for name, pair in sorted(pairs.items()):
        if len(pair) != 2:
            incomplete += 1
            continue
        width, height = (int(v) for v in NAME.match(name).group(2, 3))
        colour = Image.open(os.path.join(source, pair["colour"])).convert("RGB")
        alpha = Image.open(os.path.join(source, pair["alpha"])).convert("L")
        padded = (max(width, MINIMUM), max(height, MINIMUM))
        factor = colour.width // padded[0]
        if (factor < 2 or factor > 8 or colour.size != (padded[0] * factor, padded[1] * factor) or
                alpha.size != colour.size):
            print(f"{name}: {colour.size} and {alpha.size} are not a whole multiple of {padded[0]}x{padded[1]}")
            wrong += 1
            continue
        colour = colour.crop((0, 0, width * factor, height * factor))
        colour.putalpha(alpha.crop((0, 0, width * factor, height * factor)))
        colour.save(os.path.join(target, name + ".png"))
        made += 1
    print(f"{made} in {target}" + (f", {incomplete} without both halves" if incomplete else "") +
          (f", {wrong} of a wrong size" if wrong else ""))
    if not pictures:
        return
    collected = os.path.join(ROOT, "textures", "collected")
    enlarged = 0
    for name in sorted(os.listdir(collected)):
        match = NAME.match(name)
        if not match or name[:-4] in pairs or "_" + "0" * 16 + "_" in name:
            continue
        if not pages and (match.group(2), match.group(3)) == ("512", "448"):
            continue
        out = os.path.join(target, name)
        if not os.path.exists(out):
            xbr(Image.open(os.path.join(collected, name)), 2).save(out)
        enlarged += 1
    print(f"{enlarged} pictures by xBR in {target}")


if __name__ == "__main__":
    main()
