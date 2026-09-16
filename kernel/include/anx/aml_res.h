/*
 * anx/aml_res.h — Device resources from ACPI AML, without an interpreter.
 *
 * Firmware usually declares I2C and GPIO resources as constant buffers,
 * so their bytes appear verbatim in the DSDT. These helpers find them by
 * structure. They cannot see resources that AML computes at run time, and
 * they do not evaluate _STA, so a caller must probe the hardware before
 * trusting a result.
 */

#ifndef ANX_AML_RES_H
#define ANX_AML_RES_H

#include <anx/types.h>

/* One Device () package: [start, end) within the AML byte stream. */
struct anx_aml_device {
	uint32_t start;
	uint32_t end;
	char     name[5];
};

/* An I2cSerialBus resource. */
struct anx_aml_i2c {
	uint16_t addr;
	uint32_t speed_hz;
	char     bus[5];	/* last name segment of the controller path */
};

/* A GpioInt resource. */
struct anx_aml_gpio_int {
	uint16_t pin;
	bool     level;		/* level-triggered (false: edge) */
	bool     active_low;
	char     ctrl[5];	/* last name segment of the GPIO controller path */
};

/*
 * Find the next Device () package that starts at or after *pos. Nested
 * devices are returned too. On success *pos moves past the device opcode.
 */
bool anx_aml_next_device(const uint8_t *aml, uint32_t len, uint32_t *pos,
			 struct anx_aml_device *dev);

/* True when the device declares id as its _HID or _CID string. */
bool anx_aml_device_has_id(const uint8_t *aml, const struct anx_aml_device *dev,
			   const char *id);

/* Find a device by 4-character name and, when id is not NULL, by ID. */
bool anx_aml_find_device(const uint8_t *aml, uint32_t len, const char *name,
			 const char *id, struct anx_aml_device *dev);

/* First Memory32Fixed resource in the device. */
bool anx_aml_mem32(const uint8_t *aml, const struct anx_aml_device *dev,
		   uint32_t *base, uint32_t *size);

/* First I2cSerialBus resource in the device. */
bool anx_aml_i2c(const uint8_t *aml, const struct anx_aml_device *dev,
		 struct anx_aml_i2c *out);

/* First GpioInt resource in the device. */
bool anx_aml_gpio_int(const uint8_t *aml, const struct anx_aml_device *dev,
		      struct anx_aml_gpio_int *out);

/*
 * HID descriptor register of a HID-over-I2C device: the constant its
 * _DSM returns for function 1 of the HID-over-I2C UUID.
 */
bool anx_aml_hid_i2c_desc_reg(const uint8_t *aml,
			      const struct anx_aml_device *dev, uint16_t *reg);

#endif /* ANX_AML_RES_H */
