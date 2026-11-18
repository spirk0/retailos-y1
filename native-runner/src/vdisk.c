/* The virtual disk: the iPod's disk, built from a folder.
 *
 * Layout, as the disk of an iPod:
 *   sector 0        partition table: 1 = the firmware (type 0) at 63,
 *                   2 = FAT32 (type 0x0b) from PART_LBA to the end
 *   63 ...          the firmware partition: the Firmware-* file of the IPSW
 *   PART_LBA ...    the FAT32 volume
 *
 * The volume is made from the folder tree of the configuration, and the
 * lines of the manifest add more files (the music, at the short names of
 * iTunes). Nothing is copied: a read of file data reads the host file, and
 * the boot sector, the FAT and the folder entries are made in memory. Each
 * file and folder has one contiguous run of clusters. The volume has the
 * size of the configuration; the space that the volume does not use and
 * that is not free (other data on the host) is marked as bad clusters, so
 * the iPod shows the free space of the host.
 *
 * The volume is read only here. The IDE model keeps the guest's writes in
 * its overlay (dev_ide.c), which the snapshot saves. The layout id says
 * whether written sectors still fit: it changes with each name, size and
 * date, and with the free space in steps of 1/64 of the disk.
 */
#define _GNU_SOURCE
#include "vdisk.h"
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define SECTOR          512u
#define FW_LBA          63u
#define PART_LBA        32768u
#define RESERVED        32u
#define NFATS           2u
#define FSINFO_SECTOR   1u
#define BACKUP_SECTOR   6u
#define DIRENT          32u
#define EOC             0x0fffffffu
#define BAD_CLUSTER     0x0ffffff7u
#define MIN_CLUSTERS    65525u          /* fewer is FAT16 */
#define FREE_STEPS      64u             /* the free space is rounded down to 1/64 of the disk */
#define FD_CACHE        8

struct node {
    char *name;                 /* the long name (UTF-8), NULL for the root */
    uint8_t sfn[11];            /* the 8.3 name, padded with spaces */
    int lfn;                    /* 1: long-name entries before the 8.3 entry */
    int is_dir;
    char *host;                 /* a file: the host file */
    uint64_t size;
    time_t mtime;
    uint32_t first, nclusters;
    struct node *parent, *child, *next;
    uint8_t *dir;               /* a folder: its entries, nclusters clusters */
};

struct run { uint32_t first, n; struct node *node; };

static struct {
    int active;
    uint64_t sectors;
    int fw_fd;
    uint32_t fw_sectors;
    uint32_t vol_sectors, spc, fat_sectors, clusters;
    uint32_t alloc_end, bad_end;            /* clusters [alloc_end, bad_end) are bad */
    struct node *root;
    struct run *runs;
    uint32_t nruns, runs_cap;
    uint32_t layout, serial;
    struct { struct node *node; int fd; unsigned age; } fds[FD_CACHE];
    unsigned age;
} vd = { .fw_fd = -1 };

static uint32_t cluster_bytes(void) { return vd.spc * SECTOR; }

/* ---------- the tree ---------- */

static struct node *node_new(struct node *parent, const char *name, int is_dir) {
    struct node *n = calloc(1, sizeof *n);
    if (!n) return NULL;
    n->name = name ? strdup(name) : NULL;
    n->is_dir = is_dir;
    n->parent = parent;
    if (parent) {                                       /* sorted by name: the same layout for the same folder */
        struct node **p = &parent->child;
        while (*p && strcmp((*p)->name, name) < 0) p = &(*p)->next;
        n->next = *p;
        *p = n;
    }
    return n;
}

static struct node *find_child(struct node *dir, const char *name) {
    for (struct node *c = dir->child; c; c = c->next)
        if (!strcasecmp(c->name, name)) return c;       /* FAT names do not depend on case */
    return NULL;
}

static void node_free(struct node *n) {
    while (n) {
        struct node *next = n->next;
        node_free(n->child);
        free(n->name);
        free(n->host);
        free(n->dir);
        free(n);
        n = next;
    }
}

static void set_file(struct node *n, const char *host, const struct stat *st) {
    free(n->host);
    n->host = strdup(host);
    n->size = (uint64_t)st->st_size;
    n->mtime = st->st_mtime;
}

