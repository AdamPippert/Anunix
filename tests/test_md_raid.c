/*
 * test_md_raid.c — Software RAID layer.
 *
 * Runs RAID 0 and RAID 1 arrays over the RAM-backed mock block devices
 * in the test harness. Checks the stripe map against the members
 * directly, checks that a mirror survives a failed member, rebuilds onto
 * a replacement, and reassembles an array from its superblocks alone.
 */

#include <anx/types.h>
#include <anx/md.h>
#include <anx/blk.h>
#include <anx/mock_blk.h>
#include <anx/string.h>

#define DEV_SECTORS	8192		/* 4 MiB per mock device */
#define CHUNK		8		/* 4 KiB stripe unit */

static uint8_t wbuf[CHUNK * 4 * 512];
static uint8_t rbuf[CHUNK * 4 * 512];

/* Fill a buffer with a seed-dependent byte pattern. */
static void fill_pattern(uint8_t *buf, uint32_t sectors, uint32_t seed)
{
	uint32_t i;

	for (i = 0; i < sectors * 512; i++)
		buf[i] = (uint8_t)(seed * 31u + i * 7u + (i >> 9));
}

/* Stop an array. An array serving the whole-system API refuses to stop,
 * so give the active pointer up first. */
static void stop(struct anx_md_array *a)
{
	anx_blk_set_active(NULL);
	anx_md_stop(a);
}

static int same(const uint8_t *a, const uint8_t *b, uint32_t sectors)
{
	return anx_memcmp(a, b, (size_t)sectors * 512) == 0;
}

/* Fresh registry with n mock devices and no active device. */
static int setup(uint32_t n, struct anx_blk_dev **devs)
{
	uint32_t i;

	test_mock_blk_teardown();
	for (i = 0; i < n; i++) {
		devs[i] = test_mock_blk_add(DEV_SECTORS);
		if (!devs[i])
			return -1;
	}
	/* Registration makes the first device active; the arrays under test
	 * take their own members, so clear it. */
	anx_blk_set_active(NULL);
	return 0;
}

/* --- RAID 0 ----------------------------------------------------------- */

static int test_raid0_stripe(void)
{
	struct anx_blk_dev *devs[2];
	struct anx_md_array *a = NULL;
	uint64_t expect_sectors;
	uint32_t c;

	if (setup(2, devs) != 0)
		return -1;

	if (anx_md_create(NULL, ANX_MD_LEVEL_RAID0, CHUNK, devs, 2,
			  false, &a) != ANX_OK)
		return -2;

	/* Head metadata: 2048 sectors of slack, then the data area. */
	expect_sectors = DEV_SECTORS - ANX_MD_DATA_OFFSET;
	if (a->member_sectors != expect_sectors)
		return -3;
	if (a->array_sectors != expect_sectors * 2)
		return -4;
	if (anx_blk_dev_capacity(a->bdev) != expect_sectors * 2)
		return -5;
	if (!anx_blk_dev_is_member(devs[0]) || !anx_blk_dev_is_member(devs[1]))
		return -6;
	if (anx_md_state(a) != ANX_MD_ARRAY_CLEAN)
		return -7;

	/* Write four chunks and confirm they alternate between members. */
	fill_pattern(wbuf, CHUNK * 4, 3);
	if (anx_blk_dev_write(a->bdev, 0, CHUNK * 4, wbuf) != ANX_OK)
		return -8;

	for (c = 0; c < 4; c++) {
		struct anx_blk_dev *dev = devs[c % 2];
		uint64_t member_lba = ANX_MD_DATA_OFFSET +
				      (uint64_t)(c / 2) * CHUNK;

		if (anx_blk_dev_read(dev, member_lba, CHUNK, rbuf) != ANX_OK)
			return -9;
		if (!same(rbuf, wbuf + (size_t)c * CHUNK * 512, CHUNK))
			return -10 - (int)c;
	}

	/* A request that straddles chunk boundaries must round-trip. */
	fill_pattern(wbuf, CHUNK * 2, 11);
	if (anx_blk_dev_write(a->bdev, CHUNK / 2, CHUNK * 2, wbuf) != ANX_OK)
		return -20;
	anx_memset(rbuf, 0, sizeof(rbuf));
	if (anx_blk_dev_read(a->bdev, CHUNK / 2, CHUNK * 2, rbuf) != ANX_OK)
		return -21;
	if (!same(rbuf, wbuf, CHUNK * 2))
		return -22;

	/* Reads past the end of the array are rejected, not wrapped. */
	if (anx_blk_dev_read(a->bdev, a->array_sectors, 1, rbuf) == ANX_OK)
		return -23;

	/* A raid0 member failure takes the array down. */
	if (anx_md_fail(a, 1) != ANX_OK)
		return -24;
	if (anx_md_state(a) != ANX_MD_ARRAY_FAILED)
		return -25;

	stop(a);
	return 0;
}

