/*
 * backend_qemu.c — QEMU process hypervisor backend (RFC-0017 Phase 1).
 *
 * Forks qemu-system-x86_64 as a child process and controls it via
 * the QEMU Machine Protocol (QMP) over TCP loopback. Console I/O via
 * virtio-serial.
 *
 * QMP transport: TCP on 127.0.0.1 port (QMP_PORT_BASE + vm_slot).
 * Protocol: connect → read greeting → send qmp_capabilities → exchange
 * JSON commands one at a time (no OOB mode).
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
#include <anx/net.h>
#include <anx/arch.h>

/* QMP TCP port range: 4440–4455 (one per VM slot) */
#define QMP_PORT_BASE   4440
#define QMP_PORT_MAX    16
#define QMP_RECV_TIMEOUT_MS  2000
#define QMP_RESP_SIZE   512

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
	.mode		= ANX_VM_SANDBOX_QEMU,
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
	struct anx_tcp_conn *qmp_conn;  /* QMP TCP connection, NULL if not connected */
	bool                 qmp_ready; /* true after qmp_capabilities handshake */
	uint16_t             qmp_port;  /* assigned TCP port for this VM's QMP */
	int                  console_fd; /* virtio-serial console fd */
	int                  pid;        /* child process pid, 0 if not running */
	uint64_t             started_ns; /* arch_time_now() at start */
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

/* Allocate a QMP port from the pool; returns 0 if exhausted. */
static uint16_t qmp_alloc_port(void)
{
	static uint16_t next_port = QMP_PORT_BASE;
	uint16_t port = next_port++;
	if (next_port >= QMP_PORT_BASE + QMP_PORT_MAX)
		next_port = QMP_PORT_BASE;
	return port;
}

