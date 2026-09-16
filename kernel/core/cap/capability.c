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
#include <anx/cell.h>
#include <anx/identity.h>
#include <anx/effect_fence.h>
#include <anx/revision.h>

#define CAP_STORE_BITS	6	/* 64 buckets */

static struct anx_htable cap_table;

/* Grant state stays outside the mutable candidate declaration. */
struct capability_entry {
	struct anx_capability cap;
	uint32_t root_ceiling;
	uint32_t installed_authority;
};

static struct capability_entry *entry_for(struct anx_capability *cap)
{
	if (!cap || anx_cap_lookup(&cap->cap_oid) != cap)
		return NULL;
	return ANX_CONTAINER_OF(cap, struct capability_entry, cap);
}

int anx_cap_set_authority_ceiling(struct anx_capability *cap, uint32_t ceiling)
{
	struct capability_entry *entry = entry_for(cap);
	if (!entry || (ceiling & ~ANX_CAP_AUTH_ALL))
		return ANX_EINVAL;
	if (anx_cell_current_id() || cap->status != ANX_CAP_DRAFT ||
	    !anx_uuid_is_nil(&cap->supersedes_oid))
		return ANX_EPERM;
	entry->root_ceiling = ceiling;
	return ANX_OK;
}

static int check_authority(struct anx_capability *cap, uint32_t ceiling)
{
	const anx_cid_t *active = anx_cell_current_id();
	struct anx_cell *caller;
	uint32_t available = 0;
	bool may_install;

	if (cap->required_authority & ~ANX_CAP_AUTH_ALL)
		return ANX_EINVAL;
	if (cap->required_authority & ~ceiling)
		return ANX_EPERM;
	if (!active)
		return ANX_OK;
	caller = anx_cell_store_lookup(active);
	if (!caller)
		return ANX_EPERM;
	may_install = caller->execution.allow_side_effects && !anx_cell_status_terminal(caller->status) &&
		      anx_identity_admit(caller, NULL) == ANX_OK &&
		      anx_effect_fence_check(caller, NULL, NULL) == ANX_OK;
	if (caller->execution.allow_network)
		available |= ANX_CAP_AUTH_NETWORK;
	if (caller->execution.allow_remote_models)
		available |= ANX_CAP_AUTH_REMOTE_MODEL;
	if (caller->execution.allow_recursive_cells)
		available |= ANX_CAP_AUTH_DERIVE_CELL;
	if (caller->execution.allow_side_effects)
		available |= ANX_CAP_AUTH_SIDE_EFFECT;
	anx_cell_store_release(caller);
	return may_install && !(cap->required_authority & ~available) ? ANX_OK : ANX_EPERM;
}

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
	struct capability_entry *entry;
	struct anx_state_object *obj;
	struct anx_so_create_params params;
	int ret;

	if (!name || !version || !out)
		return ANX_EINVAL;

	entry = anx_zalloc(sizeof(*entry));
	if (!entry)
		return ANX_ENOMEM;
	cap = &entry->cap;

	/* Create the underlying State Object */
	anx_memset(&params, 0, sizeof(params));
	params.object_type = ANX_OBJ_CAPABILITY;
	params.payload = NULL;
	params.payload_size = 0;

	ret = anx_so_create(&params, &obj);
	if (ret != ANX_OK) {
		anx_free(entry);
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
	if (old == ANX_CAP_INSTALLED || old == ANX_CAP_SUSPENDED || old == ANX_CAP_SUPERSEDED ||
	    new_status == ANX_CAP_INSTALLED) {
		int ret = anx_revision_check(ANX_REVISION_IMPLEMENTATION, false);
		if (ret != ANX_OK) return ret;
	}

	cap->status = new_status;
	return ANX_OK;
}

static bool bounded_name(const char *name, size_t size)
{
	if (!name[0])
		return false;
	for (size_t i = 0; i < size; i++)
		if (!name[i])
			return true;
	return false;
}

static int check_declaration(const struct anx_capability *cap)
{
	if (!bounded_name(cap->name, sizeof(cap->name)) ||
	    !bounded_name(cap->version, sizeof(cap->version)) ||
	    (cap->required_authority & ~ANX_CAP_AUTH_ALL) ||
	    cap->required_engine_count > ANX_CAP_REQUIRED_ENGINES_MAX)
		return ANX_EINVAL;
	for (uint32_t i = 0; i < cap->required_engine_count; i++) {
		if (anx_uuid_is_nil(&cap->required_engines[i]))
			return ANX_EINVAL;
		for (uint32_t j = 0; j < i; j++)
			if (anx_uuid_compare(&cap->required_engines[i], &cap->required_engines[j]) == 0)
				return ANX_EINVAL;
	}
	return ANX_OK;
}