static void scan(const char *path, struct node *dir) {
    DIR *d = opendir(path);
    struct dirent *e;
    if (!d) return;
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char *p;
        struct stat st;
        if (asprintf(&p, "%s/%s", path, e->d_name) < 0) continue;
        if (stat(p, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                struct node *n = node_new(dir, e->d_name, 1);
                if (n) { n->mtime = st.st_mtime; scan(p, n); }
            } else if (S_ISREG(st.st_mode)) {
                struct node *n = node_new(dir, e->d_name, 0);
                if (n) set_file(n, p, &st);
            }
        }
        free(p);
    }
    closedir(d);
}

/* one manifest line: "path in the volume<TAB>host file" */
static void manifest_line(char *line) {
    char *tab = strchr(line, '\t');
    if (!tab) return;
    *tab = 0;
    char *host = tab + 1;
    host[strcspn(host, "\r\n")] = 0;
    struct stat st;
    if (stat(host, &st) != 0 || !S_ISREG(st.st_mode)) return;
    struct node *dir = vd.root;
    char *save = NULL;
    char *part = strtok_r(line, "/", &save);
    while (part) {
        char *next = strtok_r(NULL, "/", &save);
        struct node *c = find_child(dir, part);
        if (next) {                                     /* a folder on the way */
            if (!c) { c = node_new(dir, part, 1); if (!c) return; c->mtime = st.st_mtime; }
            if (!c->is_dir) return;
            dir = c;
        } else {
            if (c && c->is_dir) return;
            if (!c) c = node_new(dir, part, 0);
            if (c) set_file(c, host, &st);
        }
        part = next;
    }
}

static void read_manifest(const char *path) {
    FILE *f = path ? fopen(path, "r") : NULL;
    char line[2048];
    if (!f) return;
    while (fgets(line, sizeof line, f)) manifest_line(line);
    fclose(f);
}

/* ---------- 8.3 names ---------- */

static int sfn_valid(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || (c && strchr("!#$%&'()-@^_`{}~", c));
}

/* The 8.3 name of n. Without a change from the long name (upper case, 8.3)
 * no long-name entries; else "BASE~N.EXT", unique among the earlier
 * children of the folder. */
static void make_sfn(struct node *n) {
    char base[16] = "", ext[4] = "";
    const char *name = n->name, *dot = strrchr(name, '.');
    if (dot == name) dot = NULL;                        /* ".name": no extension */
    int exact = strlen(name) <= 12, nb = 0, ne = 0;
    for (const char *p = name; *p && (!dot || p < dot); p++) {
        unsigned char c = (unsigned char)*p;
        if (c == ' ' || c == '.') { exact = 0; continue; }
        if (c >= 'a' && c <= 'z') { c = (unsigned char)(c - 32); exact = 0; }
        if (!sfn_valid(c)) { c = '_'; exact = 0; }
        if (nb < 8) base[nb++] = (char)c; else exact = 0;
    }
    for (const char *p = dot ? dot + 1 : ""; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == ' ' || c == '.') { exact = 0; continue; }
        if (c >= 'a' && c <= 'z') { c = (unsigned char)(c - 32); exact = 0; }
        if (!sfn_valid(c)) { c = '_'; exact = 0; }
        if (ne < 3) ext[ne++] = (char)c; else exact = 0;
    }
    if (!nb) { base[0] = '_'; nb = 1; exact = 0; }
    for (int tail = 0; tail < 1000000; tail++) {
        uint8_t s[11];
        memset(s, ' ', sizeof s);
        if (tail == 0 && exact) {
            memcpy(s, base, (size_t)nb);
        } else {
            char t[12];
            int tl = snprintf(t, sizeof t, "~%d", tail + 1);
            int keep = nb < 8 - tl ? nb : 8 - tl;
            memcpy(s, base, (size_t)keep);
            memcpy(s + keep, t, (size_t)tl);
        }
        memcpy(s + 8, ext, (size_t)ne);
        int used = 0;
        for (struct node *c = n->parent->child; c && c != n; c = c->next)
            if (!memcmp(c->sfn, s, sizeof s)) { used = 1; break; }
        if (!used) {
            memcpy(n->sfn, s, sizeof s);
            n->lfn = !(tail == 0 && exact);
            return;
        }
    }
}

