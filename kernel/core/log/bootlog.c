/*
 * bootlog.c — Boot-session logging subsystem.
 *
 * Two phases:
 *   1. anx_bootlog_early_init() — installs a hook into the kprintf output
 *      path so every character written by kprintf feeds the ring buffer.
 *   2. anx_bootlog_disk_init()  — called after the disk store is mounted;
 *      persists the captured pre-disk log as a State Object, prunes
 *      sessions that exceed the retention period, and updates the index.
 *
 * The ring buffer is a plain static array (always contiguous in memory).
 * When it wraps, oldest data is silently overwritten — same behaviour as
 * the Linux printk ring buffer.
 */

#include <anx/bootlog.h>
#include <anx/kprintf.h>
#include <anx/objstore_disk.h>
#include <anx/state_object.h>
#include <anx/alloc.h>
#include <anx/string.h>
#include <anx/uuid.h>
#include <anx/net.h>
#include <anx/types.h>

/* ── ring buffer ─────────────────────────────────────────────────────────── */

static char     g_ring[ANX_BOOTLOG_RING_SIZE];
static uint32_t g_head;   /* next write position (wraps mod RING_SIZE) */
static uint32_t g_total;  /* total chars appended (saturates at UINT32_MAX) */
static bool     g_ready;  /* set by early_init */

static void ring_putc(char c)
{
	g_ring[g_head] = c;
	g_head = (g_head + 1u < ANX_BOOTLOG_RING_SIZE)
	        ? g_head + 1u : 0u;
	if (g_total < 0xFFFFFFFFu)
		g_total++;
}

void anx_bootlog_putc(char c)
{
	if (g_ready)
		ring_putc(c);
}

uint32_t anx_bootlog_current_bytes(void)
{
	return (g_total < ANX_BOOTLOG_RING_SIZE)
	       ? g_total : ANX_BOOTLOG_RING_SIZE;
}

uint32_t anx_bootlog_read_current(uint32_t offset, char *buf, uint32_t len)
{
	uint32_t avail = anx_bootlog_current_bytes();
	/* oldest byte lives at g_head when ring is full, else at 0 */
	uint32_t start = (g_total < ANX_BOOTLOG_RING_SIZE) ? 0u : g_head;
	uint32_t i;

	if (offset >= avail || !len || !buf)
		return 0u;
	if (offset + len > avail)
		len = avail - offset;
	for (i = 0; i < len; i++)
		buf[i] = g_ring[(start + offset + i) % ANX_BOOTLOG_RING_SIZE];
	return len;
}

/* ── early init ─────────────────────────────────────────────────────────── */

void anx_bootlog_early_init(void)
{
	g_head  = 0;
	g_total = 0;
	g_ready = true;
	anx_kprintf_set_ring_hook(anx_bootlog_putc);
}

/* ── session state (populated by disk_init) ─────────────────────────────── */

static bool      g_disk_ready;
static anx_oid_t g_session_oid;
static uint32_t  g_boot_seq;
static uint64_t  g_boot_time;

/* Index and config are large; keep them in BSS rather than the stack. */
static struct anx_bootlog_index g_idx;
static struct anx_bootlog_config g_cfg;

/* ── helpers ─────────────────────────────────────────────────────────────── */

static void build_hdr(struct anx_boot_session_hdr *h, uint32_t log_bytes)
{
	h->magic     = ANX_BOOTLOG_HDR_MAGIC;
	h->version   = ANX_BOOTLOG_VERSION;
	h->boot_seq  = g_boot_seq;
	h->_pad      = 0;
	h->boot_time = g_boot_time;
	h->log_bytes = log_bytes;
	anx_strlcpy(h->kver, ANX_VERSION, sizeof(h->kver));
}

/* Write or rewrite the session object with the current ring contents. */
static int persist_session(void)
{
	uint32_t log_bytes = anx_bootlog_current_bytes();
	uint32_t pay_size  = (uint32_t)sizeof(struct anx_boot_session_hdr)
	                     + log_bytes;
	struct anx_boot_session_hdr hdr;
	void *pay;
	int ret;

	build_hdr(&hdr, log_bytes);

	pay = anx_alloc(pay_size);
	if (pay) {
		anx_memcpy(pay, &hdr, sizeof(hdr));
		anx_bootlog_read_current(0,
		    (char *)pay + sizeof(hdr), log_bytes);
		ret = anx_disk_write_obj_bk(&g_session_oid,
		          ANX_OBJ_BYTE_DATA, g_boot_time,
		          pay, pay_size);
		anx_free(pay);
	} else {
		/* OOM: write header only, no log text */
		hdr.log_bytes = 0;
		ret = anx_disk_write_obj_bk(&g_session_oid,
		          ANX_OBJ_BYTE_DATA, g_boot_time,
		          &hdr, (uint32_t)sizeof(hdr));
	}
	return ret;
}

/* ── disk init ───────────────────────────────────────────────────────────── */

