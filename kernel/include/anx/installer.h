/*
 * anx/installer.h — Text-based OS installer.
 *
 * Installs Anunix to a block device from a JSON provisioning config.
 * Creates GPT partitions, formats object store, provisions credentials
 * and user accounts.
 */

#ifndef ANX_INSTALLER_H
#define ANX_INSTALLER_H

#include <anx/types.h>

/* Run the text-based installer (interactive or from provision JSON) */
int anx_installer_run(const char *provision_json, uint32_t json_len);

/* Run the installer interactively (prompts for all values) */
int anx_installer_interactive(void);

#endif /* ANX_INSTALLER_H */
