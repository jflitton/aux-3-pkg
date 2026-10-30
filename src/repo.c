/* Servers: settings, the signed package list, and downloads (docs/formats.md) */
#include "pkg.h"
#include "defaults.h"
#include "ed25519.h"
#include "http.h"

#define INDEXMAX	(4L << 20)

struct conf {
	char *server;
	unsigned char (*keys)[32];
	int nkeys, unsigned_ok;
};

static void
readconf(struct conf *c)
{
	char *path = rootpath(CONFFILE), *text = readfile(path, NULL, 65536), *p, *line;
	int i;

	memset(c, 0, sizeof(*c));
	for (i = 0; default_keys[i]; i++) {
		c->keys = xrealloc(c->keys, (c->nkeys + 1) * sizeof(*c->keys));
		if (unhex(default_keys[i], c->keys[c->nkeys], 32) == 0)
			c->nkeys++;
	}
	p = text;
	while ((line = nextline(&p))) {
		char *w, *end;

		if ((end = strchr(line, '#')))
			*end = 0;
		while (*line == ' ' || *line == '\t')
			line++;
		for (end = line + strlen(line); end > line && (end[-1] == ' ' ||
		    end[-1] == '\t' || end[-1] == '\r'); end--)
			;
		*end = 0;
		if (!*line)
			continue;
		for (w = line; *w && *w != ' ' && *w != '\t'; w++)
			;
		if (*w)
			*w++ = 0;
		while (*w == ' ' || *w == '\t')
			w++;
		if (!strcmp(line, "server") && *w) {
			free(c->server);
			c->server = xstrdup(w);
		} else if (!strcmp(line, "key")) {
			c->keys = xrealloc(c->keys, (c->nkeys + 1) * sizeof(*c->keys));
			if (unhex(w, c->keys[c->nkeys], 32) == 0)
				c->nkeys++;
			else
				warn("%s: \"key %s\" isn't 64 hex digits; ignored", path, w);
		} else if (!strcmp(line, "unsigned") && !*w) {
			c->unsigned_ok = 1;
		} else
			warn("%s: \"%s\" isn't understood; ignored", path, line);
	}
	free(text);
	free(path);
}

static int
trusted(struct conf *c, const unsigned char *pk)
{
	int i;

	for (i = 0; i < c->nkeys; i++)
		if (!memcmp(c->keys[i], pk, 32))
			return 1;
	return 0;
}

/* 0 if a line of sigs is a trusted key's good signature of the index */
static int
checksig(struct conf *c, const char *index, size_t len, char *sigs)
{
	char *line;

	while ((line = nextline(&sigs))) {
		unsigned char pk[32], sig[64];
		char *f[3];

		if (splittab(line, f, 3) != 3 || strcmp(f[0], "ed25519") ||
		    unhex(f[1], pk, 32) || unhex(f[2], sig, 64))
			continue;
		if (trusted(c, pk) && ed25519_verify(sig, index, len, pk) == 0)
			return 0;
	}
	return -1;
}

/* Parses index text (changing it) into ix; -1 having said why if it's no good. */
static int
parseindex(char *text, struct index *ix, const char *where)
{
	char *p = text, *line = nextline(&p);
	int lineno = 1, cap = 0;

	if (!line || strncmp(line, "aux-pkg-index ", 14)) {
		warn("%s isn't a package list", where);
		return -1;
	}
	if (strtol(line + 14, NULL, 10) > FORMAT) {
		warn("the package list from %s is for a newer pkg; upgrade pkg", where);
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
			warn("line %d of the package list from %s is no good", lineno, where);
			return -1;
		}
		memset(&a.m, 0, sizeof(a.m));
		a.m.name = f[0];
		a.m.version = f[1];
		a.m.summary = f[5];
		memcpy(a.sha256, f[3], 65);
		a.file = f[4];
		if (ix->n == cap) {
			cap = cap ? 2 * cap : 64;
			ix->a = xrealloc(ix->a, cap * sizeof(*ix->a));
		}
		ix->a[ix->n++] = a;
	}
	return 0;
}

