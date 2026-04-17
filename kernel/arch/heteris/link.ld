/*
 * link.ld — Heteris kernel linker script.
 *
 * Loads at 0x00080000 (512 KiB into OS lane address space).
 * Leaves 0x00000000-0x0007FFFF for interrupt vectors and firmware.
 *
 * Memory map (from Heteris Architecture Spec):
 *   0x0000_0000 - 0x7FFF_FFFF : OS lane L2 cache coherent space
 *   0x8000_0000 - 0x8000_1FFF : AI scratchpad SRAM (8KB)
 *   0x8000_2000 - 0x8000_2FFF : Tile configuration registers
 */

ENTRY(_start)

SECTIONS {
    . = 0x00080000;

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

    . = ALIGN(4096);
    _heap_start = .;
    . = . + 16M;
    _heap_end = .;

    /DISCARD/ : {
        *(.comment)
        *(.note.*)
    }
}
