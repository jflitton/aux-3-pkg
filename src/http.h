/* Fetching files over HTTP */
#ifndef HTTP_H
#define HTTP_H

#include "sha.h"

/* Fetches url into fd, adding what it writes to sha if that isn't NULL, and showing
   progress as label if stderr is a terminal. 0, or -1 having said why. *size gets the
   number of bytes written. */
int http_file(const char *url, int fd, sha256_ctx *sha, long *size, const char *label);

/* Fetches url into memory: a NUL-terminated buffer of at most max bytes, or NULL having
   said why (quietly if quiet_404 and the server hasn't got it). */
char *http_mem(const char *url, size_t *len, size_t max, int quiet_404);

#endif
