/*
 * anx/io.h — Port I/O and MMIO accessors.
 *
 * Provides inline functions for x86_64 I/O port access (in/out)
 * and memory-mapped I/O. These replace the scattered static inline
 * definitions in arch_init.c and exception.c.
 */

#ifndef ANX_IO_H
#define ANX_IO_H

#include <anx/types.h>

/* --- Port I/O (x86_64) --- */

/* Read a byte from an I/O port */
static inline uint8_t anx_inb(uint16_t port)
{
	uint8_t val;
	__asm__ volatile("inb %1, %0" : "=a"(val) : "Nd"(port));
	return val;
}

/* Read a 16-bit word from an I/O port */
static inline uint16_t anx_inw(uint16_t port)
{
	uint16_t val;
	__asm__ volatile("inw %1, %0" : "=a"(val) : "Nd"(port));
	return val;
}

/* Read a 32-bit dword from an I/O port */
static inline uint32_t anx_inl(uint16_t port)
{
	uint32_t val;
	__asm__ volatile("inl %1, %0" : "=a"(val) : "Nd"(port));
	return val;
}

/* Write a byte to an I/O port */
static inline void anx_outb(uint8_t val, uint16_t port)
{
	__asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

/* Write a 16-bit word to an I/O port */
static inline void anx_outw(uint16_t val, uint16_t port)
{
	__asm__ volatile("outw %0, %1" : : "a"(val), "Nd"(port));
}

/* Write a 32-bit dword to an I/O port */
static inline void anx_outl(uint32_t val, uint16_t port)
{
	__asm__ volatile("outl %0, %1" : : "a"(val), "Nd"(port));
}

/* Small delay via dummy write to ISA post-code port */
static inline void anx_io_wait(void)
{
	anx_outb(0, 0x80);
}

/* --- Memory-mapped I/O --- */

/* Read 8/16/32 bits from an MMIO address */
static inline uint8_t anx_mmio_read8(volatile void *addr)
{
	return *(volatile uint8_t *)addr;
}

static inline uint16_t anx_mmio_read16(volatile void *addr)
{
	return *(volatile uint16_t *)addr;
}

static inline uint32_t anx_mmio_read32(volatile void *addr)
{
	return *(volatile uint32_t *)addr;
}

/* Write 8/16/32 bits to an MMIO address */
static inline void anx_mmio_write8(volatile void *addr, uint8_t val)
{
	*(volatile uint8_t *)addr = val;
}

static inline void anx_mmio_write16(volatile void *addr, uint16_t val)
{
	*(volatile uint16_t *)addr = val;
}

static inline void anx_mmio_write32(volatile void *addr, uint32_t val)
{
	*(volatile uint32_t *)addr = val;
}

#endif /* ANX_IO_H */
