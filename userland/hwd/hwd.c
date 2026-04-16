#include <anx/cell.h>
#include <anx/errno.h>
#include "../lib/libanx/libanx.h"

/*
 * hwd — hardware discovery agent
 *
 * Probes hardware via deterministic tool cells (PCI, ACPI, device tree),
 * synthesizes a structured hw-profile/v1 State Object via a model cell,
 * generates driver stubs for unknown device classes, and posts results
 * to superrouter via anx-fetch.
 *
 * See RFC-0011 for full specification.
 *
 * Subcommands:
 *   hwd                   full discovery (boot mode)
 *   hwd rescan            re-probe and diff
 *   hwd show [--json]     print current profile
 *   hwd stubs list        list generated stubs
 *   hwd stubs show <oid>  dump stub source
 *   hwd push [--dry-run]  push profile to superrouter
 *   hwd status            cell lifecycle state of last run
 *   hwd trace <cid>       full execution trace
 */

/* TODO: implement — see RFC-0011 §3 */
int
main(int argc, char *argv[])
{
        (void)argc;
        (void)argv;
        return ANX_ENOTIMPL;
}
