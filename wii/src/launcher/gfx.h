/* The launcher's drawing (gfx.c): a canvas of 0xRRGGBB pixels, gfx_width()
 * by 480, shown by gfx_present */
#ifndef LAUNCHER_GFX_H
#define LAUNCHER_GFX_H

#include "font.h"

void gfx_init(void);
void gfx_shutdown(void);
int gfx_width(void);
int gfx_height(void);

void gfx_clear(unsigned int rgb);
void gfx_rect(int x, int y, int w, int h, unsigned int rgb, int alpha);
void gfx_round_rect(int x, int y, int w, int h, int r, unsigned int rgb, int alpha);
void gfx_gradient_v(int x, int y, int w, int h, unsigned int top, unsigned int bottom);

/* y is the top of the line; returns the width drawn */
int gfx_text(const Font* f, int x, int y, unsigned int rgb, const char* s);
int gfx_text_centered(const Font* f, int cx, int y, unsigned int rgb, const char* s);
int gfx_text_width(const Font* f, const char* s);
int gfx_line_height(const Font* f);

void gfx_present(void);

#endif
