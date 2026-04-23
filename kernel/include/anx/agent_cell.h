/*
 * anx/agent_cell.h — Agent cell dispatch interface.
 */

#ifndef ANX_AGENT_CELL_H
#define ANX_AGENT_CELL_H

#include <anx/types.h>

/*
 * Dispatch an AGENT_CALL workflow node.
 *
 * goal:           the goal string from the node spec
 * out_oids:       array to receive output OIDs from the dispatched sub-workflow
 * max_out_ports:  capacity of out_oids
 * out_count:      number of OIDs written on return
 *
 * Returns ANX_OK on success; ANX_ENOENT if no template or model path is
 * available for the goal.
 */
int anx_agent_cell_dispatch(const char *goal,
			    anx_oid_t *out_oids, uint32_t max_out_ports,
			    uint32_t *out_count);

#endif /* ANX_AGENT_CELL_H */
