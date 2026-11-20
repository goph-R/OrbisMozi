/* A plain list to pick a video from: the console's own folder and the
 * network shares named in the shares file. */
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "browser.h"
#include "osd.h"
#include "plat.h"
#include "smb.h"

#define MAX_ROOTS   16
#define MAX_DEPTH   64
#define PATH_MAX_   1200

#define COL_BG      0xFF101428u
#define COL_TEXT    0xFFE8E8F0u
#define COL_DIM     0xFF8890A8u
#define COL_FOLDER  0xFFFFD27Fu
#define COL_SEL_BG  0xFF2F5FD0u
#define COL_ERROR   0xFFFF8080u

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

    DIR *d = opendir("/app0/assets/videos");        /* clips packed into the package */
    if (d) {
        for (struct dirent *e; (e = readdir(d)) != NULL; )
            if (is_video(e->d_name)) {
                snprintf(B.roots[B.n_roots++], PATH_MAX_, "/app0/assets/videos");
                break;
            }
        closedir(d);
    }

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
    return B.cur[0] ? B.n_items : B.n_roots;
}

/* --- drawing ------------------------------------------------------------- */

/* Copies at most max_chars characters of UTF-8 text, ending with "..." if cut. */
static void shorten(char *out, size_t out_size, const char *text, int max_chars)
{
    size_t o = 0;
    int chars = 0, total = 0;

    for (const char *p = text; *p; p++)
        if ((*p & 0xC0) != 0x80)
            total++;
    int keep = total > max_chars ? max_chars - 3 : total;

    while (*text && chars < keep && o + 5 < out_size) {
        do
            out[o++] = *text++;
        while ((*text & 0xC0) == 0x80 && o + 5 < out_size);
        chars++;
    }
    if (total > max_chars)
        for (int i = 0; i < 3; i++)
            out[o++] = '.';
    out[o] = '\0';
}

static void label_of(int i, char *out, size_t out_size, int *is_dir)
{
    if (B.cur[0]) {
        snprintf(out, out_size, "%s", B.items[i].name);
        *is_dir = B.items[i].is_dir;
        return;
    }
    *is_dir = 1;
    if (smb_is_url(B.roots[i])) {
        smb_display_name(B.roots[i], out, out_size);
    } else if (strcmp(B.roots[i], "/app0/assets/videos") == 0) {
        snprintf(out, out_size, "Test clips inside the package");
    } else {
        snprintf(out, out_size, "This console  (%s)", B.roots[i]);
    }
}

static void draw(const char *busy)
{
    uint32_t *fb = plat_backbuffer();
    int W = plat_width(), H = plat_height();
    int fw = osd_font_width(), fh = osd_font_height();
    int margin_x = W / 20, margin_y = H / 20;       /* TVs may crop the edges */
    int row_h = fh + 8;
    int list_y = margin_y + 3 * fh;
    int rows = (H - margin_y - 3 * fh - list_y) / row_h;
    int cols = (W - 2 * margin_x) / fw - 2;
    char text[1300], line[1300];
    int n = count();

    osd_fill(fb, W, H, 0, 0, W, H, COL_BG);

    osd_text(fb, W, H, margin_x, margin_y, "Orbis Mozi", COL_TEXT, COL_BG);
    if (B.cur[0]) {
        if (smb_is_url(B.cur))
            smb_display_name(B.cur, text, sizeof(text));
        else
            snprintf(text, sizeof(text), "%s", B.cur);
    } else {
        snprintf(text, sizeof(text), "Choose where to look for videos");
    }
    shorten(line, sizeof(line), text, cols);
    osd_text(fb, W, H, margin_x, margin_y + fh + 4, line, COL_DIM, COL_BG);

    /* Keep the selection inside the visible part of the list. */
    if (B.sel < B.top)
        B.top = B.sel;
    if (B.sel >= B.top + rows)
        B.top = B.sel - rows + 1;
    if (B.top < 0)
        B.top = 0;

    for (int i = B.top; i < n && i < B.top + rows; i++) {
        int y = list_y + (i - B.top) * row_h, is_dir;
        uint32_t bg = i == B.sel ? COL_SEL_BG : COL_BG;

        label_of(i, text, sizeof(text), &is_dir);
        if (i == B.sel)
            osd_fill(fb, W, H, margin_x, y, W - 2 * margin_x, row_h, bg);
        shorten(line, sizeof(line), text, cols - (is_dir && B.cur[0] ? 1 : 0));
        int x = osd_text(fb, W, H, margin_x + fw, y + 4, line,
                         i == B.sel ? 0xFFFFFFFFu : is_dir ? COL_FOLDER : COL_TEXT, bg);
        if (is_dir && B.cur[0])
            osd_text(fb, W, H, x, y + 4, "/", i == B.sel ? 0xFFFFFFFFu : COL_FOLDER, bg);
    }

    if (busy) {
        osd_text(fb, W, H, margin_x + fw, list_y + 4, busy, COL_DIM, COL_BG);
    } else if (B.message[0]) {
        shorten(line, sizeof(line), B.message, cols);
        osd_text(fb, W, H, margin_x + fw, list_y + 4, line, COL_ERROR, COL_BG);
    } else if (n == 0) {
        osd_text(fb, W, H, margin_x + fw, list_y + 4, "No videos or folders here.", COL_DIM, COL_BG);
    }

    int foot_y = H - margin_y - fh;
    if (n > 0)
        snprintf(text, sizeof(text), "%d / %d", B.sel + 1, n);
    else
        text[0] = '\0';
    osd_text(fb, W, H, W - margin_x - (int)strlen(text) * fw, foot_y, text, COL_DIM, COL_BG);
    osd_text(fb, W, H, margin_x, foot_y,
             B.cur[0] ? "X open    O back    L1/R1 page up/down" : "X open    L1/R1 page up/down",
             COL_DIM, COL_BG);
    if (!B.cur[0]) {
        snprintf(text, sizeof(text), "Network shares are read from %s", shares_file());
        osd_text(fb, W, H, margin_x, foot_y - fh - 4, text, COL_DIM, COL_BG);
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

        int n = count(), page = 15;
        if (pressed & PAD_UP)                 B.sel--;
        if (pressed & PAD_DOWN)               B.sel++;
        if (pressed & (PAD_L1 | PAD_LEFT))    B.sel -= page;
        if (pressed & (PAD_R1 | PAD_RIGHT))   B.sel += page;
        if (B.sel >= n) B.sel = n - 1;
        if (B.sel < 0)  B.sel = 0;

        if (pressed & PAD_CIRCLE) {
            go_up();
        } else if ((pressed & PAD_CROSS) && n > 0) {
            if (!B.cur[0]) {
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
