/*
 * bootlog.c — Shell command: bootlog
 *
 * Subcommands:
 *   bootlog list                  — list all persisted boot sessions
 *   bootlog show [N]              — dump log for session N (default: current)
 *   bootlog diff <N1> <N2>        — line-diff two sessions
 *   bootlog config retention <N>  — set retention period in days
 *   bootlog config show           — show current config
 */

#include <anx/bootlog.h>
#include <anx/kprintf.h>
#include <anx/objstore_disk.h>
#include <anx/state_object.h>
#include <anx/alloc.h>
#include <anx/string.h>
#include <anx/types.h>

/* ── helpers ─────────────────────────────────────────────────────────────── */

static const anx_oid_t g_idx_oid = {
	ANX_BOOTLOG_IDX_OID_HI, ANX_BOOTLOG_IDX_OID_LO
};
static const anx_oid_t g_cfg_oid = {
	ANX_BOOTLOG_CFG_OID_HI, ANX_BOOTLOG_CFG_OID_LO
};

static bool load_index(struct anx_bootlog_index *idx)
{
	uint32_t actual, obj_type;
	int ret = anx_disk_read_obj(&g_idx_oid, idx, sizeof(*idx),
	                             &actual, &obj_type);
	return ret == ANX_OK && idx->magic == ANX_BOOTLOG_IDX_MAGIC;
}

static void fmt_time(uint64_t t, char *buf, uint32_t len)
{
	if (t == 0) {
		anx_strlcpy(buf, "pre-NTP", len);
		return;
	}
	/* Very simple ISO-like formatting using only integer arithmetic.
	 * Seconds since 1970-01-01 00:00:00 UTC.                         */
	uint64_t s  = t % 60;
	uint64_t m  = (t / 60) % 60;
	uint64_t h  = (t / 3600) % 24;
	uint64_t d  = t / 86400;        /* days since epoch */
	/* Approximate year/month (good enough for log display) */
	uint32_t y  = 1970u;
	uint32_t remaining = (uint32_t)d;
	while (remaining >= 365) {
		uint32_t days_in_year = ((y % 4 == 0 && (y % 100 != 0 || y % 400 == 0))
		                         ? 366u : 365u);
		if (remaining < days_in_year) break;
		remaining -= days_in_year;
		y++;
	}
	anx_snprintf(buf, len, "%04u day+%u %02llu:%02llu:%02llu",
	             y, remaining, (unsigned long long)h,
	             (unsigned long long)m, (unsigned long long)s);
}

/* ── subcommands ─────────────────────────────────────────────────────────── */

static void cmd_bootlog_list(void)
{
	static struct anx_bootlog_index idx;
	uint32_t i;

	if (!load_index(&idx)) {
		kprintf("bootlog: no session index on disk\n");
		return;
	}
	kprintf("bootlog: %u sessions on disk\n\n", idx.count);
	kprintf("  #   seq   boot-time                        log KB\n");
	kprintf("  --  ----  ------------------------------ --------\n");
	for (i = 0; i < idx.count; i++) {
		struct anx_bootlog_idx_entry *e = &idx.entries[i];
		char ts[40];
		fmt_time(e->boot_time, ts, sizeof(ts));
		kprintf("  %-2u  %-4u  %-30s %6u\n",
		        i + 1, e->boot_seq, ts, e->log_bytes / 1024u);
	}
}

