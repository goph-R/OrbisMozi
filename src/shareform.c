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
#include "ui.h"

#define COL_TEXT    0xFFFFFFFFu
#define COL_OK      0xFFA6F5B4u
#define COL_ERROR   0xFFFF9C9Cu
#define COL_SAVE_BG 0xFFFFFFFFu             /* the Save button: white, dark blue text */
#define COL_SAVE    0xFF051D49u
#define COL_BOX     0xFF2B2B30u             /* the question box: dark grey */

/* Sizes for the 1280x720 menu, taken from the 1080p mockup (times 2/3). */
#define MX          64                  /* left and right margin */
#define FIELD_Y     169                 /* the five fields */
#define FIELD_H     54
#define BUTTON_Y    510                 /* the row of buttons under them */
#define BUTTON_H    72
#define ROW_KEEP    174                 /* rows are black at 32 %: 174/256 of the light stays */

enum { F_SERVER, F_SHARE, F_USER, F_PASSWORD, F_FOLDER, N_FIELDS,
       A_DELETE = N_FIELDS, A_TEST, A_SAVE };

static const struct {
    const char *label, *title, *hint;
} g_fields[N_FIELDS] = {
    { "Server IP",         "Server address", "The computer or NAS, for example 192.168.1.10" },
    { "Share",             "Share name",     "The name of the shared folder on that server" },
    { "User",              "User name",      "Leave empty if the share needs no login" },
    { "Password",          "Password",       "Stored on this console as plain text" },
    { "Folder (optional)", "Start folder",   "A folder inside the share to start in, for example films/new" },
};

