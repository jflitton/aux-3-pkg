/*
 * The part of libc that pkg uses, written for it: memory and strings, a heap on brk, and
 * formatted output straight to a file descriptor (no stdio).
 */
#include "aux.h"

int errno;
char **environ;

int __brk(void *addr);
extern char _end[];		/* end of .bss, from rt/aux.ld */

void
exit(int status)
{
	_exit(status);
}

/* Not the System V time call (trap #0, 13), which fails for us: gettimeofday works */
time_t
time(time_t *t)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);
	if (t)
		*t = tv.tv_sec;
	return tv.tv_sec;
}

static char *curbrk = _end;

void *
sbrk(long n)
{
	char *old = curbrk;

	if (n && __brk(curbrk + n) < 0)
		return (void *)-1;
	curbrk += n;
	return old;
}

char *
getenv(const char *name)
{
	size_t n = strlen(name);
	char **e;

	for (e = environ; e && *e; e++)
		if (!strncmp(*e, name, n) && (*e)[n] == '=')
			return *e + n + 1;
	return NULL;
}

/* As A/UX's: a terminal answers TCGETA (struct termio is 18 bytes) */
int
isatty(int fd)
{
	char termio[18];

	return ioctl(fd, 0x40125401, termio) >= 0;
}

/*
 * Heap: first fit from a free list kept in address order, so that freeing can merge a
 * block with its neighbours. Each block starts with its size, header included, and is
 * a multiple of 8 bytes.
 */
struct block {
	size_t size;
	struct block *next;	/* while free */
};

#define HDR		8
#define GROW		32768
static struct block *freelist;

static void
release(struct block *b)
{
	struct block **pp = &freelist, *p;

	while ((p = *pp) && p < b)
		pp = &p->next;
	b->next = p;
	if (p && (char *)b + b->size == (char *)p) {
		b->size += p->size;
		b->next = p->next;
	}
	*pp = b;
	if (pp != &freelist) {
		struct block *prev = (struct block *)((char *)pp -
			offsetof(struct block, next));

		if ((char *)prev + prev->size == (char *)b) {
			prev->size += b->size;
			prev->next = b->next;
		}
	}
}

void *
malloc(size_t n)
{
	struct block **pp, *b;
	size_t need = (n + HDR + 7) & ~(size_t)7;

	if (n > 0x7fff0000) {
		errno = ENOMEM;
		return NULL;
	}
	for (;;) {
		for (pp = &freelist; (b = *pp); pp = &b->next) {
			if (b->size < need)
				continue;
			if (b->size - need >= 16) {
				struct block *rest = (struct block *)((char *)b + need);

				rest->size = b->size - need;
				rest->next = b->next;
				*pp = rest;
				b->size = need;
			} else
				*pp = b->next;
			return (char *)b + HDR;
		}
		/* keep the break 8-aligned, then grow by at least GROW */
		{
			size_t pad = -(size_t)(unsigned long)curbrk & 7;
			size_t grow = need > GROW ? need : GROW;

			if (pad && sbrk(pad) == (void *)-1)
				return NULL;
			b = sbrk(grow);
			if (b == (void *)-1) {
				errno = ENOMEM;
				return NULL;
			}
			b->size = grow;
			release(b);
		}
	}
}

void
free(void *p)
{
	if (p)
		release((struct block *)((char *)p - HDR));
}

void *
calloc(size_t n, size_t size)
{
	void *p;

	if (size && n > 0x7fff0000 / size) {
		errno = ENOMEM;
		return NULL;
	}
	if ((p = malloc(n * size)))
		memset(p, 0, n * size);
	return p;
}

void *
realloc(void *p, size_t n)
{
	size_t have;
	void *q;

	if (!p)
		return malloc(n);
	have = ((struct block *)((char *)p - HDR))->size - HDR;
	if (n <= have)
		return p;
	if (!(q = malloc(n)))
		return NULL;
	memcpy(q, p, have);
	free(p);
	return q;
}

void *
memcpy(void *dst, const void *src, size_t n)
{
	char *d = dst;
	const char *s = src;

	while (n--)
		*d++ = *s++;
	return dst;
}

void *
memmove(void *dst, const void *src, size_t n)
{
	char *d = dst;
	const char *s = src;

	if (d <= s || d >= s + n)
		return memcpy(dst, src, n);
	while (n--)
		d[n] = s[n];
	return dst;
}

void *
memset(void *dst, int c, size_t n)
{
	unsigned char *d = dst;

	while (n--)
		*d++ = c;
	return dst;
}

int
memcmp(const void *a, const void *b, size_t n)
{
	const unsigned char *x = a, *y = b;

	for (; n; n--, x++, y++)
		if (*x != *y)
			return *x - *y;
	return 0;
}

