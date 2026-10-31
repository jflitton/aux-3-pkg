/*
 * The A/UX 3 runtime pkg is built on: its syscalls (rt/sys.S), with the types,
 * constants and structure layouts of A/UX 3.1's headers and libc, and the small libc in
 * rt/libc.c. Names are the POSIX ones, so code written against this also builds on a
 * modern host. See docs/development.md for how the syscalls work.
 */
#ifndef AUX_H
#define AUX_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

typedef long ssize_t;
typedef long off_t;
typedef long time_t;
typedef unsigned short mode_t;
typedef int pid_t;
typedef int uid_t;
typedef int gid_t;
typedef int socklen_t;

extern int errno;
extern char **environ;

/* errno values (sys/errno.h) */
#define EPERM		1
#define ENOENT		2
#define EINTR		4
#define EIO		5
#define EBADF		9
#define EAGAIN		11
#define ENOMEM		12
#define EACCES		13
#define EEXIST		17
#define EXDEV		18
#define ENOTDIR		20
#define EISDIR		21
#define EINVAL		22
#define ETXTBSY		26
#define EFBIG		27
#define ENOSPC		28
#define EROFS		30
#define EPIPE		32
#define ERANGE		34
#define EWOULDBLOCK	55
#define EINPROGRESS	56
#define EALREADY	57
#define ENETDOWN	70
#define ENETUNREACH	71
#define ECONNRESET	74
#define EISCONN		76
#define ETIMEDOUT	80
#define ECONNREFUSED	81
#define ELOOP		82
#define ENAMETOOLONG	83
#define EHOSTDOWN	84
#define EHOSTUNREACH	85
#define ENOTEMPTY	86

/* open, fcntl (fcntl.h) */
#define O_RDONLY	0
#define O_WRONLY	1
#define O_RDWR		2
#define O_NDELAY	0x0004
#define O_APPEND	0x0008
#define O_CREAT		0x0100
#define O_TRUNC		0x0200
#define O_EXCL		0x0400
#define O_NONBLOCK	0x4000
#define F_SETFD		2
#define F_GETFL		3
#define F_SETFL		4

#define SEEK_SET	0
#define SEEK_CUR	1
#define SEEK_END	2

#define F_OK		0
#define X_OK		1
#define W_OK		2
#define R_OK		4

/* stat (sys/stat.h). The kernel's structure has the System V fields (16-bit inode, uid
   and gid) and the POSIX ones (32-bit) side by side, at the offsets given. */
struct stat {
	unsigned short	st_dev;		/* 0 */
	unsigned short	st_ino16;	/* 2 */
	mode_t		st_mode;	/* 4 */
	short		st_nlink;	/* 6 */
	unsigned short	st_uid;		/* 8 */
	unsigned short	st_gid;		/* 10 */
	unsigned short	st_rdev;	/* 12 */
	off_t		st_size;	/* 14 */
	time_t		st_atime;	/* 18 */
	unsigned long	st_ino;		/* 22 */
	time_t		st_mtime;	/* 26 */
	long		st_spare2;	/* 30 */
	time_t		st_ctime;	/* 34 */
	long		st_spare3;	/* 38 */
	long		st_blksize;	/* 42 */
	long		st_blocks;	/* 46 */
	long		st_uid32;	/* 50 */
	long		st_gid32;	/* 54 */
};

#define S_IFMT		0170000
#define S_IFDIR		0040000
#define S_IFCHR		0020000
#define S_IFBLK		0060000
#define S_IFREG		0100000
#define S_IFIFO		0010000
#define S_IFLNK		0120000
#define S_IFSOCK	0140000
#define S_ISDIR(m)	(((m) & S_IFMT) == S_IFDIR)
#define S_ISREG(m)	(((m) & S_IFMT) == S_IFREG)
#define S_ISLNK(m)	(((m) & S_IFMT) == S_IFLNK)

/* getdirent records: BSD struct direct (sys/dir.h) */
struct direct {
	unsigned long	d_fileno;
	unsigned short	d_reclen;
	unsigned short	d_namlen;
	char		d_name[256];
};

struct utimbuf {
	time_t	actime;
	time_t	modtime;
};

struct timeval {
	long	tv_sec;
	long	tv_usec;
};

/* select (sys/types.h) */
#define FD_SETSIZE	128
typedef struct {
	long	fds_bits[FD_SETSIZE / 32];
} fd_set;
#define FD_ZERO(p)	memset((p), 0, sizeof(*(p)))
#define FD_SET(n, p)	((p)->fds_bits[(n) / 32] |= (1L << ((n) % 32)))
#define FD_CLR(n, p)	((p)->fds_bits[(n) / 32] &= ~(1L << ((n) % 32)))
#define FD_ISSET(n, p)	((p)->fds_bits[(n) / 32] & (1L << ((n) % 32)))

