/*
 * pii_filter.c — PII detection and redaction for agent-driven sessions.
 *
 * Two backends share a single interface:
 *   PII_BACKEND_NATIVE  — anx_model_call() through kernel model routing
 *   PII_BACKEND_HTTP    — OpenAI-compatible HTTP POST (LiteLLM, etc.)
 *
 * The model receives a structured prompt requesting JSON output:
 *   {"found": bool, "redacted": "...", "types": "email, phone, ..."}
 *
 * On model failure, behavior is controlled by cfg.fail_open:
 *   true  (default) — pass content through with a logged warning
 *   false           — block content delivery
 */

#include "pii_filter.h"
#include <anx/alloc.h>
#include <anx/string.h>
#include <anx/kprintf.h>
#include <anx/json.h>
#include <anx/http.h>
#include <anx/model_client.h>

/* ── System prompt ───────────────────────────────────────────────── */

#define PII_SYSTEM_PROMPT					\
	"You are a PII detection and redaction system. "	\
	"Given text, detect and redact all personally "		\
	"identifiable information (PII). Replace each instance "	\
	"inline with a typed placeholder: [REDACTED_EMAIL], "	\
	"[REDACTED_PHONE], [REDACTED_SSN], "			\
	"[REDACTED_CREDIT_CARD], [REDACTED_NAME], "		\
	"[REDACTED_ADDRESS], [REDACTED_DOB], "			\
	"[REDACTED_IP], [REDACTED_PASSPORT]. "			\
	"Respond ONLY with a JSON object — no markdown, "	\
	"no explanation — with exactly these keys: "		\
	"\"found\" (boolean, true if any PII was detected), "	\
	"\"redacted\" (full text with PII replaced), "		\
	"\"types\" (comma-separated string of PII "		\
	"categories found, empty string if none)."

/* ── Module state ────────────────────────────────────────────────── */

static struct pii_config s_cfg;
static bool              s_ready;

/* ── Initialization ──────────────────────────────────────────────── */

int anx_pii_init(const struct pii_config *cfg)
{
	if (cfg) {
		s_cfg = *cfg;
	} else {
		anx_memset(&s_cfg, 0, sizeof(s_cfg));
		s_cfg.backend    = PII_BACKEND_NATIVE;
		s_cfg.enabled    = anx_model_client_ready();
		s_cfg.fail_open  = true;
		s_cfg.timeout_ms = PII_TIMEOUT_MS;
		anx_strlcpy(s_cfg.model_name, "openai/privacy-filter",
			    sizeof(s_cfg.model_name));
	}
	s_ready = true;
	kprintf("pii: filter %s (backend=%s, model=%s)\n",
		s_cfg.enabled ? "enabled" : "disabled",
		s_cfg.backend == PII_BACKEND_NATIVE ? "native" : "http",
		s_cfg.model_name);
	return 0;
}

bool anx_pii_ready(void)
{
	return s_ready && s_cfg.enabled;
}

/* ── JSON response parser ────────────────────────────────────────── */

static int parse_pii_json(const char *text, uint32_t len,
			   struct pii_result *out)
{
	struct anx_json_value  root;
	struct anx_json_value *v;
	const char            *s;
	size_t                 slen;

	if (anx_json_parse(text, len, &root) != 0)
		return -1;

	v = anx_json_get(&root, "found");
	out->found = v ? anx_json_bool(v) : false;

	v = anx_json_get(&root, "types");
	if (v && v->type == ANX_JSON_STRING)
		anx_strlcpy(out->types, anx_json_string(v), PII_TYPES_LEN);
	else
		out->types[0] = '\0';

	v = anx_json_get(&root, "redacted");
	s = v ? anx_json_string(v) : NULL;
	if (s) {
		slen = anx_strlen(s);
		out->redacted = anx_alloc(slen + 1);
		if (out->redacted)
			anx_memcpy(out->redacted, s, slen + 1);
	}

