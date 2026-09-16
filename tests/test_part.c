/*
 * test_part.c — Partition layer (RFC-0031).
 *
 * Builds a real GPT on a RAM-backed mock device, scans it, and checks the
 * partitions that come back. The clamp tests matter most: they assert the
 * failure, and that a rejected write left the parent untouched. A clamp
 * that returns an error after writing is the bug worth catching.
 */

#include <anx/types.h>
#include <anx/part.h>
#include <anx/gpt.h>
#include <anx/blk.h>
#include <anx/crc32.h>
#include <anx/mock_blk.h>
#include <anx/string.h>
#include <anx/kprintf.h>

#define DEV_SECTORS	8192		/* 4 MiB mock device */
#define ENTRY_COUNT	128
#define ENTRY_SIZE	128
#define ENTRY_LBA	2
#define ENTRY_SECTORS	((ENTRY_COUNT * ENTRY_SIZE) / 512)	/* 32 */

static uint8_t sector[512];
static uint8_t entries[ENTRY_COUNT * ENTRY_SIZE];

#define CHECK(cond, msg)						\
	do {								\
		if (!(cond)) {						\
			kprintf("  FAIL: %s\n", (msg));			\
			return -1;					\
		}							\
	} while (0)

static void put32(uint8_t *p, uint32_t v) { anx_memcpy(p, &v, 4); }
static void put64(uint8_t *p, uint64_t v) { anx_memcpy(p, &v, 8); }

/* Write a GPT entry: 1-based slot, inclusive end, ASCII name to UTF-16LE. */
static void set_entry(uint32_t slot, uint64_t start, uint64_t end,
		      const char *name)
{
	uint8_t *e = entries + (slot - 1) * ENTRY_SIZE;
	uint32_t i;

	anx_memset(e, 0, ENTRY_SIZE);
	/* Generic Linux filesystem type GUID -- the same one the real
	 * reserve partitions on jekyll carry. */
	put64(e + 0,  0x477284838F3DC60FULL);
	put64(e + 8,  0xE47D47D8693D798EULL);
	put64(e + 16, 0x1111111111111111ULL + slot);
	put64(e + 24, 0x2222222222222222ULL + slot);
	put64(e + 32, start);
	put64(e + 40, end);
	put64(e + 48, 0);
	for (i = 0; name && name[i] && i < 36; i++)
		e[56 + i * 2] = (uint8_t)name[i];
}

/* Compose a GPT header into `sector` for the given entry-array CRC. */
static void build_header(uint64_t my_lba, uint64_t alt_lba, uint64_t dev_sectors,
			 uint32_t entries_crc)
{
	uint32_t crc;

	anx_memset(sector, 0, sizeof(sector));
	put64(sector + 0,  0x5452415020494645ULL);	/* "EFI PART" */
	put32(sector + 8,  0x00010000);			/* revision 1.0 */
	put32(sector + 12, 92);				/* header size */
	put32(sector + 16, 0);				/* CRC, filled below */
	put64(sector + 24, my_lba);
	put64(sector + 32, alt_lba);
	put64(sector + 40, 34);				/* first usable */
	put64(sector + 48, dev_sectors - 34);		/* last usable */
	put64(sector + 72, ENTRY_LBA);
	put32(sector + 80, ENTRY_COUNT);
	put32(sector + 84, ENTRY_SIZE);
	put32(sector + 88, entries_crc);

	crc = anx_crc32(sector, 92);
	put32(sector + 16, crc);
}

/* Lay a valid GPT down on dev. Returns 0 on success. */
static int write_gpt(struct anx_blk_dev *dev)
{
	uint32_t entries_crc;
	uint32_t i;

	anx_memset(entries, 0, sizeof(entries));
	set_entry(1, 2048, 2048 + 1023, "TEST_EFI");		/* 1024 sec */
	set_entry(2, 4096, 4096 + 2047, "ANUNIX_RAID0_A");	/* 2048 sec */
	set_entry(4, 6400, 6400 + 511,  "SPARSE_SLOT");		/* 512 sec  */

	entries_crc = anx_crc32(entries, sizeof(entries));

	build_header(1, DEV_SECTORS - 1, DEV_SECTORS, entries_crc);
	if (anx_blk_dev_write(dev, 1, 1, sector) != ANX_OK)
		return -1;

	for (i = 0; i < ENTRY_SECTORS; i++) {
		if (anx_blk_dev_write(dev, ENTRY_LBA + i, 1,
				      entries + i * 512) != ANX_OK)
			return -1;
	}
	return 0;
}

