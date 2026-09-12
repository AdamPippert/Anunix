/*
 * test_blk_probe.c — Format guard (RFC-0031 section 8).
 *
 * The 2026.9.4 boot path formatted the active device whenever it failed to
 * find an object store, reading nothing first. These tests assert the
 * refusals, and that a refused format left sector 0 byte-identical. A guard
 * that returns an error after writing would be worse than no guard at all,
 * because it would look like it worked.
 */

#include <anx/types.h>
#include <anx/blk.h>
#include <anx/blk_probe.h>
#include <anx/objstore_disk.h>
#include <anx/md.h>
#include <anx/mock_blk.h>
#include <anx/string.h>
#include <anx/kprintf.h>

#define DEV_SECTORS	8192

static uint8_t sec[512];

#define CHECK(cond, msg)						\
	do {								\
		if (!(cond)) {						\
			kprintf("  FAIL: %s\n", (msg));			\
			return -1;					\
		}							\
	} while (0)

static struct anx_blk_dev *blank_disk(void)
{
	struct anx_blk_dev *dev;

	test_mock_blk_teardown();
	dev = test_mock_blk_add(DEV_SECTORS);
	if (dev)
		anx_blk_set_active(dev);
	return dev;
}

/* Write buf to one sector of dev. */
static int put(struct anx_blk_dev *dev, uint64_t lba, const uint8_t *buf)
{
	return anx_blk_dev_write(dev, lba, 1, buf);
}

/* Place a signature at an offset inside a zeroed sector. */
static void sig(uint32_t off, const char *bytes, uint32_t len)
{
	uint32_t i;

	anx_memset(sec, 0, sizeof(sec));
	for (i = 0; i < len; i++)
		sec[off + i] = (uint8_t)bytes[i];
}

/* Probe a disk carrying one signature at lba/off, expecting `want`. */
static int probe_case(uint64_t lba, uint32_t off, const char *bytes,
		      uint32_t len, enum anx_blk_content want,
		      const char *msg)
{
	struct anx_blk_dev *dev = blank_disk();
	enum anx_blk_content got;
	char what[ANX_PROBE_DESC_MAX];

	if (!dev)
		return -1;
	sig(off, bytes, len);
	if (put(dev, lba, sec) != ANX_OK)
		return -1;

	got = anx_blk_probe(dev, what, sizeof(what));
	if (got != want) {
		kprintf("  FAIL: %s (got %s '%s')\n", msg,
			anx_blk_content_name(got), what);
		return -1;
	}
	return 0;
}

