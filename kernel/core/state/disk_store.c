/*
 * disk_store.c — Journaled on-disk object store.
 *
 * Persists State Objects to a virtio-blk device with a write-ahead
 * journal for crash recovery. Objects are stored in a data region
 * and indexed by OID in a linear index table.
 *
 * Disk layout:
 *   Sector 0:       Superblock
 *   Sectors 1-256:  Journal (write-ahead log)
 *   Sectors 257-512: Index (OID → sector mapping)
 *   Sectors 513+:   Object data region
 */

#include <anx/types.h>
#include <anx/objstore_disk.h>
#include <anx/virtio_blk.h>
#include <anx/alloc.h>
#include <anx/string.h>
#include <anx/arch.h>
#include <anx/kprintf.h>

/* In-memory cache of superblock and index */
static struct anx_disk_super super;
static struct anx_disk_index_entry *index_cache;
static uint32_t index_count;
static bool mounted;
static uint32_t next_txn_id = 1;

/* Sector-aligned buffer for I/O */
static uint8_t sector_buf[512] __attribute__((aligned(512)));

/* --- Superblock --- */

static int read_super(void)
{
	int ret;

	ret = anx_blk_read(ANX_SUPER_SECTOR, 1, &super);
	if (ret != ANX_OK)
		return ret;
	if (super.magic != ANX_DISK_MAGIC)
		return ANX_EINVAL;
	if (super.version != ANX_DISK_VERSION)
		return ANX_EINVAL;
	return ANX_OK;
}

static int write_super(void)
{
	return anx_blk_write(ANX_SUPER_SECTOR, 1, &super);
}

/* --- Index --- */

static int load_index(void)
{
	uint32_t buf_size;
	uint8_t *buf;
	uint32_t i, j;
	int ret;

	buf_size = ANX_INDEX_SECTORS * 512;
	buf = anx_alloc(buf_size);
	if (!buf)
		return ANX_ENOMEM;

	ret = anx_blk_read(ANX_INDEX_START, ANX_INDEX_SECTORS, buf);
	if (ret != ANX_OK) {
		anx_free(buf);
		return ret;
	}

	/* Count active entries */
	index_count = 0;
	{
		struct anx_disk_index_entry *entries;

		entries = (struct anx_disk_index_entry *)buf;
		for (i = 0; i < ANX_INDEX_MAX_ENTRIES; i++) {
			if (entries[i].flags & 0x1)
				index_count++;
		}
	}

	/* Allocate in-memory cache */
	if (index_cache)
		anx_free(index_cache);

	if (index_count > 0) {
		index_cache = anx_alloc(index_count *
					sizeof(struct anx_disk_index_entry));
		if (!index_cache) {
			anx_free(buf);
			return ANX_ENOMEM;
		}

		j = 0;
		{
			struct anx_disk_index_entry *entries;

			entries = (struct anx_disk_index_entry *)buf;
			for (i = 0; i < ANX_INDEX_MAX_ENTRIES && j < index_count; i++) {
				if (entries[i].flags & 0x1)
					index_cache[j++] = entries[i];
			}
		}
	} else {
		index_cache = NULL;
	}

	anx_free(buf);
	return ANX_OK;
}

static int flush_index(void)
{
	uint8_t *buf;
	uint32_t buf_size;
	uint32_t i;
	int ret;

	buf_size = ANX_INDEX_SECTORS * 512;
	buf = anx_zalloc(buf_size);
	if (!buf)
		return ANX_ENOMEM;

	/* Write active entries into buffer */
	{
		struct anx_disk_index_entry *entries;

		entries = (struct anx_disk_index_entry *)buf;
		for (i = 0; i < index_count && i < ANX_INDEX_MAX_ENTRIES; i++)
			entries[i] = index_cache[i];
	}

	ret = anx_blk_write(ANX_INDEX_START, ANX_INDEX_SECTORS, buf);
	anx_free(buf);
	return ret;
}

static struct anx_disk_index_entry *find_index_entry(const anx_oid_t *oid)
{
	uint32_t i;

	for (i = 0; i < index_count; i++) {
		if (index_cache[i].oid.hi == oid->hi &&
		    index_cache[i].oid.lo == oid->lo)
			return &index_cache[i];
	}
	return NULL;
}

static int add_index_entry(const struct anx_disk_index_entry *entry)
{
	struct anx_disk_index_entry *new_cache;

	if (index_count >= ANX_INDEX_MAX_ENTRIES)
		return ANX_ENOMEM;

	new_cache = anx_alloc((index_count + 1) *
			       sizeof(struct anx_disk_index_entry));
	if (!new_cache)
		return ANX_ENOMEM;

	if (index_cache && index_count > 0)
		anx_memcpy(new_cache, index_cache,
			   index_count * sizeof(struct anx_disk_index_entry));
	new_cache[index_count] = *entry;
	index_count++;

	if (index_cache)
		anx_free(index_cache);
	index_cache = new_cache;
	return ANX_OK;
}

