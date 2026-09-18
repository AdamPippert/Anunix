/*
 * anx/shell.h — Kernel monitor shell.
 *
 * Interactive command loop for exercising kernel subsystems
 * over the serial console.
 */

#ifndef ANX_SHELL_H
#define ANX_SHELL_H

#include <anx/types.h>

/* Each input surface owns a cursor; history itself is shared and persisted. */
struct anx_shell_history_cursor {
	int32_t offset; /* -1: live draft, 0: newest entry */
	char draft[512];
};
void anx_shell_history_reset(struct anx_shell_history_cursor *cursor);
/* direction -1: older, +1: newer. Returns copied length or -1 if unchanged. */
int anx_shell_history_move(struct anx_shell_history_cursor *cursor,
			   int direction, char *input, uint32_t capacity);
/* Consecutive duplicates and credential-bearing commands are omitted. */
void anx_shell_history_record(const char *line);

/* ANSI input state survives partial serial reads and SSH packet boundaries. */
struct anx_shell_escape {
	uint8_t state;
	uint8_t parameter;
};
/* Decode one byte to an input key/unicode; incomplete escapes yield neither. */
uint32_t anx_shell_input_decode(struct anx_shell_escape *escape, uint8_t byte,
			       uint32_t *unicode);

/* Edit a bounded ASCII draft; returns true for handled editing/history keys. */
bool anx_shell_input_key(char *buf, uint32_t capacity, uint32_t *len,
			 uint32_t *pos, struct anx_shell_history_cursor *recall,
			 uint32_t key, uint32_t unicode);

/* Enter the interactive shell (does not return) */
void anx_shell_run(void) __attribute__((noreturn));

/* Execute a single shell command line (for programmatic use) */
void anx_shell_execute(const char *command);

#endif /* ANX_SHELL_H */
