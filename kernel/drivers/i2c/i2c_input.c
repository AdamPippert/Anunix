/*
 * i2c_input.c — HID-over-I2C input devices found through ACPI.
 *
 * The DSDT and SSDTs are searched for devices with the HID-over-I2C
 * compatible ID PNP0C50. For each one the driver resolves its I2C
 * controller and interrupt GPIO by name, powers and configures the
 * controller, reads the HID descriptor, and binds the device when its
 * report descriptor opens a mouse collection. A touchpad reports in that
 * mouse mode until the host selects its precision-touchpad mode, so its
 * reports share the boot mouse layout.
 *
 * Reports are polled. When the device has an interrupt GPIO, its level
 * says whether a report is waiting, so an idle touchpad costs one MMIO
 * read per poll.
 *
 * Supported controllers: AMD FCH DesignWare I2C (AMDI0010), with AMD FCH
 * GPIO (AMDI0030). Other controllers are logged and skipped.
 */

#include <anx/types.h>
#include <anx/i2c_input.h>
#include <anx/i2c_hid.h>
#include <anx/hid_boot.h>
#include <anx/usb_mouse.h>
#include <anx/aml_res.h>
#include <anx/acpi.h>
#include <anx/dw_i2c.h>
#include <anx/amd_fch.h>
#include <anx/fb.h>
#include <anx/string.h>
#include <anx/kprintf.h>
#include <anx/delay.h>
#include <anx/perf.h>

#define HID_I2C_CID		"PNP0C50"
#define AMD_I2C_HID		"AMDI0010"
#define AMD_GPIO_HID		"AMDI0030"

/* Input clock Linux assigns AMDI0010 (drivers/acpi/acpi_apd.c). */
#define AMD_I2C_CLK_HZ		150000000u

#define ACPI_HEADER_LEN		36
#define I2C_BUS_MAX		4
#define I2C_INPUT_MAX		4
#define REPORT_BUF		256
#define REPORT_DESC_PEEK	16
#define RESET_WAIT_MS		1000u

struct i2c_bus {
	uint32_t base;
	bool     ok;
	struct anx_dw_i2c dw;
};

struct i2c_input {
	struct anx_dw_i2c *bus;
	uint16_t addr;
	char     name[5];
	struct anx_i2c_hid_desc desc;
	uint8_t  report_id;
	volatile uint8_t *gpio;
	uint16_t gpio_pin;
	bool     active_low;
	bool     reported;
	uint8_t  buf[REPORT_BUF];
};

static struct i2c_bus   g_bus[I2C_BUS_MAX];
static uint32_t         g_bus_count;
static struct i2c_input g_dev[I2C_INPUT_MAX];
static uint32_t         g_dev_count;
static uint32_t         g_scr_w = 1024;
static uint32_t         g_scr_h = 768;
static bool             g_probed;

static struct anx_dw_i2c *bus_get(uint32_t base)
{
	struct i2c_bus *b;
	uint32_t i;
	int ret;

	for (i = 0; i < g_bus_count; i++)
		if (g_bus[i].base == base)
			return g_bus[i].ok ? &g_bus[i].dw : NULL;
	if (g_bus_count >= I2C_BUS_MAX)
		return NULL;

	b = &g_bus[g_bus_count++];
	b->base = base;
	ret = anx_dw_i2c_init(&b->dw, base, AMD_I2C_CLK_HZ);
	if (ret == ANX_ENODEV && anx_amd_i2c_power_on(base) == ANX_OK)
		ret = anx_dw_i2c_init(&b->dw, base, AMD_I2C_CLK_HZ);
	b->ok = ret == ANX_OK;
	if (!b->ok)
		kprintf("i2c-hid: controller at 0x%x unavailable (%d)\n",
			base, ret);
	return b->ok ? &b->dw : NULL;
}

static bool report_waiting(const struct i2c_input *d)
{
	bool level;

	if (!d->gpio)
		return true;
	level = anx_amd_gpio_level(d->gpio, d->gpio_pin);
	return d->active_low ? !level : level;
}

