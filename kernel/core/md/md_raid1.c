/*
 * md_raid1.c — Mirroring.
 *
 * Every member holds the same data, so array LBA and member LBA are the
 * same number. Writes go to every member that is not faulty. Reads come
 * from one active member, chosen round-robin so two readers can use two
 * drives, and fall through to the next active member when one fails.
 *
 * A member added to replace a failed one enters as a spare: it takes
 * writes so it never falls further behind, but serves no reads until
 * anx_md_resync() has copied the array onto it.
 */

#include <anx/types.h>
#include <anx/md.h>
#include <anx/string.h>
#include <anx/kprintf.h>

/* Sectors copied per rebuild pass. 64 KiB keeps the static buffer small
 * while still giving the drive a request worth queueing. */
#define RAID1_RESYNC_CHUNK	128

static bool raid1_readable(const struct anx_md_array *a, uint32_t idx)
{
	const struct anx_md_member *m = &a->members[idx];

	return m->dev && m->state == ANX_MD_MEMBER_ACTIVE;
}

int anx_md_raid1_read(struct anx_md_array *a, uint64_t lba,
		      uint32_t count, void *buf)
{
	uint32_t tried;
	int last = ANX_ENODEV;

	if (a->member_count == 0)
		return ANX_EINVAL;

	for (tried = 0; tried < a->member_count; tried++) {
		uint32_t idx = (a->read_cursor + tried) % a->member_count;
		int ret;

		if (!raid1_readable(a, idx))
			continue;

		ret = anx_md_member_read(a, idx, lba, count, buf);
		if (ret == ANX_OK) {
			/* Hand the next read to the next drive. */
			a->read_cursor = (idx + 1) % a->member_count;
			return ANX_OK;
		}
		anx_md_member_fault(a, idx);
		last = ret;
	}
	return last;
}

int anx_md_raid1_write(struct anx_md_array *a, uint64_t lba,
		       uint32_t count, const void *buf)
{
	uint32_t i;
	uint32_t ok = 0;
	int last = ANX_ENODEV;

	for (i = 0; i < a->member_count; i++) {
		struct anx_md_member *m = &a->members[i];
		int ret;

		if (!m->dev || m->state == ANX_MD_MEMBER_FAULTY)
			continue;

		/* A spare only holds valid data below the rebuild point.
		 * Writing above it is harmless but pointless: the rebuild
		 * copies that region later. */
		if (m->state == ANX_MD_MEMBER_SPARE &&
		    lba >= a->resync_offset)
			continue;

		ret = anx_md_member_write(a, i, lba, count, buf);
		if (ret == ANX_OK) {
			if (m->state == ANX_MD_MEMBER_ACTIVE)
				ok++;
			continue;
		}
		anx_md_member_fault(a, i);
		last = ret;
	}

	return ok > 0 ? ANX_OK : last;
}

/* --- Rebuild ---------------------------------------------------------- */

/* Index of the spare being rebuilt, or ANX_MD_MAX_MEMBERS when none. */
static uint32_t raid1_spare(const struct anx_md_array *a)
{
	uint32_t i;

	for (i = 0; i < a->member_count; i++)
		if (a->members[i].dev &&
		    a->members[i].state == ANX_MD_MEMBER_SPARE)
			return i;
	return ANX_MD_MAX_MEMBERS;
}

int anx_md_resync_step(struct anx_md_array *a, uint32_t max_sectors)
{
	/* Sector-aligned: NVMe describes a transfer with one PRP entry, so
	 * a 512-byte read must not straddle a page boundary. */
	static uint8_t buf[RAID1_RESYNC_CHUNK * 512]
		__attribute__((aligned(512)));
	uint32_t spare;
	uint32_t run;
	int ret;

	if (!a || !a->used)
		return ANX_EINVAL;
	if (a->level != ANX_MD_LEVEL_RAID1)
		return ANX_ENOTSUP;

	spare = raid1_spare(a);
	if (spare == ANX_MD_MAX_MEMBERS)
		return ANX_ENOENT;
	if (a->resync_offset >= a->member_sectors)
		return 0;

	run = RAID1_RESYNC_CHUNK;
	if (max_sectors != 0 && run > max_sectors)
		run = max_sectors;
	if ((uint64_t)run > a->member_sectors - a->resync_offset)
		run = (uint32_t)(a->member_sectors - a->resync_offset);

	/* Source the copy through the normal read path so a second failure
	 * mid-rebuild falls over to another surviving member. */
	ret = anx_md_raid1_read(a, a->resync_offset, run, buf);
	if (ret != ANX_OK)
		return ret;

	ret = anx_md_member_write(a, spare, a->resync_offset, run, buf);
	if (ret != ANX_OK) {
		anx_md_member_fault(a, spare);
		return ret;
	}

	a->resync_offset += run;

	if (a->resync_offset >= a->member_sectors) {
		a->members[spare].state = ANX_MD_MEMBER_ACTIVE;
		anx_md_super_sync(a);
		kprintf("md: %s: rebuild complete, member %u in sync\n",
			a->name, spare);
	}
	return (int)run;
}

int anx_md_resync(struct anx_md_array *a)
{
	uint64_t last_report = 0;

	if (!a || !a->used)
		return ANX_EINVAL;
	if (a->level != ANX_MD_LEVEL_RAID1)
		return ANX_ENOTSUP;
	if (raid1_spare(a) == ANX_MD_MAX_MEMBERS)
		return ANX_ENOENT;

	kprintf("md: %s: rebuilding %llu MiB\n", a->name,
		(unsigned long long)(a->member_sectors / 2048));

	for (;;) {
		int ret;

		/* The last step promoted the spare, so there is no rebuild
		 * left to run. */
		if (raid1_spare(a) == ANX_MD_MAX_MEMBERS)
			break;

		ret = anx_md_resync_step(a, 0);
		if (ret < 0)
			return ret;
		if (ret == 0)
			break;

		/* Report every 64 MiB so a long rebuild shows progress. */
		if (a->resync_offset - last_report >= 131072) {
			last_report = a->resync_offset;
			kprintf("md: %s: rebuilt %llu/%llu MiB\n", a->name,
				(unsigned long long)(a->resync_offset / 2048),
				(unsigned long long)(a->member_sectors / 2048));
		}
	}
	return ANX_OK;
}
