#include <anx/continuation_group.h>
int anx_continuation_group_create(const anx_cid_t *owner, const anx_cid_t *members, uint32_t count,
	uint32_t pages, struct anx_continuation_group_view *out)
{ (void)owner; (void)members; (void)count; (void)pages; (void)out; return ANX_ENOSYS; }
int anx_continuation_group_get(uint64_t id, struct anx_continuation_group_view *out)
{ (void)id; (void)out; return ANX_ENOSYS; }
int anx_continuation_group_handoff(uint64_t id, uint64_t epoch, const anx_cid_t *target, struct anx_continuation_group_view *out)
{ (void)id; (void)epoch; (void)target; (void)out; return ANX_ENOSYS; }
int anx_continuation_group_revoke(uint64_t id, uint64_t epoch, struct anx_continuation_group_view *out)
{ (void)id; (void)epoch; (void)out; return ANX_ENOSYS; }
int anx_continuation_group_destroy(uint64_t id)
{ (void)id; return ANX_ENOSYS; }
int anx_continuation_group_read(uint64_t id, uint64_t epoch, uint32_t offset, void *bytes, uint32_t size)
{ (void)id; (void)epoch; (void)offset; (void)bytes; (void)size; return ANX_ENOSYS; }
int anx_continuation_group_write(uint64_t id, uint64_t epoch, uint32_t offset, const void *bytes, uint32_t size)
{ (void)id; (void)epoch; (void)offset; (void)bytes; (void)size; return ANX_ENOSYS; }
int anx_continuation_group_check(struct anx_cell *cell)
{ (void)cell; return ANX_OK; }
