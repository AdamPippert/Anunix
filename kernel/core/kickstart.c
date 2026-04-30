/*
 * kickstart.c — Kickstart provisioning file parser and applier.
 *
 * Reads an INI-style text buffer and dispatches each key-value pair
 * to the appropriate subsystem handler.  This is the boot-time
 * provisioning path: hostname, UI theme, network config, credentials,
 * workflows, and driver load lists are all wired in here.
 *
 * Phase 1: in-memory buffer only.  File loading is deferred until the
 * disk store is accessible (Phase 2).
 */

#include <anx/types.h>
#include <anx/kickstart.h>
#include <anx/string.h>
#include <anx/kprintf.h>
#include <anx/theme.h>
#include <anx/credential.h>
#include <anx/workflow.h>
#include <anx/workflow_library.h>
#include <anx/memory.h>
#include <anx/wf_bundle.h>
#include <anx/http.h>
#include <anx/alloc.h>
#include <anx/update.h>

/* Last parse/apply error message */
static char ks_error[128];

/* Update server URL set by [system] update_server= */
static char g_update_server[128];

/* [updates] section state */
static char g_update_channel[32];
static bool g_auto_apply;

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

/* Skip leading whitespace; return pointer to first non-space character. */
static const char *
skip_ws(const char *p)
{
	while (*p == ' ' || *p == '\t')
		p++;
	return p;
}

/* Strip trailing whitespace in-place from a NUL-terminated string. */
static void
rtrim(char *s)
{
	size_t n = anx_strlen(s);

	while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' ||
			 s[n - 1] == '\r' || s[n - 1] == '\n'))
		s[--n] = '\0';
}

/* Map a section name string to its enum value. */
static enum anx_ks_section
section_for_name(const char *name)
{
	if (anx_strcmp(name, "system") == 0)
		return ANX_KS_SECTION_SYSTEM;
	if (anx_strcmp(name, "disk") == 0)
		return ANX_KS_SECTION_DISK;
	if (anx_strcmp(name, "ui") == 0)
		return ANX_KS_SECTION_UI;
	if (anx_strcmp(name, "network") == 0)
		return ANX_KS_SECTION_NETWORK;
	if (anx_strcmp(name, "credentials") == 0)
		return ANX_KS_SECTION_CREDENTIALS;
	if (anx_strcmp(name, "workflows") == 0)
		return ANX_KS_SECTION_WORKFLOWS;
	if (anx_strcmp(name, "drivers") == 0)
		return ANX_KS_SECTION_DRIVERS;
	if (anx_strcmp(name, "updates") == 0)
		return ANX_KS_SECTION_UPDATES;
	return ANX_KS_SECTION_UNKNOWN;
}

/*
 * Decompose an http:// URL into host, port, and path components.
 * port defaults to 80 when not specified.
 */
static int
parse_http_url(const char *url, char host[128], uint16_t *port_out,
	       char path[192])
{
	const char	*p = url;
	const char	*slash;
	const char	*colon;
	uint32_t	 hlen;
	uint32_t	 n;

	/* Strip scheme */
	if (anx_strncmp(p, "http://", 7) == 0)
		p += 7;

	/* Find end of host[:port] at first '/' or string end */
	slash = p;
	while (*slash && *slash != '/')
		slash++;

	/* Look for port separator within the host section */
	colon = p;
	while (colon < slash && *colon != ':')
		colon++;

	if (*colon == ':') {
		hlen = (uint32_t)(colon - p);
		if (hlen == 0 || hlen >= 128)
			return ANX_EINVAL;
		anx_memcpy(host, p, hlen);
		host[hlen] = '\0';

		/* Parse port digits */
		n = 0;
		colon++;
		while (colon < slash && *colon >= '0' && *colon <= '9') {
			n = n * 10 + (uint32_t)(*colon - '0');
			colon++;
		}
		*port_out = (uint16_t)n;
	} else {
		hlen = (uint32_t)(slash - p);
		if (hlen == 0 || hlen >= 128)
			return ANX_EINVAL;
		anx_memcpy(host, p, hlen);
		host[hlen] = '\0';
		*port_out = 80;
	}

	/* Path is everything from the first '/' onward; default to "/" */
	if (*slash == '/') {
		anx_strlcpy(path, slash, 192);
	} else {
		path[0] = '/';
		path[1] = '\0';
	}

	return ANX_OK;
}

/* ------------------------------------------------------------------ */
/* Parser                                                              */
/* ------------------------------------------------------------------ */

/*
 * Walk buf char-by-char, extracting lines.  For each line:
 *   - blank or '#' comment lines are skipped
 *   - '[section]' lines update the current section
 *   - 'key=value' or 'key:subkey=value' lines invoke cb
 *
 * Returns ANX_OK, or the first non-OK value returned by cb.
 */
