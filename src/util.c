/* Helpers: messages, memory, files, parsing, names and versions, the lock */
#include "pkg.h"

#ifdef AUX
int
listdir(const char *path, int (*fn)(const char *name, void *arg), void *arg)
{
	static char buf[4096];
	long base = 0;
	int fd, n, r = 0;

	if ((fd = open(path, O_RDONLY)) < 0)
		return -1;
	while (!r && (n = getdirent(fd, buf, sizeof(buf), &base)) > 0) {
		char *p = buf;

		while (!r && p < buf + n) {
			struct direct *d = (struct direct *)p;

			if (d->d_reclen == 0)
				break;
			if (d->d_fileno && strcmp(d->d_name, ".") && strcmp(d->d_name, ".."))
				r = fn(d->d_name, arg);
			p += d->d_reclen;
		}
	}
	close(fd);
	return n < 0 ? -1 : r;
}
#else
int
listdir(const char *path, int (*fn)(const char *name, void *arg), void *arg)
{
	DIR *d = opendir(path);
	struct dirent *e;
	int r = 0;

	if (!d)
		return -1;
	while (!r && (e = readdir(d)))
		if (strcmp(e->d_name, ".") && strcmp(e->d_name, ".."))
			r = fn(e->d_name, arg);
	closedir(d);
	return r;
}
#endif

void
warn(const char *fmt, ...)
{
	char buf[1024];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
	va_end(ap);
	if (n > (int)sizeof(buf) - 2)
		n = sizeof(buf) - 2;
	buf[n++] = '\n';
	write(2, "pkg: ", 5);
	write(2, buf, n);
}

void
say(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vdprintf(1, fmt, ap);
	va_end(ap);
}

void *
xmalloc(size_t n)
{
	void *p = malloc(n ? n : 1);

	if (!p) {
		warn("out of memory");
		exit(2);
	}
	return p;
}

void *
xrealloc(void *p, size_t n)
{
	if (!(p = realloc(p, n ? n : 1))) {
		warn("out of memory");
		exit(2);
	}
	return p;
}

char *
xstrdup(const char *s)
{
	return strcpy(xmalloc(strlen(s) + 1), s);
}

char *
xsprintf(const char *fmt, ...)
{
	va_list ap;
	char *p;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(NULL, 0, fmt, ap);
	va_end(ap);
	p = xmalloc(n + 1);
	va_start(ap, fmt);
	vsnprintf(p, n + 1, fmt, ap);
	va_end(ap);
	return p;
}

char *
rootpath(const char *path)
{
	return xsprintf("%s%s", root, path);
}

char *
readfile(const char *path, size_t *len, size_t max)
{
	struct stat st;
	char *buf;
	ssize_t n;
	size_t got = 0;
	int fd;

	if ((fd = open(path, O_RDONLY)) < 0)
		return NULL;
	if (fstat(fd, &st) < 0 || (size_t)st.st_size > max) {
		if (st.st_size > 0 && (size_t)st.st_size > max)
			errno = EFBIG;
		close(fd);
		return NULL;
	}
	buf = xmalloc(st.st_size + 1);
	while (got < (size_t)st.st_size && (n = read(fd, buf + got, st.st_size - got)) > 0)
		got += n;
	close(fd);
	buf[got] = 0;
	if (len)
		*len = got;
	return buf;
}

int
writefile(const char *path, const char *data, size_t n)
{
	char *tmp = xsprintf("%s.tmp%d", path, (int)getpid());
	ssize_t w = -1;
	int fd, saved;

	if ((fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644)) >= 0) {
		w = write(fd, data, n);
		if (close(fd) < 0)
			w = -1;
	}
	if (w != (ssize_t)n || rename(tmp, path) < 0) {
		saved = errno;
		unlink(tmp);
		free(tmp);
		errno = saved ? saved : EIO;
		return -1;
	}
	free(tmp);
	return 0;
}

char *
nextline(char **p)
{
	char *s = *p, *e;

	if (!s || !*s)
		return NULL;
	if ((e = strchr(s, '\n'))) {
		*e = 0;
		*p = e + 1;
	} else
		*p = s + strlen(s);
	return s;
}

int
splittab(char *line, char **f, int max)
{
	int n = 0;

	while (n < max) {
		f[n++] = line;
		if (!(line = strchr(line, '\t')))
			break;
		*line++ = 0;
	}
	return n;
}

int
mkdirs(const char *path, mode_t mode, void (*made)(const char *path, void *arg), void *arg)
{
	char *p = xstrdup(path), *s;
	struct stat st;
	int r = 0;

	for (s = p + 1; !r; s++) {
		int end = !*s;

		if (*s != '/' && !end)
			continue;
		*s = 0;
		if (stat(p, &st) < 0) {
			if (mkdir(p, mode) < 0)
				r = -1;
			else if (made)
				made(p, arg);
		} else if (!S_ISDIR(st.st_mode)) {
			errno = ENOTDIR;
			r = -1;
		}
		if (end)
			break;
		*s = '/';
	}
	free(p);
	return r;
}

