/* Servers: settings, the signed package lists, and downloads (docs/formats.md) */
#include "pkg.h"
#include "defaults.h"
#include "ed25519.h"
#include "http.h"

#define INDEXMAX	(4L << 20)

/* A server as /etc/pkg.conf gives it, with the keys trusted to sign its list */
struct source {
	char *name, *url;	/* url "" if there's none */
	unsigned char (*keys)[32];
	int nkeys, unsigned_ok;
};

struct conf {
	struct source *s;	/* main first, then the repo lines in order */
	int n;
};

/* url, ending in / */
static char *
slash(const char *url)
{
	return xsprintf("%s%s", url, *url && url[strlen(url) - 1] != '/' ? "/" : "");
}

/* The next word of *p, ended in place; NULL if there are no more */
static char *
word(char **p)
{
	char *s = *p, *w;

	while (*s == ' ' || *s == '\t' || *s == '\r')
		s++;
	if (!*s)
		return NULL;
	for (w = s; *s && *s != ' ' && *s != '\t' && *s != '\r'; s++)
		;
	if (*s)
		*s++ = 0;
	*p = s;
	return w;
}

static struct source *
addsource(struct conf *c, const char *name, const char *url)
{
	struct source *s;

	c->s = xrealloc(c->s, (c->n + 1) * sizeof(*c->s));
	s = &c->s[c->n++];
	memset(s, 0, sizeof(*s));
	s->name = xstrdup(name);
	s->url = slash(url);
	return s;
}

static int
addkey(struct source *s, const char *key)
{
	s->keys = xrealloc(s->keys, (s->nkeys + 1) * sizeof(*s->keys));
	if (unhex(key, s->keys[s->nkeys], 32) < 0)
		return -1;
	s->nkeys++;
	return 0;
}

/* "repo NAME URL [KEY ...] [unsigned]": rest is what follows URL */
static void
addrepo(struct conf *c, const char *path, const char *name, const char *url, char *rest)
{
	struct source *s;
	char *w;
	int i;

	if (!validname(name) || !strcmp(name, MAINSERVER)) {
		warn("%s: \"repo %s\": that name isn't allowed; ignored", path, name);
		return;
	}
	for (i = 0; i < c->n; i++)
		if (!strcmp(c->s[i].name, name)) {
			warn("%s: there are two repo lines for %s; the second is ignored", path,
				name);
			return;
		}
	s = addsource(c, name, url);
	while ((w = word(&rest)))
		if (!strcmp(w, "unsigned"))
			s->unsigned_ok = 1;
		else if (addkey(s, w) < 0)
			warn("%s: \"%s\" on the repo line for %s isn't a key (64 hex digits) "
				"or unsigned; ignored", path, w, name);
}

static void
readconf(struct conf *c)
{
	char *path = rootpath(CONFFILE), *text = readfile(path, NULL, 65536), *p, *line;
	int i;

	memset(c, 0, sizeof(*c));
	addsource(c, MAINSERVER, DEFAULT_SERVER);
	for (i = 0; default_keys[i]; i++)
		addkey(&c->s[0], default_keys[i]);
	p = text;
	while ((line = nextline(&p))) {
		char *k, *v, *url, *end;

		if ((end = strchr(line, '#')))
			*end = 0;
		if (!(k = word(&line)))
			continue;
		v = word(&line);
		if (!strcmp(k, "server") && v && !word(&line)) {
			free(c->s[0].url);
			c->s[0].url = slash(v);
		} else if (!strcmp(k, "key") && v && !word(&line)) {
			if (addkey(&c->s[0], v) < 0)
				warn("%s: \"key %s\" isn't 64 hex digits; ignored", path, v);
		} else if (!strcmp(k, "unsigned") && !v) {
			c->s[0].unsigned_ok = 1;
		} else if (!strcmp(k, "repo") && v && (url = word(&line))) {
			addrepo(c, path, v, url, line);
		} else
			warn("%s: \"%s\" isn't understood; ignored", path, k);
	}
	free(text);
	free(path);
}

static void
freeconf(struct conf *c)
{
	int i;

	for (i = 0; i < c->n; i++) {
		free(c->s[i].name);
		free(c->s[i].url);
		free(c->s[i].keys);
	}
	free(c->s);
}

static int
trusted(struct source *s, const unsigned char *pk)
{
	int i;

	for (i = 0; i < s->nkeys; i++)
		if (!memcmp(s->keys[i], pk, 32))
			return 1;
	return 0;
}

/* 0 if a line of sigs is a good signature of the index by a key trusted for s */
static int
checksig(struct source *s, const char *index, size_t len, char *sigs)
{
	char *line;

	while ((line = nextline(&sigs))) {
		unsigned char pk[32], sig[64];
		char *f[3];

		if (splittab(line, f, 3) != 3 || strcmp(f[0], "ed25519") ||
		    unhex(f[1], pk, 32) || unhex(f[2], sig, 64))
			continue;
		if (trusted(s, pk) && ed25519_verify(sig, index, len, pk) == 0)
			return 0;
	}
	return -1;
}

