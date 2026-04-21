/*
 * link.ld — x86_64 kernel linker script.
 * Loads at 1 MiB (standard for x86 kernels).
 *
 * The multiboot2 header must appear within the first 32 KiB
 * of the binary, so it goes before all other sections.
 */

ENTRY(_start)

SECTIONS {
    . = 0x100000;

    .text.boot : {
        *(.text.boot)
    }

    /* Multiboot2 header — must be within first 32 KiB of the ELF.
     * Placed after .text.boot so _start stays at 0x100000
     * (the QEMU wrapper jumps to that fixed address).
     */
    .multiboot2 : {
        KEEP(*(.multiboot2))
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

    /*
     * Heap must start above the kernel image. Force a minimum of
     * 0x210000 to leave headroom for large BSS (JPEG logo, stacks).
     * Wrapper page tables are at 0x090000 (not 0x200000 — safe).
     */
    . = ALIGN(4096);
    . = (. < 0x210000) ? 0x210000 : .;
    _heap_start = .;
    . = . + 16M;
    _heap_end = .;

    /DISCARD/ : {
        *(.comment)
        *(.note.*)
    }
}
