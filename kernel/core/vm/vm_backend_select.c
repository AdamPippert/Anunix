/*
 * vm_backend_select.c — Hypervisor backend selection (RFC-0017 Section 12.4).
 */

#include <anx/types.h>
#include <anx/vm_backend.h>

struct anx_vm_backend *anx_vm_backend_select(void)
{
	if (anx_vm_backend_qemu.available())
		return &anx_vm_backend_qemu;
	return NULL;
}
