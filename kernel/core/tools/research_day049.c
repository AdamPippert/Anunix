/* Advice does not grant authority over the manager's memory placements. */
#if defined(ANX_RESEARCH_TEST) || defined(ANX_HOST_TEST)
#include <anx/research_test.h>
#include <anx/memplane.h>
#include <anx/state_object.h>
#include <anx/cell.h>
#include <anx/external_call.h>
#include <anx/alloc.h>
#include <anx/string.h>

struct ownership_context {
	struct anx_mem_entry *entry, *owned, *second;
	struct anx_state_object *objects[2];
	struct anx_cell *caller;
};
static int advise(struct anx_external_call *call, void *arg)
{
	(void)call;
	struct ownership_context *c = arg;
	struct anx_mem_retention_hint hint = {100, 16};
	uint8_t before = c->entry->tier_mask;
	int ret = anx_memplane_hint(c->entry, &hint);
	if (ret != ANX_OK) return ret;
	if (anx_memplane_promote(c->entry, ANX_MEM_L0) != ANX_EPERM || c->entry->tier_mask != before) return -4901;
	if (anx_memplane_demote(c->entry, ANX_MEM_L2) != ANX_EPERM ||
	    anx_memplane_protect(c->entry, 0) != ANX_EPERM || c->entry->tier_mask != before ||
	    c->entry->protected_tiers != ANX_TIER_BIT(ANX_MEM_L2)) return -4902;
	for (uint32_t mode = ANX_FORGET_HARD_DELETE; mode <= ANX_FORGET_REDERIVE; mode++)
		if (anx_memplane_forget(c->entry, (enum anx_forget_mode)mode) != ANX_EPERM) return -4903;
	ret = anx_memplane_admit(&c->objects[0]->oid, ANX_ADMIT_CACHEABLE, &c->owned);
	if (ret == ANX_OK) ret = anx_memplane_hint(c->owned, &hint);
	if (ret != ANX_OK) return ret;
	if (c->caller->memory_admitted_bytes != 8 ||
	    anx_memplane_admit(&c->objects[1]->oid, ANX_ADMIT_CACHEABLE, &c->second) != ANX_ENOMEM || c->second ||
	    anx_memplane_promote(c->owned, ANX_MEM_L4) != ANX_EPERM ||
	    anx_memplane_forget(c->owned, ANX_FORGET_ARCHIVE) != ANX_EPERM ||
	    c->caller->memory_admitted_bytes != 8) return -4904;
	/* Cleanup releases charges without acquiring placement or read authority. */
	c->objects[0]->access_policy.rule_count = 1;
	c->objects[0]->access_policy.rules[0].operations = ANX_ACCESS_READ_PAYLOAD | ANX_ACCESS_WRITE_META;
	c->objects[0]->access_policy.rules[0].effect = ANX_EFFECT_DENY;
	ret = anx_memplane_forget(c->owned, ANX_FORGET_HARD_DELETE);
	if (ret != ANX_OK) return ret;
	c->owned = NULL;
	if (c->caller->memory_admitted_bytes || c->caller->memory_admission_count) return -4905;
	ret = anx_memplane_admit(&c->objects[1]->oid, ANX_ADMIT_CACHEABLE, &c->second);
	if (ret != ANX_OK) return ret;
	ret = anx_memplane_forget(c->second, ANX_FORGET_HARD_DELETE);
	if (ret == ANX_OK) c->second = NULL;
	return ret;
}

