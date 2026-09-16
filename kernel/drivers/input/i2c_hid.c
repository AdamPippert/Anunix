/*
 * i2c_hid.c — HID over I2C protocol encoding.
 *
 * Descriptor layout, command encoding, and input framing follow the
 * HID-over-I2C specification as Linux implements it in
 * drivers/hid/i2c-hid/i2c-hid-core.c. No hardware access happens here.
 */

#include <anx/i2c_hid.h>

static uint16_t rd16(const uint8_t *p)
{
	return (uint16_t)(p[0] | (p[1] << 8));
}

int anx_i2c_hid_parse_desc(const uint8_t *buf, uint32_t len,
			   struct anx_i2c_hid_desc *out)
{
	if (!buf || !out || len < ANX_I2C_HID_DESC_LEN)
		return ANX_EINVAL;

	out->desc_len        = rd16(buf + 0);
	out->bcd_version     = rd16(buf + 2);
	out->report_desc_len = rd16(buf + 4);
	out->report_desc_reg = rd16(buf + 6);
	out->input_reg       = rd16(buf + 8);
	out->max_input_len   = rd16(buf + 10);
	out->output_reg      = rd16(buf + 12);
	out->max_output_len  = rd16(buf + 14);
	out->command_reg     = rd16(buf + 16);
	out->data_reg        = rd16(buf + 18);
	out->vendor_id       = rd16(buf + 20);
	out->product_id      = rd16(buf + 22);
	out->version_id      = rd16(buf + 24);

	if (out->desc_len != ANX_I2C_HID_DESC_LEN ||
	    out->bcd_version != ANX_I2C_HID_VERSION)
		return ANX_EINVAL;
	/* An input read carries at least its own two length bytes. */
	if (out->max_input_len < 2)
		return ANX_EINVAL;
	return ANX_OK;
}

uint32_t anx_i2c_hid_cmd(uint8_t *buf, uint16_t command_reg, uint8_t opcode,
			 uint8_t report_type, uint8_t report_id)
{
	uint32_t n = 0;

	buf[n++] = (uint8_t)command_reg;
	buf[n++] = (uint8_t)(command_reg >> 8);
	if (report_id < 0x0F) {
		buf[n++] = (uint8_t)((report_type << 4) | report_id);
		buf[n++] = opcode;
	} else {
		buf[n++] = (uint8_t)((report_type << 4) | 0x0F);
		buf[n++] = opcode;
		buf[n++] = report_id;
	}
	return n;
}

int anx_i2c_hid_input(const uint8_t *buf, uint32_t len,
		      const uint8_t **payload, uint32_t *payload_len)
{
	uint16_t n;

	if (!buf || !payload || !payload_len || len < 2)
		return ANX_EINVAL;

	n = rd16(buf);
	if (n == 0) {
		*payload = buf + 2;
		*payload_len = 0;
		return ANX_OK;
	}
	if (n < 2 || n > len)
		return ANX_EINVAL;
	*payload = buf + 2;
	*payload_len = n - 2u;
	return ANX_OK;
}

bool anx_i2c_hid_mouse_report_id(const uint8_t *desc, uint32_t len,
				 uint8_t *report_id)
{
	static const uint8_t prefix[] = {
		0x05, 0x01,	/* Usage Page (Generic Desktop) */
		0x09, 0x02,	/* Usage (Mouse) */
		0xA1, 0x01,	/* Collection (Application) */
		0x85,		/* Report ID (next byte) */
	};
	uint32_t i;

	if (!desc || !report_id || len < sizeof(prefix) + 1)
		return false;
	for (i = 0; i < sizeof(prefix); i++)
		if (desc[i] != prefix[i])
			return false;
	*report_id = desc[sizeof(prefix)];
	return *report_id != 0;
}
