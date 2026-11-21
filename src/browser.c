/* The list to pick a video from: the console's own folder and the
 * network shares named in the shares file. */
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "browser.h"
#include "backdrop.h"
#include "osd.h"
#include "plat.h"
#include "shareform.h"
#include "smb.h"
#include "ui.h"

#define MAX_ROOTS   16
#define MAX_DEPTH   64
#define PATH_MAX_   1200

#define COL_TEXT    0xFFFFFFFFu
#define COL_ERROR   0xFFFF9C9Cu

typedef struct {
    char name[256];
    int  is_dir;
} Item;

static struct {
    char  roots[MAX_ROOTS][PATH_MAX_];  /* where each top-level entry leads */
    int   n_roots;

    char  cur[PATH_MAX_];               /* "" = the top-level list */
    int   root_len;                     /* length of the root that cur is inside */
    Item *items;
    int   n_items;
    int   sel, top;
    int   sel_stack[MAX_DEPTH], depth;  /* selection in each parent folder */
    char  message[200];                 /* error from the last listing */
    int   loaded;
} B;

static const char *local_root(void)
{
    const char *env = getenv("MOZI_LOCAL");
    return env ? env : "/data/video";
}

static const char *shares_file(void)
{
    const char *env = getenv("MOZI_SHARES");
    return env ? env : "/data/orbismozi/shares.txt";
}

static int is_video(const char *name)
{
    static const char *exts[] = { ".mkv", ".mp4", ".avi", ".m4v", ".mov", ".ts", ".m2ts", ".webm",
                                  ".mpg", ".mpeg", ".vob", ".wmv", ".flv", ".ogm", ".divx" };
    const char *dot = strrchr(name, '.');
    if (!dot)
        return 0;
    for (size_t i = 0; i < sizeof(exts) / sizeof(exts[0]); i++)
        if (strcasecmp(dot, exts[i]) == 0)
            return 1;
    return 0;
}

static int by_name(const void *a, const void *b)
{
    const Item *x = a, *y = b;
    if (x->is_dir != y->is_dir)
        return y->is_dir - x->is_dir;               /* folders first */
    return strcasecmp(x->name, y->name);
}

/* --- reading the lists --------------------------------------------------- */

static void read_roots(void)
{
    char line[PATH_MAX_];
    FILE *f;

    B.n_roots = 0;
    snprintf(B.roots[B.n_roots++], PATH_MAX_, "%s", local_root());

    f = fopen(shares_file(), "r");
    if (!f)
        return;
    while (B.n_roots < MAX_ROOTS && fgets(line, sizeof(line), f)) {
        char *p = line + strspn(line, " \t");
        p[strcspn(p, "\r\n")] = '\0';
        size_t n = strlen(p);
        while (n > 0 && (p[n - 1] == ' ' || p[n - 1] == '\t' || p[n - 1] == '/'))
            p[--n] = '\0';
        if (smb_is_url(p))
            snprintf(B.roots[B.n_roots++], PATH_MAX_, "%s", p);
    }
    fclose(f);
}

/* Writes the network shares of the top-level list back to the shares file. */
static void save_roots(void)
{
    FILE *f = fopen(shares_file(), "w");

    if (!f) {
        snprintf(B.message, sizeof(B.message), "cannot write %s: %s", shares_file(), strerror(errno));
        osd_log("%s", B.message);
        return;
    }
    fprintf(f, "# Orbis Mozi - network shares, one per line:\n"
               "#   smb://user:password@server/share/folder\n"
               "# In user and password, write %% / @ : as %%25 %%2F %%40 %%3A.\n");
    for (int i = 0; i < B.n_roots; i++)
        if (smb_is_url(B.roots[i]))
            fprintf(f, "%s\n", B.roots[i]);
    fclose(f);
}

/* Opens the form for the share at index i of the top-level list
 * (i == B.n_roots: a new one) and applies the result. */
static void edit_share(int i)
{
    char url[PATH_MAX_] = "";
    int is_new = i >= B.n_roots;

    if (is_new && B.n_roots >= MAX_ROOTS) {
        snprintf(B.message, sizeof(B.message), "The list is full (%d entries).", MAX_ROOTS);
        return;
    }
    if (!is_new)
        snprintf(url, sizeof(url), "%s", B.roots[i]);

    int result = shareform_run(url, sizeof(url), is_new);
    if (result == FORM_SAVE) {
        snprintf(B.roots[is_new ? B.n_roots++ : i], PATH_MAX_, "%s", url);
    } else if (result == FORM_DELETE) {
        for (int k = i; k < B.n_roots - 1; k++)
            memcpy(B.roots[k], B.roots[k + 1], PATH_MAX_);
        B.n_roots--;
    }
    if (result != FORM_CANCEL)
        save_roots();
}

