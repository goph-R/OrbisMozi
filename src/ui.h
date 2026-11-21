/* What the menu screens are drawn with: the menu font (Noto Sans, from
 * src/uifont.bin), the pictures (src/uiart.bin) and a few shapes.
 * All sizes are for the 1280x720 menu. */
#ifndef UI_H
#define UI_H

#include <stddef.h>
#include <stdint.h>

enum { UI_FONT_TEXT, UI_FONT_TITLE };       /* 24 and 42 pixels */

/* The order of the pictures in tools/mkuiart.py. */
enum {
    UI_IMG_LOGO, UI_IMG_DRIVE, UI_IMG_SHARE, UI_IMG_PLUS,
    UI_IMG_CROSS, UI_IMG_CIRCLE, UI_IMG_SQUARE, UI_IMG_TRIANGLE
};

/* Draws one line of UTF-8 text standing on the line y = baseline.
 * Returns the x position after the last character. */
int  ui_text(uint32_t *fb, int fb_w, int fb_h, int x, int baseline, int font,
             const char *text, uint32_t colour);
int  ui_text_width(int font, const char *text);

/* Copies text to out; if it is wider than max_w, it is cut and ends in "…". */
void ui_fit(int font, const char *text, int max_w, char *out, size_t out_size);

/* Draws a picture with its centre at (cx, cy). */
void ui_image(uint32_t *fb, int fb_w, int fb_h, int cx, int cy, int image);

/* Darkens a rectangle: every pixel keeps `keep`/256 of its brightness. */
void ui_dim(uint32_t *fb, int fb_w, int fb_h, int x, int y, int w, int h, int keep);

/* Draws the outline of a rectangle, `thick` pixels wide, inside x, y, w, h. */
void ui_frame(uint32_t *fb, int fb_w, int fb_h, int x, int y, int w, int h, int thick, uint32_t colour);

#endif
