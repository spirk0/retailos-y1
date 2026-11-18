/* Virtual privileged CPU state of the guest.
 *
 * The guest runs natively in a host user-mode process. There the host CPU
 * ignores MSR CPSR_c, shows the host mode on MRS, and has no banked
 * registers or SPSR. This module keeps that state for the guest: the mode,
 * the interrupt mask bits, the banked r13/r14 (r8-r12 for FIQ) and the SPSR
 * of each mode. The runner calls it for each trapped privileged instruction
 * (see privtrap.h) and for exception entry.
 *
 * The condition flags (NZCVQ), GE and the T bit stay in the host CPSR
 * (struct arm_cpu.cpsr). Pure C.
 */
#ifndef VCPU_H
#define VCPU_H
#include "armemu.h"

#define VCPU_MODE_MASK 0x0000001fu
#define VCPU_F         0x00000040u
#define VCPU_I         0x00000080u
#define VCPU_A         0x00000100u
#define VCPU_CTL_MASK  (VCPU_A | VCPU_I | VCPU_F)   /* kept here, not in the host CPSR */
#define VCPU_HOST_MASK 0xf80f0020u                   /* NZCVQ, GE, T: kept in the host CPSR */

enum {
    VCPU_USR = 0x10, VCPU_FIQ = 0x11, VCPU_IRQ = 0x12, VCPU_SVC = 0x13,
    VCPU_ABT = 0x17, VCPU_UND = 0x1b, VCPU_SYS = 0x1f
};
enum {
    VCPU_BANK_USR, VCPU_BANK_FIQ, VCPU_BANK_IRQ, VCPU_BANK_SVC,
    VCPU_BANK_ABT, VCPU_BANK_UND, VCPU_BANKS
};

struct vcpu {
    uint32_t mode;                /* current mode, 5 bits */
    uint32_t ctl;                 /* A, I, F bits of the virtual CPSR */
    uint32_t spsr[VCPU_BANKS];    /* [VCPU_BANK_USR] is not used */
    uint32_t sp[VCPU_BANKS];      /* r13 of each mode that is not the current mode */
    uint32_t lr[VCPU_BANKS];      /* r14, same rule */
    uint32_t fiq_r8[5];           /* r8-r12 of FIQ mode while the mode is not FIQ */
    uint32_t usr_r8[5];           /* r8-r12 of the other modes while the mode is FIQ */
};

/* Bank index (VCPU_BANK_*) of a mode, or -1 if the mode value is not valid */
int vcpu_bank(uint32_t mode);

/* Start in the mode and mask bits of cpsr. Banks and SPSRs are zero. */
void vcpu_reset(struct vcpu *v, uint32_t cpsr);

/* The CPSR value the guest sees */
uint32_t vcpu_cpsr(const struct vcpu *v, const struct arm_cpu *c);

/* 1 if the virtual CPSR lets an IRQ in */
int vcpu_irq_enabled(const struct vcpu *v);

/* 1 if the virtual CPSR lets an FIQ in */
int vcpu_fiq_enabled(const struct vcpu *v);

/* Emulate one privileged instruction: MRS, MSR, LDM/STM with ^, or a
 * data-processing instruction with S and rd = pc. A failed condition skips
 * it. Returns ARM_EMU_OK, ARM_EMU_UNDEF (not such an instruction, or an
 * invalid mode value) or ARM_EMU_BUS. */
int vcpu_emu_priv(struct vcpu *v, struct arm_cpu *c, uint32_t insn, const struct arm_bus *bus);

/* IRQ exception entry: c->r[15] must be the address of the next instruction
 * to execute. After the call it is the IRQ vector (0x18). */
void vcpu_take_irq(struct vcpu *v, struct arm_cpu *c);

/* FIQ exception entry: as vcpu_take_irq, but FIQ mode (banked r8-r12),
 * vector 0x1c, and F is set as well as I. */
void vcpu_take_fiq(struct vcpu *v, struct arm_cpu *c);

/* SWI exception entry: c->r[15] is the address of the SWI instruction.
 * After the call it is the SWI vector (0x08), in SVC mode. */
void vcpu_take_swi(struct vcpu *v, struct arm_cpu *c);

#endif