int anx_research_day049(void)
{
	struct anx_so_create_params p = { .object_type = ANX_OBJ_BYTE_DATA, .payload = "resource", .payload_size = 8 };
	struct anx_state_object *object = NULL;
	struct anx_cell *caller = NULL;
	struct anx_external_call *call = NULL;
	struct anx_cell_intent intent = {0};
	struct ownership_context context = {0};
	int ret = anx_so_create(&p, &object);
	if (ret == ANX_OK) ret = anx_so_seal(&object->oid);
	if (ret == ANX_OK) ret = anx_memplane_admit(&object->oid, ANX_ADMIT_LONG_TERM_CANDIDATE, &context.entry);
	if (ret == ANX_OK) ret = anx_memplane_protect(context.entry, ANX_TIER_BIT(ANX_MEM_L2));
	for (uint32_t i = 0; ret == ANX_OK && i < 2; i++) {
		ret = anx_so_create(&p, &context.objects[i]);
		if (ret == ANX_OK) ret = anx_so_seal(&context.objects[i]->oid);
	}
	anx_strlcpy(intent.name, "research-day-049", sizeof(intent.name));
	if (ret == ANX_OK) ret = anx_cell_create(ANX_CELL_TASK_EXTERNAL_CALL, &intent, &caller);
	if (ret == ANX_OK) ret = anx_external_register_handler("anxresearch049", advise, &context);
	if (ret != ANX_OK) goto out;
	call = anx_zalloc(sizeof(*call));
	if (!call) { ret = ANX_ENOMEM; goto out; }
	anx_strlcpy(call->endpoint, "anxresearch049://advice", sizeof(call->endpoint));
	caller->ext_call = call; caller->execution.allow_side_effects = true;
	caller->constraints.max_memory_admission_bytes = 8; context.caller = caller;
	ret = anx_cell_run(caller);
	if (ret != ANX_OK) goto out;
	ret = -4906;
	if (caller->memory_admitted_bytes || caller->memory_admission_count ||
	    context.entry->tier_mask != ANX_TIER_BIT(ANX_MEM_L2) || context.entry->retention.priority != 100) goto out;
	/* Validation is an enum, not a monotonically increasing trust scale. */
	const enum anx_mem_validation_state denied[] = { ANX_MEMVAL_UNVALIDATED, ANX_MEMVAL_CONTESTED,
		ANX_MEMVAL_SUPERSEDED, ANX_MEMVAL_STALE, ANX_MEMVAL_QUARANTINED };
	for (uint32_t i = 0; i < sizeof(denied) / sizeof(denied[0]); i++) {
		ret = anx_memplane_set_validation(context.entry, denied[i]);
		if (ret != ANX_OK) goto out;
		ret = -4907;
		if (anx_memplane_promote(context.entry, ANX_MEM_L4) != ANX_EPERM ||
		    context.entry->tier_mask != ANX_TIER_BIT(ANX_MEM_L2)) goto out;
	}
	for (uint32_t i = ANX_MEMVAL_PROVISIONAL; i <= ANX_MEMVAL_VALIDATED; i++) {
		ret = anx_memplane_set_validation(context.entry, (enum anx_mem_validation_state)i);
		if (ret == ANX_OK) ret = anx_memplane_promote(context.entry, ANX_MEM_L4);
		if (ret != ANX_OK) goto out;
		ret = -4908;
		if (!anx_mem_in_tier(context.entry, ANX_MEM_L4) || anx_memplane_promote(context.entry, ANX_MEM_L5) != ANX_EPERM) goto out;
		ret = anx_memplane_demote(context.entry, ANX_MEM_L4);
		if (ret != ANX_OK) goto out;
	}
	ret = ANX_OK;
out:
	anx_external_unregister_handler("anxresearch049");
	if (context.owned) anx_memplane_forget(context.owned, ANX_FORGET_HARD_DELETE);
	if (context.second) anx_memplane_forget(context.second, ANX_FORGET_HARD_DELETE);
	if (context.entry) { anx_memplane_protect(context.entry, 0); anx_memplane_forget(context.entry, ANX_FORGET_HARD_DELETE); }
	for (uint32_t i = 0; i < 2; i++) if (context.objects[i]) {
		anx_so_delete(&context.objects[i]->oid, false); anx_objstore_release(context.objects[i]);
	}
	if (object) { anx_so_delete(&object->oid, false); anx_objstore_release(object); }
	if (caller) anx_cell_destroy(caller);
	anx_free(call);
	return ret;
}
#endif
