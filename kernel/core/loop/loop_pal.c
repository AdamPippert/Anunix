/*
 * loop_pal.c — PAL (Policy and Action Learning) cross-session accumulator stub.
 *
 * Phase 5 placeholder. Full implementation will maintain per-world action
 * statistics across sessions, enabling future loop sessions to benefit from
 * learned action penalties and win rates.
 */

#include <anx/memory.h>

void anx_pal_memory_update(const char *world_uri,
                            const struct anx_loop_memory_payload *payload)
{
	(void)world_uri;
	(void)payload;
}