int
anx_ks_parse(const char *buf, uint32_t len, anx_ks_entry_fn cb, void *arg)
{
	enum anx_ks_section	cur_section = ANX_KS_SECTION_UNKNOWN;
	char			line[ANX_KS_MAX_LINE];
	uint32_t		pos = 0;
	uint32_t		lpos;
	const char		*p;
	char			*eq;
	char			*colon;
	struct anx_ks_entry	entry;
	int			ret;

	if (!buf || !cb)
		return ANX_EINVAL;

	while (pos < len) {
		/* Extract one line */
		lpos = 0;
		while (pos < len && buf[pos] != '\n') {
			if (lpos < ANX_KS_MAX_LINE - 1)
				line[lpos++] = buf[pos];
			pos++;
		}
		if (pos < len)
			pos++;	/* consume '\n' */
		line[lpos] = '\0';

		/* Skip leading whitespace */
		p = skip_ws(line);

		/* Blank line or comment */
		if (*p == '\0' || *p == '#')
			continue;

		/* Section header: [name] */
		if (*p == '[') {
			char sec_name[ANX_KS_MAX_SECTION];
			uint32_t slen = 0;
			const char *q = p + 1;

			while (*q && *q != ']' && slen < ANX_KS_MAX_SECTION - 1)
				sec_name[slen++] = *q++;
			sec_name[slen] = '\0';
			cur_section = section_for_name(sec_name);
			continue;
		}

		/* Key-value line: find '=' */
		anx_strlcpy(line, p, ANX_KS_MAX_LINE);	/* re-anchor at trimmed start */
		rtrim(line);
		eq = line;
		while (*eq && *eq != '=')
			eq++;
		if (*eq != '=')
			continue;	/* malformed — skip */

		*eq = '\0';	/* split: line is key[[:subkey]], eq+1 is value */

		anx_memset(&entry, 0, sizeof(entry));
		entry.section = cur_section;

		/* Check for key:subkey syntax */
		colon = line;
		while (*colon && *colon != ':')
			colon++;
		if (*colon == ':') {
			*colon = '\0';
			anx_strlcpy(entry.key,    line,    ANX_KS_MAX_KEY);
			anx_strlcpy(entry.subkey, colon + 1, ANX_KS_MAX_KEY);
		} else {
			anx_strlcpy(entry.key,   line, ANX_KS_MAX_KEY);
			entry.subkey[0] = '\0';
		}

		anx_strlcpy(entry.value, eq + 1, ANX_KS_MAX_VALUE);
		rtrim(entry.value);

		ret = cb(&entry, arg);
		if (ret != ANX_OK) {
			anx_snprintf(ks_error, sizeof(ks_error),
				"kickstart: handler error %d for key '%s'",
				ret, entry.key);
			return ret;
		}
	}

	return ANX_OK;
}

/* ------------------------------------------------------------------ */
/* Section handlers                                                    */
/* ------------------------------------------------------------------ */

static int
handle_system(const struct anx_ks_entry *e)
{
	if (anx_strcmp(e->key, "hostname") == 0) {
		kprintf("kickstart: hostname=%s\n", e->value);
		return ANX_OK;
	}
	if (anx_strcmp(e->key, "timezone") == 0) {
		kprintf("kickstart: timezone=%s\n", e->value);
		return ANX_OK;
	}
	if (anx_strcmp(e->key, "update_server") == 0) {
		anx_strlcpy(g_update_server, e->value, sizeof(g_update_server));
		kprintf("kickstart: update_server=%s\n", g_update_server);
		return ANX_OK;
	}
	kprintf("kickstart: [system] unknown key '%s'\n", e->key);
	return ANX_OK;
}

static int
handle_disk(const struct anx_ks_entry *e)
{
	if (anx_strcmp(e->key, "format") == 0) {
		kprintf("kickstart: disk format=%s\n", e->value);
		return ANX_OK;
	}
	if (anx_strcmp(e->key, "size_mb") == 0) {
		kprintf("kickstart: disk size=%s MB\n", e->value);
		return ANX_OK;
	}
	kprintf("kickstart: [disk] unknown key '%s'\n", e->key);
	return ANX_OK;
}

