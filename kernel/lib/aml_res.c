/*
 * aml_res.c — Find device resources in ACPI AML without an interpreter.
 *
 * A laptop declares its touchpad in the DSDT, typically as:
 *
 *   Device (TPAD) {
 *       Name (_HID, "PIXA3854")  Name (_CID, "PNP0C50")
 *       Name (SBFB, Buffer () { I2cSerialBusV2 (0x2C, ..., "\_SB.I2CD") })
 *       Name (SBFG, Buffer () { GpioInt (..., "\_SB.GPIO") { 8 } })
 *       Method (_DSM, 4) { ... If (Arg2 == One) { Return (0x20) } ... }
 *   }
 *
 * The buffers and the returned constant are stored verbatim, so a
 * structural search recovers them. Every descriptor found is checked
 * against its own length fields and must end inside the device package,
 * which rejects bytes that only look like a descriptor.
 *
 * Encodings follow the ACPI specification: Device and PkgLength in the
 * AML grammar, and the large resource descriptors for Memory32Fixed,
 * GpioInt, and I2cSerialBus. Field layouts match Linux ACPICA amlresrc.h.
 */

#include <anx/aml_res.h>
#include <anx/string.h>

#define AML_EXT_OP		0x5B
#define AML_DEVICE_OP		0x82
#define AML_NAME_OP		0x08
#define AML_STRING_PREFIX	0x0D
#define AML_RETURN_OP		0xA4
#define AML_BYTE_PREFIX		0x0A
#define AML_WORD_PREFIX		0x0B
#define AML_ONE_OP		0x01
#define AML_LEQUAL_OP		0x93
#define AML_ARG2_OP		0x6A

#define RES_MEM32_FIXED		0x86
#define RES_MEM32_FIXED_LEN	9
#define RES_GPIO		0x8C
#define RES_GPIO_FIXED_LEN	23	/* bytes before the pin table */
#define RES_SERIAL_BUS		0x8E
#define RES_SERIAL_FIXED_LEN	12	/* bytes before type-specific data */
#define SERIAL_TYPE_I2C		1
#define I2C_TYPE_DATA_MIN	6	/* connection speed and address */
#define GPIO_CONN_INTERRUPT	0

/* _DSM UUID for HID over I2C, 3cdff6f7-4267-4555-ad05-b30a3d8938de,
 * in the byte order ToUUID stores. */
static const uint8_t hid_i2c_uuid[16] = {
	0xf7, 0xf6, 0xdf, 0x3c, 0x67, 0x42, 0x55, 0x45,
	0xad, 0x05, 0xb3, 0x0a, 0x3d, 0x89, 0x38, 0xde,
};

