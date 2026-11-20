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
static FILE           *g_file;                  /* optional copy of the log on disk */

void osd_log_to_file(const char *path)
{
    pthread_mutex_lock(&g_lock);
    if (g_file)
        fclose(g_file);
    g_file = fopen(path, "w");
    pthread_mutex_unlock(&g_lock);
}

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
    if (g_file) {                               /* flushed per line: must survive a freeze */
        fprintf(g_file, "%8.3f  %s\n", (double)plat_time_us() / 1e6, line);
        fflush(g_file);
    }
    pthread_mutex_unlock(&g_lock);
}

/* Reads one UTF-8 character and returns the closest printable ASCII one:
 * accented Latin letters lose their accent, anything else becomes '?'.
 * (The built-in font only has ASCII.) */
static unsigned next_char(const char **text)
{
    static const char latin1[] =            /* U+00C0 .. U+00FF */
        "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";
    const unsigned char *p = (const unsigned char *)*text;
    unsigned c = *p++, cp = c;
    int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;

    if (extra)
        cp = c & (0x3F >> extra);
    for (; extra > 0 && (*p & 0xC0) == 0x80; extra--)
        cp = (cp << 6) | (*p++ & 0x3F);
    *text = (const char *)p;

    if (cp >= 32 && cp <= 126) return cp;
    if (cp >= 0xC0 && cp <= 0xFF) return (unsigned char)latin1[cp - 0xC0];
    switch (cp) {
    case 0x150: return 'O';  case 0x151: return 'o';    /* O, o with double acute */
    case 0x170: return 'U';  case 0x171: return 'u';    /* U, u with double acute */
    case 0x2013: case 0x2014: return '-';
    case 0x2018: case 0x2019: return '\'';
    case 0x201C: case 0x201D: return '"';
    }
    return '?';
}

int osd_text(uint32_t *fb, int fb_w, int fb_h, int x, int y,
             const char *text, uint32_t fg, uint32_t bg)
{
    while (*text) {
        unsigned c = next_char(&text);
        if (x >= 0 && y >= 0 && x + FONT_W <= fb_w && y + FONT_H <= fb_h) {
            const unsigned short *glyph = font_rows[c - 32];
            for (int row = 0; row < FONT_H; row++) {
                uint32_t *dst = fb + (size_t)(y + row) * fb_w + x;
                unsigned bits = glyph[row];
                for (int col = 0; col < FONT_W; col++)
                    dst[col] = (bits & (0x8000u >> col)) ? fg : bg;
            }
        }
        x += FONT_W;
    }
    return x;
}

void osd_fill(uint32_t *fb, int fb_w, int fb_h, int x, int y, int w, int h, uint32_t colour)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > fb_w) w = fb_w - x;
    if (y + h > fb_h) h = fb_h - y;
    for (int row = 0; row < h; row++) {
        uint32_t *dst = fb + (size_t)(y + row) * fb_w + x;
        for (int col = 0; col < w; col++)
            dst[col] = colour;
    }
}

int osd_font_width(void)  { return FONT_W; }
int osd_font_height(void) { return FONT_H; }

void osd_last_line(char *out, int out_size)
{
    pthread_mutex_lock(&g_lock);
    snprintf(out, (size_t)out_size, "%s", g_count ? g_lines[(g_count - 1) % LOG_LINES] : "");
    pthread_mutex_unlock(&g_lock);
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
