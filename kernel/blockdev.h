#ifndef BLOCKDEV_H
#define BLOCKDEV_H

#include "types.h"

/*
 * Removable block devices (USB sticks): usb/usbstorage.c registers one
 * per drive; kernel/fat32.c mounts the FAT32 volume it finds on it under
 * /mnt (from the "automount" task, never from inside the USB stack).
 */

#define BLOCKDEV_MAX 4

typedef struct blockdev {
    int      used;
    int      present;            /* 0 once unplugged */
    char     name[16];           /* "usb0", "nvme0" */
    char     kind[8];            /* "usb", "nvme": the mount point is /mnt/<kind>[N] */
    char     model[40];          /* "QEMU QEMU HARDDISK" */
    uint32_t sectors;            /* 512-byte sectors */
    uint32_t sector_size;
    /* count sectors at lba; 0 = ok */
    int    (*read)(struct blockdev* bd, uint32_t lba, uint32_t count, void* buf);
    int    (*write)(struct blockdev* bd, uint32_t lba, uint32_t count, const void* buf);
    void*    priv;
    int      mount_state;        /* 0 not tried yet, 1 mounted, -1 no usable volume */
    int      mnt;                /* fs mount id while mounted */
} blockdev_t;

/* a driver adds / removes a device (removing unmounts it first) */
blockdev_t* blockdev_register(const char* model, uint32_t sectors, uint32_t sector_size,
                              int (*read)(blockdev_t*, uint32_t, uint32_t, void*),
                              int (*write)(blockdev_t*, uint32_t, uint32_t, const void*),
                              void* priv);
/* the same for another kind of drive ("nvme") */
blockdev_t* blockdev_register_kind(const char* kind, const char* model, uint32_t sectors, uint32_t sector_size,
                                   int (*read)(blockdev_t*, uint32_t, uint32_t, void*),
                                   int (*write)(blockdev_t*, uint32_t, uint32_t, const void*),
                                   void* priv);
void        blockdev_unregister(blockdev_t* bd);
blockdev_t* blockdev_get(int i);          /* NULL if slot i is unused */

/* umount: unmounts the volume with fs mount id mnt and leaves it alone
 * (no automatic remount) until it is plugged in again; 1 if it was ours */
int  blockdev_eject(int mnt);
/* `mount -a`: try every unmounted stick again */
void blockdev_retry_all(void);

/* starts the task that mounts newly plugged sticks */
void blockdev_init(void);

#endif
