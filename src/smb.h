/* Network shares (SMB2/SMB3, via libsmb2).
 *
 * A location is written as   smb://user:password@host/share/folder/file.mkv
 * User and password are optional (guest access without them). */
#ifndef SMB_H
#define SMB_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    char name[256];
    int  is_dir;
} SmbEntry;

int  smb_is_url(const char *location);

/* Copies the location without the password, for showing on screen. */
void smb_display_name(const char *location, char *out, size_t out_size);

/* Lists a folder. On success returns 0 and a malloc'ed array in *entries
 * ("." and ".." are left out). On failure returns -1 and a message in err. */
int  smb_list(const char *url, SmbEntry **entries, int *count, char *err, size_t err_size);

/* Reading one file. Each open file has its own connection, so it may be
 * used from a different thread than the one listing folders. */
typedef struct SmbFile SmbFile;

SmbFile *smb_open(const char *url, char *err, size_t err_size);
int      smb_read(SmbFile *f, uint8_t *buf, int size);      /* bytes read, 0 at the end, -1 on error */
int64_t  smb_seek(SmbFile *f, int64_t offset, int whence);  /* new position or -1 */
int64_t  smb_size(SmbFile *f);
void     smb_close(SmbFile *f);

#endif
