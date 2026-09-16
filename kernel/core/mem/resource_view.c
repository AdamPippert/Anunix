/* Logical immutable records outlive relocation of their private physical pages. */
#include <anx/resource_view.h>
#include <anx/cell.h>
#include <anx/alloc.h>
#include <anx/page.h>
#include <anx/spinlock.h>
#include <anx/string.h>
#include <anx/uuid.h>
#include <anx/crypto.h>

struct view_record { anx_oid_t id; uint32_t offset, size, refs; bool used; uint8_t digest[32]; };
struct view_alias { anx_oid_t id; uint32_t record; bool used; };
struct resource_pool {
	anx_oid_t id;
	struct anx_cell *owner;
	uint32_t capacity, page_count, tail;
	uintptr_t pages[ANX_RESOURCE_PAGES_MAX];
	struct view_record records[ANX_RESOURCE_RECORDS_MAX];
	struct view_alias aliases[ANX_RESOURCE_ALIASES_MAX];
};
static struct resource_pool *pools[ANX_RESOURCE_POOLS_MAX];
static struct anx_spinlock view_lock = ANX_SPINLOCK_INIT;
static uint64_t sequence;
#if defined(ANX_RESEARCH_TEST) || defined(ANX_HOST_TEST)
static bool move_fault;
#endif

/* All registry helpers run under view_lock; identifiers never repeat in this boot. */
static anx_oid_t next_id(void)
{
	return (anx_oid_t){ .hi = 0x414e585256494557ULL, .lo = ++sequence };
}
static struct resource_pool *find_pool(const anx_oid_t *id)
{
	for (uint32_t i = 0; id && i < ANX_RESOURCE_POOLS_MAX; i++)
		if (pools[i] && !anx_uuid_compare(id, &pools[i]->id)) return pools[i];
	return NULL;
}
static struct view_alias *find_alias(const anx_oid_t *id, struct resource_pool **pool)
{
	for (uint32_t i = 0; id && i < ANX_RESOURCE_POOLS_MAX; i++) {
		if (!pools[i]) continue;
		for (uint32_t j = 0; j < ANX_RESOURCE_ALIASES_MAX; j++)
			if (pools[i]->aliases[j].used && !anx_uuid_compare(id, &pools[i]->aliases[j].id)) {
				*pool = pools[i]; return &pools[i]->aliases[j];
			}
	}
	return NULL;
}
static int authorized(const struct resource_pool *pool, bool new_reference)
{
	const anx_cid_t *caller = anx_cell_current_id();
	if (caller && anx_uuid_compare(caller, &pool->owner->cid)) return ANX_EPERM;
	if (new_reference && anx_cell_status_terminal(pool->owner->status)) return ANX_EPERM;
	return ANX_OK;
}
static void stats(const struct resource_pool *pool, struct anx_resource_pool_stats *out)
{
	struct anx_resource_pool_stats s = { .physical_pages = pool->page_count };
	for (uint32_t i = 0; i < ANX_RESOURCE_RECORDS_MAX; i++) if (pool->records[i].used && pool->records[i].refs) {
		s.live_records++; s.live_aliases += pool->records[i].refs; s.live_bytes += pool->records[i].size;
	}
	s.dead_bytes = pool->tail - s.live_bytes;
	*out = s;
}
static void free_page(uintptr_t page)
{
	anx_memset((void *)page, 0, ANX_PAGE_SIZE);
	anx_page_free(page, 0);
}
static void copy_out(const uintptr_t *pages, uint32_t offset, void *buffer, uint32_t size)
{
	uint8_t *dst = buffer;
	while (size) {
		uint32_t at = offset % ANX_PAGE_SIZE, n = ANX_PAGE_SIZE - at;
		if (n > size) n = size;
		anx_memcpy(dst, (void *)(pages[offset / ANX_PAGE_SIZE] + at), n);
		dst += n; offset += n; size -= n;
	}
}
static void copy_in(uintptr_t *pages, uint32_t offset, const void *buffer, uint32_t size)
{
	const uint8_t *src = buffer;
	while (size) {
		uint32_t at = offset % ANX_PAGE_SIZE, n = ANX_PAGE_SIZE - at;
		if (n > size) n = size;
		anx_memcpy((void *)(pages[offset / ANX_PAGE_SIZE] + at), src, n);
		src += n; offset += n; size -= n;
	}
}