static void make_sfns(struct node *dir) {
    for (struct node *c = dir->child; c; c = c->next) {
        make_sfn(c);
        if (c->is_dir) make_sfns(c);
    }
}

/* UTF-8 to UTF-16: returns the number of units (at most max) */
static int utf16(const char *s, uint16_t *out, int max) {
    int n = 0;
    const unsigned char *p = (const unsigned char *)s;
    while (*p && n < max) {
        uint32_t c = *p++;
        if (c >= 0xf0 && p[0] && p[1] && p[2]) { c = (c & 7) << 18 | (p[0] & 0x3fu) << 12 | (p[1] & 0x3fu) << 6 | (p[2] & 0x3fu); p += 3; }
        else if (c >= 0xe0 && p[0] && p[1]) { c = (c & 15) << 12 | (p[0] & 0x3fu) << 6 | (p[1] & 0x3fu); p += 2; }
        else if (c >= 0xc0 && p[0]) { c = (c & 31) << 6 | (p[0] & 0x3fu); p += 1; }
        if (c >= 0x10000) {
            if (n + 2 > max) break;
            c -= 0x10000;
            out[n++] = (uint16_t)(0xd800 | (c >> 10));
            out[n++] = (uint16_t)(0xdc00 | (c & 0x3ff));
        } else {
            out[n++] = (uint16_t)c;
        }
    }
    return n;
}

#define LFN_MAX 255

static int lfn_entries(const struct node *n) {
    uint16_t u[LFN_MAX];
    return n->lfn ? (utf16(n->name, u, LFN_MAX) + 12) / 13 : 0;
}

/* ---------- clusters ---------- */

static int add_run(struct node *n) {
    if (vd.nruns == vd.runs_cap) {
        uint32_t cap = vd.runs_cap ? vd.runs_cap * 2 : 256;
        struct run *r = realloc(vd.runs, cap * sizeof *r);
        if (!r) return -1;
        vd.runs = r;
        vd.runs_cap = cap;
    }
    vd.runs[vd.nruns].first = n->first;
    vd.runs[vd.nruns].n = n->nclusters;
    vd.runs[vd.nruns].node = n;
    vd.nruns++;
    return 0;
}

static uint32_t dir_entries(const struct node *dir) {
    uint32_t e = dir->parent ? 2 : 1;                   /* "." and "..", or the volume label */
    for (const struct node *c = dir->child; c; c = c->next) e += (uint32_t)lfn_entries(c) + 1;
    return e;
}

/* clusters in tree order, the root first: returns -1 if the volume is too small */
static int allocate(struct node *n) {
    uint64_t bytes = n->is_dir ? (uint64_t)dir_entries(n) * DIRENT : n->size;
    uint64_t nc = (bytes + cluster_bytes() - 1) / cluster_bytes();
    if (n->is_dir && !nc) nc = 1;
    if (nc) {
        if (vd.alloc_end + nc > vd.clusters + 2) return -1;
        n->first = vd.alloc_end;
        n->nclusters = (uint32_t)nc;
        vd.alloc_end += (uint32_t)nc;
        if (add_run(n)) return -1;
    }
    for (struct node *c = n->child; c; c = c->next)
        if (allocate(c)) return -1;
    return 0;
}

static struct run *find_run(uint32_t c) {
    uint32_t lo = 0, hi = vd.nruns;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (c < vd.runs[mid].first) hi = mid;
        else if (c >= vd.runs[mid].first + vd.runs[mid].n) lo = mid + 1;
        else return &vd.runs[mid];
    }
    return NULL;
}

static uint32_t fat_entry(uint32_t c) {
    if (c == 0) return 0x0ffffff8u;                     /* the media byte */
    if (c == 1) return EOC;
    if (c < vd.alloc_end) {
        struct run *r = find_run(c);
        if (!r) return 0;
        return c + 1 < r->first + r->n ? c + 1 : EOC;
    }
    if (c < vd.bad_end) return BAD_CLUSTER;
    return 0;
}

/* ---------- folder entries ---------- */

static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }

static void fat_time(time_t t, uint16_t *date, uint16_t *tim) {
    struct tm tm;
    gmtime_r(&t, &tm);
    if (tm.tm_year < 80) { *date = (1 << 5) | 1; *tim = 0; return; }
    *date = (uint16_t)((tm.tm_year - 80) << 9 | (tm.tm_mon + 1) << 5 | tm.tm_mday);
    *tim = (uint16_t)(tm.tm_hour << 11 | tm.tm_min << 5 | tm.tm_sec / 2);
}

static void sfn_entry(uint8_t *e, const uint8_t name[11], uint8_t attr, uint32_t first, uint32_t size, time_t mtime) {
    uint16_t date, tim;
    memset(e, 0, DIRENT);
    memcpy(e, name, 11);
    e[11] = attr;
    fat_time(mtime, &date, &tim);
    put16(e + 14, tim);
    put16(e + 16, date);
    put16(e + 18, date);
    put16(e + 20, first >> 16);
    put16(e + 22, tim);
    put16(e + 24, date);
    put16(e + 26, first & 0xffff);
    put32(e + 28, size);
}

static uint8_t sfn_checksum(const uint8_t name[11]) {
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++) sum = (uint8_t)(((sum & 1) << 7) + (sum >> 1) + name[i]);
    return sum;
}

static const int lfn_pos[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };

static uint8_t *lfn_put(uint8_t *e, const struct node *n) {
    uint16_t u[LFN_MAX];
    int len = utf16(n->name, u, LFN_MAX), count = (len + 12) / 13;
    uint8_t sum = sfn_checksum(n->sfn);
    for (int k = count; k >= 1; k--, e += DIRENT) {
        memset(e, 0, DIRENT);
        e[0] = (uint8_t)(k | (k == count ? 0x40 : 0));
        e[11] = 0x0f;
        e[13] = sum;
        for (int i = 0; i < 13; i++) {
            int at = (k - 1) * 13 + i;
            uint16_t ch = at < len ? u[at] : at == len ? 0x0000 : 0xffff;
            put16(e + lfn_pos[i], ch);
        }
    }
    return e;
}

static int make_dir(struct node *dir) {
    size_t bytes = (size_t)dir->nclusters * cluster_bytes();
    uint8_t *e = calloc(1, bytes);
    if (!e) return -1;
    dir->dir = e;
    if (dir->parent) {
        uint8_t dot[11], dotdot[11];
        memset(dot, ' ', 11); dot[0] = '.';
        memset(dotdot, ' ', 11); dotdot[0] = dotdot[1] = '.';
        sfn_entry(e, dot, 0x10, dir->first, 0, dir->mtime);
        e += DIRENT;
        sfn_entry(e, dotdot, 0x10, dir->parent->parent ? dir->parent->first : 0, 0, dir->mtime);
        e += DIRENT;
    } else {
        uint8_t label[11];
        memcpy(label, "IPOD       ", 11);
        sfn_entry(e, label, 0x08, 0, 0, 0);
        e += DIRENT;
    }
    for (struct node *c = dir->child; c; c = c->next) {
        if (c->lfn) e = lfn_put(e, c);
        sfn_entry(e, c->sfn, c->is_dir ? 0x10 : 0x20, c->first,
                  c->is_dir ? 0 : (uint32_t)(c->size > 0xffffffffu ? 0xffffffffu : c->size), c->mtime);
        e += DIRENT;
    }
    for (struct node *c = dir->child; c; c = c->next)
        if (c->is_dir && make_dir(c)) return -1;
    return 0;
}

/* ---------- the layout ---------- */

static uint32_t fnv(uint32_t h, const void *p, size_t n) {
    const uint8_t *b = p;
    while (n--) { h ^= *b++; h *= 16777619u; }
    return h;
}

static uint32_t hash_tree(uint32_t h, const struct node *n) {
    for (const struct node *c = n->child; c; c = c->next) {
        int64_t t = (int64_t)c->mtime;
        h = fnv(h, c->name, strlen(c->name) + 1);
        h = fnv(h, &c->is_dir, sizeof c->is_dir);
        if (!c->is_dir) { h = fnv(h, &c->size, sizeof c->size); h = fnv(h, &t, sizeof t); }
        h = hash_tree(h, c);
    }
    return h;
}

