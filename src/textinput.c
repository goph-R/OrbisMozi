#include <stdio.h>
#include <string.h>

#include "osd.h"
#include "plat.h"
#include "textinput.h"

#define COL_BG      0xFF101428u
#define COL_TEXT    0xFFE8E8F0u
#define COL_DIM     0xFF8890A8u
#define COL_SEL_BG  0xFF2F5FD0u
#define COL_FIELD   0xFF202848u

/* The built-in keyboard: printable ASCII, laid out in rows. */
static const char *g_rows[] = {
    "abcdefghijklm",
    "nopqrstuvwxyz",
    "0123456789.-_",
    "@/:!#$%&+=?~ ",
    "*(),;'\"<>[]\\^",
    "{}|`",
};
#define N_ROWS ((int)(sizeof(g_rows) / sizeof(g_rows[0])))

static void wait_release(void)
{
    while (plat_pad() != 0)
        plat_sleep_us(10000);
}

static int builtin_keyboard(const char *title, char *buf, int buf_size, int password)
{
    char text[256];
    int row = 0, col = 0, shift = 0;
    unsigned prev = plat_pad();
    uint64_t repeat_at = 0;

    snprintf(text, sizeof(text), "%s", buf);
    if (buf_size > (int)sizeof(text))
        buf_size = sizeof(text);

    for (;;) {
        uint32_t *fb = plat_backbuffer();
        int W = plat_width(), H = plat_height();
        int fw = osd_font_width(), fh = osd_font_height();
        int mx = W / 20, my = H / 20;
        int cell_w = 4 * fw, cell_h = fh + 14;
        int grid_x = mx, grid_y = my + 6 * fh;
        char shown[300];

        osd_fill(fb, W, H, 0, 0, W, H, COL_BG);
        osd_text(fb, W, H, mx, my, title, COL_TEXT, COL_BG);

        /* The text so far, with a cursor. */
        size_t len = strlen(text);
        if (password) {
            memset(shown, '*', len);
            shown[len] = '\0';
        } else {
            snprintf(shown, sizeof(shown), "%s", text);
        }
        strcat(shown, "_");
        osd_fill(fb, W, H, mx, my + 2 * fh, W - 2 * mx, fh + 16, COL_FIELD);
        int max_chars = (W - 2 * mx) / fw - 2;
        const char *tail = (int)strlen(shown) > max_chars ? shown + strlen(shown) - max_chars : shown;
        osd_text(fb, W, H, mx + fw, my + 2 * fh + 8, tail, 0xFFFFFFFFu, COL_FIELD);

        for (int r = 0; r < N_ROWS; r++) {
            for (int c = 0; g_rows[r][c]; c++) {
                char ch = g_rows[r][c];
                char label[4] = { ch, 0, 0, 0 };
                int sel = r == row && c == col;
                uint32_t bg = sel ? COL_SEL_BG : COL_BG;
                int x = grid_x + c * cell_w, y = grid_y + r * cell_h;

                if (shift && ch >= 'a' && ch <= 'z')
                    label[0] = (char)(ch - 32);
                if (ch == ' ')
                    snprintf(label, sizeof(label), "SPC");
                if (sel)
                    osd_fill(fb, W, H, x, y, cell_w - 4, cell_h - 4, bg);
                osd_text(fb, W, H, x + (cell_w - 4 - (int)strlen(label) * fw) / 2, y + 5, label,
                         sel ? 0xFFFFFFFFu : COL_TEXT, bg);
            }
        }

        osd_text(fb, W, H, mx, H - my - 2 * fh - 4,
                 "X type    Square delete    Triangle upper/lower case", COL_DIM, COL_BG);
        osd_text(fb, W, H, mx, H - my - fh, "OPTIONS or R1 done    O cancel", COL_DIM, COL_BG);
        plat_flip();

        unsigned pad = plat_pad();
        unsigned pressed = pad & ~prev;
        uint64_t now = plat_time_us();
        const unsigned repeating = PAD_UP | PAD_DOWN | PAD_LEFT | PAD_RIGHT | PAD_SQUARE;

        if (pressed & repeating) {
            repeat_at = now + 350000;
        } else if ((pad & repeating) && now >= repeat_at) {
            pressed |= pad & repeating;
            repeat_at = now + 80000;
        }
        prev = pad;

        if (pressed & PAD_UP)    row = (row + N_ROWS - 1) % N_ROWS;
        if (pressed & PAD_DOWN)  row = (row + 1) % N_ROWS;
        int row_len = (int)strlen(g_rows[row]);
        if (col >= row_len)
            col = row_len - 1;
        if (pressed & PAD_LEFT)  col = (col + row_len - 1) % row_len;
        if (pressed & PAD_RIGHT) col = (col + 1) % row_len;

        if (pressed & PAD_TRIANGLE)
            shift = !shift;
        if ((pressed & PAD_SQUARE) && len > 0)
            text[len - 1] = '\0';
        if ((pressed & PAD_CROSS) && (int)len < buf_size - 1) {
            char ch = g_rows[row][col];
            if (shift && ch >= 'a' && ch <= 'z')
                ch = (char)(ch - 32);
            text[len] = ch;
            text[len + 1] = '\0';
        }
        if (pressed & PAD_CIRCLE)
            return 0;
        if (pressed & (PAD_OPTIONS | PAD_R1)) {
            snprintf(buf, (size_t)buf_size, "%s", text);
            return 1;
        }
    }
}

int text_input(const char *title, char *buf, int buf_size, int password, void (*redraw)(void))
{
    int rc = plat_keyboard(title, buf, buf_size, password, redraw);

    if (rc < 0)                                     /* no system keyboard here */
        rc = builtin_keyboard(title, buf, buf_size, password);
    wait_release();                                 /* the confirming press is not for the caller */
    return rc;
}
