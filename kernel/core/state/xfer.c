/*
 * xfer.c — Transfer/import/export policy and session (P1-005).
 */

#include <anx/xfer.h>
#include <anx/string.h>
#include <anx/types.h>

static bool alpha(char c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static bool digit(char c) { return c >= '0' && c <= '9'; }

static bool bounded_text(const char *text, uint32_t capacity, uint32_t *length)
{
	if (!text) return false;
	for (uint32_t i = 0; i < capacity; i++)
		if (!text[i]) { if (length) *length = i; return true; }
	return false;
}

/* Canonical native namespace URIs only: no decoding or backend normalization. */
static bool valid_uri(const char *uri, uint32_t capacity, bool prefix, uint32_t *length)
{
	uint32_t n, i, start;
	uint32_t port = 0, port_digits = 0;
	bool in_port = false, host_character = false;
	if (!bounded_text(uri, capacity, &n) || !n || !alpha(uri[0])) return false;
	for (i = 1; i < n && uri[i] != ':'; i++)
		if (!alpha(uri[i]) && !digit(uri[i]) && uri[i] != '+' && uri[i] != '-' && uri[i] != '.') return false;
	if (i + 2 >= n || uri[i] != ':' || uri[i + 1] != '/' || uri[i + 2] != '/') return false;
	i += 3;
	if (i == n) { if (length) *length = n; return prefix; }
	start = i;
	while (i < n && uri[i] != '/') {
		char c = uri[i++];
		if (c == ':') {
			if (in_port || !host_character) return false;
			in_port = true;
		} else if (in_port) {
			if (!digit(c) || port > 6553) return false;
			port = port * 10 + (uint32_t)(c - '0');
			if (port > 65535) return false;
			port_digits++;
		} else {
			if (!alpha(c) && !digit(c) && c != '.' && c != '-' && c != '_') return false;
			if (alpha(c) || digit(c)) host_character = true;
		}
	}
	if (i == start || !host_character || (in_port && (!port_digits || !port))) return false;
	if (i < n) {
		start = ++i;
		for (; i <= n; i++) {
			if (i == n || uri[i] == '/') {
				uint32_t size = i - start;
				if ((!size && i != n) || (size == 1 && uri[start] == '.') ||
				    (size == 2 && uri[start] == '.' && uri[start + 1] == '.')) return false;
				start = i + 1;
			} else {
				char c = uri[i];
				if (!alpha(c) && !digit(c) && c != '.' && c != '-' && c != '_' && c != '~') return false;
			}
		}
	}
	if (length) *length = n;
	return true;
}

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
	if (!p || !dest_prefix || p->allowed_count > ANX_XFER_POLICY_RULES_MAX ||
	    !valid_uri(dest_prefix, ANX_XFER_POLICY_URI_MAX, true, NULL))
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

	if (!p || !dest_uri || p->allowed_count > ANX_XFER_POLICY_RULES_MAX ||
	    !valid_uri(dest_uri, ANX_XFER_DST_MAX, false, NULL))
		return ANX_EINVAL;
	/* A malformed active rule invalidates the complete policy before any allow. */
	for (i = 0; i < p->allowed_count; i++)
		if (p->allowed[i].active && !valid_uri(p->allowed[i].prefix, ANX_XFER_POLICY_URI_MAX, true, NULL))
			return ANX_EINVAL;

	for (i = 0; i < p->allowed_count; i++) {
		if (!p->allowed[i].active)
			continue;
		plen = (uint32_t)anx_strlen(p->allowed[i].prefix);
		if (anx_strncmp(dest_uri, p->allowed[i].prefix, plen) == 0 &&
		    (p->allowed[i].prefix[plen - 1] == '/' || !dest_uri[plen] || dest_uri[plen] == '/'))
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
	if (!valid_uri(src_uri, ANX_XFER_SRC_MAX, false, NULL) ||
	    (provenance_tag && !bounded_text(provenance_tag, ANX_XFER_TAG_MAX, NULL)))
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
	int rc;
	if (!sess || !data)
		return ANX_EINVAL;
	if (sess->state != ANX_XFER_ACTIVE)
		return ANX_EINVAL;
	rc = anx_xfer_policy_check(sess->policy, sess->dest_uri);
	if (rc != ANX_OK) { sess->state = ANX_XFER_ABORTED; return rc; }
	/* SHA-256 encodes the message length in 64 bits, measured in bits. */
	if (sess->bytes_written > (~(uint64_t)0 / 8) - len) {
		sess->state = ANX_XFER_ABORTED;
		return ANX_EFULL;
	}
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
	int rc;
	if (result_out) anx_memset(result_out, 0, sizeof(*result_out));
	if (!sess || !result_out)
		return ANX_EINVAL;
	if (sess->state != ANX_XFER_ACTIVE)
		return ANX_EINVAL;
	rc = anx_xfer_policy_check(sess->policy, sess->dest_uri);
	if (rc != ANX_OK) { sess->state = ANX_XFER_ABORTED; return rc; }

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
