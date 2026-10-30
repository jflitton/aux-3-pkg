/*
 * gzip decompression: RFC 1952's wrapper around RFC 1951's deflate. The structure
 * follows Mark Adler's puff.c (zlib license). Huffman codes are decoded with a table of
 * the next 9 bits, so most symbols take one lookup; longer codes go bit by bit as in
 * puff. Output goes to a 32KB ring, the deflate window, which is handed to the sink each
 * time it fills.
 */
#include "gunzip.h"

#define MAXBITS		15
#define FASTBITS	9
#define WSIZE		32768

struct huffman {
	short count[MAXBITS + 1];	/* codes of each length */
	short symbol[288];		/* symbols in code order */
	unsigned short fast[1 << FASTBITS];	/* length << 12 | symbol, or 0 */
};

struct state {
	int fd;
	unsigned char in[8192];
	size_t inpos, inlen;
	uint32_t bitbuf;
	int bitcnt;
	const char *err;
	unsigned char win[WSIZE];
	size_t wpos, flushed;
	uint32_t total, crc;
	gz_sink sink;
	void *arg;
	int stop;
};

static int
getbyte(struct state *s)
{
	if (s->inpos == s->inlen) {
		ssize_t n = read(s->fd, s->in, sizeof(s->in));

		if (n <= 0) {
			if (!s->err)
				s->err = n < 0 ? strerror(errno) : "file ends too soon";
			return -1;
		}
		s->inpos = 0;
		s->inlen = n;
	}
	return s->in[s->inpos++];
}

/* the next n bits (n <= 24), or 0 and s->err set */
static uint32_t
bits(struct state *s, int n)
{
	uint32_t v;

	while (s->bitcnt < n) {
		int b = getbyte(s);

		if (b < 0)
			return 0;
		s->bitbuf |= (uint32_t)b << s->bitcnt;
		s->bitcnt += 8;
	}
	v = s->bitbuf & ((1UL << n) - 1);
	s->bitbuf >>= n;
	s->bitcnt -= n;
	return v;
}

uint32_t
crc32(uint32_t crc, const unsigned char *p, size_t n)
{
	static uint32_t table[256];
	uint32_t c;
	int i, k;

	if (!table[1])
		for (i = 0; i < 256; i++) {
			for (c = i, k = 0; k < 8; k++)
				c = c & 1 ? 0xedb88320 ^ (c >> 1) : c >> 1;
			table[i] = c;
		}
	crc = ~crc;
	while (n--)
		crc = table[(crc ^ *p++) & 0xff] ^ (crc >> 8);
	return ~crc;
}

static void
flush(struct state *s)
{
	if (s->wpos > s->flushed && !s->stop) {
		s->crc = crc32(s->crc, s->win + s->flushed, s->wpos - s->flushed);
		s->stop = s->sink(s->arg, s->win + s->flushed, s->wpos - s->flushed);
	}
	s->flushed = s->wpos;
}

static void
out(struct state *s, int c)
{
	s->win[s->wpos++] = c;
	s->total++;
	if (s->wpos == WSIZE) {
		flush(s);
		s->wpos = s->flushed = 0;
	}
}

/* Builds h from the code lengths of n symbols. -1 if they're more than a code can have
   (incomplete codes are allowed: a missing code is an error only if it turns up). */
static int
construct(struct huffman *h, const short *length, int n)
{
	short offs[MAXBITS + 1];
	int sym, len, left, code, i;

	memset(h->count, 0, sizeof(h->count));
	memset(h->fast, 0, sizeof(h->fast));
	for (sym = 0; sym < n; sym++)
		h->count[length[sym]]++;
	if (h->count[0] == n)
		return 0;
	left = 1;
	for (len = 1; len <= MAXBITS; len++) {
		left = 2 * left - h->count[len];
		if (left < 0)
			return -1;
	}
	offs[1] = 0;
	for (len = 1; len < MAXBITS; len++)
		offs[len + 1] = offs[len] + h->count[len];
	for (sym = 0; sym < n; sym++)
		if (length[sym])
			h->symbol[offs[length[sym]]++] = sym;

	/* the fast table: each code of FASTBITS or fewer bits, reversed (deflate sends
	   codes from their top bit), at every index whose low bits are that code */
	code = 0;
	i = 0;
	for (len = 1; len <= FASTBITS; len++) {
		int k;

		for (k = 0; k < h->count[len]; k++, code++, i++) {
			int rev = 0, b, fill;

			for (b = 0; b < len; b++)
				rev |= ((code >> b) & 1) << (len - 1 - b);
			for (fill = rev; fill < (1 << FASTBITS); fill += 1 << len)
				h->fast[fill] = len << 12 | h->symbol[i];
		}
		code <<= 1;
	}
	return 0;
}

