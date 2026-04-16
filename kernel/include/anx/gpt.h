/*
 * anx/gpt.h — GUID Partition Table (GPT) support.
 *
 * Read, write, and create GPT partition tables on block devices.
 * Used by the installer to partition disks for Anunix installation.
 */

#ifndef ANX_GPT_H
#define ANX_GPT_H

#include <anx/types.h>

#define ANX_GPT_MAX_PARTS	16

/* Well-known partition type GUIDs (as two uint64s: lo, hi) */
#define ANX_GPT_TYPE_EFI_LO	0x11D2F81FC12A7328ULL
#define ANX_GPT_TYPE_EFI_HI	0x3BC93EC9A0004BBAULL

#define ANX_GPT_TYPE_ANX_LO	0x414E5849444F4F53ULL	/* "ANXIDOOS" */
#define ANX_GPT_TYPE_ANX_HI	0x544F524553594E41ULL	/* "TORESYNA" */

struct anx_gpt_partition {
	uint64_t type_lo, type_hi;	/* partition type GUID */
	uint64_t guid_lo, guid_hi;	/* unique partition GUID */
	uint64_t start_lba;
	uint64_t end_lba;		/* inclusive */
	uint64_t attributes;
	char name[72];			/* UTF-16LE in GPT, ASCII here */
};

struct anx_gpt_table {
	uint32_t partition_count;
	uint64_t disk_guid_lo, disk_guid_hi;
	struct anx_gpt_partition partitions[ANX_GPT_MAX_PARTS];
};

/* Read the GPT from the block device */
int anx_gpt_read(struct anx_gpt_table *table);

/* Write a GPT to the block device (protective MBR + primary + backup) */
int anx_gpt_write(const struct anx_gpt_table *table);

/* Create a standard Anunix partition layout (EFI + data) */
int anx_gpt_create_default(const char *label);

/* Find a partition by type GUID */
const struct anx_gpt_partition *anx_gpt_find_type(
	const struct anx_gpt_table *table,
	uint64_t type_lo, uint64_t type_hi);

#endif /* ANX_GPT_H */
