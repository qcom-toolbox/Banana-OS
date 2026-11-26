/* SPDX-License-Identifier: GPL-3.0-or-later */
/* kernel/ext4move.c on Linux: a filesystem at the start of a disk image,
 * full of everything (files of all sizes, a fragmented one with an extent
 * tree, a sparse one, one given blocks not written, hard links, short and
 * long symbolic links, deep folders, a folder of many files, owners,
 * permissions and dates, /swapfile), moved delta blocks further in.
 *   move IMG MB DELTA            the move; IMG.old and IMG.new: the two
 *                                filesystems, for e2fsck (run.sh)
 *   move IMG MB DELTA cut N      the disk stops taking writes after N of
 *                                them (a power cut): the old filesystem must
 *                                still be whole, every file as it was */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include "ext4.h"
#include "ext4move.h"

static int fd;
static long writes, cut_after = -1;
static uint32_t off_new;                         /* the new filesystem's first sector */
static int rd(void* c, uint32_t s, uint32_t n, void* b) {
    uint32_t o = (uint32_t)(uintptr_t)c;
    return pread(fd, b, (size_t)n * 512, ((off_t)s + o) * 512) == (ssize_t)n * 512 ? 0 : -1;
}
static int wr(void* c, uint32_t s, uint32_t n, const void* b) {
    if (cut_after >= 0 && writes >= cut_after) return -1;
    writes++;
    uint32_t o = (uint32_t)(uintptr_t)c;
    return pwrite(fd, b, (size_t)n * 512, ((off_t)s + o) * 512) == (ssize_t)n * 512 ? 0 : -1;
}
static int fl(void* c) { (void)c; return 0; }
static int fails;
static void quiet(const char* l) { printf("  | %s\n", l); }
#define CHECK(x, ...) do { if (!(x)) { printf("FAIL line %d: %s ", __LINE__, #x); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static void fill(uint8_t* b, uint32_t n, uint32_t seed) { for (uint32_t i = 0; i < n; i++) { seed = seed * 1103515245u + 12345u; b[i] = (uint8_t)(seed >> 16); } }

static uint32_t P(const char* path) {
    uint32_t ino = EXT4_ROOT_INO;
    char tmp[512];
    strcpy(tmp, path);
    for (char* t = strtok(tmp, "/"); t; t = strtok(0, "/")) { ino = ext4_lookup(ino, t, 0); if (!ino) return 0; }
    return ino;
}

static uint8_t big[32 << 20], got[32 << 20];

/* the files: name, size, seed (0: a folder) */
#define NFILES 12
static const struct { const char* path; uint32_t size, seed; } F[NFILES] = {
    { "/home/banana/empty", 0, 1 }, { "/home/banana/one", 1, 2 }, { "/home/banana/small.txt", 4095, 3 },
    { "/home/banana/block", 4096, 4 }, { "/home/banana/odd", 5000, 5 }, { "/home/banana/Music/song.ogg", 3 << 20, 6 },
    { "/bin/bash", 1200000, 7 }, { "/usr/lib/big.so", 24 << 20, 8 }, { "/usr/share/a/b/c/d/deep", 70000, 9 },
    { "/home/banana/Documents/report.odt", 200000, 10 }, { "/etc/passwd", 300, 11 }, { "/apps/terminal/app-x86_64", 900000, 12 },
};

static uint32_t mkpath(const char* path, int file) {
    char tmp[512];
    strcpy(tmp, path);
    uint32_t dir = EXT4_ROOT_INO;
    char* save = 0;
    char* t = strtok_r(tmp, "/", &save);
    while (t) {
        char* next = strtok_r(0, "/", &save);
        uint32_t ino = ext4_lookup(dir, t, 0);
        if (!ino) ino = next || !file ? ext4_mkdir(dir, t, 0755, 1000, 1000) : ext4_create(dir, t, 0640, 1000, 100);
        if (!ino) return 0;
        dir = ino;
        t = next;
    }
    return dir;
}

static int same(uint32_t ino, const uint8_t* want, uint32_t n) {
    long r = ext4_read(ino, 0, got, n + 10);
    return r == (long)n && !memcmp(got, want, n);
}

