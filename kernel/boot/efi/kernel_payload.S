/*
 * kernel_payload.S — Embedded kernel binary for EFI stub.
 */

.section .kernel, "a"
.global _kernel_start
.global _kernel_end

_kernel_start:
	.incbin "build/x86_64/anunix.bin"
_kernel_end:
