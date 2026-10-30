/*
 * Exercises rt/ on A/UX: every syscall pkg uses, checked against facts about a stock
 * A/UX 3.1 disk where it can be, and the heap and formatting. Prints a line per check and
 * ends with "rt-test: N passed, M failed". Run by tools/aux-run.sh.
 */
#include "aux.h"

static int passed, failed;

static void
check(int ok, const char *what, const char *fmt, ...)
{
	char detail[200];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(detail, sizeof(detail), fmt, ap);
	va_end(ap);
	dprintf(1, "%s %s%s%s\n", ok ? "ok  " : "FAIL", what, *detail ? ": " : "", detail);
	if (ok)
		passed++;
	else
		failed++;
}

static long
ms_since(struct timeval *t0)
{
	struct timeval t;

	gettimeofday(&t, NULL);
	return (t.tv_sec - t0->tv_sec) * 1000 + (t.tv_usec - t0->tv_usec) / 1000;
}

static void
test_format(void)
{
	char buf[64];
	int n;

	n = snprintf(buf, sizeof(buf), "%5d|%-5d|%05d|%x|%X|%o|%c|%s|%-4s|%3s|%.2s|%%",
		42, -7, -42, 0xbeef, 0xbeef, 8, 'z', "hi", "ab", "x", "xyz");
	check(!strcmp(buf, "   42|-7   |-0042|beef|BEEF|10|z|hi|ab  |  x|xy|%"),
		"snprintf", "%s (%d)", buf, n);
	n = snprintf(buf, 4, "%s", "truncated");
	check(n == 9 && !strcmp(buf, "tru"), "snprintf truncates", "%d %s", n, buf);
	check(strtol("-123", NULL, 10) == -123 && strtoul("0x1F", NULL, 0) == 31 &&
		strtoul("0755", NULL, 0) == 0755, "strtol", "");
}

static void
test_stat(void)
{
	struct stat st;
	char buf[64];
	ssize_t n;
	int r;

	r = stat("/bin/sum", &st);
	check(r == 0 && st.st_size == 5560 &&
		st.st_mode == (S_IFREG | 0755) && st.st_uid == 2 && st.st_gid == 2,
		"stat /bin/sum", "size %ld mode %o uid %u/%ld gid %u/%ld ino %u/%lu nlink %d",
		st.st_size, st.st_mode, st.st_uid, st.st_uid32, st.st_gid, st.st_gid32,
		st.st_ino16, st.st_ino, st.st_nlink);
	r = lstat("/usr/bin/awk", &st);
	check(r == 0 && S_ISLNK(st.st_mode), "lstat symlink",
		"mode %o", st.st_mode);
	n = readlink("/usr/bin/awk", buf, sizeof(buf));
	check(n == 8 && !memcmp(buf, "/bin/awk", 8), "readlink", "%ld", n);
	errno = 0;
	r = stat("/no/such/file", &st);
	check(r < 0 && errno == ENOENT, "stat ENOENT",
		"errno %d", errno);
	r = stat("/etc", &st);
	check(r == 0 && S_ISDIR(st.st_mode), "stat /etc", "mode %o",
		st.st_mode);
}

static void
test_files(void)
{
	char path[32], path2[32], buf[1000], back[1000];
	struct stat st;
	struct utimbuf ut;
	int fd, i, ok, r;
	long total;

	snprintf(path, sizeof(path), "/tmp/rt.%d", getpid());
	snprintf(path2, sizeof(path2), "%s.2", path);
	for (i = 0; i < (int)sizeof(buf); i++)
		buf[i] = i * 7;
	umask(022);
	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0640);
	check(fd >= 0, "open O_CREAT", "%d errno %d", fd, errno);
	for (total = 0, i = 0; i < 10; i++)
		total += write(fd, buf, sizeof(buf));
	check(total == 10000, "write", "%ld", total);
	r = fsync(fd);
	check(r == 0, "fsync", "errno %d", errno);
	check(close(fd) == 0, "close", "");
	r = stat(path, &st);
	check(r == 0 && st.st_size == 10000 && (st.st_mode & 0777) == 0640, "stat new file",
		"size %ld mode %o", st.st_size, st.st_mode);
	fd = open(path, O_RDWR);
	check(lseek(fd, 0, SEEK_END) == 10000, "lseek SEEK_END", "");
	check(lseek(fd, 3000, SEEK_SET) == 3000 && read(fd, back, sizeof(back)) == 1000,
		"lseek, read", "");
	check(!memcmp(back, buf, sizeof(buf)), "read back", "");
	r = ftruncate(fd, 1234) || fstat(fd, &st);
	check(r == 0 && st.st_size == 1234, "ftruncate, fstat", "size %ld", st.st_size);
	close(fd);
	ut.actime = 1000000000;
	ut.modtime = 1100000000;
	r = utime(path, &ut) || stat(path, &st);
	check(r == 0 && st.st_mtime == 1100000000 && st.st_atime == 1000000000, "utime",
		"%ld %ld", st.st_atime, st.st_mtime);
	r = chmod(path, 0604) || stat(path, &st);
	check(r == 0 && (st.st_mode & 0777) == 0604, "chmod", "%o", st.st_mode);
	r = chown(path, 2, 3) || stat(path, &st);
	check(r == 0 && st.st_uid == 2 && st.st_gid == 3, "chown", "%u %u", st.st_uid,
		st.st_gid);
	r = rename(path, path2) || stat(path, &st) == 0 || stat(path2, &st);
	check(r == 0, "rename", "errno %d", errno);
	r = link(path2, path) || stat(path, &st);
	check(r == 0 && st.st_nlink == 2, "link", "nlink %d", st.st_nlink);
	r = unlink(path) || unlink(path2);
	ok = r == 0 && stat(path2, &st) < 0 && errno == ENOENT;
	check(ok, "unlink", "errno %d", errno);
	fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
	ok = fd >= 0 && open(path, O_WRONLY | O_CREAT | O_EXCL, 0644) < 0 && errno == EEXIST;
	check(ok, "O_EXCL", "errno %d", errno);
	close(fd);
	r = symlink("/etc/passwd", path2) || lstat(path2, &st);
	ok = r == 0 && S_ISLNK(st.st_mode);
	r = stat(path2, &st);
	check(ok && r == 0 && st.st_size == 406, "symlink", "errno %d size %ld", errno,
		st.st_size);
	unlink(path);
	unlink(path2);
	check(access("/etc/passwd", R_OK) == 0 && access("/nope", F_OK) < 0, "access", "");
}

