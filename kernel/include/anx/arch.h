/*
 * anx/arch.h — Architecture abstraction interface.
 *
 * Every architecture must implement these functions. Core kernel code
 * calls only these — never architecture-specific headers directly.
 */

#ifndef ANX_ARCH_H
#define ANX_ARCH_H

#include <anx/types.h>

/* Early hardware initialization, called before anything else */
void arch_early_init(void);

/* Full hardware initialization after memory subsystem is up */
void arch_init(void);

/* Halt the processor */
void arch_halt(void) __attribute__((noreturn));

/* Disable/enable interrupts, return previous state */
bool arch_irq_disable(void);
void arch_irq_enable(void);
void arch_irq_restore(bool state);

/* Read current timestamp (nanoseconds) */
anx_time_t arch_time_now(void);

/* Console I/O for early boot (before any drivers) */
void arch_console_putc(char c);
void arch_console_puts(const char *s);
int  arch_console_getc(void);
bool arch_console_has_input(void);

/* Memory barrier primitives */
void arch_mb(void);	/* full memory barrier */
void arch_rmb(void);	/* read barrier */
void arch_wmb(void);	/* write barrier */

#endif /* ANX_ARCH_H */
