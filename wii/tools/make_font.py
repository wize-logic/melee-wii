#!/usr/bin/env python3
"""Renders the launcher's font (wii/src/launcher/font_data.c): DejaVu Sans
(regular and bold), at the sizes the launcher draws, as 8-bit coverage
bitmaps of the printable ASCII characters and a few more.

    make_font.py REGULAR.ttf BOLD.ttf OUT.c

The output is checked in: the build needs neither the fonts nor Pillow.
DejaVu's fonts are free to use and redistribute (their licence, from
Bitstream Vera's, is in wii/src/launcher/DejaVu-LICENSE.txt).
"""

import sys

from PIL import Image, ImageDraw, ImageFont

# (name, file index, pixel size)
SIZES = [("small", 0, 15), ("body", 0, 18), ("body_bold", 1, 18), ("title", 1, 28)]
EXTRA = "•—·←→↑↓…×✓"  # bullet, dash, dot, arrows, ellipsis, times, check
CHARS = [chr(c) for c in range(32, 127)] + list(EXTRA)


def main() -> None:
    regular, bold, out = sys.argv[1], sys.argv[2], sys.argv[3]
    files = [regular, bold]
    lines = ["/* Written by wii/tools/make_font.py: DejaVu Sans for the launcher */",
             "",
             '#include "font.h"',
             ""]
    fonts = []
    for name, which, px in SIZES:
        font = ImageFont.truetype(files[which], px)
        ascent, descent = font.getmetrics()
        glyphs = []
        data = bytearray()
        for ch in CHARS:
            left, top, right, bottom = font.getbbox(ch)
            w, h = max(0, right - left), max(0, bottom - top)
            advance = round(font.getlength(ch))
            offset = len(data)
            if w and h:
                img = Image.new("L", (w, h), 0)
                ImageDraw.Draw(img).text((-left, -top), ch, font=font, fill=255)
                data += img.tobytes()
            glyphs.append((ord(ch), left, top, w, h, advance, offset))
        lines.append(f"static const unsigned char {name}_data[{len(data)}] = {{")
        for i in range(0, len(data), 24):
            lines.append("    " + ", ".join(str(b) for b in data[i:i + 24]) + ",")
        lines.append("};")
        lines.append(f"static const FontGlyph {name}_glyphs[{len(glyphs)}] = {{")
        for code, left, top, w, h, adv, off in glyphs:
            lines.append(f"    {{ 0x{code:04X}, {left}, {top}, {w}, {h}, {adv}, {off} }},")
        lines.append("};")
        fonts.append((name, ascent, descent, len(glyphs)))
    for name, ascent, descent, count in fonts:
        lines.append(f"const Font font_{name} = {{ {ascent}, {descent}, {count}, {name}_glyphs, {name}_data }};")
    with open(out, "w") as f:
        f.write("\n".join(lines) + "\n")


if __name__ == "__main__":
    main()
