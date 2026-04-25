/*
 * vm_backend_select.c — Hypervisor backend selection (RFC-0017 Section 12.4
 * + sandbox extension).
 *
 * Selection rules:
 *
 *   ANX_VM_SANDBOX_AUTO        → firecracker > qemu > anx-native
 *   ANX_VM_SANDBOX_FIRECRACKER → firecracker only (NULL if unavailable)
 *   ANX_VM_SANDBOX_QEMU        → qemu only
 *   ANX_VM_SANDBOX_ANX_NATIVE  → anx-native only
 *
 * The anx-native backend is always available because it executes inside
 * the host kernel; firecracker and qemu may report unavailable on bare
 * metal where there is no userland to fork into.
 */

#include <anx/types.h>
#include <anx/vm.h>
#include <anx/vm_backend.h>

static struct anx_vm_backend *all_backends[] = {
	&anx_vm_backend_firecracker,
	&anx_vm_backend_qemu,
	&anx_vm_backend_anx_native,
};

#define ANX_VM_BACKEND_COUNT \
	(sizeof(all_backends) / sizeof(all_backends[0]))

struct anx_vm_backend *anx_vm_backend_select(enum anx_vm_sandbox_mode mode)
{
	uint32_t i;

	if (mode == ANX_VM_SANDBOX_AUTO) {
		for (i = 0; i < ANX_VM_BACKEND_COUNT; i++) {
			struct anx_vm_backend *b = all_backends[i];

			if (b && b->available && b->available())
				return b;
		}
		return NULL;
	}

	for (i = 0; i < ANX_VM_BACKEND_COUNT; i++) {
		struct anx_vm_backend *b = all_backends[i];

		if (!b || b->mode != mode)
			continue;
		if (b->available && b->available())
			return b;
	}
	return NULL;
}
