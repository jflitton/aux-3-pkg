/*
 * Ed25519 signature verification (RFC 8032), after TweetNaCl (Bernstein, van Gastel,
 * Janssen, Lange, Schwabe, Smetsers; public domain). Field elements are 16 limbs of 16
 * bits held in 64-bit integers. Only public data passes through here, so nothing needs
 * to run in constant time.
 *
 * The limbs fit in 32 bits whenever two are multiplied, so the products are written as
 * 32x32->64 multiplications, which the 68020 does in one instruction (mul.l with a
 * register pair) instead of calling libgcc's 64-bit multiply.
 */
#include "ed25519.h"
#include "sha.h"

typedef int64_t gf[16];

/* The curve's d and 2d, the base point (X, Y), and sqrt(-1), computed from their
   definitions (d = -121665/121666, y = 4/5, x even) rather than copied. */
static const gf gf0, gf1 = { 1 };
static const gf D = {
	0x78a3, 0x1359, 0x4dca, 0x75eb, 0xd8ab, 0x4141, 0x0a4d, 0x0070,
	0xe898, 0x7779, 0x4079, 0x8cc7, 0xfe73, 0x2b6f, 0x6cee, 0x5203,
};
static const gf D2 = {
	0xf159, 0x26b2, 0x9b94, 0xebd6, 0xb156, 0x8283, 0x149a, 0x00e0,
	0xd130, 0xeef3, 0x80f2, 0x198e, 0xfce7, 0x56df, 0xd9dc, 0x2406,
};
static const gf X = {
	0xd51a, 0x8f25, 0x2d60, 0xc956, 0xa7b2, 0x9525, 0xc760, 0x692c,
	0xdc5c, 0xfdd6, 0xe231, 0xc0a4, 0x53fe, 0xcd6e, 0x36d3, 0x2169,
};
static const gf Y = {
	0x6658, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666,
	0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666,
};
static const gf I = {
	0xa0b0, 0x4a0e, 0x1b27, 0xc4ee, 0xe478, 0xad2f, 0x1806, 0x2f43,
	0xd7a7, 0x3dfb, 0x0099, 0x2b4d, 0xdf0b, 0x4fc1, 0x2480, 0x2b83,
};

/* the group order, little-endian */
static const unsigned char L[32] = {
	0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58, 0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9,
	0xde, 0x14, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x10,
};

static void
set(gf r, const gf a)
{
	memcpy(r, a, sizeof(gf));
}

/* each limb to 16 bits, the excess carried up; out of the top, 2^256 = 38 */
static void
carry(gf o)
{
	int64_t c;
	int i;

	for (i = 0; i < 16; i++) {
		c = o[i] >> 16;
		o[i] &= 0xffff;
		if (i < 15)
			o[i + 1] += c;
		else
			o[0] += 38 * c;
	}
}

/* if b, swap p and q */
static void
sel(gf p, gf q, int b)
{
	int64_t t, m = -(int64_t)b;
	int i;

	for (i = 0; i < 16; i++) {
		t = m & (p[i] ^ q[i]);
		p[i] ^= t;
		q[i] ^= t;
	}
}

/* fully reduced mod p = 2^255 - 19, as 32 bytes little-endian */
static void
pack25519(unsigned char *o, const gf n)
{
	gf m, t;
	int i, j, b;

	set(t, n);
	carry(t);
	carry(t);
	carry(t);
	for (j = 0; j < 2; j++) {
		m[0] = t[0] - 0xffed;
		for (i = 1; i < 15; i++) {
			m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1);
			m[i - 1] &= 0xffff;
		}
		m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
		b = (m[15] >> 16) & 1;
		m[14] &= 0xffff;
		sel(t, m, 1 - b);
	}
	for (i = 0; i < 16; i++) {
		o[2 * i] = t[i] & 0xff;
		o[2 * i + 1] = t[i] >> 8;
	}
}

static int
neq25519(const gf a, const gf b)
{
	unsigned char c[32], d[32];

	pack25519(c, a);
	pack25519(d, b);
	return memcmp(c, d, 32) != 0;
}

static int
par25519(const gf a)
{
	unsigned char d[32];

	pack25519(d, a);
	return d[0] & 1;
}

static void
unpack25519(gf o, const unsigned char *n)
{
	int i;

	for (i = 0; i < 16; i++)
		o[i] = n[2 * i] + ((int64_t)n[2 * i + 1] << 8);
	o[15] &= 0x7fff;
}

static void
A(gf o, const gf a, const gf b)
{
	int i;

	for (i = 0; i < 16; i++)
		o[i] = a[i] + b[i];
}

static void
Z(gf o, const gf a, const gf b)
{
	int i;

	for (i = 0; i < 16; i++)
		o[i] = a[i] - b[i];
}

static void
M(gf o, const gf a, const gf b)
{
	int64_t t[31];
	int i, j;

	memset(t, 0, sizeof(t));
	for (i = 0; i < 16; i++) {
		int32_t ai = (int32_t)a[i];

		for (j = 0; j < 16; j++)
			t[i + j] += (int64_t)ai * (int32_t)b[j];
	}
	for (i = 0; i < 15; i++)
		t[i] += 38 * t[i + 16];
	for (i = 0; i < 16; i++)
		o[i] = t[i];
	carry(o);
	carry(o);
}

static void
S(gf o, const gf a)
{
	M(o, a, a);
}

