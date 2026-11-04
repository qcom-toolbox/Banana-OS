/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "fsdisk.h"
#include "config.h"
#include "settings.h"
#include "fs.h"
#include "ata.h"
#include "atapi.h"
#include "disk.h"
#include "types.h"
#include "kheap.h"
#include "kstring.h"
#include "serial.h"
#include "timer.h"
#include "task.h"
#include "blockdev.h"
#include "crc32c.h"

#define FSDISK_MAGIC   0x414E4142u /* "BANA" */
#define FSDISK_VERSION 3u   /* = FS_SNAPSHOT_V3; version 1 and 2 disks (Banana OS 0.4, 0.5) still load */
#define FSDISK_SECTOR  512u

/* The disk starts with a copy of the CD's boot image (GRUB's hybrid MBR,
 * core.img and the ISO9660 filesystem with the kernels); the saved
 * filesystem lives after that boot area - see "the filesystem on disk". */

#define FSDISK_COPY_CHUNK_BLOCKS 32u /* 32 * 2048B ATAPI blocks = 64KiB per chunk */

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t version;
    uint32_t payload_bytes;
    uint32_t checksum;
    uint32_t seq;           /* which slot is newer (0 on disks from before the slots) */
    uint32_t super_check;   /* this header's own check */
    /* layout 4 only (a layout-3 header ends above: its files start at byte 24) */
    uint32_t layout;        /* 4 */
    uint32_t slot_sectors;  /* the size the slots were made with */
} fsdisk_super_t;

/* Boot-image copy chunk buffer. The filesystem snapshot itself is now
 * variable-sized (file data lives on the heap), so it gets a heap buffer
 * of exactly the right size per sync instead of a worst-case static one. */
#define FSDISK_COPY_BUF_BYTES (FSDISK_COPY_CHUNK_BLOCKS * 2048u)
static uint8_t g_buf[FSDISK_COPY_BUF_BYTES];


static int        g_have_target = 0;
static int        g_fi_state = -1;   /* fsdisk_find_install(): not looked up yet */
static ata_disk_t g_target;

static uint32_t checksum_of(const uint8_t* buf, uint32_t len) {
    uint32_t sum = 0;
    for (uint32_t i = 0; i < len; i++) sum = (sum * 131u) + buf[i];
    return sum;
}

static uint32_t bytes_to_sectors(uint32_t bytes) {
    return (bytes + FSDISK_SECTOR - 1u) / FSDISK_SECTOR;
}

int fsdisk_find_target(ata_disk_t* out) {
    ata_disk_t all[DISK_MAX];
    int n = disk_probe(all, DISK_MAX);

    int found = 0;
    ata_disk_t match;
    for (int i = 0; i < n; i++) {
        if (!all[i].is_atapi) {
            found++;
            match = all[i];
        }
    }
    if (found == 1) { *out = match; return 1; }
    if (found == 0) return 0;
    return -1;
}

/* ── where the boot image comes from: the CD, or the USB stick ────────
 * Banana_OS.iso is a "hybrid" image: written as it is onto a USB stick
 * (dd, Rufus in DD mode, Etcher) it boots like the CD - and the stick then
 * holds the very same bytes from its first sector on. So install and update
 * read the image from a CD/DVD drive, or failing that from a USB stick
 * that holds one (512-byte sectors: one 2048-byte CD block is 4 of them). */
typedef struct {
    ata_disk_t  cd;
    blockdev_t* usb;                 /* NULL: the CD drive */
} img_src_t;

static int src_read(const img_src_t* s, uint32_t blk, uint32_t n, void* buf) {
    if (s->usb) return s->usb->present ? s->usb->read(s->usb, blk * 4u, n * 4u, buf) : -1;
    return disk_cd_read(&s->cd, blk, n, buf);
}

/* the ISO9660 image's size, from its primary volume descriptor (block 16) */
static int src_iso_size(const img_src_t* s, uint32_t* out_bytes) {
    if (!s->usb) return disk_cd_iso_size(&s->cd, out_bytes);
    uint8_t* pvd = (uint8_t*)kmalloc(2048);
    if (!pvd) return -1;
    int ok = src_read(s, 16, 1, pvd) == 0 && pvd[0] == 1 && memcmp(pvd + 1, "CD001", 5) == 0;
    uint32_t blocks = ok ? ((uint32_t)pvd[80] | (uint32_t)pvd[81] << 8 | (uint32_t)pvd[82] << 16 | (uint32_t)pvd[83] << 24) : 0;
    kfree(pvd);
    if (!ok || !blocks || (uint64_t)blocks * 4u > s->usb->sectors) return -1;
    *out_bytes = blocks * 2048u;
    return 0;
}

/* a Banana OS image: its volume label (Banana Boot images), or GRUB's search
 * for its build marker in the first 4 MB (its EFI program - images before) */
static int src_is_banana(const img_src_t* s, uint32_t iso_bytes) {
    uint8_t* pvd = (uint8_t*)kmalloc(2048);
    if (pvd) {
        int label = src_read(s, 16, 1, pvd) == 0 && memcmp(pvd + 40, "BANANA_OS ", 10) == 0;
        kfree(pvd);
        if (label) return 1;
    }
    uint32_t blocks = iso_bytes / 2048u < 2048u ? iso_bytes / 2048u : 2048u;
    uint8_t* b = (uint8_t*)kmalloc(blocks * 2048u);
    if (!b) return 0;
    int found = 0;
    static const char KEY[] = "--file /.disk/";
    if (src_read(s, 0, blocks, b) == 0)
        for (uint32_t k = 0; k + sizeof(KEY) < blocks * 2048u && !found; k++)
            if (b[k] == '-' && memcmp(b + k, KEY, sizeof(KEY) - 1) == 0) found = 1;
    kfree(b);
    return found;
}

/* the CD/DVD drive holding the Banana OS boot image - or else a USB stick holding it */
static int find_source(img_src_t* out) {
    ata_disk_t all[DISK_MAX];
    int n = disk_probe(all, DISK_MAX);
    uint32_t bytes;
    memset(out, 0, sizeof(*out));
    for (int i = 0; i < n; i++)
        if (all[i].is_atapi && disk_cd_iso_size(&all[i], &bytes) == 0) { out->cd = all[i]; return 1; }
    for (int i = 0; i < BLOCKDEV_MAX; i++) {
        blockdev_t* bd = blockdev_get(i);
        if (!bd || !bd->present || bd->sector_size != 512 || strcmp(bd->kind, "usb") != 0) continue;
        out->usb = bd;
        if (src_iso_size(out, &bytes) == 0 && src_is_banana(out, bytes)) {
            klog("fsdisk: the Banana OS image on the USB stick %s (%s)\n", bd->name, bd->model);
            return 1;
        }
    }
    out->usb = NULL;
    return 0;
}

/* ── the boot image on the disk: the CD's, with its own marker ─────────
 * The installed disk is a copy of the CD, so both would hold the file the
 * UEFI GRUB looks for ("search --file /.disk/<build>.uuid", embedded in
 * its EFI program) - and a CD started by hand could then load the disk's
 * system, files and password (VMware lists the disk first). So the copy
 * gets the marker renamed to <build>.inst, in its file system (Rock Ridge
 * and Joliet names) and in its own EFI program's search: the CD's GRUB
 * only finds the CD, the disk's only the disk. (BIOS GRUB does not search:
 * it uses the drive it was started from.) Same length, nothing moves. */

#define CARRY 2048u                        /* one CD block kept back: a name split across chunks */

static char     g_mark_old[64], g_mark_new[64];    /* "<build>.uuid" / "<build>.inst" */
static uint32_t g_mark_len;

/* finds the build marker in the CD's EFI program (in its first 4 MB) */
static void find_marker(const img_src_t* src, uint32_t total_blocks) {
    g_mark_len = 0;
    uint32_t blocks = total_blocks < 2048u ? total_blocks : 2048u;
    uint8_t* b = (uint8_t*)kmalloc(blocks * 2048u);
    if (!b) return;
    if (src_read(src, 0, blocks, b) == 0) {
        static const char KEY[] = "--file /.disk/";
        uint32_t kl = sizeof(KEY) - 1, n = blocks * 2048u;
        for (uint32_t k = 0; k + kl + 8 < n; k++) {
            if (memcmp(b + k, KEY, kl) != 0) continue;
            uint32_t a = k + kl, e = a;
            while (e < n && e - a < 48 && b[e] > ' ' && b[e] != '\n') e++;
            uint32_t len = e - a;
            if (len > 5 && len < sizeof(g_mark_old) && memcmp(b + e - 5, ".uuid", 5) == 0) {
                memcpy(g_mark_old, b + a, len);
                memcpy(g_mark_new, b + a, len - 5);
                memcpy(g_mark_new + len - 5, ".inst", 5);
                g_mark_old[len] = g_mark_new[len] = 0;
                g_mark_len = len;
            }
            break;
        }
    }
    kfree(b);
}

/* grub/medium.cfg: tells the kernel which copy GRUB started (see
 * sysinfo_live_boot). The line is put together here at run time: written
 * out whole, it would sit in this kernel's own image on the CD and be
 * rewritten in the disk's copy along with medium.cfg. */
static void medium_line(char* out, const char* word) {
    kstrlcpy(out, "set banana_", 32);
    kstrlcpy(out + 11, "medium=", 16);
    kstrlcpy(out + 18, word, 12);
}

/* the boot loader's screen size line, "set banana_video=" and a value
 * padded to 12 characters (loader/common.h) - put together at run time
 * like the medium line */
#define VIDEO_FIELD 12
static char g_video_new[VIDEO_FIELD + 1];        /* what install / update write ("" : as the CD has it) */

static uint32_t video_key(char* out) {
    kstrlcpy(out, "set banana_", 32);
    kstrlcpy(out + 11, "video=", 16);
    return 17;
}

/* every video line in b[0..n) gets the value; how many there were */
static int patch_video(uint8_t* b, uint32_t n, const char* value) {
    char key[32], field[VIDEO_FIELD];
    uint32_t K = video_key(key);
    uint32_t vl = (uint32_t)strlen(value);
    for (uint32_t i = 0; i < VIDEO_FIELD; i++) field[i] = i < vl ? value[i] : ' ';
    int hits = 0;
    for (uint32_t k = 0; k + K + VIDEO_FIELD <= n; k++)
        if (b[k] == 's' && memcmp(b + k, key, K) == 0) {
            memcpy(b + k + K, field, VIDEO_FIELD);
            hits++;
            k += K + VIDEO_FIELD - 1;
        }
    return hits;
}

/* the CD's bytes as they go onto the disk */
static void patch_marker(uint8_t* b, uint32_t n) {
    if (g_video_new[0]) patch_video(b, n, g_video_new);
    char cd[32], disk[32];
    medium_line(cd, "live-cd");
    medium_line(disk, "install");
    uint32_t M = (uint32_t)strlen(cd);
    for (uint32_t k = 0; k + M <= n; k++)
        if (b[k] == 's' && memcmp(b + k, cd, M) == 0) memcpy(b + k, disk, M);
    uint32_t L = g_mark_len;
    if (!L) return;
    for (uint32_t k = 0; k + L <= n; k++) {
        if (b[k] == (uint8_t)g_mark_old[0] && memcmp(b + k, g_mark_old, L) == 0) memcpy(b + k, g_mark_new, L);
        /* Joliet: UCS-2, big-endian */
        if (k + 2 * L <= n && b[k] == 0 && b[k + 1] == (uint8_t)g_mark_old[0]) {
            uint32_t m = 0;
            while (m < L && b[k + 2 * m] == 0 && b[k + 2 * m + 1] == (uint8_t)g_mark_old[m]) m++;
            if (m == L) for (m = 0; m < L; m++) b[k + 2 * m + 1] = (uint8_t)g_mark_new[m];
        }
    }
}

/* reads the CD's image in chunks, patched, and hands them to sink(first block, blocks, data) */
typedef int (*image_sink_t)(uint32_t blk, uint32_t nblk, const uint8_t* data, void* ctx);

/* how far the copy is (the installer's progress bar): CD blocks, +1 for the files */
static volatile uint32_t g_prog_done, g_prog_total;

void fsdisk_progress(uint32_t* done, uint32_t* total) { *done = g_prog_done; *total = g_prog_total; }

static fsdisk_timing_t g_tm;
void fsdisk_timing(fsdisk_timing_t* out) { *out = g_tm; }

/* The image is read 1 MB at a time (READ_BLOCKS), with the block before
 * and the block after it: a name the patch renames may cross a chunk's
 * edge, and is found whole that way (patching is the same on both sides:
 * a renamed name is not found again). Each chunk goes to the sink at its
 * own place, so a sink can skip what has not changed. */
#define READ_BLOCKS  512u                  /* 1 MB of CD blocks per read */
#define HASH_BLOCKS  32u                   /* the unit of the image's checksum table: 64 KB */
#define HTAB_MAX     16384u                /* covers images up to 1 GiB */

/* New installs reserve at least 1 GiB, with room for image growth.
 * Entry 2 of the hybrid MBR describes ext4; entry 1 remains the ESP.
 * Legacy layouts have no entry 2 and retain their 128 MiB boundary. */
#define BOOT_LEGACY_LBA (128u * 1024u * 1024u / FSDISK_SECTOR)
#define BOOT_MIN_LBA    (256u * 1024u * 1024u / FSDISK_SECTOR)
#define BOOT_INSTALL_LBA (1024u * 1024u * 1024u / FSDISK_SECTOR)
#define BOOT_MAX_LBA    (2u * BOOT_INSTALL_LBA)
#define BOOT_HEADROOM  (256u * 1024u * 1024u)
#define BOOT_TABLE_SECT 256u

static uint32_t boot_u32(const uint8_t* p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static void boot_put32(uint8_t* p, uint32_t v) {
    for (unsigned i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8u * i));
}
static uint32_t boot_partition(const ata_disk_t* d, uint8_t* part) {
    uint8_t mbr[FSDISK_SECTOR];
    if (part) memset(part, 0, 16);
    if (disk_read(d, 0, 1, mbr) != 0) return 0;
    const uint8_t* e = mbr + 462;
    if (mbr[510] != 0x55 || mbr[511] != 0xaa || memcmp(mbr + 440, "BnOS", 4) || e[4] != 0x83)
        return BOOT_LEGACY_LBA;
    uint32_t base = boot_u32(e + 8), n = boot_u32(e + 12);
    if (base < BOOT_MIN_LBA || base > BOOT_MAX_LBA || (base & (base - 1u)) ||
        !n || (uint64_t)base + n > d->sectors) return 0;
    if (part) memcpy(part, e, 16);
    return base;
}
static uint32_t boot_table_lba(uint32_t base) {
    return base - (base == BOOT_LEGACY_LBA ? 64u : BOOT_TABLE_SECT);
}
static uint32_t boot_table_sectors(uint32_t base) {
    return base == BOOT_LEGACY_LBA ? 17u : 1u + HTAB_MAX * 4u / FSDISK_SECTOR;
}
static int boot_image_fits(uint32_t base, uint32_t bytes) {
    return base && (uint64_t)bytes <= (uint64_t)HTAB_MAX * HASH_BLOCKS * 2048u &&
           (uint64_t)bytes <= (uint64_t)boot_table_lba(base) * FSDISK_SECTOR;
}
static uint32_t boot_install_base(uint32_t bytes) {
    /* Keep the checksum table's image limit independent of reserved space.
     * Use 64-bit arithmetic before rounding so oversized input cannot wrap. */
    if ((uint64_t)bytes > (uint64_t)HTAB_MAX * HASH_BLOCKS * 2048u) return 0;
    uint64_t spare = ((uint64_t)bytes + 1u) / 2u;
    if (spare < BOOT_HEADROOM) spare = BOOT_HEADROOM;
    uint64_t required = (uint64_t)bytes + spare;
    uint32_t base = BOOT_INSTALL_LBA;
    while (required > (uint64_t)boot_table_lba(base) * FSDISK_SECTOR) {
        if (base == BOOT_MAX_LBA) return 0;
        base *= 2u;
    }
    return base;
}

/* need(first block, blocks): 0 skips that chunk (NULL: every chunk) */
typedef int (*chunk_need_t)(uint32_t blk, uint32_t nblk);

