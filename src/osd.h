/* On-screen text: a scrolling log and a text drawing helper. */
#ifndef OSD_H
#define OSD_H

#include <stdint.h>

/* Adds a line to the log (printf style). Also goes to stdout. Thread safe. */
void osd_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Draws the log full-screen into the back buffer and flips. Main thread only. */
void osd_show_log(void);

/* Draws one line of text with a solid background box. */
void osd_text(uint32_t *fb, int fb_w, int fb_h, int x, int y,
              const char *text, uint32_t fg, uint32_t bg);

#endif
