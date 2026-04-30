/*
 * anx/cexl.h — CounterEXample Learning (CEXL) critic-loop (RFC-0020 Phase 12).
 *
 * anx_loop_cexl_process() scans all ANX_OBJ_COUNTEREXAMPLE objects belonging
 * to a session, derives per-action rejection statistics, and feeds them into
 * the PAL cross-session accumulator via anx_pal_memory_update().
 *
 * Caller must hold no objstore locks when calling this function.
 */

#ifndef ANX_CEXL_H
#define ANX_CEXL_H

#include <anx/types.h>

/*
 * Process counterexamples from session_oid for world_uri.
 *
 * Returns ANX_OK on success.
 * Returns ANX_ENOENT if no counterexamples are found for the session
 *   (not an error — just means no rejections occurred).
 * Returns ANX_EINVAL on bad arguments.
 */
int anx_loop_cexl_process(anx_oid_t session_oid, const char *world_uri);

#endif /* ANX_CEXL_H */
