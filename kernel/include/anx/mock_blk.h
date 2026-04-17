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

/* Enable the RAM-backed mock block device with the given capacity.
 * Clamped to an internal pool size. Call once per test. */
void test_mock_blk_init(uint64_t sectors);

/* Disable the mock block device (anx_blk_ready() returns false). */
void test_mock_blk_teardown(void);

#endif /* ANX_MOCK_BLK_H */
