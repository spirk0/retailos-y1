/* The backlight of the iPod 5G/5.5G LCD, from the firmware's pins.
 *
 * Traced on retailOS 1.3 (MMIO_TRACE) and the same as Rockbox
 * (backlight-nano_video.c):
 *   GPIO B3   enables the LED driver. At the enable the driver is at
 *             level 16 of 32.
 *   GPIO L7   the LED on or off.
 *   GPIO D7   steps the level: a low pulse. The firmware makes the pulse
 *             with a unit at 0x7000c300: the length goes to 0x7000c320 +
 *             4n, bit 30 of 0x7000c300 starts it. A short pulse (length 1)
 *             is one level darker, a long one (0x7f) one level brighter.
 * A wake of the backlight is 15 darker pulses (to level 1) and then the
 * fade-in to the Brightness setting (31 brighter pulses for the full
 * setting). The backlight timer makes the fade-out (darker pulses), then
 * L7 and B3 go low. Each step in Settings > Brightness is one pulse.
 *
 * The app gets the visible level (0 = dark, 1-32) through mach_backlight()
 * at each change.
 */
#include "dev.h"
#include <string.h>

#define LEVELS          32
#define LEVEL_AT_ENABLE 16
#define LONG_PULSE      0x40u           /* from this length: brighter */

static struct {
    int enabled, led, level, sent;
    uint32_t len;                       /* the length of the next pulse */
} bl;

int backlight_level(void) { return bl.enabled && bl.led ? bl.level : 0; }

static void update(void) {
    int v = backlight_level();
    if (v == bl.sent) return;
    bl.sent = v;
    mach_backlight(v);
}

void backlight_reset(void) {
    memset(&bl, 0, sizeof bl);
}

void backlight_pins(int enable, int led) {
    if (enable && !bl.enabled) bl.level = LEVEL_AT_ENABLE;
    bl.enabled = enable != 0;
    bl.led = led != 0;
    update();
}

void backlight_pulse_length(uint32_t len) { bl.len = len; }

void backlight_pulse(void) {
    if (!bl.enabled) return;
    if (bl.len >= LONG_PULSE) { if (bl.level < LEVELS) bl.level++; }
    else if (bl.level > 1) bl.level--;
    update();
}

int backlight_snapshot(FILE *f, int save) {
    if (snap_rw(f, &bl, sizeof bl, save)) return -1;
    if (!save) mach_backlight(backlight_level());       /* the app gets the level of the snapshot */
    return 0;
}
