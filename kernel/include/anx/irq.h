/*
 * anx/irq.h — Dynamic IRQ handler registration.
 *
 * Allows device drivers to register interrupt handlers for PIC IRQ
 * lines 0-15 (mapped to vectors 32-47 on x86_64).
 */

#ifndef ANX_IRQ_H
#define ANX_IRQ_H

#include <anx/types.h>

typedef void (*anx_irq_handler_t)(uint32_t irq, void *arg);

/* Register a handler for a PIC IRQ line (0-15) */
int anx_irq_register(uint8_t irq, anx_irq_handler_t handler, void *arg);

/* Unmask a PIC IRQ line so the device can deliver interrupts */
void anx_irq_unmask(uint8_t irq);

/* Mask a PIC IRQ line */
void anx_irq_mask(uint8_t irq);

#endif /* ANX_IRQ_H */
