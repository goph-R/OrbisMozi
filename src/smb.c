#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include <smb2/smb2.h>
#include <smb2/libsmb2.h>

#include "smb.h"

#define SMB_TIMEOUT 15              /* seconds before a silent server counts as gone */

static int parse_url(const char *url, SmbParts *u);

int smb_is_url(const char *location)
{
    return strncasecmp(location, "smb://", 6) == 0;
}

static void copy_part(char *dst, size_t dst_size, const char *src, size_t len)
{
    if (len >= dst_size)
        len = dst_size - 1;
    memcpy(dst, src, len);
    dst[len] = '\0';
}

/* Undoes %XX escapes in place (used for user name and password). */
static void unescape(char *text)
{
    char *out = text;

    for (const char *p = text; *p; p++) {
        unsigned v;
        if (p[0] == '%' && p[1] && p[2] && sscanf(p + 1, "%2x", &v) == 1) {
            *out++ = (char)v;
            p += 2;
        } else {
            *out++ = *p;
        }
    }
    *out = '\0';
}

/* Appends text to out, escaping the characters that structure an address. */
static void append_escaped(char *out, size_t out_size, const char *text)
{
    size_t n = strlen(out);

    for (; *text && n + 4 < out_size; text++) {
        if (strchr("%/@:", *text) || (unsigned char)*text <= ' ')
            n += (size_t)snprintf(out + n, out_size - n, "%%%02X", (unsigned char)*text);
        else
            out[n++] = *text;
    }
    out[n] = '\0';
}

int smb_join(const SmbParts *u, char *out, size_t out_size)
{
    if (!u->host[0] || !u->share[0] || out_size < 16)
        return -1;
    snprintf(out, out_size, "smb://");
    if (u->user[0]) {
        append_escaped(out, out_size, u->user);
        if (u->password[0]) {
            strncat(out, ":", out_size - strlen(out) - 1);
            append_escaped(out, out_size, u->password);
        }
        strncat(out, "@", out_size - strlen(out) - 1);
    }
    size_t n = strlen(out);
    const char *path = u->path;
    while (*path == '/')
        path++;
    snprintf(out + n, out_size - n, "%s/%s%s%s", u->host, u->share, *path ? "/" : "", path);
    n = strlen(out);
    while (n > 0 && out[n - 1] == '/')
        out[--n] = '\0';
    return 0;
}

int smb_split(const char *url, SmbParts *u)
{
    return parse_url(url, u);
}

static int parse_url(const char *url, SmbParts *u)
{
    memset(u, 0, sizeof(*u));
    if (!smb_is_url(url))
        return -1;

    const char *p = url + 6;
    const char *slash = strchr(p, '/');
    const char *auth_end = slash ? slash : p + strlen(p);

    /* user[:password]@ - the last '@' before the first '/' ends it,
     * so a password may contain '@' and ':'. */
    const char *at = NULL;
    for (const char *c = p; c < auth_end; c++)
        if (*c == '@')
            at = c;
    if (at) {
        const char *colon = memchr(p, ':', (size_t)(at - p));
        if (colon) {
            copy_part(u->user, sizeof(u->user), p, (size_t)(colon - p));
            copy_part(u->password, sizeof(u->password), colon + 1, (size_t)(at - colon - 1));
        } else {
            copy_part(u->user, sizeof(u->user), p, (size_t)(at - p));
        }
        unescape(u->user);
        unescape(u->password);
        p = at + 1;
    }

    copy_part(u->host, sizeof(u->host), p, (size_t)(auth_end - p));
    if (!u->host[0] || !slash)
        return -1;

    p = slash + 1;
    slash = strchr(p, '/');
    copy_part(u->share, sizeof(u->share), p, slash ? (size_t)(slash - p) : strlen(p));
    if (!u->share[0])
        return -1;
    if (slash) {
        while (*slash == '/')
            slash++;
        snprintf(u->path, sizeof(u->path), "%s", slash);
        size_t n = strlen(u->path);
        while (n > 0 && u->path[n - 1] == '/')
            u->path[--n] = '\0';
    }
    return 0;
}

void smb_display_name(const char *location, char *out, size_t out_size)
{
    SmbParts u;

    if (parse_url(location, &u) != 0) {
        snprintf(out, out_size, "%s", location);
        return;
    }
    snprintf(out, out_size, "smb://%s/%s%s%s", u.host, u.share, u.path[0] ? "/" : "", u.path);
}

