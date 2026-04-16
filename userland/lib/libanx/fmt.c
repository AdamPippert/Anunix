#include "libanx.h"
#include <anx/cell.h>
#include <anx/state_object.h>
#include <anx/engine.h>
#include <anx/memplane.h>
#include <string.h>

void
anx_fmt_oid(char *buf, size_t len, const anx_oid_t *oid)
{
	/* Print first 8 hex chars of the 16-byte UUID */
	const unsigned char *b = (const unsigned char *)oid->bytes;
	snprintf(buf, len, "%02x%02x%02x%02x", b[0], b[1], b[2], b[3]);
}

void
anx_fmt_tier(char *buf, size_t len, uint8_t tier_mask)
{
	/* Find highest set tier bit */
	int tier = 0;
	for (int i = 5; i >= 0; i--) {
		if (tier_mask & (1 << i)) {
			tier = i;
			break;
		}
	}
	snprintf(buf, len, "L%d", tier);
}

const char *
anx_fmt_obj_type(enum anx_object_type t)
{
	switch (t) {
	case ANX_OBJ_BYTE_DATA:        return "byte";
	case ANX_OBJ_STRUCTURED_DATA:  return "struct";
	case ANX_OBJ_EMBEDDED_MODEL:   return "embed";
	case ANX_OBJ_GRAPH_DATA:       return "graph";
	case ANX_OBJ_EXECUTION_OUTPUT: return "output";
	case ANX_OBJ_EXECUTION_TRACE:  return "trace";
	case ANX_OBJ_CAPABILITY:       return "cap";
	case ANX_OBJ_CREDENTIAL:       return "cred";
	case ANX_OBJ_MEMORY:           return "mem";
	default:                       return "unknown";
	}
}

const char *
anx_fmt_cell_status(enum anx_cell_status s)
{
	switch (s) {
	case ANX_CELL_PENDING:    return "pending";
	case ANX_CELL_RUNNING:    return "running";
	case ANX_CELL_COMPLETED:  return "completed";
	case ANX_CELL_FAILED:     return "failed";
	case ANX_CELL_CANCELLED:  return "cancelled";
	case ANX_CELL_BLOCKED:    return "blocked";
	default:                  return "unknown";
	}
}

const char *
anx_fmt_engine_class(enum anx_engine_class c)
{
	switch (c) {
	case ANX_ENGINE_DETERMINISTIC_TOOL:      return "tool";
	case ANX_ENGINE_LOCAL_MODEL:             return "local-model";
	case ANX_ENGINE_REMOTE_MODEL:            return "remote-model";
	case ANX_ENGINE_HYBRID:                  return "hybrid";
	case ANX_ENGINE_EXECUTION_SERVICE:       return "exec-svc";
	case ANX_ENGINE_INSTALLED_CAPABILITY:    return "installed-cap";
	default:                                 return "unknown";
	}
}

const char *
anx_fmt_mem_validation(enum anx_mem_validation_state v)
{
	switch (v) {
	case ANX_MEMVAL_UNVALIDATED:   return "unval";
	case ANX_MEMVAL_PROVISIONAL:   return "prov";
	case ANX_MEMVAL_VALIDATED:     return "valid";
	case ANX_MEMVAL_CONTESTED:     return "contest";
	case ANX_MEMVAL_SUPERSEDED:    return "super";
	default:                       return "unknown";
	}
}
