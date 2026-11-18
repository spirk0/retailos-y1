/* Broadcom BCM2722 video companion chip of the iPod 5G, and the host
 * interface of its VideoCore processor.
 *
 * This is a port of the QEMU model, which boots the retailOS to its first
 * frame: qemu-david hw/arm/ipod_video/ipod_video_bcm.c and ipod_video_vc.c.
 * The comments there derive each constant from the firmware; this file
 * keeps the same names and the same behavior, and gives only a summary.
 *
 * The guest reaches the chip through eight registers, 64 KB apart, at
 * 0x30000000: a write pointer, a read pointer, a data port that moves the
 * pointer, and a control register, each for two ports. All chip memory
 * (firmware image, mailbox, VideoCore interface, display surfaces) is
 * reached through the data ports only.
 *
 * The VideoCore firmware does not run. This model gives the answers that
 * the guest's driver waits for, and speaks the channel protocol of the
 * host interface: text commands (channel type 1) and the display calls
 * (type 2). "Create surface" returns an address in chip memory, the guest
 * writes its pixels there, and "present" selects the surface for display.
 */
#include "dev.h"
#include "frameout.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>

#ifndef RUNNER_TMP
#define RUNNER_TMP "/data/local/tmp/"
#endif

#define BCM_BASE        0x30000000u
#define BCM_MMIO_SIZE   0x00080000u
#define BCM_MEM_SIZE    0x00400000u     /* the chip's low address space */
#define BCM_CTRL_BASE   0x10000000u     /* VideoCore control registers (chip address) */
#define BCM_CTRL_SIZE   0x00002000u
#define LCD_WIDTH       320
#define LCD_HEIGHT      240

/* registers, 64 KB apart */
enum {
    BCM_REG_DATA, BCM_REG_WR_ADDR, BCM_REG_RD_ADDR, BCM_REG_CONTROL,
    BCM_REG_ALT_DATA, BCM_REG_ALT_WR_ADDR, BCM_REG_ALT_RD_ADDR, BCM_REG_ALT_CONTROL
};
#define BCM_CONTROL_WR_READY   0x02
#define BCM_CONTROL_RD_READY   0x10
#define BCM_CONTROL_BOOT_READY 0x40
#define BCM_CONTROL_KICK       0x31     /* host-to-VideoCore doorbell */

/* addresses inside the chip */
#define BCMA_HANDSHAKE  0x001F0u
#define BCMA_COMMAND    0x001F8u        /* mailbox */
#define BCMA_STATUS     0x001FCu        /* the VideoCore puts the interface base here */
#define BCMA_CMDPARAM   0xE0000u        /* Rockbox framebuffer: the scanout after reset */
#define BCM_HS_MAGIC        0xA5A50000u
#define BCM_HS_MAGIC_MASK   0xFFFF0000u
#define BCM_HS_START        1u
#define BCM_HS_RUNNING      2u
#define BCM_VC_RELEASE      0x0C00u     /* in the control window */
#define BCM_VC_DOORBELL     0x0400u
#define BCM_VC_RELEASE_GO   0x80000000u
#define BCM_VC_RELEASE_ACK  0x00000001u
#define BCM_VC_DOORBELL_MAGIC 0xA5A50000u
#define BCM_VC_DOORBELL_MASK  0xFFFF0000u
#define BCM_VC_DONE         1u          /* mailbox answer: finished */
#define BCM_CMD(x)          ((~((uint32_t)(x)) << 16) | ((uint32_t)(x)))
#define BCMCMD_LCD_UPDATE       BCM_CMD(0)
#define BCMCMD_LCD_UPDATERECT   BCM_CMD(5)

