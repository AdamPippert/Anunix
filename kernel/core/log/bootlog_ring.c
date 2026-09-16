/*
 * bootlog_ring.c — A rolling boot-log ring at the end of the OS partition.
 *
 * The session objects in bootlog.c depend on the object store mounting, on
 * the allocator, on a fixed-OID index and on retention pruning. Every one of
 * those is a way for a boot log to not exist, and a boot log is most wanted
 * exactly when something in that chain has broken. In practice the newest
 * persisted session on jekyll was several boots old while the machine was
 * being debugged, which is the worst possible time to lose the record.
 *
 * This is the blunt instrument that sits underneath. A fixed ring of slots
 * lives in the last few megabytes of the partition and is written through
 * anx_blk_write() directly. No allocation, no index object, no OIDs, no
 * dependency on the store having mounted. The newest slot is whichever
 * carries the highest sequence number, so recovery needs no superblock of
 * its own and a torn write costs one slot rather than the ring.
 *
 * Placement. The store's data region grows upward from ANX_DATA_START and
 * the partition is far larger than the store will use, so the ring takes the
 * tail. That needs no change to the store's on-disk layout and so cannot
 * invalidate an existing store.
 *
 * Safety. The ring writes only when sector 0 of the active device carries the
 * Anunix superblock magic. Without that check a machine that failed to mount
 * a store could be writing into the tail of somebody else's disk, which is
 * precisely the failure RFC-0031 exists to prevent.
 */

#include <anx/bootlog.h>
#include <anx/blk.h>
#include <anx/objstore_disk.h>
#include <anx/string.h>
#include <anx/kprintf.h>

/* One sector of header, then the log text. */
struct blog_slot_hdr {
	uint32_t magic;			/* ANX_BLOG_RING_MAGIC */
	uint32_t version;
	uint64_t seq;			/* monotonic; highest is newest */
	uint64_t boot_time;
	uint32_t log_bytes;		/* text bytes following this sector */
	uint32_t flags;			/* ANX_BLOG_SLOT_PRUNED */
	char     kver[32];
	uint8_t  _pad[512 - 64];
} __attribute__((packed));

static bool     g_ring_ok;
static uint64_t g_ring_start;		/* first sector of the ring */
static uint64_t g_slot;			/* slot this boot writes */
static uint64_t g_seq;			/* sequence for this boot */

static uint8_t  g_sector[512] __attribute__((aligned(8)));

static uint64_t slot_lba(uint64_t slot)
{
	return g_ring_start + slot * ANX_BLOG_SLOT_SECTORS;
}

/* True when the active device carries an Anunix store superblock. */
static bool device_is_ours(void)
{
	struct anx_disk_super *sb = (struct anx_disk_super *)g_sector;

	if (anx_blk_read(ANX_SUPER_SECTOR, 1, g_sector) != ANX_OK)
		return false;
	return sb->magic == ANX_DISK_MAGIC;
}

int anx_bootlog_ring_init(void)
{
	uint64_t cap, best_seq = 0;
	uint64_t i, next = 0;

	g_ring_ok = false;

	if (!anx_blk_ready())
		return ANX_ENODEV;
	if (!device_is_ours())
		return ANX_EPERM;

	cap = anx_blk_capacity();
	if (cap <= (uint64_t)ANX_BLOG_RING_SECTORS + ANX_DATA_START)
		return ANX_EFULL;

	g_ring_start = cap - ANX_BLOG_RING_SECTORS;

	/* Newest slot is the highest sequence; this boot takes the one after. */
	for (i = 0; i < ANX_BLOG_RING_SLOTS; i++) {
		struct blog_slot_hdr *h = (struct blog_slot_hdr *)g_sector;

		if (anx_blk_read(slot_lba(i), 1, g_sector) != ANX_OK)
			continue;
		if (h->magic != ANX_BLOG_RING_MAGIC)
			continue;
		if (h->seq >= best_seq) {
			best_seq = h->seq;
			next = (i + 1u) % ANX_BLOG_RING_SLOTS;
		}
	}

	g_seq     = best_seq + 1u;
	g_slot    = next;
	g_ring_ok = true;

	kprintf("bootlog: ring slot %u/%u at lba %u, seq %u\n",
		(uint32_t)g_slot, (uint32_t)ANX_BLOG_RING_SLOTS,
		(uint32_t)slot_lba(g_slot), (uint32_t)g_seq);
	return ANX_OK;
}