static int stream_image(const img_src_t* src, uint32_t iso_bytes, image_sink_t sink, void* ctx, chunk_need_t need, const uint8_t* partition) {
    uint32_t total = iso_bytes / 2048u;
    char vv[24];
    g_video_new[0] = 0;
    if (cfg_get(CFG_SETTINGS, "boot_resolution", vv, sizeof(vv)) && vv[0] && strlen(vv) <= VIDEO_FIELD)
        kstrlcpy(g_video_new, vv, sizeof(g_video_new));
    g_prog_done = 0;
    g_prog_total = total + 1;
    g_tm.image_bytes = iso_bytes;
    find_marker(src, total);
    /* [the block before][the chunk][the block after] */
    uint8_t* buf = (uint8_t*)kmalloc(READ_BLOCKS * 2048u + 2u * CARRY);
    uint8_t* prev = (uint8_t*)kmalloc(CARRY);
    if (!buf || !prev) { kfree(buf); kfree(prev); return -1; }
    memset(buf, 0, CARRY);
    uint32_t done = 0;
    int rc = 0, have_before = 1;
    while (done < total && rc == 0) {
        uint32_t n = total - done < READ_BLOCKS ? total - done : READ_BLOCKS;
        uint32_t after = done + n < total ? 1u : 0u;
        if (need && !need(done, n)) { done += n; g_prog_done = done; have_before = 0; continue; }
        uint32_t t0 = timer_ms();
        if (!have_before && src_read(src, done - 1u, 1, buf) != 0) { rc = -1; break; }
        if (src_read(src, done, n + after, buf + CARRY) != 0) { rc = -1; break; }
        g_tm.read_ms += timer_ms() - t0;
        memcpy(prev, buf + CARRY + (n - 1u) * 2048u, CARRY);      /* (as read: the next chunk's "before") */
        patch_marker(buf, CARRY + (n + after) * 2048u);
        if (!done && partition) memcpy(buf + CARRY + 462, partition, 16);
        t0 = timer_ms();
        rc = sink(done, n, buf + CARRY, ctx);
        g_tm.disk_ms += timer_ms() - t0;
        done += n;
        g_prog_done = done;
        memcpy(buf, prev, CARRY);
        have_before = 1;
        task_yield();                      /* the desktop keeps drawing (the installer's progress) */
    }
    kfree(prev);
    kfree(buf);
    return rc;
}

/* ── the boot image's checksum table ──────────────────────────────────
 * Install and update keep a CRC-32C of every 64 KB of the boot image they
 * wrote, in a few sectors at the end of the boot area. An update then
 * finds what changed by reading the CD alone, and writes only that; it
 * does not have to read the whole disk back to know it is up to date. The
 * table names the image it was made for (its volume descriptor, which
 * holds the time the CD was made): an image written another way - an
 * older Banana OS's update - makes it stale, and it is not used. */
typedef struct {
    uint32_t tab[HTAB_MAX];
    uint32_t n;                            /* chunks in tab */
    int      valid;                        /* tab is what the disk holds */
} htab_t;

static htab_t g_old, g_new;                /* the disk's table, and the one being made */

static void hash_chunks(uint32_t blk, uint32_t nblk, const uint8_t* data) {
    for (uint32_t k = 0; k < nblk; k += HASH_BLOCKS) {
        uint32_t i = (blk + k) / HASH_BLOCKS, len = (nblk - k < HASH_BLOCKS ? nblk - k : HASH_BLOCKS) * 2048u;
        if (i < HTAB_MAX) { g_new.tab[i] = crc32c(0, data + k * 2048u, len); if (i + 1 > g_new.n) g_new.n = i + 1; }
    }
}

static int sink_write(uint32_t blk, uint32_t nblk, const uint8_t* data, void* ctx) {
    hash_chunks(blk, nblk, data);
    g_tm.written_bytes += nblk * 2048u;
    return disk_write((const ata_disk_t*)ctx, blk * 4u, nblk * 4u, data) == 0 ? 0 : -1;
}

/* an update: only the 64 KB pieces whose checksum is not the disk's (runs of them in one write) */
static int sink_delta(uint32_t blk, uint32_t nblk, const uint8_t* data, void* ctx) {
    hash_chunks(blk, nblk, data);
    uint32_t run = 0, run_len = 0;
    for (uint32_t k = 0; k <= nblk; k += HASH_BLOCKS) {
        uint32_t i = (blk + k) / HASH_BLOCKS;
        int changed = k < nblk && !(g_old.valid && i < g_old.n && g_old.tab[i] == g_new.tab[i]);
        if (changed) {
            if (!run_len) run = k;
            run_len += nblk - k < HASH_BLOCKS ? nblk - k : HASH_BLOCKS;
            continue;
        }
        if (run_len) {
            g_tm.written_bytes += run_len * 2048u;
            if (disk_write((const ata_disk_t*)ctx, (blk + run) * 4u, run_len * 4u, data + run * 2048u) != 0) return -1;
            run_len = 0;
        }
    }
    return 0;
}

/* ── verifying what was written ──────────────────────────────────────
 * Install and update read the whole boot image back from the disk and
 * compare every 64 KB piece with the checksum of what was meant to be
 * there (g_new). A piece the disk did not keep is written again; if it
 * still differs, the update fails and says so - before, a write the disk
 * silently dropped stayed, and the table told the next updates that
 * piece was fine. */
static uint8_t g_bad[HTAB_MAX];

/* how many pieces of the image on the disk are not g_new's (g_bad marks them); -1: no memory */
static int verify_image(const ata_disk_t* d, uint32_t iso_bytes) {
    uint32_t total = iso_bytes / 2048u;
    uint8_t* buf = (uint8_t*)kmalloc(READ_BLOCKS * 2048u);
    if (!buf) return -1;
    int bad = 0;
    for (uint32_t done = 0; done < total; ) {
        uint32_t n = total - done < READ_BLOCKS ? total - done : READ_BLOCKS;
        int rd = disk_read(d, done * 4u, n * 4u, buf);
        for (uint32_t k = 0; k < n; k += HASH_BLOCKS) {
            uint32_t i = (done + k) / HASH_BLOCKS, len = (n - k < HASH_BLOCKS ? n - k : HASH_BLOCKS) * 2048u;
            if (i >= HTAB_MAX) continue;
            g_bad[i] = rd != 0 || i >= g_new.n || crc32c(0, buf + k * 2048u, len) != g_new.tab[i];
            bad += g_bad[i];
        }
        done += n;
        task_yield();
    }
    kfree(buf);
    return bad;
}

static int need_bad(uint32_t blk, uint32_t nblk) {
    for (uint32_t i = blk / HASH_BLOCKS; i < (blk + nblk + HASH_BLOCKS - 1u) / HASH_BLOCKS && i < HTAB_MAX; i++)
        if (g_bad[i]) return 1;
    return 0;
}

/* the marked pieces again, from the source (patched as before) */
static int sink_fix(uint32_t blk, uint32_t nblk, const uint8_t* data, void* ctx) {
    for (uint32_t k = 0; k < nblk; k += HASH_BLOCKS) {
        uint32_t i = (blk + k) / HASH_BLOCKS, len = nblk - k < HASH_BLOCKS ? nblk - k : HASH_BLOCKS;
        if (i >= HTAB_MAX || !g_bad[i]) continue;
        g_tm.written_bytes += len * 2048u;
        if (disk_write((const ata_disk_t*)ctx, (blk + k) * 4u, len * 4u, data + k * 2048u) != 0) return -1;
    }
    return 0;
}

/* the image on the disk is g_new's, or is made so (two tries); FSDISK_OK or FSDISK_ERR_IO */
static int verify_and_fix(const img_src_t* src, const ata_disk_t* d, uint32_t iso_bytes, const uint8_t* partition) {
    for (int attempt = 0; ; attempt++) {
        if (disk_flush(d) != 0) return FSDISK_ERR_IO;
        int bad = verify_image(d, iso_bytes);
        if (bad == 0) return FSDISK_OK;
        if (bad < 0) { klog("fsdisk: no memory to verify the boot image\n"); return FSDISK_ERR_IO; }
        klog("fsdisk: %d of %u pieces of the boot image on the disk are not what was written%s\n",
             bad, g_new.n, attempt < 2 ? " - writing them again" : "");
        if (attempt == 2) return FSDISK_ERR_IO;
        /* (sink_fix leaves g_new as it is) */
        if (stream_image(src, iso_bytes, sink_fix, (void*)d, need_bad, partition) != 0) return FSDISK_ERR_IO;
    }
}

/* comparing with the table: how many 64 KB pieces differ */
static int sink_hash_compare(uint32_t blk, uint32_t nblk, const uint8_t* data, void* ctx) {
    hash_chunks(blk, nblk, data);
    int* differ = (int*)ctx;
    for (uint32_t k = 0; k < nblk; k += HASH_BLOCKS) {
        uint32_t i = (blk + k) / HASH_BLOCKS;
        if (!(i < g_old.n && g_old.tab[i] == g_new.tab[i])) (*differ)++;
    }
    return 0;
}

/* Copies the CD's boot image (block-exact, per the ISO9660 PVD) onto the
 * disk from LBA 0 - 2048-byte CD blocks become 4 disk sectors each. */
static int copy_boot_image(const img_src_t* src, const ata_disk_t* dst, uint32_t iso_bytes, const uint8_t* partition) {
    memset(&g_new, 0, sizeof(g_new));
    return stream_image(src, iso_bytes, sink_write, (void*)dst, NULL, partition);
}

/* ── the filesystem on disk ───────────────────────────────────────────
 * Two slots written in turn: a sync writes the slot that does NOT hold the
 * newest good copy (by sequence number), so a power cut mid-write leaves
 * the previous save intact; the checksum tells a half-written one apart.
 *
 * Layout 4 (this version): the boot image gets the first 128 MB and the
 * two slots share the rest of the disk, half each (one slot when the disk
 * is too small for two). The slot size comes from the disk's size and is
 * also written in every header, so the second slot is found even when the
 * first one is damaged.
 * Layout 3 (earlier installs): 32 MB boot area, slots of ~256 MB at fixed
 * places. Still read; `update` moves such a disk to layout 4. */

#define LAYOUT_V4           4u
#define V4_BASE_LBA         (128u * 1024u * 1024u / FSDISK_SECTOR)
#define V4_TAIL_SECTORS     2048u                        /* 1 MB left free at the end */
#define V4_MIN_SLOT_SECTORS (32u * 1024u * 1024u / FSDISK_SECTOR)
/* Legacy disks reserve the last 32 KiB of the 128 MiB boot area for
 * checksums. Partitioned installs use 128 KiB at their own boundary. */
#define HTAB_MAGIC   0x48534842u              /* "BHSH" */

#define V3_BASE_LBA         (32u * 1024u * 1024u / FSDISK_SECTOR)
#define V3_SLOT_SECTORS     (((uint32_t)24u + (256u * 1024u * 1024u) + 65535u) / FSDISK_SECTOR)
#define MAX_PAYLOAD_LIMIT   (1024u * 1024u * 1024u)      /* the files live in RAM anyway */

typedef struct {
    uint32_t version;        /* 3 or 4 */
    uint32_t base;           /* first slot's LBA */
    uint32_t slot_sectors;
    int      nslots;
} layout_t;

static layout_t g_layout;            /* the installed disk's */
static int      g_cur_slot = 0;      /* the slot holding the newest copy */
static uint32_t g_seq = 0;
static uint32_t g_synced_gen = 0;    /* fs_generation() of what is on disk */
static int      g_busy = 0;
static int      g_last_err = 0;

static void layout_v3(const ata_disk_t* d, layout_t* l) {
    l->version = 3;
    l->base = V3_BASE_LBA;
    l->slot_sectors = V3_SLOT_SECTORS;
    l->nslots = d->sectors >= V3_BASE_LBA + 2u * V3_SLOT_SECTORS ? 2 : 1;
}

/* layout 4 for a disk of this size; 0 if it is too small even for one slot */
static int layout_v4(const ata_disk_t* d, layout_t* l) {
    l->version = LAYOUT_V4;
    l->base = V4_BASE_LBA;
    if (d->sectors <= V4_BASE_LBA + V4_TAIL_SECTORS + 2048u) return 0;
    uint32_t avail = (uint32_t)d->sectors - V4_BASE_LBA - V4_TAIL_SECTORS;
    if (avail >= 2u * V4_MIN_SLOT_SECTORS) {
        l->nslots = 2;
        l->slot_sectors = (avail / 2u) & ~2047u;          /* 1 MB aligned */
    } else {
        l->nslots = 1;
        l->slot_sectors = avail & ~2047u;
    }
    return 1;
}

/* the header before the files: layout 4 adds two fields (layout 3's is 24 bytes) */
static uint32_t hdr_size(uint32_t version) { return version == LAYOUT_V4 ? (uint32_t)sizeof(fsdisk_super_t) : 24u; }

static uint32_t slot_lba_of(const layout_t* l, int slot) { return l->base + (uint32_t)slot * l->slot_sectors; }

static uint32_t slot_capacity(const layout_t* l) {
    uint32_t cap = l->slot_sectors * FSDISK_SECTOR - hdr_size(l->version);
    if (l->version == 3) cap = 256u * 1024u * 1024u;
    return cap < MAX_PAYLOAD_LIMIT ? cap : MAX_PAYLOAD_LIMIT;
}

static uint32_t super_check(const fsdisk_super_t* sb) {
    uint32_t c = sb->magic ^ (sb->seq * 2654435761u) ^ sb->payload_bytes ^ sb->checksum ^ 0x5EC0DE5Au;
    if (sb->layout) c ^= sb->layout * 0x9E3779B1u ^ sb->slot_sectors * 0x85EBCA77u;   /* (layout 3: unchanged) */
    return c;
}

/* Big writes go out in pieces, and the other tasks run in between: a save
 * in the background never holds up the desktop for more than one piece. */
#define WRITE_CHUNK_SECTORS 128u                          /* 64 KiB */

/* One command at a time on the installed disk: a save writing in the
 * background and a file being read in for a program take turns. */
static volatile int g_dlock;
static void dlock(void)   { while (g_dlock) task_sleep_ms(1); g_dlock = 1; }
static void dunlock(void) { g_dlock = 0; }

static int locked_write(const ata_disk_t* disk, uint32_t lba, uint32_t n, const void* buf) {
    dlock();
    int rc = disk_write(disk, lba, n, buf);
    dunlock();
    return rc;
}

static int locked_flush(const ata_disk_t* disk) {
    dlock();
    int rc = disk_flush(disk);
    dunlock();
    return rc;
}

static int write_chunked(const ata_disk_t* disk, uint32_t lba, uint32_t sectors, const uint8_t* buf) {
    while (sectors) {
        uint32_t n = sectors < WRITE_CHUNK_SECTORS ? sectors : WRITE_CHUNK_SECTORS;
        if (locked_write(disk, lba, n, buf) != 0) return -1;
        lba += n;
        sectors -= n;
        buf += n * FSDISK_SECTOR;
        if (sectors) task_yield();
    }
    return 0;
}

/* writes a snapshot payload (already serialized, after its header space) into a slot */
static int write_payload(const ata_disk_t* disk, const layout_t* l, int slot, uint32_t seq,
                         uint8_t* buf, uint32_t payload, uint32_t snap_version) {
    uint32_t hdr = hdr_size(l->version);
    if (payload > slot_capacity(l)) return FSDISK_ERR_FULL;
    uint32_t sectors = bytes_to_sectors(hdr + payload);
    if (sectors > l->slot_sectors && l->version == LAYOUT_V4) return FSDISK_ERR_FULL;
    if (disk->sectors < slot_lba_of(l, slot) + sectors) return FSDISK_ERR_TOO_SMALL;
    fsdisk_super_t sb;
    memset(&sb, 0, sizeof(sb));
    sb.magic         = FSDISK_MAGIC;
    sb.version       = snap_version;
    sb.payload_bytes = payload;
    sb.checksum      = checksum_of(buf + hdr, payload);
    sb.seq           = seq;
    if (l->version == LAYOUT_V4) { sb.layout = LAYOUT_V4; sb.slot_sectors = l->slot_sectors; }
    sb.super_check   = super_check(&sb);
    memcpy(buf, &sb, hdr);
    return (write_chunked(disk, slot_lba_of(l, slot), sectors, buf) == 0 && disk_flush(disk) == 0) ? FSDISK_OK : FSDISK_ERR_IO;
}

static int write_snapshot_slot(const ata_disk_t* disk, const layout_t* l, int slot, uint32_t seq) {
    uint32_t gen = fs_generation();          /* (taken before: a change while writing syncs again) */
    uint32_t payload = fs_snapshot_size();
    if (payload > slot_capacity(l)) return FSDISK_ERR_FULL;
    uint32_t hdr = hdr_size(l->version);
    uint32_t sectors = bytes_to_sectors(hdr + payload);
    uint8_t* buf = (uint8_t*)kzalloc(sectors * FSDISK_SECTOR);
    if (!buf) return FSDISK_ERR_IO;
    fs_snapshot_save(buf + hdr);
    int rc = write_payload(disk, l, slot, seq, buf, payload, FSDISK_VERSION);
    kfree(buf);
    if (rc == FSDISK_OK) { g_cur_slot = slot; g_seq = seq; g_synced_gen = gen; }
    return rc;
}

/* the next save: into the other slot (or the only one) */
static int write_snapshot_to(const ata_disk_t* disk) {
    int slot = g_layout.nslots == 2 ? (g_cur_slot ^ 1) : 0;
    return write_snapshot_slot(disk, &g_layout, slot, g_seq + 1);
}

