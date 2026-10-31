/* Package descriptions, and the records of installed packages (docs/formats.md) */
#include "pkg.h"

/* One "key value" line into m. Keys it doesn't know go to m->extra. */
static int
metaline(struct meta *m, char *line, const char **err)
{
	char *v = strchr(line, ' ');

	if (!*line || *line == '#')
		return 0;
	if (!v) {
		*err = "a line has no value";
		return -1;
	}
	*v++ = 0;
	if (!strcmp(line, "format")) {
		if ((m->format = strtol(v, NULL, 10)) > FORMAT) {
			*err = "it's for a newer pkg (run pkg upgrade pkg)";
			return -1;
		}
	} else if (!strcmp(line, "shell") && (*v != '/' || validpath(v + 1))) {
		*err = "a shell line isn't a path from /";
		return -1;
	} else if (!strcmp(line, "name")) {
		free(m->name);
		m->name = xstrdup(v);
	} else if (!strcmp(line, "version")) {
		free(m->version);
		m->version = xstrdup(v);
	} else if (!strcmp(line, "summary")) {
		free(m->summary);
		m->summary = xstrdup(v);
	} else if (!strcmp(line, "installed") || !strcmp(line, "from") ||
	    !strcmp(line, "server")) {
		*err = "it has an installed, from or server line, which are pkg's own";
		return -1;
	} else {
		char *old = m->extra;

		m->extra = xsprintf("%s%s %s\n", old ? old : "", line, v);
		free(old);
	}
	return 0;
}

static int
checkmeta(struct meta *m, const char **err)
{
	if (!m->name || !validname(m->name))
		*err = "its name is missing or not allowed";
	else if (!m->version || !validversion(m->version))
		*err = "its version is missing or not allowed";
	else if (!m->summary)
		*err = "it has no summary";
	else
		return 0;
	return -1;
}

int
parsemeta(char *text, struct meta *m, const char **err)
{
	char *line;
	int format = 0;

	memset(m, 0, sizeof(*m));
	while ((line = nextline(&text))) {
		if (!strncmp(line, "format ", 7))
			format = 1;
		if (metaline(m, line, err) < 0)
			return -1;
	}
	if (!format) {
		*err = "it has no format line";
		return -1;
	}
	return checkmeta(m, err);
}

void
freemeta(struct meta *m)
{
	free(m->name);
	free(m->version);
	free(m->summary);
	free(m->extra);
	memset(m, 0, sizeof(*m));
}

static char *
recordpath(const char *name)
{
	return xsprintf("%s%s/installed/%s", root, PKGDIR, name);
}

void
addentry(struct record *r, struct entry *e)
{
	if (r->n == r->cap) {
		r->cap = r->cap ? 2 * r->cap : 64;
		r->e = xrealloc(r->e, r->cap * sizeof(*r->e));
	}
	r->e[r->n] = *e;
	r->e[r->n].path = xstrdup(e->path);
	r->e[r->n].target = e->target ? xstrdup(e->target) : NULL;
	r->n++;
}

struct entry *
findentry(struct record *r, const char *path)
{
	int i;

	for (i = 0; i < r->n; i++)
		if (!strcmp(r->e[i].path, path))
			return &r->e[i];
	return NULL;
}

int
readrecord(const char *name, struct record *r)
{
	char *path = recordpath(name), *text, *p, *line;
	const char *err = NULL;

	memset(r, 0, sizeof(*r));
	text = readfile(path, NULL, 16 << 20);
	free(path);
	if (!text)
		return -1;
	p = text;
	while ((line = nextline(&p))) {
		struct entry e;
		char *f[7];
		int n;

		if (!(line[0] && line[1] == '\t')) {
			if (!strncmp(line, "installed ", 10))
				r->installed = strtol(line + 10, NULL, 10);
			else if (!strncmp(line, "from ", 5)) {
				free(r->from);
				r->from = xstrdup(line + 5);
			} else if (!strncmp(line, "server ", 7)) {
				free(r->server);
				r->server = xstrdup(line + 7);
			} else if (metaline(&r->m, line, &err) < 0)
				break;
			continue;
		}
		memset(&e, 0, sizeof(e));
		e.type = line[0];
		n = splittab(line + 2, f, 7);
		e.path = f[0];
		if (e.type == 'f' && n == 6) {
			e.mode = strtol(f[1], NULL, 8);
			e.uid = strtol(f[2], NULL, 10);
			e.gid = strtol(f[3], NULL, 10);
			e.size = strtol(f[4], NULL, 10);
			if (strlen(f[5]) == 64)
				memcpy(e.sha256, f[5], 65);
		} else if (e.type == 'l' && n == 2) {
			e.target = f[1];
		} else if (e.type != 'd' || n != 1) {
			err = "a line isn't understood";
			break;
		}
		addentry(r, &e);
	}
	free(text);
	if (!err)
		checkmeta(&r->m, &err);
	if (err) {
		warn("the record of %s is damaged: %s", name, err);
		freerecord(r);
		return -1;
	}
	return 0;
}

