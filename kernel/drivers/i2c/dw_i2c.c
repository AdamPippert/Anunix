/*
 * dw_i2c.c — Synopsys DesignWare I2C master, polled.
 *
 * Register offsets and bits follow Linux
 * drivers/i2c/busses/i2c-designware-core.h. SCL timing uses the fast-mode
 * values from i2c-designware-master.c: tHIGH 600 ns and tLOW 1300 ns, each
 * with a 300 ns fall-time allowance. The driver keeps the FIFOs within
 * their depth instead of using interrupts.
 */

#include <anx/types.h>
#include <anx/dw_i2c.h>
#include <anx/mmio.h>
#include <anx/string.h>
#include <anx/delay.h>
#include <anx/perf.h>

#define DW_IC_CON		0x00
#define DW_IC_TAR		0x04
#define DW_IC_DATA_CMD		0x10
#define DW_IC_FS_SCL_HCNT	0x1C
#define DW_IC_FS_SCL_LCNT	0x20
#define DW_IC_INTR_MASK		0x30
#define DW_IC_RAW_INTR_STAT	0x34
#define DW_IC_RX_TL		0x38
#define DW_IC_TX_TL		0x3C
#define DW_IC_CLR_INTR		0x40
#define DW_IC_CLR_TX_ABRT	0x54
#define DW_IC_CLR_STOP_DET	0x60
#define DW_IC_ENABLE		0x6C
#define DW_IC_TXFLR		0x74
#define DW_IC_RXFLR		0x78
#define DW_IC_TX_ABRT_SOURCE	0x80
#define DW_IC_ENABLE_STATUS	0x9C
#define DW_IC_COMP_PARAM_1	0xF4
#define DW_IC_COMP_TYPE		0xFC

#define DW_IC_COMP_TYPE_VALUE	0x44570140u

#define DW_IC_CON_MASTER	(1u << 0)
#define DW_IC_CON_SPEED_FAST	(2u << 1)
#define DW_IC_CON_RESTART_EN	(1u << 5)
#define DW_IC_CON_SLAVE_DISABLE	(1u << 6)

#define DW_IC_DATA_CMD_READ	(1u << 8)
#define DW_IC_DATA_CMD_STOP	(1u << 9)
#define DW_IC_DATA_CMD_RESTART	(1u << 10)

#define DW_IC_INTR_TX_ABRT	(1u << 6)
#define DW_IC_INTR_STOP_DET	(1u << 9)

#define FS_THIGH_NS		600u
#define FS_TLOW_NS		1300u
#define FALL_NS			300u

#define ENABLE_TIMEOUT_US	100000u
#define XFER_BASE_US		50000u
#define XFER_PER_BYTE_US	2000u

static uint32_t rd(struct anx_dw_i2c *bus, uint32_t off)
{
	return *(volatile uint32_t *)(bus->base + off);
}

static void wr(struct anx_dw_i2c *bus, uint32_t off, uint32_t val)
{
	*(volatile uint32_t *)(bus->base + off) = val;
}

/* The controller applies IC_ENABLE only between transfers; wait for it. */
static int set_enable(struct anx_dw_i2c *bus, bool on)
{
	uint64_t start = anx_rdtsc();
	uint64_t budget = anx_tsc_budget_us(ENABLE_TIMEOUT_US);
	uint32_t want = on ? 1u : 0u;

	wr(bus, DW_IC_ENABLE, want);
	while ((rd(bus, DW_IC_ENABLE_STATUS) & 1u) != want) {
		if (anx_rdtsc() - start > budget)
			return ANX_ETIMEDOUT;
		anx_delay_us(100);
	}
	return ANX_OK;
}

