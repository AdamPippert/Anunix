/*
 * crc32.c — CRC-32 (IEEE 802.3, reflected, polynomial 0xEDB88320).
 *
 * Table is built on first use. The kernel is single-threaded through this
 * path, so no locking is needed; a second initialisation would be harmless
 * anyway because the table is deterministic.
 */

#include <anx/crc32.h>

static uint32_t crc32_table[256];
static bool crc32_ready;

static void crc32_init(void)
{
	uint32_t i, j, c;

	for (i = 0; i < 256; i++) {
		c = i;
		for (j = 0; j < 8; j++) {
			if (c & 1)
				c = 0xEDB88320 ^ (c >> 1);
			else
				c >>= 1;
		}
		crc32_table[i] = c;
	}
	crc32_ready = true;
}

uint32_t anx_crc32(const void *data, uint32_t len)
{
	const uint8_t *p = (const uint8_t *)data;
	uint32_t crc = 0xFFFFFFFF;
	uint32_t i;

	if (!crc32_ready)
		crc32_init();

	for (i = 0; i < len; i++)
		crc = crc32_table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);

	return crc ^ 0xFFFFFFFF;
}