static void
test_dirs(void)
{
	char buf[2048];
	struct stat st;
	long base = 0;
	int fd, n, r, entries = 0, inittab = 0, dots = 0;

	r = mkdir("/tmp/rtdir", 0755) || stat("/tmp/rtdir", &st);
	check(r == 0 && S_ISDIR(st.st_mode), "mkdir", "errno %d", errno);
	r = mkdir("/tmp/rtdir", 0755);
	check(r < 0 && errno == EEXIST, "mkdir EEXIST", "errno %d", errno);
	close(open("/tmp/rtdir/f", O_WRONLY | O_CREAT, 0644));
	r = rmdir("/tmp/rtdir");
	check(r < 0, "rmdir non-empty", "errno %d", errno);
	unlink("/tmp/rtdir/f");
	r = rmdir("/tmp/rtdir") || stat("/tmp/rtdir", &st) == 0;
	check(r == 0, "rmdir", "errno %d", errno);

	fd = open("/etc", O_RDONLY);
	while ((n = getdirent(fd, buf, sizeof(buf), &base)) > 0) {
		char *p = buf;

		while (p < buf + n) {
			struct direct *d = (struct direct *)p;

			if (d->d_reclen == 0)
				break;
			if (d->d_fileno) {
				entries++;
				if (d->d_namlen == 7 && !memcmp(d->d_name, "inittab", 8))
					inittab = 1;
				if (!strcmp(d->d_name, ".") || !strcmp(d->d_name, ".."))
					dots++;
			}
			p += d->d_reclen;
		}
	}
	close(fd);
	check(n == 0 && inittab && dots == 2 && entries > 100, "getdirent /etc",
		"%d entries, last %d, errno %d", entries, n, errno);
}

static void
test_heap(void)
{
	char *p[64], *big, *q;
	int i, j, ok = 1;

	for (i = 0; i < 64; i++) {
		p[i] = malloc(1 + i * 37);
		memset(p[i], i, 1 + i * 37);
	}
	for (i = 0; i < 64; i += 2)
		free(p[i]);
	for (i = 0; i < 64; i += 2) {
		p[i] = malloc(1 + i * 37);
		memset(p[i], i, 1 + i * 37);
	}
	for (i = 0; i < 64; i++)
		for (j = 0; j < 1 + i * 37; j++)
			if (p[i][j] != (char)i)
				ok = 0;
	check(ok, "malloc, free", "");
	q = strdup("realloc me");
	q = realloc(q, 100000);
	check(q && !strcmp(q, "realloc me"), "realloc", "");
	big = malloc(1 << 20);
	check(big != NULL, "malloc 1MB", "");
	if (big) {
		memset(big, 0x5a, 1 << 20);
		check(big[0] == 0x5a && big[(1 << 20) - 1] == 0x5a, "touch 1MB", "");
	}
	free(big);
	free(q);
	for (i = 0; i < 64; i++)
		free(p[i]);
}

static void
test_net(void)
{
	struct sockaddr_in sin;
	struct timeval t0, tv;
	long ms;
	int fd, r;

	fd = socket(AF_INET, SOCK_STREAM, 0);
	check(fd >= 0, "socket", "%d errno %d", fd, errno);
	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	sin.sin_port = htons(1);
	sin.sin_addr.s_addr = htonl(0x7f000001);
	r = connect(fd, (struct sockaddr *)&sin, sizeof(sin));
	check(r < 0 && errno == ECONNREFUSED, "connect 127.0.0.1:1 refused", "%d errno %d",
		r, errno);
	close(fd);
	check(signal(SIGPIPE, SIG_IGN) != (void (*)(int))-1, "signal SIGPIPE", "");

	gettimeofday(&t0, NULL);
	tv.tv_sec = 0;
	tv.tv_usec = 300000;
	r = select(0, NULL, NULL, NULL, &tv);
	ms = ms_since(&t0);
	check(r == 0 && ms >= 250, "select timeout", "%d after %ld ms", r, ms);
}

int
main(int argc, char **argv)
{
	struct timeval tv;
	time_t now = time(NULL);
	int r;

	check(argc == 2 && !strcmp(argv[1], "arg"), "argv", "argc %d argv[0] %s", argc,
		argv[0]);
	check(getenv("PATH") != NULL, "environ", "PATH=%s", getenv("PATH"));
	r = gettimeofday(&tv, NULL);
	check(r == 0 && tv.tv_sec - now <= 2 && now - tv.tv_sec <= 2 && now > 1700000000 &&
		tv.tv_usec >= 0 && tv.tv_usec < 1000000,
		"time, gettimeofday", "%ld %ld.%06ld", now, tv.tv_sec, tv.tv_usec);
	check(1, "getuid", "%d, pid %d", getuid(), getpid());
	test_format();
	test_stat();
	test_files();
	test_dirs();
	test_heap();
	test_net();
	dprintf(1, "rt-test: %d passed, %d failed\n", passed, failed);
	return failed != 0;
}