int test_blk_probe(void)
{
	struct anx_blk_dev *dev;
	enum anx_blk_content got;
	char what[ANX_PROBE_DESC_MAX];
	uint8_t before[512];
	uint8_t after[512];
	uint32_t magic;
	int ret;

	/* ---- a zeroed device is blank ---- */
	dev = blank_disk();
	CHECK(dev != NULL, "could not create mock device");
	got = anx_blk_probe(dev, what, sizeof(what));
	CHECK(got == ANX_CONTENT_BLANK, "a zeroed device must probe BLANK");

	/* ---- foreign structures ---- */
	CHECK(probe_case(1, 0, "EFI PART", 8, ANX_CONTENT_FOREIGN,
			 "GPT must probe FOREIGN") == 0, "gpt");
	CHECK(probe_case(0, 0x1FE, "\x55\xAA", 2, ANX_CONTENT_FOREIGN,
			 "MBR signature must probe FOREIGN") == 0, "mbr");
	CHECK(probe_case(0, 0, "XFSB", 4, ANX_CONTENT_FOREIGN,
			 "xfs must probe FOREIGN") == 0, "xfs");
	CHECK(probe_case(0, 0, "LUKS\xBA\xBE", 6, ANX_CONTENT_FOREIGN,
			 "LUKS must probe FOREIGN") == 0, "luks");
	CHECK(probe_case(2, 0x38, "\x53\xEF", 2, ANX_CONTENT_FOREIGN,
			 "ext4 must probe FOREIGN") == 0, "ext4");
	CHECK(probe_case(128, 0x40, "_BHRfS_M", 8, ANX_CONTENT_FOREIGN,
			 "btrfs must probe FOREIGN") == 0, "btrfs");
	CHECK(probe_case(0, 3, "NTFS    ", 8, ANX_CONTENT_FOREIGN,
			 "ntfs must probe FOREIGN") == 0, "ntfs");
	CHECK(probe_case(8, 0, "\xFC\x4E\x2B\xA9", 4, ANX_CONTENT_FOREIGN,
			 "Linux RAID must probe FOREIGN") == 0, "mdraid");

	/* ---- unrecognised bytes are still someone's data ---- */
	dev = blank_disk();
	CHECK(dev != NULL, "mock device");
	anx_memset(sec, 0x42, sizeof(sec));
	CHECK(put(dev, 0, sec) == ANX_OK, "write junk");
	got = anx_blk_probe(dev, what, sizeof(what));
	CHECK(got == ANX_CONTENT_FOREIGN,
	      "unrecognised non-zero data must probe FOREIGN, not BLANK");

	/* ---- our own store is ours ---- */
	dev = blank_disk();
	CHECK(dev != NULL, "mock device");
	anx_memset(sec, 0, sizeof(sec));
	magic = ANX_DISK_MAGIC;
	anx_memcpy(sec, &magic, 4);
	CHECK(put(dev, 0, sec) == ANX_OK, "write store magic");
	got = anx_blk_probe(dev, what, sizeof(what));
	CHECK(got == ANX_CONTENT_ANUNIX,
	      "an Anunix superblock must probe ANUNIX, not FOREIGN");

	/* ---- the guard refuses a foreign disk and writes nothing ---- */
	dev = blank_disk();
	CHECK(dev != NULL, "mock device");
	anx_memset(sec, 0, sizeof(sec));
	anx_memcpy(sec, "EFI PART", 8);
	CHECK(put(dev, 1, sec) == ANX_OK, "write GPT header");
	/* Seed sector 0 with a protective-MBR-like pattern we can check. */
	anx_memset(sec, 0, sizeof(sec));
	sec[0x1FE] = 0x55;
	sec[0x1FF] = 0xAA;
	sec[0] = 0xEE;
	CHECK(put(dev, 0, sec) == ANX_OK, "write protective MBR");
	CHECK(anx_blk_dev_read(dev, 0, 1, before) == ANX_OK, "snapshot");

	ret = anx_disk_format("should-not-happen");
	CHECK(ret != ANX_OK, "format of a partitioned disk must fail");
	CHECK(ret == ANX_EEXIST, "refusal for content must be ANX_EEXIST");

	CHECK(anx_blk_dev_read(dev, 0, 1, after) == ANX_OK, "re-read");
	CHECK(anx_memcmp(before, after, 512) == 0,
	      "a refused format must leave sector 0 byte-identical");

	/* ---- an array member is refused too ---- */
	dev = blank_disk();
	CHECK(dev != NULL, "mock device");
	anx_blk_dev_claim(dev);
	ret = anx_disk_format("should-not-happen");
	CHECK(ret == ANX_EBUSY, "an array member must be refused with EBUSY");
	anx_blk_dev_release(dev);

	/* ---- a blank device formats ---- */
	dev = blank_disk();
	CHECK(dev != NULL, "mock device");
	CHECK(anx_disk_format("testvol") == ANX_OK,
	      "a blank device must format");
	CHECK(anx_blk_dev_read(dev, 0, 1, after) == ANX_OK, "read super");
	anx_memcpy(&magic, after, 4);
	CHECK(magic == ANX_DISK_MAGIC, "superblock magic missing after format");

	/* ---- reformatting our own store is allowed ---- */
	CHECK(anx_disk_format("testvol2") == ANX_OK,
	      "an existing Anunix store must be reformattable");

	/* ---- the forced path overwrites a foreign disk ---- */
	dev = blank_disk();
	CHECK(dev != NULL, "mock device");
	anx_memset(sec, 0, sizeof(sec));
	anx_memcpy(sec, "XFSB", 4);
	CHECK(put(dev, 0, sec) == ANX_OK, "write xfs magic");
	CHECK(anx_disk_format("forced") == ANX_EEXIST,
	      "guarded format must still refuse xfs");
	CHECK(anx_disk_format_forced("forced") == ANX_OK,
	      "forced format must proceed");
	CHECK(anx_blk_dev_read(dev, 0, 1, after) == ANX_OK, "read super");
	anx_memcpy(&magic, after, 4);
	CHECK(magic == ANX_DISK_MAGIC, "forced format did not write a store");

	test_mock_blk_teardown();
	return 0;
}

