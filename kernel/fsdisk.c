#include "fsdisk.h"
#include "fs.h"
#include "ata.h"
#include "types.h"

#define FSDISK_MAGIC   0x414E4142u /* "BANA" */
#define FSDISK_VERSION 1u
#define FSDISK_SECTOR  512u

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t version;
    uint32_t payload_bytes;
    uint32_t checksum;
} fsdisk_super_t;

/* Scratch buffer sized exactly to fs.c's current capacity (superblock +
 * the full dirs[]/files[] snapshot, ~132KB), rounded up to a sector. This
 * kernel has no heap allocator, so a static buffer it is - reused for
 * both writing (install/sync) and reading (boot-time load), which is
 * fine since neither is ever called concurrently with the other. */
#define FSDISK_PAYLOAD_MAX \
    (FS_MAX_DIRS * sizeof(fs_dir_t) + FS_MAX_FILES * sizeof(fs_file_t) + sizeof(int32_t))
#define FSDISK_BUF_BYTES \
    ((((uint32_t)sizeof(fsdisk_super_t) + FSDISK_PAYLOAD_MAX + FSDISK_SECTOR - 1u) / FSDISK_SECTOR) * FSDISK_SECTOR)

static uint8_t g_buf[FSDISK_BUF_BYTES];

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

static int write_snapshot_to(const ata_disk_t* disk) {
    uint32_t payload = fs_snapshot_size();
    if ((uint32_t)sizeof(fsdisk_super_t) + payload > FSDISK_BUF_BYTES) return -1;

    fs_snapshot_save(g_buf + sizeof(fsdisk_super_t));

    fsdisk_super_t sb;
    sb.magic         = FSDISK_MAGIC;
    sb.version       = FSDISK_VERSION;
    sb.payload_bytes = payload;
    sb.checksum      = checksum_of(g_buf + sizeof(fsdisk_super_t), payload);

    uint8_t* p = (uint8_t*)&sb;
    for (uint32_t i = 0; i < sizeof(sb); i++) g_buf[i] = p[i];

    uint32_t total   = (uint32_t)sizeof(fsdisk_super_t) + payload;
    uint32_t sectors = bytes_to_sectors(total);
    /* zero the pad between payload end and the sector boundary so a
     * later read-back's checksum check is deterministic */
    for (uint32_t i = total; i < sectors * FSDISK_SECTOR; i++) g_buf[i] = 0;

    uint32_t lba = 0, done = 0;
    while (done < sectors) {
        uint8_t chunk = (uint8_t)((sectors - done > 255u) ? 255u : (sectors - done));
        if (ata_write_sectors(disk->bus, disk->is_slave, lba, chunk,
                               g_buf + (uint32_t)done * FSDISK_SECTOR) != 0)
            return -1;
        lba  += chunk;
        done += chunk;
    }
    return 0;
}

int fsdisk_install(void) {
    ata_disk_t target;
    if (fsdisk_find_target(&target) != 1) return -1;
    if (write_snapshot_to(&target) != 0) return -1;

    g_target = target;
    g_have_target = 1;
    return 0;
}

int fsdisk_sync(void) {
    if (!g_have_target) return -1;
    return write_snapshot_to(&g_target);
}

int fsdisk_try_load(void) {
    ata_disk_t target;
    if (fsdisk_find_target(&target) != 1) return 0; /* none, or ambiguous */

    if (ata_read_sectors(target.bus, target.is_slave, 0, 1, g_buf) != 0) return 0;

    fsdisk_super_t sb;
    uint8_t* p = (uint8_t*)&sb;
    for (uint32_t i = 0; i < sizeof(sb); i++) p[i] = g_buf[i];

    if (sb.magic != FSDISK_MAGIC || sb.version != FSDISK_VERSION) return 0;
    if (sb.payload_bytes == 0 || sb.payload_bytes != fs_snapshot_size()) return 0;
    if ((uint32_t)sizeof(fsdisk_super_t) + sb.payload_bytes > FSDISK_BUF_BYTES) return 0;

    uint32_t total   = (uint32_t)sizeof(fsdisk_super_t) + sb.payload_bytes;
    uint32_t sectors = bytes_to_sectors(total);

    uint32_t lba = 0, done = 0;
    while (done < sectors) {
        uint8_t chunk = (uint8_t)((sectors - done > 255u) ? 255u : (sectors - done));
        if (ata_read_sectors(target.bus, target.is_slave, lba, chunk,
                              g_buf + (uint32_t)done * FSDISK_SECTOR) != 0)
            return 0;
        lba  += chunk;
        done += chunk;
    }

    if (checksum_of(g_buf + sizeof(fsdisk_super_t), sb.payload_bytes) != sb.checksum) return 0;

    fs_snapshot_load(g_buf + sizeof(fsdisk_super_t));

    g_target = target;
    g_have_target = 1;
    return 1;
}

int fsdisk_is_installed(void) {
    return g_have_target;
}
