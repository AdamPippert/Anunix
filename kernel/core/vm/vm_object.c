/*
 * vm_object.c — VM Object lifecycle management (RFC-0017).
 *
 * Manages a static registry of up to ANX_VM_MAX_VMS VM objects.
 * OIDs are generated from a monotonic counter (Phase 1 — full UUID v7 later).
 */

#include <anx/types.h>
#include <anx/vm.h>
#include <anx/vm_backend.h>
#include <anx/string.h>
#include <anx/alloc.h>
#include <anx/kprintf.h>

#define ANX_VM_MAX_VMS		32

static struct anx_vm_object vm_table[ANX_VM_MAX_VMS];
static uint32_t vm_count;
static bool vm_initialized;
static uint64_t vm_oid_seq;	/* monotonic OID counter */

static anx_oid_t vm_oid_generate(void)
{
	anx_oid_t oid;

	oid.hi = 0x414e5856494d5500ULL;	/* "ANXVMU\0\0" */
	oid.lo = ++vm_oid_seq;
	return oid;
}

static bool vm_oid_eq(const anx_oid_t *a, const anx_oid_t *b)
{
	return a->hi == b->hi && a->lo == b->lo;
}

int anx_vm_init(void)
{
	anx_memset(vm_table, 0, sizeof(vm_table));
	vm_count = 0;
	vm_oid_seq = 0;
	vm_initialized = true;
	kprintf("vm: subsystem initialized (max %u VMs)\n", ANX_VM_MAX_VMS);
	return ANX_OK;
}

struct anx_vm_object *anx_vm_object_get(const anx_oid_t *vm_oid)
{
	uint32_t i;

	if (!vm_initialized || !vm_oid)
		return NULL;

	for (i = 0; i < ANX_VM_MAX_VMS; i++) {
		if (vm_table[i].in_use &&
		    vm_oid_eq(&vm_table[i].oid, vm_oid))
			return &vm_table[i];
	}
	return NULL;
}

void anx_vm_object_release(struct anx_vm_object *vm)
{
	(void)vm;	/* reference counting deferred to Phase 2 */
}

int anx_vm_create(const struct anx_vm_config *config, anx_oid_t *vm_oid_out)
{
	struct anx_vm_object *vm;
	struct anx_vm_backend *backend;
	uint32_t i;
	int ret;

	if (!config || !vm_oid_out)
		return ANX_EINVAL;
	if (config->name[0] == '\0')
		return ANX_EINVAL;
	if (vm_count >= ANX_VM_MAX_VMS)
		return ANX_ENOMEM;

	/* Reject duplicate names */
	for (i = 0; i < ANX_VM_MAX_VMS; i++) {
		if (vm_table[i].in_use &&
		    anx_strncmp(vm_table[i].config.name, config->name,
				ANX_VM_NAME_MAX) == 0)
			return ANX_EEXIST;
	}

	/* Select backend by sandbox mode (default AUTO if unset) */
	backend = anx_vm_backend_select(config->sandbox_mode);
	if (!backend) {
		kprintf("vm: no hypervisor backend available for mode %d\n",
			(int)config->sandbox_mode);
		return ANX_ENODEV;
	}

	/* Find a free table slot */
	vm = NULL;
	for (i = 0; i < ANX_VM_MAX_VMS; i++) {
		if (!vm_table[i].in_use) {
			vm = &vm_table[i];
			break;
		}
	}
	if (!vm)
		return ANX_ENOMEM;

	anx_memset(vm, 0, sizeof(*vm));
	vm->in_use  = true;
	vm->oid     = vm_oid_generate();
	anx_memcpy(&vm->config, config, sizeof(*config));
	vm->state   = ANX_VM_DEFINED;
	vm->backend = backend;

	/* Let the backend do any internal setup */
	ret = backend->create(vm);
	if (ret != ANX_OK) {
		anx_memset(vm, 0, sizeof(*vm));
		return ret;
	}

	*vm_oid_out = vm->oid;
	vm_count++;

	kprintf("vm: created '%s' (%u cpu, %u MB) via %s [mode=%d]\n",
		config->name, config->cpu.count,
		(uint32_t)config->memory.size_mb,
		backend->name, (int)config->sandbox_mode);
	return ANX_OK;
}

