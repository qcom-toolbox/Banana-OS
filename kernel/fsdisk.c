#include "fsdisk.h"
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

/* the CD/DVD drive holding the Banana OS boot image */
static int find_atapi_source(ata_disk_t* out) {
    ata_disk_t all[DISK_MAX];
    int n = disk_probe(all, DISK_MAX);
    uint32_t bytes;
    for (int i = 0; i < n; i++)
        if (all[i].is_atapi && disk_cd_iso_size(&all[i], &bytes) == 0) { *out = all[i]; return 1; }
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
static void find_marker(const ata_disk_t* src, uint32_t total_blocks) {
    g_mark_len = 0;
    uint32_t blocks = total_blocks < 2048u ? total_blocks : 2048u;
    uint8_t* b = (uint8_t*)kmalloc(blocks * 2048u);
    if (!b) return;
    if (disk_cd_read(src, 0, blocks, b) == 0) {
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

/* the CD's bytes as they go onto the disk */
static void patch_marker(uint8_t* b, uint32_t n) {
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

static int stream_image(const ata_disk_t* src, uint32_t iso_bytes, image_sink_t sink, void* ctx) {
    uint32_t total = iso_bytes / 2048u;
    g_prog_done = 0;
    g_prog_total = total + 1;
    find_marker(src, total);
    uint8_t* buf = (uint8_t*)kmalloc(FSDISK_COPY_BUF_BYTES + CARRY);
    if (!buf) return -1;
    uint32_t done = 0, carry_blk = 0;
    int have_carry = 0, rc = 0;
    while (done < total && rc == 0) {
        uint32_t chunk = total - done;
        if (chunk > FSDISK_COPY_CHUNK_BLOCKS) chunk = FSDISK_COPY_CHUNK_BLOCKS;
        if (disk_cd_read(src, done, chunk, buf + CARRY) != 0) { rc = -1; break; }
        uint8_t* start = have_carry ? buf : buf + CARRY;
        uint32_t len = chunk * 2048u + (have_carry ? CARRY : 0);
        uint32_t first = have_carry ? carry_blk : done;
        patch_marker(start, len);
        done += chunk;
        g_prog_done = done;
        task_yield();                      /* the desktop keeps drawing (the installer's progress) */
        if (done < total) {
            /* the last block waits for the next chunk (a name may continue there) */
            if (len / 2048u > 1) rc = sink(first, len / 2048u - 1, start, ctx);
            memcpy(buf, start + len - CARRY, CARRY);
            have_carry = 1;
            carry_blk = first + len / 2048u - 1;
        } else {
            rc = sink(first, len / 2048u, start, ctx);
        }
    }
    kfree(buf);
    return rc;
}

static int sink_write(uint32_t blk, uint32_t nblk, const uint8_t* data, void* ctx) {
    return disk_write((const ata_disk_t*)ctx, blk * 4u, nblk * 4u, data) == 0 ? 0 : -1;
}

/* Copies the CD's boot image (block-exact, per the ISO9660 PVD) onto the
 * disk from LBA 0 - 2048-byte CD blocks become 4 disk sectors each. */
static int copy_boot_image(const ata_disk_t* src, const ata_disk_t* dst, uint32_t iso_bytes) {
    return stream_image(src, iso_bytes, sink_write, (void*)dst);
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
    return (disk_write(disk, slot_lba_of(l, slot), sectors, buf) == 0 && disk_flush(disk) == 0) ? FSDISK_OK : FSDISK_ERR_IO;
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

int fsdisk_install(void) {
    ata_disk_t target;
    int tr = fsdisk_find_target(&target);
    if (tr == 0) return FSDISK_ERR_NO_TARGET;
    if (tr < 0)  return FSDISK_ERR_AMBIGUOUS;

    ata_disk_t source;
    if (!find_atapi_source(&source)) return FSDISK_ERR_NO_SOURCE;

    uint32_t iso_bytes;
    if (disk_cd_iso_size(&source, &iso_bytes) != 0) return FSDISK_ERR_NO_SOURCE;
    if (iso_bytes > V4_BASE_LBA * FSDISK_SECTOR) return FSDISK_ERR_ISO_TOO_BIG;

    layout_t l;
    if (!layout_v4(&target, &l)) return FSDISK_ERR_TOO_SMALL;
    if (fs_snapshot_size() > slot_capacity(&l)) return FSDISK_ERR_FULL;

    if (copy_boot_image(&source, &target, iso_bytes) != 0) return FSDISK_ERR_IO;
    /* old headers: a layout-3 install's (past the new image), our second slot */
    if (iso_bytes <= V3_BASE_LBA * FSDISK_SECTOR) wipe_header(&target, V3_BASE_LBA);
    if (l.nslots == 2) wipe_header(&target, slot_lba_of(&l, 1));
    int rc = write_snapshot_slot(&target, &l, 0, 1);
    if (rc != FSDISK_OK) return rc;
    g_prog_done = g_prog_total;

    g_layout = l;
    g_target = target;
    g_have_target = 1;
    g_fi_state = -1;
    return FSDISK_OK;
}

int fsdisk_sync(void) {
    if (!g_have_target) return FSDISK_ERR_NO_TARGET;
    while (g_busy) task_sleep_ms(10);        /* the autosave task and `sync` take turns */
    g_busy = 1;
    int rc = write_snapshot_to(&g_target);
    g_busy = 0;
    g_last_err = rc;
    return rc;
}

int fsdisk_try_load(void) {
    ata_disk_t target;
    if (fsdisk_find_target(&target) != 1) return 0; /* none, or ambiguous */

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
        if (!g_have_target) continue;
        uint32_t gen = fs_generation();
        if (gen == g_synced_gen) { warned = 0; continue; }
        if (gen != seen) { seen = gen; changed_at = timer_ms(); continue; }   /* still changing */
        if (timer_ms() - changed_at < AUTOSAVE_QUIET_MS) continue;
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
    return g_have_target && fs_generation() != g_synced_gen;
}

int fsdisk_last_error(void) { return g_last_err; }

void fsdisk_space(uint32_t* used_bytes, uint32_t* capacity_bytes) {
    *used_bytes = fs_snapshot_size();
    *capacity_bytes = g_have_target ? slot_capacity(&g_layout) : 0;
}

/* ── updating an install from the CD: the system only, files kept ── */

static ata_disk_t g_fi_disk;

int fsdisk_find_install(ata_disk_t* out) {
    if (g_fi_state < 0) {
        g_fi_state = 0;
        ata_disk_t d;
        layout_t l;
        fsdisk_super_t sb[2];
        int have[2];
        if (fsdisk_find_target(&d) == 1 && detect(&d, &l, sb, have)) {
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
    if (fsdisk_find_target(&d) != 1 || !detect(&d, &l, sb, have)) return 0;
    return (int)l.version;
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
    ata_disk_t dst, src;
    uint32_t iso_bytes;
    if (!fsdisk_find_install(&dst)) return FSDISK_ERR_NO_INSTALL;
    if (!find_atapi_source(&src) || disk_cd_iso_size(&src, &iso_bytes) != 0) return FSDISK_ERR_NO_SOURCE;
    if (fsdisk_install_layout() != (int)LAYOUT_V4) return 0;    /* an old layout: always worth updating */
    cmp_ctx_t c = { &dst, (uint8_t*)kmalloc(FSDISK_COPY_BUF_BYTES + CARRY), 1 };
    if (!c.buf) return FSDISK_ERR_IO;
    int rc = stream_image(&src, iso_bytes, sink_compare, &c);
    kfree(c.buf);
    if (rc != 0) return FSDISK_ERR_IO;
    return c.same;
}

/* Moves a layout-3 install's files to layout 4: the newest good copy is
 * written to the new first slot (and checked) before the new, bigger boot
 * image goes over the old place. */
static int migrate_to_v4(const ata_disk_t* d) {
    layout_t old, nl;
    fsdisk_super_t sb[2];
    int have[2];
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
    ata_disk_t dst, src;
    uint32_t iso_bytes;
    if (!fsdisk_find_install(&dst)) return FSDISK_ERR_NO_INSTALL;
    if (!find_atapi_source(&src) || disk_cd_iso_size(&src, &iso_bytes) != 0) return FSDISK_ERR_NO_SOURCE;
    if (iso_bytes > V4_BASE_LBA * FSDISK_SECTOR) return FSDISK_ERR_ISO_TOO_BIG;
    int rc = migrate_to_v4(&dst);
    if (rc != FSDISK_OK) return rc;
    /* the boot area only: the files after it stay as they are */
    if (copy_boot_image(&src, &dst, iso_bytes) != 0) return FSDISK_ERR_IO;
    if (iso_bytes <= V3_BASE_LBA * FSDISK_SECTOR) wipe_header(&dst, V3_BASE_LBA);
    if (disk_flush(&dst) != 0) return FSDISK_ERR_IO;
    g_fi_state = -1;
    g_prog_done = g_prog_total;
    return FSDISK_OK;
}
