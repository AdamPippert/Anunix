/*
 * anx/vm.h — VM Objects (RFC-0017).
 *
 * Dual-nature virtual machine primitive: ANX_OBJ_VM is the at-rest
 * configuration/state tree; ANX_CELL_VM is the running instance.
 */

#ifndef ANX_VM_H
#define ANX_VM_H

#include <anx/types.h>
#include <anx/state_object.h>

/* --- VM power states --- */

enum anx_vm_state {
	ANX_VM_DEFINED,		/* configured, not running */
	ANX_VM_RUNNING,		/* active cell exists */
	ANX_VM_PAUSED,		/* execution suspended, cell still live */
	ANX_VM_SAVED,		/* suspended to disk */
	ANX_VM_DELETED,
};

/* --- Disk format --- */

enum anx_vm_disk_format {
	ANX_VM_DISK_RAW,
	ANX_VM_DISK_QCOW2,
};

/* --- Network attachment mode --- */

enum anx_vm_net_mode {
	ANX_VM_NET_USER,	/* SLIRP user-mode networking */
	ANX_VM_NET_BRIDGE,	/* host bridge attachment */
	ANX_VM_NET_INTERNAL,	/* isolated internal network */
};

/* --- Firmware type --- */

enum anx_vm_firmware {
	ANX_VM_FW_BIOS,
	ANX_VM_FW_UEFI,
	ANX_VM_FW_DIRECT_KERNEL,
};

/* --- Display type --- */

enum anx_vm_display {
	ANX_VM_DISPLAY_NONE,
	ANX_VM_DISPLAY_VNC,
	ANX_VM_DISPLAY_FRAMEBUFFER,
};

/* --- VM capability field mask (Section 10.2) --- */

#define ANX_VM_FIELD_CPU		(1u << 0)
#define ANX_VM_FIELD_MEMORY		(1u << 1)
#define ANX_VM_FIELD_BOOT		(1u << 2)
#define ANX_VM_FIELD_DISKS		(1u << 3)
#define ANX_VM_FIELD_NETWORK		(1u << 4)
#define ANX_VM_FIELD_DISPLAY		(1u << 5)
#define ANX_VM_FIELD_SERIAL		(1u << 6)
#define ANX_VM_FIELD_AGENT_POLICY	(1u << 7)
#define ANX_VM_FIELD_ALL		(0xFFu)

/* --- Network interface config --- */

struct anx_vm_netdev {
	enum anx_vm_net_mode	mode;
	char			mac[18];	/* "xx:xx:xx:xx:xx:xx\0" */
	char			bridge[32];	/* host bridge name */
	uint16_t		fwd_host_port;	/* for user-mode port forward */
	uint16_t		fwd_guest_port;
};

/* --- Disk attachment config --- */

struct anx_vm_disk_config {
	anx_oid_t		disk_oid;
	enum anx_vm_disk_format	format;
	uint32_t		index;
	bool			read_only;
};

/* --- CPU config --- */

struct anx_vm_cpu_config {
	uint32_t	count;		/* vCPU count */
	char		model[32];	/* "host", "qemu64", etc. */
	uint32_t	sockets;
	uint32_t	cores;
	uint32_t	threads;
};

/* --- Memory config --- */

struct anx_vm_mem_config {
	uint64_t	size_mb;
	bool		balloon;
	bool		hugepages;
};

/* --- Boot config --- */

struct anx_vm_boot_config {
	enum anx_vm_firmware	firmware;
	anx_oid_t		kernel_ref;	/* direct_kernel only */
	anx_oid_t		initrd_ref;
	char			cmdline[512];
	anx_oid_t		nvram_ref;	/* uefi only */
};

/* --- Agent access policy --- */

struct anx_vm_agent_policy {
	uint32_t	hotplug_fields;		/* ANX_VM_FIELD_* mask */
	uint32_t	max_cpu_count;
	uint64_t	max_memory_mb;
	uint32_t	max_disk_count;
	uint32_t	max_net_count;
	uint32_t	snapshot_max_count;
};

/* --- Top-level VM config --- */

