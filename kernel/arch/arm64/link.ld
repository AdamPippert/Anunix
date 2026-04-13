/*
 * link.ld — ARM64 kernel linker script.
 * Loads at 0x40080000 (typical for Asahi Linux / Apple Silicon).
 */

ENTRY(_start)

SECTIONS {
    . = 0x40080000;

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
