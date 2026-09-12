/*
 * blk_probe.c — Recognise what is already on a block device (RFC-0031 §8).
 *
 * The 2026.9.4 boot path formatted the active device whenever it failed to
 * find an object store on it, reading nothing first. Booted against a disk
 * partitioned like jekyll it overwrote the protective MBR and the GPT
 * header with its own superblock. This file is what stands in the way of
 * that: everything not recognisably written by Anunix is treated as data
 * belonging to someone else.
 */

#include <anx/blk_probe.h>
#include <anx/blk.h>
#include <anx/objstore_disk.h>
#include <anx/md.h>
#include <anx/alloc.h>
#include <anx/string.h>

/*
 * Sectors that carry a signature we look for. btrfs sits furthest out at
 * 0x10040, which is sector 128. Reading one sector at a time keeps the
 * buffer small and lets a short device fail only the sectors past its end.
 */
#define SECT_MBR	0	/* MBR sig, Anunix store, XFS, LUKS, ANXR */
#define SECT_GPT	1	/* "EFI PART" */
#define SECT_EXT	2	/* ext2/3/4 magic at byte 0x438 */
#define SECT_MDRAID	8	/* Linux RAID 1.x at byte 0x1000 */
#define SECT_BTRFS	128	/* btrfs at byte 0x10040 */

static bool match(const uint8_t *buf, uint32_t off, const char *sig,
		  uint32_t len)
{
	uint32_t i;

	for (i = 0; i < len; i++) {
		if (buf[off + i] != (uint8_t)sig[i])
			return false;
	}
	return true;
}

static bool all_zero(const uint8_t *buf, uint32_t len)
{
	uint32_t i;

	for (i = 0; i < len; i++) {
		if (buf[i])
			return false;
	}
	return true;
}

static void say(char *desc, uint32_t desc_len, const char *what)
{
	if (desc && desc_len)
		anx_strlcpy(desc, what, desc_len);
}

const char *anx_blk_content_name(enum anx_blk_content c)
{
	switch (c) {
	case ANX_CONTENT_BLANK:
		return "blank";
	case ANX_CONTENT_ANUNIX:
		return "Anunix";
	case ANX_CONTENT_FOREIGN:
		return "foreign";
	}
	return "unknown";
}

enum anx_blk_content anx_blk_probe(struct anx_blk_dev *dev, char *desc,
				   uint32_t desc_len)
{
	uint8_t *buf;
	uint64_t capacity;
	uint32_t magic;
	enum anx_blk_content result = ANX_CONTENT_BLANK;
	bool saw_data = false;

	say(desc, desc_len, "unreadable");

	if (!dev)
		return ANX_CONTENT_FOREIGN;

	capacity = anx_blk_dev_capacity(dev);
	if (capacity == 0)
		return ANX_CONTENT_FOREIGN;

	buf = anx_alloc(512);
	if (!buf)
		return ANX_CONTENT_FOREIGN;	/* cannot check: assume theirs */

	/* ---- sector 0: our own store, arrays, and whole-device formats ---- */
	if (anx_blk_dev_read(dev, SECT_MBR, 1, buf) != ANX_OK) {
		anx_free(buf);
		return ANX_CONTENT_FOREIGN;
	}

	anx_memcpy(&magic, buf, 4);
	if (magic == ANX_DISK_MAGIC) {
		say(desc, desc_len, "Anunix object store");
		anx_free(buf);
		return ANX_CONTENT_ANUNIX;
	}
	if (magic == ANX_MD_MAGIC) {
		say(desc, desc_len, "Anunix RAID member");
		anx_free(buf);
		return ANX_CONTENT_ANUNIX;
	}

	if (match(buf, 0, "XFSB", 4)) {
		say(desc, desc_len, "xfs");
		anx_free(buf);
		return ANX_CONTENT_FOREIGN;
	}
	if (match(buf, 0, "LUKS\xBA\xBE", 6)) {
		say(desc, desc_len, "LUKS");
		anx_free(buf);
		return ANX_CONTENT_FOREIGN;
	}
	if (match(buf, 3, "NTFS    ", 8)) {
		say(desc, desc_len, "ntfs");
		anx_free(buf);
		return ANX_CONTENT_FOREIGN;
	}
	if (match(buf, 0x36, "FAT", 3) || match(buf, 0x52, "FAT32", 5)) {
		say(desc, desc_len, "fat");
		anx_free(buf);
		return ANX_CONTENT_FOREIGN;
	}

	/*
	 * The MBR signature is checked after the filesystems above, because a
	 * FAT or NTFS boot sector carries it too and the filesystem name is
	 * the more useful thing to report.
	 */
	if (buf[0x1FE] == 0x55 && buf[0x1FF] == 0xAA) {
		say(desc, desc_len, "MBR partition table");
		result = ANX_CONTENT_FOREIGN;
	}
	if (!all_zero(buf, 512))
		saw_data = true;

	/* ---- sector 1: GPT ---- */
	if (result == ANX_CONTENT_BLANK && capacity > SECT_GPT) {
		if (anx_blk_dev_read(dev, SECT_GPT, 1, buf) != ANX_OK) {
			anx_free(buf);
			return ANX_CONTENT_FOREIGN;
		}
		if (match(buf, 0, "EFI PART", 8)) {
			say(desc, desc_len, "GPT");
			anx_free(buf);
			return ANX_CONTENT_FOREIGN;
		}
		if (!all_zero(buf, 512))
			saw_data = true;
	}

	/* ---- sector 2: ext2/3/4 superblock magic at byte 0x438 ---- */
	if (result == ANX_CONTENT_BLANK && capacity > SECT_EXT) {
		if (anx_blk_dev_read(dev, SECT_EXT, 1, buf) == ANX_OK) {
			if (buf[0x38] == 0x53 && buf[0x39] == 0xEF) {
				say(desc, desc_len, "ext2/3/4");
				anx_free(buf);
				return ANX_CONTENT_FOREIGN;
			}
			if (!all_zero(buf, 512))
				saw_data = true;
		}
	}

	/* ---- sector 8: Linux RAID 1.x superblock at byte 0x1000 ---- */
	if (result == ANX_CONTENT_BLANK && capacity > SECT_MDRAID) {
		if (anx_blk_dev_read(dev, SECT_MDRAID, 1, buf) == ANX_OK) {
			anx_memcpy(&magic, buf, 4);
			if (magic == 0xA92B4EFCu) {
				say(desc, desc_len, "Linux RAID member");
				anx_free(buf);
				return ANX_CONTENT_FOREIGN;
			}
		}
	}

	/* ---- sector 128: btrfs at byte 0x10040 ---- */
	if (result == ANX_CONTENT_BLANK && capacity > SECT_BTRFS) {
		if (anx_blk_dev_read(dev, SECT_BTRFS, 1, buf) == ANX_OK) {
			if (match(buf, 0x40, "_BHRfS_M", 8)) {
				say(desc, desc_len, "btrfs");
				anx_free(buf);
				return ANX_CONTENT_FOREIGN;
			}
		}
	}

	anx_free(buf);

	if (result == ANX_CONTENT_FOREIGN)
		return result;

	/*
	 * Nothing matched. A device whose probed sectors are all zero is the
	 * blank disk a fresh install starts from. One carrying bytes we did
	 * not recognise is not: an unknown format is still someone's data.
	 */
	if (saw_data) {
		say(desc, desc_len, "unrecognised data");
		return ANX_CONTENT_FOREIGN;
	}

	say(desc, desc_len, "blank");
	return ANX_CONTENT_BLANK;
}