/* a slot's header, if it looks like a Banana OS filesystem of that layout */
static int read_super_at(const ata_disk_t* d, uint32_t lba, uint32_t layout_version, fsdisk_super_t* sb) {
    if (lba >= d->sectors) return 0;
    if (disk_read(d, lba, 1, g_buf) != 0) return 0;
    memcpy(sb, g_buf, sizeof(*sb));
    if (layout_version != LAYOUT_V4) { sb->layout = 0; sb->slot_sectors = 0; }   /* (files start at byte 24) */
    if (sb->magic != FSDISK_MAGIC) return 0;
    if (sb->version != FS_SNAPSHOT_V1 && sb->version != FS_SNAPSHOT_V2 && sb->version != FS_SNAPSHOT_V3) return 0;
    if (sb->payload_bytes == 0 || sb->payload_bytes > MAX_PAYLOAD_LIMIT) return 0;
    if (layout_version == LAYOUT_V4) {
        if (sb->layout != LAYOUT_V4 || sb->slot_sectors == 0 || sb->super_check != super_check(sb)) return 0;
    } else {
        /* disks from before the two slots have no sequence number (0, unchecked) */
        if (sb->seq != 0 && sb->super_check != super_check(sb)) return 0;
        if (sb->payload_bytes > 256u * 1024u * 1024u) return 0;
    }
    return 1;
}

/* Which layout the disk has, and its slot headers. Layout 4 first: a disk
 * reinstalled over a layout-3 one may still hold that one's old header. */
static int detect(const ata_disk_t* d, layout_t* l, fsdisk_super_t sb[2], int have[2]) {
    have[0] = have[1] = 0;
    if (layout_v4(d, l)) {
        have[0] = read_super_at(d, slot_lba_of(l, 0), LAYOUT_V4, &sb[0]);
        if (have[0]) {
            /* the size the slots were made with (the disk may have grown since) */
            l->slot_sectors = sb[0].slot_sectors;
            l->nslots = d->sectors >= V4_BASE_LBA + 2u * l->slot_sectors + V4_TAIL_SECTORS ? 2 : 1;
        }
        if (l->nslots == 2) {
            have[1] = read_super_at(d, slot_lba_of(l, 1), LAYOUT_V4, &sb[1]);
            if (have[1] && !have[0]) {
                l->slot_sectors = sb[1].slot_sectors;     /* (it was found where the formula says) */
            }
        }
        if (have[0] || have[1]) return 1;
    }
    layout_v3(d, l);
    have[0] = read_super_at(d, slot_lba_of(l, 0), 3, &sb[0]);
    if (l->nslots == 2) have[1] = read_super_at(d, slot_lba_of(l, 1), 3, &sb[1]);
    return have[0] || have[1];
}

/* the newest good copy's slot; reads it into a fresh buffer (header included) */
static int read_newest(const ata_disk_t* d, const layout_t* l, const fsdisk_super_t sb[2], const int have[2],
                       uint8_t** out, int* slot_out, int skip_first) {
    int first = (have[1] && (!have[0] || sb[1].seq > sb[0].seq)) ? 1 : 0;
    int order[2] = { first, first ^ 1 };
    for (int k = skip_first ? 1 : 0; k < 2; k++) {
        int s = order[k];
        if (!have[s]) continue;
        uint32_t sectors = bytes_to_sectors(hdr_size(l->version) + sb[s].payload_bytes);
        if (slot_lba_of(l, s) + sectors > d->sectors) continue;
        uint8_t* buf = (uint8_t*)kmalloc(sectors * FSDISK_SECTOR);
        if (!buf) continue;
        if (disk_read(d, slot_lba_of(l, s), sectors, buf) == 0 &&
            checksum_of(buf + hdr_size(l->version), sb[s].payload_bytes) == sb[s].checksum) {
            *out = buf;
            *slot_out = s;
            return k;          /* 0: the newest, 1: the older one */
        }
        kfree(buf);
    }
    return -1;
}

/* a copy left by an earlier install must never be taken for this one */
static void wipe_header(const ata_disk_t* d, uint32_t lba) {
    if (lba >= d->sectors) return;
    memset(g_buf, 0, FSDISK_SECTOR);
    disk_write(d, lba, 1, g_buf);
}

/* ── layout 5: every file in a place of its own ─────────────────────
 * Layouts 3 and 4 wrote the whole filesystem as one blob on every save -
 * hundreds of MB for one changed setting, while nothing else could run.
 * Layout 5 works like the file systems of other systems:
 *
 *   128 MB          the mark: says the disk is layout 5, where its parts are
 *   128 MB + 4 KiB  the files' area: each file's data in one extent of 4 KiB
 *                   blocks, found in the free space between the others
 *   end - 1 MB      two metadata slots (256 KiB each): the folders and, for
 *                   every file, its name, folder, size, extent and checksum
 *
 * A save writes only the files whose data changed since the last one - each
 * into a NEW extent, never over the one the saved metadata points at - then
 * the metadata into the slot not holding the newest copy (copy-on-write).
 * A power cut at any moment leaves the previous save whole. The extents
 * the new metadata no longer uses are free from then on. Renaming, moving
 * or deleting a file writes nothing but the metadata.
 *
 * When the metadata outgrows its slot (thousands of files), the slot only
 * holds its header (version 6) and the body goes into an extent of the
 * files' area, copy-on-write like the files. Version 5 (in the slot) is
 * still written while it fits, so an older Banana OS can still read it.
 *
 * Files are not read at boot: each is read in the first time it is used
 * (v5_read_file), so what the disk holds is not bounded by the RAM.
 *
 * Older disks are moved to layout 5 at boot (v5_migrate), the copy they
 * were loaded from untouched until the new one is complete. */

#define V5_MAGIC        0x354E4142u    /* "BAN5" */
#define V5_MAGIC2       0x4B52414Du    /* "MARK" */
#define V5_BLOCK_SECT   8u             /* 4 KiB blocks */
#define V5_META_SECT    512u           /* per metadata slot */
#define V5_NAME         32u            /* names in version 5/6 records (Banana OS before build 151) */
#define V5_MAX_DIRS     16384u         /* ... and the tables of then */
#define V5_MAX_FILES    65536u
#define V5_DIR_REC      (4u + V5_NAME + 4u)                   /* idx, name, parent */
#define V5_FILE_REC     (4u + V5_NAME + 4u + 4u * 3u)         /* idx, name, parent, size, lba, check */
#define V5_TIMES_MAGIC  0x454D4954u    /* "TIME": the dates, after the records */
#define V5_TIME_REC     12u            /* idx, mtime, ctime */
#define V5_BODY_MAX     (4u + V5_MAX_DIRS * V5_DIR_REC + 8u + V5_MAX_FILES * V5_FILE_REC + \
                         12u + (V5_MAX_DIRS + V5_MAX_FILES) * V5_TIME_REC)
#define V5_INLINE_MAX   ((V5_META_SECT - 1u) * FSDISK_SECTOR) /* a body that fits in the slot */

/* Version 7 (build 151): ext4-like metadata. Names up to 255 bytes, a mode,
 * an owner, a group and three dates per entry, and CRC-32C (ext4's metadata
 * checksum) over the header, the body and every file written since. The
 * body is inline in the slot while it fits, else in an extent like v6's.
 *
 *   body:  "BFS7", home, ndirs, dir records, nfiles, file records
 *   dir:   idx, parent, mode:16, namelen:16, uid, gid, mtime, ctime, atime, name (padded to 4)
 *   file:  idx, parent, size, lba, check, mode:16, namelen:16, uid, gid,
 *          mtime, ctime, atime, flags (V7_F_CRC: check is CRC-32C), name (padded to 4)
 *
 * Banana OS before build 151 does not read version 7 (it would load its
 * older copy): going back to an older release is not supported. */
#define V7_BODY_MAGIC   0x37534642u    /* "BFS7" */
#define V7_DIR_FIXED    32u
#define V7_FILE_FIXED   48u
#define V7_F_CRC        1u
#define V7_BODY_MAX     (512u * 1024u * 1024u)
/* where the fields a save fills in late sit in a file record */
#define V7_OFF_SIZE     8u
#define V7_OFF_LBA      12u
#define V7_OFF_CHECK    16u
#define V7_OFF_FLAGS    44u

typedef struct __attribute__((packed)) {
    uint32_t magic, magic2;
    uint32_t version;
    uint32_t nonce;        /* this install's: metadata from an earlier one is ignored */
    uint32_t data_lba;     /* the files' area: [data_lba, data_end) */
    uint32_t data_end;
    uint32_t meta_lba;     /* the two metadata slots */
    uint32_t check;
} v5_mark_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t version;
    uint32_t nonce;
    uint32_t seq;
    uint32_t body_bytes;
    uint32_t body_check;
    uint32_t check;
} v5_meta_t;

/* version 6: the same, the body at body_lba in the files' area */
typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t version;
    uint32_t nonce;
    uint32_t seq;
    uint32_t body_bytes;
    uint32_t body_check;
    uint32_t body_lba;
    uint32_t check;
} v6_meta_t;

/* where each file's saved copy is (by file index) */
typedef struct {
    uint8_t  valid;
    uint8_t  crc;          /* check is CRC-32C (written since version 7), else checksum_of() */
    uint32_t gen;          /* the file's data_gen when it was written */
    uint32_t lba, sectors; /* its extent (0 sectors: an empty file) */
    uint32_t size, check;
} v5_rec_t;

/* the parts of the disk in use, sorted by lba: the files' extents, the
 * metadata body's, and while moving an older disk to layout 5, the copy
 * being moved */
typedef struct { uint32_t lba, sectors; } v5_ext_t;

static int       g_v5;                     /* the installed disk is layout 5 */
static uint32_t  g_swap_lba, g_swap_sect;   /* the swap area (end of this file) */
static v5_mark_t g_mark;
static v5_rec_t* g_rec;                    /* by file index (grows with the file table) */
static int       g_nrecs;
static v5_ext_t* g_ext;
static int       g_next, g_ext_cap;
static uint32_t  g_body_lba, g_body_sect;  /* the saved metadata body's extent (version 6) */

static uint32_t mark_check(const v5_mark_t* m) {
    return checksum_of((const uint8_t*)m, (uint32_t)sizeof(*m) - 4u) ^ 0x5A5A1234u;
}
static uint32_t meta_check(const v5_meta_t* h) {
    return checksum_of((const uint8_t*)h, (uint32_t)sizeof(*h) - 4u) ^ 0xC0FFEE55u;
}
static uint32_t meta6_check(const v6_meta_t* h) {
    return checksum_of((const uint8_t*)h, (uint32_t)sizeof(*h) - 4u) ^ 0xC0FFEE66u;
}
/* version 7 has the v6 header's fields; CRC-32C seals it */
static uint32_t meta7_check(const v6_meta_t* h) {
    return crc32c(0x7BFA7A11u, h, (uint32_t)sizeof(*h) - 4u);
}

/* a file's saved data against its record's check */
static uint32_t data_check(int crc, const uint8_t* buf, uint32_t len) {
    return crc ? crc32c(0, buf, len) : checksum_of(buf, len);
}

/* records for file indexes below n: 0, or -1 (no memory) */
static int rec_grow(int n) {
    if (n <= g_nrecs) return 0;
    v5_rec_t* r = (v5_rec_t*)krealloc(g_rec, (size_t)n * sizeof(v5_rec_t));
    if (!r) return -1;
    memset(r + g_nrecs, 0, (size_t)(n - g_nrecs) * sizeof(v5_rec_t));
    g_rec = r;
    g_nrecs = n;
    return 0;
}

static void rec_reset(void) {
    if (g_rec) memset(g_rec, 0, (size_t)g_nrecs * sizeof(v5_rec_t));
}

static void ext_reset(void) { g_next = 0; }

static int ext_room(void) {
    if (g_next < g_ext_cap) return 0;
    int cap = g_ext_cap ? g_ext_cap * 2 : 1024;
    v5_ext_t* e = (v5_ext_t*)krealloc(g_ext, (size_t)cap * sizeof(v5_ext_t));
    if (!e) return -1;
    g_ext = e;
    g_ext_cap = cap;
    return 0;
}

/* at the end, unsorted (loading: ext_sort once afterwards) */
static int ext_push(uint32_t lba, uint32_t sectors) {
    if (!sectors) return 0;
    if (ext_room() != 0) return -1;
    g_ext[g_next].lba = lba;
    g_ext[g_next].sectors = sectors;
    g_next++;
    return 0;
}

/* heapsort by lba: tens of thousands of extents at boot */
static void ext_sift(int i, int n) {
    for (;;) {
        int c = 2 * i + 1;
        if (c >= n) return;
        if (c + 1 < n && g_ext[c + 1].lba > g_ext[c].lba) c++;
        if (g_ext[i].lba >= g_ext[c].lba) return;
        v5_ext_t t = g_ext[i]; g_ext[i] = g_ext[c]; g_ext[c] = t;
        i = c;
    }
}

static void ext_sort(void) {
    for (int i = g_next / 2 - 1; i >= 0; i--) ext_sift(i, g_next);
    for (int n = g_next - 1; n > 0; n--) {
        v5_ext_t t = g_ext[0]; g_ext[0] = g_ext[n]; g_ext[n] = t;
        ext_sift(0, n);
    }
}

static int ext_add(uint32_t lba, uint32_t sectors) {
    if (!sectors) return 0;
    if (ext_room() != 0) return -1;
    int i = g_next;
    while (i > 0 && g_ext[i - 1].lba > lba) { g_ext[i] = g_ext[i - 1]; i--; }
    g_ext[i].lba = lba;
    g_ext[i].sectors = sectors;
    g_next++;
    return 0;
}

static void ext_remove(uint32_t lba, uint32_t sectors) {
    if (!sectors) return;
    for (int i = 0; i < g_next; i++) {
        if (g_ext[i].lba != lba) continue;
        for (int j = i; j + 1 < g_next; j++) g_ext[j] = g_ext[j + 1];
        g_next--;
        return;
    }
}

/* the first free stretch of the files' area that holds `sectors` (0: full) */
static uint32_t ext_alloc(uint32_t sectors) {
    uint32_t cur = g_mark.data_lba;
    for (int i = 0; i <= g_next; i++) {
        uint32_t next = i < g_next ? g_ext[i].lba : g_mark.data_end;
        if (next > g_mark.data_end) next = g_mark.data_end;
        if (next > cur && next - cur >= sectors) {
            if (ext_add(cur, sectors) != 0) return 0;
            return cur;
        }
        if (i < g_next) {
            uint32_t end = g_ext[i].lba + g_ext[i].sectors;
            end = (end + V5_BLOCK_SECT - 1u) & ~(V5_BLOCK_SECT - 1u);
            if (end > cur) cur = end;
        }
    }
    return 0;
}

static uint32_t v5_sectors_for(uint32_t size) {
    return ((size + 4095u) / 4096u) * V5_BLOCK_SECT;
}

static void v5_new_mark(const ata_disk_t* d, v5_mark_t* m) {
    memset(m, 0, sizeof(*m));
    m->magic = V5_MAGIC;
    m->magic2 = V5_MAGIC2;
    m->version = 5;
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    m->nonce = (lo ^ (hi * 2654435761u) ^ timer_ms()) | 1u;
    m->data_lba = V4_BASE_LBA + V5_BLOCK_SECT;
    m->meta_lba = (d->sectors - V4_TAIL_SECTORS) & ~(V5_BLOCK_SECT - 1u);
    m->data_end = m->meta_lba;
    m->check = mark_check(m);
}

/* a layout-5 disk: its mark */
static int v5_detect(const ata_disk_t* d, v5_mark_t* out) {
    if (d->sectors <= V4_BASE_LBA + V4_TAIL_SECTORS + 2048u) return 0;
    uint8_t sec[FSDISK_SECTOR];
    if (disk_read(d, V4_BASE_LBA, 1, sec) != 0) return 0;
    v5_mark_t m;
    memcpy(&m, sec, sizeof(m));
    if (m.magic != V5_MAGIC || m.magic2 != V5_MAGIC2 || m.version != 5 || m.check != mark_check(&m)) return 0;
    if (m.data_lba < V4_BASE_LBA + V5_BLOCK_SECT || m.data_end <= m.data_lba || m.meta_lba < m.data_end ||
        m.meta_lba + 2u * V5_META_SECT > d->sectors)
        return 0;
    *out = m;
    return 1;
}

/* a metadata slot's header, either version */
typedef struct {
    uint32_t version;           /* 5, 6 or 7 */
    uint32_t seq, body_bytes, body_check;
    uint32_t body_lba;          /* 0: right after the header, in the slot (version 5, inline 7) */
} meta_info_t;

