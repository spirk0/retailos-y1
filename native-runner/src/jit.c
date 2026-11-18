/* Template JIT for interpreted guest code. See jit.h.
 *
 * Host register use in translated code:
 *   r11  struct arm_cpu of the guest (r[0..15] at 0..60, cpsr at 64)
 *   r10  struct jit_ctx (the direct table, the epoch, the budget)
 *   r9   the guest flags (NZCVQ in bits 31..27). The host APSR holds them
 *        only where the translator knows it ("flags live").
 *   r8   the budget: a chained jump stops when r7 reaches it
 *   r7   guest instructions executed by the chained blocks before this one
 *   r4-r6  base, index and final address of a load or store (they survive
 *        a helper call)
 *   r0-r3, r12  scratch
 * An exit returns the number of guest instructions that its block executed,
 * and EXIT_* bits. The epilogue adds r7, the count of the chained blocks
 * before: EXIT_COUNT must hold the budget plus one block. The exit stores
 * the next pc in cpu->r[15].
 *
 * Chaining: a static exit (a branch, the next instruction) jumps straight
 * to the next block, once jit_run() patched it. The target block starts
 * with a check of its epoch word: jit_run() validates a block (compares its
 * guest words) once per epoch, and the runner starts a new epoch at each
 * handler entry, because native guest code can change code between them.
 * Block layout: epoch word, chain entry (the check), entry (the body).
 *
 * Indirect jumps (a function return, ldr pc, bx) to an ARM address look in
 * the indirect-branch cache: {guest pc, chain entry} by the pc bits 2..13,
 * filled by jit_run() after each lookup. A hit jumps to the chain entry as
 * a chained exit does (the budget check, then the epoch check of the
 * target). Without it, each return went back to jit_run(): one entry per
 * 33 guest instructions during playback.
 */
#define _GNU_SOURCE
#include "jit.h"
#include "hostprof.h"
#include "privtrap.h"
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

#define CODE_SIZE       (4u << 20)
#define WORDS_SIZE      (1u << 18)          /* guest words of all blocks */
#define TABLE_SIZE      65536u              /* a flush comes before it fills: at most ~20% used */
#define PROBES          8
#define BLOCK_SPACE     (64u * 1024u)       /* bytes that one block can use at most */
#define PAGE0_END       0x1000u
#define IBC_BITS        12                  /* indirect-branch cache: 4096 entries */
#define IBC_SIZE        (1u << IBC_BITS)

#define EXIT_COUNT      0xffffu             /* instructions: see MAX_ENTRY_BUDGET */
#define EXIT_INTERWORK  0x10000u            /* cpu->r[15] is a loaded value: bit 0 selects Thumb */
#define EXIT_LINK       0x20000u            /* jit_ctx.link_site wants the block of cpu->r[15] */
#define EXIT_MARK       0x40000u            /* a helper's exit: never 0 */
/* the budget of one entry: the count of a chain, plus the last block, plus a
 * helper's index, must fit EXIT_COUNT */
#define MAX_ENTRY_BUDGET (EXIT_COUNT - 2u * JIT_MAX_BLOCK)

/* g_flags: what happened in the last block */
#define JF_SLOW         1u                  /* a slow access or instruction: the idle count starts again */
#define JF_STOP         2u                  /* a device write or a privileged instruction */
#define JF_RC           4u                  /* an instruction could not execute: g_rc */

/* host registers */
enum { R0, R1, R2, R3, R4, R5, R6, R7, R8, R9, R10, R11, R12 };

/* r10 points here: the offsets are in the generated code */
struct jit_ctx {
    const uint8_t *direct;      /* 0 */
    uint32_t epoch;             /* 4 */
    uint32_t budget;            /* 8: instructions for this entry */
    uint32_t *link_site;        /* 12: the branch of an EXIT_LINK exit */
    const struct ibc_entry *ibc;    /* 16: the indirect-branch cache */
};

struct ibc_entry {
    uint32_t pc;                /* 0 */
    uint32_t *chain;            /* 4: the chain entry of the block of pc */
};
static struct ibc_entry g_ibc[IBC_SIZE];

struct block {
    uint32_t pc;
    uint32_t n;                 /* guest words */
    uint32_t *words;            /* copy of the guest words */
    uint32_t *code;             /* the epoch word */
    uint32_t *chain;            /* the chain entry: the epoch check, or the body for static code */
    uint32_t *entry;            /* the body */
    int is_static;              /* inside a range of jit_static_code(): never compared again */
};

static struct { uint32_t lo, hi; } g_static[JIT_MAX_STATIC];
static int g_n_static;

void jit_static_code(uint32_t lo, uint32_t hi) {
    if (g_n_static < JIT_MAX_STATIC && lo < hi) {
        g_static[g_n_static].lo = lo;
        g_static[g_n_static].hi = hi;
        g_n_static++;
    }
}

/* 1 if the guest words [pc, pc + 4n) are inside one static range */
static int in_static(uint32_t pc, uint32_t n) {
    for (int i = 0; i < g_n_static; i++)
        if (pc >= g_static[i].lo && pc - g_static[i].lo + 4 * n <= g_static[i].hi - g_static[i].lo) return 1;
    return 0;
}