	anx_json_free(&root);
	return 0;
}

/* ── JSON string escaping ────────────────────────────────────────── */

static size_t json_escape(const char *src, size_t src_len,
			   char *dst, size_t dst_cap)
{
	size_t d = 0;
	size_t i;
	unsigned char c;

	for (i = 0; i < src_len && d + 4 < dst_cap; i++) {
		c = (unsigned char)src[i];
		if (c == '"') {
			dst[d++] = '\\'; dst[d++] = '"';
		} else if (c == '\\') {
			dst[d++] = '\\'; dst[d++] = '\\';
		} else if (c == '\n') {
			dst[d++] = '\\'; dst[d++] = 'n';
		} else if (c == '\r') {
			dst[d++] = '\\'; dst[d++] = 'r';
		} else if (c == '\t') {
			dst[d++] = '\\'; dst[d++] = 't';
		} else if (c < 0x20) {
			/* control character → \u00XX */
			static const char hex[] = "0123456789abcdef";
			dst[d++] = '\\'; dst[d++] = 'u';
			dst[d++] = '0';  dst[d++] = '0';
			dst[d++] = hex[(c >> 4) & 0xF];
			dst[d++] = hex[c & 0xF];
		} else {
			dst[d++] = (char)c;
		}
	}
	if (d < dst_cap) dst[d] = '\0';
	return d;
}

/* ── Anunix native backend ───────────────────────────────────────── */

static int call_native(const char *content, size_t len,
		        struct pii_result *out)
{
	struct anx_model_request  req;
	struct anx_model_response resp;
	int rc;

	/* Truncate if over limit */
	if (len > PII_MAX_CONTENT) len = PII_MAX_CONTENT;

	/* Content must be NUL-terminated for model_call */
	char *buf = anx_alloc(len + 1);
	if (!buf) return -1;
	anx_memcpy(buf, content, len);
	buf[len] = '\0';

	anx_memset(&req,  0, sizeof(req));
	anx_memset(&resp, 0, sizeof(resp));
	req.model        = s_cfg.model_name;
	req.system       = PII_SYSTEM_PROMPT;
	req.user_message = buf;
	req.max_tokens   = 4096;

	rc = anx_model_call(&req, &resp);
	anx_free(buf);

	if (rc != 0 || !resp.content) {
		anx_model_response_free(&resp);
		return -1;
	}

	rc = parse_pii_json(resp.content, resp.content_len, out);
	anx_model_response_free(&resp);
	return rc;
}

/* ── HTTP (OpenAI-compatible) backend ────────────────────────────── */

static int call_http(const char *content, size_t len,
		     struct pii_result *out)
{
	struct anx_http_response http_resp;
	char *body;
	char  auth_hdr[PII_MAX_APIKEY + 32];
	char *inner;
	size_t body_cap;
	size_t pos;
	int    rc;

	/* Allocate: escaped content (2× worst case) + prompt + JSON overhead */
	body_cap = len * 2 + anx_strlen(PII_SYSTEM_PROMPT) * 2 + 512;
	if (body_cap > PII_MAX_CONTENT * 3) body_cap = PII_MAX_CONTENT * 3;
	body = anx_alloc(body_cap);
	if (!body) return -1;

	/* Build OpenAI chat completions request */
	pos = 0;
#define APP(s) do { \
	size_t _l = anx_strlen(s); \
	if (pos + _l < body_cap) { \
		anx_memcpy(body + pos, (s), _l); pos += _l; \
	} \
} while (0)

	APP("{\"model\":\"");
	APP(s_cfg.model_name);
	APP("\",\"messages\":[{\"role\":\"system\",\"content\":\"");
	pos += json_escape(PII_SYSTEM_PROMPT,
			   anx_strlen(PII_SYSTEM_PROMPT),
			   body + pos, body_cap - pos);
	APP("\"},{\"role\":\"user\",\"content\":\"");
	{
		size_t used = len > PII_MAX_CONTENT ? PII_MAX_CONTENT : len;
		pos += json_escape(content, used, body + pos, body_cap - pos);
	}
	APP("\"}],\"max_tokens\":4096,"
	    "\"response_format\":{\"type\":\"json_object\"}}");
	if (pos < body_cap) body[pos] = '\0';