/* VideoCore host interface: all offsets are from VC_IFACE_BASE */
#define VC_IFACE_BASE   0x00180000u
#define VC_OFF_HOST_SIG 0x10
#define VC_OFF_HOST_ACK 0x11
#define VC_OFF_VC_ACK   0x20
#define VC_OFF_VC_SIG   0x21
#define VC_NUM_CHANNELS 8
#define VC_CTRL_OFF(ch) (0x100 + (ch) * 0x80)
#define VC_CTRL_LEN     0x50
#define VC_RING_LEN     0x800
#define VC_TX_OFF(ch)   (0x1000 + (ch) * 0x1000)
#define VC_RX_OFF(ch)   (VC_TX_OFF(ch) + VC_RING_LEN)
#define VC_CH_TYPE      0x04
#define VC_CH_TX_START  0x06
#define VC_CH_TX_END    0x08
#define VC_CH_RX_START  0x0a
#define VC_CH_RX_END    0x0c
#define VC_CH_TX_RD     0x10    /* the model moves it */
#define VC_CH_TX_WR     0x20    /* the host moves it */
#define VC_CH_RX_RD     0x30    /* the host moves it */
#define VC_CH_RX_WR     0x40    /* the model moves it */
#define VC_CH_GENCMD    0
#define VC_CH_GFX       1
#define VC_TYPE_GENCMD  1
#define VC_TYPE_GFX     2
#define VC_MSG_MAGIC    0xF1A55A1Fu
#define VC_MSG_HDR_LEN  16
#define VC_MSG_OFF_SEQ      4
#define VC_MSG_OFF_OPCODE   8
#define VC_MSG_OFF_LENGTH   12
#define VC_MSG_OFF_FLAGS    14
#define VC_MAX_PAYLOAD  512
#define VC_SURFACE_HEAP 0x00200000u
#define VC_MAX_SURFACES 8
#define VC_GFX_PRESENT  3
#define VC_GFX_CREATE   8
#define VC_FORMAT_RGB565 1
#define VC_FORMAT_RGBA32 15

#define FRAME_MIN_INTERVAL_US 250000u   /* at most four frame files per second */

struct bcm_port {
    uint32_t wr_addr;       /* write pointer, moves with each access */
    uint32_t rd_addr;
    uint32_t wr_start;      /* where the current run of writes started */
};

struct vc_surface {
    uint32_t valid, addr, width, height, stride, size, format;
};

static struct {
    struct bcm_port port[2];
    uint32_t vc_running;
    uint32_t vc_iface_live;
    struct vc_surface surface[VC_MAX_SURFACES];
    uint32_t vc_heap;
    uint32_t scan_addr, scan_width, scan_height, scan_stride, scan_format;
    uint32_t dirty;
} bcm;
static uint8_t *bcm_mem;    /* BCM_MEM_SIZE */
static uint8_t *bcm_ctrl;   /* BCM_CTRL_SIZE */
static unsigned g_frames;
int g_frame_presented;

static uint16_t ld16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t ld32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static void st16(uint8_t *p, uint32_t v) { p[0] = v & 0xff; p[1] = (v >> 8) & 0xff; }
static void st32(uint8_t *p, uint32_t v) { st16(p, v); st16(p + 2, v >> 16); }

/* ---------- VideoCore host interface ---------- */

static uint8_t *vc_iface(uint32_t off) { return bcm_mem + VC_IFACE_BASE + off; }
static uint16_t vc_ld16(uint32_t off) { return ld16(vc_iface(off)); }
static void vc_st16(uint32_t off, uint16_t v) { st16(vc_iface(off), v); }

/* how much of a ring is in use (firmware 0xf5834) */
static uint32_t vc_occupancy(uint16_t start, uint16_t end, uint16_t rd, uint16_t wr) {
    return wr < rd ? (uint32_t)(end - rd) + (wr - start) : (uint32_t)(wr - rd);
}

/* copy len bytes out of a ring. dst == NULL skips them. */
static void vc_ring_read(uint16_t start, uint16_t end, uint16_t *rd, uint8_t *dst, uint32_t len) {
    for (uint32_t i = 0; i < len; i++) {
        if (dst) dst[i] = *vc_iface(*rd);
        if (++*rd == end) *rd = start;
    }
}

static void vc_ring_write(uint16_t start, uint16_t end, uint16_t *wr, const uint8_t *src, uint32_t len) {
    for (uint32_t i = 0; i < len; i++) {
        *vc_iface(*wr) = src[i];
        if (++*wr == end) *wr = start;
    }
}

/* text commands that the VideoCore firmware registers (vmcs.bin) */
static const char *const vc_gencmd_names[] = {
    "tasks", "version", "commands", "power_down", "set_vll_dir",
    "led_control", "disk_notify", "usb_inserted", "audio_enable",
    "inuse_notify", "power_control", "audio_control", "motor_control",
    "camera_control", "display_control", "end_application",
    "power_management", "load_application", "vmcs_display_clear",
    "vmcs_display_enable",
    "pm_set_policy", "pm_get_status", "pm_show_stats",
    "pm_start_logging", "pm_stop_logging",
};

