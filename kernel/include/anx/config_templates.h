#ifndef ANX_CONFIG_TEMPLATES_H
#define ANX_CONFIG_TEMPLATES_H

#include <anx/types.h>

/* Enumerate built-in templates; return NULL after the last template. */
const char *anx_config_template_name(uint32_t index);

/* Describe a template; return NULL for an unknown name. */
const char *anx_config_template_describe(const char *name);

/* Apply appearance and tiling now; save theme and tiling separately to persist. */
int anx_config_template_apply(const char *name);

#endif /* ANX_CONFIG_TEMPLATES_H */
