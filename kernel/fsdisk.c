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

/* Reserved space at the start of the target disk for the raw-copied boot
 * image (GRUB's hybrid MBR + core.img + the ISO9660 filesystem holding
 * kernel.bin) - generous headroom over the current ~9-15MB build. This
 * region is a byte-for-byte copy of the CD, so our own filesystem
 * snapshot has to live *after* it, never inside it. */
#define FSDISK_BOOT_RESERVE_BYTES (32u * 1024u * 1024u)
#define FSDISK_BASE_LBA (FSDISK_BOOT_RESERVE_BYTES / FSDISK_SECTOR)

#define FSDISK_COPY_CHUNK_BLOCKS 32u /* 32 * 2048B ATAPI blocks = 64KiB per chunk */

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t version;
    uint32_t payload_bytes;
    uint32_t checksum;
    uint32_t seq;           /* which slot is newer (0 on disks from before the slots) */
    uint32_t super_check;   /* this header's own check */
} fsdisk_super_t;

/* Boot-image copy chunk buffer. The filesystem snapshot itself is now
 * variable-sized (file data lives on the heap), so it gets a heap buffer
 * of exactly the right size per sync instead of a worst-case static one. */
#define FSDISK_COPY_BUF_BYTES (FSDISK_COPY_CHUNK_BLOCKS * 2048u)
static uint8_t g_buf[FSDISK_COPY_BUF_BYTES];

/* largest snapshot we'll ever try to read back (sanity bound) */
#define FSDISK_MAX_PAYLOAD (256u * 1024u * 1024u)

static int        g_have_target = 0;
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

/* Raw-copies `iso_bytes` (already block-exact, per the ISO9660 PVD) from
 * the ATAPI source onto the target ATA disk starting at LBA 0, translating
 * between the source's 2048-byte blocks and the target's 512-byte
 * sectors (1 block = 4 sectors) as it goes. */
static int copy_boot_image(const ata_disk_t* src, const ata_disk_t* dst, uint32_t iso_bytes) {
    uint32_t total_blocks = iso_bytes / 2048u;
    uint32_t done_blocks  = 0;

    while (done_blocks < total_blocks) {
        uint32_t chunk = total_blocks - done_blocks;
        if (chunk > FSDISK_COPY_CHUNK_BLOCKS) chunk = FSDISK_COPY_CHUNK_BLOCKS;

        if (disk_cd_read(src, done_blocks, chunk, g_buf) != 0)
            return -1;

        if (disk_write(dst, done_blocks * 4u, chunk * 4u, g_buf) != 0) return -1;
        done_blocks += chunk;
    }
    return 0;
}

/* ── the filesystem on disk: two slots, written in turn ──────────────
 * A sync writes the slot that does NOT hold the newest good copy, then
 * that one becomes the newest (by sequence number). A power cut during a
 * write leaves the other slot - the previous save - intact, and the
 * checksum tells the half-written one apart. Disks too small for two
 * slots (under ~550 MB) keep the single slot older versions used. */

#define FSDISK_SLOT_SECTORS (((uint32_t)sizeof(fsdisk_super_t) + FSDISK_MAX_PAYLOAD + 65535u) / FSDISK_SECTOR)

static int      g_cur_slot = 0;      /* the slot holding the newest copy */
static uint32_t g_seq = 0;
static uint32_t g_synced_gen = 0;    /* fs_generation() of what is on disk */
static int      g_busy = 0;
static int      g_last_err = 0;

static int two_slots(const ata_disk_t* d) {
    return d->sectors >= FSDISK_BASE_LBA + 2u * FSDISK_SLOT_SECTORS;
}
static uint32_t slot_lba(int slot) { return FSDISK_BASE_LBA + (uint32_t)slot * FSDISK_SLOT_SECTORS; }

static uint32_t super_check(const fsdisk_super_t* sb) {
    return sb->magic ^ (sb->seq * 2654435761u) ^ sb->payload_bytes ^ sb->checksum ^ 0x5EC0DE5Au;
}

static int write_snapshot_slot(const ata_disk_t* disk, int slot, uint32_t seq) {
    uint32_t gen = fs_generation();          /* (taken before: a change while writing syncs again) */
    uint32_t payload = fs_snapshot_size();
    if (payload > FSDISK_MAX_PAYLOAD) return FSDISK_ERR_FULL;
    uint32_t total   = (uint32_t)sizeof(fsdisk_super_t) + payload;
    uint32_t sectors = bytes_to_sectors(total);
    if (disk->sectors < slot_lba(slot) + sectors) return FSDISK_ERR_TOO_SMALL;

    uint8_t* buf = (uint8_t*)kzalloc(sectors * FSDISK_SECTOR);
    if (!buf) return FSDISK_ERR_IO;
    fs_snapshot_save(buf + sizeof(fsdisk_super_t));

    fsdisk_super_t sb;
    memset(&sb, 0, sizeof(sb));
    sb.magic         = FSDISK_MAGIC;
    sb.version       = FSDISK_VERSION;
    sb.payload_bytes = payload;
    sb.checksum      = checksum_of(buf + sizeof(fsdisk_super_t), payload);
    sb.seq           = seq;
    sb.super_check   = super_check(&sb);
    memcpy(buf, &sb, sizeof(sb));

    int rc = (disk_write(disk, slot_lba(slot), sectors, buf) == 0 && disk_flush(disk) == 0) ? FSDISK_OK : FSDISK_ERR_IO;
    kfree(buf);
    if (rc == FSDISK_OK) { g_cur_slot = slot; g_seq = seq; g_synced_gen = gen; }
    return rc;
}

