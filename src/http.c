/*
 * HTTP/1.1 GET, plain HTTP only, one request per connection. Names are looked up in
 * /etc/hosts and then with DNS, asking the servers in /etc/resolv.conf directly: A/UX's
 * resolver is in Apple's libc, which pkg doesn't use. Every wait has a timeout, so a
 * dead server or network can't leave pkg hanging.
 */
#include "pkg.h"
#include "http.h"

#ifndef FIONBIO
#define FIONBIO		0x8004667e	/* _IOW('f', 126, int), the same on A/UX */
#endif
#define CONNECT_SECS	20
#define READ_SECS	60
#define DNS_SECS	4

struct sink {
	int fd;			/* or -1: to buf */
	sha256_ctx *sha;
	char *buf;
	size_t len, cap, max;
	long total, expect;
	const char *label;
	long shown;		/* when progress was last shown, in ms */
};

struct conn {
	int fd;
	unsigned char buf[4096];
	int pos, len;
};

static long
now_ms(void)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);
	return tv.tv_sec * 1000L + tv.tv_usec / 1000;
}

/* wait until fd can be read (or written); -1 with errno ETIMEDOUT if it can't in time */
static int
await(int fd, int writing, int secs)
{
	fd_set set;
	struct timeval tv;
	int r;

	do {
		FD_ZERO(&set);
		FD_SET(fd, &set);
		tv.tv_sec = secs;
		tv.tv_usec = 0;
		r = select(fd + 1, writing ? NULL : &set, writing ? &set : NULL, NULL, &tv);
	} while (r < 0 && errno == EINTR);
	if (r == 0)
		errno = ETIMEDOUT;
	return r > 0 ? 0 : -1;
}

/* a.b.c.d, each part decimal, 0x hex or 0 octal as inet_addr takes them: A/UX's own
   /etc/hosts has 0x7F.0x00.0x00.0x01 */
static int
quad(const char *s, unsigned long *addr)
{
	unsigned long a = 0;
	int i;

	for (i = 0; i < 4; i++) {
		char *end;
		unsigned long v;

		if (*s < '0' || *s > '9')
			return -1;
		v = strtoul(s, &end, 0);
		if (v > 255 || (i < 3 && *end != '.') || (i == 3 && *end))
			return -1;
		a = a << 8 | v;
		s = end + 1;
	}
	*addr = a;
	return 0;
}

static int
samename(const char *a, const char *b)
{
	for (; *a && *b; a++, b++) {
		int x = *a >= 'A' && *a <= 'Z' ? *a + 32 : *a;
		int y = *b >= 'A' && *b <= 'Z' ? *b + 32 : *b;

		if (x != y)
			return 0;
	}
	return !*a && !*b;
}

static int
from_hosts(const char *host, unsigned long *addr)
{
	char *text = readfile("/etc/hosts", NULL, 1 << 20), *p = text, *line;
	int found = 0;

	while (!found && (line = nextline(&p))) {
		char *w[16], *c;
		int n = 0;

		if ((c = strchr(line, '#')))
			*c = 0;
		for (c = line; *c && n < 16;) {
			while (*c == ' ' || *c == '\t')
				*c++ = 0;
			if (!*c)
				break;
			w[n++] = c;
			while (*c && *c != ' ' && *c != '\t')
				c++;
		}
		while (n > 1 && !found)
			if (samename(w[--n], host) && quad(w[0], addr) == 0)
				found = 1;
	}
	free(text);
	return found ? 0 : -1;
}

/* skip a (possibly compressed) name in a DNS message; the offset after it, or -1 */
static int
skipname(const unsigned char *m, int len, int i)
{
	while (i < len) {
		if (m[i] == 0)
			return i + 1;
		if ((m[i] & 0xc0) == 0xc0)
			return i + 2 <= len ? i + 2 : -1;
		i += m[i] + 1;
	}
	return -1;
}

