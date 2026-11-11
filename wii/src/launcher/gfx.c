/* The launcher's drawing: a canvas in MEM2 (RGB, 640 pixels wide, or 854
 * when Wii Settings say 16:9, so shapes keep their proportions on a wide
 * TV), drawn to by shapes and text, and turned into the video interface's
 * YUV frame buffer each frame. The frame buffers are at the top of MEM1,
 * which the game's arena takes over (and clears) once the launcher is done. */

#include "gfx.h"

#include <gccore.h>
#include <malloc.h>
#include <string.h>

#define HEIGHT 480

static GXRModeObj* s_mode;
static u32* s_xfb[2];
static int s_which;
static u32* s_canvas;
static int s_width = 640;

/* wii/src/os.c: the top of the memory the game's arena will have */
void* melee_wii_arena_scratch(unsigned int size);

void gfx_init(void)
{
    u32 size;
    static GXRModeObj mode;
    VIDEO_Init();
    mode = *VIDEO_GetPreferredMode(NULL);
    if (CONF_GetAspectRatio() == CONF_ASPECT_16_9) {
        /* the picture over 704 pixels of the line, as Wii programs show
         * 16:9 (the game's video does the same, wii/src/vi.c) */
        mode.viWidth = 704;
        mode.viXOrigin = (VI_MAX_WIDTH_NTSC - 704) / 2;
    }
    s_mode = &mode;
    VIDEO_Configure(s_mode);
    size = VIDEO_GetFrameBufferSize(s_mode);
    s_xfb[0] = melee_wii_arena_scratch(size * 2);
    s_xfb[1] = (u32*) ((u8*) s_xfb[0] + ((size + 31) & ~31u));
    s_width = CONF_GetAspectRatio() == CONF_ASPECT_16_9 ? 854 : 640;
    s_canvas = memalign(32, s_width * HEIGHT * 4);
    memset(s_canvas, 0, s_width * HEIGHT * 4);
    VIDEO_ClearFrameBuffer(s_mode, s_xfb[0], COLOR_BLACK);
    VIDEO_ClearFrameBuffer(s_mode, s_xfb[1], COLOR_BLACK);
    VIDEO_SetNextFramebuffer(s_xfb[0]);
    VIDEO_SetBlack(false);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    if (s_mode->viTVMode & VI_NON_INTERLACE) {
        VIDEO_WaitVSync();
    }
}

void gfx_shutdown(void)
{
    VIDEO_SetBlack(true);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    free(s_canvas);
    s_canvas = NULL;
}

int gfx_width(void)
{
    return s_width;
}

int gfx_height(void)
{
    return HEIGHT;
}

static inline u32 blend(u32 dst, u32 src, int alpha)
{
    if (alpha >= 255) {
        return src;
    }
    u32 rb = (((src & 0xFF00FF) * alpha) + ((dst & 0xFF00FF) * (255 - alpha))) >> 8;
    u32 g = (((src & 0x00FF00) * alpha) + ((dst & 0x00FF00) * (255 - alpha))) >> 8;
    return (rb & 0xFF00FF) | (g & 0x00FF00);
}

void gfx_clear(u32 rgb)
{
    for (int i = 0; i < s_width * HEIGHT; i++) {
        s_canvas[i] = rgb;
    }
}

void gfx_rect(int x, int y, int w, int h, u32 rgb, int alpha)
{
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w > s_width ? s_width : x + w, y1 = y + h > HEIGHT ? HEIGHT : y + h;
    for (int yy = y0; yy < y1; yy++) {
        u32* row = s_canvas + yy * s_width;
        for (int xx = x0; xx < x1; xx++) {
            row[xx] = blend(row[xx], rgb, alpha);
        }
    }
}

/* A rectangle with rounded corners (radius r), its edges anti-aliased */
void gfx_round_rect(int x, int y, int w, int h, int r, u32 rgb, int alpha)
{
    if (r * 2 > h) {
        r = h / 2;
    }
    if (r * 2 > w) {
        r = w / 2;
    }
    for (int yy = 0; yy < h; yy++) {
        int py = y + yy;
        if (py < 0 || py >= HEIGHT) {
            continue;
        }
        u32* row = s_canvas + py * s_width;
        for (int xx = 0; xx < w; xx++) {
            int px = x + xx;
            int cover = 255;
            if (px < 0 || px >= s_width) {
                continue;
            }
            /* in a corner: coverage from the distance to its centre */
            int cx = xx < r ? r - 1 - xx : xx >= w - r ? xx - (w - r) : -1;
            int cy = yy < r ? r - 1 - yy : yy >= h - r ? yy - (h - r) : -1;
            if (cx >= 0 && cy >= 0) {
                /* distance (in 1/16 pixel) of the pixel's centre from the corner's */
                int dx = cx * 16 + 8, dy = cy * 16 + 8;
                int d2 = dx * dx + dy * dy, rr = r * 16;
                if (d2 >= (rr + 8) * (rr + 8)) {
                    continue;
                }
                if (d2 > (rr - 8) * (rr - 8)) {
                    /* the edge: a linear ramp over one pixel */
                    int d = 0;
                    while ((d + 1) * (d + 1) <= d2) {
                        d++;
                    }
                    cover = (rr + 8 - d) * 255 / 16;
                    if (cover < 0) {
                        cover = 0;
                    } else if (cover > 255) {
                        cover = 255;
                    }
                }
            }
            row[px] = blend(row[px], rgb, cover * alpha / 255);
        }
    }
}

