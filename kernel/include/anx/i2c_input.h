/*
 * anx/i2c_input.h — HID-over-I2C input devices found through ACPI.
 *
 * Laptop touchpads have no PCI function; ACPI alone describes them. The
 * driver binds mouse-compatible HID-over-I2C devices on AMD FCH I2C
 * controllers and polls their reports. The kernel main loop calls
 * anx_i2c_input_poll().
 */

#ifndef ANX_I2C_INPUT_H
#define ANX_I2C_INPUT_H

#include <anx/types.h>

/* Find and start devices. Safe to call more than once. */
int anx_i2c_input_init(void);

/* Read pending reports and deliver pointer events. */
void anx_i2c_input_poll(void);

#endif /* ANX_I2C_INPUT_H */
