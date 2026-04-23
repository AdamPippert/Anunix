/*
 * loop_internal.h — Private declarations shared across loop_*.c files.
 */

#ifndef ANX_LOOP_INTERNAL_H
#define ANX_LOOP_INTERNAL_H

#include <anx/loop.h>
#include <anx/spinlock.h>

extern struct anx_loop_session g_loop_sessions[ANX_LOOP_MAX_SESSIONS];
extern struct anx_spinlock     g_loop_lock;
extern bool                    g_loop_initialized;

#endif /* ANX_LOOP_INTERNAL_H */
