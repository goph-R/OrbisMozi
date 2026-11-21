#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "extsub.h"
#include "osd.h"
#include "smb.h"

#define MAX_FILE    (4 * 1024 * 1024)   /* a subtitle file is a few hundred kB at most */

/* --- reading the file ----------------------------------------------------- */

static char *read_local(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    char *data;

    if (!f)
        return NULL;
    data = malloc(MAX_FILE + 1);
    *size = data ? fread(data, 1, MAX_FILE, f) : 0;
    fclose(f);
    return data;
}

static char *read_share(const char *url, size_t *size)
{
    char err[200];
    SmbFile *f = smb_open(url, err, sizeof(err));
    char *data;
    int n;

    if (!f)
        return NULL;
    data = malloc(MAX_FILE + 1);
    *size = 0;
    while (data && *size < MAX_FILE &&
           (n = smb_read(f, (uint8_t *)data + *size, (int)(MAX_FILE - *size))) > 0)
        *size += (size_t)n;
    smb_close(f);
    return data;
}

/* The video's location with its extension replaced. Returns 0 on success. */
static int beside(const char *video, const char *ext, char *out, size_t out_size)
{
    const char *slash = strrchr(video, '/');
    const char *dot = strrchr(video, '.');
    size_t stem = dot && (!slash || dot > slash) ? (size_t)(dot - video) : strlen(video);

    if (stem + strlen(ext) + 1 > out_size)
        return -1;
    memcpy(out, video, stem);
    strcpy(out + stem, ext);
    return 0;
}

/* --- text encoding: everything becomes UTF-8 ------------------------------- */

