/* The PP502x GPIO ports A-L and their interrupts.
 *
 * Twelve 8-bit ports in three groups of four (A-D, E-H, I-L), 0x80 bytes
 * per group. In a group, each register has one word per port (port A at
 * +0, B at +4, ...):
 *   0x00 ENABLE, 0x10 OUTPUT_EN, 0x20 OUTPUT_VAL, 0x30 INPUT_VAL,
 *   0x40 INT_STAT, 0x50 INT_EN, 0x60 INT_LEV, 0x70 INT_CLR (write only).
 * At +0x800 each register has a bitwise alias: a write of (mask << 8) |
 * value changes only the mask bits. The retailOS sets its pins up this
 * way.
 *
 * An input pin with its interrupt enabled sets its status bit when its
 * level becomes the level in INT_LEV (or when it matches at the enable).
 * INT_CLR clears status bits. A group with an enabled status bit raises its
 * interrupt line (32 + group). The firmware's handler sets the next level
 * before it clears the status, as Rockbox does.
 *
 * The inputs read high (nothing pressed, no accessory), except the pins
 * that gpio_set_input() pulls low: the hold switch (A5, low = hold on).
 */
#include "dev.h"
#include <string.h>

#define GPIO_PORTS      12
#define GPIO_GROUP      0x80u
#define GPIO_BITWISE    0x800u
#define GPIO_IRQ_BASE   32
#define R_ENABLE        0x00u
#define R_OUTPUT_EN     0x10u
#define R_OUTPUT_VAL    0x20u
#define R_INPUT_VAL     0x30u
#define R_INT_STAT      0x40u
#define R_INT_EN        0x50u
#define R_INT_LEV       0x60u
#define R_INT_CLR       0x70u

static struct {
    uint8_t enable[GPIO_PORTS], output_en[GPIO_PORTS], output_val[GPIO_PORTS];
    uint8_t int_stat[GPIO_PORTS], int_en[GPIO_PORTS], int_lev[GPIO_PORTS];
    uint8_t low[GPIO_PORTS];            /* input pins pulled low (gpio_set_input) */
    uint8_t match[GPIO_PORTS];          /* pins whose level matched INT_LEV at the last check */
} gpio;

static uint8_t input(unsigned port) { return (uint8_t)~gpio.low[port]; }

/* the backlight pins (dev_backlight.c): B3 drives the LED driver, L7 the LED */
#define BACKLIGHT_EN_PORT   1u
#define BACKLIGHT_EN_BIT    3u
#define BACKLIGHT_LED_PORT  11u
#define BACKLIGHT_LED_BIT   7u

/* the interrupt line of the group of port */
static void update_line(unsigned port) {
    unsigned g = port / 4, any = 0;
    for (unsigned p = g * 4; p < g * 4 + 4; p++) any |= gpio.int_stat[p] & gpio.int_en[p];
    intc_set_irq(GPIO_IRQ_BASE + (int)g, any != 0);
}

/* a pin that comes to its interrupt level sets its status bit */
static void check(unsigned port) {
    uint8_t m = (uint8_t)(gpio.int_en[port] & ~(input(port) ^ gpio.int_lev[port]));
    gpio.int_stat[port] |= (uint8_t)(m & ~gpio.match[port]);
    gpio.match[port] = m;
    update_line(port);
}

static uint8_t *reg_of(uint32_t r, unsigned port) {
    switch (r) {
    case R_ENABLE:     return &gpio.enable[port];
    case R_OUTPUT_EN:  return &gpio.output_en[port];
    case R_OUTPUT_VAL: return &gpio.output_val[port];
    case R_INT_STAT:   return &gpio.int_stat[port];
    case R_INT_EN:     return &gpio.int_en[port];
    case R_INT_LEV:    return &gpio.int_lev[port];
    default:           return NULL;
    }
}

/* reg: the offset in the GPIO page. 0 if the offset is no register */
static int decode(uint32_t reg, uint32_t *r, unsigned *port) {
    if (reg >= GPIO_BITWISE) reg -= GPIO_BITWISE;
    if (reg >= 3 * GPIO_GROUP) return 0;
    *r = reg & 0x70u;
    *port = (reg / GPIO_GROUP) * 4 + ((reg >> 2) & 3);
    return 1;
}

uint32_t gpio_read(uint32_t reg) {
    uint32_t r;
    unsigned port;
    if (!decode(reg, &r, &port)) return 0;
    if (r == R_INPUT_VAL) return 0xffffff00u | input(port);
    if (r == R_INT_CLR) return 0;
    return *reg_of(r, port);
}

void gpio_write(uint32_t reg, uint32_t v) {
    uint32_t r;
    unsigned port;
    if (!decode(reg, &r, &port)) return;
    if (r == R_INPUT_VAL) return;
    if (r == R_INT_CLR) {
        gpio.int_stat[port] &= (uint8_t)~v;
        update_line(port);
        return;
    }
    uint8_t *p = reg_of(r, port);
    if (reg >= GPIO_BITWISE) {
        uint8_t mask = (uint8_t)(v >> 8);
        *p = (uint8_t)((*p & ~mask) | (v & mask));
    } else {
        *p = (uint8_t)v;
    }
    if (r == R_INT_EN || r == R_INT_LEV || r == R_INT_STAT) check(port);
    if (r == R_OUTPUT_VAL && (port == BACKLIGHT_EN_PORT || port == BACKLIGHT_LED_PORT))
        backlight_pins((gpio.output_val[BACKLIGHT_EN_PORT] >> BACKLIGHT_EN_BIT) & 1,
                       (gpio.output_val[BACKLIGHT_LED_PORT] >> BACKLIGHT_LED_BIT) & 1);
}

void gpio_set_input(unsigned port, unsigned bit, int level) {
    if (port >= GPIO_PORTS || bit > 7) return;
    if (level) gpio.low[port] &= (uint8_t)~(1u << bit);
    else gpio.low[port] |= (uint8_t)(1u << bit);
    check(port);
}

/* The hold switch: GPIO A bit 5, low = hold on (Rockbox: button_hold() of
 * the iPod Video). */
#define HOLD_PORT 0u
#define HOLD_BIT  5u

void gpio_toggle_hold(void) {
    int on = (gpio.low[HOLD_PORT] >> HOLD_BIT) & 1;
    gpio_set_input(HOLD_PORT, HOLD_BIT, on);            /* on: back to high (off) */
}

void gpio_reset(void) {
    memset(&gpio, 0, sizeof gpio);
}

int gpio_snapshot(FILE *f, int save) {
    return snap_rw(f, &gpio, sizeof gpio, save);
}
