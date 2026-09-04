/*
 * md_raid0.c — Striping.
 *
 * Array LBAs are cut into chunks of chunk_sectors. Chunk N lives on
 * member (N mod member_count) at chunk offset (N div member_count), so a
 * sequential read of one stripe issues one request per member and the
 * drives work in parallel. Capacity is member_sectors × member_count.
 *
 * There is no redundancy. One failed member fails the array, and the
 * request that discovered the failure returns the driver's error.
 */

#include <anx/types.h>
#include <anx/md.h>

/*
 * Walk the request one chunk at a time. A request never spans a chunk
 * boundary on a member, so each pass issues one member request of at
 * most chunk_sectors.
 */
static int raid0_io(struct anx_md_array *a, uint64_t lba, uint32_t count,
		    uint8_t *buf, bool write)
{
	uint32_t chunk = a->chunk_sectors;

	if (chunk == 0 || a->member_count == 0)
		return ANX_EINVAL;

	while (count > 0) {
		uint64_t chunk_index = lba / chunk;
		uint32_t offset      = (uint32_t)(lba % chunk);
		uint32_t idx         = (uint32_t)(chunk_index %
						  a->member_count);
		uint64_t member_lba  = (chunk_index / a->member_count) *
				       chunk + offset;
		uint32_t run         = chunk - offset;
		int ret;

		if (run > count)
			run = count;

		if (write)
			ret = anx_md_member_write(a, idx, member_lba, run,
						  buf);
		else
			ret = anx_md_member_read(a, idx, member_lba, run,
						 buf);
		if (ret != ANX_OK) {
			anx_md_member_fault(a, idx);
			return ret;
		}

		lba   += run;
		buf   += (uint64_t)run * 512;
		count -= run;
	}
	return ANX_OK;
}

int anx_md_raid0_read(struct anx_md_array *a, uint64_t lba,
		      uint32_t count, void *buf)
{
	return raid0_io(a, lba, count, buf, false);
}

int anx_md_raid0_write(struct anx_md_array *a, uint64_t lba,
		       uint32_t count, const void *buf)
{
	return raid0_io(a, lba, count, (uint8_t *)(uintptr_t)buf, true);
}