int
update(const char *server, int allow_unsigned)
{
	struct conf c;
	struct index ix;
	char *url = NULL, *index = NULL, *sigs = NULL, *copy = NULL, *path = NULL, *dir;
	size_t len = 0;
	int ret = -1, good;

	readconf(&c);
	if (!server)
		server = c.server ? c.server : DEFAULT_SERVER;
	if (!*server) {
		warn("no server to get packages from: give one with -s, or a server line in "
			"%s", CONFFILE);
		goto out;
	}
	url = xsprintf("%s%s", server, server[strlen(server) - 1] == '/' ? "" : "/");
	path = xsprintf("%sindex", url);
	if (!(index = http_mem(path, &len, INDEXMAX, 0)))
		goto out;
	free(path);
	path = xsprintf("%sindex.sig", url);
	sigs = http_mem(path, NULL, 65536, 1);
	good = sigs && checksig(&c, index, len, sigs) == 0;
	if (!good && !(allow_unsigned || c.unsigned_ok)) {
		warn("the package list from %s isn't signed by a key pkg trusts.\n"
			"If you trust that server, add its key to %s (see pkg's README), or "
			"use -u to accept it unsigned.", url, CONFFILE);
		goto out;
	}
	memset(&ix, 0, sizeof(ix));
	copy = xstrdup(index);
	if (parseindex(copy, &ix, url) < 0)
		goto out;
	free(ix.a);

	dir = rootpath(PKGDIR);
	mkdirs(dir, 0755, NULL, NULL);
	free(dir);
	free(path);
	path = rootpath(PKGDIR "/index.url");
	{
		char *meta = xsprintf("%s\n%s\n", url, good ? "signed" : "unsigned");
		char *ipath = rootpath(PKGDIR "/index");

		ret = writefile(ipath, index, len) || writefile(path, meta, strlen(meta));
		if (ret)
			warn("can't save the package list in %s: %s", ipath, strerror(errno));
		free(meta);
		free(ipath);
	}
	if (ret == 0)
		say("%d package%s at %s%s\n", ix.n, ix.n == 1 ? "" : "s", url,
			good ? "" : " (unsigned: not checked)");
out:
	free(url);
	free(index);
	free(sigs);
	free(copy);
	free(path);
	free(c.server);
	free(c.keys);
	return ret ? -1 : 0;
}

int
loadindex(struct index *ix)
{
	char *path = rootpath(PKGDIR "/index.url"), *meta, *p, *line;

	memset(ix, 0, sizeof(*ix));
	meta = readfile(path, NULL, 4096);
	free(path);
	path = rootpath(PKGDIR "/index");
	ix->buf = readfile(path, NULL, INDEXMAX);
	free(path);
	if (!meta || !ix->buf) {
		warn("there's no package list yet: run pkg update");
		free(meta);
		free(ix->buf);
		return -1;
	}
	p = meta;
	ix->url = xstrdup(nextline(&p));
	line = nextline(&p);
	ix->signed_ = line && !strcmp(line, "signed");
	free(meta);
	if (parseindex(ix->buf, ix, ix->url) < 0) {
		free(ix->buf);
		free(ix->url);
		free(ix->a);
		return -1;
	}
	return 0;
}

struct avail *
findavail(struct index *ix, const char *name)
{
	struct avail *best = NULL;
	int i;

	for (i = 0; i < ix->n; i++)
		if (!strcmp(ix->a[i].m.name, name) &&
		    (!best || vercmp(ix->a[i].m.version, best->m.version) > 0))
			best = &ix->a[i];
	return best;
}

int
fetch_package(struct index *ix, struct avail *a, char **path)
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
	url = xsprintf("%s%s", ix->url, a->file);
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
