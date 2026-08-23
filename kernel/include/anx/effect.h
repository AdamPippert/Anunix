/*
 * anx/effect.h — Protected Operation ABI: prepare/dispatch/settle
 * (RFC-0028).
 *
 * A three-phase state machine for any operation that crosses a trust
 * or process boundary (network, external process, credential-gated
 * device). A timeout during dispatch does not tell you whether the
 * external side effect happened — treating it as either success or
 * failure is a category error. This gives that ambiguity an explicit,
 * terminal state (ANX_EFFECT_UNKNOWN) instead of forcing a guess.
 *
 * anx_effect_prepare() is the enforcement point for both gates this
 * RFC introduces: CAN_CALL (the existing RFC-0003 execution-policy
 * check — does this cell's execution_policy actually permit side
 * effects) and CAN_SEND (anx_sink_check_send(), RFC-0028's new
 * information-flow check). Neither implies the other.
 */

#ifndef ANX_EFFECT_H
#define ANX_EFFECT_H

#include <anx/types.h>
#include <anx/capability.h>

enum anx_effect_phase {
	ANX_EFFECT_PREPARED,
	ANX_EFFECT_DISPATCHING,
	ANX_EFFECT_COMMITTED,
	ANX_EFFECT_RESTORED,
	ANX_EFFECT_UNKNOWN,	/* ambiguous outcome — terminal, never retried */
};

struct anx_pending_effect {
	anx_cid_t cell;
	struct anx_sink *sink;		/* NULL if this effect has no Sink (no data leaves) */
	anx_oid_t object_oid;		/* object whose data is flowing, if any; nil if none */
	enum anx_effect_phase phase;
};

/*
 * Prepare a pending effect for `cell` to send `object_oid` (may be a
 * nil OID if no object-backed data is involved) to `sink` (may be
 * NULL if this effect has no data-flow component — e.g. a pure
 * control operation). Revalidates:
 *   CAN_CALL — cell->execution.allow_side_effects must be true
 *   CAN_SEND — anx_sink_check_send(sink, object_oid), skipped if sink is NULL
 * On success, *out holds a heap-allocated ANX_EFFECT_PREPARED record;
 * the caller owns it and must resolve it via commit/restore/mark_unknown.
 * Returns:
 *   ANX_OK      prepared
 *   ANX_EINVAL  null cell/out
 *   ANX_ENOENT  cell does not resolve
 *   ANX_EPERM   CAN_CALL or CAN_SEND denied
 *   ANX_ENOMEM  allocation failure
 */
int anx_effect_prepare(anx_cid_t cell, struct anx_sink *sink,
		       const anx_oid_t *object_oid,
		       struct anx_pending_effect **out);

/* PREPARED -> DISPATCHING. Returns ANX_EINVAL if not in PREPARED. */
int anx_effect_mark_dispatching(struct anx_pending_effect *effect);

/*
 * DISPATCHING -> COMMITTED: the external effect is confirmed to have
 * happened. Returns ANX_EINVAL if not in DISPATCHING.
 */
int anx_effect_commit(struct anx_pending_effect *effect);

/*
 * DISPATCHING -> RESTORED: the caller certifies the operation never
 * began (e.g. the connection was refused before any bytes were sent).
 * The kernel cannot verify this itself — only call this when the
 * dispatch code has that certification. Returns ANX_EINVAL if not in
 * DISPATCHING.
 */
int anx_effect_restore(struct anx_pending_effect *effect);

/*
 * DISPATCHING -> UNKNOWN: the outcome is ambiguous (e.g. a fault or
 * timeout mid-dispatch with no certification either way). Terminal —
 * no further transition is valid from UNKNOWN, so nothing downstream
 * can silently retry or treat an unknown effect as success or failure.
 * Returns ANX_EINVAL if not in DISPATCHING.
 */
int anx_effect_mark_unknown(struct anx_pending_effect *effect);

/* Free a resolved (COMMITTED/RESTORED/UNKNOWN) pending effect record. */
void anx_effect_destroy(struct anx_pending_effect *effect);

#endif /* ANX_EFFECT_H */
