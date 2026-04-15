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

    . = ALIGN(2048);
    .vectors : {
        KEEP(*(.vectors))
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

    . = ALIGN(4096);
    _heap_start = .;
    . = . + 16M;
    _heap_end = .;

    /DISCARD/ : {
        *(.comment)
        *(.note.*)
    }
}