/* 0 found, -1 no answer from this server, -2 the name doesn't exist */
static int
dns_ask(unsigned long server, const char *host, unsigned long *addr)
{
	unsigned char q[300], m[1024];
	struct sockaddr_in sin;
	int fd, n = 12, try, id = (getpid() ^ time(NULL)) & 0xffff;
	const char *s = host;

	memset(q, 0, 12);
	q[0] = id >> 8, q[1] = id;
	q[2] = 1;			/* recursion desired */
	q[5] = 1;			/* one question */
	while (*s) {
		const char *dot = strchr(s, '.');
		int l = dot ? dot - s : (int)strlen(s);

		if (l == 0 || l > 63 || n + l + 6 > (int)sizeof(q))
			return -2;
		q[n++] = l;
		memcpy(q + n, s, l);
		n += l;
		s += l + (dot != NULL);
	}
	q[n++] = 0;
	q[n++] = 0, q[n++] = 1;		/* A */
	q[n++] = 0, q[n++] = 1;		/* IN */

	if ((fd = socket(AF_INET, SOCK_DGRAM, 0)) < 0)
		return -1;
	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	sin.sin_port = htons(53);
	sin.sin_addr.s_addr = htonl(server);
	for (try = 0; try < 2; try++) {
		int len, i, an, rcode;

		if (sendto(fd, q, n, 0, (struct sockaddr *)&sin, sizeof(sin)) != n)
			break;
		if (await(fd, 0, DNS_SECS) < 0)
			continue;
		if ((len = recvfrom(fd, m, sizeof(m), 0, NULL, NULL)) < 12 ||
		    m[0] != q[0] || m[1] != q[1] || !(m[2] & 0x80))
			continue;
		close(fd);
		rcode = m[3] & 15;
		if (rcode == 3)
			return -2;
		if (rcode)
			return -1;
		an = m[6] << 8 | m[7];
		i = 12;
		for (n = m[4] << 8 | m[5]; n > 0 && i >= 0; n--)
			if ((i = skipname(m, len, i)) >= 0)
				i += 4;
		for (; an > 0 && i >= 0 && i + 10 <= len; an--) {
			int type, rdlen;

			if ((i = skipname(m, len, i)) < 0 || i + 10 > len)
				break;
			type = m[i] << 8 | m[i + 1];
			rdlen = m[i + 8] << 8 | m[i + 9];
			i += 10;
			if (type == 1 && rdlen == 4 && i + 4 <= len) {
				*addr = (unsigned long)m[i] << 24 | (unsigned long)m[i + 1] << 16 |
					(unsigned long)m[i + 2] << 8 | m[i + 3];
				return 0;
			}
			i += rdlen;
		}
		return -2;
	}
	close(fd);
	return -1;
}

static int
resolve(const char *host, unsigned long *addr)
{
	char *conf, *p, *line;
	int asked = 0, r = -1;

	if (quad(host, addr) == 0 || from_hosts(host, addr) == 0)
		return 0;
	conf = readfile("/etc/resolv.conf", NULL, 65536);
	p = conf;
	while (r == -1 && (line = nextline(&p))) {
		unsigned long server;
		char *a = line;

		if (strncmp(line, "nameserver", 10))
			continue;
		a += 10;
		while (*a == ' ' || *a == '\t')
			a++;
		if (quad(a, &server) < 0)
			continue;
		asked = 1;
		r = dns_ask(server, host, addr);
	}
	free(conf);
	if (r == -2)
		warn("there's no host called %s", host);
	else if (r < 0 && !asked)
		warn("can't look up %s: it isn't in /etc/hosts, and /etc/resolv.conf names no "
			"DNS server", host);
	else if (r < 0)
		warn("can't look up %s: no DNS server answered", host);
	return r < 0 ? -1 : 0;
}

static int
dial(const char *host, int port)
{
	struct sockaddr_in sin;
	unsigned long addr;
	int fd, on = 1, off = 0, err = 0, r;
	socklen_t len = sizeof(err);

	if (resolve(host, &addr) < 0)
		return -1;
	if ((fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
		warn("can't make a socket: %s", strerror(errno));
		return -1;
	}
	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	sin.sin_port = htons(port);
	sin.sin_addr.s_addr = htonl(addr);
	ioctl(fd, FIONBIO, &on);
	r = connect(fd, (struct sockaddr *)&sin, sizeof(sin));
	if (r < 0 && errno == EINPROGRESS) {
		if (await(fd, 1, CONNECT_SECS) == 0 &&
		    getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) == 0)
			r = err ? (errno = err, -1) : 0;
	}
	if (r < 0) {
		warn("can't connect to %s port %d: %s", host, port, strerror(errno));
		close(fd);
		return -1;
	}
	ioctl(fd, FIONBIO, &off);
	return fd;
}