int anx_dw_i2c_init(struct anx_dw_i2c *bus, uint64_t phys, uint32_t clk_hz)
{
	uint32_t khz = clk_hz / 1000u;
	uint32_t param;

	if (!bus || khz == 0)
		return ANX_EINVAL;

	anx_memset(bus, 0, sizeof(*bus));
	bus->base = anx_mmio_map(phys, 0x1000);
	if (!bus->base)
		return ANX_ENOMEM;
	if (rd(bus, DW_IC_COMP_TYPE) != DW_IC_COMP_TYPE_VALUE)
		return ANX_ENODEV;
	if (set_enable(bus, false) != ANX_OK)
		return ANX_EIO;

	wr(bus, DW_IC_CON, DW_IC_CON_MASTER | DW_IC_CON_SPEED_FAST |
			   DW_IC_CON_RESTART_EN | DW_IC_CON_SLAVE_DISABLE);
	/* Counts are clock periods: kHz x ns / 10^6, less the fixed
	 * delays the controller adds to each phase. */
	wr(bus, DW_IC_FS_SCL_HCNT,
	   (khz * (FS_THIGH_NS + FALL_NS) + 500000u) / 1000000u - 3u);
	wr(bus, DW_IC_FS_SCL_LCNT,
	   (khz * (FS_TLOW_NS + FALL_NS) + 500000u) / 1000000u - 1u);
	wr(bus, DW_IC_INTR_MASK, 0);
	wr(bus, DW_IC_RX_TL, 0);
	wr(bus, DW_IC_TX_TL, 0);
	(void)rd(bus, DW_IC_CLR_INTR);

	param = rd(bus, DW_IC_COMP_PARAM_1);
	bus->tx_depth = ((param >> 16) & 0xFFu) + 1u;
	bus->rx_depth = ((param >> 8) & 0xFFu) + 1u;
	return ANX_OK;
}

static bool aborted(struct anx_dw_i2c *bus)
{
	if (!(rd(bus, DW_IC_RAW_INTR_STAT) & DW_IC_INTR_TX_ABRT))
		return false;
	bus->last_abort = rd(bus, DW_IC_TX_ABRT_SOURCE);
	(void)rd(bus, DW_IC_CLR_TX_ABRT);
	return true;
}

int anx_dw_i2c_xfer(struct anx_dw_i2c *bus, uint16_t addr,
		    const uint8_t *wbuf, uint32_t wlen,
		    uint8_t *rbuf, uint32_t rlen)
{
	uint32_t sent = 0, issued = 0, got = 0;
	uint64_t start, budget;
	int ret = ANX_OK;

	if (!bus || !bus->base || (wlen && !wbuf) || (rlen && !rbuf) ||
	    wlen + rlen == 0)
		return ANX_EINVAL;

	if (set_enable(bus, false) != ANX_OK)
		return ANX_EIO;
	wr(bus, DW_IC_TAR, addr & 0x3FFu);
	(void)rd(bus, DW_IC_CLR_INTR);
	if (set_enable(bus, true) != ANX_OK)
		return ANX_EIO;

	start = anx_rdtsc();
	budget = anx_tsc_budget_us(XFER_BASE_US +
				   (uint64_t)XFER_PER_BYTE_US * (wlen + rlen));

	while (sent < wlen || got < rlen) {
		if (aborted(bus)) {
			ret = ANX_EIO;
			break;
		}

		while (sent < wlen && rd(bus, DW_IC_TXFLR) < bus->tx_depth) {
			uint32_t cmd = wbuf[sent];

			if (sent + 1 == wlen && rlen == 0)
				cmd |= DW_IC_DATA_CMD_STOP;
			wr(bus, DW_IC_DATA_CMD, cmd);
			sent++;
		}

		if (sent == wlen) {
			/* Issue no more reads than the RX FIFO can hold. */
			while (issued < rlen && issued - got < bus->rx_depth &&
			       rd(bus, DW_IC_TXFLR) < bus->tx_depth) {
				uint32_t cmd = DW_IC_DATA_CMD_READ;

				if (issued == 0 && wlen)
					cmd |= DW_IC_DATA_CMD_RESTART;
				if (issued + 1 == rlen)
					cmd |= DW_IC_DATA_CMD_STOP;
				wr(bus, DW_IC_DATA_CMD, cmd);
				issued++;
			}
			while (got < rlen && rd(bus, DW_IC_RXFLR) > 0)
				rbuf[got++] = (uint8_t)rd(bus, DW_IC_DATA_CMD);
		}

		if (sent == wlen && got == rlen)
			break;
		if (anx_rdtsc() - start > budget) {
			ret = ANX_ETIMEDOUT;
			break;
		}
		anx_delay_us(20);
	}

	if (ret == ANX_OK) {
		while (!(rd(bus, DW_IC_RAW_INTR_STAT) & DW_IC_INTR_STOP_DET)) {
			if (aborted(bus)) {
				ret = ANX_EIO;
				break;
			}
			if (anx_rdtsc() - start > budget) {
				ret = ANX_ETIMEDOUT;
				break;
			}
			anx_delay_us(20);
		}
	}
	(void)rd(bus, DW_IC_CLR_STOP_DET);
	(void)set_enable(bus, false);
	return ret;
}