int anx_bootlog_ring_flush(void)
{
	struct blog_slot_hdr *h = (struct blog_slot_hdr *)g_sector;
	uint32_t have, off, max_bytes;
	uint64_t lba;

	if (!g_ring_ok)
		return ANX_ENODEV;

	max_bytes = (ANX_BLOG_SLOT_SECTORS - 1u) * 512u;
	have = anx_bootlog_current_bytes();
	if (have > max_bytes)
		have = max_bytes;

	/*
	 * Text first, header last. A power loss midway then leaves a slot
	 * whose header still describes the previous, complete contents, or no
	 * valid header at all -- never a header promising text that was not
	 * written.
	 */
	lba = slot_lba(g_slot) + 1u;
	for (off = 0; off < have; off += 512u) {
		uint32_t n = have - off;

		if (n > 512u)
			n = 512u;
		anx_memset(g_sector, 0, sizeof(g_sector));
		anx_bootlog_read_current(off, (char *)g_sector, n);
		if (anx_blk_write(lba + off / 512u, 1, g_sector) != ANX_OK)
			return ANX_EIO;
	}

	anx_memset(g_sector, 0, sizeof(g_sector));
	h->magic     = ANX_BLOG_RING_MAGIC;
	h->version   = ANX_BOOTLOG_VERSION;
	h->seq       = g_seq;
	h->boot_time = 0;
	h->log_bytes = have;
	h->flags     = 0;
	anx_strlcpy(h->kver, ANX_VERSION, sizeof(h->kver));

	return anx_blk_write(slot_lba(g_slot), 1, g_sector);
}

int anx_bootlog_ring_prune(uint64_t seq)
{
	uint64_t i;

	if (!g_ring_ok)
		return ANX_ENODEV;

	for (i = 0; i < ANX_BLOG_RING_SLOTS; i++) {
		struct blog_slot_hdr *h = (struct blog_slot_hdr *)g_sector;

		if (anx_blk_read(slot_lba(i), 1, g_sector) != ANX_OK)
			continue;
		if (h->magic != ANX_BLOG_RING_MAGIC || h->seq != seq)
			continue;
		/*
		 * Mark rather than erase. The text stays until the ring wraps
		 * onto it, so a prune made in error is recoverable and an
		 * agent pruning aggressively cannot destroy evidence.
		 */
		h->flags |= ANX_BLOG_SLOT_PRUNED;
		return anx_blk_write(slot_lba(i), 1, g_sector);
	}
	return ANX_ENOENT;
}

int anx_bootlog_ring_list(struct anx_blog_ring_entry *out, uint32_t max,
			  uint32_t *count)
{
	uint64_t i;
	uint32_t n = 0;

	if (!out || !count)
		return ANX_EINVAL;
	*count = 0;
	if (!g_ring_ok)
		return ANX_ENODEV;

	for (i = 0; i < ANX_BLOG_RING_SLOTS && n < max; i++) {
		struct blog_slot_hdr *h = (struct blog_slot_hdr *)g_sector;

		if (anx_blk_read(slot_lba(i), 1, g_sector) != ANX_OK)
			continue;
		if (h->magic != ANX_BLOG_RING_MAGIC)
			continue;
		out[n].slot      = (uint32_t)i;
		out[n].seq       = h->seq;
		out[n].log_bytes = h->log_bytes;
		out[n].pruned    = (h->flags & ANX_BLOG_SLOT_PRUNED) != 0;
		anx_strlcpy(out[n].kver, h->kver, sizeof(out[n].kver));
		n++;
	}
	*count = n;
	return ANX_OK;
}
