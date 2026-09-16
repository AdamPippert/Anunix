#ifndef ANX_OBSERVATION_H
#define ANX_OBSERVATION_H
#include <anx/state_object.h>

#define ANX_OBSERVATION_MAX 128U
#define ANX_OBSERVATION_OBJECT_MAX (1024U * 1024U)
enum anx_observation_kind { ANX_OBSERVATION_VISUAL = 1, ANX_OBSERVATION_STRUCTURED };
struct anx_observation_spec {
	anx_cid_t owner;
	anx_oid_t subject, content, provenance;
	enum anx_observation_kind kind;
	bool full_coverage;
};
struct anx_observation_view {
	anx_oid_t id, superseded_by;
	uint64_t sequence, subject_version;
	struct anx_observation_spec spec;
};
int anx_observation_publish(const struct anx_observation_spec *spec, anx_oid_t *out);
int anx_observation_describe(const anx_oid_t *id, struct anx_observation_view *out);
int anx_observation_supersede(const anx_oid_t *previous, const anx_oid_t *replacement);
int anx_observation_read(const anx_oid_t *id, uint64_t offset, void *out, uint64_t bytes);
int anx_observation_release(const anx_oid_t *id);
#endif
