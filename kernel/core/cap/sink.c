/*
 * sink.c — Sink registry (RFC-0028 Protected Operation ABI).
 *
 * Tiny linear registry keyed by name, mirroring the external-call
 * handler registry (kernel/core/exec/external.c) — Sinks are cheap to
 * register and cheap to replace, since tests register and tear down
 * per case.
 */

#include <anx/types.h>
#include <anx/capability.h>
#include <anx/string.h>

#define ANX_SINK_MAX	16

static struct anx_sink sinks[ANX_SINK_MAX];
static bool sink_active[ANX_SINK_MAX];
static bool initialized;

void anx_sink_registry_init(void)
{
	uint32_t i;

	for (i = 0; i < ANX_SINK_MAX; i++) {
		sinks[i].name[0] = '\0';
		sinks[i].max_sensitivity = ANX_SENSITIVITY_PUBLIC;
		sink_active[i] = false;
	}
	initialized = true;
}

struct anx_sink *anx_sink_lookup(const char *name)
{
	uint32_t i;

	if (!name)
		return NULL;
	for (i = 0; i < ANX_SINK_MAX; i++) {
		if (sink_active[i] && anx_strcmp(sinks[i].name, name) == 0)
			return &sinks[i];
	}
	return NULL;
}

int anx_sink_register(const char *name, enum anx_sensitivity max_sensitivity,
		      struct anx_sink **out)
{
	struct anx_sink *slot;
	uint32_t i;

	if (!name || !name[0])
		return ANX_EINVAL;
	if (!initialized)
		anx_sink_registry_init();

	slot = anx_sink_lookup(name);
	if (slot) {
		slot->max_sensitivity = max_sensitivity;
		if (out)
			*out = slot;
		return ANX_OK;
	}

	for (i = 0; i < ANX_SINK_MAX; i++) {
		if (sink_active[i])
			continue;
		anx_strlcpy(sinks[i].name, name, sizeof(sinks[i].name));
		sinks[i].max_sensitivity = max_sensitivity;
		sink_active[i] = true;
		if (out)
			*out = &sinks[i];
		return ANX_OK;
	}
	return ANX_ENOMEM;
}

int anx_sink_check_send(const struct anx_sink *sink, const anx_oid_t *object_oid)
{
	enum anx_sensitivity level;

	if (!sink || !object_oid)
		return ANX_EINVAL;

	level = anx_object_get_sensitivity(object_oid);
	if (level > sink->max_sensitivity)
		return ANX_EPERM;
	return ANX_OK;
}
