#ifndef ANX_PROCEDURE_H
#define ANX_PROCEDURE_H
#include <anx/model_use.h>
#define ANX_PROCEDURE_MAX 16U
#define ANX_PROCEDURE_SCHEMA "anx:procedure/recipe/v1"
#define ANX_PROCEDURE_EVIDENCE_SCHEMA "anx:procedure/evidence/v1"
enum anx_procedure_state { ANX_PROCEDURE_DRAFT, ANX_PROCEDURE_VALIDATED };
struct anx_procedure_view {
	uint64_t id, epoch, version, predecessor;
	anx_cid_t owner;
	anx_oid_t artifact, evidence, knowledge;
	enum anx_procedure_state state;
};
/* Compile/promotion are controller operations; the recipe itself grants no authority. */
int anx_procedure_compile(const anx_cid_t *owner, uint64_t source_use, const anx_oid_t *knowledge,
		uint64_t predecessor, struct anx_procedure_view *out);
int anx_procedure_validate(uint64_t id, uint64_t epoch, uint64_t replay_use, struct anx_procedure_view *out);
int anx_procedure_get(uint64_t id, struct anx_procedure_view *out);
int anx_procedure_execute(uint64_t id, uint64_t epoch, uint64_t use, uint64_t use_epoch, struct anx_anxml_response *out);
/* Removal releases the handle; sealed evidence and artifacts remain in the object store. */
int anx_procedure_destroy(uint64_t id);
#endif
