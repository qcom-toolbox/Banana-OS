#include "blockdev.h"
#include "fat32.h"
#include "fs.h"
#include "task.h"
#include "kstring.h"
#include "serial.h"

static blockdev_t g_bd[BLOCKDEV_MAX];

blockdev_t* blockdev_register(const char* model, uint32_t sectors, uint32_t sector_size,
                              int (*read)(blockdev_t*, uint32_t, uint32_t, void*),
                              int (*write)(blockdev_t*, uint32_t, uint32_t, const void*),
                              void* priv) {
    return blockdev_register_kind("usb", model, sectors, sector_size, read, write, priv);
}

blockdev_t* blockdev_register_kind(const char* kind, const char* model, uint32_t sectors, uint32_t sector_size,
                                   int (*read)(blockdev_t*, uint32_t, uint32_t, void*),
                                   int (*write)(blockdev_t*, uint32_t, uint32_t, const void*),
                                   void* priv) {
    int same = 0;
    for (int i = 0; i < BLOCKDEV_MAX; i++) if (g_bd[i].used && strcmp(g_bd[i].kind, kind) == 0) same++;
    for (int i = 0; i < BLOCKDEV_MAX; i++) {
        blockdev_t* bd = &g_bd[i];
        if (bd->used) continue;
        memset(bd, 0, sizeof(*bd));
        bd->used = 1;
        bd->present = 1;
        kstrlcpy(bd->kind, kind, sizeof(bd->kind));
        ksnprintf(bd->name, sizeof(bd->name), "%s%d", kind, same);
        kstrlcpy(bd->model, model, sizeof(bd->model));
        bd->sectors = sectors;
        bd->sector_size = sector_size;
        bd->read = read;
        bd->write = write;
        bd->priv = priv;
        return bd;
    }
    return NULL;
}

void blockdev_unregister(blockdev_t* bd) {
    if (!bd || !bd->used) return;
    bd->present = 0;
    if (bd->mount_state == 1) {
        klog("blockdev: %s removed while mounted at %s\n", bd->name, fs_mount_point(bd->mnt));
        fs_unmount(bd->mnt);            /* the volume's ops unmount() forgets it */
    }
    bd->used = 0;
}

blockdev_t* blockdev_get(int i) {
    return (i >= 0 && i < BLOCKDEV_MAX && g_bd[i].used) ? &g_bd[i] : NULL;
}

/* Mounting reads the whole directory tree, which takes many USB transfers:
 * that happens here, in a task of its own, rather than inside the USB
 * stack's enumeration (which runs from usb_poll()). */
static void automount_task(void) {
    task_set_background();
    for (;;) {
        for (int i = 0; i < BLOCKDEV_MAX; i++) {
            blockdev_t* bd = &g_bd[i];
            if (!fs_is_ready() || !bd->used || !bd->present || bd->mount_state != 0) continue;
            char err[80];
            int mnt = fat32_mount(bd, NULL, err, sizeof(err));
            if (mnt > 0) {
                bd->mnt = mnt;
                bd->mount_state = 1;
            } else {
                bd->mount_state = -1;
                klog("blockdev: %s not mounted: %s\n", bd->name, err);
            }
        }
        task_sleep_ms(250);
    }
}

void blockdev_init(void) {
    task_create("automount", automount_task);
}

int blockdev_eject(int mnt) {
    for (int i = 0; i < BLOCKDEV_MAX; i++) {
        blockdev_t* bd = &g_bd[i];
        if (!bd->used || bd->mount_state != 1 || bd->mnt != mnt) continue;
        fs_unmount(mnt);
        bd->mount_state = 2;
        bd->mnt = 0;
        return 1;
    }
    return 0;
}

void blockdev_retry_all(void) {
    for (int i = 0; i < BLOCKDEV_MAX; i++)
        if (g_bd[i].used && g_bd[i].present && g_bd[i].mount_state != 1) g_bd[i].mount_state = 0;
}
