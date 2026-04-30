/*
 * anx/update.h — OS update channel.
 *
 * Fetches a kernel update image from anunix-distd, stages it on disk,
 * and reboots.  Version strings use YYYY.M.D dot notation.
 *
 * Typical call sequence (driven by kickstart auto_apply):
 *   anx_update_check()  — compare running vs available
 *   anx_update_fetch()  — download and stage on disk
 *   anx_update_reboot() — reboot to apply
 */

#ifndef ANX_UPDATE_H
#define ANX_UPDATE_H

#include <anx/types.h>

#define ANX_UPDATE_VERSION_LEN	32

/* Parsed update manifest returned by anx_update_check(). */
struct anx_update_manifest {
	char     version[ANX_UPDATE_VERSION_LEN];
	uint32_t size;		/* binary size in bytes */
};

/* Return the running kernel version string (static, never NULL). */
const char *anx_update_running_version(void);

/* Fetch the update manifest for <channel>/<arch> from <server>.
 * <auth_header> may be NULL; if non-NULL it is appended to the HTTP request.
 * Returns ANX_OK and fills *out on success, or negative on error. */
int anx_update_check(const char *server, const char *channel, const char *arch,
		     const char *auth_header,
		     struct anx_update_manifest *out);

/* Download update binary for <channel>/<arch> and stage it on disk.
 * <auth_header> may be NULL. */
int anx_update_fetch(const char *server, const char *channel, const char *arch,
		     const char *auth_header);

/* Check, fetch, and (if a newer version is available) stage then reboot.
 * This is the single call triggered by kickstart auto_apply=true. */
int anx_update_auto_apply(const char *server, const char *channel,
			   const char *auth_header);

/* Reboot to apply a staged update (noreturn). */
void anx_update_reboot(void) __attribute__((noreturn));

#endif /* ANX_UPDATE_H */
