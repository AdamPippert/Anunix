/*
 * memory.h — PAL (Policy and Action Learning) cross-session memory accumulator.
 *
 * Stub for RFC-0020 Phase 5. Provides the interface for feeding
 * post-commit memory consolidation payloads into the cross-session
 * accumulator so that future sessions benefit from learned action costs.
 */

#ifndef ANX_MEMORY_H
#define ANX_MEMORY_H

#include <anx/loop.h>

/*
 * Update the PAL cross-session memory accumulator for a given world URI
 * with the consolidation payload produced after a committed or aborted
 * loop session.  This is a stub — full implementation comes in Phase 5.
 */
void anx_pal_memory_update(const char *world_uri,
                            const struct anx_loop_memory_payload *payload);

#endif /* ANX_MEMORY_H */
