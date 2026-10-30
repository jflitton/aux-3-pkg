/*
 * Installing, removing and verifying packages.
 *
 * Installing reads the package once, as it's decompressed. Each file is written to a
 * temporary name in its own directory (.pkg-PID-N) and only renamed into place once the
 * whole package has been read, its checksum matched and no file found in the way; until
 * then nothing visible changes, and on any failure the temporaries, and directories made
 * for them, are removed again. A file in the way is one that exists and doesn't belong to
 * an older version of the same package: docs/decisions.md says why there's no --force.
 */
#include "pkg.h"
#include "gunzip.h"
#include "tar.h"

#define METAMAX	65536

struct pending {
	char *tmp, *target;
};

struct inst {
	const char *file, *server, *expect;
	struct record new, old;
	int have_old, got_meta, same;
	char *meta;
	size_t metalen;
	struct entry cur;	/* the member being read */
	long mtime;
	char *tmp, *target;
	int fd;
	sha256_ctx sha;
	struct pending *pend;
	int npend, cappend;
	char **made;		/* directories made, in order */
	int nmade, capmade;
	int counter, root_user;
	char err[600];
};

static void
fail(struct inst *s, const char *fmt, ...)
{
	va_list ap;

	if (s->err[0])
		return;
	va_start(ap, fmt);
	vsnprintf(s->err, sizeof(s->err), fmt, ap);
	va_end(ap);
}

static void
made(const char *path, void *arg)
{
	struct inst *s = arg;
	struct entry e;

	if (strlen(path) <= strlen(root))
		return;		/* root itself */
	if (s->nmade == s->capmade) {
		s->capmade = s->capmade ? 2 * s->capmade : 16;
		s->made = xrealloc(s->made, s->capmade * sizeof(char *));
	}
	s->made[s->nmade++] = xstrdup(path);
	memset(&e, 0, sizeof(e));
	e.type = 'd';
	e.path = (char *)path + strlen(root) + 1;
	addentry(&s->new, &e);
}

/* make path's parent directories, recording those made */
static int
parents(struct inst *s, const char *target)
{
	char *dir = xstrdup(target), *slash = strrchr(dir, '/');
	int r = 0;

	if (slash && slash > dir) {
		*slash = 0;
		if (mkdirs(dir, 0755, made, s) < 0) {
			fail(s, "can't make %s: %s", dir, strerror(errno));
			r = -1;
		}
	}
	free(dir);
	return r;
}

/* fail with a description of what's at target, if anything is */
static int
in_the_way(struct inst *s, const char *path, const char *target)
{
	struct stat st;
	struct entry *mine;
	char *who;

	if (lstat(target, &st) < 0)
		return 0;
	mine = s->have_old ? findentry(&s->old, path) : NULL;
	if (mine && mine->type != 'd' && !S_ISDIR(st.st_mode))
		return 0;
	if (S_ISDIR(st.st_mode))
		fail(s, "/%s is already there, as a directory", path);
	else if ((who = owner(path, 0))) {
		fail(s, "/%s is already there, from package %s", path, who);
		free(who);
	} else
		fail(s, "/%s is already there, and no package installed it", path);
	return -1;
}

