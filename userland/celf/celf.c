#include <anx/cell.h>
#include <anx/errno.h>
#include <anx/kprintf.h>
#include <anx/string.h>
#include "../lib/libanx/libanx.h"

/*
 * celf — cell inspector
 *
 * Replaces ps/strace/lsof. Shows live cell tree with intent, plan,
 * engine assignment, and full execution trace.
 *
 * See RFC-0011 §4.1 for full specification.
 *
 * Usage:
 *   celf              list all cells
 *   celf <cid>        show details for one cell
 *   celf tree         display hierarchical cell tree
 */

/* ── Iterator state ─────────────────────────────────────────────────── */

struct celf_state {
	int        count;
	int        tree_mode;
	anx_cid_t  filter_cid;
	int        filter_set;
};

/* ── Single-cell display ────────────────────────────────────────────── */

static void print_cell_line(const struct anx_cell *c, int indent)
{
	char cid_buf[64];
	int i;

	for (i = 0; i < indent * 2; i++)
		kprintf(" ");

	anx_fmt_oid(cid_buf, sizeof(cid_buf), (const anx_oid_t *)&c->cid);
	kprintf("%-20s  %-16s  %s\n",
		cid_buf,
		anx_fmt_cell_status(c->status),
		c->intent.name[0] ? c->intent.name : "(unnamed)");
}

static void print_cell_detail(const struct anx_cell *c)
{
	char buf[64];
	uint32_t i;

	anx_fmt_oid(buf, sizeof(buf), (const anx_oid_t *)&c->cid);
	kprintf("CID        : %s\n", buf);
	kprintf("Type       : %s\n", anx_fmt_cell_status(c->status));
	kprintf("Status     : %s\n", anx_fmt_cell_status(c->status));
	kprintf("Name       : %s\n", c->intent.name[0] ? c->intent.name : "(unnamed)");
	kprintf("Objective  : %s\n", c->intent.objective[0] ? c->intent.objective : "(none)");
	kprintf("Priority   : %u\n", c->intent.priority);
	kprintf("Depth      : %u\n", c->recursion_depth);
	kprintf("Children   : %u\n", c->child_count);
	kprintf("Outputs    : %u\n", c->output_count);

	for (i = 0; i < c->output_count; i++) {
		anx_fmt_oid(buf, sizeof(buf), &c->output_refs[i]);
		kprintf("  out[%u]   : %s\n", i, buf);
	}
}

/* ── Iterator callbacks ─────────────────────────────────────────────── */

static int cb_list(struct anx_cell *cell, void *arg)
{
	struct celf_state *st = (struct celf_state *)arg;
	st->count++;
	print_cell_line(cell, 0);
	return 0;
}

static int cb_tree(struct anx_cell *cell, void *arg)
{
	struct celf_state *st = (struct celf_state *)arg;
	st->count++;
	print_cell_line(cell, (int)cell->recursion_depth);
	return 0;
}

static int cb_find(struct anx_cell *cell, void *arg)
{
	struct celf_state *st = (struct celf_state *)arg;
	if (cell->cid.hi == st->filter_cid.hi &&
	    cell->cid.lo == st->filter_cid.lo) {
		print_cell_detail(cell);
		st->count++;
	}
	return 0;
}

/* ── main ───────────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
	struct celf_state st;

	anx_memset(&st, 0, sizeof(st));

	if (argc >= 2 && anx_strcmp(argv[1], "tree") == 0) {
		kprintf("%-20s  %-16s  %s\n", "CID", "STATUS", "NAME");
		kprintf("%-20s  %-16s  %s\n",
			"--------------------",
			"----------------",
			"----");
		st.tree_mode = 1;
		anx_cell_store_iterate(cb_tree, &st);
		kprintf("\n%d cell(s)\n", st.count);
		return ANX_OK;
	}

	if (argc >= 2) {
		/* Treat argument as hex CID lo word for filtering */
		st.filter_cid.lo = anx_strtoull(argv[1], NULL, 16);
		st.filter_set = 1;
		anx_cell_store_iterate(cb_find, &st);
		if (!st.count)
			kprintf("celf: no cell found matching '%s'\n", argv[1]);
		return ANX_OK;
	}

	/* Default: list all cells */
	kprintf("%-20s  %-16s  %s\n", "CID", "STATUS", "NAME");
	kprintf("%-20s  %-16s  %s\n",
		"--------------------",
		"----------------",
		"----");
	anx_cell_store_iterate(cb_list, &st);
	kprintf("\n%d cell(s)\n", st.count);
	return ANX_OK;
}
