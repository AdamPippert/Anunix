#include <anx/procedure.h>
int anx_procedure_compile(const anx_cid_t *owner, uint64_t source_use, const anx_oid_t *knowledge,
		uint64_t predecessor, struct anx_procedure_view *out)
{ (void)owner; (void)source_use; (void)knowledge; (void)predecessor; (void)out; return ANX_ENOSYS; }
int anx_procedure_validate(uint64_t id, uint64_t epoch, uint64_t replay_use, struct anx_procedure_view *out)
{ (void)id; (void)epoch; (void)replay_use; (void)out; return ANX_ENOSYS; }
int anx_procedure_get(uint64_t id, struct anx_procedure_view *out)
{ (void)id; (void)out; return ANX_ENOSYS; }
int anx_procedure_execute(uint64_t id, uint64_t epoch, uint64_t use, uint64_t use_epoch, struct anx_anxml_response *out)
{ (void)id; (void)epoch; (void)use; (void)use_epoch; (void)out; return ANX_ENOSYS; }
int anx_procedure_destroy(uint64_t id)
{ (void)id; return ANX_ENOSYS; }
