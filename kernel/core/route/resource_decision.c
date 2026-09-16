#include <anx/resource_decision.h>
#include <anx/effect_fence.h>
#include <anx/identity.h>
#include <anx/sched_domain.h>
#include <anx/alloc.h>
#include <anx/string.h>
#include <anx/uuid.h>
#include <anx/page.h>
#include <anx/spinlock.h>
struct resource_decision {
	struct anx_resource_decision_view view;
	struct anx_cell *owner;
	anx_cid_t parent;
};
static struct resource_decision *decisions[ANX_RESOURCE_DECISION_MAX];
static struct anx_spinlock decision_lock=ANX_SPINLOCK_INIT;
static uint64_t sequence;
static struct resource_decision *find(uint64_t id)
{
	for (uint32_t i=0;i<ANX_RESOURCE_DECISION_MAX;i++) if (decisions[i] && decisions[i]->view.id==id) return decisions[i];
	return NULL;
}
static int access(struct resource_decision *d)
{
	if (!d) return ANX_ENOENT;
	const anx_cid_t *caller=anx_cell_current_id();
	return caller && anx_uuid_compare(caller,&d->view.owner) ? ANX_EPERM : ANX_OK;
}
static int observe(struct resource_decision *d, struct anx_resource_decision_view *out)
{
	struct anx_resource_decision_view v={0};
	if (anx_cell_status_terminal(d->owner->status) || d->owner->runtime_active) return ANX_EBUSY;
	if (anx_uuid_compare(&d->owner->parent_cid,&d->parent)) return ANX_EPERM;
	int ret=anx_cell_check_scope(d->owner);
	if (ret==ANX_OK) ret=anx_cell_check_contract(d->owner);
	if (ret==ANX_OK) ret=anx_sched_domain_check(d->owner);
	if (ret==ANX_OK) ret=anx_identity_admit(d->owner,&v.identity_record);
	if (ret==ANX_OK) ret=anx_effect_fence_check(d->owner,NULL,NULL);
	v.owner=d->view.owner;v.fence=d->owner->effect_fence_id;
	if (ret==ANX_OK && !anx_uuid_is_nil(&v.fence)) {
		struct anx_effect_fence_view fence;
		ret=anx_effect_fence_get(&v.fence,&fence);
		if (ret==ANX_OK) v.fence_generation=fence.generation;
	}
	if (ret==ANX_OK) ret=anx_logical_graph_get(d->view.logical.id,&v.logical);
	if (ret==ANX_OK) ret=anx_resource_shape_get(d->view.shape.id,&v.shape);
	if (ret==ANX_OK && (anx_uuid_compare(&v.logical.owner,&v.owner) || anx_uuid_compare(&v.shape.owner,&v.owner))) ret=ANX_EPERM;
	if (ret==ANX_OK) ret=anx_phase_get(&v.owner,&v.phase);
	if (ret==ANX_OK) ret=anx_resource_shape_check(v.shape.id,v.shape.epoch,0,&v.owner,&v.shape.source);
	if (ret==ANX_OK && (uint64_t)d->view.desired_replicas*ANX_PAGE_SIZE>v.phase.memory_bytes) ret=ANX_ENOMEM;
	if (ret==ANX_OK) *out=v;
	return ret;
}
static bool same_observation(const struct anx_resource_decision_view *a, const struct anx_resource_decision_view *b)
{
	const struct anx_logical_graph_view *x=&a->logical,*y=&b->logical;
	const struct anx_phase_view *p=&a->phase,*q=&b->phase;
	const struct anx_resource_shape_view *s=&a->shape,*t=&b->shape;
	return x->id==y->id && x->epoch==y->epoch && x->completed==y->completed && x->state==y->state && x->count==y->count &&
		x->physical_operations==y->physical_operations && x->reused_operations==y->reused_operations && x->result==y->result &&
		!anx_memcmp(x->program_digest,y->program_digest,32) &&
		p->epoch==q->epoch && p->parked==q->parked && p->phase==q->phase && p->role==q->role &&
		p->tier==q->tier && p->memory_bytes==q->memory_bytes && p->accelerator==q->accelerator &&
		p->accelerator_pct==q->accelerator_pct && !anx_uuid_compare(&p->lease_id,&q->lease_id) &&
		s->id==t->id && s->epoch==t->epoch && s->replicas==t->replicas && s->resident_replicas==t->resident_replicas &&
		s->physical_pages==t->physical_pages && s->resident_bytes==t->resident_bytes && s->geometry==t->geometry &&
		!anx_uuid_compare(&s->source.oid,&t->source.oid) && s->source.version==t->source.version &&
		s->source.size==t->source.size && s->source.sensitivity==t->source.sensitivity && !anx_memcmp(s->source.digest,t->source.digest,32) &&
		!anx_uuid_compare(&a->identity_record,&b->identity_record) && !anx_uuid_compare(&a->fence,&b->fence) && a->fence_generation==b->fence_generation;
}
int anx_resource_decision_propose(uint64_t graph, uint64_t shape, uint32_t replicas, struct anx_resource_decision_view *out)
{
	if (anx_cell_current_id()) return ANX_EPERM;
	if (!graph || !shape || !out || !replicas || replicas>ANX_RESOURCE_SHAPE_REPLICAS) return ANX_EINVAL;
	struct resource_decision *d=anx_zalloc(sizeof(*d));
	if (!d) return ANX_ENOMEM;
	d->view.logical.id=graph;d->view.shape.id=shape;d->view.desired_replicas=replicas;
	bool flags;anx_spin_lock_irqsave(&decision_lock,&flags);
	struct anx_logical_graph_view logical;
	int ret=anx_logical_graph_get(graph,&logical);
	if (ret==ANX_OK) {
		d->view.owner=logical.owner;d->owner=anx_cell_store_lookup(&logical.owner);
		if (!d->owner) ret=ANX_ENOENT;
		else d->parent=d->owner->parent_cid;
	}
	struct anx_resource_decision_view observed;
	if (ret==ANX_OK) ret=observe(d,&observed);
	uint32_t slot=0;
	while (slot<ANX_RESOURCE_DECISION_MAX && decisions[slot]) slot++;
	if (ret==ANX_OK && (slot==ANX_RESOURCE_DECISION_MAX || sequence==~(uint64_t)0)) ret=ANX_EFULL;
	if (ret==ANX_OK) {
		d->view=observed;d->view.id=++sequence;d->view.desired_replicas=replicas;
		decisions[slot]=d;*out=d->view;
	}
	anx_spin_unlock_irqrestore(&decision_lock,flags);
	if (ret!=ANX_OK) {if (d->owner) anx_cell_store_release(d->owner);anx_free(d);}
	return ret;
}
int anx_resource_decision_commit(uint64_t id, struct anx_resource_shape_view *out)
{
	if (anx_cell_current_id()) return ANX_EPERM;
	if (!id || !out) return ANX_EINVAL;
	/* No planner or external callback runs inside this bootstrap-CPU commit boundary. */
	bool flags;anx_spin_lock_irqsave(&decision_lock,&flags);
	struct resource_decision *d=find(id);int ret=access(d);
	if (ret==ANX_OK && d->view.state!=ANX_DECISION_PROPOSED) ret=ANX_EBUSY;
	if (ret==ANX_OK) {
		struct anx_resource_decision_view observed;
		ret=observe(d,&observed);
		if (ret==ANX_OK && !same_observation(&d->view,&observed)) ret=ANX_EBUSY;
		struct anx_resource_shape_view result;
		if (ret==ANX_OK) ret=anx_resource_shape_resize(d->view.shape.id,d->view.shape.epoch,d->view.desired_replicas,&result);
		d->view.result=ret;d->view.state=ret==ANX_OK ? ANX_DECISION_COMMITTED : ANX_DECISION_STALE;
		if (ret==ANX_OK) {d->view.applied_shape_epoch=result.epoch;*out=result;}
	}
	anx_spin_unlock_irqrestore(&decision_lock,flags);return ret;
}
int anx_resource_decision_get(uint64_t id, struct anx_resource_decision_view *out)
{
	if (!id || !out) return ANX_EINVAL;
	bool flags;anx_spin_lock_irqsave(&decision_lock,&flags);
	struct resource_decision *d=find(id);int ret=access(d);
	if (ret==ANX_OK) *out=d->view;
	anx_spin_unlock_irqrestore(&decision_lock,flags);return ret;
}
int anx_resource_decision_destroy(uint64_t id)
{
	if (anx_cell_current_id()) return ANX_EPERM;
	if (!id) return ANX_EINVAL;
	bool flags;anx_spin_lock_irqsave(&decision_lock,&flags);
	struct resource_decision *d=find(id);int ret=access(d);
	if (ret==ANX_OK) {
		for (uint32_t i=0;i<ANX_RESOURCE_DECISION_MAX;i++) if (decisions[i]==d) decisions[i]=NULL;
		anx_cell_store_release(d->owner);anx_memset(d,0,sizeof(*d));anx_free(d);
	}
	anx_spin_unlock_irqrestore(&decision_lock,flags);return ret;
}
