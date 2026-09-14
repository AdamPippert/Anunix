#include <anx/sched_domain.h>
#include <anx/arch.h>
#include <anx/spinlock.h>
#include <anx/string.h>
#include <anx/uuid.h>

struct scheduler_domain {
	struct anx_sched_domain_view view;
	struct anx_cell *owner;
	anx_cid_t declared_parent;
	uint32_t declared_depth, depth;
};
static struct scheduler_domain domains[ANX_SCHED_DOMAINS_MAX];
static struct anx_spinlock domain_lock = ANX_SPINLOCK_INIT;
static uint64_t sequence;

static struct scheduler_domain *find(uint64_t id)
{
	for (uint32_t i = 0; id && i < ANX_SCHED_DOMAINS_MAX; i++)
		if (domains[i].owner && domains[i].view.id == id) return &domains[i];
	return NULL;
}
static struct scheduler_domain *for_cell(const anx_cid_t *cid)
{
	for (uint32_t i = 0; i < ANX_SCHED_DOMAINS_MAX; i++)
		if (domains[i].owner && !anx_uuid_compare(cid, &domains[i].view.owner)) return &domains[i];
	return NULL;
}
static int live(struct scheduler_domain *d, anx_time_t now)
{
	for (uint32_t depth = 0; d && depth < ANX_SCHED_DOMAIN_DEPTH_MAX; depth++) {
		if (d->view.revoked || anx_cell_status_terminal(d->owner->status) ||
		    anx_uuid_compare(&d->owner->parent_cid, &d->declared_parent) ||
		    d->owner->recursion_depth != d->declared_depth) return ANX_EPERM;
		if (d->view.authority.expires_at && now >= d->view.authority.expires_at) return ANX_ETIMEDOUT;
		if (!d->view.parent) return ANX_OK;
		d = find(d->view.parent);
	}
	return ANX_EPERM;
}
static bool subset(const struct anx_sched_domain_spec *child, const struct anx_sched_domain_spec *parent)
{
	return !(child->cpu_mask & ~parent->cpu_mask) && !(child->queue_mask & ~parent->queue_mask) &&
		child->maximum_priority <= parent->maximum_priority &&
		(!parent->expires_at || (child->expires_at && child->expires_at <= parent->expires_at));
}
int anx_sched_domain_create(const anx_cid_t *owner, uint64_t parent,
		const struct anx_sched_domain_spec *spec, struct anx_sched_domain_view *out)
{
	if (anx_cell_current_id()) return ANX_EPERM;
	if (!owner || !spec || !out || spec->schema != 1 || !spec->cpu_mask || !spec->queue_mask ||
	    (spec->queue_mask >> ANX_QUEUE_CLASS_COUNT) || (int)spec->maximum_priority < 0 ||
	    spec->maximum_priority > ANX_PRIO_CRITICAL) return ANX_EINVAL;
	if (spec->cpu_mask != 1) return ANX_ENOTSUP;
	anx_time_t now = arch_time_now();
	if (spec->expires_at && spec->expires_at <= now) return ANX_ETIMEDOUT;
	struct anx_cell *cell = anx_cell_store_lookup(owner);
	if (!cell) return ANX_ENOENT;
	int ret = anx_cell_check_scope(cell);
	if (ret == ANX_OK && (anx_cell_status_terminal(cell->status) || cell->runtime_active)) ret = ANX_EPERM;
	if (ret != ANX_OK) { anx_cell_store_release(cell); return ret; }
	bool flags;
	anx_spin_lock_irqsave(&domain_lock, &flags);
	struct scheduler_domain *p = parent ? find(parent) : NULL;
	if (for_cell(owner)) { ret = ANX_EEXIST; goto done; }
	if (parent && !p) { ret = ANX_ENOENT; goto done; }
	if ((!parent && !anx_uuid_is_nil(&cell->parent_cid)) ||
	    (p && (anx_uuid_compare(&cell->parent_cid, &p->view.owner) || !subset(spec, &p->view.authority)))) {
		ret = ANX_EPERM; goto done;
	}
	if (p && (ret = live(p, now)) != ANX_OK) goto done;
	if ((p && p->depth + 1 >= ANX_SCHED_DOMAIN_DEPTH_MAX) || sequence == ~(uint64_t)0) { ret = ANX_EFULL; goto done; }
	uint32_t i;
	for (i = 0; i < ANX_SCHED_DOMAINS_MAX; i++) if (!domains[i].owner) break;
	if (i == ANX_SCHED_DOMAINS_MAX) { ret = ANX_EFULL; goto done; }
	struct scheduler_domain *d = &domains[i];
	anx_memset(d, 0, sizeof(*d));
	d->owner = cell; d->declared_parent = cell->parent_cid; d->declared_depth = cell->recursion_depth;
	d->depth = p ? p->depth + 1 : 0;
	d->view.id = ++sequence; d->view.epoch = 1; d->view.parent = parent;
	d->view.owner = *owner; d->view.authority = *spec;
	*out = d->view;
done:
	anx_spin_unlock_irqrestore(&domain_lock, flags);
	if (ret != ANX_OK) anx_cell_store_release(cell);
	return ret;
}
int anx_sched_domain_get(uint64_t id, struct anx_sched_domain_view *out)
{
	if (!id || !out) return ANX_EINVAL;
	bool flags;
	anx_spin_lock_irqsave(&domain_lock, &flags);
	struct scheduler_domain *d = find(id);
	const anx_cid_t *caller = anx_cell_current_id();
	int ret = !d ? ANX_ENOENT : caller && anx_uuid_compare(caller, &d->view.owner) ? ANX_EPERM : ANX_OK;
	if (ret == ANX_OK) *out = d->view;
	anx_spin_unlock_irqrestore(&domain_lock, flags);
	return ret;
}
static bool descendant(struct scheduler_domain *d, uint64_t id)
{
	for (uint32_t depth = 0; d && depth < ANX_SCHED_DOMAIN_DEPTH_MAX; depth++) {
		if (d->view.id == id) return true;
		d = find(d->view.parent);
	}
	return false;
}
int anx_sched_domain_revoke(uint64_t id, uint64_t epoch)
{
	if (anx_cell_current_id()) return ANX_EPERM;
	if (!id || !epoch) return ANX_EINVAL;
	bool flags;
	anx_spin_lock_irqsave(&domain_lock, &flags);
	struct scheduler_domain *d = find(id);
	int ret = !d ? ANX_ENOENT : d->view.epoch != epoch || d->view.revoked ? ANX_EBUSY : ANX_OK;
	if (ret == ANX_OK) for (uint32_t i = 0; i < ANX_SCHED_DOMAINS_MAX; i++)
		if (domains[i].owner && !domains[i].view.revoked && descendant(&domains[i], id)) {
			domains[i].view.revoked = true; domains[i].view.epoch++;
		}
	anx_spin_unlock_irqrestore(&domain_lock, flags);
	return ret;
}
int anx_sched_domain_destroy(uint64_t id)
{
	if (anx_cell_current_id()) return ANX_EPERM;
	if (!id) return ANX_EINVAL;
	bool flags;
	struct anx_cell *owner = NULL;
	anx_spin_lock_irqsave(&domain_lock, &flags);
	struct scheduler_domain *d = find(id);
	int ret = !d ? ANX_ENOENT : !anx_cell_status_terminal(d->owner->status) || d->owner->runtime_active ? ANX_EBUSY : ANX_OK;
	if (ret == ANX_OK) for (uint32_t i = 0; i < ANX_SCHED_DOMAINS_MAX; i++)
		if (domains[i].owner && domains[i].view.parent == id) { ret = ANX_EBUSY; break; }
	if (ret == ANX_OK) { owner = d->owner; anx_memset(d, 0, sizeof(*d)); }
	anx_spin_unlock_irqrestore(&domain_lock, flags);
	if (owner) anx_cell_store_release(owner);
	return ret;
}
static int check(struct anx_cell *cell, bool queued, enum anx_queue_class queue, enum anx_sched_priority priority)
{
	if (!cell) return ANX_EINVAL;
	struct anx_cell *current = cell;
	int ret = ANX_OK;
	bool flags, managed = false;
	anx_time_t now = arch_time_now();
	anx_spin_lock_irqsave(&domain_lock, &flags);
	for (uint32_t depth = 0;; depth++) {
		struct scheduler_domain *d = for_cell(&current->cid);
		if (d) {
			managed = true;
			ret = live(d, now);
			if (ret != ANX_OK) break;
			if (!(d->view.authority.cpu_mask & 1U) || (queued &&
			    (!(d->view.authority.queue_mask & (1U << queue)) || priority > d->view.authority.maximum_priority))) {
				ret = ANX_EPERM; break;
			}
		}
		if (anx_uuid_is_nil(&current->parent_cid)) break;
		if (depth + 1 >= ANX_SCHED_DOMAIN_DEPTH_MAX) { ret = ANX_EPERM; break; }
		struct anx_cell *parent = anx_cell_store_lookup(&current->parent_cid);
		if (!parent) { ret = ANX_EPERM; break; }
		if (current != cell) anx_cell_store_release(current);
		current = parent;
	}
	if (current != cell) anx_cell_store_release(current);
	anx_spin_unlock_irqrestore(&domain_lock, flags);
	if (ret == ANX_OK && managed) ret = anx_cell_check_scope(cell);
	return ret;
}
int anx_sched_domain_check(struct anx_cell *cell)
{
	return check(cell, false, ANX_QUEUE_INTERACTIVE, ANX_PRIO_LOW);
}
int anx_sched_domain_check_queue(struct anx_cell *cell, enum anx_queue_class queue, enum anx_sched_priority priority)
{
	if ((int)queue < 0 || queue >= ANX_QUEUE_CLASS_COUNT || (int)priority < 0 || priority > ANX_PRIO_CRITICAL) return ANX_EINVAL;
	return check(cell, true, queue, priority);
}
