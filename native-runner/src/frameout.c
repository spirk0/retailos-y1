/* The frame stream to the app.
 *
 * The app showed frame.ppm, which the runner wrote at most four times per
 * second and the app read every 100 ms: animations lost most frames, and a
 * key showed its effect up to about 350 ms late. Now each present goes at
 * once to the named pipe frames.fifo (in RUNNER_TMP, mode 0666, so that the
 * app can read it without su). A pipe keeps the data in memory: no writes
 * to the flash. FRAMES_FIFO=<path> gives another name: the app takes a new
 * one for each runner, so that it cannot open the pipe of an earlier
 * runner, which no writer opens again.
 *
 * A message is FRAMEOUT_HEADER bytes (magic "IPF1", width and height as 16
 * bit, a sequence number, 4 reserved) and then the pixels in RGB565, little
 * endian, row by row. The pipe gets 1 MB if the kernel allows it, so that a
 * frame fits in one write. The writer never blocks: a message that the pipe
 * cannot take waits (frameout_poll), and while it waits a newer frame takes
 * the place of an older one that did not start yet. The reader gets whole
 * messages only. Without a reader the frames are dropped. Events (a click
 * of the clicker, a volume change) are header-only messages: they
 * go before the next frame, after the one that is in the pipe.
 */
#define _GNU_SOURCE
#include "frameout.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#ifndef F_SETPIPE_SZ
#define F_SETPIPE_SZ 1031
#endif
#ifndef RUNNER_TMP
#define RUNNER_TMP "/data/local/tmp/"   /* as machine.c and dev_bcm.c */
#endif
#define PIPE_BYTES  (1u << 20)
#define MAX_PIXELS  (1024u * 768u)

struct msg {
    uint8_t *buf;
    uint32_t len, off;                  /* off: bytes written; the message waits while off < len */
};

static struct msg g_cur, g_next;        /* the message in the pipe, the newest frame after it */
#define MAX_EVENTS 16
static uint8_t g_events[MAX_EVENTS][FRAMEOUT_HEADER];
static unsigned g_n_events;                 /* events that wait, oldest first */
static int g_fd = -1, g_made;
static uint32_t g_seq;
static uint32_t g_state[FRAMEOUT_STATES];
static uint8_t g_state_set[FRAMEOUT_STATES];
static int g_any_state;
static void queue_event(uint32_t kind, uint32_t value);

static const char *fifo_path(void) {
    static char buf[512];
    const char *e = getenv("RUNNER_TMP"), *f = getenv("FRAMES_FIFO");
    if (f && *f) snprintf(buf, sizeof buf, "%s", f);
    else snprintf(buf, sizeof buf, "%sframes.fifo", (e && *e) ? e : RUNNER_TMP);
    return buf;
}

/* make the pipe once; open it when a reader is there */
static int writer(void) {
    if (g_fd >= 0) return 1;
    if (!g_made) {
        g_made = 1;
        signal(SIGPIPE, SIG_IGN);                       /* a reader that leaves must not end the runner */
        unlink(fifo_path());
        if (mkfifo(fifo_path(), 0666) == 0) chmod(fifo_path(), 0666);
        else fprintf(stderr, "frameout: cannot make %s\n", fifo_path());
    }
    g_fd = open(fifo_path(), O_WRONLY | O_NONBLOCK);    /* ENXIO: no reader yet */
    if (g_fd < 0) return 0;
    fcntl(g_fd, F_SETPIPE_SZ, (int)PIPE_BYTES);
    g_cur.off = g_cur.len = 0;                          /* a new reader starts with a whole message */
    g_n_events = 0;                                     /* events for the reader that left */
    for (uint32_t k = 0; k < FRAMEOUT_STATES; k++)      /* the new reader gets the states first */
        if (g_state_set[k]) queue_event(k, g_state[k]);
    return 1;
}

static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }

static int build(struct msg *m, const uint8_t *pixels, uint32_t w, uint32_t h, uint32_t stride, uint32_t bpp) {
    uint32_t len = FRAMEOUT_HEADER + w * h * 2;
    if (!m->buf) m->buf = malloc(FRAMEOUT_HEADER + MAX_PIXELS * 2);
    if (!m->buf || w * h > MAX_PIXELS) return 0;
    uint8_t *p = m->buf;
    memset(p, 0, FRAMEOUT_HEADER);
    put32(p, FRAMEOUT_MAGIC);
    put16(p + 4, w);
    put16(p + 6, h);
    put32(p + 8, ++g_seq);
    p += FRAMEOUT_HEADER;
    for (uint32_t y = 0; y < h; y++, p += w * 2) {
        const uint8_t *row = pixels + (size_t)y * stride;
        if (bpp == 2) { memcpy(p, row, w * 2); continue; }
        for (uint32_t x = 0; x < w; x++) {               /* RGBA32: R first in memory */
            const uint8_t *s = row + x * 4;
            put16(p + x * 2, (uint32_t)(s[0] >> 3) << 11 | (uint32_t)(s[1] >> 2) << 5 | (uint32_t)(s[2] >> 3));
        }
    }
    m->len = len;
    m->off = 0;
    return 1;
}

/* the events, while no frame is half in the pipe: each one fits in one
 * write (a write of up to PIPE_BUF bytes is whole or nothing) */
static int flush_events(void) {
    while (g_n_events) {
        ssize_t n = write(g_fd, g_events[0], FRAMEOUT_HEADER);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && errno == EAGAIN) return 0;
        if (n != (ssize_t)FRAMEOUT_HEADER) { close(g_fd); g_fd = -1; return 0; }
        memmove(g_events[0], g_events[1], (size_t)(--g_n_events) * FRAMEOUT_HEADER);
    }
    return 1;
}

/* write the current message as far as the pipe takes it; then the events, then the next */
static void flush(void) {
    while (g_fd >= 0) {
        if (g_cur.off == g_cur.len) {                   /* the current one is out */
            if (!flush_events()) return;
            if (g_next.len == 0) return;
            struct msg t = g_cur;                       /* the newest frame is next */
            g_cur = g_next;
            g_next = t;
            g_next.len = g_next.off = 0;
        }
        ssize_t n = write(g_fd, g_cur.buf + g_cur.off, g_cur.len - g_cur.off);
        if (n > 0) { g_cur.off += (uint32_t)n; continue; }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && errno == EAGAIN) return;           /* the pipe is full: frameout_poll() goes on */
        close(g_fd);                                    /* the reader left */
        g_fd = -1;
        return;
    }
}

void frameout_send(const uint8_t *pixels, uint32_t width, uint32_t height, uint32_t stride, uint32_t bpp) {
    if (!writer()) return;
    /* a message that did not start yet is replaced; one that started must end first */
    struct msg *m = (g_cur.off == g_cur.len || g_cur.off == 0) ? &g_cur : &g_next;
    if (!build(m, pixels, width, height, stride, bpp)) return;
    if (m == &g_cur) g_next.len = g_next.off = 0;      /* the newest frame is in g_cur */
    flush();
}

void frameout_poll(void) {
    if (g_fd < 0 && g_any_state) writer();              /* a new reader gets the states */
    flush();
}

static void queue_event(uint32_t kind, uint32_t value) {
    if (g_n_events == MAX_EVENTS) return;           /* the reader is slow: drop */
    uint8_t *e = g_events[g_n_events++];
    memset(e, 0, FRAMEOUT_HEADER);
    put32(e, FRAMEOUT_EVENT_MAGIC);
    put16(e + 4, kind);
    put32(e + 8, value);
}

void frameout_event(uint32_t kind, uint32_t value) {
    if (!writer()) return;
    queue_event(kind, value);
    flush();
}

void frameout_state(uint32_t kind, uint32_t value) {
    if (kind < FRAMEOUT_STATES) {
        g_state[kind] = value;
        g_state_set[kind] = 1;
        g_any_state = 1;
    }
    if (g_fd < 0) { writer(); flush(); return; }    /* a new reader gets all states from writer() */
    frameout_event(kind, value);
}
