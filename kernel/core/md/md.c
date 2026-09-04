/*
 * md.c — Software RAID array lifecycle and dispatch.
 *
 * Holds the array table, registers each started array as a block device,
 * and routes array I/O to the level-specific code in md_raid0.c and
 * md_raid1.c. Members are ordinary block devices; the only thing that
 * marks them as belonging to an array is the ANX_BLK_F_MEMBER flag and
 * the superblock they carry.
 */

#include <anx/types.h>
#include <anx/md.h>
#include <anx/blk.h>
#include <anx/arch.h>
#include <anx/uuid.h>
#include <anx/string.h>
#include <anx/kprintf.h>

static struct anx_md_array md_arrays[ANX_MD_MAX_ARRAYS];

/* --- Array table ----------------------------------------------------- */

struct anx_md_array *anx_md_alloc_array(void)
{
	uint32_t i;

	for (i = 0; i < ANX_MD_MAX_ARRAYS; i++) {
		if (md_arrays[i].used)
			continue;
		anx_memset(&md_arrays[i], 0, sizeof(md_arrays[i]));
		md_arrays[i].used = true;
		return &md_arrays[i];
	}
	return NULL;
}

void anx_md_free_array(struct anx_md_array *a)
{
	if (!a)
		return;
	anx_memset(a, 0, sizeof(*a));
}

uint32_t anx_md_count(void)
{
	uint32_t i, n = 0;

	for (i = 0; i < ANX_MD_MAX_ARRAYS; i++)
		if (md_arrays[i].used)
			n++;
	return n;
}

struct anx_md_array *anx_md_at(uint32_t index)
{
	uint32_t i, n = 0;

	for (i = 0; i < ANX_MD_MAX_ARRAYS; i++) {
		if (!md_arrays[i].used)
			continue;
		if (n == index)
			return &md_arrays[i];
		n++;
	}
	return NULL;
}

struct anx_md_array *anx_md_find(const char *name)
{
	uint32_t i;

	if (!name)
		return NULL;
	for (i = 0; i < ANX_MD_MAX_ARRAYS; i++) {
		if (md_arrays[i].used &&
		    anx_strcmp(md_arrays[i].name, name) == 0)
			return &md_arrays[i];
	}
	return NULL;
}

/* --- State reporting -------------------------------------------------- */

uint32_t anx_md_state(const struct anx_md_array *a)
{
	uint32_t i;
	uint32_t active = 0;
	uint32_t spare = 0;

	if (!a || !a->used)
		return ANX_MD_ARRAY_FAILED;

	for (i = 0; i < a->member_count; i++) {
		if (!a->members[i].dev)
			continue;
		if (a->members[i].state == ANX_MD_MEMBER_ACTIVE)
			active++;
		else if (a->members[i].state == ANX_MD_MEMBER_SPARE)
			spare++;
	}

	if (a->level == ANX_MD_LEVEL_RAID0)
		return active == a->member_count ? ANX_MD_ARRAY_CLEAN
						 : ANX_MD_ARRAY_FAILED;

	if (active == 0)
		return ANX_MD_ARRAY_FAILED;
	if (spare > 0)
		return ANX_MD_ARRAY_REBUILDING;
	if (active < a->member_count)
		return ANX_MD_ARRAY_DEGRADED;
	return ANX_MD_ARRAY_CLEAN;
}

const char *anx_md_state_name(uint32_t state)
{
	switch (state) {
	case ANX_MD_ARRAY_CLEAN:	return "clean";
	case ANX_MD_ARRAY_DEGRADED:	return "degraded";
	case ANX_MD_ARRAY_REBUILDING:	return "rebuilding";
	default:			return "failed";
	}
}

const char *anx_md_level_name(uint32_t level)
{
	switch (level) {
	case ANX_MD_LEVEL_RAID0:	return "raid0";
	case ANX_MD_LEVEL_RAID1:	return "raid1";
	default:			return "unknown";
	}
}

const char *anx_md_member_state_name(uint32_t state)
{
	switch (state) {
	case ANX_MD_MEMBER_ACTIVE:	return "active";
	case ANX_MD_MEMBER_FAULTY:	return "faulty";
	case ANX_MD_MEMBER_SPARE:	return "spare";
	default:			return "missing";
	}
}

