/* Fixed routing cases and reference winners belong to the compiled harness. */
#include <anx/optimization_harness.h>
#include <anx/state_object.h>
#include <anx/alloc.h>
#include <anx/crypto.h>
#include <anx/string.h>
#include <anx/uuid.h>

struct issued_receipt {
	uint32_t state; /* 0 free, 1 reserved, 2 issued */
	anx_oid_t oid;
	uint8_t action_digest[32];
	uint8_t payload_digest[32];
	bool passed;
};
static struct issued_receipt issued[ANX_ROUTE_HARNESS_RECEIPTS];
static struct anx_spinlock issuance_lock = ANX_SPINLOCK_INIT;

static void hash_word(struct anx_sha256_ctx *hash, uint64_t value)
{
	uint8_t bytes[8];
	for (uint32_t i = 0; i < 8; i++) bytes[i] = (uint8_t)(value >> (8 * i));
	anx_sha256_update(hash, bytes, sizeof(bytes));
}

static void action_digest(const struct anx_route_tuning_action *a, uint8_t out[32])
{
	struct anx_sha256_ctx hash;
	uint8_t task_digest[32];
	uint64_t fields[] = {
		a->schema, a->expected_generation, (uint32_t)a->weights.locality_bonus,
		(uint32_t)a->weights.local_first_bonus, (uint32_t)a->weights.gpu_cost_divisor,
		(uint32_t)a->weights.cpu_cost_divisor, (uint32_t)a->weights.degraded_penalty,
		(uint32_t)a->weights.private_data_bonus, (uint32_t)a->weights.topology_overlap_bonus,
		(uint32_t)a->weights.topology_mismatch_penalty, a->target.schema, a->target.build.schema,
		a->target.build.architecture, a->target.build.research_test, a->target.engine_id.hi,
		a->target.engine_id.lo, a->target.knowledge.oid.hi, a->target.knowledge.oid.lo,
		a->target.knowledge.version, a->target.knowledge.sensitivity,
	};
	anx_sha256_init(&hash);
	for (uint32_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) hash_word(&hash, fields[i]);
	anx_sha256_update(&hash, a->target.engine_digest, sizeof(a->target.engine_digest));
	anx_sha256_update(&hash, a->target.knowledge.digest, sizeof(a->target.knowledge.digest));
	anx_route_policy_digest(&a->task, task_digest);
	anx_sha256_update(&hash, task_digest, sizeof(task_digest));
	anx_sha256_final(&hash, out);
}

static int evaluate_cases(const struct anx_route_tuning_action *action, struct anx_route_evaluation *record)
{
	struct anx_resource_twin *twin = anx_zalloc(sizeof(*twin));
	struct anx_cell *cell = anx_zalloc(sizeof(*cell));
	struct anx_twin_simulate_result result;
	int ret = ANX_ENOMEM;
	if (!twin || !cell) goto out;
	twin->engine_count = 2;
	for (uint32_t i = 0; i < 2; i++) {
		struct anx_twin_engine_snapshot *engine = &twin->engines[i];
		engine->eid.lo = i + 1;
		engine->engine_class = i ? ANX_ENGINE_REMOTE_MODEL : ANX_ENGINE_LOCAL_MODEL;
		engine->status = ANX_ENGINE_AVAILABLE;
		engine->readiness = ANX_READY_HEALTHY;
		engine->quality_score = i ? 60 : 50;
		engine->is_local = !i;
		engine->requires_network = i;
	}
	record->case_count = ANX_ROUTE_HARNESS_CASES;
	for (uint32_t i = 0; i < ANX_ROUTE_HARNESS_CASES; i++) {
		cell->routing.strategy = ANX_ROUTE_LOCAL_FIRST;
		cell->constraints.locality = i == 1 ? ANX_LOCAL_ONLY : ANX_REMOTE_ALLOWED;
		cell->execution.allow_network = i != 2;
		cell->execution.allow_remote_models = i != 3;
		ret = anx_twin_simulate(twin, cell, &action->weights, &result);
		if (ret != ANX_OK) goto out;
		record->observed_winner[i] = result.candidate_count ? result.winner_index : ~(uint32_t)0;
		/* Every fixed case requires the local engine, including the preference case. */
		if (!result.candidate_count || result.winner_index != 0 ||
		    anx_route_policy_result_check(&action->task, twin, &result) != ANX_OK) record->failure_count++;
	}
out:
	anx_free(twin);
	anx_free(cell);
	return ret;
}

