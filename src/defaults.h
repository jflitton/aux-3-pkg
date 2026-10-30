/*
 * The server pkg uses unless /etc/pkg.conf or -s names another, and the public keys it
 * trusts to sign package lists (64 hex digits each): see docs/publishing.md. The build
 * for this machine sets DEFAULT_SERVER to "", so tests never reach the real one.
 */
#ifndef DEFAULT_SERVER
#define DEFAULT_SERVER	"http://auxpkg.zerobytesfree.org/"
#endif

static const char *const default_keys[] = {
	"b2811c9622fccde68f7b45a55c97a6c7a8a247f61239e17b9d792a47ee00aa0a",
	NULL
};
