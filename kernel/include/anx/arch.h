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

/* Framebuffer discovery — arch fills in info if available */
struct anx_fb_info;
void arch_fb_detect(struct anx_fb_info *info);

/* Console I/O for early boot (before any drivers) */
void arch_console_putc(char c);
void arch_console_puts(const char *s);
int  arch_console_getc(void);
bool arch_console_has_input(void);

/* Exception and interrupt initialization */
void arch_exception_init(void);

/* Timer tick count (monotonically increasing) */
uint64_t arch_timer_ticks(void);

/* Register a callback fired on every PIT tick (IRQ context, keep it short) */
void arch_set_timer_callback(void (*fn)(void));

/* Hardware capability probing */
struct anx_hw_inventory;
void arch_probe_hw(struct anx_hw_inventory *inv);

/* Boot command line (from bootloader, NULL if unavailable) */
const char *arch_boot_cmdline(void);

/* Memory barrier primitives */
void arch_mb(void);	/* full memory barrier */
void arch_rmb(void);	/* read barrier */
void arch_wmb(void);	/* write barrier */

/*
 * Ring-3 execution primitives (real, not simulated — see posix.c).
 *
 * arch_enter_usermode() drops to ring 3 at `entry` with the given
 * user stack and does not return via the normal call/ret path — it
 * "returns" the exit code only when the running program invokes the
 * exit syscall, which calls arch_return_to_kernel() to resume the
 * saved kernel context. Exactly one usermode program may be in flight
 * at a time (no nested exec).
 */
uint64_t arch_enter_usermode(uint64_t entry, uint64_t user_rsp);
void arch_return_to_kernel(int code) __attribute__((noreturn));

/*
 * Copies filesz bytes from src to vaddr, then zero-fills the remaining
 * (memsz - filesz) bytes — one ELF PT_LOAD segment. On x86_64 this is a
 * raw pointer write (vaddr is a physical==virtual identity-mapped
 * address, only valid inside the real kernel); host test builds no-op
 * this since there is no such address space to write into.
 */
void arch_exec_load_segment(uint64_t vaddr, const void *src,
			     uint64_t filesz, uint64_t memsz);

#endif /* ANX_ARCH_H */