/* the next save: into the other slot (or the only one) */
static int write_snapshot_to(const ata_disk_t* disk) {
    int slot = two_slots(disk) ? (g_cur_slot ^ 1) : 0;
    return write_snapshot_slot(disk, slot, g_seq + 1);
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
    if (iso_bytes > FSDISK_BOOT_RESERVE_BYTES) return FSDISK_ERR_ISO_TOO_BIG;

    if (fs_snapshot_size() > FSDISK_MAX_PAYLOAD) return FSDISK_ERR_FULL;
    uint32_t fs_sectors  = bytes_to_sectors((uint32_t)sizeof(fsdisk_super_t) + fs_snapshot_size());
    uint32_t need_sectors = FSDISK_BASE_LBA + fs_sectors;
    if (target.sectors < need_sectors) return FSDISK_ERR_TOO_SMALL;

    if (copy_boot_image(&source, &target, iso_bytes) != 0) return FSDISK_ERR_IO;
    /* a copy left in the second slot by an earlier install must never win */
    if (two_slots(&target)) {
        memset(g_buf, 0, FSDISK_SECTOR);
        if (disk_write(&target, slot_lba(1), 1, g_buf) != 0) return FSDISK_ERR_IO;
    }
    int rc = write_snapshot_slot(&target, 0, 1);
    if (rc != FSDISK_OK) return rc;

    g_target = target;
    g_have_target = 1;
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

/* reads and loads one slot's copy; 1 if it is good */
static int load_slot(const ata_disk_t* d, int slot, const fsdisk_super_t* sb) {
    uint32_t total   = (uint32_t)sizeof(fsdisk_super_t) + sb->payload_bytes;
    uint32_t sectors = bytes_to_sectors(total);
    if (slot_lba(slot) + sectors > d->sectors) return 0;
    uint8_t* buf = (uint8_t*)kmalloc(sectors * FSDISK_SECTOR);
    if (!buf) return 0;
    int ok = disk_read(d, slot_lba(slot), sectors, buf) == 0;
    if (ok && checksum_of(buf + sizeof(fsdisk_super_t), sb->payload_bytes) != sb->checksum) ok = 0;
    if (ok && fs_snapshot_load(buf + sizeof(fsdisk_super_t), sb->payload_bytes, sb->version) != 0) ok = 0;
    kfree(buf);
    return ok;
}

/* a slot's header, if it looks like a Banana OS filesystem */
static int read_super(const ata_disk_t* d, int slot, fsdisk_super_t* sb) {
    if (disk_read(d, slot_lba(slot), 1, g_buf) != 0) return 0;
    memcpy(sb, g_buf, sizeof(*sb));
    if (sb->magic != FSDISK_MAGIC) return 0;
    if (sb->version != FS_SNAPSHOT_V1 && sb->version != FS_SNAPSHOT_V2 && sb->version != FS_SNAPSHOT_V3) return 0;
    if (sb->payload_bytes == 0 || sb->payload_bytes > FSDISK_MAX_PAYLOAD) return 0;
    /* disks from before the two slots have no sequence number (0, unchecked) */
    if (sb->seq != 0 && sb->super_check != super_check(sb)) return 0;
    return 1;
}

int fsdisk_try_load(void) {
    ata_disk_t target;
    if (fsdisk_find_target(&target) != 1) return 0; /* none, or ambiguous */
    if (target.sectors <= FSDISK_BASE_LBA) return 0; /* too small to hold our region at all */

    fsdisk_super_t sb[2];
    int have[2] = { read_super(&target, 0, &sb[0]), 0 };
    if (two_slots(&target)) have[1] = read_super(&target, 1, &sb[1]);
    if (!have[0] && !have[1]) return 0;

    /* the newest first; if it is damaged (a cut during its write), the other */
    int first = (have[1] && (!have[0] || sb[1].seq > sb[0].seq)) ? 1 : 0;
    int order[2] = { first, first ^ 1 };
    for (int k = 0; k < 2; k++) {
        int s = order[k];
        if (!have[s] || !load_slot(&target, s, &sb[s])) continue;
        g_target = target;
        g_have_target = 1;
        g_cur_slot = s;
        g_seq = sb[s].seq;
        g_synced_gen = fs_generation();
        /* upgrade an older disk to the current format right away */
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