static uint32_t *g_code, *g_code_end, *g_code_free;
static uint32_t *g_words, *g_words_free;
static struct block g_table[TABLE_SIZE];
static uint32_t *g_epilogue;
static uint32_t (*g_enter)(struct arm_cpu *c, const uint32_t *code, struct jit_ctx *ctx);
static struct jit_ctx g_ctx;
static uint32_t g_epoch = 1;

static struct vcpu *g_v;
static const struct interp_env *g_env;
static uint32_t g_flags;
static int g_rc;

uint64_t jit_n_translated, jit_n_blocks, jit_n_flushes, jit_n_stale, jit_n_collide;
uint64_t jit_n_step, jit_n_read, jit_n_write, jit_n_link, jit_n_validate, jit_n_priv;

/* ---------- helpers that translated code calls ---------- */

/* a load that is not direct: kind is the size, | 0x10 for sign extension */
static uint32_t h_read(struct arm_cpu *c, uint32_t addr, uint32_t kind) {
    uint32_t val = 0, size = kind & 7;
    (void)c;
    jit_n_read++;
    g_flags |= JF_SLOW;
    g_env->bus->read(g_env->bus->ctx, addr, (int)size, &val);
    if (size == 1) val = (kind & 0x10) ? (uint32_t)(int32_t)(int8_t)val : val & 0xff;
    else if (size == 2) val = (kind & 0x10) ? (uint32_t)(int32_t)(int16_t)val : val & 0xffff;
    return val;
}

/* a store that is not direct: returns 1 if the block must stop */
static uint32_t h_write(struct arm_cpu *c, uint32_t addr, uint32_t size, uint32_t val) {
    (void)c;
    jit_n_write++;
    g_flags |= JF_SLOW;
    g_env->bus->write(g_env->bus->ctx, addr, (int)size, size == 4 ? val : val & ((1u << (8 * size)) - 1));
    if (g_env->need_stop && !g_env->need_stop(g_env->ctx)) return 0;
    g_flags |= JF_STOP;
    return 1;
}

/* Interpret the instruction at c->r[15]. Returns 0 to continue the block,
 * else the exit code of the block. */
static uint32_t h_step(struct arm_cpu *c, uint32_t flags, uint32_t idx) {
    uint32_t pc = c->r[15];
    struct interp_run one;
    jit_n_step++;
    memset(&one, 0, sizeof one);
    one.budget = 1;
    c->cpsr = (c->cpsr & 0x07ffffffu) | (flags & 0xf8000000u);
    int rc = interp_run(g_v, c, g_env, &one);
    if (rc != INTERP_OK) {
        g_flags |= JF_RC;
        g_rc = rc;
        c->r[15] = pc;
        return idx | EXIT_MARK;
    }
    if (one.last_slow) g_flags |= JF_SLOW;
    if (one.stop) g_flags |= JF_STOP;
    if (one.stop || c->r[15] != pc + 4 || (c->cpsr & ARM_CPSR_T)) return (idx + 1) | EXIT_MARK;
    return 0;
}

/* A privileged instruction (orig: the original of a trap word, or an
 * unpatched MRS/MSR) at c->r[15]. Returns 0 to continue the block, else the
 * exit code of the block. */
static uint32_t h_priv(struct arm_cpu *c, uint32_t flags, uint32_t idx, uint32_t orig) {
    uint32_t pc = c->r[15];
    jit_n_priv++;
    c->cpsr = (c->cpsr & 0x07ffffffu) | (flags & 0xf8000000u);
    g_flags |= JF_SLOW;
    if (vcpu_emu_priv(g_v, c, orig, g_env->bus) != ARM_EMU_OK) {
        g_flags |= JF_RC;
        g_rc = INTERP_ERROR;
        c->r[15] = pc;
        return idx | EXIT_MARK;
    }
    int stop = !g_env->irq_ready || g_env->irq_ready(g_env->ctx);
    if (stop) g_flags |= JF_STOP;
    if (stop || c->r[15] != pc + 4 || (c->cpsr & ARM_CPSR_T)) return (idx + 1) | EXIT_MARK;
    return 0;
}

/* ---------- code emission ---------- */

struct tr {
    uint32_t *p;
    int flags_live;             /* the host APSR holds the guest flags */
    int idx;                    /* guest instruction in the block */
};

#define MAX_FIX 8
struct fixups { uint32_t *at[MAX_FIX]; int n; };

static void emit(struct tr *t, uint32_t w) { *t->p++ = w; }

static uint32_t b_insn(uint32_t cond, const uint32_t *at, const uint32_t *target) {
    int32_t off = (int32_t)((const uint8_t *)target - ((const uint8_t *)at + 8)) >> 2;
    return (cond << 28) | 0x0a000000u | ((uint32_t)off & 0x00ffffffu);
}

static void emit_b(struct tr *t, uint32_t cond, const uint32_t *target) {
    *t->p = b_insn(cond, t->p, target);
    t->p++;
}

/* a forward branch: the target is set by fix_here() */
static void emit_fwd(struct tr *t, struct fixups *f, uint32_t cond) {
    f->at[f->n++] = t->p;
    emit(t, cond << 28);                                /* the condition, for fix_here() */
}

static void fix_here(struct tr *t, struct fixups *f) {
    for (int i = 0; i < f->n; i++) *f->at[i] = b_insn(*f->at[i] >> 28, f->at[i], t->p);
    f->n = 0;
}

