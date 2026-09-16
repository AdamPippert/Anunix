#ifndef ANX_SCHED_DOMAIN_H
#define ANX_SCHED_DOMAIN_H
#include <anx/sched.h>
#define ANX_SCHED_DOMAINS_MAX 32U
#define ANX_SCHED_DOMAIN_DEPTH_MAX 8U
struct anx_sched_domain_spec {
	uint32_t schema, cpu_mask, queue_mask;
	enum anx_sched_priority maximum_priority;
	anx_time_t expires_at;
};
struct anx_sched_domain_view {
	uint64_t id, epoch, parent;
	anx_cid_t owner;
	struct anx_sched_domain_spec authority;
	bool revoked;
};
/* Native synchronous dispatch currently supports only bootstrap CPU bit zero. */
int anx_sched_domain_create(const anx_cid_t *owner, uint64_t parent,
		const struct anx_sched_domain_spec *spec, struct anx_sched_domain_view *out);
int anx_sched_domain_get(uint64_t id, struct anx_sched_domain_view *out);
int anx_sched_domain_revoke(uint64_t id, uint64_t epoch);
/* A terminal owner and no child domains are required before removal. */
int anx_sched_domain_destroy(uint64_t id);
/* Internal admission checks. Cells outside a domain retain ordinary dispatch. */
int anx_sched_domain_check(struct anx_cell *cell);
int anx_sched_domain_check_queue(struct anx_cell *cell, enum anx_queue_class queue, enum anx_sched_priority priority);
#endif
