/*
 * anx/model_client.h — Claude Messages API client.
 *
 * Formats Claude API requests, sends them through the HTTP client
 * with credential injection, and parses structured responses.
 * Works through a TLS proxy for HTTPS endpoints.
 */

#ifndef ANX_MODEL_CLIENT_H
#define ANX_MODEL_CLIENT_H

#include <anx/types.h>

/* Model request parameters */
struct anx_model_request {
	const char *model;		/* e.g., "claude-sonnet-4-5-20241022" */
	const char *system;		/* system prompt (NULL for none) */
	const char *user_message;	/* user message content */
	uint32_t max_tokens;		/* max response tokens */
};

/* Parsed model response */
struct anx_model_response {
	char *content;			/* assistant response text */
	uint32_t content_len;
	int32_t input_tokens;
	int32_t output_tokens;
	char *stop_reason;		/* "end_turn", "max_tokens", etc. */
	int status_code;		/* HTTP status */
};

/* Configuration for the model endpoint */
struct anx_model_endpoint {
	const char *host;		/* proxy host (e.g., "10.0.2.2") */
	uint16_t port;			/* proxy port (e.g., 8080) */
	const char *cred_name;		/* credential name for x-api-key */
};

/* Initialize the model client with an endpoint config (also persists to disk) */
void anx_model_client_init(const struct anx_model_endpoint *ep);
/* Restore endpoint config from disk (called at boot after disk init) */
void anx_model_client_load(void);

/* Send a message to the Claude API and get a response */
int anx_model_call(const struct anx_model_request *req,
		    struct anx_model_response *resp);

/* Free a model response */
void anx_model_response_free(struct anx_model_response *resp);

/* Check if the model client is configured */
bool anx_model_client_ready(void);

#endif /* ANX_MODEL_CLIENT_H */