static size_t put_utf8(char *out, unsigned cp)
{
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | cp >> 6);
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | cp >> 12);
        out[1] = (char)(0x80 | (cp >> 6 & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | cp >> 18);
    out[1] = (char)(0x80 | (cp >> 12 & 0x3F));
    out[2] = (char)(0x80 | (cp >> 6 & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

static int is_utf8(const unsigned char *s, size_t n)
{
    for (size_t i = 0; i < n;) {
        int more = s[i] < 0x80 ? 0 : (s[i] & 0xE0) == 0xC0 ? 1 : (s[i] & 0xF0) == 0xE0 ? 2
                 : (s[i] & 0xF8) == 0xF0 ? 3 : -1;
        if (more < 0 || i + more >= n + (more == 0))
            return 0;
        for (int k = 1; k <= more; k++)
            if ((s[i + k] & 0xC0) != 0x80)
                return 0;
        i += more + 1;
    }
    return 1;
}

/* Windows-1250 (Central European), the bytes 0x80..0xFF. */
static const uint16_t CP1250[128] = {
    0x20AC,0xFFFD,0x201A,0xFFFD,0x201E,0x2026,0x2020,0x2021,0xFFFD,0x2030,0x0160,0x2039,0x015A,0x0164,0x017D,0x0179,
    0xFFFD,0x2018,0x2019,0x201C,0x201D,0x2022,0x2013,0x2014,0xFFFD,0x2122,0x0161,0x203A,0x015B,0x0165,0x017E,0x017A,
    0x00A0,0x02C7,0x02D8,0x0141,0x00A4,0x0104,0x00A6,0x00A7,0x00A8,0x00A9,0x015E,0x00AB,0x00AC,0x00AD,0x00AE,0x017B,
    0x00B0,0x00B1,0x02DB,0x0142,0x00B4,0x00B5,0x00B6,0x00B7,0x00B8,0x0105,0x015F,0x00BB,0x013D,0x02DD,0x013E,0x017C,
    0x0154,0x00C1,0x00C2,0x0102,0x00C4,0x0139,0x0106,0x00C7,0x010C,0x00C9,0x0118,0x00CB,0x011A,0x00CD,0x00CE,0x010E,
    0x0110,0x0143,0x0147,0x00D3,0x00D4,0x0150,0x00D6,0x00D7,0x0158,0x016E,0x00DA,0x0170,0x00DC,0x00DD,0x0162,0x00DF,
    0x0155,0x00E1,0x00E2,0x0103,0x00E4,0x013A,0x0107,0x00E7,0x010D,0x00E9,0x0119,0x00EB,0x011B,0x00ED,0x00EE,0x010F,
    0x0111,0x0144,0x0148,0x00F3,0x00F4,0x0151,0x00F6,0x00F7,0x0159,0x016F,0x00FA,0x0171,0x00FC,0x00FD,0x0163,0x02D9,
};

/* Returns a new, zero-terminated UTF-8 copy of the file and says in *how
 * what it was taken for. */
static char *to_utf8(const char *raw, size_t n, const char **how)
{
    const unsigned char *s = (const unsigned char *)raw;
    char *out;
    size_t o = 0;

    if (n >= 2 && ((s[0] == 0xFF && s[1] == 0xFE) || (s[0] == 0xFE && s[1] == 0xFF))) {
        int le = s[0] == 0xFF;
        out = malloc(n * 2 + 1);
        for (size_t i = 2; out && i + 1 < n; i += 2) {
            unsigned cp = le ? s[i] | s[i + 1] << 8 : s[i] << 8 | s[i + 1];
            if (cp >= 0xD800 && cp < 0xDC00 && i + 3 < n) {         /* a pair makes one character */
                unsigned lo = le ? s[i + 2] | s[i + 3] << 8 : s[i + 2] << 8 | s[i + 3];
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                i += 2;
            }
            o += put_utf8(out + o, cp);
        }
        *how = "UTF-16";
    } else if (is_utf8(s, n)) {
        size_t skip = n >= 3 && s[0] == 0xEF && s[1] == 0xBB && s[2] == 0xBF ? 3 : 0;   /* byte order mark */
        out = malloc(n + 1);
        if (out)
            memcpy(out, raw + skip, o = n - skip);
        *how = "UTF-8";
    } else {
        out = malloc(n * 3 + 1);
        for (size_t i = 0; out && i < n; i++)
            o += put_utf8(out + o, s[i] < 0x80 ? s[i] : CP1250[s[i] - 0x80]);
        *how = "Windows-1250";
    }
    if (out)
        out[o] = '\0';
    return out;
}

/* --- the SRT format --------------------------------------------------------- */

/* "01:02:03,456" (also with a dot, also without hours). Returns the seconds
 * and moves *p behind the time, or returns -1. */
static double read_time(const char **p)
{
    const char *s = *p;
    int part[3] = { 0, 0, 0 }, n = 0;
    double frac = 0.0, scale = 0.1;

    while (*s == ' ' || *s == '\t')
        s++;
    if (*s < '0' || *s > '9')
        return -1.0;
    for (;;) {
        while (*s >= '0' && *s <= '9')
            part[n] = part[n] * 10 + (*s++ - '0');
        if (*s != ':' || n == 2)
            break;
        n++;
        s++;
    }
    if (*s == ',' || *s == '.')
        for (s++; *s >= '0' && *s <= '9'; s++, scale /= 10.0)
            frac += (*s - '0') * scale;
    *p = s;
    return (n == 2 ? part[0] * 3600.0 + part[1] * 60.0 + part[2]
          : n == 1 ? part[0] * 60.0 + part[1] : part[0]) + frac;
}

/* Copies one line of subtitle text, leaving out <i>-style and {\an8}-style marks. */
static void add_text(ExtCue *cue, const char *line, size_t len)
{
    size_t n = strlen(cue->text);

    if (n && n + 1 < sizeof(cue->text))
        cue->text[n++] = '\n';
    for (size_t i = 0; i < len && n + 1 < sizeof(cue->text); i++) {
        char close = line[i] == '<' ? '>' : line[i] == '{' ? '}' : 0;
        const char *end = close ? memchr(line + i, close, len - i) : NULL;
        if (end && end - (line + i) < 40) {
            i = (size_t)(end - line);
            continue;
        }
        cue->text[n++] = line[i];
    }
    while (n > 0 && (cue->text[n - 1] == ' ' || cue->text[n - 1] == '\n'))
        n--;
    cue->text[n] = '\0';
}

static int parse_srt(const char *text, ExtSub *sub)
{
    int cap = 0;
    ExtCue *cue = NULL;                 /* the one being filled, if any */

    for (const char *line = text; *line;) {
        const char *eol = line + strcspn(line, "\n");
        size_t len = (size_t)(eol - line);
        const char *arrow;

        while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == ' '))
            len--;

        if (len == 0) {
            cue = NULL;                 /* an empty line ends the text */
        } else if ((arrow = strstr(line, "-->")) != NULL && arrow < line + len) {
            const char *a = line, *b = arrow + 3;
            double start = read_time(&a), end = read_time(&b);

            cue = NULL;
            if (start >= 0.0 && end > start) {
                if (sub->count == cap) {
                    ExtCue *grown = realloc(sub->cues, (size_t)(cap = cap ? cap * 2 : 512) * sizeof(ExtCue));
                    if (!grown)
                        break;
                    sub->cues = grown;
                }
                cue = &sub->cues[sub->count++];
                memset(cue, 0, sizeof(*cue));
                cue->start = start;
                cue->end = end;
            }
        } else if (cue) {
            add_text(cue, line, len);
        }                               /* else: the running number before a time line */
        line = *eol ? eol + 1 : eol;
    }
    return sub->count;
}

int extsub_load(const char *video, ExtSub *sub)
{
    static const char *const EXT[] = { ".srt", ".SRT" };
    char where[1500], shown[1024];
    char *raw = NULL, *text;
    const char *how = "";
    size_t size = 0;

    memset(sub, 0, sizeof(*sub));
    for (size_t i = 0; !raw && i < sizeof(EXT) / sizeof(EXT[0]); i++)
        if (beside(video, EXT[i], where, sizeof(where)) == 0)
            raw = smb_is_url(where) ? read_share(where, &size) : read_local(where, &size);
    if (!raw)
        return -1;

    text = to_utf8(raw, size, &how);
    free(raw);
    if (text)
        parse_srt(text, sub);
    free(text);

    smb_display_name(where, shown, sizeof(shown));
    osd_log("subtitle file %s: %d lines (%s)", shown, sub->count, how);
    if (sub->count == 0) {
        extsub_free(sub);
        return -1;
    }
    return 0;
}

void extsub_free(ExtSub *sub)
{
    free(sub->cues);
    sub->cues = NULL;
    sub->count = 0;
}