static int vc_gencmd_known(const char *cmd) {
    size_t n = strcspn(cmd, " \t");
    for (size_t i = 0; i < sizeof vc_gencmd_names / sizeof vc_gencmd_names[0]; i++)
        if (strlen(vc_gencmd_names[i]) == n && !strncmp(cmd, vc_gencmd_names[i], n)) return 1;
    return 0;
}

/* Channel type 1. A known command gets an empty string (success), an
 * unknown command gets the firmware's own error text. Returns the reply
 * length with the NUL. */
static uint32_t vc_handle_gencmd(const char *cmd, char *reply, uint32_t reply_max) {
    if (vc_gencmd_known(cmd)) reply[0] = '\0';
    else snprintf(reply, reply_max, "error=1 error_msg=\"Command not registered\"");
    fprintf(stderr, "vc: gencmd \"%s\" -> \"%s\"\n", cmd, reply);
    return (uint32_t)strlen(reply) + 1;
}

static uint64_t g_presents;             /* surfaces shown: the guest's frame rate */
uint64_t bcm_presents(void) { return g_presents; }

/* the surface on the screen to the app (frameout.c) */
static void send_scanout(void) {
    uint32_t bpp = bcm.scan_format == VC_FORMAT_RGBA32 ? 4 : 2;
    if (!g_dev_replay && bcm.scan_width && bcm.scan_height && bcm.scan_width <= 1024 &&
        bcm.scan_stride >= bcm.scan_width * bpp &&
        (uint64_t)bcm.scan_addr + (uint64_t)bcm.scan_stride * bcm.scan_height <= BCM_MEM_SIZE)
        frameout_send(bcm_mem + bcm.scan_addr, bcm.scan_width, bcm.scan_height, bcm.scan_stride, bpp);
}

/* After a snapshot load the firmware draws only when the screen changes:
 * the app gets the screen of the snapshot once. */
static int g_resend;

static void bcm_scanout(const struct vc_surface *s) {
    g_presents++;
    bcm.scan_addr = s->addr;
    bcm.scan_width = s->width;
    bcm.scan_height = s->height;
    bcm.scan_stride = s->stride;
    bcm.scan_format = s->format;
    bcm.dirty = 1;
    if (!g_dev_replay) mach_key_log_present();
    /* each present goes to the app at once (frameout.c); frame.ppm stays
     * for the tools, at most four times per second */
    send_scanout();
}

/* Channel type 2, the display calls. The reply payload is 16 bytes. */
static uint32_t vc_handle_gfx(uint32_t opcode, const uint8_t *req, uint32_t req_len, uint8_t *reply) {
    memset(reply, 0, 16);
    if (opcode == VC_GFX_CREATE && req_len >= 32) {
        uint32_t format = req[4], width = ld32(req + 8), height = ld32(req + 12);
        uint32_t stride = ld32(req + 16), want = ld32(req + 24);
        uint32_t size = (uint64_t)stride * height > BCM_MEM_SIZE ? 0 : stride * height;
        struct vc_surface *surf = NULL;
        uint32_t handle = 0;                        /* handles start at 1 */
        for (uint32_t i = 0; i < VC_MAX_SURFACES; i++)
            if (!bcm.surface[i].valid) { surf = &bcm.surface[i]; handle = i + 1; break; }
        if (!surf || size == 0) {
            fprintf(stderr, "vc: no room for a %ux%u surface, stride %u\n", width, height, stride);
            return 16;
        }
        if (want == 0) {                            /* 0: the chip selects the address */
            want = bcm.vc_heap;
            bcm.vc_heap = (bcm.vc_heap + size + 15) & ~15u;
        }
        if ((uint64_t)want + size > BCM_MEM_SIZE) {
            fprintf(stderr, "vc: surface at %#x does not fit in chip memory\n", want);
            return 16;
        }
        surf->valid = 1; surf->addr = want; surf->width = width; surf->height = height;
        surf->stride = stride; surf->size = size; surf->format = format;
        st32(reply, handle);
        st32(reply + 4, surf->addr);
        fprintf(stderr, "vc: surface %u = %ux%u fmt %u stride %u at chip %#x\n",
                handle, width, height, format, stride, surf->addr);
    } else if (opcode == VC_GFX_PRESENT && req_len >= 32) {
        uint32_t handle = ld32(req + 12);
        if (handle == 0 || handle > VC_MAX_SURFACES || !bcm.surface[handle - 1].valid) {
            fprintf(stderr, "vc: present of unknown surface %u\n", handle);
            return 16;
        }
        bcm_scanout(&bcm.surface[handle - 1]);
    }
    return 16;
}