static int
begin(void *arg, struct tarentry *e)
{
	struct inst *s = arg;
	const char *why;
	struct stat st;

	if (!s->got_meta) {
		if (strcmp(e->name, "+PKG") || e->type != '0' || e->size > METAMAX) {
			fail(s, "it doesn't start with +PKG: not a package");
			return -1;
		}
		s->meta = xmalloc(e->size + 1);
		s->metalen = 0;
		return 0;
	}
	memset(&s->cur, 0, sizeof(s->cur));
	if ((why = validpath(e->name)) || (e->name[0] == '+' && (why = "reserved name"))) {
		fail(s, "it has a file that isn't allowed (%s: %s)", e->name, why);
		return -1;
	}
	if (findentry(&s->new, e->name) && e->type != '5') {
		fail(s, "%s is in it twice", e->name);
		return -1;
	}
	s->cur.path = e->name;
	s->cur.mode = e->mode & 07777;
	s->cur.uid = e->uid;
	s->cur.gid = e->gid;
	s->cur.size = e->size;
	s->mtime = e->mtime;
	s->target = xsprintf("%s/%s", root, e->name);

	switch (e->type) {
	case '5':
		s->cur.type = 'd';
		if (lstat(s->target, &st) == 0) {
			if (!S_ISDIR(st.st_mode))
				return in_the_way(s, e->name, s->target);
			/* there already: ours only if the old version made it */
			if (s->have_old && findentry(&s->old, e->name) &&
			    !findentry(&s->new, e->name))
				addentry(&s->new, &s->cur);
		} else if (mkdirs(s->target, 0755, made, s) < 0) {
			fail(s, "can't make %s: %s", s->target, strerror(errno));
			return -1;
		} else {
			chmod(s->target, s->cur.mode);
			if (s->root_user)
				chown(s->target, s->cur.uid, s->cur.gid);
		}
		return 0;
	case '0':
	case '2':
		s->cur.type = e->type == '0' ? 'f' : 'l';
		if (parents(s, s->target) < 0 || in_the_way(s, e->name, s->target) < 0)
			return -1;
		s->tmp = xstrdup(s->target);
		*strrchr(s->tmp, '/') = 0;
		{
			char *t = xsprintf("%s/.pkg-%d-%d", s->tmp, (int)getpid(), s->counter++);

			free(s->tmp);
			s->tmp = t;
		}
		if (s->cur.type == 'l') {
			if (!e->linkname[0]) {
				fail(s, "%s is a link to nothing", e->name);
				return -1;
			}
			s->cur.target = e->linkname;
			if (symlink(e->linkname, s->tmp) < 0) {
				fail(s, "can't make %s: %s", s->tmp, strerror(errno));
				return -1;
			}
		} else {
			s->fd = open(s->tmp, O_WRONLY | O_CREAT | O_EXCL, 0600);
			if (s->fd < 0) {
				fail(s, "can't make %s: %s", s->tmp, strerror(errno));
				return -1;
			}
			sha256_init(&s->sha);
		}
		if (s->npend == s->cappend) {
			s->cappend = s->cappend ? 2 * s->cappend : 64;
			s->pend = xrealloc(s->pend, s->cappend * sizeof(*s->pend));
		}
		s->pend[s->npend].tmp = s->tmp;
		s->pend[s->npend].target = s->target;
		s->npend++;
		s->tmp = s->target = NULL;
		return 0;
	default:
		fail(s, "%s is a kind of file pkg can't install (type %c)", e->name, e->type);
		return -1;
	}
}

static int
data(void *arg, const unsigned char *p, size_t n)
{
	struct inst *s = arg;

	if (!s->got_meta) {
		memcpy(s->meta + s->metalen, p, n);
		s->metalen += n;
		return 0;
	}
	sha256_update(&s->sha, p, n);
	while (n > 0) {
		ssize_t w = write(s->fd, p, n);

		if (w <= 0) {
			fail(s, "can't write %s: %s", s->pend[s->npend - 1].target,
				w < 0 ? strerror(errno) : "disk full?");
			return -1;
		}
		p += w, n -= w;
	}
	return 0;
}

static int
end(void *arg)
{
	struct inst *s = arg;
	struct pending *pd;
	const char *why;

	if (!s->got_meta) {
		s->meta[s->metalen] = 0;
		s->got_meta = 1;
		if (parsemeta(s->meta, &s->new.m, &why) < 0) {
			fail(s, "its +PKG is no good: %s", why);
			return -1;
		}
		if (s->expect && strcmp(s->new.m.name, s->expect)) {
			fail(s, "it's package %s, not %s", s->new.m.name, s->expect);
			return -1;
		}
		if (readrecord(s->new.m.name, &s->old) == 0) {
			s->have_old = 1;
			/* the same version from somewhere else replaces it */
			if (!strcmp(s->old.m.version, s->new.m.version) &&
			    (s->old.server && s->server ? !strcmp(s->old.server, s->server) :
			    s->old.server == s->server)) {
				s->same = 1;
				return 1;
			}
		}
		return 0;
	}
	free(s->target);
	s->target = NULL;
	if (s->cur.type == 'd')
		return 0;
	pd = &s->pend[s->npend - 1];
	if (s->cur.type == 'f') {
		unsigned char d[32];
		struct utimbuf ut;

		if (close(s->fd) < 0) {
			s->fd = -1;
			fail(s, "can't write %s: %s", pd->target, strerror(errno));
			return -1;
		}
		s->fd = -1;
		sha256_final(&s->sha, d);
		hex(d, 32, s->cur.sha256);
		chmod(pd->tmp, s->cur.mode);
		if (s->root_user)
			chown(pd->tmp, s->cur.uid, s->cur.gid);
		ut.actime = ut.modtime = s->mtime;
		utime(pd->tmp, &ut);
	}
	addentry(&s->new, &s->cur);
	return 0;
}