static int list_local(const char *dir)
{
    DIR *d = opendir(dir);
    int cap = 0;

    if (!d) {
        snprintf(B.message, sizeof(B.message), "cannot open %s: %s", dir, strerror(errno));
        return -1;
    }
    for (struct dirent *e; (e = readdir(d)) != NULL; ) {
        char full[PATH_MAX_ + 300];
        struct stat st;

        if (e->d_name[0] == '.')
            continue;
        snprintf(full, sizeof(full), "%s/%s", dir, e->d_name);
        if (stat(full, &st) != 0)
            continue;
        int is_dir = S_ISDIR(st.st_mode);
        if (!is_dir && !is_video(e->d_name))
            continue;
        if (B.n_items == cap) {
            cap = cap ? cap * 2 : 256;
            B.items = realloc(B.items, (size_t)cap * sizeof(Item));
        }
        snprintf(B.items[B.n_items].name, sizeof(B.items[0].name), "%s", e->d_name);
        B.items[B.n_items].is_dir = is_dir;
        B.n_items++;
    }
    closedir(d);
    return 0;
}

static int list_share(const char *url)
{
    SmbEntry *entries;
    int count;

    if (smb_list(url, &entries, &count, B.message, sizeof(B.message)) != 0)
        return -1;
    B.items = malloc((size_t)(count ? count : 1) * sizeof(Item));
    for (int i = 0; i < count; i++) {
        if (entries[i].name[0] == '.' || (!entries[i].is_dir && !is_video(entries[i].name)))
            continue;
        snprintf(B.items[B.n_items].name, sizeof(B.items[0].name), "%s", entries[i].name);
        B.items[B.n_items].is_dir = entries[i].is_dir;
        B.n_items++;
    }
    free(entries);
    return 0;
}

static void draw(const char *busy);

/* Fills the list for B.cur. On failure the list is empty and B.message set. */
static void load(void)
{
    free(B.items);
    B.items = NULL;
    B.n_items = 0;
    B.message[0] = '\0';
    B.loaded = 1;

    if (!B.cur[0]) {
        read_roots();
        return;
    }
    if (smb_is_url(B.cur)) {
        draw("Connecting...");                      /* the network can take a moment */
        list_share(B.cur);
    } else {
        list_local(B.cur);
    }
    if (B.n_items > 1)
        qsort(B.items, (size_t)B.n_items, sizeof(Item), by_name);
    if (B.message[0])
        osd_log("%s", B.message);
}

static int count(void)
{
    return B.cur[0] ? B.n_items : B.n_roots + 1;    /* + "Add network share" */
}

/* --- drawing ------------------------------------------------------------- */

/* The layout follows the main menu mockup, which is drawn at 1920x1080;
 * the menu runs at 1280x720, so every measure here is 2/3 of the drawing. */
#define MX          64                  /* left and right margin */
#define ROOT_Y      269                 /* top-level list: big rows with a picture */
#define ROOT_H      81
#define ROOT_PITCH  90
#define ROOT_ROWS   3
#define LIST_Y      122                 /* inside a folder: plain rows, no logo above */
#define LIST_PITCH  36
#define LIST_ROWS   13
#define ROW_KEEP    174                 /* rows are black at 32 %: 174/256 of the light stays */

static int visible_rows(void)
{
    return B.cur[0] ? LIST_ROWS : ROOT_ROWS;
}