#define ANX_VM_MAX_DISKS	8
#define ANX_VM_MAX_NETS		4
#define ANX_VM_NAME_MAX		64

struct anx_vm_config {
	char				name[ANX_VM_NAME_MAX];
	struct anx_vm_cpu_config	cpu;
	struct anx_vm_mem_config	memory;
	struct anx_vm_boot_config	boot;
	struct anx_vm_disk_config	disks[ANX_VM_MAX_DISKS];
	uint32_t			disk_count;
	struct anx_vm_netdev		nets[ANX_VM_MAX_NETS];
	uint32_t			net_count;
	enum anx_vm_display		display;
	struct anx_vm_agent_policy	agent_policy;
};

/* --- VM Object (kernel-internal representation) --- */

struct anx_vm_object {
	bool				in_use;		/* slot occupied */
	anx_oid_t			oid;		/* unique VM ID */
	struct anx_vm_config		config;
	enum anx_vm_state		state;
	anx_oid_t			cell_oid;	/* current ANX_CELL_VM, if running */
	anx_oid_t			parent_vm_oid;	/* set for snapshots */
	uint64_t			snapshot_time;	/* set for snapshots */
	struct anx_vm_backend		*backend;	/* selected hypervisor */
	void				*backend_priv;	/* backend-specific state */
};

/* --- Runtime stats --- */

struct anx_vm_stats {
	uint64_t	cpu_time_us;
	uint64_t	mem_used_mb;
	uint64_t	disk_read_bytes;
	uint64_t	disk_write_bytes;
	uint64_t	net_rx_bytes;
	uint64_t	net_tx_bytes;
	uint64_t	uptime_seconds;
};

/* --- Public API --- */

/* Creation and deletion */
int anx_vm_create(const struct anx_vm_config *config, anx_oid_t *vm_oid_out);
int anx_vm_destroy(const anx_oid_t *vm_oid);

/* Lifecycle */
int anx_vm_start(const anx_oid_t *vm_oid);
int anx_vm_stop(const anx_oid_t *vm_oid, bool force);
int anx_vm_pause(const anx_oid_t *vm_oid);
int anx_vm_resume(const anx_oid_t *vm_oid);

/* Configuration */
int anx_vm_config_get_field(const anx_oid_t *vm_oid, const char *field,
			    char *out, uint32_t out_size);
int anx_vm_config_set_field(const anx_oid_t *vm_oid, const char *field,
			    const char *value, uint32_t field_mask);
int anx_vm_config_dump(const anx_oid_t *vm_oid,
		       struct anx_vm_config *config_out);

/* Snapshots */
int anx_vm_snapshot(const anx_oid_t *vm_oid, const char *name,
		    anx_oid_t *snap_out);
int anx_vm_clone(const anx_oid_t *snap_oid, const char *new_name,
		 anx_oid_t *new_vm_out);

/* Disk management */
int anx_vm_disk_create(uint64_t size_bytes, enum anx_vm_disk_format fmt,
		       anx_oid_t *disk_out);
int anx_vm_disk_attach(const anx_oid_t *vm_oid,
		       const struct anx_vm_disk_config *cfg);
int anx_vm_disk_detach(const anx_oid_t *vm_oid, uint32_t disk_index);

/* Console and exec */
int anx_vm_console_read(const anx_oid_t *vm_oid, char *buf, uint32_t len,
			uint32_t *read_out);
int anx_vm_exec(const anx_oid_t *vm_oid, const char *command,
		char *stdout_out, uint32_t stdout_size);

/* Enumeration */
int anx_vm_list(anx_oid_t *results, uint32_t max, uint32_t *count_out);
int anx_vm_state_get(const anx_oid_t *vm_oid, enum anx_vm_state *state_out);
int anx_vm_stats_get(const anx_oid_t *vm_oid, struct anx_vm_stats *stats_out);

/* Internal: init subsystem */
int anx_vm_init(void);

/* Internal: look up vm object by oid */
struct anx_vm_object *anx_vm_object_get(const anx_oid_t *vm_oid);
void anx_vm_object_release(struct anx_vm_object *vm);

#endif /* ANX_VM_H */