/* Parses server s's list (changing it) onto the end of ix; -1 having said why if it's no
   good. */
static int
parseindex(char *text, struct index *ix, struct server *s)
{
	char *p = text, *line = nextline(&p);
	int lineno = 1, cap = ix->n;

	if (!line || strncmp(line, "aux-pkg-index ", 14)) {
		warn("%s isn't a package list", s->url);
		return -1;
	}
	if (strtol(line + 14, NULL, 10) > INDEX_FORMAT) {
		warn("the package list from %s is for a newer pkg; upgrade pkg", s->url);
		return -1;
	}
	while ((line = nextline(&p))) {
		struct avail a;
		char *f[6], *end;

		lineno++;
		if (!*line || *line == '#')
			continue;
		if (splittab(line, f, 6) != 6 || !validname(f[0]) || !validversion(f[1]) ||
		    (a.size = strtol(f[2], &end, 10)) < 0 || *end || strlen(f[3]) != 64 ||
		    validpath(f[4]) || strchr(f[4], ':')) {
			warn("line %d of the package list from %s is no good", lineno, s->url);
			return -1;
		}
		memset(&a.m, 0, sizeof(a.m));
		a.m.name = f[0];
		a.m.version = f[1];
		a.m.summary = f[5];
		memcpy(a.sha256, f[3], 65);
		a.file = f[4];
		a.s = s;
		if (ix->n == cap) {
			cap = cap ? 2 * cap : 64;
			ix->a = xrealloc(ix->a, cap * sizeof(*ix->a));
		}
		ix->a[ix->n++] = a;
	}
	return 0;
}

static char *
listpath(const char *name)
{
	return xsprintf("%s%s/lists/%s", root, PKGDIR, name);
}

/* Gets s's package list and saves it in lists/: 0, or -1 having said why */
static int
getlist(struct source *s, int *good)
{
	struct index ix;
	struct server sv;
	char *path, *index, *sigs, *copy = NULL;
	size_t len = 0;
	int ret = -1;

	path = xsprintf("%sindex", s->url);
	index = http_mem(path, &len, INDEXMAX, 0);
	free(path);
	if (!index)
		return -1;
	path = xsprintf("%sindex.sig", s->url);
	sigs = http_mem(path, NULL, 65536, 1);
	free(path);
	path = NULL;
	*good = sigs && checksig(s, index, len, sigs) == 0;
	if (!*good && !s->unsigned_ok) {
		if (!strcmp(s->name, MAINSERVER))
			warn("the package list from %s isn't signed by a key pkg trusts.\n"
				"If you trust that server, add its key to %s (see pkg's README), "
				"or use --unsigned to accept it unsigned.", s->url, CONFFILE);
		else
			warn("the package list from %s isn't signed by a key pkg trusts for %s.\n"
				"If you trust that server, put its key on its repo line in %s (see "
				"pkg's README), or unsigned there to accept it unsigned.", s->url,
				s->name, CONFFILE);
		goto out;
	}
	memset(&ix, 0, sizeof(ix));
	sv.name = s->name;
	sv.url = s->url;
	copy = xstrdup(index);
	if (parseindex(copy, &ix, &sv) < 0)
		goto out;
	free(ix.a);
	path = listpath(s->name);
	if (writefile(path, index, len) < 0) {
		warn("can't save the package list in %s: %s", path, strerror(errno));
		goto out;
	}
	say("%s: %d package%s at %s%s\n", s->name, ix.n, ix.n == 1 ? "" : "s", s->url,
		*good ? "" : " (unsigned: not checked)");
	ret = 0;
out:
	free(index);
	free(sigs);
	free(copy);
	free(path);
	return ret;
}

/* The servers whose lists pkg has: ix->ns is 0 if there are none yet. -1 having said why
   if the file saying what they are is damaged. */
static int
readservers(struct index *ix)
{
	char *path = rootpath(PKGDIR "/servers"), *text, *p, *line;
	int ret = 0;

	memset(ix, 0, sizeof(*ix));
	text = readfile(path, NULL, 65536);
	p = text;
	while ((line = nextline(&p))) {
		struct server *s;
		char *f[3];

		if (splittab(line, f, 3) != 3 || !validname(f[0]) || strncmp(f[1], "http://", 7)) {
			warn("%s is damaged: run pkg update", path);
			ret = -1;
			break;
		}
		ix->s = xrealloc(ix->s, (ix->ns + 1) * sizeof(*ix->s));
		s = &ix->s[ix->ns++];
		memset(s, 0, sizeof(*s));
		s->name = xstrdup(f[0]);
		s->url = xstrdup(f[1]);
		s->signed_ = !strcmp(f[2], "signed");
	}
	free(text);
	free(path);
	return ret;
}

static void
freeservers(struct index *ix)
{
	int i;

	for (i = 0; i < ix->ns; i++) {
		free(ix->s[i].name);
		free(ix->s[i].url);
		free(ix->s[i].buf);
	}
	free(ix->s);
	free(ix->a);
}

/* removes a list from lists/ that's not one of the servers' */
static int
prune(const char *name, void *arg)
{
	if (!findserver(arg, name)) {
		char *path = listpath(name);

		unlink(path);
		free(path);
	}
	return 0;
}

