/*
 * anx/memory.h — PAL memory accumulator interface.
 *
 * anx_pal_memory_update() receives consolidated trajectory payloads
 * from the IBAL loop and stores them for world-model training.
 * The current implementation is a stub; Phase 6 will wire this to
 * the JEPA training buffer.
 */
#ifndef ANX_MEMORY_H
#define ANX_MEMORY_H

#include <anx/loop.h>

/* Accumulate one session's consolidated stats into the world model. */
void anx_pal_memory_update(const char *world_uri,
			    const struct anx_loop_memory_payload *payload);

#endif /* ANX_MEMORY_H */
