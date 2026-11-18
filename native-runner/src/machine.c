/* native-runner: the machine. Guest memory map, loader, capture and
 * snapshot loading. The devices are in dev_*.c, the CPU side in cpu.c.
 *
 * Guest DRAM is a file, mapped at the guest's own addresses: 0x10000000,
 * and after the firmware's remap also 0x0 and the alias addresses. Device
 * registers stay unmapped, so each access faults into cpu.c, which calls
 * the bus functions in this file.
 */
#define _GNU_SOURCE
#include "runner.h"
#include "machine.h"
#include "dev.h"
#include "frameout.h"
#include "privtrap.h"
#include "pv.h"
#include "jit.h"
#include "vdisk.h"
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#ifdef __ANDROID__
#include <linux/ashmem.h>
#endif

#ifndef RUNNER_TMP
#define RUNNER_TMP "/data/local/tmp/"
#endif

#define LOW_WINDOW  0x04000000u     /* after the remap, DRAM is also at 0x0-0x4000000 */
#define LOW_ALIAS   0x04000000u     /* bit-26 alias of the low window: the same DRAM */
#define PAGE0_END   0x1000u         /* the host cannot map guest page 0 */
#define ALIAS_BIT26 0x04000000u
#define ALIAS_UNCACHED 0x80000000u
#define IRAM_END    (GUEST_IRAM_BASE + GUEST_IRAM_SIZE)
#define NOR_SIZE    0x100000u
#define NOR_HW_REVISION_OFF 0x405c
#define HW_REVISION 0x000b0005u     /* iPod video 5G; IPOD_HWVR changes it (0x000b0010: 5.5G) */
#define SNAP_MAGIC  0x32534e43u     /* "CNS2": with the COP state and the build stamp */
/* A snapshot fits only the runner build that saved it: the device state is
 * saved as raw structs. Another build ignores it and boots cold. */
static const char g_build[32] = __DATE__ " " __TIME__;

struct runner {
    int disk_fd;
};

struct gctx { uint32_t r[16], cpsr; };

static struct runner *g_r;
static int g_cold_boot;                 /* from reset: not from a capture or a snapshot */
static void probe_install(uint32_t *image);

static uint32_t hw_revision(void) {
    const char *e = getenv("IPOD_HWVR");
    return e ? (uint32_t)strtoul(e, NULL, 0) : HW_REVISION;
}
static int g_low_mapped;            /* the low window and the aliases are mapped */
static int g_dram_fd = -1;
static int g_dram_ashmem;               /* 1: DRAM is ashmem, guest-dram.bin is only the snapshot copy */
static uint8_t nor_buf[NOR_SIZE];   /* NOR flash: the low window before the remap */
static int g_snap_saved;
static struct gctx g_snap_ctx;
static struct vcpu g_snap_vcpu;
static int g_have_snap;

static const char *rpath(const char *name) {
    static char buf[512];
    const char *e = getenv("RUNNER_TMP");
    snprintf(buf, sizeof buf, "%s%s", (e && *e) ? e : RUNNER_TMP, name);
    return buf;
}

/* ---------- guest memory ---------- */

/* host address of a guest DRAM address (any alias), or 0 */
static uint8_t *dram_ptr(uint32_t a) {
    if (a & 0x40000000u) return NULL;                   /* IRAM and others: not a DRAM alias */
    uint32_t c = a & ~(ALIAS_BIT26 | ALIAS_UNCACHED);
    if (dev_remapped() && c < LOW_WINDOW) return (uint8_t *)(uintptr_t)(GUEST_DRAM_BASE + c);
    if (c - GUEST_DRAM_BASE < GUEST_DRAM_SIZE) return (uint8_t *)(uintptr_t)c;
    return NULL;
}

/* host address of [addr, addr+size) of guest RAM, or 0 */
static uint8_t *ram_ptr(uint32_t addr, uint32_t size) {
    if (addr >= GUEST_IRAM_BASE && addr + size <= IRAM_END) return (uint8_t *)(uintptr_t)addr;
    uint8_t *first = dram_ptr(addr);
    if (!first || !dram_ptr(addr + size - 1)) return NULL;
    return first;
}

int mach_ram_read(uint32_t addr, void *buf, uint32_t len) {
    uint8_t *p = len ? ram_ptr(addr, len) : NULL;
    if (!p) return -1;
    memcpy(buf, p, len);
    return 0;
}

int mach_ram_write(uint32_t addr, const void *buf, uint32_t len) {
    uint8_t *p = len ? ram_ptr(addr, len) : NULL;
    if (!p) return -1;
    memcpy(p, buf, len);
    return 0;
}

