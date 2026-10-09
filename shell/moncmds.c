#include "moncmds.h"
#include "netcmds.h"
#include "../kernel/terminal.h"
#include "../kernel/keyboard.h"
#include "../kernel/kstring.h"
#include "../kernel/fs.h"
#include "../kernel/fsdisk.h"
#include "../kernel/fat32.h"
#include "../kernel/meminfo.h"
#include "../kernel/sysinfo.h"
#include "../kernel/task.h"
#include "../kernel/smp.h"
#include "../kernel/timer.h"
#include "../kernel/daemon.h"
#include "../kernel/gui.h"

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
    amount((uint64_t)m.free_kb << 10, shift, a, sizeof(a));
    terminal_write_color("               total        used        free       files   available\n",
                         VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    ksnprintf(line, sizeof(line), "Mem:    %12s%12s%12s%12s%12s", t, u, f, fi, a);
    terminal_writeln(line);
    amount(0, shift, t, sizeof(t));
    ksnprintf(line, sizeof(line), "Swap:   %12s%12s%12s", t, t, t);
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
        kstrlcpy(r[n].fs, "bananafs", sizeof(r[n].fs));
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
        if (!point[0] || !fat32_space(mnt, &tot, &fr)) continue;
        kstrlcpy(r[n].fs, "fat32", sizeof(r[n].fs));
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
        ksnprintf(line, sizeof(line), "%-12s %8u %7u %7u %4u%% /", fsdisk_is_installed() ? "bananafs" : "ramfs",
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
    else if (k == 1) ksnprintf(out, (size_t)cap, "Cores: %d", cpu_count());
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
        int pri = x->prio == TASK_PRIO_HIGH ? 15 : x->prio == TASK_PRIO_BACKGROUND ? 30 : 20;
        int ni = pri - 20;
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

int moncmd_dispatch(const char* line) {
    char buf[256];
    kstrlcpy(buf, line, sizeof(buf));
    char* argv[16];
    int argc = shell_split_args(buf, argv, 16);
    if (argc == 0) return 0;
    if (strcmp(argv[0], "free") == 0) { cmd_free(argc, argv); return 1; }
    if (strcmp(argv[0], "df") == 0) { cmd_df(argc, argv); return 1; }
    if (strcmp(argv[0], "htop") == 0) { cmd_htop(); return 1; }
    return 0;
}
