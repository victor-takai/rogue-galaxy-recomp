"""The collected 2D art made ready for an upscaler: prepare.py [--all] [COLLECTED] [OUT].

Only the text by default: the fonts' glyphs and the menus' labels drawn as
pictures. The user found icons, portraits and the like upscaled worse than
their pixels (a watercolour look, details lost, heavy outlines) and the
letters better. A glyph is one of a palette's run of pictures at most 24x26
pixels, all but 10% grey, at least 70% of them of one height of 12 or more
(a font sheet's row; a sheet of window pieces varies); a label is at most 26
high, at least 2.2 times as wide, at least 60% grey. The help pages, text and
pictures together, stay out. --all prepares everything.

COLLECTED (default textures/collected) holds what debug\\RogueGalaxy-collect.bat
wrote: RGBA PNGs named TEXELS_CLUT_WxH.png, alpha doubled (255 opaque). An image
upscaler, an AI one above all, handles colour and transparency badly together,
so each picture becomes two opaque ones in OUT (default textures/to-upscale):

  NAME.png        its colour, with the colour of the nearest visible pixels
                  spread into the transparent ones, so the edges keep their
                  colour when the two are put back together;
  NAME.alpha.png  its transparency as grey.

Both are padded to at least MINIMUM pixels a side, their last column and row
repeated: Real-ESRGAN's Vulkan build loses the GPU on a picture a few pixels
across (vkQueueSubmit failed -4), and every picture after it in the run comes
out wrong; make_pack.py crops the padding off. Upscale OUT into
textures/upscaled with any upscaler (4x for 8x play), then run make_pack.py.
Files already prepared are left alone. Pictures without a palette (clut
0000000000000000) are skipped: in Rogue Galaxy they are the frame itself.
"""
import collections
import colorsys
import os
import sys

from PIL import Image, ImageChops, ImageFilter

MINIMUM = 32
ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))


def bleed(rgba):
    """The colour of the visible pixels spread over the transparent ones."""
    rgb = rgba.convert("RGB")
    known = rgba.getchannel("A").point(lambda a: 255 if a > 0 else 0)
    if known.getextrema() == (0, 0):
        return rgb
    filled = Image.composite(rgb, Image.new("RGB", rgb.size), known)
    for _ in range(max(rgb.size)):
        if known.getextrema() == (255, 255):
            break
        grown = known.filter(ImageFilter.MaxFilter(3))
        new = ImageChops.subtract(grown, known)
        if new.getextrema() == (0, 0):
            break
        spread = filled.filter(ImageFilter.MaxFilter(3))
        filled = Image.composite(spread, filled, new)
        known = grown
    return filled


def pad(image):
    """At least MINIMUM a side, the last column and row repeated."""
    width, height = image.size
    padded = Image.new(image.mode, (max(width, MINIMUM), max(height, MINIMUM)))
    padded.paste(image, (0, 0))
    if padded.width > width:
        padded.paste(image.crop((width - 1, 0, width, height)).resize((padded.width - width, height)), (width, 0))
    if padded.height > height:
        row = padded.crop((0, height - 1, padded.width, height))
        padded.paste(row.resize((padded.width, padded.height - height)), (0, height))
    return padded


def grey_share(rgba):
    """The share of its visible pixels that are grey (or near black or white)."""
    visible = grey = 0
    for r, g, b, a in rgba.getdata():
        if a <= 64:
            continue
        visible += 1
        _, lightness, saturation = colorsys.rgb_to_hls(r / 255, g / 255, b / 255)
        if saturation < 0.25 or not 0.08 < lightness < 0.92:
            grey += 1
    return grey / visible if visible else 0.0


def text_names(source, names):
    """The pictures that are text: glyphs of a font, menu labels."""
    shape = {}
    for name in names:
        rgba = Image.open(os.path.join(source, name)).convert("RGBA")
        shape[name] = (rgba.width, rgba.height, grey_share(rgba))
    heights = collections.defaultdict(list)
    for name, (width, height, grey) in shape.items():
        if width <= 24 and height <= 26 and grey > 0.9:
            heights[name.split("_")[1]].append(height)
    fonts = set()
    for palette, values in heights.items():
        height, count = collections.Counter(values).most_common(1)[0]
        if len(values) >= 8 and height >= 12 and count >= 0.7 * len(values):
            fonts.add(palette)
    text = set()
    for name, (width, height, grey) in shape.items():
        label = height <= 26 and width >= 2.2 * height and grey > 0.6
        glyph = name.split("_")[1] in fonts and width <= 24 and height <= 26 and grey > 0.9
        if label or glyph:
            text.add(name)
    return text


def main():
    arguments = [a for a in sys.argv[1:] if a != "--all"]
    everything = "--all" in sys.argv[1:]
    source = arguments[0] if len(arguments) > 0 else os.path.join(ROOT, "textures", "collected")
    target = arguments[1] if len(arguments) > 1 else os.path.join(ROOT, "textures", "to-upscale")
    os.makedirs(target, exist_ok=True)
    names = []
    for name in sorted(os.listdir(source)):
        parts = name[:-4].split("_") if name.endswith(".png") else []
        if len(parts) == 3 and parts[1] != "0" * 16:
            names.append(name)
    if not everything:
        text = text_names(source, names)
        print(f"{len(text)} of {len(names)} pictures are text")
        names = [name for name in names if name in text]
    made = skipped = 0
    for name in names:
        colour = os.path.join(target, name)
        alpha = os.path.join(target, name[:-4] + ".alpha.png")
        if os.path.exists(colour) and os.path.exists(alpha):
            skipped += 1
            continue
        rgba = Image.open(os.path.join(source, name)).convert("RGBA")
        pad(bleed(rgba)).save(colour)
        pad(rgba.getchannel("A").convert("RGB")).save(alpha)
        made += 1
    print(f"{made} prepared, {skipped} already there, in {target}")


if __name__ == "__main__":
    main()
