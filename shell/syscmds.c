/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "syscmds.h"
#include "fstest.h"
#include "selftest.h"
#include "crashtest.h"
#include "netcmds.h"
#include "../kernel/terminal.h"
#include "../kernel/kstring.h"
#include "../kernel/fs.h"
#include "../kernel/fat32.h"
#include "../kernel/blockdev.h"
#include "../kernel/volume.h"
#include "../kernel/disk.h"
#include "../kernel/fsdisk.h"
#include "../kernel/kheap.h"
#include "../usb/usbcore.h"
#include "../kernel/pkg.h"
#include "../kernel/builtin_apps.h"
#include "../kernel/app.h"
#include "../kernel/gui.h"
#include "../kernel/audio.h"
#include "../kernel/settings.h"
#include "aptcmd.h"
#include "adbcmd.h"
#include "datecmd.h"
#include "themecmd.h"
#include "devcmd.h"
#include "diskcmd.h"
#include "diskbench.h"
#include "../kernel/ext4.h"
#include "../kernel/notification.h"

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

/* "sata1" -> the drive, number 0; "sata1p2" -> the drive, partition 2 */
static blockdev_t* parse_dev(const char* s, int* part) {
    char name[16];
    int n = 0;
    while (s[n] && n < (int)sizeof(name) - 1) { name[n] = s[n]; n++; }
    name[n] = 0;
    *part = 0;
    blockdev_t* bd = blockdev_find(name);
    if (bd) return bd;
    /* a trailing p<N> */
    for (int i = n - 1; i > 0; i--) {
        if (name[i] >= '0' && name[i] <= '9') continue;
        if (name[i] != 'p' || i == n - 1) break;
        uint32_t v;
        if (!k_parse_u32(name + i + 1, &v)) break;
        name[i] = 0;
        *part = (int)v;
        return blockdev_find(name);
    }
    return NULL;
}

static void size_str(uint64_t bytes, char* out, int cap) {
    static const char U[] = "BKMGTP";
    int u = 0;
    uint64_t whole = bytes, frac = 0;
    while (whole >= 1024 && u < 5) { frac = (whole % 1024) * 10 / 1024; whole /= 1024; u++; }
    if (u && whole < 10) ksnprintf(out, (size_t)cap, "%u.%u%c", (uint32_t)whole, (uint32_t)frac, U[u]);
    else ksnprintf(out, (size_t)cap, "%u%c", (uint32_t)whole, U[u]);
}

/* the mount point of bd's volume at base ("" if not mounted) */
static const char* point_of(blockdev_t* bd, uint32_t base) {
    for (int v = 0; v < bd->nvol; v++) if (bd->vol_base[v] == base) return fs_mount_point(bd->vol_mnt[v]);
    return "";
}

