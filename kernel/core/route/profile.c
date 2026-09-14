/* Compile finite routing envelopes; runtime selection never changes global policy. */
#include <anx/route_profile.h>
#include <anx/route.h>
#include <anx/state_object.h>
#include <anx/alloc.h>
#include <anx/crypto.h>
#include <anx/string.h>
#include <anx/uuid.h>

struct issued_profile { uint32_t state; anx_oid_t oid; uint8_t digest[32]; };

int anx_route_profile_requirements(uint32_t count, struct anx_route_profile_budget *out)
{
	if (!out || !count || count > ANX_ROUTE_PROFILE_CASES_MAX) return ANX_EINVAL;
	struct anx_route_profile_budget needed = {
		.artifact_payload_bytes = sizeof(struct anx_route_profile),
		.compiler_scratch_bytes = sizeof(struct anx_route_profile) + sizeof(struct anx_cell) +
			2 * sizeof(struct anx_resource_twin),
		.simulation_calls = count * 2,
	};
	*out = needed;
	return ANX_OK;
}

int anx_route_profile_compile(const struct anx_route_weight_policy *weights,
		const struct anx_route_profile_case *cases, uint32_t count, anx_oid_t *out)
{
	struct anx_route_profile_budget budget;
	if (anx_cell_current_id()) return ANX_EPERM;
	int ret = anx_route_profile_requirements(count, &budget);
	return ret == ANX_OK ? anx_route_profile_compile_bounded(weights, cases, count, &budget, out) : ret;
}
static struct issued_profile issued[ANX_ROUTE_PROFILE_MAX];
static struct anx_spinlock profile_lock = ANX_SPINLOCK_INIT;

static void hash_word(struct anx_sha256_ctx *hash, uint64_t value)
{
	uint8_t bytes[8];
	for (uint32_t i = 0; i < 8; i++) bytes[i] = (uint8_t)(value >> (i * 8));
	anx_sha256_update(hash, bytes, sizeof(bytes));
}

static void environment_digest(const struct anx_resource_twin *twin, uint8_t out[32])
{
	struct anx_sha256_ctx hash;
	anx_sha256_init(&hash);
	hash_word(&hash, twin->engine_count);
	for (uint32_t i = 0; i < twin->engine_count; i++) {
		const struct anx_twin_engine_snapshot *e = &twin->engines[i];
		uint64_t fields[] = { e->eid.hi, e->eid.lo, e->engine_class, e->status, e->readiness,
			e->supports_private_data, e->requires_network, e->cpu_weight, e->gpu_weight, e->quality_score,
			e->is_local, e->has_topology_affinity, e->topology_bk_lo, e->topology_bk_hi };
		for (uint32_t j = 0; j < sizeof(fields) / sizeof(fields[0]); j++) hash_word(&hash, fields[j]);
	}
	for (uint32_t i = 0; i < ANX_QUEUE_CLASS_COUNT; i++) hash_word(&hash, twin->queue_depth[i]);
	anx_sha256_final(&hash, out);
}

static bool case_matches(const struct anx_route_profile_case *c, const struct anx_cell *cell)
{
	return c->strategy == cell->routing.strategy && c->locality == cell->constraints.locality &&
		c->allow_network == cell->execution.allow_network && c->allow_remote_models == cell->execution.allow_remote_models &&
		c->topology_set == cell->constraints.topology_bk_set &&
		(!c->topology_set || (c->topology_lo == cell->constraints.topology_bk_lo && c->topology_hi == cell->constraints.topology_bk_hi));
}

static void case_cell(const struct anx_route_profile_case *c, struct anx_cell *cell)
{
	cell->routing.strategy = c->strategy; cell->constraints.locality = c->locality;
	cell->execution.allow_network = c->allow_network; cell->execution.allow_remote_models = c->allow_remote_models;
	cell->constraints.topology_bk_set = c->topology_set;
	cell->constraints.topology_bk_lo = c->topology_lo; cell->constraints.topology_bk_hi = c->topology_hi;
}

