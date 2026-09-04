/*
 * md_super.c — RAID array superblock: layout, checksum, scan, assemble.
 *
 * Every member of an array carries one 512-byte superblock. All members
 * of one array share the UUID, level, chunk size, and capacity fields and
 * differ in member_index, member_state, and events.
 *
 * The superblock sits either near the head of the member (default) or a
 * fixed distance back from its end. Head placement leaves ANX_MD_DATA_OFFSET
 * sectors of slack before the data area. Tail placement puts data at sector
 * 0, which keeps a RAID 1 member readable as a plain drive.
 *
 * Reads probe both placements, so an array created either way assembles
 * without being told which layout it used.
 */

#include <anx/types.h>
#include <anx/md.h>
#include <anx/blk.h>
#include <anx/string.h>
#include <anx/kprintf.h>

/* The superblock occupies exactly one sector. */
_Static_assert(sizeof(struct anx_md_super) == 512,
	       "anx_md_super must be one 512-byte sector");

/*
 * FNV-1a over the superblock with the checksum field zeroed. The array
 * metadata is our own format and never leaves the machine, so a 32-bit
 * non-cryptographic hash is enough to catch a torn or stale sector.
 */
static uint32_t md_checksum(const struct anx_md_super *sb)
{
	struct anx_md_super copy;
	const uint8_t *p = (const uint8_t *)&copy;
	uint32_t hash = 2166136261u;
	uint32_t i;

	anx_memcpy(&copy, sb, sizeof(copy));
	copy.checksum = 0;

	for (i = 0; i < sizeof(copy); i++) {
		hash ^= p[i];
		hash *= 16777619u;
	}
	return hash;
}

/* Sector holding the superblock for the given placement on this device. */
static int md_super_sector(struct anx_blk_dev *dev, bool tail,
			   uint64_t *out)
{
	uint64_t cap = anx_blk_dev_capacity(dev);

	if (!tail) {
		if (cap <= ANX_MD_DATA_OFFSET)
			return ANX_EINVAL;
		*out = ANX_MD_SB_SECTOR_HEAD;
		return ANX_OK;
	}
	if (cap <= ANX_MD_SB_TAIL_BACK)
		return ANX_EINVAL;
	*out = cap - ANX_MD_SB_TAIL_BACK;
	return ANX_OK;
}

/* Read and validate the superblock at one placement. */
static int md_super_read_at(struct anx_blk_dev *dev, bool tail,
			    struct anx_md_super *sb)
{
	uint64_t sector;
	int ret;

	ret = md_super_sector(dev, tail, &sector);
	if (ret != ANX_OK)
		return ret;

	ret = anx_blk_dev_read(dev, sector, 1, sb);
	if (ret != ANX_OK)
		return ret;

	if (sb->magic != ANX_MD_MAGIC)
		return ANX_ENOENT;
	if (sb->version != ANX_MD_SB_VERSION)
		return ANX_ENOTSUP;
	if (sb->checksum != md_checksum(sb))
		return ANX_EIO;
	if (sb->member_count == 0 ||
	    sb->member_count > ANX_MD_MAX_MEMBERS ||
	    sb->member_index >= sb->member_count)
		return ANX_EINVAL;
	if (sb->level != ANX_MD_LEVEL_RAID0 &&
	    sb->level != ANX_MD_LEVEL_RAID1)
		return ANX_ENOTSUP;
	/* A tail superblock must say so; a head one must not. */
	if (((sb->sb_flags & ANX_MD_SB_TAIL) != 0) != tail)
		return ANX_EINVAL;
	return ANX_OK;
}

/* Read the superblock from either placement. */
static int md_super_read(struct anx_blk_dev *dev, struct anx_md_super *sb)
{
	if (md_super_read_at(dev, false, sb) == ANX_OK)
		return ANX_OK;
	return md_super_read_at(dev, true, sb);
}

int anx_md_super_write(struct anx_md_array *a, uint32_t idx)
{
	struct anx_md_super sb;
	struct anx_md_member *m;
	uint64_t sector;
	int ret;

	if (!a || idx >= a->member_count)
		return ANX_EINVAL;
	m = &a->members[idx];
	if (!m->dev)
		return ANX_ENODEV;

	ret = md_super_sector(m->dev, a->tail_meta, &sector);
	if (ret != ANX_OK)
		return ret;

	anx_memset(&sb, 0, sizeof(sb));
	sb.magic         = ANX_MD_MAGIC;
	sb.version       = ANX_MD_SB_VERSION;
	sb.level         = a->level;
	sb.chunk_sectors = a->chunk_sectors;
	anx_memcpy(sb.uuid, a->uuid, sizeof(sb.uuid));
	anx_strlcpy(sb.name, a->name, sizeof(sb.name));
	sb.member_count   = a->member_count;
	sb.member_index   = idx;
	sb.data_offset    = m->data_offset;
	sb.member_sectors = a->member_sectors;
	sb.array_sectors  = a->array_sectors;
	sb.events         = a->events;
	sb.member_state   = m->state;
	sb.sb_flags       = a->tail_meta ? ANX_MD_SB_TAIL : 0;
	sb.resync_offset  = a->resync_offset;
	sb.created        = a->created;
	sb.checksum       = md_checksum(&sb);

	m->events = a->events;
	return anx_blk_dev_write(m->dev, sector, 1, &sb);
}

int anx_md_super_sync(struct anx_md_array *a)
{
	uint32_t i;
	int last = ANX_OK;

	if (!a)
		return ANX_EINVAL;

	a->events++;
	for (i = 0; i < a->member_count; i++) {
		int ret;

		if (!a->members[i].dev)
			continue;
		ret = anx_md_super_write(a, i);
		if (ret != ANX_OK)
			last = ret;
	}
	return last;
}

