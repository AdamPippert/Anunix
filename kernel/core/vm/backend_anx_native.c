/*
 * backend_anx_native.c — Anunix-native sandbox backend (RFC-0017 sandbox
 * extension).
 *
 * The Anunix-native sandbox is *not* a hardware VM.  It is an isolated
 * execution context that runs on the host kernel with explicit object
 * passing and a workflow object reference.  Conceptually it sits between
 * a Firecracker microVM (which still emulates a CPU) and a plain cell
 * spawn (which has no resource ceiling and no policy gate).
 *
 * Why a separate backend:
 *
 *   - Agents frequently need a sandboxed unit of work that consumes
 *     State Object inputs and produces State Object outputs.  The
 *     workflow object is the most natural unit.
 *   - Booting a kernel for a 5-second tool call is wasteful; a real
 *     hypervisor adds 200ms+ of startup latency that dominates a
 *     small computation.
 *   - The cell runtime already provides admission, tracing, and policy
 *     gates.  Wrapping it as a "VM Object" lets agents speak a single
 *     API regardless of whether the sandbox is a hypervisor or a cell.
 *
 * What this backend actually does:
 *
 *   - On start: validate the agent_sandbox spec (input OIDs exist,
 *     workflow OID exists, output capacity sane), record the start
 *     timestamp, and run the referenced workflow synchronously.
 *   - On stop: collect the workflow's output OIDs into the sandbox spec
 *     so the caller can read them via anx_vm_agent_sandbox_get().
 *   - Pause/resume map to workflow-runtime semantics; in this phase
 *     they are no-ops because the workflow runs to completion in start().
 *
 * The backend never forks — it runs entirely in the host kernel — so it
 * works on bare metal where firecracker and qemu return ANX_ENOTSUP.
 */

#include <anx/types.h>
#include <anx/vm.h>
#include <anx/vm_backend.h>
#include <anx/string.h>
#include <anx/kprintf.h>
#include <anx/alloc.h>
#include <anx/state_object.h>
#include <anx/workflow.h>
#include <anx/arch.h>
#include <anx/sandbox_lens.h>

/* Forward declarations */
static bool anx_native_available(void);
static int  anx_native_create(struct anx_vm_object *vm);
static int  anx_native_start(struct anx_vm_object *vm);
static int  anx_native_pause(struct anx_vm_object *vm);
static int  anx_native_resume(struct anx_vm_object *vm);
static int  anx_native_stop(struct anx_vm_object *vm, bool force);
static int  anx_native_destroy(struct anx_vm_object *vm);
static int  anx_native_snapshot(struct anx_vm_object *vm, anx_oid_t *snap_out);
static int  anx_native_hotplug_cpu(struct anx_vm_object *vm, uint32_t count);
static int  anx_native_hotplug_mem(struct anx_vm_object *vm, uint64_t size_mb);
static int  anx_native_hotplug_net(struct anx_vm_object *vm,
				   const struct anx_vm_netdev *dev);
static int  anx_native_console_read(struct anx_vm_object *vm, char *buf,
				    uint32_t len, uint32_t *read_out);
static int  anx_native_console_write(struct anx_vm_object *vm, const char *buf,
				     uint32_t len);
static int  anx_native_exec(struct anx_vm_object *vm, const char *cmd,
			    char *stdout_out, uint32_t stdout_size);
static int  anx_native_stats(struct anx_vm_object *vm,
			     struct anx_vm_stats *out);

struct anx_vm_backend anx_vm_backend_anx_native = {
	.name		= "anx-native",
	.mode		= ANX_VM_SANDBOX_ANX_NATIVE,
	.available	= anx_native_available,
	.create		= anx_native_create,
	.start		= anx_native_start,
	.pause		= anx_native_pause,
	.resume		= anx_native_resume,
	.stop		= anx_native_stop,
	.destroy	= anx_native_destroy,
	.snapshot	= anx_native_snapshot,
	.hotplug_cpu	= anx_native_hotplug_cpu,
	.hotplug_mem	= anx_native_hotplug_mem,
	.hotplug_net	= anx_native_hotplug_net,
	.console_read	= anx_native_console_read,
	.console_write	= anx_native_console_write,
	.exec		= anx_native_exec,
	.stats		= anx_native_stats,
};

