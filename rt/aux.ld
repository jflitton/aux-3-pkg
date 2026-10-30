/*
 * Lays a program out as an A/UX 3 demand-paged executable (a.out magic 0413), which
 * tools/elf2aout.py then writes as COFF:
 *
 *   - the file's first 0xa8 bytes are its headers (file, a.out, three sections); .text
 *     follows at the same address as its file offset, 0xa8, and starts with _start
 *   - .data follows .text in the file and is mapped at 0x400000 plus its file offset
 *     modulo 8KB, A/UX's page size
 *   - .bss follows .data
 */
OUTPUT_FORMAT("elf32-m68k")
ENTRY(_start)

SECTIONS
{
	. = 0xa8;
	.text : {
		*(.text.start)
		*(.text .text.*)
		*(.rodata .rodata.*)
		. = ALIGN(4);
	}
	. = 0x400000 + (. & 0x1fff);
	.data : {
		*(.data .data.*)
		*(.sdata .sdata.*)
		. = ALIGN(4);
	}
	.bss : {
		*(.bss .bss.*)
		*(.sbss .sbss.*)
		*(COMMON)
		. = ALIGN(4);
	}
	_end = .;
	/DISCARD/ : {
		*(.comment)
		*(.note .note.*)
		*(.eh_frame .eh_frame_hdr)
	}
}