static void record_digest(const uintptr_t *pages, uint32_t offset, uint32_t size, uint8_t out[32])
{
	struct anx_sha256_ctx hash;
	anx_sha256_init(&hash);
	while (size) {
		uint32_t at = offset % ANX_PAGE_SIZE, n = ANX_PAGE_SIZE - at;
		if (n > size) n = size;
		anx_sha256_update(&hash, (void *)(pages[offset / ANX_PAGE_SIZE] + at), n);
		offset += n; size -= n;
	}
	anx_sha256_final(&hash, out);
}

int anx_resource_pool_create(const anx_cid_t *owner, uint32_t capacity, anx_oid_t *out)
{
	if (anx_cell_current_id()) return ANX_EPERM;
	if (!owner || !out || !capacity || capacity > ANX_RESOURCE_PAGES_MAX) return ANX_EINVAL;
	struct anx_cell *cell = anx_cell_store_lookup(owner);
	if (!cell) return ANX_ENOENT;
	if (anx_cell_status_terminal(cell->status)) { anx_cell_store_release(cell); return ANX_EPERM; }
	struct resource_pool *pool = anx_zalloc(sizeof(*pool));
	if (!pool) { anx_cell_store_release(cell); return ANX_ENOMEM; }
	bool flags;
	uint32_t slot;
	anx_spin_lock_irqsave(&view_lock, &flags);
	for (slot = 0; slot < ANX_RESOURCE_POOLS_MAX; slot++) if (!pools[slot]) break;
	if (slot == ANX_RESOURCE_POOLS_MAX || sequence == ~(uint64_t)0) {
		anx_spin_unlock_irqrestore(&view_lock, flags);
		anx_free(pool); anx_cell_store_release(cell); return ANX_EFULL;
	}
	pool->id = next_id(); pool->owner = cell; pool->capacity = capacity;
	pools[slot] = pool; *out = pool->id;
	anx_spin_unlock_irqrestore(&view_lock, flags);
	return ANX_OK;
}

int anx_resource_pool_destroy(const anx_oid_t *id)
{
	bool flags;
	if (anx_cell_current_id()) return ANX_EPERM;
	if (!id) return ANX_EINVAL;
	anx_spin_lock_irqsave(&view_lock, &flags);
	struct resource_pool *pool = find_pool(id);
	if (!pool) { anx_spin_unlock_irqrestore(&view_lock, flags); return ANX_ENOENT; }
	for (uint32_t i = 0; i < ANX_RESOURCE_ALIASES_MAX; i++) if (pool->aliases[i].used) {
		anx_spin_unlock_irqrestore(&view_lock, flags); return ANX_EBUSY;
	}
	for (uint32_t i = 0; i < ANX_RESOURCE_POOLS_MAX; i++) if (pools[i] == pool) pools[i] = NULL;
	for (uint32_t i = 0; i < pool->page_count; i++) free_page(pool->pages[i]);
	anx_spin_unlock_irqrestore(&view_lock, flags);
	anx_cell_store_release(pool->owner); anx_free(pool);
	return ANX_OK;
}

int anx_resource_pool_stats(const anx_oid_t *id, struct anx_resource_pool_stats *out)
{
	bool flags;
	int ret;
	if (!id || !out) return ANX_EINVAL;
	anx_spin_lock_irqsave(&view_lock, &flags);
	struct resource_pool *pool = find_pool(id);
	ret = pool ? authorized(pool, false) : ANX_ENOENT;
	if (ret == ANX_OK) stats(pool, out);
	anx_spin_unlock_irqrestore(&view_lock, flags);
	return ret;
}