/* the next symbol, or -1 */
static int
decode(struct state *s, const struct huffman *h)
{
	int code = 0, first = 0, index = 0, len, e;

	while (s->bitcnt < FASTBITS && s->inpos < s->inlen) {
		s->bitbuf |= (uint32_t)s->in[s->inpos++] << s->bitcnt;
		s->bitcnt += 8;
	}
	e = h->fast[s->bitbuf & ((1 << FASTBITS) - 1)];
	if (e && (e >> 12) <= s->bitcnt) {
		s->bitbuf >>= e >> 12;
		s->bitcnt -= e >> 12;
		return e & 0xfff;
	}
	for (len = 1; len <= MAXBITS; len++) {
		int count;

		code |= bits(s, 1);
		if (s->err)
			return -1;
		count = h->count[len];
		if (code - count < first)
			return h->symbol[index + (code - first)];
		index += count;
		first += count;
		first <<= 1;
		code <<= 1;
	}
	s->err = "bad Huffman code";
	return -1;
}

static const short lbase[29] = {
	3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99,
	115, 131, 163, 195, 227, 258,
};
static const short lext[29] = {
	0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0,
};
static const unsigned short dbase[30] = {
	1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025,
	1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577,
};
static const short dext[30] = {
	0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12,
	12, 13, 13,
};

/* a block's compressed data, up to its end code */
static int
codes(struct state *s, const struct huffman *lencode, const struct huffman *distcode)
{
	for (;;) {
		int sym = decode(s, lencode);
		unsigned len, dist;

		if (sym < 0)
			return -1;
		if (sym < 256) {
			out(s, sym);
		} else if (sym == 256) {
			return 0;
		} else {
			sym -= 257;
			if (sym >= 29) {
				s->err = "bad length code";
				return -1;
			}
			len = lbase[sym] + bits(s, lext[sym]);
			sym = decode(s, distcode);
			if (sym < 0)
				return -1;
			if (sym >= 30) {
				s->err = "bad distance code";
				return -1;
			}
			dist = dbase[sym] + bits(s, dext[sym]);
			if (s->err)
				return -1;
			if (dist > s->total) {
				s->err = "distance too far back";
				return -1;
			}
			while (len--)
				out(s, s->win[(s->wpos - dist) & (WSIZE - 1)]);
		}
		if (s->stop)
			return -1;
	}
}

static int
stored(struct state *s)
{
	unsigned len, nlen;

	s->bitbuf >>= s->bitcnt & 7;
	s->bitcnt -= s->bitcnt & 7;
	len = bits(s, 16);
	nlen = bits(s, 16);
	if (s->err)
		return -1;
	if (len != (~nlen & 0xffff)) {
		s->err = "bad stored block";
		return -1;
	}
	while (len--) {
		int c = s->bitcnt ? (int)bits(s, 8) : getbyte(s);

		if (c < 0 || s->err)
			return -1;
		out(s, c);
		if (s->stop)
			return -1;
	}
	return 0;
}

static int
fixed(struct state *s)
{
	static struct huffman lencode, distcode;
	static int built;

	if (!built) {
		short lengths[288];
		int i;

		for (i = 0; i < 144; i++)
			lengths[i] = 8;
		for (; i < 256; i++)
			lengths[i] = 9;
		for (; i < 280; i++)
			lengths[i] = 7;
		for (; i < 288; i++)
			lengths[i] = 8;
		construct(&lencode, lengths, 288);
		for (i = 0; i < 30; i++)
			lengths[i] = 5;
		construct(&distcode, lengths, 30);
		built = 1;
	}
	return codes(s, &lencode, &distcode);
}

