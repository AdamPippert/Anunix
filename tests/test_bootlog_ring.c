/*
 * test_bootlog_ring.c — Rolling boot-log ring in the OS partition.
 *
 * The ring exists so a boot log survives the object store failing, so the
 * cases that matter are the refusals and the wrap: it must not write to a
 * device that is not ours, it must pick the slot after the newest, and it
 * must keep going once every slot has been used.
 */

#include <anx/types.h>
#include <anx/bootlog.h>
#include <anx/blk.h>
#include <anx/objstore_disk.h>
#include <anx/mock_blk.h>
#include <anx/string.h>
#include <anx/kprintf.h>

/* Big enough for the ring plus the store's reserved head. */
#define DEV_SECTORS	8192u	/* the mock's maximum */

#define CHECK(cond, msg)						\
	do {								\
		if (!(cond)) {						\
			kprintf("  FAIL: %s\n", (msg));			\
			return -1;					\
		}							\
	} while (0)

static uint8_t sector[512];

static void write_superblock(void)
{
	struct anx_disk_super *sb = (struct anx_disk_super *)sector;

	anx_memset(sector, 0, sizeof(sector));
	sb->magic   = ANX_DISK_MAGIC;
	sb->version = ANX_DISK_VERSION;
	anx_blk_write(ANX_SUPER_SECTOR, 1, sector);
}

static void wipe_superblock(void)
{
	anx_memset(sector, 0, sizeof(sector));
	anx_blk_write(ANX_SUPER_SECTOR, 1, sector);
}

int test_bootlog_ring(void)
{
	struct anx_blog_ring_entry list[ANX_BLOG_RING_SLOTS];
	uint32_t count = 0, i;
	uint64_t first_seq;

	test_mock_blk_init(DEV_SECTORS);

	/* ---- refuses a device that carries no Anunix superblock ---- */
	wipe_superblock();
	CHECK(anx_bootlog_ring_init() != ANX_OK,
	      "ring must refuse a device with no Anunix superblock");
	CHECK(anx_bootlog_ring_flush() != ANX_OK,
	      "flush must refuse before a successful init");

	/* ---- claims a slot on a device that is ours ---- */
	write_superblock();
	CHECK(anx_bootlog_ring_init() == ANX_OK, "ring init on our device");
	CHECK(anx_bootlog_ring_flush() == ANX_OK, "first flush");

	CHECK(anx_bootlog_ring_list(list, ANX_BLOG_RING_SLOTS, &count) == ANX_OK,
	      "list after one boot");
	CHECK(count == 1, "exactly one session recorded");
	CHECK(!list[0].pruned, "a fresh session is not pruned");
	first_seq = list[0].seq;

	/* ---- each boot takes the next slot, with a higher sequence ---- */
	for (i = 1; i < ANX_BLOG_RING_SLOTS; i++) {
		CHECK(anx_bootlog_ring_init() == ANX_OK, "later boot init");
		CHECK(anx_bootlog_ring_flush() == ANX_OK, "later boot flush");
	}
	CHECK(anx_bootlog_ring_list(list, ANX_BLOG_RING_SLOTS, &count) == ANX_OK,
	      "list after filling the ring");
	CHECK(count == ANX_BLOG_RING_SLOTS, "every slot in use");

	/* ---- wrapping overwrites the oldest, not the newest ---- */
	CHECK(anx_bootlog_ring_init() == ANX_OK, "boot that wraps the ring");
	CHECK(anx_bootlog_ring_flush() == ANX_OK, "flush after wrap");
	CHECK(anx_bootlog_ring_list(list, ANX_BLOG_RING_SLOTS, &count) == ANX_OK,
	      "list after wrap");
	CHECK(count == ANX_BLOG_RING_SLOTS, "ring stays full after wrapping");
	{
		uint64_t lowest = list[0].seq, highest = list[0].seq;

		for (i = 1; i < count; i++) {
			if (list[i].seq < lowest)
				lowest = list[i].seq;
			if (list[i].seq > highest)
				highest = list[i].seq;
		}
		CHECK(lowest > first_seq,
		      "the oldest session was the one overwritten");
		CHECK(highest - lowest == ANX_BLOG_RING_SLOTS - 1,
		      "sequences stay contiguous across a wrap");

		/* ---- pruning marks, and does not remove ---- */
		CHECK(anx_bootlog_ring_prune(highest) == ANX_OK,
		      "prune the newest session");
		CHECK(anx_bootlog_ring_list(list, ANX_BLOG_RING_SLOTS,
					     &count) == ANX_OK, "list after prune");
		CHECK(count == ANX_BLOG_RING_SLOTS,
		      "a pruned session is still listed");
		{
			bool found = false;

			for (i = 0; i < count; i++)
				if (list[i].seq == highest && list[i].pruned)
					found = true;
			CHECK(found, "the pruned session is marked pruned");
		}
		CHECK(anx_bootlog_ring_prune(highest + 1000u) == ANX_ENOENT,
		      "pruning an unknown sequence reports ENOENT");
	}

	test_mock_blk_teardown();
	return 0;
}