static int
handle_ui(const struct anx_ks_entry *e)
{
	if (anx_strcmp(e->key, "theme") == 0) {
		if (anx_strcmp(e->value, "pretty") == 0)
			anx_theme_set_mode(ANX_THEME_PRETTY);
		else if (anx_strcmp(e->value, "boring") == 0)
			anx_theme_set_mode(ANX_THEME_BORING);
		else
			kprintf("kickstart: unknown theme '%s'\n", e->value);
		return ANX_OK;
	}
	if (anx_strcmp(e->key, "font_scale") == 0) {
		char cfg[32];
		anx_snprintf(cfg, sizeof(cfg), "font_scale=%s", e->value);
		anx_theme_apply_config(cfg);
		return ANX_OK;
	}
	if (anx_strcmp(e->key, "compositor") == 0) {
		kprintf("kickstart: compositor mode=%s\n", e->value);
		return ANX_OK;
	}
	kprintf("kickstart: [ui] unknown key '%s'\n", e->key);
	return ANX_OK;
}

static int
handle_network(const struct anx_ks_entry *e)
{
	if (anx_strcmp(e->key, "mode") == 0) {
		kprintf("kickstart: network mode=%s\n", e->value);
	} else if (anx_strcmp(e->key, "ip") == 0) {
		kprintf("kickstart: network ip=%s\n", e->value);
	} else if (anx_strcmp(e->key, "netmask") == 0) {
		kprintf("kickstart: network netmask=%s\n", e->value);
	} else if (anx_strcmp(e->key, "gateway") == 0) {
		kprintf("kickstart: network gateway=%s\n", e->value);
	} else if (anx_strcmp(e->key, "dns") == 0) {
		kprintf("kickstart: network dns=%s\n", e->value);
	} else {
		kprintf("kickstart: [network] unknown key '%s'\n", e->key);
	}
	return ANX_OK;
}

static int
handle_credentials(const struct anx_ks_entry *e)
{
	int ret;

	/* key:subkey=value — subkey is the credential name, value is the secret */
	if (anx_strcmp(e->key, "key") == 0 && e->subkey[0] != '\0') {
		ret = anx_credential_create(e->subkey, ANX_CRED_API_KEY,
					    e->value, (uint32_t)anx_strlen(e->value));
		if (ret != ANX_OK)
			kprintf("kickstart: warning: credential '%s' create failed (%d)\n",
				e->subkey, ret);
		else
			kprintf("kickstart: credential '%s' registered\n", e->subkey);
		return ANX_OK;
	}
	kprintf("kickstart: [credentials] unknown key '%s'\n", e->key);
	return ANX_OK;
}

static int
handle_workflows(const struct anx_ks_entry *e)
{
	anx_oid_t			 wf_oid;
	int				 ret;
	char				 host[128];
	char				 path[192];
	uint16_t			 port;
	char				 auth_hdr[256];
	char				 tok[256];
	uint32_t			 tok_len;
	struct anx_http_response	 resp;

	if (anx_strcmp(e->key, "load") == 0) {
		ret = anx_wf_lib_instantiate(e->value, e->value, &wf_oid);
		if (ret != ANX_OK)
			kprintf("kickstart: workflow load=%s failed (%d)\n",
				e->value, ret);
		else {
			kprintf("kickstart: workflow loaded: %s\n", e->value);
			anx_pal_prime_kickstart(e->value);
		}
		return ANX_OK;
	}
	if (anx_strcmp(e->key, "autorun") == 0) {
		ret = anx_wf_lib_instantiate(e->value, e->value, &wf_oid);
		if (ret != ANX_OK) {
			kprintf("kickstart: workflow autorun=%s failed (%d)\n",
				e->value, ret);
			return ANX_OK;
		}
		anx_pal_prime_kickstart(e->value);
		ret = anx_wf_run(&wf_oid, NULL);
		if (ret != ANX_OK)
			kprintf("kickstart: workflow autorun=%s run failed (%d)\n",
				e->value, ret);
		else
			kprintf("kickstart: workflow autorunning: %s\n", e->value);
		return ANX_OK;
	}
	if (anx_strcmp(e->key, "fetch") == 0) {
		ret = parse_http_url(e->value, host, &port, path);
		if (ret != ANX_OK) {
			kprintf("kickstart: workflow fetch: bad URL '%s'\n",
				e->value);
			return ANX_OK;
		}

		anx_memset(&resp, 0, sizeof(resp));

		tok_len = sizeof(tok);
		if (anx_credential_read("superrouter-api-key", tok,
					(uint32_t)sizeof(tok) - 1,
					(uint32_t *)&tok_len) == ANX_OK) {
			tok[tok_len] = '\0';
			anx_snprintf(auth_hdr, sizeof(auth_hdr),
				"Authorization: Bearer %s\r\n", tok);
			ret = anx_http_get_authed(host, port, path,
						  auth_hdr, &resp);
		} else {
			ret = anx_http_get(host, port, path, &resp);
		}

		if (ret != ANX_OK) {
			kprintf("kickstart: workflow fetch=%s HTTP error (%d)\n",
				e->value, ret);
			return ANX_OK;
		}

		if (resp.status_code == 200) {
			ret = anx_wf_bundle_register(resp.body, resp.body_len);
			if (ret != ANX_OK)
				kprintf("kickstart: workflow fetch=%s register failed (%d)\n",
					e->value, ret);
			else
				kprintf("kickstart: workflow fetched and registered: %s\n",
					e->value);
		} else {
			kprintf("kickstart: workflow fetch=%s HTTP %d\n",
				e->value, resp.status_code);
		}

		anx_http_response_free(&resp);
		return ANX_OK;
	}
	kprintf("kickstart: [workflows] unknown key '%s'\n", e->key);
	return ANX_OK;
}