int anx_bootlog_disk_init(void)
{
	static const anx_oid_t cfg_oid = {
		ANX_BOOTLOG_CFG_OID_HI, ANX_BOOTLOG_CFG_OID_LO
	};
	static const anx_oid_t idx_oid = {
		ANX_BOOTLOG_IDX_OID_HI, ANX_BOOTLOG_IDX_OID_LO
	};
	uint32_t actual, obj_type;
	int ret;

	/* 1. Load or create config ─────────────────────────────────────── */
	ret = anx_disk_read_obj(&cfg_oid, &g_cfg, sizeof(g_cfg),
	                        &actual, &obj_type);
	if (ret != ANX_OK || g_cfg.magic != ANX_BOOTLOG_CFG_MAGIC) {
		g_cfg.magic          = ANX_BOOTLOG_CFG_MAGIC;
		g_cfg.retention_days = ANX_BOOTLOG_DEFAULT_RETENTION;
		g_cfg.next_boot_seq  = 1;
		g_cfg._pad           = 0;
	}

	/* 2. Claim this boot's sequence number ─────────────────────────── */
	g_boot_seq  = g_cfg.next_boot_seq++;
	g_boot_time = (uint64_t)anx_ntp_unix_time();  /* 0 if pre-NTP */

	/* 3. Generate session OID ──────────────────────────────────────── */
	anx_uuid_generate(&g_session_oid);

	/* 4. Load or create index ──────────────────────────────────────── */
	anx_memset(&g_idx, 0, sizeof(g_idx));
	ret = anx_disk_read_obj(&idx_oid, &g_idx, sizeof(g_idx),
	                        &actual, &obj_type);
	if (ret != ANX_OK || g_idx.magic != ANX_BOOTLOG_IDX_MAGIC) {
		g_idx.magic = ANX_BOOTLOG_IDX_MAGIC;
		g_idx.count = 0;
	}

	/* 5. Prune expired sessions ─────────────────────────────────────
	 * Only prune when boot_time is valid (> 0).
	 * An entry with boot_time == 0 is never pruned (unknown age).    */
	if (g_boot_time > 0 && g_cfg.retention_days > 0) {
		uint64_t cutoff = g_boot_time
		    - (uint64_t)g_cfg.retention_days * 86400ULL;
		uint32_t out = 0;
		uint32_t i;

		for (i = 0; i < g_idx.count; i++) {
			struct anx_bootlog_idx_entry *e = &g_idx.entries[i];
			if (e->boot_time > 0 && e->boot_time < cutoff) {
				anx_oid_t prune_oid = e->oid;
				anx_disk_delete_obj(&prune_oid);
				kprintf("bootlog: pruned session %u\n",
				        e->boot_seq);
			} else {
				g_idx.entries[out++] = *e;
			}
		}
		g_idx.count = out;
	}

	/* 6. Write initial session object with the pre-disk log ─────────*/
	persist_session();

	/* 7. Append this session to the index ──────────────────────────── */
	if (g_idx.count < ANX_BOOTLOG_MAX_SESSIONS) {
		struct anx_bootlog_idx_entry *e =
		    &g_idx.entries[g_idx.count++];
		e->oid       = g_session_oid;
		e->boot_time = g_boot_time;
		e->boot_seq  = g_boot_seq;
		e->log_bytes = anx_bootlog_current_bytes();
	}

	/* 8. Flush config and index to disk ────────────────────────────── */
	anx_disk_write_obj(&cfg_oid, ANX_OBJ_BYTE_DATA,
	                   &g_cfg, sizeof(g_cfg));
	anx_disk_write_obj(&idx_oid, ANX_OBJ_BYTE_DATA,
	                   &g_idx, sizeof(g_idx));

	g_disk_ready = true;

	kprintf("bootlog: session %u started (log %u bytes on disk)\n",
	        g_boot_seq, anx_bootlog_current_bytes());
	return ANX_OK;
}

/* ── shutdown ────────────────────────────────────────────────────────────── */

void anx_bootlog_shutdown(void)
{
	static const anx_oid_t idx_oid = {
		ANX_BOOTLOG_IDX_OID_HI, ANX_BOOTLOG_IDX_OID_LO
	};
	uint32_t i;

	if (!g_disk_ready)
		return;

	/* Refresh boot_time if NTP synced after disk_init */
	if (g_boot_time == 0)
		g_boot_time = (uint64_t)anx_ntp_unix_time();

	/* Rewrite session with the full log accumulated during this boot */
	persist_session();

	/* Update the index entry with the final log byte count */
	for (i = 0; i < g_idx.count; i++) {
		struct anx_bootlog_idx_entry *e = &g_idx.entries[i];
		if (e->oid.hi == g_session_oid.hi &&
		    e->oid.lo == g_session_oid.lo) {
			e->log_bytes = anx_bootlog_current_bytes();
			e->boot_time = g_boot_time;
			break;
		}
	}

	{
		static const anx_oid_t idx_w = {
			ANX_BOOTLOG_IDX_OID_HI, ANX_BOOTLOG_IDX_OID_LO
		};
		(void)idx_oid;
		anx_disk_write_obj(&idx_w, ANX_OBJ_BYTE_DATA,
		                   &g_idx, sizeof(g_idx));
	}
}
