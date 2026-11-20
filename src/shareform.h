#ifndef SHAREFORM_H
#define SHAREFORM_H

#include <stddef.h>

enum { FORM_CANCEL, FORM_SAVE, FORM_DELETE };

/* The form for adding or changing a network share. url holds the current
 * address (empty for a new share) and receives the new one on FORM_SAVE. */
int shareform_run(char *url, size_t url_size, int is_new);

#endif
