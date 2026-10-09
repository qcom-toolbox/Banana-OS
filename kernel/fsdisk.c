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
#include "blockdev.h"

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

/* a Banana OS image: GRUB's search for its build marker in the first 4 MB (its EFI program) */
static int src_is_banana(const img_src_t* s, uint32_t iso_bytes) {
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

static int stream_image(const img_src_t* src, uint32_t iso_bytes, image_sink_t sink, void* ctx) {
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
        if (src_read(src, done, chunk, buf + CARRY) != 0) { rc = -1; break; }
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
static int copy_boot_image(const img_src_t* src, const ata_disk_t* dst, uint32_t iso_bytes) {
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
#define V5_DIR_REC      (4u + FS_NAME_LEN + 4u)               /* idx, name, parent */
#define V5_FILE_REC     (4u + FS_NAME_LEN + 4u + 4u * 3u)     /* idx, name, parent, size, lba, check */
#define V5_BODY_MAX     (4u + FS_MAX_DIRS * V5_DIR_REC + 8u + FS_MAX_FILES * V5_FILE_REC)
#define V5_INLINE_MAX   ((V5_META_SECT - 1u) * FSDISK_SECTOR) /* a body that fits in the slot */

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
    uint32_t gen;          /* the file's data_gen when it was written */
    uint32_t lba, sectors; /* its extent (0 sectors: an empty file) */
    uint32_t size, check;
} v5_rec_t;

/* the parts of the disk in use, sorted by lba: the files' extents, the
 * metadata body's, and while moving an older disk to layout 5, the copy
 * being moved */
typedef struct { uint32_t lba, sectors; } v5_ext_t;

static int       g_v5;                     /* the installed disk is layout 5 */
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
    uint32_t seq, body_bytes, body_check;
    uint32_t body_lba;          /* 0: right after the header, in the slot (version 5) */
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
    } else if (h6.version == 6) {
        if (h6.check != meta6_check(&h6)) return 0;
        h->seq = h6.seq; h->body_bytes = h6.body_bytes; h->body_check = h6.body_check; h->body_lba = h6.body_lba;
        body_lba = h6.body_lba;
        if (body_lba < m->data_lba || body_lba + bytes_to_sectors(h6.body_bytes) > m->data_end) return 0;
    } else {
        return 0;
    }
    if (h->body_bytes < 12u || h->body_bytes > V5_BODY_MAX) return 0;
    uint32_t sectors = bytes_to_sectors(h->body_bytes);
    uint8_t* b = (uint8_t*)kmalloc(sectors * FSDISK_SECTOR);
    if (!b) return 0;
    if (disk_read(d, body_lba, sectors, b) != 0 || checksum_of(b, h->body_bytes) != h->body_check) {
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
    if (nd > FS_MAX_DIRS || off + nd * V5_DIR_REC + 8u > len) return 0;
    off += nd * V5_DIR_REC + 4u;
    uint32_t nf = rd32(b + off);
    off += 4;
    if (nf > FS_MAX_FILES || off + nf * V5_FILE_REC > len) return 0;
    for (uint32_t i = 0; i < nf; i++, off += V5_FILE_REC) {
        const uint8_t* r = b + off;
        uint32_t idx = rd32(r), size = rd32(r + 4 + FS_NAME_LEN + 4), lba = rd32(r + 4 + FS_NAME_LEN + 8);
        if (idx >= FS_MAX_FILES || size > FS_MAX_FILE_SIZE) return 0;
        if (size && (lba < m->data_lba || lba + v5_sectors_for(size) > m->data_end)) return 0;
    }
    return 1;
}

/* Rebuilds the tree from a checked body. The files' data stays on the disk
 * until used (v5_read_file): a boot no longer reads every file. */
static void v5_load_body(const uint8_t* b) {
    uint32_t off = 4, nd = rd32(b);
    char name[FS_NAME_LEN];
    fs_restore_begin();
    for (uint32_t i = 0; i < nd; i++, off += V5_DIR_REC) {
        memcpy(name, b + off + 4, FS_NAME_LEN);
        name[FS_NAME_LEN - 1] = '\0';
        fs_restore_dir((int)rd32(b + off), name, (int32_t)rd32(b + off + 4 + FS_NAME_LEN));
    }
    int home = (int32_t)rd32(b + off);
    off += 4;
    uint32_t nf = rd32(b + off);
    off += 4;
    ext_reset();
    rec_reset();
    for (uint32_t i = 0; i < nf; i++, off += V5_FILE_REC) {
        const uint8_t* r = b + off;
        uint32_t idx = rd32(r), size = rd32(r + 4 + FS_NAME_LEN + 4);
        uint32_t lba = rd32(r + 4 + FS_NAME_LEN + 8), check = rd32(r + 4 + FS_NAME_LEN + 12);
        int parent = (int32_t)rd32(r + 4 + FS_NAME_LEN);
        memcpy(name, r + 4, FS_NAME_LEN);
        name[FS_NAME_LEN - 1] = '\0';
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
    if (checksum_of(buf, size) != r.check) {
        klog("fsdisk: file #%d could not be read back (damaged)\n", idx);
        return -1;
    }
    return 0;
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
        if (!have[s] || !v5_body_ok(body[s], h[s].body_bytes, &m)) continue;
        g_mark = m;
        v5_load_body(body[s]);
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
        fs_set_disk_reader(v5_read_file);
    }
    return rc;
}

/* writes one file's private copy into a fresh extent */
static int v5_write_file(const ata_disk_t* d, v5_rec_t* rec, const uint8_t* data) {
    rec->sectors = v5_sectors_for(rec->size);
    rec->check = checksum_of(data, rec->size);
    if (!rec->sectors) { rec->lba = 0; return FSDISK_OK; }
    rec->lba = ext_alloc(rec->sectors);
    if (!rec->lba) { rec->sectors = 0; return FSDISK_ERR_FULL; }
    return write_chunked(d, rec->lba, rec->sectors, data) == 0 ? FSDISK_OK : FSDISK_ERR_IO;
}

/* A save: the changed files, then the metadata (see above). The tree is
 * read in one go before anything is written - the changed files' data is
 * copied - so the other tasks can keep changing files while it writes. */
static int v5_sync(const ata_disk_t* d) {
    uint32_t gen = fs_generation();
    int nfs = fs_file_slots(), nds = fs_dir_slots();     /* (files made while this writes: the next save) */
    uint32_t body_sect = bytes_to_sectors(4u + (uint32_t)nds * V5_DIR_REC + 8u + (uint32_t)nfs * V5_FILE_REC);
    if (rec_grow(nfs) != 0) return FSDISK_ERR_IO;
    uint8_t* meta = (uint8_t*)kzalloc((1u + body_sect) * FSDISK_SECTOR);
    uint8_t** copy = (uint8_t**)kzalloc((size_t)(nfs + 1) * sizeof(uint8_t*));
    uint32_t* rec_off = (uint32_t*)kzalloc((size_t)(nfs + 1) * sizeof(uint32_t));
    v5_rec_t* g_nrec = (v5_rec_t*)kzalloc((size_t)(nfs + 1) * sizeof(v5_rec_t));   /* the save being written */
    if (!meta || !copy || !rec_off || !g_nrec) { kfree(meta); kfree(copy); kfree(rec_off); kfree(g_nrec); return FSDISK_ERR_IO; }
    uint8_t* b = meta + FSDISK_SECTOR;
    int rc = FSDISK_OK;

    /* 1. the tree as it is now (nothing yields in here) */
    uint32_t off = 4, nd = 0;
    for (int i = 1; i < nds; i++) {
        const fs_dir_t* dir = fs_get_dir(i);
        if (!dir || !dir->used || dir->mnt) continue;
        wr32(b + off, (uint32_t)i);
        memcpy(b + off + 4, dir->name, FS_NAME_LEN);
        wr32(b + off + 4 + FS_NAME_LEN, (uint32_t)dir->parent_dir);
        off += V5_DIR_REC;
        nd++;
    }
    wr32(b, nd);
    wr32(b + off, (uint32_t)fs_home_dir());
    off += 4;
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
            n->valid = 1;
            n->gen = f->data_gen;
            n->size = f->size;
            uint32_t sectors = v5_sectors_for(f->size);
            if (sectors) {
                copy[i] = (uint8_t*)kmalloc(sectors * FSDISK_SECTOR);
                if (!copy[i]) { rc = FSDISK_ERR_IO; break; }
                memcpy(copy[i], f->content, f->size);
                memset(copy[i] + f->size, 0, sectors * FSDISK_SECTOR - f->size);
            }
        }
        uint8_t* r = b + off;
        wr32(r, (uint32_t)i);
        memcpy(r + 4, f->name, FS_NAME_LEN);
        wr32(r + 4 + FS_NAME_LEN, (uint32_t)f->parent_dir);
        wr32(r + 4 + FS_NAME_LEN + 4, n->size);
        rec_off[i] = off;
        off += V5_FILE_REC;
        nf++;
    }
    wr32(b + nf_off, nf);
    uint32_t body_bytes = off;

    /* 2. the changed files, each into a new extent (the other tasks run in between) */
    for (int i = 0; i < nfs && rc == FSDISK_OK; i++) {
        v5_rec_t* n = &g_nrec[i];
        if (!n->valid || (g_rec[i].valid && g_rec[i].gen == n->gen)) continue;
        rc = v5_write_file(d, n, copy[i] ? copy[i] : (const uint8_t*)"");
        task_yield();
    }
    uint32_t new_body_lba = 0, new_body_sect = 0;
    if (rc == FSDISK_OK) {
        for (int i = 0; i < nfs; i++) {
            if (!g_nrec[i].valid) continue;
            wr32(b + rec_off[i] + 4 + FS_NAME_LEN + 8, g_nrec[i].lba);
            wr32(b + rec_off[i] + 4 + FS_NAME_LEN + 12, g_nrec[i].check);
        }
        /* 3. the data is on the disk before the metadata that points at it */
        int slot = g_cur_slot ^ 1;
        uint32_t lba = g_mark.meta_lba + (uint32_t)slot * V5_META_SECT;
        uint32_t seq = g_seq + 1, body_check = checksum_of(b, body_bytes);
        uint32_t hdr_sectors = 1u + bytes_to_sectors(body_bytes);     /* the header, and the body after it */
        if (body_bytes <= V5_INLINE_MAX) {
            v5_meta_t h;
            memset(&h, 0, sizeof(h));
            h.magic = V5_MAGIC;
            h.version = 5;
            h.nonce = g_mark.nonce;
            h.seq = seq;
            h.body_bytes = body_bytes;
            h.body_check = body_check;
            h.check = meta_check(&h);
            memcpy(meta, &h, sizeof(h));
        } else {
            /* too big for the slot: the body into an extent of its own, the slot gets the header */
            new_body_sect = bytes_to_sectors(body_bytes);
            new_body_lba = ext_alloc(new_body_sect);
            if (!new_body_lba) { new_body_sect = 0; rc = FSDISK_ERR_FULL; }
            else if (write_chunked(d, new_body_lba, new_body_sect, b) != 0) rc = FSDISK_ERR_IO;
            v6_meta_t h;
            memset(&h, 0, sizeof(h));
            h.magic = V5_MAGIC;
            h.version = 6;
            h.nonce = g_mark.nonce;
            h.seq = seq;
            h.body_bytes = body_bytes;
            h.body_check = body_check;
            h.body_lba = new_body_lba;
            h.check = meta6_check(&h);
            memset(meta, 0, FSDISK_SECTOR);
            memcpy(meta, &h, sizeof(h));
            hdr_sectors = 1;
        }
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
        if (copy[i]) kfree(copy[i]);
    }
    kfree(copy);
    kfree(rec_off);
    kfree(g_nrec);
    kfree(meta);
    if (rc == FSDISK_OK) g_synced_gen = gen;
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
    fs_set_disk_reader(v5_read_file);
    return FSDISK_OK;
}