#undef APP

	/* Authorization header if an API key is configured */
	auth_hdr[0] = '\0';
	if (s_cfg.api_key[0]) {
		/* Build "Authorization: Bearer <key>\r\n" manually */
		size_t hlen = anx_strlcpy(auth_hdr,
					    "Authorization: Bearer ",
					    sizeof(auth_hdr));
		hlen += anx_strlcpy(auth_hdr + hlen, s_cfg.api_key,
				     sizeof(auth_hdr) - hlen);
		anx_strlcpy(auth_hdr + hlen, "\r\n",
			     sizeof(auth_hdr) - hlen);
	}

	anx_memset(&http_resp, 0, sizeof(http_resp));
	rc = anx_http_post_authed(
		s_cfg.http_host,
		s_cfg.http_port ? s_cfg.http_port : 80,
		s_cfg.http_path[0] ? s_cfg.http_path : "/v1/chat/completions",
		auth_hdr[0] ? auth_hdr : NULL,
		"application/json",
		body, (uint32_t)pos,
		&http_resp);
	anx_free(body);

	if (rc != 0 || !http_resp.body ||
	    http_resp.status_code < 200 || http_resp.status_code > 299) {
		kprintf("pii: http call failed (rc=%d status=%d)\n",
			rc, http_resp.status_code);
		anx_http_response_free(&http_resp);
		return -1;
	}

	/* The outer response is OpenAI-shaped; extract choices[0].message.content */
	inner = NULL;
	{
		struct anx_json_value  root;
		struct anx_json_value *choices, *c0, *msg, *cnt;

		if (anx_json_parse(http_resp.body, http_resp.body_len,
				    &root) == 0) {
			choices = anx_json_get(&root, "choices");
			c0      = choices ? anx_json_array_get(choices, 0)
					  : NULL;
			msg     = c0     ? anx_json_get(c0, "message") : NULL;
			cnt     = msg    ? anx_json_get(msg, "content") : NULL;
			if (cnt && cnt->type == ANX_JSON_STRING
			    && cnt->v.string.str) {
				size_t ilen = cnt->v.string.len + 1;
				inner = anx_alloc(ilen);
				if (inner)
					anx_memcpy(inner, cnt->v.string.str,
						   ilen);
			}
			anx_json_free(&root);
		}
	}
	anx_http_response_free(&http_resp);

	if (!inner) return -1;
	rc = parse_pii_json(inner, (uint32_t)anx_strlen(inner), out);
	anx_free(inner);
	return rc;
}

/* ── Public API ──────────────────────────────────────────────────── */

int anx_pii_check(const char *content, size_t len,
		   struct pii_result *result)
{
	int rc;

	anx_memset(result, 0, sizeof(*result));

	if (!s_ready || !s_cfg.enabled)
		return 0;  /* disabled → pass through */

	if (s_cfg.backend == PII_BACKEND_NATIVE)
		rc = call_native(content, len, result);
	else
		rc = call_http(content, len, result);

	if (rc != 0) {
		kprintf("pii: model call failed (fail_%s)\n",
			s_cfg.fail_open ? "open" : "closed");
		if (s_cfg.fail_open) {
			/* Return unflagged so caller delivers original */
			result->found = false;
			return 0;
		}
		return -1;  /* fail-closed: caller must block delivery */
	}

	if (result->found)
		kprintf("pii: detected [%s]\n", result->types);

	return 0;
}

void anx_pii_result_free(struct pii_result *result)
{
	if (result && result->redacted) {
		anx_free(result->redacted);
		result->redacted = NULL;
	}
}
