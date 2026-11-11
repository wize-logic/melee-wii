#!/usr/bin/env python3
"""Draws the Homebrew Channel icon (wii/release/apps/melee/icon.png, 128x48)
in the launcher's colours, with DejaVu Sans Bold (free to use; its licence is
wii/src/launcher/DejaVu-LICENSE.txt).

    make_icon.py DejaVuSans-Bold.ttf OUT.png
"""
import sys

from PIL import Image, ImageDraw, ImageFont

W, H = 128, 48
TOP, BOTTOM = (0x0D, 0x14, 0x24), (0x1B, 0x24, 0x40)  # the launcher's BG_TOP, BG_BOTTOM
GOLD, WHITE = (0xF2, 0xC2, 0x30), (235, 240, 255)  # GOLD


def main() -> None:
    font_path, out = sys.argv[1], sys.argv[2]
    img = Image.new("RGBA", (W, H))
    draw = ImageDraw.Draw(img)
    for y in range(H):
        t = y / (H - 1)
        draw.line([(0, y), (W, y)], fill=tuple(round(a + (b - a) * t) for a, b in zip(TOP, BOTTOM)) + (255,))
    title = ImageFont.truetype(font_path, 26)
    small = ImageFont.truetype(font_path, 11)
    w = draw.textlength("MELEE", font=title)
    draw.text(((W - w) / 2 + 1, 4), "MELEE", font=title, fill=(0, 0, 0, 160))
    draw.text(((W - w) / 2, 3), "MELEE", font=title, fill=GOLD)
    w = draw.textlength("for the Wii", font=small)
    draw.text(((W - w) / 2, 32), "for the Wii", font=small, fill=WHITE)
    draw.rectangle([0, 0, W - 1, H - 1], outline=GOLD + (255,))
    img.save(out)


if __name__ == "__main__":
    main()
