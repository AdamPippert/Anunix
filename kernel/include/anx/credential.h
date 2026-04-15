/*
 * anx/credential.h — Credential Objects and secrets store (RFC-0008).
 *
 * Kernel-enforced secrets management. Credential payloads are opaque
 * to all subsystems except anx_credential_read(), which enforces
 * access policy. Payloads never appear in traces, provenance, or
 * network messages.
 */

#ifndef ANX_CREDENTIAL_H
#define ANX_CREDENTIAL_H

#include <anx/types.h>

/* Credential types */
enum anx_credential_type {
	ANX_CRED_API_KEY,	/* Bearer token / API key */
	ANX_CRED_TOKEN,		/* OAuth / session token */
	ANX_CRED_CERTIFICATE,	/* TLS client certificate */
	ANX_CRED_PRIVATE_KEY,	/* Private key material */
	ANX_CRED_PASSWORD,	/* Username/password pair */
	ANX_CRED_OPAQUE,	/* Untyped secret bytes */
};

/* Credential metadata (safe to display — never contains payload) */
struct anx_credential_info {
	char name[128];
	enum anx_credential_type cred_type;
	uint32_t secret_len;
	anx_time_t created_at;
	anx_time_t last_accessed;
	uint32_t access_count;
	bool active;
};

/* Initialize the credential store */
void anx_credstore_init(void);

/* Create a new credential (sealed immediately, payload zeroed from input) */
int anx_credential_create(const char *name,
			    enum anx_credential_type cred_type,
			    const void *secret, uint32_t secret_len);

/* Read the credential payload into buf (enforces access policy) */
int anx_credential_read(const char *name,
			 void *buf, uint32_t buf_len,
			 uint32_t *actual_len);

/* Get credential metadata without accessing the payload */
int anx_credential_info(const char *name,
			 struct anx_credential_info *info);

/* Rotate a credential to a new value */
int anx_credential_rotate(const char *name,
			    const void *new_secret, uint32_t new_len);

/* Revoke a credential immediately (zeroes payload) */
int anx_credential_revoke(const char *name);

/* List credential names (without payloads) */
int anx_credential_list(struct anx_credential_info *out,
			 uint32_t max_entries, uint32_t *count);

/* Check if a credential exists and is active */
bool anx_credential_exists(const char *name);

#endif /* ANX_CREDENTIAL_H */