/* movw, and movt if the top half is not 0 (movw clears it) */
static void emit_const(struct tr *t, unsigned rd, uint32_t v) {
    emit(t, 0xe3000000u | ((v >> 12) & 0xf) << 16 | rd << 12 | (v & 0xfff));            /* movw */
    if (v >> 16) emit(t, 0xe3400000u | ((v >> 28) & 0xf) << 16 | rd << 12 | ((v >> 16) & 0xfff));  /* movt */
}

static void emit_ldr_cpu(struct tr *t, unsigned rd, unsigned off) { emit(t, 0xe59b0000u | rd << 12 | off); }
static void emit_str_cpu(struct tr *t, unsigned rd, unsigned off) { emit(t, 0xe58b0000u | rd << 12 | off); }
static void emit_mov(struct tr *t, unsigned rd, unsigned rm) { emit(t, 0xe1a00000u | rd << 12 | rm); }

/* guest register g into host register rd; r15 reads as pc + plus */
static void emit_get(struct tr *t, unsigned rd, unsigned g, uint32_t pc, uint32_t plus) {
    if (g == 15) emit_const(t, rd, pc + plus);
    else emit_ldr_cpu(t, rd, 4 * g);
}

static void emit_put(struct tr *t, unsigned rs, unsigned g) { emit_str_cpu(t, rs, 4 * g); }

static void emit_flags_in(struct tr *t) {
    if (!t->flags_live) emit(t, 0xe128f009u);           /* msr APSR_nzcvq, r9 */
    t->flags_live = 1;
}

static void emit_flags_out(struct tr *t) { emit(t, 0xe10f9000u); }     /* mrs r9, APSR */

static void emit_call(struct tr *t, const void *fn) {
    emit_const(t, R12, (uint32_t)(uintptr_t)fn);
    emit(t, 0xe12fff3cu);                               /* blx r12 */
    t->flags_live = 0;
}

/* exit with a known next pc, after count more instructions: a chain site.
 * The branch at "site" goes to the link request until jit_run() patches it
 * to the chain entry of the next block. */
static void emit_exit(struct tr *t, uint32_t next_pc, uint32_t count) {
    struct fixups budget = { { 0 }, 0 }, store = { { 0 }, 0 };
    emit(t, 0xe2877000u | count);                       /* add r7, r7, #count */
    emit(t, 0xe1570008u);                               /* cmp r7, r8 */
    emit_fwd(t, &budget, 0x2);                          /* bhs: the budget is used */
    uint32_t *site = t->p;
    emit(t, b_insn(0xe, site, site + 1));               /* b: patched later */
    emit_const(t, R12, (uint32_t)(uintptr_t)site);     /* the link request */
    emit(t, 0xe58ac00cu);                               /* str r12, [r10, #12] */
    emit_const(t, R0, EXIT_LINK);
    emit_fwd(t, &store, 0xe);
    fix_here(t, &budget);
    emit_const(t, R0, 0);                               /* r7 has the count */
    fix_here(t, &store);
    emit_const(t, R12, next_pc);
    emit_str_cpu(t, R12, 60);
    emit_b(t, 0xe, g_epilogue);
    t->flags_live = 0;
}

/* exit with a known next pc, never chained: the runner must look first */
static void emit_exit_stop(struct tr *t, uint32_t next_pc, uint32_t count) {
    emit_const(t, R12, next_pc);
    emit_str_cpu(t, R12, 60);
    emit_const(t, R0, count);
    emit_b(t, 0xe, g_epilogue);
}

/* Exit to the pc in host register r0, after count more instructions,
 * through the indirect-branch cache if the target is there. */
static void emit_exit_reg(struct tr *t, uint32_t count, int interwork) {
    struct fixups raw = { { 0 }, 0 }, counted = { { 0 }, 0 };
    if (interwork) {
        emit(t, 0xe3100003u);                           /* tst r0, #3: Thumb, or not aligned */
        emit_fwd(t, &raw, 0x1);                         /* bne */
    } else {
        emit(t, 0xe3c00003u);                           /* bic r0, r0, #3 */
    }
    emit(t, 0xe2877000u | count);                       /* add r7, r7, #count */
    emit(t, 0xe1570008u);                               /* cmp r7, r8 */
    emit_fwd(t, &counted, 0x2);                         /* bhs: the budget is used */
    emit(t, 0xe59ac010u);                               /* ldr r12, [r10, #16]: the cache */
    emit(t, 0xe7e01150u | (IBC_BITS - 1) << 16);        /* ubfx r1, r0, #2, #IBC_BITS */
    emit(t, 0xe08cc181u);                               /* add r12, r12, r1, lsl #3 */
    emit(t, 0xe59c1000u);                               /* ldr r1, [r12]: its pc */
    emit(t, 0xe1510000u);                               /* cmp r1, r0 */
    emit(t, 0x059cf004u);                               /* ldreq pc, [r12, #4]: its chain entry */
    fix_here(t, &counted);                              /* a miss: r7 has the count */
    emit_str_cpu(t, R0, 60);
    emit_const(t, R0, interwork ? EXIT_INTERWORK : 0);
    emit_b(t, 0xe, g_epilogue);
    if (interwork) {
        fix_here(t, &raw);
        emit_str_cpu(t, R0, 60);
        emit_const(t, R0, count | EXIT_INTERWORK);
        emit_b(t, 0xe, g_epilogue);
    }
    t->flags_live = 0;
}

/* store the guest pc of instruction idx: the bus reads it as the pc of an access */
static void emit_set_pc(struct tr *t, uint32_t pc) {
    emit_const(t, R12, pc);
    emit_str_cpu(t, R12, 60);
}