static int
fill(struct conn *c)
{
	ssize_t n;

	if (await(c->fd, 0, READ_SECS) < 0)
		return -1;
	if ((n = recv(c->fd, c->buf, sizeof(c->buf), 0)) < 0)
		return -1;
	c->pos = 0;
	c->len = n;
	return n;
}

/* a line without its CRLF, or NULL at the end or on an error */
static char *
getline_(struct conn *c, char *line, int max)
{
	int n = 0;

	for (;;) {
		if (c->pos == c->len && fill(c) <= 0)
			return NULL;
		while (c->pos < c->len) {
			char ch = c->buf[c->pos++];

			if (ch == '\n') {
				if (n && line[n - 1] == '\r')
					n--;
				line[n] = 0;
				return line;
			}
			if (n < max - 1)
				line[n++] = ch;
		}
	}
}

static void
progress(struct sink *s, int done)
{
	long t = now_ms();
	char line[100];

	if (!s->label || !isatty(2) || (!done && t - s->shown < 500))
		return;
	s->shown = t;
	if (s->expect > 0)
		snprintf(line, sizeof(line), "\r%s: %ld of %ld KB", s->label,
			(s->total + 1023) / 1024, (s->expect + 1023) / 1024);
	else
		snprintf(line, sizeof(line), "\r%s: %ld KB", s->label, (s->total + 1023) / 1024);
	write(2, line, strlen(line));
	if (done)
		write(2, "\n", 1);
}

static int
put(struct sink *s, const unsigned char *p, size_t n)
{
	if (s->sha)
		sha256_update(s->sha, p, n);
	s->total += n;
	if (s->fd >= 0) {
		while (n > 0) {
			ssize_t w = write(s->fd, p, n);

			if (w <= 0) {
				warn("can't save the download: %s", w < 0 ? strerror(errno) :
					"disk full?");
				return -1;
			}
			p += w, n -= w;
		}
	} else {
		if (s->len + n > s->max) {
			warn("the server sent more than expected");
			return -1;
		}
		if (s->len + n + 1 > s->cap) {
			s->cap = (s->len + n + 1) * 2;
			s->buf = xrealloc(s->buf, s->cap);
		}
		memcpy(s->buf + s->len, p, n);
		s->len += n;
	}
	progress(s, 0);
	return 0;
}

/* n bytes of body, or (n < 0) all there is */
static int
body(struct conn *c, struct sink *s, long n)
{
	while (n != 0) {
		long take;

		if (c->pos == c->len) {
			int r = fill(c);

			if (r < 0) {
				warn("the connection failed: %s", strerror(errno));
				return -1;
			}
			if (r == 0) {
				if (n < 0)
					return 0;
				warn("the connection closed before the end of the file");
				return -1;
			}
		}
		take = c->len - c->pos;
		if (n >= 0 && take > n)
			take = n;
		if (put(s, c->buf + c->pos, take) < 0)
			return -1;
		c->pos += take;
		if (n > 0)
			n -= take;
	}
	return 0;
}

static int
chunked(struct conn *c, struct sink *s)
{
	char line[256];

	for (;;) {
		char *end;
		long n;

		if (!getline_(c, line, sizeof(line)))
			goto broken;
		n = strtol(line, &end, 16);
		if (end == line || n < 0)
			goto broken;
		if (n == 0)
			break;
		if (body(c, s, n) < 0)
			return -1;
		if (!getline_(c, line, sizeof(line)) || line[0])
			goto broken;
	}
	while (getline_(c, line, sizeof(line)) && line[0])
		;		/* trailers */
	return 0;
broken:
	warn("the server's reply is garbled");
	return -1;
}

static int
parseurl(const char *url, char *host, int hostmax, int *port, const char **path)
{
	const char *h, *e;

	if (!strncmp(url, "https:", 6)) {
		warn("%s: pkg can't use HTTPS; give the server's http:// address", url);
		return -1;
	}
	if (strncmp(url, "http://", 7)) {
		warn("%s isn't an http:// address", url);
		return -1;
	}
	h = url + 7;
	for (e = h; *e && *e != ':' && *e != '/'; e++)
		;
	if (e == h || e - h >= hostmax) {
		warn("%s: the host name is missing or too long", url);
		return -1;
	}
	memcpy(host, h, e - h);
	host[e - h] = 0;
	*port = 80;
	if (*e == ':') {
		*port = strtol(e + 1, (char **)&e, 10);
		if (*port <= 0 || *port > 65535 || (*e && *e != '/')) {
			warn("%s: the port isn't right", url);
			return -1;
		}
	}
	*path = *e ? e : "/";
	return 0;
}

