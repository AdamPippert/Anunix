/*
 * mt7925_regmap.c — Chip-address to BAR offset translation.
 *
 * The MT7925's register space is far larger than its 2 MiB BAR, so the chip
 * exposes it through a window. Most blocks have a fixed slot in the BAR; the
 * rest are reached by programming a remap register and then reading through a
 * moving window. A driver that does not implement this can only reach the
 * blocks that happen to have fixed slots, and will silently read the wrong
 * block for everything else.
 *
 * That is what happened here. mt7925_reg.h defined MT_CONN_HW_VER as BAR
 * offset 0xd000 and called it a chip-version register. Offset 0xd000 falls
 * inside the fixed slot for WF_UMAC_TOP (PSE) -- a Wi-Fi MAC block, powered
 * down on a cold chip -- so the "chip id" read returned the 0xdeadbeef
 * power-off sentinel every time. The chip was being asked the wrong question,
 * and the answer was taken as evidence that it was asleep.
 *
 * The real chip identifier lives at chip address 0x70010200, which has no
 * fixed slot and is unreachable without the remap path below.
 *
 * Translation follows __mt7925_reg_addr() in Linux v6.12 mt76,
 * drivers/net/wireless/mediatek/mt76/mt7925/pci.c, and the fixed map is
 * copied from the table in that function.
 */

#include <anx/types.h>
#include <anx/mt7925.h>
#include <anx/kprintf.h>

struct reg_map {
	uint32_t phys;		/* chip address */
	uint32_t maps;		/* BAR offset */
	uint32_t size;
};

static const struct reg_map fixed_map[] = {
	{ 0x830c0000, 0x000000, 0x0001000 }, /* WF_MCU_BUS_CR_REMAP */
	{ 0x54000000, 0x002000, 0x0001000 }, /* WFDMA PCIE0 MCU DMA0 */
	{ 0x55000000, 0x003000, 0x0001000 }, /* WFDMA PCIE0 MCU DMA1 */
	{ 0x56000000, 0x004000, 0x0001000 }, /* WFDMA reserved */
	{ 0x57000000, 0x005000, 0x0001000 }, /* WFDMA MCU wrap CR */
	{ 0x58000000, 0x006000, 0x0001000 }, /* WFDMA PCIE1 MCU DMA0 */
	{ 0x59000000, 0x007000, 0x0001000 }, /* WFDMA PCIE1 MCU DMA1 */
	{ 0x820c0000, 0x008000, 0x0004000 }, /* WF_UMAC_TOP (PLE) */
	{ 0x820c8000, 0x00c000, 0x0002000 }, /* WF_UMAC_TOP (PSE) */
	{ 0x820cc000, 0x00e000, 0x0002000 }, /* WF_UMAC_TOP (PP) */
	{ 0x74030000, 0x010000, 0x0001000 }, /* PCIe MAC */
	{ 0x820e0000, 0x020000, 0x0000400 }, /* WF_LMAC_TOP BN0 (WF_CFG) */
	{ 0x820e1000, 0x020400, 0x0000200 }, /* WF_LMAC_TOP BN0 (WF_TRB) */
	{ 0x820e2000, 0x020800, 0x0000400 }, /* WF_LMAC_TOP BN0 (WF_AGG) */
	{ 0x820e3000, 0x020c00, 0x0000400 }, /* WF_LMAC_TOP BN0 (WF_ARB) */
	{ 0x820e4000, 0x021000, 0x0000400 }, /* WF_LMAC_TOP BN0 (WF_TMAC) */
	{ 0x820e5000, 0x021400, 0x0000800 }, /* WF_LMAC_TOP BN0 (WF_RMAC) */
	{ 0x820ce000, 0x021c00, 0x0000200 }, /* WF_LMAC_TOP (WF_SEC) */
	{ 0x820e7000, 0x021e00, 0x0000200 }, /* WF_LMAC_TOP BN0 (WF_DMA) */
	{ 0x820cf000, 0x022000, 0x0001000 }, /* WF_LMAC_TOP (WF_PF) */
	{ 0x820e9000, 0x023400, 0x0000200 }, /* WF_LMAC_TOP BN0 (WF_WTBLOFF) */
	{ 0x820ea000, 0x024000, 0x0000200 }, /* WF_LMAC_TOP BN0 (WF_ETBF) */
	{ 0x820eb000, 0x024200, 0x0000400 }, /* WF_LMAC_TOP BN0 (WF_LPON) */
	{ 0x820ec000, 0x024600, 0x0000200 }, /* WF_LMAC_TOP BN0 (WF_INT) */
	{ 0x820ed000, 0x024800, 0x0000800 }, /* WF_LMAC_TOP BN0 (WF_MIB) */
	{ 0x820ca000, 0x026000, 0x0002000 }, /* WF_LMAC_TOP BN0 (WF_MUCOP) */
	{ 0x820d0000, 0x030000, 0x0010000 }, /* WF_LMAC_TOP (WF_WTBLON) */
	{ 0x40000000, 0x070000, 0x0010000 }, /* WF_UMAC_SYSRAM */
	{ 0x00400000, 0x080000, 0x0010000 }, /* WF_MCU_SYSRAM */
	{ 0x00410000, 0x090000, 0x0010000 }, /* WF_MCU_SYSRAM (config register) */
	{ 0x820f0000, 0x0a0000, 0x0000400 }, /* WF_LMAC_TOP BN1 (WF_CFG) */
	{ 0x820f1000, 0x0a0600, 0x0000200 }, /* WF_LMAC_TOP BN1 (WF_TRB) */
	{ 0x820f2000, 0x0a0800, 0x0000400 }, /* WF_LMAC_TOP BN1 (WF_AGG) */
	{ 0x820f3000, 0x0a0c00, 0x0000400 }, /* WF_LMAC_TOP BN1 (WF_ARB) */
	{ 0x820f4000, 0x0a1000, 0x0000400 }, /* WF_LMAC_TOP BN1 (WF_TMAC) */
	{ 0x820f5000, 0x0a1400, 0x0000800 }, /* WF_LMAC_TOP BN1 (WF_RMAC) */
	{ 0x820f7000, 0x0a1e00, 0x0000200 }, /* WF_LMAC_TOP BN1 (WF_DMA) */
	{ 0x820f9000, 0x0a3400, 0x0000200 }, /* WF_LMAC_TOP BN1 (WF_WTBLOFF) */
	{ 0x820fa000, 0x0a4000, 0x0000200 }, /* WF_LMAC_TOP BN1 (WF_ETBF) */
	{ 0x820fb000, 0x0a4200, 0x0000400 }, /* WF_LMAC_TOP BN1 (WF_LPON) */
	{ 0x820fc000, 0x0a4600, 0x0000200 }, /* WF_LMAC_TOP BN1 (WF_INT) */
	{ 0x820fd000, 0x0a4800, 0x0000800 }, /* WF_LMAC_TOP BN1 (WF_MIB) */
	{ 0x820c4000, 0x0a8000, 0x0004000 }, /* WF_LMAC_TOP BN1 (WF_MUCOP) */
	{ 0x820b0000, 0x0ae000, 0x0001000 }, /* [APB2] WFSYS_ON */
	{ 0x80020000, 0x0b0000, 0x0010000 }, /* WF_TOP_MISC_OFF */
	{ 0x81020000, 0x0c0000, 0x0010000 }, /* WF_TOP_MISC_ON */
	{ 0x7c020000, 0x0d0000, 0x0010000 }, /* CONN_INFRA, wfdma */
	{ 0x7c060000, 0x0e0000, 0x0010000 }, /* CONN_INFRA, conn_host_csr_top */
	{ 0x7c000000, 0x0f0000, 0x0010000 }, /* CONN_INFRA */
	{ 0x70020000, 0x1f0000, 0x0010000 }, /* Reserved for CBTOP */
	{ 0x7c500000, 0x060000, 0x2000000 }, /* remap */
};