int fsdisk_install(void) {
    ata_disk_t target;
    int tr = fsdisk_find_target(&target);
    if (tr == 0) return FSDISK_ERR_NO_TARGET;
    if (tr < 0)  return FSDISK_ERR_AMBIGUOUS;

    img_src_t source;
    if (!find_source(&source)) return FSDISK_ERR_NO_SOURCE;

    uint32_t iso_bytes;
    if (src_iso_size(&source, &iso_bytes) != 0) return FSDISK_ERR_NO_SOURCE;
    if (iso_bytes > V4_BASE_LBA * FSDISK_SECTOR) return FSDISK_ERR_ISO_TOO_BIG;

    layout_t l;
    if (!layout_v4(&target, &l)) return FSDISK_ERR_TOO_SMALL;
    v5_mark_t m;
    v5_new_mark(&target, &m);
    if (bytes_to_sectors(fs_snapshot_size()) > m.data_end - m.data_lba) return FSDISK_ERR_FULL;

    if (copy_boot_image(&source, &target, iso_bytes) != 0) return FSDISK_ERR_IO;
    /* old headers: a layout-3 install's (past the new image), a layout-4 one's */
    if (iso_bytes <= V3_BASE_LBA * FSDISK_SECTOR) wipe_header(&target, V3_BASE_LBA);
    wipe_header(&target, V4_BASE_LBA);
    if (l.nslots == 2) wipe_header(&target, slot_lba_of(&l, 1));
    int rc = v5_format(&target, 0, 0);
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
    int rc = g_v5 ? v5_sync(&g_target) : write_snapshot_to(&g_target);
    g_busy = 0;
    g_last_err = rc;
    return rc;
}

