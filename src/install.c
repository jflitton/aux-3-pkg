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

/*
 * Shells: a package's "shell PATH" lines name its programs that are login shells.
 * Installing it adds them to /etc/shells, the list A/UX's chsh, ftpd and Login check;
 * removing it takes them out, and is refused while one is someone's login shell.
 */

/* m's shells, NULL-terminated */
static char **
shells(struct meta *m)
{
	char **v = xmalloc(sizeof(char *));
	const char *s = m->extra, *e;
	int n = 0;

	while (s && (e = strchr(s, '\n'))) {
		if (!strncmp(s, "shell ", 6)) {
			v = xrealloc(v, (n + 2) * sizeof(char *));
			v[n] = xmalloc(e - s - 5);
			memcpy(v[n], s + 6, e - s - 6);
			v[n++][e - s - 6] = 0;
		}
		s = e + 1;
	}
	v[n] = NULL;
	return v;
}

static int
listed(char **v, const char *s)
{
	for (; v && *v; v++)
		if (!strcmp(*v, s))
			return 1;
	return 0;
}

static void
freev(char **v)
{
	char **p;

	for (p = v; p && *p; p++)
		free(*p);
	free(v);
}

/* Whether shell is in /etc/shells (-1 if that can't be read), having added it (add > 0)
   or taken it out (add < 0) if need be. The file keeps its owner and mode. */
static int
etcshells(const char *shell, int add)
{
	char *path = rootpath("/etc/shells"), *text, *out, *p, *line;
	size_t len, n = 0, l;
	struct stat st;
	int was = 0;

	if (stat(path, &st) < 0 || !(text = readfile(path, &len, 1 << 20))) {
		if (add > 0)
			warn("can't add %s to /etc/shells: %s", shell, strerror(errno));
		else if (add < 0)
			warn("can't take %s out of /etc/shells: %s", shell, strerror(errno));
		free(path);
		return -1;
	}
	out = xmalloc(len + strlen(shell) + 2);
	p = text;
	while ((line = nextline(&p))) {
		if (!strcmp(line, shell)) {
			was = 1;
			if (add < 0)
				continue;
		}
		l = strlen(line);
		memcpy(out + n, line, l);
		out[n + l] = '\n';
		n += l + 1;
	}
	if ((add > 0 && !was) || (add < 0 && was)) {
		if (add > 0) {
			l = strlen(shell);
			memcpy(out + n, shell, l);
			out[n + l] = '\n';
			n += l + 1;
		}
		if (writefile(path, out, n) < 0)
			warn("can't change /etc/shells: %s", strerror(errno));
		else {
			chown(path, st.st_uid, st.st_gid);
			chmod(path, st.st_mode & 07777);
			say(add > 0 ? "Added %s to /etc/shells\n" : "Took %s out of /etc/shells\n",
				shell);
		}
	}
	free(text);
	free(out);
	free(path);
	return was;
}

/* The users in /etc/passwd whose login shell is shell, as "a, b", or NULL */
static char *
users_of(const char *shell)
{
	char *path = rootpath("/etc/passwd"), *text = readfile(path, NULL, 4 << 20);
	char *p = text, *line, *who = NULL;

	while (p && (line = nextline(&p))) {
		char *f[7], *c = line;
		int n = 0;

		while (n < 7) {
			f[n++] = c;
			if (!(c = strchr(c, ':')))
				break;
			*c++ = 0;
		}
		if (n == 7 && !strcmp(f[6], shell)) {
			char *old = who;

			who = old ? xsprintf("%s, %s", old, f[0]) : xstrdup(f[0]);
			free(old);
		}
	}
	free(text);
	free(path);
	return who;
}

/* Why old's shells that keep (if any) doesn't have too can't go, or NULL if they can */
static char *
shells_in_use(struct meta *old, struct meta *keep)
{
	char **sh = shells(old), **k = keep ? shells(keep) : NULL, *who, *why = NULL;
	int i;

	for (i = 0; sh[i] && !why; i++)
		if (!listed(k, sh[i]) && (who = users_of(sh[i]))) {
			why = xsprintf("%s is the login shell of %s; change that with chsh first",
				sh[i], who);
			free(who);
		}
	freev(sh);
	freev(k);
	return why;
}