/* one message of a channel: put the reply on its VC-to-host ring */
static void vc_dispatch(int ch, uint32_t seq, uint32_t opcode, const uint8_t *payload, uint32_t len) {
    uint8_t reply[VC_MSG_HDR_LEN + VC_MAX_PAYLOAD];
    char text[VC_MAX_PAYLOAD];
    uint32_t reply_len, ctrl = VC_CTRL_OFF(ch);

    if (ch == VC_CH_GENCMD) {
        if (opcode != 1) return;
        uint32_t n = len < sizeof text - 1 ? len : sizeof text - 1;
        memcpy(text, payload, n);
        text[n] = '\0';
        reply_len = vc_handle_gencmd(text, (char *)reply + VC_MSG_HDR_LEN, VC_MAX_PAYLOAD);
    } else if (ch == VC_CH_GFX) {
        reply_len = vc_handle_gfx(opcode, payload, len, reply + VC_MSG_HDR_LEN);
    } else {
        return;
    }
    /* the reply header: same magic and sequence number, result 0 */
    st32(reply, VC_MSG_MAGIC);
    st32(reply + VC_MSG_OFF_SEQ, seq);
    st32(reply + VC_MSG_OFF_OPCODE, 0);
    st16(reply + VC_MSG_OFF_LENGTH, reply_len);
    st16(reply + VC_MSG_OFF_FLAGS, 0);
    uint32_t padded = VC_MSG_HDR_LEN + ((reply_len + 15) & ~15u);
    if (padded > sizeof reply) return;

    uint16_t start = vc_ld16(ctrl + VC_CH_RX_START), end = vc_ld16(ctrl + VC_CH_RX_END);
    uint16_t rd = vc_ld16(ctrl + VC_CH_RX_RD), wr = vc_ld16(ctrl + VC_CH_RX_WR);
    int32_t space = (int32_t)(end - start - 16) - (int32_t)vc_occupancy(start, end, rd, wr);
    if ((int32_t)padded > space) {
        fprintf(stderr, "vc: channel %d reply of %u bytes does not fit in %d\n", ch, padded, space);
        return;
    }
    vc_ring_write(start, end, &wr, reply, padded);
    vc_st16(ctrl + VC_CH_RX_WR, wr);
    *vc_iface(VC_OFF_VC_SIG) ^= 1u << ch;           /* signal ^ acknowledge: channel has data */
}

/* handle all complete messages that the host put on a channel */
static void vc_pump(int ch) {
    uint32_t ctrl = VC_CTRL_OFF(ch);
    if (!bcm.vc_iface_live || vc_ld16(ctrl + VC_CH_TYPE) == 0) return;
    uint16_t start = vc_ld16(ctrl + VC_CH_TX_START), end = vc_ld16(ctrl + VC_CH_TX_END);
    uint16_t rd = vc_ld16(ctrl + VC_CH_TX_RD), wr = vc_ld16(ctrl + VC_CH_TX_WR);

    for (;;) {
        uint8_t hdr[VC_MSG_HDR_LEN], payload[VC_MAX_PAYLOAD];
        uint32_t avail = vc_occupancy(start, end, rd, wr);
        if (avail < VC_MSG_HDR_LEN) break;
        uint16_t peek = rd;
        vc_ring_read(start, end, &peek, hdr, VC_MSG_HDR_LEN);
        if (ld32(hdr) != VC_MSG_MAGIC) {
            fprintf(stderr, "vc: channel %d message without the magic: ring dropped\n", ch);
            rd = wr;
            break;
        }
        uint32_t seq = ld32(hdr + VC_MSG_OFF_SEQ), opcode = ld32(hdr + VC_MSG_OFF_OPCODE);
        uint32_t len = ld16(hdr + VC_MSG_OFF_LENGTH), padded = (len + 15) & ~15u;
        if (avail < VC_MSG_HDR_LEN + padded) break; /* the message is not complete */
        rd = peek;
        if (len > VC_MAX_PAYLOAD) {
            vc_ring_read(start, end, &rd, payload, VC_MAX_PAYLOAD);
            vc_ring_read(start, end, &rd, NULL, padded - VC_MAX_PAYLOAD);
            len = VC_MAX_PAYLOAD;
        } else {
            vc_ring_read(start, end, &rd, payload, padded);
        }
        vc_st16(ctrl + VC_CH_TX_RD, rd);            /* release the space before the answer */
        vc_dispatch(ch, seq, opcode, payload, len);
    }
    vc_st16(ctrl + VC_CH_TX_RD, rd);
    /* acknowledge the host's doorbell bit of this channel */
    *vc_iface(VC_OFF_VC_ACK) = (uint8_t)((*vc_iface(VC_OFF_VC_ACK) & ~(1u << ch))
                                         | (*vc_iface(VC_OFF_HOST_SIG) & (1u << ch)));
}

/* A write landed in chip memory. The write that matters is the host's
 * write pointer of a channel: it makes a message exist. */
static void vc_wrote(uint32_t addr, unsigned size) {
    if (!bcm.vc_iface_live || addr < VC_IFACE_BASE || addr - VC_IFACE_BASE >= VC_TX_OFF(0)) return;
    uint32_t off = addr - VC_IFACE_BASE;
    for (int ch = 0; ch < VC_NUM_CHANNELS; ch++) {
        uint32_t wr = VC_CTRL_OFF(ch) + VC_CH_TX_WR;
        if (off < wr + 2 && off + size > wr) { vc_pump(ch); return; }
    }
}

static void vc_kick(void) {
    for (int ch = 0; ch < VC_NUM_CHANNELS; ch++) vc_pump(ch);
}

static void vc_make_channel(int ch, uint16_t type) {
    uint32_t ctrl = VC_CTRL_OFF(ch);
    vc_st16(ch * 2, ctrl);                          /* the channel offset table */
    memset(vc_iface(ctrl), 0, VC_CTRL_LEN);
    vc_st16(ctrl + VC_CH_TYPE, type);
    vc_st16(ctrl + VC_CH_TX_START, VC_TX_OFF(ch));
    vc_st16(ctrl + VC_CH_TX_END, VC_TX_OFF(ch) + VC_RING_LEN);
    vc_st16(ctrl + VC_CH_RX_START, VC_RX_OFF(ch));
    vc_st16(ctrl + VC_CH_RX_END, VC_RX_OFF(ch) + VC_RING_LEN);
    vc_st16(ctrl + VC_CH_TX_RD, VC_TX_OFF(ch));
    vc_st16(ctrl + VC_CH_TX_WR, VC_TX_OFF(ch));
    vc_st16(ctrl + VC_CH_RX_RD, VC_RX_OFF(ch));
    vc_st16(ctrl + VC_CH_RX_WR, VC_RX_OFF(ch));
}

/* build the interface in chip memory */
static void vc_publish(void) {
    memset(vc_iface(0), 0, VC_TX_OFF(0) + VC_NUM_CHANNELS * 0x1000);
    vc_make_channel(VC_CH_GENCMD, VC_TYPE_GENCMD);
    vc_make_channel(VC_CH_GFX, VC_TYPE_GFX);
    st32(vc_iface(VC_OFF_HOST_SIG), 0);
    st32(vc_iface(VC_OFF_VC_ACK), 0);
    bcm.vc_iface_live = 1;
}

/* ---------- chip memory and the port registers ---------- */

static uint8_t *bcm_mem_ptr(uint32_t addr, unsigned size) {
    if (addr <= BCM_MEM_SIZE - size) return bcm_mem + addr;
    if (addr >= BCM_CTRL_BASE && addr - BCM_CTRL_BASE <= BCM_CTRL_SIZE - size)
        return bcm_ctrl + (addr - BCM_CTRL_BASE);
    return NULL;
}

/* the data port is as wide as the access: a wider store would overwrite what follows */
static void bcm_mem_write(uint32_t addr, uint32_t value, unsigned size) {
    uint8_t *p = bcm_mem_ptr(addr, size);
    if (!p) return;
    if (size == 1) *p = (uint8_t)value;
    else if (size == 2) st16(p, value);
    else st32(p, value);
}

static uint32_t bcm_mem_read(uint32_t addr, unsigned size) {
    uint8_t *p = bcm_mem_ptr(addr, size);
    if (!p) return 0;
    uint32_t value = size == 1 ? *p : size == 2 ? ld16(p) : ld32(p);
    /* bit 0 of the release register reads 1 while the VideoCore runs */
    if (bcm.vc_running && addr == BCM_CTRL_BASE + BCM_VC_RELEASE) value |= BCM_VC_RELEASE_ACK;
    return value;
}

/* the 32-bit word that an access is part of */
static uint32_t bcm_mem_word(uint32_t addr) {
    uint8_t *p = bcm_mem_ptr(addr & ~3u, 4);
    return p ? ld32(p) : 0;
}

/* The address registers are 16 bits wide: the guest writes a 32-bit address
 * as two halfwords, low half first, and the register shifts. */
static uint32_t bcm_addr_shift(uint32_t reg, uint32_t value, unsigned size) {
    return size == 2 ? (reg >> 16) | (value << 16) : value;
}

static void bcm_run_command(uint32_t cmd) {
    if (cmd != BCMCMD_LCD_UPDATE && cmd != BCMCMD_LCD_UPDATERECT) return;
    bcm.dirty = 1;
    st32(bcm_mem + BCMA_COMMAND, BCM_VC_DONE);      /* the chip overwrites the mailbox when it finished */
}

/* chip-side effects of a write, when the full word is in place */
static void bcm_wrote(const struct bcm_port *p, uint32_t addr, unsigned size) {
    if (addr >= BCM_CTRL_BASE) {
        uint32_t off = (addr - BCM_CTRL_BASE) & ~3u, word = bcm_mem_word(addr);
        if (off == BCM_VC_RELEASE) {
            if (word & BCM_VC_RELEASE_GO) bcm.vc_running = 1;
        } else if (off == BCM_VC_DOORBELL) {
            if ((word & BCM_VC_DOORBELL_MASK) == BCM_VC_DOORBELL_MAGIC && bcm.vc_running) {
                /* the driver waits for the mailbox, then reads the interface base beside it */
                st32(bcm_mem + BCMA_COMMAND, BCM_VC_DONE);
                vc_publish();
                st32(bcm_mem + BCMA_STATUS, VC_IFACE_BASE);
            }
        }
        return;
    }
    if ((addr & ~3u) == BCMA_HANDSHAKE && p->wr_start == BCMA_HANDSHAKE) {
        uint32_t word = bcm_mem_word(addr);
        if ((word & BCM_HS_MAGIC_MASK) == BCM_HS_MAGIC && (word & 0xffff) == BCM_HS_START)
            st32(bcm_mem + BCMA_HANDSHAKE, BCM_HS_MAGIC | BCM_HS_RUNNING);
        return;
    }
    /* a command is a write that started at the mailbox, not a bulk transfer that passes it */
    if ((addr & ~3u) == BCMA_COMMAND && p->wr_start == BCMA_COMMAND) {
        bcm_run_command(bcm_mem_word(addr));
        return;
    }
    vc_wrote(addr, size);
}

void bcm_reset(void) {
    if (!bcm_mem) {
        bcm_mem = calloc(1, BCM_MEM_SIZE);
        bcm_ctrl = calloc(1, BCM_CTRL_SIZE);
        if (!bcm_mem || !bcm_ctrl) { perror("bcm: alloc"); exit(1); }
    }
    memset(&bcm, 0, sizeof bcm);
    memset(bcm_mem, 0, BCM_MEM_SIZE);
    memset(bcm_ctrl, 0, BCM_CTRL_SIZE);
    /* The chip starts with its firmware in operation and the interface
     * published, as a boot ROM leaves it: the retailOS looks for the
     * interface before it releases the VideoCore itself. */
    bcm.vc_running = 1;
    bcm.vc_heap = VC_SURFACE_HEAP;
    vc_publish();
    bcm.scan_addr = BCMA_CMDPARAM;
    bcm.scan_width = LCD_WIDTH;
    bcm.scan_height = LCD_HEIGHT;
    bcm.scan_stride = LCD_WIDTH * 2;
    bcm.scan_format = VC_FORMAT_RGB565;
}

int bcm_access(uint32_t addr, int is_write, uint32_t *value, int size) {
    uint32_t offset = addr - BCM_BASE;
    if (!bcm_mem) bcm_reset();
    if (offset >= BCM_MMIO_SIZE) {                  /* no device there */
        if (!is_write) *value = 0;
        return 1;
    }
    unsigned reg = offset >> 16;
    struct bcm_port *p = &bcm.port[reg >= BCM_REG_ALT_DATA ? 1 : 0];
    if (is_write) {
        uint32_t v = *value, a;
        switch (reg) {
        case BCM_REG_DATA: case BCM_REG_ALT_DATA:
            a = p->wr_addr;
            bcm_mem_write(a, v, size);
            p->wr_addr += size;
            bcm_wrote(p, a, size);
            break;
        case BCM_REG_WR_ADDR: case BCM_REG_ALT_WR_ADDR:
            p->wr_addr = bcm_addr_shift(p->wr_addr, v, size);
            p->wr_start = p->wr_addr;
            break;
        case BCM_REG_RD_ADDR: case BCM_REG_ALT_RD_ADDR:
            p->rd_addr = bcm_addr_shift(p->rd_addr, v, size);
            break;
        default:                                    /* CONTROL: bootstrap bytes, or the doorbell */
            if (v == BCM_CONTROL_KICK) vc_kick();
            break;
        }
        return 1;
    }
    switch (reg) {
    case BCM_REG_DATA: case BCM_REG_ALT_DATA:
        *value = bcm_mem_read(p->rd_addr, size);
        p->rd_addr += size;
        break;
    case BCM_REG_RD_ADDR: case BCM_REG_ALT_RD_ADDR:
        *value = 1;                                 /* the chip accepts a new read address */
        break;
    case BCM_REG_WR_ADDR: case BCM_REG_ALT_WR_ADDR:
        *value = p->wr_addr;
        break;
    default:                                        /* CONTROL: always ready */
        *value = BCM_CONTROL_WR_READY | BCM_CONTROL_RD_READY | BCM_CONTROL_BOOT_READY;
        break;
    }
    return 1;
}

/* 1 if single writes to [a, a + n) have no side effect: plain chip memory,
 * not the handshake and mailbox words, not the VideoCore interface (see
 * bcm_wrote()). The control window is above BCM_MEM_SIZE. */
static int bcm_plain(uint32_t a, uint32_t n) {
    if (a > BCM_MEM_SIZE || n > BCM_MEM_SIZE - a) return 0;
    if (a < BCMA_STATUS + 4 && a + n > BCMA_HANDSHAKE) return 0;
    if (a < VC_IFACE_BASE + VC_TX_OFF(0) && a + n > VC_IFACE_BASE) return 0;
    return 1;
}

/* A DMA transfer to a data port: count bytes in units of width. The
 * firmware sends each screen update this way, about 30,000 bytes, through
 * DMA block A. As single writes, each byte went through the whole device
 * path. A transfer to plain chip memory is one copy. Returns 0 if the
 * caller must make single writes: not a data port, or an address with a
 * side effect. */
int bcm_write_burst(uint32_t addr, const uint8_t *data, uint32_t count, unsigned width) {
    uint32_t offset = addr - BCM_BASE;
    if (!bcm_mem) bcm_reset();
    if (offset >= BCM_MMIO_SIZE) return 0;
    unsigned reg = offset >> 16;
    if (reg != BCM_REG_DATA && reg != BCM_REG_ALT_DATA) return 0;
    struct bcm_port *p = &bcm.port[reg == BCM_REG_ALT_DATA ? 1 : 0];
    uint32_t n = count - count % width;
    if (!bcm_plain(p->wr_addr, n)) return 0;
    memcpy(bcm_mem + p->wr_addr, data, n);
    p->wr_addr += n;
    return 1;
}

/* ---------- frame output ---------- */

static const char *rpath(const char *name) {
    static char buf[512];
    const char *e = getenv("RUNNER_TMP");
    snprintf(buf, sizeof buf, "%s%s", (e && *e) ? e : RUNNER_TMP, name);
    return buf;
}

/* RGB565 to 8-bit channels. Tables: the Y1 build has no divide
 * instruction, so three divisions per pixel were 230,000 library calls per
 * frame. With the tables, a frame takes about 4.5 ms on the Y1. */
static uint8_t g_c5[32], g_c6[64];

static void color_tables(void) {
    for (int i = 0; i < 32; i++) g_c5[i] = (uint8_t)(i * 255 / 31);
    for (int i = 0; i < 64; i++) g_c6[i] = (uint8_t)(i * 255 / 63);
}

/* Write the scanout surface as frame.ppm. Returns the number of pixels
 * that are not black, or -1. */
static long bcm_dump_frame(void) {
    uint32_t w = bcm.scan_width, h = bcm.scan_height, stride = bcm.scan_stride;
    uint32_t bpp = bcm.scan_format == VC_FORMAT_RGBA32 ? 4 : 2;
    static uint8_t row[1024 * 3];
    char tmp[512];
    long lit = 0;
    if (w == 0 || h == 0 || w > 1024 || stride < w * bpp ||
        (uint64_t)bcm.scan_addr + (uint64_t)stride * h > BCM_MEM_SIZE) {
        fprintf(stderr, "vc: cannot show a %ux%u surface at %#x, stride %u\n", w, h, bcm.scan_addr, stride);
        return -1;
    }
    if (!g_c5[31]) color_tables();
    snprintf(tmp, sizeof tmp, "%s", rpath("frame.ppm.tmp"));
    FILE *f = fopen(tmp, "wb");
    if (!f) return -1;
    fprintf(f, "P6\n%u %u\n255\n", w, h);
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t *src = bcm_mem + bcm.scan_addr + y * stride;
        for (uint32_t x = 0; x < w; x++) {
            uint8_t r, g, b;
            if (bpp == 4) {                         /* RGBA32: R first in memory */
                r = src[x * 4]; g = src[x * 4 + 1]; b = src[x * 4 + 2];
            } else {
                uint16_t px = ld16(src + x * 2);
                r = g_c5[(px >> 11) & 31];
                g = g_c6[(px >> 5) & 63];
                b = g_c5[px & 31];
            }
            row[x * 3] = r; row[x * 3 + 1] = g; row[x * 3 + 2] = b;
            lit += (r | g | b) != 0;
        }
        fwrite(row, 3, w, f);
    }
    fclose(f);
    rename(tmp, rpath("frame.ppm"));                /* a reader never gets half a file */
    return lit;
}

void bcm_frame_poll(uint64_t now_us) {
    static uint64_t last;
    if (g_resend) { g_resend = 0; send_scanout(); }
    frameout_poll();
    if (!bcm_mem || !bcm.dirty) return;
    if (g_frames && now_us - last < FRAME_MIN_INTERVAL_US) return;
    last = now_us;
    bcm.dirty = 0;
    long lit = bcm_dump_frame();
    if (lit < 0) return;
    g_frames++;
    g_frame_presented = 1;
    fprintf(stderr, "vc: frame %u: %ux%u at chip %#x, %ld of %u pixels lit\n", g_frames,
            bcm.scan_width, bcm.scan_height, bcm.scan_addr, lit, bcm.scan_width * bcm.scan_height);
}

/* ---------- checkpoint ---------- */

int bcm_snapshot_save(FILE *f) {
    uint32_t sizes[3] = { sizeof bcm, BCM_MEM_SIZE, BCM_CTRL_SIZE };
    if (!bcm_mem) return -1;
    if (fwrite(sizes, sizeof sizes, 1, f) != 1) return -1;
    if (fwrite(&bcm, sizeof bcm, 1, f) != 1) return -1;
    if (fwrite(bcm_mem, 1, BCM_MEM_SIZE, f) != BCM_MEM_SIZE) return -1;
    if (fwrite(bcm_ctrl, 1, BCM_CTRL_SIZE, f) != BCM_CTRL_SIZE) return -1;
    return 0;
}

int bcm_snapshot_load(FILE *f) {
    uint32_t sizes[3];
    if (fread(sizes, sizeof sizes, 1, f) != 1) return -1;
    if (sizes[0] != sizeof bcm || sizes[1] != BCM_MEM_SIZE || sizes[2] != BCM_CTRL_SIZE) return -1;
    if (!bcm_mem) bcm_reset();
    if (fread(&bcm, sizeof bcm, 1, f) != 1) return -1;
    if (fread(bcm_mem, 1, BCM_MEM_SIZE, f) != BCM_MEM_SIZE) return -1;
    if (fread(bcm_ctrl, 1, BCM_CTRL_SIZE, f) != BCM_CTRL_SIZE) return -1;
    bcm.dirty = 1;                                  /* show the frame of the snapshot */
    g_resend = 1;
    return 0;
}
