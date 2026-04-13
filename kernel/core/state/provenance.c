/*
 * provenance.c — Append-only provenance log for State Objects.
 */

#include <anx/types.h>
#include <anx/provenance.h>
#include <anx/alloc.h>
#include <anx/string.h>

#define PROV_INITIAL_CAP	8

struct anx_prov_log *anx_prov_log_create(void)
{
	struct anx_prov_log *log = anx_zalloc(sizeof(*log));

	if (!log)
		return NULL;

	log->events = anx_alloc(PROV_INITIAL_CAP * sizeof(struct anx_prov_event));
	if (!log->events) {
		anx_free(log);
		return NULL;
	}

	log->count = 0;
	log->capacity = PROV_INITIAL_CAP;
	return log;
}

int anx_prov_log_append(struct anx_prov_log *log,
			const struct anx_prov_event *event)
{
	if (log->count >= log->capacity) {
		uint32_t new_cap = log->capacity * 2;
		struct anx_prov_event *new_buf;

		new_buf = anx_alloc(new_cap * sizeof(struct anx_prov_event));
		if (!new_buf)
			return ANX_ENOMEM;

		anx_memcpy(new_buf, log->events,
			   log->count * sizeof(struct anx_prov_event));
		anx_free(log->events);
		log->events = new_buf;
		log->capacity = new_cap;
	}

	anx_memcpy(&log->events[log->count], event,
		   sizeof(struct anx_prov_event));
	log->events[log->count].event_id = log->count;
	log->count++;
	return ANX_OK;
}

const struct anx_prov_event *anx_prov_log_get(const struct anx_prov_log *log,
					      uint32_t index)
{
	if (index >= log->count)
		return NULL;
	return &log->events[index];
}

uint32_t anx_prov_log_count(const struct anx_prov_log *log)
{
	return log->count;
}

void anx_prov_log_destroy(struct anx_prov_log *log)
{
	if (!log)
		return;
	if (log->events)
		anx_free(log->events);
	anx_free(log);
}
