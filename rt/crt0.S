/*
 * Program entry. The kernel starts us at the first byte of .text (0xa8, after the
 * headers) with the stack holding argc, argv[0..argc-1], 0, envp[...], 0, and d0 the
 * stack limit, which we don't need. As A/UX's own crt0, less its FPU and compatibility
 * set-up: pkg uses neither.
 */
	.section .text.start, "ax"
	.globl	_start
_start:
	move.l	(%sp), %d1		/* argc */
	lea	4(%sp), %a0		/* argv */
	lea	4(%a0, %d1.l*4), %a1	/* envp: past argv's terminating 0 */
	move.l	%a1, environ
	move.l	%a1, -(%sp)
	move.l	%a0, -(%sp)
	move.l	%d1, -(%sp)
	jsr	main
	move.l	%d0, (%sp)
	jsr	exit
