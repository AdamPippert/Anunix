/*
 * anx/provenance.h — Append-only provenance log for State Objects.
 */

#ifndef ANX_PROVENANCE_H
#define ANX_PROVENANCE_H

#include <anx/types.h>

/* Provenance event types */
enum anx_prov_type {
	ANX_PROV_CREATED,
	ANX_PROV_MUTATED,
	ANX_PROV_DERIVED_FROM,
	ANX_PROV_SEALED,
	ANX_PROV_POLICY_CHANGED,
	ANX_PROV_ACCESSED,
	ANX_PROV_MIGRATED,
};

/* A single provenance event */
struct anx_prov_event {
	uint64_t event_id;		/* monotonic within this object */
	anx_time_t timestamp;
	enum anx_prov_type event_type;
	anx_cid_t actor_cell;		/* cell that caused this event */
	anx_oid_t *input_oids;		/* objects consumed (DERIVED_FROM) */
	uint32_t input_count;
	char description[128];
	bool reproducible;
};

/* Append-only provenance log */
struct anx_prov_log {
	struct anx_prov_event *events;
	uint32_t count;
	uint32_t capacity;
};

/* Create an empty provenance log */
struct anx_prov_log *anx_prov_log_create(void);

/* Append an event. Returns 0 on success. */
int anx_prov_log_append(struct anx_prov_log *log,
			const struct anx_prov_event *event);

/* Get event by index */
const struct anx_prov_event *anx_prov_log_get(const struct anx_prov_log *log,
					      uint32_t index);

/* Get event count */
uint32_t anx_prov_log_count(const struct anx_prov_log *log);

/* Destroy a provenance log */
void anx_prov_log_destroy(struct anx_prov_log *log);

#endif /* ANX_PROVENANCE_H */