/* the cluster size: the largest that still gives a FAT32 volume */
static int geometry(void) {
    static const uint32_t spcs[] = { 64, 32, 16, 8, 4, 2, 1 };
    if (vd.sectors <= PART_LBA + RESERVED + 1024) return -1;
    vd.vol_sectors = (uint32_t)(vd.sectors - PART_LBA < 0xffffffffu ? vd.sectors - PART_LBA : 0xffffffffu);
    for (unsigned i = 0; i < sizeof spcs / sizeof spcs[0]; i++) {
        uint32_t spc = spcs[i], fs = 1;
        for (;;) {
            uint32_t data = vd.vol_sectors - RESERVED - NFATS * fs;
            uint32_t need = (uint32_t)(((uint64_t)data / spc + 2) * 4 + SECTOR - 1) / SECTOR;
            if (need <= fs) break;
            fs = need;
        }
        uint32_t clusters = (vd.vol_sectors - RESERVED - NFATS * fs) / spc;
        if (clusters >= MIN_CLUSTERS + 16 && clusters < 0x0ffffff0u) {
            vd.spc = spc;
            vd.fat_sectors = fs;
            vd.clusters = clusters;
            return 0;
        }
    }
    return -1;
}

int vdisk_open(const struct vdisk_config *cfg) {
    struct stat st;
    vdisk_close();
    vd.sectors = cfg->size_bytes / SECTOR;
    vd.fw_fd = cfg->firmware ? open(cfg->firmware, O_RDONLY) : -1;
    if (vd.fw_fd < 0 || fstat(vd.fw_fd, &st) != 0) goto fail;
    vd.fw_sectors = (uint32_t)((st.st_size + SECTOR - 1) / SECTOR);
    if (FW_LBA + vd.fw_sectors > PART_LBA || geometry()) goto fail;
    vd.root = node_new(NULL, NULL, 1);
    if (!vd.root) goto fail;
    scan(cfg->root, vd.root);
    read_manifest(cfg->manifest);
    make_sfns(vd.root);
    vd.alloc_end = 2;
    if (allocate(vd.root) || make_dir(vd.root)) goto fail;
    uint64_t step = cfg->size_bytes / FREE_STEPS;
    uint64_t want = step ? cfg->free_bytes / step * step / cluster_bytes() : cfg->free_bytes / cluster_bytes();
    uint32_t avail = vd.clusters + 2 - vd.alloc_end;
    vd.bad_end = vd.alloc_end + (want < avail ? avail - (uint32_t)want : 0);
    uint32_t h = 2166136261u;
    h = fnv(h, &cfg->size_bytes, sizeof cfg->size_bytes);
    h = fnv(h, &vd.bad_end, sizeof vd.bad_end);
    h = fnv(h, &vd.fw_sectors, sizeof vd.fw_sectors);
    vd.layout = hash_tree(h, vd.root);
    vd.serial = vd.layout ^ 0x1234abcdu;
    vd.active = 1;
    return 0;
fail:
    vdisk_close();
    return -1;
}

void vdisk_close(void) {
    for (int i = 0; i < FD_CACHE; i++) if (vd.fds[i].node && vd.fds[i].fd >= 0) close(vd.fds[i].fd);
    if (vd.fw_fd >= 0) close(vd.fw_fd);
    node_free(vd.root);
    free(vd.runs);
    memset(&vd, 0, sizeof vd);
    vd.fw_fd = -1;
}

int vdisk_active(void) { return vd.active; }
uint64_t vdisk_sectors(void) { return vd.active ? vd.sectors : 0; }
uint32_t vdisk_layout_id(void) { return vd.layout; }

/* ---------- reads ---------- */

static int host_fd(struct node *n) {
    int oldest = 0;
    for (int i = 0; i < FD_CACHE; i++) {
        if (vd.fds[i].node == n) { vd.fds[i].age = ++vd.age; return vd.fds[i].fd; }
        if (vd.fds[i].age < vd.fds[oldest].age) oldest = i;
    }
    if (vd.fds[oldest].node && vd.fds[oldest].fd >= 0) close(vd.fds[oldest].fd);
    vd.fds[oldest].node = n;
    vd.fds[oldest].fd = open(n->host, O_RDONLY);
    vd.fds[oldest].age = ++vd.age;
    return vd.fds[oldest].fd;
}