void gfx_gradient_v(int x, int y, int w, int h, u32 top, u32 bottom)
{
    for (int yy = 0; yy < h; yy++) {
        int a = h > 1 ? yy * 255 / (h - 1) : 0;
        gfx_rect(x, y + yy, w, 1, blend(top, bottom, a), 255);
    }
}

/* UTF-8 */
static const char* next_code(const char* s, unsigned* code)
{
    unsigned char c = (unsigned char) *s;
    if (c < 0x80) {
        *code = c;
        return s + 1;
    }
    if ((c & 0xE0) == 0xC0 && s[1]) {
        *code = ((c & 0x1F) << 6) | (s[1] & 0x3F);
        return s + 2;
    }
    if ((c & 0xF0) == 0xE0 && s[1] && s[2]) {
        *code = ((c & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
        return s + 3;
    }
    *code = '?';
    return s + 1;
}

static const FontGlyph* glyph(const Font* f, unsigned code)
{
    for (int i = 0; i < f->count; i++) {
        if (f->glyphs[i].code == code) {
            return &f->glyphs[i];
        }
    }
    return code != '?' ? glyph(f, '?') : NULL;
}

int gfx_text_width(const Font* f, const char* s)
{
    int w = 0;
    while (*s) {
        unsigned code;
        const FontGlyph* g;
        s = next_code(s, &code);
        g = glyph(f, code);
        if (g != NULL) {
            w += g->advance;
        }
    }
    return w;
}

int gfx_line_height(const Font* f)
{
    return f->ascent + f->descent;
}

int gfx_text(const Font* f, int x, int y, u32 rgb, const char* s)
{
    int pen = x;
    while (*s) {
        unsigned code;
        const FontGlyph* g;
        s = next_code(s, &code);
        g = glyph(f, code);
        if (g == NULL) {
            continue;
        }
        const unsigned char* bits = f->data + g->offset;
        for (int gy = 0; gy < g->h; gy++) {
            int py = y + g->top + gy;
            if (py < 0 || py >= HEIGHT) {
                continue;
            }
            u32* row = s_canvas + py * s_width;
            for (int gx = 0; gx < g->w; gx++) {
                int px = pen + g->left + gx;
                int a = bits[gy * g->w + gx];
                if (a != 0 && px >= 0 && px < s_width) {
                    row[px] = blend(row[px], rgb, a);
                }
            }
        }
        pen += g->advance;
    }
    return pen - x;
}

int gfx_text_centered(const Font* f, int cx, int y, u32 rgb, const char* s)
{
    int w = gfx_text_width(f, s);
    gfx_text(f, cx - w / 2, y, rgb, s);
    return w;
}

/* The canvas to the next frame buffer (YUV 4:2:2, two pixels a word),
 * squeezed to 640 pixels when it is wider, then shown at the next retrace */
static inline void yuv(u32 c, int* y, int* u, int* v)
{
    int r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
    *y = (16829 * r + 33039 * g + 6416 * b + 32768) / 65536 + 16;
    *u = (-9714 * r - 19070 * g + 28784 * b + 32768) / 65536 + 128;
    *v = (28784 * r - 24103 * g - 4681 * b + 32768) / 65536 + 128;
}

void gfx_present(void)
{
    u32* xfb;
    int lines = s_mode->xfbHeight < HEIGHT ? s_mode->xfbHeight : HEIGHT;
    s_which ^= 1;
    xfb = s_xfb[s_which];
    for (int yy = 0; yy < lines; yy++) {
        const u32* row = s_canvas + yy * s_width;
        u32* out = xfb + yy * (s_mode->fbWidth / 2);
        for (int xx = 0; xx < s_mode->fbWidth; xx += 2) {
            int y1, u1, v1, y2, u2, v2;
            u32 c1 = row[xx * s_width / s_mode->fbWidth];
            u32 c2 = row[(xx + 1) * s_width / s_mode->fbWidth];
            yuv(c1, &y1, &u1, &v1);
            yuv(c2, &y2, &u2, &v2);
            out[xx / 2] = ((u32) y1 << 24) | ((u32) ((u1 + u2) / 2) << 16) | ((u32) y2 << 8) |
                          (u32) ((v1 + v2) / 2);
        }
    }
    DCFlushRange(xfb, VIDEO_GetFrameBufferSize(s_mode));
    VIDEO_SetNextFramebuffer(xfb);
    VIDEO_Flush();
    VIDEO_WaitVSync();
}