/* longest first, so a directory goes after what's in it */
static void
rmdirs(char **dirs, int n)
{
	int i, j;

	for (i = 1; i < n; i++)
		for (j = i; j > 0 && strlen(dirs[j - 1]) < strlen(dirs[j]); j--) {
			char *t = dirs[j];

			dirs[j] = dirs[j - 1];
			dirs[j - 1] = t;
		}
	for (i = 0; i < n; i++)
		rmdir(dirs[i]);
}

static void
undo(struct inst *s)
{
	int i;

	if (s->fd >= 0)
		close(s->fd);
	for (i = 0; i < s->npend; i++)
		unlink(s->pend[i].tmp);
	rmdirs(s->made, s->nmade);
}

/* What an upgrade leaves behind: the old version's files that the new one doesn't
   have. */
static void
remove_leftovers(struct inst *s)
{
	char **dirs = xmalloc(s->old.n * sizeof(char *) + 1);
	int i, nd = 0;

	for (i = 0; i < s->old.n; i++) {
		struct entry *e = &s->old.e[i];
		char *p;

		if (findentry(&s->new, e->path))
			continue;
		p = xsprintf("%s/%s", root, e->path);
		if (e->type == 'd')
			dirs[nd++] = p;
		else {
			if (unlink(p) < 0 && errno != ENOENT)
				warn("can't remove %s: %s", p, strerror(errno));
			free(p);
		}
	}
	rmdirs(dirs, nd);
	for (i = 0; i < nd; i++)
		free(dirs[i]);
	free(dirs);
}

int
install_file(const char *file, const char *from, const char *server, const char *expect)
{
	static struct inst st;
	struct inst *s = &st;
	struct tar t;
	const char *gzerr = NULL;
	int fd, r, i, ok;

	memset(s, 0, sizeof(*s));
	s->file = file;
	s->server = server;
	s->expect = expect;
	s->fd = -1;
	s->root_user = getuid() == 0;
	memset(&t, 0, sizeof(t));
	t.begin = begin;
	t.data = data;
	t.end = end;
	t.arg = s;

	if ((fd = open(file, O_RDONLY)) < 0) {
		warn("can't open %s: %s", file, strerror(errno));
		return -1;
	}
	r = gunzip(fd, tar_feed, &t, &gzerr);
	close(fd);
	if (r == 0 && tar_finish(&t) < 0)
		r = -1;
	if (r == 0 && !s->got_meta)
		fail(s, "it's empty: not a package");
	if (r == -1 && !s->err[0])
		fail(s, "%s", t.err ? t.err : gzerr ? gzerr : "unreadable");

	ok = !s->err[0] && (r == 0 || s->same);
	if (!ok || s->same) {
		undo(s);
		if (s->same)
			say("%s %s is already installed\n", s->new.m.name, s->new.m.version);
		else
			warn("can't install %s: %s", file, s->err);
	} else {
		/* everything's read and checked: put it in place */
		for (i = 0; i < s->npend; i++)
			if (rename(s->pend[i].tmp, s->pend[i].target) < 0) {
				warn("can't put %s in place: %s; %s is only partly installed",
					s->pend[i].target, strerror(errno), s->new.m.name);
				for (; i < s->npend; i++)
					unlink(s->pend[i].tmp);
				ok = 0;
				break;
			}
		if (s->have_old)
			remove_leftovers(s);
		s->new.installed = time(NULL);
		s->new.from = xstrdup(from);
		s->new.server = server ? xstrdup(server) : NULL;
		if (writerecord(&s->new) < 0)
			ok = 0;
		if (ok && s->have_old) {
			int c = vercmp(s->new.m.version, s->old.m.version);

			if (c == 0)
				say("Reinstalled %s %s\n", s->new.m.name, s->new.m.version);
			else
				say("%s %s from %s to %s\n", c > 0 ? "Upgraded" : "Downgraded",
					s->new.m.name, s->old.m.version, s->new.m.version);
		} else if (ok)
			say("Installed %s %s\n", s->new.m.name, s->new.m.version);
	}
	for (i = 0; i < s->npend; i++) {
		free(s->pend[i].tmp);
		free(s->pend[i].target);
	}
	free(s->pend);
	for (i = 0; i < s->nmade; i++)
		free(s->made[i]);
	free(s->made);
	free(s->tmp);
	free(s->target);
	free(s->meta);
	freerecord(&s->new);
	if (s->have_old)
		freerecord(&s->old);
	return ok ? 0 : -1;
}

