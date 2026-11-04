/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "moncmds.h"
#include "../kernel/latency.h"
#include "../kernel/fpu.h"
#include "netcmds.h"
#include "../kernel/terminal.h"
#include "../kernel/keyboard.h"
#include "../kernel/kstring.h"
#include "../kernel/fs.h"
#include "../kernel/fsdisk.h"
#include "../kernel/fat32.h"
#include "../kernel/meminfo.h"
#include "../kernel/zswap.h"
#include "../kernel/sysinfo.h"
#include "../kernel/sensors.h"
#include "../kernel/applesmc.h"
#include "../kernel/gamepad.h"
#include "../kernel/task.h"
#include "../kernel/app.h"
#include "../kernel/smp.h"
#include "../kernel/timer.h"
#include "../kernel/daemon.h"
#include "../kernel/gui.h"
#include "../kernel/kptest.h"

static int has_opt(int argc, char** argv, const char* opt) {
    for (int i = 1; i < argc; i++) if (strcmp(argv[i], opt) == 0) return 1;
    return 0;
}

/* 1.2G, 340M, 12K, 512B - like `df -h` */
static void human(uint64_t b, char* out, int cap) {
    static const char unit[] = "BKMGTP";
    int i = 0;
    uint64_t rem = 0;
    while (b >= 1024 && i < 5) { rem = b & 1023u; b >>= 10; i++; }
    if (i == 0) ksnprintf(out, (size_t)cap, "%uB", (uint32_t)b);
    else if (b < 10) ksnprintf(out, (size_t)cap, "%u.%u%c", (uint32_t)b, (uint32_t)(rem * 10u / 1024u), unit[i]);
    else ksnprintf(out, (size_t)cap, "%u%c", (uint32_t)b, unit[i]);
}

/* an amount in the unit asked for: shift 10 (KiB), 20 (MiB), 30 (GiB), or -1 (human) */
static void amount(uint64_t bytes, int shift, char* out, int cap) {
    if (shift < 0) human(bytes, out, cap);
    else ksnprintf(out, (size_t)cap, "%llu", (unsigned long long)(bytes >> shift));
}

/* ── free: where the RAM is ───────────────────────────────────────── */

