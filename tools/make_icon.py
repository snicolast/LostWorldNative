"""Builds res/lostworld.ico: an orange rounded badge with a white inner ring and "TLW"
in white condensed capitals (Anton, from third_party/fonts/). No official artwork.
Every size is drawn on its own (4x supersampled) so the small ones stay sharp.
Usage: python tools/make_icon.py   (needs Pillow)
"""
import os
from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DST = os.path.join(ROOT, "res", "lostworld.ico")
FONT = os.path.join(ROOT, "third_party", "fonts", "Anton-Regular.ttf")
ORANGE = (214, 104, 18, 255)
WHITE = (255, 255, 255, 255)
TEXT = "TLW"


def draw(size, ss=4):
    s = size * ss
    im = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    radius = size // 5
    d.rounded_rectangle([0, 0, s - 1, s - 1], radius * ss, fill=ORANGE)
    if size >= 24:  # white inner ring (too fine to read below that)
        m = max(1, size // 20) * ss
        w = max(1, int(size / 20 * 0.8 * ss))
        d.rounded_rectangle([m, m, s - 1 - m, s - 1 - m], radius * ss - m, outline=WHITE, width=w)

    font = ImageFont.truetype(FONT, int(s * (0.50 if size >= 24 else 0.66)))
    bb = d.textbbox((0, 0), TEXT, font=font)
    tw, th = bb[2] - bb[0], bb[3] - bb[1]
    text = Image.new("RGBA", (tw, th), (0, 0, 0, 0))
    ImageDraw.Draw(text).text((-bb[0], -bb[1]), TEXT, font=font, fill=WHITE)
    maxw = int(s * (0.70 if size >= 24 else 0.90))
    if tw > maxw:  # squeeze it sideways to fit the badge
        text = text.resize((maxw, th), Image.LANCZOS)
    im.alpha_composite(text, ((s - text.width) // 2, (s - th) // 2))
    return im.resize((size, size), Image.LANCZOS)


def main():
    sizes = [256, 64, 48, 32, 24, 16]
    images = [draw(n) for n in sizes]
    images[0].save(DST, format="ICO", sizes=[(n, n) for n in sizes], append_images=images[1:])
    print(f"{DST}: {', '.join(map(str, sizes))}")


if __name__ == "__main__":
    main()
