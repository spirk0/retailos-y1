/* ARM interpreter for the guest (ARMv5, ARM state). See interp.h.
 *
 * Speed matters here: the runner interprets every IRQ handler and every
 * copy loop to a device. So the common instructions (data processing,
 * LDR/STR, LDRH/STRH, LDM/STM, branches) have inline paths, and a guest RAM
 * access is a host access at the same address when env->direct permits it.
 * Rare instructions use armemu.c and vcpu.c.
 */
#include "interp.h"
#include "privtrap.h"
#include <string.h>

#define PAGE0_END   0x1000u         /* the host never maps guest page 0 */
#define UDF_MASK    0xfff000f0u
#define UDF_BASE    0xe7f000f0u

enum { SLOW_PRIV = 1, SLOW_MEM = 2, SLOW_WRITE = 4 };

static const uint8_t g_no_direct[1u << 16];

uint32_t interp_pc_ring[INTERP_PC_RING];
unsigned interp_pc_ring_i;
int interp_pc_ring_on;

/* bit f of g_cond[cond] is 1 if the condition passes for flags NZCV = f */
static uint16_t g_cond[16];

static void cond_init(void) {
    for (unsigned f = 0; f < 16; f++) {
        int n = (f >> 3) & 1, z = (f >> 2) & 1, c = (f >> 1) & 1, v = f & 1;
        int pass[16] = { z, !z, c, !c, n, !n, v, !v, c && !z, !c || z, n == v, n != v,
                         !z && n == v, z || n != v, 1, 1 };
        for (unsigned k = 0; k < 16; k++) g_cond[k] |= (uint16_t)(pass[k] << f);
    }
}

static inline int cond_pass(uint32_t insn, uint32_t cpsr) {
    return (g_cond[insn >> 28] >> (cpsr >> 28)) & 1;
}

/* The state of one interp_run() call. Its arm_bus goes to armemu.c and
 * vcpu.c, so that their accesses also use the direct path. */
struct ictx {
    struct arm_bus bus;
    const struct interp_env *env;
    const uint8_t *direct;
    struct arm_cpu *c;
    uint32_t pc;                    /* the instruction that executes */
    int slow;
};

/* 1 if the host can access [a, a + size) at the same address */
static inline int is_direct(const uint8_t *direct, uint32_t a, uint32_t size) {
    if (!direct[a >> 16] || a < PAGE0_END) return 0;
    return (a & 0xffffu) <= 0x10000u - size || direct[(a + size - 1) >> 16];
}

static inline uint32_t host_read(uint32_t a, int size) {
    const void *p = (const void *)(uintptr_t)a;
    if (size == 1) return *(const uint8_t *)p;
    if (size == 2) { uint16_t h; memcpy(&h, p, 2); return h; }
    uint32_t w;
    memcpy(&w, p, 4);
    return w;
}

static inline void host_write(uint32_t a, int size, uint32_t v) {
    void *p = (void *)(uintptr_t)a;
    if (size == 1) *(uint8_t *)p = (uint8_t)v;
    else if (size == 2) { uint16_t h = (uint16_t)v; memcpy(p, &h, 2); }
    else memcpy(p, &v, 4);
}

/* A device, page 0 or another address that the host does not map. The
 * bus of the runner reads c->r[15] as the pc of the access. */
static int slow_read(struct ictx *x, uint32_t a, int size, uint32_t *val) {
    uint32_t save = x->c->r[15];
    x->slow |= SLOW_MEM;
    x->c->r[15] = x->pc;
    int rc = x->env->bus->read(x->env->bus->ctx, a, size, val);
    x->c->r[15] = save;
    return rc;
}

static int slow_write(struct ictx *x, uint32_t a, int size, uint32_t val) {
    uint32_t save = x->c->r[15];
    x->slow |= SLOW_MEM | SLOW_WRITE;
    x->c->r[15] = x->pc;
    int rc = x->env->bus->write(x->env->bus->ctx, a, size, val);
    x->c->r[15] = save;
    return rc;
}

static inline int mem_read(struct ictx *x, uint32_t a, int size, uint32_t *val) {
    if (is_direct(x->direct, a, (uint32_t)size)) { *val = host_read(a, size); return 0; }
    return slow_read(x, a, size, val);
}

static inline int mem_write(struct ictx *x, uint32_t a, int size, uint32_t val) {
    if (is_direct(x->direct, a, (uint32_t)size)) { host_write(a, size, val); return 0; }
    return slow_write(x, a, size, val);
}