int anx_route_profile_compile_bounded(const struct anx_route_weight_policy *weights,
		const struct anx_route_profile_case *cases, uint32_t count,
		const struct anx_route_profile_budget *budget, anx_oid_t *out)
{
	struct anx_route_profile_budget required, limits;
	struct anx_route_profile *profile = NULL;
	struct anx_resource_twin *twin = NULL, *after = NULL;
	struct anx_cell *cell = NULL;
	struct anx_state_object *object = NULL;
	struct anx_so_create_params p = {0};
	struct anx_route_tuning_state current;
	uint8_t digest[32];
	uint32_t slot;
	bool flags;
	int ret;
	if (anx_cell_current_id()) return ANX_EPERM;
	if (!out || !cases || !budget || !weights ||
	    anx_route_profile_requirements(count, &required) != ANX_OK) return ANX_EINVAL;
	limits = *budget;
	/* Feasibility precedes allocation, issuance, environment capture, and simulation. */
	if (limits.artifact_payload_bytes < required.artifact_payload_bytes ||
	    limits.compiler_scratch_bytes < required.compiler_scratch_bytes) return ANX_ENOMEM;
	if (limits.simulation_calls < required.simulation_calls) return ANX_EFULL;
	if (anx_route_weight_policy_validate(weights) != ANX_OK) return ANX_EINVAL;
	anx_spin_lock_irqsave(&profile_lock, &flags);
	for (slot = 0; slot < ANX_ROUTE_PROFILE_MAX; slot++) if (!issued[slot].state) break;
	if (slot < ANX_ROUTE_PROFILE_MAX) issued[slot].state = 1;
	anx_spin_unlock_irqrestore(&profile_lock, flags);
	if (slot == ANX_ROUTE_PROFILE_MAX) return ANX_EFULL;
	ret = ANX_ENOMEM;
	profile = anx_zalloc(sizeof(*profile)); cell = anx_zalloc(sizeof(*cell));
	if (!profile || !cell) goto done;
	ret = anx_route_tuning_snapshot(&profile->incumbent);
	if (ret != ANX_OK) goto done;
	if (profile->incumbent.trial_active) { ret = ANX_EBUSY; goto done; }
	ret = anx_twin_snapshot(&twin);
	if (ret != ANX_OK) goto done;
	/* The live planner considers at most eight engines; never certify a larger set. */
	if (!twin->engine_count || twin->engine_count > ANX_MAX_ROUTE_CANDIDATES) { ret = ANX_ENOTSUP; goto done; }
	profile->schema = 1; profile->case_count = count; profile->weights = *weights;
	profile->build = *anx_kernel_profile_current(); profile->environment = *twin;
	environment_digest(twin, profile->environment_digest);
	for (uint32_t i = 0; i < count; i++) {
		struct anx_twin_simulate_result incumbent, candidate;
		if (!cases[i].topology_set && (cases[i].topology_lo || cases[i].topology_hi)) { ret = ANX_EINVAL; goto done; }
		case_cell(&cases[i], cell);
		for (uint32_t j = 0; j < i; j++) if (case_matches(&profile->cases[j], cell)) { ret = ANX_EINVAL; goto done; }
		ret = anx_twin_simulate(twin, cell, &profile->incumbent.weights, &incumbent);
		if (ret == ANX_OK) ret = anx_twin_simulate(twin, cell, weights, &candidate);
		if (ret != ANX_OK) goto done;
		if (!incumbent.candidate_count || !candidate.candidate_count || incumbent.winner_index != candidate.winner_index ||
		    incumbent.has_margin != candidate.has_margin || (incumbent.has_margin && candidate.margin < incumbent.margin)) {
			ret = ANX_EPERM; goto done;
		}
		profile->cases[i] = cases[i];
		profile->results[i] = (struct anx_route_profile_case_result){
			twin->engines[candidate.winner_index].eid, incumbent.winner_score, candidate.winner_score,
			incumbent.margin, candidate.margin, candidate.has_margin };
	}
	/* Reject a changing environment or incumbent before issuing the profile. */
	ret = anx_twin_snapshot(&after);
	if (ret != ANX_OK) goto done;
	environment_digest(after, digest); anx_route_tuning_snapshot(&current);
	if (anx_memcmp(digest, profile->environment_digest, sizeof(digest)) ||
	    anx_memcmp(&current, &profile->incumbent, sizeof(current))) { ret = ANX_EBUSY; goto done; }
	p.object_type = ANX_OBJ_STRUCTURED_DATA; p.schema_uri = ANX_ROUTE_PROFILE_SCHEMA; p.schema_version = "1";
	p.payload = profile; p.payload_size = sizeof(*profile);
	ret = anx_so_create(&p, &object);
	if (ret == ANX_OK) ret = anx_so_seal(&object->oid);
	if (ret != ANX_OK) goto done;
	anx_sha256(profile, sizeof(*profile), digest);
	anx_spin_lock_irqsave(&profile_lock, &flags);
	issued[slot].oid = object->oid; anx_memcpy(issued[slot].digest, digest, sizeof(digest)); issued[slot].state = 2;
	anx_spin_unlock_irqrestore(&profile_lock, flags);
	*out = object->oid;
done:
	if (ret != ANX_OK) {
		anx_spin_lock_irqsave(&profile_lock, &flags);
		anx_memset(&issued[slot], 0, sizeof(issued[slot]));
		anx_spin_unlock_irqrestore(&profile_lock, flags);
		if (object) anx_so_delete(&object->oid, false);
	}
	if (object) anx_objstore_release(object);
	anx_twin_destroy(twin); anx_twin_destroy(after); anx_free(profile); anx_free(cell);
	return ret;
}

