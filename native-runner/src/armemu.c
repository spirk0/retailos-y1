/* ARM load/store instruction emulator. See armemu.h. */
#include "armemu.h"

int arm_cond_pass(uint32_t insn, uint32_t cpsr) {
    int n = (cpsr >> 31) & 1, z = (cpsr >> 30) & 1, c = (cpsr >> 29) & 1, v = (cpsr >> 28) & 1;
    switch (insn >> 28) {
    case 0x0: return z;
    case 0x1: return !z;
    case 0x2: return c;
    case 0x3: return !c;
    case 0x4: return n;
    case 0x5: return !n;
    case 0x6: return v;
    case 0x7: return !v;
    case 0x8: return c && !z;
    case 0x9: return !c || z;
    case 0xa: return n == v;
    case 0xb: return n != v;
    case 0xc: return !z && n == v;
    case 0xd: return z || n != v;
    default:  return 1;          /* AL; 0xf is decoded by the caller */
    }
}

static uint32_t reg(const struct arm_cpu *c, unsigned n) {
    return n == 15 ? c->r[15] + 8 : c->r[n];
}

static uint32_t ror32(uint32_t v, unsigned n) {
    n &= 31;
    return n ? (v >> n) | (v << (32 - n)) : v;
}

/* register shifted by an immediate amount (shift field bits 11..5) */
static uint32_t shift_imm(const struct arm_cpu *c, uint32_t insn) {
    uint32_t rm = reg(c, insn & 0xf);
    unsigned amount = (insn >> 7) & 0x1f;
    switch ((insn >> 5) & 3) {
    case 0: return rm << amount;                                   /* LSL */
    case 1: return amount ? rm >> amount : 0;                      /* LSR #0 means #32 */
    case 2: return amount ? (uint32_t)((int32_t)rm >> amount)      /* ASR #0 means #32 */
                          : (rm & 0x80000000u ? 0xffffffffu : 0);
    default:                                                       /* ROR, or RRX for #0 */
        return amount ? ror32(rm, amount) : (rm >> 1) | ((c->cpsr & ARM_CPSR_C) ? 0x80000000u : 0);
    }
}

uint32_t arm_operand2(const struct arm_cpu *c, uint32_t insn) {
    if (insn & 0x02000000u)                                        /* immediate, rotated */
        return ror32(insn & 0xff, ((insn >> 8) & 0xf) * 2);
    if (!(insn & 0x10))
        return shift_imm(c, insn);
    /* register shifted by a register */
    uint32_t rm = reg(c, insn & 0xf);
    unsigned amount = c->r[(insn >> 8) & 0xf] & 0xff;
    switch ((insn >> 5) & 3) {
    case 0: return amount >= 32 ? 0 : rm << amount;
    case 1: return amount >= 32 ? 0 : rm >> amount;
    case 2: return amount >= 32 ? (rm & 0x80000000u ? 0xffffffffu : 0) : (uint32_t)((int32_t)rm >> amount);
    default: return ror32(rm, amount);
    }
}

/* a value loaded into pc: bit 0 selects Thumb state (ARMv5T) */
static void load_pc(struct arm_cpu *c, uint32_t val) {
    if (val & 1) {
        c->cpsr |= ARM_CPSR_T;
        c->r[15] = val & ~1u;
    } else {
        c->cpsr &= ~ARM_CPSR_T;
        c->r[15] = val & ~3u;
    }
}