/* old's shells that keep (if any) doesn't have too out of /etc/shells */
static void
drop_shells(struct meta *old, struct meta *keep)
{
	char **sh = shells(old), **k = keep ? shells(keep) : NULL;
	int i;

	for (i = 0; sh[i]; i++)
		if (!listed(k, sh[i]))
			etcshells(sh[i], -1);
	freev(sh);
	freev(k);
}

/* Before anything is put in place: the package's shells are in it, and the old version's
   that it drops aren't in use */
static void
check_shells(struct inst *s)
{
	char **sh = shells(&s->new.m), *why;
	struct entry *e;
	int i;

	for (i = 0; sh[i]; i++)
		if (!(e = findentry(&s->new, sh[i] + 1)) || e->type == 'd')
			fail(s, "its shell %s isn't a file in it", sh[i]);
	freev(sh);
	if (s->have_old && (why = shells_in_use(&s->old.m, &s->new.m))) {
		fail(s, "%s", why);
		free(why);
	}
}

/*
 * Boot commands: a package's "boot COMMAND" lines are run at every boot, each by a line
 * pkg adds to /etc/inittab after the one that runs /etc/rc, so before the network
 * daemons and the login window:
 *
 *	pk00:2:wait:COMMAND </dev/syscon >/dev/syscon 2>&1	# pkg NAME
 *
 * init waits for it. Removing the package takes its lines out, and an upgrade changes them
 * to the new version's. A line is found again by what follows its third colon, so one
 * the user has turned off (action "off") is still the package's.
 */

/* m's boot commands, NULL-terminated */
static char **
boots(struct meta *m)
{
	char **v = xmalloc(sizeof(char *));
	const char *s = m->extra, *e;
	int n = 0;

	while (s && (e = strchr(s, '\n'))) {
		if (!strncmp(s, "boot ", 5)) {
			v = xrealloc(v, (n + 2) * sizeof(char *));
			v[n] = xmalloc(e - s - 4);
			memcpy(v[n], s + 5, e - s - 5);
			v[n++][e - s - 5] = 0;
		}
		s = e + 1;
	}
	v[n] = NULL;
	return v;
}

/* what follows an inittab line's third colon, or NULL */
static const char *
initproc(const char *line)
{
	int i;

	for (i = 0; i < 3; i++)
		if (!(line = strchr(line, ':')))
			return NULL;
		else
			line++;
	return line;
}

/* Whether name's boot command cmd is in /etc/inittab (-1 if that can't be read), having
   added it (add > 0) or taken it out (add < 0) if need be. The file keeps its owner and
   mode. */
static int
etcinittab(const char *name, const char *cmd, int add)
{
	char *path = rootpath("/etc/inittab"), *text, *out, *p, *line, *proc, *mine;
	char **lines = NULL;
	size_t len, n = 0, l;
	struct stat st;
	int nl = 0, at = -1, was = 0, i, id;

	if (stat(path, &st) < 0 || !(text = readfile(path, &len, 1 << 20))) {
		if (add > 0)
			warn("can't add %s to /etc/inittab: %s", cmd, strerror(errno));
		else if (add < 0)
			warn("can't take %s out of /etc/inittab: %s", cmd, strerror(errno));
		free(path);
		return -1;
	}
	mine = xsprintf("%s </dev/syscon >/dev/syscon 2>&1\t# pkg %s", cmd, name);
	p = text;
	while ((line = nextline(&p))) {
		lines = xrealloc(lines, (nl + 1) * sizeof(char *));
		lines[nl++] = line;
		proc = (char *)initproc(line);
		if (proc && !strcmp(proc, mine))
			was = 1;
		/* new lines go after /etc/rc's and any of pkg's that follow it */
		if (proc && (!strncmp(proc, "/etc/rc ", 8) || !strcmp(proc, "/etc/rc")))
			at = nl;
		else if (at == nl - 1 && !strncmp(line, "pk", 2) && proc && strstr(proc, "\t# pkg "))
			at = nl;
	}
	if ((add > 0 && !was) || (add < 0 && was)) {
		char ids[100] = {0};

		for (i = 0; i < nl; i++)
			if (lines[i][0] == 'p' && lines[i][1] == 'k' && lines[i][2] >= '0' &&
			    lines[i][2] <= '9' && lines[i][3] >= '0' && lines[i][3] <= '9' &&
			    lines[i][4] == ':')
				ids[(lines[i][2] - '0') * 10 + lines[i][3] - '0'] = 1;
		for (id = 0; id < 100 && ids[id]; id++)
			;
		if (add > 0 && id == 100) {
			warn("can't add %s to /etc/inittab: no pkNN id left", cmd);
			goto done;
		}
		if (at < 0)
			at = nl;
		out = xmalloc(len + strlen(mine) + 32);
		for (i = 0; i <= nl; i++) {
			if (add > 0 && i == at)
				n += snprintf(out + n, len + strlen(mine) + 32 - n, "pk%02d:2:wait:%s\n",
					id, mine);
			if (i == nl)
				break;
			if (add < 0 && (proc = (char *)initproc(lines[i])) && !strcmp(proc, mine))
				continue;
			l = strlen(lines[i]);
			memcpy(out + n, lines[i], l);
			out[n + l] = '\n';
			n += l + 1;
		}
		if (writefile(path, out, n) < 0)
			warn("can't change /etc/inittab: %s", strerror(errno));
		else {
			chown(path, st.st_uid, st.st_gid);
			chmod(path, st.st_mode & 07777);
			say(add > 0 ? "Added %s to /etc/inittab, to run at every boot\n" :
				"Took %s out of /etc/inittab\n", cmd);
		}
		free(out);
	}
done:
	free(lines);
	free(mine);
	free(text);
	free(path);
	return was;
}

