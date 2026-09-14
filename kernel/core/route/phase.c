/* Phase hints reserve through the existing ledger, one lease per attached cell. */
#include <anx/phase.h>
#include <anx/cell.h>
#include <anx/string.h>
#include <anx/uuid.h>

struct phase_record {
	struct anx_cell *owner;
	struct anx_engine_lease *lease;
	struct anx_phase_contract contract;
	struct anx_phase_view view;
};
static struct phase_record phases[ANX_PHASE_OWNER_MAX];
static struct anx_spinlock phase_lock = ANX_SPINLOCK_INIT;
static uint64_t sequence;

int anx_phase_resize(const anx_cid_t *owner, uint64_t epoch, uint64_t bytes,
		uint32_t pct, struct anx_phase_view *out)
{
	(void)owner; (void)epoch; (void)bytes; (void)pct; (void)out;
	return ANX_ENOTSUP;
}

static struct phase_record *lookup(const anx_cid_t *owner)
{
	for (uint32_t i = 0; i < ANX_PHASE_OWNER_MAX; i++)
		if (phases[i].owner && !anx_uuid_compare(owner, &phases[i].view.owner)) return &phases[i];
	return NULL;
}

static int caller_check(const anx_cid_t *owner)
{
	const anx_cid_t *caller = anx_cell_current_id();
	if (!owner || anx_uuid_is_nil(owner)) return ANX_EINVAL;
	return caller && anx_uuid_compare(owner, caller) ? ANX_EPERM : ANX_OK;
}

static bool role_allows(enum anx_phase_role role, enum anx_phase_kind phase)
{
	if (phase == ANX_PHASE_WAIT) return true;
	if (role == ANX_ROLE_CONTROL) return phase == ANX_PHASE_ORCHESTRATION;
	if (role == ANX_ROLE_ORCHESTRATOR) return phase == ANX_PHASE_ORCHESTRATION || phase == ANX_PHASE_REVIEW;
	return phase == ANX_PHASE_TOOL || phase == ANX_PHASE_INFERENCE || phase == ANX_PHASE_REVIEW;
}

static int validate(const struct anx_phase_contract *contract)
{
	uint32_t enabled = 0;
	if (!contract || contract->role < ANX_ROLE_CONTROL || contract->role > ANX_ROLE_RUNNER) return ANX_EINVAL;
	for (uint32_t i = 0; i < ANX_PHASE_COUNT; i++) {
		const struct anx_phase_limit *l = &contract->limits[i];
		if (!l->enabled) {
			if (l->tier || l->memory_bytes || l->accelerator || l->accelerator_pct) return ANX_EINVAL;
			continue;
		}
		if (i == ANX_PHASE_IDLE || !role_allows(contract->role, i)) return ANX_EPERM;
		if ((int)l->tier < 0 || l->tier >= ANX_MEM_TIER_COUNT ||
		    (int)l->accelerator < 0 || l->accelerator >= ANX_ACCEL_COUNT || l->accelerator_pct > 100 ||
		    (l->accelerator == ANX_ACCEL_NONE && l->accelerator_pct)) return ANX_EINVAL;
		/* Accelerator phases belong to runners; role hints never create control privileges. */
		if (contract->role != ANX_ROLE_RUNNER && l->accelerator != ANX_ACCEL_NONE) return ANX_EPERM;
		enabled++;
	}
	return enabled ? ANX_OK : ANX_EINVAL;
}

int anx_phase_attach(const anx_cid_t *owner, const struct anx_phase_contract *contract)
{
	struct anx_cell *cell;
	bool flags;
	int ret;
	if (anx_cell_current_id()) return ANX_EPERM;
	ret = caller_check(owner);
	if (ret == ANX_OK) ret = validate(contract);
	if (ret != ANX_OK) return ret;
	cell = anx_cell_store_lookup(owner);
	if (!cell) return ANX_ENOENT;
	if (anx_cell_status_terminal(cell->status)) { anx_cell_store_release(cell); return ANX_EPERM; }
	ret = ANX_EFULL;
	anx_spin_lock_irqsave(&phase_lock, &flags);
	if (lookup(owner)) ret = ANX_EEXIST;
	else if (sequence != ~(uint64_t)0) {
		for (uint32_t i = 0; i < ANX_PHASE_OWNER_MAX; i++) if (!phases[i].owner) {
			struct phase_record *p = &phases[i];
			p->owner = cell; p->contract = *contract;
			p->view.owner = *owner; p->view.role = contract->role; p->view.epoch = ++sequence;
			ret = ANX_OK;
			break;
		}
	}
	anx_spin_unlock_irqrestore(&phase_lock, flags);
	if (ret != ANX_OK) anx_cell_store_release(cell);
	return ret;
}

