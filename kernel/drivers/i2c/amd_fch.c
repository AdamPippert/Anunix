/*
 * amd_fch.c — AMD Fusion Controller Hub helpers for I2C input devices.
 *
 * Firmware may leave an FCH I2C block power-gated after boot; the ACPI
 * _PS0 method that turns it on never runs without an AML interpreter.
 * This file performs the same AOAC sequence directly, and reads the GPIO
 * level a touchpad uses to signal a pending report.
 *
 * Sources for the register layout:
 *   - AOAC: coreboot soc/amd/common/block/include/amdblocks/aoac.h and
 *     acpimmio_map.h; device numbers from soc/amd/phoenix aoac_defs.h.
 *   - The power-on sequence: method DSAD in the Framework Laptop 16 DSDT.
 *   - GPIO: Linux drivers/pinctrl/pinctrl-amd.h and pinctrl-amd.c.
 */

#include <anx/types.h>
#include <anx/amd_fch.h>
#include <anx/mmio.h>
#include <anx/kprintf.h>
#include <anx/delay.h>
#include <anx/perf.h>

#define FCH_ACPI_MMIO		0xFED80000u
#define FCH_AOAC_BANK		0x1E00u
#define AOAC_D3_CTL(dev)	(0x40u + (dev) * 2u)
#define AOAC_D3_STATE(dev)	(AOAC_D3_CTL(dev) + 1u)
#define AOAC_TARGET_STATE	0x03u	/* 0 = D0 */
#define AOAC_PWR_ON_DEV		(1u << 3)
#define AOAC_STATE_ON		0x07u	/* power reset, reference clock, reset released */
#define AOAC_TIMEOUT_US		100000u

/* FCH I2C0..I2C3 sit at consecutive 4 KiB blocks, AOAC devices 5..8. */
#define FCH_I2C0_BASE		0xFEDC2000u
#define FCH_I2C_SPACING		0x1000u
#define FCH_I2C_COUNT		4u
#define AOAC_DEV_I2C0		5u

#define GPIO_PIN_STS		(1u << 16)

int anx_amd_i2c_power_on(uint32_t i2c_base)
{
	volatile uint8_t *aoac;
	uint32_t idx, dev;
	uint64_t start, budget;
	uint8_t ctl;

	if (i2c_base < FCH_I2C0_BASE ||
	    (i2c_base - FCH_I2C0_BASE) % FCH_I2C_SPACING != 0)
		return ANX_ENOTSUP;
	idx = (i2c_base - FCH_I2C0_BASE) / FCH_I2C_SPACING;
	if (idx >= FCH_I2C_COUNT)
		return ANX_ENOTSUP;
	dev = AOAC_DEV_I2C0 + idx;

	aoac = anx_mmio_map(FCH_ACPI_MMIO + FCH_AOAC_BANK, 0x100);
	if (!aoac)
		return ANX_ENOMEM;

	ctl = aoac[AOAC_D3_CTL(dev)];
	if ((ctl & AOAC_TARGET_STATE) == 0 &&
	    (aoac[AOAC_D3_STATE(dev)] & AOAC_STATE_ON) == AOAC_STATE_ON)
		return ANX_OK;

	aoac[AOAC_D3_CTL(dev)] = (uint8_t)((ctl & ~AOAC_TARGET_STATE) |
					   AOAC_PWR_ON_DEV);

	start = anx_rdtsc();
	budget = anx_tsc_budget_us(AOAC_TIMEOUT_US);
	while ((aoac[AOAC_D3_STATE(dev)] & AOAC_STATE_ON) != AOAC_STATE_ON) {
		if (anx_rdtsc() - start > budget) {
			kprintf("amd-fch: I2C%u did not power on\n", idx);
			return ANX_ETIMEDOUT;
		}
		anx_delay_us(100);
	}
	kprintf("amd-fch: powered on I2C%u\n", idx);
	return ANX_OK;
}

volatile uint8_t *anx_amd_gpio_map(uint32_t gpio_base)
{
	return anx_mmio_map(gpio_base, 0x1000);
}

bool anx_amd_gpio_level(volatile uint8_t *gpio, uint16_t pin)
{
	volatile uint32_t *reg = (volatile uint32_t *)(gpio + (uint32_t)pin * 4u);

	return (*reg & GPIO_PIN_STS) != 0;
}