/* --- RAID 1 ----------------------------------------------------------- */

static int test_raid1_mirror(void)
{
	struct anx_blk_dev *devs[3];
	struct anx_md_array *a = NULL;
	uint64_t expect_sectors;
	uint32_t i;

	if (setup(3, devs) != 0)
		return -1;

	if (anx_md_create(NULL, ANX_MD_LEVEL_RAID1, 0, devs, 2,
			  false, &a) != ANX_OK)
		return -2;

	expect_sectors = DEV_SECTORS - ANX_MD_DATA_OFFSET;
	if (a->array_sectors != expect_sectors)
		return -3;
	if (a->member_sectors != expect_sectors)
		return -4;

	/* Every member holds the same bytes at the same offset. */
	fill_pattern(wbuf, 4, 5);
	if (anx_blk_dev_write(a->bdev, 16, 4, wbuf) != ANX_OK)
		return -5;
	for (i = 0; i < 2; i++) {
		if (anx_blk_dev_read(devs[i], ANX_MD_DATA_OFFSET + 16, 4,
				     rbuf) != ANX_OK)
			return -6;
		if (!same(rbuf, wbuf, 4))
			return -7 - (int)i;
	}

	/* Reads alternate between members but always return the data. */
	for (i = 0; i < 4; i++) {
		anx_memset(rbuf, 0, sizeof(rbuf));
		if (anx_blk_dev_read(a->bdev, 16, 4, rbuf) != ANX_OK)
			return -10;
		if (!same(rbuf, wbuf, 4))
			return -11;
	}

	/* Lose one member: the array degrades but keeps serving. */
	if (anx_md_fail(a, 0) != ANX_OK)
		return -12;
	if (anx_md_state(a) != ANX_MD_ARRAY_DEGRADED)
		return -13;
	anx_memset(rbuf, 0, sizeof(rbuf));
	if (anx_blk_dev_read(a->bdev, 16, 4, rbuf) != ANX_OK)
		return -14;
	if (!same(rbuf, wbuf, 4))
		return -15;

	/* Writes while degraded reach the surviving member. */
	fill_pattern(wbuf, 4, 9);
	if (anx_blk_dev_write(a->bdev, 16, 4, wbuf) != ANX_OK)
		return -16;
	if (anx_blk_dev_read(devs[1], ANX_MD_DATA_OFFSET + 16, 4, rbuf)
	    != ANX_OK)
		return -17;
	if (!same(rbuf, wbuf, 4))
		return -18;

	/* Replace the failed member and rebuild onto it. */
	if (anx_md_add(a, devs[2]) != ANX_OK)
		return -19;
	if (anx_md_state(a) != ANX_MD_ARRAY_REBUILDING)
		return -20;
	if (anx_md_resync(a) != ANX_OK)
		return -21;
	if (anx_md_state(a) != ANX_MD_ARRAY_CLEAN)
		return -22;
	if (a->members[0].dev != devs[2])
		return -23;

	/* The rebuilt member carries the current contents. */
	anx_memset(rbuf, 0, sizeof(rbuf));
	if (anx_blk_dev_read(devs[2], ANX_MD_DATA_OFFSET + 16, 4, rbuf)
	    != ANX_OK)
		return -24;
	if (!same(rbuf, wbuf, 4))
		return -25;

	/* The drive that failed is free for reuse. */
	if (anx_blk_dev_is_member(devs[0]))
		return -26;

	stop(a);
	return 0;
}

/* --- Tail metadata ----------------------------------------------------- */

static int test_raid1_tail_metadata(void)
{
	struct anx_blk_dev *devs[2];
	struct anx_md_array *a = NULL;

	if (setup(2, devs) != 0)
		return -1;

	if (anx_md_create(NULL, ANX_MD_LEVEL_RAID1, 0, devs, 2,
			  true, &a) != ANX_OK)
		return -2;

	if (a->members[0].data_offset != 0)
		return -3;
	if (a->member_sectors != DEV_SECTORS - ANX_MD_SB_TAIL_BACK)
		return -4;

	/* Array sector 0 is member sector 0, so firmware reading the member
	 * as a plain drive sees the same bytes the array serves. */
	fill_pattern(wbuf, 2, 21);
	if (anx_blk_dev_write(a->bdev, 0, 2, wbuf) != ANX_OK)
		return -5;
	if (anx_blk_dev_read(devs[0], 0, 2, rbuf) != ANX_OK)
		return -6;
	if (!same(rbuf, wbuf, 2))
		return -7;

	stop(a);
	return 0;
}

/* --- Superblocks and assembly ------------------------------------------ */