int
writerecord(struct record *r)
{
	char *dir = xsprintf("%s%s/installed", root, PKGDIR), *path;
	char *text;
	size_t cap = 512, len = 0;
	int i, ret;

#define ADD(...) do { \
	char *s_ = xsprintf(__VA_ARGS__); \
	size_t n_ = strlen(s_); \
	if (len + n_ + 1 > cap) { \
		while (len + n_ + 1 > cap) \
			cap *= 2; \
		text = xrealloc(text, cap); \
	} \
	memcpy(text + len, s_, n_ + 1); \
	len += n_; \
	free(s_); \
} while (0)

	text = xmalloc(cap);
	text[0] = 0;
	ADD("format %d\nname %s\nversion %s\nsummary %s\n", r->m.format, r->m.name,
		r->m.version, r->m.summary);
	if (r->m.extra)
		ADD("%s", r->m.extra);
	ADD("installed %ld\nfrom %s\n", r->installed, r->from ? r->from : "");
	if (r->server)
		ADD("server %s\n", r->server);
	for (i = 0; i < r->n; i++) {
		struct entry *e = &r->e[i];

		if (e->type == 'f')
			ADD("f\t%s\t%o\t%d\t%d\t%ld\t%s\n", e->path, e->mode & 07777, e->uid,
				e->gid, e->size, e->sha256);
		else if (e->type == 'l')
			ADD("l\t%s\t%s\n", e->path, e->target);
		else
			ADD("d\t%s\n", e->path);
	}
#undef ADD
	ret = mkdirs(dir, 0755, NULL, NULL);
	path = recordpath(r->m.name);
	if (ret == 0)
		ret = writefile(path, text, len);
	if (ret < 0)
		warn("can't write %s: %s", path, strerror(errno));
	free(dir);
	free(path);
	free(text);
	return ret;
}

void
freerecord(struct record *r)
{
	int i;

	for (i = 0; i < r->n; i++) {
		free(r->e[i].path);
		free(r->e[i].target);
	}
	free(r->e);
	free(r->from);
	free(r->server);
	freemeta(&r->m);
	memset(r, 0, sizeof(*r));
}

struct names {
	char **v;
	int n, cap;
};

static int
addname(const char *name, void *arg)
{
	struct names *l = arg;

	if (!validname(name))
		return 0;	/* a temporary, or not ours */
	if (l->n == l->cap) {
		l->cap = l->cap ? 2 * l->cap : 32;
		l->v = xrealloc(l->v, l->cap * sizeof(char *));
	}
	l->v[l->n++] = xstrdup(name);
	return 0;
}

int
listinstalled(char ***names)
{
	char *dir = xsprintf("%s%s/installed", root, PKGDIR);
	struct names l = { NULL, 0, 0 };
	int i, j;

	listdir(dir, addname, &l);
	free(dir);
	for (i = 1; i < l.n; i++)
		for (j = i; j > 0 && strcmp(l.v[j - 1], l.v[j]) > 0; j--) {
			char *t = l.v[j];

			l.v[j] = l.v[j - 1];
			l.v[j - 1] = t;
		}
	*names = l.v;
	return l.n;
}

char *
owner(const char *path, int dirs)
{
	char **names, *found = NULL;
	int i, n = listinstalled(&names);

	for (i = 0; i < n; i++) {
		struct record r;

		if (!found && readrecord(names[i], &r) == 0) {
			struct entry *e = findentry(&r, path);

			if (e && (dirs || e->type != 'd'))
				found = xstrdup(names[i]);
			freerecord(&r);
		}
		free(names[i]);
	}
	free(names);
	return found;
}