static int
handle_drivers(const struct anx_ks_entry *e)
{
	if (anx_strcmp(e->key, "load") == 0) {
		kprintf("kickstart: driver load=%s (Phase 2)\n", e->value);
		return ANX_OK;
	}
	if (anx_strcmp(e->key, "disable") == 0) {
		kprintf("kickstart: driver disable=%s (Phase 2)\n", e->value);
		return ANX_OK;
	}
	if (anx_strcmp(e->key, "fetch") == 0) {
		kprintf("kickstart: driver fetch=%s (Phase 2)\n", e->value);
		return ANX_OK;
	}
	kprintf("kickstart: [drivers] unknown key '%s'\n", e->key);
	return ANX_OK;
}

static int
handle_updates(const struct anx_ks_entry *e)
{
	if (anx_strcmp(e->key, "channel") == 0) {
		anx_strlcpy(g_update_channel, e->value, sizeof(g_update_channel));
		kprintf("kickstart: update channel=%s\n", g_update_channel);
		return ANX_OK;
	}
	if (anx_strcmp(e->key, "auto_apply") == 0) {
		g_auto_apply = (anx_strcmp(e->value, "true") == 0);
		kprintf("kickstart: update auto_apply=%s\n",
			g_auto_apply ? "true" : "false");
		return ANX_OK;
	}
	kprintf("kickstart: [updates] unknown key '%s'\n", e->key);
	return ANX_OK;
}

/* Dispatch callback for anx_ks_apply. */
static int
apply_entry(const struct anx_ks_entry *e, void *arg)
{
	(void)arg;

	switch (e->section) {
	case ANX_KS_SECTION_SYSTEM:
		return handle_system(e);
	case ANX_KS_SECTION_DISK:
		return handle_disk(e);
	case ANX_KS_SECTION_UI:
		return handle_ui(e);
	case ANX_KS_SECTION_NETWORK:
		return handle_network(e);
	case ANX_KS_SECTION_CREDENTIALS:
		return handle_credentials(e);
	case ANX_KS_SECTION_WORKFLOWS:
		return handle_workflows(e);
	case ANX_KS_SECTION_DRIVERS:
		return handle_drivers(e);
	case ANX_KS_SECTION_UPDATES:
		return handle_updates(e);
	default:
		kprintf("kickstart: entry in unknown section (key='%s')\n", e->key);
		return ANX_OK;
	}
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

/* Apply a kickstart from a memory buffer.  Main boot-time entry point. */
int
anx_ks_apply(const char *buf, uint32_t len)
{
	int ret;

	if (!buf || len == 0)
		return ANX_EINVAL;
	kprintf("kickstart: applying config (%u bytes)\n", len);

	ret = anx_ks_parse(buf, len, apply_entry, NULL);
	if (ret != ANX_OK)
		return ret;

	if (g_auto_apply && g_update_server[0] != '\0' &&
	    g_update_channel[0] != '\0') {
		char auth_hdr[256];
		char tok[256];
		uint32_t tok_len = sizeof(tok) - 1;
		const char *auth = NULL;

		if (anx_credential_read("superrouter-api-key", tok,
					(uint32_t)sizeof(tok) - 1,
					&tok_len) == ANX_OK) {
			tok[tok_len] = '\0';
			anx_snprintf(auth_hdr, sizeof(auth_hdr),
				"Authorization: Bearer %s\r\n", tok);
			auth = auth_hdr;
		}
		anx_update_auto_apply(g_update_server, g_update_channel, auth);
	}

	return ANX_OK;
}

/* Apply from a path — disk loading deferred to Phase 2. */
int
anx_ks_apply_path(const char *path)
{
	(void)path;
	kprintf("kickstart: file loading not yet implemented (use anx_ks_apply)\n");
	return ANX_ENOTSUP;
}

/* Return the last error string (static buffer, never NULL). */
const char *
anx_ks_last_error(void)
{
	return ks_error;
}