static void cmd_bootlog_show(uint32_t slot)
{
	/* slot == 0  → current boot's ring buffer (not yet flushed to disk) */
	if (slot == 0) {
		uint32_t total = anx_bootlog_current_bytes();
		uint32_t off   = 0;
		char buf[256];
		uint32_t n;

		kprintf("--- current boot log (%u bytes) ---\n", total);
		while (off < total) {
			n = anx_bootlog_read_current(off, buf,
			        sizeof(buf) - 1u);
			if (!n) break;
			buf[n] = '\0';
			kprintf("%s", buf);
			off += n;
		}
		return;
	}

	/* slot >= 1 → look up in index (1-based) */
	static struct anx_bootlog_index idx;
	if (!load_index(&idx) || slot > idx.count) {
		kprintf("bootlog: session %u not found (have %u)\n",
		        slot, idx.count);
		return;
	}

	struct anx_bootlog_idx_entry *e = &idx.entries[slot - 1u];
	uint32_t pay_size = (uint32_t)sizeof(struct anx_boot_session_hdr)
	                    + e->log_bytes;
	char *pay = (char *)anx_alloc(pay_size);
	if (!pay) {
		kprintf("bootlog: OOM reading session %u\n", slot);
		return;
	}

	uint32_t actual, obj_type;
	anx_oid_t session_oid = e->oid;
	int ret = anx_disk_read_obj(&session_oid, pay, pay_size,
	                             &actual, &obj_type);
	if (ret != ANX_OK) {
		kprintf("bootlog: disk read failed (%d)\n", ret);
		anx_free(pay);
		return;
	}

	struct anx_boot_session_hdr *hdr = (struct anx_boot_session_hdr *)pay;
	if (hdr->magic != ANX_BOOTLOG_HDR_MAGIC) {
		kprintf("bootlog: corrupt session object\n");
		anx_free(pay);
		return;
	}

	char ts[40];
	fmt_time(hdr->boot_time, ts, sizeof(ts));
	kprintf("--- session %u  seq %u  %s  kver %s (%u bytes) ---\n",
	        slot, hdr->boot_seq, ts, hdr->kver, hdr->log_bytes);

	const char *log = pay + sizeof(struct anx_boot_session_hdr);
	uint32_t remaining = (actual > sizeof(struct anx_boot_session_hdr))
	    ? actual - (uint32_t)sizeof(struct anx_boot_session_hdr) : 0u;
	uint32_t off = 0;
	while (off < remaining) {
		uint32_t chunk = (remaining - off < 256u)
		    ? (remaining - off) : 256u;
		char tmp[257];
		anx_memcpy(tmp, log + off, chunk);
		tmp[chunk] = '\0';
		kprintf("%s", tmp);
		off += chunk;
	}
	anx_free(pay);
}

static void cmd_bootlog_config_show(void)
{
	struct anx_bootlog_config cfg;
	uint32_t actual, obj_type;
	int ret = anx_disk_read_obj(&g_cfg_oid, &cfg, sizeof(cfg),
	                             &actual, &obj_type);
	if (ret != ANX_OK || cfg.magic != ANX_BOOTLOG_CFG_MAGIC) {
		kprintf("bootlog: config not initialised yet\n");
		return;
	}
	kprintf("bootlog config:\n");
	kprintf("  retention  %u days\n", cfg.retention_days);
	kprintf("  next seq   %u\n", cfg.next_boot_seq);
}

static void cmd_bootlog_config_retention(uint32_t days)
{
	struct anx_bootlog_config cfg;
	uint32_t actual, obj_type;
	int ret = anx_disk_read_obj(&g_cfg_oid, &cfg, sizeof(cfg),
	                             &actual, &obj_type);
	if (ret != ANX_OK || cfg.magic != ANX_BOOTLOG_CFG_MAGIC) {
		cfg.magic          = ANX_BOOTLOG_CFG_MAGIC;
		cfg.retention_days = ANX_BOOTLOG_DEFAULT_RETENTION;
		cfg.next_boot_seq  = 1;
		cfg._pad           = 0;
	}
	cfg.retention_days = days;
	ret = anx_disk_write_obj(&g_cfg_oid, ANX_OBJ_BYTE_DATA,
	                          &cfg, sizeof(cfg));
	if (ret == ANX_OK)
		kprintf("bootlog: retention set to %u days\n", days);
	else
		kprintf("bootlog: write failed (%d)\n", ret);
}

/* Line-oriented diff: print lines present in A but not B (simple O(n²)).
 * Used for bootlog diff: only practical for small logs. */