int anx_md_parse_level(const char *s)
{
	if (!s)
		return ANX_EINVAL;
	if (anx_strcmp(s, "0") == 0 || anx_strcmp(s, "raid0") == 0)
		return ANX_MD_LEVEL_RAID0;
	if (anx_strcmp(s, "1") == 0 || anx_strcmp(s, "raid1") == 0)
		return ANX_MD_LEVEL_RAID1;
	return ANX_EINVAL;
}

/* --- Member I/O ------------------------------------------------------- */

int anx_md_member_read(struct anx_md_array *a, uint32_t idx, uint64_t lba,
		       uint32_t count, void *buf)
{
	struct anx_md_member *m;

	if (!a || idx >= a->member_count)
		return ANX_EINVAL;
	m = &a->members[idx];
	if (!m->dev || m->state == ANX_MD_MEMBER_FAULTY)
		return ANX_ENODEV;
	if (lba + count > a->member_sectors)
		return ANX_EINVAL;
	return anx_blk_dev_read(m->dev, m->data_offset + lba, count, buf);
}

int anx_md_member_write(struct anx_md_array *a, uint32_t idx, uint64_t lba,
			uint32_t count, const void *buf)
{
	struct anx_md_member *m;

	if (!a || idx >= a->member_count)
		return ANX_EINVAL;
	m = &a->members[idx];
	if (!m->dev || m->state == ANX_MD_MEMBER_FAULTY)
		return ANX_ENODEV;
	if (lba + count > a->member_sectors)
		return ANX_EINVAL;
	return anx_blk_dev_write(m->dev, m->data_offset + lba, count, buf);
}

void anx_md_member_fault(struct anx_md_array *a, uint32_t idx)
{
	struct anx_md_member *m;

	if (!a || idx >= a->member_count)
		return;
	m = &a->members[idx];
	if (!m->dev || m->state == ANX_MD_MEMBER_FAULTY)
		return;

	kprintf("md: %s: member %u (%s) failed\n",
		a->name, idx, m->dev->name);
	m->state = ANX_MD_MEMBER_FAULTY;

	/* Record the failure so a later boot does not trust this member.
	 * The write goes to the surviving members too; if it cannot reach
	 * the failed one, that is exactly the case being recorded. */
	anx_md_super_sync(a);
}

/* --- Array block device ---------------------------------------------- */

static int md_blk_read(struct anx_blk_dev *dev, uint64_t lba,
		       uint32_t count, void *buf)
{
	struct anx_md_array *a = dev->priv;

	if (!a || !a->used)
		return ANX_ENODEV;
	if (lba + count > a->array_sectors)
		return ANX_EINVAL;
	if (a->level == ANX_MD_LEVEL_RAID0)
		return anx_md_raid0_read(a, lba, count, buf);
	return anx_md_raid1_read(a, lba, count, buf);
}

static int md_blk_write(struct anx_blk_dev *dev, uint64_t lba,
			uint32_t count, const void *buf)
{
	struct anx_md_array *a = dev->priv;

	if (!a || !a->used)
		return ANX_ENODEV;
	if (lba + count > a->array_sectors)
		return ANX_EINVAL;
	if (a->level == ANX_MD_LEVEL_RAID0)
		return anx_md_raid0_write(a, lba, count, buf);
	return anx_md_raid1_write(a, lba, count, buf);
}

static uint64_t md_blk_capacity(struct anx_blk_dev *dev)
{
	struct anx_md_array *a = dev->priv;

	return (a && a->used) ? a->array_sectors : 0;
}

static const struct anx_blk_ops md_ops = {
	.read     = md_blk_read,
	.write    = md_blk_write,
	.capacity = md_blk_capacity,
	.name     = "md",
};

