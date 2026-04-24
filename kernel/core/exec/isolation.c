/*
 * isolation.c — Untrusted application/process isolation (P2-003).
 */

#include <anx/isolation.h>
#include <anx/spinlock.h>
#include <anx/string.h>
#include <anx/uuid.h>
#include <anx/arch.h>
#include <anx/types.h>

/* ------------------------------------------------------------------ */
/* Domain registry                                                      */
/* ------------------------------------------------------------------ */

struct domain_entry {
	anx_cid_t             cid;
	enum anx_trust_domain domain;
	bool                  active;
};

static struct domain_entry  domain_reg[ANX_ISOLATION_CELL_MAX];
static struct anx_spinlock  iso_lock;

/* ------------------------------------------------------------------ */
/* IPC policy                                                           */
/* ------------------------------------------------------------------ */

static struct anx_ipc_policy sys_policy;

/* ------------------------------------------------------------------ */
/* Violation log                                                        */
/* ------------------------------------------------------------------ */

static struct anx_violation_event vlog[ANX_VIOLATION_LOG_MAX];
static uint32_t                   vlog_count;

/* ------------------------------------------------------------------ */
/* Domain registry operations                                           */
/* ------------------------------------------------------------------ */

int
anx_isolation_set_domain(anx_cid_t cid, enum anx_trust_domain domain)
{
	uint32_t i;
	bool flags;

	if (domain >= ANX_DOMAIN_COUNT)
		return ANX_EINVAL;

	anx_spin_lock_irqsave(&iso_lock, &flags);

	/* update existing */
	for (i = 0; i < ANX_ISOLATION_CELL_MAX; i++) {
		if (domain_reg[i].active &&
		    anx_uuid_compare(&domain_reg[i].cid, &cid) == 0) {
			domain_reg[i].domain = domain;
			anx_spin_unlock_irqrestore(&iso_lock, flags);
			return ANX_OK;
		}
	}

	/* insert new */
	for (i = 0; i < ANX_ISOLATION_CELL_MAX; i++) {
		if (!domain_reg[i].active) {
			domain_reg[i].cid    = cid;
			domain_reg[i].domain = domain;
			domain_reg[i].active = true;
			anx_spin_unlock_irqrestore(&iso_lock, flags);
			return ANX_OK;
		}
	}

	anx_spin_unlock_irqrestore(&iso_lock, flags);
	return ANX_EFULL;
}

enum anx_trust_domain
anx_isolation_get_domain(anx_cid_t cid)
{
	uint32_t i;
	bool flags;

	anx_spin_lock_irqsave(&iso_lock, &flags);

	for (i = 0; i < ANX_ISOLATION_CELL_MAX; i++) {
		if (domain_reg[i].active &&
		    anx_uuid_compare(&domain_reg[i].cid, &cid) == 0) {
			enum anx_trust_domain d = domain_reg[i].domain;
			anx_spin_unlock_irqrestore(&iso_lock, flags);
			return d;
		}
	}

	anx_spin_unlock_irqrestore(&iso_lock, flags);
	return ANX_DOMAIN_TRUSTED;
}

/* ------------------------------------------------------------------ */
/* Access checks                                                        */
/* ------------------------------------------------------------------ */

static void
log_violation(anx_cid_t accessor, enum anx_trust_domain adom,
              anx_cid_t owner, enum anx_trust_domain odom,
              const char *resource_desc)
{
	struct anx_violation_event *v;
	uint32_t slot;

	if (vlog_count >= ANX_VIOLATION_LOG_MAX)
		slot = ANX_VIOLATION_LOG_MAX - 1;   /* overwrite last slot */
	else
		slot = vlog_count++;

	v = &vlog[slot];
	v->offender        = accessor;
	v->offender_domain = adom;
	v->owner           = owner;
	v->owner_domain    = odom;
	v->timestamp_ns    = arch_time_now();
	v->active          = true;

	if (resource_desc)
		anx_strlcpy(v->message, resource_desc, ANX_VIOLATION_MSG_MAX);
	else
		v->message[0] = '\0';
}

int
anx_isolation_check(const struct anx_ipc_policy *policy,
                    anx_cid_t accessor, anx_cid_t owner,
                    const char *resource_desc)
{
	enum anx_trust_domain adom, odom;
	bool flags;
	bool allowed;

	if (!policy)
		return ANX_EINVAL;

	adom = anx_isolation_get_domain(accessor);
	odom = anx_isolation_get_domain(owner);

	if (adom >= ANX_DOMAIN_COUNT || odom >= ANX_DOMAIN_COUNT)
		return ANX_EINVAL;

	allowed = policy->allow[adom][odom];

	if (!allowed) {
		anx_spin_lock_irqsave(&iso_lock, &flags);
		log_violation(accessor, adom, owner, odom, resource_desc);
		anx_spin_unlock_irqrestore(&iso_lock, flags);
		return ANX_EPERM;
	}

	return ANX_OK;
}

void
anx_isolation_set_policy(const struct anx_ipc_policy *policy)
{
	bool flags;

	if (!policy)
		return;

	anx_spin_lock_irqsave(&iso_lock, &flags);
	sys_policy = *policy;
	anx_spin_unlock_irqrestore(&iso_lock, flags);
}

void
anx_isolation_get_policy(struct anx_ipc_policy *out)
{
	bool flags;

	if (!out)
		return;

	anx_spin_lock_irqsave(&iso_lock, &flags);
	*out = sys_policy;
	anx_spin_unlock_irqrestore(&iso_lock, flags);
}

/* ------------------------------------------------------------------ */
/* Violation log                                                        */
/* ------------------------------------------------------------------ */

uint32_t
anx_isolation_violation_count(void)
{
	bool flags;
	uint32_t n;

	anx_spin_lock_irqsave(&iso_lock, &flags);
	n = vlog_count;
	anx_spin_unlock_irqrestore(&iso_lock, flags);
	return n;
}

int
anx_isolation_violation_log(struct anx_violation_event *out,
                             uint32_t max, uint32_t *count_out)
{
	uint32_t n, i;
	bool flags;

	if (!out || !count_out)
		return ANX_EINVAL;

	anx_spin_lock_irqsave(&iso_lock, &flags);

	n = (vlog_count < max) ? vlog_count : max;
	for (i = 0; i < n; i++)
		out[i] = vlog[i];
	*count_out = vlog_count;

	anx_spin_unlock_irqrestore(&iso_lock, flags);
	return ANX_OK;
}

void
anx_isolation_violation_reset(void)
{
	bool flags;

	anx_spin_lock_irqsave(&iso_lock, &flags);
	anx_memset(vlog, 0, sizeof(vlog));
	vlog_count = 0;
	anx_spin_unlock_irqrestore(&iso_lock, flags);
}

/* ------------------------------------------------------------------ */
/* Init                                                                 */
/* ------------------------------------------------------------------ */

void
anx_isolation_init(void)
{
	anx_spin_init(&iso_lock);
	anx_memset(domain_reg, 0, sizeof(domain_reg));
	anx_memset(&sys_policy, 0, sizeof(sys_policy));
	anx_memset(vlog, 0, sizeof(vlog));
	vlog_count = 0;
}
