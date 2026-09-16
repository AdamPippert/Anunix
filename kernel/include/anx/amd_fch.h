/*
 * anx/amd_fch.h — AMD Fusion Controller Hub helpers for I2C input devices.
 */

#ifndef ANX_AMD_FCH_H
#define ANX_AMD_FCH_H

#include <anx/types.h>

/*
 * Power on the FCH I2C block at i2c_base through AOAC power gating.
 * Returns ANX_OK when the block is on, ANX_ENOTSUP when the address is
 * not an FCH I2C block, or ANX_ETIMEDOUT.
 */
int anx_amd_i2c_power_on(uint32_t i2c_base);

/* Map the FCH GPIO bank whose registers start at gpio_base. */
volatile uint8_t *anx_amd_gpio_map(uint32_t gpio_base);

/* Input level of one pin in a mapped GPIO bank. */
bool anx_amd_gpio_level(volatile uint8_t *gpio, uint16_t pin);

#endif /* ANX_AMD_FCH_H */
