/*
 * efi_link.ld — Linker script for the EFI stub.
 *
 * Produces a position-independent executable that ld.lld can
 * convert to PE32+ format with --oformat=pei-x86-64.
 */

ENTRY(efi_main)

SECTIONS {
    . = 0;

    .text : {
        *(.text .text.*)
    }

    .rodata : {
        *(.rodata .rodata.*)
    }

    .data : {
        *(.data .data.*)
    }

    .kernel : {
        *(.kernel)
    }

    .bss : {
        *(.bss .bss.*)
    }

    /DISCARD/ : {
        *(.comment)
        *(.note.*)
    }
}
