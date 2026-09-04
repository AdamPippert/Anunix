/*
 * anx/mock_blk.h — Test-only RAM-backed block device controls.
 *
 * These entry points live in the test harness (mock_arch.c) and have
 * no in-kernel implementation. Including this header from production
 * code is a mistake.
 */

#ifndef ANX_MOCK_BLK_H
#define ANX_MOCK_BLK_H

#include <anx/types.h>
#include <anx/blk.h>

/* Drop any existing mock devices, then register one with the given
 * capacity and make it the active block device. Clamped to an internal
 * pool size. Call once per test. */
void test_mock_blk_init(uint64_t sectors);

/* Register one more RAM-backed device and return it. The first one
 * registered becomes active; later ones are free for a RAID array.
 * Returns NULL once the harness runs out of pools. */
struct anx_blk_dev *test_mock_blk_add(uint64_t sectors);

/* Drop every mock device (anx_blk_ready() returns false). */
void test_mock_blk_teardown(void);

#endif /* ANX_MOCK_BLK_H */