void *
memchr(const void *s, int c, size_t n)
{
	const unsigned char *p = s;

	for (; n; n--, p++)
		if (*p == (unsigned char)c)
			return (void *)p;
	return NULL;
}

size_t
strlen(const char *s)
{
	const char *p = s;

	while (*p)
		p++;
	return p - s;
}

int
strcmp(const char *a, const char *b)
{
	while (*a && *a == *b)
		a++, b++;
	return (unsigned char)*a - (unsigned char)*b;
}

int
strncmp(const char *a, const char *b, size_t n)
{
	for (; n; n--, a++, b++) {
		if (*a != *b)
			return (unsigned char)*a - (unsigned char)*b;
		if (!*a)
			break;
	}
	return 0;
}

char *
strchr(const char *s, int c)
{
	for (;; s++) {
		if (*s == (char)c)
			return (char *)s;
		if (!*s)
			return NULL;
	}
}

char *
strstr(const char *s, const char *find)
{
	size_t n = strlen(find);

	for (; *s; s++)
		if (!strncmp(s, find, n))
			return (char *)s;
	return n ? NULL : (char *)s;
}

char *
strrchr(const char *s, int c)
{
	const char *last = NULL;

	for (;; s++) {
		if (*s == (char)c)
			last = s;
		if (!*s)
			return (char *)last;
	}
}

char *
strcpy(char *dst, const char *src)
{
	char *d = dst;

	while ((*d++ = *src++))
		;
	return dst;
}

char *
strdup(const char *s)
{
	size_t n = strlen(s) + 1;
	char *p = malloc(n);

	return p ? memcpy(p, s, n) : NULL;
}

unsigned long
strtoul(const char *s, char **end, int base)
{
	unsigned long v = 0;
	const char *p = s;
	int d;

	while (*p == ' ' || *p == '\t')
		p++;
	if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
		p += 2;
		base = 16;
	} else if (base == 0)
		base = *p == '0' ? 8 : 10;
	for (;; p++) {
		if (*p >= '0' && *p <= '9')
			d = *p - '0';
		else if (*p >= 'a' && *p <= 'z')
			d = *p - 'a' + 10;
		else if (*p >= 'A' && *p <= 'Z')
			d = *p - 'A' + 10;
		else
			break;
		if (d >= base)
			break;
		v = v * base + d;
	}
	if (end)
		*end = (char *)p;
	return v;
}

long
strtol(const char *s, char **end, int base)
{
	while (*s == ' ' || *s == '\t')
		s++;
	if (*s == '-')
		return -(long)strtoul(s + 1, end, base);
	return strtoul(*s == '+' ? s + 1 : s, end, base);
}

static const struct {
	short err;
	const char *msg;
} errs[] = {
	{ EPERM, "Not owner" },
	{ ENOENT, "No such file or directory" },
	{ EINTR, "Interrupted system call" },
	{ EIO, "I/O error" },
	{ EBADF, "Bad file number" },
	{ EAGAIN, "Resource temporarily unavailable" },
	{ ENOMEM, "Not enough memory" },
	{ EACCES, "Permission denied" },
	{ EEXIST, "File exists" },
	{ EXDEV, "Cross-device link" },
	{ ENOTDIR, "Not a directory" },
	{ EISDIR, "Is a directory" },
	{ EINVAL, "Invalid argument" },
	{ ETXTBSY, "Text file busy" },
	{ EFBIG, "File too large" },
	{ ENOSPC, "No space left on device" },
	{ EROFS, "Read-only file system" },
	{ EPIPE, "Broken pipe" },
	{ EWOULDBLOCK, "Operation would block" },
	{ EINPROGRESS, "Operation now in progress" },
	{ ENETDOWN, "Network is down" },
	{ ENETUNREACH, "Network is unreachable" },
	{ ECONNRESET, "Connection reset by peer" },
	{ ETIMEDOUT, "Connection timed out" },
	{ ECONNREFUSED, "Connection refused" },
	{ ELOOP, "Too many levels of symbolic links" },
	{ ENAMETOOLONG, "File name too long" },
	{ EHOSTDOWN, "Host is down" },
	{ EHOSTUNREACH, "No route to host" },
	{ ENOTEMPTY, "Directory not empty" },
};

char *
strerror(int err)
{
	static char buf[24];
	size_t i;

	for (i = 0; i < sizeof(errs) / sizeof(errs[0]); i++)
		if (errs[i].err == err)
			return (char *)errs[i].msg;
	snprintf(buf, sizeof(buf), "Error %d", err);
	return buf;
}

