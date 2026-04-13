/*
 * link.ld — x86_64 kernel linker script.
 * Loads at 1 MiB (standard for UEFI-loaded kernels).
 */

ENTRY(_start)

SECTIONS {
    . = 0x100000;

    .text.boot : {
        *(.text.boot)
    }

    .text : {
        *(.text .text.*)
    }

    .rodata : {
        *(.rodata .rodata.*)
    }

    .data : {
        *(.data .data.*)
    }

    . = ALIGN(8);
    _bss_start = .;
    .bss : {
        *(.bss .bss.*)
        *(COMMON)
    }
    _bss_end = .;

    /DISCARD/ : {
        *(.comment)
        *(.note.*)
    }
}