static int ibus_read(void *ctx, uint32_t a, int size, uint32_t *val) { return mem_read(ctx, a, size, val); }
static int ibus_write(void *ctx, uint32_t a, int size, uint32_t val) { return mem_write(ctx, a, size, val); }

static inline uint32_t ror32(uint32_t v, unsigned n) {
    n &= 31;
    return n ? (v >> n) | (v << (32 - n)) : v;
}

/* register rn, where r15 reads as the instruction address + 8 */
#define RD_REG(c, n, pc) ((n) == 15 ? (pc) + 8 : (c)->r[n])

/* operand 2 of a data-processing instruction, with the shifter carry-out */
static inline uint32_t shifter(const struct arm_cpu *c, uint32_t insn, uint32_t pc, uint32_t *carry) {
    uint32_t cin = (c->cpsr >> 29) & 1;
    if (insn & 0x02000000u) {                               /* rotated immediate */
        unsigned rot = ((insn >> 8) & 0xf) * 2;
        uint32_t v = ror32(insn & 0xff, rot);
        *carry = rot ? v >> 31 : cin;
        return v;
    }
    unsigned m = insn & 0xf, type = (insn >> 5) & 3, amount;
    uint32_t rm = RD_REG(c, m, pc);
    if (!(insn & 0x10)) {                                   /* shift by immediate */
        amount = (insn >> 7) & 0x1f;
        switch (type) {
        case 0:
            if (!amount) { *carry = cin; return rm; }
            *carry = (rm >> (32 - amount)) & 1;
            return rm << amount;
        case 1:                                             /* lsr #0 means #32 */
            if (!amount) { *carry = rm >> 31; return 0; }
            *carry = (rm >> (amount - 1)) & 1;
            return rm >> amount;
        case 2:                                             /* asr #0 means #32 */
            if (!amount) { *carry = rm >> 31; return (uint32_t)((int32_t)rm >> 31); }
            *carry = (rm >> (amount - 1)) & 1;
            return (uint32_t)((int32_t)rm >> amount);
        default:
            if (!amount) { *carry = rm & 1; return (rm >> 1) | (cin << 31); }   /* rrx */
            *carry = (rm >> (amount - 1)) & 1;
            return ror32(rm, amount);
        }
    }
    if (m == 15) rm += 4;                                   /* shift by register: pc reads +12 */
    amount = c->r[(insn >> 8) & 0xf] & 0xff;
    if (amount == 0) { *carry = cin; return rm; }
    switch (type) {
    case 0:
        if (amount < 32) { *carry = (rm >> (32 - amount)) & 1; return rm << amount; }
        *carry = amount == 32 ? rm & 1 : 0;
        return 0;
    case 1:
        if (amount < 32) { *carry = (rm >> (amount - 1)) & 1; return rm >> amount; }
        *carry = amount == 32 ? rm >> 31 : 0;
        return 0;
    case 2:
        if (amount < 32) { *carry = (rm >> (amount - 1)) & 1; return (uint32_t)((int32_t)rm >> amount); }
        *carry = rm >> 31;
        return (uint32_t)((int32_t)rm >> 31);
    default:
        amount &= 31;
        if (amount == 0) { *carry = rm >> 31; return rm; }
        *carry = (rm >> (amount - 1)) & 1;
        return ror32(rm, amount);
    }
}

/* register offset of LDR/STR: shifted by an immediate, no carry-out */
static inline uint32_t shift_imm(const struct arm_cpu *c, uint32_t insn, uint32_t pc) {
    uint32_t rm = RD_REG(c, insn & 0xf, pc);
    unsigned amount = (insn >> 7) & 0x1f;
    switch ((insn >> 5) & 3) {
    case 0: return rm << amount;
    case 1: return amount ? rm >> amount : 0;
    case 2: return (uint32_t)((int32_t)rm >> (amount ? amount : 31));
    default: return amount ? ror32(rm, amount) : (rm >> 1) | (((c->cpsr >> 29) & 1) << 31);
    }
}

/* a value loaded into pc: bit 0 selects Thumb state (ARMv5T) */
static inline void load_pc(struct arm_cpu *c, uint32_t val) {
    if (val & 1) { c->cpsr |= ARM_CPSR_T; c->r[15] = val & ~1u; }
    else { c->cpsr &= ~ARM_CPSR_T; c->r[15] = val & ~3u; }
}