int anx_md_start_array(struct anx_md_array *a)
{
	struct anx_blk_dev *active = anx_blk_active();
	bool takeover = false;
	uint32_t i;

	if (!a || !a->used)
		return ANX_EINVAL;
	if (a->array_sectors == 0)
		return ANX_EINVAL;

	for (i = 0; i < a->member_count; i++) {
		if (a->members[i].dev && a->members[i].dev == active)
			takeover = true;
	}

	a->bdev = anx_blk_dev_register(&md_ops, a, "md");
	if (!a->bdev)
		return ANX_EFULL;
	a->bdev->flags |= ANX_BLK_F_ARRAY;

	/* The array name is the registry name, so "raid detail md0" and
	 * "disk" agree on what to call it. */
	anx_strlcpy(a->name, a->bdev->name, sizeof(a->name));

	for (i = 0; i < a->member_count; i++)
		if (a->members[i].dev)
			anx_blk_dev_claim(a->members[i].dev);

	kprintf("md: %s: %s, %u members, %llu MiB, %s\n",
		a->name, anx_md_level_name(a->level), a->member_count,
		(unsigned long long)(a->array_sectors / 2048),
		anx_md_state_name(anx_md_state(a)));

	/*
	 * A member held the active device, so the object store was about to
	 * be mounted from one slice of the array. Point the whole-system API
	 * at the array instead.
	 */
	if (takeover || !active)
		anx_blk_set_active(a->bdev);

	return ANX_OK;
}

int anx_md_stop(struct anx_md_array *a)
{
	uint32_t i;

	if (!a || !a->used)
		return ANX_EINVAL;
	if (a->bdev && anx_blk_active() == a->bdev)
		return ANX_EBUSY;

	anx_md_super_sync(a);

	for (i = 0; i < a->member_count; i++)
		if (a->members[i].dev)
			anx_blk_dev_release(a->members[i].dev);

	if (a->bdev)
		anx_blk_dev_unregister(a->bdev);

	kprintf("md: %s stopped\n", a->name);
	anx_md_free_array(a);
	return ANX_OK;
}

/* --- Creation --------------------------------------------------------- */

/* Round down to a whole number of chunks so the stripe map stays uniform. */
static uint64_t md_round_chunks(uint64_t sectors, uint32_t chunk)
{
	if (chunk == 0)
		return sectors;
	return (sectors / chunk) * chunk;
}

int anx_md_create(const char *name, uint32_t level, uint32_t chunk_sectors,
		  struct anx_blk_dev **members, uint32_t count,
		  bool tail_meta, struct anx_md_array **out)
{
	struct anx_md_array *a;
	uint64_t data_offset;
	uint64_t smallest = 0;
	uint32_t i, j;
	int ret;

	(void)name;

	if (!members || count < 2 || count > ANX_MD_MAX_MEMBERS)
		return ANX_EINVAL;
	if (level != ANX_MD_LEVEL_RAID0 && level != ANX_MD_LEVEL_RAID1)
		return ANX_ENOTSUP;

	if (level == ANX_MD_LEVEL_RAID0) {
		if (chunk_sectors == 0)
			chunk_sectors = ANX_MD_DEFAULT_CHUNK;
		/* A power-of-two chunk keeps the stripe map to shifts and
		 * masks, and matches how every drive reports its geometry. */
		if ((chunk_sectors & (chunk_sectors - 1)) != 0)
			return ANX_EINVAL;
	} else {
		chunk_sectors = 0;
	}

	/* Reject duplicates and devices already spoken for. */
	for (i = 0; i < count; i++) {
		if (!members[i])
			return ANX_EINVAL;
		if (anx_blk_dev_is_member(members[i]))
			return ANX_EBUSY;
		if (members[i]->flags & ANX_BLK_F_ARRAY)
			return ANX_EINVAL;
		for (j = i + 1; j < count; j++)
			if (members[i] == members[j])
				return ANX_EINVAL;
	}

	/* Every member contributes the capacity of the smallest one. */
	data_offset = tail_meta ? 0 : ANX_MD_DATA_OFFSET;
	for (i = 0; i < count; i++) {
		uint64_t cap = anx_blk_dev_capacity(members[i]);
		uint64_t usable;

		if (tail_meta) {
			if (cap <= ANX_MD_SB_TAIL_BACK)
				return ANX_EINVAL;
			usable = cap - ANX_MD_SB_TAIL_BACK;
		} else {
			if (cap <= ANX_MD_DATA_OFFSET)
				return ANX_EINVAL;
			usable = cap - ANX_MD_DATA_OFFSET;
		}
		if (smallest == 0 || usable < smallest)
			smallest = usable;
	}

	smallest = md_round_chunks(smallest, chunk_sectors);
	if (smallest == 0)
		return ANX_EINVAL;

	a = anx_md_alloc_array();
	if (!a)
		return ANX_EFULL;

	a->level          = level;
	a->chunk_sectors  = chunk_sectors;
	a->member_count   = count;
	a->member_sectors = smallest;
	a->array_sectors  = (level == ANX_MD_LEVEL_RAID0)
			    ? smallest * count : smallest;
	a->tail_meta      = tail_meta;
	a->created        = arch_time_now();
	a->events         = 1;
	a->resync_offset  = 0;

	/* Array identity: one UUID shared by every member. */
	{
		struct anx_uuid u;

		anx_uuid_generate(&u);
		anx_memcpy(&a->uuid[0], &u.hi, 8);
		anx_memcpy(&a->uuid[8], &u.lo, 8);
	}

	for (i = 0; i < count; i++) {
		a->members[i].dev         = members[i];
		a->members[i].data_offset = data_offset;
		a->members[i].state       = ANX_MD_MEMBER_ACTIVE;
		a->members[i].events      = a->events;
	}

	ret = anx_md_start_array(a);
	if (ret != ANX_OK) {
		anx_md_free_array(a);
		return ret;
	}

	for (i = 0; i < count; i++) {
		ret = anx_md_super_write(a, i);
		if (ret != ANX_OK) {
			kprintf("md: %s: superblock write to %s failed (%d)\n",
				a->name, members[i]->name, ret);
			anx_md_stop(a);
			return ret;
		}
	}

	if (out)
		*out = a;
	return ANX_OK;
}