int anx_resource_view_insert(const anx_oid_t *id, const void *data, uint32_t size, anx_oid_t *out)
{
	bool flags;
	int ret;
	uint32_t record, alias, old_count, needed, allocated;
	if (!id || !data || !size || size > ANX_RESOURCE_ITEM_MAX || !out) return ANX_EINVAL;
	anx_spin_lock_irqsave(&view_lock, &flags);
	struct resource_pool *pool = find_pool(id);
	ret = pool ? authorized(pool, true) : ANX_ENOENT;
	if (ret != ANX_OK) goto done;
	for (record = 0; record < ANX_RESOURCE_RECORDS_MAX; record++) if (!pool->records[record].used) break;
	for (alias = 0; alias < ANX_RESOURCE_ALIASES_MAX; alias++) if (!pool->aliases[alias].used) break;
	if (record == ANX_RESOURCE_RECORDS_MAX || alias == ANX_RESOURCE_ALIASES_MAX || sequence > ~(uint64_t)0 - 2) {
		ret = ANX_EFULL; goto done;
	}
	if (size > pool->capacity * ANX_PAGE_SIZE - pool->tail) { ret = ANX_ENOMEM; goto done; }
	needed = (pool->tail + size + ANX_PAGE_SIZE - 1) / ANX_PAGE_SIZE;
	old_count = pool->page_count;
	for (allocated = old_count; allocated < needed; allocated++) {
		pool->pages[allocated] = anx_page_alloc(0);
		if (!pool->pages[allocated]) break;
		anx_memset((void *)pool->pages[allocated], 0, ANX_PAGE_SIZE);
	}
	if (allocated != needed) {
		for (uint32_t i = old_count; i < allocated; i++) { free_page(pool->pages[i]); pool->pages[i] = 0; }
		ret = ANX_ENOMEM; goto done;
	}
	copy_in(pool->pages, pool->tail, data, size);
	pool->records[record] = (struct view_record){ .id = next_id(), .offset = pool->tail, .size = size, .refs = 1, .used = true };
	record_digest(pool->pages, pool->tail, size, pool->records[record].digest);
	pool->aliases[alias] = (struct view_alias){ next_id(), record, true };
	pool->tail += size; pool->page_count = needed; *out = pool->aliases[alias].id;
done:
	anx_spin_unlock_irqrestore(&view_lock, flags);
	return ret;
}

int anx_resource_view_clone(const anx_oid_t *id, anx_oid_t *out)
{
	bool flags;
	int ret;
	struct resource_pool *pool = NULL;
	if (!id || !out) return ANX_EINVAL;
	anx_spin_lock_irqsave(&view_lock, &flags);
	struct view_alias *alias = find_alias(id, &pool);
	ret = alias ? authorized(pool, true) : ANX_ENOENT;
	if (ret == ANX_OK) {
		uint32_t slot;
		for (slot = 0; slot < ANX_RESOURCE_ALIASES_MAX; slot++) if (!pool->aliases[slot].used) break;
		if (slot == ANX_RESOURCE_ALIASES_MAX || sequence == ~(uint64_t)0) ret = ANX_EFULL;
		else {
			pool->aliases[slot] = (struct view_alias){ next_id(), alias->record, true };
			pool->records[alias->record].refs++; *out = pool->aliases[slot].id;
		}
	}
	anx_spin_unlock_irqrestore(&view_lock, flags);
	return ret;
}

int anx_resource_view_release(const anx_oid_t *id)
{
	bool flags;
	int ret;
	struct resource_pool *pool = NULL;
	if (!id) return ANX_EINVAL;
	anx_spin_lock_irqsave(&view_lock, &flags);
	struct view_alias *alias = find_alias(id, &pool);
	ret = alias ? authorized(pool, false) : ANX_ENOENT;
	if (ret == ANX_OK) { pool->records[alias->record].refs--; anx_memset(alias, 0, sizeof(*alias)); }
	anx_spin_unlock_irqrestore(&view_lock, flags);
	return ret;
}

