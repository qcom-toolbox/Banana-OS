#include "fsdisk.h"
#include "fs.h"
#include "ata.h"
#include "atapi.h"
#include "types.h"
#include "kheap.h"
#include "kstring.h"

#define FSDISK_MAGIC   0x414E4142u /* "BANA" */
#define FSDISK_VERSION 2u   /* = FS_SNAPSHOT_V2; version 1 disks (Banana OS 0.4) still load */
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
    ata_disk_t all[4];
    ata_probe_disks(all);

    int found = 0;
    ata_disk_t match;
    for (int i = 0; i < 4; i++) {
        if (all[i].present && !all[i].is_atapi) {
            found++;
            match = all[i];
        }
    }
    if (found == 1) { *out = match; return 1; }
    if (found == 0) return 0;
    return -1;
}

static int find_atapi_source(ata_disk_t* out) {
    ata_disk_t all[4];
    ata_probe_disks(all);
    for (int i = 0; i < 4; i++) {
        if (all[i].present && all[i].is_atapi) { *out = all[i]; return 1; }
    }
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

        if (atapi_read_blocks(src->bus, src->is_slave, done_blocks, chunk, g_buf) != 0)
            return -1;

        uint32_t ata_lba     = done_blocks * 4u;
        uint32_t ata_sectors = chunk * 4u;
        uint32_t off = 0;
        while (ata_sectors > 0) {
            uint8_t piece = (uint8_t)((ata_sectors > 255u) ? 255u : ata_sectors);
            if (ata_write_sectors(dst->bus, dst->is_slave, ata_lba, piece, g_buf + off) != 0)
                return -1;
            ata_lba     += piece;
            ata_sectors -= piece;
            off         += (uint32_t)piece * FSDISK_SECTOR;
        }
        done_blocks += chunk;
    }
    return 0;
}

/* Writes superblock + payload from `buf` (header space included at the
 * front) starting at FSDISK_BASE_LBA. */
static int write_sectors(const ata_disk_t* disk, const uint8_t* buf, uint32_t sectors) {
    uint32_t lba = FSDISK_BASE_LBA, done = 0;
    while (done < sectors) {
        uint8_t chunk = (uint8_t)((sectors - done > 255u) ? 255u : (sectors - done));
        if (ata_write_sectors(disk->bus, disk->is_slave, lba, chunk,
                               buf + done * FSDISK_SECTOR) != 0)
            return -1;
        lba  += chunk;
        done += chunk;
    }
    return 0;
}

static int write_snapshot_to(const ata_disk_t* disk) {
    uint32_t payload = fs_snapshot_size();
    uint32_t total   = (uint32_t)sizeof(fsdisk_super_t) + payload;
    uint32_t sectors = bytes_to_sectors(total);
    if (disk->sectors < FSDISK_BASE_LBA + sectors) return FSDISK_ERR_TOO_SMALL;

    uint8_t* buf = (uint8_t*)kzalloc(sectors * FSDISK_SECTOR);
    if (!buf) return FSDISK_ERR_IO;
    fs_snapshot_save(buf + sizeof(fsdisk_super_t));

    fsdisk_super_t sb;
    sb.magic         = FSDISK_MAGIC;
    sb.version       = FSDISK_VERSION;
    sb.payload_bytes = payload;
    sb.checksum      = checksum_of(buf + sizeof(fsdisk_super_t), payload);
    memcpy(buf, &sb, sizeof(sb));

    int rc = write_sectors(disk, buf, sectors) == 0 ? FSDISK_OK : FSDISK_ERR_IO;
    kfree(buf);
    return rc;
}

int fsdisk_install(void) {
    ata_disk_t target;
    int tr = fsdisk_find_target(&target);
    if (tr == 0) return FSDISK_ERR_NO_TARGET;
    if (tr < 0)  return FSDISK_ERR_AMBIGUOUS;

    ata_disk_t source;
    if (!find_atapi_source(&source)) return FSDISK_ERR_NO_SOURCE;

    uint32_t iso_bytes;
    if (atapi_iso_size(source.bus, source.is_slave, &iso_bytes) != 0) return FSDISK_ERR_NO_SOURCE;
    if (iso_bytes > FSDISK_BOOT_RESERVE_BYTES) return FSDISK_ERR_ISO_TOO_BIG;

    uint32_t fs_sectors  = bytes_to_sectors((uint32_t)sizeof(fsdisk_super_t) + fs_snapshot_size());
    uint32_t need_sectors = FSDISK_BASE_LBA + fs_sectors;
    if (target.sectors < need_sectors) return FSDISK_ERR_TOO_SMALL;

    if (copy_boot_image(&source, &target, iso_bytes) != 0) return FSDISK_ERR_IO;
    int rc = write_snapshot_to(&target);
    if (rc != FSDISK_OK) return rc;

    g_target = target;
    g_have_target = 1;
    return FSDISK_OK;
}

int fsdisk_sync(void) {
    if (!g_have_target) return FSDISK_ERR_NO_TARGET;
    return write_snapshot_to(&g_target);
}

int fsdisk_try_load(void) {
    ata_disk_t target;
    if (fsdisk_find_target(&target) != 1) return 0; /* none, or ambiguous */
    if (target.sectors <= FSDISK_BASE_LBA) return 0; /* too small to hold our region at all */

    if (ata_read_sectors(target.bus, target.is_slave, FSDISK_BASE_LBA, 1, g_buf) != 0) return 0;

    fsdisk_super_t sb;
    memcpy(&sb, g_buf, sizeof(sb));
    if (sb.magic != FSDISK_MAGIC) return 0;
    if (sb.version != FS_SNAPSHOT_V1 && sb.version != FS_SNAPSHOT_V2) return 0;
    if (sb.payload_bytes == 0 || sb.payload_bytes > FSDISK_MAX_PAYLOAD) return 0;

    uint32_t total   = (uint32_t)sizeof(fsdisk_super_t) + sb.payload_bytes;
    uint32_t sectors = bytes_to_sectors(total);
    if (FSDISK_BASE_LBA + sectors > target.sectors) return 0;

    uint8_t* buf = (uint8_t*)kmalloc(sectors * FSDISK_SECTOR);
    if (!buf) return 0;
    uint32_t lba = FSDISK_BASE_LBA, done = 0;
    int ok = 1;
    while (done < sectors) {
        uint8_t chunk = (uint8_t)((sectors - done > 255u) ? 255u : (sectors - done));
        if (ata_read_sectors(target.bus, target.is_slave, lba, chunk,
                              buf + done * FSDISK_SECTOR) != 0) { ok = 0; break; }
        lba  += chunk;
        done += chunk;
    }
    if (ok && checksum_of(buf + sizeof(fsdisk_super_t), sb.payload_bytes) != sb.checksum) ok = 0;
    if (ok && fs_snapshot_load(buf + sizeof(fsdisk_super_t), sb.payload_bytes, sb.version) != 0) ok = 0;
    kfree(buf);
    if (!ok) return 0;

    g_target = target;
    g_have_target = 1;
    /* upgrade a 0.4 (v1) disk to the current format right away */
    if (sb.version != FSDISK_VERSION) write_snapshot_to(&target);
    return 1;
}

int fsdisk_is_installed(void) {
    return g_have_target;
}
