/* Trap words for privileged guest instructions. See privtrap.h. */
#include "privtrap.h"

/* UDF #imm16, condition AL: 1110 0111 1111 imm12 1111 imm4 */
#define UDF_BASE 0xe7f000f0u
#define UDF_MASK 0xfff000f0u

/* payload = kind (3 bits) : data (13 bits). Kind 0 is not used: the Linux
 * kernel keeps UDF words with small immediates for breakpoints. */
enum { KIND_MRS = 1, KIND_MSR_REG = 2, KIND_FIXED = 3 };

/* exception returns: exact words only */
static const uint32_t fixed_insn[] = {
    0xe8fddfffu,    /* ldm sp!, {r0-r12, lr, pc}^ */
    0xe8fd81ffu,    /* ldm sp!, {r0-r8, pc}^ */
    0xe1b0f00eu,    /* movs pc, lr */
    0xe25ef004u,    /* subs pc, lr, #4 */
    0xe25ef008u,    /* subs pc, lr, #8 */
    PRIV_SEMIHOST,  /* svc 0x123456: natively it is a system call of the host */
};
#define N_FIXED (sizeof fixed_insn / sizeof fixed_insn[0])

static uint32_t udf(unsigned kind, unsigned data) {
    unsigned p = (kind << 13) | data;
    return UDF_BASE | ((uint32_t)(p >> 4) << 8) | (p & 0xf);
}

uint32_t priv_encode(uint32_t insn) {
    if ((insn & 0xffbf0fffu) == 0xe10f0000u) {              /* mrs rd, cpsr|spsr */
        unsigned rd = (insn >> 12) & 0xf, r = (insn >> 22) & 1;
        return rd == 15 ? 0 : udf(KIND_MRS, (r << 4) | rd);
    }
    if ((insn & 0xffb0fff0u) == 0xe120f000u) {              /* msr cpsr|spsr_<fields>, rm */
        unsigned rm = insn & 0xf, mask = (insn >> 16) & 0xf, r = (insn >> 22) & 1;
        return (rm == 15 || !mask) ? 0 : udf(KIND_MSR_REG, (r << 8) | (mask << 4) | rm);
    }
    for (unsigned i = 0; i < N_FIXED; i++)
        if (insn == fixed_insn[i]) return udf(KIND_FIXED, i);
    return 0;
}

uint32_t priv_decode(uint32_t word) {
    if ((word & UDF_MASK) != UDF_BASE) return 0;
    unsigned p = (((word >> 8) & 0xfff) << 4) | (word & 0xf);
    unsigned kind = p >> 13, data = p & 0x1fff;
    switch (kind) {
    case KIND_MRS:
        if (data >> 5 || (data & 0xf) == 15) return 0;
        return 0xe10f0000u | ((uint32_t)(data >> 4) << 22) | ((data & 0xf) << 12);
    case KIND_MSR_REG:
        if (data >> 9 || (data & 0xf) == 15 || !((data >> 4) & 0xf)) return 0;
        return 0xe120f000u | ((uint32_t)(data >> 8) << 22) | (((data >> 4) & 0xf) << 16) | (data & 0xf);
    case KIND_FIXED:
        return data < N_FIXED ? fixed_insn[data] : 0;
    default:
        return 0;
    }
}

size_t priv_patch(uint32_t *words, size_t n) {
    size_t count = 0;
    for (size_t i = 0; i < n; i++) {
        uint32_t t = priv_encode(words[i]);
        if (t) { words[i] = t; count++; }
    }
    return count;
}
