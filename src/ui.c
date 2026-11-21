#include <stdlib.h>
#include <string.h>

#include "osd.h"
#include "ui.h"

/* The font and the pictures are linked into the program as they are. */
__asm__(".section .rodata\n"
        ".global uifont_blob\n"
        "uifont_blob:\n"
        ".incbin \"src/uifont.bin\"\n"
        ".global uiart_blob\n"
        "uiart_blob:\n"
        ".incbin \"src/uiart.bin\"\n"
        ".previous\n");
extern const uint8_t uifont_blob[], uiart_blob[];

#define MAX_FONTS 4

typedef struct {
    const uint8_t *table;       /* 12 bytes per glyph, sorted by code point */
    int count, ascent;
} Font;

typedef struct {
    const uint8_t *cov;         /* w*h coverage */
    int w, h, xoff, yoff, advance;
} Glyph;

static Font g_fonts[MAX_FONTS];
static int  g_loaded;

static unsigned rd16(const uint8_t *p) { return p[0] | p[1] << 8; }
static unsigned rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (unsigned)p[3] << 24; }

static Font *font_of(int font)
{
    if (!g_loaded) {
        const uint8_t *b = uifont_blob;
        g_loaded = 1;
        if (memcmp(b, "MZF1", 4) == 0) {
            for (int f = 0; f < (int)rd16(b + 4) && f < MAX_FONTS; f++) {
                const uint8_t *h = b + 6 + 12 * f;
                g_fonts[f].ascent = (int)rd16(h + 4);
                g_fonts[f].count = (int)rd16(h + 6);
                g_fonts[f].table = b + rd32(h + 8);
            }
        }
    }
    return &g_fonts[font >= 0 && font < MAX_FONTS ? font : 0];
}

