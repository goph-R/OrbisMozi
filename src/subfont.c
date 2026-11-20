#include <stdlib.h>
#include <string.h>

#include "subfont.h"

/* The font file is linked into the program as it is. */
__asm__(".section .rodata\n"
        ".global subfont_blob\n"
        "subfont_blob:\n"
        ".incbin \"src/subfont.bin\"\n"
        ".previous\n");
extern const uint8_t subfont_blob[];

#define MAX_LINES   5
#define MAX_GLYPHS  512

typedef struct {
    const uint8_t *fill;        /* w*h coverage */
    uint8_t *outline;           /* (w+2r)*(h+2r) coverage, made on first use */
    int cp, w, h, xoff, yoff, advance;
} Glyph;

typedef struct {
    int    line_h, radius, count;
    Glyph *glyphs;              /* sorted by code point */
} Font;

static Font g_fonts[2];
static int  g_loaded;

static unsigned rd16(const uint8_t *p) { return p[0] | p[1] << 8; }
static unsigned rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (unsigned)p[3] << 24; }

static void load(void)
{
    const uint8_t *b = subfont_blob;
    int n = (int)rd16(b + 4);

    g_loaded = 1;
    if (memcmp(b, "MZF1", 4) != 0)
        return;
    for (int f = 0; f < n && f < 2; f++) {
        const uint8_t *h = b + 6 + 12 * f;
        Font *font = &g_fonts[f];
        const uint8_t *t = b + rd32(h + 8);

        font->line_h = (int)rd16(h + 2);
        font->radius = rd16(h) >= 44 ? 3 : 2;
        font->count = (int)rd16(h + 6);
        font->glyphs = calloc((size_t)font->count, sizeof(Glyph));
        for (int i = 0; i < font->count; i++, t += 12) {
            Glyph *g = &font->glyphs[i];
            g->cp = (int)rd16(t);
            g->w = t[2];
            g->h = t[3];
            g->xoff = (int8_t)t[4];
            g->yoff = (int8_t)t[5];
            g->advance = t[6];
            g->fill = b + rd32(t + 8);
        }
    }
}

static Glyph *find(Font *font, int cp)
{
    int lo = 0, hi = font->count - 1;

    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (font->glyphs[mid].cp == cp)
            return &font->glyphs[mid];
        if (font->glyphs[mid].cp < cp)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return cp == '?' ? NULL : find(font, '?');
}

/* The outline is the glyph grown by `radius` pixels in every direction. */
static void make_outline(Font *font, Glyph *g)
{
    int r = font->radius, ow = g->w + 2 * r, oh = g->h + 2 * r;

    g->outline = calloc((size_t)ow * oh, 1);
    for (int y = 0; y < g->h; y++) {
        for (int x = 0; x < g->w; x++) {
            uint8_t a = g->fill[y * g->w + x];
            if (!a)
                continue;
            for (int dy = -r; dy <= r; dy++) {
                for (int dx = -r; dx <= r; dx++) {
                    if (dx * dx + dy * dy > r * r + r)      /* roughly a disc */
                        continue;
                    uint8_t *o = &g->outline[(y + r + dy) * ow + (x + r + dx)];
                    if (*o < a)
                        *o = a;
                }
            }
        }
    }
}

static int next_cp(const char **text)
{
    const unsigned char *p = (const unsigned char *)*text;
    unsigned c = *p++, cp = c;
    int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;

    if (extra)
        cp = c & (0x3F >> extra);
    for (; extra > 0 && (*p & 0xC0) == 0x80; extra--)
        cp = (cp << 6) | (*p++ & 0x3F);
    *text = (const char *)p;
    return (int)cp;
}

/* Blends one coverage plane in a single colour (0 = black, 255 = white). */
static void blend(uint32_t *fb, int W, int H, int x0, int y0, const uint8_t *cov, int w, int h, unsigned level)
{
    for (int y = 0; y < h; y++) {
        int py = y0 + y;
        if (py < 0 || py >= H)
            continue;
        for (int x = 0; x < w; x++) {
            int px = x0 + x;
            unsigned a = cov[y * w + x];
            if (!a || px < 0 || px >= W)
                continue;
            uint32_t *d = &fb[(size_t)py * W + px];
            unsigned r = (*d >> 16) & 255, g = (*d >> 8) & 255, b = *d & 255;
            r += ((int)level - (int)r) * (int)a / 255;
            g += ((int)level - (int)g) * (int)a / 255;
            b += ((int)level - (int)b) * (int)a / 255;
            *d = 0xFF000000u | r << 16 | g << 8 | b;
        }
    }
}

static int width_of(Font *font, const int *cps, int n)
{
    int w = 0;

    for (int i = 0; i < n; i++) {
        Glyph *g = find(font, cps[i]);
        if (g)
            w += g->advance;
    }
    return w;
}

static void draw_line(uint32_t *fb, int W, int H, Font *font, const int *cps, int n, int top_y)
{
    int x0 = (W - width_of(font, cps, n)) / 2;

    for (int pass = 0; pass < 2; pass++) {          /* all outlines first, then the letters */
        int x = x0;
        for (int i = 0; i < n; i++) {
            Glyph *g = find(font, cps[i]);
            if (!g)
                continue;
            if (g->w > 0) {
                if (pass == 0) {
                    int r = font->radius;
                    if (!g->outline)
                        make_outline(font, g);
                    blend(fb, W, H, x + g->xoff - r, top_y + g->yoff - r, g->outline,
                          g->w + 2 * r, g->h + 2 * r, 0);
                } else {
                    blend(fb, W, H, x + g->xoff, top_y + g->yoff, g->fill, g->w, g->h, 255);
                }
            }
            x += g->advance;
        }
    }
}

void subfont_draw(uint32_t *fb, int fb_w, int fb_h, const char *text, int bottom_y)
{
    static int cps[MAX_GLYPHS];
    int start[MAX_LINES], len[MAX_LINES], lines = 0, n = 0;

    if (!g_loaded)
        load();
    Font *font = &g_fonts[fb_h >= 1000 ? 1 : 0];
    if (!font->count || !text[0])
        return;

    /* Split into lines: at '\n', and at the last space that still fits. */
    int max_w = fb_w * 9 / 10;
    int line_start = 0, last_space = -1;
    while (lines < MAX_LINES) {
        int cp = *text ? next_cp(&text) : 0;
        if (cp == 0 || cp == '\n' || n >= MAX_GLYPHS - 1) {
            start[lines] = line_start;
            len[lines++] = n - line_start;
            line_start = n;
            last_space = -1;
            if (cp != '\n')
                break;
            continue;
        }
        if (cp == '\r')
            continue;
        if (cp == ' ')
            last_space = n;
        cps[n++] = cp;
        if (width_of(font, cps + line_start, n - line_start) > max_w && last_space > line_start) {
            start[lines] = line_start;
            len[lines++] = last_space - line_start;
            line_start = last_space + 1;
            last_space = -1;
        }
    }

    int y = bottom_y - lines * font->line_h;
    for (int i = 0; i < lines; i++, y += font->line_h)
        if (len[i] > 0)
            draw_line(fb, fb_w, fb_h, font, cps + start[i], len[i], y);
}
