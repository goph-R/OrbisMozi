/* The form for a network share: five text fields, a connection test, save. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "backdrop.h"
#include "osd.h"
#include "plat.h"
#include "shareform.h"
#include "smb.h"
#include "textinput.h"

#define COL_TEXT    0xFFE8E8F0u
#define COL_DIM     0xFFB4C2E4u
#define COL_SEL_BG  0xFF3D7BFFu
#define COL_OK      0xFF80E090u
#define COL_ERROR   0xFFFF8080u

enum { F_SERVER, F_SHARE, F_USER, F_PASSWORD, F_FOLDER, N_FIELDS,
       A_TEST = N_FIELDS, A_SAVE, A_DELETE };

static const struct {
    const char *label, *title, *hint;
} g_fields[N_FIELDS] = {
    { "Server",   "Server address", "The computer or NAS: its IP address, for example 192.168.1.10" },
    { "Share",    "Share name",     "The name of the shared folder on that server" },
    { "User",     "User name",      "Leave empty if the share needs no login" },
    { "Password", "Password",       "Stored on this console as plain text" },
    { "Folder",   "Start folder",   "Optional: a folder inside the share to start in, for example films/new" },
};

static struct {
    SmbParts parts;
    int      is_new, sel;
    char     message[200];
    uint32_t message_colour;
    const char *busy;
} F;

static char *field_text(int i, int *size)
{
    switch (i) {
    case F_SERVER:   *size = sizeof(F.parts.host);     return F.parts.host;
    case F_SHARE:    *size = sizeof(F.parts.share);    return F.parts.share;
    case F_USER:     *size = sizeof(F.parts.user);     return F.parts.user;
    case F_PASSWORD: *size = sizeof(F.parts.password); return F.parts.password;
    default:         *size = 256;                      return F.parts.path;
    }
}

static int row_count(void)
{
    return F.is_new ? A_SAVE + 1 : A_DELETE + 1;
}

static void draw(void)
{
    uint32_t *fb = plat_backbuffer();
    int W = plat_width(), H = plat_height();
    int fw = osd_font_width(), fh = osd_font_height();
    int mx = W / 20, my = H / 20;
    int row_h = fh + 10, y0 = my + 3 * fh;
    int value_x = mx + 14 * fw;
    int max_chars = (W - mx - value_x) / fw - 2;

    backdrop_draw(fb, W, H);
    backdrop_shade(fb, W, H, mx - 16, y0 - 10, W - 2 * mx + 32, (row_count() + 4) * row_h + 20);
    osd_text(fb, W, H, mx, my, "Orbis Mozi", COL_TEXT, 0);
    osd_text(fb, W, H, mx, my + fh + 4, F.is_new ? "Add a network share" : "Change a network share",
             COL_DIM, 0);

    for (int i = 0; i < row_count(); i++) {
        int y = y0 + i * row_h + (i >= N_FIELDS ? row_h / 2 : 0);
        int sel = i == F.sel;
        uint32_t bg = sel ? COL_SEL_BG : 0;

        if (sel)
            osd_fill(fb, W, H, mx, y, W - 2 * mx, row_h, bg);
        if (i < N_FIELDS) {
            char shown[300];
            int size;
            const char *text = field_text(i, &size);
            size_t len = strlen(text);

            if (i == F_PASSWORD) {
                memset(shown, '*', len);
                shown[len] = '\0';
            } else {
                snprintf(shown, sizeof(shown), "%s", text);
            }
            if ((int)strlen(shown) > max_chars)
                strcpy(shown + max_chars - 3, "...");
            osd_text(fb, W, H, mx + fw, y + 5, g_fields[i].label, sel ? 0xFFFFFFFFu : COL_DIM, bg);
            osd_text(fb, W, H, value_x, y + 5, len ? shown : "-", sel ? 0xFFFFFFFFu : COL_TEXT, bg);
        } else {
            const char *label = i == A_TEST ? "Test connection" : i == A_SAVE ? "Save" : "Delete this share";
            osd_text(fb, W, H, mx + fw, y + 5, label, sel ? 0xFFFFFFFFu : COL_TEXT, bg);
        }
    }

    int msg_y = y0 + (row_count() + 2) * row_h;
    if (F.busy)
        osd_text(fb, W, H, mx + fw, msg_y, F.busy, COL_DIM, 0);
    else if (F.message[0])
        osd_text(fb, W, H, mx + fw, msg_y, F.message, F.message_colour, 0);

    if (F.sel < N_FIELDS)
        osd_text(fb, W, H, mx, H - my - 2 * fh - 4, g_fields[F.sel].hint, COL_DIM, 0);
    osd_text(fb, W, H, mx, H - my - fh, F.sel < N_FIELDS ? "X edit    O cancel" : "X choose    O cancel",
             COL_DIM, 0);
    plat_flip();
}

static void say(uint32_t colour, const char *text)
{
    int max_chars = (plat_width() - 2 * (plat_width() / 20)) / osd_font_width() - 2;

    snprintf(F.message, sizeof(F.message), "%s", text);
    if (max_chars < (int)sizeof(F.message) - 1 && (int)strlen(F.message) > max_chars)
        strcpy(F.message + max_chars - 3, "...");
    F.message_colour = colour;
}

/* Builds the address from the fields; complains on screen if it cannot. */
static int build(char *url, size_t url_size)
{
    if (!F.parts.host[0] || !F.parts.share[0]) {
        say(COL_ERROR, "Server and share are needed.");
        return -1;
    }
    return smb_join(&F.parts, url, url_size);
}

