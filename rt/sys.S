/*
 * A/UX 3 syscalls, as A/UX 3.1's libc makes them. There are two gates:
 *
 *   trap #0   the System V calls: number in d0, arguments on the stack as the caller
 *             left them (the kernel reads them above the return address)
 *   trap #15  the BSD-style ones (sockets, stat, mkdir, rename, ...): number in d0,
 *             arguments in a0, d1, a1, d2, a2, d3
 *
 * Either way the result comes back in d0, or carry set and the error number in d0.
 */

	.macro	SYS0 name, num
	.globl	\name
	.type	\name, @function
\name:
	move.l	#\num, %d0
	trap	#0
	bcs.w	__cerror
	rts
	.endm

	/* Loads six arguments whatever the call takes: reading past the real ones only reads
	   the caller's frame. movem to registers leaves the condition codes alone. */
	.macro	SYS15 name, num
	.globl	\name
	.type	\name, @function
\name:
	movem.l	%d2-%d3/%a2, -(%sp)
	movea.l	16(%sp), %a0
	move.l	20(%sp), %d1
	movea.l	24(%sp), %a1
	move.l	28(%sp), %d2
	movea.l	32(%sp), %a2
	move.l	36(%sp), %d3
	move.l	#\num, %d0
	trap	#15
	movem.l	(%sp)+, %d2-%d3/%a2
	bcs.w	__cerror
	rts
	.endm

	.text

	.globl	__cerror
__cerror:
	move.l	%d0, errno
	moveq	#-1, %d0
	movea.l	%d0, %a0
	rts

	SYS0	_exit, 1
	SYS0	read, 3
	SYS0	write, 4
	SYS0	open, 5
	SYS0	close, 6
	SYS0	link, 9
	SYS0	unlink, 10
	SYS0	chdir, 12
	SYS0	chmod, 15
	SYS0	chown, 16
	SYS0	__brk, 17
	SYS0	lseek, 19
	SYS0	getpid, 20
	SYS0	getuid, 24
	SYS0	utime, 30
	SYS0	access, 33
	SYS0	sync, 36
	SYS0	kill, 37
	SYS0	dup, 41
	SYS0	signal, 48
	SYS0	ioctl, 54
	SYS0	execve, 59
	SYS0	umask, 60
	SYS0	fcntl, 62

	SYS15	accept, 0x46
	SYS15	bind, 0x47
	SYS15	connect, 0x48
	SYS15	getsockname, 0x4c
	SYS15	getsockopt, 0x4d
	SYS15	listen, 0x4e
	SYS15	recv, 0x4f
	SYS15	recvfrom, 0x50
	SYS15	select, 0x52
	SYS15	send, 0x53
	SYS15	sendto, 0x55
	SYS15	setsockopt, 0x5a
	SYS15	shutdown, 0x5b
	SYS15	socket, 0x5c
	SYS15	mkdir, 0x6c
	SYS15	rmdir, 0x6d
	SYS15	getdirent, 0x6e
	SYS15	lstat, 0x6f
	SYS15	symlink, 0x70
	SYS15	readlink, 0x71
	SYS15	ftruncate, 0x73
	SYS15	fsync, 0x74
	SYS15	rename, 0x7b
	SYS15	fstat, 0x7c
	SYS15	stat, 0x7d
	/* 3.0.1's call, which 3.1 keeps as _gettimeofday; 3.1's own, 0xa8, isn't in 3.0.1.
	   It takes no timezone. */
	SYS15	gettimeofday, 0x88
