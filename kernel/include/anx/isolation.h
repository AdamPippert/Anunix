/*
 * anx/isolation.h — Untrusted application/process isolation (P2-003).
 *
 * Provides trust domain assignment for execution cells, cross-domain
 * access policy enforcement, and violation event logging.
 */

#ifndef ANX_ISOLATION_H
#define ANX_ISOLATION_H

#include <anx/types.h>

/* ------------------------------------------------------------------ */
/* Trust domains                                                        */
/* ------------------------------------------------------------------ */

enum anx_trust_domain {
	ANX_DOMAIN_TRUSTED    = 0,   /* kernel or fully-trusted cell      */
	ANX_DOMAIN_RESTRICTED = 1,   /* trusted but limited capabilities  */
	ANX_DOMAIN_SANDBOXED  = 2,   /* isolated; no cross-domain SHM     */
	ANX_DOMAIN_UNTRUSTED  = 3,   /* content/renderer processes        */
	ANX_DOMAIN_COUNT,
};

/* ------------------------------------------------------------------ */
/* Cross-domain IPC policy                                              */
/* ------------------------------------------------------------------ */

/*
 * Policy matrix: entry [src][dst] = true means the source domain is
 * allowed to access resources owned by the destination domain.
 */
struct anx_ipc_policy {
	bool allow[ANX_DOMAIN_COUNT][ANX_DOMAIN_COUNT];
};

/* ------------------------------------------------------------------ */
/* Violation log                                                        */
/* ------------------------------------------------------------------ */

#define ANX_VIOLATION_MSG_MAX   128u
#define ANX_VIOLATION_LOG_MAX    16u

struct anx_violation_event {
	anx_cid_t           offender;
	enum anx_trust_domain offender_domain;
	anx_cid_t           owner;
	enum anx_trust_domain owner_domain;
	char                message[ANX_VIOLATION_MSG_MAX];
	uint64_t            timestamp_ns;
	bool                active;
};

/* ------------------------------------------------------------------ */
/* Domain registry                                                      */
/* ------------------------------------------------------------------ */

#define ANX_ISOLATION_CELL_MAX  64u

/* Assign a trust domain to a cell. */
int anx_isolation_set_domain(anx_cid_t cid, enum anx_trust_domain domain);

/* Look up the domain for a cell (default: TRUSTED). */
enum anx_trust_domain anx_isolation_get_domain(anx_cid_t cid);

/* ------------------------------------------------------------------ */
/* Access checks                                                        */
/* ------------------------------------------------------------------ */

/*
 * Check whether accessor (in its domain) may access a resource owned
 * by owner (in its domain), under the given policy.
 * Returns ANX_OK if allowed, ANX_EPERM if denied (and logs violation).
 */
int anx_isolation_check(const struct anx_ipc_policy *policy,
                          anx_cid_t accessor, anx_cid_t owner,
                          const char *resource_desc);

/* Set the default system-wide IPC policy. */
void anx_isolation_set_policy(const struct anx_ipc_policy *policy);

/* Read the current system-wide IPC policy. */
void anx_isolation_get_policy(struct anx_ipc_policy *out);

/* ------------------------------------------------------------------ */
/* Violation log                                                        */
/* ------------------------------------------------------------------ */

/* Return number of violation events recorded since last reset. */
uint32_t anx_isolation_violation_count(void);

/* Retrieve violation events (up to max); fills count_out. */
int anx_isolation_violation_log(struct anx_violation_event *out,
                                  uint32_t max, uint32_t *count_out);

/* Clear violation log. */
void anx_isolation_violation_reset(void);

/* Initialise isolation subsystem. */
void anx_isolation_init(void);

#endif /* ANX_ISOLATION_H */
