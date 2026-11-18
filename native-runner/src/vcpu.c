/* Virtual privileged CPU state of the guest. See vcpu.h. */
#include "vcpu.h"

#define IRQ_VECTOR 0x18u
#define FIQ_VECTOR 0x1cu
#define SWI_VECTOR 0x08u

int vcpu_bank(uint32_t mode) {
    switch (mode & VCPU_MODE_MASK) {
    case VCPU_USR: case VCPU_SYS: return VCPU_BANK_USR;
    case VCPU_FIQ: return VCPU_BANK_FIQ;
    case VCPU_IRQ: return VCPU_BANK_IRQ;
    case VCPU_SVC: return VCPU_BANK_SVC;
    case VCPU_ABT: return VCPU_BANK_ABT;
    case VCPU_UND: return VCPU_BANK_UND;
    default: return -1;
    }
}

void vcpu_reset(struct vcpu *v, uint32_t cpsr) {
    struct vcpu zero = {0};
    *v = zero;
    v->mode = cpsr & VCPU_MODE_MASK;
    v->ctl = cpsr & VCPU_CTL_MASK;
}

uint32_t vcpu_cpsr(const struct vcpu *v, const struct arm_cpu *c) {
    return (c->cpsr & VCPU_HOST_MASK) | v->ctl | v->mode;
}

int vcpu_irq_enabled(const struct vcpu *v) {
    return !(v->ctl & VCPU_I);
}

int vcpu_fiq_enabled(const struct vcpu *v) {
    return !(v->ctl & VCPU_F);
}

/* Change the mode: swap the banked registers. The caller checked the mode. */
static void switch_mode(struct vcpu *v, struct arm_cpu *c, uint32_t mode) {
    int from = vcpu_bank(v->mode), to = vcpu_bank(mode);
    if (from != to) {
        if (from == VCPU_BANK_FIQ)
            for (int i = 0; i < 5; i++) { v->fiq_r8[i] = c->r[8 + i]; c->r[8 + i] = v->usr_r8[i]; }
        v->sp[from] = c->r[13];
        v->lr[from] = c->r[14];
        c->r[13] = v->sp[to];
        c->r[14] = v->lr[to];
        if (to == VCPU_BANK_FIQ)
            for (int i = 0; i < 5; i++) { v->usr_r8[i] = c->r[8 + i]; c->r[8 + i] = v->fiq_r8[i]; }
    }
    v->mode = mode & VCPU_MODE_MASK;
}

/* Write the bits of val selected by mask into the virtual CPSR.
 * with_t: also write the T bit (exception return only). */
static int write_cpsr(struct vcpu *v, struct arm_cpu *c, uint32_t val, uint32_t mask, int with_t) {
    if (v->mode == VCPU_USR) mask &= 0xff0f0000u;       /* user mode: flags only */
    if (!with_t) mask &= ~ARM_CPSR_T;
    uint32_t cur = vcpu_cpsr(v, c);
    uint32_t next = (cur & ~mask) | (val & mask);
    if (vcpu_bank(next) < 0) return ARM_EMU_UNDEF;
    c->cpsr = (c->cpsr & ~VCPU_HOST_MASK) | (next & VCPU_HOST_MASK);
    v->ctl = next & VCPU_CTL_MASK;
    switch_mode(v, c, next & VCPU_MODE_MASK);
    return ARM_EMU_OK;
}

/* CPSR := SPSR of the current mode, then make pc fit the new T bit */
static int exception_return(struct vcpu *v, struct arm_cpu *c, uint32_t pc) {
    int bank = vcpu_bank(v->mode);
    if (bank > VCPU_BANK_USR) {                         /* user/system mode has no SPSR */
        int rc = write_cpsr(v, c, v->spsr[bank], 0xffffffffu, 1);
        if (rc) return rc;
    }
    c->r[15] = pc & ((c->cpsr & ARM_CPSR_T) ? ~1u : ~3u);
    return ARM_EMU_OK;
}

static uint32_t field_mask(uint32_t insn) {
    uint32_t m = 0;
    if (insn & (1u << 16)) m |= 0x000000ffu;            /* c */
    if (insn & (1u << 17)) m |= 0x0000ff00u;            /* x */
    if (insn & (1u << 18)) m |= 0x00ff0000u;            /* s */
    if (insn & (1u << 19)) m |= 0xff000000u;            /* f */
    return m;
}

static int emu_mrs(struct vcpu *v, struct arm_cpu *c, uint32_t insn) {
    unsigned rd = (insn >> 12) & 0xf;
    int bank = vcpu_bank(v->mode);
    int spsr = (insn >> 22) & 1;
    if (rd == 15) return ARM_EMU_UNDEF;
    c->r[rd] = (spsr && bank > VCPU_BANK_USR) ? v->spsr[bank] : vcpu_cpsr(v, c);
    c->r[15] += 4;
    return ARM_EMU_OK;
}

static int emu_msr(struct vcpu *v, struct arm_cpu *c, uint32_t insn) {
    uint32_t val = (insn & 0x02000000u) ? arm_operand2(c, insn) : c->r[insn & 0xf];
    uint32_t mask = field_mask(insn);
    if (insn & (1u << 22)) {                            /* SPSR */
        int bank = vcpu_bank(v->mode);
        if (bank > VCPU_BANK_USR) v->spsr[bank] = (v->spsr[bank] & ~mask) | (val & mask);
    } else {
        int rc = write_cpsr(v, c, val, mask, 0);
        if (rc) return rc;
    }
    c->r[15] += 4;
    return ARM_EMU_OK;
}

