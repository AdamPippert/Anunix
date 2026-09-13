/* Retention advice ranks eviction but cannot create protection. */
#if defined(ANX_RESEARCH_TEST) || defined(ANX_HOST_TEST)
#include <anx/research_test.h>
#include <anx/memplane.h>
#include <anx/cell.h>
#include <anx/state_object.h>
#include <anx/external_call.h>
#include <anx/alloc.h>
#include <anx/string.h>
#include <anx/uuid.h>

struct retention_context {
	struct anx_mem_entry *protected_entry, *hinted;
};

static int retention_handler(struct anx_external_call *call, void *context)
{
	struct retention_context *state = context;
	struct anx_mem_retention_hint hint = {50, 2};
	anx_oid_t victim = ANX_UUID_NIL;
	(void)call;
	if (anx_memplane_hint(state->protected_entry, &hint) != ANX_EPERM ||
	    anx_memplane_protect(state->protected_entry, 0) != ANX_EPERM ||
	    anx_memplane_protect(state->hinted, ANX_TIER_BIT(ANX_MEM_L0)) != ANX_EPERM ||
	    anx_memplane_evict(&state->hinted->oid, 1, ANX_MEM_L0, &victim) != ANX_EPERM ||
	    !anx_uuid_is_nil(&victim))
		return -2010;
	if (anx_memplane_hint(state->hinted, &hint) != ANX_OK || state->hinted->protected_tiers != 0)
		return -2011;
	return ANX_OK;
}