static void line_diff(const char *a, uint32_t a_len,
                       const char *b, uint32_t b_len)
{
	uint32_t ai = 0;
	while (ai < a_len) {
		/* Find end of line in A */
		uint32_t ae = ai;
		while (ae < a_len && a[ae] != '\n') ae++;
		uint32_t line_len = ae - ai;

		/* Search for this line in B */
		bool found = false;
		uint32_t bi = 0;
		while (bi < b_len) {
			uint32_t be = bi;
			while (be < b_len && b[be] != '\n') be++;
			if ((be - bi) == line_len &&
			    anx_memcmp(a + ai, b + bi, line_len) == 0) {
				found = true;
				break;
			}
			bi = be + 1u;
		}
		if (!found) {
			char tmp[257];
			uint32_t n = (line_len < 256u) ? line_len : 256u;
			anx_memcpy(tmp, a + ai, n);
			tmp[n] = '\0';
			kprintf("< %s\n", tmp);
		}
		ai = ae + 1u;
	}
}

static void cmd_bootlog_diff(uint32_t slot1, uint32_t slot2)
{
	static struct anx_bootlog_index idx;
	if (!load_index(&idx)) {
		kprintf("bootlog: no index\n");
		return;
	}
	if (slot1 == 0 || slot2 == 0 ||
	    slot1 > idx.count || slot2 > idx.count) {
		kprintf("bootlog: diff requires two valid 1-based session numbers\n");
		return;
	}

	struct anx_bootlog_idx_entry *e1 = &idx.entries[slot1 - 1u];
	struct anx_bootlog_idx_entry *e2 = &idx.entries[slot2 - 1u];

	uint32_t pay1 = (uint32_t)sizeof(struct anx_boot_session_hdr) + e1->log_bytes;
	uint32_t pay2 = (uint32_t)sizeof(struct anx_boot_session_hdr) + e2->log_bytes;

	char *buf1 = (char *)anx_alloc(pay1);
	char *buf2 = (char *)anx_alloc(pay2);
	if (!buf1 || !buf2) {
		kprintf("bootlog: OOM\n");
		anx_free(buf1);
		anx_free(buf2);
		return;
	}

	uint32_t actual, obj_type;
	anx_oid_t oid1 = e1->oid, oid2 = e2->oid;
	anx_disk_read_obj(&oid1, buf1, pay1, &actual, &obj_type);
	anx_disk_read_obj(&oid2, buf2, pay2, &actual, &obj_type);

	const char *log1 = buf1 + sizeof(struct anx_boot_session_hdr);
	const char *log2 = buf2 + sizeof(struct anx_boot_session_hdr);

	kprintf("--- lines in session %u not in session %u ---\n", slot1, slot2);
	line_diff(log1, e1->log_bytes, log2, e2->log_bytes);
	kprintf("--- lines in session %u not in session %u ---\n", slot2, slot1);
	line_diff(log2, e2->log_bytes, log1, e1->log_bytes);

	anx_free(buf1);
	anx_free(buf2);
}

/* ── public entry point ──────────────────────────────────────────────────── */

void cmd_bootlog(int argc, char **argv)
{
	if (argc < 2 || anx_strcmp(argv[1], "list") == 0) {
		cmd_bootlog_list();
		return;
	}

	if (anx_strcmp(argv[1], "show") == 0) {
		uint32_t slot = 0;
		if (argc >= 3)
			slot = (uint32_t)anx_strtoul(argv[2], NULL, 10);
		cmd_bootlog_show(slot);
		return;
	}

	if (anx_strcmp(argv[1], "diff") == 0) {
		if (argc < 4) {
			kprintf("usage: bootlog diff <session1> <session2>\n");
			return;
		}
		uint32_t s1 = (uint32_t)anx_strtoul(argv[2], NULL, 10);
		uint32_t s2 = (uint32_t)anx_strtoul(argv[3], NULL, 10);
		cmd_bootlog_diff(s1, s2);
		return;
	}

	if (anx_strcmp(argv[1], "config") == 0) {
		if (argc >= 4 && anx_strcmp(argv[2], "retention") == 0) {
			uint32_t days = (uint32_t)anx_strtoul(argv[3], NULL, 10);
			if (days == 0) {
				kprintf("bootlog: retention must be >= 1 day\n");
				return;
			}
			cmd_bootlog_config_retention(days);
		} else {
			cmd_bootlog_config_show();
		}
		return;
	}

	kprintf("bootlog: unknown subcommand '%s'\n", argv[1]);
	kprintf("usage: bootlog [list|show [N]|diff N1 N2|config [retention N|show]]\n");
}