static void populate(void) {
    for (int i = 0; i < NFILES; i++) {
        uint32_t f = mkpath(F[i].path, 1);
        fill(big, F[i].size, F[i].seed);
        CHECK(f && ext4_write(f, 0, big, F[i].size) == 0, "%s", F[i].path);
    }
    /* fragmented: two files written in turns (more than 4 extents: a tree) */
    uint32_t home = P("/home/banana");
    uint32_t a = ext4_create(home, "frag-a", 0644, 1000, 1000), b = ext4_create(home, "frag-b", 0644, 1000, 1000);
    static uint8_t fa[6 << 20], fb[6 << 20];
    fill(fa, sizeof fa, 77);
    fill(fb, sizeof fb, 78);
    for (uint32_t o = 0; o < sizeof fa; o += 8192) { ext4_write(a, o, fa + o, 8192); ext4_write(b, o, fb + o, 8192); }
    /* sparse: data at 10 MB only */
    uint32_t s = ext4_create(home, "sparse", 0600, 1000, 1000);
    fill(big, 5000, 79);
    CHECK(ext4_write(s, 10u << 20, big, 5000) == 0, "sparse");
    /* blocks given, not written (reads as zeros) */
    uint32_t pre = ext4_create(home, "prealloc", 0600, 1000, 1000);
    CHECK(ext4_fallocate(pre, 2u << 20) == 0, "fallocate");
    uint32_t sw = ext4_create(EXT4_ROOT_INO, "swapfile", 0600, 0, 0);
    CHECK(ext4_fallocate(sw, 40u << 20) == 0, "swapfile");
    /* links */
    uint32_t bash = P("/bin/bash");
    CHECK(ext4_link(P("/bin"), "sh", bash) == 0, "hard link");
    CHECK(ext4_link(home, "bash-too", bash) == 0, "hard link 2");
    CHECK(ext4_symlink(home, "short", "/home/banana/one", 1000, 1000), "short link");
    char longt[300];
    memset(longt, 0, sizeof longt);
    for (int i = 0; i < 200; i++) longt[i] = (char)('a' + i % 26);
    CHECK(ext4_symlink(home, "long", longt, 1000, 1000), "long link");
    /* a folder of many files, some deleted, and a file in lost+found */
    uint32_t many = ext4_mkdir(home, "many", 0700, 1000, 1000);
    for (int i = 0; i < 1500; i++) {
        char n[32];
        snprintf(n, sizeof n, "file-%04d", i);
        uint32_t f = ext4_create(many, n, 0644, 1000, 1000);
        uint32_t v = (uint32_t)i * 2654435761u;
        ext4_write(f, 0, &v, 4);
    }
    for (int i = 0; i < 1500; i += 3) { char n[32]; snprintf(n, sizeof n, "file-%04d", i); ext4_unlink(many, n); }
    uint32_t lf = ext4_create(EXT4_LPF_INO, "#1234", 0600, 0, 0);
    ext4_write(lf, 0, "found", 5);
    /* owners, permissions, dates */
    CHECK(ext4_setattr(P("/etc/passwd"), 04755, 0, 0, 1600000000u, 1600000001u, 1600000002u) == 0, "setattr");
    CHECK(ext4_setattr(P("/usr/share/a/b"), 0711, 5, 6, 1500000000u, 1500000001u, 1500000002u) == 0, "setattr dir");
    CHECK(ext4_flush() == 0, "flush");
}

