/*
 * splash_img.S — Embedded JPEG logo for boot splash.
 *
 * The logo-full.jpg is embedded as raw bytes in the kernel binary.
 * The splash code decodes and displays it during boot.
 */

.section .rodata
.global _splash_jpg_start
.global _splash_jpg_end

_splash_jpg_start:
	.incbin "assets/logo-full.jpg"
_splash_jpg_end:
