/* The frame stream to the app: each present of the guest, at once, through
 * the named pipe frames.fifo in RUNNER_TMP. See frameout.c. */
#ifndef FRAMEOUT_H
#define FRAMEOUT_H
#include <stdint.h>

#define FRAMEOUT_MAGIC  0x31465049u     /* "IPF1" */
#define FRAMEOUT_HEADER 16u             /* magic, width (16 bit), height (16 bit), sequence, 4 reserved */

/* Send a frame: height rows of width pixels, stride bytes apart, bpp 2
 * (RGB565) or 4 (RGBA32, sent as RGB565). The pipe gets RGB565, little
 * endian. Never blocks: a frame that the reader cannot take yet waits, and
 * a newer frame replaces it. */
void frameout_send(const uint8_t *pixels, uint32_t width, uint32_t height, uint32_t stride, uint32_t bpp);

/* Write what waits, as far as the pipe takes it (housekeeping) */
void frameout_poll(void);

/* An event for the app: a message of FRAMEOUT_HEADER bytes only (magic
 * "IPE1", kind as 16 bit, value as 32 bit at offset 8). It goes after the
 * frame that is in the pipe. Without a reader it is dropped. */
#define FRAMEOUT_EVENT_MAGIC 0x31455049u    /* "IPE1" */
#define FRAMEOUT_EVENT_CLICK 1              /* value: the click length in us */
#define FRAMEOUT_EVENT_VOLUME 2             /* value: left | right << 16, attenuation in cB, 0xffff = mute */
#define FRAMEOUT_EVENT_BACKLIGHT 3          /* value: 0 = dark, 1-32 = the brightness level */
void frameout_event(uint32_t kind, uint32_t value);

/* A state for the app (volume, backlight): an event now, and the last
 * value of each kind again to each new reader (the app after a resume, or
 * a new app). Kinds below FRAMEOUT_STATES. */
#define FRAMEOUT_STATES 8
void frameout_state(uint32_t kind, uint32_t value);

#endif
