/* What pkg's parts share. The formats are in docs/formats.md. */
#ifndef PKG_H
#define PKG_H

#include "sys.h"
#include "sha.h"

#define PKG_VERSION	"0.3"
#define FORMAT		2	/* of packages and records this pkg reads */
#define INDEX_FORMAT	1	/* of package lists */

/* under root */
#define PKGDIR		"/usr/local/lib/pkg"
#define CONFFILE	"/etc/pkg.conf"

#define MAINSERVER	"main"	/* the built-in server's name, or pkg.conf's server line's */

extern const char *root;	/* -r: where to install, "" for / */

/* util.c */
void warn(const char *fmt, ...);	/* "pkg: ...", newline added, to stderr */
void say(const char *fmt, ...);		/* to stdout as given */
void *xmalloc(size_t n);		/* these exit if out of memory */
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xsprintf(const char *fmt, ...);
char *rootpath(const char *path);	/* root + path, path starting with / */
char *readfile(const char *path, size_t *len, size_t max);	/* NUL-terminated */
int writefile(const char *path, const char *data, size_t n);	/* via a temporary */
char *nextline(char **p);		/* splits a buffer into lines, in place */
int splittab(char *line, char **f, int max);	/* number of fields */
int mkdirs(const char *path, mode_t mode, void (*made)(const char *path, void *arg),
	void *arg);			/* mkdir -p; made() for each one created */
int validname(const char *s);
int validversion(const char *s);
const char *validpath(const char *s);	/* NULL, or why it isn't */
int vercmp(const char *a, const char *b);
int hashfile(const char *path, char out[65]);
int lock(void);
void unlock(void);

/* A package's description: from +PKG, a record, or the index. */
struct meta {
	int format;
	char *name, *version, *summary;
	char *extra;	/* the other "key value" lines, as they were */
};
int parsemeta(char *text, struct meta *m, const char **err);
void freemeta(struct meta *m);

/* What a package installed (docs/formats.md, "On the Mac") */
struct entry {
	char type;		/* 'f', 'l' or 'd' */
	char *path, *target;	/* target for 'l' */
	mode_t mode;
	int uid, gid;
	long size;
	char sha256[65];
};
struct record {
	struct meta m;
	long installed;
	char *from;	/* the URL or file it came from */
	char *server;	/* the server's name; NULL if it came from a file */
	struct entry *e;
	int n, cap;
	char *buf;	/* the strings point into this */
};
int readrecord(const char *name, struct record *r);	/* -1 if not installed */
int writerecord(struct record *r);
void freerecord(struct record *r);
void addentry(struct record *r, struct entry *e);
struct entry *findentry(struct record *r, const char *path);
int listinstalled(char ***names);	/* sorted; returns how many */
char *owner(const char *path, int dirs);	/* the installed package that has path (or
					   made it, if dirs and it's a directory), or NULL */

/* install.c */
int install_file(const char *file, const char *from, const char *server,
	const char *expect_name);
int remove_package(const char *name);	/* refuses while a shell of its is in use */
int verify_package(const char *name);
int package_meta(const char *file, struct meta *m);

/* repo.c */
struct server {
	char *name, *url;	/* url ends in / */
	int signed_;
	char *buf;		/* its list, which its packages' strings point into */
};
struct avail {
	struct meta m;
	long size;
	char sha256[65];
	char *file;
	struct server *s;	/* the server it's on */
};
struct index {
	struct server *s;	/* in the order they're searched: main first */
	int ns;
	struct avail *a;	/* every server's packages, server by server */
	int n;
};
int update(const char *server, int allow_unsigned);
int loadindex(struct index *ix);
struct server *findserver(struct index *ix, const char *name);
/* The highest version of name on s, or if s is NULL, on the first server that has it */
struct avail *findavail(struct index *ix, const char *name, struct server *s);
int fetch_package(struct avail *a, char **path);

#endif