static struct anx_blk_dev *fresh_disk(void)
{
	struct anx_blk_dev *dev;

	test_mock_blk_teardown();
	dev = test_mock_blk_add(DEV_SECTORS);
	if (!dev)
		return NULL;
	anx_blk_set_active(NULL);
	return dev;
}

int test_part(void)
{
	struct anx_blk_dev *dev, *p1, *p2, *p4;
	const struct anx_part *pp;
	uint8_t buf[512];
	uint8_t probe[512];
	uint32_t i;
	int n;

	/* ---- parse a valid table ---- */
	dev = fresh_disk();
	CHECK(dev != NULL, "could not create mock device");
	CHECK(write_gpt(dev) == 0, "could not write test GPT");

	n = anx_part_scan(dev);
	CHECK(n == 3, "expected 3 partitions from the test GPT");

	/* ---- naming follows the GPT entry index, not scan order ---- */
	p1 = anx_blk_dev_find("mock0p1");
	p2 = anx_blk_dev_find("mock0p2");
	p4 = anx_blk_dev_find("mock0p4");
	CHECK(p1 && p2 && p4, "partitions not named <parent>p<entry index>");
	CHECK(anx_blk_dev_find("mock0p3") == NULL,
	      "empty GPT slot 3 must not register a device");

	/* ---- geometry ---- */
	pp = anx_part_of(p2);
	CHECK(pp != NULL, "anx_part_of returned NULL for a partition");
	CHECK(pp->start_lba == 4096, "partition 2 start_lba wrong");
	CHECK(pp->sectors == 2048, "partition 2 length wrong");
	CHECK(anx_blk_dev_capacity(p2) == 2048, "capacity must be the length");
	CHECK(anx_strcmp(pp->label, "ANUNIX_RAID0_A") == 0,
	      "GPT label did not decode");
	CHECK(anx_part_is_partition(p2), "partition not flagged as one");
	CHECK(!anx_part_is_partition(dev), "parent flagged as a partition");

	/* ---- offset: partition LBA 0 is parent start_lba ---- */
	for (i = 0; i < 512; i++)
		buf[i] = (uint8_t)(i * 7u + 3u);
	CHECK(anx_blk_dev_write(p2, 0, 1, buf) == ANX_OK,
	      "write to partition LBA 0 failed");
	CHECK(anx_blk_dev_read(dev, 4096, 1, probe) == ANX_OK,
	      "read back from parent failed");
	CHECK(anx_memcmp(buf, probe, 512) == 0,
	      "partition LBA 0 did not land on parent start_lba");

	/* ---- clamp: last valid sector works, one past does not ---- */
	CHECK(anx_blk_dev_read(p2, 2047, 1, probe) == ANX_OK,
	      "read of the last sector must succeed");
	CHECK(anx_blk_dev_read(p2, 2048, 1, probe) == ANX_ERANGE,
	      "read one sector past the end must return ANX_ERANGE");
	CHECK(anx_blk_dev_read(p2, 2040, 16, probe) == ANX_ERANGE,
	      "read straddling the end must return ANX_ERANGE");

	/* ---- clamp on write, and prove nothing was written ---- */
	anx_memset(buf, 0xAB, sizeof(buf));
	/* Seed the parent sector just past the partition with a known value. */
	anx_memset(probe, 0x5A, sizeof(probe));
	CHECK(anx_blk_dev_write(dev, 4096 + 2048, 1, probe) == ANX_OK,
	      "seeding the sector past the partition failed");

	CHECK(anx_blk_dev_write(p2, 2048, 1, buf) == ANX_ERANGE,
	      "write past the end must return ANX_ERANGE");

	CHECK(anx_blk_dev_read(dev, 4096 + 2048, 1, probe) == ANX_OK,
	      "re-read of the guarded sector failed");
	for (i = 0; i < 512; i++)
		CHECK(probe[i] == 0x5A,
		      "rejected write modified the parent past the partition");

	/* ---- overflow: an lba near UINT64_MAX must not wrap in ---- */
	CHECK(anx_blk_dev_read(p2, 0xFFFFFFFFFFFFFFFFULL, 1, probe)
	      == ANX_ERANGE, "huge lba must be rejected, not wrapped");
	CHECK(anx_blk_dev_write(p2, 0xFFFFFFFFFFFFFF00ULL, 8, buf)
	      == ANX_ERANGE, "huge lba write must be rejected");

	/* ---- partitions do not nest ---- */
	CHECK(anx_part_scan(p2) == 0, "a partition must not be scanned");

	/* ---- a corrupt header CRC is refused ---- */
	dev = fresh_disk();
	CHECK(dev != NULL, "could not create mock device");
	CHECK(write_gpt(dev) == 0, "could not write test GPT");
	CHECK(anx_blk_dev_read(dev, 1, 1, sector) == ANX_OK, "header read");
	sector[16] ^= 0xFF;			/* break the header CRC */
	CHECK(anx_blk_dev_write(dev, 1, 1, sector) == ANX_OK, "header write");
	/* Break the backup too, so there is nothing to fall back to. */
	anx_memset(sector, 0, sizeof(sector));
	CHECK(anx_blk_dev_write(dev, DEV_SECTORS - 1, 1, sector) == ANX_OK,
	      "backup header write");
	CHECK(anx_part_scan(dev) == 0,
	      "a table with a bad header CRC must register nothing");

	/* ---- a damaged primary falls back to the backup ---- */
	dev = fresh_disk();
	CHECK(dev != NULL, "could not create mock device");
	CHECK(write_gpt(dev) == 0, "could not write test GPT");
	/* Write a valid backup header at the last sector, then destroy the
	 * primary. my_lba/alt_lba swap for the backup. */
	build_header(DEV_SECTORS - 1, 1, DEV_SECTORS,
		     anx_crc32(entries, sizeof(entries)));
	CHECK(anx_blk_dev_write(dev, DEV_SECTORS - 1, 1, sector) == ANX_OK,
	      "backup header write");
	anx_memset(sector, 0, sizeof(sector));
	CHECK(anx_blk_dev_write(dev, 1, 1, sector) == ANX_OK,
	      "primary header wipe");
	CHECK(anx_part_scan(dev) == 3,
	      "a wiped primary must fall back to the backup header");

	/* ---- entries outside the device are skipped ---- */
	dev = fresh_disk();
	CHECK(dev != NULL, "could not create mock device");
	anx_memset(entries, 0, sizeof(entries));
	set_entry(1, 2048, 2048 + 1023, "GOOD");
	set_entry(2, DEV_SECTORS - 8, DEV_SECTORS + 4096, "RUNS_OFF_END");
	set_entry(3, 0, 1023, "START_AT_ZERO");
	set_entry(5, 5000, 4000, "END_BEFORE_START");
	build_header(1, DEV_SECTORS - 1, DEV_SECTORS,
		     anx_crc32(entries, sizeof(entries)));
	CHECK(anx_blk_dev_write(dev, 1, 1, sector) == ANX_OK, "header write");
	for (i = 0; i < ENTRY_SECTORS; i++)
		CHECK(anx_blk_dev_write(dev, ENTRY_LBA + i, 1,
					entries + i * 512) == ANX_OK,
		      "entry write");
	CHECK(anx_part_scan(dev) == 1,
	      "only the in-bounds entry may register");
	CHECK(anx_blk_dev_find("mock0p1") != NULL, "the good entry is missing");
	CHECK(anx_blk_dev_find("mock0p2") == NULL, "out-of-range entry registered");
	CHECK(anx_blk_dev_find("mock0p3") == NULL, "zero-start entry registered");
	CHECK(anx_blk_dev_find("mock0p5") == NULL, "inverted entry registered");

	test_mock_blk_teardown();
	return 0;
}
