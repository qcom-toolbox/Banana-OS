#include "syscmds.h"
#include "netcmds.h"
#include "../kernel/terminal.h"
#include "../kernel/kstring.h"
#include "../kernel/fs.h"
#include "../kernel/fat32.h"
#include "../kernel/blockdev.h"
#include "../usb/usbcore.h"

static void say(const char* fmt, ...) {
    char buf[256];
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    kvsnprintf(buf, sizeof(buf), fmt, ap);
    __builtin_va_end(ap);
    terminal_write(buf);
}

static void fail(const char* cmd, const char* msg) {
    terminal_write_color(cmd, VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
    terminal_write_color(": ", VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
    terminal_write_color(msg, VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
    terminal_putchar('\n');
}

/* ── mount / umount ───────────────────────────────────────────────── */

static void cmd_mount(int argc, char** argv) {
    if (argc >= 2 && strcmp(argv[1], "-a") == 0) {
        blockdev_retry_all();
        say("looking for USB sticks to mount again...\n");
        return;
    }
    int any = 0;
    char line[160];
    for (int m = 1; m <= FS_MAX_MOUNTS; m++) {
        if (!fat32_describe(m, line, sizeof(line))) continue;
        terminal_writeln(line);
        any = 1;
    }
    /* sticks that are plugged in but not mounted */
    for (int i = 0; i < BLOCKDEV_MAX; i++) {
        blockdev_t* bd = blockdev_get(i);
        if (!bd || !bd->present || bd->mount_state == 1) continue;
        uint32_t mib = (uint32_t)(((uint64_t)bd->sectors * bd->sector_size) >> 20);
        say("%s: %s, %u MiB - %s\n", bd->name, bd->model, mib,
            bd->mount_state == 0 ? "mounting..." : bd->mount_state == 2 ? "unmounted (`mount -a` mounts it again)"
                                                                     : "no FAT32 volume (see the boot log)");
        any = 1;
    }
    if (!any) terminal_writeln("no USB stick mounted - plug one in (FAT32), or `usb rescan`");
}

static void cmd_umount(int argc, char** argv) {
    int mnt = 0;
    if (argc >= 2) {
        mnt = fs_path_mount(argv[1]);
        if (!mnt) { fail(argv[0], "not a mounted USB stick"); return; }
    } else {
        for (int m = 1; m <= FS_MAX_MOUNTS && !mnt; m++) if (fs_mount_point(m)[0]) mnt = m;
        if (!mnt) { fail(argv[0], "nothing is mounted"); return; }
    }
    char point[FS_PATH_LEN];
    kstrlcpy(point, fs_mount_point(mnt), sizeof(point));
    if (!blockdev_eject(mnt)) fs_unmount(mnt);
    say("%s unmounted - the stick can be removed\n", point);
}

int syscmd_dispatch(const char* line) {
    char buf[1024];
    kstrlcpy(buf, line, sizeof(buf));
    char* argv[16];
    int argc = shell_split_args(buf, argv, 16);
    if (argc == 0) return 0;
    if (strcmp(argv[0], "mount") == 0) { cmd_mount(argc, argv); return 1; }
    if (strcmp(argv[0], "umount") == 0 || strcmp(argv[0], "eject") == 0) { cmd_umount(argc, argv); return 1; }
    return 0;
}
