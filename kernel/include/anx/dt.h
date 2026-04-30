/*
 * anx/dt.h — Device tree presence check.
 *
 * Minimal interface for querying the system device tree. Used by the driver
 * probe layer to locate platform devices (e.g., Apple ANS NVMe controller)
 * that are not exposed via PCIe configuration space.
 */

#ifndef ANX_DT_H
#define ANX_DT_H

#include <anx/types.h>

/* Returns true if the system device tree contains a node with the given
 * compatible string. Always false until arm64 DT parsing is implemented. */
bool anx_dt_has_compatible(const char *compatible);

#endif /* ANX_DT_H */