int anx_resource_view_info(const anx_oid_t *id, struct anx_resource_view_info *out)
{
	bool flags;
	int ret;
	struct resource_pool *pool = NULL;
	if (!id || !out) return ANX_EINVAL;
	anx_spin_lock_irqsave(&view_lock, &flags);
	struct view_alias *alias = find_alias(id, &pool);
	ret = alias ? authorized(pool, false) : ANX_ENOENT;
	if (ret == ANX_OK) {
		struct view_record *r = &pool->records[alias->record];
		*out = (struct anx_resource_view_info){ r->id, pool->owner->cid, r->size };
	}
	anx_spin_unlock_irqrestore(&view_lock, flags);
	return ret;
}

int anx_resource_view_read(const anx_oid_t *id, uint32_t offset, void *data, uint32_t size)
{
	bool flags;
	int ret;
	struct resource_pool *pool = NULL;
	if (!id || (!data && size)) return ANX_EINVAL;
	anx_spin_lock_irqsave(&view_lock, &flags);
	struct view_alias *alias = find_alias(id, &pool);
	ret = alias ? authorized(pool, false) : ANX_ENOENT;
	if (ret == ANX_OK) {
		struct view_record *r = &pool->records[alias->record];
		if (offset > r->size || size > r->size - offset) ret = ANX_EINVAL;
		else {
			uint8_t digest[32];
			record_digest(pool->pages, r->offset, r->size, digest);
			if (anx_memcmp(digest, r->digest, sizeof(digest))) ret = ANX_EIO;
			else { copy_out(pool->pages, r->offset + offset, data, size); ret = (int)size; }
		}
	}
	anx_spin_unlock_irqrestore(&view_lock, flags);
	return ret;
}

int anx_resource_pool_compact(const anx_oid_t *id, uint32_t minimum_dead_bytes,
		uint32_t headroom_pages, struct anx_resource_pool_stats *out)
{
	uintptr_t destination[ANX_RESOURCE_PAGES_MAX] = {0};
	uint32_t offsets[ANX_RESOURCE_RECORDS_MAX] = {0};
	struct anx_resource_pool_stats before;
	uint32_t needed, allocated = 0, cursor = 0;
	bool flags;
	int ret = ANX_OK;
	if (anx_cell_current_id()) return ANX_EPERM;
	if (!id || !out || headroom_pages > ANX_RESOURCE_PAGES_MAX) return ANX_EINVAL;
	anx_spin_lock_irqsave(&view_lock, &flags);
	struct resource_pool *pool = find_pool(id);
	if (!pool) { ret = ANX_ENOENT; goto done; }
	stats(pool, &before);
	needed = (before.live_bytes + ANX_PAGE_SIZE - 1) / ANX_PAGE_SIZE;
	if (before.dead_bytes < minimum_dead_bytes || needed >= pool->page_count) { ret = ANX_EBUSY; goto done; }
	if (needed > headroom_pages) { ret = ANX_ENOMEM; goto done; }
	for (; allocated < needed; allocated++) {
		destination[allocated] = anx_page_alloc(0);
		if (!destination[allocated]) { ret = ANX_ENOMEM; goto done; }
		anx_memset((void *)destination[allocated], 0, ANX_PAGE_SIZE);
	}
	for (uint32_t i = 0; i < ANX_RESOURCE_RECORDS_MAX; i++) {
		const struct view_record *r = &pool->records[i];
		if (!r->used || !r->refs) continue;
		offsets[i] = cursor;
		uint32_t source_offset = r->offset, left = r->size;
		while (left) {
			uint32_t from = source_offset % ANX_PAGE_SIZE, to = cursor % ANX_PAGE_SIZE;
			uint32_t count = ANX_PAGE_SIZE - from;
			if (count > ANX_PAGE_SIZE - to) count = ANX_PAGE_SIZE - to;
			if (count > left) count = left;
			anx_memcpy((void *)(destination[cursor / ANX_PAGE_SIZE] + to),
				(void *)(pool->pages[source_offset / ANX_PAGE_SIZE] + from), count);
			source_offset += count; cursor += count; left -= count;
		}
		uint8_t digest[32];
		record_digest(destination, offsets[i], r->size, digest);
		if (anx_memcmp(digest, r->digest, sizeof(digest))) { ret = ANX_EIO; goto done; }
	}
	/* No failure points remain. Readers and alias changes share this lock. */
	for (uint32_t i = 0; i < pool->page_count; i++) free_page(pool->pages[i]);
	anx_memset(pool->pages, 0, sizeof(pool->pages));
	for (uint32_t i = 0; i < needed; i++) pool->pages[i] = destination[i];
	pool->page_count = needed; pool->tail = before.live_bytes; allocated = 0;
	for (uint32_t i = 0; i < ANX_RESOURCE_RECORDS_MAX; i++) {
		if (pool->records[i].refs) pool->records[i].offset = offsets[i];
		else anx_memset(&pool->records[i], 0, sizeof(pool->records[i]));
	}
	stats(pool, out);
done:
	for (uint32_t i = 0; i < allocated; i++) free_page(destination[i]);
	anx_spin_unlock_irqrestore(&view_lock, flags);
	return ret;
}

