/* The launcher's font (font_data.c, from wii/tools/make_font.py) */
#ifndef LAUNCHER_FONT_H
#define LAUNCHER_FONT_H

typedef struct FontGlyph {
    unsigned short code;
    signed char left, top; /* the bitmap's offset from the pen, the top from the line's top */
    unsigned char w, h;
    unsigned char advance;
    unsigned int offset; /* into the coverage data */
} FontGlyph;

typedef struct Font {
    int ascent, descent;
    int count;
    const FontGlyph* glyphs;
    const unsigned char* data;
} Font;

extern const Font font_small, font_body, font_body_bold, font_title;

#endif
