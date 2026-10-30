/* Ed25519 signature verification (RFC 8032) */
#ifndef ED25519_H
#define ED25519_H

#include "sys.h"

/* 0 if sig is pk's signature of msg, else -1 */
int ed25519_verify(const unsigned char sig[64], const void *msg, size_t n,
	const unsigned char pk[32]);

#endif
