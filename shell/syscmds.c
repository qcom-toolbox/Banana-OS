#include "syscmds.h"
#include "netcmds.h"
#include "../kernel/terminal.h"
#include "../kernel/kstring.h"
#include "../kernel/fs.h"
#include "../kernel/fat32.h"
#include "../kernel/blockdev.h"
#include "../usb/usbcore.h"
#include "../kernel/pkg.h"
#include "../kernel/app.h"
#include "../kernel/gui.h"
#include "../kernel/audio.h"

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
    say("%s unmounted - the drive can be removed\n", point);
}


/* ── pkg ──────────────────────────────────────────────────────────── */

static void print_info(const pkg_info_t* p) {
    say("  %-14s %-8s %-7s %s\n", p->name, p->version[0] ? p->version : "-", p->type, p->title);
    if (p->description[0]) say("                 %s\n", p->description);
}

static void abs_path(const char* in, char* out, int cap) {
    if (in[0] == '/' || in[0] == '~') { kstrlcpy(out, in, (size_t)cap); return; }
    char cwd[FS_PATH_LEN];
    fs_cwd_path(cwd, sizeof(cwd));
    ksnprintf(out, (size_t)cap, "%s%s%s", cwd, strcmp(cwd, "/") ? "/" : "", in);
}

static void cmd_pkg(int argc, char** argv) {
    const char* sub = argc >= 2 ? argv[1] : "help";
    char msg[160];
    if (strcmp(sub, "install") == 0 && argc >= 3) {
        for (int i = 2; i < argc; i++) {
            char path[FS_PATH_LEN];
            abs_path(argv[i], path, sizeof(path));
            if (pkg_install(path, msg, sizeof(msg)) == 0) say("pkg: %s\n", msg);
            else fail("pkg install", msg);
        }
    } else if ((strcmp(sub, "remove") == 0 || strcmp(sub, "uninstall") == 0) && argc >= 3) {
        for (int i = 2; i < argc; i++) {
            if (pkg_remove(argv[i], msg, sizeof(msg)) == 0) say("pkg: %s\n", msg);
            else fail("pkg remove", msg);
        }
    } else if (strcmp(sub, "list") == 0 || strcmp(sub, "ls") == 0) {
        static pkg_info_t list[64];
        int n = pkg_list(list, 64);
        if (!n) { terminal_writeln("no apps installed - `pkg install <file.bpk>` (try ~/Examples)"); return; }
        say("  %-14s %-8s %-7s %s\n", "NAME", "VERSION", "TYPE", "TITLE");
        for (int i = 0; i < n && i < 64; i++) print_info(&list[i]);
    } else if (strcmp(sub, "info") == 0 && argc >= 3) {
        pkg_info_t p;
        char path[FS_PATH_LEN];
        abs_path(argv[2], path, sizeof(path));
        if (pkg_get(argv[2], &p) == 0) say("installed in %s/%s:\n", PKG_DIR, p.name);
        else if (pkg_inspect(path, &p, msg, sizeof(msg)) == 0) say("package %s (%u files, %u bytes):\n", argv[2], p.files, p.bytes);
        else { fail("pkg info", msg); return; }
        print_info(&p);
        if (p.author[0]) say("  author: %s\n", p.author);
        say("  programs: %s%s%s\n", p.has_i686 ? "i686 " : "", p.has_x86_64 ? "x86_64" : "",
            (!p.has_i686 && !p.has_x86_64) ? "none" : "");
    } else if (strcmp(sub, "run") == 0 && argc >= 3) {
        int rc = pkg_run(argv[2], argc - 2, argv + 2, 0, msg, sizeof(msg));
        if (rc < 0 && msg[0]) fail(argv[2], msg);
    } else if (strcmp(sub, "ps") == 0) {
        app_list();
    } else {
        terminal_writeln("usage: pkg install <file.bpk>...   install (or upgrade) apps");
        terminal_writeln("       pkg list                    installed apps");
        terminal_writeln("       pkg info <name|file.bpk>    details");
        terminal_writeln("       pkg remove <name>...        uninstall");
        terminal_writeln("       pkg run <name> [args]       run one (or just type its name)");
        terminal_writeln("       pkg ps                      running apps");
        terminal_writeln("Apps are built on Linux with the Banana OS SDK (sdk/ in the source tree).");
    }
}


/* ── sound ────────────────────────────────────────────────────────── */

static void cmd_play(int argc, char** argv) {
    if (argc < 2) { terminal_writeln("usage: play <file.wav>     (play -s stops what is playing)"); return; }
    if (strcmp(argv[1], "-s") == 0 || strcmp(argv[1], "stop") == 0) { audio_stop(); say("stopped\n"); return; }
    char err[128];
    if (audio_play_wav(argv[1], err, sizeof(err)) != 0) { fail("play", err); return; }
    say("playing %s (%s) - it goes on in the background; `play -s` stops it\n", argv[1], audio_device_name());
}

static void cmd_beep(int argc, char** argv) {
    uint32_t hz = 880, ms = 200;
    if (argc >= 2) k_parse_u32(argv[1], &hz);
    if (argc >= 3) k_parse_u32(argv[2], &ms);
    audio_beep((int)hz, (int)ms);
}

static void cmd_volume(int argc, char** argv) {
    if (argc >= 2) {
        uint32_t v;
        if (!k_parse_u32(argv[1], &v) || v > 100) { fail("volume", "0 to 100"); return; }
        audio_set_volume((int)v);
    }
    say("volume: %d%%\n", audio_get_volume());
}
int syscmd_try_app(const char* line) {
    char buf[1024];
    kstrlcpy(buf, line, sizeof(buf));
    char* argv[16];
    int argc = shell_split_args(buf, argv, 16);
    if (argc == 0) return 0;
    pkg_info_t p;
    if (pkg_get(argv[0], &p) != 0) return 0;
    char err[128];
    err[0] = 0;
    int rc = pkg_run(argv[0], argc, argv, 0, err, sizeof(err));
    if (rc < 0 && err[0]) fail(argv[0], err);
    return 1;
}
int syscmd_dispatch(const char* line) {
    char buf[1024];
    kstrlcpy(buf, line, sizeof(buf));
    char* argv[16];
    int argc = shell_split_args(buf, argv, 16);
    if (argc == 0) return 0;
    if (strcmp(argv[0], "mount") == 0) { cmd_mount(argc, argv); return 1; }
    if (strcmp(argv[0], "umount") == 0 || strcmp(argv[0], "eject") == 0) { cmd_umount(argc, argv); return 1; }
    if (strcmp(argv[0], "pkg") == 0) { cmd_pkg(argc, argv); return 1; }
    if (strcmp(argv[0], "play") == 0) { cmd_play(argc, argv); return 1; }
    if (strcmp(argv[0], "beep") == 0) { cmd_beep(argc, argv); return 1; }
    if (strcmp(argv[0], "volume") == 0) { cmd_volume(argc, argv); return 1; }
    if (strcmp(argv[0], "lsaudio") == 0) { audio_list(); return 1; }
    if (strcmp(argv[0], "apps") == 0) {
        if (!gui_open_apps()) cmd_pkg(2, (char*[]){ "pkg", "list" });
        return 1;
    }
    if (strcmp(argv[0], "taskmgr") == 0) {
        if (!gui_open_taskmgr()) terminal_writeln("taskmgr: the Task Manager is part of the desktop (startx) - try `top` here");
        return 1;
    }
    return 0;
}
