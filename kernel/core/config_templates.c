#include <anx/config_templates.h>
#include <anx/config.h>
#include <anx/string.h>
#include <anx/theme.h>

static const struct {
	const char *name;
	const char *description;
	const char *theme;
	const char *tiling;
} templates[] = {
#define ANX_CONFIG_TEMPLATE(name, description, theme, tiling) \
	{ name, description, theme, tiling },
#include "../../config/templates/macos.inc"
#include "../../config/templates/windows.inc"
#include "../../config/templates/omarchy.inc"
#undef ANX_CONFIG_TEMPLATE
};

#define TEMPLATE_COUNT (sizeof(templates) / sizeof(templates[0]))

static int template_index(const char *name)
{
	uint32_t i;

	if (name)
		for (i = 0; i < TEMPLATE_COUNT; i++)
			if (anx_strcmp(templates[i].name, name) == 0)
				return (int)i;
	return ANX_ENOENT;
}

const char *anx_config_template_name(uint32_t index)
{
	return index < TEMPLATE_COUNT ? templates[index].name : NULL;
}

const char *anx_config_template_describe(const char *name)
{
	int i = template_index(name);

	return i < 0 ? NULL : templates[i].description;
}

int anx_config_template_apply(const char *name)
{
	struct anx_theme previous;
	int i = template_index(name), rc;

	if (i < 0)
		return i;
	previous = *anx_theme_get();
	rc = anx_theme_apply_config_checked(templates[i].theme);
	if (rc != ANX_OK)
		return rc;
	/* Config validates the complete tiling document before it changes layout. */
	rc = anx_config_set("tiling", templates[i].tiling);
	if (rc != ANX_OK)
		anx_theme_restore(&previous);
	return rc;
}