/* 1 for each 64 KB block where the host has guest RAM at the same address */
uint8_t mach_direct[1u << 16];

static void direct_range(uint32_t base, uint32_t size) {
    for (uint32_t b = base >> 16; b < (base + size) >> 16; b++) mach_direct[b] = 1;
}

/* the host mappings changed: IRAM, DRAM, and after the remap the low window and the aliases */
static void update_direct(void) {
    memset(mach_direct, 0, sizeof mach_direct);
    direct_range(GUEST_IRAM_BASE, GUEST_IRAM_SIZE);
    direct_range(GUEST_DRAM_BASE, GUEST_DRAM_SIZE);
    if (!g_low_mapped) return;
    direct_range(0, LOW_WINDOW);                        /* page 0 is excluded by the interpreter */
    direct_range(LOW_ALIAS, GUEST_DRAM_SIZE);
    direct_range(GUEST_DRAM_BASE | ALIAS_BIT26, GUEST_DRAM_SIZE);
    direct_range(GUEST_DRAM_BASE | ALIAS_UNCACHED, GUEST_DRAM_SIZE);
}

static void unmapped_access(uint32_t addr, int is_write) {
    static int n;
    if (n >= 20) return;
    n++;
    fprintf(stderr, "unmapped guest %s at %08x\n", is_write ? "write" : "read", addr);
}

int mach_mem_read(uint32_t addr, int size, uint32_t *val) {
    uint32_t v = 0;
    uint8_t *p;
    if (!dev_remapped() && addr < LOW_WINDOW) {         /* NOR flash */
        if (addr + (uint32_t)size <= NOR_SIZE) memcpy(&v, nor_buf + addr, (size_t)size);
    } else if (dev_decode(addr)) {
        dev_access(addr, 0, &v, size);
    } else if ((p = ram_ptr(addr, (uint32_t)size)) != NULL) {
        memcpy(&v, p, (size_t)size);
    } else {
        unmapped_access(addr, 0);
    }
    *val = v;
    return 0;
}

int mach_mem_write(uint32_t addr, int size, uint32_t val) {
    uint8_t *p;
    if (addr < 0x40u && dev_remapped()) {               /* the exception vectors: a NULL pointer write */
        static int n;
        if (n++ < 20) fprintf(stderr, "guest write to the vectors: %08x = %08x (%d bytes) at pc %08x\n",
                              addr, val, size, dev_access_pc());
    }
    if (!dev_remapped() && addr < LOW_WINDOW) return 0;                    /* NOR flash: no effect */
    if (dev_decode(addr)) dev_access(addr, 1, &val, size);
    else if ((p = ram_ptr(addr, (uint32_t)size)) != NULL) memcpy(p, &val, (size_t)size);
    else unmapped_access(addr, 1);
    return 0;
}

int mach_fetch(uint32_t pc, uint32_t *word) {
    uint8_t *p = ram_ptr(pc & ~3u, 4);
    if (!p) return 0;
    memcpy(word, p, 4);
    return 1;
}

uint32_t mach_exec_alias(uint32_t pc) {
    if (!dev_remapped())
        /* before the remap the firmware runs at the DRAM base: a low address is the same code */
        return (pc >= PAGE0_END && pc < LOW_WINDOW) ? GUEST_DRAM_BASE + pc : 0;
    return pc < PAGE0_END ? pc + LOW_ALIAS : 0;
}

/* The firmware's remap unit put DRAM at address 0: map the low window and
 * the aliases. Page 0 cannot be mapped on the target kernel: cpu.c runs
 * its code through the alias at LOW_ALIAS and emulates data accesses to it. */
void mach_remap_dram(void) {
    static const uint32_t alias[] = { LOW_ALIAS, GUEST_DRAM_BASE | ALIAS_BIT26, GUEST_DRAM_BASE | ALIAS_UNCACHED };
    if (mmap((void *)(uintptr_t)0x0, LOW_WINDOW, PROT_READ | PROT_WRITE | PROT_EXEC,
             MAP_SHARED | MAP_FIXED, g_dram_fd, 0) == MAP_FAILED &&
        mmap((void *)(uintptr_t)PAGE0_END, LOW_WINDOW - PAGE0_END, PROT_READ | PROT_WRITE | PROT_EXEC,
             MAP_SHARED | MAP_FIXED, g_dram_fd, PAGE0_END) == MAP_FAILED) {
        perror("map low window");
        exit(1);
    }
    for (unsigned i = 0; i < sizeof alias / sizeof alias[0]; i++)
        if (mmap((void *)(uintptr_t)alias[i], GUEST_DRAM_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC,
                 MAP_SHARED | MAP_FIXED, g_dram_fd, 0) == MAP_FAILED) {
            perror("map DRAM alias");
            exit(1);
        }
    g_low_mapped = 1;
    update_direct();
}

