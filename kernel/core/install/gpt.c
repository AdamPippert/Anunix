/*
 * gpt.c — GUID Partition Table creation and parsing.
 *
 * Creates GPT partition tables with a protective MBR for booting.
 * The default layout creates an EFI System Partition (100 MiB) and
 * an Anunix data partition (rest of disk).
 */

#include <anx/types.h>
#include <anx/gpt.h>
#include <anx/virtio_blk.h>
#include <anx/alloc.h>
#include <anx/string.h>
#include <anx/arch.h>
#include <anx/kprintf.h>

/* GPT header signature */
#define GPT_SIGNATURE		0x5452415020494645ULL	/* "EFI PART" */
#define GPT_REVISION		0x00010000		/* 1.0 */
#define GPT_HEADER_SIZE		92

/* Protective MBR partition type for GPT */
#define MBR_TYPE_GPT		0xEE

/* CRC32 for GPT checksums */
static uint32_t crc32_table[256];
static bool crc32_ready;

static void crc32_init(void)
{
	uint32_t i, j, c;

	for (i = 0; i < 256; i++) {
		c = i;
		for (j = 0; j < 8; j++) {
			if (c & 1)
				c = 0xEDB88320 ^ (c >> 1);
			else
				c >>= 1;
		}
		crc32_table[i] = c;
	}
	crc32_ready = true;
}

static uint32_t crc32(const void *data, uint32_t len)
{
	const uint8_t *p = (const uint8_t *)data;
	uint32_t crc = 0xFFFFFFFF;
	uint32_t i;

	if (!crc32_ready)
		crc32_init();

	for (i = 0; i < len; i++)
		crc = crc32_table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);

	return crc ^ 0xFFFFFFFF;
}

/* Generate a pseudo-random GUID from TSC */
static void make_guid(uint64_t *lo, uint64_t *hi)
{
	*lo = arch_time_now() ^ 0x4E58414E55584944ULL;
	*hi = arch_time_now() ^ 0x4F53544F52455359ULL;
}

/* Write the protective MBR */
static int write_protective_mbr(uint64_t disk_sectors)
{
	uint8_t *mbr;
	uint32_t size_lba;
	int ret;

	mbr = anx_zalloc(512);
	if (!mbr)
		return ANX_ENOMEM;

	/* MBR partition entry 1 at offset 446 */
	mbr[446 + 0] = 0x00;		/* not active */
	mbr[446 + 1] = 0x00;		/* CHS start (ignored for GPT) */
	mbr[446 + 2] = 0x02;
	mbr[446 + 3] = 0x00;
	mbr[446 + 4] = MBR_TYPE_GPT;	/* GPT protective */
	mbr[446 + 5] = 0xFF;		/* CHS end */
	mbr[446 + 6] = 0xFF;
	mbr[446 + 7] = 0xFF;

	/* LBA start = 1 */
	mbr[446 + 8] = 0x01;
	mbr[446 + 9] = 0x00;
	mbr[446 + 10] = 0x00;
	mbr[446 + 11] = 0x00;

	/* Size in LBA (capped at 0xFFFFFFFF) */
	size_lba = (disk_sectors > 0xFFFFFFFF)
		   ? 0xFFFFFFFF : (uint32_t)(disk_sectors - 1);
	mbr[446 + 12] = (uint8_t)(size_lba & 0xFF);
	mbr[446 + 13] = (uint8_t)((size_lba >> 8) & 0xFF);
	mbr[446 + 14] = (uint8_t)((size_lba >> 16) & 0xFF);
	mbr[446 + 15] = (uint8_t)((size_lba >> 24) & 0xFF);

	/* MBR signature */
	mbr[510] = 0x55;
	mbr[511] = 0xAA;

	ret = anx_blk_write(0, 1, mbr);
	anx_free(mbr);
	return ret;
}

/* Encode a partition entry (128 bytes) into buffer */
static void encode_part_entry(uint8_t *buf,
			       const struct anx_gpt_partition *part)
{
	anx_memset(buf, 0, 128);

	/* Type GUID (mixed endian per GPT spec) */
	anx_memcpy(buf + 0, &part->type_lo, 8);
	anx_memcpy(buf + 8, &part->type_hi, 8);

	/* Unique GUID */
	anx_memcpy(buf + 16, &part->guid_lo, 8);
	anx_memcpy(buf + 24, &part->guid_hi, 8);

	/* Start/end LBA (little-endian) */
	anx_memcpy(buf + 32, &part->start_lba, 8);
	anx_memcpy(buf + 40, &part->end_lba, 8);

	/* Attributes */
	anx_memcpy(buf + 48, &part->attributes, 8);

	/* Name (ASCII → simple UTF-16LE: just zero-extend each byte) */
	{
		int i;

		for (i = 0; i < 36 && part->name[i]; i++) {
			buf[56 + i * 2] = (uint8_t)part->name[i];
			buf[56 + i * 2 + 1] = 0;
		}
	}
}

