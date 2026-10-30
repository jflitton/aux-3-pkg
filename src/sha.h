/* SHA-256 (package and file hashes) and SHA-512 (inside Ed25519), FIPS 180-4 */
#ifndef SHA_H
#define SHA_H

#include "sys.h"

typedef struct {
	uint32_t h[8];
	uint64_t len;
	unsigned char buf[64];
} sha256_ctx;

typedef struct {
	uint64_t h[8];
	uint64_t len;
	unsigned char buf[128];
} sha512_ctx;

void sha256_init(sha256_ctx *c);
void sha256_update(sha256_ctx *c, const void *data, size_t n);
void sha256_final(sha256_ctx *c, unsigned char out[32]);

void sha512_init(sha512_ctx *c);
void sha512_update(sha512_ctx *c, const void *data, size_t n);
void sha512_final(sha512_ctx *c, unsigned char out[64]);

/* out gets 64 hex digits and a 0 */
void hex(const unsigned char *in, size_t n, char *out);
/* 0, or -1 if s isn't exactly 2n hex digits */
int unhex(const char *s, unsigned char *out, size_t n);

#endif