static inline void set_nz(struct arm_cpu *c, uint32_t res) {
    c->cpsr = (c->cpsr & 0x3fffffffu) | (res & ARM_CPSR_N) | (res ? 0 : ARM_CPSR_Z);
}

static inline void set_nzcv(struct arm_cpu *c, uint32_t res, uint32_t carry, uint32_t overflow) {
    c->cpsr = (c->cpsr & 0x0fffffffu) | (res & ARM_CPSR_N) | (res ? 0 : ARM_CPSR_Z) |
              (carry << 29) | (overflow << 28);
}

/* a + b + cin, with the flags if s */
static inline uint32_t adc_flags(struct arm_cpu *c, uint32_t a, uint32_t b, uint32_t cin, int s) {
    uint64_t wide = (uint64_t)a + b + cin;
    uint32_t res = (uint32_t)wide;
    if (s) set_nzcv(c, res, (uint32_t)(wide >> 32), ((~(a ^ b) & (a ^ res)) >> 31) & 1);
    return res;
}

static int priv(struct ictx *x, struct vcpu *v, uint32_t insn) {
    x->slow |= SLOW_PRIV;
    return vcpu_emu_priv(v, x->c, insn, &x->bus) == ARM_EMU_OK ? INTERP_OK : INTERP_ERROR;
}

static int via_armemu(struct ictx *x, uint32_t insn) {
    int rc = arm_emu_loadstore(x->c, insn, &x->bus);
    return rc == ARM_EMU_OK ? INTERP_OK : rc == ARM_EMU_UNDEF ? INTERP_STOP : INTERP_ERROR;
}

static int exec_dp(struct ictx *x, struct vcpu *v, uint32_t insn) {
    struct arm_cpu *c = x->c;
    uint32_t pc = x->pc;
    unsigned op = (insn >> 21) & 0xf, rd = (insn >> 12) & 0xf, rn = (insn >> 16) & 0xf;
    int s = (insn >> 20) & 1;
    uint32_t carry, res;
    if (s && rd == 15) return priv(x, v, insn);             /* exception return */
    uint32_t a = RD_REG(c, rn, pc);
    if (rn == 15 && (insn & 0x02000010u) == 0x10) a += 4;   /* shift by register: pc reads +12 */
    uint32_t b = shifter(c, insn, pc, &carry);
    switch (op) {
    case 0x0: case 0x8: res = a & b; break;                 /* AND, TST */
    case 0x1: case 0x9: res = a ^ b; break;                 /* EOR, TEQ */
    case 0x2: case 0xa: res = adc_flags(c, a, ~b, 1, s); goto arith;    /* SUB, CMP */
    case 0x3: res = adc_flags(c, b, ~a, 1, s); goto arith;             /* RSB */
    case 0x4: case 0xb: res = adc_flags(c, a, b, 0, s); goto arith;     /* ADD, CMN */
    case 0x5: res = adc_flags(c, a, b, (c->cpsr >> 29) & 1, s); goto arith;   /* ADC */
    case 0x6: res = adc_flags(c, a, ~b, (c->cpsr >> 29) & 1, s); goto arith;  /* SBC */
    case 0x7: res = adc_flags(c, b, ~a, (c->cpsr >> 29) & 1, s); goto arith;  /* RSC */
    case 0xc: res = a | b; break;
    case 0xd: res = b; break;
    case 0xe: res = a & ~b; break;
    default:  res = ~b; break;
    }
    if (s) c->cpsr = (c->cpsr & 0x1fffffffu) | (res & ARM_CPSR_N) | (res ? 0 : ARM_CPSR_Z) | (carry << 29);
arith:
    if (op >= 0x8 && op <= 0xb) { c->r[15] = pc + 4; return INTERP_OK; }   /* tests write no register */
    if (rd == 15) { c->r[15] = res & ~3u; return INTERP_OK; }             /* ARMv5: no interworking here */
    c->r[rd] = res;
    c->r[15] = pc + 4;
    return INTERP_OK;
}