static void test_connection(void)
{
    char url[1500], err[200], text[240];
    SmbEntry *entries;
    int count;

    if (build(url, sizeof(url)) != 0)
        return;
    F.busy = "Connecting...";
    draw();
    F.busy = NULL;

    if (smb_list(url, &entries, &count, err, sizeof(err)) == 0) {
        free(entries);
        snprintf(text, sizeof(text), "Connection works: %d entries found.", count);
        say(COL_OK, text);
    } else {
        osd_log("share test: %s", err);
        say(COL_ERROR, err);
    }
}

int shareform_run(char *url, size_t url_size, int is_new)
{
    unsigned prev = plat_pad();
    uint64_t repeat_at = 0;

    memset(&F, 0, sizeof(F));
    F.is_new = is_new;
    if (!is_new)
        smb_split(url, &F.parts);

    for (;;) {
        draw();

        unsigned pad = plat_pad();
        unsigned pressed = pad & ~prev;
        uint64_t now = plat_time_us();
        const unsigned moves = PAD_UP | PAD_DOWN;

        if (pressed & moves) {
            repeat_at = now + 350000;
        } else if ((pad & moves) && now >= repeat_at) {
            pressed |= pad & moves;
            repeat_at = now + 120000;
        }
        prev = pad;

        int n = row_count();
        if (pressed & PAD_UP)   F.sel = (F.sel + n - 1) % n;
        if (pressed & PAD_DOWN) F.sel = (F.sel + 1) % n;

        if (pressed & PAD_CIRCLE)
            return FORM_CANCEL;
        if (!(pressed & PAD_CROSS))
            continue;

        if (F.sel < N_FIELDS) {
            int size;
            char *text = field_text(F.sel, &size);
            if (text_input(g_fields[F.sel].title, text, size, F.sel == F_PASSWORD, draw))
                F.message[0] = '\0';                /* an old test result no longer applies */
            prev = plat_pad();
        } else if (F.sel == A_TEST) {
            test_connection();
            prev = plat_pad();
        } else if (F.sel == A_SAVE) {
            if (build(url, url_size) == 0)
                return FORM_SAVE;
        } else if (F.sel == A_DELETE) {
            return FORM_DELETE;
        }
    }
}