static int test_md_assemble(void)
{
	struct anx_blk_dev *devs[2];
	struct anx_md_array *a = NULL;
	uint8_t uuid[16];

	if (setup(2, devs) != 0)
		return -1;

	if (anx_md_create(NULL, ANX_MD_LEVEL_RAID0, CHUNK, devs, 2,
			  false, &a) != ANX_OK)
		return -2;

	fill_pattern(wbuf, CHUNK * 3, 13);
	if (anx_blk_dev_write(a->bdev, CHUNK, CHUNK * 3, wbuf) != ANX_OK)
		return -3;
	anx_memcpy(uuid, a->uuid, sizeof(uuid));

	/* Stop the array. The members keep their superblocks. */
	anx_blk_set_active(NULL);
	if (anx_md_stop(a) != ANX_OK)
		return -4;
	if (anx_md_count() != 0)
		return -5;
	if (anx_blk_dev_is_member(devs[0]))
		return -6;

	/* Assembly rebuilds the array from the members alone. */
	if (anx_md_assemble() != 1)
		return -7;
	if (anx_md_count() != 1)
		return -8;

	a = anx_md_at(0);
	if (!a)
		return -9;
	if (a->level != ANX_MD_LEVEL_RAID0)
		return -10;
	if (a->chunk_sectors != CHUNK)
		return -11;
	if (a->member_count != 2)
		return -12;
	if (a->array_sectors != (DEV_SECTORS - ANX_MD_DATA_OFFSET) * 2)
		return -13;
	if (anx_memcmp(a->uuid, uuid, sizeof(uuid)) != 0)
		return -14;
	if (anx_md_state(a) != ANX_MD_ARRAY_CLEAN)
		return -15;

	/* The data written before the stop reads back through the new array. */
	anx_memset(rbuf, 0, sizeof(rbuf));
	if (anx_blk_dev_read(a->bdev, CHUNK, CHUNK * 3, rbuf) != ANX_OK)
		return -16;
	if (!same(rbuf, wbuf, CHUNK * 3))
		return -17;

	/* A second scan must not start the same array twice. */
	if (anx_md_assemble() != 0)
		return -18;
	if (anx_md_count() != 1)
		return -19;

	/* Erasing a superblock takes the member out of future assemblies. */
	anx_blk_set_active(NULL);
	if (anx_md_stop(a) != ANX_OK)
		return -20;
	if (anx_md_zero_super(devs[1]) != ANX_OK)
		return -21;
	if (anx_md_assemble() != 0)
		return -22;
	if (anx_md_count() != 0)
		return -23;

	return 0;
}

/* --- Argument checking -------------------------------------------------- */

static int test_md_rejects_bad_arrays(void)
{
	struct anx_blk_dev *devs[2];
	struct anx_md_array *a = NULL;
	struct anx_blk_dev *dup[2];

	if (setup(2, devs) != 0)
		return -1;

	/* One member is not an array. */
	if (anx_md_create(NULL, ANX_MD_LEVEL_RAID0, CHUNK, devs, 1,
			  false, &a) == ANX_OK)
		return -2;

	/* The same drive cannot fill two roles. */
	dup[0] = devs[0];
	dup[1] = devs[0];
	if (anx_md_create(NULL, ANX_MD_LEVEL_RAID0, CHUNK, dup, 2,
			  false, &a) == ANX_OK)
		return -3;

	/* RAID 0 chunks must be a power of two. */
	if (anx_md_create(NULL, ANX_MD_LEVEL_RAID0, 12, devs, 2,
			  false, &a) == ANX_OK)
		return -4;

	/* Levels other than 0 and 1 are not implemented. */
	if (anx_md_create(NULL, 5, CHUNK, devs, 2, false, &a) == ANX_OK)
		return -5;

	/* A drive already in an array cannot join another. */
	if (anx_md_create(NULL, ANX_MD_LEVEL_RAID1, 0, devs, 2,
			  false, &a) != ANX_OK)
		return -6;
	if (anx_md_create(NULL, ANX_MD_LEVEL_RAID1, 0, devs, 2,
			  false, NULL) != ANX_EBUSY)
		return -7;

	/* An array serving the whole-system API cannot be stopped. */
	anx_blk_set_active(a->bdev);
	if (anx_md_stop(a) != ANX_EBUSY)
		return -8;
	anx_blk_set_active(NULL);
	anx_md_stop(a);

	if (anx_md_parse_level("raid1") != ANX_MD_LEVEL_RAID1)
		return -9;
	if (anx_md_parse_level("0") != ANX_MD_LEVEL_RAID0)
		return -10;
	if (anx_md_parse_level("raid5") >= 0)
		return -11;

	return 0;
}

int test_md_raid(void)
{
	int ret;

	ret = test_raid0_stripe();
	if (ret != 0)
		goto out;
	ret = test_raid1_mirror();
	if (ret != 0)
		goto out;
	ret = test_raid1_tail_metadata();
	if (ret != 0)
		goto out;
	ret = test_md_assemble();
	if (ret != 0)
		goto out;
	ret = test_md_rejects_bad_arrays();
out:
	test_mock_blk_teardown();
	return ret;
}
