/*
 * anx/vm_backend.h — Hypervisor backend interface (RFC-0017 Section 12).
 *
 * All backends implement this interface. The correct backend is selected
 * at VM Object creation time based on host capabilities.
 */

#ifndef ANX_VM_BACKEND_H
#define ANX_VM_BACKEND_H

#include <anx/types.h>
#include <anx/vm.h>

struct anx_vm_backend {
	const char *name;

	/* Sandbox mode this backend serves (ANX_VM_SANDBOX_*) */
	enum anx_vm_sandbox_mode mode;

	/* Returns true if this backend is usable on the current host */
	bool (*available)(void);

	/* Lifecycle */
	int (*create)(struct anx_vm_object *vm);
	int (*start)(struct anx_vm_object *vm);
	int (*pause)(struct anx_vm_object *vm);
	int (*resume)(struct anx_vm_object *vm);
	int (*stop)(struct anx_vm_object *vm, bool force);
	int (*destroy)(struct anx_vm_object *vm);

	/* State capture */
	int (*snapshot)(struct anx_vm_object *vm, anx_oid_t *snap_out);

	/* Hotplug (may return ANX_ENOTSUP if not supported while running) */
	int (*hotplug_cpu)(struct anx_vm_object *vm, uint32_t count);
	int (*hotplug_mem)(struct anx_vm_object *vm, uint64_t size_mb);
	int (*hotplug_net)(struct anx_vm_object *vm,
			   const struct anx_vm_netdev *dev);

	/* Console I/O */
	int (*console_read)(struct anx_vm_object *vm, char *buf, uint32_t len,
			    uint32_t *read_out);
	int (*console_write)(struct anx_vm_object *vm, const char *buf,
			     uint32_t len);

	/* Guest exec via guest agent (virtio-serial) */
	int (*exec)(struct anx_vm_object *vm, const char *cmd,
		    char *stdout_out, uint32_t stdout_size);

	/* Stats */
	int (*stats)(struct anx_vm_object *vm, struct anx_vm_stats *out);
};

/* Backend registry */
extern struct anx_vm_backend anx_vm_backend_qemu;
extern struct anx_vm_backend anx_vm_backend_firecracker;
extern struct anx_vm_backend anx_vm_backend_anx_native;

/*
 * Select a backend.  If `mode` is ANX_VM_SANDBOX_AUTO the kernel picks the
 * lightest available backend (firecracker > qemu > anx-native).  Otherwise
 * the named backend is returned only if `available()` reports true.
 */
struct anx_vm_backend *anx_vm_backend_select(enum anx_vm_sandbox_mode mode);

#endif /* ANX_VM_BACKEND_H */
