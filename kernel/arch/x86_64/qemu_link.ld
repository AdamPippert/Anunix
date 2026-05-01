/*
 * qemu_link.ld — Linker script for 32-bit multiboot QEMU wrapper.
 *
 * Single PT_LOAD segment starting at 0x100000.  The multiboot header
 * is the very first bytes, so QEMU's 8 KiB scan always finds the magic.
 *
 * Page tables and stack live at hardcoded addresses 0x090000–0x096000
 * (defined directly in qemu_boot.S, not via .bss) so no BSS PT_LOAD
 * segment is emitted to confuse QEMU's multiboot detector.
 */

ENTRY(_start)
OUTPUT_FORMAT(elf32-i386)

SECTIONS {
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

    /* Kernel payload — embedded 64-bit binary */
    . = ALIGN(4096);
    _kernel_payload = .;
    .kernel : {
        *(.kernel)
    }
    _kernel_payload_end = .;
    _kernel_size = _kernel_payload_end - _kernel_payload;

    /* Trampoline — 64-bit code that copies kernel and jumps */
    . = ALIGN(4096);
    .trampoline : {
        *(.trampoline)
    }

    /DISCARD/ : {
        *(.bss .bss.*)
        *(.comment)
        *(.note.*)
    }
}
