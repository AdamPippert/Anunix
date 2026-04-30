/*
 * apple_ans.c — Apple ANS2/ANS3 NVMe controller driver (STUB).
 *
 * The ANS controller is Apple's custom NVMe implementation found in all
 * Apple Silicon Macs. It is NOT a standard PCIe NVMe device. Hardware
 * access requires:
 *   1. Device tree parsing to get the MMIO base address
 *   2. Apple Mailbox (ASC) protocol for controller bring-up
 *   3. Apple-specific NVMe admin commands for namespace enumeration
 *
 * MMIO base addresses (hardcoded fallback, from Asahi Linux DTS):
 *   M1/M2 Mac Studio:  0x277000000  (t8103/t8112)
 *   M1/M2 MacBook Pro: 0x277000000
 *
 * References:
 *   - Asahi Linux drivers/nvme/host/apple.c
 *   - https://github.com/AsahiLinux/linux/blob/asahi/drivers/nvme/host/apple.c
 */

#include <anx/types.h>
#include <anx/kprintf.h>
#include <anx/apple_ans.h>

int anx_apple_ans_init(void)
{
	kprintf("apple-ans: controller found (driver not yet implemented)\n");
	return ANX_ENOSYS;
}
