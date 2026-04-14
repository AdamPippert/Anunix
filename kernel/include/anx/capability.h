/*
 * anx/capability.h — Capability Objects (RFC-0007).
 *
 * A Capability Object encodes validated operational competence
 * the system has acquired. Once installed, it participates in
 * routing as a first-class engine class.
 *
 * This is a stub for the initial kernel — basic lifecycle and
 * store, no procedure interpretation or network distribution yet.
 */

#ifndef ANX_CAPABILITY_H
#define ANX_CAPABILITY_H

#include <anx/types.h>
#include <anx/list.h>
#include <anx/spinlock.h>

/* --- Capability lifecycle states (RFC-0007 Section 8) --- */

enum anx_cap_status {
	ANX_CAP_DRAFT,
	ANX_CAP_VALIDATING,
	ANX_CAP_VALIDATED,
	ANX_CAP_INSTALLED,
	ANX_CAP_SUSPENDED,
	ANX_CAP_SUPERSEDED,
	ANX_CAP_RETIRED,
	ANX_CAP_STATUS_COUNT,
};

/* --- Capability struct --- */

struct anx_capability {
	/* Identity */
	anx_oid_t cap_oid;		/* underlying State Object OID */
	char name[128];
	char version[32];
	enum anx_cap_status status;

	/* Contracts */
	uint32_t input_cap_mask;	/* required input capabilities */
	uint32_t output_cap_mask;	/* declared output capabilities */

	/* Dependencies */
	anx_eid_t required_engines[8];
	uint32_t required_engine_count;

	/* Supersession */
	anx_oid_t supersedes_oid;	/* capability this supersedes */

	/* Trust */
	uint32_t validation_score;	/* 0-100 */
	uint32_t invocation_count;
	uint32_t success_count;

	/* Installed engine reference (set when installed) */
	anx_eid_t installed_engine_id;

	/* Bookkeeping */
	struct anx_spinlock lock;
	struct anx_list_head store_link;
};

/* --- Capability Store API --- */

/* Initialize the capability store */
void anx_cap_store_init(void);

/* Create a new capability (starts in DRAFT state) */
int anx_cap_create(const char *name, const char *version,
		   struct anx_capability **out);

/* Look up a capability by OID */
struct anx_capability *anx_cap_lookup(const anx_oid_t *oid);

/* Transition capability lifecycle state */
int anx_cap_transition(struct anx_capability *cap,
		       enum anx_cap_status new_status);

/* Install a validated capability into the engine registry */
int anx_cap_install(struct anx_capability *cap);

/* Uninstall a capability from the engine registry */
int anx_cap_uninstall(struct anx_capability *cap);

/* Record an invocation result */
void anx_cap_record_invocation(struct anx_capability *cap, bool success);

#endif /* ANX_CAPABILITY_H */
