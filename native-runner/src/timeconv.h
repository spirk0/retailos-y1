/* Time unit conversion without the 64-bit library division, which costs
 * hundreds of cycles on the target. Pure C. */
#ifndef TIMECONV_H
#define TIMECONV_H
#include <stdint.h>

/* ns / 1000: long division in 16-bit digits, each step a 32-bit division */
static inline uint64_t ns_to_us(uint64_t ns) {
    uint32_t hi = (uint32_t)(ns >> 32), lo = (uint32_t)ns;
    uint32_t q1 = hi / 1000, r = hi % 1000;
    uint32_t t = (r << 16) | (lo >> 16);
    uint32_t q2 = t / 1000;
    t = ((t % 1000) << 16) | (lo & 0xffffu);
    return ((uint64_t)q1 << 32) + ((uint64_t)q2 << 16) + t / 1000;
}

#endif
