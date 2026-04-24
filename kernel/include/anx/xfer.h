/*
 * anx/xfer.h — Transfer/import/export policy and session (P1-005).
 *
 * Provides destination policy enforcement, resume/retry primitives,
 * and metadata/provenance recording for artifact movement.  All state
 * is caller-owned (stack or pool); no internal allocation.
 */

#ifndef ANX_XFER_H
#define ANX_XFER_H

#include <anx/types.h>
#include <anx/crypto.h>

/* ------------------------------------------------------------------ */
/* Constants                                                            */
/* ------------------------------------------------------------------ */

#define ANX_XFER_POLICY_NAME_MAX   64u
#define ANX_XFER_POLICY_URI_MAX   128u
#define ANX_XFER_POLICY_RULES_MAX   8u
#define ANX_XFER_SRC_MAX          128u
#define ANX_XFER_DST_MAX          128u
#define ANX_XFER_TAG_MAX           64u

/* ------------------------------------------------------------------ */
/* Transfer policy                                                      */
/* ------------------------------------------------------------------ */

struct anx_xfer_allowed_dest {
	char prefix[ANX_XFER_POLICY_URI_MAX];
	bool active;
};

struct anx_xfer_policy {
	char name[ANX_XFER_POLICY_NAME_MAX];
	uint32_t flags;
	struct anx_xfer_allowed_dest allowed[ANX_XFER_POLICY_RULES_MAX];
	uint32_t allowed_count;
};

/* Initialise a policy struct. */
void anx_xfer_policy_init(struct anx_xfer_policy *p,
                            const char *name, uint32_t flags);

/* Add a destination URI prefix to the policy allow-list. */
int anx_xfer_policy_allow(struct anx_xfer_policy *p,
                            const char *dest_prefix);

/* Check dest_uri against the policy.
 * Returns ANX_OK if permitted, ANX_EPERM if rejected. */
int anx_xfer_policy_check(const struct anx_xfer_policy *p,
                            const char *dest_uri);

/* ------------------------------------------------------------------ */
/* Transfer session                                                     */
/* ------------------------------------------------------------------ */

enum anx_xfer_state {
	ANX_XFER_IDLE,
	ANX_XFER_ACTIVE,
	ANX_XFER_INTERRUPTED,
	ANX_XFER_COMMITTED,
	ANX_XFER_ABORTED,
};

struct anx_xfer_session {
	enum anx_xfer_state           state;
	char                          src_uri[ANX_XFER_SRC_MAX];
	char                          dest_uri[ANX_XFER_DST_MAX];
	const struct anx_xfer_policy *policy;

	/* Running SHA-256 over all written bytes. */
	struct anx_sha256_ctx         hash_ctx;

	uint64_t                      bytes_written;
	uint64_t                      resume_offset;  /* snapshot at interrupt */

	char                          provenance_tag[ANX_XFER_TAG_MAX];
};

/* ------------------------------------------------------------------ */
/* Transfer result                                                      */
/* ------------------------------------------------------------------ */

struct anx_xfer_result {
	uint64_t     bytes_transferred;
	struct anx_hash final_hash;      /* SHA-256 of all written bytes */
	bool         hash_valid;
	char         provenance_tag[ANX_XFER_TAG_MAX];
	char         dest_uri[ANX_XFER_DST_MAX];
};

/* ------------------------------------------------------------------ */
/* Session API                                                          */
/* ------------------------------------------------------------------ */

/* Begin a new transfer. Validates dest_uri against policy.
 * Caller provides the session struct (no allocation). */
int anx_xfer_begin(const struct anx_xfer_policy *policy,
                    const char *src_uri, const char *dest_uri,
                    const char *provenance_tag,
                    struct anx_xfer_session *sess);

/* Write a data chunk; updates hash and byte count. */
int anx_xfer_write(struct anx_xfer_session *sess,
                    const void *data, uint32_t len);

/* Pause the transfer; saves resume_offset = bytes_written. */
int anx_xfer_interrupt(struct anx_xfer_session *sess);

/* Resume an interrupted transfer; re-validates policy. */
int anx_xfer_resume(struct anx_xfer_session *sess);

/* Finalise: compute hash, fill result_out, mark COMMITTED. */
int anx_xfer_commit(struct anx_xfer_session *sess,
                     struct anx_xfer_result *result_out);

/* Reset session to IDLE. */
void anx_xfer_reset(struct anx_xfer_session *sess);

#endif /* ANX_XFER_H */