/* branch to f if [a, a + size) is not direct. Changes the host flags. */
static void emit_check(struct tr *t, unsigned a, unsigned size, struct fixups *f) {
    emit(t, 0xe59ac000u);                               /* ldr r12, [r10]: the direct table */
    emit(t, 0xe7dcc820u | a);                           /* ldrb r12, [r12, a, lsr #16] */
    emit(t, 0xe35c0000u);                               /* cmp r12, #0 */
    emit_fwd(t, f, 0x0);                                /* beq */
    emit(t, 0xe3500a01u | a << 16);                     /* cmp a, #0x1000 */
    emit_fwd(t, f, 0x3);                                /* blo */
    if (size > 1) {
        emit(t, 0xe280c000u | a << 16 | (size - 1));    /* add r12, a, #size-1 */
        emit(t, 0xe02cc000u | a);                       /* eor r12, r12, a */
        emit(t, 0xe1b0c82cu);                           /* movs r12, r12, lsr #16 */
        emit_fwd(t, f, 0x1);                            /* bne */
    }
    t->flags_live = 0;
}

/* call the interpreter for this instruction */
static void emit_step(struct tr *t, uint32_t pc) {
    emit_set_pc(t, pc);
    emit_mov(t, R0, R11);
    emit_mov(t, R1, R9);
    emit_const(t, R2, (uint32_t)t->idx);
    emit_call(t, (const void *)h_step);
    emit_ldr_cpu(t, R9, 64);                            /* the interpreter can change the flags */
    emit(t, 0xe3500000u);                               /* cmp r0, #0 */
    emit_b(t, 0x1, g_epilogue);                         /* bne: r0 is the exit code */
}

/* a privileged instruction: vcpu.c through h_priv(), without the interpreter */
static void emit_priv(struct tr *t, uint32_t pc, uint32_t orig) {
    emit_set_pc(t, pc);
    emit_mov(t, R0, R11);
    emit_mov(t, R1, R9);
    emit_const(t, R2, (uint32_t)t->idx);
    emit_const(t, R3, orig);
    emit_call(t, (const void *)h_priv);
    emit_ldr_cpu(t, R9, 64);                            /* MSR and exception returns change the flags */
    emit(t, 0xe3500000u);                               /* cmp r0, #0 */
    emit_b(t, 0x1, g_epilogue);                         /* bne: r0 is the exit code */
}

/* ---------- templates. Each returns 1 if the block ends, 0 to continue,
 * -1 if it has no template for the instruction. ---------- */

static int t_dp(struct tr *t, uint32_t insn, uint32_t pc) {
    unsigned op = (insn >> 21) & 0xf, rd = (insn >> 12) & 0xf, rn = (insn >> 16) & 0xf;
    unsigned cond = insn >> 28, s = (insn >> 20) & 1;
    int imm = (insn >> 25) & 1, regshift = !imm && (insn & 0x10);
    int uses_rn = op != 0xd && op != 0xf, writes = op < 0x8 || op > 0xb;
    int rrx = !imm && !regshift && ((insn >> 5) & 3) == 3 && ((insn >> 7) & 0x1f) == 0;
    if (s && rd == 15) return -1;                       /* exception return */
    if (regshift && (rn == 15 || (insn & 0xf) == 15 || ((insn >> 8) & 0xf) == 15)) return -1;
    if (writes && rd == 15 && cond != 0xe) return -1;
    if (uses_rn) emit_get(t, R1, rn, pc, 8);
    if (!imm) emit_get(t, R2, insn & 0xf, pc, 8);
    if (regshift) emit_get(t, R3, (insn >> 8) & 0xf, pc, 8);
    if (writes && cond != 0xe) emit_get(t, R0, rd, pc, 8);      /* kept if the condition fails */
    if (cond != 0xe || s || op == 0x5 || op == 0x6 || op == 0x7 || rrx) emit_flags_in(t);
    uint32_t host = (insn & 0xfff00000u) | (uses_rn ? R1 << 16 : 0) | (R0 << 12);
    if (imm) host |= insn & 0xfffu;
    else if (regshift) host |= R3 << 8 | (insn & 0xf0u) | R2;
    else host |= (insn & 0xff0u) | R2;
    emit(t, host);
    if (s) emit_flags_out(t);
    if (!writes) return 0;
    if (rd == 15) { emit_exit_reg(t, (uint32_t)t->idx + 1, 0); return 1; }
    emit_put(t, R0, rd);
    return 0;
}

static int t_multiply(struct tr *t, uint32_t insn, uint32_t pc) {
    unsigned rd = (insn >> 16) & 0xf, rn = (insn >> 12) & 0xf, rs = (insn >> 8) & 0xf, rm = insn & 0xf;
    unsigned cond = insn >> 28, s = (insn >> 20) & 1;
    int is_long = (insn & 0x0f8000f0u) == 0x00800090u;
    if ((insn & 0x0fc000f0u) != 0x00000090u && !is_long) return -1;
    if (rd == 15 || rs == 15 || rm == 15 || ((is_long || (insn & 0x00200000u)) && rn == 15)) return -1;
    emit_get(t, R2, rm, pc, 8);
    emit_get(t, R3, rs, pc, 8);
    if (is_long) {
        emit_get(t, R0, rd, pc, 8);                     /* RdHi, RdLo: accumulate, or kept */
        emit_get(t, R1, rn, pc, 8);
    } else {
        if (insn & 0x00200000u) emit_get(t, R1, rn, pc, 8);
        if (cond != 0xe) emit_get(t, R0, rd, pc, 8);
    }
    if (cond != 0xe || s) emit_flags_in(t);
    unsigned lo = (is_long || (insn & 0x00200000u)) ? R1 : 0;     /* MUL: the field must be 0 */
    emit(t, (insn & 0xfff000f0u) | R0 << 16 | lo << 12 | R3 << 8 | R2);
    if (s) emit_flags_out(t);
    if (is_long) emit_put(t, R1, rn);
    emit_put(t, R0, rd);
    return 0;
}

