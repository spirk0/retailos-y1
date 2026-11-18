/* CLI entry of the native runner.
 *
 *     runner <osos.bin> <disk.img>
 *
 * Environment: see README.md (RUNNER_TMP, QBOOT_*, MMIO_TRACE, NO_SNAPSHOT,
 * RUNNER_NICE).
 */
#include "runner.h"
#include "prio.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/resource.h>

#define MAX_RELAUNCH 30

/* Guest address ranges that the runner maps at fixed addresses. */
static const struct { unsigned long start, end; } guest_ranges[] = {
    { 0x00000000ul, 0x08000000ul },   /* low window and its bit-26 alias */
    { 0x10000000ul, 0x18000000ul },   /* DRAM and its bit-26 alias */
    { 0x40000000ul, 0x40020000ul },   /* IRAM */
    { 0x90000000ul, 0x94000000ul },   /* DRAM, uncached alias */
};

/* 1 if no host mapping (program, libraries, heap) is in a guest range */
static int guest_ranges_free(void) {
    FILE *f = fopen("/proc/self/maps", "r");
    char line[256];
    int ok = 1;
    if (!f) return 1;
    while (fgets(line, sizeof line, f)) {
        unsigned long a, b;
        if (sscanf(line, "%lx-%lx", &a, &b) != 2) continue;
        for (unsigned i = 0; i < sizeof guest_ranges / sizeof guest_ranges[0]; i++)
            if (a < guest_ranges[i].end && b > guest_ranges[i].start) ok = 0;
    }
    fclose(f);
    return ok;
}

int main(int argc, char **argv) {
    setbuf(stderr, NULL);
    if (argc < 2) {
        fprintf(stderr, "usage: %s <osos.bin> [disk.img]\n", argv[0]);
        return 2;
    }
    /* The host loader puts the program and its libraries at random addresses.
     * If one of them is in a guest range, start again for a new layout. */
    if (!guest_ranges_free()) {
        const char *e = getenv("RUNNER_RELAUNCH");
        int n = e ? atoi(e) : 0;
        char num[16];
        if (n >= MAX_RELAUNCH) {
            fprintf(stderr, "runner: a host mapping uses a guest address range after %d starts\n", n);
            return 1;
        }
        snprintf(num, sizeof num, "%d", n + 1);
        setenv("RUNNER_RELAUNCH", num, 1);
        execv("/proc/self/exe", argv);
        perror("runner: execv");
        return 1;
    }
    int nice = runner_nice_value(getenv("RUNNER_NICE"));
    if (setpriority(PRIO_PROCESS, 0, nice)) perror("runner: setpriority");
    struct runner *r = runner_create();
    /* the disk first: a snapshot fits only its layout */
    if (getenv("VDISK")) {
        if (runner_open_vdisk(r) != 0) { fprintf(stderr, "runner: cannot build the disk from %s\n", getenv("VDISK")); return 1; }
    } else if (argc > 2 && runner_load_disk(r, argv[2]) != 0) { perror("runner: open disk"); return 1; }
    if (runner_load_osos(r, argv[1]) != 0) { perror("runner: load osos"); return 1; }
    runner_run(r);
    return 0;
}
