/*
 * cells.c — List Execution Cells.
 *
 * Anunix equivalent of `ps`. Shows cell CID, intent name, lifecycle
 * status, type, attempt count, child count, and output count.
 *
 * USAGE
 *   cells              List active cells
 *   cells -a           List all cells including completed/failed
 *   cells <cid-prefix> Show details for a specific cell
 */

#include <anx/types.h>
#include <anx/tools.h>
#include <anx/cell.h>
#include <anx/kprintf.h>
#include <anx/string.h>
#include <anx/uuid.h>


void cmd_cells(int argc, char **argv)
{
	(void)argc;
	(void)argv;

	/* The cell store doesn't have an iteration API yet.
	 * For now, report that it exists but needs the iteration
	 * primitive (like we added for objstore).
	 */
	kprintf("cells: cell store iteration not yet implemented\n");
	kprintf("  Use 'cell show' for known cell CIDs\n");
	kprintf("  Use 'cell create <name>' to create cells\n");
}