int anx_phase_get(const anx_cid_t *owner, struct anx_phase_view *out)
{
	bool flags;
	int ret = caller_check(owner);
	if (ret != ANX_OK) return ret;
	if (!out) return ANX_EINVAL;
	anx_spin_lock_irqsave(&phase_lock, &flags);
	struct phase_record *p = lookup(owner);
	if (!p) ret = ANX_ENOENT;
	else *out = p->view;
	anx_spin_unlock_irqrestore(&phase_lock, flags);
	return ret;
}

int anx_phase_begin(const anx_cid_t *owner, uint64_t epoch, const struct anx_phase_request *request)
{
	bool flags;
	int ret = caller_check(owner);
	if (ret != ANX_OK) return ret;
	if (!request || !epoch || request->phase <= ANX_PHASE_IDLE || request->phase >= ANX_PHASE_COUNT) return ANX_EINVAL;
	anx_spin_lock_irqsave(&phase_lock, &flags);
	struct phase_record *p = lookup(owner);
	if (!p) { ret = ANX_ENOENT; goto out; }
	if (p->view.epoch != epoch || p->lease) { ret = ANX_EBUSY; goto out; }
	if (anx_cell_status_terminal(p->owner->status)) { ret = ANX_EPERM; goto out; }
	const struct anx_phase_limit *l = &p->contract.limits[request->phase];
	if (!l->enabled || request->memory_bytes > l->memory_bytes ||
	    request->accelerator_pct > l->accelerator_pct) { ret = ANX_EPERM; goto out; }
	if (sequence == ~(uint64_t)0) { ret = ANX_EFULL; goto out; }
	anx_eid_t id;
	anx_uuid_generate(&id);
	ret = anx_lease_grant(&id, l->tier, request->memory_bytes, l->accelerator, request->accelerator_pct, &p->lease);
	if (ret != ANX_OK) goto out;
	p->view.phase = request->phase; p->view.epoch = ++sequence; p->view.lease_id = id;
	p->view.tier = l->tier; p->view.memory_bytes = request->memory_bytes;
	p->view.accelerator = l->accelerator; p->view.accelerator_pct = request->accelerator_pct;
out:
	anx_spin_unlock_irqrestore(&phase_lock, flags);
	return ret;
}

int anx_phase_finish(const anx_cid_t *owner, uint64_t epoch)
{
	bool flags;
	int ret = caller_check(owner);
	if (ret != ANX_OK) return ret;
	if (!epoch) return ANX_EINVAL;
	anx_spin_lock_irqsave(&phase_lock, &flags);
	struct phase_record *p = lookup(owner);
	if (!p) ret = ANX_ENOENT;
	else if (p->view.epoch != epoch || !p->lease) ret = ANX_EBUSY;
	else {
		ret = anx_lease_release(p->lease);
		if (ret == ANX_OK) {
			p->lease = NULL;
			anx_memset(&p->view, 0, sizeof(p->view));
			p->view.owner = *owner; p->view.role = p->contract.role;
			/* Exhaustion can close leases but cannot authorize any new phase. */
			p->view.epoch = sequence == ~(uint64_t)0 ? 0 : ++sequence;
		}
	}
	anx_spin_unlock_irqrestore(&phase_lock, flags);
	return ret;
}

int anx_phase_detach(const anx_cid_t *owner)
{
	struct anx_cell *cell = NULL;
	bool flags;
	int ret;
	if (anx_cell_current_id()) return ANX_EPERM;
	ret = caller_check(owner);
	if (ret != ANX_OK) return ret;
	anx_spin_lock_irqsave(&phase_lock, &flags);
	struct phase_record *p = lookup(owner);
	if (!p) ret = ANX_ENOENT;
	else if (p->lease) ret = ANX_EBUSY;
	else { cell = p->owner; anx_memset(p, 0, sizeof(*p)); }
	anx_spin_unlock_irqrestore(&phase_lock, flags);
	if (cell) anx_cell_store_release(cell);
	return ret;
}
