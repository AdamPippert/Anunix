/*
 * anx/kickstart.h — Kickstart provisioning file support.
 *
 * A simple INI-style text format for deploying Anunix at scale:
 * baking in drivers, pre-configuring UI, deploying credentials,
 * and auto-loading workflows.  Format is line-oriented, sections
 * delimited by [name], key=value or key:subkey=value pairs, and
 * '#' line comments.
 */

#ifndef ANX_KICKSTART_H
#define ANX_KICKSTART_H

#include <anx/types.h>

#define ANX_KS_MAX_LINE		256
#define ANX_KS_MAX_SECTION	 32
#define ANX_KS_MAX_KEY		 64
#define ANX_KS_MAX_VALUE	192

/* Known sections */
enum anx_ks_section {
	ANX_KS_SECTION_UNKNOWN = 0,
	ANX_KS_SECTION_SYSTEM,
	ANX_KS_SECTION_DISK,
	ANX_KS_SECTION_UI,
	ANX_KS_SECTION_NETWORK,
	ANX_KS_SECTION_CREDENTIALS,
	ANX_KS_SECTION_WORKFLOWS,
	ANX_KS_SECTION_DRIVERS,
	ANX_KS_SECTION_UPDATES,
};

/* A single parsed key-value pair with its section */
struct anx_ks_entry {
	enum anx_ks_section	section;
	char			key[ANX_KS_MAX_KEY];
	char			subkey[ANX_KS_MAX_KEY];	/* "key:subkey=value"; empty if unused */
	char			value[ANX_KS_MAX_VALUE];
};

/* Callback invoked for each entry during parse+apply */
typedef int (*anx_ks_entry_fn)(const struct anx_ks_entry *entry, void *arg);

/* Parse a kickstart file from a memory buffer, calling cb for each entry.
 * Returns ANX_OK or first non-ANX_OK return from cb. */
int anx_ks_parse(const char *buf, uint32_t len, anx_ks_entry_fn cb, void *arg);

/* Apply a kickstart from a memory buffer (calls internal handlers for each section).
 * This is the main entry point used at boot. */
int anx_ks_apply(const char *buf, uint32_t len);

/* Apply from a path (reads the file then calls anx_ks_apply). */
int anx_ks_apply_path(const char *path);

/* Return a string describing the last parse error (static buffer). */
const char *anx_ks_last_error(void);

#endif /* ANX_KICKSTART_H */