int anx_route_evaluate(const struct anx_route_tuning_action *action, anx_oid_t *out)
{
	struct anx_route_tuning_action proposal;
	struct anx_route_evaluation record = {0};
	struct anx_so_create_params params = {0};
	struct anx_state_object *object = NULL;
	uint8_t payload_digest[32];
	uint32_t slot;
	bool flags;
	int ret;
	if (anx_cell_current_id()) return ANX_EPERM;
	if (!action || !out) return ANX_EINVAL;
	proposal = *action;
	if (proposal.schema != 1 || !proposal.expected_generation || proposal.target.schema != 1 ||
	    anx_route_policy_validate(&proposal.task, &proposal.weights) != ANX_OK) return ANX_EINVAL;
	ret = anx_route_target_check(&proposal.target);
	if (ret != ANX_OK) return ret;
	anx_spin_lock_irqsave(&issuance_lock, &flags);
	for (slot = 0; slot < ANX_ROUTE_HARNESS_RECEIPTS; slot++)
		if (!issued[slot].state) break;
	if (slot < ANX_ROUTE_HARNESS_RECEIPTS) issued[slot].state = 1;
	anx_spin_unlock_irqrestore(&issuance_lock, flags);
	if (slot == ANX_ROUTE_HARNESS_RECEIPTS) return ANX_EFULL;
	record.schema = 1;
	action_digest(&proposal, record.action_digest);
	ret = evaluate_cases(&proposal, &record);
	if (ret != ANX_OK) goto out;
	params.object_type = ANX_OBJ_STRUCTURED_DATA;
	params.schema_uri = ANX_ROUTE_HARNESS_SCHEMA;
	params.schema_version = "1";
	params.payload = &record;
	params.payload_size = sizeof(record);
	params.parent_oids = &proposal.target.knowledge.oid;
	params.parent_count = 1;
	ret = anx_so_create(&params, &object);
	if (ret == ANX_OK) ret = anx_so_seal(&object->oid);
	if (ret != ANX_OK) goto out;
	anx_sha256(&record, sizeof(record), payload_digest);
	anx_spin_lock_irqsave(&issuance_lock, &flags);
	issued[slot].oid = object->oid;
	anx_memcpy(issued[slot].action_digest, record.action_digest, sizeof(record.action_digest));
	anx_memcpy(issued[slot].payload_digest, payload_digest, sizeof(payload_digest));
	issued[slot].passed = record.failure_count == 0;
	issued[slot].state = 2;
	anx_spin_unlock_irqrestore(&issuance_lock, flags);
	*out = object->oid;
out:
	if (ret != ANX_OK) {
		anx_spin_lock_irqsave(&issuance_lock, &flags);
		issued[slot].state = 0;
		anx_spin_unlock_irqrestore(&issuance_lock, flags);
		if (object) anx_so_delete(&object->oid, false);
	}
	if (object) anx_objstore_release(object);
	return ret;
}

int anx_route_evaluation_check(const struct anx_route_tuning_action *action,
			       const struct anx_route_artifact_ref *receipt)
{
	uint8_t digest[32];
	bool flags;
	int ret = ANX_EPERM;
	if (anx_cell_current_id()) return ANX_EPERM;
	if (!action || !receipt) return ANX_EINVAL;
	action_digest(action, digest);
	anx_spin_lock_irqsave(&issuance_lock, &flags);
	for (uint32_t i = 0; i < ANX_ROUTE_HARNESS_RECEIPTS; i++) {
		const struct issued_receipt *entry = &issued[i];
		if (entry->state == 2 && !anx_uuid_compare(&entry->oid, &receipt->oid)) {
			if (entry->passed && !anx_memcmp(entry->action_digest, digest, sizeof(digest)) &&
			    !anx_memcmp(entry->payload_digest, receipt->digest, sizeof(receipt->digest))) ret = ANX_OK;
			break;
		}
	}
	anx_spin_unlock_irqrestore(&issuance_lock, flags);
	return ret;
}

int anx_route_evaluated_action_check(const struct anx_route_tuning_action *action)
{
	uint8_t digest[32];
	bool flags;
	if (anx_cell_current_id()) return ANX_EPERM;
	if (!action) return ANX_EINVAL;
	action_digest(action, digest);
	for (uint32_t i = 0; i < ANX_ROUTE_HARNESS_RECEIPTS; i++) {
		struct issued_receipt receipt;
		struct anx_object_handle h = {0};
		uint8_t payload[32];
		anx_spin_lock_irqsave(&issuance_lock, &flags);
		receipt = issued[i];
		anx_spin_unlock_irqrestore(&issuance_lock, flags);
		if (receipt.state != 2 || !receipt.passed || anx_memcmp(receipt.action_digest, digest, sizeof(digest))) continue;
		int ret = anx_so_open(&receipt.oid, ANX_OPEN_READ, &h);
		if (ret != ANX_OK) continue;
		anx_spin_lock(&h.obj->lock);
		if (h.obj->state != ANX_OBJ_SEALED || h.obj->object_type != ANX_OBJ_STRUCTURED_DATA ||
		    h.obj->payload_size != sizeof(struct anx_route_evaluation) || !h.obj->payload) ret = ANX_EPERM;
		else {
			anx_sha256(h.obj->payload, (uint32_t)h.obj->payload_size, payload);
			if (anx_memcmp(payload, receipt.payload_digest, sizeof(payload))) ret = ANX_EPERM;
		}
		anx_spin_unlock(&h.obj->lock);
		anx_so_close(&h);
		if (ret != ANX_OK) continue;
		anx_spin_lock_irqsave(&issuance_lock, &flags);
		ret = !anx_memcmp(&receipt, &issued[i], sizeof(receipt)) ? ANX_OK : ANX_EPERM;
		anx_spin_unlock_irqrestore(&issuance_lock, flags);
		if (ret == ANX_OK) return ret;
	}
	return ANX_EPERM;
}

int anx_route_evaluation_release(const anx_oid_t *receipt)
{
	bool flags;
	int ret = ANX_ENOENT;
	if (anx_cell_current_id()) return ANX_EPERM;
	if (!receipt || anx_uuid_is_nil(receipt)) return ANX_EINVAL;
	anx_spin_lock_irqsave(&issuance_lock, &flags);
	for (uint32_t i = 0; i < ANX_ROUTE_HARNESS_RECEIPTS; i++) {
		if (issued[i].state == 2 && !anx_uuid_compare(&issued[i].oid, receipt)) {
			anx_memset(&issued[i], 0, sizeof(issued[i]));
			ret = ANX_OK;
			break;
		}
	}
	anx_spin_unlock_irqrestore(&issuance_lock, flags);
	return ret;
}
