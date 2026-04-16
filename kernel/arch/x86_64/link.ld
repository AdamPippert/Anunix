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
     * Heap must start ABOVE 0x210000 to avoid the multiboot
     * wrapper's page tables at 0x200000 (PML4 + PDPT, still
     * active in CR3). With the 640KB JPEG logo + 64KB stack,
     * BSS can extend past 0x200000 — force heap above.
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