int anx_md_zero_super(struct anx_blk_dev *dev)
{
	static uint8_t zero[512];
	struct anx_md_super sb;
	uint64_t sector;
	bool tail;
	int ret;

	if (!dev)
		return ANX_EINVAL;
	if (anx_blk_dev_is_member(dev))
		return ANX_EBUSY;

	if (md_super_read_at(dev, false, &sb) == ANX_OK) {
		tail = false;
	} else if (md_super_read_at(dev, true, &sb) == ANX_OK) {
		tail = true;
	} else {
		return ANX_ENOENT;
	}

	ret = md_super_sector(dev, tail, &sector);
	if (ret != ANX_OK)
		return ret;

	anx_memset(zero, 0, sizeof(zero));
	return anx_blk_dev_write(dev, sector, 1, zero);
}

/* --- Assembly -------------------------------------------------------- */

/*
 * Candidate members found in one registry scan. Held at file scope, not on
 * the stack: one superblock is 512 bytes, and 16 of them would take a fifth
 * of the kernel stack. Assembly runs at boot and from the shell, never
 * concurrently with itself.
 */
static struct {
	struct anx_blk_dev *dev;
	struct anx_md_super sb;
} md_cand[ANX_BLK_MAX_DEVS];

/* Start one array from the candidates listed in idx, which share a UUID. */
static int md_assemble_one(const uint32_t *idx, uint32_t count)
{
	struct anx_md_array *a;
	const struct anx_md_super *ref = &md_cand[idx[0]].sb;
	uint32_t i;
	uint32_t present = 0;
	int ret;

	a = anx_md_alloc_array();
	if (!a) {
		kprintf("md: array table full, skipping %s\n", ref->name);
		return ANX_EFULL;
	}

	anx_strlcpy(a->name, ref->name, sizeof(a->name));
	anx_memcpy(a->uuid, ref->uuid, sizeof(a->uuid));
	a->level          = ref->level;
	a->chunk_sectors  = ref->chunk_sectors;
	a->member_count   = ref->member_count;
	a->member_sectors = ref->member_sectors;
	a->array_sectors  = ref->array_sectors;
	a->created        = ref->created;
	a->tail_meta      = (ref->sb_flags & ANX_MD_SB_TAIL) != 0;
	a->events         = 0;
	a->resync_offset  = ref->resync_offset;

	for (i = 0; i < count; i++) {
		const struct anx_md_super *sb = &md_cand[idx[i]].sb;
		struct anx_md_member *m;

		if (sb->member_index >= a->member_count)
			continue;
		m = &a->members[sb->member_index];
		if (m->dev) {
			kprintf("md: %s: duplicate member %u, ignoring %s\n",
				a->name, sb->member_index,
				md_cand[idx[i]].dev->name);
			continue;
		}
		if (sb->member_state == ANX_MD_MEMBER_FAULTY)
			continue;

		m->dev         = md_cand[idx[i]].dev;
		m->data_offset = sb->data_offset;
		m->state       = sb->member_state;
		m->events      = sb->events;
		if (sb->events > a->events) {
			a->events        = sb->events;
			a->resync_offset = sb->resync_offset;
		}
		present++;
	}

	/*
	 * A member left behind by an earlier write is stale. Its event
	 * counter lags the newest superblock, so drop it rather than serve
	 * old data from it. RAID 0 has no redundancy, so a stale member
	 * there means the whole array is untrustworthy.
	 */
	for (i = 0; i < a->member_count; i++) {
		struct anx_md_member *m = &a->members[i];

		if (!m->dev)
			continue;
		if (m->events == a->events)
			continue;
		kprintf("md: %s: member %u (%s) is stale, dropping\n",
			a->name, i, m->dev->name);
		m->dev   = NULL;
		m->state = ANX_MD_MEMBER_EMPTY;
		present--;
	}

	if (a->level == ANX_MD_LEVEL_RAID0 && present != a->member_count) {
		kprintf("md: %s: raid0 needs %u members, found %u — not started\n",
			a->name, a->member_count, present);
		anx_md_free_array(a);
		return ANX_ENODEV;
	}
	if (present == 0) {
		anx_md_free_array(a);
		return ANX_ENODEV;
	}

	ret = anx_md_start_array(a);
	if (ret != ANX_OK) {
		anx_md_free_array(a);
		return ret;
	}
	return ANX_OK;
}

int anx_md_assemble(void)
{
	bool taken[ANX_BLK_MAX_DEVS];
	uint32_t found = 0;
	uint32_t i, j;
	int started = 0;

	/* Collect every unclaimed device that carries a superblock. */
	for (i = 0; i < anx_blk_dev_count(); i++) {
		struct anx_blk_dev *dev = anx_blk_dev_at(i);

		if (!dev || anx_blk_dev_is_member(dev))
			continue;
		if (dev->flags & ANX_BLK_F_ARRAY)
			continue;
		if (md_super_read(dev, &md_cand[found].sb) != ANX_OK)
			continue;
		md_cand[found].dev = dev;
		taken[found] = false;
		found++;
	}

	/* Group by array UUID and start each group. */
	for (i = 0; i < found; i++) {
		uint32_t group[ANX_MD_MAX_MEMBERS];
		uint32_t n = 0;

		if (taken[i])
			continue;
		taken[i] = true;
		group[n++] = i;

		for (j = i + 1; j < found; j++) {
			if (taken[j])
				continue;
			if (anx_memcmp(md_cand[j].sb.uuid,
				       md_cand[i].sb.uuid, 16) != 0)
				continue;
			taken[j] = true;
			if (n < ANX_MD_MAX_MEMBERS)
				group[n++] = j;
		}

		if (md_assemble_one(group, n) == ANX_OK)
			started++;
	}
	return started;
}