/* --- Member administration -------------------------------------------- */

int anx_md_fail(struct anx_md_array *a, uint32_t member_index)
{
	if (!a || !a->used || member_index >= a->member_count)
		return ANX_EINVAL;
	if (!a->members[member_index].dev)
		return ANX_ENODEV;

	anx_md_member_fault(a, member_index);
	return ANX_OK;
}

int anx_md_add(struct anx_md_array *a, struct anx_blk_dev *dev)
{
	uint64_t cap;
	uint64_t usable;
	uint32_t slot = ANX_MD_MAX_MEMBERS;
	uint32_t i;
	int ret;

	if (!a || !a->used || !dev)
		return ANX_EINVAL;
	if (a->level != ANX_MD_LEVEL_RAID1)
		return ANX_ENOTSUP;
	if (anx_blk_dev_is_member(dev) || (dev->flags & ANX_BLK_F_ARRAY))
		return ANX_EBUSY;

	for (i = 0; i < a->member_count; i++) {
		struct anx_md_member *m = &a->members[i];

		if (!m->dev || m->state == ANX_MD_MEMBER_FAULTY) {
			slot = i;
			break;
		}
	}
	if (slot == ANX_MD_MAX_MEMBERS)
		return ANX_EFULL;

	cap = anx_blk_dev_capacity(dev);
	usable = a->tail_meta ? (cap > ANX_MD_SB_TAIL_BACK
				 ? cap - ANX_MD_SB_TAIL_BACK : 0)
			      : (cap > ANX_MD_DATA_OFFSET
				 ? cap - ANX_MD_DATA_OFFSET : 0);
	if (usable < a->member_sectors)
		return ANX_EINVAL;

	/* The slot may still hold the failed drive. Let it go before the
	 * replacement takes its place, so it shows as free again. */
	if (a->members[slot].dev)
		anx_blk_dev_release(a->members[slot].dev);

	a->members[slot].dev         = dev;
	a->members[slot].data_offset = a->tail_meta ? 0 : ANX_MD_DATA_OFFSET;
	a->members[slot].state       = ANX_MD_MEMBER_SPARE;
	a->resync_offset             = 0;

	anx_blk_dev_claim(dev);

	ret = anx_md_super_sync(a);
	if (ret != ANX_OK)
		kprintf("md: %s: metadata update after add failed (%d)\n",
			a->name, ret);

	kprintf("md: %s: %s added as spare, run \"raid resync %s\"\n",
		a->name, dev->name, a->name);
	return ANX_OK;
}

/* --- Boot assembly ---------------------------------------------------- */

void anx_md_init(void)
{
	int started = anx_md_assemble();

	if (started > 0)
		kprintf("md: %d array(s) assembled\n", started);
}