static int exec_multiply(struct arm_cpu *c, uint32_t insn, uint32_t pc) {
    unsigned rd = (insn >> 16) & 0xf, rn = (insn >> 12) & 0xf, rs = (insn >> 8) & 0xf, rm = insn & 0xf;
    int s = (insn >> 20) & 1;
    if ((insn & 0x0f800000u) == 0x00000000u) {              /* MUL, MLA */
        uint32_t res = c->r[rm] * c->r[rs] + ((insn & 0x00200000u) ? c->r[rn] : 0);
        c->r[rd] = res;
        if (s) set_nz(c, res);
    } else {                                                /* UMULL, UMLAL, SMULL, SMLAL */
        uint64_t res;
        if (insn & 0x00400000u) res = (uint64_t)((int64_t)(int32_t)c->r[rm] * (int32_t)c->r[rs]);
        else res = (uint64_t)c->r[rm] * c->r[rs];
        if (insn & 0x00200000u) res += ((uint64_t)c->r[rd] << 32) | c->r[rn];
        c->r[rn] = (uint32_t)res;                           /* RdLo */
        c->r[rd] = (uint32_t)(res >> 32);                   /* RdHi */
        if (s) c->cpsr = (c->cpsr & ~(ARM_CPSR_N | ARM_CPSR_Z)) | ((uint32_t)(res >> 32) & ARM_CPSR_N) |
                         (res ? 0 : ARM_CPSR_Z);
    }
    c->r[15] = pc + 4;
    return INTERP_OK;
}

/* LDRH/STRH/LDRSB/LDRSH. LDRD, STRD and SWP go to armemu.c. */
static int exec_extra(struct ictx *x, uint32_t insn) {
    struct arm_cpu *c = x->c;
    uint32_t pc = x->pc;
    unsigned rn = (insn >> 16) & 0xf, rd = (insn >> 12) & 0xf, sh = (insn >> 5) & 3;
    int load = (insn >> 20) & 1;
    if (sh == 0 || (!load && sh != 1)) return via_armemu(x, insn);
    uint32_t off = (insn & 0x00400000u) ? (((insn >> 8) & 0xf) << 4) | (insn & 0xf) : RD_REG(c, insn & 0xf, pc);
    uint32_t base = RD_REG(c, rn, pc);
    uint32_t idx = (insn & 0x00800000u) ? base + off : base - off;
    uint32_t addr = (insn & 0x01000000u) ? idx : base;
    int wback = !(insn & 0x01000000u) || (insn & 0x00200000u);
    uint32_t val;
    if (load) {
        int size = sh == 2 ? 1 : 2;
        if (mem_read(x, addr, size, &val)) return INTERP_ERROR;
        if (sh == 2) val = (uint32_t)(int32_t)(int8_t)val;
        else if (sh == 3) val = (uint32_t)(int32_t)(int16_t)val;
        if (wback && rn != 15) c->r[rn] = idx;
        c->r[15] = pc + 4;
        if (rd != 15) c->r[rd] = val;
        return INTERP_OK;
    }
    if (mem_write(x, addr, 2, RD_REG(c, rd, pc) & 0xffff)) return INTERP_ERROR;
    if (wback && rn != 15) c->r[rn] = idx;
    c->r[15] = pc + 4;
    return INTERP_OK;
}

/* LDR/STR/LDRB/STRB */
static int exec_single(struct ictx *x, uint32_t insn) {
    struct arm_cpu *c = x->c;
    uint32_t pc = x->pc;
    unsigned rn = (insn >> 16) & 0xf, rd = (insn >> 12) & 0xf;
    int byte = (insn >> 22) & 1, size = byte ? 1 : 4;
    uint32_t off = (insn & 0x02000000u) ? shift_imm(c, insn, pc) : insn & 0xfff;
    uint32_t base = RD_REG(c, rn, pc);
    uint32_t idx = (insn & 0x00800000u) ? base + off : base - off;
    uint32_t addr = (insn & 0x01000000u) ? idx : base;
    int wback = !(insn & 0x01000000u) || (insn & 0x00200000u);
    uint32_t val;
    if (insn & 0x00100000u) {
        if (mem_read(x, addr, size, &val)) return INTERP_ERROR;
        if (wback && rn != 15) c->r[rn] = idx;
        c->r[15] = pc + 4;
        if (rd == 15) load_pc(c, val);
        else c->r[rd] = val;                                /* wins over a write-back of the same register */
        return INTERP_OK;
    }
    val = rd == 15 ? pc + 12 : c->r[rd];
    if (mem_write(x, addr, size, byte ? val & 0xff : val)) return INTERP_ERROR;
    if (wback && rn != 15) c->r[rn] = idx;
    c->r[15] = pc + 4;
    return INTERP_OK;
}

