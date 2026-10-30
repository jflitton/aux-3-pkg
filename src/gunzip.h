/* gzip decompression (RFC 1951 deflate, RFC 1952 gzip) */
#ifndef GUNZIP_H
#define GUNZIP_H

#include "sys.h"

/* Gets the decompressed data in pieces; a nonzero return stops decompression. */
typedef int (*gz_sink)(void *arg, const unsigned char *data, size_t n);

/* Decompresses the gzip file read from fd. 0 when the whole file was decompressed and
   its CRC and length matched; -1 if it's damaged or not gzip, with *err saying why; or
   what the sink returned to stop. */
int gunzip(int fd, gz_sink sink, void *arg, const char **err);

/* CRC-32 as gzip uses it: crc32(0, ...) to start */
uint32_t crc32(uint32_t crc, const unsigned char *p, size_t n);

#endif
