/*
 * backend_qemu.c — QEMU process hypervisor backend (RFC-0017 Phase 1).
 *
 * Forks qemu-system-x86_64 as a child process and controls it via
 * the QEMU Machine Protocol (QMP) socket. Console I/O via virtio-serial.
 *
 * On bare metal this backend is not used (no OS to fork into).
 * On Linux/jekyll it is the primary backend until KVM is implemented.
 * On macOS it enables development testing without KVM.
 *
 * In the kernel's freestanding environment this file provides the
 * interface stubs; the actual fork/exec is performed by the POSIX
 * compatibility layer (kernel/core/posix/).
 */

#include <anx/types.h>
#include <anx/vm.h>
#include <anx/vm_backend.h>
#include <anx/string.h>
#include <anx/kprintf.h>
#include <anx/alloc.h>
#include <anx/posix.h>

/* Forward declarations */
static bool qemu_available(void);
static int  qemu_create(struct anx_vm_object *vm);
static int  qemu_start(struct anx_vm_object *vm);
static int  qemu_pause(struct anx_vm_object *vm);
static int  qemu_resume(struct anx_vm_object *vm);
static int  qemu_stop(struct anx_vm_object *vm, bool force);
static int  qemu_destroy(struct anx_vm_object *vm);
static int  qemu_snapshot(struct anx_vm_object *vm, anx_oid_t *snap_out);
static int  qemu_hotplug_cpu(struct anx_vm_object *vm, uint32_t count);
static int  qemu_hotplug_mem(struct anx_vm_object *vm, uint64_t size_mb);
static int  qemu_hotplug_net(struct anx_vm_object *vm,
			     const struct anx_vm_netdev *dev);
static int  qemu_console_read(struct anx_vm_object *vm, char *buf,
			      uint32_t len, uint32_t *read_out);
static int  qemu_console_write(struct anx_vm_object *vm, const char *buf,
			       uint32_t len);
static int  qemu_exec(struct anx_vm_object *vm, const char *cmd,
		      char *stdout_out, uint32_t stdout_size);
static int  qemu_stats(struct anx_vm_object *vm, struct anx_vm_stats *out);

struct anx_vm_backend anx_vm_backend_qemu = {
	.name		= "qemu",
	.available	= qemu_available,
	.create		= qemu_create,
	.start		= qemu_start,
	.pause		= qemu_pause,
	.resume		= qemu_resume,
	.stop		= qemu_stop,
	.destroy	= qemu_destroy,
	.snapshot	= qemu_snapshot,
	.hotplug_cpu	= qemu_hotplug_cpu,
	.hotplug_mem	= qemu_hotplug_mem,
	.hotplug_net	= qemu_hotplug_net,
	.console_read	= qemu_console_read,
	.console_write	= qemu_console_write,
	.exec		= qemu_exec,
	.stats		= qemu_stats,
};

/*
 * Per-VM QEMU process state.
 * Allocated in backend_priv on the vm object.
 */
struct qemu_priv {
	int	qmp_fd;		/* QMP Unix socket fd, -1 if not connected */
	int	console_fd;	/* virtio-serial console fd */
	int	pid;		/* child process pid, 0 if not running */
};

static bool qemu_available(void)
{
	/* Always claim available; actual binary check happens at start time */
	return true;
}

static struct qemu_priv *qemu_priv_get(struct anx_vm_object *vm)
{
	return (struct qemu_priv *)vm->backend_priv;
}

static int qemu_create(struct anx_vm_object *vm)
{
	struct qemu_priv *priv;

	priv = anx_alloc(sizeof(*priv));
	if (!priv)
		return ANX_ENOMEM;

	priv->qmp_fd     = -1;
	priv->console_fd = -1;
	priv->pid        = 0;

	vm->backend_priv = priv;
	return ANX_OK;
}

/*
 * Build the QEMU command line from the VM config.
 * Writes at most buf_size bytes into buf.
 */