/* LDM/STM without ^, when the whole range is direct. Else armemu.c. */
static int exec_multi(struct ictx *x, uint32_t insn) {
    struct arm_cpu *c = x->c;
    uint32_t pc = x->pc;
    unsigned rn = (insn >> 16) & 0xf, list = insn & 0xffff;
    if (rn == 15 || !list) return via_armemu(x, insn);
    uint32_t n = list - ((list >> 1) & 0x5555u);        /* bit count, without a library call */
    n = (n & 0x3333u) + ((n >> 2) & 0x3333u);
    n = (n + (n >> 4)) & 0x0f0fu;
    n = (n + (n >> 8)) & 0x1fu;
    uint32_t base = c->r[rn], start, final;
    int pre = (insn >> 24) & 1;
    if (insn & 0x00800000u) { start = base + (pre ? 4 : 0); final = base + 4 * n; }
    else { start = base - 4 * n + (pre ? 0 : 4); final = base - 4 * n; }
    if (!is_direct(x->direct, start, 4 * n)) return via_armemu(x, insn);
    const uint32_t *p = (const uint32_t *)(uintptr_t)start;
    int wback = (insn >> 21) & 1;
    if (insn & 0x00100000u) {
        uint32_t newpc = 0;
        if (wback) c->r[rn] = final;                        /* a loaded base wins over write-back */
        for (unsigned i = 0; i < 15; i++)
            if ((list >> i) & 1) memcpy(&c->r[i], p++, 4);
        if (list & 0x8000) memcpy(&newpc, p, 4);
        c->r[15] = pc + 4;
        if (list & 0x8000) load_pc(c, newpc);
        return INTERP_OK;
    }
    uint32_t *q = (uint32_t *)(uintptr_t)start;
    for (unsigned i = 0; i < 15; i++)
        if ((list >> i) & 1) memcpy(q++, &c->r[i], 4);
    if (list & 0x8000) { uint32_t w = pc + 12; memcpy(q, &w, 4); }
    if (wback) c->r[rn] = final;
    c->r[15] = pc + 4;
    return INTERP_OK;
}

static void branch_exchange(struct arm_cpu *c, uint32_t target) {
    if (target & 1) { c->cpsr |= ARM_CPSR_T; c->r[15] = target & ~1u; }
    else { c->cpsr &= ~ARM_CPSR_T; c->r[15] = target & ~3u; }
}

/* BX, BLX, CLZ, MRS/MSR, DSP extensions */
static int exec_misc(struct ictx *x, struct vcpu *v, uint32_t insn) {
    struct arm_cpu *c = x->c;
    uint32_t pc = x->pc;
    if ((insn & 0x0ffffff0u) == 0x012fff10u) { branch_exchange(c, c->r[insn & 0xf]); return INTERP_OK; }
    if ((insn & 0x0ffffff0u) == 0x012fff30u) {              /* BLX register */
        uint32_t t = c->r[insn & 0xf];
        c->r[14] = pc + 4;
        branch_exchange(c, t);
        return INTERP_OK;
    }
    if ((insn & 0x0fff0ff0u) == 0x016f0f10u) {              /* CLZ */
        uint32_t m = c->r[insn & 0xf];
        c->r[(insn >> 12) & 0xf] = m ? (uint32_t)__builtin_clz(m) : 32;
        c->r[15] = pc + 4;
        return INTERP_OK;
    }
    if ((insn & 0x0fb000f0u) == 0x01000000u || (insn & 0x0fb000f0u) == 0x01200000u)
        return priv(x, v, insn);                            /* an MRS/MSR that the loader did not patch */
    return INTERP_STOP;
}