struct peek {
	struct meta *m;
	char *buf;
	size_t len;
	const char *err;
	int got;
};

static int
peek_begin(void *arg, struct tarentry *e)
{
	struct peek *p = arg;

	if (strcmp(e->name, "+PKG") || e->type != '0' || e->size > METAMAX) {
		p->err = "it doesn't start with +PKG: not a package";
		return -1;
	}
	p->buf = xmalloc(e->size + 1);
	return 0;
}

static int
peek_data(void *arg, const unsigned char *d, size_t n)
{
	struct peek *p = arg;

	memcpy(p->buf + p->len, d, n);
	p->len += n;
	return 0;
}

static int
peek_end(void *arg)
{
	struct peek *p = arg;

	p->buf[p->len] = 0;
	if (parsemeta(p->buf, p->m, &p->err) < 0)
		return -1;
	p->got = 1;
	return 1;		/* that's all we want */
}

int
package_meta(const char *file, struct meta *m)
{
	struct peek p;
	struct tar t;
	const char *gzerr = NULL;
	int fd;

	memset(&p, 0, sizeof(p));
	memset(&t, 0, sizeof(t));
	p.m = m;
	t.begin = peek_begin;
	t.data = peek_data;
	t.end = peek_end;
	t.arg = &p;
	if ((fd = open(file, O_RDONLY)) < 0) {
		warn("can't open %s: %s", file, strerror(errno));
		return -1;
	}
	gunzip(fd, tar_feed, &t, &gzerr);
	close(fd);
	free(p.buf);
	if (!p.got) {
		warn("%s: %s", file, p.err ? p.err : t.err ? t.err : gzerr ? gzerr :
			"not a package");
		return -1;
	}
	return 0;
}

int
remove_package(const char *name)
{
	struct record r;
	char **dirs, *path;
	int i, nd = 0, ret = 0;

	if (readrecord(name, &r) < 0) {
		warn("%s isn't installed", name);
		return -1;
	}
	dirs = xmalloc(r.n * sizeof(char *) + 1);
	for (i = 0; i < r.n; i++) {
		struct entry *e = &r.e[i];
		char *p = xsprintf("%s/%s", root, e->path);

		if (e->type == 'd') {
			dirs[nd++] = p;
			continue;
		}
		if (unlink(p) < 0 && errno != ENOENT) {
			warn("can't remove %s: %s", p, strerror(errno));
			ret = -1;
		}
		free(p);
	}
	rmdirs(dirs, nd);
	for (i = 0; i < nd; i++)
		free(dirs[i]);
	free(dirs);
	if (ret == 0) {
		path = xsprintf("%s%s/installed/%s", root, PKGDIR, name);
		unlink(path);
		free(path);
		say("Removed %s %s\n", r.m.name, r.m.version);
	} else
		warn("%s is only partly removed; its record is kept", name);
	freerecord(&r);
	return ret;
}

int
verify_package(const char *name)
{
	struct record r;
	int i, bad = 0;

	if (readrecord(name, &r) < 0) {
		warn("%s isn't installed", name);
		return -1;
	}
	for (i = 0; i < r.n; i++) {
		struct entry *e = &r.e[i];
		char *p = xsprintf("%s/%s", root, e->path), link[256], sum[65];
		const char *problem = NULL;
		struct stat st;
		ssize_t n;

		if (lstat(p, &st) < 0)
			problem = "missing";
		else if (e->type == 'd' && !S_ISDIR(st.st_mode))
			problem = "not a directory";
		else if (e->type == 'l' && (!S_ISLNK(st.st_mode) ||
		    (n = readlink(p, link, sizeof(link) - 1)) < 0 ||
		    (link[n] = 0, strcmp(link, e->target))))
			problem = "not the link it was";
		else if (e->type == 'f' && !S_ISREG(st.st_mode))
			problem = "not a file";
		else if (e->type == 'f' && (st.st_size != e->size || hashfile(p, sum) < 0 ||
		    strcmp(sum, e->sha256)))
			problem = "changed";
		else if (e->type == 'f' && (st.st_mode & 07777) != e->mode)
			problem = "permissions changed";
		if (problem) {
			say("%s: /%s: %s\n", name, e->path, problem);
			bad++;
		}
		free(p);
	}
	freerecord(&r);
	return bad ? -1 : 0;
}