/* every file, link and date as populate() left them */
static void verify(const char* what) {
    for (int i = 0; i < NFILES; i++) {
        fill(big, F[i].size, F[i].seed);
        uint32_t f = P(F[i].path);
        CHECK(f && same(f, big, F[i].size), "%s: %s", what, F[i].path);
    }
    static uint8_t fa[6 << 20], fb[6 << 20];
    fill(fa, sizeof fa, 77);
    fill(fb, sizeof fb, 78);
    CHECK(same(P("/home/banana/frag-a"), fa, sizeof fa) && same(P("/home/banana/frag-b"), fb, sizeof fb), "%s: fragmented", what);
    uint32_t s = P("/home/banana/sparse");
    memset(big, 0, 10u << 20);
    fill(big + (10u << 20), 5000, 79);
    CHECK(s && same(s, big, (10u << 20) + 5000), "%s: sparse", what);
    uint32_t pre = P("/home/banana/prealloc");
    memset(big, 0, 2u << 20);
    CHECK(pre && same(pre, big, 2u << 20), "%s: prealloc", what);
    ext4_stat_t st, st2;
    CHECK(ext4_stat(P("/bin/bash"), &st) == 0 && st.links == 3 && P("/bin/sh") == P("/bin/bash") &&
          P("/home/banana/bash-too") == P("/bin/bash"), "%s: hard links (%u)", what, st.links);
    char t[300];
    memset(t, 0, sizeof t);
    CHECK(ext4_read(P("/home/banana/short"), 0, t, 299) == 16 && !strcmp(t, "/home/banana/one"), "%s: short link", what);
    memset(t, 0, sizeof t);
    CHECK(ext4_read(P("/home/banana/long"), 0, t, 299) == 200 && t[0] == 'a' && t[199] == 'r', "%s: long link", what);
    uint32_t many = P("/home/banana/many");
    int n = 0, ok = 1;
    for (int i = 0; i < 1500; i++) {
        char nm[32];
        snprintf(nm, sizeof nm, "file-%04d", i);
        uint32_t f = ext4_lookup(many, nm, 0);
        if (i % 3 == 0) { ok &= !f; continue; }
        uint32_t v = 0, want = (uint32_t)i * 2654435761u;
        ok &= f && ext4_read(f, 0, &v, 4) == 4 && v == want;
        n++;
    }
    CHECK(ok && n == 1000, "%s: many (%d)", what, n);
    CHECK(ext4_stat(P("/etc/passwd"), &st) == 0 && (st.mode & 07777) == 04755 && st.uid == 0 &&
          st.atime == 1600000000u && st.mtime == 1600000001u && st.ctime == 1600000002u, "%s: passwd attrs", what);
    CHECK(ext4_stat(P("/usr/share/a/b"), &st) == 0 && (st.mode & 07777) == 0711 && st.uid == 5 && st.gid == 6 &&
          st.mtime == 1500000001u, "%s: dir attrs", what);
    CHECK(ext4_stat(P("/home/banana/small.txt"), &st2) == 0 && (st2.mode & 07777) == 0640 && st2.uid == 1000 && st2.gid == 100,
          "%s: file owner", what);
    memset(t, 0, sizeof t);
    CHECK(ext4_read(ext4_lookup(EXT4_LPF_INO, "#1234", 0), 0, t, 10) == 5 && !strcmp(t, "found"), "%s: lost+found", what);
}

