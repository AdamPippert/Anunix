/*
 * pii_filter.h — PII detection and redaction for agent-driven sessions.
 *
 * Calls an AI model (Anunix native model routing or an external
 * OpenAI-compatible endpoint) to detect and redact personally
 * identifiable information before content is delivered to an agent actor.
 *
 * Human-controlled sessions bypass PII filtering entirely.
 * The model prompt is structured so the response is always a JSON object:
 *   {"found": bool, "redacted": "...", "types": "email, phone, ..."}
 */

#ifndef ANX_PII_FILTER_H
#define ANX_PII_FILTER_H

#include <anx/types.h>

#define PII_MAX_CONTENT   65536  /* max bytes sent to model per check */
#define PII_MAX_HOST        256
#define PII_MAX_PATH        128
#define PII_MAX_MODEL        64
#define PII_MAX_APIKEY      256
#define PII_TYPES_LEN       256  /* comma-separated PII type list */
#define PII_TIMEOUT_MS    15000  /* default model call timeout */

enum pii_backend {
	PII_BACKEND_NATIVE = 0,   /* anx_model_client (Anunix OS, default) */
	PII_BACKEND_HTTP,         /* OpenAI-compatible HTTP endpoint (Lite) */
};

struct pii_config {
	bool             enabled;
	bool             fail_open;    /* pass-through on model error (default true) */
	enum pii_backend backend;
	char             http_host[PII_MAX_HOST];
	uint16_t         http_port;
	char             http_path[PII_MAX_PATH];   /* /v1/chat/completions */
	char             model_name[PII_MAX_MODEL]; /* openai/privacy-filter */
	char             api_key[PII_MAX_APIKEY];
	uint32_t         timeout_ms;
};

struct pii_result {
	bool  found;
	char *redacted;              /* heap-allocated; call anx_pii_result_free */
	char  types[PII_TYPES_LEN]; /* e.g. "email, phone, ssn" */
};

/*
 * Initialize the PII filter. Call once at browser startup.
 * Pass NULL to auto-configure: native backend when model client is ready,
 * disabled otherwise.
 */
int  anx_pii_init(const struct pii_config *cfg);

/*
 * Scan content for PII. Returns 0 on success, -1 on model error.
 * result->found is false and result->redacted is NULL when no PII found.
 * Caller must call anx_pii_result_free() when done with result.
 * Content over PII_MAX_CONTENT bytes is silently truncated.
 */
int  anx_pii_check(const char *content, size_t len,
		    struct pii_result *result);

/* Release heap memory in a pii_result. */
void anx_pii_result_free(struct pii_result *result);

/* True if configured and ready to check. */
bool anx_pii_ready(void);

#endif /* ANX_PII_FILTER_H */
