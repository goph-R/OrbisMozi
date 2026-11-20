/* The subtitle font: anti-aliased, with accented letters, drawn white with
 * a black outline. Glyph data comes from src/subfont.bin (tools/mksubfont.py). */
#ifndef SUBFONT_H
#define SUBFONT_H

#include <stdint.h>

/* Draws UTF-8 text (lines separated by '\n', long lines are wrapped),
 * centred, with its last line ending at bottom_y. */
void subfont_draw(uint32_t *fb, int fb_w, int fb_h, const char *text, int bottom_y);

#endif