/* i^(p-2) */
static void
inv25519(gf o, const gf i)
{
	gf c;
	int a;

	set(c, i);
	for (a = 253; a >= 0; a--) {
		S(c, c);
		if (a != 2 && a != 4)
			M(c, c, i);
	}
	set(o, c);
}

/* i^((p-5)/8) */
static void
pow2523(gf o, const gf i)
{
	gf c;
	int a;

	set(c, i);
	for (a = 250; a >= 0; a--) {
		S(c, c);
		if (a != 1)
			M(c, c, i);
	}
	set(o, c);
}

/* p += q, extended coordinates (X, Y, Z, T) */
static void
add(gf p[4], gf q[4])
{
	gf a, b, c, d, t, e, f, g, h;

	Z(a, p[1], p[0]);
	Z(t, q[1], q[0]);
	M(a, a, t);
	A(b, p[0], p[1]);
	A(t, q[0], q[1]);
	M(b, b, t);
	M(c, p[3], q[3]);
	M(c, c, D2);
	M(d, p[2], q[2]);
	A(d, d, d);
	Z(e, b, a);
	Z(f, d, c);
	A(g, d, c);
	A(h, b, a);
	M(p[0], e, f);
	M(p[1], h, g);
	M(p[2], g, f);
	M(p[3], e, h);
}

static void
cswap(gf p[4], gf q[4], int b)
{
	int i;

	for (i = 0; i < 4; i++)
		sel(p[i], q[i], b);
}

static void
pack(unsigned char *r, gf p[4])
{
	gf tx, ty, zi;

	inv25519(zi, p[2]);
	M(tx, p[0], zi);
	M(ty, p[1], zi);
	pack25519(r, ty);
	r[31] ^= par25519(tx) << 7;
}

/* p = s * q (s 32 bytes, little-endian); q is changed */
static void
scalarmult(gf p[4], gf q[4], const unsigned char *s)
{
	int i;

	set(p[0], gf0);
	set(p[1], gf1);
	set(p[2], gf1);
	set(p[3], gf0);
	for (i = 255; i >= 0; i--) {
		int b = (s[i / 8] >> (i & 7)) & 1;

		cswap(p, q, b);
		add(q, p);
		add(p, p);
		cswap(p, q, b);
	}
}

static void
scalarbase(gf p[4], const unsigned char *s)
{
	gf q[4];

	set(q[0], X);
	set(q[1], Y);
	set(q[2], gf1);
	M(q[3], X, Y);
	scalarmult(p, q, s);
}

/* r = minus the point whose encoding is p; -1 if p isn't a point */
static int
unpackneg(gf r[4], const unsigned char p[32])
{
	gf t, chk, num, den, den2, den4, den6;

	set(r[2], gf1);
	unpack25519(r[1], p);
	S(num, r[1]);
	M(den, num, D);
	Z(num, num, r[2]);
	A(den, r[2], den);

	S(den2, den);
	S(den4, den2);
	M(den6, den4, den2);
	M(t, den6, num);
	M(t, t, den);

	pow2523(t, t);
	M(t, t, num);
	M(t, t, den);
	M(t, t, den);
	M(r[0], t, den);

	S(chk, r[0]);
	M(chk, chk, den);
	if (neq25519(chk, num))
		M(r[0], r[0], I);
	S(chk, r[0]);
	M(chk, chk, den);
	if (neq25519(chk, num))
		return -1;
	if (par25519(r[0]) == (p[31] >> 7))
		Z(r[0], gf0, r[0]);
	M(r[3], r[0], r[1]);
	return 0;
}

/* r = x mod L, x 64 bytes little-endian (a SHA-512 hash) */
static void
reduce(unsigned char r[32], const unsigned char h[64])
{
	int64_t x[64], c;
	int i, j;

	for (i = 0; i < 64; i++)
		x[i] = h[i];
	for (i = 63; i >= 32; i--) {
		c = 0;
		for (j = i - 32; j < i - 12; j++) {
			x[j] += c - 16 * x[i] * L[j - (i - 32)];
			c = (x[j] + 128) >> 8;
			x[j] -= c * 256;
		}
		x[j] += c;
		x[i] = 0;
	}
	c = 0;
	for (j = 0; j < 32; j++) {
		x[j] += c - (x[31] >> 4) * L[j];
		c = x[j] >> 8;
		x[j] &= 255;
	}
	for (j = 0; j < 32; j++)
		x[j] -= c * L[j];
	for (i = 0; i < 32; i++) {
		x[i + 1] += x[i] >> 8;
		r[i] = x[i] & 255;
	}
}

/* s < L, as RFC 8032 requires (otherwise signatures are malleable) */
static int
below_l(const unsigned char *s)
{
	int i;

	for (i = 31; i >= 0; i--) {
		if (s[i] < L[i])
			return 1;
		if (s[i] > L[i])
			return 0;
	}
	return 0;
}

int
ed25519_verify(const unsigned char sig[64], const void *msg, size_t n,
	const unsigned char pk[32])
{
	gf p[4], q[4];
	unsigned char h[64], k[32], t[32];
	sha512_ctx c;

	if (!below_l(sig + 32) || unpackneg(q, pk))
		return -1;
	sha512_init(&c);
	sha512_update(&c, sig, 32);
	sha512_update(&c, pk, 32);
	sha512_update(&c, msg, n);
	sha512_final(&c, h);
	reduce(k, h);
	scalarmult(p, q, k);
	scalarbase(q, sig + 32);
	add(p, q);
	pack(t, p);
	return memcmp(t, sig, 32) ? -1 : 0;
}
