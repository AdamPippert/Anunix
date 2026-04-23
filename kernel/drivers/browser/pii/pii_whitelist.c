/*
 * pii_whitelist.c — Per-domain PII bypass whitelist.
 */

#include "pii_whitelist.h"
#include <anx/string.h>

static char     s_domains[PII_WHITELIST_MAX][PII_DOMAIN_MAX];
static uint32_t s_count;

void anx_pii_whitelist_init(void)
{
	s_count = 0;
}

int anx_pii_whitelist_add(const char *domain)
{
	uint32_t i;

	if (!domain || !domain[0])
		return -1;

	for (i = 0; i < s_count; i++) {
		if (anx_strcmp(s_domains[i], domain) == 0)
			return 0;   /* already present */
	}

	if (s_count >= PII_WHITELIST_MAX)
		return -1;

	anx_strlcpy(s_domains[s_count++], domain, PII_DOMAIN_MAX);
	return 0;
}

int anx_pii_whitelist_remove(const char *domain)
{
	uint32_t i;

	for (i = 0; i < s_count; i++) {
		if (anx_strcmp(s_domains[i], domain) == 0) {
			uint32_t j;
			for (j = i + 1; j < s_count; j++)
				anx_strlcpy(s_domains[j - 1], s_domains[j],
					    PII_DOMAIN_MAX);
			s_count--;
			return 0;
		}
	}
	return -1;
}

/* Extract the hostname from a URL into buf (NUL-terminated). */
static void extract_host(const char *url, char *buf, size_t cap)
{
	const char *p = url;
	const char *start;
	size_t len;

	if (anx_strncmp(p, "https://", 8) == 0) p += 8;
	else if (anx_strncmp(p, "http://",  7) == 0) p += 7;

	start = p;
	while (*p && *p != '/' && *p != ':' && *p != '?' && *p != '#')
		p++;

	len = (size_t)(p - start);
	if (len >= cap) len = cap - 1;
	anx_memcpy(buf, start, len);
	buf[len] = '\0';
}

bool anx_pii_whitelist_check(const char *url)
{
	char     host[PII_DOMAIN_MAX];
	uint32_t i;

	if (!url) return false;
	extract_host(url, host, sizeof(host));
	if (!host[0]) return false;

	for (i = 0; i < s_count; i++) {
		if (anx_strcmp(s_domains[i], host) == 0)
			return true;
	}
	return false;
}

uint32_t anx_pii_whitelist_list(char out[][PII_DOMAIN_MAX],
				 uint32_t max_entries)
{
	uint32_t n = s_count < max_entries ? s_count : max_entries;
	uint32_t i;

	for (i = 0; i < n; i++)
		anx_strlcpy(out[i], s_domains[i], PII_DOMAIN_MAX);
	return n;
}