static int check_dependencies(const struct anx_capability *cap)
{
	for (uint32_t i = 0; i < cap->required_engine_count; i++) {
		struct anx_engine *eng = anx_engine_lookup(&cap->required_engines[i]);
		if (!eng)
			return ANX_ENOENT;
		if (eng->status != ANX_ENGINE_AVAILABLE && eng->status != ANX_ENGINE_DEGRADED)
			return ANX_EPERM;
	}
	return ANX_OK;
}

static int do_install(struct anx_capability *cap, uint32_t ceiling)
{
	struct capability_entry *entry = entry_for(cap);
	struct anx_engine *eng;
	int ret;

	if (!entry)
		return ANX_EINVAL;
	ret = anx_revision_check(ANX_REVISION_IMPLEMENTATION, false);
	if (ret != ANX_OK)
		return ret;
	ret = check_declaration(cap);
	if (ret != ANX_OK)
		return ret;
	ret = check_authority(cap, ceiling);
	if (ret != ANX_OK)
		return ret;
	ret = check_dependencies(cap);
	if (ret != ANX_OK)
		return ret;

	/* Register as an engine */
	ret = anx_engine_register(cap->name,
				  ANX_ENGINE_INSTALLED_CAPABILITY,
				  cap->output_cap_mask,
				  &eng);
	if (ret != ANX_OK)
		return ret;

	/* Transition to installed */
	ret = anx_cap_transition(cap, ANX_CAP_INSTALLED);
	if (ret != ANX_OK) {
		anx_engine_unregister(eng);
		return ret;
	}
	cap->installed_engine_id = eng->eid;
	entry->installed_authority = cap->required_authority;

	return ANX_OK;
}

int anx_cap_install(struct anx_capability *cap)
{
	struct capability_entry *entry = entry_for(cap);
	if (!entry)
		return ANX_EINVAL;

	/* Must be validated before installation */
	if (cap->status != ANX_CAP_VALIDATED)
		return ANX_EPERM;

	/*
	 * A candidate that declares an incumbent to supersede must go
	 * through anx_cap_install_gated() and clear the measured-null
	 * promotion gate (RFC-0029) — it never gets to skip straight to
	 * installed just because it exists.
	 */
	if (!anx_uuid_is_nil(&cap->supersedes_oid))
		return ANX_EPERM;

	return do_install(cap, entry->root_ceiling);
}

int anx_cap_install_gated(struct anx_capability *cap,
			  const struct anx_promotion_trial *trial,
			  uint32_t num_candidates_tried)
{
	struct anx_capability *incumbent;
	struct capability_entry *entry;
	bool promote = false;
	int ret;

	if (!cap || !trial)
		return ANX_EINVAL;

	if (cap->status != ANX_CAP_VALIDATED)
		return ANX_EPERM;

	/* No incumbent to compare against — use anx_cap_install(). */
	if (anx_uuid_is_nil(&cap->supersedes_oid))
		return ANX_EINVAL;
	incumbent = anx_cap_lookup(&cap->supersedes_oid);
	if (!incumbent)
		return ANX_ENOENT;
	if (incumbent->status != ANX_CAP_INSTALLED ||
	    anx_uuid_is_nil(&incumbent->installed_engine_id) ||
	    !anx_engine_lookup(&incumbent->installed_engine_id))
		return ANX_EPERM;
	entry = entry_for(incumbent);
	if (!entry)
		return ANX_EINVAL;

	ret = anx_promotion_gate_evaluate(trial, num_candidates_tried, &promote);
	if (ret != ANX_OK)
		return ret;
	if (!promote)
		return ANX_EPERM;

	return do_install(cap, entry->installed_authority);
}

int anx_cap_uninstall(struct anx_capability *cap)
{
	struct anx_engine *eng;
	int ret;

	if (!cap)
		return ANX_EINVAL;
	ret = anx_revision_check(ANX_REVISION_IMPLEMENTATION, false);
	if (ret != ANX_OK)
		return ret;

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
	int ret;

	if (!cap)
		return ANX_EINVAL;
	if (cap->status != ANX_CAP_DRAFT)
		return ANX_EPERM;
	ret = check_declaration(cap);
	if (ret != ANX_OK)
		return ret;
	ret = check_dependencies(cap);
	if (ret != ANX_OK) {
		cap->validation_score = 0;
		return ret;
	}

	ret = anx_cap_transition(cap, ANX_CAP_VALIDATING);
	if (ret != ANX_OK)
		return ret;

	/* Structural readiness only; behavioral evaluation remains separate. */
	cap->validation_score = 100;
	ret = anx_cap_transition(cap, ANX_CAP_VALIDATED);
	if (ret != ANX_OK)
		return ret;
	kprintf("[cap] validated: %s v%s score=%u\n",
		cap->name, cap->version, cap->validation_score);
	return ANX_OK;
}
