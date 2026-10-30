/* Reading a ustar archive as it's decompressed: fed in pieces, it calls back per member */
#ifndef TAR_H
#define TAR_H

#include "sys.h"

struct tarentry {
	char name[257];		/* prefix/name (155 + 1 + 100), without a trailing / */
	char linkname[101];
	int type;		/* '0' file, '5' directory, '2' symlink, ... */
	mode_t mode;
	int uid, gid;
	long size, mtime;
};

struct tar {
	/* each returns nonzero to stop */
	int (*begin)(void *arg, struct tarentry *e);
	int (*data)(void *arg, const unsigned char *p, size_t n);
	int (*end)(void *arg);
	void *arg;
	const char *err;	/* why the archive is bad, when tar_feed returns -1 */

	unsigned char hdr[512];
	int hlen, zeros, done;
	long left, pad;
	struct tarentry e;
};

/* Zero t and set the callbacks first. Returns 0 to go on, -1 if the archive is bad, or
   what a callback returned to stop. Has the signature of a gz_sink. */
int tar_feed(void *t, const unsigned char *p, size_t n);
/* -1 (and t->err) if the archive stopped partway through a member */
int tar_finish(struct tar *t);

#endif
