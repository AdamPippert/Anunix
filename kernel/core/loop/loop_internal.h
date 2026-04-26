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

/* loop_branch.c — Phase 13 */
int anx_loop_branch_schedule_and_merge(anx_oid_t parent_oid);

#endif /* ANX_LOOP_INTERNAL_H */
