#ifndef ANX_CONFIG_H
#define ANX_CONFIG_H

#include <anx/types.h>

#define ANX_CONFIG_TEXT_MAX 2048
#define ANX_CONFIG_HOTKEY_MAX 64

struct anx_config_hotkey {
	uint32_t modifiers;
	uint32_t keycode;
};

/* Enumerate the supported system:config object names. */
const char *anx_config_name(uint32_t index);
/* Serialize live configuration; return byte count or a negative error. */
int anx_config_show(const char *name, char *out, uint32_t capacity);
/* Validate all fields before applying a partial text configuration live. */
int anx_config_set(const char *name, const char *text);
/* Save live settings to a durable named text object. */
int anx_config_save(const char *name);
/* Validate and apply a named configuration object. */
int anx_config_load(const char *name);
/* Load network policy at boot, reusing an already acquired DHCP lease. */
int anx_config_load_network(void);
/* Load desktop settings after theme, GUI, and hotkeys are initialized. */
void anx_config_load_desktop(void);
/* Execute the config shell builtin. */
int cmd_config(int argc, char **argv);
/* Snapshot hotkey chords in stable registration order. */
int anx_wm_hotkeys_snapshot(struct anx_config_hotkey *out, uint32_t capacity);
/* Return a stable name for a registered hotkey action. */
const char *anx_wm_hotkey_name(uint32_t index);
/* Atomically replace chords, rejecting duplicates and unsupported keys. */
int anx_wm_hotkeys_apply(const struct anx_config_hotkey *keys, uint32_t count);

#endif
