/* A dialog over a standing picture: a list to choose from, or a page of
 * text. Used over a paused video, so it works on the 720-line and the
 * 1080-line screen alike. */
#ifndef MENU_H
#define MENU_H

#include <stdint.h>

/* Shows `count` lines under a title and lets the user pick one with the
 * d-pad and cross. `still` is the picture behind the dialog (one full
 * screen, already dimmed). Returns the line picked, or -1 for circle. */
int  menu_pick(const uint32_t *still, const char *title, const char *const *items, int count, int selected);

/* Shows a two-column table (left[i] beside right[i]) until cross or circle. */
void menu_table(const uint32_t *still, const char *title, const char *const *left,
                const char *const *right, int count);

#endif