int mach_cold_boot(void) { return g_cold_boot; }

int mach_disk_fd(void) { return g_r ? g_r->disk_fd : -1; }

void mach_disk_read(uint64_t lba, uint32_t n, uint8_t *buf) {
    static int log = -1;                                /* DISK_LOG=1: each read of the firmware */
    if (log < 0) log = getenv("DISK_LOG") != NULL;
    if (log) fprintf(stderr, "disk: read %llu +%u\n", (unsigned long long)lba, n);
    if (vdisk_active()) { vdisk_read(lba, n, buf); return; }
    int fd = mach_disk_fd();
    memset(buf, 0, (size_t)n * 512);
    if (fd >= 0 && pread(fd, buf, (size_t)n * 512, (off_t)(lba * 512)) < 0)
        fprintf(stderr, "ide: read error at sector %llu\n", (unsigned long long)lba);
}

uint64_t mach_disk_sectors(void) {
    struct stat st;
    if (vdisk_active()) return vdisk_sectors();
    if (mach_disk_fd() >= 0 && fstat(mach_disk_fd(), &st) == 0) return (uint64_t)st.st_size / 512;
    return 0;
}

/* ---------- the virtual disk ----------
 * VDISK=<folder> builds the disk from a folder (vdisk.c), with
 * VDISK_FIRMWARE=<file> (the firmware partition) and VDISK_MANIFEST=<file>
 * (more files). The size and the free space are those of the folder's file
 * system, or VDISK_SIZE_MB and VDISK_FREE_MB. VDISK_OVERLAY=<file> keeps
 * the written sectors from one hibernate to the next cold boot, while the
 * layout stays the same. */
#define OVERLAY_MAGIC 0x31564f56u           /* "VOV1" */

int runner_open_vdisk(struct runner *r) {
    (void)r;
    const char *root = getenv("VDISK");
    struct statfs fs;
    struct vdisk_config cfg = { root, getenv("VDISK_FIRMWARE"), getenv("VDISK_MANIFEST"), 0, 0 };
    if (!root) return -1;
    if (getenv("VDISK_SIZE_MB")) {
        cfg.size_bytes = strtoull(getenv("VDISK_SIZE_MB"), NULL, 0) << 20;
        cfg.free_bytes = getenv("VDISK_FREE_MB") ? strtoull(getenv("VDISK_FREE_MB"), NULL, 0) << 20 : cfg.size_bytes / 2;
    } else if (statfs(root, &fs) == 0) {
        cfg.size_bytes = (uint64_t)fs.f_blocks * (uint64_t)fs.f_bsize;
        cfg.free_bytes = (uint64_t)fs.f_bavail * (uint64_t)fs.f_bsize;
    }
    if (vdisk_open(&cfg)) return -1;
    fprintf(stderr, "vdisk: %s, %llu MB, layout %08x\n", root,
            (unsigned long long)(vdisk_sectors() >> 11), vdisk_layout_id());
    return 0;
}

static void overlay_file(int save) {
    const char *path = getenv("VDISK_OVERLAY");
    uint32_t head[2] = { OVERLAY_MAGIC, vdisk_layout_id() }, got[2];
    if (!path || !vdisk_active()) return;
    if (save) {
        char tmp[512];
        snprintf(tmp, sizeof tmp, "%s.tmp", path);
        FILE *f = fopen(tmp, "wb");
        if (!f) return;
        int bad = snap_rw(f, head, sizeof head, 1) || ide_overlay_snapshot(f, 1);
        if (fclose(f) != 0) bad = 1;
        if (!bad) rename(tmp, path);
        return;
    }
    FILE *f = fopen(path, "rb");
    if (!f) return;
    if (!snap_rw(f, got, sizeof got, 0) && got[0] == head[0] && got[1] == head[1] && !ide_overlay_snapshot(f, 0))
        fprintf(stderr, "vdisk: the written sectors of %s\n", path);
    else
        fprintf(stderr, "vdisk: %s is for another layout, ignored\n", path);
    fclose(f);
}

/* ---------- snapshot ----------
 * guest-dram.bin has DRAM, snap.bin the rest: CPU state, IRAM and the device
 * state. If snap.bin is there at the start, the runner continues from it.
 * With DRAM in ashmem, the snapshot copies DRAM to and from the file. */