/* LDR/STR/LDRB/STRB, and LDRH/STRH/LDRSB/LDRSH (extra = 1) */
static int t_single(struct tr *t, uint32_t insn, uint32_t pc, int extra) {
    unsigned rn = (insn >> 16) & 0xf, rd = (insn >> 12) & 0xf, cond = insn >> 28;
    int pre = (insn >> 24) & 1, up = (insn >> 23) & 1, wback = !pre || ((insn >> 21) & 1);
    int load = (insn >> 20) & 1, regoff;
    unsigned size, kind;
    uint32_t fast_op, pc_plus = extra ? 8 : 12;         /* r15 as the stored value, as interp.c */                                   /* host load or store of r0 at [r?] */
    struct fixups skip = { { 0 }, 0 }, slow = { { 0 }, 0 }, done = { { 0 }, 0 };

    if (wback && rn == 15) return -1;
    if (extra) {
        unsigned sh = (insn >> 5) & 3;
        if (sh == 0 || (!load && sh != 1) || (load && rd == 15)) return -1;   /* SWP, LDRD/STRD */
        regoff = !((insn >> 22) & 1);
        if (regoff && (insn & 0xf) == 15) return -1;
        size = sh == 2 ? 1 : 2;
        kind = size | (sh >= 2 ? 0x10 : 0);
        fast_op = sh == 1 ? (load ? 0xe1d000b0u : 0xe1c000b0u) : sh == 2 ? 0xe1d000d0u : 0xe1d000f0u;
    } else {
        regoff = (insn >> 25) & 1;
        if (regoff && ((insn & 0xf) == 15 || (((insn >> 5) & 3) == 3 && ((insn >> 7) & 0x1f) == 0))) return -1;
        size = (insn >> 22) & 1 ? 1 : 4;
        kind = size;
        fast_op = size == 1 ? (load ? 0xe5d00000u : 0xe5c00000u) : (load ? 0xe5900000u : 0xe5800000u);
    }

    if (cond != 0xe) { emit_flags_in(t); emit_fwd(t, &skip, cond ^ 1); }
    emit_get(t, R4, rn, pc, 8);
    if (regoff) {
        emit_get(t, R2, insn & 0xf, pc, 8);
        uint32_t shift = extra ? 0 : insn & 0xff0u;
        emit(t, (up ? 0xe0800000u : 0xe0400000u) | R4 << 16 | R5 << 12 | shift | R2);   /* add/sub r5, r4, r2, shift */
    } else {
        uint32_t off = extra ? (((insn >> 4) & 0xf0u) | (insn & 0xfu)) : insn & 0xfffu;
        emit_const(t, R12, off);
        emit(t, (up ? 0xe0800000u : 0xe0400000u) | R4 << 16 | R5 << 12 | R12);          /* add/sub r5, r4, r12 */
    }
    unsigned a = pre ? R5 : R4;
    emit_check(t, a, size, &slow);
    if (!load) emit_get(t, R0, rd, pc, pc_plus);
    emit(t, fast_op | a << 16 | R0 << 12);
    emit_fwd(t, &done, 0xe);

    fix_here(t, &slow);
    emit_set_pc(t, pc);
    emit_mov(t, R1, a);
    if (load) {
        emit_mov(t, R0, R11);
        emit_const(t, R2, kind);
        emit_call(t, (const void *)h_read);
    } else {
        emit_get(t, R3, rd, pc, pc_plus);
        emit_mov(t, R0, R11);
        emit_const(t, R2, size);
        emit_call(t, (const void *)h_write);
        emit(t, 0xe3500000u);                           /* cmp r0, #0 */
        emit_fwd(t, &done, 0x0);                        /* beq: continue */
        if (wback) emit_put(t, R5, rn);
        emit_exit_stop(t, pc + 4, (uint32_t)t->idx + 1);  /* the write stopped the core */
    }

    fix_here(t, &done);
    if (wback) emit_put(t, R5, rn);
    t->flags_live = 0;
    if (load && rd == 15) {
        emit_exit_reg(t, (uint32_t)t->idx + 1, 1);
        if (skip.n) { fix_here(t, &skip); emit_exit(t, pc + 4, (uint32_t)t->idx + 1); }
        return 1;
    }
    if (load) emit_put(t, R0, rd);
    fix_here(t, &skip);
    return 0;
}

