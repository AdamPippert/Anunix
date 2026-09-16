/*
 * anx/dw_i2c.h — Synopsys DesignWare I2C master, polled.
 */

#ifndef ANX_DW_I2C_H
#define ANX_DW_I2C_H

#include <anx/types.h>

struct anx_dw_i2c {
	volatile uint8_t *base;
	uint32_t tx_depth;
	uint32_t rx_depth;
	uint32_t last_abort;	/* IC_TX_ABRT_SOURCE of the last failure */
};

/*
 * Map and configure a controller for 400 kHz fast mode. clk_hz is the
 * controller's input clock. Returns ANX_ENODEV when the block does not
 * identify as DesignWare, for example because it is powered off.
 */
int anx_dw_i2c_init(struct anx_dw_i2c *bus, uint64_t phys, uint32_t clk_hz);

/*
 * Write wlen bytes, then read rlen bytes with a repeated start, as one
 * transaction ending in a stop. Either length may be zero.
 * Returns ANX_OK, ANX_EIO when the target does not acknowledge, or
 * ANX_ETIMEDOUT.
 */
int anx_dw_i2c_xfer(struct anx_dw_i2c *bus, uint16_t addr,
		    const uint8_t *wbuf, uint32_t wlen,
		    uint8_t *rbuf, uint32_t rlen);

#endif /* ANX_DW_I2C_H */
