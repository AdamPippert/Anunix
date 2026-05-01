/*
 * anx/bootlog.h — Boot-session logging subsystem.
 *
 * Every boot is assigned a session object that captures all kernel
 * output in a contiguous 256 KB in-memory ring buffer. When the disk
 * store is ready the session is persisted. A fixed-OID index object
 * tracks all sessions; sessions older than the retention period are
 * pruned on each boot.
 *
 * Boot comparison / history: read sessions from the index via the
 * shell `bootlog` command, or call anx_bootlog_read_session() directly.
 */

#ifndef ANX_BOOTLOG_H
#define ANX_BOOTLOG_H

#include <anx/types.h>

/* ── lifecycle ──────────────────────────────────────────────────────────── */

/* Call before any kprintf output — installs the ring-buffer hook. */
void anx_bootlog_early_init(void);

/* Call after anx_disk_store_init() succeeds — persists the session object,
 * prunes expired sessions, and updates the boot index. Returns ANX_OK. */
int  anx_bootlog_disk_init(void);

/* Call on orderly shutdown — rewrites the session with the final log. */
void anx_bootlog_shutdown(void);

/* ── current-session ring-buffer access ─────────────────────────────────── */

/* Total bytes of log captured since boot (capped at ring size). */
uint32_t anx_bootlog_current_bytes(void);

/* Copy up to `len` bytes of the current log starting at `offset` into `buf`.
 * Data is always presented in chronological order. Returns bytes copied. */
uint32_t anx_bootlog_read_current(uint32_t offset, char *buf, uint32_t len);

/* ── on-disk format ──────────────────────────────────────────────────────── */

#define ANX_BOOTLOG_HDR_MAGIC  0x424C4F47u   /* 'BLOG' */
#define ANX_BOOTLOG_VERSION    1u
#define ANX_BOOTLOG_RING_SIZE  (256u * 1024u)

/* Precedes the raw log text in each session object payload. */
struct anx_boot_session_hdr {
	uint32_t magic;         /* ANX_BOOTLOG_HDR_MAGIC                    */
	uint32_t version;       /* ANX_BOOTLOG_VERSION                       */
	uint32_t boot_seq;      /* monotonic 1-based boot counter            */
	uint32_t _pad;
	uint64_t boot_time;     /* unix timestamp at disk_init (0 = pre-NTP) */
	uint32_t log_bytes;     /* bytes of log text following this struct   */
	char     kver[24];      /* kernel version string                     */
} __attribute__((packed));

/* ── config object (fixed well-known OID) ───────────────────────────────── */

#define ANX_BOOTLOG_CFG_OID_HI  0x424C4F47434F4E46ULL  /* 'BLOGCONF' */
#define ANX_BOOTLOG_CFG_OID_LO  0x49475F42440001FFULL

#define ANX_BOOTLOG_CFG_MAGIC         0x424C4346u  /* 'BLCF' */
#define ANX_BOOTLOG_DEFAULT_RETENTION 365u          /* days   */

struct anx_bootlog_config {
	uint32_t magic;           /* ANX_BOOTLOG_CFG_MAGIC                  */
	uint32_t retention_days;  /* sessions older than this are pruned     */
	uint32_t next_boot_seq;   /* incremented on every boot               */
	uint32_t _pad;
} __attribute__((packed));

/* ── session index object (fixed well-known OID) ────────────────────────── */

#define ANX_BOOTLOG_IDX_OID_HI  0x424C4F47494E4458ULL  /* 'BLOGINDX' */
#define ANX_BOOTLOG_IDX_OID_LO  0x424F4F544C4F4700ULL  /* 'BOOTLOG\0' */

#define ANX_BOOTLOG_IDX_MAGIC    0x424C4958u  /* 'BLIX' */
#define ANX_BOOTLOG_MAX_SESSIONS 1024u

struct anx_bootlog_idx_entry {
	anx_oid_t oid;        /* session object OID                          */
	uint64_t  boot_time;  /* unix timestamp (0 = pre-NTP)                */
	uint32_t  boot_seq;   /* sequence number                             */
	uint32_t  log_bytes;  /* bytes of log stored in the session object   */
} __attribute__((packed));

/* Index payload stored in the index object. */
struct anx_bootlog_index {
	uint32_t magic;    /* ANX_BOOTLOG_IDX_MAGIC                          */
	uint32_t count;    /* number of valid entries                         */
	struct anx_bootlog_idx_entry entries[ANX_BOOTLOG_MAX_SESSIONS];
} __attribute__((packed));

#endif /* ANX_BOOTLOG_H */
