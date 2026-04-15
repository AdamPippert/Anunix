/*
 * anx/engine_lease.h — Resource leasing for model engines.
 *
 * A lease represents memory and accelerator reservations held
 * by an engine while it is loaded and serving. The kernel tracks
 * total resource usage to enforce capacity limits.
 */

#ifndef ANX_ENGINE_LEASE_H
#define ANX_ENGINE_LEASE_H

#include <anx/types.h>
#include <anx/memplane.h>
#include <anx/list.h>
#include <anx/spinlock.h>

/* --- Accelerator types --- */

enum anx_accel_type {
	ANX_ACCEL_NONE,
	ANX_ACCEL_GPU,
	ANX_ACCEL_NPU,
	ANX_ACCEL_COUNT,
};

/* --- Resource lease --- */

struct anx_engine_lease {
	anx_eid_t engine_id;

	/* Memory reservation */
	enum anx_mem_tier mem_tier;
	uint64_t mem_reserved_bytes;
	uint64_t mem_used_bytes;

	/* Accelerator reservation */
	enum anx_accel_type accel;
	uint32_t accel_pct;		/* percentage of accelerator (0-100) */

	/* Lifetime */
	anx_time_t granted_at;
	anx_time_t expires_at;		/* 0 = no expiry */

	/* Bookkeeping */
	struct anx_spinlock lock;
	struct anx_list_head lease_link;
};

/* --- Lease API --- */

/* Initialize the lease subsystem */
void anx_lease_init(void);

/* Grant a resource lease to an engine */
int anx_lease_grant(const anx_eid_t *engine_id,
		    enum anx_mem_tier tier,
		    uint64_t mem_bytes,
		    enum anx_accel_type accel,
		    uint32_t accel_pct,
		    struct anx_engine_lease **out);

/* Look up the lease for an engine */
struct anx_engine_lease *anx_lease_lookup(const anx_eid_t *engine_id);

/* Release a lease (frees reservations) */
int anx_lease_release(struct anx_engine_lease *lease);

/* Query total available resources */
int anx_lease_avail_mem(enum anx_mem_tier tier, uint64_t *avail_out);
int anx_lease_avail_accel(enum anx_accel_type accel, uint32_t *pct_out);

#endif /* ANX_ENGINE_LEASE_H */