/*
 * Per-VM Anunix-native state.
 */
struct anx_native_priv {
	uint64_t			started_ns;
	uint64_t			stopped_ns;
	bool				ran;	  /* true once workflow has been invoked */
	int				last_status;
	struct anx_sandbox_lens		lens;	  /* installed at start */
	struct anx_sandbox_lens_save	lens_save;
	bool				lens_installed;
};

static bool oid_is_zero(const anx_oid_t *o)
{
	return o->hi == 0 && o->lo == 0;
}

static bool anx_native_available(void)
{
	return true;
}

static struct anx_native_priv *priv_get(struct anx_vm_object *vm)
{
	return (struct anx_native_priv *)vm->backend_priv;
}

static int anx_native_create(struct anx_vm_object *vm)
{
	struct anx_native_priv *priv;
	const struct anx_vm_agent_sandbox *s = &vm->config.sandbox;

	if (s->input_count > ANX_VM_SANDBOX_MAX_PORTS ||
	    s->output_count > ANX_VM_SANDBOX_MAX_PORTS)
		return ANX_EINVAL;

	priv = anx_alloc(sizeof(*priv));
	if (!priv)
		return ANX_ENOMEM;

	priv->started_ns  = 0;
	priv->stopped_ns  = 0;
	priv->ran         = false;
	priv->last_status = ANX_OK;
	vm->backend_priv  = priv;
	return ANX_OK;
}

/*
 * Build the sandbox lens from the agent_sandbox spec.
 * Note: this runs *before* the lens is installed, so it uses the raw
 * spec — no objstore access needed here.
 */
static void build_lens(const struct anx_vm_object *vm,
		       struct anx_sandbox_lens *out)
{
	const struct anx_vm_agent_sandbox *s = &vm->config.sandbox;
	uint32_t copy_in, copy_grp;

	anx_memset(out, 0, sizeof(*out));
	out->vm_oid = vm->oid;

	copy_in = s->input_count < ANX_LENS_MAX_INPUTS
		  ? s->input_count : ANX_LENS_MAX_INPUTS;
	anx_memcpy(out->inputs, s->inputs, copy_in * sizeof(anx_oid_t));
	out->input_count = copy_in;

	anx_strlcpy(out->output_ns, s->output_ns, ANX_LENS_NS_MAX);

	copy_grp = s->group_count < ANX_LENS_MAX_GROUPS
		   ? s->group_count : ANX_LENS_MAX_GROUPS;
	anx_memcpy(out->groups, s->groups, copy_grp * sizeof(anx_oid_t));
	out->group_count = copy_grp;
}

/*
 * Start the sandbox: validate inputs exist in the object store using
 * the raw (lens-bypassing) lookup, install the lens, run the referenced
 * workflow under the lens, then restore the prior lens before returning.
 */
static int anx_native_start(struct anx_vm_object *vm)
{
	struct anx_native_priv *priv = priv_get(vm);
	struct anx_vm_agent_sandbox *s = &vm->config.sandbox;
	uint32_t i;
	int ret;

	if (!priv)
		return ANX_EINVAL;

	for (i = 0; i < s->input_count; i++) {
		struct anx_state_object *obj;

		obj = anx_objstore_lookup_raw(&s->inputs[i]);
		if (!obj) {
			kprintf("vm: anx-native: input[%u] OID not found\n", i);
			return ANX_ENOENT;
		}
		anx_objstore_release(obj);
	}

	build_lens(vm, &priv->lens);
	ret = anx_sandbox_lens_install(&priv->lens_save, &priv->lens);
	if (ret != ANX_OK) {
		kprintf("vm: anx-native: lens install failed (%d)\n", ret);
		return ret;
	}
	priv->lens_installed = true;

	priv->started_ns = arch_time_now();
	vm->state = ANX_VM_RUNNING;

	if (oid_is_zero(&s->workflow_oid)) {
		/*
		 * No workflow attached; the sandbox is a passive holder
		 * of inputs/outputs.  Leave the lens installed until
		 * stop() so any out-of-band code that reads via
		 * anx_objstore_lookup honours it.
		 */
		return ANX_OK;
	}

	{
		struct anx_wf_object *wf;
		uint32_t copy;

		ret = anx_wf_run(&s->workflow_oid, NULL);
		priv->last_status = ret;
		if (ret != ANX_OK) {
			kprintf("vm: anx-native: workflow run failed (%d)\n",
				ret);
			/*
			 * Keep the lens installed so the caller stays in the
			 * sandboxed view until they call stop().  The error
			 * is propagated; vm_object's start() wrapper will
			 * reset state to DEFINED but we still need stop() to
			 * uninstall the lens cleanly.
			 */
			return ret;
		}

		wf = anx_wf_object_get(&s->workflow_oid);
		if (!wf)
			return ANX_OK;

		copy = wf->output_count;
		if (copy > ANX_VM_SANDBOX_MAX_PORTS)
			copy = ANX_VM_SANDBOX_MAX_PORTS;
		anx_memcpy(s->outputs, wf->output_oids,
			   copy * sizeof(anx_oid_t));
		s->output_count = copy;
		priv->ran = true;
	}

	return ANX_OK;
}