/* save != 0: DRAM to guest-dram.bin, else back. Returns 0 on success. */
static int dram_copy(int save) {
    FILE *f = fopen(rpath("guest-dram.bin"), save ? "wb" : "rb");
    if (!f) return -1;
    int bad = snap_rw(f, (void *)(uintptr_t)GUEST_DRAM_BASE, GUEST_DRAM_SIZE, save);
    if (fclose(f) != 0) bad = 1;
    return bad ? -1 : 0;
}

static void snapshot_save(const struct arm_cpu *c, const struct vcpu *v) {
    struct gctx ctx;
    uint32_t magic = SNAP_MAGIC;
    FILE *f;
    g_snap_saved = 1;
    for (int i = 0; i < 16; i++) ctx.r[i] = c->r[i];
    ctx.cpsr = c->cpsr;
    if (!(f = fopen(rpath("snap.bin.tmp"), "wb"))) return;
    struct vcpu vv = *v;
    char build[sizeof g_build];
    uint64_t guest_us = cpu_guest_time_us();
    uint32_t layout = vdisk_layout_id();
    memcpy(build, g_build, sizeof build);
    int bad = snap_rw(f, &magic, sizeof magic, 1) || snap_rw(f, build, sizeof build, 1) ||
              snap_rw(f, &guest_us, sizeof guest_us, 1) || snap_rw(f, &layout, sizeof layout, 1) ||
              snap_rw(f, &ctx, sizeof ctx, 1) || snap_rw(f, &vv, sizeof vv, 1) ||
              snap_rw(f, (void *)(uintptr_t)GUEST_IRAM_BASE, GUEST_IRAM_SIZE, 1) || dev_snapshot(f, 1) ||
              cpu_cop_snapshot(f, 1);
    if (fflush(f) != 0 || fsync(fileno(f)) != 0) bad = 1;
    fclose(f);
    if (!bad) {
        if (g_dram_ashmem) bad = dram_copy(1);
        else fsync(g_dram_fd);
    }
    if (bad) { fprintf(stderr, "snapshot: write error\n"); return; }
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s", rpath("snap.bin.tmp"));
    rename(tmp, rpath("snap.bin"));
    fprintf(stderr, "snapshot saved (pc=%08x cpsr=%08x)\n", c->r[15], vcpu_cpsr(v, c));
}

/* IRAM must be mapped. Returns 1 if the snapshot was loaded. */
static int snapshot_load(void) {
    uint32_t magic;
    FILE *f = fopen(rpath("snap.bin"), "rb");
    if (!f) return 0;
    char build[sizeof g_build];
    uint64_t guest_us = 0;
    uint32_t layout = 0;
    int ok = !snap_rw(f, &magic, sizeof magic, 0) && magic == SNAP_MAGIC &&
             !snap_rw(f, build, sizeof build, 0) && !memcmp(build, g_build, sizeof build) &&
             !snap_rw(f, &guest_us, sizeof guest_us, 0) && (cpu_set_guest_time_us(guest_us), 1) &&
             !snap_rw(f, &layout, sizeof layout, 0) && layout == vdisk_layout_id() &&
             !snap_rw(f, &g_snap_ctx, sizeof g_snap_ctx, 0) && !snap_rw(f, &g_snap_vcpu, sizeof g_snap_vcpu, 0) &&
             !snap_rw(f, (void *)(uintptr_t)GUEST_IRAM_BASE, GUEST_IRAM_SIZE, 0) && !dev_snapshot(f, 0) &&
             !cpu_cop_snapshot(f, 0);
    fclose(f);
    if (ok && g_dram_ashmem && dram_copy(0)) ok = 0;
    if (!ok) fprintf(stderr, "snapshot: snap.bin is not usable, ignored\n");
    return ok;
}

/* ---------- load ---------- */

/* Guest DRAM is shared memory: the aliases map the same memory at more than
 * one address. ashmem keeps it in RAM. As a file on /data, the kernel wrote
 * the 64 MB back to flash all the time, and a store to a page under
 * write-back waited for the flash: the audio had stalls of up to 150 ms.
 * DRAM_FILE=1 (or a kernel without ashmem) uses the file guest-dram.bin. */