/* one metadata slot, checked: its header and body (kfree the body) */
static int v5_read_meta(const ata_disk_t* d, const v5_mark_t* m, int slot, meta_info_t* h, uint8_t** body) {
    uint32_t lba = m->meta_lba + (uint32_t)slot * V5_META_SECT;
    uint8_t sec[FSDISK_SECTOR];
    if (disk_read(d, lba, 1, sec) != 0) return 0;
    v5_meta_t h5;
    v6_meta_t h6;
    memcpy(&h5, sec, sizeof(h5));
    memcpy(&h6, sec, sizeof(h6));
    if (h5.magic != V5_MAGIC || h5.nonce != m->nonce) return 0;
    uint32_t body_lba;
    if (h5.version == 5) {
        if (h5.check != meta_check(&h5) || h5.body_bytes > V5_INLINE_MAX) return 0;
        h->seq = h5.seq; h->body_bytes = h5.body_bytes; h->body_check = h5.body_check; h->body_lba = 0;
        body_lba = lba + 1u;
    } else if (h6.version == 6 || h6.version == 7) {
        if (h6.check != (h6.version == 7 ? meta7_check(&h6) : meta6_check(&h6))) return 0;
        h->seq = h6.seq; h->body_bytes = h6.body_bytes; h->body_check = h6.body_check; h->body_lba = h6.body_lba;
        body_lba = h6.body_lba;
        if (h6.version == 7 && !body_lba) {                 /* inline, after the header */
            if (h6.body_bytes > V5_INLINE_MAX) return 0;
            body_lba = lba + 1u;
        } else if (body_lba < m->data_lba || body_lba + bytes_to_sectors(h6.body_bytes) > m->data_end) {
            return 0;
        }
    } else {
        return 0;
    }
    h->version = h5.version;
    if (h->body_bytes < 12u || h->body_bytes > (h->version == 7 ? V7_BODY_MAX : V5_BODY_MAX)) return 0;
    uint32_t sectors = bytes_to_sectors(h->body_bytes);
    uint8_t* b = (uint8_t*)kmalloc(sectors * FSDISK_SECTOR);
    if (!b) return 0;
    int rd = 0;
    for (uint32_t s = 0; s < sectors && rd == 0; s += WRITE_CHUNK_SECTORS) {   /* (a big body: in pieces) */
        uint32_t n = sectors - s < WRITE_CHUNK_SECTORS ? sectors - s : WRITE_CHUNK_SECTORS;
        rd = disk_read(d, body_lba + s, n, b + s * FSDISK_SECTOR);
    }
    if (rd != 0 || data_check(h->version == 7, b, h->body_bytes) != h->body_check) {
        kfree(b);
        return 0;
    }
    *body = b;
    return 1;
}

static uint32_t rd32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }
static void     wr32(uint8_t* p, uint32_t v) { memcpy(p, &v, 4); }

/* checks a metadata body before anything of the live tree is replaced */
static int v5_body_ok(const uint8_t* b, uint32_t len, const v5_mark_t* m) {
    uint32_t off = 0, nd = rd32(b);
    off = 4;
    if (nd > V5_MAX_DIRS || off + nd * V5_DIR_REC + 8u > len) return 0;
    off += nd * V5_DIR_REC + 4u;
    uint32_t nf = rd32(b + off);
    off += 4;
    if (nf > V5_MAX_FILES || off + nf * V5_FILE_REC > len) return 0;
    for (uint32_t i = 0; i < nf; i++, off += V5_FILE_REC) {
        const uint8_t* r = b + off;
        uint32_t idx = rd32(r), size = rd32(r + 4 + V5_NAME + 4), lba = rd32(r + 4 + V5_NAME + 8);
        if (idx >= V5_MAX_FILES || size > FS_MAX_FILE_SIZE) return 0;
        if (size && (lba < m->data_lba || lba + v5_sectors_for(size) > m->data_end)) return 0;
    }
    return 1;
}

/* Rebuilds the tree from a checked body. The files' data stays on the disk
 * until used (v5_read_file): a boot no longer reads every file. */
static void v5_load_body(const uint8_t* b, uint32_t len) {
    uint32_t off = 4, nd = rd32(b);
    char name[V5_NAME];
    fs_restore_begin();
    for (uint32_t i = 0; i < nd; i++, off += V5_DIR_REC) {
        memcpy(name, b + off + 4, V5_NAME);
        name[V5_NAME - 1] = '\0';
        fs_restore_dir((int)rd32(b + off), name, (int32_t)rd32(b + off + 4 + V5_NAME));
    }
    int home = (int32_t)rd32(b + off);
    off += 4;
    uint32_t nf = rd32(b + off);
    off += 4;
    ext_reset();
    rec_reset();
    for (uint32_t i = 0; i < nf; i++, off += V5_FILE_REC) {
        const uint8_t* r = b + off;
        uint32_t idx = rd32(r), size = rd32(r + 4 + V5_NAME + 4);
        uint32_t lba = rd32(r + 4 + V5_NAME + 8), check = rd32(r + 4 + V5_NAME + 12);
        int parent = (int32_t)rd32(r + 4 + V5_NAME);
        memcpy(name, r + 4, V5_NAME);
        name[V5_NAME - 1] = '\0';
        uint32_t sectors = v5_sectors_for(size);
        if (rec_grow((int)idx + 1) != 0 || fs_restore_file_lazy((int)idx, name, parent, size) != 0) continue;
        v5_rec_t* rec = &g_rec[idx];
        rec->valid = 1;
        rec->gen = fs_file_info((int)idx)->data_gen;
        rec->lba = sectors ? lba : 0;
        rec->sectors = sectors;
        rec->size = size;
        rec->check = check;
        ext_push(rec->lba, sectors);
    }
    /* the dates (saved since build 103; older saves have none: 0) */
    if (off + 8u <= len && rd32(b + off) == V5_TIMES_MAGIC) {
        off += 4;
        for (int kind = 1; kind >= 0 && off + 4u <= len; kind--) {
            uint32_t n = rd32(b + off);
            off += 4;
            for (uint32_t k = 0; k < n && off + V5_TIME_REC <= len; k++, off += V5_TIME_REC)
                fs_set_times(kind, (int)rd32(b + off), rd32(b + off + 4), rd32(b + off + 8));
        }
    }
    ext_sort();
    fs_restore_end(home);
}

static uint16_t rd16(const uint8_t* p) { uint16_t v; memcpy(&v, p, 2); return v; }
static void     wr16(uint8_t* p, uint16_t v) { memcpy(p, &v, 2); }
static uint32_t pad4(uint32_t n) { return (n + 3u) & ~3u; }

/* a version-7 body, checked to the last byte before anything is replaced */
static int v7_body_ok(const uint8_t* b, uint32_t len, const v5_mark_t* m) {
    if (len < 16u || rd32(b) != V7_BODY_MAGIC) return 0;
    uint32_t off = 12, nd = rd32(b + 8);
    if (nd > FS_MAX_DIRS) return 0;
    for (uint32_t i = 0; i < nd; i++) {
        if (off + V7_DIR_FIXED > len) return 0;
        uint32_t idx = rd32(b + off), nl = rd16(b + off + 10);
        if (idx >= FS_MAX_DIRS || nl == 0 || nl >= FS_NAME_LEN) return 0;
        off += V7_DIR_FIXED + pad4(nl);
        if (off > len) return 0;
    }
    if (off + 4u > len) return 0;
    uint32_t nf = rd32(b + off);
    off += 4;
    if (nf > FS_MAX_FILES) return 0;
    for (uint32_t i = 0; i < nf; i++) {
        if (off + V7_FILE_FIXED > len) return 0;
        const uint8_t* r = b + off;
        uint32_t idx = rd32(r), size = rd32(r + V7_OFF_SIZE), lba = rd32(r + V7_OFF_LBA), nl = rd16(r + 22);
        if (idx >= FS_MAX_FILES || size > FS_MAX_FILE_SIZE || nl == 0 || nl >= FS_NAME_LEN) return 0;
        if (size && (lba < m->data_lba || lba + v5_sectors_for(size) > m->data_end)) return 0;
        off += V7_FILE_FIXED + pad4(nl);
        if (off > len) return 0;
    }
    return 1;
}

static void v7_load_body(const uint8_t* b) {
    char name[FS_NAME_LEN];
    uint32_t off = 12, nd = rd32(b + 8);
    int home = (int32_t)rd32(b + 4);
    fs_restore_begin();
    for (uint32_t i = 0; i < nd; i++) {
        const uint8_t* r = b + off;
        uint32_t idx = rd32(r), nl = rd16(r + 10);
        memcpy(name, r + V7_DIR_FIXED, nl);
        name[nl] = '\0';
        fs_restore_dir((int)idx, name, (int32_t)rd32(r + 4));          /* (the root, #0, is there already) */
        fs_set_times(1, (int)idx, rd32(r + 20), rd32(r + 24));
        fs_set_meta(1, (int)idx, rd16(r + 8), rd32(r + 12), rd32(r + 16), rd32(r + 28));
        off += V7_DIR_FIXED + pad4(nl);
    }
    uint32_t nf = rd32(b + off);
    off += 4;
    ext_reset();
    rec_reset();
    for (uint32_t i = 0; i < nf; i++) {
        const uint8_t* r = b + off;
        uint32_t idx = rd32(r), size = rd32(r + V7_OFF_SIZE), nl = rd16(r + 22);
        off += V7_FILE_FIXED + pad4(nl);
        memcpy(name, r + V7_FILE_FIXED, nl);
        name[nl] = '\0';
        uint32_t sectors = v5_sectors_for(size);
        if (rec_grow((int)idx + 1) != 0 || fs_restore_file_lazy((int)idx, name, (int32_t)rd32(r + 4), size) != 0) continue;
        fs_set_times(0, (int)idx, rd32(r + 32), rd32(r + 36));
        fs_set_meta(0, (int)idx, rd16(r + 20), rd32(r + 24), rd32(r + 28), rd32(r + 40));
        v5_rec_t* rec = &g_rec[idx];
        rec->valid = 1;
        rec->crc = (rd32(r + V7_OFF_FLAGS) & V7_F_CRC) ? 1 : 0;
        rec->gen = fs_file_info((int)idx)->data_gen;
        rec->lba = sectors ? rd32(r + V7_OFF_LBA) : 0;
        rec->sectors = sectors;
        rec->size = size;
        rec->check = rd32(r + V7_OFF_CHECK);
        ext_push(rec->lba, sectors);
    }
    ext_sort();
    fs_restore_end(home);
}

/* Reads a file's saved copy for fs.c, the first time the file is used. The
 * extent stays put meanwhile: a save moves a file only once it changed, and
 * a change (or a delete) waits until it is read in. */
static int v5_read_file(int idx, uint8_t* buf, uint32_t size) {
    if (!g_v5 || !g_have_target || idx < 0 || idx >= g_nrecs) return -1;
    v5_rec_t r = g_rec[idx];
    if (!r.valid || r.size != size) return -1;
    if (!size) return 0;
    uint32_t full = size / FSDISK_SECTOR, tail = size % FSDISK_SECTOR, done = 0;
    int rc = 0;
    while (done < full && rc == 0) {
        uint32_t n = full - done < WRITE_CHUNK_SECTORS ? full - done : WRITE_CHUNK_SECTORS;
        dlock();
        rc = disk_read(&g_target, r.lba + done, n, buf + done * FSDISK_SECTOR);
        dunlock();
        done += n;
        if (done < full) task_yield();       /* a big file: the desktop keeps going */
    }
    if (rc == 0 && tail) {
        uint8_t sec[FSDISK_SECTOR];
        dlock();
        rc = disk_read(&g_target, r.lba + full, 1, sec);
        dunlock();
        memcpy(buf + full * FSDISK_SECTOR, sec, tail);
    }
    if (rc != 0) return -1;
    if (data_check(r.crc, buf, size) != r.check) {
        klog("fsdisk: file #%d could not be read back (damaged)\n", idx);
        return -1;
    }
    return 0;
}

/* Part of a file, straight from its extent (big files: apps, music). Read
 * 64 KiB at a time into a read-ahead buffer, so reading a file in small
 * steps costs one disk command per 64 KiB. (The file's checksum covers it
 * whole: a part cannot be checked.) */
#define RA_BYTES (64u * 1024u)
static uint8_t* g_ra;
static int      g_ra_idx = -1;
static uint32_t g_ra_lba, g_ra_off, g_ra_len;

static int v5_read_range(int idx, uint32_t off, uint8_t* buf, uint32_t len) {
    if (!g_v5 || !g_have_target || idx < 0 || idx >= g_nrecs) return -1;
    v5_rec_t r = g_rec[idx];
    if (!r.valid || off > r.size || len > r.size - off) return -1;
    if (!g_ra && !(g_ra = (uint8_t*)kmalloc(RA_BYTES))) return -1;
    while (len) {
        if (g_ra_idx == idx && g_ra_lba == r.lba && off >= g_ra_off && off < g_ra_off + g_ra_len) {
            uint32_t n = g_ra_off + g_ra_len - off;
            if (n > len) n = len;
            memcpy(buf, g_ra + (off - g_ra_off), n);
            buf += n;
            off += n;
            len -= n;
            continue;
        }
        /* the 64 KiB from the sector holding off (the extent is whole 4 KiB blocks) */
        uint32_t start = off & ~(FSDISK_SECTOR - 1u), n = r.size - start;
        if (n > RA_BYTES) n = RA_BYTES;
        g_ra_idx = -1;
        dlock();
        int rc = disk_read(&g_target, r.lba + start / FSDISK_SECTOR, bytes_to_sectors(n), g_ra);
        dunlock();
        if (rc != 0) return -1;
        g_ra_idx = idx;
        g_ra_lba = r.lba;
        g_ra_off = start;
        g_ra_len = n;
    }
    return 0;
}

/* the saved copy is the file as it is now: its RAM copy can be dropped */
static int v5_clean(int idx, uint32_t data_gen) {
    return g_v5 && idx >= 0 && idx < g_nrecs && g_rec[idx].valid && g_rec[idx].gen == data_gen;
}

static void set_disk_ops(void) {
    fs_set_disk_ops(v5_read_file, v5_read_range, v5_clean);
}

/* A layout-5 disk: the newest good metadata (and every file) loaded.
 * 1 loaded, 0 not layout 5, -1 layout 5 but no metadata could be read. */
static int v5_try_load(const ata_disk_t* d) {
    v5_mark_t m;
    if (!v5_detect(d, &m)) return 0;
    meta_info_t h[2];
    uint8_t* body[2] = { NULL, NULL };
    int have[2];
    for (int s = 0; s < 2; s++) have[s] = v5_read_meta(d, &m, s, &h[s], &body[s]);
    int first = (have[1] && (!have[0] || h[1].seq > h[0].seq)) ? 1 : 0;
    int rc = -1;
    for (int k = 0; k < 2 && rc < 0; k++) {
        int s = k ? first ^ 1 : first;
        if (!have[s]) continue;
        if (h[s].version == 7 ? !v7_body_ok(body[s], h[s].body_bytes, &m) : !v5_body_ok(body[s], h[s].body_bytes, &m))
            continue;
        g_mark = m;
        if (h[s].version == 7) v7_load_body(body[s]);
        else v5_load_body(body[s], h[s].body_bytes);
        g_body_lba = h[s].body_lba;
        g_body_sect = g_body_lba ? bytes_to_sectors(h[s].body_bytes) : 0;
        ext_add(g_body_lba, g_body_sect);
        g_cur_slot = s;
        g_seq = h[s].seq;
        rc = 1;
    }
    for (int s = 0; s < 2; s++) if (body[s]) kfree(body[s]);
    if (rc == 1) {
        g_v5 = 1;
        set_disk_ops();
    }
    return rc;
}

/* writes one file's private copy into a fresh extent */
static int v5_write_file(const ata_disk_t* d, v5_rec_t* rec, const uint8_t* data) {
    rec->sectors = v5_sectors_for(rec->size);
    rec->crc = 1;
    rec->check = crc32c(0, data, rec->size);
    if (!rec->sectors) { rec->lba = 0; return FSDISK_OK; }
    rec->lba = ext_alloc(rec->sectors);
    if (!rec->lba) { rec->sectors = 0; return FSDISK_ERR_FULL; }
    /* the data's own sectors (not a byte past it: it is the file's buffer), the last one padded */
    uint32_t full = rec->size / FSDISK_SECTOR, tail = rec->size % FSDISK_SECTOR;
    if (full && write_chunked(d, rec->lba, full, data) != 0) return FSDISK_ERR_IO;
    if (tail) {
        uint8_t sec[FSDISK_SECTOR];
        memset(sec, 0, sizeof(sec));
        memcpy(sec, data + full * FSDISK_SECTOR, tail);
        if (locked_write(d, rec->lba + full, 1, sec) != 0) return FSDISK_ERR_IO;
    }
    return FSDISK_OK;
}

/* A save: the changed files, then the metadata (see above). The tree is
 * read in one go before anything is written - the changed files' data is
 * copied - so the other tasks can keep changing files while it writes. */