/* old's boot commands that keep (if any) doesn't have too out of /etc/inittab */
static void
drop_boots(struct meta *old, struct meta *keep)
{
	char **b = boots(old), **k = keep ? boots(keep) : NULL;
	int i;

	for (i = 0; b[i]; i++)
		if (!listed(k, b[i]))
			etcinittab(old->name, b[i], -1);
	freev(b);
	freev(k);
}

/* Before anything is put in place: each boot command's program is a file in the package */
static void
check_boots(struct inst *s)
{
	char **b = boots(&s->new.m), *prog;
	struct entry *e;
	int i;

	for (i = 0; b[i]; i++) {
		prog = xstrdup(b[i]);
		if (strchr(prog, ' '))
			*strchr(prog, ' ') = 0;
		if (!(e = findentry(&s->new, prog + 1)) || e->type == 'd')
			fail(s, "its boot command's program %s isn't a file in it", prog);
		free(prog);
	}
	freev(b);
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
	if (r == 0 && !s->err[0] && !s->same) {
		check_shells(s);
		check_boots(s);
	}

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
		if (ok) {
			char **sh = shells(&s->new.m), **b = boots(&s->new.m);

			if (s->have_old) {
				drop_shells(&s->old.m, &s->new.m);
				drop_boots(&s->old.m, &s->new.m);
			}
			for (i = 0; sh[i]; i++)
				etcshells(sh[i], 1);
			for (i = 0; b[i]; i++)
				etcinittab(s->new.m.name, b[i], 1);
			freev(sh);
			freev(b);
		}
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
	char **dirs, *path, *why;
	int i, nd = 0, ret = 0;

	if (readrecord(name, &r) < 0) {
		warn("%s isn't installed", name);
		return -1;
	}
	if ((why = shells_in_use(&r.m, NULL))) {
		warn("can't remove %s: %s", name, why);
		free(why);
		freerecord(&r);
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
		drop_shells(&r.m, NULL);
		drop_boots(&r.m, NULL);
	} else
		warn("%s is only partly removed; its record is kept", name);
	freerecord(&r);
	return ret;
}

int
verify_package(const char *name)
{
	struct record r;
	char **sh;
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
	sh = shells(&r.m);
	for (i = 0; sh[i]; i++)
		if (etcshells(sh[i], 0) != 1) {
			say("%s: %s: not in /etc/shells\n", name, sh[i]);
			bad++;
		}
	freev(sh);
	sh = boots(&r.m);
	for (i = 0; sh[i]; i++)
		if (etcinittab(name, sh[i], 0) != 1) {
			say("%s: %s: not in /etc/inittab\n", name, sh[i]);
			bad++;
		}
	freev(sh);
	freerecord(&r);
	return bad ? -1 : 0;
}