static uint16_t rd16(const uint8_t *p)
{
	return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t rd32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool name_char(uint8_t c)
{
	return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

static bool bytes_equal(const uint8_t *a, const uint8_t *b, uint32_t n)
{
	uint32_t i;

	for (i = 0; i < n; i++)
		if (a[i] != b[i])
			return false;
	return true;
}

/* Decode a PkgLength. Returns the bytes it occupies, or 0 past the end. */
static uint32_t pkg_length(const uint8_t *aml, uint32_t len, uint32_t at,
			   uint32_t *value)
{
	uint32_t extra, v, k;

	if (at >= len)
		return 0;
	extra = aml[at] >> 6;
	if (at + 1 + extra > len)
		return 0;
	if (extra == 0) {
		*value = aml[at] & 0x3F;
		return 1;
	}
	v = aml[at] & 0x0F;
	for (k = 0; k < extra; k++)
		v |= (uint32_t)aml[at + 1 + k] << (4 + 8 * k);
	*value = v;
	return 1 + extra;
}

bool anx_aml_next_device(const uint8_t *aml, uint32_t len, uint32_t *pos,
			 struct anx_aml_device *dev)
{
	uint32_t i;

	if (!aml || !pos || !dev)
		return false;

	for (i = *pos; i + 2 < len; i++) {
		uint32_t pkg, nb, name_at, k;
		bool ok = true;

		if (aml[i] != AML_EXT_OP || aml[i + 1] != AML_DEVICE_OP)
			continue;
		nb = pkg_length(aml, len, i + 2, &pkg);
		if (nb == 0 || pkg < nb + 4 || pkg > len - (i + 2))
			continue;
		name_at = i + 2 + nb;
		for (k = 0; k < 4; k++)
			ok = ok && name_char(aml[name_at + k]);
		if (!ok)
			continue;

		dev->start = i;
		dev->end   = i + 2 + pkg;
		anx_memcpy(dev->name, aml + name_at, 4);
		dev->name[4] = '\0';
		*pos = i + 2;
		return true;
	}
	return false;
}

bool anx_aml_device_has_id(const uint8_t *aml, const struct anx_aml_device *dev,
			   const char *id)
{
	uint32_t idlen, i;

	if (!aml || !dev || !id)
		return false;
	idlen = (uint32_t)anx_strlen(id);

	for (i = dev->start; i + 6 + idlen + 1 <= dev->end; i++) {
		const uint8_t *p = aml + i;

		if (p[0] != AML_NAME_OP || p[1] != '_' ||
		    p[5] != AML_STRING_PREFIX)
			continue;
		if (!((p[2] == 'H' || p[2] == 'C') && p[3] == 'I' && p[4] == 'D'))
			continue;
		if (bytes_equal(p + 6, (const uint8_t *)id, idlen) &&
		    p[6 + idlen] == '\0')
			return true;
	}
	return false;
}

bool anx_aml_find_device(const uint8_t *aml, uint32_t len, const char *name,
			 const char *id, struct anx_aml_device *dev)
{
	uint32_t pos = 0;

	while (anx_aml_next_device(aml, len, &pos, dev)) {
		if (name && !bytes_equal((const uint8_t *)dev->name,
					 (const uint8_t *)name, 4))
			continue;
		if (id && !anx_aml_device_has_id(aml, dev, id))
			continue;
		return true;
	}
	return false;
}

bool anx_aml_mem32(const uint8_t *aml, const struct anx_aml_device *dev,
		   uint32_t *base, uint32_t *size)
{
	uint32_t i;

	if (!aml || !dev || !base || !size)
		return false;

	for (i = dev->start; i + 3 + RES_MEM32_FIXED_LEN <= dev->end; i++) {
		const uint8_t *d = aml + i;

		if (d[0] != RES_MEM32_FIXED || rd16(d + 1) != RES_MEM32_FIXED_LEN)
			continue;
		*base = rd32(d + 4);
		*size = rd32(d + 8);
		return true;
	}
	return false;
}

/*
 * Copy the last name segment of a NUL-terminated resource source path,
 * such as "\_SB.I2CD", if the whole string ends before limit.
 */
static bool path_tail(const uint8_t *s, const uint8_t *limit, char out[5])
{
	const uint8_t *seg = s;
	const uint8_t *p;
	uint32_t k;

	for (p = s; p < limit && *p != '\0'; p++)
		if (*p == '\\' || *p == '.' || *p == '^')
			seg = p + 1;
	if (p >= limit || p - seg != 4)
		return false;
	for (k = 0; k < 4; k++) {
		if (!name_char(seg[k]))
			return false;
		out[k] = (char)seg[k];
	}
	out[4] = '\0';
	return true;
}

bool anx_aml_i2c(const uint8_t *aml, const struct anx_aml_device *dev,
		 struct anx_aml_i2c *out)
{
	uint32_t i;

	if (!aml || !dev || !out)
		return false;

	for (i = dev->start;
	     i + RES_SERIAL_FIXED_LEN + I2C_TYPE_DATA_MIN <= dev->end; i++) {
		const uint8_t *d = aml + i;
		uint32_t total, tdlen;

		if (d[0] != RES_SERIAL_BUS || d[5] != SERIAL_TYPE_I2C)
			continue;
		total = 3u + rd16(d + 1);
		tdlen = rd16(d + 10);
		if (tdlen < I2C_TYPE_DATA_MIN || total > dev->end - i ||
		    RES_SERIAL_FIXED_LEN + tdlen >= total)
			continue;
		if (!path_tail(d + RES_SERIAL_FIXED_LEN + tdlen, d + total,
			       out->bus))
			continue;
		out->speed_hz = rd32(d + 12);
		out->addr     = rd16(d + 16);
		return true;
	}
	return false;
}

bool anx_aml_gpio_int(const uint8_t *aml, const struct anx_aml_device *dev,
		      struct anx_aml_gpio_int *out)
{
	uint32_t i;

	if (!aml || !dev || !out)
		return false;

	for (i = dev->start; i + RES_GPIO_FIXED_LEN <= dev->end; i++) {
		const uint8_t *d = aml + i;
		uint32_t total, pin_off, src_off;
		uint16_t int_flags;

		if (d[0] != RES_GPIO || d[4] != GPIO_CONN_INTERRUPT)
			continue;
		total   = 3u + rd16(d + 1);
		pin_off = rd16(d + 14);
		src_off = rd16(d + 17);
		if (total > dev->end - i || pin_off < RES_GPIO_FIXED_LEN ||
		    pin_off + 2 > total || src_off < RES_GPIO_FIXED_LEN ||
		    src_off >= total)
			continue;
		if (!path_tail(d + src_off, d + total, out->ctrl))
			continue;

		int_flags = rd16(d + 7);
		out->pin        = rd16(d + pin_off);
		out->level      = (int_flags & 0x1) == 0;
		out->active_low = ((int_flags >> 1) & 0x3) == 1;
		return true;
	}
	return false;
}

bool anx_aml_hid_i2c_desc_reg(const uint8_t *aml,
			      const struct anx_aml_device *dev, uint16_t *reg)
{
	uint32_t i, k, m;

	if (!aml || !dev || !reg)
		return false;

	for (i = dev->start; i + sizeof(hid_i2c_uuid) <= dev->end; i++) {
		if (!bytes_equal(aml + i, hid_i2c_uuid, sizeof(hid_i2c_uuid)))
			continue;

		/* After the UUID: If (Arg2 == One) { Return (constant) } */
		for (k = i + sizeof(hid_i2c_uuid); k + 3 <= dev->end; k++) {
			if (aml[k] != AML_LEQUAL_OP || aml[k + 1] != AML_ARG2_OP ||
			    aml[k + 2] != AML_ONE_OP)
				continue;
			for (m = k + 3; m + 2 < dev->end && m < k + 12; m++) {
				if (aml[m] != AML_RETURN_OP)
					continue;
				if (aml[m + 1] == AML_BYTE_PREFIX) {
					*reg = aml[m + 2];
					return true;
				}
				if (aml[m + 1] == AML_WORD_PREFIX && m + 3 < dev->end) {
					*reg = rd16(aml + m + 2);
					return true;
				}
				if (aml[m + 1] == AML_ONE_OP) {
					*reg = 1;
					return true;
				}
				return false;
			}
		}
		return false;
	}
	return false;
}
