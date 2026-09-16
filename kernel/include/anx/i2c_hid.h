/*
 * anx/i2c_hid.h — HID over I2C protocol encoding.
 *
 * Pure translation for the HID-over-I2C protocol (Microsoft, version 1.0):
 * the HID descriptor, command bytes, and input report framing. The bus
 * driver moves bytes; this layer gives them meaning, and the host test
 * build covers it.
 */

#ifndef ANX_I2C_HID_H
#define ANX_I2C_HID_H

#include <anx/types.h>

#define ANX_I2C_HID_DESC_LEN		30
#define ANX_I2C_HID_VERSION		0x0100

#define ANX_I2C_HID_OP_RESET		0x01
#define ANX_I2C_HID_OP_SET_POWER	0x08
#define ANX_I2C_HID_PWR_ON		0x00
#define ANX_I2C_HID_PWR_SLEEP		0x01

/* Longest command anx_i2c_hid_cmd() writes. */
#define ANX_I2C_HID_CMD_MAX		5

struct anx_i2c_hid_desc {
	uint16_t desc_len;
	uint16_t bcd_version;
	uint16_t report_desc_len;
	uint16_t report_desc_reg;
	uint16_t input_reg;
	uint16_t max_input_len;
	uint16_t output_reg;
	uint16_t max_output_len;
	uint16_t command_reg;
	uint16_t data_reg;
	uint16_t vendor_id;
	uint16_t product_id;
	uint16_t version_id;
};

/* Decode and validate a HID descriptor. Returns ANX_OK or ANX_EINVAL. */
int anx_i2c_hid_parse_desc(const uint8_t *buf, uint32_t len,
			   struct anx_i2c_hid_desc *out);

/*
 * Encode a command for the command register: the register address, then
 * report type and ID, then the opcode. Report IDs of 15 and above take an
 * extra byte. Returns the length written, at most ANX_I2C_HID_CMD_MAX.
 */
uint32_t anx_i2c_hid_cmd(uint8_t *buf, uint16_t command_reg, uint8_t opcode,
			 uint8_t report_type, uint8_t report_id);

/*
 * Frame an input read. The first two bytes give the length including
 * themselves. A length of 0 means nothing is pending, or a reset finished;
 * *payload_len is then 0. Returns ANX_OK or ANX_EINVAL.
 */
int anx_i2c_hid_input(const uint8_t *buf, uint32_t len,
		      const uint8_t **payload, uint32_t *payload_len);

/*
 * True when a report descriptor opens a mouse collection with a report ID:
 * Usage Page (Generic Desktop), Usage (Mouse), Collection (Application),
 * Report ID. Its reports then carry the boot mouse layout after the ID.
 */
bool anx_i2c_hid_mouse_report_id(const uint8_t *desc, uint32_t len,
				 uint8_t *report_id);

#endif /* ANX_I2C_HID_H */