/* The buttons, left to right. "Delete this share" exists only when changing a share. */
static const struct {
    const char *label;
    int x, w;
} g_buttons[3] = {
    { "Delete this share", 64,   319 },
    { "Test connection",   724,  305 },
    { "Save",              1041, 175 },
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

static int field_y(int i)
{
    return FIELD_Y + (i * 190 + 1) / 3;         /* a pitch of 95 mockup pixels */
}

/* The white frame around the selected row or button. */
static void select_frame(uint32_t *fb, int W, int H, int x, int y, int w, int h)
{
    ui_frame(fb, W, H, x - 8, y - 8, w + 16, h + 16, 5, COL_TEXT);
}

/* Paints the form into the back buffer, without showing it yet. */
static void paint(uint32_t *fb, int W, int H)
{
    char shown[300];

    backdrop_draw(fb, W, H);
    ui_image(fb, W, H, 87, 97, F.is_new ? UI_IMG_PLUS : UI_IMG_SHARE);
    ui_text(fb, W, H, 128, 109, UI_FONT_TEXT, F.is_new ? "Add network share" : "Edit network share", COL_TEXT);
    osd_fill(fb, W, H, MX, 147, W - 2 * MX, 1, COL_TEXT);

    for (int i = 0; i < N_FIELDS; i++) {
        int y = field_y(i), size;
        const char *text = field_text(i, &size);
        size_t len = strlen(text);
        char value[300];

        if (i == F_PASSWORD) {
            if (len > 40)
                len = 40;
            memset(value, '*', len);
            value[len] = '\0';
        } else {
            snprintf(value, sizeof(value), "%s", text);
        }
        ui_dim(fb, W, H, MX, y, W - 2 * MX, FIELD_H, ROW_KEEP);
        if (i == F.sel)
            select_frame(fb, W, H, MX, y, W - 2 * MX, FIELD_H);
        int label_end = ui_text(fb, W, H, 130, y + 35, UI_FONT_TEXT, g_fields[i].label, COL_TEXT);
        ui_fit(UI_FONT_TEXT, len ? value : "-", 1164 - label_end - 40, shown, sizeof(shown));
        ui_text(fb, W, H, 1164 - ui_text_width(UI_FONT_TEXT, shown), y + 35, UI_FONT_TEXT, shown, COL_TEXT);
    }

    for (int i = F.is_new ? A_TEST : A_DELETE; i <= A_SAVE; i++) {
        const char *label = g_buttons[i - A_DELETE].label;
        int x = g_buttons[i - A_DELETE].x, w = g_buttons[i - A_DELETE].w;

        if (i == A_SAVE)
            osd_fill(fb, W, H, x, BUTTON_Y, w, BUTTON_H, COL_SAVE_BG);
        else
            ui_dim(fb, W, H, x, BUTTON_Y, w, BUTTON_H, ROW_KEEP);
        if (i == F.sel)
            select_frame(fb, W, H, x, BUTTON_Y, w, BUTTON_H);
        ui_text(fb, W, H, x + (w - ui_text_width(UI_FONT_TEXT, label)) / 2, BUTTON_Y + 47, UI_FONT_TEXT,
                label, i == A_SAVE ? COL_SAVE : COL_TEXT);
    }

    int x = ui_hint(fb, W, H, MX - 1, UI_IMG_CROSS, F.sel < N_FIELDS ? "Edit" : "Select");
    x = ui_hint(fb, W, H, x, UI_IMG_CIRCLE, "Cancel");

    /* To the right of the button hints: what is going on, else help for the selected field. */
    const char *note = F.busy ? F.busy : F.sel < N_FIELDS ? g_fields[F.sel].hint : F.message;
    uint32_t colour = F.busy || F.sel < N_FIELDS ? COL_TEXT : F.message_colour;
    ui_fit(UI_FONT_TEXT, note, W - MX - x, shown, sizeof(shown));
    ui_text(fb, W, H, W - MX - ui_text_width(UI_FONT_TEXT, shown), 644, UI_FONT_TEXT, shown, colour);
}

static void draw(void)
{
    paint(plat_backbuffer(), plat_width(), plat_height());
    plat_flip();
}

/* A question in a box over the darkened form, with OK and Cancel under it.
 * Cancel is selected at first, so a stray press does no harm. Returns 1 for OK. */
static int confirm(const char *question, const char *detail)
{
    enum { BOX_W = 620, BOX_H = 250, BTN_W = 175, BTN_H = 54, GAP = 28 };
    unsigned prev = plat_pad();
    int ok = 0;
    uint32_t *still = NULL;   /* the dimmed form, painted once: the backdrop stops moving */
    int still_w = 0, still_h = 0;

    for (;;) {
        uint32_t *fb = plat_backbuffer();
        int W = plat_width(), H = plat_height();
        int bx = (W - BOX_W) / 2, by = (H - BOX_H) / 2;
        int ok_x = W / 2 - GAP / 2 - BTN_W, cancel_x = W / 2 + GAP / 2, btn_y = by + BOX_H - BTN_H - 36;
        char shown[300];

        if (still && still_w == W && still_h == H) {
            memcpy(fb, still, (size_t)W * H * sizeof(*fb));
        } else {
            paint(fb, W, H);
            ui_dim(fb, W, H, 0, 0, W, H, 110);
            free(still);
            still = malloc((size_t)W * H * sizeof(*fb));
            if (still) {
                memcpy(still, fb, (size_t)W * H * sizeof(*fb));
                still_w = W;
                still_h = H;
            }
        }
        osd_fill(fb, W, H, bx, by, BOX_W, BOX_H, COL_BOX);
        ui_frame(fb, W, H, bx, by, BOX_W, BOX_H, 1, COL_TEXT);
        ui_text(fb, W, H, (W - ui_text_width(UI_FONT_TEXT, question)) / 2, by + 62, UI_FONT_TEXT, question, COL_TEXT);
        ui_fit(UI_FONT_TEXT, detail, BOX_W - 80, shown, sizeof(shown));
        ui_text(fb, W, H, (W - ui_text_width(UI_FONT_TEXT, shown)) / 2, by + 104, UI_FONT_TEXT, shown, COL_TEXT);

        osd_fill(fb, W, H, ok_x, btn_y, BTN_W, BTN_H, COL_SAVE_BG);
        ui_dim(fb, W, H, cancel_x, btn_y, BTN_W, BTN_H, 110);
        select_frame(fb, W, H, ok ? ok_x : cancel_x, btn_y, BTN_W, BTN_H);
        ui_text(fb, W, H, ok_x + (BTN_W - ui_text_width(UI_FONT_TEXT, "OK")) / 2, btn_y + 35, UI_FONT_TEXT, "OK", COL_BOX);
        ui_text(fb, W, H, cancel_x + (BTN_W - ui_text_width(UI_FONT_TEXT, "Cancel")) / 2, btn_y + 35, UI_FONT_TEXT,
                "Cancel", COL_TEXT);
        plat_flip();

        unsigned pad = plat_pad();
        unsigned pressed = pad & ~prev;
        prev = pad;
        if (pressed & PAD_LEFT)   ok = 1;
        if (pressed & PAD_RIGHT)  ok = 0;
        if (pressed & PAD_CIRCLE) ok = 0;
        if (pressed & (PAD_CIRCLE | PAD_CROSS)) {
            free(still);
            return ok;
        }
    }
}

static void say(uint32_t colour, const char *text)
{
    snprintf(F.message, sizeof(F.message), "%s", text);
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
        const unsigned moves = PAD_UP | PAD_DOWN | PAD_LEFT | PAD_RIGHT;

        if (pressed & moves) {
            repeat_at = now + 350000;
        } else if ((pad & moves) && now >= repeat_at) {
            pressed |= pad & moves;
            repeat_at = now + 120000;
        }
        prev = pad;

        /* The fields are a column; the buttons under them are one row. Down from the
         * last field lands on Save, and the list wraps around at both ends. */
        int first = F.is_new ? A_TEST : A_DELETE;
        if (F.sel < N_FIELDS) {
            if (pressed & PAD_UP)   F.sel = F.sel > 0 ? F.sel - 1 : A_SAVE;
            if (pressed & PAD_DOWN) F.sel = F.sel < N_FIELDS - 1 ? F.sel + 1 : A_SAVE;
        } else {
            if (pressed & PAD_UP)                           F.sel = N_FIELDS - 1;
            else if (pressed & PAD_DOWN)                    F.sel = 0;
            else if ((pressed & PAD_LEFT) && F.sel > first) F.sel--;
            else if ((pressed & PAD_RIGHT) && F.sel < A_SAVE) F.sel++;
        }

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
            char where[400];
            snprintf(where, sizeof(where), "//%s/%s", F.parts.host, F.parts.share);
            if (confirm("Delete this share?", where))
                return FORM_DELETE;
            prev = plat_pad();
        }
    }
}