static int v5_sync(const ata_disk_t* d) {
    uint32_t gen = fs_generation();
    int nfs = fs_file_slots(), nds = fs_dir_slots();     /* (files made while this writes: the next save) */
    /* the body's size: every entry that may be saved, with its name */
    uint64_t need = 16u;
    for (int i = 0; i < nds; i++) {
        const fs_dir_t* dir = fs_get_dir(i);
        if (dir && dir->used && !dir->mnt) need += V7_DIR_FIXED + pad4((uint32_t)strlen(dir->name));
    }
    for (int i = 0; i < nfs; i++) {
        fs_file_t* f = fs_file_info(i);
        if (f && f->used && !f->mnt) need += V7_FILE_FIXED + pad4((uint32_t)strlen(f->name));
    }
    if (need > V7_BODY_MAX) return FSDISK_ERR_FULL;
    uint32_t body_sect = bytes_to_sectors((uint32_t)need);
    if (rec_grow(nfs) != 0) return FSDISK_ERR_IO;
    uint8_t* meta = (uint8_t*)kzalloc((1u + body_sect) * FSDISK_SECTOR);
    uint32_t* rec_off = (uint32_t*)kzalloc((size_t)(nfs + 1) * sizeof(uint32_t));
    v5_rec_t* g_nrec = (v5_rec_t*)kzalloc((size_t)(nfs + 1) * sizeof(v5_rec_t));   /* the save being written */
    if (!meta || !rec_off || !g_nrec) { kfree(meta); kfree(rec_off); kfree(g_nrec); return FSDISK_ERR_IO; }
    uint8_t* b = meta + FSDISK_SECTOR;
    int rc = FSDISK_OK;

    /* 1. the tree as it is now (nothing yields in here): version 7 */
    uint32_t off = 12, nd = 0;
    wr32(b, V7_BODY_MAGIC);
    wr32(b + 4, (uint32_t)fs_home_dir());
    for (int i = 0; i < nds; i++) {
        const fs_dir_t* dir = fs_get_dir(i);
        if (!dir || !dir->used || dir->mnt) continue;
        uint32_t nl = (uint32_t)strlen(dir->name);
        if (!nl) continue;
        uint8_t* r = b + off;
        wr32(r, (uint32_t)i);
        wr32(r + 4, (uint32_t)dir->parent_dir);
        wr16(r + 8, (uint16_t)fs_dir_mode(i));
        wr16(r + 10, (uint16_t)nl);
        wr32(r + 12, dir->uid);
        wr32(r + 16, dir->gid);
        wr32(r + 20, dir->mtime);
        wr32(r + 24, dir->ctime);
        wr32(r + 28, dir->atime);
        memcpy(r + V7_DIR_FIXED, dir->name, nl);
        off += V7_DIR_FIXED + pad4(nl);
        nd++;
    }
    wr32(b + 8, nd);
    uint32_t nf_off = off, nf = 0;
    off += 4;
    for (int i = 0; i < nfs; i++) {
        fs_file_t* f = fs_file_info(i);
        v5_rec_t* n = &g_nrec[i];
        if (!f || !f->used || f->mnt) continue;
        if (g_rec[i].valid && (g_rec[i].gen == f->data_gen || !f->loaded)) {
            *n = g_rec[i];                                  /* unchanged: stays where it is */
        } else if (!f->loaded) {
            continue;                                       /* (no data to save: never read in) */
        } else {
            n->valid = 1;                                   /* changed: written in step 2 */
            n->gen = f->data_gen;
            n->size = f->size;
        }
        uint32_t nl = (uint32_t)strlen(f->name);
        if (!nl) { *n = (v5_rec_t){0}; continue; }
        uint8_t* r = b + off;
        wr32(r, (uint32_t)i);
        wr32(r + 4, (uint32_t)f->parent_dir);
        wr32(r + V7_OFF_SIZE, n->size);
        wr16(r + 20, (uint16_t)fs_file_mode(i));
        wr16(r + 22, (uint16_t)nl);
        wr32(r + 24, f->uid);
        wr32(r + 28, f->gid);
        wr32(r + 32, f->mtime);
        wr32(r + 36, f->ctime);
        wr32(r + 40, f->atime);
        memcpy(r + V7_FILE_FIXED, f->name, nl);
        rec_off[i] = off;
        off += V7_FILE_FIXED + pad4(nl);
        nf++;
    }
    wr32(b + nf_off, nf);
    uint32_t body_bytes = off;

    /* 2. the changed files, each into a new extent (the other tasks run in
     * between). Written straight from the file's RAM - no copy, which would
     * need as much memory again: while one is written it counts as being
     * loaded, so a change to it waits (reading it does not). */
    for (int i = 0; i < nfs && rc == FSDISK_OK; i++) {
        v5_rec_t* n = &g_nrec[i];
        if (!n->valid || (g_rec[i].valid && g_rec[i].gen == n->gen)) continue;
        fs_file_t* f = fs_file_info(i);
        if (!f || !f->used || f->mnt || !f->loaded || f->loading || f->data_gen != n->gen) {
            /* changed again (or deleted) while the files before it were written:
             * this save keeps its last saved copy, the next one writes it */
            if (g_rec[i].valid) *n = g_rec[i];
            else { n->size = 0; n->gen = 0; rc = v5_write_file(d, n, (const uint8_t*)""); }
        } else {
            f->loading = 1;
            rc = v5_write_file(d, n, (const uint8_t*)f->content);
            f->loading = 0;
        }
        if (rec_off[i]) wr32(b + rec_off[i] + V7_OFF_SIZE, n->size);
        task_yield();
    }
    uint32_t new_body_lba = 0, new_body_sect = 0;
    if (rc == FSDISK_OK) {
        for (int i = 0; i < nfs; i++) {
            if (!g_nrec[i].valid || !rec_off[i]) continue;
            wr32(b + rec_off[i] + V7_OFF_LBA, g_nrec[i].lba);
            wr32(b + rec_off[i] + V7_OFF_CHECK, g_nrec[i].check);
            wr32(b + rec_off[i] + V7_OFF_FLAGS, g_nrec[i].crc ? V7_F_CRC : 0u);
        }
        /* 3. the data is on the disk before the metadata that points at it */
        int slot = g_cur_slot ^ 1;
        uint32_t lba = g_mark.meta_lba + (uint32_t)slot * V5_META_SECT;
        uint32_t seq = g_seq + 1, body_check = crc32c(0, b, body_bytes);
        uint32_t hdr_sectors = 1u + bytes_to_sectors(body_bytes);     /* the header, and the body after it */
        if (body_bytes > V5_INLINE_MAX) {
            /* too big for the slot: the body into an extent of its own, the slot gets the header */
            new_body_sect = bytes_to_sectors(body_bytes);
            new_body_lba = ext_alloc(new_body_sect);
            if (!new_body_lba) { new_body_sect = 0; rc = FSDISK_ERR_FULL; }
            else if (write_chunked(d, new_body_lba, new_body_sect, b) != 0) rc = FSDISK_ERR_IO;
            hdr_sectors = 1;
        }
        v6_meta_t h;
        memset(&h, 0, sizeof(h));
        h.magic = V5_MAGIC;
        h.version = 7;
        h.nonce = g_mark.nonce;
        h.seq = seq;
        h.body_bytes = body_bytes;
        h.body_check = body_check;
        h.body_lba = new_body_lba;                                  /* 0: inline, after the header */
        h.check = meta7_check(&h);
        memset(meta, 0, FSDISK_SECTOR);
        memcpy(meta, &h, sizeof(h));
        if (rc == FSDISK_OK) {
            if (locked_flush(d) != 0 || locked_write(d, lba, hdr_sectors, meta) != 0 || locked_flush(d) != 0)
                rc = FSDISK_ERR_IO;
            else {
                g_cur_slot = slot;
                g_seq = seq;
            }
        }
    }
    /* the metadata body extent: the new one is in use now, or was never used */
    if (rc == FSDISK_OK) {
        ext_remove(g_body_lba, g_body_sect);
        g_body_lba = new_body_lba;
        g_body_sect = new_body_sect;
    } else {
        ext_remove(new_body_lba, new_body_sect);
    }

    /* 4. what the saved copy no longer uses is free; a failed save frees its own extents */
    for (int i = 0; i < nfs; i++) {
        v5_rec_t* o = &g_rec[i];
        v5_rec_t* n = &g_nrec[i];
        int fresh = n->valid && !(o->valid && o->gen == n->gen);
        if (rc == FSDISK_OK) {
            if (o->valid && (!n->valid || fresh)) ext_remove(o->lba, o->sectors);
            *o = *n;
        } else if (fresh) {
            ext_remove(n->lba, n->sectors);
        }
    }
    kfree(rec_off);
    kfree(g_nrec);
    kfree(meta);
    if (rc == FSDISK_OK) g_synced_gen = gen;
    g_ra_idx = -1;                          /* (extents were freed: the read-ahead may be stale) */
    return rc;
}

/* Makes `d` a layout-5 disk holding the current tree. [keep_lba, keep_lba +
 * keep_sect) - the older copy the tree was loaded from - is not written over
 * until the mark goes on, last: until then the disk is still what it was. */
static int v5_format(const ata_disk_t* d, uint32_t keep_lba, uint32_t keep_sect) {
    v5_mark_t m;
    v5_new_mark(d, &m);
    if (m.data_end <= m.data_lba + 2048u) return FSDISK_ERR_TOO_SMALL;
    if (keep_sect && m.meta_lba < keep_lba + keep_sect && m.meta_lba + 2u * V5_META_SECT > keep_lba)
        return FSDISK_ERR_TOO_SMALL;                         /* (a small layout-3 disk: stays as it is) */
    g_mark = m;
    ext_reset();
    if (keep_sect) ext_add(keep_lba, keep_sect);
    rec_reset();
    g_body_lba = g_body_sect = 0;
    g_cur_slot = 1;                                          /* the first save goes to slot 0 */
    g_seq = 0;
    uint8_t zero[FSDISK_SECTOR];
    memset(zero, 0, sizeof(zero));
    disk_write(d, m.meta_lba + V5_META_SECT, 1, zero);       /* an earlier install's slot 1 */
    int rc = v5_sync(d);
    if (keep_sect) ext_remove(keep_lba, keep_sect);
    if (rc != FSDISK_OK) return rc;
    memcpy(zero, &m, sizeof(m));
    if (disk_write(d, V4_BASE_LBA, 1, zero) != 0 || disk_flush(d) != 0) return FSDISK_ERR_IO;
    g_v5 = 1;
    set_disk_ops();
    return FSDISK_OK;
}

/* ── layout 6: ext4 ─────────────────────────────────────────────────
 * The installed disk's files in an ext4 filesystem (kernel/ext4.c) from
 * the end of the boot area to 1 MB before the disk's end - the same place
 * layout 5 used. Every change goes to it as it is made (a block cache in
 * between, written within a second): RAM is a cache, not a copy to save.
 * Linux mounts its MBR partition (legacy disks: an offset of 128 MiB); e2fsck
 * checks it. Layout-5 disks become ext4 in place at boot (e4_migrate):
 * their files' data stays where it is, the inodes point at it. */

#include "ext4.h"
#include "tz.h"
#include "settings.h"

static int       g_e4;                   /* the installed disk is ext4 */
static ext4_dev_t g_e4dev;
static uint32_t  g_e4_base;              /* the filesystem's first sector on the disk */
static uint32_t  g_swap_ino;
static volatile int g_frozen;
static uint32_t     g_frozen_at;

static int e4_read(void* ctx, uint32_t s, uint32_t n, void* buf) {
    (void)ctx;
    dlock();
    int rc = disk_read(&g_target, g_e4_base + s, n, buf);
    dunlock();
    return rc;
}

static int e4_write(void* ctx, uint32_t s, uint32_t n, const void* buf) {
    (void)ctx;
    /* frozen for a hypervisor's backup: the write waits (10 minutes at most) */
    while (g_frozen && timer_ms() - g_frozen_at < 600000u) task_sleep_ms(50);
    dlock();
    int rc = disk_write(&g_target, g_e4_base + s, n, buf);
    dunlock();
    return rc;
}

static int e4_flush_dev(void* ctx) {
    (void)ctx;
    return locked_flush(&g_target);
}

/* the filesystem's place on disk d */
static void e4_geometry(const ata_disk_t* d, ext4_dev_t* dev, uint32_t* base) {
    *base = boot_partition(d, NULL);
    uint32_t end = (d->sectors - V4_TAIL_SECTORS) & ~(V5_BLOCK_SECT - 1u);
    memset(dev, 0, sizeof(*dev));
    dev->read = e4_read;
    dev->write = e4_write;
    dev->flush = e4_flush_dev;
    dev->sectors = *base && end > *base ? end - *base : 0;
}

/* an ext4 Banana OS disk: its superblock, named BANANA_OS */
static int e4_detect(const ata_disk_t* d) {
    uint32_t base = boot_partition(d, NULL);
    if (!base || d->sectors <= (uint64_t)base + V4_TAIL_SECTORS + 2048u) return 0;
    uint8_t sec[FSDISK_SECTOR];
    if (disk_read(d, base + 2u, 1, sec) != 0) return 0;          /* byte 1024: the superblock */
    return sec[0x38] == 0x53 && sec[0x39] == 0xEF && !memcmp(sec + 0x78, "BANANA_OS", 9);
}

/* ── the tree's backing: fs.c's changes, written through ── */

static uint32_t to_utc(uint32_t local) { return local ? tz_to_utc(local) : 0; }
static uint32_t to_local(uint32_t utc) { return utc ? tz_to_local(utc) : 0; }

static int op4_read(void* c, uint32_t node, uint8_t* buf, uint32_t size) {
    (void)c;
    return ext4_read(node, 0, buf, size) == (long)size ? 0 : -1;
}
static int op4_write(void* c, uint32_t node, const void* data, uint32_t len) {
    (void)c;
    /* over what is there (no blocks freed and given out again), then cut */
    if (len && ext4_write(node, 0, data, len) != 0) return -1;
    return ext4_truncate(node, len);
}
static int op4_append(void* c, uint32_t node, uint32_t old, const void* data, uint32_t len) {
    (void)c;
    return ext4_write(node, old, data, len);
}
static int op4_create(void* c, uint32_t parent, const char* name, int is_dir, uint32_t* node) {
    (void)c;
    ext4_stat_t st;
    uint32_t uid = 0, gid = 0;
    if (ext4_stat(parent, &st) == 0) { uid = st.uid; gid = st.gid; }  /* (owned like its folder, as fs.c has it) */
    *node = is_dir ? ext4_mkdir(parent, name, 0755, uid, gid) : ext4_create(parent, name, 0644, uid, gid);
    return *node ? 0 : -1;
}
static int op4_remove(void* c, uint32_t node, int is_dir) { (void)c; (void)node; (void)is_dir; return -1; }
static int op4_rename(void* c, uint32_t node, int is_dir, uint32_t np, const char* nn) {
    (void)c; (void)node; (void)is_dir; (void)np; (void)nn;
    return -1;
}
static void op4_unmount(void* c) { (void)c; }
static int op4_remove_at(void* c, uint32_t parent, const char* name, uint32_t node, int is_dir) {
    (void)c; (void)node;
    return is_dir ? ext4_rmdir(parent, name) : ext4_unlink(parent, name);
}
static int op4_rename_at(void* c, uint32_t parent, const char* name, uint32_t node, int is_dir, uint32_t np, const char* nn) {
    (void)c; (void)node; (void)is_dir;
    return ext4_rename(parent, name, np, nn);
}
static int op4_read_at(void* c, uint32_t node, uint32_t off, void* buf, uint32_t len) {
    (void)c;
    return (int)ext4_read(node, off, buf, len);
}
static int op4_write_at(void* c, uint32_t node, uint32_t off, const void* data, uint32_t len) {
    (void)c;
    return ext4_write(node, off, data, len);
}
static int op4_truncate(void* c, uint32_t node, uint32_t size) { (void)c; return ext4_truncate(node, size); }
static int op4_setattr(void* c, uint32_t node, int is_dir, uint32_t perms, uint32_t uid, uint32_t gid,
                       uint32_t atime, uint32_t mtime) {
    (void)c; (void)is_dir;
    return ext4_setattr(node, perms, uid, gid, atime == ~0u ? ~0u : to_utc(atime), mtime == ~0u ? ~0u : to_utc(mtime), ~0u);
}
static int op4_link(void* c, uint32_t parent, const char* name, uint32_t node) {
    (void)c;
    return ext4_link(parent, name, node);
}
static int op4_symlink(void* c, uint32_t parent, const char* name, const char* target, uint32_t* node) {
    (void)c;
    ext4_stat_t st;
    uint32_t uid = 0, gid = 0;
    if (ext4_stat(parent, &st) == 0) { uid = st.uid; gid = st.gid; }
    *node = ext4_symlink(parent, name, target, uid, gid);
    return *node ? 0 : -1;
}

static const fs_mount_ops_t g_e4ops = {
    op4_read, op4_write, op4_append, op4_create, op4_remove, op4_rename, op4_unmount,
    op4_remove_at, op4_rename_at, op4_read_at, op4_write_at, op4_truncate, op4_setattr, op4_symlink,
    op4_link,
};

/* ── loading the tree ── */

typedef struct { char name[256]; uint32_t ino, type; } e4ent_t;
typedef struct { e4ent_t* e; int n, cap; } e4list_t;

static int e4_collect(void* arg, const char* name, uint32_t ino, uint32_t type) {
    e4list_t* l = (e4list_t*)arg;
    if (l->n == l->cap) {
        int cap = l->cap ? l->cap * 2 : 32;
        e4ent_t* e = (e4ent_t*)krealloc(l->e, (size_t)cap * sizeof(e4ent_t));
        if (!e) return 1;
        l->e = e;
        l->cap = cap;
    }
    kstrlcpy(l->e[l->n].name, name, sizeof(l->e[0].name));
    l->e[l->n].ino = ino;
    l->e[l->n].type = type;
    l->n++;
    return 0;
}

static void e4_meta(int is_dir, int idx, const ext4_stat_t* st) {
    fs_set_meta(is_dir, idx, st->mode, st->uid, st->gid, to_local(st->atime));
    fs_set_times(is_dir, idx, to_local(st->mtime), to_local(st->ctime));
}

/* folder idx (inode ino): its entries into the tree, one level - its own
 * folders marked unread, read when something looks in them (fs.c) */