/* LDM/STM without ^ */
static int t_multi(struct tr *t, uint32_t insn, uint32_t pc) {
    unsigned rn = (insn >> 16) & 0xf, list = insn & 0xffff, cond = insn >> 28, n = 0, k;
    int pre = (insn >> 24) & 1, up = (insn >> 23) & 1, wback = (insn >> 21) & 1, load = (insn >> 20) & 1;
    struct fixups skip = { { 0 }, 0 }, slow = { { 0 }, 0 }, done = { { 0 }, 0 };
    if (rn == 15 || !list) return -1;
    for (unsigned i = 0; i < 16; i++) n += (list >> i) & 1;
    int32_t start = up ? (pre ? 4 : 0) : -(int32_t)(4 * n) + (pre ? 0 : 4);
    int32_t final = up ? (int32_t)(4 * n) : -(int32_t)(4 * n);

    if (cond != 0xe) { emit_flags_in(t); emit_fwd(t, &skip, cond ^ 1); }
    emit_get(t, R4, rn, pc, 8);
    emit_const(t, R12, (uint32_t)(start < 0 ? -start : start));
    emit(t, (start < 0 ? 0xe0400000u : 0xe0800000u) | R4 << 16 | R5 << 12 | R12);   /* r5 = first address */
    if (wback) {
        emit_const(t, R12, (uint32_t)(final < 0 ? -final : final));
        emit(t, (final < 0 ? 0xe0400000u : 0xe0800000u) | R4 << 16 | R6 << 12 | R12);
    }
    emit_check(t, R5, 4 * n, &slow);
    if (load) {
        if (wback) emit_put(t, R6, rn);                 /* a loaded base wins over write-back */
        k = 0;
        for (unsigned i = 0; i < 15; i++) {
            if (!((list >> i) & 1)) continue;
            emit(t, 0xe5950000u | R12 << 12 | 4 * k++);     /* ldr r12, [r5, #4k] */
            emit_put(t, R12, i);
        }
        if (list & 0x8000) emit(t, 0xe5950000u | R0 << 12 | 4 * k);  /* ldr r0, [r5, #4k] */
    } else {
        k = 0;
        for (unsigned i = 0; i < 16; i++) {
            if (!((list >> i) & 1)) continue;
            emit_get(t, R12, i, pc, 12);
            emit(t, 0xe5850000u | R12 << 12 | 4 * k++);     /* str r12, [r5, #4k] */
        }
        if (wback) emit_put(t, R6, rn);
    }
    emit_fwd(t, &done, 0xe);

    fix_here(t, &slow);                                 /* not direct: the interpreter does it, then exit */
    emit_set_pc(t, pc);
    emit_mov(t, R0, R11);
    emit_mov(t, R1, R9);
    emit_const(t, R2, (uint32_t)t->idx);
    emit_call(t, (const void *)h_step);
    emit_ldr_cpu(t, R9, 64);
    emit(t, 0xe3500000u);                               /* cmp r0, #0 */
    emit_b(t, 0x1, g_epilogue);                         /* bne */
    struct fixups cont = { { 0 }, 0 };
    emit_fwd(t, &cont, 0xe);                            /* h_step continued: after this instruction */

    fix_here(t, &done);
    t->flags_live = 0;
    if (load && (list & 0x8000)) {
        emit_exit_reg(t, (uint32_t)t->idx + 1, 1);
        fix_here(t, &cont);                             /* not reached: h_step exits for a pc load */
        if (skip.n) { fix_here(t, &skip); emit_exit(t, pc + 4, (uint32_t)t->idx + 1); }
        return 1;
    }
    fix_here(t, &skip);
    fix_here(t, &cont);
    return 0;
}

static int t_branch(struct tr *t, uint32_t insn, uint32_t pc) {
    unsigned cond = insn >> 28;
    struct fixups skip = { { 0 }, 0 };
    int32_t off = (int32_t)(insn << 8) >> 6;
    if (cond != 0xe) { emit_flags_in(t); emit_fwd(t, &skip, cond ^ 1); }
    if (insn & 0x01000000u) {                           /* BL */
        emit_const(t, R12, pc + 4);
        emit_put(t, R12, 14);
    }
    emit_exit(t, pc + 8 + (uint32_t)off, (uint32_t)t->idx + 1);
    if (skip.n) { fix_here(t, &skip); emit_exit(t, pc + 4, (uint32_t)t->idx + 1); }
    return 1;
}

static int t_bx(struct tr *t, uint32_t insn, uint32_t pc) {
    unsigned cond = insn >> 28, rm = insn & 0xf;
    struct fixups skip = { { 0 }, 0 };
    if (rm == 15) return -1;
    if (cond != 0xe) { emit_flags_in(t); emit_fwd(t, &skip, cond ^ 1); }
    emit_get(t, R0, rm, pc, 8);
    if ((insn & 0x0ffffff0u) == 0x012fff30u) {          /* BLX: lr after reading rm */
        emit_const(t, R12, pc + 4);
        emit_put(t, R12, 14);
    }
    emit_exit_reg(t, (uint32_t)t->idx + 1, 1);
    if (skip.n) { fix_here(t, &skip); emit_exit(t, pc + 4, (uint32_t)t->idx + 1); }
    return 1;
}

