/* ustar (POSIX.1-1988) archives, read as a stream */
#include "tar.h"

static long
octal(const unsigned char *p, int n, int *bad)
{
	long v = 0;
	int i = 0;

	while (i < n && p[i] == ' ')
		i++;
	for (; i < n && p[i] >= '0' && p[i] <= '7'; i++)
		v = v * 8 + p[i] - '0';
	if (i < n && p[i] != ' ' && p[i] != 0)
		*bad = 1;
	return v;
}

static int
header(struct tar *t)
{
	const unsigned char *h = t->hdr;
	struct tarentry *e = &t->e;
	long sum = 0;
	int i, bad = 0, n;

	for (i = 0; i < 512; i++)
		sum += i >= 148 && i < 156 ? ' ' : h[i];
	if (sum != octal(h + 148, 8, &bad) || bad) {
		t->err = "a header's checksum is wrong";
		return -1;
	}
	if (memcmp(h + 257, "ustar", 5)) {
		t->err = "not a ustar archive";
		return -1;
	}
	memset(e, 0, sizeof(*e));
	/* name: prefix (155 bytes at 345), a /, then name (100 at 0) */
	n = 0;
	if (h[345]) {
		for (i = 0; i < 155 && h[345 + i]; i++)
			e->name[n++] = h[345 + i];
		e->name[n++] = '/';
	}
	for (i = 0; i < 100 && h[i]; i++)
		e->name[n++] = h[i];
	while (n > 1 && e->name[n - 1] == '/')
		n--;
	e->name[n] = 0;
	for (i = 0; i < 100 && h[157 + i]; i++)
		e->linkname[i] = h[157 + i];
	e->type = h[156] ? h[156] : '0';
	e->mode = octal(h + 100, 8, &bad);
	e->uid = octal(h + 108, 8, &bad);
	e->gid = octal(h + 116, 8, &bad);
	e->size = octal(h + 124, 12, &bad);
	e->mtime = octal(h + 136, 12, &bad);
	if (bad || e->size < 0) {
		t->err = "a header has a bad number";
		return -1;
	}
	if (e->type != '0' && e->size) {
		t->err = "a member that isn't a file has contents";
		return -1;
	}
	return 0;
}

int
tar_feed(void *arg, const unsigned char *p, size_t n)
{
	struct tar *t = arg;
	int r;

	while (n > 0 && !t->done) {
		size_t take;

		if (t->left > 0) {
			take = (size_t)t->left < n ? (size_t)t->left : n;
			if ((r = t->data(t->arg, p, take)))
				return r;
			t->left -= take;
			p += take, n -= take;
			if (t->left == 0 && (r = t->end(t->arg)))
				return r;
			continue;
		}
		if (t->pad > 0) {
			take = (size_t)t->pad < n ? (size_t)t->pad : n;
			t->pad -= take;
			p += take, n -= take;
			continue;
		}
		take = 512 - t->hlen < (int)n ? 512 - t->hlen : (int)n;
		memcpy(t->hdr + t->hlen, p, take);
		t->hlen += take;
		p += take, n -= take;
		if (t->hlen < 512)
			continue;
		t->hlen = 0;
		for (r = 0; r < 512 && !t->hdr[r]; r++)
			;
		if (r == 512) {
			if (++t->zeros == 2)
				t->done = 1;
			continue;
		}
		t->zeros = 0;
		if (header(t) < 0)
			return -1;
		if ((r = t->begin(t->arg, &t->e)))
			return r;
		t->left = t->e.size;
		t->pad = (512 - t->e.size % 512) % 512;
		if (t->left == 0 && (r = t->end(t->arg)))
			return r;
	}
	return 0;
}

int
tar_finish(struct tar *t)
{
	if (t->left || t->pad || t->hlen) {
		t->err = "the archive stops partway through a member";
		return -1;
	}
	return 0;
}
