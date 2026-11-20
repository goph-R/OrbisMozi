#ifndef BROWSER_H
#define BROWSER_H

#include <stddef.h>

/* Shows the file list until the user picks a video; its location (a local
 * path or an smb:// address) is written to out. Returns 1. The list keeps
 * its place between calls, so after a film the same folder is shown again. */
int browser_pick(char *out, size_t out_size);

#endif
