#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "font_data.h"
#include "osd.h"
#include "plat.h"

#define LOG_LINES 64
#define LOG_COLS  160

static char            g_lines[LOG_LINES][LOG_COLS];
static int             g_count;                 /* total lines ever logged */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

void osd_log(const char *fmt, ...)
{
    char line[LOG_COLS];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);

    printf("[mozi] %s\n", line);

    pthread_mutex_lock(&g_lock);
    memcpy(g_lines[g_count % LOG_LINES], line, LOG_COLS);
    g_count++;
    pthread_mutex_unlock(&g_lock);
}

void osd_text(uint32_t *fb, int fb_w, int fb_h, int x, int y,
              const char *text, uint32_t fg, uint32_t bg)
{
    for (; *text; text++, x += FONT_W) {
        unsigned c = (unsigned char)*text;
        if (c < 32 || c > 126)
            c = '?';
        if (x < 0 || y < 0 || x + FONT_W > fb_w || y + FONT_H > fb_h)
            continue;

        const unsigned short *glyph = font_rows[c - 32];
        for (int row = 0; row < FONT_H; row++) {
            uint32_t *dst = fb + (size_t)(y + row) * fb_w + x;
            unsigned bits = glyph[row];
            for (int col = 0; col < FONT_W; col++)
                dst[col] = (bits & (0x8000u >> col)) ? fg : bg;
        }
    }
}

void osd_show_log(void)
{
    uint32_t *fb = plat_backbuffer();
    int w = plat_width(), h = plat_height();
    const uint32_t bg = 0xFF101428u, fg = 0xFFE8E8F0u;
    const int margin = 40;
    int rows = (h - 2 * margin) / FONT_H;
    int cols = (w - 2 * margin) / FONT_W;

    for (size_t i = 0; i < (size_t)w * h; i++)
        fb[i] = bg;

    if (rows > LOG_LINES) rows = LOG_LINES;
    if (cols > LOG_COLS - 1) cols = LOG_COLS - 1;

    pthread_mutex_lock(&g_lock);
    int first = g_count > rows ? g_count - rows : 0;
    for (int i = first; i < g_count; i++) {
        char line[LOG_COLS];
        memcpy(line, g_lines[i % LOG_LINES], LOG_COLS);
        line[cols] = '\0';
        osd_text(fb, w, h, margin, margin + (i - first) * FONT_H, line, fg, bg);
    }
    pthread_mutex_unlock(&g_lock);

    plat_flip();
}