static int dram_open(void) {
#ifdef ASHMEM_SET_SIZE
    if (!getenv("DRAM_FILE")) {
        int fd = open("/dev/ashmem", O_RDWR);
        if (fd >= 0) {
            char name[ASHMEM_NAME_LEN] = "guest-dram";
            if (ioctl(fd, ASHMEM_SET_NAME, name) == 0 && ioctl(fd, ASHMEM_SET_SIZE, (size_t)GUEST_DRAM_SIZE) == 0) {
                g_dram_ashmem = 1;
                return fd;
            }
            close(fd);
        }
        fprintf(stderr, "loader: no ashmem (%s): DRAM is the file guest-dram.bin\n", strerror(errno));
    }
#endif
    int fd = open(rpath("guest-dram.bin"), O_RDWR | O_CREAT, 0600);
    if (fd < 0 || ftruncate(fd, GUEST_DRAM_SIZE) != 0) return -1;
    return fd;
}

/* Address ranges of the firmware image that hold code (retailOS 5G, the osos
 * build of 2013): the text segment and the load image of the IRAM code.
 * Source: the executed-code coverage of the QEMU reference (a tool of the
 * bring-up, no longer in the repository). The loader replaces privileged instructions only in
 * these ranges: outside them a word that looks like one is data (an
 * example is at 0x5b5634). */
static const struct { uint32_t start, end; } code_ranges[] = {
    { 0x000000, 0x300000 },
    { 0x716000, 0x735a00 },
};

/* The loader patched the code ranges once (privtrap.c, pv.c), so native
 * execution already relies on this: the firmware does not change its code.
 * The JIT then need not compare its blocks there in each new epoch. The
 * same DRAM is at the DRAM base (before the remap), at 0 and at the
 * aliases. Page 0 is left out: the exception vectors can change. */
static void static_code_ranges(void) {
    static const uint32_t base[] = { 0, LOW_ALIAS, GUEST_DRAM_BASE, GUEST_DRAM_BASE | ALIAS_BIT26,
                                     GUEST_DRAM_BASE | ALIAS_UNCACHED };
    for (unsigned b = 0; b < sizeof base / sizeof base[0]; b++)
        for (unsigned i = 0; i < sizeof code_ranges / sizeof code_ranges[0]; i++) {
            uint32_t lo = code_ranges[i].start < PAGE0_END ? PAGE0_END : code_ranges[i].start;
            jit_static_code(base[b] + lo, base[b] + code_ranges[i].end);
        }
}

struct runner *runner_create(void) {
    struct runner *r = calloc(1, sizeof *r);
    if (!r) { perror("alloc"); exit(1); }
    r->disk_fd = -1;
    g_r = r;
    dev_reset();
    return r;
}