int anx_gpt_create_default(const char *label)
{
	uint64_t disk_sectors;
	uint64_t efi_start, efi_end;
	uint64_t anx_start, anx_end;
	uint64_t last_usable;
	uint8_t *header;
	uint8_t *entries;
	uint32_t entries_size;
	uint32_t header_crc, entries_crc;
	struct anx_gpt_partition efi_part, anx_part;
	int ret;

	if (!anx_blk_ready())
		return ANX_EIO;

	disk_sectors = anx_blk_capacity();
	if (disk_sectors < 2048)
		return ANX_EINVAL;	/* too small */

	kprintf("gpt: creating partitions on %u MiB disk\n",
		(uint32_t)(disk_sectors * 512 / (1024 * 1024)));

	/* Layout:
	 *   LBA 0:     Protective MBR
	 *   LBA 1:     Primary GPT header
	 *   LBA 2-33:  Primary partition entries (32 sectors = 128 entries)
	 *   LBA 34:    EFI System Partition start
	 *   ...
	 *   LBA -33:   Backup partition entries
	 *   LBA -1:    Backup GPT header
	 */

	efi_start = 34;
	efi_end = efi_start + (100 * 1024 * 1024 / 512) - 1;  /* 100 MiB */
	anx_start = efi_end + 1;
	last_usable = disk_sectors - 34;
	anx_end = last_usable;

	if (anx_end <= anx_start)
		return ANX_EINVAL;

	/* Write protective MBR */
	ret = write_protective_mbr(disk_sectors);
	if (ret != ANX_OK)
		return ret;

	/* Build partition entries */
	anx_memset(&efi_part, 0, sizeof(efi_part));
	efi_part.type_lo = ANX_GPT_TYPE_EFI_LO;
	efi_part.type_hi = ANX_GPT_TYPE_EFI_HI;
	make_guid(&efi_part.guid_lo, &efi_part.guid_hi);
	efi_part.start_lba = efi_start;
	efi_part.end_lba = efi_end;
	anx_strlcpy(efi_part.name, "EFI System", sizeof(efi_part.name));

	anx_memset(&anx_part, 0, sizeof(anx_part));
	anx_part.type_lo = ANX_GPT_TYPE_ANX_LO;
	anx_part.type_hi = ANX_GPT_TYPE_ANX_HI;
	make_guid(&anx_part.guid_lo, &anx_part.guid_hi);
	anx_part.start_lba = anx_start;
	anx_part.end_lba = anx_end;
	if (label)
		anx_strlcpy(anx_part.name, label, sizeof(anx_part.name));
	else
		anx_strlcpy(anx_part.name, "Anunix", sizeof(anx_part.name));

	/* Encode partition entries (32 sectors = 128 entries x 128 bytes) */
	entries_size = 32 * 512;
	entries = anx_zalloc(entries_size);
	if (!entries)
		return ANX_ENOMEM;

	encode_part_entry(entries + 0, &efi_part);
	encode_part_entry(entries + 128, &anx_part);

	entries_crc = crc32(entries, entries_size);

	/* Write primary partition entries (LBA 2-33) */
	ret = anx_blk_write(2, 32, entries);
	if (ret != ANX_OK) {
		anx_free(entries);
		return ret;
	}

	/* Write backup partition entries */
	ret = anx_blk_write(disk_sectors - 33, 32, entries);
	anx_free(entries);
	if (ret != ANX_OK)
		return ret;

	/* Build primary GPT header (LBA 1) */
	header = anx_zalloc(512);
	if (!header)
		return ANX_ENOMEM;

	anx_memcpy(header + 0, &(uint64_t){GPT_SIGNATURE}, 8);
	anx_memcpy(header + 8, &(uint32_t){GPT_REVISION}, 4);
	anx_memcpy(header + 12, &(uint32_t){GPT_HEADER_SIZE}, 4);
	/* header CRC at offset 16 — fill after computing */
	anx_memcpy(header + 24, &(uint64_t){1}, 8);		/* my LBA */
	anx_memcpy(header + 32, &(uint64_t){disk_sectors - 1}, 8); /* backup LBA */
	anx_memcpy(header + 40, &(uint64_t){34}, 8);		/* first usable */
	anx_memcpy(header + 48, &(uint64_t){last_usable}, 8);	/* last usable */

	{
		uint64_t dg_lo, dg_hi;

		make_guid(&dg_lo, &dg_hi);
		anx_memcpy(header + 56, &dg_lo, 8);
		anx_memcpy(header + 64, &dg_hi, 8);
	}

	anx_memcpy(header + 72, &(uint64_t){2}, 8);		/* entries start LBA */
	anx_memcpy(header + 80, &(uint32_t){128}, 4);		/* number of entries */
	anx_memcpy(header + 84, &(uint32_t){128}, 4);		/* entry size */
	anx_memcpy(header + 88, &entries_crc, 4);		/* entries CRC */

	/* Compute header CRC (offset 16 must be 0 during computation) */
	header_crc = crc32(header, GPT_HEADER_SIZE);
	anx_memcpy(header + 16, &header_crc, 4);

	/* Write primary header (LBA 1) */
	ret = anx_blk_write(1, 1, header);
	if (ret != ANX_OK) {
		anx_free(header);
		return ret;
	}

	/* Build and write backup header (LBA disk_sectors-1) */
	/* Swap my_lba / backup_lba, update entries_start_lba */
	anx_memcpy(header + 16, &(uint32_t){0}, 4);		/* clear CRC */
	anx_memcpy(header + 24, &(uint64_t){disk_sectors - 1}, 8); /* my LBA */
	anx_memcpy(header + 32, &(uint64_t){1}, 8);		/* backup = primary */
	anx_memcpy(header + 72, &(uint64_t){disk_sectors - 33}, 8); /* entries */

	header_crc = crc32(header, GPT_HEADER_SIZE);
	anx_memcpy(header + 16, &header_crc, 4);

	ret = anx_blk_write(disk_sectors - 1, 1, header);
	anx_free(header);
	if (ret != ANX_OK)
		return ret;

	kprintf("gpt: EFI partition: %u MiB (LBA %u-%u)\n",
		(uint32_t)((efi_end - efi_start + 1) * 512 / (1024 * 1024)),
		(uint32_t)efi_start, (uint32_t)efi_end);
	kprintf("gpt: Anunix partition: %u MiB (LBA %u-%u)\n",
		(uint32_t)((anx_end - anx_start + 1) * 512 / (1024 * 1024)),
		(uint32_t)anx_start, (uint32_t)anx_end);

	return ANX_OK;
}