#define FIXED_MAP_COUNT	(sizeof(fixed_map) / sizeof(fixed_map[0]))

/* Saved remap-window contents, restored before the next translation. */
static uint32_t g_backup_l1;
static uint32_t g_backup_l2;

static void remap_restore(void)
{
	if (g_backup_l1) {
		anx_mt7925_bar_wr(MT_HIF_REMAP_L1, g_backup_l1);
		g_backup_l1 = 0;
	}
	if (g_backup_l2) {
		anx_mt7925_bar_wr(MT_HIF_REMAP_L2, g_backup_l2);
		g_backup_l2 = 0;
	}
}

/* Point the L1 window at addr's 64 KiB block and return the BAR offset. */
static uint32_t reg_map_l1(uint32_t addr)
{
	uint32_t offset = addr & 0xFFFFu;
	uint32_t base   = addr >> 16;
	uint32_t v;

	g_backup_l1 = anx_mt7925_bar_rd(MT_HIF_REMAP_L1);

	v = (g_backup_l1 & ~MT_HIF_REMAP_L1_MASK) |
	    ((base << 16) & MT_HIF_REMAP_L1_MASK);
	anx_mt7925_bar_wr(MT_HIF_REMAP_L1, v);

	/* A read pushes the write out before the window is used. */
	(void)anx_mt7925_bar_rd(MT_HIF_REMAP_L1);

	return MT_HIF_REMAP_BASE_L1 + offset;
}

/* The L2 window takes a full address and is reached through the L1 window. */
static uint32_t reg_map_l2(uint32_t addr)
{
	uint32_t base = MT_HIF_REMAP_BASE_L2 >> 16;
	uint32_t v;

	g_backup_l2 = anx_mt7925_bar_rd(MT_HIF_REMAP_L1);

	v = (g_backup_l2 & ~MT_HIF_REMAP_L1_MASK) |
	    ((base << 16) & MT_HIF_REMAP_L1_MASK);
	anx_mt7925_bar_wr(MT_HIF_REMAP_L1, v);

	anx_mt7925_bar_wr(MT_HIF_REMAP_L2, addr);
	(void)anx_mt7925_bar_rd(MT_HIF_REMAP_L1);

	return MT_HIF_REMAP_BASE_L1;
}

uint32_t anx_mt7925_reg_addr(uint32_t addr)
{
	uint32_t i;

	/* Below the BAR size the chip address is the BAR offset. */
	if (addr < 0x200000u)
		return addr;

	remap_restore();

	for (i = 0; i < FIXED_MAP_COUNT; i++) {
		uint32_t ofs;

		if (addr < fixed_map[i].phys)
			continue;
		ofs = addr - fixed_map[i].phys;
		if (ofs > fixed_map[i].size)
			continue;
		return fixed_map[i].maps + ofs;
	}

	if ((addr >= 0x18000000u && addr < 0x18c00000u) ||
	    (addr >= 0x70000000u && addr < 0x78000000u) ||
	    (addr >= 0x7c000000u && addr < 0x7c400000u))
		return reg_map_l1(addr);

	return reg_map_l2(addr);
}

uint32_t anx_mt7925_rr(uint32_t chip_addr)
{
	return anx_mt7925_bar_rd(anx_mt7925_reg_addr(chip_addr));
}

void anx_mt7925_wr(uint32_t chip_addr, uint32_t val)
{
	anx_mt7925_bar_wr(anx_mt7925_reg_addr(chip_addr), val);
}