static int e4_load_dir(int idx, uint32_t ino) {
    e4list_t l = { 0, 0, 0 };
    if (ext4_readdir(ino, e4_collect, &l) != 0) { kfree(l.e); return -1; }
    for (int i = 0; i < l.n; i++) {
        e4ent_t* e = &l.e[i];
        ext4_stat_t st;
        if (ext4_stat(e->ino, &st) != 0) continue;
        uint32_t t = st.mode & EXT4_S_IFMT;
        if (!idx && !strcmp(e->name, "swapfile")) { g_swap_ino = e->ino; continue; }   /* (the swap area: not a file to show) */
        if (t == EXT4_S_IFDIR) {
            int d = fs_mount_add_dir(0, idx, e->name, e->ino);
            if (d < 0) continue;
            e4_meta(1, d, &st);
            fs_set_unread(d);
        } else if ((t == EXT4_S_IFREG || t == EXT4_S_IFLNK) && st.size <= FS_MAX_FILE_SIZE) {
            int f = fs_mount_add_file(0, idx, e->name, e->ino, (uint32_t)st.size);
            if (f < 0) continue;
            e4_meta(0, f, &st);
            if (st.links > 1) fs_join_links(f);           /* (another name of a file already in the tables) */
        }
    }
    kfree(l.e);
    return 0;
}

/* the time zone (and whether the clock keeps UTC) from the settings file,
 * before the dates are read - the tree that holds them is not loaded yet */
static void e4_peek_zone(void) {
    uint32_t etc = ext4_lookup(EXT4_ROOT_INO, "etc", 0), f = etc ? ext4_lookup(etc, "settings.conf", 0) : 0;
    if (!f) return;
    char buf[2048];
    long n = ext4_read(f, 0, buf, sizeof(buf) - 1);
    if (n <= 0) return;
    buf[n] = 0;
    for (char* line = buf; line && *line; ) {
        char* nl = strchr(line, '\n');
        if (nl) *nl = 0;
        if (!strncmp(line, "timezone=", 9)) { int z = tz_find(line + 9); if (z >= 0) tz_use(z); }
        line = nl ? nl + 1 : 0;
    }
}

static int e4_attach(void) {
    fs_restore_begin();
    fs_set_system_backend(&g_e4ops, 0, EXT4_ROOT_INO);
    ext4_stat_t st;
    if (ext4_stat(EXT4_ROOT_INO, &st) == 0) e4_meta(1, 0, &st);
    e4_peek_zone();
    /* the root folder only: the others are read when used */
    g_swap_ino = 0;
    fs_set_dir_loader(e4_load_dir);
    e4_load_dir(0, EXT4_ROOT_INO);
    int home = fs_find_dir("/home/banana");
    fs_restore_end(home > 0 ? home : 0);
    g_e4 = 1;
    g_v5 = 0;
    return 1;
}

/* 1 the disk's ext4 was loaded, 0 not ext4, -1 ext4 that would not mount */
static int e4_try_load(const ata_disk_t* d) {
    if (!e4_detect(d)) return 0;
    g_target = *d;
    e4_geometry(d, &g_e4dev, &g_e4_base);
    if (ext4_mount(&g_e4dev) != 0) { klog("fsdisk: the ext4 filesystem could not be mounted\n"); return -1; }
    return e4_attach();
}

/* ── copying the tree onto a new filesystem (install) ── */

#define COPY_CHUNK (256u * 1024u)

static int e4_copy_dir(int idx, uint32_t ino, uint8_t* buf) {
    int rc = 0;
    fs_populate(idx);                                    /* (a folder not read yet: read now) */
    const fs_dir_t* dd = fs_get_dir(idx);
    if (!dd) return -1;
    for (int f = dd->kid_file; f >= 0 && rc == 0; ) {
        fs_file_t* ff = fs_file_info(f);
        int next = ff->sib;
        if (ff->used && !ff->mnt) {
            uint32_t mode = fs_file_mode(f), node;
            if ((mode & FS_S_IFMT) == FS_S_IFLNK) {
                char t[FS_PATH_LEN];
                int n = fs_read(f, 0, t, sizeof(t) - 1);
                t[n > 0 ? n : 0] = 0;
                node = ext4_symlink(ino, ff->name, t, ff->uid, ff->gid);
            } else {
                node = ext4_create(ino, ff->name, mode & 07777u, ff->uid, ff->gid);
                for (uint32_t off = 0; node && off < ff->size && rc == 0; ) {
                    uint32_t n = ff->size - off < COPY_CHUNK ? ff->size - off : COPY_CHUNK;
                    if (fs_read(f, off, buf, n) != (int)n || ext4_write(node, off, buf, n) != 0) rc = FSDISK_ERR_IO;
                    off += n;
                    task_yield();
                }
            }
            if (!node) rc = FSDISK_ERR_FULL;
            else {
                ext4_setattr(node, ~0u, ~0u, ~0u, to_utc(ff->atime), to_utc(ff->mtime), to_utc(ff->ctime));
                fs_set_node(0, f, node);
            }
        }
        f = next;
    }
    for (int d = dd->kid_dir; d >= 0 && rc == 0; ) {
        const fs_dir_t* sd = fs_get_dir(d);
        int next = sd->sib;
        if (!sd->mnt) {
            uint32_t node = ext4_mkdir(ino, sd->name, fs_dir_mode(d) & 07777u, sd->uid, sd->gid);
            if (!node) rc = FSDISK_ERR_FULL;
            else {
                fs_set_node(1, d, node);
                rc = e4_copy_dir(d, node, buf);
                ext4_setattr(node, ~0u, ~0u, ~0u, to_utc(sd->atime), to_utc(sd->mtime), to_utc(sd->ctime));
            }
        }
        d = next;
    }
    return rc;
}

/* an empty ext4 on d, the tree copied onto it, the tree written through to it from then on */
static int e4_format(const ata_disk_t* d) {
    g_target = *d;
    e4_geometry(d, &g_e4dev, &g_e4_base);
    ext4_set_hidden(1);                                    /* (not a Banana OS disk until it is whole) */
    if (ext4_mkfs(&g_e4dev, "BANANA_OS", 0, 0) != 0) return FSDISK_ERR_TOO_SMALL;
    uint8_t* buf = (uint8_t*)kmalloc(COPY_CHUNK);
    if (!buf) return FSDISK_ERR_IO;
    const fs_dir_t* root = fs_get_dir(0);
    if (root) ext4_setattr(EXT4_ROOT_INO, fs_dir_mode(0) & 07777u, root->uid, root->gid, ~0u, ~0u, ~0u);
    fs_set_node(1, 0, EXT4_ROOT_INO);
    int rc = e4_copy_dir(0, EXT4_ROOT_INO, buf);
    kfree(buf);
    if (rc == FSDISK_OK && ext4_activate() != 0) rc = FSDISK_ERR_IO;
    if (rc != FSDISK_OK) return rc;
    fs_set_system_backend(&g_e4ops, 0, EXT4_ROOT_INO);
    fs_set_dir_loader(e4_load_dir);                    /* (folders given back later are read again from it) */
    g_e4 = 1;
    g_v5 = 0;
    return FSDISK_OK;
}

/* ── a layout-5 disk becomes ext4, its files' data where it is ── */

static int e4_migrate(const ata_disk_t* d) {
    ext4_dev_t dev;
    uint32_t base;
    e4_geometry(d, &dev, &base);
    if (base + dev.sectors != g_mark.meta_lba || g_mark.data_lba != base + V5_BLOCK_SECT) return FSDISK_ERR_TOO_SMALL;
    uint32_t fixed = ext4_fixed_blocks(dev.sectors);
    uint32_t fixed_end = base + fixed * V5_BLOCK_SECT;
    /* 1. what lies where ext4 must have its superblock and descriptors moves out */
    if (ext_add(g_mark.data_lba, fixed_end - g_mark.data_lba) != 0) return FSDISK_ERR_IO;
    int moved = 0;
    for (int i = 0; i < g_nrecs; i++) {
        v5_rec_t* r = &g_rec[i];
        if (!r->valid || !r->sectors || r->lba >= fixed_end) continue;
        uint8_t* data = (uint8_t*)kmalloc(r->size ? r->size : 1);
        if (!data) return FSDISK_ERR_IO;
        v5_rec_t old = *r;
        if (v5_read_file(i, data, r->size) != 0) { kfree(data); return FSDISK_ERR_IO; }
        int rc = v5_write_file(d, r, data);
        kfree(data);
        if (rc != FSDISK_OK) return rc;
        ext_remove(old.lba, old.sectors);
        moved++;
    }
    /* the metadata pointing at the new places (its body: outside too) */
    if (moved || (g_body_lba && g_body_lba < fixed_end)) {
        int rc = v5_sync(d);
        if (rc != FSDISK_OK) return rc;
    }
    /* 2. ext4 around the files' extents (and the old metadata body: kept until the end) */
    int nkeep = 0;
    uint32_t (*keep)[2] = (uint32_t (*)[2])kmalloc((size_t)(g_nrecs + 2) * sizeof(*keep));
    if (!keep) return FSDISK_ERR_IO;
    for (int i = 0; i < g_nrecs; i++) {
        if (!g_rec[i].valid || !g_rec[i].sectors) continue;
        keep[nkeep][0] = (g_rec[i].lba - base) / V5_BLOCK_SECT;
        keep[nkeep][1] = g_rec[i].sectors / V5_BLOCK_SECT;
        nkeep++;
    }
    uint32_t body_blk = 0, body_n = 0;
    if (g_body_lba && g_body_sect) {
        body_blk = (g_body_lba - base) / V5_BLOCK_SECT;
        body_n = (g_body_sect + V5_BLOCK_SECT - 1u) / V5_BLOCK_SECT;
        keep[nkeep][0] = body_blk;
        keep[nkeep][1] = body_n;
        nkeep++;
    }
    /* (sorted, as mkfs wants them: by block) */
    for (int gap = nkeep / 2; gap > 0; gap /= 2)
        for (int i = gap; i < nkeep; i++) {
            uint32_t a = keep[i][0], b = keep[i][1];
            int j = i;
            while (j >= gap && keep[j - gap][0] > a) { keep[j][0] = keep[j - gap][0]; keep[j][1] = keep[j - gap][1]; j -= gap; }
            keep[j][0] = a;
            keep[j][1] = b;
        }
    g_target = *d;
    g_e4dev = dev;
    g_e4_base = base;
    ext4_set_hidden(1);
    int rc = ext4_mkfs(&g_e4dev, "BANANA_OS", (const uint32_t (*)[2])keep, nkeep);
    kfree(keep);
    if (rc != 0) return FSDISK_ERR_TOO_SMALL;
    /* 3. the folders and files, the files' inodes pointing at their extents */
    fs_set_node(1, 0, EXT4_ROOT_INO);
    const fs_dir_t* root = fs_get_dir(0);
    if (root) ext4_setattr(EXT4_ROOT_INO, fs_dir_mode(0) & 07777u, root->uid, root->gid, ~0u, ~0u, ~0u);
    rc = FSDISK_OK;
    int ndirs = fs_dir_slots(), nfiles = fs_file_slots();
    /* folders in an order where each parent comes first */
    for (int pass = 0, left = 1; left && pass < 256 && rc == FSDISK_OK; pass++) {
        left = 0;
        for (int i = 1; i < ndirs && rc == FSDISK_OK; i++) {
            const fs_dir_t* dd = fs_get_dir(i);
            if (!dd || dd->mnt || dd->node) continue;
            const fs_dir_t* p = fs_get_dir(dd->parent_dir);
            if (!p || (dd->parent_dir && !p->node)) { left = 1; continue; }
            uint32_t pino = dd->parent_dir ? p->node : EXT4_ROOT_INO;
            uint32_t node = ext4_mkdir(pino, dd->name, fs_dir_mode(i) & 07777u, dd->uid, dd->gid);
            if (!node) { rc = FSDISK_ERR_FULL; break; }
            fs_set_node(1, i, node);
        }
    }
    for (int i = 0; i < nfiles && rc == FSDISK_OK; i++) {
        fs_file_t* f = fs_file_info(i);
        if (!f || !f->used || f->mnt) continue;
        const fs_dir_t* p = fs_get_dir(f->parent_dir);
        uint32_t pino = f->parent_dir ? (p ? p->node : 0) : EXT4_ROOT_INO;
        if (!pino) continue;
        uint32_t mode = fs_file_mode(i), node;
        v5_rec_t* r = i < g_nrecs ? &g_rec[i] : 0;
        if ((mode & FS_S_IFMT) == FS_S_IFLNK) {
            char t[FS_PATH_LEN];
            int n = fs_read(i, 0, t, sizeof(t) - 1);
            t[n > 0 ? n : 0] = 0;
            node = ext4_symlink(pino, f->name, t, f->uid, f->gid);
            if (node && r && r->valid && r->sectors)              /* (its old extent: free) */
                ext4_free_run((r->lba - base) / V5_BLOCK_SECT, r->sectors / V5_BLOCK_SECT);
        } else {
            node = ext4_create(pino, f->name, mode & 07777u, f->uid, f->gid);
            if (node && r && r->valid && r->sectors &&
                ext4_adopt(node, r->size, (r->lba - base) / V5_BLOCK_SECT, r->sectors / V5_BLOCK_SECT) != 0)
                rc = FSDISK_ERR_IO;
        }
        if (!node) { rc = FSDISK_ERR_FULL; break; }
        ext4_setattr(node, ~0u, ~0u, ~0u, to_utc(f->atime), to_utc(f->mtime), to_utc(f->ctime));
        fs_set_node(0, i, node);
    }
    for (int i = 1; i < ndirs && rc == FSDISK_OK; i++) {
        const fs_dir_t* dd = fs_get_dir(i);
        if (dd && !dd->mnt && dd->node)
            ext4_setattr(dd->node, ~0u, ~0u, ~0u, to_utc(dd->atime), to_utc(dd->mtime), to_utc(dd->ctime));
    }
    if (rc != FSDISK_OK) return rc;                        /* (still layout 5: its superblock has no magic) */
    /* 4. it is ext4 from here on; the old metadata body's blocks go back */
    if (ext4_activate() != 0) return FSDISK_ERR_IO;
    if (body_n) ext4_free_run(body_blk, body_n);
    ext4_flush();
    /* the tree as ext4 has it now (nodes, sizes, dates) */
    return e4_attach() == 1 ? FSDISK_OK : FSDISK_ERR_IO;
}

/* ── several disks ──────────────────────────────────────────────────
 * Banana OS used to want exactly one hard disk: with a second one attached
 * it found no installed system. Now every disk is looked at: the first
 * holding a Banana OS install is the system's (its volumes never mounted),
 * the others are ordinary drives, and install asks which disk to use. */

int fsdisk_holds_banana(const ata_disk_t* d) {
    if (d->is_atapi || !d->sectors) return 0;
    v5_mark_t m;
    layout_t l;
    fsdisk_super_t sb[2];
    int have[2];
    return e4_detect(d) || v5_detect(d, &m) || detect(d, &l, sb, have);
}

int fsdisk_find_installed(ata_disk_t* out) {
    ata_disk_t all[DISK_MAX];
    int n = disk_probe(all, DISK_MAX), found = 0;
    for (int i = 0; i < n; i++) {
        if (!fsdisk_holds_banana(&all[i])) continue;
        if (!found && out) *out = all[i];
        found++;
    }
    if (found > 1) klog("fsdisk: %d disks hold Banana OS - using the first one\n", found);
    return found;
}

int fsdisk_targets(ata_disk_t* out, int max) {
    ata_disk_t all[DISK_MAX];
    int n = disk_probe(all, DISK_MAX), k = 0;
    for (int i = 0; i < n && k < max; i++)
        if (!all[i].is_atapi && all[i].sectors) out[k++] = all[i];
    return k;
}

int fsdisk_install(void) {
    ata_disk_t target;
    int tr = fsdisk_find_target(&target);
    if (tr == 0) return FSDISK_ERR_NO_TARGET;
    if (tr < 0)  return FSDISK_ERR_AMBIGUOUS;
    return fsdisk_install_to(&target);
}

static int htab_save(const ata_disk_t* d, uint32_t iso_bytes);
static int htab_drop(const ata_disk_t* d);