/* Simplified read — just find partitions for the installer */
int anx_gpt_read(struct anx_gpt_table *table)
{
	uint8_t *header_buf;
	uint8_t *entries_buf;
	uint64_t sig;
	uint32_t num_entries, entry_size;
	uint32_t i;
	int ret;

	if (!anx_blk_ready())
		return ANX_EIO;

	anx_memset(table, 0, sizeof(*table));

	header_buf = anx_alloc(512);
	if (!header_buf)
		return ANX_ENOMEM;

	ret = anx_blk_read(1, 1, header_buf);
	if (ret != ANX_OK) {
		anx_free(header_buf);
		return ret;
	}

	anx_memcpy(&sig, header_buf, 8);
	if (sig != GPT_SIGNATURE) {
		anx_free(header_buf);
		return ANX_EINVAL;
	}

	anx_memcpy(&table->disk_guid_lo, header_buf + 56, 8);
	anx_memcpy(&table->disk_guid_hi, header_buf + 64, 8);
	anx_memcpy(&num_entries, header_buf + 80, 4);
	anx_memcpy(&entry_size, header_buf + 84, 4);
	anx_free(header_buf);

	if (entry_size != 128 || num_entries == 0)
		return ANX_EINVAL;

	entries_buf = anx_alloc(32 * 512);
	if (!entries_buf)
		return ANX_ENOMEM;

	ret = anx_blk_read(2, 32, entries_buf);
	if (ret != ANX_OK) {
		anx_free(entries_buf);
		return ret;
	}

	for (i = 0; i < num_entries && table->partition_count < ANX_GPT_MAX_PARTS; i++) {
		uint8_t *e = entries_buf + i * 128;
		uint64_t type_lo;

		anx_memcpy(&type_lo, e, 8);
		if (type_lo == 0)
			continue;	/* empty entry */

		{
			struct anx_gpt_partition *p;
			int j;

			p = &table->partitions[table->partition_count];
			anx_memcpy(&p->type_lo, e + 0, 8);
			anx_memcpy(&p->type_hi, e + 8, 8);
			anx_memcpy(&p->guid_lo, e + 16, 8);
			anx_memcpy(&p->guid_hi, e + 24, 8);
			anx_memcpy(&p->start_lba, e + 32, 8);
			anx_memcpy(&p->end_lba, e + 40, 8);
			anx_memcpy(&p->attributes, e + 48, 8);

			/* Decode UTF-16LE name to ASCII */
			for (j = 0; j < 36; j++) {
				p->name[j] = (char)e[56 + j * 2];
				if (p->name[j] == '\0')
					break;
			}
			p->name[36] = '\0';
			table->partition_count++;
		}
	}

	anx_free(entries_buf);
	return ANX_OK;
}

int anx_gpt_write(const struct anx_gpt_table *table)
{
	/* For now, only creation via anx_gpt_create_default is supported */
	(void)table;
	return ANX_ENOSYS;
}

const struct anx_gpt_partition *anx_gpt_find_type(
	const struct anx_gpt_table *table,
	uint64_t type_lo, uint64_t type_hi)
{
	uint32_t i;

	for (i = 0; i < table->partition_count; i++) {
		if (table->partitions[i].type_lo == type_lo &&
		    table->partitions[i].type_hi == type_hi)
			return &table->partitions[i];
	}
	return NULL;
}
