/*
 * anx/objstore_disk.h — Persistent on-disk object store.
 *
 * Journaled storage for State Objects on a block device. Objects are
 * addressed by OID, stored with metadata, and indexed for fast lookup.
 * The write-ahead journal ensures crash recovery.
 */

#ifndef ANX_OBJSTORE_DISK_H
#define ANX_OBJSTORE_DISK_H

#include <anx/types.h>

/* On-disk superblock magic */
#define ANX_DISK_MAGIC		0x414E5844	/* "ANXD" */
#define ANX_DISK_VERSION	1

/* Sector layout constants */
#define ANX_SUPER_SECTOR	0
#define ANX_JOURNAL_START	1
#define ANX_JOURNAL_SECTORS	256	/* 128 KiB journal */
#define ANX_INDEX_START		(ANX_JOURNAL_START + ANX_JOURNAL_SECTORS)
#define ANX_INDEX_SECTORS	256	/* 128 KiB index */
#define ANX_DATA_START		(ANX_INDEX_START + ANX_INDEX_SECTORS)

/* On-disk superblock (fits in one 512-byte sector) */
struct anx_disk_super {
	uint32_t magic;
	uint32_t version;
	uint64_t total_sectors;		/* disk capacity */
	uint64_t obj_count;		/* number of stored objects */
	uint64_t next_data_sector;	/* next free sector in data region */
	uint32_t journal_head;		/* next journal entry slot */
	uint32_t journal_committed;	/* last committed journal entry */
	uint8_t  uuid[16];		/* filesystem UUID */
	char     label[32];		/* human-readable label */
	uint8_t  _pad[512 - 80];	/* pad to 512 bytes */
} __attribute__((packed));

/* Journal entry types */
enum anx_journal_op {
	ANX_JRNL_NOP = 0,
	ANX_JRNL_WRITE_OBJ,		/* object written to data region */
	ANX_JRNL_DELETE_OBJ,		/* object removed */
	ANX_JRNL_COMMIT,		/* transaction commit marker */
};

/* Journal entry (64 bytes — 8 per sector) */
struct anx_journal_entry {
	uint32_t txn_id;
	uint32_t op;			/* anx_journal_op */
	anx_oid_t oid;			/* object ID (16 bytes) */
	uint64_t data_sector;		/* where object data starts */
	uint32_t data_sectors;		/* how many sectors */
	uint32_t obj_size;		/* payload size in bytes */
	uint8_t  _pad[8];		/* pad to 64 bytes */
} __attribute__((packed));

#define ANX_JRNL_ENTRIES_PER_SECTOR	(512 / 64)
#define ANX_JRNL_MAX_ENTRIES		(ANX_JOURNAL_SECTORS * ANX_JRNL_ENTRIES_PER_SECTOR)

/* Index entry (48 bytes — maps OID to disk location) */
struct anx_disk_index_entry {
	anx_oid_t oid;			/* 16 bytes */
	uint64_t data_sector;
	uint32_t data_sectors;
	uint32_t payload_size;
	uint32_t obj_type;		/* anx_object_type */
	uint32_t flags;			/* 0x1 = active, 0x2 = sealed */
};

#define ANX_INDEX_ENTRIES_PER_SECTOR	(512 / 48)
#define ANX_INDEX_MAX_ENTRIES		(ANX_INDEX_SECTORS * ANX_INDEX_ENTRIES_PER_SECTOR)

/* Initialize the disk object store (format or mount) */
int anx_disk_store_init(void);

/* Format a block device with an empty Anunix object store */
int anx_disk_format(const char *label);

/* Write a State Object to disk (journaled) */
int anx_disk_write_obj(const anx_oid_t *oid, uint32_t obj_type,
			const void *payload, uint32_t payload_size);

/* Read a State Object from disk */
int anx_disk_read_obj(const anx_oid_t *oid,
		       void *payload_buf, uint32_t buf_size,
		       uint32_t *actual_size, uint32_t *obj_type);

/* Delete a State Object from disk (journaled) */
int anx_disk_delete_obj(const anx_oid_t *oid);

/* Check if an object exists on disk */
bool anx_disk_obj_exists(const anx_oid_t *oid);

/* Get disk store statistics */
int anx_disk_stats(uint64_t *obj_count, uint64_t *used_sectors,
		    uint64_t *total_sectors);

/* Replay the journal to recover from crash */
int anx_disk_recover(void);

#endif /* ANX_OBJSTORE_DISK_H */