static void lsblk_dev(blockdev_t* bd) {
    char sz[16], b[200];
    size_str((uint64_t)bd->sectors * bd->sector_size, sz, sizeof(sz));
    const char* whole = point_of(bd, 0);
    ksnprintf(b, sizeof(b), "%-10s %7s  disk  %-6s %-16s %s", bd->name, sz,
              whole[0] ? volume_fs_name(bd->vol_mnt[0]) : "", bd->system ? "(Banana OS)" : whole, bd->model);
    terminal_write_color(b, VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    terminal_putchar('\n');
    vol_part_t* parts = (vol_part_t*)kmalloc(sizeof(vol_part_t) * VOL_MAX_PARTS);
    if (!parts) return;
    int gpt = 0, np = volume_partitions(bd, parts, VOL_MAX_PARTS, &gpt);
    for (int i = 0; i < np; i++) {
        const char* pt = point_of(bd, parts[i].base);
        const char* fst = "";
        for (int v = 0; v < bd->nvol; v++) if (bd->vol_base[v] == parts[i].base) fst = volume_fs_name(bd->vol_mnt[v]);
        char nm[24];
        ksnprintf(nm, sizeof(nm), "%s%sp%d", i + 1 < np ? "|-" : "`-", bd->name, parts[i].number);
        size_str((uint64_t)parts[i].sectors * bd->sector_size, sz, sizeof(sz));
        ksnprintf(b, sizeof(b), "%-10s %7s  part  %-6s %-16s %s%s%s", nm, sz, fst, pt, parts[i].type,
                  parts[i].label[0] ? " - " : "", parts[i].label);
        terminal_writeln(b);
    }
    kfree(parts);
}

static void cmd_lsblk(void) {
    terminal_write_color("NAME          SIZE  TYPE  FSTYPE MOUNTPOINT       MODEL / PARTITION TYPE\n",
                         VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
    /* the disks that are no block device: the installed one (IDE / SATA) and CD drives */
    ata_disk_t all[DISK_MAX];
    int n = disk_probe(all, DISK_MAX);
    ata_disk_t inst;
    int have_inst = fsdisk_is_installed() && fsdisk_find_installed(&inst);
    for (int i = 0; i < n; i++) {
        const ata_disk_t* d = &all[i];
        if (disk_blockdev(d) && !d->is_atapi) continue;
        char where[32], sz[16], b[200];
        disk_describe(d, where, sizeof(where));
        if (d->is_atapi) {
            ksnprintf(b, sizeof(b), "%-10s %7s  rom   %-6s %-16s %s", "cd", "-", "iso9660", "", d->model);
        } else {
            size_str((uint64_t)d->sectors * 512u, sz, sizeof(sz));
            int sys = have_inst && disk_same(d, &inst);
            ksnprintf(b, sizeof(b), "%-10s %7s  disk  %-6s %-16s %s (%s)", sys ? "system" : "disk", sz,
                      sys ? "banana" : "", sys ? "/" : "", d->model, where);
        }
        terminal_writeln(b);
    }
    for (int i = 0; i < BLOCKDEV_MAX; i++) {
        blockdev_t* bd = blockdev_get(i);
        if (bd && bd->present) lsblk_dev(bd);
    }
}

static void cmd_mount(int argc, char** argv) {
    if (argc >= 2 && strcmp(argv[1], "-a") == 0) {
        blockdev_retry_all();
        say("looking for drives to mount again...\n");
        return;
    }
    if (argc >= 2) {
        /* mount <drive>[p<N>] [folder] */
        int part = 0;
        blockdev_t* bd = parse_dev(argv[1], &part);
        if (!bd) { fail(argv[0], "no such drive (lsblk lists them: sata1, usb0, nvme0p2...)"); return; }
        if (bd->system) { fail(argv[0], "that is Banana OS's own disk"); return; }
        char err[96];
        if (!part) {
            if (bd->nvol) { say("%s: already mounted at %s\n", bd->name, fs_mount_point(bd->mnt)); return; }
            int k = volume_mount_all(bd, err, sizeof(err));
            if (k <= 0) { fail(argv[0], err); bd->mount_state = -1; return; }
            for (int v = 0; v < bd->nvol; v++) say("%s: mounted at %s\n", bd->name, fs_mount_point(bd->vol_mnt[v]));
            return;
        }
        vol_part_t* parts = (vol_part_t*)kmalloc(sizeof(vol_part_t) * VOL_MAX_PARTS);
        if (!parts) return;
        int np = volume_partitions(bd, parts, VOL_MAX_PARTS, NULL), found = -1;
        for (int i = 0; i < np; i++) if (parts[i].number == part) found = i;
        if (found < 0) { kfree(parts); fail(argv[0], "no such partition"); return; }
        uint32_t base = parts[found].base;
        kfree(parts);
        if (point_of(bd, base)[0]) { say("%sp%d: already mounted at %s\n", bd->name, part, point_of(bd, base)); return; }
        int m = volume_mount_at(bd, base, argc >= 3 ? argv[2] : NULL, err, sizeof(err));
        if (m <= 0) { fail(argv[0], err); return; }
        say("%sp%d: mounted at %s\n", bd->name, part, fs_mount_point(m));
        return;
    }
    int any = 0;
    char line[160];
    for (int m = 1; m <= FS_MAX_MOUNTS; m++) {
        if (!volume_describe(m, line, sizeof(line))) continue;
        terminal_writeln(line);
        any = 1;
    }
    /* drives that are there but not mounted */
    for (int i = 0; i < BLOCKDEV_MAX; i++) {
        blockdev_t* bd = blockdev_get(i);
        if (!bd || !bd->present || bd->mount_state == 1 || bd->system) continue;
        uint32_t mib = (uint32_t)(((uint64_t)bd->sectors * bd->sector_size) >> 20);
        say("%s: %s, %u MiB - %s\n", bd->name, bd->model, mib,
            bd->mount_state == 0 ? "mounting..." : bd->mount_state == 2 ? "unmounted (`mount -a` or `mount <drive>` mounts it again)"
                                                                     : "no FAT32 or exFAT volume (see the boot log)");
        any = 1;
    }
    if (!any) terminal_writeln("no drive mounted - plug one in (FAT32 or exFAT), or `usb rescan`");
}

static void cmd_umount(int argc, char** argv) {
    int mnt = 0;
    if (argc >= 2) {
        int part = 0;
        blockdev_t* bd = argv[1][0] != '/' && argv[1][0] != '~' ? parse_dev(argv[1], &part) : NULL;
        if (bd && !part) {                           /* a whole drive: every volume */
            if (!bd->nvol) { fail(argv[0], "nothing of it is mounted"); return; }
            blockdev_eject_all(bd);
            say("%s unmounted - the drive can be removed\n", bd->name);
            return;
        }
        mnt = fs_path_mount(argv[1]);
        if (!mnt) { fail(argv[0], "not a mounted drive"); return; }
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
    } else if (strcmp(sub, "verify") == 0 || strcmp(sub, "check") == 0) {
        /* every file of each package against the list made when it was installed */
        static pkg_info_t list[256];
        int n = 0;
        if (argc >= 3) {
            for (int i = 2; i < argc && n < 256; i++) kstrlcpy(list[n++].name, argv[i], sizeof(list[0].name));
        } else n = pkg_list(list, 256);
        int damaged = 0, unknown = 0;
        for (int i = 0; i < n; i++) {
            char first[64];
            int bad = pkg_verify(list[i].name, 1, first, sizeof(first));
            if (bad == -2) { say("  %-14s not installed\n", list[i].name); continue; }
            if (bad == -1) { unknown++; say("  %-14s no file list (installed by an older Banana OS)\n", list[i].name); continue; }
            if (bad == 0) { say("  %-14s ok\n", list[i].name); continue; }
            damaged++;
            say("  %-14s %d file%s damaged or missing (%s)\n", list[i].name, bad, bad == 1 ? "" : "s", first);
            int rc = builtin_app_reinstall(list[i].name, msg, sizeof(msg));
            if (rc == 0) say("  %-14s repaired: %s\n", "", msg);
            else if (rc == 1) say("  %-14s %s\n", "", msg);
            else if (rc == -1) say("  %-14s install it again: apt install %s (or pkg install its .bpk)\n", "", list[i].name);
            else fail("pkg verify", msg);
        }
        say("pkg verify: %d checked, %d damaged%s\n", n, damaged,
            unknown ? " (some without a file list: reinstall them to get one)" : "");
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
        terminal_writeln("       pkg verify [name...]        check the installed files; repair built-in apps");
        terminal_writeln("       pkg run <name> [args]       run one (or just type its name)");
        terminal_writeln("       pkg ps                      running apps");
        terminal_writeln("From repositories: apt update / search / install <name> / upgrade (or the App Store).");
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
        char vs[8];
        ksnprintf(vs, sizeof(vs), "%u", v);
        settings_set("volume", vs);
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
    if (strcmp(argv[0], "lsblk") == 0) { cmd_lsblk(); return 1; }
    if (strcmp(argv[0], "umount") == 0 || strcmp(argv[0], "eject") == 0) { cmd_umount(argc, argv); return 1; }
    if (strcmp(argv[0], "fstest") == 0) return fstest_cmd(argc, argv);
    if (strcmp(argv[0], "crashtest") == 0) return crashtest_cmd(argc, argv);
    if (strcmp(argv[0], "selftest") == 0) return selftest_cmd(argc, argv);
    if (datecmd_dispatch(argc, argv)) return 1;
    if (themecmd_dispatch(argc, argv)) return 1;
    if (devcmd_dispatch(argc, argv)) return 1;
    if (diskcmd_dispatch(argc, argv)) return 1;
    if (strcmp(argv[0], "diskbench") == 0) return diskbench_cmd(argc, argv);
    if (strcmp(argv[0], "drop_caches") == 0) {
        /* as Linux's /proc/sys/vm/drop_caches: what RAM holds of the disk goes */
        uint32_t files = fs_evict(0xFFFFFFFFu, 0);
        uint32_t blocks = 0, entries = 0;
        if (ext4_mounted()) { ext4_flush(); blocks = ext4_cache_trim(ext4_cache_bytes()); }
        entries = fs_unload_idle(0);
        char line[160];
        ksnprintf(line, sizeof(line), "drop_caches: %u KB of file data, %u KB of disk blocks, %u entries of folders not in use",
                  files / 1024u, blocks / 1024u, entries);
        terminal_writeln(line);
        return 1;
    }
    if (strcmp(argv[0], "apt") == 0 || strcmp(argv[0], "apt-get") == 0) { cmd_apt(argc, argv); return 1; }
    if (strcmp(argv[0], "adb") == 0) { cmd_adb(argc, argv); return 1; }
    if (strcmp(argv[0], "fastboot") == 0) { cmd_fastboot(argc, argv); return 1; }
    if (strcmp(argv[0], "pkg") == 0 && argc >= 2 &&
        (!strcmp(argv[1], "update") || !strcmp(argv[1], "upgrade") || !strcmp(argv[1], "search") ||
         !strcmp(argv[1], "show") || !strcmp(argv[1], "autoremove") || !strcmp(argv[1], "sources"))) { cmd_apt(argc, argv); return 1; }
    if (strcmp(argv[0], "pkg") == 0) { cmd_pkg(argc, argv); return 1; }
    if (strcmp(argv[0], "play") == 0) { cmd_play(argc, argv); return 1; }
    if (strcmp(argv[0], "beep") == 0) { cmd_beep(argc, argv); return 1; }
    if (strcmp(argv[0], "volume") == 0) { cmd_volume(argc, argv); return 1; }
    if (strcmp(argv[0], "lsaudio") == 0) { audio_list(); return 1; }
    if (strcmp(argv[0], "apps") == 0) {
        if (!gui_open_apps()) cmd_pkg(2, (char*[]){ "pkg", "list" });
        return 1;
    }
    if (strcmp(argv[0], "settings") == 0) {
        int apps = argc > 1 && strcmp(argv[1], "apps") == 0;           /* `settings apps [name]` */
        /* `settings time` / `settings keyboard`: that page */
        int page = argc > 1 && !strcmp(argv[1], "time") ? SETTINGS_PAGE_TIME :
                   argc > 1 && !strcmp(argv[1], "keyboard") ? SETTINGS_PAGE_KEYBOARD :
                   argc > 1 && !strcmp(argv[1], "themes") ? SETTINGS_PAGE_THEMES :
                   argc > 1 && !strcmp(argv[1], "about") ? SETTINGS_PAGE_ABOUT :
                   argc > 1 && !strcmp(argv[1], "wifi") ? SETTINGS_PAGE_WIFI :
                   argc > 1 && !strcmp(argv[1], "power") ? SETTINGS_PAGE_POWER : -1;
        if (argc > 1 && !apps && page < 0) { terminal_writeln("usage: settings [apps [name] | time | keyboard | themes | about | wifi | power]"); return 1; }
        if (!(apps ? gui_open_settings_app(argc > 2 ? argv[2] : NULL) : gui_open_settings())) terminal_writeln("settings: Settings is part of the desktop - run `startx` first");
        else if (page >= 0) settings_open_page(page);
        return 1;
    }
    if (strcmp(argv[0], "notify") == 0) {
        if (argc < 2 || argc > 3) terminal_writeln("usage: notify TITLE [BODY]");
        else if (notify_post("Shell", argv[1], argc == 3 ? argv[2] : "", -1) <= 0)
            terminal_writeln("notify: notification refused (rate limit)");
        return 1;
    }
    if (strcmp(argv[0], "taskmgr") == 0) {
        if (!gui_open_taskmgr()) terminal_writeln("taskmgr: the Task Manager is part of the desktop (startx) - try `top` here");
        return 1;
    }
    return 0;
}