/* r13/r14 of user mode, for LDM/STM with ^ and no pc */
static uint32_t *user_reg(struct vcpu *v, struct arm_cpu *c, unsigned i) {
    int bank = vcpu_bank(v->mode);
    if (i >= 8 && i <= 12 && bank == VCPU_BANK_FIQ) return &v->usr_r8[i - 8];
    if (i == 13 && bank != VCPU_BANK_USR) return &v->sp[VCPU_BANK_USR];
    if (i == 14 && bank != VCPU_BANK_USR) return &v->lr[VCPU_BANK_USR];
    return &c->r[i];
}

static int emu_multi_s(struct vcpu *v, struct arm_cpu *c, uint32_t insn, const struct arm_bus *bus) {
    unsigned rn = (insn >> 16) & 0xf, list = insn & 0xffff;
    int wback = (insn >> 21) & 1, load = (insn >> 20) & 1;
    uint32_t addr, final, vals[16];
    if (rn == 15 || !list) return ARM_EMU_UNDEF;
    arm_multi_range(insn, c->r[rn], &addr, &final);

    if (load && (list & 0x8000)) {                      /* exception return */
        for (unsigned i = 0, k = 0; i < 16; i++) {
            if (!((list >> i) & 1)) continue;
            if (bus->read(bus->ctx, addr + 4 * k, 4, &vals[i])) return ARM_EMU_BUS;
            k++;
        }
        if (wback) c->r[rn] = final;
        for (unsigned i = 0; i < 15; i++)
            if ((list >> i) & 1) c->r[i] = vals[i];
        return exception_return(v, c, vals[15]);
    }
    /* user-bank transfer: no write-back */
    for (unsigned i = 0, k = 0; i < 16; i++) {
        if (!((list >> i) & 1)) continue;
        uint32_t *r = user_reg(v, c, i);
        if (load) {
            if (bus->read(bus->ctx, addr + 4 * k, 4, r)) return ARM_EMU_BUS;
        } else {
            uint32_t val = i == 15 ? c->r[15] + 12 : *r;
            if (bus->write(bus->ctx, addr + 4 * k, 4, val)) return ARM_EMU_BUS;
        }
        k++;
    }
    c->r[15] += 4;
    return ARM_EMU_OK;
}

/* data-processing instruction with S and rd = pc: pc := result, CPSR := SPSR */
static int emu_dp_s_pc(struct vcpu *v, struct arm_cpu *c, uint32_t insn) {
    unsigned rn = (insn >> 16) & 0xf;
    uint32_t a = rn == 15 ? c->r[15] + 8 : c->r[rn];
    uint32_t b = arm_operand2(c, insn);
    uint32_t carry = (c->cpsr & ARM_CPSR_C) ? 1 : 0, res;
    switch ((insn >> 21) & 0xf) {
    case 0x0: res = a & b; break;
    case 0x1: res = a ^ b; break;
    case 0x2: res = a - b; break;
    case 0x3: res = b - a; break;
    case 0x4: res = a + b; break;
    case 0x5: res = a + b + carry; break;
    case 0x6: res = a - b - !carry; break;
    case 0x7: res = b - a - !carry; break;
    case 0xc: res = a | b; break;
    case 0xd: res = b; break;
    case 0xe: res = a & ~b; break;
    case 0xf: res = ~b; break;
    default: return ARM_EMU_UNDEF;                      /* TST/TEQ/CMP/CMN */
    }
    return exception_return(v, c, res);
}

int vcpu_emu_priv(struct vcpu *v, struct arm_cpu *c, uint32_t insn, const struct arm_bus *bus) {
    enum { MRS, MSR, MULTI_S, DP_S_PC } kind;
    if ((insn >> 28) == 0xf) return ARM_EMU_UNDEF;
    if ((insn & 0x0fbf0fffu) == 0x010f0000u) kind = MRS;
    else if ((insn & 0x0fb0fff0u) == 0x0120f000u || (insn & 0x0fb0f000u) == 0x0320f000u) kind = MSR;
    else if ((insn & 0x0e400000u) == 0x08400000u) kind = MULTI_S;
    else if ((insn & 0x0c10f000u) == 0x0010f000u && (insn & 0x0e000090u) != 0x00000090u) kind = DP_S_PC;
    else return ARM_EMU_UNDEF;
    if (!arm_cond_pass(insn, c->cpsr)) { c->r[15] += 4; return ARM_EMU_OK; }
    switch (kind) {
    case MRS: return emu_mrs(v, c, insn);
    case MSR: return emu_msr(v, c, insn);
    case MULTI_S: return emu_multi_s(v, c, insn, bus);
    default: return emu_dp_s_pc(v, c, insn);
    }
}

/* exception entry: mode, SPSR, lr, mask bits, vector */
static void take_exception(struct vcpu *v, struct arm_cpu *c, uint32_t mode, uint32_t lr, uint32_t vector) {
    uint32_t old = vcpu_cpsr(v, c);
    switch_mode(v, c, mode);
    v->spsr[vcpu_bank(mode)] = old;
    c->r[14] = lr;
    v->ctl |= VCPU_I | VCPU_A;                          /* same bits as the QEMU reference */
    c->cpsr &= ~ARM_CPSR_T;
    c->r[15] = vector;
}

void vcpu_take_irq(struct vcpu *v, struct arm_cpu *c) {
    /* the handler returns with subs pc, lr, #4 */
    take_exception(v, c, VCPU_IRQ, c->r[15] + 4, IRQ_VECTOR);
}

void vcpu_take_fiq(struct vcpu *v, struct arm_cpu *c) {
    take_exception(v, c, VCPU_FIQ, c->r[15] + 4, FIQ_VECTOR);
    v->ctl |= VCPU_F;                                   /* an FIQ masks FIQs too */
}

void vcpu_take_swi(struct vcpu *v, struct arm_cpu *c) {
    take_exception(v, c, VCPU_SVC, c->r[15] + 4, SWI_VECTOR);
}
