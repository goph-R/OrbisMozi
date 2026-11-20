/* The menu background: an abstract blue picture with dust drifting upwards. */
#ifndef BACKDROP_H
#define BACKDROP_H

#include <stdint.h>

/* Fills the whole buffer with the background at the current moment. */
void backdrop_draw(uint32_t *fb, int fb_w, int fb_h);

/* Darkens a rectangle of what is already drawn (a panel for text to sit on). */
void backdrop_shade(uint32_t *fb, int fb_w, int fb_h, int x, int y, int w, int h);

#endif
