/*
 * qemu_link.ld — Linker script for 32-bit multiboot QEMU wrapper.
 *
 * Layout:
 *   0x100000  .multiboot + .text + .rodata (wrapper entry code)
 *   0x200000  .bss (page tables + stack — safe from kernel copy)
 *   0x300000  .kernel (embedded 64-bit kernel binary)
 *   0x310000  .trampoline (copies kernel to 0x100000 and jumps)
 *
 * The kernel binary is copied from 0x300000 to 0x100000 at boot.
 * The .multiboot header must be within the first 8 KiB of the file,
 * so it's placed at the lowest load address.
 */

ENTRY(_start)
OUTPUT_FORMAT(elf32-i386)

SECTIONS {
    /* Wrapper code at lowest address (multiboot in first 8 KiB) */
    . = 0x100000;

    .multiboot : {
        KEEP(*(.multiboot))
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

    /* Page tables and stack well above kernel copy range */
    . = 0x200000;
    .bss : {
        *(.bss .bss.*)
    }

    /* Kernel payload above BSS */
    . = ALIGN(4096);
    _kernel_payload = .;
    .kernel : {
        *(.kernel)
    }
    _kernel_payload_end = .;
    _kernel_size = _kernel_payload_end - _kernel_payload;

    /* Trampoline above kernel payload */
    . = ALIGN(4096);
    .trampoline : {
        *(.trampoline)
    }

    /DISCARD/ : {
        *(.comment)
        *(.note.*)
    }
}