int fsdisk_install_to(const ata_disk_t* tp) {
    ata_disk_t target = *tp;
    memset(&g_tm, 0, sizeof(g_tm));
    uint32_t t_start = timer_ms();
    if (target.is_atapi || !target.sectors) return FSDISK_ERR_NO_TARGET;
    /* what was mounted from it goes, and it is never mounted again */
    blockdev_set_system(disk_blockdev(&target));

    img_src_t source;
    if (!find_source(&source)) return FSDISK_ERR_NO_SOURCE;

    uint32_t iso_bytes;
    if (src_iso_size(&source, &iso_bytes) != 0) return FSDISK_ERR_NO_SOURCE;
    uint32_t base = boot_install_base(iso_bytes);
    if (!base) return FSDISK_ERR_ISO_TOO_BIG;
    if (target.sectors <= (uint64_t)base + V4_TAIL_SECTORS + V4_MIN_SLOT_SECTORS)
        return FSDISK_ERR_TOO_SMALL;
    uint8_t partition[16];
    memset(partition, 0, sizeof(partition));
    partition[1] = partition[5] = 0xfe;
    partition[2] = partition[3] = partition[6] = partition[7] = 0xff;
    partition[4] = 0x83;
    boot_put32(partition + 8, base);
    uint32_t end = (target.sectors - V4_TAIL_SECTORS) & ~(V5_BLOCK_SECT - 1u);
    boot_put32(partition + 12, end - base);

    layout_t l;
    if (!layout_v4(&target, &l)) return FSDISK_ERR_TOO_SMALL;
    if (bytes_to_sectors(fs_snapshot_size()) > end - base) return FSDISK_ERR_FULL;

    if (htab_drop(&target) != 0) return FSDISK_ERR_IO;
    if (copy_boot_image(&source, &target, iso_bytes, partition) != 0) return FSDISK_ERR_IO;
    if (verify_and_fix(&source, &target, iso_bytes, partition) != FSDISK_OK) return FSDISK_ERR_IO;
    /* old headers: a layout-3 install's (past the new image), a layout-4 one's */
    if (iso_bytes <= V3_BASE_LBA * FSDISK_SECTOR) wipe_header(&target, V3_BASE_LBA);
    if (iso_bytes <= V4_BASE_LBA * FSDISK_SECTOR) wipe_header(&target, V4_BASE_LBA);
    if (l.nslots == 2 && slot_lba_of(&l, 1) >= bytes_to_sectors(iso_bytes))
        wipe_header(&target, slot_lba_of(&l, 1));
    uint32_t t_files = timer_ms();
    int rc = e4_format(&target);
    g_tm.files_ms = timer_ms() - t_files;
    if (rc == FSDISK_OK) htab_save(&target, iso_bytes);     /* (the next update: only what changed) */
    g_tm.total_ms = timer_ms() - t_start;
    if (rc != FSDISK_OK) return rc;
    g_prog_done = g_prog_total;

    g_layout = l;
    g_target = target;
    g_have_target = 1;
    g_fi_state = -1;
    return FSDISK_OK;
}

/* a hypervisor backing the disk up (the guest agent's fsfreeze): everything
 * is written now, then nothing until thawed - the disk stays as the backup
 * reads it */
int fsdisk_freeze(int on) {
    if (!on) { g_frozen = 0; return FSDISK_OK; }
    if (g_frozen) return FSDISK_OK;
    int rc = g_have_target ? fsdisk_sync() : FSDISK_OK;
    g_frozen = 1;
    g_frozen_at = timer_ms();
    return rc == FSDISK_ERR_NO_TARGET ? FSDISK_OK : rc;
}

int fsdisk_frozen(void) { return g_frozen; }

static int sync_disk(int durable) {
    if (!g_have_target) return FSDISK_ERR_NO_TARGET;
    if (g_frozen) return durable ? FSDISK_ERR_FROZEN : FSDISK_OK;
    while (g_busy) task_sleep_ms(10);        /* the autosave task and `sync` take turns */
    /* A backup may have frozen the disk while we waited for another flush. */
    if (g_frozen) return durable ? FSDISK_ERR_FROZEN : FSDISK_OK;
    g_busy = 1;
    int rc = g_e4 ? (ext4_flush() == 0 ? FSDISK_OK : FSDISK_ERR_IO)
           : g_v5 ? v5_sync(&g_target) : write_snapshot_to(&g_target);
    g_busy = 0;
    g_last_err = rc;
    return rc;
}

int fsdisk_sync(void) { return sync_disk(0); }
int fsdisk_sync_durable(void) { return sync_disk(1); }

/* a layout-5 disk just loaded: made ext4 (it stays layout 5 if that fails) */
static void e4_move_over(const ata_disk_t* d) {
    uint32_t t = timer_ms();
    int rc = e4_migrate(d);
    if (rc == FSDISK_OK) klog("fsdisk: the disk is ext4 now - its files where they were (%u ms)\n", timer_ms() - t);
    else klog("fsdisk: the disk stays on layout 5 (%d)\n", rc);
}

int fsdisk_try_load(void) {
    ata_disk_t target;
    if (!fsdisk_find_installed(&target)) return 0;     /* (other disks: ordinary drives) */
    blockdev_set_system(disk_blockdev(&target));

    int r6 = e4_try_load(&target);
    if (r6 < 0) return -1;
    if (r6 == 1) {
        g_have_target = 1;
        return 1;
    }

    int r5 = v5_try_load(&target);
    if (r5 < 0) return -1;
    if (r5 == 1) {
        g_target = target;
        g_have_target = 1;
        g_synced_gen = fs_generation();
        e4_move_over(&target);
        return 1;
    }

    layout_t l;
    fsdisk_super_t sb[2];
    int have[2];
    if (!detect(&target, &l, sb, have)) return 0;

    /* the newest first; if it is damaged (a cut during its write), the other */
    for (int skip = 0; skip < 2; skip++) {
        uint8_t* buf = 0;
        int s = -1;
        int k = read_newest(&target, &l, sb, have, &buf, &s, skip);
        if (k < 0) break;
        int ok = fs_snapshot_load(buf + hdr_size(l.version), sb[s].payload_bytes, sb[s].version) == 0;
        kfree(buf);
        if (!ok) { if (k == 1) break; continue; }
        g_layout = l;
        g_target = target;
        g_have_target = 1;
        g_cur_slot = s;
        g_seq = sb[s].seq;
        g_synced_gen = fs_generation();
        /* move the disk to layout 5; the copy just loaded stays as it is until that is done */
        int rc = v5_format(&target, slot_lba_of(&l, s), bytes_to_sectors(hdr_size(l.version) + sb[s].payload_bytes));
        if (rc == FSDISK_OK) {
            klog("fsdisk: the disk was moved to layout 5 (only changed files are written from now on)\n");
            e4_move_over(&target);
            return 1;
        }
        klog("fsdisk: the disk stays on layout %u (%d)\n", l.version, rc);
        g_cur_slot = s;
        g_seq = sb[s].seq;
        /* upgrade an older format right away, and repair a damaged newest slot */
        if (sb[s].version != FSDISK_VERSION || k == 1) fsdisk_sync();
        return 1;
    }
    return -1;   /* a Banana OS disk, but no copy could be read */
}

int fsdisk_is_installed(void) {
    return g_have_target;
}

/* ── autosave: a change is on disk a few seconds after it was made ── */
#define AUTOSAVE_QUIET_MS 2000u

static void autosave_task(void) {
    task_set_background();
    uint32_t seen = 0, changed_at = 0, retry_at = 0;
    int warned = 0;
    for (;;) {
        task_sleep_ms(500);
        fs_trim();                              /* RAM low: cached file data goes */
        if (g_frozen && timer_ms() - g_frozen_at > 600000u) {     /* never thawed: not forever */
            klog("fsdisk: frozen for 10 minutes - thawed\n");
            g_frozen = 0;
        }
        if (!g_have_target || g_frozen) continue;
        if (g_e4) {
            /* ext4: the changed blocks to the disk within a second, the cache
             * smaller when RAM runs low (what the files hold is on the disk) */
            uint32_t total = kheap_total_bytes();
            int low = total - kheap_used_bytes() < total / 8;
            if (low) ext4_cache_trim(ext4_cache_bytes() / 2);
            /* many entries in the tables, or RAM low: folders nobody looked in for
             * 10 minutes give theirs back (read again from the disk when used) */
            static uint32_t last_unload;
            if ((low || fs_entries() > 50000u) && timer_ms() - last_unload > 10000u) {
                last_unload = timer_ms();
                uint32_t n = fs_unload_idle(low ? 60000u : 600000u);
                if (n) klog("fsdisk: %u entries of folders not used lately given back\n", n);
            }
            if (!ext4_dirty_blocks()) { warned = 0; continue; }
            if (ext4_flush() != 0 && !warned) { klog("fsdisk: could not write to the disk\n"); warned = 1; }
            continue;
        }
        uint32_t gen = fs_generation();
        if (gen == g_synced_gen) { warned = 0; continue; }
        /* RAM running low: save now, even while things change - a saved file's
         * RAM can be given back, an unsaved one's cannot (a long download) */
        uint32_t total = kheap_total_bytes();
        int low = total - kheap_used_bytes() < total / 8;
        if (!low) {
            if (gen != seen) { seen = gen; changed_at = timer_ms(); continue; }   /* still changing */
            if (timer_ms() - changed_at < AUTOSAVE_QUIET_MS) continue;
        }
        if ((int32_t)(timer_ms() - retry_at) < 0) continue;
        int rc = fsdisk_sync();
        if (rc != FSDISK_OK) {
            if (!warned) klog("autosave: could not write the filesystem to disk (%d)\n", rc);
            warned = 1;
            retry_at = timer_ms() + 30000u;     /* try again in a while */
        }
    }
}

void fsdisk_start_autosave(void) {
    static int started = 0;
    if (started) return;
    started = 1;
    task_create("autosave", autosave_task);
}

int fsdisk_pending(void) {
    if (g_e4) return ext4_dirty_blocks() != 0;
    return g_have_target && fs_generation() != g_synced_gen;
}

int fsdisk_last_error(void) { return g_last_err; }

void fsdisk_space(uint64_t* used_bytes, uint64_t* capacity_bytes) {
    if (g_e4) {
        uint64_t b, f;
        uint32_t i, fi;
        ext4_statfs(&b, &f, &i, &fi);
        uint64_t swap = (uint64_t)g_swap_sect * FSDISK_SECTOR;          /* (the swap file: not room for files) */
        *capacity_bytes = b * 4096u - swap;
        *used_bytes = (b - f) * 4096u > swap ? (b - f) * 4096u - swap : 0;
        return;
    }
    if (g_v5) {
        uint64_t used = (uint64_t)g_body_sect * FSDISK_SECTOR;
        for (int i = 0; i < g_nrecs; i++) if (g_rec[i].valid) used += (uint64_t)g_rec[i].sectors * FSDISK_SECTOR;
        *used_bytes = used;
        *capacity_bytes = (uint64_t)(g_mark.data_end - g_mark.data_lba - g_swap_sect) * FSDISK_SECTOR;   /* (the swap area: not for files) */
        return;
    }
    *used_bytes = fs_snapshot_size();
    *capacity_bytes = g_have_target ? slot_capacity(&g_layout) : 0;
}

void fsdisk_describe(char* out, int cap) {
    out[0] = 0;
    if (!g_have_target) return;
    char where[48];
    disk_describe(&g_target, where, sizeof(where));
    ksnprintf(out, (size_t)cap, "%s (%s)", where, g_target.model[0] ? g_target.model : "disk");
}

/* ── updating an install from the CD: the system only, files kept ── */

static ata_disk_t g_fi_disk;

int fsdisk_find_install(ata_disk_t* out) {
    if (g_fi_state < 0) {
        g_fi_state = 0;
        ata_disk_t d;
        if (fsdisk_find_installed(&d)) {
            g_fi_disk = d;
            g_fi_state = 1;
        }
    }
    if (g_fi_state && out) *out = g_fi_disk;
    return g_fi_state;
}

/* 1: the disk is on the current layout */
int fsdisk_install_layout(void) {
    ata_disk_t d;
    layout_t l;
    fsdisk_super_t sb[2];
    int have[2];
    v5_mark_t m;
    if (!fsdisk_find_installed(&d)) return 0;
    if (e4_detect(&d)) return 6;
    if (v5_detect(&d, &m)) return 5;
    if (!detect(&d, &l, sb, have)) return 0;
    return (int)l.version;
}

typedef struct __attribute__((packed)) {
    uint32_t magic, version, n, iso_bytes;
    uint32_t pvd_check;                      /* CRC-32C of the image's volume descriptor (CD block 16) on the disk */
    uint32_t tab_check;                      /* CRC-32C of the checksums */
    uint32_t check;                          /* of the fields above */
} htab_hdr_t;

/* the volume descriptor as the disk has it: its CRC-32C, or 0 */
static uint32_t pvd_check_of(const ata_disk_t* d) {
    uint8_t* b = (uint8_t*)kmalloc(2048);
    if (!b) return 0;
    uint32_t c = disk_read(d, 16u * 4u, 4, b) == 0 ? crc32c(0, b, 2048) | 1u : 0;
    kfree(b);
    return c;
}

/* g_old: the disk's table, if it is the one of the image the disk holds */
static void htab_load(const ata_disk_t* d) {
    memset(&g_old, 0, sizeof(g_old));
    uint32_t base = boot_partition(d, NULL);
    if (!base) return;
    uint32_t sectors = boot_table_sectors(base);
    uint8_t* b = (uint8_t*)kmalloc(sectors * FSDISK_SECTOR);
    if (!b) return;
    htab_hdr_t h;
    if (disk_read(d, boot_table_lba(base), sectors, b) == 0) {
        memcpy(&h, b, sizeof(h));
        if (h.magic == HTAB_MAGIC && h.version == 1 && h.n <= (sectors - 1u) * FSDISK_SECTOR / 4u &&
            h.check == crc32c(0, &h, 24u) &&
            h.tab_check == crc32c(0, b + FSDISK_SECTOR, h.n * 4u) && h.pvd_check == pvd_check_of(d)) {
            memcpy(g_old.tab, b + FSDISK_SECTOR, h.n * 4u);
            g_old.n = h.n;
            g_old.valid = 1;
        }
    }
    kfree(b);
}

/* g_new onto the disk, for the image just written (after it is flushed) */
static int htab_save(const ata_disk_t* d, uint32_t iso_bytes) {
    uint32_t base = boot_partition(d, NULL);
    if (!base) return -1;
    uint32_t sectors = boot_table_sectors(base);
    if (g_new.n > (sectors - 1u) * FSDISK_SECTOR / 4u) return -1;
    uint8_t* b = (uint8_t*)kzalloc(sectors * FSDISK_SECTOR);
    if (!b) return -1;
    htab_hdr_t h = { HTAB_MAGIC, 1, g_new.n, iso_bytes, pvd_check_of(d), 0, 0 };
    memcpy(b + FSDISK_SECTOR, g_new.tab, g_new.n * 4u);
    h.tab_check = crc32c(0, b + FSDISK_SECTOR, g_new.n * 4u);
    h.check = crc32c(0, &h, 24u);
    memcpy(b, &h, sizeof(h));
    int rc = h.pvd_check && disk_write(d, boot_table_lba(base), sectors, b) == 0 && disk_flush(d) == 0 ? 0 : -1;
    kfree(b);
    return rc;
}

/* the table goes before the image changes: half an update leaves none (the next one writes all) */
static int htab_drop(const ata_disk_t* d) {
    uint32_t base = boot_partition(d, NULL);
    if (!base) return -1;
    uint32_t lba = boot_table_lba(base);
    if (lba >= d->sectors) return 0;
    uint8_t z[FSDISK_SECTOR];
    memset(z, 0, sizeof(z));
    return disk_write(d, lba, 1, z) == 0 && disk_flush(d) == 0 ? 0 : -1;
}

/* the last comparison against the table (fsdisk_compare_boot), which the
 * update right after it uses: only the CD chunks that changed are read again */
static htab_t     g_cmp;
static int        g_cmp_ok;
static ata_disk_t g_cmp_disk;
static uint32_t   g_cmp_iso, g_cmp_src;

/* the CD (or stick) image's volume descriptor: its CRC-32C (it names the build), or 0 */
static uint32_t src_pvd_check(const img_src_t* s) {
    uint8_t* b = (uint8_t*)kmalloc(2048);
    if (!b) return 0;
    uint32_t c = src_read(s, 16, 1, b) == 0 ? crc32c(0, b, 2048) | 1u : 0;
    kfree(b);
    return c;
}

static int need_changed(uint32_t blk, uint32_t nblk) {
    for (uint32_t i = blk / HASH_BLOCKS; i < (blk + nblk + HASH_BLOCKS - 1u) / HASH_BLOCKS; i++)
        if (!(i < g_old.n && i < g_cmp.n && g_old.tab[i] == g_cmp.tab[i])) return 1;
    return 0;
}

typedef struct { const ata_disk_t* disk; uint8_t* buf; int same; } cmp_ctx_t;

static int sink_compare(uint32_t blk, uint32_t nblk, const uint8_t* data, void* vctx) {
    cmp_ctx_t* c = (cmp_ctx_t*)vctx;
    if (!c->same) return 0;
    if (disk_read(c->disk, blk * 4u, nblk * 4u, c->buf) != 0) return -1;
    if (memcmp(c->buf, data, nblk * 2048u) != 0) c->same = 0;
    return 0;
}

