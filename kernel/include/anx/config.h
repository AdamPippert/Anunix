/*
 * anx/config.h — RLM-driven configuration over the world graph (RFC-0027).
 *
 * Configuration is not a new primitive. A setting is a node in the "config.*"
 * slice of a dedicated world graph (RFC-0026): its identity is the node oid, its
 * value is a node property, and every change is a patch that clears the commit
 * gate — so configuration is governed, versioned, and provenance-tracked for
 * free. A schema validator on the gate type-checks declared keys; the
 * configurator (anx_configurator_*) lets an RLM rollout propose evidence-backed
 * changes through the very same gate.
 */

#ifndef ANX_CONFIG_H
#define ANX_CONFIG_H

#include <anx/types.h>
#include <anx/worldgraph.h>

#define ANX_CONFIG_AREA_MAX	24
#define ANX_CONFIG_KEY_MAX	32
#define ANX_CONFIG_VAL_MAX	48
#define ANX_CONFIG_MAX_SCHEMA	32
#define ANX_CONFIG_SPEC_MAX	96

/* Declared value types. Undeclared keys are accepted as free strings. */
enum anx_config_type {
	ANX_CFG_STRING,
	ANX_CFG_INT,
	ANX_CFG_BOOL,
	ANX_CFG_ENUM,
};

/*
 * Initialize the configurator: register the "config.writer" provider (authority
 * over the config.* slice) and the config schema validator on the commit gate.
 * Idempotent.
 */
void anx_config_init(void);

/*
 * Declare a typed schema for (area, key). `spec` is the allowed comma-set for
 * ANX_CFG_ENUM and ignored otherwise. Declared keys are type-checked at the
 * commit gate. Returns ANX_OK, ANX_EFULL if the schema table is full, or
 * ANX_EINVAL.
 */
int anx_config_declare(const char *area, const char *key,
		       enum anx_config_type type, const char *spec);

/*
 * Set (area, key) = value through the world-graph commit gate: governed,
 * versioned, provenance-tracked. Returns ANX_OK on commit, or the gate's
 * rejection code (e.g. ANX_EINVAL when a declared type check fails).
 */
int anx_config_set(const char *area, const char *key, const char *value);

/*
 * As anx_config_set, but fills `report` (may be NULL) with the commit-gate
 * outcome so a caller can surface the rejection reason.
 */
int anx_config_try(const char *area, const char *key, const char *value,
		   struct anx_world_commit_report *report);

/* Read (area, key) into `out`. Returns ANX_OK or ANX_ENOENT. */
int anx_config_get(const char *area, const char *key, char *out, size_t len);

/*
 * Visit every config entry, or only those in `area` if non-NULL. `cb` returns
 * non-zero to stop, which is propagated. Returns ANX_OK or the stop value.
 */
typedef int (*anx_config_iter_fn)(const char *area, const char *key,
				  const char *value, void *arg);
int anx_config_list(const char *area, anx_config_iter_fn cb, void *arg);

/* The dedicated config world graph (lazily created). NULL on alloc failure. */
struct anx_world_graph *anx_config_graph(void);

/* --- RLM configurator (RFC-0027 Section 5) --- */

/*
 * Apply a text proposal as one atomic, gated patch. Each line of the form
 *   SET <area> <key> <value...>
 * becomes an add-or-update; all other lines are ignored (models emit prose).
 * The whole patch commits or none of it does. Authored by "config.rlm".
 * `report` (may be NULL) carries the gate outcome. Returns the commit result.
 */
int anx_configurator_apply(const char *proposal,
			   struct anx_world_commit_report *report);

/*
 * Run an RLM rollout on `goal`, then apply its final response as a proposal.
 * The rollout uses the installed inference adapter (the model client in
 * production, a test double under test). Returns the commit result, or a
 * negative inference error if the rollout did not complete.
 */
int anx_configurator_run(const char *goal,
			 struct anx_world_commit_report *report);

#endif /* ANX_CONFIG_H */
