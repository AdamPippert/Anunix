/*
 * anx/spinlock.h — Kernel spinlocks.
 *
 * Uses compiler atomic builtins for portability across ARM64 and x86_64.
 */

#ifndef ANX_SPINLOCK_H
#define ANX_SPINLOCK_H

#include <anx/types.h>
#include <anx/arch.h>

struct anx_spinlock {
	volatile uint32_t locked;
};

#define ANX_SPINLOCK_INIT { .locked = 0 }

/* Initialize a spinlock */
static inline void anx_spin_init(struct anx_spinlock *lock)
{
	lock->locked = 0;
}

/* Acquire spinlock (busy-wait) */
static inline void anx_spin_lock(struct anx_spinlock *lock)
{
	while (__atomic_exchange_n(&lock->locked, 1, __ATOMIC_ACQUIRE))
		;
}

/* Release spinlock */
static inline void anx_spin_unlock(struct anx_spinlock *lock)
{
	__atomic_store_n(&lock->locked, 0, __ATOMIC_RELEASE);
}

/* Try to acquire, returns true on success */
static inline bool anx_spin_trylock(struct anx_spinlock *lock)
{
	return !__atomic_exchange_n(&lock->locked, 1, __ATOMIC_ACQUIRE);
}

/* Acquire with IRQs disabled, saves previous IRQ state */
static inline void anx_spin_lock_irqsave(struct anx_spinlock *lock,
					  bool *flags)
{
	*flags = arch_irq_disable();
	anx_spin_lock(lock);
}

/* Release and restore IRQ state */
static inline void anx_spin_unlock_irqrestore(struct anx_spinlock *lock,
					       bool flags)
{
	anx_spin_unlock(lock);
	arch_irq_restore(flags);
}

#endif /* ANX_SPINLOCK_H */