int fsdisk_compare_boot(void) {
    memset(&g_tm, 0, sizeof(g_tm));
    g_cmp_ok = 0;
    uint32_t t_start = timer_ms();
    ata_disk_t dst;
    img_src_t src;
    uint32_t iso_bytes;
    if (!fsdisk_find_install(&dst)) return FSDISK_ERR_NO_INSTALL;
    if (!find_source(&src) || src_iso_size(&src, &iso_bytes) != 0) return FSDISK_ERR_NO_SOURCE;
    uint8_t partition[16];
    uint32_t base = boot_partition(&dst, partition);
    if (!base) return FSDISK_ERR_IO;
    if (!boot_image_fits(base, iso_bytes))
        return FSDISK_ERR_ISO_TOO_BIG;
    int lay = fsdisk_install_layout();
    if (lay != (int)LAYOUT_V4 && lay != 5 && lay != 6) return 0;            /* an old layout: always worth updating */
    htab_load(&dst);
    memset(&g_new, 0, sizeof(g_new));
    int rc, same;
    if (g_old.valid) {
        /* the CD against the table: nothing of the disk is read */
        int differ = 0;
        rc = stream_image(&src, iso_bytes, sink_hash_compare, &differ, NULL, partition);
        same = differ == 0 && g_new.n == g_old.n;
        g_cmp = g_new;
        g_cmp_ok = rc == 0;
        g_cmp_disk = dst;
        g_cmp_iso = iso_bytes;
        g_cmp_src = src_pvd_check(&src);
    } else {
        /* no table (installed by an older Banana OS): the disk is read back */
        cmp_ctx_t c = { &dst, (uint8_t*)kmalloc(READ_BLOCKS * 2048u), 1 };
        if (!c.buf) return FSDISK_ERR_IO;
        rc = stream_image(&src, iso_bytes, sink_compare, &c, NULL, partition);
        kfree(c.buf);
        same = c.same;
    }
    g_tm.total_ms = timer_ms() - t_start;
    if (rc != 0) return FSDISK_ERR_IO;
    return same;
}

/* Moves a layout-3 install's files to layout 4: the newest good copy is
 * written to the new first slot (and checked) before the new, bigger boot
 * image goes over the old place. */
static int migrate_to_v4(const ata_disk_t* d) {
    layout_t old, nl;
    fsdisk_super_t sb[2];
    int have[2];
    v5_mark_t m;
    if (e4_detect(d) || v5_detect(d, &m)) return FSDISK_OK;     /* layouts 5 and 6: the files stay where they are */
    if (!detect(d, &old, sb, have) || old.version == LAYOUT_V4) return FSDISK_OK;
    if (!layout_v4(d, &nl)) return FSDISK_ERR_TOO_SMALL;
    uint8_t* buf = 0;
    int s = -1;
    if (read_newest(d, &old, sb, have, &buf, &s, 0) < 0) return FSDISK_ERR_IO;
    uint32_t payload = sb[s].payload_bytes;
    if (payload > slot_capacity(&nl)) { kfree(buf); return FSDISK_ERR_FULL; }
    uint32_t nsec = bytes_to_sectors(hdr_size(LAYOUT_V4) + payload);
    uint8_t* nbuf = (uint8_t*)kzalloc(nsec * FSDISK_SECTOR);
    if (!nbuf) { kfree(buf); return FSDISK_ERR_IO; }
    memcpy(nbuf + hdr_size(LAYOUT_V4), buf + hdr_size(old.version), payload);
    kfree(buf);
    buf = nbuf;
    if (nl.nslots == 2) wipe_header(d, slot_lba_of(&nl, 1));
    int rc = write_payload(d, &nl, 0, sb[s].seq + 1, buf, payload, sb[s].version);
    if (rc == FSDISK_OK) {
        /* read it back before anything of the old layout is given up */
        fsdisk_super_t chk;
        if (!read_super_at(d, slot_lba_of(&nl, 0), LAYOUT_V4, &chk) ||
            disk_read(d, slot_lba_of(&nl, 0), nsec, buf) != 0 ||
            checksum_of(buf + hdr_size(LAYOUT_V4), payload) != chk.checksum)
            rc = FSDISK_ERR_IO;
    }
    kfree(buf);
    return rc;
}

int fsdisk_update(void) {
    ata_disk_t d;
    if (!fsdisk_find_install(&d)) return FSDISK_ERR_NO_INSTALL;
    return fsdisk_update_on(&d);
}

int fsdisk_update_on(const ata_disk_t* dp) {
    memset(&g_tm, 0, sizeof(g_tm));
    uint32_t t_start = timer_ms();
    ata_disk_t dst;
    img_src_t src;
    uint32_t iso_bytes;
    dst = *dp;
    if (!fsdisk_holds_banana(&dst)) return FSDISK_ERR_NO_INSTALL;
    if (!find_source(&src) || src_iso_size(&src, &iso_bytes) != 0) return FSDISK_ERR_NO_SOURCE;
    uint8_t partition[16];
    uint32_t base = boot_partition(&dst, partition);
    if (!base) return FSDISK_ERR_IO;
    if (!boot_image_fits(base, iso_bytes))
        return FSDISK_ERR_ISO_TOO_BIG;
    int rc = migrate_to_v4(&dst);
    if (rc != FSDISK_OK) return rc;
    /* the boot area only: the files after it stay as they are. With the
     * table, only the 64 KB pieces that changed are written */
    htab_load(&dst);
    memset(&g_new, 0, sizeof(g_new));
    /* just compared (same disk, same CD): only the chunks that differ are read */
    chunk_need_t need = NULL;
    if (g_cmp_ok && g_old.valid && disk_same(&g_cmp_disk, &dst) && g_cmp_iso == iso_bytes && g_cmp_src && g_cmp_src == src_pvd_check(&src)) {
        g_new = g_cmp;
        need = need_changed;
    }
    g_cmp_ok = 0;
    if (htab_drop(&dst) != 0) return FSDISK_ERR_IO;
    if (stream_image(&src, iso_bytes, sink_delta, (void*)&dst, need, partition) != 0) return FSDISK_ERR_IO;
    if (iso_bytes <= V3_BASE_LBA * FSDISK_SECTOR) wipe_header(&dst, V3_BASE_LBA);
    /* what the disk really holds now, not what the table says it should */
    rc = verify_and_fix(&src, &dst, iso_bytes, partition);
    if (rc != FSDISK_OK) return rc;           /* (no table saved: the next update writes everything) */
    htab_save(&dst, iso_bytes);              /* (without it the next update writes everything: no harm) */
    g_fi_state = -1;
    g_prog_done = g_prog_total;
    g_tm.total_ms = timer_ms() - t_start;
    return FSDISK_OK;
}

/* ── the boot loader's screen size, on the installed disk ── */
int fsdisk_set_boot_video(const char* value) {
    if (!fsdisk_is_installed() || !g_have_target || strlen(value) > VIDEO_FIELD) return -1;
    const uint32_t CH = 256;                          /* sectors per read (128 KB) */
    uint8_t* buf = (uint8_t*)kmalloc((CH + 1) * FSDISK_SECTOR);
    if (!buf) return -1;
    dlock();
    /* the boot image's size: its ISO9660 volume descriptor */
    uint32_t sectors = 0;
    if (disk_read(&g_target, 16 * 4, 4, buf) == 0 && buf[0] == 1 && memcmp(buf + 1, "CD001", 5) == 0)
        sectors = ((uint32_t)buf[80] | (uint32_t)buf[81] << 8 | (uint32_t)buf[82] << 16 | (uint32_t)buf[83] << 24) * 4u;
    uint32_t base = boot_partition(&g_target, NULL);
    if (!base) { kfree(buf); dunlock(); return -1; }
    if (sectors > base) sectors = base;
    int hits = 0, rc = 0;
    for (uint32_t s = 0; s < sectors && rc == 0; s += CH) {
        /* one sector more: a line across the chunk's end is found whole */
        uint32_t n = sectors - s < CH ? sectors - s : CH;
        uint32_t rd = s + n < sectors ? n + 1 : n;
        if (disk_read(&g_target, s, rd, buf) != 0) { rc = -1; break; }
        int h = patch_video(buf, n * FSDISK_SECTOR + (rd > n ? 29u : 0u), value);
        if (h) {
            if (disk_write(&g_target, s, rd, buf) != 0) rc = -1;
            hits += h;
        }
        if ((s / CH) % 16 == 0) task_yield();
    }
    /* the image is not the one its checksum table describes any more */
    if (hits && (htab_drop(&g_target) != 0 || disk_flush(&g_target) != 0)) rc = -1;
    dunlock();
    kfree(buf);
    if (rc == 0 && hits) {
        cfg_set(CFG_SETTINGS, "boot_resolution", value, "# Banana OS settings (the Settings app writes this)\n");
        cfg_persist();
    }
    return rc == 0 ? hits : -1;
}

/* ── fsck: the installed disk's metadata, extents and file data ──────── */

/* reads a file's extent back in 64 KiB steps: 0 its check matches, 1 not, -1 unreadable */
static int verify_rec(const v5_rec_t* r, uint8_t* buf) {
    uint32_t done = 0, crc = 0, sum = 0;
    while (done < r->size) {
        uint32_t n = r->size - done;
        if (n > RA_BYTES) n = RA_BYTES;
        dlock();
        int rc = disk_read(&g_target, r->lba + done / FSDISK_SECTOR, bytes_to_sectors(n), buf);
        dunlock();
        if (rc != 0) return -1;
        if (r->crc) crc = crc32c(crc, buf, n);
        else for (uint32_t k = 0; k < n; k++) sum = sum * 131u + buf[k];
        done += n;
        task_yield();
    }
    return (r->crc ? crc : sum) == r->check ? 0 : 1;
}

int fsdisk_check(int full, void (*out)(const char* line)) {
    char line[160];
    int problems = 0;
    if (!g_have_target) { out("disk: none installed - the files live in RAM only"); return 0; }
    if (g_e4) return ext4_check(out);
    if (!g_v5) {
        ksnprintf(line, sizeof(line), "disk: layout %u (older) - its copies were checked when loaded", g_layout.version);
        out(line);
        return 0;
    }
    if (fsdisk_pending()) fsdisk_sync();              /* check what is on the disk now */
    while (g_busy) task_sleep_ms(10);
    g_busy = 1;
    /* the two metadata slots: the one in use must be whole */
    for (int s = 0; s < 2; s++) {
        meta_info_t h;
        uint8_t* body = NULL;
        int ok = v5_read_meta(&g_target, &g_mark, s, &h, &body);
        int good = ok && (h.version == 7 ? v7_body_ok(body, h.body_bytes, &g_mark) : v5_body_ok(body, h.body_bytes, &g_mark));
        if (good)
            ksnprintf(line, sizeof(line), "metadata slot %d: version %u, save #%u, %u bytes, checksums good%s",
                      s, h.version, h.seq, h.body_bytes, s == g_cur_slot ? " (in use)" : "");
        else
            ksnprintf(line, sizeof(line), "metadata slot %d: %s", s, s == g_cur_slot ? "DAMAGED (the one in use)" : "empty or older (unused)");
        out(line);
        if (!good && s == g_cur_slot) problems++;
        kfree(body);
    }
    /* the extents: inside the files' area, none over another */
    uint32_t prev_end = g_mark.data_lba;
    for (int i = 0; i < g_next; i++) {
        uint32_t a = g_ext[i].lba, e = a + g_ext[i].sectors;
        if (a < g_mark.data_lba || e > g_mark.data_end) {
            ksnprintf(line, sizeof(line), "extent at sector %u (%u sectors) lies outside the files' area", a, g_ext[i].sectors);
            out(line);
            problems++;
        } else if (a < prev_end && i) {
            ksnprintf(line, sizeof(line), "extent at sector %u overlaps the one before it", a);
            out(line);
            problems++;
        }
        if (e > prev_end) prev_end = e;
    }
    /* every saved record belongs to a live file of that size */
    int nrec = 0;
    uint64_t bytes = 0;
    for (int i = 0; i < g_nrecs; i++) {
        if (!g_rec[i].valid) continue;
        nrec++;
        bytes += g_rec[i].size;
        fs_file_t* f = fs_file_info(i);
        if (!f || !f->used || f->mnt) {
            ksnprintf(line, sizeof(line), "record #%d has no file (space not given back)", i);
            out(line);
            problems++;
        } else if (!f->loaded && f->size != g_rec[i].size) {
            ksnprintf(line, sizeof(line), "'%s': %u bytes, its saved copy %u", f->name, f->size, g_rec[i].size);
            out(line);
            problems++;
        }
    }
    ksnprintf(line, sizeof(line), "extents: %d in use, %u files saved (%u MiB)", g_next, (uint32_t)nrec, (uint32_t)(bytes >> 20));
    out(line);
    /* -f: every file's data read back against its checksum */
    if (full) {
        uint8_t* buf = (uint8_t*)kmalloc(RA_BYTES);
        int bad = 0, done = 0;
        if (buf) {
            for (int i = 0; i < g_nrecs; i++) {
                if (!g_rec[i].valid || !g_rec[i].size) continue;
                v5_rec_t r = g_rec[i];
                int v = verify_rec(&r, buf);
                done++;
                if (v != 0) {
                    fs_file_t* f = fs_file_info(i);
                    ksnprintf(line, sizeof(line), "'%s' (#%d): %s", f && f->used ? f->name : "?", i,
                              v < 0 ? "cannot be read" : "data does not match its checksum");
                    out(line);
                    bad++;
                }
            }
            kfree(buf);
        }
        ksnprintf(line, sizeof(line), "data: %d files read back, %d bad", done, bad);
        out(line);
        problems += bad;
    }
    g_busy = 0;
    return problems;
}

/* ── the swap area (kernel/zswap.c) ───────────────────────────────────
 * A run of the files' area kept for swapped pages: at the top of the free
 * space, taken at each boot and never saved (what it held means nothing
 * after a restart) - an older Banana OS sees free space there. */

int fsdisk_swap_area(uint32_t want, uint32_t min, uint32_t* lba, uint32_t* sectors) {
    if (g_e4 && g_have_target && !g_swap_sect) {
        /* ext4: /swapfile, as Linux has one - blocks given out (not written,
         * they read as zeros) in one run; kept from boot to boot, never shown
         * in the tree (e4_load_dir), so nothing deletes it while it is in use */
        uint64_t b, f;
        uint32_t i, fi;
        ext4_statfs(&b, &f, &i, &fi);
        uint32_t room = (uint32_t)(f * 8u / 2u);                 /* half the free space at most */
        if (want > room) want = room;
        want &= ~(V5_BLOCK_SECT - 1u);
        uint32_t s0 = 0, sn = 0;
        if (g_swap_ino && (ext4_first_run(g_swap_ino, &s0, &sn) != 0 || sn < min)) {
            ext4_unlink(EXT4_ROOT_INO, "swapfile");             /* (too small, or in pieces: made again) */
            g_swap_ino = 0;
            sn = 0;
        }
        if (!g_swap_ino) {
            if (want < min) return -1;
            g_swap_ino = ext4_create(EXT4_ROOT_INO, "swapfile", 0600, 0, 0);
            if (!g_swap_ino || ext4_fallocate(g_swap_ino, (uint64_t)want * FSDISK_SECTOR) != 0 ||
                ext4_first_run(g_swap_ino, &s0, &sn) != 0 || sn < min) {
                if (g_swap_ino) ext4_unlink(EXT4_ROOT_INO, "swapfile");
                g_swap_ino = 0;
                return -1;
            }
            ext4_flush();
        }
        if (sn > want && want >= min) sn = want;
        g_swap_lba = g_e4_base + s0;
        g_swap_sect = sn;
        *lba = g_swap_lba;
        *sectors = sn;
        klog("fsdisk: swap file of %u MB at sector %u\n", sn / 2048u, g_swap_lba);
        return 0;
    }
    if (!g_v5 || !g_have_target || g_swap_sect) return -1;
    want &= ~(V5_BLOCK_SECT - 1u);
    min &= ~(V5_BLOCK_SECT - 1u);
    /* the largest free run, its top part (files grow from the bottom) */
    uint32_t best_lo = 0, best_len = 0, cur = g_mark.data_lba;
    for (int i = 0; i <= g_next; i++) {
        uint32_t next = i < g_next ? g_ext[i].lba : g_mark.data_end;
        if (next > g_mark.data_end) next = g_mark.data_end;
        if (next > cur && next - cur > best_len) { best_lo = cur; best_len = next - cur; }
        if (i < g_next) {
            uint32_t end = (g_ext[i].lba + g_ext[i].sectors + V5_BLOCK_SECT - 1u) & ~(V5_BLOCK_SECT - 1u);
            if (end > cur) cur = end;
        }
    }
    /* at most half of it: the files keep room to grow */
    uint32_t n = best_len / 2u;
    if (n > want) n = want;
    n &= ~(V5_BLOCK_SECT - 1u);
    if (n < min || n == 0) return -1;
    uint32_t at = (best_lo + best_len - n) & ~(V5_BLOCK_SECT - 1u);
    if (ext_add(at, n) != 0) return -1;
    g_swap_lba = at;
    g_swap_sect = n;
    *lba = at;
    *sectors = n;
    klog("fsdisk: swap area of %u MB at sector %u\n", n / 2048u, at);
    return 0;
}

int fsdisk_swap_io(int write, uint32_t lba, uint32_t n, void* buf) {
    if (!g_swap_sect || lba < g_swap_lba || lba + n > g_swap_lba + g_swap_sect) return -1;
    dlock();
    int rc = write ? disk_write(&g_target, lba, n, buf) : disk_read(&g_target, lba, n, buf);
    dunlock();
    return rc;
}

const char* fsdisk_fs_type(void) {
    return g_e4 ? "ext4" : g_have_target ? "bananafs" : "ramfs";
}