int main(int argc, char** argv) {
    if (argc < 4) { fprintf(stderr, "usage: move IMG MB DELTA [cut N]\n"); return 2; }
    uint64_t mb = strtoull(argv[2], 0, 10);
    uint32_t delta = (uint32_t)strtoul(argv[3], 0, 10);
    if (argc > 5 && !strcmp(argv[4], "cut")) cut_after = strtol(argv[5], 0, 10);
    fd = open(argv[1], O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0 || ftruncate(fd, (off_t)(mb << 20))) { perror(argv[1]); return 2; }
    uint32_t sectors = (uint32_t)((mb << 20) / 512);
    off_new = delta * 8u;
    ext4_dev_t old_dev = { rd, wr, fl, (void*)(uintptr_t)0, sectors };
    ext4_dev_t new_dev = { rd, wr, fl, (void*)(uintptr_t)off_new, sectors - off_new };
    long cut = cut_after;
    cut_after = -1;
    CHECK(ext4_mkfs(&old_dev, "BANANA_OS", 0, 0) == 0, "mkfs");
    populate();
    verify("before");
    CHECK(ext4_unmount() == 0, "unmount");

    /* a disk too small for the files: refused, nothing changed */
    ext4_dev_t tiny = { rd, wr, fl, (void*)(uintptr_t)(sectors - 20000u * 8u), 20000u * 8u };
    CHECK(ext4move_check(&old_dev, &tiny, (sectors / 8u) - 20000u) == EXT4MOVE_FULL, "a filesystem too small: refused");
    CHECK(ext4move_check(&old_dev, &new_dev, delta) == EXT4MOVE_OK, "room");

    writes = 0;
    cut_after = cut;
    ext4move_info_t info;
    int rc = ext4move_run(&old_dev, &new_dev, delta, &info);
    long done_writes = writes;
    cut_after = -1;
    if (cut < 0) {
        CHECK(rc == EXT4MOVE_OK, "the move: %d", rc);
        printf("move: %u files, %u folders, %u links, %u symlinks; %u blocks moved in, %u kept, %u freed; %ld writes\n",
               info.files, info.dirs, info.links, info.symlinks, info.moved_blocks, info.kept_blocks, info.freed_blocks, done_writes);
        /* the old one: still whole (until the disk points at the new one) */
        CHECK(ext4_mount(&old_dev) == 0, "the old one still mounts");
        verify("after (old)");
        CHECK(ext4_unmount() == 0, "unmount");
        char cp[600];
        snprintf(cp, sizeof cp, "dd if=%s of=%s.old bs=512 count=%u status=none", argv[1], argv[1], sectors);
        CHECK(system(cp) == 0, "dd old");
        CHECK(ext4_mount(&new_dev) == 0, "the new one mounts");
        verify("after (new)");
        CHECK(!ext4_lookup(EXT4_ROOT_INO, "swapfile", 0), "no swap file");
        uint32_t xl = 9, fr = 9;
        CHECK(ext4_scrub(0, quiet, &xl, &fr) >= 0 && !xl && !fr, "scrub: %u %u", xl, fr);
        /* and it works: files written, deleted */
        uint32_t f = ext4_create(EXT4_ROOT_INO, "after", 0644, 0, 0);
        fill(big, 5 << 20, 123);
        CHECK(f && ext4_write(f, 0, big, 5 << 20) == 0 && same(f, big, 5 << 20), "a new file");
        CHECK(ext4_unlink(P("/usr/lib"), "big.so") == 0, "a file deleted");
        CHECK(ext4_unmount() == 0, "unmount");
        printf("move: %ld writes in all\n", done_writes);
    } else {
        /* a cut: the old filesystem as it was, its files all there */
        CHECK(rc != EXT4MOVE_OK || cut >= done_writes, "the cut is seen (%d)", rc);
        char cp[600];
        snprintf(cp, sizeof cp, "dd if=%s of=%s.cut bs=512 count=%u status=none", argv[1], argv[1], sectors);
        CHECK(system(cp) == 0, "dd cut");
        CHECK(ext4_mount(&old_dev) == 0, "the old one mounts after a cut at write %ld", cut);
        verify("after a cut (old)");
        CHECK(ext4_unmount() == 0, "unmount");
        /* a move afterwards still works */
        rc = ext4move_run(&old_dev, &new_dev, delta, &info);
        CHECK(rc == EXT4MOVE_OK, "the move again: %d", rc);
        CHECK(ext4_mount(&new_dev) == 0, "the new one mounts");
        verify("after a cut and a move again (new)");
        CHECK(ext4_unmount() == 0, "unmount");
    }
    /* the two filesystems as images of their own, for e2fsck */
    char p[600];
    if (cut >= 0) snprintf(p, sizeof p, "dd if=%s of=%s.old bs=512 count=%u status=none && dd if=%s of=%s.new bs=512 skip=%u status=none",
                           argv[1], argv[1], sectors, argv[1], argv[1], off_new);
    else snprintf(p, sizeof p, "dd if=%s of=%s.new bs=512 skip=%u status=none", argv[1], argv[1], off_new);
    CHECK(system(p) == 0, "dd");
    close(fd);
    printf("move: %s\n", fails ? "FAILED" : "all as it should be");
    return fails != 0;
}