int anx_route_profile_choose(const anx_oid_t *oid, const struct anx_cell *cell,
		const struct anx_route_tuning_state *incumbent, struct anx_route_weight_policy *out)
{
	struct anx_route_profile *profile = NULL;
	struct anx_resource_twin *twin = NULL;
	struct anx_object_handle h = {0};
	struct issued_profile receipt = {0};
	uint8_t digest[32];
	uint32_t slot;
	bool flags, matched = false;
	int ret;
	if (!oid || !cell || !incumbent || !out) return ANX_EINVAL;
	anx_spin_lock_irqsave(&profile_lock, &flags);
	for (slot = 0; slot < ANX_ROUTE_PROFILE_MAX; slot++) if (issued[slot].state == 2 && !anx_uuid_compare(oid, &issued[slot].oid)) break;
	if (slot < ANX_ROUTE_PROFILE_MAX) receipt = issued[slot];
	anx_spin_unlock_irqrestore(&profile_lock, flags);
	if (slot == ANX_ROUTE_PROFILE_MAX) return ANX_EPERM;
	ret = anx_so_open(oid, ANX_OPEN_READ, &h);
	if (ret != ANX_OK) return ret;
	profile = anx_alloc(sizeof(*profile));
	ret = ANX_ENOMEM;
	if (!profile) goto done;
	if (h.obj->state != ANX_OBJ_SEALED || h.obj->object_type != ANX_OBJ_STRUCTURED_DATA ||
	    h.obj->payload_size != sizeof(*profile)) { ret = ANX_EPERM; goto done; }
	ret = anx_so_read_payload(&h, 0, profile, sizeof(*profile));
	if (ret != (int)sizeof(*profile)) { if (ret >= 0) ret = ANX_EINVAL; goto done; }
	anx_sha256(profile, sizeof(*profile), digest);
	if (anx_memcmp(digest, receipt.digest, sizeof(digest)) || profile->schema != 1 ||
	    !profile->case_count || profile->case_count > ANX_ROUTE_PROFILE_CASES_MAX) { ret = ANX_EPERM; goto done; }
	ret = anx_kernel_profile_check(&profile->build);
	if (ret != ANX_OK) goto done;
	if (incumbent->trial_active || incumbent->generation != profile->incumbent.generation ||
	    anx_memcmp(&incumbent->weights, &profile->incumbent.weights, sizeof(incumbent->weights))) { ret = ANX_EBUSY; goto done; }
	for (uint32_t i = 0; i < profile->case_count; i++) if (case_matches(&profile->cases[i], cell)) matched = true;
	if (!matched) { ret = ANX_EBUSY; goto done; }
	ret = anx_twin_snapshot(&twin);
	if (ret != ANX_OK) goto done;
	environment_digest(twin, digest);
	if (anx_memcmp(digest, profile->environment_digest, sizeof(digest))) { ret = ANX_EBUSY; goto done; }
	anx_spin_lock_irqsave(&profile_lock, &flags);
	ret = anx_memcmp(&receipt, &issued[slot], sizeof(receipt)) ? ANX_EPERM : ANX_OK;
	if (ret == ANX_OK) *out = profile->weights;
	anx_spin_unlock_irqrestore(&profile_lock, flags);
done:
	anx_so_close(&h); anx_twin_destroy(twin); anx_free(profile);
	return ret;
}

int anx_route_profile_release(const anx_oid_t *oid)
{
	bool flags;
	int ret = ANX_ENOENT;
	if (anx_cell_current_id()) return ANX_EPERM;
	if (!oid) return ANX_EINVAL;
	anx_spin_lock_irqsave(&profile_lock, &flags);
	for (uint32_t i = 0; i < ANX_ROUTE_PROFILE_MAX; i++) if (issued[i].state == 2 && !anx_uuid_compare(oid, &issued[i].oid)) {
		anx_memset(&issued[i], 0, sizeof(issued[i])); ret = ANX_OK; break;
	}
	anx_spin_unlock_irqrestore(&profile_lock, flags);
	return ret;
}