static struct smb2_context *connect_share(const SmbParts *u, char *err, size_t err_size)
{
    struct smb2_context *smb2 = smb2_init_context();

    if (!smb2) {
        snprintf(err, err_size, "out of memory");
        return NULL;
    }
    smb2_set_timeout(smb2, SMB_TIMEOUT);
    if (u->user[0])
        smb2_set_user(smb2, u->user);
    if (u->password[0])
        smb2_set_password(smb2, u->password);

    int rc = smb2_connect_share(smb2, u->host, u->share, u->user[0] ? u->user : NULL);
    if (rc < 0) {
        snprintf(err, err_size, "cannot connect to //%s/%s: %s", u->host, u->share, smb2_get_error(smb2));
        smb2_destroy_context(smb2);
        return NULL;
    }
    return smb2;
}

int smb_list(const char *url, SmbEntry **entries, int *count, char *err, size_t err_size)
{
    SmbParts u;
    SmbEntry *list = NULL;
    int n = 0, cap = 0;

    *entries = NULL;
    *count = 0;
    if (parse_url(url, &u) != 0) {
        snprintf(err, err_size, "not a valid share address (smb://user:password@host/share)");
        return -1;
    }

    struct smb2_context *smb2 = connect_share(&u, err, err_size);
    if (!smb2)
        return -1;

    struct smb2dir *dir = smb2_opendir(smb2, u.path);
    if (!dir) {
        snprintf(err, err_size, "cannot open folder: %s", smb2_get_error(smb2));
        smb2_disconnect_share(smb2);
        smb2_destroy_context(smb2);
        return -1;
    }

    for (struct smb2dirent *e; (e = smb2_readdir(smb2, dir)) != NULL; ) {
        if (strcmp(e->name, ".") == 0 || strcmp(e->name, "..") == 0)
            continue;
        if (e->st.smb2_type != SMB2_TYPE_FILE && e->st.smb2_type != SMB2_TYPE_DIRECTORY)
            continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 256;
            list = realloc(list, (size_t)cap * sizeof(*list));
        }
        snprintf(list[n].name, sizeof(list[n].name), "%s", e->name);
        list[n].is_dir = e->st.smb2_type == SMB2_TYPE_DIRECTORY;
        n++;
    }

    smb2_closedir(smb2, dir);
    smb2_disconnect_share(smb2);
    smb2_destroy_context(smb2);
    *entries = list;
    *count = n;
    return 0;
}

struct SmbFile {
    struct smb2_context *smb2;
    struct smb2fh       *fh;
    int64_t              size;
    int                  max_read;
};

SmbFile *smb_open(const char *url, char *err, size_t err_size)
{
    SmbParts u;
    struct smb2_stat_64 st;

    if (parse_url(url, &u) != 0 || !u.path[0]) {
        snprintf(err, err_size, "not a valid file address on a share");
        return NULL;
    }

    struct smb2_context *smb2 = connect_share(&u, err, err_size);
    if (!smb2)
        return NULL;

    struct smb2fh *fh = smb2_open(smb2, u.path, O_RDONLY);
    if (!fh) {
        snprintf(err, err_size, "cannot open file: %s", smb2_get_error(smb2));
        smb2_disconnect_share(smb2);
        smb2_destroy_context(smb2);
        return NULL;
    }

    SmbFile *f = calloc(1, sizeof(*f));
    f->smb2 = smb2;
    f->fh = fh;
    f->size = smb2_fstat(smb2, fh, &st) == 0 ? (int64_t)st.smb2_size : -1;
    f->max_read = (int)smb2_get_max_read_size(smb2);
    if (f->max_read <= 0 || f->max_read > 1024 * 1024)
        f->max_read = 1024 * 1024;
    return f;
}

int smb_read(SmbFile *f, uint8_t *buf, int size)
{
    if (size > f->max_read)
        size = f->max_read;
    int n = smb2_read(f->smb2, f->fh, buf, (uint32_t)size);
    return n < 0 ? -1 : n;
}

int64_t smb_seek(SmbFile *f, int64_t offset, int whence)
{
    uint64_t pos = 0;

    if (smb2_lseek(f->smb2, f->fh, offset, whence, &pos) < 0)
        return -1;
    return (int64_t)pos;
}

int64_t smb_size(SmbFile *f)
{
    return f->size;
}

void smb_close(SmbFile *f)
{
    if (!f)
        return;
    smb2_close(f->smb2, f->fh);
    smb2_disconnect_share(f->smb2);
    smb2_destroy_context(f->smb2);
    free(f);
}
