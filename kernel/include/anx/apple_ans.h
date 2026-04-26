/*
 * anx/apple_ans.h — Apple ANS2/ANS3 NVMe controller (M1/M2/M3 internal storage).
 *
 * MMIO base discovered via device tree. Protocol: Apple mailbox + NVMe.
 */

#ifndef ANX_APPLE_ANS_H
#define ANX_APPLE_ANS_H

#include <anx/types.h>

/* Apple ANS2/ANS3 NVMe controller (M1/M2/M3 internal storage).
 * MMIO base discovered via device tree. Protocol: Apple mailbox + NVMe. */
int anx_apple_ans_init(void);

#endif /* ANX_APPLE_ANS_H */