/*
 * Formatting: %d %i %u %x %X %o %c %s %p %%, with flags '-' and '0', a width, a
 * precision (strings only), either given as '*', and the size letters l and z, which
 * change nothing here (int, long and size_t are all 32 bits).
 */
struct out {
	char *buf;
	size_t size, len;
};

static void
put(struct out *o, char c)
{
	if (o->len + 1 < o->size)
		o->buf[o->len] = c;
	o->len++;
}

static void
pad(struct out *o, char c, int n)
{
	while (n-- > 0)
		put(o, c);
}

int
vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
	struct out o = { buf, size, 0 };
	char num[12];

	for (; *fmt; fmt++) {
		int left = 0, zero = 0, width = 0, prec = -1, base, len, neg;
		unsigned long v;
		const char *s;
		size_t start = o.len;

		if (*fmt != '%') {
			put(&o, *fmt);
			continue;
		}
		for (fmt++; *fmt == '-' || *fmt == '0'; fmt++)
			if (*fmt == '-')
				left = 1;
			else
				zero = 1;
		if (*fmt == '*') {
			width = va_arg(ap, int);
			fmt++;
		} else
			while (*fmt >= '0' && *fmt <= '9')
				width = width * 10 + *fmt++ - '0';
		if (*fmt == '.') {
			prec = 0;
			if (*++fmt == '*') {
				prec = va_arg(ap, int);
				fmt++;
			} else
				while (*fmt >= '0' && *fmt <= '9')
					prec = prec * 10 + *fmt++ - '0';
		}
		while (*fmt == 'l' || *fmt == 'z')
			fmt++;
		switch (*fmt) {
		case 'c':
			pad(&o, ' ', left ? 0 : width - 1);
			put(&o, va_arg(ap, int));
			pad(&o, ' ', left ? width - 1 : 0);
			continue;
		case 's':
			if (!(s = va_arg(ap, const char *)))
				s = "(null)";
			for (len = 0; s[len] && (prec < 0 || len < prec); len++)
				;
			pad(&o, ' ', left ? 0 : width - len);
			while (len-- > 0)
				put(&o, *s++);
			pad(&o, ' ', left ? width - (int)(o.len - start) : 0);
			continue;
		case 'd':
		case 'i': {
			long d = va_arg(ap, long);

			neg = d < 0;
			v = neg ? -(unsigned long)d : (unsigned long)d;
			base = 10;
			break;
		}
		case 'u':
			v = va_arg(ap, unsigned long), neg = 0, base = 10;
			break;
		case 'o':
			v = va_arg(ap, unsigned long), neg = 0, base = 8;
			break;
		case 'p':
		case 'x':
		case 'X':
			v = va_arg(ap, unsigned long), neg = 0, base = 16;
			break;
		case '%':
			put(&o, '%');
			continue;
		default:
			put(&o, '%');
			if (!*fmt)
				fmt--;
			else
				put(&o, *fmt);
			continue;
		}
		len = 0;
		do {
			int dig = v % base;

			num[len++] = dig < 10 ? '0' + dig :
				(*fmt == 'X' ? 'A' : 'a') + dig - 10;
			v /= base;
		} while (v);
		if (zero && !left) {
			if (neg)
				put(&o, '-');
			pad(&o, '0', width - len - neg);
		} else {
			pad(&o, ' ', left ? 0 : width - len - neg);
			if (neg)
				put(&o, '-');
		}
		while (len)
			put(&o, num[--len]);
		if (left)
			pad(&o, ' ', width - (int)(o.len - start));
	}
	if (size)
		buf[o.len < size ? o.len : size - 1] = 0;
	return o.len;
}

int
snprintf(char *buf, size_t n, const char *fmt, ...)
{
	va_list ap;
	int r;

	va_start(ap, fmt);
	r = vsnprintf(buf, n, fmt, ap);
	va_end(ap);
	return r;
}

int
vdprintf(int fd, const char *fmt, va_list ap)
{
	char buf[512], *big = NULL;
	va_list ap2;
	int n;

	va_copy(ap2, ap);
	n = vsnprintf(buf, sizeof(buf), fmt, ap);
	if (n >= (int)sizeof(buf) && (big = malloc(n + 1)))
		vsnprintf(big, n + 1, fmt, ap2);
	va_end(ap2);
	write(fd, big ? big : buf, big || n < (int)sizeof(buf) ? n : (int)sizeof(buf) - 1);
	free(big);
	return n;
}

int
dprintf(int fd, const char *fmt, ...)
{
	va_list ap;
	int r;

	va_start(ap, fmt);
	r = vdprintf(fd, fmt, ap);
	va_end(ap);
	return r;
}