/* sockets (sys/socket.h, netinet/in.h) */
#define AF_INET		2
#define SOCK_STREAM	1
#define SOCK_DGRAM	2
#define SOL_SOCKET	0xffff
#define SO_REUSEADDR	0x0004
#define SO_KEEPALIVE	0x0008
#define SO_RCVTIMEO	0x1006
#define SO_ERROR	0x1007
#define INADDR_ANY	0
#define SOMAXCONN	5
#define MSG_PEEK	0x2

struct sockaddr {
	unsigned short	sa_family;
	char		sa_data[14];
};

struct in_addr {
	unsigned long	s_addr;
};

struct sockaddr_in {
	short		sin_family;
	unsigned short	sin_port;
	struct in_addr	sin_addr;
	char		sin_zero[8];
};

/* A/UX is big-endian, like the network */
#define htons(x)	((unsigned short)(x))
#define ntohs(x)	((unsigned short)(x))
#define htonl(x)	((unsigned long)(x))
#define ntohl(x)	((unsigned long)(x))

/* signals (sys/signal.h) */
#define SIGHUP		1
#define SIGINT		2
#define SIGKILL		9
#define SIGPIPE		13
#define SIGALRM		14
#define SIGTERM		15
#define SIG_DFL		((void (*)(int))0)
#define SIG_IGN		((void (*)(int))1)

/* syscalls (rt/sys.S) */
void _exit(int status) __attribute__((noreturn));
ssize_t read(int fd, void *buf, size_t n);
ssize_t write(int fd, const void *buf, size_t n);
int open(const char *path, int flags, ...);
int close(int fd);
int link(const char *from, const char *to);
int unlink(const char *path);
int chdir(const char *path);
int chmod(const char *path, mode_t mode);
int chown(const char *path, uid_t uid, gid_t gid);
off_t lseek(int fd, off_t off, int whence);
pid_t getpid(void);
uid_t getuid(void);
int utime(const char *path, const struct utimbuf *times);
int access(const char *path, int mode);
void sync(void);
int kill(pid_t pid, int sig);
int dup(int fd);
int ioctl(int fd, int req, ...);
int execve(const char *path, char *const argv[], char *const envp[]);
mode_t umask(mode_t mask);
int fcntl(int fd, int cmd, ...);
void (*signal(int sig, void (*func)(int)))(int);
int stat(const char *path, struct stat *st);
int lstat(const char *path, struct stat *st);
int fstat(int fd, struct stat *st);
int mkdir(const char *path, mode_t mode);
int rmdir(const char *path);
int rename(const char *from, const char *to);
int symlink(const char *target, const char *path);
ssize_t readlink(const char *path, char *buf, size_t n);
int getdirent(int fd, char *buf, int n, long *basep);
int ftruncate(int fd, off_t len);
int fsync(int fd);
int gettimeofday(struct timeval *tv, void *tz);
int select(int n, fd_set *r, fd_set *w, fd_set *e, struct timeval *timeout);
int socket(int domain, int type, int proto);
int connect(int fd, const struct sockaddr *addr, int len);
int bind(int fd, const struct sockaddr *addr, int len);
int listen(int fd, int backlog);
int accept(int fd, struct sockaddr *addr, int *len);
int getsockname(int fd, struct sockaddr *addr, int *len);
int shutdown(int fd, int how);
ssize_t send(int fd, const void *buf, size_t n, int flags);
ssize_t recv(int fd, void *buf, size_t n, int flags);
ssize_t sendto(int fd, const void *buf, size_t n, int flags, const struct sockaddr *to,
	int tolen);
ssize_t recvfrom(int fd, void *buf, size_t n, int flags, struct sockaddr *from,
	int *fromlen);
int setsockopt(int fd, int level, int name, const void *val, int len);
int getsockopt(int fd, int level, int name, void *val, socklen_t *len);

/* libc (rt/libc.c) */
void exit(int status) __attribute__((noreturn));
time_t time(time_t *t);
void *sbrk(long n);
void *malloc(size_t n);
void *calloc(size_t n, size_t size);
void *realloc(void *p, size_t n);
void free(void *p);
char *getenv(const char *name);
int isatty(int fd);

void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);
void *memchr(const void *s, int c, size_t n);
size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
char *strchr(const char *s, int c);
char *strstr(const char *s, const char *find);
char *strrchr(const char *s, int c);
char *strcpy(char *dst, const char *src);
char *strdup(const char *s);
char *strerror(int err);
long strtol(const char *s, char **end, int base);
unsigned long strtoul(const char *s, char **end, int base);

int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap);
int snprintf(char *buf, size_t n, const char *fmt, ...);
int dprintf(int fd, const char *fmt, ...);
int vdprintf(int fd, const char *fmt, va_list ap);

#endif
