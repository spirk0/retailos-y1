/* ARM (ARMv5TE, ARM state) load/store instruction emulator.
 *
 * The runner executes guest code natively. An instruction reaches this
 * emulator only when its data access faults: a device register (MMIO), or
 * guest page 0, which the host cannot map. So only instructions that access
 * memory are implemented. Pure C, no host dependencies.
 */
#ifndef ARMEMU_H
#define ARMEMU_H
#include <stdint.h>

#define ARM_CPSR_N (1u << 31)
#define ARM_CPSR_Z (1u << 30)
#define ARM_CPSR_C (1u << 29)
#define ARM_CPSR_V (1u << 28)
#define ARM_CPSR_T (1u << 5)

/* r[15] is the address of the instruction to emulate. On success it is the
 * address of the next instruction to execute. */
struct arm_cpu {
    uint32_t r[16];
    uint32_t cpsr;
};

/* Guest memory access. size is 1, 2 or 4 bytes. Return 0 on success. */
struct arm_bus {
    int (*read)(void *ctx, uint32_t addr, int size, uint32_t *val);
    int (*write)(void *ctx, uint32_t addr, int size, uint32_t val);
    void *ctx;
};

enum {
    ARM_EMU_OK = 0,
    ARM_EMU_UNDEF = -1,   /* not a load/store instruction this emulator knows */
    ARM_EMU_BUS = -2      /* a bus callback failed */
};

/* 1 if the condition field of insn passes with these flags */
int arm_cond_pass(uint32_t insn, uint32_t cpsr);

/* Operand 2 of a data-processing instruction (immediate, or register with an
 * immediate or register shift). pc reads as r[15] + 8. */
uint32_t arm_operand2(const struct arm_cpu *c, uint32_t insn);

/* Address range of an LDM/STM: *start is the address of the lowest register
 * in the list, *final is the base register value after write-back. */
void arm_multi_range(uint32_t insn, uint32_t base, uint32_t *start, uint32_t *final);

/* Emulate one load/store instruction: LDR/STR/LDRB/STRB, LDRH/STRH/LDRSB/
 * LDRSH, LDRD/STRD, LDM/STM (without ^), SWP/SWPB. A failed condition
 * skips the instruction. Returns ARM_EMU_*. */
int arm_emu_loadstore(struct arm_cpu *c, uint32_t insn, const struct arm_bus *bus);

#endif