/* LDR/STR/LDRB/STRB */
static int emu_single(struct arm_cpu *c, uint32_t insn, const struct arm_bus *bus) {
    unsigned rn = (insn >> 16) & 0xf, rd = (insn >> 12) & 0xf;
    int pre = (insn >> 24) & 1, up = (insn >> 23) & 1, byte = (insn >> 22) & 1;
    int wback = (insn >> 21) & 1, load = (insn >> 20) & 1;
    uint32_t off;
    if (insn & 0x02000000u) {
        if (insn & 0x10) return ARM_EMU_UNDEF;      /* media instruction space */
        off = shift_imm(c, insn);
    } else {
        off = insn & 0xfff;
    }
    uint32_t base = reg(c, rn);
    uint32_t idx = up ? base + off : base - off;
    uint32_t addr = pre ? idx : base;
    uint32_t next = c->r[15] + 4;
    if (load) {
        uint32_t val;
        if (bus->read(bus->ctx, addr, byte ? 1 : 4, &val)) return ARM_EMU_BUS;
        if (byte) val &= 0xff;
        if ((!pre || wback) && rn != 15) c->r[rn] = idx;
        c->r[15] = next;
        if (rd == 15) load_pc(c, val);
        else c->r[rd] = val;                        /* wins over a write-back of the same register */
    } else {
        uint32_t val = rd == 15 ? c->r[15] + 12 : c->r[rd];
        if (bus->write(bus->ctx, addr, byte ? 1 : 4, byte ? val & 0xff : val)) return ARM_EMU_BUS;
        if ((!pre || wback) && rn != 15) c->r[rn] = idx;
        c->r[15] = next;
    }
    return ARM_EMU_OK;
}

/* LDRH/STRH/LDRSB/LDRSH/LDRD/STRD */
static int emu_extra(struct arm_cpu *c, uint32_t insn, const struct arm_bus *bus) {
    unsigned rn = (insn >> 16) & 0xf, rd = (insn >> 12) & 0xf, sh = (insn >> 5) & 3;
    int pre = (insn >> 24) & 1, up = (insn >> 23) & 1, imm = (insn >> 22) & 1;
    int wback = (insn >> 21) & 1, load = (insn >> 20) & 1;
    uint32_t off = imm ? (((insn >> 8) & 0xf) << 4) | (insn & 0xf) : reg(c, insn & 0xf);
    uint32_t base = reg(c, rn);
    uint32_t idx = up ? base + off : base - off;
    uint32_t addr = pre ? idx : base;
    uint32_t next = c->r[15] + 4;
    uint32_t val, val2;

    if (load) {                                     /* LDRH, LDRSB, LDRSH */
        int size = sh == 2 ? 1 : 2;
        if (bus->read(bus->ctx, addr, size, &val)) return ARM_EMU_BUS;
        if (sh == 1) val &= 0xffff;
        else if (sh == 2) val = (uint32_t)(int32_t)(int8_t)val;
        else val = (uint32_t)(int32_t)(int16_t)val;
        if ((!pre || wback) && rn != 15) c->r[rn] = idx;
        c->r[15] = next;
        if (rd != 15) c->r[rd] = val;
        return ARM_EMU_OK;
    }
    if (sh == 1) {                                  /* STRH */
        if (bus->write(bus->ctx, addr, 2, reg(c, rd) & 0xffff)) return ARM_EMU_BUS;
    } else if (sh == 2) {                           /* LDRD: rd, rd+1 */
        if (rd & 1 || rd == 14) return ARM_EMU_UNDEF;
        if (bus->read(bus->ctx, addr, 4, &val) || bus->read(bus->ctx, addr + 4, 4, &val2))
            return ARM_EMU_BUS;
        if ((!pre || wback) && rn != 15) c->r[rn] = idx;
        c->r[rd] = val;
        c->r[rd + 1] = val2;
        c->r[15] = next;
        return ARM_EMU_OK;
    } else {                                        /* STRD */
        if (rd & 1 || rd == 14) return ARM_EMU_UNDEF;
        if (bus->write(bus->ctx, addr, 4, c->r[rd]) || bus->write(bus->ctx, addr + 4, 4, c->r[rd + 1]))
            return ARM_EMU_BUS;
    }
    if ((!pre || wback) && rn != 15) c->r[rn] = idx;
    c->r[15] = next;
    return ARM_EMU_OK;
}

/* SWP/SWPB */
static int emu_swp(struct arm_cpu *c, uint32_t insn, const struct arm_bus *bus) {
    unsigned rn = (insn >> 16) & 0xf, rd = (insn >> 12) & 0xf, rm = insn & 0xf;
    int size = (insn >> 22) & 1 ? 1 : 4;
    uint32_t addr = reg(c, rn), old;
    if (bus->read(bus->ctx, addr, size, &old)) return ARM_EMU_BUS;
    if (bus->write(bus->ctx, addr, size, size == 1 ? reg(c, rm) & 0xff : reg(c, rm))) return ARM_EMU_BUS;
    if (rd != 15) c->r[rd] = size == 1 ? old & 0xff : old;
    c->r[15] += 4;
    return ARM_EMU_OK;
}