static void zero_range(uintptr_t *pages, uint32_t offset, uint32_t size)
{
	while (size) {
		uint32_t at = offset % ANX_PAGE_SIZE, n = ANX_PAGE_SIZE - at;
		if (n > size) n = size;
		anx_memset((void *)(pages[offset / ANX_PAGE_SIZE] + at), 0, n);
		offset += n; size -= n;
	}
}

int anx_resource_view_move(const anx_oid_t *handle, const anx_oid_t *destination_pool,
		uint32_t headroom_pages, struct anx_resource_move_result *out)
{
	struct resource_pool *source = NULL;
	uint8_t digest[32];
	uint32_t record, free_aliases = 0, source_aliases = 0, needed, old_count, allocated;
	bool flags;
	int ret = ANX_OK;
	if (anx_cell_current_id()) return ANX_EPERM;
	if (!handle || !destination_pool || !out || headroom_pages > ANX_RESOURCE_PAGES_MAX) return ANX_EINVAL;
	anx_spin_lock_irqsave(&view_lock, &flags);
	struct view_alias *alias = find_alias(handle, &source);
	struct resource_pool *destination = find_pool(destination_pool);
	if (!alias || !destination) { ret = ANX_ENOENT; goto done; }
	if (source == destination) { ret = ANX_EBUSY; goto done; }
	if (anx_uuid_compare(&source->owner->cid, &destination->owner->cid)) { ret = ANX_EPERM; goto done; }
	struct view_record *r = &source->records[alias->record];
	uint32_t source_record = alias->record;
	for (record = 0; record < ANX_RESOURCE_RECORDS_MAX; record++) if (!destination->records[record].used) break;
	for (uint32_t i = 0; i < ANX_RESOURCE_ALIASES_MAX; i++) {
		if (!destination->aliases[i].used) free_aliases++;
		if (source->aliases[i].used && source->aliases[i].record == source_record) source_aliases++;
	}
	if (!r->used || !r->refs || r->refs != source_aliases) { ret = ANX_EIO; goto done; }
	if (record == ANX_RESOURCE_RECORDS_MAX || free_aliases < r->refs) { ret = ANX_EFULL; goto done; }
	if (r->size > destination->capacity * ANX_PAGE_SIZE - destination->tail) { ret = ANX_ENOMEM; goto done; }
	needed = (destination->tail + r->size + ANX_PAGE_SIZE - 1) / ANX_PAGE_SIZE;
	old_count = destination->page_count;
	if (needed - old_count > headroom_pages) { ret = ANX_ENOMEM; goto done; }
	record_digest(source->pages, r->offset, r->size, digest);
	if (anx_memcmp(digest, r->digest, sizeof(digest))) { ret = ANX_EIO; goto done; }
	for (allocated = old_count; allocated < needed; allocated++) {
		destination->pages[allocated] = anx_page_alloc(0);
		if (!destination->pages[allocated]) { ret = ANX_ENOMEM; goto rollback; }
		anx_memset((void *)destination->pages[allocated], 0, ANX_PAGE_SIZE);
	}
	uint32_t from = r->offset, to = destination->tail, left = r->size;
	while (left) {
		uint32_t source_at = from % ANX_PAGE_SIZE, target_at = to % ANX_PAGE_SIZE;
		uint32_t count = ANX_PAGE_SIZE - source_at;
		if (count > ANX_PAGE_SIZE - target_at) count = ANX_PAGE_SIZE - target_at;
		if (count > left) count = left;
		anx_memcpy((void *)(destination->pages[to / ANX_PAGE_SIZE] + target_at),
			(void *)(source->pages[from / ANX_PAGE_SIZE] + source_at), count);
		from += count; to += count; left -= count;
	}
#if defined(ANX_RESEARCH_TEST) || defined(ANX_HOST_TEST)
	if (move_fault) {
		*(uint8_t *)(destination->pages[destination->tail / ANX_PAGE_SIZE] + destination->tail % ANX_PAGE_SIZE) ^= 1;
		move_fault = false;
	}
#endif
	record_digest(destination->pages, destination->tail, r->size, digest);
	if (anx_memcmp(digest, r->digest, sizeof(digest))) {
		zero_range(destination->pages, destination->tail, r->size);
		ret = ANX_EIO; goto rollback;
	}
	/* Copy and verification succeeded. No failure points remain under this lock. */
	struct anx_resource_move_result result = { source->id, destination->id, r->size, needed - old_count };
	destination->records[record] = *r;
	destination->records[record].offset = destination->tail;
	destination->tail += r->size; destination->page_count = needed;
	uint32_t target = 0;
	for (uint32_t i = 0; i < ANX_RESOURCE_ALIASES_MAX; i++) {
		struct view_alias *a = &source->aliases[i];
		if (!a->used || a->record != source_record) continue;
		while (destination->aliases[target].used) target++;
		destination->aliases[target] = *a;
		destination->aliases[target].record = record;
		anx_memset(a, 0, sizeof(*a)); target++;
	}
	zero_range(source->pages, r->offset, r->size);
	anx_memset(r, 0, sizeof(*r));
	*out = result;
	goto done;
rollback:
	for (uint32_t i = old_count; i < allocated; i++) {
		free_page(destination->pages[i]); destination->pages[i] = 0;
	}
done:
	anx_spin_unlock_irqrestore(&view_lock, flags);
	return ret;
}

#if defined(ANX_RESEARCH_TEST) || defined(ANX_HOST_TEST)
int anx_resource_view_test_move_fault(bool enabled)
{
	bool flags;
	if (anx_cell_current_id()) return ANX_EPERM;
	anx_spin_lock_irqsave(&view_lock, &flags);
	move_fault = enabled;
	anx_spin_unlock_irqrestore(&view_lock, flags);
	return ANX_OK;
}

int anx_resource_view_test_corrupt(const anx_oid_t *id)
{
	bool flags;
	struct resource_pool *pool = NULL;
	if (anx_cell_current_id()) return ANX_EPERM;
	anx_spin_lock_irqsave(&view_lock, &flags);
	struct view_alias *alias = find_alias(id, &pool);
	if (alias) {
		uint32_t offset = pool->records[alias->record].offset;
		*(uint8_t *)(pool->pages[offset / ANX_PAGE_SIZE] + offset % ANX_PAGE_SIZE) ^= 1;
	}
	anx_spin_unlock_irqrestore(&view_lock, flags);
	return alias ? ANX_OK : ANX_ENOENT;
}
#endif
