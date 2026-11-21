#include <string.h>

#include "menu.h"
#include "osd.h"
#include "plat.h"
#include "ui.h"

#define COL_TEXT    0xFFFFFFFFu
#define COL_BOX     0xFF2B2B30u
#define MAX_ROWS    10                  /* lines of a list that fit on the screen */

/* Sizes are written for a 720-line screen and grow with the real one. */
typedef struct {
    uint32_t *fb;
    int W, H, font, title_font;
    int box_w, row_h, pad, title_h, x, y, h;
} Box;

static int scaled(int v, int H) { return v * H / 720; }

/* Paints the standing picture and an empty dialog with its title. */
static Box open_box(const uint32_t *still, const char *title, int rows, int width)
{
    Box b;

    b.fb = plat_backbuffer();
    b.W = plat_width();
    b.H = plat_height();
    b.font = b.H >= 1000 ? UI_FONT_LARGE : UI_FONT_TEXT;
    b.title_font = b.font;
    b.box_w = scaled(width, b.H);
    b.row_h = scaled(50, b.H);
    b.pad = scaled(28, b.H);
    b.title_h = scaled(64, b.H);
    b.h = b.title_h + rows * b.row_h + b.pad;
    b.x = (b.W - b.box_w) / 2;
    b.y = (b.H - b.h) / 2;

    memcpy(b.fb, still, (size_t)b.W * b.H * sizeof(*b.fb));
    osd_fill(b.fb, b.W, b.H, b.x, b.y, b.box_w, b.h, COL_BOX);
    ui_frame(b.fb, b.W, b.H, b.x, b.y, b.box_w, b.h, 1, COL_TEXT);
    ui_text(b.fb, b.W, b.H, (b.W - ui_text_width(b.title_font, title)) / 2, b.y + scaled(44, b.H),
            b.title_font, title, COL_TEXT);
    osd_fill(b.fb, b.W, b.H, b.x + b.pad, b.y + b.title_h - scaled(8, b.H), b.box_w - 2 * b.pad, 1, 0xFF6A6A72u);
    return b;
}

static int row_y(const Box *b, int row) { return b->y + b->title_h + row * b->row_h; }
static int row_base(const Box *b, int row) { return row_y(b, row) + b->row_h * 35 / 50; }

int menu_pick(const uint32_t *still, const char *title, const char *const *items, int count, int selected)
{
    unsigned prev = plat_pad();
    uint64_t repeat_at = 0;
    int rows = count < MAX_ROWS ? count : MAX_ROWS;     /* a longer list scrolls */
    int top = 0;

    if (selected < 0 || selected >= count)
        selected = 0;
    for (;;) {
        Box b = open_box(still, title, rows, 620);
        char shown[300];

        if (selected < top)
            top = selected;
        if (selected >= top + rows)
            top = selected - rows + 1;

        for (int r = 0; r < rows; r++) {
            int i = top + r, on = i == selected;
            if (on)
                osd_fill(b.fb, b.W, b.H, b.x + b.pad, row_y(&b, r) + 2, b.box_w - 2 * b.pad, b.row_h - 4, COL_TEXT);
            ui_fit(b.font, items[i], b.box_w - 2 * b.pad - 2 * scaled(18, b.H), shown, sizeof(shown));
            ui_text(b.fb, b.W, b.H, b.x + b.pad + scaled(18, b.H), row_base(&b, r), b.font, shown,
                    on ? COL_BOX : COL_TEXT);
        }
        if (count > rows) {                             /* where the window is in the list */
            int track_h = rows * b.row_h - 4, bar_w = scaled(4, b.H);
            int bar_h = track_h * rows / count, bar_y = (track_h - bar_h) * top / (count - rows);
            int bar_x = b.x + b.box_w - b.pad / 2 - bar_w / 2;
            osd_fill(b.fb, b.W, b.H, bar_x, row_y(&b, 0) + 2, bar_w, track_h, 0xFF4A4A52u);
            osd_fill(b.fb, b.W, b.H, bar_x, row_y(&b, 0) + 2 + bar_y, bar_w, bar_h, COL_TEXT);
        }
        plat_flip();

        unsigned pad = plat_pad();
        unsigned pressed = pad & ~prev;
        uint64_t now = plat_time_us();
        const unsigned repeating = PAD_UP | PAD_DOWN;

        if (pressed & repeating) {
            repeat_at = now + 400000;
        } else if ((pad & repeating) && now >= repeat_at) {
            pressed |= pad & repeating;
            repeat_at = now + 120000;
        }
        prev = pad;

        if (pressed & PAD_UP)     selected = selected > 0 ? selected - 1 : count - 1;
        if (pressed & PAD_DOWN)   selected = selected + 1 < count ? selected + 1 : 0;
        if (pressed & PAD_CIRCLE) return -1;
        if (pressed & PAD_CROSS)  return selected;
    }
}

void menu_table(const uint32_t *still, const char *title, const char *const *left,
                const char *const *right, int count)
{
    unsigned prev = plat_pad();

    for (;;) {
        Box b = open_box(still, title, count, 760);

        for (int i = 0; i < count; i++) {
            ui_text(b.fb, b.W, b.H, b.x + b.pad + scaled(18, b.H), row_base(&b, i), b.font, left[i], COL_TEXT);
            ui_text(b.fb, b.W, b.H, b.x + b.box_w * 45 / 100, row_base(&b, i), b.font, right[i], 0xFFC8C8D0u);
        }
        plat_flip();

        unsigned pad = plat_pad();
        unsigned pressed = pad & ~prev;
        prev = pad;
        if (pressed & (PAD_CROSS | PAD_CIRCLE))
            return;
    }
}
