/* SPDX-License-Identifier: GPL-3.0-or-later */
/* ext4_scrub (kernel/ext4.c): a file's blocks marked free in the bitmap -
 * the next file written goes over them, which is how an update (many files
 * written) broke programs that were fine. Shown happening without the
 * scrub, then the scrub finding it and marking them used, so the next file
 * goes elsewhere; e2fsck checks the image after (run.sh).
 *   scrub <image> [MB] */
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
static void quiet(const char* l) { printf("  | %s\n", l); }

static uint8_t buf[2 << 20], got[2 << 20];
static void fill(uint8_t* b, uint32_t n, uint32_t seed) { for (uint32_t i = 0; i < n; i++) { seed = seed * 1664525u + 1013904223u; b[i] = (uint8_t)(seed >> 24); } }

static uint32_t put(const char* name, uint32_t size, uint32_t seed) {
    uint32_t ino = ext4_create(EXT4_ROOT_INO, name, 0644, 0, 0);
    fill(buf, size, seed);
    CHECK(ino && ext4_write(ino, 0, buf, size) == 0, "write %s", name);
    return ino;
}
static int same(uint32_t ino, uint32_t size, uint32_t seed) {
    fill(buf, size, seed);
    return ext4_read(ino, 0, got, size) == (long)size && !memcmp(got, buf, size);
}

/* a filesystem with a program on it whose blocks the bitmap says are free;
 * fix: scrubbed first. 1 if the program stayed as written after more files */
static int run(const char* img, uint64_t mb, int fix) {
    fd = open(img, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0 || ftruncate(fd, (off_t)(mb << 20))) { perror(img); exit(2); }
    ext4_dev_t dev = { rd, wr, fl, 0, (uint32_t)((mb << 20) / 512) };
    CHECK(ext4_mkfs(&dev, "BANANA_OS", 0, 0) == 0, "mkfs");
    for (int i = 0; i < 20; i++) { char n[16]; snprintf(n, sizeof n, "f%d", i); put(n, 3000 + (uint32_t)i * 7777, (uint32_t)i); }
    uint32_t bash = put("bash", 1200000, 42);
    CHECK(ext4_flush() == 0, "flush");
    uint32_t xl = 9, fr = 9;
    CHECK(ext4_scrub(0, quiet, &xl, &fr) == 0 && !xl && !fr, "a clean filesystem: no problems (%u %u)", xl, fr);
    /* the damage: bash's blocks marked free (an older bug) */
    uint32_t sec = 0, nsec = 0;
    CHECK(ext4_first_run(bash, &sec, &nsec) == 0 && nsec, "bash's run");
    ext4_free_run(sec / 8, nsec / 8);
    CHECK(ext4_flush() == 0, "flush");
    if (fix) {
        CHECK(ext4_scrub(1, quiet, &xl, &fr) > 0 && fr == nsec / 8 && !xl, "the scrub finds bash's %u blocks marked free (%u)", nsec / 8, fr);
        CHECK(ext4_scrub(0, quiet, &xl, &fr) == 0 && !fr, "after the fix: nothing left (%u)", fr);
    }
    /* an update: new files written */
    for (int i = 0; i < 6; i++) { char n[16]; snprintf(n, sizeof n, "new%d", i); put(n, 400000, 100u + (uint32_t)i); }
    CHECK(ext4_flush() == 0, "flush");
    int ok = same(bash, 1200000, 42);
    CHECK(ext4_unmount() == 0, "unmount");
    close(fd);
    return ok;
}

int main(int argc, char** argv) {
    uint64_t mb = argc > 2 ? strtoull(argv[2], 0, 10) : 64;
    char other[512];
    snprintf(other, sizeof other, "%s.broken", argv[1]);
    int without = run(other, mb, 0);
    CHECK(!without, "without the scrub the new files went over bash (the bug this guards against)");
    int with = run(argv[1], mb, 1);
    CHECK(with, "with the scrub bash stayed as written");
    unlink(other);
    printf("scrub: %s\n", fails ? "FAILED" : "a file's blocks marked free are found and marked used; it then stays as written");
    return fails != 0;
}
