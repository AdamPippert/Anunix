#include <anx/continuation_group.h>
#include <anx/engine_lease.h>
#include <anx/identity.h>
#include <anx/effect_fence.h>
#include <anx/sched_domain.h>
#include <anx/alloc.h>
#include <anx/page.h>
#include <anx/string.h>
#include <anx/uuid.h>
struct continuation_group {
	struct anx_continuation_group_view view;
	struct anx_cell *owner, *members[ANX_CONTINUATION_GROUP_MEMBERS];
	anx_oid_t identity, member_identity[ANX_CONTINUATION_GROUP_MEMBERS];
	struct anx_engine_lease *lease;
	uintptr_t pages[ANX_CONTINUATION_GROUP_PAGES];
};
static struct continuation_group *groups[ANX_CONTINUATION_GROUP_MAX];
static struct anx_spinlock group_lock = ANX_SPINLOCK_INIT;
static uint64_t sequence;
static struct continuation_group *find(uint64_t id)
{
	for (uint32_t i = 0; i < ANX_CONTINUATION_GROUP_MAX; i++) if (groups[i] && groups[i]->view.id == id) return groups[i];
	return NULL;
}
static int member_index(struct continuation_group *g, const anx_cid_t *cid)
{
	for (uint32_t i = 0; i < g->view.count; i++) if (!anx_uuid_compare(cid, &g->view.members[i])) return (int)i;
	return -1;
}
static bool enrolled(const anx_cid_t *cid)
{
	for (uint32_t i = 0; i < ANX_CONTINUATION_GROUP_MAX; i++)
		if (groups[i] && (!anx_uuid_compare(cid, &groups[i]->view.owner) || member_index(groups[i], cid) >= 0)) return true;
	return false;
}
static int inspect(struct continuation_group *g)
{
	if (!g) return ANX_ENOENT;
	const anx_cid_t *caller = anx_cell_current_id();
	return caller && anx_uuid_compare(caller, &g->view.owner) && member_index(g, caller) < 0 ? ANX_EPERM : ANX_OK;
}
static int cell_current(struct anx_cell *cell, const anx_oid_t *identity)
{
	if (anx_cell_status_terminal(cell->status)) return ANX_EPERM;
	anx_oid_t now;
	int ret = anx_cell_check_scope(cell);
	if (ret == ANX_OK) ret = anx_cell_check_contract(cell);
	if (ret == ANX_OK) ret = anx_sched_domain_check(cell);
	if (ret == ANX_OK) ret = anx_identity_admit(cell, &now);
	if (ret == ANX_OK && anx_uuid_compare(&now, identity)) ret = ANX_EBUSY;
	if (ret == ANX_OK) ret = anx_effect_fence_check(cell, NULL, NULL);
	return ret;
}
static int group_current(struct continuation_group *g)
{
	if (g->view.revoked) return ANX_EPERM;
	if (!anx_uuid_is_nil(&g->owner->parent_cid)) return ANX_EPERM;
	int ret = cell_current(g->owner, &g->identity);
	if (ret == ANX_OK && anx_lease_lookup(&g->view.lease_id) != g->lease) ret = ANX_EPERM;
	if (ret == ANX_OK && (g->lease->parent || g->lease->mem_tier != ANX_MEM_L0 || g->lease->accel != ANX_ACCEL_NONE ||
	    g->lease->accel_pct || g->lease->mem_reserved_bytes != g->view.memory_bytes ||
	    g->lease->mem_used_bytes != g->view.memory_bytes)) ret = ANX_EBUSY;
	if (ret == ANX_OK && g->lease->expires_at && arch_time_now() >= g->lease->expires_at) ret = ANX_ETIMEDOUT;
	return ret;
}
static int member_current(struct continuation_group *g, uint32_t member)
{
	if (anx_uuid_compare(&g->members[member]->parent_cid, &g->view.owner)) return ANX_EPERM;
	return cell_current(g->members[member], &g->member_identity[member]);
}
static bool active(struct continuation_group *g)
{
	if (g->owner->runtime_active) return true;
	for (uint32_t i = 0; i < g->view.count; i++) if (g->members[i]->runtime_active) return true;
	return false;
}
static void free_pages(struct continuation_group *g)
{
	for (uint32_t i = 0; i < ANX_CONTINUATION_GROUP_PAGES; i++) if (g->pages[i]) {
		anx_memset((void *)g->pages[i], 0, ANX_PAGE_SIZE);
		anx_page_free(g->pages[i], 0); g->pages[i] = 0;
	}
	g->view.physical_pages = 0;
}
static void release_cells(struct continuation_group *g)
{
	for (uint32_t i = 0; i < g->view.count; i++) if (g->members[i]) anx_cell_store_release(g->members[i]);
	if (g->owner) anx_cell_store_release(g->owner);
}
int anx_continuation_group_create(const anx_cid_t *owner, const anx_cid_t *members, uint32_t count,
	uint32_t pages, struct anx_continuation_group_view *out)
{
	if (anx_cell_current_id()) return ANX_EPERM;
	if (!owner || anx_uuid_is_nil(owner) || !members || !out || !count || count > ANX_CONTINUATION_GROUP_MEMBERS ||
	    !pages || pages > ANX_CONTINUATION_GROUP_PAGES) return ANX_EINVAL;
	struct continuation_group *g = anx_zalloc(sizeof(*g));
	if (!g) return ANX_ENOMEM;
	g->view.owner = *owner; g->view.count = count; g->view.epoch = 1;
	g->view.cpu_slots = 1; g->view.memory_bytes = pages * ANX_PAGE_SIZE;
	anx_memcpy(g->view.members, members, count * sizeof(*members));
	g->owner = anx_cell_store_lookup(owner);
	int ret = g->owner ? ANX_OK : ANX_ENOENT;
	bool flags; anx_spin_lock_irqsave(&group_lock, &flags);
	if (ret == ANX_OK && enrolled(owner)) ret = ANX_EEXIST;
	if (ret == ANX_OK && (!anx_uuid_is_nil(&g->owner->parent_cid) || g->owner->status != ANX_CELL_CREATED ||
	    g->owner->runtime_active)) ret = ANX_EPERM;
	if (ret == ANX_OK) ret = anx_identity_admit(g->owner, &g->identity);
	if (ret == ANX_OK) ret = cell_current(g->owner, &g->identity);
	for (uint32_t i = 0; ret == ANX_OK && i < count; i++) {
		if (anx_uuid_is_nil(&g->view.members[i]) || !anx_uuid_compare(&g->view.members[i], owner)) { ret = ANX_EINVAL; break; }
		if (enrolled(&g->view.members[i])) { ret = ANX_EEXIST; break; }
		for (uint32_t j = 0; j < i; j++) if (!anx_uuid_compare(&g->view.members[i], &g->view.members[j])) ret = ANX_EEXIST;
		if (ret != ANX_OK) break;
		g->members[i] = anx_cell_store_lookup(&g->view.members[i]);
		if (!g->members[i]) { ret = ANX_ENOENT; break; }
		if (g->members[i]->status != ANX_CELL_CREATED || g->members[i]->runtime_active) { ret = ANX_EPERM; break; }
		ret = anx_identity_admit(g->members[i], &g->member_identity[i]);
		if (ret == ANX_OK) ret = member_current(g, i);
	}
	uint32_t slot = 0;
	while (slot < ANX_CONTINUATION_GROUP_MAX && groups[slot]) slot++;
	if (ret == ANX_OK && (slot == ANX_CONTINUATION_GROUP_MAX || sequence == ~(uint64_t)0)) ret = ANX_EFULL;
	if (ret == ANX_OK) {
		anx_uuid_generate(&g->view.lease_id);
		ret = anx_lease_grant(&g->view.lease_id, ANX_MEM_L0, g->view.memory_bytes, ANX_ACCEL_NONE, 0, &g->lease);
	}
	for (uint32_t i = 0; ret == ANX_OK && i < pages; i++) {
		g->pages[i] = anx_page_alloc(0);
		if (!g->pages[i]) ret = ANX_ENOMEM;
		else { anx_memset((void *)g->pages[i], 0, ANX_PAGE_SIZE); g->view.physical_pages++; }
	}
	if (ret == ANX_OK) {
		g->lease->mem_used_bytes = g->view.memory_bytes;
		g->view.id = ++sequence; groups[slot] = g; *out = g->view;
	} else {
		free_pages(g); if (g->lease) anx_lease_release(g->lease); release_cells(g); anx_free(g);
	}
	anx_spin_unlock_irqrestore(&group_lock, flags); return ret;
}
int anx_continuation_group_get(uint64_t id, struct anx_continuation_group_view *out)
{
	if (!id || !out) return ANX_EINVAL;
	bool flags; anx_spin_lock_irqsave(&group_lock, &flags);
	struct continuation_group *g = find(id); int ret = inspect(g);
	if (ret == ANX_OK) *out = g->view;
	anx_spin_unlock_irqrestore(&group_lock, flags); return ret;
}
int anx_continuation_group_handoff(uint64_t id, uint64_t epoch, const anx_cid_t *target, struct anx_continuation_group_view *out)
{
	if (anx_cell_current_id()) return ANX_EPERM;
	if (!id || !epoch || !target || !out) return ANX_EINVAL;
	bool flags; anx_spin_lock_irqsave(&group_lock, &flags);
	struct continuation_group *g = find(id);
	int ret = g ? ANX_OK : ANX_ENOENT, member = -1;
	if (ret == ANX_OK && g->view.epoch != epoch) ret = ANX_EBUSY;
	if (ret == ANX_OK) ret = group_current(g);
	if (ret == ANX_OK && active(g)) ret = ANX_EBUSY;
	if (ret == ANX_OK) { member = member_index(g, target); if (member < 0) ret = ANX_EPERM; }
	if (ret == ANX_OK && ((uint32_t)member == g->view.holder || g->members[member]->status != ANX_CELL_CREATED)) ret = ANX_EBUSY;
	if (ret == ANX_OK) ret = member_current(g, member);
	if (ret == ANX_OK && epoch == ~(uint64_t)0) ret = ANX_EFULL;
	if (ret == ANX_OK) { g->view.holder = member; g->view.epoch++; *out = g->view; }
	anx_spin_unlock_irqrestore(&group_lock, flags); return ret;
}
int anx_continuation_group_check(struct anx_cell *cell)
{
	if (!cell) return ANX_EINVAL;
	bool flags; anx_spin_lock_irqsave(&group_lock, &flags);
	int ret = ANX_OK;
	for (uint32_t i = 0; i < ANX_CONTINUATION_GROUP_MAX; i++) if (groups[i]) {
		struct continuation_group *g = groups[i]; int member = member_index(g, &cell->cid);
		if (member < 0) continue;
		if (g->members[member] != cell) { ret = ANX_EPERM; break; }
		ret = group_current(g);
		if (ret == ANX_OK && (uint32_t)member != g->view.holder) ret = ANX_EBUSY;
		if (ret == ANX_OK) ret = member_current(g, member);
		break;
	}
	anx_spin_unlock_irqrestore(&group_lock, flags); return ret;
}
static int memory_access(uint64_t id, uint64_t epoch, uint32_t offset, void *bytes, uint32_t size, bool write)
{
	if (!id || !epoch || !bytes || !size) return ANX_EINVAL;
	bool flags; anx_spin_lock_irqsave(&group_lock, &flags);
	struct continuation_group *g = find(id); int ret = inspect(g);
	const anx_cid_t *caller = anx_cell_current_id();
	if (ret == ANX_OK && caller && anx_uuid_compare(caller, &g->view.members[g->view.holder])) ret = ANX_EPERM;
	if (ret == ANX_OK && g->view.epoch != epoch) ret = ANX_EBUSY;
	if (ret == ANX_OK) ret = group_current(g);
	if (ret == ANX_OK) ret = member_current(g, g->view.holder);
	if (ret == ANX_OK && (offset > g->view.memory_bytes || size > g->view.memory_bytes - offset)) ret = ANX_EINVAL;
	if (ret == ANX_OK) {
		uint8_t *buffer = bytes;
		while (size) {
			uint32_t at = offset % ANX_PAGE_SIZE, n = ANX_PAGE_SIZE - at;
			if (n > size) n = size;
			void *page = (void *)(g->pages[offset / ANX_PAGE_SIZE] + at);
			if (write) anx_memcpy(page, buffer, n); else anx_memcpy(buffer, page, n);
			buffer += n; offset += n; size -= n;
		}
	}
	anx_spin_unlock_irqrestore(&group_lock, flags); return ret;
}
int anx_continuation_group_read(uint64_t id, uint64_t epoch, uint32_t offset, void *bytes, uint32_t size)
{ return memory_access(id, epoch, offset, bytes, size, false); }
int anx_continuation_group_write(uint64_t id, uint64_t epoch, uint32_t offset, const void *bytes, uint32_t size)
{ return memory_access(id, epoch, offset, (void *)bytes, size, true); }
int anx_continuation_group_revoke(uint64_t id, uint64_t epoch, struct anx_continuation_group_view *out)
{
	if (anx_cell_current_id()) return ANX_EPERM;
	if (!id || !epoch || !out) return ANX_EINVAL;
	bool flags; anx_spin_lock_irqsave(&group_lock, &flags);
	struct continuation_group *g = find(id); int ret = g ? ANX_OK : ANX_ENOENT;
	if (ret == ANX_OK && (g->view.epoch != epoch || g->view.revoked || active(g))) ret = ANX_EBUSY;
	if (ret == ANX_OK && epoch == ~(uint64_t)0) ret = ANX_EFULL;
	if (ret == ANX_OK) {
		uint64_t used = g->lease->mem_used_bytes; g->lease->mem_used_bytes = 0;
		ret = anx_lease_revoke(g->lease);
		if (ret != ANX_OK) g->lease->mem_used_bytes = used;
		else { free_pages(g); g->view.revoked = true; g->view.epoch++; *out = g->view; }
	}
	anx_spin_unlock_irqrestore(&group_lock, flags); return ret;
}
int anx_continuation_group_destroy(uint64_t id)
{
	if (anx_cell_current_id()) return ANX_EPERM;
	if (!id) return ANX_EINVAL;
	bool flags; anx_spin_lock_irqsave(&group_lock, &flags);
	struct continuation_group *g = find(id); int ret = g ? ANX_OK : ANX_ENOENT;
	if (ret == ANX_OK && active(g)) ret = ANX_EBUSY;
	if (ret == ANX_OK) {
		uint64_t used = g->lease->mem_used_bytes; g->lease->mem_used_bytes = 0;
		ret = anx_lease_release(g->lease);
		if (ret != ANX_OK) g->lease->mem_used_bytes = used;
		else {
			free_pages(g); release_cells(g);
			for (uint32_t i = 0; i < ANX_CONTINUATION_GROUP_MAX; i++) if (groups[i] == g) groups[i] = NULL;
			anx_memset(g, 0, sizeof(*g)); anx_free(g);
		}
	}
	anx_spin_unlock_irqrestore(&group_lock, flags); return ret;
}