int anx_research_day020(void)
{
	struct anx_state_object *objects[3] = {0};
	struct anx_mem_entry *entries[3] = {0};
	struct anx_so_create_params params = {0};
	struct anx_mem_retention_hint hint = {100, 1}, before;
	struct anx_object_handle handle = {0};
	struct anx_cell *caller = NULL;
	struct anx_external_call *call = NULL;
	struct anx_cell_intent intent = {0};
	struct retention_context context = {0};
	anx_oid_t pool[3], invalid[2], victim, sentinel;
	char payload[8];
	int rc;

	for (int i = 0; i < 3; i++) {
		params.object_type = ANX_OBJ_BYTE_DATA;
		params.payload = "retained";
		params.payload_size = 8;
		rc = anx_so_create(&params, &objects[i]);
		if (rc != ANX_OK)
			goto out;
		rc = anx_so_seal(&objects[i]->oid);
		if (rc != ANX_OK)
			goto out;
		rc = anx_memplane_admit(&objects[i]->oid, ANX_ADMIT_CACHEABLE, &entries[i]);
		if (rc != ANX_OK)
			goto out;
		pool[i] = objects[i]->oid;
		entries[i]->decay_score = 1000;
	}
	rc = -2001;
	if (anx_memplane_hint(entries[1], &hint) != ANX_OK)
		goto out;
	rc = -2002;
	if (anx_memplane_protect(entries[0], ANX_TIER_BIT(ANX_MEM_L0)) != ANX_OK ||
	    anx_memplane_evict(pool, 3, ANX_MEM_L0, &victim) != ANX_OK ||
	    anx_uuid_compare(&victim, &pool[2]) || anx_mem_in_tier(entries[2], ANX_MEM_L0) ||
	    !anx_mem_in_tier(entries[0], ANX_MEM_L0) || !anx_mem_in_tier(entries[1], ANX_MEM_L0))
		goto out;
	/* Maximum advisory priority does not pin the only eligible placement. */
	rc = -2003;
	if (anx_memplane_evict(&pool[1], 1, ANX_MEM_L0, &victim) != ANX_OK ||
	    anx_uuid_compare(&victim, &pool[1]) || !anx_mem_in_tier(entries[1], ANX_MEM_L1) ||
	    anx_memplane_promote(entries[1], ANX_MEM_L0) != ANX_OK ||
	    anx_memplane_promote(entries[2], ANX_MEM_L0) != ANX_OK)
		goto out;
	entries[2]->decay_score = 500;
	rc = -2004;
	if (anx_memplane_decay_entry(entries[1]) != ANX_OK || entries[1]->retention.sweeps != 0 ||
	    anx_memplane_evict(pool, 3, ANX_MEM_L0, &victim) != ANX_OK ||
	    anx_uuid_compare(&victim, &pool[1]))
		goto out;
	anx_uuid_generate(&sentinel);
	victim = sentinel;
	rc = -2005;
	if (anx_memplane_evict(&pool[0], 1, ANX_MEM_L0, &victim) != ANX_ENOENT ||
	    anx_uuid_compare(&victim, &sentinel) ||
	    anx_memplane_demote(entries[0], ANX_MEM_L0) != ANX_EBUSY)
		goto out;
	for (int mode = ANX_FORGET_HARD_DELETE; mode <= ANX_FORGET_REDERIVE; mode++) {
		if (anx_memplane_forget(entries[0], (enum anx_forget_mode)mode) != ANX_EBUSY)
			goto out;
	}
	entries[0]->validation = ANX_MEMVAL_VALIDATED;
	rc = -2006;
	if (anx_memplane_decay_entry(entries[0]) != ANX_OK ||
	    entries[0]->validation != ANX_MEMVAL_STALE || !anx_mem_in_tier(entries[0], ANX_MEM_L0))
		goto out;
	before = entries[1]->retention;
	for (int bad = 0; bad < 3; bad++) {
		hint.priority = bad == 0 ? 101 : 100;
		hint.sweeps = bad == 1 ? 17 : (bad == 2 ? 0 : 1);
		rc = -2007;
		if (anx_memplane_hint(entries[1], &hint) != ANX_EINVAL ||
		    anx_memcmp(&before, &entries[1]->retention, sizeof(before)))
			goto out;
	}
	invalid[0] = pool[2];
	invalid[1] = sentinel;
	rc = -2008;
	if (anx_memplane_evict(invalid, 2, ANX_MEM_L0, &victim) != ANX_ENOENT ||
	    !anx_mem_in_tier(entries[2], ANX_MEM_L0) || anx_uuid_compare(&victim, &sentinel))
		goto out;
	invalid[1] = pool[2];
	if (anx_memplane_evict(invalid, 2, ANX_MEM_L0, &victim) != ANX_EINVAL ||
	    anx_memplane_evict(pool, 33, ANX_MEM_L0, &victim) != ANX_EINVAL ||
	    anx_memplane_evict(pool, 3, ANX_MEM_L2, &victim) != ANX_EINVAL ||
	    anx_memplane_protect(entries[0], ANX_TIER_BIT(ANX_MEM_L2)) != ANX_EINVAL ||
	    anx_memplane_protect(entries[0], 1U << ANX_MEM_TIER_COUNT) != ANX_EINVAL ||
	    !anx_mem_in_tier(entries[2], ANX_MEM_L0) || anx_uuid_compare(&victim, &sentinel))
		goto out;
	objects[0]->access_policy.rule_count = 1;
	objects[0]->access_policy.rules[0].operations = ANX_ACCESS_WRITE_META;
	objects[0]->access_policy.rules[0].effect = ANX_EFFECT_DENY;
	context.protected_entry = entries[0];
	context.hinted = entries[1];
	rc = anx_external_register_handler("anxresearch020", retention_handler, &context);
	if (rc != ANX_OK)
		goto out;
	call = anx_zalloc(sizeof(*call));
	rc = ANX_ENOMEM;
	if (!call)
		goto out;
	anx_strlcpy(call->endpoint, "anxresearch020://hints", sizeof(call->endpoint));
	anx_strlcpy(intent.name, "research-day-020", sizeof(intent.name));
	rc = anx_cell_create(ANX_CELL_TASK_EXTERNAL_CALL, &intent, &caller);
	if (rc != ANX_OK)
		goto out;
	caller->execution.allow_side_effects = true;
	caller->ext_call = call;
	rc = anx_cell_run(caller);
	if (rc != ANX_OK)
		goto out;
	rc = anx_so_open(&pool[0], ANX_OPEN_READ, &handle);
	if (rc != ANX_OK)
		goto out;
	rc = -2012;
	if (anx_so_read_payload(&handle, 0, payload, sizeof(payload)) != 8 ||
	    anx_memcmp(payload, "retained", 8) || objects[0]->state != ANX_OBJ_SEALED ||
	    entries[0]->protected_tiers != ANX_TIER_BIT(ANX_MEM_L0) ||
	    entries[0]->retention.priority != 0)
		goto out;
	rc = ANX_OK;
out:
	if (handle.obj)
		anx_so_close(&handle);
	if (caller)
		anx_cell_destroy(caller);
	if (call)
		anx_free(call);
	anx_external_unregister_handler("anxresearch020");
	for (int i = 0; i < 3; i++) {
		if (entries[i]) {
			anx_memplane_protect(entries[i], 0);
			anx_memplane_forget(entries[i], ANX_FORGET_HARD_DELETE);
		}
		if (objects[i])
			anx_objstore_release(objects[i]);
	}
	return rc;
}
#endif
