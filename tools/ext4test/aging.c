/* SPDX-License-Identifier: GPL-3.0-or-later */
/* An installed disk after many updates (kernel/ext4.c): packages upgraded
 * again and again - each new version written beside the old one (in chunks
 * of every size, sometimes grown with fallocate first), renamed over it, the
 * old one gone - with small files coming and going in between, so free space
 * is in pieces and big files get many extents (trees of depth 1). Every file
 * is checked against what was written after each round, after flushes, with
 * the cache trimmed hard, and after remounts; e2fsck checks the image after
 * (run.sh). A binary read back wrong here is "exec format error" there.
 *   aging <image> [MB] [rounds] [seed] */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include "ext4.h"

static int fd;
static int rd(void* c, uint32_t s, uint32_t n, void* b) { (void)c; return pread(fd, b, (size_t)n * 512, (off_t)s * 512) == (ssize_t)n * 512 ? 0 : -1; }
static int wr(void* c, uint32_t s, uint32_t n, const void* b) { (void)c; return pwrite(fd, b, (size_t)n * 512, (off_t)s * 512) == (ssize_t)n * 512 ? 0 : -1; }
static int fl(void* c) { (void)c; return 0; }
static int fails;
#define CHECK(x, ...) do { if (!(x)) { printf("FAIL line %d: %s ", __LINE__, #x); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static uint32_t rnd_state;
static uint32_t rnd(void) { rnd_state = rnd_state * 1103515245u + 12345u; return rnd_state >> 8; }
static void fill(uint8_t* b, uint32_t n, uint32_t seed) { for (uint32_t i = 0; i < n; i++) { seed = seed * 1664525u + 1013904223u; b[i] = (uint8_t)(seed >> 24); } }

#define NPKG 24
#define NSMALL 200
typedef struct { uint32_t size, seed, version; int live; } want_t;
static want_t pkg[NPKG], small[NSMALL];
static uint32_t dir_apps, dir_tmp;
static uint8_t buf[6 << 20], got[6 << 20];

static uint32_t lookup(uint32_t dir, const char* name) { return ext4_lookup(dir, name, 0); }

static int check_file(uint32_t dir, const char* name, const want_t* w, const char* when) {
    uint32_t ino = lookup(dir, name);
    if (!w->live) { CHECK(!ino, "%s: %s should be gone", when, name); return ino == 0; }
    if (!ino) { CHECK(0, "%s: %s missing", when, name); return 0; }
    fill(buf, w->size, w->seed);
    memset(got, 0xEE, w->size + 16);
    long r = ext4_read(ino, 0, got, w->size + 16);
    if (r != (long)w->size) { CHECK(0, "%s: %s read %ld of %u", when, name, r, w->size); return 0; }
    if (memcmp(got, buf, w->size)) {
        uint32_t i = 0;
        while (i < w->size && got[i] == buf[i]) i++;
        CHECK(0, "%s: %s v%u (%u bytes) differs from byte %u (block %u)", when, name, w->version, w->size, i, i / 4096);
        return 0;
    }
    return 1;
}

static void check_all(const char* when) {
    char nm[32];
    for (int i = 0; i < NPKG; i++) { snprintf(nm, sizeof nm, "pkg%d", i); check_file(dir_apps, nm, &pkg[i], when); }
    for (int i = 0; i < NSMALL; i++) { snprintf(nm, sizeof nm, "s%d", i); check_file(dir_tmp, nm, &small[i], when); }
}

/* a file written as pkg install does: chunks of one size or of many */
static int write_file(uint32_t ino, uint32_t size, uint32_t seed) {
    fill(buf, size, seed);
    uint32_t mode = rnd() % 4, off = 0;
    if (mode == 3 && ext4_fallocate(ino, size) != 0) return -1;
    while (off < size) {
        uint32_t step = mode == 0 ? 65536 : mode == 1 ? 1 + rnd() % 300000 : mode == 2 ? 4096 * (1 + rnd() % 64) : 1 + rnd() % 70000;
        uint32_t n = size - off < step ? size - off : step;
        if (ext4_write(ino, off, buf + off, n) != 0) return -1;
        off += n;
    }
    return 0;
}

