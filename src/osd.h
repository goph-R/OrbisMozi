/* On-screen text: a scrolling log and a text drawing helper. */
#ifndef OSD_H
#define OSD_H

#include <stdint.h>

/* Adds a line to the log (printf style). Also goes to stdout. Thread safe. */
void osd_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Also writes every log line to this file (emptied first). The file can be
 * fetched from the console afterwards, which helps when the screen is frozen. */
void osd_log_to_file(const char *path);

/* Draws the log full-screen into the back buffer and flips. Main thread only. */
void osd_show_log(void);

/* Draws one line of UTF-8 text on a solid background box, or straight onto
 * what is already there if bg is 0. Returns the x position after the last
 * character. Accents are dropped (ASCII-only font). */
int  osd_text(uint32_t *fb, int fb_w, int fb_h, int x, int y,
              const char *text, uint32_t fg, uint32_t bg);

/* Fills a rectangle, clipped to the buffer. */
void osd_fill(uint32_t *fb, int fb_w, int fb_h, int x, int y, int w, int h, uint32_t colour);

int  osd_font_width(void);
int  osd_font_height(void);

/* Copies the newest log line (empty string if there is none). */
void osd_last_line(char *out, int out_size);

#endif