static int anx_native_pause(struct anx_vm_object *vm)
{
	(void)vm;
	return ANX_OK;
}

static int anx_native_resume(struct anx_vm_object *vm)
{
	(void)vm;
	return ANX_OK;
}

static int anx_native_stop(struct anx_vm_object *vm, bool force)
{
	struct anx_native_priv *priv = priv_get(vm);

	(void)force;
	if (!priv)
		return ANX_OK;

	if (priv->lens_installed) {
		anx_sandbox_lens_restore(&priv->lens_save);
		priv->lens_installed = false;
	}

	priv->stopped_ns = arch_time_now();
	vm->state = ANX_VM_DEFINED;
	return ANX_OK;
}

static int anx_native_destroy(struct anx_vm_object *vm)
{
	struct anx_native_priv *priv = priv_get(vm);

	if (!priv)
		return ANX_OK;
	if (priv->lens_installed) {
		anx_sandbox_lens_restore(&priv->lens_save);
		priv->lens_installed = false;
	}
	anx_free(priv);
	vm->backend_priv = NULL;
	return ANX_OK;
}

static int anx_native_snapshot(struct anx_vm_object *vm, anx_oid_t *snap_out)
{
	(void)vm; (void)snap_out;
	return ANX_ENOTSUP;
}

static int anx_native_hotplug_cpu(struct anx_vm_object *vm, uint32_t count)
{
	(void)vm; (void)count;
	return ANX_ENOTSUP;
}

static int anx_native_hotplug_mem(struct anx_vm_object *vm, uint64_t size_mb)
{
	(void)vm; (void)size_mb;
	return ANX_ENOTSUP;
}

static int anx_native_hotplug_net(struct anx_vm_object *vm,
				  const struct anx_vm_netdev *dev)
{
	(void)vm; (void)dev;
	return ANX_ENOTSUP;
}

static int anx_native_console_read(struct anx_vm_object *vm, char *buf,
				   uint32_t len, uint32_t *read_out)
{
	(void)vm; (void)buf; (void)len;
	*read_out = 0;
	return ANX_OK;
}

static int anx_native_console_write(struct anx_vm_object *vm, const char *buf,
				    uint32_t len)
{
	(void)vm; (void)buf; (void)len;
	return ANX_OK;
}

static int anx_native_exec(struct anx_vm_object *vm, const char *cmd,
			   char *stdout_out, uint32_t stdout_size)
{
	(void)vm; (void)cmd; (void)stdout_out; (void)stdout_size;
	return ANX_ENOTSUP;
}

static int anx_native_stats(struct anx_vm_object *vm,
			    struct anx_vm_stats *out)
{
	struct anx_native_priv *priv = priv_get(vm);

	anx_memset(out, 0, sizeof(*out));
	if (!priv)
		return ANX_OK;

	if (priv->started_ns) {
		uint64_t end = priv->stopped_ns ? priv->stopped_ns
						: arch_time_now();
		out->uptime_seconds = (end - priv->started_ns) / 1000000000ULL;
	}
	return ANX_OK;
}