static int exec(struct ictx *x, struct vcpu *v, uint32_t insn) {
    struct arm_cpu *c = x->c;
    uint32_t pc = x->pc;
    uint32_t cond = insn >> 28;
    if (cond == 0xf) {
        if ((insn & 0xfe000000u) == 0xfa000000u) {          /* BLX immediate: to Thumb */
            int32_t off = (int32_t)(insn << 8) >> 6;
            c->r[14] = pc + 4;
            c->r[15] = pc + 8 + (uint32_t)off + ((insn >> 23) & 2);
            c->cpsr |= ARM_CPSR_T;
            return INTERP_OK;
        }
        return INTERP_STOP;                                 /* PLD and others: let the host do it */
    }
    if (cond != 0xe && !cond_pass(insn, c->cpsr)) {
        c->r[15] = pc + 4;
        return INTERP_OK;
    }

    switch ((insn >> 25) & 7) {
    case 0:
        if ((insn & 0x90) == 0x90) {
            if ((insn & 0x0f0000f0u) == 0x00000090u) return exec_multiply(c, insn, pc);
            return exec_extra(x, insn);
        }
        if ((insn & 0x01900000u) == 0x01000000u) return exec_misc(x, v, insn);
        return exec_dp(x, v, insn);
    case 1:
        if ((insn & 0x01900000u) == 0x01000000u) {
            if (insn & 0x00200000u) return priv(x, v, insn);    /* MSR immediate */
            return INTERP_STOP;                             /* undefined space */
        }
        return exec_dp(x, v, insn);
    case 2:
        return exec_single(x, insn);
    case 3:
        if (insn & 0x10) {
            uint32_t orig = (insn & UDF_MASK) == UDF_BASE ? priv_decode(insn) : 0;
            if (!orig) return INTERP_ERROR;                 /* undefined instruction */
            x->slow |= SLOW_PRIV;
            if (!cond_pass(orig, c->cpsr)) { c->r[15] = pc + 4; return INTERP_OK; }
            if (orig == PRIV_SEMIHOST) { x->env->semihost(x->env->ctx, c); return INTERP_OK; }
            return priv(x, v, orig);
        }
        return exec_single(x, insn);
    case 4:
        if (insn & 0x00400000u) return priv(x, v, insn);   /* LDM/STM with ^ */
        return exec_multi(x, insn);
    case 5: {                                               /* B, BL */
        int32_t off = (int32_t)(insn << 8) >> 6;
        if (insn & 0x01000000u) c->r[14] = pc + 4;
        c->r[15] = pc + 8 + (uint32_t)off;
        return INTERP_OK;
    }
    case 6:
        return INTERP_STOP;                                 /* coprocessor load/store */
    default:
        if ((insn & 0x0f000000u) == 0x0f000000u) {          /* SWI */
            x->slow |= SLOW_PRIV;
            vcpu_take_swi(v, c);
            return INTERP_OK;
        }
        return INTERP_STOP;                                 /* coprocessor */
    }
}

int interp_run(struct vcpu *v, struct arm_cpu *c, const struct interp_env *env, struct interp_run *run) {
    struct ictx x = { { ibus_read, ibus_write, 0 }, env, env->direct ? env->direct : g_no_direct, c, 0, 0 };
    uint32_t n = 0, idle = run->idle;
    int rc = INTERP_OK;
    x.bus.ctx = &x;
    if (!g_cond[0xe]) cond_init();
    run->stop = 0;
    while (n < run->budget) {
        uint32_t pc = c->r[15], insn;
        if (c->cpsr & ARM_CPSR_T) { rc = INTERP_STOP; break; }  /* Thumb is not interpreted */
        if (is_direct(x.direct, pc, 4)) insn = *(const uint32_t *)(uintptr_t)pc;
        else if (!env->fetch(env->ctx, pc, &insn)) { rc = INTERP_STOP; break; }
        x.pc = pc;
        x.slow = 0;
        if (interp_pc_ring_on) interp_pc_ring[interp_pc_ring_i++ % INTERP_PC_RING] = pc | (v->mode == VCPU_IRQ);
        rc = exec(&x, v, insn);
        if (rc != INTERP_OK) { c->r[15] = pc; break; }
        n++;
        if (x.slow) {
            idle = 0;
            /* a device write can stop the core, an MSR can unmask an IRQ */
            if (((x.slow & SLOW_PRIV) && (!env->irq_ready || env->irq_ready(env->ctx))) ||
                ((x.slow & SLOW_WRITE) && (!env->need_stop || env->need_stop(env->ctx)))) {
                run->stop = 1;
                break;
            }
        } else if (run->idle_limit && ++idle >= run->idle_limit) {
            run->stop = 1;
            break;
        }
    }
    run->n = n;
    run->idle = idle;
    run->last_slow = x.slow;
    return rc;
}

int interp_step(struct vcpu *v, struct arm_cpu *c, const struct interp_env *env, int *slow) {
    struct interp_run run = { 1, 0, 0, 0, 0, 0 };
    int rc = interp_run(v, c, env, &run);
    *slow = (run.last_slow & SLOW_PRIV) ? 1 : 0;
    return rc;
}