int fsdisk_try_load(void) {
    ata_disk_t target;
    if (fsdisk_find_target(&target) != 1) return 0; /* none, or ambiguous */

    int r5 = v5_try_load(&target);
    if (r5 < 0) return -1;
    if (r5 == 1) {
        g_target = target;
        g_have_target = 1;
        g_synced_gen = fs_generation();
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

void fsdisk_space(uint64_t* used_bytes, uint64_t* capacity_bytes) {
    if (g_v5) {
        uint64_t used = (uint64_t)g_body_sect * FSDISK_SECTOR;
        for (int i = 0; i < g_nrecs; i++) if (g_rec[i].valid) used += (uint64_t)g_rec[i].sectors * FSDISK_SECTOR;
        *used_bytes = used;
        *capacity_bytes = (uint64_t)(g_mark.data_end - g_mark.data_lba) * FSDISK_SECTOR;
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
        layout_t l;
        fsdisk_super_t sb[2];
        int have[2];
        v5_mark_t m;
        if (fsdisk_find_target(&d) == 1 && (v5_detect(&d, &m) || detect(&d, &l, sb, have))) {
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
    if (fsdisk_find_target(&d) != 1) return 0;
    if (v5_detect(&d, &m)) return 5;
    if (!detect(&d, &l, sb, have)) return 0;
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
    ata_disk_t dst;
    img_src_t src;
    uint32_t iso_bytes;
    if (!fsdisk_find_install(&dst)) return FSDISK_ERR_NO_INSTALL;
    if (!find_source(&src) || src_iso_size(&src, &iso_bytes) != 0) return FSDISK_ERR_NO_SOURCE;
    int lay = fsdisk_install_layout();
    if (lay != (int)LAYOUT_V4 && lay != 5) return 0;            /* an old layout: always worth updating */
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
    v5_mark_t m;
    if (v5_detect(d, &m)) return FSDISK_OK;                     /* layout 5: its files stay where they are */
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
    ata_disk_t dst;
    img_src_t src;
    uint32_t iso_bytes;
    if (!fsdisk_find_install(&dst)) return FSDISK_ERR_NO_INSTALL;
    if (!find_source(&src) || src_iso_size(&src, &iso_bytes) != 0) return FSDISK_ERR_NO_SOURCE;
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
