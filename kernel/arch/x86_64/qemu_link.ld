/*
 * qemu_link.ld — Linker script for 32-bit multiboot QEMU wrapper.
 *
 * Layout:
 *   0x090000  .bss  — page tables + stack (BELOW kernel copy range)
 *   0x100000  .multiboot + .text + .rodata + .data (wrapper entry)
 *   0x200000+  .kernel (embedded 64-bit kernel binary)
 *   ALIGN     .trampoline (copies kernel to 0x100000 and jumps)
 *
 * Page tables live at 0x090000 so the kernel copy (0x100000...) never
 * overwrites them. QEMU flushes softmmu TLBs when page-table pages are
 * written; placing tables below 0x100000 avoids that flush entirely.
 *
 * Note: .bss must be the first LOAD segment so the multiboot header
 * (in .multiboot at 0x100000) still lands within the first 8 KiB of
 * the first CODE segment — QEMU scans each LOAD segment independently.
 */

ENTRY(_start)
OUTPUT_FORMAT(elf32-i386)

SECTIONS {
    /* Page tables and stack — safely below the kernel copy range */
    . = 0x090000;
    .bss : {
        *(.bss .bss.*)
    }

    /* Wrapper code — multiboot header must be within first 8 KiB of
     * the file (QEMU scans for magic 0x1BADB002). We place it at
     * the very start of the .multiboot section at 0x100000. */
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

    /* Kernel payload — immediately after wrapper data */
    . = ALIGN(4096);
    _kernel_payload = .;
    .kernel : {
        *(.kernel)
    }
    _kernel_payload_end = .;
    _kernel_size = _kernel_payload_end - _kernel_payload;

    /* Trampoline — above kernel payload, runs in 64-bit mode */
    . = ALIGN(4096);
    .trampoline : {
        *(.trampoline)
    }

    /DISCARD/ : {
        *(.comment)
        *(.note.*)
    }
}
