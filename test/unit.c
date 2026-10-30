/*
 * Unit tests of pkg's pieces against test vectors from test/gen-unit.py: SHA-256 and
 * SHA-512, Ed25519 verification, gzip. Runs on the host and on A/UX (make test-unit,
 * make test-aux-unit); prints a line per failure and a summary.
 */
#include "pkg.h"
#include "ed25519.h"
#include "gunzip.h"
#include "unit-vectors.h"

const char *root = "";
int verbose;
static int passed, failed;

static void
check(int ok, const char *what, int i)
{
	if (ok) {
		passed++;
		return;
	}
	failed++;
	dprintf(1, "FAIL %s %d\n", what, i);
}

static long
ms(void)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);
	return tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static void
test_sha(void)
{
	unsigned char buf[1000], d[64];
	char h[129];
	unsigned i, j;

	for (i = 0; i < sizeof(shas) / sizeof(shas[0]); i++) {
		sha256_ctx c;
		sha512_ctx c5;
		int n = shas[i].len;

		for (j = 0; j < (unsigned)n; j++)
			buf[j] = shamsg[i][j];
		sha256_init(&c);
		/* in uneven pieces, to exercise the buffering */
		for (j = 0; j < (unsigned)n; j += 7)
			sha256_update(&c, buf + j, n - j < 7 ? n - j : 7);
		sha256_final(&c, d);
		hex(d, 32, h);
		check(!strcmp(h, shas[i].sha256), "sha256", i);
		sha512_init(&c5);
		sha512_update(&c5, buf, n);
		sha512_final(&c5, d);
		hex(d, 64, h);
		check(!strcmp(h, shas[i].sha512), "sha512", i);
	}
}

static void
test_ed25519(void)
{
	unsigned i;
	long t0;

	for (i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++) {
		int r;

		t0 = ms();
		r = ed25519_verify(sigs[i].sig, sigs[i].msg, sigs[i].n, sigs[i].pk);
		check((r == 0) == sigs[i].good, "ed25519", i);
		if (i == 0)
			dprintf(1, "ed25519_verify: %ld ms\n", ms() - t0);
	}
}

struct gzout {
	sha256_ctx c;
	long n;
};

static int
gzsink(void *arg, const unsigned char *data, size_t n)
{
	struct gzout *o = arg;

	sha256_update(&o->c, data, n);
	o->n += n;
	return 0;
}

static void
test_gunzip(void)
{
	const char *path = "/tmp/unit.gz";
	unsigned i;

	for (i = 0; i < sizeof(gzs) / sizeof(gzs[0]); i++) {
		struct gzout o;
		unsigned char d[32];
		char h[65];
		const char *err = NULL;
		long t0;
		int fd, r;

		fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		write(fd, gzs[i].gz, gzs[i].len);
		close(fd);
		sha256_init(&o.c);
		o.n = 0;
		fd = open(path, O_RDONLY);
		t0 = ms();
		r = gunzip(fd, gzsink, &o, &err);
		t0 = ms() - t0;
		close(fd);
		sha256_final(&o.c, d);
		hex(d, 32, h);
		if (gzs[i].good) {
			check(r == 0 && !strcmp(h, gzs[i].sha256), "gunzip", i);
			if (r)
				dprintf(1, "  gunzip %d: %s\n", i, err);
			if (o.n > 100000)
				dprintf(1, "gunzip: %ld bytes in %ld ms\n", o.n, t0);
		} else {
			check(r == -1 && err, "gunzip damaged", i);
		}
	}
	unlink(path);
}

/* versions in increasing order, Debian's examples and ours */
static const char *const versions[] = {
	"0", "0.9", "1.0~beta1", "1.0~rc1", "1.0", "1.0a", "1.0-1", "1.0-2", "1.0.1",
	"1.2", "1.9", "1.10", "1.10.0", "2", "10", "a", "b", "b1",
};

static void
test_versions(void)
{
	int n = sizeof(versions) / sizeof(versions[0]), i, j;

	for (i = 0; i < n; i++)
		for (j = 0; j < n; j++) {
			int c = vercmp(versions[i], versions[j]);

			check((c < 0) == (i < j) && (c == 0) == (i == j), versions[i], j);
		}
	check(vercmp("1.01", "1.1") == 0, "leading zeros", 0);
}

static void
test_paths(void)
{
	static const char *const good[] = {
		"usr/local/bin/x", "usr/local/lib/pkgx", "mac/bin/%Terminal", "a b/c d",
	};
	static const char *const bad[] = {
		"", "/usr/x", "usr//x", "usr/./x", "usr/../x", "..", "x/", "a\tb",
		"usr/local/lib/pkg", "usr/local/lib/pkg/installed/x",
	};
	unsigned i;

	for (i = 0; i < sizeof(good) / sizeof(good[0]); i++)
		check(!validpath(good[i]), good[i], i);
	for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
		check(validpath(bad[i]) != NULL, bad[i], i);
	check(validname("gcc") && validname("libg++") && validname("x11.fonts-1") &&
		!validname("Gcc") && !validname("-x") && !validname(""), "names", 0);
}

int
main(void)
{
	test_versions();
	test_paths();
	test_sha();
	test_ed25519();
	test_gunzip();
	dprintf(1, "unit: %d passed, %d failed\n", passed, failed);
	return failed != 0;
}