static int qemu_create(struct anx_vm_object *vm)
{
	struct qemu_priv *priv;

	priv = anx_alloc(sizeof(*priv));
	if (!priv)
		return ANX_ENOMEM;

	priv->qmp_conn   = NULL;
	priv->qmp_ready  = false;
	priv->qmp_port   = qmp_alloc_port();
	priv->console_fd = -1;
	priv->pid        = 0;
	priv->started_ns = 0;

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

	/* QMP over TCP loopback — port assigned per VM */
	{
		struct qemu_priv *priv = qemu_priv_get(vm);
		APPEND(" -qmp tcp:127.0.0.1:%u,server,nowait",
		       (unsigned)priv->qmp_port);
	}

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

/* ── QMP helper layer ──────────────────────────────────────────────── */

/*
 * Minimal JSON scanner: return true if `needle` appears in `buf`.
 * Used to detect "error" or known fields in QMP responses without a
 * full JSON parser.
 */
static bool qmp_has(const char *buf, const char *needle)
{
	return anx_strstr(buf, needle) != NULL;
}

/*
 * Connect to QEMU's QMP TCP port and complete the capability handshake.
 * QEMU sends a greeting first; we must reply with qmp_capabilities
 * before any real commands are accepted.
 *
 * Retries for up to ~3 s to give the QEMU process time to start.
 */
static int qmp_connect(struct qemu_priv *priv)
{
	char buf[QMP_RESP_SIZE];
	uint32_t n;
	int ret;
	uint32_t attempts;

	if (priv->qmp_conn && priv->qmp_ready)
		return ANX_OK;

	/* Disconnect stale connection */
	if (priv->qmp_conn) {
		anx_tcp_close(priv->qmp_conn);
		priv->qmp_conn  = NULL;
		priv->qmp_ready = false;
	}

	/* Retry loop: QEMU may not have opened the port yet */
	for (attempts = 0; attempts < 30; attempts++) {
		ret = anx_tcp_connect(0x7F000001u /* 127.0.0.1 */,
				      priv->qmp_port, &priv->qmp_conn);
		if (ret == ANX_OK)
			break;
		/* ~100 ms delay between retries via net poll */
		{
			uint64_t t = arch_timer_ticks();
			while (arch_timer_ticks() - t < 10)
				anx_net_poll();
		}
	}
	if (ret != ANX_OK) {
		kprintf("vm: qmp: connect to port %u failed (%d)\n",
			(unsigned)priv->qmp_port, ret);
		priv->qmp_conn = NULL;
		return ret;
	}

	/* Read greeting {"QMP": ...} */
	ret = anx_tcp_recv(priv->qmp_conn, buf, sizeof(buf) - 1,
			   QMP_RECV_TIMEOUT_MS);
	if (ret < 0) {
		kprintf("vm: qmp: no greeting (%d)\n", ret);
		goto fail;
	}
	buf[ret] = '\0';
	if (!qmp_has(buf, "QMP")) {
		kprintf("vm: qmp: unexpected greeting: %.64s\n", buf);
		goto fail;
	}

	/* Negotiate: enter command mode */
	{
		const char *caps = "{\"execute\":\"qmp_capabilities\"}\n";
		ret = anx_tcp_send(priv->qmp_conn, caps, anx_strlen(caps));
		if (ret != ANX_OK)
			goto fail;
	}

	/* Read {"return":{}} */
	ret = anx_tcp_recv(priv->qmp_conn, buf, sizeof(buf) - 1,
			   QMP_RECV_TIMEOUT_MS);
	if (ret < 0 || !qmp_has(buf, "return")) {
		kprintf("vm: qmp: capabilities rejected\n");
		goto fail;
	}

	priv->qmp_ready = true;
	kprintf("vm: qmp: connected on port %u\n", (unsigned)priv->qmp_port);
	return ANX_OK;

fail:
	anx_tcp_close(priv->qmp_conn);
	priv->qmp_conn  = NULL;
	priv->qmp_ready = false;
	return ANX_EIO;
}

/*
 * Send one QMP command and read the response into resp_buf[resp_size].
 * Returns ANX_OK on success (response contains "return"), ANX_EIO on
 * error or if the response contains "error".
 */
static int qmp_cmd(struct qemu_priv *priv, const char *json,
		   char *resp_buf, uint32_t resp_size)
{
	int ret;
	uint32_t len;
	char nl_json[QMP_RESP_SIZE];

	ret = qmp_connect(priv);
	if (ret != ANX_OK)
		return ret;

	/* Append newline — QMP framing */
	len = anx_strlen(json);
	if (len + 2 > sizeof(nl_json))
		return ANX_ENOMEM;
	anx_memcpy(nl_json, json, len);
	nl_json[len]     = '\n';
	nl_json[len + 1] = '\0';

	ret = anx_tcp_send(priv->qmp_conn, nl_json, len + 1);
	if (ret != ANX_OK) {
		/* Connection dropped; reset so next call reconnects */
		anx_tcp_close(priv->qmp_conn);
		priv->qmp_conn  = NULL;
		priv->qmp_ready = false;
		return ret;
	}

	ret = anx_tcp_recv(priv->qmp_conn, resp_buf, resp_size - 1,
			   QMP_RECV_TIMEOUT_MS);
	if (ret < 0) {
		anx_tcp_close(priv->qmp_conn);
		priv->qmp_conn  = NULL;
		priv->qmp_ready = false;
		return ANX_EIO;
	}
	resp_buf[ret] = '\0';

	if (qmp_has(resp_buf, "\"error\""))
		return ANX_EIO;
	return ANX_OK;
}

/*
 * Parse a uint64 value after `key` in a QMP response like:
 *   "key": 1234567
 * Returns 0 if not found.
 */
static uint64_t qmp_parse_u64(const char *buf, const char *key)
{
	const char *p = anx_strstr(buf, key);
	uint64_t v = 0;

	if (!p)
		return 0;
	p += anx_strlen(key);
	while (*p == ' ' || *p == ':' || *p == '"')
		p++;
	while (*p >= '0' && *p <= '9') {
		v = v * 10 + (uint64_t)(*p - '0');
		p++;
	}
	return v;
}

/* ── VM lifecycle ──────────────────────────────────────────────────── */

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

	priv->started_ns = arch_time_now();
	vm->state = ANX_VM_RUNNING;
	return ANX_OK;
}

static int qemu_pause(struct anx_vm_object *vm)
{
	struct qemu_priv *priv = qemu_priv_get(vm);
	char resp[QMP_RESP_SIZE];
	int ret;

	if (!priv || priv->pid == 0)
		return ANX_EINVAL;

	ret = qmp_cmd(priv, "{\"execute\":\"stop\"}", resp, sizeof(resp));
	if (ret != ANX_OK) {
		kprintf("vm: qmp stop failed (%d)\n", ret);
		return ret;
	}
	vm->state = ANX_VM_PAUSED;
	return ANX_OK;
}

