/*
 * cells.c — List Execution Cells.
 *
 * Anunix equivalent of `ps`. Shows cell CID, intent name, lifecycle
 * status, type, attempt count, child count, and output count.
 *
 * USAGE
 *   cells              List all cells
 */

#include <anx/types.h>
#include <anx/tools.h>
#include <anx/cell.h>
#include <anx/kprintf.h>
#include <anx/string.h>
#include <anx/uuid.h>

static const char *status_str(enum anx_cell_status s)
{
	switch (s) {
	case ANX_CELL_CREATED:		return "created";
	case ANX_CELL_ADMITTED:		return "admitted";
	case ANX_CELL_PLANNING:		return "planning";
	case ANX_CELL_PLANNED:		return "planned";
	case ANX_CELL_QUEUED:		return "queued";
	case ANX_CELL_RUNNING:		return "running";
	case ANX_CELL_WAITING:		return "waiting";
	case ANX_CELL_VALIDATING:	return "validating";
	case ANX_CELL_COMMITTING:	return "committing";
	case ANX_CELL_COMPLETED:	return "completed";
	case ANX_CELL_FAILED:		return "failed";
	case ANX_CELL_CANCELLED:	return "cancelled";
	case ANX_CELL_COMPENSATING:	return "compensating";
	case ANX_CELL_COMPENSATED:	return "compensated";
	default:			return "?";
	}
}

struct cells_ctx {
	uint32_t count;
};

static int cells_callback(struct anx_cell *cell, void *arg)
{
	struct cells_ctx *ctx = (struct cells_ctx *)arg;
	char cid_str[37];

	anx_uuid_to_string(&cell->cid, cid_str, sizeof(cid_str));
	kprintf("  %s  %s  %s\n", cid_str, cell->intent.name,
		status_str(cell->status));
	ctx->count++;
	return 0;
}

void cmd_cells(int argc, char **argv)
{
	struct cells_ctx ctx;

	(void)argc;
	(void)argv;

	ctx.count = 0;
	kprintf("Execution Cells:\n");
	anx_cell_store_iterate(cells_callback, &ctx);

	if (ctx.count == 0)
		kprintf("  (no cells)\n");
	else
		kprintf("\n%u cells\n", ctx.count);
}