int main(int argc, char** argv) {
    uint64_t mb = argc > 2 ? strtoull(argv[2], 0, 10) : 160;
    int rounds = argc > 3 ? atoi(argv[3]) : 300;
    rnd_state = argc > 4 ? (uint32_t)strtoul(argv[4], 0, 10) : 7;
    fd = open(argv[1], O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0 || ftruncate(fd, (off_t)(mb << 20))) { perror(argv[1]); return 2; }
    ext4_dev_t dev = { rd, wr, fl, 0, (uint32_t)((mb << 20) / 512) };
    CHECK(ext4_mkfs(&dev, "BANANA_OS", 0, 0) == 0, "mkfs");
    dir_apps = ext4_mkdir(EXT4_ROOT_INO, "apps", 0755, 0, 0);
    dir_tmp = ext4_mkdir(EXT4_ROOT_INO, "tmp", 0755, 0, 0);
    char nm[32], tmp[40];
    unsigned upgrades = 0, smalls = 0, remounts = 0;
    for (int round = 0; round < rounds && !fails; round++) {
        /* small files come and go, cutting the free space up */
        for (int k = 0; k < 20; k++) {
            int i = (int)(rnd() % NSMALL);
            snprintf(nm, sizeof nm, "s%d", i);
            if (small[i].live && rnd() % 2) {
                CHECK(ext4_unlink(dir_tmp, nm) == 0, "unlink %s", nm);
                small[i].live = 0;
            } else if (!small[i].live) {
                uint32_t ino = ext4_create(dir_tmp, nm, 0644, 0, 0);
                if (!ino) continue;
                small[i] = (want_t){ 1 + rnd() % 40000, rnd(), 1, 1 };
                if (write_file(ino, small[i].size, small[i].seed) != 0) { ext4_unlink(dir_tmp, nm); small[i].live = 0; }
                smalls++;
            }
        }
        /* a package upgraded: the new version beside the old, renamed over it */
        int p = (int)(rnd() % NPKG);
        snprintf(nm, sizeof nm, "pkg%d", p);
        snprintf(tmp, sizeof tmp, "pkg%d.new", p);
        uint32_t ino = ext4_create(dir_apps, tmp, 0755, 0, 0);
        CHECK(ino, "create %s", tmp);
        uint32_t size = 100000 + rnd() % (5u << 20), seed = rnd();
        if (write_file(ino, size, seed) != 0) {
            ext4_unlink(dir_apps, tmp);                    /* (full: the old version stays) */
        } else {
            CHECK(ext4_rename(dir_apps, tmp, dir_apps, nm) == 0, "rename %s", tmp);
            pkg[p] = (want_t){ size, seed, pkg[p].version + 1, 1 };
            upgrades++;
        }
        switch (rnd() % 8) {
        case 0: CHECK(ext4_flush() == 0, "flush"); break;
        case 1: ext4_cache_trim(1u << 30); break;          /* (RAM short: the cache gives everything back) */
        case 2:
            CHECK(ext4_unmount() == 0, "unmount");
            CHECK(ext4_mount(&dev) == 0, "mount");
            remounts++;
            break;
        }
        if (round % 10 == 9) check_all("round");
    }
    check_all("end");
    CHECK(ext4_unmount() == 0, "unmount");
    CHECK(ext4_mount(&dev) == 0, "mount");
    check_all("after remount");
    uint64_t blocks, freeb;
    uint32_t inodes, freei;
    ext4_statfs(&blocks, &freeb, &inodes, &freei);
    CHECK(ext4_unmount() == 0, "unmount");
    printf("aging: %u upgrades, %u small files, %u remounts; %llu of %llu blocks free; %s\n", upgrades, smalls, remounts,
           (unsigned long long)freeb, (unsigned long long)blocks, fails ? "FAILED" : "every file as written");
    return fails != 0;
}
