/*
 * test_i2c_hid.c — HID-over-I2C protocol encoding.
 *
 * Commands and descriptor fields are little-endian byte layouts; a single
 * swapped byte makes a touchpad ignore the host silently. These tests pin
 * the exact bytes, and use the start of the Framework Laptop 16 touchpad
 * report descriptor to check mouse detection.
 */

#include <anx/types.h>
#include <anx/i2c_hid.h>
#include <anx/hid_boot.h>
#include <anx/string.h>
#include <anx/kprintf.h>

#define CHECK(cond, msg)						\
	do {								\
		if (!(cond)) {						\
			kprintf("  FAIL: %s\n", (msg));			\
			return -1;					\
		}							\
	} while (0)

static const uint8_t desc_bytes[ANX_I2C_HID_DESC_LEN] = {
	0x1E, 0x00,	/* wHIDDescLength 30 */
	0x00, 0x01,	/* bcdVersion 1.00 */
	0x34, 0x12,	/* wReportDescLength */
	0x21, 0x00,	/* wReportDescRegister */
	0x22, 0x00,	/* wInputRegister */
	0x20, 0x00,	/* wMaxInputLength */
	0x23, 0x00,	/* wOutputRegister */
	0x10, 0x00,	/* wMaxOutputLength */
	0x24, 0x00,	/* wCommandRegister */
	0x25, 0x00,	/* wDataRegister */
	0x3A, 0x09,	/* wVendorID 093A */
	0x74, 0x02,	/* wProductID 0274 */
	0x00, 0x01,	/* wVersionID */
	0, 0, 0, 0,	/* reserved */
};

/* Framework Laptop 16 touchpad (PIXA3854), first bytes of its report
 * descriptor, read from jekyll under Linux. */
static const uint8_t pixa_desc_start[] = {
	0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x85, 0x01,
	0x05, 0x01, 0x09, 0x01, 0xa1, 0x00, 0x05, 0x09,
};

/* Framework keyboard boot interface: a keyboard, not a mouse. */
static const uint8_t keyboard_desc_start[] = {
	0x05, 0x01, 0x09, 0x06, 0xa1, 0x01, 0x05, 0x07,
};

static int descriptor(void)
{
	struct anx_i2c_hid_desc d;
	uint8_t bad[ANX_I2C_HID_DESC_LEN];

	CHECK(anx_i2c_hid_parse_desc(desc_bytes, sizeof(desc_bytes), &d) == ANX_OK,
	      "valid descriptor parses");
	CHECK(d.report_desc_len == 0x1234 && d.report_desc_reg == 0x21,
	      "report descriptor length and register");
	CHECK(d.input_reg == 0x22 && d.max_input_len == 0x20, "input fields");
	CHECK(d.command_reg == 0x24 && d.data_reg == 0x25, "command fields");
	CHECK(d.vendor_id == 0x093A && d.product_id == 0x0274, "IDs");

	anx_memcpy(bad, desc_bytes, sizeof(bad));
	bad[2] = 0x01;	/* bcdVersion 0x0101 */
	CHECK(anx_i2c_hid_parse_desc(bad, sizeof(bad), &d) == ANX_EINVAL,
	      "unknown protocol version refused");

	anx_memcpy(bad, desc_bytes, sizeof(bad));
	bad[0] = 0x1C;	/* wrong descriptor length */
	CHECK(anx_i2c_hid_parse_desc(bad, sizeof(bad), &d) == ANX_EINVAL,
	      "wrong descriptor length refused");

	CHECK(anx_i2c_hid_parse_desc(desc_bytes, 20, &d) == ANX_EINVAL,
	      "short read refused");
	return 0;
}

static int commands(void)
{
	uint8_t c[ANX_I2C_HID_CMD_MAX];
	uint32_t n;

	n = anx_i2c_hid_cmd(c, 0x0024, ANX_I2C_HID_OP_SET_POWER, 0,
			    ANX_I2C_HID_PWR_ON);
	CHECK(n == 4 && c[0] == 0x24 && c[1] == 0x00 && c[2] == 0x00 &&
	      c[3] == 0x08, "SET_POWER ON is 24 00 00 08");

	n = anx_i2c_hid_cmd(c, 0x0024, ANX_I2C_HID_OP_RESET, 0, 0);
	CHECK(n == 4 && c[2] == 0x00 && c[3] == 0x01, "RESET is 24 00 00 01");

	n = anx_i2c_hid_cmd(c, 0x0124, 0x02, 1, 0x12);
	CHECK(n == 5 && c[0] == 0x24 && c[1] == 0x01 && c[2] == 0x1F &&
	      c[3] == 0x02 && c[4] == 0x12,
	      "report ID 0x12 escapes to a trailing byte");
	return 0;
}

static int input_framing(void)
{
	static const uint8_t empty[8] = { 0x00, 0x00 };
	static const uint8_t mouse[16] = {
		0x09, 0x00, 0x01, 0x01, 0x05, 0xFB, 0x00, 0x00, 0x00,
	};
	static const uint8_t overlong[4] = { 0x40, 0x00, 0x01, 0x02 };
	const uint8_t *p;
	uint32_t plen;
	uint8_t id;
	struct anx_hid_mouse_delta m;

	CHECK(anx_i2c_hid_input(empty, sizeof(empty), &p, &plen) == ANX_OK &&
	      plen == 0, "length 0 means no report");

	CHECK(anx_i2c_hid_input(mouse, sizeof(mouse), &p, &plen) == ANX_OK,
	      "report frame");
	CHECK(plen == 7 && p[0] == 0x01, "payload excludes the length bytes");
	CHECK(anx_hid_mouse_report(p + 1, plen - 1, &m) == ANX_OK &&
	      m.buttons == 0x01 && m.dx == 5 && m.dy == -5,
	      "mouse-mode payload decodes after the report ID");

	CHECK(anx_i2c_hid_input(overlong, sizeof(overlong), &p, &plen) ==
	      ANX_EINVAL, "a length past the buffer is refused");

	CHECK(anx_i2c_hid_mouse_report_id(pixa_desc_start,
					  sizeof(pixa_desc_start), &id) && id == 1,
	      "Framework touchpad descriptor is a mouse with report ID 1");
	CHECK(!anx_i2c_hid_mouse_report_id(keyboard_desc_start,
					   sizeof(keyboard_desc_start), &id),
	      "a keyboard descriptor is not a mouse");
	return 0;
}

int test_i2c_hid(void)
{
	if (descriptor())
		return -1;
	if (commands())
		return -1;
	if (input_framing())
		return -1;
	return 0;
}
