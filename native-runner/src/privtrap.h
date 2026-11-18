/* Trap words for privileged guest instructions.
 *
 * A host user-mode process does not trap on MRS/MSR or on an exception
 * return: the CPU executes them with the wrong effect. The runner replaces
 * each such instruction in guest memory with an undefined instruction (UDF),
 * which gives SIGILL. The 16-bit immediate of the UDF holds the original
 * instruction, so a trap word does not depend on its address: guest code
 * that copies itself (IRAM) or runs through a memory alias still works, and
 * no table is necessary.
 *
 * Only exact, unconditional encodings are patched. A data word that only
 * looks like an instruction must not change.
 */
#ifndef PRIVTRAP_H
#define PRIVTRAP_H
#include <stddef.h>
#include <stdint.h>

/* The ARM semihosting call. The firmware's C library prints its panic
 * messages with it. It also gets a trap word. */
#define PRIV_SEMIHOST 0xef123456u

/* Trap word for a privileged instruction, or 0 if insn is not one that this
 * module replaces. */
uint32_t priv_encode(uint32_t insn);

/* Original instruction of a trap word, or 0 if word is not a trap word. */
uint32_t priv_decode(uint32_t word);

/* Replace each privileged instruction in words[0..n) with its trap word.
 * Returns the number of words changed. */
size_t priv_patch(uint32_t *words, size_t n);

#endif