void arm_multi_range(uint32_t insn, uint32_t base, uint32_t *start, uint32_t *final) {
    int pre = (insn >> 24) & 1, up = (insn >> 23) & 1;
    unsigned n = 0;
    for (unsigned i = 0; i < 16; i++) n += (insn >> i) & 1;
    /* the lowest register always uses the lowest address */
    *start = up ? base + (pre ? 4 : 0) : base - 4 * n + (pre ? 0 : 4);
    *final = up ? base + 4 * n : base - 4 * n;
}

/* LDM/STM without ^ */
static int emu_multi(struct arm_cpu *c, uint32_t insn, const struct arm_bus *bus) {
    unsigned rn = (insn >> 16) & 0xf, list = insn & 0xffff;
    int wback = (insn >> 21) & 1, load = (insn >> 20) & 1;
    if (insn & 0x00400000u) return ARM_EMU_UNDEF;   /* ^ form: privileged, see vcpu.c */
    if (rn == 15 || !list) return ARM_EMU_UNDEF;
    uint32_t addr, final;
    arm_multi_range(insn, c->r[rn], &addr, &final);
    uint32_t next = c->r[15] + 4, newpc = 0;
    int pc_loaded = 0;

    if (load) {
        uint32_t vals[16];
        for (unsigned i = 0, k = 0; i < 16; i++) {
            if (!((list >> i) & 1)) continue;
            if (bus->read(bus->ctx, addr + 4 * k, 4, &vals[i])) return ARM_EMU_BUS;
            k++;
        }
        if (wback) c->r[rn] = final;
        for (unsigned i = 0; i < 15; i++)
            if ((list >> i) & 1) c->r[i] = vals[i];   /* a loaded base wins over write-back */
        if (list & 0x8000) { pc_loaded = 1; newpc = vals[15]; }
    } else {
        for (unsigned i = 0, k = 0; i < 16; i++) {
            if (!((list >> i) & 1)) continue;
            uint32_t val = i == 15 ? c->r[15] + 12 : c->r[i];
            if (bus->write(bus->ctx, addr + 4 * k, 4, val)) return ARM_EMU_BUS;
            k++;
        }
        if (wback) c->r[rn] = final;
    }
    c->r[15] = next;
    if (pc_loaded) load_pc(c, newpc);
    return ARM_EMU_OK;
}

int arm_emu_loadstore(struct arm_cpu *c, uint32_t insn, const struct arm_bus *bus) {
    if (c->cpsr & ARM_CPSR_T) return ARM_EMU_UNDEF;          /* Thumb: not implemented */
    if ((insn >> 28) == 0xf) return ARM_EMU_UNDEF;           /* unconditional space */
    unsigned cls = (insn >> 25) & 7;
    int known = cls == 2 || cls == 3 || cls == 4 ||
                (cls == 0 && (insn & 0x90) == 0x90);
    if (!known) return ARM_EMU_UNDEF;
    if (cls == 0) {
        unsigned sh = (insn >> 5) & 3;
        if (sh == 0) {
            /* multiplies are not memory instructions; SWP is */
            if ((insn & 0x0fb00ff0u) != 0x01000090u) return ARM_EMU_UNDEF;
            if (!arm_cond_pass(insn, c->cpsr)) { c->r[15] += 4; return ARM_EMU_OK; }
            return emu_swp(c, insn, bus);
        }
        if (!arm_cond_pass(insn, c->cpsr)) { c->r[15] += 4; return ARM_EMU_OK; }
        return emu_extra(c, insn, bus);
    }
    if (!arm_cond_pass(insn, c->cpsr)) { c->r[15] += 4; return ARM_EMU_OK; }
    if (cls == 4) return emu_multi(c, insn, bus);
    return emu_single(c, insn, bus);
}
