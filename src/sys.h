/*
 * What pkg needs from the system. On A/UX that's our own runtime (rt/aux.h); on a modern
 * host, for testing, the system's libc. pkg's code uses only what both provide, plus
 * listdir() below, since A/UX reads directories with getdirent and a host with readdir.
 */
#ifndef SYS_H
#define SYS_H

#ifdef AUX
#include "aux.h"
#else
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>
#endif

/* Calls fn(name, arg) for each entry of directory path but . and ..; stops at the first
   nonzero return and returns it. -1 if the directory can't be read. */
int listdir(const char *path, int (*fn)(const char *name, void *arg), void *arg);

#endif
