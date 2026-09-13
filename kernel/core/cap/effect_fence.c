#include <anx/effect_fence.h>
#include <anx/effect.h>
#include <anx/cell.h>
#include <anx/spinlock.h>
#include <anx/string.h>
#include <anx/uuid.h>

static struct anx_effect_fence_view fences[ANX_EFFECT_FENCE_MAX];
static uint32_t fence_count;
static struct anx_spinlock fence_lock = ANX_SPINLOCK_INIT;

static struct anx_effect_fence_view *find_fence(const anx_oid_t *id)
{
	for (uint32_t i = 0; i < fence_count; i++)
		if (!anx_uuid_compare(&fences[i].id, id))
			return &fences[i];
	return NULL;
}

static bool closed(enum anx_effect_fence_state state)
{
	return state >= ANX_FENCE_REJECTED;
}

static int state_check(const struct anx_effect_fence_view *fence)
{
	if (!fence)
		return ANX_ENOENT;
	if (fence->state == ANX_FENCE_HELD)
		return ANX_EBUSY;
	return closed(fence->state) ? ANX_EPERM : ANX_OK;
}

static int change_state(struct anx_effect_fence_view *fence, enum anx_effect_fence_state state)
{
	if (closed(fence->state))
		return ANX_EPERM;
	if (state == fence->state || (state == ANX_FENCE_RUNNING && fence->state != ANX_FENCE_HELD) ||
	    (state == ANX_FENCE_REJECTED && fence->state != ANX_FENCE_HELD))
		return ANX_EINVAL;
	/* Exhausted control revisions must still permit a permanent safety fence. */
	if (fence->generation == ~(uint64_t)0 && !closed(state))
		return ANX_EFULL;
	fence->state = state;
	if (fence->generation != ~(uint64_t)0)
		fence->generation++;
	if (closed(state))
		fence->epoch++;
	return ANX_OK;
}

int anx_effect_fence_create(struct anx_effect_fence_view *out)
{
	anx_oid_t id;
	struct anx_effect_fence_view *fence;
	bool flags;
	if (!out)
		return ANX_EINVAL;
	if (anx_cell_current_id())
		return ANX_EPERM;
	anx_uuid_generate(&id);
	anx_spin_lock_irqsave(&fence_lock, &flags);
	if (fence_count == ANX_EFFECT_FENCE_MAX || find_fence(&id)) {
		anx_spin_unlock_irqrestore(&fence_lock, flags);
		return ANX_EFULL;
	}
	fence = &fences[fence_count++];
	anx_memset(fence, 0, sizeof(*fence));
	fence->id = id;
	fence->epoch = fence->generation = 1;
	fence->state = ANX_FENCE_RUNNING;
	*out = *fence;
	anx_spin_unlock_irqrestore(&fence_lock, flags);
	return ANX_OK;
}

int anx_effect_fence_get(const anx_oid_t *id, struct anx_effect_fence_view *out)
{
	struct anx_effect_fence_view *fence;
	bool flags;
	if (!id || anx_uuid_is_nil(id) || !out)
		return ANX_EINVAL;
	anx_spin_lock_irqsave(&fence_lock, &flags);
	fence = find_fence(id);
	if (fence)
		*out = *fence;
	anx_spin_unlock_irqrestore(&fence_lock, flags);
	return fence ? ANX_OK : ANX_ENOENT;
}

int anx_effect_fence_bind(struct anx_cell *cell, const anx_oid_t *id)
{
	struct anx_effect_fence_view view;
	struct anx_cell *registered;
	int ret;
	if (anx_cell_current_id())
		return ANX_EPERM;
	if (!cell)
		return ANX_EINVAL;
	ret = anx_effect_fence_get(id, &view);
	if (ret != ANX_OK)
		return ret;
	registered = anx_cell_store_lookup(&cell->cid);
	if (!registered)
		return ANX_ENOENT;
	if (registered != cell) {
		anx_cell_store_release(registered);
		return ANX_EPERM;
	}
	anx_spin_lock(&cell->lock);
	if (cell->status != ANX_CELL_CREATED || cell->runtime_active ||
	    !anx_uuid_is_nil(&cell->effect_fence_id) || !anx_uuid_is_nil(&cell->parent_cid))
		ret = ANX_EBUSY;
	else
		cell->effect_fence_id = *id;
	anx_spin_unlock(&cell->lock);
	anx_cell_store_release(registered);
	return ret;
}

int anx_effect_fence_transition(const anx_oid_t *id, uint64_t generation, enum anx_effect_fence_state state)
{
	struct anx_effect_fence_view *fence;
	bool flags;
	int ret;
	if (anx_cell_current_id())
		return ANX_EPERM;
	if (!id || anx_uuid_is_nil(id) || !generation || (int)state < 0 || state > ANX_FENCE_TIMED_OUT)
		return ANX_EINVAL;
	anx_spin_lock_irqsave(&fence_lock, &flags);
	fence = find_fence(id);
	if (!fence)
		ret = ANX_ENOENT;
	else if (fence->generation != generation)
		ret = ANX_EBUSY;
	else
		ret = change_state(fence, state);
	anx_spin_unlock_irqrestore(&fence_lock, flags);
	return ret;
}