/* one guest instruction: 1 if the block ends after it */
static int translate_one(struct tr *t, uint32_t insn, uint32_t pc) {
    int r = -1;
    if ((insn >> 28) != 0xf) {
        switch ((insn >> 25) & 7) {
        case 0:
            if ((insn & 0x0ffffff0u) == 0x012fff10u || (insn & 0x0ffffff0u) == 0x012fff30u) r = t_bx(t, insn, pc);
            else if ((insn & 0x90u) == 0x90u) {
                if ((insn & 0x0f0000f0u) == 0x00000090u) r = t_multiply(t, insn, pc);
                else r = t_single(t, insn, pc, 1);
            } else if ((insn & 0x01900000u) != 0x01000000u) r = t_dp(t, insn, pc);
            break;
        case 1:
            if ((insn & 0x01900000u) != 0x01000000u) r = t_dp(t, insn, pc);
            break;
        case 2: r = t_single(t, insn, pc, 0); break;
        case 3:
            if (!(insn & 0x10)) r = t_single(t, insn, pc, 0);
            else if ((insn & 0xfff000f0u) == 0xe7f000f0u) {     /* a trap word */
                uint32_t orig = priv_decode(insn);
                if (orig && orig != PRIV_SEMIHOST) { emit_priv(t, pc, orig); r = 0; }
            }
            break;
        case 4: if (!(insn & 0x00400000u)) r = t_multi(t, insn, pc); break;
        case 5: r = t_branch(t, insn, pc); break;
        default: break;
        }
    }
    if (r >= 0) return r;
    emit_step(t, pc);                                   /* no template: the interpreter */
    return 0;
}

/* ---------- blocks ---------- */

static int is_direct(const uint8_t *direct, uint32_t a) {
    return direct[a >> 16] && a >= PAGE0_END;
}

/* Where the host reads the code of a block at pc: pc, or for page 0 the
 * same memory at its alias (interp_env.page0_alias). 0 if neither is direct. */
static uint32_t code_base(const uint8_t *direct, uint32_t pc) {
    uint32_t alias = g_env->page0_alias;
    if (is_direct(direct, pc)) return pc;
    if (pc < PAGE0_END && alias && direct[(pc + alias) >> 16]) return pc + alias;
    return 0;
}

static void build_stubs(void) {
    uint32_t *p = g_code;
    g_enter = (uint32_t (*)(struct arm_cpu *, const uint32_t *, struct jit_ctx *))(uintptr_t)p;
    *p++ = 0xe92d4ff8u;                                 /* push {r3-r11, lr} */
    *p++ = 0xe1a0b000u;                                 /* mov r11, r0 */
    *p++ = 0xe1a0a002u;                                 /* mov r10, r2 */
    *p++ = 0xe59b9040u;                                 /* ldr r9, [r11, #64] */
    *p++ = 0xe3a07000u;                                 /* mov r7, #0 */
    *p++ = 0xe59a8008u;                                 /* ldr r8, [r10, #8] */
    *p++ = 0xe12fff11u;                                 /* bx r1 */
    g_epilogue = p;
    *p++ = 0xe0800007u;                                 /* add r0, r0, r7 */
    *p++ = 0xe59bc040u;                                 /* ldr r12, [r11, #64] */
    *p++ = 0xe3ccc4f8u;                                 /* bic r12, r12, #0xf8000000 */
    *p++ = 0xe20914f8u;                                 /* and r1, r9, #0xf8000000 */
    *p++ = 0xe18cc001u;                                 /* orr r12, r12, r1 */
    *p++ = 0xe58bc040u;                                 /* str r12, [r11, #64] */
    *p++ = 0xe8bd8ff8u;                                 /* pop {r3-r11, pc} */
    g_code_free = p;
    __builtin___clear_cache((char *)g_code, (char *)p);
}

static void flush_all(void) {
    jit_n_flushes++;
    memset(g_table, 0, sizeof g_table);
    for (uint32_t i = 0; i < IBC_SIZE; i++) g_ibc[i].pc = 1;   /* empty: no target has bit 0 set */
    g_words_free = g_words;
    build_stubs();
}