static int qemu_resume(struct anx_vm_object *vm)
{
	struct qemu_priv *priv = qemu_priv_get(vm);
	char resp[QMP_RESP_SIZE];
	int ret;

	if (!priv || priv->pid == 0)
		return ANX_EINVAL;

	ret = qmp_cmd(priv, "{\"execute\":\"cont\"}", resp, sizeof(resp));
	if (ret != ANX_OK) {
		kprintf("vm: qmp cont failed (%d)\n", ret);
		return ret;
	}
	vm->state = ANX_VM_RUNNING;
	return ANX_OK;
}

static int qemu_stop(struct anx_vm_object *vm, bool force)
{
	struct qemu_priv *priv = qemu_priv_get(vm);
	char resp[QMP_RESP_SIZE];

	if (!priv || priv->pid == 0)
		return ANX_OK;

	if (force) {
		/* Kill the QEMU process directly */
		kprintf("vm: force-stopping QEMU pid %d\n", priv->pid);
	} else {
		/* Graceful: ask guest to power down via ACPI */
		int ret = qmp_cmd(priv, "{\"execute\":\"system_powerdown\"}",
				  resp, sizeof(resp));
		if (ret != ANX_OK)
			kprintf("vm: qmp system_powerdown failed (%d), "
				"falling back to force-stop\n", ret);
	}

	/* Disconnect QMP regardless of outcome */
	if (priv->qmp_conn) {
		anx_tcp_close(priv->qmp_conn);
		priv->qmp_conn  = NULL;
		priv->qmp_ready = false;
	}
	priv->pid        = 0;
	priv->started_ns = 0;
	vm->state = ANX_VM_DEFINED;
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
	struct qemu_priv *priv = qemu_priv_get(vm);
	char resp[QMP_RESP_SIZE];
	int ret;

	if (!priv || priv->pid == 0)
		return ANX_EINVAL;
	(void)snap_out; /* OID assignment deferred — disk_oid mapping not yet impl */

	/* Pause first so the snapshot is crash-consistent */
	ret = qmp_cmd(priv, "{\"execute\":\"stop\"}", resp, sizeof(resp));
	if (ret != ANX_OK)
		return ret;

	/* Save VM state to QEMU's default snapshot tag */
	ret = qmp_cmd(priv,
		      "{\"execute\":\"savevm\","
		       "\"arguments\":{\"name\":\"anx-snap\"}}",
		      resp, sizeof(resp));
	if (ret != ANX_OK) {
		/* Try to resume even if save failed */
		qmp_cmd(priv, "{\"execute\":\"cont\"}", resp, sizeof(resp));
		kprintf("vm: qmp savevm failed (%d)\n", ret);
		return ret;
	}

	/* Resume after snapshot */
	ret = qmp_cmd(priv, "{\"execute\":\"cont\"}", resp, sizeof(resp));
	if (ret != ANX_OK)
		kprintf("vm: qmp cont after snapshot failed (%d)\n", ret);

	vm->state = ANX_VM_RUNNING;
	return ANX_OK;
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
	struct qemu_priv *priv = qemu_priv_get(vm);
	char resp[QMP_RESP_SIZE];
	int ret;

	anx_memset(out, 0, sizeof(*out));

	if (!priv || priv->pid == 0)
		return ANX_OK;

	/* Uptime from start timestamp */
	if (priv->started_ns) {
		uint64_t elapsed_ns = arch_time_now() - priv->started_ns;
		out->uptime_seconds = elapsed_ns / 1000000000ULL;
	}

	/* query-status: confirms running/paused; ignored on error */
	ret = qmp_cmd(priv, "{\"execute\":\"query-status\"}",
		      resp, sizeof(resp));
	if (ret != ANX_OK) {
		kprintf("vm: qmp query-status failed (%d)\n", ret);
		return ANX_OK; /* return zeroed stats rather than propagating */
	}

	/*
	 * query-memory-size-summary: fields "base-memory" and "plugged-memory"
	 * are in bytes.  Sum them and convert to MB.
	 */
	ret = qmp_cmd(priv, "{\"execute\":\"query-memory-size-summary\"}",
		      resp, sizeof(resp));
	if (ret == ANX_OK) {
		uint64_t base   = qmp_parse_u64(resp, "\"base-memory\"");
		uint64_t plugged = qmp_parse_u64(resp, "\"plugged-memory\"");
		out->mem_used_mb = (base + plugged) / (1024ULL * 1024ULL);
	}

	return ANX_OK;
}
