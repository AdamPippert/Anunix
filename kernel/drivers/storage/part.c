/*
 * part.c — Partition devices backed by GPT entries (RFC-0031).
 *
 * Each usable GPT entry on a drive becomes its own block device whose
 * operations add the partition's start LBA and clamp every request to the
 * partition's length. The clamp is the reason this file exists: without it
 * an object store format would run off the end of its partition and into
 * whatever the host operating system keeps next door.
 */

#include <anx/part.h>
#include <anx/blk.h>
#include <anx/gpt.h>
#include <anx/alloc.h>
#include <anx/string.h>
#include <anx/kprintf.h>

/*
 * Partition state is allocated once per partition and never freed: a
 * partition lives as long as the registry slot that points at it, and the
 * registry is static. Bounding the count keeps a malformed table from
 * exhausting the heap.
 */
#define PART_MAX	(ANX_BLK_MAX_DEVS * 2)

static struct anx_part part_pool[PART_MAX];

/* A pool slot is free when it has no parent. Slots are reused, so a drive
 * that comes and goes does not exhaust the pool. */
static struct anx_part *part_alloc(void)
{
	uint32_t i;

	for (i = 0; i < PART_MAX; i++) {
		if (!part_pool[i].parent)
			return &part_pool[i];
	}
	return NULL;
}

static int part_blk_read(struct anx_blk_dev *dev, uint64_t lba,
			 uint32_t count, void *buf)
{
	struct anx_part *p = dev->priv;

	if (count == 0)
		return ANX_OK;
	/*
	 * Written so the arithmetic cannot overflow: establish lba is
	 * inside the partition first, then subtract. "lba + count" would
	 * wrap for an lba near UINT64_MAX and admit an out-of-bounds read.
	 */
	if (lba >= p->sectors || count > p->sectors - lba)
		return ANX_ERANGE;

	return anx_blk_dev_read(p->parent, p->start_lba + lba, count, buf);
}

static int part_blk_write(struct anx_blk_dev *dev, uint64_t lba,
			  uint32_t count, const void *buf)
{
	struct anx_part *p = dev->priv;

	if (count == 0)
		return ANX_OK;
	if (lba >= p->sectors || count > p->sectors - lba)
		return ANX_ERANGE;

	return anx_blk_dev_write(p->parent, p->start_lba + lba, count, buf);
}

static uint64_t part_blk_capacity(struct anx_blk_dev *dev)
{
	struct anx_part *p = dev->priv;

	return p->sectors;
}

static const struct anx_blk_ops part_ops = {
	.read     = part_blk_read,
	.write    = part_blk_write,
	.capacity = part_blk_capacity,
	.name     = "part",
};

bool anx_part_is_partition(const struct anx_blk_dev *dev)
{
	return dev && dev->ops == &part_ops;
}

const struct anx_part *anx_part_of(const struct anx_blk_dev *dev)
{
	if (!anx_part_is_partition(dev))
		return NULL;
	return (const struct anx_part *)dev->priv;
}

/* Build "<parent>p<index>" into dst. */
static void part_name(char *dst, uint32_t size, const char *parent,
		      uint32_t index)
{
	uint32_t len;

	anx_strlcpy(dst, parent, size);
	len = (uint32_t)anx_strlen(dst);
	if (len + 4 >= size)
		len = size - 5;

	dst[len++] = 'p';
	/* GPT entry indices stay below 128, so two digits are enough. */
	if (index >= 10)
		dst[len++] = (char)('0' + (index / 10) % 10);
	dst[len++] = (char)('0' + index % 10);
	dst[len] = '\0';
}

int anx_part_scan(struct anx_blk_dev *dev)
{
	struct anx_gpt_table *table;
	uint64_t parent_sectors;
	uint32_t i;
	int registered = 0;
	int ret;

	if (!dev || !dev->used)
		return ANX_ENODEV;

	/* Partitions do not nest. */
	if (anx_part_is_partition(dev))
		return 0;

	parent_sectors = anx_blk_dev_capacity(dev);
	if (parent_sectors == 0)
		return 0;

	table = anx_alloc(sizeof(*table));
	if (!table)
		return ANX_ENOMEM;

	ret = anx_gpt_read_dev(dev, table);
	if (ret != ANX_OK) {
		/*
		 * No GPT is the common case for a drive Anunix owns outright.
		 * It is not an error, and it must not be noisy at boot.
		 */
		anx_free(table);
		return 0;
	}

	for (i = 0; i < table->partition_count; i++) {
		const struct anx_gpt_partition *e = &table->partitions[i];
		struct anx_blk_dev *pdev;
		struct anx_part *p;
		char name[ANX_BLK_NAME_MAX];
		uint64_t sectors;

		/*
		 * Reject entries that a malformed or hostile table could use
		 * to address sectors outside the parent. end_lba is
		 * inclusive.
		 */
		if (e->start_lba == 0)
			continue;
		if (e->end_lba < e->start_lba)
			continue;
		if (e->end_lba >= parent_sectors)
			continue;

		sectors = e->end_lba - e->start_lba + 1;

		p = part_alloc();
		if (!p) {
			kprintf("part: pool full, dropping %s entry %u\n",
				dev->name, e->index);
			break;
		}

		p->parent    = dev;
		p->start_lba = e->start_lba;
		p->sectors   = sectors;
		p->index     = e->index;
		p->type_lo   = e->type_lo;
		p->type_hi   = e->type_hi;
		anx_strlcpy(p->label, e->name, sizeof(p->label));

		part_name(name, sizeof(name), dev->name, e->index);

		pdev = anx_blk_dev_register_named(&part_ops, p, name);
		if (!pdev) {
			kprintf("part: could not register %s\n", name);
			p->parent = NULL;	/* return the slot */
			break;
		}

		registered++;

		kprintf("part: %s on %s lba %llu+%llu%s%s\n",
			name, dev->name,
			(unsigned long long)p->start_lba,
			(unsigned long long)p->sectors,
			p->label[0] ? " " : "",
			p->label);
	}

	anx_free(table);
	return registered;
}

void anx_part_scan_all(void)
{
	uint32_t i;
	uint32_t count;

	/*
	 * Snapshot the count first. Registering partitions grows the
	 * registry, and a partition must not itself be scanned — taking the
	 * count up front bounds the loop to the drives present on entry.
	 */
	count = anx_blk_dev_count();

	for (i = 0; i < count; i++) {
		struct anx_blk_dev *dev = anx_blk_dev_at(i);

		if (!dev)
			continue;
		if (anx_part_is_partition(dev))
			continue;
		anx_part_scan(dev);
	}
}

void anx_part_forget_children(struct anx_blk_dev *parent)
{
	uint32_t i;

	if (!parent)
		return;

	/*
	 * Walk the registry directly rather than by index: unregistering
	 * compacts what anx_blk_dev_at() returns, and a partition removed
	 * mid-walk would make the walk skip its neighbour.
	 */
	for (i = 0; i < PART_MAX; i++) {
		struct anx_part *p = &part_pool[i];
		struct anx_blk_dev *dev;
		uint32_t j, count;

		if (p->parent != parent)
			continue;

		count = anx_blk_dev_count();
		for (j = 0; j < count; j++) {
			dev = anx_blk_dev_at(j);
			if (dev && dev->priv == p &&
			    anx_part_is_partition(dev)) {
				anx_blk_dev_unregister(dev);
				break;
			}
		}
		p->parent = NULL;
	}
}