int anx_vm_destroy(const anx_oid_t *vm_oid)
{
	struct anx_vm_object *vm;

	vm = anx_vm_object_get(vm_oid);
	if (!vm)
		return ANX_ENOENT;

	if (vm->state == ANX_VM_RUNNING || vm->state == ANX_VM_PAUSED)
		anx_vm_stop(vm_oid, true);

	vm->backend->destroy(vm);

	kprintf("vm: destroyed '%s'\n", vm->config.name);

	anx_memset(vm, 0, sizeof(*vm));
	vm_count--;
	return ANX_OK;
}

int anx_vm_start(const anx_oid_t *vm_oid)
{
	struct anx_vm_object *vm;
	int ret;

	vm = anx_vm_object_get(vm_oid);
	if (!vm)
		return ANX_ENOENT;
	if (vm->state == ANX_VM_RUNNING)
		return ANX_EBUSY;
	if (vm->state != ANX_VM_DEFINED && vm->state != ANX_VM_SAVED)
		return ANX_EINVAL;

	ret = vm->backend->start(vm);
	if (ret != ANX_OK)
		return ret;

	vm->state = ANX_VM_RUNNING;
	kprintf("vm: started '%s'\n", vm->config.name);
	return ANX_OK;
}

int anx_vm_stop(const anx_oid_t *vm_oid, bool force)
{
	struct anx_vm_object *vm;
	int ret;

	vm = anx_vm_object_get(vm_oid);
	if (!vm)
		return ANX_ENOENT;
	if (vm->state != ANX_VM_RUNNING && vm->state != ANX_VM_PAUSED)
		return ANX_EINVAL;

	ret = vm->backend->stop(vm, force);
	if (ret != ANX_OK)
		return ret;

	vm->state = ANX_VM_DEFINED;
	kprintf("vm: stopped '%s'\n", vm->config.name);
	return ANX_OK;
}

int anx_vm_pause(const anx_oid_t *vm_oid)
{
	struct anx_vm_object *vm;
	int ret;

	vm = anx_vm_object_get(vm_oid);
	if (!vm)
		return ANX_ENOENT;
	if (vm->state != ANX_VM_RUNNING)
		return ANX_EINVAL;

	ret = vm->backend->pause(vm);
	if (ret != ANX_OK)
		return ret;

	vm->state = ANX_VM_PAUSED;
	kprintf("vm: paused '%s'\n", vm->config.name);
	return ANX_OK;
}

int anx_vm_resume(const anx_oid_t *vm_oid)
{
	struct anx_vm_object *vm;
	int ret;

	vm = anx_vm_object_get(vm_oid);
	if (!vm)
		return ANX_ENOENT;
	if (vm->state != ANX_VM_PAUSED)
		return ANX_EINVAL;

	ret = vm->backend->resume(vm);
	if (ret != ANX_OK)
		return ret;

	vm->state = ANX_VM_RUNNING;
	kprintf("vm: resumed '%s'\n", vm->config.name);
	return ANX_OK;
}

int anx_vm_list(anx_oid_t *results, uint32_t max, uint32_t *count_out)
{
	uint32_t i, n = 0;

	if (!results || !count_out)
		return ANX_EINVAL;

	for (i = 0; i < ANX_VM_MAX_VMS && n < max; i++) {
		if (vm_table[i].in_use)
			results[n++] = vm_table[i].oid;
	}
	*count_out = n;
	return ANX_OK;
}

int anx_vm_state_get(const anx_oid_t *vm_oid, enum anx_vm_state *state_out)
{
	struct anx_vm_object *vm;

	vm = anx_vm_object_get(vm_oid);
	if (!vm)
		return ANX_ENOENT;
	*state_out = vm->state;
	return ANX_OK;
}

