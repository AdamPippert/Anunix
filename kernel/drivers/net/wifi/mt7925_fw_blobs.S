/*
 * mt7925_fw_blobs.S — MT7925 embedded firmware blobs.
 *
 * Patch firmware: decompressed mt7925_patch_e1_hdr.bin (197792 bytes)
 *   Build date: 2026-01-06 15:30:07, project: ALPS
 *
 * RAM firmware: decompressed mt7925_wm.bin (1246968 bytes)
 *   Main WM MCU firmware image.
 *
 * Paths are relative to project root (Makefile cwd).
 */

.section .rodata

.global mt7925_patch_fw
.global mt7925_patch_fw_end
.balign 4096
mt7925_patch_fw:
	.incbin "kernel/firmware/mt7925_patch.bin"
mt7925_patch_fw_end:

.global mt7925_patch_fw_size
mt7925_patch_fw_size:
	.long mt7925_patch_fw_end - mt7925_patch_fw

.balign 4096
.global mt7925_ram_fw
.global mt7925_ram_fw_end
mt7925_ram_fw:
	.incbin "kernel/firmware/mt7925_ram.bin"
mt7925_ram_fw_end:

.global mt7925_ram_fw_size
mt7925_ram_fw_size:
	.long mt7925_ram_fw_end - mt7925_ram_fw