/*
 * Active-device selection (RFC-0031 section 4).
 *
 * "First registered wins" picks whatever the firmware enumerated first,
 * which on a partitioned machine is typically an EFI system partition.
 * Selection must follow the superblock instead.
 */
int test_blk_select(void)
{
	struct anx_blk_dev *d0, *d1, *d2, *chosen;
	uint32_t magic = ANX_DISK_MAGIC;
	uint32_t version = ANX_DISK_VERSION;

	/* Three devices; the store lives on the third. */
	test_mock_blk_teardown();
	d0 = test_mock_blk_add(DEV_SECTORS);
	d1 = test_mock_blk_add(DEV_SECTORS);
	d2 = test_mock_blk_add(DEV_SECTORS);
	CHECK(d0 && d1 && d2, "could not create mock devices");
	CHECK(anx_blk_active() == d0, "first registered should start active");

	anx_memset(sec, 0, sizeof(sec));
	anx_memcpy(sec, &magic, 4);
	anx_memcpy(sec + 4, &version, 4);
	CHECK(put(d2, 0, sec) == ANX_OK, "write superblock to d2");

	chosen = anx_disk_select_store();
	CHECK(chosen == d2, "selection must follow the superblock, not order");
	CHECK(anx_blk_active() == d2, "active device must be updated");

	/* No store anywhere: leave the active device alone rather than
	 * picking something arbitrary to write to. */
	test_mock_blk_teardown();
	d0 = test_mock_blk_add(DEV_SECTORS);
	CHECK(d0 != NULL, "mock device");
	CHECK(anx_disk_select_store() == NULL,
	      "no superblock anywhere must select nothing");

	/* An array member carrying a stale superblock is never selected:
	 * it belongs to its array. */
	test_mock_blk_teardown();
	d0 = test_mock_blk_add(DEV_SECTORS);
	d1 = test_mock_blk_add(DEV_SECTORS);
	CHECK(d0 && d1, "mock devices");
	anx_memset(sec, 0, sizeof(sec));
	anx_memcpy(sec, &magic, 4);
	anx_memcpy(sec + 4, &version, 4);
	CHECK(put(d1, 0, sec) == ANX_OK, "write superblock to d1");
	anx_blk_dev_claim(d1);
	CHECK(anx_disk_select_store() == NULL,
	      "an array member must never be selected");
	anx_blk_dev_release(d1);
	CHECK(anx_disk_select_store() == d1,
	      "released member should now be selectable");

	/* A superblock this kernel does not understand is not a store. */
	test_mock_blk_teardown();
	d0 = test_mock_blk_add(DEV_SECTORS);
	CHECK(d0 != NULL, "mock device");
	anx_memset(sec, 0, sizeof(sec));
	anx_memcpy(sec, &magic, 4);
	{
		uint32_t bad = ANX_DISK_VERSION + 99;

		anx_memcpy(sec + 4, &bad, 4);
	}
	CHECK(put(d0, 0, sec) == ANX_OK, "write future-version superblock");
	CHECK(anx_disk_select_store() == NULL,
	      "an unknown superblock version must not be selected");

	test_mock_blk_teardown();
	return 0;
}