int
validname(const char *s)
{
	const char *p;

	if (!((*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9')))
		return 0;
	for (p = s; *p; p++)
		if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || strchr("+-._", *p)))
			return 0;
	return p - s <= 64 && !(p - s > 4 && !strcmp(p - 4, ".pkg"));	/* that's a file */
}

int
validversion(const char *s)
{
	const char *p;

	for (p = s; *p; p++)
		if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
		    (*p >= '0' && *p <= '9') || strchr("+-._~", *p)))
			return 0;
	return p > s && p - s <= 64;
}

const char *
validpath(const char *s)
{
	const char *p, *c;

	if (!*s)
		return "empty name";
	if (strlen(s) > 255)
		return "name longer than 255 bytes";
	if (*s == '/')
		return "absolute path";
	if (s[strlen(s) - 1] == '/')
		return "empty, . or .. in path";
	for (p = s; *p; p++)
		if ((unsigned char)*p < 0x20 || *p == 0x7f)
			return "control character in name";
	for (c = s; *c; c = *p ? p + 1 : p) {
		size_t n;

		p = strchr(c, '/');
		if (!p)
			p = c + strlen(c);
		n = p - c;
		if (n == 0 || (n == 1 && c[0] == '.') || (n == 2 && c[0] == '.' && c[1] == '.'))
			return "empty, . or .. in path";
	}
	if (!strncmp(s, &PKGDIR[1], strlen(PKGDIR) - 1) &&
	    (s[strlen(PKGDIR) - 1] == '/' || !s[strlen(PKGDIR) - 1]))
		return "inside pkg's own directory";
	return NULL;
}

/* Debian's ordering: for each character, ~ first, then the end, then letters, then
   everything else */
static int
order(int c)
{
	if (c == '~')
		return -1;
	if (!c || (c >= '0' && c <= '9'))
		return 0;
	if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))
		return c;
	return c + 256;
}

#define ISDIGIT(c)	((c) >= '0' && (c) <= '9')

int
vercmp(const char *a, const char *b)
{
	while (*a || *b) {
		int first = 0;

		while ((*a && !ISDIGIT(*a)) || (*b && !ISDIGIT(*b))) {
			int ca = order(*a), cb = order(*b);

			if (ca != cb)
				return ca < cb ? -1 : 1;
			a++, b++;
		}
		while (*a == '0')
			a++;
		while (*b == '0')
			b++;
		while (ISDIGIT(*a) && ISDIGIT(*b)) {
			if (!first)
				first = *a - *b;
			a++, b++;
		}
		if (ISDIGIT(*a))
			return 1;
		if (ISDIGIT(*b))
			return -1;
		if (first)
			return first < 0 ? -1 : 1;
	}
	return 0;
}

int
hashfile(const char *path, char out[65])
{
	static unsigned char buf[8192];
	unsigned char d[32];
	sha256_ctx c;
	ssize_t n;
	int fd;

	if ((fd = open(path, O_RDONLY)) < 0)
		return -1;
	sha256_init(&c);
	while ((n = read(fd, buf, sizeof(buf))) > 0)
		sha256_update(&c, buf, n);
	close(fd);
	if (n < 0)
		return -1;
	sha256_final(&c, d);
	hex(d, 32, out);
	return 0;
}

static char *lockpath;

int
lock(void)
{
	char *dir = rootpath(PKGDIR), buf[16];
	int fd, tries;

	lockpath = rootpath(PKGDIR "/lock");
	if (mkdirs(dir, 0755, NULL, NULL) < 0) {
		warn("can't make %s: %s", dir, strerror(errno));
		free(dir);
		return -1;
	}
	free(dir);
	for (tries = 0; tries < 2; tries++) {
		char *old;
		long pid;

		if ((fd = open(lockpath, O_WRONLY | O_CREAT | O_EXCL, 0644)) >= 0) {
			snprintf(buf, sizeof(buf), "%d\n", (int)getpid());
			write(fd, buf, strlen(buf));
			close(fd);
			return 0;
		}
		if (errno != EEXIST)
			break;
		old = readfile(lockpath, NULL, 64);
		pid = old ? strtol(old, NULL, 10) : 0;
		free(old);
		if (pid == getpid())
			return 0;	/* taken before an upgrade ran the new pkg in its place */
		if (pid > 0 && (kill(pid, 0) == 0 || errno == EPERM)) {
			warn("another pkg is running (process %ld)", pid);
			return -1;
		}
		unlink(lockpath);	/* left by a pkg that's gone */
	}
	warn("can't make %s: %s", lockpath, strerror(errno));
	return -1;
}

void
unlock(void)
{
	if (lockpath)
		unlink(lockpath);
}