static int
get(const char *url, struct sink *s, int quiet_404)
{
	char host[256], line[1024], *where = NULL, *cur = xstrdup(url);
	int redirects, ret = -1;

	for (redirects = 0; redirects < 5; redirects++) {
		static struct conn c;
		const char *path;
		char *req, *hostport;
		int port, status, is_chunked = 0;
		long length = -1;
		ssize_t n, w;

		if (parseurl(cur, host, sizeof(host), &port, &path) < 0)
			break;
		if ((c.fd = dial(host, port)) < 0)
			break;
		c.pos = c.len = 0;
		hostport = port == 80 ? xstrdup(host) : xsprintf("%s:%d", host, port);
		req = xsprintf("GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: pkg/%s (A/UX)\r\n"
			"Accept-Encoding: identity\r\nConnection: close\r\n\r\n", path,
			hostport, PKG_VERSION);
		free(hostport);
		for (n = 0, w = 0; n < (ssize_t)strlen(req); n += w)
			if ((w = send(c.fd, req + n, strlen(req) - n, 0)) <= 0)
				break;
		free(req);
		if (w <= 0 || !getline_(&c, line, sizeof(line)) ||
		    strncmp(line, "HTTP/1.", 7) || !line[8] ||
		    (status = strtol(line + 9, NULL, 10)) < 100) {
			warn("%s: no proper answer from %s", cur, host);
			close(c.fd);
			break;
		}
		free(where);
		where = NULL;
		while (getline_(&c, line, sizeof(line)) && line[0]) {
			char *v = strchr(line, ':'), *k;

			if (!v)
				continue;
			*v++ = 0;
			while (*v == ' ' || *v == '\t')
				v++;
			for (k = line; *k; k++)
				if (*k >= 'A' && *k <= 'Z')
					*k += 32;
			if (!strcmp(line, "content-length"))
				length = strtol(v, NULL, 10);
			else if (!strcmp(line, "transfer-encoding") && strstr(v, "chunked"))
				is_chunked = 1;
			else if (!strcmp(line, "location"))
				where = xstrdup(v);
		}
		if (status >= 300 && status < 400 && where) {
			close(c.fd);
			if (where[0] == '/') {
				char *abs = xsprintf("http://%s:%d%s", host, port, where);

				free(where);
				where = abs;
			}
			if (!strncmp(where, "https:", 6)) {
				warn("%s redirects to HTTPS, which pkg can't use", cur);
				break;
			}
			free(cur);
			cur = where;
			where = NULL;
			continue;
		}
		if (status != 200) {
			if (status != 404 || !quiet_404)
				warn("%s: the server answered %s", cur, line[0] ? line + 9 : "");
			close(c.fd);
			ret = status == 404 ? -2 : -1;
			break;
		}
		s->expect = length;
		ret = is_chunked ? chunked(&c, s) : body(&c, s, length);
		close(c.fd);
		progress(s, 1);
		break;
	}
	if (redirects == 5)
		warn("%s: too many redirects", url);
	free(where);
	free(cur);
	return ret;
}

int
http_file(const char *url, int fd, sha256_ctx *sha, long *size, const char *label)
{
	struct sink s;
	int r;

	memset(&s, 0, sizeof(s));
	s.fd = fd;
	s.sha = sha;
	s.label = label;
	r = get(url, &s, 0);
	*size = s.total;
	return r < 0 ? -1 : 0;
}

char *
http_mem(const char *url, size_t *len, size_t max, int quiet_404)
{
	struct sink s;

	memset(&s, 0, sizeof(s));
	s.fd = -1;
	s.max = max;
	if (get(url, &s, quiet_404) < 0) {
		free(s.buf);
		return NULL;
	}
	if (!s.buf)
		s.buf = xmalloc(1);
	s.buf[s.len] = 0;
	if (len)
		*len = s.len;
	return s.buf;
}