int runner_load_osos(struct runner *r, const char *path) {
    const char *qd = getenv("QBOOT_DRAM");              /* a QEMU DRAM capture, not the osos image */
    FILE *f;
    (void)r;
    if (qd) path = qd;
    if ((f = fopen(rpath("nor.bin"), "rb")) != NULL) {
        size_t got = fread(nor_buf, 1, sizeof nor_buf, f);
        fclose(f);
        (void)got;
    }
    uint32_t rev = hw_revision();
    memcpy(nor_buf + NOR_HW_REVISION_OFF, &rev, 4);

    /* main() made sure that no host mapping uses a guest address range */
    if (mmap((void *)(uintptr_t)GUEST_IRAM_BASE, GUEST_IRAM_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC,
             MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == MAP_FAILED) return -1;
    if ((g_dram_fd = dram_open()) < 0) return -1;
    void *p = mmap((void *)(uintptr_t)GUEST_DRAM_BASE, GUEST_DRAM_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_SHARED | MAP_FIXED, g_dram_fd, 0);
    if (p == MAP_FAILED) return -1;
    update_direct();
    static_code_ranges();
    if (pv_init()) fprintf(stderr, "loader: no paravirtual page at %#x\n", PV_BASE);

    if (snapshot_load()) {
        g_have_snap = 1;
        return 0;
    }
    dev_reset();                                        /* a snapshot that failed to load left partial state */
    if (!(f = fopen(path, "rb"))) return -1;
    size_t n = fread(p, 1, GUEST_DRAM_SIZE, f);
    fclose(f);
    /* The firmware's interrupt mask functions run natively (pv.c) */
    if (pv_active() && !(getenv("PV") && !atoi(getenv("PV"))))
        fprintf(stderr, "loader: %d interrupt mask functions paravirtual\n", pv_patch((uint32_t *)p, n / 4));
    /* Replace the privileged instructions of the firmware with trap words (privtrap.h) */
    size_t count = 0;
    for (unsigned i = 0; i < sizeof code_ranges / sizeof code_ranges[0]; i++)
        if (code_ranges[i].end <= n)
            count += priv_patch((uint32_t *)p + code_ranges[i].start / 4,
                                (code_ranges[i].end - code_ranges[i].start) / 4);
    fprintf(stderr, "loader: %zu bytes, %zu privileged instructions replaced with trap words\n", n, count);
    probe_install((uint32_t *)p);
    return n > 0 ? 0 : -1;
}

/* ---------- probes ----------
 * PROBE=addr,addr,...: the loader puts an undefined instruction at each
 * guest code address. The first time the guest gets there, the runner logs
 * it with the guest time, puts the original word back and continues. A
 * debug aid: it shows if and when the guest runs a function. */

#define MAX_PROBES 16
#define PROBE_WORD 0xe7fdeffeu              /* UDF #0xdefe: not a trap word */
static struct { uint32_t addr, word; } g_probe[MAX_PROBES];
static int g_n_probe;

static void probe_install(uint32_t *image) {
    const char *e = getenv("PROBE");
    char *end;
    while (e && *e && g_n_probe < MAX_PROBES) {
        uint32_t a = (uint32_t)strtoul(e, &end, 0) & ~3u;
        if (end == e) break;
        if (a < 0x300000u) {
            g_probe[g_n_probe].addr = a;
            g_probe[g_n_probe].word = image[a / 4];
            image[a / 4] = PROBE_WORD;
            g_n_probe++;
        }
        e = *end == ',' ? end + 1 : end;
    }
    if (g_n_probe) fprintf(stderr, "probe: %d addresses\n", g_n_probe);
}

int mach_probe_hit(uint32_t pc, uint32_t lr) {
    uint32_t a = pc & 0x03ffffffu;                      /* any alias of the low window */
    if (a >= GUEST_DRAM_BASE) a -= GUEST_DRAM_BASE;
    for (int i = 0; i < g_n_probe; i++) {
        if (g_probe[i].addr != a || !g_probe[i].word) continue;
        uint32_t *w = (uint32_t *)(uintptr_t)(GUEST_DRAM_BASE + a);
        *w = g_probe[i].word;
        __builtin___clear_cache((char *)w, (char *)(w + 1));
        g_probe[i].word = 0;
        fprintf(stderr, "probe: guest at %08x, lr %08x (guest time %llu ms)\n", a, lr,
                (unsigned long long)(cpu_guest_time_us() / 1000));
        return 1;
    }
    return 0;
}

int runner_load_disk(struct runner *r, const char *path) {
    r->disk_fd = open(path, O_RDWR);
    return r->disk_fd >= 0 ? 0 : -1;
}

/* ---------- QEMU capture (qboot) ---------- */

/* Rebuild the device state of a QEMU capture: replay the device accesses
 * that QEMU logged before the capture point (QEMU_MMIO_LOG, cut at the
 * capture point). Line format: "<addr> R|W <value> <size>". A read of USEC_TIMER
 * gives the timer value at the capture point. Returns the number of read
 * values that differ from the QEMU values, or -1. */
static long qboot_replay(const char *path, uint32_t *usec) {
    FILE *f = fopen(path, "r");
    char line[128];
    unsigned long n = 0;
    long mismatch = 0;
    if (!f) return -1;
    g_dev_replay = 1;
    dev_set_access_pc(0);
    while (fgets(line, sizeof line, f)) {
        unsigned addr, val, size;
        char rw;
        if (sscanf(line, "%x %c %x %u", &addr, &rw, &val, &size) != 4) continue;
        if (addr == 0x60005010 && rw == 'R') { *usec = val; continue; }
        uint32_t v = val, mask = size >= 4 ? 0xffffffffu : (1u << (8 * size)) - 1;
        n++;
        if (rw == 'W') { dev_access(addr, 1, &v, (int)size); continue; }
        v = 0;
        dev_access(addr, 0, &v, (int)size);
        if ((v ^ val) & mask) {
            if (mismatch < 20)
                fprintf(stderr, "qboot: replay line %lu: read %08x gives %08x, QEMU had %08x\n",
                        n, addr, v & mask, val & mask);
            mismatch++;
        }
    }
    fclose(f);
    g_dev_replay = 0;
    fprintf(stderr, "qboot: replayed %lu device accesses, %ld read values differ from QEMU\n", n, mismatch);
    return mismatch;
}

/* QBOOT_CTX (r0-r15, cpsr), QBOOT_BANK (sp/lr of each CPU mode),
 * QBOOT_IRAM (IRAM dump), QBOOT_MMIO (device accesses before the capture).
 * QBOOT_DRAM made runner_load_osos load the DRAM dump. Returns 1 if the
 * capture was loaded. */
static int qboot_load(struct arm_cpu *cpu, struct vcpu *v) {
    static const char *const names[VCPU_BANKS] = { "usr", "fiq", "irq", "svc", "abt", "und" };
    const char *path = getenv("QBOOT_CTX");
    unsigned w[17];
    FILE *f = path ? fopen(path, "r") : NULL;
    if (!f) return 0;
    for (int i = 0; i < 17; i++)
        if (fscanf(f, "%x", &w[i]) != 1) { fclose(f); fprintf(stderr, "qboot: bad context file\n"); exit(1); }
    fclose(f);
    for (int i = 0; i < 16; i++) cpu->r[i] = w[i];
    cpu->cpsr = w[16] & VCPU_HOST_MASK;
    vcpu_reset(v, w[16]);

    if ((path = getenv("QBOOT_BANK")) != NULL && (f = fopen(path, "r")) != NULL) {
        char name[8];
        unsigned sp, lr;
        while (fscanf(f, "%7s %x %x", name, &sp, &lr) == 3)
            for (int b = 0; b < VCPU_BANKS; b++)
                if (!strcmp(name, names[b]) && b != vcpu_bank(v->mode)) { v->sp[b] = sp; v->lr[b] = lr; }
        fclose(f);
    } else {
        fprintf(stderr, "qboot: no QBOOT_BANK: the stack pointers of the other CPU modes are zero\n");
    }
    if ((path = getenv("QBOOT_IRAM")) != NULL && (f = fopen(path, "rb")) != NULL) {
        uint32_t *iram = (uint32_t *)(uintptr_t)GUEST_IRAM_BASE;
        size_t n = fread(iram, 1, GUEST_IRAM_SIZE, f);
        fclose(f);
        fprintf(stderr, "qboot: iram %zu bytes, %zu trap words\n", n, priv_patch(iram, n / 4));
    }
    uint32_t usec = 0;
    if ((path = getenv("QBOOT_MMIO")) == NULL || qboot_replay(path, &usec) < 0)
        fprintf(stderr, "qboot: no QBOOT_MMIO: the devices are in their reset state\n");
    if (!dev_remapped()) {
        fprintf(stderr, "qboot: the capture has no DRAM remap in its device accesses\n");
        exit(1);
    }
    mach_remap_dram();
    timers_resume(usec);
    fprintf(stderr, "qboot: resume pc=%08x cpsr=%08x sp=%08x usec=%08x\n", w[15], w[16], w[13], usec);
    return 1;
}

/* cold boot: what the boot ROM leaves in IRAM for the firmware */
static void cold_boot_iram(void) {
    uint32_t *iw = (uint32_t *)(uintptr_t)GUEST_IRAM_BASE, *gw = (uint32_t *)(uintptr_t)GUEST_DRAM_BASE;
    uint32_t blk = 0x4001fe00 - GUEST_IRAM_BASE;
    for (uint32_t d = 0x40018000; d < 0x40020000; d += 4) {
        uint32_t src = d - 0x3cea234;
        if (src + 4 <= 0x735a00) iw[(d - GUEST_IRAM_BASE) / 4] = gw[src / 4];
    }
    iw[blk / 4 + 0x04 / 4] = 0x100;                       /* SysInfo length */
    iw[blk / 4 + 0x84 / 4] = hw_revision();
    iw[blk / 4 + 0xe0 / 4] = GUEST_DRAM_SIZE;             /* SDRAM size */
    iw[(0x4001ff18 - GUEST_IRAM_BASE) / 4] = 0x53797349;  /* "IsyS" */
    iw[(0x4001ff1c - GUEST_IRAM_BASE) / 4] = 0x4001fe00;  /* pointer to the block */
}

static void on_term(int sig);

void runner_run(struct runner *r) {
    struct arm_cpu cpu;
    struct vcpu v;
    struct sigaction sa;
    (void)r;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_term;
    sa.sa_flags = SA_ONSTACK | SA_RESTART;              /* it can come while guest code runs natively */
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    memset(&cpu, 0, sizeof cpu);
    if (g_have_snap) {
        for (int i = 0; i < 16; i++) cpu.r[i] = g_snap_ctx.r[i];
        cpu.cpsr = g_snap_ctx.cpsr & VCPU_HOST_MASK;
        v = g_snap_vcpu;
        if (dev_remapped()) mach_remap_dram();
        fprintf(stderr, "runner: resume from snapshot pc=%08x\n", cpu.r[15]);
    } else if (qboot_load(&cpu, &v)) {
        /* qboot_load set the state */
    } else {
        cold_boot_iram();
        g_cold_boot = 1;
        overlay_file(0);
        cpu.r[15] = GUEST_DRAM_BASE;                    /* reset state of the core, at the image start */
        vcpu_reset(&v, VCPU_SVC | VCPU_I | VCPU_F);
        timers_resume(0);
        fprintf(stderr, "runner: cold boot at %08x\n", cpu.r[15]);
    }
    cpu_run(&cpu, &v);
}

void runner_destroy(struct runner *r) {
    if (r->disk_fd >= 0) close(r->disk_fd);
    free(r);
}

/* ---------- housekeeping ---------- */

/* Key input from stdin and from the file "keys" (the UI front end writes
 * there): see wheel_key() for the characters. */
/* KEY_LOG=1: the time from a key to the next present */
static int g_key_log = -1;
static uint64_t g_key_ns;                               /* the host time of a key that no present answered */

static uint64_t host_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

/* The clicker ticked. The value is the length of the click in the
 * firmware's audio: 11.4 ms (502 samples at 44.1 kHz, measured). The app
 * does not use the event now (it vibrated for each click before). */
#define CLICK_US 11380u

void mach_click(void) {
    if (!g_dev_replay) frameout_event(FRAMEOUT_EVENT_CLICK, CLICK_US);
}

/* The headphone volume of the codec: the app applies it to its audio track,
 * at once. A scale of the samples here would come as late as the app's
 * audio buffer (0.5 s or more). */
void mach_volume(uint32_t left_cb, uint32_t right_cb) {
    fprintf(stderr, "codec: volume %u/%u cB\n", left_cb, right_cb);
    if (!g_dev_replay) frameout_state(FRAMEOUT_EVENT_VOLUME, (left_cb & 0xffffu) | (right_cb & 0xffffu) << 16);
}

/* The backlight (dev_backlight.c): the app sets the screen brightness. A
 * state of the frame stream: a new reader gets the last level. */
void mach_backlight(int level) {
    if (level == 0 || level == 32) fprintf(stderr, "backlight: level %d\n", level);
    if (!g_dev_replay) frameout_state(FRAMEOUT_EVENT_BACKLIGHT, (uint32_t)level);
}

void mach_key_log_present(void) {
    if (!g_key_ns) return;
    fprintf(stderr, "key: present %llu ms after the key\n", (unsigned long long)((host_ns() - g_key_ns) / 1000000u));
    g_key_ns = 0;
}

/* Hibernate: save a snapshot and exit, as a sleeping iPod keeps its state.
 * The next start resumes from it (snapshot_load()). SIGTERM asks for it,
 * as does the input byte 'Z', and with HIBERNATE_ON_EOF=1 the end of stdin.
 * The app uses SIGTERM: su kept the stdin of a runner open for about two
 * minutes after its app was gone. */
static volatile sig_atomic_t g_hibernate;

static void on_term(int sig) {
    (void)sig;
    g_hibernate = 1;                                    /* the next housekeeping saves and exits */
}

static void poll_input(void) {
    static int nonblock, pressed, on_eof = -1;
    if (on_eof < 0) on_eof = getenv("HIBERNATE_ON_EOF") && atoi(getenv("HIBERNATE_ON_EOF"));
    if (g_key_log < 0) g_key_log = getenv("KEY_LOG") != NULL;
    char buf[64];
    ssize_t n;
    if (!nonblock) {
        int fl = fcntl(0, F_GETFL, 0);
        if (fl >= 0) fcntl(0, F_SETFL, fl | O_NONBLOCK);
        nonblock = 1;
    }
    if (pressed) { wheel_key(0); pressed = 0; }         /* release the button of the last call */
    n = read(0, buf, sizeof buf);
    if (n == 0 && on_eof) g_hibernate = 1;
    if (n <= 0) {
        int kf = open(rpath("keys"), O_RDONLY | O_NONBLOCK);
        if (kf < 0) return;
        n = read(kf, buf, sizeof buf);
        close(kf);
    }
    for (ssize_t i = 0; i < n; i++) {
        if (buf[i] == 'Z') { g_hibernate = 1; continue; }
        wheel_key(buf[i]);
        pressed = 1;
    }
    if (n > 0 && g_key_log && !g_key_ns) g_key_ns = host_ns();
}

void mach_housekeeping(const struct arm_cpu *c, const struct vcpu *v) {
    poll_input();
    bcm_frame_poll(cpu_guest_time_us());
    if (!g_snap_saved && g_frame_presented && getenv("SNAPSHOT")) snapshot_save(c, v);
    if (g_hibernate) {
        if (g_frame_presented) {                        /* not during the boot: a cold boot is as good */
            snapshot_save(c, v);
            overlay_file(1);
        }
        fprintf(stderr, "runner: hibernated\n");
        exit(0);
    }
}