static void draw_root_row(uint32_t *fb, int W, int H, int i, int y)
{
    char path[PATH_MAX_], fitted[PATH_MAX_];
    const char *label;
    int image;

    path[0] = '\0';
    if (i >= B.n_roots) {
        label = "Add network share";
        image = UI_IMG_PLUS;
    } else if (smb_is_url(B.roots[i])) {
        label = "Local network";
        image = UI_IMG_SHARE;
        smb_display_name(B.roots[i], path, sizeof(path));
    } else {
        label = "This console";
        image = UI_IMG_DRIVE;
        snprintf(path, sizeof(path), "%s", B.roots[i]);
    }

    ui_dim(fb, W, H, MX, y, W - 2 * MX, ROOT_H, ROW_KEEP);
    if (i == B.sel)
        ui_frame(fb, W, H, MX - 8, y - 8, W - 2 * MX + 16, ROOT_H + 16, 5, COL_TEXT);
    ui_image(fb, W, H, 109, y + ROOT_H / 2, image);
    int x = ui_text(fb, W, H, 154, y + 50, UI_FONT_TEXT, label, COL_TEXT);

    int right = W - MX - 52;
    ui_fit(UI_FONT_TEXT, path, right - x - 40, fitted, sizeof(fitted));
    ui_text(fb, W, H, right - ui_text_width(UI_FONT_TEXT, fitted), y + 50, UI_FONT_TEXT, fitted, COL_TEXT);
}

static void draw(const char *busy)
{
    uint32_t *fb = plat_backbuffer();
    int W = plat_width(), H = plat_height();
    char text[1300], line[1300];
    int n = count(), rows = visible_rows();

    backdrop_draw(fb, W, H);

    /* What is said above the list: where we are, or what went wrong. */
    uint32_t colour = COL_TEXT;
    int is_place = 0;
    if (busy) {
        snprintf(text, sizeof(text), "%s", busy);
    } else if (B.message[0]) {
        snprintf(text, sizeof(text), "%s", B.message);
        colour = COL_ERROR;
    } else if (!B.cur[0]) {
        snprintf(text, sizeof(text), "Choose where to look for videos");
    } else if (smb_is_url(B.cur)) {
        smb_display_name(B.cur, text, sizeof(text));
        is_place = 1;
    } else {
        snprintf(text, sizeof(text), "%s", B.cur);
        is_place = 1;
    }

    if (!B.cur[0]) {
        /* Top-level list: logo, name, version, a thin line, then the text. */
        ui_image(fb, W, H, 115, 92, UI_IMG_LOGO);
        int x = ui_text(fb, W, H, 180, 107, UI_FONT_TITLE, "Orbis Mozi", COL_TEXT);
        ui_text(fb, W, H, x + 13, 107, UI_FONT_TEXT, "v" APP_VERSION, COL_TEXT);
        osd_fill(fb, W, H, MX, 158, W - 2 * MX, 1, COL_TEXT);
        ui_fit(UI_FONT_TEXT, text, W - 2 * MX, line, sizeof(line));
        ui_text(fb, W, H, MX, 220, UI_FONT_TEXT, line, colour);
    } else {
        /* Inside a folder the text is the heading, so the list gets the room.
         * A long location loses its beginning: the end says where we are. */
        if (is_place && ui_text_width(UI_FONT_TEXT, text) > W - 2 * MX) {
            const char *p = text;
            do {
                p++;
                while ((*p & 0xC0) == 0x80)
                    p++;
                snprintf(line, sizeof(line), "\xE2\x80\xA6%s", p);
            } while (*p && ui_text_width(UI_FONT_TEXT, line) > W - 2 * MX);
        } else {
            ui_fit(UI_FONT_TEXT, text, W - 2 * MX, line, sizeof(line));
        }
        ui_text(fb, W, H, MX, 84, UI_FONT_TEXT, line, colour);
        osd_fill(fb, W, H, MX, 104, W - 2 * MX, 1, COL_TEXT);
    }

    /* Keep the selection inside the visible part of the list. */
    if (B.sel < B.top)
        B.top = B.sel;
    if (B.sel >= B.top + rows)
        B.top = B.sel - rows + 1;
    if (B.top < 0)
        B.top = 0;

    if (!B.cur[0]) {
        for (int i = B.top; i < n && i < B.top + rows; i++)
            draw_root_row(fb, W, H, i, ROOT_Y + (i - B.top) * ROOT_PITCH);
    } else {
        ui_dim(fb, W, H, MX, LIST_Y, W - 2 * MX, LIST_ROWS * LIST_PITCH, ROW_KEEP);
        for (int i = B.top; i < n && i < B.top + rows; i++) {
            int y = LIST_Y + (i - B.top) * LIST_PITCH;

            snprintf(text, sizeof(text), "%s%s", B.items[i].name, B.items[i].is_dir ? "/" : "");
            ui_fit(UI_FONT_TEXT, text, W - 2 * MX - 40, line, sizeof(line));
            ui_text(fb, W, H, MX + 20, y + 26, UI_FONT_TEXT, line, COL_TEXT);
            if (i == B.sel)
                ui_frame(fb, W, H, MX, y, W - 2 * MX, LIST_PITCH, 3, COL_TEXT);
        }
        if (n == 0 && !busy && !B.message[0])
            ui_text(fb, W, H, MX + 20, LIST_Y + 26, UI_FONT_TEXT, "No videos or folders here.", COL_TEXT);
    }

    /* Bottom: what the buttons do, and the place in the list. */
    int x = ui_hint(fb, W, H, MX - 1, UI_IMG_CROSS, "Open");
    if (B.cur[0]) {
        ui_hint(fb, W, H, x, UI_IMG_CIRCLE, "Back");
        if (n > 0) {
            snprintf(text, sizeof(text), "%d / %d", B.sel + 1, n);
            ui_text(fb, W, H, W - MX - ui_text_width(UI_FONT_TEXT, text), 644, UI_FONT_TEXT, text, COL_TEXT);
        }
    } else {
        ui_hint(fb, W, H, x, UI_IMG_SQUARE, "Change / Delete a share");
    }

    plat_flip();
}

