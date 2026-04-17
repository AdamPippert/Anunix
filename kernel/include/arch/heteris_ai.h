/*
 * heteris_ai.h — Inline assembly wrappers for Heteris AI extension instructions.
 *
 * Since no assembler understands Heteris AI mnemonics yet, we encode
 * instructions as raw .word directives. The 64-bit AI micro-op is emitted
 * as two consecutive 32-bit words (little-endian).
 *
 * AI Micro-Op Format (64-bit):
 *   [63:56] opcode(8)  [55:51] rd(5)   [50:46] rs1(5)  [45:41] rs2(5)
 *   [40:37] tile(4)    [36:25] imm(12) [24:20] dtype(5) [19:15] func(5)
 *   [14:0]  reserved(15)
 *
 * Opcodes:
 *   0x10 = AI.MAC.BF16,  0x11 = AI.MAC.INT8
 *   0x20 = AI.LD.TILE,   0x21 = AI.ST.TILE
 *   0x30 = AI.SYNC,      0x31 = AI.TILE.CFG
 */

#ifndef HETERIS_AI_H
#define HETERIS_AI_H

#include <anx/types.h>

/* Build a 64-bit AI instruction encoding */
static inline uint64_t _heteris_ai_encode(
	uint8_t opcode, uint8_t rd, uint8_t rs1, uint8_t rs2,
	uint8_t tile, uint16_t imm, uint8_t dtype, uint8_t func)
{
	return ((uint64_t)opcode << 56) |
	       ((uint64_t)(rd & 0x1F) << 51) |
	       ((uint64_t)(rs1 & 0x1F) << 46) |
	       ((uint64_t)(rs2 & 0x1F) << 41) |
	       ((uint64_t)(tile & 0x0F) << 37) |
	       ((uint64_t)(imm & 0xFFF) << 25) |
	       ((uint64_t)(dtype & 0x1F) << 20) |
	       ((uint64_t)(func & 0x1F) << 15);
}

/*
 * Emit a 64-bit AI instruction as two 32-bit .word directives.
 * The low 32 bits use opcode field [6:0] = 0x0B (RISC-V custom-0)
 * as a marker so the simulator can distinguish AI from RV64 instructions.
 *
 * Note: in actual hardware, AI instructions are fetched from the AI
 * lane's fetch port (the upper 64 bits of the 128-bit fetch). In
 * simulation, they're inline in the instruction stream with the
 * custom-0 marker.
 */
#define HETERIS_AI_CUSTOM0_MARKER 0x0B

static inline uint64_t _heteris_ai_encode_sim(
	uint8_t opcode, uint8_t rd, uint8_t rs1, uint8_t rs2,
	uint8_t tile, uint16_t imm, uint8_t dtype, uint8_t func)
{
	uint64_t raw = _heteris_ai_encode(opcode, rd, rs1, rs2,
	                                   tile, imm, dtype, func);
	/* Replace lowest 7 bits with custom-0 marker for simulator detection */
	raw = (raw & ~0x7FULL) | HETERIS_AI_CUSTOM0_MARKER;
	return raw;
}

/* --- Instruction wrappers --- */

static inline void heteris_ai_sync(void)
{
	uint64_t insn = _heteris_ai_encode_sim(0x30, 0, 0, 0, 0, 0, 0, 0);
	uint32_t lo = (uint32_t)insn;
	uint32_t hi = (uint32_t)(insn >> 32);
	__asm__ volatile(
		".word %0\n"
		".word %1\n"
		: : "i"(lo), "i"(hi)
	);
}

/* These can't use "i" constraints because the values aren't compile-time
   constants when parameters are variables. For variable inputs, we store
   the instruction to memory and execute it. For now, provide the encoding
   functions and use MMIO-based tile configuration in arch_init.c instead. */

#endif /* HETERIS_AI_H */
