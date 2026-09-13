/* Baseline catalog adapter carries observation metadata without coherence checks. */
#include <anx/observation.h>
#include <anx/string.h>
#include <anx/uuid.h>
static struct anx_observation_view catalog[ANX_OBSERVATION_MAX];
static struct anx_observation_view *lookup(const anx_oid_t *id)
{
	for (uint32_t i = 0; i < ANX_OBSERVATION_MAX; i++)
		if (!anx_uuid_is_nil(&catalog[i].id) && !anx_uuid_compare(&catalog[i].id, id)) return &catalog[i];
	return NULL;
}
int anx_observation_publish(const struct anx_observation_spec *spec, anx_oid_t *out)
{
	for (uint32_t i = 0; i < ANX_OBSERVATION_MAX; i++) if (anx_uuid_is_nil(&catalog[i].id)) {
		catalog[i].spec = *spec; anx_uuid_generate(&catalog[i].id); *out = catalog[i].id; return ANX_OK;
	}
	return ANX_EFULL;
}
int anx_observation_describe(const anx_oid_t *id, struct anx_observation_view *out)
{ struct anx_observation_view *v = lookup(id); if (!v) return ANX_ENOENT; *out = *v; return ANX_OK; }
int anx_observation_supersede(const anx_oid_t *previous, const anx_oid_t *replacement)
{ struct anx_observation_view *v = lookup(previous); if (!v) return ANX_ENOENT; v->superseded_by = *replacement; return ANX_OK; }
int anx_observation_read(const anx_oid_t *id, uint64_t offset, void *out, uint64_t bytes)
{
	struct anx_observation_view *v = lookup(id);
	struct anx_object_handle h = {0};
	if (!v) return ANX_ENOENT;
	int ret = anx_so_open(&v->spec.content, ANX_OPEN_READ, &h);
	if (ret == ANX_OK) ret = anx_so_read_payload(&h, offset, out, bytes);
	anx_so_close(&h); return ret;
}
int anx_observation_release(const anx_oid_t *id)
{ struct anx_observation_view *v = lookup(id); if (!v) return ANX_ENOENT; anx_memset(v, 0, sizeof(*v)); return ANX_OK; }