/*
 * Gets every server's list. One that can't be had leaves what pkg had from that server
 * before, so a server being down doesn't take its packages away; one that's no longer in
 * /etc/pkg.conf is dropped.
 */
int
update(const char *server, int allow_unsigned)
{
	struct conf c;
	struct index old, now;
	char *text = xstrdup(""), *path, *dir;
	int i, n = 0, ret = 0;

	readconf(&c);
	if (server) {
		free(c.s[0].url);
		c.s[0].url = slash(server);
	}
	c.s[0].unsigned_ok |= allow_unsigned;
	for (i = 0; i < c.n; i++)
		n += *c.s[i].url != 0;
	if (!n) {
		warn("no server to get packages from: give one with -s, or a server line in "
			"%s", CONFFILE);
		free(text);
		freeconf(&c);
		return -1;
	}
	readservers(&old);
	dir = rootpath(PKGDIR "/lists");
	mkdirs(dir, 0755, NULL, NULL);
	for (i = 0; i < c.n; i++) {
		struct source *s = &c.s[i];
		struct server *was;
		char *t = NULL;
		int good;

		if (!*s->url)
			continue;
		if (getlist(s, &good) == 0)
			t = xsprintf("%s%s\t%s\t%s\n", text, s->name, s->url,
				good ? "signed" : "unsigned");
		else {
			ret = -1;
			if ((was = findserver(&old, s->name))) {
				t = xsprintf("%s%s\t%s\t%s\n", text, was->name, was->url,
					was->signed_ ? "signed" : "unsigned");
				warn("kept the package list pkg had from %s", s->name);
			}
		}
		if (t) {
			free(text);
			text = t;
		}
	}
	path = rootpath(PKGDIR "/servers");
	if (writefile(path, text, strlen(text)) < 0) {
		warn("can't save %s: %s", path, strerror(errno));
		ret = -1;
	} else if (readservers(&now) == 0) {
		listdir(dir, prune, &now);
		freeservers(&now);
	}
	free(path);
	free(dir);
	free(text);
	freeservers(&old);
	freeconf(&c);
	return ret;
}

int
loadindex(struct index *ix)
{
	int i;

	if (readservers(ix) < 0)
		return -1;
	if (ix->ns == 0) {
		warn("there's no package list yet: run pkg update");
		return -1;
	}
	for (i = 0; i < ix->ns; i++) {
		struct server *s = &ix->s[i];
		char *path = listpath(s->name);

		s->buf = readfile(path, NULL, INDEXMAX);
		free(path);
		if (!s->buf) {
			warn("the package list from %s is missing: run pkg update", s->name);
			return -1;
		}
		if (parseindex(s->buf, ix, s) < 0)
			return -1;
	}
	return 0;
}

struct server *
findserver(struct index *ix, const char *name)
{
	int i;

	for (i = 0; i < ix->ns; i++)
		if (!strcmp(ix->s[i].name, name))
			return &ix->s[i];
	return NULL;
}

struct avail *
findavail(struct index *ix, const char *name, struct server *s)
{
	struct avail *best = NULL;
	int i;

	for (i = 0; i < ix->n; i++) {
		struct avail *a = &ix->a[i];

		if (best && a->s != best->s)
			break;		/* the first server that has it is the one */
		if ((!s || a->s == s) && !strcmp(a->m.name, name) &&
		    (!best || vercmp(a->m.version, best->m.version) > 0))
			best = a;
	}
	return best;
}

int
fetch_package(struct avail *a, char **path)
{
	char *dir = rootpath(PKGDIR "/download"), *url, *part, *label;
	unsigned char d[32];
	char sum[65];
	sha256_ctx sha;
	long size;
	int fd, r;

	mkdirs(dir, 0755, NULL, NULL);
	*path = xsprintf("%s/%s-%s.pkg", dir, a->m.name, a->m.version);
	part = xsprintf("%s.part", *path);
	url = xsprintf("%s%s", a->s->url, a->file);
	label = xsprintf("%s %s", a->m.name, a->m.version);
	free(dir);
	if ((fd = open(part, O_WRONLY | O_CREAT | O_TRUNC, 0644)) < 0) {
		warn("can't make %s: %s", part, strerror(errno));
		r = -1;
	} else {
		sha256_init(&sha);
		r = http_file(url, fd, &sha, &size, label);
		if (close(fd) < 0 && r == 0) {
			warn("can't save %s: %s", part, strerror(errno));
			r = -1;
		}
	}
	if (r == 0) {
		sha256_final(&sha, d);
		hex(d, 32, sum);
		if (size != a->size || strcmp(sum, a->sha256)) {
			warn("the download of %s doesn't match the package list: it's damaged, or "
				"the server has changed since pkg update", url);
			r = -1;
		} else if (rename(part, *path) < 0) {
			warn("can't rename %s: %s", part, strerror(errno));
			r = -1;
		}
	}
	if (r < 0) {
		unlink(part);
		free(*path);
		*path = NULL;
	}
	free(part);
	free(url);
	free(label);
	return r;
}