/* Looks a character up; unknown ones become '?'. Returns 0 if there is nothing to use. */
static int find(const Font *font, int cp, Glyph *g)
{
    int lo = 0, hi = font->count - 1;

    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        const uint8_t *t = font->table + 12 * mid;
        int c = (int)rd16(t);
        if (c == cp) {
            g->w = t[2];
            g->h = t[3];
            g->xoff = (int8_t)t[4];
            g->yoff = (int8_t)t[5];
            g->advance = t[6];
            g->cov = uifont_blob + rd32(t + 8);
            return 1;
        }
        if (c < cp)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return cp == '?' ? 0 : find(font, '?', g);
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

int ui_text_width(int font, const char *text)
{
    const Font *f = font_of(font);
    Glyph g;
    int w = 0;

    while (*text)
        if (find(f, next_cp(&text), &g))
            w += g.advance;
    return w;
}

int ui_text(uint32_t *fb, int W, int H, int x, int baseline, int font, const char *text, uint32_t colour)
{
    const Font *f = font_of(font);
    int cr = (colour >> 16) & 255, cg = (colour >> 8) & 255, cb = colour & 255;
    Glyph g;

    while (*text) {
        if (!find(f, next_cp(&text), &g))
            continue;
        int x0 = x + g.xoff, y0 = baseline - f->ascent + g.yoff;
        for (int row = 0; row < g.h; row++) {
            int py = y0 + row;
            if (py < 0 || py >= H)
                continue;
            for (int col = 0; col < g.w; col++) {
                int px = x0 + col, a = g.cov[row * g.w + col];
                if (!a || px < 0 || px >= W)
                    continue;
                uint32_t *d = &fb[(size_t)py * W + px];
                int r = (*d >> 16) & 255, gr = (*d >> 8) & 255, b = *d & 255;
                r += (cr - r) * a / 255;
                gr += (cg - gr) * a / 255;
                b += (cb - b) * a / 255;
                *d = 0xFF000000u | (uint32_t)r << 16 | (uint32_t)gr << 8 | (uint32_t)b;
            }
        }
        x += g.advance;
    }
    return x;
}

void ui_fit(int font, const char *text, int max_w, char *out, size_t out_size)
{
    const Font *f = font_of(font);
    const char *p = text, *cut = text;      /* cut: how much fits together with the "…" */
    int w = 0, dots = ui_text_width(font, "\xE2\x80\xA6");
    Glyph g;

    while (*p) {
        int adv = find(f, next_cp(&p), &g) ? g.advance : 0;
        if (w + adv > max_w) {
            size_t n = (size_t)(cut - text);
            if (n + 4 > out_size)
                n = out_size > 4 ? out_size - 4 : 0;
            if (out_size >= 4) {
                memcpy(out, text, n);
                memcpy(out + n, "\xE2\x80\xA6", 4);
            } else if (out_size > 0) {
                out[0] = '\0';
            }
            return;
        }
        w += adv;
        if (w + dots <= max_w)
            cut = p;
    }
    if (out_size > 0) {
        size_t n = strlen(text);
        if (n >= out_size)
            n = out_size - 1;
        memcpy(out, text, n);
        out[n] = '\0';
    }
}

void ui_image(uint32_t *fb, int W, int H, int cx, int cy, int image)
{
    const uint8_t *b = uiart_blob;

    if (memcmp(b, "MZA1", 4) != 0 || image < 0 || image >= (int)rd16(b + 4))
        return;
    const uint8_t *e = b + 6 + 8 * image;
    int w = (int)rd16(e), h = (int)rd16(e + 2);
    const uint8_t *px = b + rd32(e + 4);
    int x0 = cx - w / 2, y0 = cy - h / 2;

    for (int row = 0; row < h; row++) {
        int y = y0 + row;
        if (y < 0 || y >= H)
            continue;
        for (int col = 0; col < w; col++) {
            const uint8_t *s = px + ((size_t)row * w + col) * 4;
            int x = x0 + col, keep = 255 - s[3];
            if (!s[3] || x < 0 || x >= W)
                continue;
            uint32_t *d = &fb[(size_t)y * W + x];
            /* The picture's colours are already multiplied by its alpha. */
            unsigned r = s[0] + ((*d >> 16) & 255) * keep / 255;
            unsigned g = s[1] + ((*d >> 8) & 255) * keep / 255;
            unsigned bl = s[2] + (*d & 255) * keep / 255;
            *d = 0xFF000000u | r << 16 | g << 8 | bl;
        }
    }
}

void ui_dim(uint32_t *fb, int W, int H, int x, int y, int w, int h, int keep)
{
    int x1 = x + w > W ? W : x + w, y1 = y + h > H ? H : y + h;

    if (x < 0) x = 0;
    if (y < 0) y = 0;
    for (int py = y; py < y1; py++) {
        uint32_t *d = &fb[(size_t)py * W + x];
        for (int px = x; px < x1; px++, d++) {
            uint32_t rb = ((*d & 0x00FF00FFu) * (unsigned)keep >> 8) & 0x00FF00FFu;
            uint32_t g = ((*d & 0x0000FF00u) * (unsigned)keep >> 8) & 0x0000FF00u;
            *d = 0xFF000000u | rb | g;
        }
    }
}

void ui_frame(uint32_t *fb, int W, int H, int x, int y, int w, int h, int thick, uint32_t colour)
{
    osd_fill(fb, W, H, x, y, w, thick, colour);
    osd_fill(fb, W, H, x, y + h - thick, w, thick, colour);
    osd_fill(fb, W, H, x, y + thick, thick, h - 2 * thick, colour);
    osd_fill(fb, W, H, x + w - thick, y + thick, thick, h - 2 * thick, colour);
}

int ui_hint(uint32_t *fb, int fb_w, int fb_h, int x, int image, const char *label)
{
    ui_image(fb, fb_w, fb_h, x + 24, 635, image);
    return ui_text(fb, fb_w, fb_h, x + 67, 644, UI_FONT_TEXT, label, 0xFFFFFFFFu) + 79;
}