static int read_register(struct i2c_input *d, uint16_t reg, uint8_t *buf,
			 uint32_t len)
{
	uint8_t w[2];

	w[0] = (uint8_t)reg;
	w[1] = (uint8_t)(reg >> 8);
	return anx_dw_i2c_xfer(d->bus, d->addr, w, 2, buf, len);
}

static int send_cmd(struct i2c_input *d, uint8_t opcode, uint8_t report_id)
{
	uint8_t cmd[ANX_I2C_HID_CMD_MAX];
	uint32_t n = anx_i2c_hid_cmd(cmd, d->desc.command_reg, opcode, 0,
				     report_id);

	return anx_dw_i2c_xfer(d->bus, d->addr, cmd, n, NULL, 0);
}

static uint32_t input_len(const struct i2c_input *d)
{
	return d->desc.max_input_len < REPORT_BUF ? d->desc.max_input_len
						  : REPORT_BUF;
}

/* Power on and reset; the device answers the reset with an empty report. */
static int device_reset(struct i2c_input *d)
{
	uint64_t start, budget;

	if (send_cmd(d, ANX_I2C_HID_OP_SET_POWER, ANX_I2C_HID_PWR_ON) != ANX_OK)
		return ANX_EIO;
	anx_delay_ms(1);
	if (send_cmd(d, ANX_I2C_HID_OP_RESET, 0) != ANX_OK)
		return ANX_EIO;

	start = anx_rdtsc();
	budget = anx_tsc_budget_us((uint64_t)RESET_WAIT_MS * 1000);
	for (;;) {
		const uint8_t *payload;
		uint32_t plen;

		if (report_waiting(d) &&
		    anx_dw_i2c_xfer(d->bus, d->addr, NULL, 0, d->buf,
				    input_len(d)) == ANX_OK &&
		    anx_i2c_hid_input(d->buf, input_len(d), &payload,
				      &plen) == ANX_OK && plen == 0)
			break;
		if (anx_rdtsc() - start > budget) {
			kprintf("i2c-hid: %s reset not acknowledged; continuing\n",
				d->name);
			break;
		}
		anx_delay_ms(5);
	}

	/* Some devices sleep again after reset; Linux repeats power-on. */
	return send_cmd(d, ANX_I2C_HID_OP_SET_POWER, ANX_I2C_HID_PWR_ON);
}

static void try_bind(const uint8_t *aml, uint32_t len,
		     const struct anx_aml_device *adev)
{
	struct anx_aml_i2c i2c;
	struct anx_aml_gpio_int gpio;
	struct anx_aml_device ctrl;
	struct i2c_input *d;
	uint8_t raw[ANX_I2C_HID_DESC_LEN];
	uint8_t rdesc[REPORT_DESC_PEEK];
	uint32_t base, size;
	uint16_t desc_reg;

	if (g_dev_count >= I2C_INPUT_MAX)
		return;
	if (!anx_aml_i2c(aml, adev, &i2c)) {
		kprintf("i2c-hid: %s has no I2C resource\n", adev->name);
		return;
	}
	if (!anx_aml_hid_i2c_desc_reg(aml, adev, &desc_reg)) {
		kprintf("i2c-hid: %s: HID descriptor register not found\n",
			adev->name);
		return;
	}
	if (!anx_aml_find_device(aml, len, i2c.bus, AMD_I2C_HID, &ctrl) ||
	    !anx_aml_mem32(aml, &ctrl, &base, &size)) {
		kprintf("i2c-hid: %s: controller %s not supported\n",
			adev->name, i2c.bus);
		return;
	}

	d = &g_dev[g_dev_count];
	anx_memset(d, 0, sizeof(*d));
	anx_memcpy(d->name, adev->name, sizeof(d->name));
	d->addr = i2c.addr;
	d->bus = bus_get(base);
	if (!d->bus)
		return;

	if (anx_aml_gpio_int(aml, adev, &gpio)) {
		struct anx_aml_device gdev;
		uint32_t gbase, gsize;

		if (anx_aml_find_device(aml, len, gpio.ctrl, AMD_GPIO_HID, &gdev) &&
		    anx_aml_mem32(aml, &gdev, &gbase, &gsize)) {
			d->gpio = anx_amd_gpio_map(gbase);
			d->gpio_pin = gpio.pin;
			d->active_low = gpio.active_low;
		}
	}

	if (read_register(d, desc_reg, raw, sizeof(raw)) != ANX_OK ||
	    anx_i2c_hid_parse_desc(raw, sizeof(raw), &d->desc) != ANX_OK) {
		kprintf("i2c-hid: %s at 0x%02x on %s not responding\n",
			d->name, (uint32_t)d->addr, i2c.bus);
		return;
	}
	if (read_register(d, d->desc.report_desc_reg, rdesc,
			  sizeof(rdesc)) != ANX_OK ||
	    !anx_i2c_hid_mouse_report_id(rdesc, sizeof(rdesc), &d->report_id)) {
		kprintf("i2c-hid: %s %04x:%04x is not a pointing device; skipped\n",
			d->name, (uint32_t)d->desc.vendor_id,
			(uint32_t)d->desc.product_id);
		return;
	}
	if (device_reset(d) != ANX_OK) {
		kprintf("i2c-hid: %s power-on failed\n", d->name);
		return;
	}

	g_dev_count++;
	kprintf("i2c-hid: %s %04x:%04x at 0x%02x on %s, report %u, %s\n",
		d->name, (uint32_t)d->desc.vendor_id,
		(uint32_t)d->desc.product_id, (uint32_t)d->addr, i2c.bus,
		(uint32_t)d->report_id,
		d->gpio ? "GPIO ready line" : "polled");
}

