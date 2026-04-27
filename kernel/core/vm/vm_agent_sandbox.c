/*
 * vm_agent_sandbox.c — Agent-facing sandbox helpers (RFC-0017 sandbox
 * extension).
 *
 * Agents almost never want to author a complete struct anx_vm_config from
 * scratch.  They want to say: "give me a sandbox of mode X, hand it these
 * inputs, run this workflow, return its outputs."  This file provides
 * that thin facade on top of anx_vm_create().
 *
 * Defaults:
 *   - cpu.count           = 1     (microVMs prefer single vCPU)
 *   - memory.size_mb      = 256   (firecracker default)
 *   - boot.firmware       = direct_kernel for firecracker; bios for qemu;
 *                           irrelevant for anx-native
 *   - display             = none
 *   - net_count / disk_count remain 0 unless the caller filled them in
 *
 * Resource caps from the sandbox spec (memory_kb_cap) are clamped against
 * whatever the caller put in cfg->memory.size_mb so an aggressive cap
 * always wins over a permissive request.
 */

#include <anx/types.h>
#include <anx/vm.h>
#include <anx/string.h>
#include <anx/kprintf.h>

static uint32_t sandbox_name_seq;

static void sandbox_apply_defaults(struct anx_vm_config *c)
{
	if (c->cpu.count == 0)
		c->cpu.count = 1;
	if (c->memory.size_mb == 0)
		c->memory.size_mb = 256;
	if (c->display == 0)
		c->display = ANX_VM_DISPLAY_NONE;

	switch (c->sandbox_mode) {
	case ANX_VM_SANDBOX_FIRECRACKER:
		/* Firecracker has no firmware; force direct_kernel. */
		c->boot.firmware = ANX_VM_FW_DIRECT_KERNEL;
		c->memory.balloon = false;
		break;
	case ANX_VM_SANDBOX_ANX_NATIVE:
		/*
		 * Anunix-native sandboxes don't boot a guest, so firmware
		 * is meaningless.  Pick a value that won't trip validation
		 * elsewhere.
		 */
		c->boot.firmware = ANX_VM_FW_DIRECT_KERNEL;
		break;
	case ANX_VM_SANDBOX_QEMU:
	case ANX_VM_SANDBOX_AUTO:
	default:
		break;
	}
}

static void sandbox_apply_caps(struct anx_vm_config *c)
{
	const struct anx_vm_agent_sandbox *s = &c->sandbox;
	uint64_t cap_mb;

	if (s->memory_kb_cap == 0)
		return;

	cap_mb = s->memory_kb_cap / 1024ULL;
	if (cap_mb == 0)
		cap_mb = 1;
	if (c->memory.size_mb > cap_mb)
		c->memory.size_mb = cap_mb;
}

static void sandbox_default_name(struct anx_vm_config *c)
{
	if (c->name[0] != '\0')
		return;
	anx_snprintf(c->name, ANX_VM_NAME_MAX, "anx-sandbox-%u",
		     ++sandbox_name_seq);
}

int anx_vm_agent_sandbox_create(const struct anx_vm_config *cfg,
				anx_oid_t *vm_oid_out)
{
	struct anx_vm_config working;

	if (!cfg || !vm_oid_out)
		return ANX_EINVAL;

	if (cfg->sandbox.input_count > ANX_VM_SANDBOX_MAX_PORTS ||
	    cfg->sandbox.output_count > ANX_VM_SANDBOX_MAX_PORTS)
		return ANX_EINVAL;

	anx_memcpy(&working, cfg, sizeof(working));
	sandbox_default_name(&working);
	sandbox_apply_defaults(&working);
	sandbox_apply_caps(&working);

	kprintf("vm: agent sandbox '%s' mode=%d cpu=%u mem=%uMB "
		"inputs=%u workflow=%s\n",
		working.name, (int)working.sandbox_mode,
		working.cpu.count, (uint32_t)working.memory.size_mb,
		working.sandbox.input_count,
		(working.sandbox.workflow_oid.hi |
		 working.sandbox.workflow_oid.lo) ? "yes" : "no");

	return anx_vm_create(&working, vm_oid_out);
}

int anx_vm_agent_sandbox_get(const anx_oid_t *vm_oid,
			     struct anx_vm_agent_sandbox *out)
{
	struct anx_vm_object *vm;

	if (!vm_oid || !out)
		return ANX_EINVAL;

	vm = anx_vm_object_get(vm_oid);
	if (!vm)
		return ANX_ENOENT;

	anx_memcpy(out, &vm->config.sandbox, sizeof(*out));
	return ANX_OK;
}

int anx_vm_agent_sandbox_attach_group(const anx_oid_t *vm_oid,
				      const anx_oid_t *group_oid)
{
	struct anx_vm_object *vm;
	struct anx_vm_agent_sandbox *s;

	if (!vm_oid || !group_oid)
		return ANX_EINVAL;
	vm = anx_vm_object_get(vm_oid);
	if (!vm)
		return ANX_ENOENT;
	if (vm->state != ANX_VM_DEFINED)
		return ANX_EBUSY;

	s = &vm->config.sandbox;
	if (s->group_count >= ANX_VM_SANDBOX_MAX_GROUPS)
		return ANX_ENOMEM;
	s->groups[s->group_count++] = *group_oid;
	return ANX_OK;
}
