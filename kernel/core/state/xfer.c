/*
 * xfer.c — Transfer/import/export policy and session (P1-005).
 */

#include <anx/xfer.h>
#include <anx/string.h>
#include <anx/types.h>

/* ------------------------------------------------------------------ */
/* Policy                                                               */
/* ------------------------------------------------------------------ */

void
anx_xfer_policy_init(struct anx_xfer_policy *p,
                      const char *name, uint32_t flags)
{
	if (!p)
		return;
	anx_memset(p, 0, sizeof(*p));
	if (name)
		anx_strlcpy(p->name, name, sizeof(p->name));
	p->flags = flags;
}

int
anx_xfer_policy_allow(struct anx_xfer_policy *p, const char *dest_prefix)
{
	if (!p || !dest_prefix)
		return ANX_EINVAL;
	if (p->allowed_count >= ANX_XFER_POLICY_RULES_MAX)
		return ANX_EFULL;

	anx_strlcpy(p->allowed[p->allowed_count].prefix, dest_prefix,
	            sizeof(p->allowed[p->allowed_count].prefix));
	p->allowed[p->allowed_count].active = true;
	p->allowed_count++;
	return ANX_OK;
}

int
anx_xfer_policy_check(const struct anx_xfer_policy *p, const char *dest_uri)
{
	uint32_t i, plen;

	if (!p || !dest_uri)
		return ANX_EINVAL;

	for (i = 0; i < p->allowed_count; i++) {
		if (!p->allowed[i].active)
			continue;
		plen = (uint32_t)anx_strlen(p->allowed[i].prefix);
		if (anx_strncmp(dest_uri, p->allowed[i].prefix, plen) == 0)
			return ANX_OK;
	}
	return ANX_EPERM;
}

/* ------------------------------------------------------------------ */
/* Session                                                              */
/* ------------------------------------------------------------------ */

int
anx_xfer_begin(const struct anx_xfer_policy *policy,
                const char *src_uri, const char *dest_uri,
                const char *provenance_tag,
                struct anx_xfer_session *sess)
{
	int rc;

	if (!policy || !src_uri || !dest_uri || !sess)
		return ANX_EINVAL;

	rc = anx_xfer_policy_check(policy, dest_uri);
	if (rc != ANX_OK)
		return rc;

	anx_memset(sess, 0, sizeof(*sess));
	sess->policy = policy;
	anx_strlcpy(sess->src_uri,  src_uri,  sizeof(sess->src_uri));
	anx_strlcpy(sess->dest_uri, dest_uri, sizeof(sess->dest_uri));
	if (provenance_tag)
		anx_strlcpy(sess->provenance_tag, provenance_tag,
		            sizeof(sess->provenance_tag));
	anx_sha256_init(&sess->hash_ctx);
	sess->bytes_written  = 0;
	sess->resume_offset  = 0;
	sess->state          = ANX_XFER_ACTIVE;
	return ANX_OK;
}

int
anx_xfer_write(struct anx_xfer_session *sess,
                const void *data, uint32_t len)
{
	if (!sess || !data)
		return ANX_EINVAL;
	if (sess->state != ANX_XFER_ACTIVE)
		return ANX_EINVAL;
	if (len == 0)
		return ANX_OK;

	anx_sha256_update(&sess->hash_ctx, data, len);
	sess->bytes_written += len;
	return ANX_OK;
}

int
anx_xfer_interrupt(struct anx_xfer_session *sess)
{
	if (!sess)
		return ANX_EINVAL;
	if (sess->state != ANX_XFER_ACTIVE)
		return ANX_EINVAL;

	sess->resume_offset = sess->bytes_written;
	sess->state         = ANX_XFER_INTERRUPTED;
	return ANX_OK;
}

int
anx_xfer_resume(struct anx_xfer_session *sess)
{
	int rc;

	if (!sess)
		return ANX_EINVAL;
	if (sess->state != ANX_XFER_INTERRUPTED)
		return ANX_EINVAL;

	/* Re-validate destination against policy. */
	rc = anx_xfer_policy_check(sess->policy, sess->dest_uri);
	if (rc != ANX_OK) {
		sess->state = ANX_XFER_ABORTED;
		return rc;
	}

	sess->state = ANX_XFER_ACTIVE;
	return ANX_OK;
}

int
anx_xfer_commit(struct anx_xfer_session *sess,
                 struct anx_xfer_result *result_out)
{
	if (!sess || !result_out)
		return ANX_EINVAL;
	if (sess->state != ANX_XFER_ACTIVE)
		return ANX_EINVAL;

	anx_sha256_final(&sess->hash_ctx, result_out->final_hash.bytes);
	result_out->hash_valid         = true;
	result_out->bytes_transferred  = sess->bytes_written;
	anx_strlcpy(result_out->provenance_tag, sess->provenance_tag,
	            sizeof(result_out->provenance_tag));
	anx_strlcpy(result_out->dest_uri, sess->dest_uri,
	            sizeof(result_out->dest_uri));
	sess->state = ANX_XFER_COMMITTED;
	return ANX_OK;
}

void
anx_xfer_reset(struct anx_xfer_session *sess)
{
	if (!sess)
		return;
	anx_memset(sess, 0, sizeof(*sess));
	sess->state = ANX_XFER_IDLE;
}