static void scan_table(const uint8_t *table, uint32_t table_len)
{
	const uint8_t *aml;
	uint32_t len, pos = 0;
	struct anx_aml_device dev;

	if (!table || table_len <= ACPI_HEADER_LEN)
		return;
	aml = table + ACPI_HEADER_LEN;
	len = table_len - ACPI_HEADER_LEN;

	while (anx_aml_next_device(aml, len, &pos, &dev))
		if (anx_aml_device_has_id(aml, &dev, HID_I2C_CID))
			try_bind(aml, len, &dev);
}

int anx_i2c_input_init(void)
{
	const struct anx_fb_info *fb;
	const uint8_t *table;
	uint32_t len, i;

	if (g_probed)
		return g_dev_count > 0 ? ANX_OK : ANX_ENOENT;
	g_probed = true;

	fb = anx_fb_get_info();
	if (fb && fb->available) {
		g_scr_w = fb->width;
		g_scr_h = fb->height;
	}

	table = anx_acpi_find_table("DSDT", 0, &len);
	if (!table) {
		kprintf("i2c-hid: no DSDT\n");
		return ANX_ENOENT;
	}
	scan_table(table, len);
	for (i = 0; (table = anx_acpi_find_table("SSDT", i, &len)) != NULL; i++)
		scan_table(table, len);

	kprintf("i2c-hid: %u device(s)\n", g_dev_count);
	return g_dev_count > 0 ? ANX_OK : ANX_ENOENT;
}

void anx_i2c_input_poll(void)
{
	uint32_t i;

	for (i = 0; i < g_dev_count; i++) {
		struct i2c_input *d = &g_dev[i];
		struct anx_hid_mouse_delta m;
		struct anx_hid_mouse_report r;
		const uint8_t *payload;
		uint32_t plen;

		if (!report_waiting(d))
			continue;
		if (anx_dw_i2c_xfer(d->bus, d->addr, NULL, 0, d->buf,
				    input_len(d)) != ANX_OK)
			continue;
		if (anx_i2c_hid_input(d->buf, input_len(d), &payload,
				      &plen) != ANX_OK ||
		    plen < 4 || payload[0] != d->report_id)
			continue;

		if (!d->reported) {
			kprintf("i2c-hid: first report from %s: %02x %02x %02x\n",
				d->name, (uint32_t)payload[1],
				(uint32_t)payload[2], (uint32_t)payload[3]);
			d->reported = true;
		}
		if (anx_hid_mouse_report(payload + 1, plen - 1, &m) != ANX_OK)
			continue;
		r.buttons = m.buttons;
		r.x       = m.dx;
		r.y       = m.dy;
		r.wheel   = m.wheel;
		anx_usb_mouse_report(&r, g_scr_w, g_scr_h);
	}
}