static int
dynamic(struct state *s)
{
	static const short order[19] = {
		16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15,
	};
	static struct huffman lencode, distcode;
	short lengths[288 + 32];
	int nlen, ndist, ncode, index;

	nlen = bits(s, 5) + 257;
	ndist = bits(s, 5) + 1;
	ncode = bits(s, 4) + 4;
	if (s->err)
		return -1;
	if (nlen > 286 || ndist > 30) {
		s->err = "bad block header";
		return -1;
	}
	for (index = 0; index < ncode; index++)
		lengths[order[index]] = bits(s, 3);
	for (; index < 19; index++)
		lengths[order[index]] = 0;
	if (s->err || construct(&lencode, lengths, 19) < 0) {
		if (!s->err)
			s->err = "bad code lengths";
		return -1;
	}
	for (index = 0; index < nlen + ndist;) {
		int sym = decode(s, &lencode), len = 0, rep;

		if (sym < 0)
			return -1;
		if (sym < 16) {
			lengths[index++] = sym;
			continue;
		}
		if (sym == 16) {
			if (index == 0) {
				s->err = "repeat with no length";
				return -1;
			}
			len = lengths[index - 1];
			rep = 3 + bits(s, 2);
		} else if (sym == 17)
			rep = 3 + bits(s, 3);
		else
			rep = 11 + bits(s, 7);
		if (s->err)
			return -1;
		if (index + rep > nlen + ndist) {
			s->err = "too many lengths";
			return -1;
		}
		while (rep--)
			lengths[index++] = len;
	}
	if (lengths[256] == 0) {
		s->err = "no end code";
		return -1;
	}
	if (construct(&lencode, lengths, nlen) < 0 ||
	    construct(&distcode, lengths + nlen, ndist) < 0) {
		s->err = "bad code lengths";
		return -1;
	}
	return codes(s, &lencode, &distcode);
}

static int
skipstring(struct state *s)
{
	int c;

	while ((c = getbyte(s)) > 0)
		;
	return c;
}

int
gunzip(int fd, gz_sink sink, void *arg, const char **err)
{
	static struct state st;
	struct state *s = &st;
	int flags, last, type, i, r = 0;
	uint32_t crc, isize;

	memset(s, 0, sizeof(*s));
	s->fd = fd;
	s->sink = sink;
	s->arg = arg;
	if (getbyte(s) != 0x1f || getbyte(s) != 0x8b || getbyte(s) != 8) {
		*err = s->err ? s->err : "not a gzip file";
		return -1;
	}
	flags = getbyte(s);
	for (i = 0; i < 6; i++)
		getbyte(s);
	if (flags & 0xe0) {
		*err = "unknown gzip flags";
		return -1;
	}
	if (flags & 4) {
		int xlen = getbyte(s);

		xlen |= getbyte(s) << 8;
		while (xlen-- > 0)
			getbyte(s);
	}
	if (flags & 8)
		skipstring(s);
	if (flags & 16)
		skipstring(s);
	if (flags & 2) {
		getbyte(s);
		getbyte(s);
	}
	if (s->err) {
		*err = s->err;
		return -1;
	}
	do {
		last = bits(s, 1);
		type = bits(s, 2);
		if (s->err)
			r = -1;
		else if (type == 0)
			r = stored(s);
		else if (type == 1)
			r = fixed(s);
		else if (type == 2)
			r = dynamic(s);
		else {
			s->err = "bad block type";
			r = -1;
		}
	} while (!last && !r);
	if (!r)
		flush(s);
	if (s->stop)
		return s->stop;
	if (r) {
		*err = s->err;
		return -1;
	}
	s->bitbuf >>= s->bitcnt & 7;
	s->bitcnt -= s->bitcnt & 7;
	crc = bits(s, 16);
	crc |= bits(s, 16) << 16;
	isize = bits(s, 16);
	isize |= bits(s, 16) << 16;
	if (s->err) {
		*err = s->err;
		return -1;
	}
	if (crc != s->crc || isize != s->total) {
		*err = "CRC or length doesn't match: the file is damaged";
		return -1;
	}
	return 0;
}