static int qemu_build_cmdline(struct anx_vm_object *vm, char *buf,
			      uint32_t buf_size)
{
	const struct anx_vm_config *c = &vm->config;
	uint32_t n = 0;

#define APPEND(fmt, ...) \
	do { \
		int _r = anx_snprintf(buf + n, buf_size - n, fmt, ##__VA_ARGS__); \
		if (_r < 0 || (uint32_t)_r >= buf_size - n) return ANX_ENOMEM; \
		n += (uint32_t)_r; \
	} while (0)

	APPEND("qemu-system-x86_64");
	APPEND(" -m %lluM", c->memory.size_mb);
	APPEND(" -smp %u", c->cpu.count);
	APPEND(" -nographic");
	APPEND(" -no-reboot");

	/* Serial console → stdio */
	APPEND(" -serial mon:stdio");

	/* QMP socket for control */
	APPEND(" -qmp unix:/tmp/anx-vm-%s.sock,server,nowait", c->name);

	/* Network */
	if (c->net_count > 0) {
		APPEND(" -netdev user,id=net0");
		if (c->nets[0].fwd_host_port)
			APPEND(",hostfwd=tcp::%u-:%u",
			       c->nets[0].fwd_host_port,
			       c->nets[0].fwd_guest_port);
		APPEND(" -device virtio-net-pci,netdev=net0");
		if (c->nets[0].mac[0])
			APPEND(",mac=%s", c->nets[0].mac);
	}

	/* Disks */
	{
		uint32_t i;

		for (i = 0; i < c->disk_count; i++) {
			APPEND(" -drive file=/dev/zero,format=raw,if=virtio");
			/* TODO: map disk_oid to actual file path */
		}
	}

	/* Boot */
	if (c->boot.firmware == ANX_VM_FW_DIRECT_KERNEL &&
	    c->boot.cmdline[0]) {
		APPEND(" -append \"%s\"", c->boot.cmdline);
	}

#undef APPEND
	return ANX_OK;
}

static int qemu_start(struct anx_vm_object *vm)
{
	struct qemu_priv *priv = qemu_priv_get(vm);
	char cmdline[1024];
	int ret;

	if (!priv)
		return ANX_EINVAL;

	ret = qemu_build_cmdline(vm, cmdline, sizeof(cmdline));
	if (ret != ANX_OK)
		return ret;

	kprintf("vm: qemu start: %s\n", cmdline);

	/*
	 * Fork a child cell and exec QEMU in it.
	 * anx_posix_fork() returns the child CID; we store the lo word
	 * as a proxy PID for stop/kill.
	 */
	{
		anx_cid_t child = anx_posix_fork();

		if (child.hi == 0 && child.lo == 0)
			return ANX_ENOTSUP;	/* fork not available (bare metal) */

		priv->pid = (int)(child.lo & 0x7fffffff);
		ret = anx_posix_exec(cmdline);
		if (ret != ANX_OK) {
			kprintf("vm: qemu exec failed (%d)\n", ret);
			priv->pid = 0;
			return ret;
		}
	}

	return ANX_OK;
}

static int qemu_pause(struct anx_vm_object *vm)
{
	(void)vm;
	/* TODO: send QMP stop command */
	return ANX_ENOTSUP;
}

static int qemu_resume(struct anx_vm_object *vm)
{
	(void)vm;
	/* TODO: send QMP cont command */
	return ANX_ENOTSUP;
}

static int qemu_stop(struct anx_vm_object *vm, bool force)
{
	struct qemu_priv *priv = qemu_priv_get(vm);

	if (!priv || priv->pid == 0)
		return ANX_OK;

	/* TODO: send QMP system_powerdown for graceful stop */
	(void)force;
	priv->pid = 0;
	return ANX_OK;
}

static int qemu_destroy(struct anx_vm_object *vm)
{
	struct qemu_priv *priv = qemu_priv_get(vm);

	if (!priv)
		return ANX_OK;

	if (priv->pid != 0)
		qemu_stop(vm, true);

	anx_free(priv);
	vm->backend_priv = NULL;
	return ANX_OK;
}

static int qemu_snapshot(struct anx_vm_object *vm, anx_oid_t *snap_out)
{
	(void)vm; (void)snap_out;
	/* TODO: QMP savevm + copy object tree */
	return ANX_ENOTSUP;
}

static int qemu_hotplug_cpu(struct anx_vm_object *vm, uint32_t count)
{
	(void)vm; (void)count;
	return ANX_ENOTSUP;
}

static int qemu_hotplug_mem(struct anx_vm_object *vm, uint64_t size_mb)
{
	(void)vm; (void)size_mb;
	return ANX_ENOTSUP;
}

static int qemu_hotplug_net(struct anx_vm_object *vm,
			    const struct anx_vm_netdev *dev)
{
	(void)vm; (void)dev;
	return ANX_ENOTSUP;
}

static int qemu_console_read(struct anx_vm_object *vm, char *buf,
			     uint32_t len, uint32_t *read_out)
{
	struct qemu_priv *priv = qemu_priv_get(vm);

	if (!priv || priv->console_fd < 0)
		return ANX_EINVAL;

	{
		ssize_t n = anx_posix_read(priv->console_fd, buf, len);

		if (n < 0)
			return (int)n;
		*read_out = (uint32_t)n;
		return ANX_OK;
	}
}

static int qemu_console_write(struct anx_vm_object *vm, const char *buf,
			      uint32_t len)
{
	struct qemu_priv *priv = qemu_priv_get(vm);

	if (!priv || priv->console_fd < 0)
		return ANX_EINVAL;

	return anx_posix_write(priv->console_fd, buf, len);
}

static int qemu_exec(struct anx_vm_object *vm, const char *cmd,
		     char *stdout_out, uint32_t stdout_size)
{
	(void)vm; (void)cmd; (void)stdout_out; (void)stdout_size;
	/* TODO: guest agent protocol over virtio-serial */
	return ANX_ENOTSUP;
}

static int qemu_stats(struct anx_vm_object *vm, struct anx_vm_stats *out)
{
	(void)vm;
	anx_memset(out, 0, sizeof(*out));
	/* TODO: QMP query-kvm / query-status */
	return ANX_OK;
}