/* --- Journal --- */

static int write_journal_entry(const struct anx_journal_entry *entry)
{
	uint32_t slot = super.journal_head;
	uint32_t sector = ANX_JOURNAL_START +
			  slot / ANX_JRNL_ENTRIES_PER_SECTOR;
	uint32_t offset = (slot % ANX_JRNL_ENTRIES_PER_SECTOR) * 64;
	int ret;

	/* Read the sector, modify entry, write back */
	ret = anx_blk_read(sector, 1, sector_buf);
	if (ret != ANX_OK)
		return ret;

	anx_memcpy(sector_buf + offset, entry, sizeof(*entry));

	ret = anx_blk_write(sector, 1, sector_buf);
	if (ret != ANX_OK)
		return ret;

	super.journal_head = (slot + 1) % ANX_JRNL_MAX_ENTRIES;
	return ANX_OK;
}

static int journal_begin_write(const anx_oid_t *oid, uint64_t data_sector,
				uint32_t data_sectors, uint32_t obj_size)
{
	struct anx_journal_entry entry;

	anx_memset(&entry, 0, sizeof(entry));
	entry.txn_id = next_txn_id;
	entry.op = ANX_JRNL_WRITE_OBJ;
	entry.oid = *oid;
	entry.data_sector = data_sector;
	entry.data_sectors = data_sectors;
	entry.obj_size = obj_size;

	return write_journal_entry(&entry);
}

static int journal_commit(void)
{
	struct anx_journal_entry entry;
	int ret;

	anx_memset(&entry, 0, sizeof(entry));
	entry.txn_id = next_txn_id;
	entry.op = ANX_JRNL_COMMIT;

	ret = write_journal_entry(&entry);
	if (ret != ANX_OK)
		return ret;

	super.journal_committed = super.journal_head;
	next_txn_id++;

	return write_super();
}

/* --- Public API --- */

int anx_disk_format(const char *label)
{
	uint8_t *zero_buf;
	uint32_t i;
	int ret;

	if (!anx_blk_ready())
		return ANX_EIO;

	kprintf("disk: formatting...\n");

	/* Zero superblock + journal + index */
	zero_buf = anx_zalloc(512);
	if (!zero_buf)
		return ANX_ENOMEM;

	for (i = 0; i < ANX_DATA_START; i++) {
		ret = anx_blk_write(i, 1, zero_buf);
		if (ret != ANX_OK) {
			anx_free(zero_buf);
			return ret;
		}
	}
	anx_free(zero_buf);

	/* Write superblock */
	anx_memset(&super, 0, sizeof(super));
	super.magic = ANX_DISK_MAGIC;
	super.version = ANX_DISK_VERSION;
	super.total_sectors = anx_blk_capacity();
	super.obj_count = 0;
	super.next_data_sector = ANX_DATA_START;
	super.journal_head = 0;
	super.journal_committed = 0;

	/* Generate a simple UUID from timestamp */
	{
		uint64_t t = arch_time_now();

		anx_memcpy(super.uuid, &t, 8);
		t = arch_time_now();
		anx_memcpy(super.uuid + 8, &t, 8);
	}

	if (label)
		anx_strlcpy(super.label, label, sizeof(super.label));
	else
		anx_strlcpy(super.label, "anunix", sizeof(super.label));

	ret = write_super();
	if (ret != ANX_OK)
		return ret;

	/* Reset in-memory state */
	if (index_cache) {
		anx_free(index_cache);
		index_cache = NULL;
	}
	index_count = 0;
	mounted = true;
	next_txn_id = 1;

	kprintf("disk: formatted '%s' (%u MiB)\n",
		super.label,
		(uint32_t)(super.total_sectors * 512 / (1024 * 1024)));
	return ANX_OK;
}

int anx_disk_store_init(void)
{
	int ret;

	if (!anx_blk_ready())
		return ANX_ENOENT;

	ret = read_super();
	if (ret != ANX_OK) {
		/* Not formatted — caller should format first */
		return ret;
	}

	ret = load_index();
	if (ret != ANX_OK)
		return ret;

	/* Check if journal needs recovery */
	if (super.journal_head != super.journal_committed) {
		kprintf("disk: journal recovery needed\n");
		ret = anx_disk_recover();
		if (ret != ANX_OK)
			return ret;
	}

	mounted = true;
	kprintf("disk: mounted '%s' (%u objects)\n",
		super.label, (uint32_t)super.obj_count);
	return ANX_OK;
}

