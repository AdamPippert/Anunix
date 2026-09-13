/*
 * anx/crc32.h — CRC-32 (IEEE 802.3, reflected, polynomial 0xEDB88320).
 *
 * The GPT header and partition array both carry CRC-32 checksums, and the
 * partition scanner runs long before the installer, so this lives in lib
 * rather than in the installer that first needed it.
 */

#ifndef ANX_CRC32_H
#define ANX_CRC32_H

#include <anx/types.h>

/* CRC-32 over len bytes at data. */
uint32_t anx_crc32(const void *data, uint32_t len);

#endif /* ANX_CRC32_H */