/* --- moving around ------------------------------------------------------- */

static void enter(const char *location, int is_root)
{
    if (B.depth < MAX_DEPTH)
        B.sel_stack[B.depth++] = B.sel;
    snprintf(B.cur, sizeof(B.cur), "%s", location);
    if (is_root)
        B.root_len = (int)strlen(B.cur);
    B.sel = B.top = 0;
    load();
}

static void go_up(void)
{
    if (!B.cur[0])
        return;
    char *slash = strrchr(B.cur, '/');
    if ((int)strlen(B.cur) <= B.root_len || !slash)
        B.cur[0] = '\0';                            /* back to the top-level list */
    else
        *slash = '\0';
    load();
    B.sel = B.depth > 0 ? B.sel_stack[--B.depth] : 0;
    if (B.sel >= count())
        B.sel = count() > 0 ? count() - 1 : 0;
    B.top = 0;
}

int browser_pick(char *out, size_t out_size)
{
    unsigned prev = plat_pad();                     /* ignore buttons still held */
    uint64_t repeat_at = 0;

    plat_set_mode(1280, 720);                       /* a film may have left 1080p behind */
    if (!B.loaded)
        load();

    for (;;) {
        draw(NULL);

        unsigned pad = plat_pad();
        unsigned pressed = pad & ~prev;
        uint64_t now = plat_time_us();
        const unsigned moves = PAD_UP | PAD_DOWN | PAD_L1 | PAD_R1 | PAD_LEFT | PAD_RIGHT;

        /* Holding a direction repeats it. */
        if (pressed & moves) {
            repeat_at = now + 350000;
        } else if ((pad & moves) && now >= repeat_at) {
            pressed |= pad & moves;
            repeat_at = now + 50000;
        }
        prev = pad;

        int n = count(), page = visible_rows();
        if (pressed & PAD_UP)                 B.sel--;
        if (pressed & PAD_DOWN)               B.sel++;
        if (pressed & (PAD_L1 | PAD_LEFT))    B.sel -= page;
        if (pressed & (PAD_R1 | PAD_RIGHT))   B.sel += page;
        if (B.sel >= n) B.sel = n - 1;
        if (B.sel < 0)  B.sel = 0;

        if (pressed & PAD_CIRCLE) {
            go_up();
        } else if ((pressed & PAD_SQUARE) && !B.cur[0] && B.sel < B.n_roots && smb_is_url(B.roots[B.sel])) {
            edit_share(B.sel);
            prev = plat_pad();
        } else if ((pressed & PAD_CROSS) && n > 0) {
            if (!B.cur[0] && B.sel >= B.n_roots) {
                edit_share(B.n_roots);
                prev = plat_pad();
            } else if (!B.cur[0]) {
                enter(B.roots[B.sel], 1);
            } else {
                char target[PATH_MAX_ + 300];
                snprintf(target, sizeof(target), "%s/%s", B.cur, B.items[B.sel].name);
                if (B.items[B.sel].is_dir) {
                    enter(target, 0);
                } else {
                    snprintf(out, out_size, "%s", target);
                    return 1;
                }
            }
        }
    }
}
