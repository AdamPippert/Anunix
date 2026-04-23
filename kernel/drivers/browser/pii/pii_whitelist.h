/*
 * pii_whitelist.h — Per-domain PII bypass whitelist.
 *
 * Domains on the whitelist bypass PII filtering entirely for all agent
 * sessions. Managed via browser settings or the REST API:
 *   GET    /api/v1/pii/whitelist
 *   POST   /api/v1/pii/whitelist        body: {"domain":"example.com"}
 *   DELETE /api/v1/pii/whitelist        body: {"domain":"example.com"}
 */

#ifndef ANX_PII_WHITELIST_H
#define ANX_PII_WHITELIST_H

#include <anx/types.h>

#define PII_WHITELIST_MAX   64
#define PII_DOMAIN_MAX     256

/* Initialize (empty list). Call once at startup. */
void anx_pii_whitelist_init(void);

/* Add a domain. Returns 0 on success, -1 if full or duplicate. */
int  anx_pii_whitelist_add(const char *domain);

/* Remove a domain. Returns 0 on success, -1 if not found. */
int  anx_pii_whitelist_remove(const char *domain);

/*
 * Return true if the hostname extracted from url is whitelisted.
 * Handles "http://host/path" and "https://host/path" forms.
 */
bool anx_pii_whitelist_check(const char *url);

/*
 * Copy up to max_entries domain strings into out[][].
 * Returns the number of entries written.
 */
uint32_t anx_pii_whitelist_list(char out[][PII_DOMAIN_MAX],
				 uint32_t max_entries);

#endif /* ANX_PII_WHITELIST_H */