int jit_init(void) {
    void *p = mmap(NULL, CODE_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return -1;
    g_code = p;
    g_code_end = g_code + CODE_SIZE / 4;
    hostprof_jit_range((uint32_t)(uintptr_t)g_code, (uint32_t)(uintptr_t)g_code_end);
    g_words = mmap(NULL, WORDS_SIZE * 4, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (g_words == MAP_FAILED) return -1;
    flush_all();
    return 0;
}

static uint32_t hash_pc(uint32_t pc) {
    return ((pc >> 2) ^ (pc >> 17) ^ (pc >> 28)) & (TABLE_SIZE - 1);
}

/* the slot of pc, or the slot for a new block of pc */
static struct block *slot_for(uint32_t pc) {
    uint32_t h = hash_pc(pc);
    for (int i = 0; i < PROBES; i++) {
        struct block *b = &g_table[(h + (uint32_t)i) & (TABLE_SIZE - 1)];
        if (!b->code || b->pc == pc) return b;
    }
    jit_n_collide++;
    return &g_table[h];                                 /* all used by other pcs: replace the first */
}

static struct block *translate(uint32_t pc, const uint8_t *direct) {
    struct tr t;
    uint32_t n = 0, delta = code_base(direct, pc) - pc;    /* host address of the code - pc */
    if ((size_t)((uint8_t *)g_code_end - (uint8_t *)g_code_free) < BLOCK_SPACE ||
        g_words_free + JIT_MAX_BLOCK > g_words + WORDS_SIZE)
        flush_all();
    struct block *b = slot_for(pc);
    t.p = g_code_free;
    t.flags_live = 0;
    uint32_t *code = t.p;
    struct fixups body = { { 0 }, 0 };
    emit(&t, 0);                                        /* the epoch word: not valid yet */
    uint32_t *chain = t.p;
    emit(&t, 0xe59ac004u);                              /* ldr r12, [r10, #4]: the epoch */
    emit(&t, 0xe51f1010u);                              /* ldr r1, [pc, #-16]: the epoch word */
    emit(&t, 0xe15c0001u);                              /* cmp r12, r1 */
    emit_fwd(&t, &body, 0x0);                           /* beq */
    emit_const(&t, R12, pc);                            /* not validated in this epoch: to jit_run() */
    emit_str_cpu(&t, R12, 60);
    emit_const(&t, R0, 0);
    emit_b(&t, 0xe, g_epilogue);
    fix_here(&t, &body);
    uint32_t *entry = t.p;
    for (;;) {
        uint32_t ipc = pc + 4 * n;
        if (n == JIT_MAX_BLOCK || !direct[(ipc + delta) >> 16] || ipc + delta < PAGE0_END) {
            emit_exit(&t, ipc, n);
            break;
        }
        uint32_t insn = *(const uint32_t *)(uintptr_t)(ipc + delta);
        g_words_free[n] = insn;
        t.idx = (int)n;
        n++;
        if (translate_one(&t, insn, ipc)) break;
    }
    __builtin___clear_cache((char *)code, (char *)t.p);
    g_code_free = t.p;
    b->pc = pc;
    b->n = n;
    b->words = g_words_free;
    b->code = code;
    b->is_static = in_static(pc, n);
    b->chain = b->is_static ? entry : chain;            /* static: linked jumps skip the epoch check */
    b->entry = entry;
    g_words_free += n;
    jit_n_translated++;
    return b;
}

static struct block *lookup(uint32_t pc, const uint8_t *direct) {
    uint32_t base = code_base(direct, pc);
    if (!base) return NULL;
    struct block *b = slot_for(pc);
    if (b->code && b->pc == pc) {
        if (*b->code == g_epoch || b->is_static) return b;
        jit_n_validate++;
        if (!memcmp((const void *)(uintptr_t)base, b->words, 4 * b->n)) { *b->code = g_epoch; return b; }
        jit_n_stale++;                                  /* the guest changed the code */
    }
    b = translate(pc, direct);
    *b->code = g_epoch;
    return b;
}

static void load_pc(struct arm_cpu *c, uint32_t val) {
    if (val & 1) { c->cpsr |= ARM_CPSR_T; c->r[15] = val & ~1u; }
    else { c->cpsr &= ~ARM_CPSR_T; c->r[15] = val & ~3u; }
}

void jit_flush(void) {
    flush_all();
}

void jit_new_epoch(void) {
    g_epoch++;
    if (!g_epoch) g_epoch = 1;                          /* 0 marks a block that was never valid */
}

int jit_run(struct vcpu *v, struct arm_cpu *c, const struct interp_env *env, struct interp_run *run) {
    uint32_t n = 0, idle = run->idle;
    int rc = INTERP_OK;
    g_v = v;
    g_env = env;
    g_ctx.direct = env->direct;
    g_ctx.epoch = g_epoch;
    g_ctx.ibc = g_ibc;
    run->stop = 0;
    run->last_slow = 0;
    while (n < run->budget) {
        if (c->cpsr & ARM_CPSR_T) { rc = INTERP_STOP; break; }
        struct block *b = lookup(c->r[15], env->direct);
        if (!b) {                                       /* not direct code (page 0): interpret one */
            struct interp_run one;
            memset(&one, 0, sizeof one);
            one.budget = 1;
            rc = interp_run(v, c, env, &one);
            if (rc != INTERP_OK) break;
            n += one.n;
            if (one.last_slow) {
                idle = 0;
                if (one.stop) { run->stop = 1; break; }
            } else if (run->idle_limit && ++idle >= run->idle_limit) {
                run->stop = 1;
                break;
            }
            continue;
        }
        struct ibc_entry *e = &g_ibc[(c->r[15] >> 2) & (IBC_SIZE - 1)];
        e->pc = c->r[15];                               /* for the indirect jumps to it */
        e->chain = b->chain;
        g_flags = 0;
        g_ctx.budget = run->budget - n;
        if (g_ctx.budget > MAX_ENTRY_BUDGET) g_ctx.budget = MAX_ENTRY_BUDGET;
        uint32_t ret = g_enter(c, b->entry, &g_ctx);
        uint32_t cnt = ret & EXIT_COUNT;
        jit_n_blocks++;
        n += cnt;
        if (ret & EXIT_INTERWORK) load_pc(c, c->r[15]);
        if (ret & EXIT_LINK) {                          /* patch the exit to the next block */
            jit_n_link++;
            struct block *next = lookup(c->r[15], env->direct);
            if (next && g_ctx.link_site >= g_code && g_ctx.link_site < g_code_free) {
                *g_ctx.link_site = b_insn(0xe, g_ctx.link_site, next->chain);
                __builtin___clear_cache((char *)g_ctx.link_site, (char *)(g_ctx.link_site + 1));
            }
        }
        if (g_flags & JF_RC) { rc = g_rc; break; }
        if (g_flags & JF_SLOW) { idle = 0; run->last_slow = 1; }
        else idle += cnt;
        if (g_flags & JF_STOP) { run->stop = 1; break; }
        if (run->idle_limit && idle >= run->idle_limit) { run->stop = 1; break; }
    }
    run->n = n;
    run->idle = idle;
    return rc;
}
