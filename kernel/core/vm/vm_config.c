/*
 * vm_config.c — VM configuration get/set/dump (RFC-0017 Section 6).
 *
 * Config fields are addressed by dotted path strings matching the
 * schema in the RFC (e.g. "cpu.count", "memory.size_mb", "network.0.mac").
 */

#include <anx/types.h>
#include <anx/vm.h>
#include <anx/string.h>
#include <anx/kprintf.h>
/* errno codes in anx/types.h */

int anx_vm_config_get_field(const anx_oid_t *vm_oid, const char *field,
			    char *out, uint32_t out_size)
{
	struct anx_vm_object *vm;
	const struct anx_vm_config *c;

	vm = anx_vm_object_get(vm_oid);
	if (!vm)
		return ANX_ENOENT;
	c = &vm->config;

	if (anx_strcmp(field, "name") == 0) {
		anx_strlcpy(out, c->name, out_size);
		return ANX_OK;
	}
	if (anx_strcmp(field, "cpu.count") == 0) {
		anx_snprintf(out, out_size, "%u", c->cpu.count);
		return ANX_OK;
	}
	if (anx_strcmp(field, "cpu.model") == 0) {
		anx_strlcpy(out, c->cpu.model, out_size);
		return ANX_OK;
	}
	if (anx_strcmp(field, "memory.size_mb") == 0) {
		anx_snprintf(out, out_size, "%llu", c->memory.size_mb);
		return ANX_OK;
	}
	if (anx_strcmp(field, "memory.balloon") == 0) {
		anx_strlcpy(out, c->memory.balloon ? "true" : "false",
			    out_size);
		return ANX_OK;
	}
	if (anx_strcmp(field, "boot.cmdline") == 0) {
		anx_strlcpy(out, c->boot.cmdline, out_size);
		return ANX_OK;
	}
	if (anx_strcmp(field, "disk_count") == 0) {
		anx_snprintf(out, out_size, "%u", c->disk_count);
		return ANX_OK;
	}
	if (anx_strcmp(field, "net_count") == 0) {
		anx_snprintf(out, out_size, "%u", c->net_count);
		return ANX_OK;
	}
	if (anx_strcmp(field, "sandbox.mode") == 0) {
		const char *s;

		switch (c->sandbox_mode) {
		case ANX_VM_SANDBOX_FIRECRACKER:	s = "firecracker"; break;
		case ANX_VM_SANDBOX_QEMU:		s = "qemu"; break;
		case ANX_VM_SANDBOX_ANX_NATIVE:		s = "anx-native"; break;
		case ANX_VM_SANDBOX_AUTO:		/* fallthrough */
		default:				s = "auto"; break;
		}
		anx_strlcpy(out, s, out_size);
		return ANX_OK;
	}
	if (anx_strcmp(field, "sandbox.input_count") == 0) {
		anx_snprintf(out, out_size, "%u", c->sandbox.input_count);
		return ANX_OK;
	}
	if (anx_strcmp(field, "sandbox.output_count") == 0) {
		anx_snprintf(out, out_size, "%u", c->sandbox.output_count);
		return ANX_OK;
	}

	return ANX_ENOENT;
}

int anx_vm_config_set_field(const anx_oid_t *vm_oid, const char *field,
			    const char *value, uint32_t field_mask)
{
	struct anx_vm_object *vm;
	struct anx_vm_config *c;

	vm = anx_vm_object_get(vm_oid);
	if (!vm)
		return ANX_ENOENT;

	/* Snapshots are sealed — no writes (Phase 2: check ANX_OBJ_SEALED flag) */
	if (!vm->in_use)
		return ANX_EPERM;

	c = &vm->config;

	if (anx_strcmp(field, "cpu.count") == 0) {
		uint32_t v;

		if (!(field_mask & ANX_VM_FIELD_CPU))
			return ANX_EPERM;
		v = (uint32_t)anx_strtoul(value, NULL, 10);
		if (v == 0 || v > 256)
			return ANX_EINVAL;
		if (vm->state == ANX_VM_RUNNING)
			return ANX_ENOTSUP;	/* hotplug not yet implemented */
		c->cpu.count = v;
		return ANX_OK;
	}
	if (anx_strcmp(field, "memory.size_mb") == 0) {
		uint64_t v;

		if (!(field_mask & ANX_VM_FIELD_MEMORY))
			return ANX_EPERM;
		v = anx_strtoull(value, NULL, 10);
		if (v == 0 || v > 1024 * 1024)
			return ANX_EINVAL;
		if (vm->state == ANX_VM_RUNNING)
			return ANX_ENOTSUP;
		c->memory.size_mb = v;
		return ANX_OK;
	}
	if (anx_strcmp(field, "boot.cmdline") == 0) {
		if (!(field_mask & ANX_VM_FIELD_BOOT))
			return ANX_EPERM;
		if (vm->state == ANX_VM_RUNNING)
			return ANX_ENOTSUP;
		anx_strlcpy(c->boot.cmdline, value, sizeof(c->boot.cmdline));
		return ANX_OK;
	}

	return ANX_ENOENT;
}

int anx_vm_config_dump(const anx_oid_t *vm_oid, struct anx_vm_config *config_out)
{
	struct anx_vm_object *vm;

	vm = anx_vm_object_get(vm_oid);
	if (!vm)
		return ANX_ENOENT;

	anx_memcpy(config_out, &vm->config, sizeof(*config_out));
	return ANX_OK;
}