int anx_disk_write_obj(const anx_oid_t *oid, uint32_t obj_type,
			const void *payload, uint32_t payload_size)
{
	uint32_t sectors_needed;
	uint64_t data_sector;
	uint8_t *write_buf;
	struct anx_disk_index_entry idx_entry;
	int ret;

	if (!mounted)
		return ANX_EIO;

	sectors_needed = (payload_size + 511) / 512;
	if (sectors_needed == 0)
		sectors_needed = 1;

	data_sector = super.next_data_sector;
	if (data_sector + sectors_needed > super.total_sectors)
		return ANX_ENOMEM;

	/* Journal the write intent */
	ret = journal_begin_write(oid, data_sector, sectors_needed,
				  payload_size);
	if (ret != ANX_OK)
		return ret;

	/* Write object data */
	write_buf = anx_zalloc(sectors_needed * 512);
	if (!write_buf)
		return ANX_ENOMEM;
	anx_memcpy(write_buf, payload, payload_size);

	ret = anx_blk_write(data_sector, sectors_needed, write_buf);
	anx_free(write_buf);
	if (ret != ANX_OK)
		return ret;

	/* Update index */
	anx_memset(&idx_entry, 0, sizeof(idx_entry));
	idx_entry.oid = *oid;
	idx_entry.data_sector = data_sector;
	idx_entry.data_sectors = sectors_needed;
	idx_entry.payload_size = payload_size;
	idx_entry.obj_type = obj_type;
	idx_entry.flags = 0x1;	/* active */

	/* Check if this OID already exists (update in place) */
	{
		struct anx_disk_index_entry *existing;

		existing = find_index_entry(oid);
		if (existing) {
			*existing = idx_entry;
		} else {
			ret = add_index_entry(&idx_entry);
			if (ret != ANX_OK)
				return ret;
			super.obj_count++;
		}
	}

	super.next_data_sector = data_sector + sectors_needed;

	/* Flush index and commit journal */
	ret = flush_index();
	if (ret != ANX_OK)
		return ret;

	return journal_commit();
}

int anx_disk_read_obj(const anx_oid_t *oid,
		       void *payload_buf, uint32_t buf_size,
		       uint32_t *actual_size, uint32_t *obj_type)
{
	struct anx_disk_index_entry *entry;
	uint8_t *read_buf;
	uint32_t copy_size;
	int ret;

	if (!mounted)
		return ANX_EIO;

	entry = find_index_entry(oid);
	if (!entry)
		return ANX_ENOENT;

	read_buf = anx_alloc(entry->data_sectors * 512);
	if (!read_buf)
		return ANX_ENOMEM;

	ret = anx_blk_read(entry->data_sector, entry->data_sectors, read_buf);
	if (ret != ANX_OK) {
		anx_free(read_buf);
		return ret;
	}

	copy_size = entry->payload_size;
	if (copy_size > buf_size)
		copy_size = buf_size;
	anx_memcpy(payload_buf, read_buf, copy_size);
	anx_free(read_buf);

	if (actual_size)
		*actual_size = entry->payload_size;
	if (obj_type)
		*obj_type = entry->obj_type;

	return ANX_OK;
}

int anx_disk_delete_obj(const anx_oid_t *oid)
{
	struct anx_disk_index_entry *entry;
	int ret;

	if (!mounted)
		return ANX_EIO;

	entry = find_index_entry(oid);
	if (!entry)
		return ANX_ENOENT;

	entry->flags = 0;	/* mark inactive */
	super.obj_count--;

	ret = flush_index();
	if (ret != ANX_OK)
		return ret;

	return write_super();
}

bool anx_disk_obj_exists(const anx_oid_t *oid)
{
	if (!mounted)
		return false;
	return find_index_entry(oid) != NULL;
}

int anx_disk_stats(uint64_t *obj_count, uint64_t *used_sectors,
		    uint64_t *total_sectors)
{
	if (!mounted)
		return ANX_EIO;

	if (obj_count)
		*obj_count = super.obj_count;
	if (used_sectors)
		*used_sectors = super.next_data_sector;
	if (total_sectors)
		*total_sectors = super.total_sectors;
	return ANX_OK;
}

int anx_disk_recover(void)
{
	/*
	 * Simple recovery: find the last COMMIT marker in the journal
	 * and set journal_committed = journal_head to that point.
	 * Uncommitted writes are discarded (the index wasn't flushed).
	 *
	 * For Phase 1, just reset the journal pointers. Full replay
	 * comes when we have more complex multi-step transactions.
	 */
	super.journal_committed = super.journal_head;
	kprintf("disk: journal recovered (reset to head)\n");

	/* Reload index from disk (authoritative after crash) */
	return load_index();
}