static void read_mbr(uint8_t *s) {
    put32(s + 0x1be + 8, FW_LBA);
    put32(s + 0x1be + 12, vd.fw_sectors);               /* type 0: the firmware */
    s[0x1ce + 4] = 0x0b;
    put32(s + 0x1ce + 8, PART_LBA);
    put32(s + 0x1ce + 12, vd.vol_sectors);
    s[510] = 0x55; s[511] = 0xaa;
}

static void read_boot(uint8_t *s) {
    static const uint8_t jump[3] = { 0xeb, 0x58, 0x90 };
    memcpy(s, jump, 3);
    memcpy(s + 3, "MSDOS5.0", 8);
    put16(s + 11, SECTOR);
    s[13] = (uint8_t)vd.spc;
    put16(s + 14, RESERVED);
    s[16] = NFATS;
    s[21] = 0xf8;
    put16(s + 24, 63);
    put16(s + 26, 255);
    put32(s + 28, PART_LBA);
    put32(s + 32, vd.vol_sectors);
    put32(s + 36, vd.fat_sectors);
    put32(s + 44, 2);                                   /* the root folder */
    put16(s + 48, FSINFO_SECTOR);
    put16(s + 50, BACKUP_SECTOR);
    s[64] = 0x80;
    s[66] = 0x29;
    put32(s + 67, vd.serial);
    memcpy(s + 71, "IPOD       ", 11);
    memcpy(s + 82, "FAT32   ", 8);
    s[510] = 0x55; s[511] = 0xaa;
}

static void read_fsinfo(uint8_t *s) {
    uint32_t free_clusters = vd.clusters + 2 - vd.bad_end;
    put32(s, 0x41615252u);
    put32(s + 484, 0x61417272u);
    put32(s + 488, free_clusters);
    put32(s + 492, vd.bad_end);
    put32(s + 508, 0xaa550000u);
}

/* one sector of the volume: v is the sector in the partition */
static void read_volume(uint32_t v, uint8_t *s) {
    if (v < RESERVED) {
        uint32_t r = v >= BACKUP_SECTOR ? v - BACKUP_SECTOR : v;
        if (r == 0) read_boot(s);
        else if (r == FSINFO_SECTOR) read_fsinfo(s);
        return;
    }
    uint32_t f = v - RESERVED;
    if (f < NFATS * vd.fat_sectors) {
        uint32_t c0 = (f % vd.fat_sectors) * (SECTOR / 4);
        for (uint32_t i = 0; i < SECTOR / 4; i++)
            if (c0 + i < vd.clusters + 2) put32(s + 4 * i, fat_entry(c0 + i));
        return;
    }
    uint32_t d = f - NFATS * vd.fat_sectors;
    uint32_t c = d / vd.spc + 2;
    if (c >= vd.alloc_end) return;
    struct run *r = find_run(c);
    if (!r) return;
    uint64_t off = (uint64_t)(c - r->first) * cluster_bytes() + (uint64_t)(d % vd.spc) * SECTOR;
    struct node *n = r->node;
    if (n->is_dir) { memcpy(s, n->dir + off, SECTOR); return; }
    if (off >= n->size) return;
    int fd = host_fd(n);
    if (fd >= 0 && pread(fd, s, SECTOR, (off_t)off) < 0) memset(s, 0, SECTOR);
}

void vdisk_read(uint64_t lba, uint32_t n, uint8_t *buf) {
    memset(buf, 0, (size_t)n * SECTOR);
    if (!vd.active) return;
    for (uint32_t i = 0; i < n; i++) {
        uint64_t a = lba + i;
        uint8_t *s = buf + (size_t)i * SECTOR;
        if (a == 0) read_mbr(s);
        else if (a >= FW_LBA && a < FW_LBA + vd.fw_sectors) {
            if (pread(vd.fw_fd, s, SECTOR, (off_t)((a - FW_LBA) * SECTOR)) < 0) memset(s, 0, SECTOR);
        }
        else if (a >= PART_LBA && a < vd.sectors) read_volume((uint32_t)(a - PART_LBA), s);
    }
}
