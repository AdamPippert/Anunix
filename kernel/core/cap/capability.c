/*
 * capability.c — Capability Objects stub implementation.
 *
 * Basic lifecycle management and store. Procedure interpretation,
 * validation suites, and network distribution are deferred.
 */

#include <anx/types.h>
#include <anx/capability.h>
#include <anx/engine.h>
#include <anx/state_object.h>
#include <anx/alloc.h>
#include <anx/uuid.h>
#include <anx/hashtable.h>
#include <anx/string.h>
#include <anx/kprintf.h>

#define CAP_STORE_BITS	6	/* 64 buckets */

static struct anx_htable cap_table;

/* Lifecycle transition table */
static const bool cap_transitions[ANX_CAP_STATUS_COUNT][ANX_CAP_STATUS_COUNT] = {
	/* draft -> */
	[ANX_CAP_DRAFT] = {
		[ANX_CAP_VALIDATING] = true,
		[ANX_CAP_RETIRED] = true,
	},
	/* validating -> */
	[ANX_CAP_VALIDATING] = {
		[ANX_CAP_VALIDATED] = true,
		[ANX_CAP_DRAFT] = true,		/* validation failed, back to draft */
		[ANX_CAP_RETIRED] = true,
	},
	/* validated -> */
	[ANX_CAP_VALIDATED] = {
		[ANX_CAP_INSTALLED] = true,
		[ANX_CAP_RETIRED] = true,
	},
	/* installed -> */
	[ANX_CAP_INSTALLED] = {
		[ANX_CAP_SUSPENDED] = true,
		[ANX_CAP_SUPERSEDED] = true,
		[ANX_CAP_RETIRED] = true,
	},
	/* suspended -> */
	[ANX_CAP_SUSPENDED] = {
		[ANX_CAP_INSTALLED] = true,	/* reinstated */
		[ANX_CAP_RETIRED] = true,
	},
	/* superseded -> */
	[ANX_CAP_SUPERSEDED] = {
		[ANX_CAP_RETIRED] = true,
	},
	/* retired is terminal */
};

void anx_cap_store_init(void)
{
	anx_htable_init(&cap_table, CAP_STORE_BITS);
}

int anx_cap_create(const char *name, const char *version,
		   struct anx_capability **out)
{
	struct anx_capability *cap;
	struct anx_state_object *obj;
	struct anx_so_create_params params;
	int ret;

	if (!name || !version || !out)
		return ANX_EINVAL;

	cap = anx_zalloc(sizeof(*cap));
	if (!cap)
		return ANX_ENOMEM;

	/* Create the underlying State Object */
	anx_memset(&params, 0, sizeof(params));
	params.object_type = ANX_OBJ_CAPABILITY;
	params.payload = NULL;
	params.payload_size = 0;

	ret = anx_so_create(&params, &obj);
	if (ret != ANX_OK) {
		anx_free(cap);
		return ret;
	}

	cap->cap_oid = obj->oid;
	anx_strlcpy(cap->name, name, sizeof(cap->name));
	anx_strlcpy(cap->version, version, sizeof(cap->version));
	cap->status = ANX_CAP_DRAFT;

	anx_spin_init(&cap->lock);
	anx_list_init(&cap->store_link);

	uint64_t hash = anx_uuid_hash(&cap->cap_oid);
	anx_htable_add(&cap_table, &cap->store_link, hash);

	anx_objstore_release(obj);

	*out = cap;
	return ANX_OK;
}

struct anx_capability *anx_cap_lookup(const anx_oid_t *oid)
{
	uint64_t hash = anx_uuid_hash(oid);
	struct anx_list_head *pos;

	ANX_HTABLE_FOR_BUCKET(pos, &cap_table, hash) {
		struct anx_capability *cap;

		cap = ANX_LIST_ENTRY(pos, struct anx_capability, store_link);
		if (anx_uuid_compare(&cap->cap_oid, oid) == 0)
			return cap;
	}
	return NULL;
}

int anx_cap_transition(struct anx_capability *cap,
		       enum anx_cap_status new_status)
{
	enum anx_cap_status old;

	if (!cap)
		return ANX_EINVAL;
	if ((int)new_status < 0 || new_status >= ANX_CAP_STATUS_COUNT)
		return ANX_EINVAL;

	old = cap->status;
	if ((int)old < 0 || old >= ANX_CAP_STATUS_COUNT)
		return ANX_EINVAL;
	if (!cap_transitions[old][new_status])
		return ANX_EINVAL;

	cap->status = new_status;
	return ANX_OK;
}

int anx_cap_install(struct anx_capability *cap)
{
	struct anx_engine *eng;
	int ret;

	if (!cap)
		return ANX_EINVAL;

	/* Must be validated before installation */
	if (cap->status != ANX_CAP_VALIDATED)
		return ANX_EPERM;

	/* Register as an engine */
	ret = anx_engine_register(cap->name,
				  ANX_ENGINE_INSTALLED_CAPABILITY,
				  cap->output_cap_mask,
				  &eng);
	if (ret != ANX_OK)
		return ret;

	cap->installed_engine_id = eng->eid;

	/* Transition to installed */
	ret = anx_cap_transition(cap, ANX_CAP_INSTALLED);
	if (ret != ANX_OK) {
		anx_engine_unregister(eng);
		return ret;
	}

	return ANX_OK;
}

int anx_cap_uninstall(struct anx_capability *cap)
{
	struct anx_engine *eng;

	if (!cap)
		return ANX_EINVAL;

	if (cap->status != ANX_CAP_INSTALLED &&
	    cap->status != ANX_CAP_SUSPENDED)
		return ANX_EPERM;

	/* Remove from engine registry */
	if (!anx_uuid_is_nil(&cap->installed_engine_id)) {
		eng = anx_engine_lookup(&cap->installed_engine_id);
		if (eng)
			anx_engine_unregister(eng);
		cap->installed_engine_id = ANX_UUID_NIL;
	}

	return ANX_OK;
}

void anx_cap_record_invocation(struct anx_capability *cap, bool success)
{
	if (!cap)
		return;

	anx_spin_lock(&cap->lock);
	cap->invocation_count++;
	if (success)
		cap->success_count++;
	anx_spin_unlock(&cap->lock);
}

int anx_cap_validate(struct anx_capability *cap)
{
	int score = 100;
	uint32_t i;
	int ret;

	if (!cap)
		return ANX_EINVAL;
	if (cap->status != ANX_CAP_DRAFT)
		return ANX_EPERM;

	ret = anx_cap_transition(cap, ANX_CAP_VALIDATING);
	if (ret != ANX_OK)
		return ret;

	if (cap->name[0] == '\0')
		score -= 20;
	if (cap->version[0] == '\0')
		score -= 20;

	for (i = 0; i < cap->required_engine_count; i++) {
		struct anx_engine *eng = anx_engine_lookup(&cap->required_engines[i]);
		if (!eng) {
			score -= 25;
			kprintf("[cap] validate: required engine %u not found\n", i);
		}
	}

	if (score < 0)
		score = 0;
	cap->validation_score = (uint32_t)score;

	if (score >= 50) {
		ret = anx_cap_transition(cap, ANX_CAP_VALIDATED);
		if (ret != ANX_OK)
			return ret;
		kprintf("[cap] validated: %s v%s score=%u\n",
			cap->name, cap->version, (unsigned int)score);
		return ANX_OK;
	}

	anx_cap_transition(cap, ANX_CAP_DRAFT);
	kprintf("[cap] validation failed: %s v%s score=%u\n",
		cap->name, cap->version, (unsigned int)score);
	return ANX_EPERM;
}
