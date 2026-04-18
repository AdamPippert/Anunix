/*
 * anx/shell.h — Kernel monitor shell.
 *
 * Interactive command loop for exercising kernel subsystems
 * over the serial console.
 */

#ifndef ANX_SHELL_H
#define ANX_SHELL_H

/* Enter the interactive shell (does not return) */
void anx_shell_run(void) __attribute__((noreturn));

/* Execute a single shell command line (for programmatic use) */
void anx_shell_execute(const char *command);

#endif /* ANX_SHELL_H */
