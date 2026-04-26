/*
 * anx/ibal.h — IBAL high-level API (RFC-0020 Phase 9+).
 *
 * Provides state persistence for the Energy-Based Model scorer weights
 * (save/load via the state object store) and a convenience "run to
 * completion" wrapper over the low-level loop session API.
 */

#ifndef ANX_IBAL_H
#define ANX_IBAL_H

#include <anx/types.h>
#include <anx/loop.h>

/* Maximum number of EBM scorer weight slots persisted per state snapshot. */
#define ANX_EBM_MAX_SCORERS  8

/* ------------------------------------------------------------------ */
/* Phase 9: EBM weight persistence                                     */
/* ------------------------------------------------------------------ */

/*
 * Serialize current EBM scorer weights plus per-action IBAL statistics
 * into an ANX_OBJ_MEMORY_CONSOLIDATION object and seal it.
 * Writes the new object's OID to *oid_out.
 * The object is retained in the store; the caller does not hold a
 * reference and must not call anx_objstore_release() on it.
 */
int anx_ibal_save_state(anx_oid_t *oid_out);

/*
 * Reload EBM scorer weights from a previously saved state object.
 * Returns ANX_OK on success, ANX_EINVAL if the magic/version is wrong,
 * or ANX_ENOENT if the OID is not found.
 */
int anx_ibal_load_state(anx_oid_t state_oid);

/* ------------------------------------------------------------------ */
/* Convenience run API                                                 */
/* ------------------------------------------------------------------ */

/*
 * Create a loop session from *params, advance it until it reaches a
 * terminal state (HALTED, COMMITTED, or ABORTED), then write the
 * session OID to *session_oid_out.
 * Returns ANX_OK on success.  The session is NOT automatically
 * committed; the caller should inspect status and call
 * anx_loop_session_commit() when desired.
 */
int anx_ibal_run(const struct anx_loop_create_params *params,
		 anx_oid_t *session_oid_out);

#endif /* ANX_IBAL_H */
