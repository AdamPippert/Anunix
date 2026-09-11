/* ICM metadata projections follow caller authority, not claimed annotations. */
#if defined(ANX_RESEARCH_TEST) || defined(ANX_HOST_TEST)
#include <anx/research_test.h>
#include <anx/icm.h>
#include <anx/cell.h>
#include <anx/external_call.h>
#include <anx/meta.h>
#include <anx/string.h>
#include <anx/uuid.h>

struct context_state {
	anx_oid_t private_oid, public_oid;
	char domain[37];
	bool owner;
	uint32_t private_seen, public_seen;
};

static int context_collect(const struct anx_icm_view *view, void *context)
{
	struct context_state *state = context;
	if (anx_uuid_compare(&view->oid, &state->private_oid) == 0)
		state->private_seen++;
	if (anx_uuid_compare(&view->oid, &state->public_oid) == 0)
		state->public_seen++;
	return ANX_OK;
}

static int context_handler(struct anx_external_call *call, void *context)
{
	struct context_state *state = context;
	struct anx_icm_view view, before;
	struct anx_object_handle handle = {0};
	int ret;
	(void)call;
	anx_memset(&view, 0xa5, sizeof(view));
	before = view;
	ret = anx_icm_read_view(&state->private_oid, &view);
	if (ret != (state->owner ? ANX_OK : ANX_EPERM) ||
	    (!state->owner && anx_memcmp(&view, &before, sizeof(view))))
		return -1701;
	ret = anx_icm_tag(&state->private_oid, NULL, NULL,
		state->owner ? "own-reviewed" : "system", NULL, NULL, NULL);
	if (ret != (state->owner ? ANX_OK : ANX_EPERM))
		return -1702;
	ret = anx_icm_publish(&state->private_oid, "anx:research/day017@1");
	if (ret != (state->owner ? ANX_OK : ANX_EPERM))
		return -1703;
	state->private_seen = state->public_seen = 0;
	if (anx_icm_catalog(state->domain, context_collect, state) != ANX_OK ||
	    state->private_seen != (state->owner ? 1U : 0U) || state->public_seen != 1 ||
	    anx_icm_count(state->domain) != (state->owner ? 2 : 1))
		return -1704;
	state->private_seen = state->public_seen = 0;
	if (anx_icm_published(context_collect, state) != ANX_OK ||
	    state->private_seen != (state->owner ? 1U : 0U))
		return -1705;
	/* A visible object's claimed authority cannot grant access elsewhere. */
	if (anx_icm_tag(&state->public_oid, NULL, NULL, "system", NULL, NULL, NULL) != ANX_OK)
		return -1706;
	if (anx_icm_read_view(&state->private_oid, &view) != (state->owner ? ANX_OK : ANX_EPERM))
		return -1707;
	ret = anx_so_open(&state->private_oid, ANX_OPEN_READ, &handle);
	if (handle.obj)
		anx_so_close(&handle);
	return ret == (state->owner ? ANX_OK : ANX_EPERM) ? ANX_OK : -1708;
}

int anx_research_day017(void)
{
	struct anx_cell *owner = NULL, *other = NULL;
	struct anx_cell_intent intent = {0};
	struct anx_external_call call = {0};
	struct anx_state_object *secret = NULL, *public = NULL;
	struct anx_so_create_params params = {0};
	struct anx_access_policy policy = {0};
	struct context_state state = {0};
	uint32_t secret_refs, public_refs;
	int rc;

	anx_strlcpy(intent.name, "research-day-017-owner", sizeof(intent.name));
	rc = anx_cell_create(ANX_CELL_TASK_EXTERNAL_CALL, &intent, &owner);
	if (rc != ANX_OK)
		goto out;
	anx_strlcpy(intent.name, "research-day-017-other", sizeof(intent.name));
	rc = anx_cell_create(ANX_CELL_TASK_EXTERNAL_CALL, &intent, &other);
	if (rc != ANX_OK)
		goto out;
	params.object_type = ANX_OBJ_BYTE_DATA;
	params.payload = "context";
	params.payload_size = 7;
	params.creator_cell = owner->cid;
	rc = anx_so_create(&params, &secret);
	if (rc != ANX_OK)
		goto out;
	params.creator_cell = other->cid;
	rc = anx_so_create(&params, &public);
	if (rc != ANX_OK)
		goto out;
	state.private_oid = secret->oid;
	state.public_oid = public->oid;
	anx_uuid_to_string(&secret->oid, state.domain, sizeof(state.domain));
	rc = anx_icm_tag(&secret->oid, state.domain, "doc", "own", "active", NULL, NULL);
	if (rc != ANX_OK)
		goto out;
	rc = anx_icm_tag(&public->oid, state.domain, "doc", "external", "active", NULL, NULL);
	if (rc != ANX_OK)
		goto out;
	rc = anx_icm_publish(&secret->oid, "anx:research/day017@0");
	if (rc != ANX_OK)
		goto out;
	policy.rule_count = 2;
	policy.rules[0].principal = owner->cid;
	policy.rules[0].operations = ANX_ACCESS_READ_META | ANX_ACCESS_WRITE_META | ANX_ACCESS_READ_PAYLOAD;
	policy.rules[0].effect = ANX_EFFECT_ALLOW;
	policy.rules[1].operations = policy.rules[0].operations;
	policy.rules[1].effect = ANX_EFFECT_DENY;
	secret->access_policy = policy;
	rc = anx_so_seal(&secret->oid);
	if (rc != ANX_OK)
		goto out;
	secret_refs = secret->refcount;
	public_refs = public->refcount;
	anx_strlcpy(call.endpoint, "anxresearch017://context", sizeof(call.endpoint));
	rc = anx_external_register_handler("anxresearch017", context_handler, &state);
	if (rc != ANX_OK)
		goto out;
	owner->execution.allow_side_effects = other->execution.allow_side_effects = true;
	owner->ext_call = other->ext_call = &call;
	rc = anx_cell_run(other);
	if (rc != ANX_OK)
		goto out;
	state.owner = true;
	rc = anx_cell_run(owner);
	if (rc != ANX_OK)
		goto out;
	rc = -1709;
	if (anx_memcmp(&secret->access_policy, &policy, sizeof(policy)) ||
	    secret->refcount != secret_refs || public->refcount != public_refs ||
	    anx_meta_get(public->user_meta, "anno.icm.role") != NULL ||
	    anx_strcmp(anx_meta_get(secret->user_meta, ANX_ICM_KEY_AUTHORITY)->v.str.data, "own-reviewed"))
		goto out;
	rc = ANX_OK;
out:
	if (secret)
		anx_objstore_release(secret);
	if (public)
		anx_objstore_release(public);
	if (owner)
		anx_cell_destroy(owner);
	if (other)
		anx_cell_destroy(other);
	anx_external_unregister_handler("anxresearch017");
	return rc;
}
#endif