int anx_vm_stats_get(const anx_oid_t *vm_oid, struct anx_vm_stats *stats_out)
{
	struct anx_vm_object *vm;

	vm = anx_vm_object_get(vm_oid);
	if (!vm)
		return ANX_ENOENT;
	if (vm->state != ANX_VM_RUNNING)
		return ANX_EINVAL;
	return vm->backend->stats(vm, stats_out);
}

int anx_vm_console_read(const anx_oid_t *vm_oid, char *buf, uint32_t len,
			uint32_t *read_out)
{
	struct anx_vm_object *vm;

	vm = anx_vm_object_get(vm_oid);
	if (!vm)
		return ANX_ENOENT;
	if (vm->state != ANX_VM_RUNNING && vm->state != ANX_VM_PAUSED)
		return ANX_EINVAL;
	return vm->backend->console_read(vm, buf, len, read_out);
}

int anx_vm_exec(const anx_oid_t *vm_oid, const char *command,
		char *stdout_out, uint32_t stdout_size)
{
	struct anx_vm_object *vm;

	vm = anx_vm_object_get(vm_oid);
	if (!vm)
		return ANX_ENOENT;
	if (vm->state != ANX_VM_RUNNING)
		return ANX_EINVAL;
	return vm->backend->exec(vm, command, stdout_out, stdout_size);
}

int anx_vm_snapshot(const anx_oid_t *vm_oid, const char *name,
		    anx_oid_t *snap_out)
{
	struct anx_vm_object *vm;

	vm = anx_vm_object_get(vm_oid);
	if (!vm)
		return ANX_ENOENT;
	(void)name;
	return vm->backend->snapshot(vm, snap_out);
}

int anx_vm_clone(const anx_oid_t *snap_oid, const char *new_name,
		 anx_oid_t *new_vm_out)
{
	struct anx_vm_object *snap;
	struct anx_vm_config new_cfg;

	snap = anx_vm_object_get(snap_oid);
	if (!snap)
		return ANX_ENOENT;

	anx_memcpy(&new_cfg, &snap->config, sizeof(new_cfg));
	anx_strlcpy(new_cfg.name, new_name, ANX_VM_NAME_MAX);

	return anx_vm_create(&new_cfg, new_vm_out);
}

int anx_vm_disk_create(uint64_t size_bytes, enum anx_vm_disk_format fmt,
		       anx_oid_t *disk_out)
{
	/* Phase 1: return a synthetic OID; real disk images added in Phase 2 */
	(void)size_bytes; (void)fmt;
	disk_out->hi = 0x414e5844534b0000ULL;	/* "ANXDSK\0\0" */
	disk_out->lo = ++vm_oid_seq;
	return ANX_OK;
}

int anx_vm_disk_attach(const anx_oid_t *vm_oid,
		       const struct anx_vm_disk_config *cfg)
{
	struct anx_vm_object *vm;

	vm = anx_vm_object_get(vm_oid);
	if (!vm)
		return ANX_ENOENT;
	if (vm->config.disk_count >= ANX_VM_MAX_DISKS)
		return ANX_ENOMEM;
	if (vm->state == ANX_VM_RUNNING)
		return ANX_ENOTSUP;

	anx_memcpy(&vm->config.disks[vm->config.disk_count], cfg, sizeof(*cfg));
	vm->config.disk_count++;
	return ANX_OK;
}

int anx_vm_disk_detach(const anx_oid_t *vm_oid, uint32_t disk_index)
{
	struct anx_vm_object *vm;
	uint32_t i;

	vm = anx_vm_object_get(vm_oid);
	if (!vm)
		return ANX_ENOENT;
	if (disk_index >= vm->config.disk_count)
		return ANX_EINVAL;
	if (vm->state == ANX_VM_RUNNING)
		return ANX_ENOTSUP;

	for (i = disk_index; i + 1 < vm->config.disk_count; i++)
		vm->config.disks[i] = vm->config.disks[i + 1];
	vm->config.disk_count--;
	return ANX_OK;
}
