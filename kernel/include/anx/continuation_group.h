#ifndef ANX_CONTINUATION_GROUP_H
#define ANX_CONTINUATION_GROUP_H
#include <anx/cell.h>
#define ANX_CONTINUATION_GROUP_MAX 16U
#define ANX_CONTINUATION_GROUP_MEMBERS 4U
#define ANX_CONTINUATION_GROUP_PAGES 4U
struct anx_continuation_group_view {
	uint64_t id, epoch;
	anx_cid_t owner, members[ANX_CONTINUATION_GROUP_MEMBERS];
	anx_eid_t lease_id;
	uint32_t count, holder, cpu_slots, memory_bytes, physical_pages;
	bool revoked;
};
/* Controller enrolls direct children with one execution holder and one shared scratch reservation. */
int anx_continuation_group_create(const anx_cid_t *owner, const anx_cid_t *members, uint32_t count,
	uint32_t pages, struct anx_continuation_group_view *out);
int anx_continuation_group_get(uint64_t id, struct anx_continuation_group_view *out);
/* Controller handoff requires quiescent members and a current target. */
int anx_continuation_group_handoff(uint64_t id, uint64_t epoch, const anx_cid_t *target, struct anx_continuation_group_view *out);
int anx_continuation_group_revoke(uint64_t id, uint64_t epoch, struct anx_continuation_group_view *out);
int anx_continuation_group_destroy(uint64_t id);
/* Current holder or controller; no raw page mappings leave the group. */
int anx_continuation_group_read(uint64_t id, uint64_t epoch, uint32_t offset, void *bytes, uint32_t size);
int anx_continuation_group_write(uint64_t id, uint64_t epoch, uint32_t offset, const void *bytes, uint32_t size);
/* Runtime and scheduler hook. Unenrolled Cells retain their ordinary admission. */
int anx_continuation_group_check(struct anx_cell *cell);
#endif