static void cmd_free(int argc, char** argv) {
    int shift = -1;                                    /* -h: the default */
    if (has_opt(argc, argv, "-b")) shift = 0;
    if (has_opt(argc, argv, "-k")) shift = 10;
    if (has_opt(argc, argv, "-m")) shift = 20;
    if (has_opt(argc, argv, "-g")) shift = 30;
    if (has_opt(argc, argv, "--help")) {
        terminal_writeln("Usage: free [-h|-b|-k|-m|-g]   RAM in use and free (-h: 1.2G style, the default)");
        return;
    }
    meminfo_t m;
    meminfo_get(&m);
    char t[16], u[16], f[16], fi[16], a[16], line[128];
    amount((uint64_t)m.total_kb << 10, shift, t, sizeof(t));
    amount((uint64_t)m.used_kb << 10, shift, u, sizeof(u));
    amount((uint64_t)m.free_kb << 10, shift, f, sizeof(f));
    amount((uint64_t)m.files_kb << 10, shift, fi, sizeof(fi));
    amount((uint64_t)m.avail_kb << 10, shift, a, sizeof(a));
    terminal_write_color("               total        used        free       files   available\n",
                         VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    ksnprintf(line, sizeof(line), "Mem:    %12s%12s%12s%12s%12s", t, u, f, fi, a);
    terminal_writeln(line);
    /* the swap: app memory compressed in RAM (kernel/zswap.c) */
    zswap_stats_t z;
    zswap_stats(&z);
    amount((uint64_t)z.pages * 4096u, shift, t, sizeof(t));
    amount(z.bytes, shift, u, sizeof(u));
    char d[16], dt[16];
    amount((uint64_t)z.disk_pages * 4096u, shift, d, sizeof(d));
    amount(zswap_disk_bytes(), shift, dt, sizeof(dt));
    if (zswap_disk_bytes()) ksnprintf(line, sizeof(line), "Swap:   %12s compressed in %s of RAM, %s on the disk (of %s)%s", t, u, d, dt, zswap_enabled() ? "" : " (off)");
    else ksnprintf(line, sizeof(line), "Swap:   %12s compressed in %s of RAM%s", t, u, zswap_enabled() ? "" : " (off)");
    terminal_writeln(line);
    /* what the numbers mean, when the machine has RAM Banana OS leaves alone */
    char inst[16], tot[16], big[16];
    human((uint64_t)m.installed_kb << 10, inst, sizeof(inst));
    human((uint64_t)m.total_kb << 10, tot, sizeof(tot));
    human((uint64_t)m.largest_kb << 10, big, sizeof(big));
    ksnprintf(line, sizeof(line), "Installed: %s - Banana OS runs in %s of it (largest free block %s)", inst, tot, big);
    terminal_write_color(line, VGA_COLOR_DARK_GREY, VGA_COLOR_BLACK);
    terminal_putchar('\n');
}

/* ── swap: compressed memory (kernel/zswap.c) ───────────────────── */

static void cmd_swap(int argc, char** argv) {
    char line[160], a[16], b[16];
    if (argc > 1 && !strcmp(argv[1], "on")) zswap_enable(1);
    else if (argc > 1 && !strcmp(argv[1], "off")) zswap_enable(0);
    else if (argc > 1 && !strcmp(argv[1], "now")) {
        uint32_t n = zswap_compress_now();
        ksnprintf(line, sizeof(line), "swap: %u pages of app memory compressed (%u KiB)", n, n * 4u);
        terminal_writeln(line);
    } else if (argc > 1 && !strcmp(argv[1], "disk")) {
        if (!zswap_disk_bytes()) { terminal_writeln("swap: no swap area (an installed system has one on its disk)"); return; }
        uint32_t n = zswap_writeback(0xFFFFFFFFu);
        ksnprintf(line, sizeof(line), "swap: %u compressed pages written to the disk", n);
        terminal_writeln(line);
    } else if (argc > 1) {
        terminal_writeln("usage: swap [on | off | now | disk]   (no argument: what is compressed, what is on the disk)");
        return;
    }
    zswap_stats_t z;
    zswap_stats(&z);
    human((uint64_t)z.pages * 4096u, a, sizeof(a));
    human(z.bytes, b, sizeof(b));
    ksnprintf(line, sizeof(line), "Compressed memory: %s - %s of app memory (%u pages, %u of them zeros) in %s of RAM",
              zswap_enabled() ? "on" : "off", a, z.pages, z.zero_pages, b);
    terminal_writeln(line);
    uint64_t ratio10 = z.bytes ? (uint64_t)z.pages * 40960u / z.bytes : 0;
    if (z.bytes) ksnprintf(line, sizeof(line), "  %u.%ux smaller; since boot %llu pages compressed, %llu brought back", (uint32_t)(ratio10 / 10), (uint32_t)(ratio10 % 10),
                           (unsigned long long)z.pages_out, (unsigned long long)z.pages_in);
    else ksnprintf(line, sizeof(line), "  since boot %llu pages compressed, %llu brought back", (unsigned long long)z.pages_out, (unsigned long long)z.pages_in);
    terminal_writeln(line);
    if (zswap_disk_bytes()) {
        human(z.disk_used, a, sizeof(a));
        human(zswap_disk_bytes(), b, sizeof(b));
        ksnprintf(line, sizeof(line), "Swap area on the disk: %s used of %s; since boot %llu pages written, %llu read back", a, b,
                  (unsigned long long)z.disk_out, (unsigned long long)z.disk_in);
    } else ksnprintf(line, sizeof(line), "Swap area on the disk: none (the live CD: an installed system has one)");
    terminal_writeln(line);
    terminal_writeln("  (apps' pages not used lately are compressed when RAM runs low, brought back when touched;");
    terminal_writeln("   the oldest compressed ones go to the disk when that is not enough)");
}

/* ── df: where the disk space is ──────────────────────────────────── */

typedef struct {
    char     fs[16];
    uint64_t size, used, avail;
    char     mount[FS_PATH_LEN];
} df_row_t;

static int df_rows(df_row_t* r, int max) {
    int n = 0;
    meminfo_t m;
    meminfo_get(&m);
    if (fsdisk_is_installed()) {
        fsdisk_space(&r[n].used, &r[n].size);
        kstrlcpy(r[n].fs, fsdisk_fs_type(), sizeof(r[n].fs));
    } else {
        /* the live CD (or not installed): the files live in RAM */
        r[n].size = (uint64_t)m.total_kb << 10;
        r[n].used = (uint64_t)m.files_kb << 10;
        kstrlcpy(r[n].fs, "ramfs", sizeof(r[n].fs));
    }
    r[n].avail = r[n].size > r[n].used ? r[n].size - r[n].used : 0;
    if (!fsdisk_is_installed()) r[n].avail = (uint64_t)m.free_kb << 10;
    kstrlcpy(r[n].mount, "/", sizeof(r[n].mount));
    n++;
    for (int mnt = 1; mnt <= FS_MAX_MOUNTS && n < max; mnt++) {
        const char* point = fs_mount_point(mnt);
        uint64_t tot, fr;
        if (!point[0] || !volume_space(mnt, &tot, &fr)) continue;
        kstrlcpy(r[n].fs, volume_fs_name(mnt), sizeof(r[n].fs));
        r[n].size = tot;
        r[n].avail = fr;
        r[n].used = tot > fr ? tot - fr : 0;
        kstrlcpy(r[n].mount, point, sizeof(r[n].mount));
        n++;
    }
    return n;
}

static void cmd_df(int argc, char** argv) {
    if (has_opt(argc, argv, "--help")) {
        terminal_writeln("Usage: df [-h|-k|-m] [-i]   disk space: size, used, free (-h: 1.2G style, the default)");
        terminal_writeln("                            -i: files and folders (inodes) instead of bytes");
        return;
    }
    char line[160];
    if (has_opt(argc, argv, "-i")) {
        uint32_t max = FS_MAX_FILES + FS_MAX_DIRS, used = fs_used_files() + fs_used_dirs();
        terminal_write_color("Filesystem     Inodes   IUsed   IFree IUse% Mounted on\n", VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
        ksnprintf(line, sizeof(line), "%-12s %8u %7u %7u %4u%% /", fsdisk_fs_type(),
                  max, used, max - used, (used * 100u + max - 1u) / max);
        terminal_writeln(line);
        ksnprintf(line, sizeof(line), "(up to %u files and %u folders; %u files and %u folders now)",
                  (uint32_t)FS_MAX_FILES, (uint32_t)FS_MAX_DIRS, fs_used_files(), fs_used_dirs());
        terminal_write_color(line, VGA_COLOR_DARK_GREY, VGA_COLOR_BLACK);
        terminal_putchar('\n');
        return;
    }
    int shift = -1;
    if (has_opt(argc, argv, "-k")) shift = 10;
    if (has_opt(argc, argv, "-m")) shift = 20;
    df_row_t rows[FS_MAX_MOUNTS + 1];
    int n = df_rows(rows, FS_MAX_MOUNTS + 1);
    terminal_write_color(shift == 10 ? "Filesystem     1K-blocks       Used  Available Use% Mounted on\n"
                         : shift == 20 ? "Filesystem     1M-blocks       Used  Available Use% Mounted on\n"
                         : "Filesystem      Size  Used Avail Use% Mounted on\n", VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    for (int i = 0; i < n; i++) {
        char s[24], u[24], a[24];
        amount(rows[i].size, shift, s, sizeof(s));
        amount(rows[i].used, shift, u, sizeof(u));
        amount(rows[i].avail, shift, a, sizeof(a));
        uint32_t pct = rows[i].size ? (uint32_t)((rows[i].used * 100u + rows[i].size - 1u) / rows[i].size) : 0;
        if (shift < 0) ksnprintf(line, sizeof(line), "%-14s %5s %5s %5s %3u%% %s", rows[i].fs, s, u, a, pct, rows[i].mount);
        else ksnprintf(line, sizeof(line), "%-14s %9s %10s %10s %3u%% %s", rows[i].fs, s, u, a, pct, rows[i].mount);
        terminal_write_color(line, pct >= 90 ? VGA_COLOR_LIGHT_RED : VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
        terminal_putchar('\n');
    }
    if (fsdisk_is_installed()) {
        char d[96];
        fsdisk_describe(d, sizeof(d));
        ksnprintf(line, sizeof(line), "/ is on %s", d);
    } else {
        ksnprintf(line, sizeof(line), "/ is in RAM (not installed: the files are lost at shutdown - `install` keeps them)");
    }
    terminal_write_color(line, VGA_COLOR_DARK_GREY, VGA_COLOR_BLACK);
    terminal_putchar('\n');
}

/* ── htop: top in colour ──────────────────────────────────────────── */

static void put_n(char c, int n, uint8_t fg, uint8_t bg) {
    char s[2] = { c, 0 };
    for (int i = 0; i < n; i++) terminal_write_color(s, fg, bg);
}

static uint8_t level_color(uint32_t pct) {
    return pct >= 85 ? VGA_COLOR_LIGHT_RED : pct >= 60 ? VGA_COLOR_YELLOW : VGA_COLOR_LIGHT_GREEN;
}

/* "  0[|||||||       23%]": a meter `w` wide, two coloured parts (a then b,
 * out of total) and a text at its right end */
static void meter(const char* label, uint64_t a, uint8_t ca, uint64_t b, uint8_t cb,
                  uint64_t total, const char* text, int w) {
    terminal_write_color(label, VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    terminal_write_color("[", VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    int inner = w - (int)strlen(label) - 2;
    if (inner < 4) inner = 4;
    int na = total ? (int)((a * (uint64_t)inner) / total) : 0;
    int nb = total ? (int)(((a + b) * (uint64_t)inner) / total) - na : 0;
    if (na > inner) na = inner;
    if (na + nb > inner) nb = inner - na;
    int tl = (int)strlen(text), tstart = inner - tl;
    for (int i = 0; i < inner; i++) {
        char s[2] = { ' ', 0 };
        uint8_t fg = VGA_COLOR_LIGHT_GREY;
        if (i >= tstart && i - tstart < tl) { s[0] = text[i - tstart]; fg = VGA_COLOR_LIGHT_GREY; }
        else if (i < na) { s[0] = '|'; fg = ca; }
        else if (i < na + nb) { s[0] = '|'; fg = cb; }
        terminal_write_color(s, fg, VGA_COLOR_BLACK);
    }
    terminal_write_color("]", VGA_COLOR_WHITE, VGA_COLOR_BLACK);
}

/* the text of the right-hand column, line by line */
static void info_line(int k, char* out, int cap, int ntasks, int nrun) {
    out[0] = 0;
    uint32_t s = timer_ms() / 1000u;
    if (k == 0) ksnprintf(out, (size_t)cap, "Tasks: %d, %d running", ntasks, nrun);
    else if (k == 1) {
        uint32_t la[3];
        task_loadavg(la);
        ksnprintf(out, (size_t)cap, "Cores: %d, load average: %u.%02u %u.%02u %u.%02u", cpu_count(),
                  la[0] / 100, la[0] % 100, la[1] / 100, la[1] % 100, la[2] / 100, la[2] % 100);
    }
    else if (k == 2) ksnprintf(out, (size_t)cap, "Uptime: %02u:%02u:%02u", s / 3600u, (s / 60u) % 60u, s % 60u);
    else if (k == 3) ksnprintf(out, (size_t)cap, "Files: %u, folders: %u", fs_used_files(), fs_used_dirs());
}

enum { SORT_CPU, SORT_PID, SORT_TIME };

static int before(const task_info_t* a, const task_info_t* b, int key) {
    if (key == SORT_PID) return a->pid < b->pid;
    if (key == SORT_TIME) return a->ticks_total > b->ticks_total;
    if (a->cpu_pct != b->cpu_pct) return a->cpu_pct > b->cpu_pct;
    return a->ticks_total > b->ticks_total;
}

static void htop_draw(int sort) {
    int W = (int)terminal_get_width(), H = (int)terminal_get_height();
    if (W < 40) W = 40;
    if (W > 160) W = 160;
    static task_info_t t[TASK_MAX];
    int n = task_count();
    if (n > TASK_MAX) n = TASK_MAX;
    task_snapshot(t, n);
    int nrun = 0;
    for (int i = 0; i < n; i++) if (t[i].state == TASK_RUNNING || t[i].state == TASK_AWAY || t[i].state == TASK_READY) nrun++;
    for (int i = 1; i < n; i++) {                       /* insertion sort: a few dozen tasks */
        task_info_t x = t[i];
        int j = i - 1;
        while (j >= 0 && before(&x, &t[j], sort)) { t[j + 1] = t[j]; j--; }
        t[j + 1] = x;
    }

    terminal_clear();
    int two = W >= 70;                                  /* meters left, info right */
    int mw = two ? W / 2 - 1 : W - 1;
    int line = 0, info = 0;
    char lab[16], txt[32], right[64];
    meminfo_t m;
    meminfo_get(&m);

    /* one meter per core */
    for (int c = 0; c < cpu_count(); c++, line++) {
        uint32_t p = task_core_pct(c);
        ksnprintf(lab, sizeof(lab), "%3d", c);
        ksnprintf(txt, sizeof(txt), "%u.0%%", p);
        meter(lab, p, level_color(p), 0, 0, 100, txt, mw);
        if (two) { info_line(info++, right, sizeof(right), n, nrun); terminal_write("  "); terminal_write(right); }
        terminal_putchar('\n');
    }
    /* memory: in use (green) and file data (blue), of what Banana OS runs in */
    char a[16], b[16];
    human((uint64_t)m.used_kb << 10, a, sizeof(a));
    human((uint64_t)m.total_kb << 10, b, sizeof(b));
    ksnprintf(txt, sizeof(txt), "%s/%s", a, b);
    uint32_t files = m.files_kb < m.used_kb ? m.files_kb : m.used_kb;
    meter("Mem", m.used_kb - files, VGA_COLOR_LIGHT_GREEN, files, VGA_COLOR_LIGHT_BLUE, m.total_kb, txt, mw);
    if (two) { info_line(info++, right, sizeof(right), n, nrun); terminal_write("  "); terminal_write(right); }
    terminal_putchar('\n');
    line++;
    /* the disk the files are saved on (or the RAM they live in) */
    uint64_t du, dc;
    if (fsdisk_is_installed()) fsdisk_space(&du, &dc);
    else { du = (uint64_t)m.files_kb << 10; dc = (uint64_t)m.total_kb << 10; }
    human(du, a, sizeof(a));
    human(dc, b, sizeof(b));
    ksnprintf(txt, sizeof(txt), "%s/%s", a, b);
    meter("Dsk", du, VGA_COLOR_LIGHT_MAGENTA, 0, 0, dc ? dc : 1, txt, mw);
    if (two) { info_line(info++, right, sizeof(right), n, nrun); terminal_write("  "); terminal_write(right); }
    terminal_putchar('\n');
    line++;
    while (two && info < 4) {                           /* the rest of the right column */
        info_line(info++, right, sizeof(right), n, nrun);
        put_n(' ', mw + 2, VGA_COLOR_BLACK, VGA_COLOR_BLACK);
        terminal_writeln(right);
        line++;
    }
    if (!two) {
        for (int k = 0; k < 4; k++, line++) {
            info_line(k, right, sizeof(right), n, nrun);
            terminal_writeln(right);
        }
    }
    terminal_putchar('\n');
    line++;

    /* the task list */
    char row[192];
    ksnprintf(row, sizeof(row), "%5s %3s %3s S %5s %4s %9s  %s", "PID", "PRI", "NI", "CPU%", "CORE", "TIME+", "Command");
    int l = (int)strlen(row);
    if (l < W - 1) { memset(row + l, ' ', (size_t)(W - 1 - l)); row[W - 1] = 0; }
    terminal_write_color(row, VGA_COLOR_BLACK, VGA_COLOR_LIGHT_GREEN);
    terminal_putchar('\n');
    line++;
    int rows = H - line - 2;
    for (int i = 0; i < n && i < rows; i++) {
        const task_info_t* x = &t[i];
        int ni = x->nice, pri = 20 + ni;
        char st = x->state == TASK_SLEEPING ? 'S' : 'R';
        char core[8];
        if (x->state == TASK_AWAY) ksnprintf(core, sizeof(core), x->cpu > 0 ? "%d" : "-", x->cpu);
        else if (x->state == TASK_RUNNING) kstrlcpy(core, "0", sizeof(core));
        else kstrlcpy(core, "-", sizeof(core));
        uint32_t ti = x->ticks_total;                   /* 10 ms ticks */
        char tm[16];
        ksnprintf(tm, sizeof(tm), "%u:%02u.%02u", ti / 6000u, (ti / 100u) % 60u, ti % 100u);
        ksnprintf(row, sizeof(row), "%5u %3d %3d ", x->pid, pri, ni);
        terminal_write(row);
        char sb[2] = { st, 0 };
        terminal_write_color(sb, st == 'R' ? VGA_COLOR_LIGHT_GREEN : VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
        ksnprintf(row, sizeof(row), " %5u", x->cpu_pct);
        terminal_write_color(row, x->cpu_pct >= 50 ? VGA_COLOR_LIGHT_RED : x->cpu_pct ? VGA_COLOR_WHITE : VGA_COLOR_LIGHT_GREY,
                             VGA_COLOR_BLACK);
        ksnprintf(row, sizeof(row), " %4s %9s  ", core, tm);
        terminal_write(row);
        terminal_write_color(x->name, x->prio == TASK_PRIO_BACKGROUND ? VGA_COLOR_LIGHT_GREY : VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        terminal_putchar('\n');
    }

    /* the keys, htop style */
    static const char* const keys[][2] = { { "q", "Quit" }, { "P", "CPU%" }, { "N", "PID" }, { "T", "TIME+" } };
    for (int k = 0; k < 4; k++) {
        terminal_write_color(keys[k][0], VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        int sel = (k == 1 && sort == SORT_CPU) || (k == 2 && sort == SORT_PID) || (k == 3 && sort == SORT_TIME);
        ksnprintf(row, sizeof(row), "%-6s", keys[k][1]);
        terminal_write_color(row, VGA_COLOR_BLACK, sel ? VGA_COLOR_LIGHT_GREEN : VGA_COLOR_CYAN);
    }
}

static void cmd_htop(void) {
    int my_vt = terminal_vt_get_active();
    int sort = SORT_CPU;
    for (;;) {
        daemon_poll(0);
        htop_draw(sort);
        for (int i = 0; i < 20; i++) {                  /* a second, keys looked at every 50 ms */
            gui_poll();
            terminal_vt_set_active(my_vt);
            char c = (gui_focused_vt() == my_vt) ? keyboard_try_getchar() : 0;
            if (c == 'q' || c == 'Q' || c == 3) { terminal_clear(); return; }
            int ns = c == 'P' || c == 'p' ? SORT_CPU : c == 'N' || c == 'n' ? SORT_PID : c == 'T' || c == 't' ? SORT_TIME : -1;
            if (ns >= 0) { sort = ns; break; }
            task_sleep_ms(50);
        }
    }
}

/* gamepad: the game controllers plugged in and what they are doing */
static void cmd_gamepad(void) {
    static const char* NAMES[17] = { "A", "B", "X", "Y", "LB", "RB", "Back", "Start", "LS", "RS",
                                     "Up", "Down", "Left", "Right", "Guide", "LT", "RT" };
    char row[200];
    int any = 0;
    for (int i = 0; i < GP_MAX; i++) {
        gamepad_t g;
        if (!gamepad_get(i, &g)) continue;
        any = 1;
        ksnprintf(row, sizeof(row), "%d: %s\n   left %d,%d  right %d,%d  triggers %d %d\n   buttons:",
                  i, g.name, g.lx, g.ly, g.rx, g.ry, g.lt, g.rt);
        terminal_write(row);
        for (int b = 0; b < 17; b++) if (g.buttons & (1u << b)) { terminal_write(" "); terminal_write(NAMES[b]); }
        terminal_write(g.buttons ? "\n" : " none\n");
    }
    if (!any) terminal_write("No game controller plugged in.\n");
}

/* sensors: the processor's temperature */
static void cmd_sensors(void) {
    char row[128];
    const char* src = sensors_cpu_source();
    int t = sensors_cpu_temp();
    if (applesmc_present()) {                       /* a Mac: its SMC's sensors and fans */
        static const struct { const char* key; const char* name; } T[] = {
            { "TC0P", "CPU proximity" }, { "TC0D", "CPU die" }, { "TC0E", "CPU" }, { "TC0F", "CPU" },
            { "TG0P", "GPU proximity" }, { "TG0D", "GPU die" }, { "Th0H", "heatsink" }, { "Ts0P", "palm rest" },
            { "TB0T", "battery" }, { "TM0P", "memory" },
        };
        terminal_write("Apple SMC:\n");
        for (unsigned i = 0; i < sizeof(T) / sizeof(T[0]); i++) {
            int c = applesmc_temp(T[i].key);
            if (c == APPLESMC_NONE) continue;
            ksnprintf(row, sizeof(row), "  %-16s %d C  (%s)\n", T[i].name, c, T[i].key);
            terminal_write(row);
        }
        int rpm[8];
        int nf = applesmc_fans(rpm, 8);
        for (int i = 0; i < nf; i++) {
            ksnprintf(row, sizeof(row), "  fan %d           %d RPM\n", i + 1, rpm[i]);
            terminal_write(row);
        }
        if (!src || t == SENSOR_NONE) return;
    }
    if (!src || t == SENSOR_NONE) {
        terminal_write("No temperature sensor this kernel can read (a virtual machine, or an unsupported processor).\n");
        return;
    }
    int tj = sensors_cpu_tjmax();
    if (tj) ksnprintf(row, sizeof(row), "CPU temperature:  %d C  (throttles at %d C)\n", t, tj);
    else ksnprintf(row, sizeof(row), "CPU temperature:  %d C\n", t);
    terminal_write(row);
    ksnprintf(row, sizeof(row), "Sensor:           %s\n", src);
    terminal_write(row);
}

/* lscpu: the processors, their cores and hardware threads */
static void cmd_lscpu(void) {
    const sysinfo_t* si = sysinfo_get();
    char row[160];
    int n = cpu_count(), tpc = smp_threads_per_core(), np = smp_phys_cores();
    int perf = 0, eff = 0, pkgs = 0;
    uint32_t seen_pkg[SMP_MAX_CPUS];
    for (int i = 0; i < n; i++) {
        const cpu_topo_t* t = cpu_topo(i);
        if (t->type == CPU_TYPE_PERF) perf++;
        if (t->type == CPU_TYPE_EFF) eff++;
        int k;
        for (k = 0; k < pkgs; k++) if (seen_pkg[k] == t->pkg) break;
        if (k == pkgs) seen_pkg[pkgs++] = t->pkg;
    }
    ksnprintf(row, sizeof(row), "Processor:            %s\n", si->cpu_brand[0] ? si->cpu_brand : si->cpu_vendor);
    terminal_write(row);
    ksnprintf(row, sizeof(row), "Logical processors:   %d running (%d listed by the firmware)\n", n, smp_cores_found());
    terminal_write(row);
    ksnprintf(row, sizeof(row), "Physical cores:       %d in %d package%s\n", np, pkgs ? pkgs : 1, pkgs > 1 ? "s" : "");
    terminal_write(row);
    ksnprintf(row, sizeof(row), "Threads per core:     %d%s\n", tpc, tpc > 1 ? " (Hyper-Threading / SMT on)" : "");
    terminal_write(row);
    if (perf || eff) {
        ksnprintf(row, sizeof(row), "Hybrid:               %d on performance cores, %d on efficient cores\n", perf, eff);
        terminal_write(row);
    }
    ksnprintf(row, sizeof(row), "Interrupt controller: %s\n", n > 1 ? (smp_x2apic() ? "x2APIC" : "xAPIC") : "8259 (one processor)");
    terminal_write(row);
    char fp[64];
    fpu_describe(fp, sizeof(fp));
    ksnprintf(row, sizeof(row), "Registers for apps:   %s\n", fp);
    terminal_write(row);
    if (n < 2 && smp_cores_found() < 2) return;
    ksnprintf(row, sizeof(row), "%4s %6s %4s %5s %6s %5s %5s", "CPU", "APIC", "PKG", "CORE", "THREAD", "TYPE", "BUSY");
    terminal_write_color(row, VGA_COLOR_BLACK, VGA_COLOR_LIGHT_GREEN);
    terminal_putchar('\n');
    for (int i = 0; i < n; i++) {
        const cpu_topo_t* t = cpu_topo(i);
        ksnprintf(row, sizeof(row), "%4d %6u %4u %5d %6u %5s %4u%%%s\n", i, t->apic, t->pkg, t->phys, t->smt,
                  t->type == CPU_TYPE_PERF ? "P" : t->type == CPU_TYPE_EFF ? "E" : "-", task_core_pct(i),
                  i == 0 ? "  (kernel)" : "");
        terminal_write(row);
    }
}

/* sched: what the scheduler sees, task by task */
/* sched calls [reset]: the system calls app threads made on the other
 * cores - done there, or sent to the boot core (each costs two switches
 * and a wait for the boot core) */
static void cmd_sched_calls(int reset) {
#ifndef __x86_64__
    (void)reset;
    terminal_writeln("sched calls: apps run on the other cores only on the 64-bit system");
    return;
#else
    static app_call_stat_t st[256];
    int n = app_call_stats(st, 256, reset);
    char row[128];
    ksnprintf(row, sizeof(row), "%-16s %10s %10s", "CALL", "ON ITS CORE", "BOOT CORE");
    terminal_write_color(row, VGA_COLOR_BLACK, VGA_COLOR_LIGHT_GREEN);
    terminal_putchar('\n');
    uint64_t here = 0, home = 0;
    for (int i = 0; i < n && i < 256; i++) {
        ksnprintf(row, sizeof(row), "%-16s %10u %10u\n", st[i].name, st[i].here, st[i].home);
        terminal_write(row);
        here += st[i].here; home += st[i].home;
    }
    ksnprintf(row, sizeof(row), "%-16s %10llu %10llu%s\n", "total", (unsigned long long)here, (unsigned long long)home,
              reset ? "   (counts reset)" : "");
    terminal_write(row);
#endif
}

static void cmd_sched(void) {
    static task_info_t t[TASK_MAX];
    int n = task_count();
    if (n > TASK_MAX) n = TASK_MAX;
    task_snapshot(t, n);
    uint32_t la[3];
    task_loadavg(la);
    int nrun = 0;
    for (int i = 0; i < n; i++) if (t[i].state == TASK_RUNNING || t[i].state == TASK_READY || t[i].state == TASK_AWAY) nrun++;
    char row[160];
    ksnprintf(row, sizeof(row), "Load average: %u.%02u %u.%02u %u.%02u (1, 5, 15 min)   %d tasks, %d wanting a processor, %d core%s\n",
              la[0] / 100, la[0] % 100, la[1] / 100, la[1] % 100, la[2] / 100, la[2] % 100, n, nrun, cpu_count(),
              cpu_count() == 1 ? "" : "s");
    terminal_write(row);
    ksnprintf(row, sizeof(row), "%4s %3s %6s %-6s %4s %8s %8s %8s %8s %7s  %s", "PID", "NI", "WEIGHT", "STATE", "CPU%",
              "GAVE UP", "PREEMPT", "WAIT avg", "WAIT max", "LONGEST", "NAME");
    terminal_write_color(row, VGA_COLOR_BLACK, VGA_COLOR_LIGHT_GREEN);
    terminal_putchar('\n');
    for (int i = 0; i < n; i++) {
        const task_info_t* x = &t[i];
        if (x->state == TASK_UNUSED) continue;
        char st[12];
        if (x->state == TASK_AWAY && x->cpu > 0) ksnprintf(st, sizeof(st), "core%d", x->cpu);
        else kstrlcpy(st, task_state_str(x->state), sizeof(st));
        char avg[16], mx[16];
        if (x->lat_avg_us >= 10000) ksnprintf(avg, sizeof(avg), "%ums", x->lat_avg_us / 1000);
        else ksnprintf(avg, sizeof(avg), "%uus", x->lat_avg_us);
        if (x->lat_max_us >= 10000) ksnprintf(mx, sizeof(mx), "%ums", x->lat_max_us / 1000);
        else ksnprintf(mx, sizeof(mx), "%uus", x->lat_max_us);
        ksnprintf(row, sizeof(row), "%4u %3d %6u %-6s %4u %8u %8u %8s %8s %5ums  %s\n", x->pid, x->nice, x->weight, st,
                  x->cpu_pct, x->vcsw, x->icsw, avg, mx, x->longest_ms, x->name);
        terminal_write_color(row, x->longest_ms >= 500 ? VGA_COLOR_LIGHT_RED : VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    }
    uint32_t kp = 0;
    for (int i = 0; i < n; i++) kp += t[i].kpreempts;
    ksnprintf(row, sizeof(row), "Preempted in kernel code (image / SVG decoding): %u times\n", kp);
    terminal_write(row);
    terminal_writeln("GAVE UP: it slept / waited / yielded;  PREEMPT: its slice ended or a woken task took over;");
    terminal_writeln("WAIT: from ready to running;  LONGEST: its longest run without letting other tasks in.");
}

/* renice N pid...: another share of the processor for running tasks */
static void cmd_renice(int argc, char** argv) {
    int i = 1;
    if (i < argc && strcmp(argv[i], "-n") == 0) i++;
    if (i >= argc) { terminal_writeln("usage: renice [-n] N pid...   (N: -20 most processor time .. 19 least)"); return; }
    const char* v = argv[i++];
    int neg = *v == '-';
    if (*v == '-' || *v == '+') v++;
    if (*v < '0' || *v > '9' || i >= argc) { terminal_writeln("usage: renice [-n] N pid...   (N: -20 most processor time .. 19 least)"); return; }
    int nice = 0;
    while (*v >= '0' && *v <= '9') nice = nice * 10 + (*v++ - '0');
    if (neg) nice = -nice;
    for (; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0) continue;
        uint32_t pid;
        char b[96];
        if (!k_parse_u32(argv[i], &pid)) { ksnprintf(b, sizeof(b), "renice: %s: not a process number", argv[i]); terminal_writeln(b); continue; }
        int old = task_get_nice((int)pid);
        if (task_set_nice((int)pid, nice) < 0) { ksnprintf(b, sizeof(b), "renice: %u: no such task", pid); terminal_writeln(b); continue; }
        ksnprintf(b, sizeof(b), "%u (process ID) old priority %d, new priority %d", pid, old, task_get_nice((int)pid));
        terminal_writeln(b);
    }
}

int moncmd_dispatch(const char* line) {
    char buf[256];
    kstrlcpy(buf, line, sizeof(buf));
    char* argv[16];
    int argc = shell_split_args(buf, argv, 16);
    if (argc == 0) return 0;
    if (strcmp(argv[0], "latency") == 0) {
        if (argc == 2 && !strcmp(argv[1], "on")) latency_set(1);
        else if (argc == 2 && !strcmp(argv[1], "off")) latency_set(0);
        else if (argc == 1 || (argc == 2 && !strcmp(argv[1], "status"))) latency_report();
        else { terminal_writeln("usage: latency [on | off | status]"); return 1; }
        terminal_writeln("latency: sample written to serial log");
        return 1;
    }
    if (strcmp(argv[0], "free") == 0) { cmd_free(argc, argv); return 1; }
    if (strcmp(argv[0], "swap") == 0) { cmd_swap(argc, argv); return 1; }
    if (strcmp(argv[0], "df") == 0) { cmd_df(argc, argv); return 1; }
    if (strcmp(argv[0], "htop") == 0) { cmd_htop(); return 1; }
    if (strcmp(argv[0], "sched") == 0) {
        if (argc >= 2 && !strcmp(argv[1], "calls")) cmd_sched_calls(argc >= 3 && !strcmp(argv[2], "reset"));
        else cmd_sched();
        return 1;
    }
    if (strcmp(argv[0], "lscpu") == 0) { cmd_lscpu(); return 1; }
    if (strcmp(argv[0], "sensors") == 0) { cmd_sensors(); return 1; }
    if (strcmp(argv[0], "gamepad") == 0) { cmd_gamepad(); return 1; }
    if (strcmp(argv[0], "kptest") == 0) {
        if (argc < 3) { terminal_writeln("usage: kptest <picture.png|jpg|gif|bmp> <drawing.svg> [seconds]   (checks kernel preemption)"); return 1; }
        char rep[400];
        int secs = 10;
        if (argc > 3) { uint32_t v; if (k_parse_u32(argv[3], &v) && v > 0 && v < 600) secs = (int)v; }
        kptest_run(argv[1], argv[2], secs, rep, sizeof(rep));
        terminal_writeln(rep);
        return 1;
    }
    if (strcmp(argv[0], "renice") == 0) { cmd_renice(argc, argv); return 1; }
    return 0;
}