static int request_state(struct anx_cell *cell, enum anx_effect_fence_state state)
{
	const anx_cid_t *active = anx_cell_current_id();
	struct anx_effect_fence_view *fence;
	struct anx_cell *registered;
	bool flags;
	int ret;
	if (!cell)
		return ANX_EINVAL;
	if (anx_uuid_is_nil(&cell->effect_fence_id))
		return state == ANX_FENCE_CANCELLED ? ANX_OK : ANX_ENOENT;
	if (active && anx_uuid_compare(active, &cell->cid))
		return ANX_EPERM;
	registered = anx_cell_store_lookup(&cell->cid);
	if (!registered)
		return ANX_ENOENT;
	if (registered != cell || (state == ANX_FENCE_HELD && anx_cell_status_terminal(cell->status))) {
		anx_cell_store_release(registered);
		return ANX_EPERM;
	}
	anx_spin_lock_irqsave(&fence_lock, &flags);
	fence = find_fence(&cell->effect_fence_id);
	if (!fence)
		ret = ANX_ENOENT;
	else if (fence->state == state || (state == ANX_FENCE_CANCELLED && closed(fence->state)))
		ret = ANX_OK;
	else
		ret = change_state(fence, state);
	anx_spin_unlock_irqrestore(&fence_lock, flags);
	anx_cell_store_release(registered);
	return ret;
}

int anx_effect_fence_hold(struct anx_cell *cell)
{
	return request_state(cell, ANX_FENCE_HELD);
}

int anx_effect_fence_cancel(struct anx_cell *cell)
{
	return request_state(cell, ANX_FENCE_CANCELLED);
}

int anx_effect_fence_check(const struct anx_cell *cell, anx_oid_t *id_out, uint64_t *epoch_out)
{
	struct anx_effect_fence_view *fence;
	bool flags;
	int ret;
	if (id_out) *id_out = ANX_UUID_NIL;
	if (epoch_out) *epoch_out = 0;
	if (!cell)
		return ANX_EINVAL;
	if (anx_uuid_is_nil(&cell->effect_fence_id))
		return ANX_OK;
	anx_spin_lock_irqsave(&fence_lock, &flags);
	fence = find_fence(&cell->effect_fence_id);
	if (fence) {
		if (id_out) *id_out = fence->id;
		if (epoch_out) *epoch_out = fence->epoch;
	}
	ret = state_check(fence);
	anx_spin_unlock_irqrestore(&fence_lock, flags);
	return ret;
}

int anx_effect_fence_observe_read(const anx_oid_t *oid, enum anx_sensitivity sensitivity)
{
	const anx_cid_t *active = anx_cell_current_id();
	struct anx_effect_fence_view *fence;
	struct anx_cell *cell;
	bool flags;
	int ret = ANX_OK;
	if (!active)
		return ANX_OK;
	cell = anx_cell_store_lookup(active);
	if (!cell)
		return ANX_ENOENT;
	if (anx_uuid_is_nil(&cell->effect_fence_id))
		goto out;
	if (!oid || anx_uuid_is_nil(oid) || (int)sensitivity < 0 || sensitivity > ANX_SENSITIVITY_RESTRICTED) {
		ret = ANX_EINVAL;
		goto out;
	}
	anx_spin_lock_irqsave(&fence_lock, &flags);
	fence = find_fence(&cell->effect_fence_id);
	if (!fence)
		ret = ANX_ENOENT;
	else {
		if (sensitivity > fence->read_sensitivity) {
			fence->read_sensitivity = sensitivity;
			fence->read_origin = *oid;
		}
		if (fence->read_count != ~(uint64_t)0)
			fence->read_count++;
	}
	anx_spin_unlock_irqrestore(&fence_lock, flags);
out:
	anx_cell_store_release(cell);
	return ret;
}

static int sink_check(const struct anx_effect_fence_view *fence, const struct anx_sink *sink)
{
	if (!fence)
		return ANX_ENOENT;
	if (!sink)
		return fence->read_sensitivity == ANX_SENSITIVITY_PUBLIC ? ANX_OK : ANX_EPERM;
	if ((int)sink->max_sensitivity < 0 || sink->max_sensitivity > ANX_SENSITIVITY_RESTRICTED)
		return ANX_EINVAL;
	return fence->read_sensitivity <= sink->max_sensitivity ? ANX_OK : ANX_EPERM;
}

int anx_effect_fence_check_sink(const struct anx_cell *cell, const struct anx_sink *sink)
{
	bool flags;
	int ret;
	if (!cell)
		return ANX_EINVAL;
	if (anx_uuid_is_nil(&cell->effect_fence_id))
		return ANX_OK;
	anx_spin_lock_irqsave(&fence_lock, &flags);
	ret = sink_check(find_fence(&cell->effect_fence_id), sink);
	anx_spin_unlock_irqrestore(&fence_lock, flags);
	return ret;
}

int anx_effect_fence_dispatch(struct anx_pending_effect *effect)
{
	struct anx_effect_fence_view *fence;
	struct anx_cell *cell;
	bool flags;
	int ret = ANX_OK;
	if (!effect)
		return ANX_EINVAL;
	cell = anx_cell_store_lookup(&effect->cell);
	if (!cell)
		return ANX_ENOENT;
	anx_spin_lock_irqsave(&fence_lock, &flags);
	if (effect->phase != ANX_EFFECT_PREPARED)
		ret = ANX_EINVAL;
	else if (anx_uuid_compare(&cell->effect_fence_id, &effect->fence_id))
		ret = ANX_EPERM;
	else if (anx_uuid_is_nil(&effect->fence_id))
		ret = effect->fence_epoch ? ANX_EPERM : ANX_OK;
	else {
		fence = find_fence(&effect->fence_id);
		if (!fence)
			ret = ANX_ENOENT;
		else if (fence->epoch != effect->fence_epoch)
			ret = ANX_EPERM;
		else
			ret = state_check(fence);
		if (ret == ANX_OK)
			ret = sink_check(fence, effect->sink);
	}
	if (ret == ANX_OK)
		effect->phase = ANX_EFFECT_DISPATCHING;
	anx_spin_unlock_irqrestore(&fence_lock, flags);
	anx_cell_store_release(cell);
	return ret;
}
