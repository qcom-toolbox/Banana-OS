#include "taskmgr.h"
#include "gfx.h"
#include "kstring.h"
#include "kheap.h"
#include "timer.h"
#include "task.h"
#include "smp.h"
#include "fs.h"
#include "winframe.h"
#include "ctxmenu.h"
#include "app.h"
#include "gui.h"
#include "audio.h"
#include "../net/net.h"

#define TITLE_H  20
#define TABS_Y   24
#define TAB_H    20
#define BODY_Y   (TABS_Y + TAB_H + 6)
#define ROW_H    14
#define HIST     60
#define MAX_ROWS 40

#define C_PANEL  0x001D232Cu
#define C_TITLE  0x00384562u
#define C_TEXT   0x00E8EEF6u
#define C_DIM    0x00AAB6C6u
#define C_LIST   0x00141920u
#define C_SEL    0x003A5A8Au
#define C_CPU    0x0057B65Au
#define C_MEM    0x003A7BD5u

enum { TAB_APPS = 0, TAB_PROCS, TAB_PERF, TAB_COUNT };
static const char* const TAB_NAMES[TAB_COUNT] = { "Apps & windows", "Processes", "Performance" };

/* a row of the Apps tab: a window, or a running app */
typedef struct { int is_app; int handle; char text[64]; char info[40]; } row_t;

static int        g_open;
static win_geom_t g_win = { .x = 130, .y = 60, .w = 560, .h = 420, .min_w = 420, .min_h = 300 };
static int        g_tab;
static int        g_sel = -1;
static uint32_t   g_gen;
static uint32_t   g_sample_ms;
static uint8_t    g_cpu_hist[HIST], g_mem_hist[HIST];
static int        g_hist_n;
static row_t      g_rows[MAX_ROWS];
static int        g_nrows;
static int        g_menu_row = -1;
static char       g_status[96];

static void bevel(int x, int y, int w, int h, uint32_t base, uint32_t hi, uint32_t lo) {
    gfx_fill_rect(x, y, w, h, base);
    gfx_fill_rect(x, y, w, 1, hi);
    gfx_fill_rect(x, y, 1, h, hi);
    gfx_fill_rect(x, y + h - 1, w, 1, lo);
    gfx_fill_rect(x + w - 1, y, 1, h, lo);
}

#include "kbnav.h"
static kbnav_t g_nav;

static void button(int x, int y, int w, const char* label, int enabled) {
    kbnav_add(&g_nav, x, y, w, 18);
    uint32_t base = enabled ? 0x00303740u : 0x00262B33u;
    bevel(x, y, w, 18, base, 0x00535D6Eu, 0x0015191Fu);
    gfx_draw_text(x + (w - (int)strlen(label) * 8) / 2, y + 5, label, enabled ? C_TEXT : 0x00707A88u, base);
}

static int inside(int mx, int my, int x, int y, int w, int h) {
    return mx >= x && mx < x + w && my >= y && my < y + h;
}

static uint32_t cpu_now(void) {
    static task_info_t t[TASK_MAX];
    int n = task_count();
    if (n > TASK_MAX) n = TASK_MAX;
    task_snapshot(t, n);
    uint32_t busy = 0;
    for (int i = 0; i < n; i++) if (t[i].state != TASK_UNUSED) busy += t[i].cpu_pct;
    return busy > 100 ? 100 : busy;
}

static uint32_t mem_pct(void) {
    uint32_t total = kheap_total_bytes();
    return total ? (uint32_t)((uint64_t)kheap_used_bytes() * 100 / total) : 0;
}

static void sample(void) {
    if (g_hist_n == HIST) {
        memmove(g_cpu_hist, g_cpu_hist + 1, HIST - 1);
        memmove(g_mem_hist, g_mem_hist + 1, HIST - 1);
        g_hist_n--;
    }
    g_cpu_hist[g_hist_n] = (uint8_t)cpu_now();
    g_mem_hist[g_hist_n] = (uint8_t)mem_pct();
    g_hist_n++;
}

/* the Apps tab's rows: open windows, then running apps */
static void build_rows(void) {
    gui_win_info_t w[24];
    int nw = gui_windows(w, 24);
    g_nrows = 0;
    for (int i = 0; i < nw && g_nrows < MAX_ROWS; i++) {
        row_t* r = &g_rows[g_nrows++];
        r->is_app = 0;
        r->handle = w[i].handle;
        kstrlcpy(r->text, w[i].title, sizeof(r->text));
        kstrlcpy(r->info, w[i].minimized ? "window, minimized" : w[i].focused ? "window, active" : "window", sizeof(r->info));
    }
    app_info_t a[8];
    int na = app_snapshot(a, 8);
    for (int i = 0; i < na && g_nrows < MAX_ROWS; i++) {
        row_t* r = &g_rows[g_nrows++];
        r->is_app = 1;
        r->handle = a[i].id;
        ksnprintf(r->text, sizeof(r->text), "%s (app)", a[i].name);
        ksnprintf(r->info, sizeof(r->info), "%s, %u KiB, %d thread%s", a[i].desktop ? "desktop" : "terminal",
                  a[i].mem / 1024, a[i].threads, a[i].threads == 1 ? "" : "s");
    }
    if (g_sel >= g_nrows) g_sel = g_nrows - 1;
}

static void end_task(int row) {
    if (row < 0 || row >= g_nrows) return;
    row_t* r = &g_rows[row];
    if (r->is_app) app_kill(r->handle);
    else gui_window_close(r->handle);
    ksnprintf(g_status, sizeof(g_status), "Ended %s", r->text);
    g_gen++;
}

static void switch_to(int row) {
    if (row < 0 || row >= g_nrows || g_rows[row].is_app) return;
    gui_window_activate(g_rows[row].handle);
}

/* ── public ───────────────────────────────────────────────────────── */

void taskmgr_open(void) {
    g_open = 1;
    g_status[0] = 0;
    if (!g_hist_n) sample();
    build_rows();
    win_clamp(&g_win);
    g_gen++;
}

void taskmgr_close(void) {
    g_open = 0;
    g_win.dragging = g_win.resizing = 0;
    g_gen++;
}

int taskmgr_is_open(void) { return g_open; }
int taskmgr_contains(int mx, int my) { return g_open && inside(mx, my, g_win.x, g_win.y, g_win.w, g_win.h); }

uint32_t taskmgr_signature(void) {
    if (!g_open) return 0;
    if (timer_ms() - g_sample_ms >= 1000) {        /* live: once a second */
        g_sample_ms = timer_ms();
        sample();
        build_rows();
        g_gen++;
    }
    return g_gen * 2654435761u ^ (uint32_t)(g_win.x << 16 | g_win.y) ^ (uint32_t)(g_win.w << 20 | g_win.h << 4);
}

static int list_top(void) { return g_win.y + BODY_Y + ROW_H + 2; }

static int row_at(int mx, int my) {
    int lx = g_win.x + 8, lw = g_win.w - 16;
    if (mx < lx || mx >= lx + lw || my < list_top()) return -1;
    int r = (my - list_top()) / ROW_H;
    return r < g_nrows ? r : -1;
}

/* Processes tab: the task shown on list row r (unused slots are skipped), -1 */
static int g_menu_pid = -1;
static int proc_at(int mx, int my) {
    int lx = g_win.x + 8, lw = g_win.w - 16;
    if (mx < lx || mx >= lx + lw || my < list_top()) return -1;
    int r = (my - list_top()) / ROW_H;
    static task_info_t t[TASK_MAX];
    int n = task_count();
    if (n > TASK_MAX) n = TASK_MAX;
    task_snapshot(t, n);
    for (int i = 0; i < n; i++) {
        if (t[i].state == TASK_UNUSED) continue;
        if (r-- == 0) return (int)t[i].pid;
    }
    return -1;
}

static const struct { const char* name; int nice; } PRIOS[] = {
    { "High", -10 }, { "Above normal", -5 }, { "Normal", 0 }, { "Below normal", 5 }, { "Low", 10 },
};

static void menu_cb(int id, void* arg) {
    (void)arg;
    if (id == 1) switch_to(g_menu_row);
    else if (id == 2) end_task(g_menu_row);
    else if (id >= 10 && id < 15 && g_menu_pid >= 0) {
        int k = id - 10;
        if (task_set_nice(g_menu_pid, PRIOS[k].nice) == 0)
            ksnprintf(g_status, sizeof(g_status), "Task %d: priority %s (nice %d)", g_menu_pid, PRIOS[k].name, PRIOS[k].nice);
        g_gen++;
    }
}

void taskmgr_rclick(int mx, int my) {
    if (g_tab == TAB_PROCS) {
        /* a process: its priority (nice value) */
        int pid = proc_at(mx, my);
        if (pid < 0) return;
        g_menu_pid = pid;
        int cur = task_get_nice(pid);
        static char labels[5][32];
        ctx_item_t items[5];
        for (int k = 0; k < 5; k++) {
            ksnprintf(labels[k], sizeof(labels[k]), "%s%s", cur == PRIOS[k].nice ? "* " : "  ", PRIOS[k].name);
            items[k].label = labels[k];
            items[k].id = 10 + k;
            items[k].disabled = 0;
        }
        ctxmenu_open(mx, my, items, 5, menu_cb, NULL);
        g_gen++;
        return;
    }
    if (g_tab != TAB_APPS) return;
    int r = row_at(mx, my);
    if (r < 0) return;
    g_sel = r;
    g_menu_row = r;
    ctx_item_t items[] = {
        { "Switch to", 1, g_rows[r].is_app },
        { CTX_SEP, 0, 0 },
        { "End task", 2, 0 },
    };
    ctxmenu_open(mx, my, items, 3, menu_cb, NULL);
    g_gen++;
}

void taskmgr_click(int mx, int my) {
    if (!taskmgr_contains(mx, my)) return;
    int lx = mx - g_win.x, ly = my - g_win.y;
    g_gen++;
    if (ly < TITLE_H + 2) {
        int b = win_button_press(&g_win, 4, 12, mx, my);
        if (b == WIN_BTN_CLOSE) { taskmgr_close(); return; }
        if (b) return;
        win_title_press(&g_win, mx, my);
        return;
    }
    if (win_grip_press(&g_win, mx, my)) return;
    kbnav_mouse(&g_nav);
    if (ly >= TABS_Y && ly < TABS_Y + TAB_H) {
        int x = 6;
        for (int t = 0; t < TAB_COUNT; t++) {
            int w = (int)strlen(TAB_NAMES[t]) * 8 + 20;
            if (lx >= x && lx < x + w) { g_tab = t; g_sel = -1; g_status[0] = 0; }
            x += w + 4;
        }
        return;
    }
    if (g_tab == TAB_APPS) {
        int by = g_win.h - 44;
        if (ly >= by && ly < by + 18) {
            if (lx >= g_win.w - 208 && lx < g_win.w - 108) switch_to(g_sel);
            else if (lx >= g_win.w - 100 && lx < g_win.w - 8) end_task(g_sel);
            return;
        }
        int r = row_at(mx, my);
        static uint32_t last_ms;
        static int last_row = -1;
        if (r >= 0) {
            uint32_t now = timer_ms();
            if (r == last_row && now - last_ms < 450) switch_to(r);   /* double-click */
            last_row = r;
            last_ms = now;
        }
        g_sel = r;
    }
}

/* the keyboard: Tab / arrows, Enter on a row switches to it, Delete ends it */
int taskmgr_navkey(int code) {
    if (!g_open) return 0;
    g_gen++;
    if (code == KB_DEL && g_tab == TAB_APPS && g_sel >= 0) { end_task(g_sel); return 1; }
    int cx, cy;
    int r = kbnav_key(&g_nav, code, &cx, &cy);
    if (r == 1) {
        int shown = g_nav.shown, focus = g_nav.focus;
        int row = g_tab == TAB_APPS ? row_at(cx, cy) : -1;
        taskmgr_click(cx, cy);
        if (row >= 0) taskmgr_click(cx, cy);          /* (a double click: switch to it) */
        g_nav.shown = shown;
        g_nav.focus = focus;
    } else if (r == 2 && g_tab == TAB_APPS) {
        int fx, fy, fw, fh;
        if (kbnav_focused(&g_nav, &fx, &fy, &fw, &fh)) { int row = row_at(fx + fw / 2, fy + fh / 2); if (row >= 0) g_sel = row; }
    }
    return r != 0;
}

void taskmgr_mouse(int mx, int my, int left) {
    if (win_mouse(&g_win, mx, my, left)) g_gen++;
}

static void graph(int x, int y, int w, int h, const uint8_t* hist, int n, uint32_t color, const char* label) {
    bevel(x, y, w, h, C_LIST, 0x0010141Cu, 0x00404B5Cu);
    for (int g = 1; g < 4; g++) gfx_fill_rect(x + 1, y + h * g / 4, w - 2, 1, 0x00232A35u);
    int bw = (w - 4) / HIST;
    if (bw < 1) bw = 1;
    for (int i = 0; i < n; i++) {
        int bh = (h - 4) * hist[i] / 100;
        if (bh < 1 && hist[i]) bh = 1;
        int bx = x + 2 + (HIST - n + i) * bw;
        gfx_fill_rect(bx, y + h - 2 - bh, bw > 1 ? bw - 1 : 1, bh, color);
    }
    gfx_draw_text(x + 6, y + 5, label, C_TEXT, C_LIST);
}

static void human(uint32_t bytes, char* out, int cap) {
    if (bytes >= (1u << 20)) ksnprintf(out, (size_t)cap, "%u.%u MiB", bytes >> 20, ((bytes >> 10) & 1023) * 10 / 1024);
    else ksnprintf(out, (size_t)cap, "%u KiB", bytes >> 10);
}

void taskmgr_draw(const fb_info_t* fi) {
    (void)fi;
    if (!g_open) return;
    int x = g_win.x, y = g_win.y, W = g_win.w, H = g_win.h;
    kbnav_begin(&g_nav);
    bevel(x, y, W, H, C_PANEL, 0x00505D72u, 0x0010141Cu);
    bevel(x + 3, y + 3, W - 6, TITLE_H - 1, C_TITLE, 0x00647692u, 0x00111923u);
    gfx_draw_text(x + 10, y + 7, "Task Manager", 0x00FFFFFFu, C_TITLE);
    win_draw_buttons(&g_win, 4, 12);

    int tx = x + 6;
    for (int t = 0; t < TAB_COUNT; t++) {
        int w = (int)strlen(TAB_NAMES[t]) * 8 + 20;
        uint32_t bg = t == g_tab ? 0x00F2F2F2u : 0x003A4250u;
        bevel(tx, y + TABS_Y, w, TAB_H, bg, 0x00808A9Au, 0x00202630u);
        kbnav_add(&g_nav, tx, y + TABS_Y, w, TAB_H);
        gfx_draw_text(tx + 10, y + TABS_Y + 6, TAB_NAMES[t], t == g_tab ? 0x00101010u : C_TEXT, bg);
        tx += w + 4;
    }
    char line[128];
    int lx = x + 8, lw = W - 16;

    if (g_tab == TAB_APPS) {
        int lh = H - BODY_Y - 56;
        bevel(lx, y + BODY_Y, lw, lh, C_LIST, 0x0010141Cu, 0x00404B5Cu);
        gfx_fill_rect(lx + 1, y + BODY_Y + 1, lw - 2, ROW_H, 0x00252C37u);
        gfx_draw_text(lx + 8, y + BODY_Y + 4, "Name", C_DIM, 0x00252C37u);
        gfx_draw_text(lx + lw - 200, y + BODY_Y + 4, "Status", C_DIM, 0x00252C37u);
        int maxr = (lh - ROW_H - 4) / ROW_H;
        for (int i = 0; i < g_nrows && i < maxr; i++) {
            int ry = list_top() + i * ROW_H;
            uint32_t bg = i == g_sel ? C_SEL : C_LIST;
            kbnav_add(&g_nav, lx + 1, ry, lw - 2, ROW_H);
            if (i == g_sel) gfx_fill_rect(lx + 1, ry, lw - 2, ROW_H, bg);
            char t[48];
            kstrlcpy(t, g_rows[i].text, sizeof(t));
            int maxc = (lw - 220) / 8;
            if (maxc > 0 && maxc < (int)sizeof(t)) t[maxc] = 0;
            gfx_fill_rect(lx + 8, ry + 3, 8, 8, g_rows[i].is_app ? 0x00D0A030u : 0x003A7BD5u);
            gfx_draw_text(lx + 22, ry + 3, t, C_TEXT, bg);
            gfx_draw_text(lx + lw - 200, ry + 3, g_rows[i].info, C_DIM, bg);
        }
        if (!g_nrows) gfx_draw_text(lx + 8, list_top() + 4, "Nothing is open.", C_DIM, C_LIST);
        int by = y + H - 44;
        button(x + W - 208, by, 100, "Switch to", g_sel >= 0 && !g_rows[g_sel].is_app);
        button(x + W - 100, by, 92, "End task", g_sel >= 0);
    } else if (g_tab == TAB_PROCS) {
        static task_info_t t[TASK_MAX];
        int n = task_count();
        if (n > TASK_MAX) n = TASK_MAX;
        task_snapshot(t, n);
        int lh = H - BODY_Y - 30;
        bevel(lx, y + BODY_Y, lw, lh, C_LIST, 0x0010141Cu, 0x00404B5Cu);
        gfx_fill_rect(lx + 1, y + BODY_Y + 1, lw - 2, ROW_H, 0x00252C37u);
        gfx_draw_text(lx + 8, y + BODY_Y + 4, "PID  Name                    State     CPU   Time   Prio", C_DIM, 0x00252C37u);
        int row = 0, maxr = (lh - ROW_H - 4) / ROW_H;
        for (int i = 0; i < n && row < maxr; i++) {
            if (t[i].state == TASK_UNUSED) continue;
            uint32_t s = t[i].ticks_total / 100;
            char st[16];
            if (t[i].state == TASK_AWAY && t[i].cpu > 0) ksnprintf(st, sizeof(st), "core %d", t[i].cpu);
            else kstrlcpy(st, t[i].state == TASK_AWAY ? "wait core" : task_state_str(t[i].state), sizeof(st));
            ksnprintf(line, sizeof(line), "%-4u %-23s %-9s %3u%%  %u:%02u   %3d", t[i].pid, t[i].name,
                      st, t[i].cpu_pct, s / 60, s % 60, t[i].nice);
            gfx_draw_text(lx + 8, list_top() + row * ROW_H + 3, line, t[i].cpu_pct >= 50 ? 0x00F0A060u : C_TEXT, C_LIST);
            row++;
        }
    } else {
        int gw = (W - 24) / 2, gh = 120, gy = y + BODY_Y + 4;
        char lab[48];
        ksnprintf(lab, sizeof(lab), "CPU %u%%", g_hist_n ? g_cpu_hist[g_hist_n - 1] : 0);
        graph(lx, gy, gw, gh, g_cpu_hist, g_hist_n, C_CPU, lab);
        ksnprintf(lab, sizeof(lab), "Memory %u%%", g_hist_n ? g_mem_hist[g_hist_n - 1] : 0);
        graph(lx + gw + 8, gy, gw, gh, g_mem_hist, g_hist_n, C_MEM, lab);
        int ty = gy + gh + 14;
        char a[24], b[24];
        human(kheap_used_bytes(), a, sizeof(a));
        human(kheap_total_bytes(), b, sizeof(b));
        ksnprintf(line, sizeof(line), "Memory in use:  %s of %s (largest free block %u KiB)", a, b, kheap_largest_free() / 1024);
        gfx_draw_text(lx, ty, line, C_TEXT, C_PANEL);
        uint32_t up = timer_ms() / 1000;
        int ntask = 0;
        static task_info_t t[TASK_MAX];
        int n = task_count();
        if (n > TASK_MAX) n = TASK_MAX;
        task_snapshot(t, n);
        for (int i = 0; i < n; i++) if (t[i].state != TASK_UNUSED) ntask++;
        ksnprintf(line, sizeof(line), "Uptime:         %u:%02u:%02u", up / 3600, up / 60 % 60, up % 60);
        gfx_draw_text(lx, ty + 16, line, C_TEXT, C_PANEL);
        ksnprintf(line, sizeof(line), "Tasks:          %d kernel tasks, %d app%s", ntask, app_count(), app_count() == 1 ? "" : "s");
        gfx_draw_text(lx, ty + 32, line, C_TEXT, C_PANEL);
        ksnprintf(line, sizeof(line), "Files:          %u files, %u folders (%u KiB in RAM)", fs_used_files(), fs_used_dirs(),
                  fs_ram_used_bytes() / 1024);
        gfx_draw_text(lx, ty + 48, line, C_TEXT, C_PANEL);
        netif_t* nif = net_if();
        char ip[16] = "-";
        if (nif->dev && nif->configured) ip4_to_str(nif->ip, ip);
        ksnprintf(line, sizeof(line), "Network:        %s %s", nif->dev ? nif->dev->ifname : "none", ip);
        gfx_draw_text(lx, ty + 64, line, C_TEXT, C_PANEL);
        ksnprintf(line, sizeof(line), "Sound:          %s", audio_device_name());
        gfx_draw_text(lx, ty + 80, line, C_TEXT, C_PANEL);
        ksnprintf(line, sizeof(line), "Kernel:         %s", BANANA_ARCH_DESC);
        gfx_draw_text(lx, ty + 96, line, C_TEXT, C_PANEL);
        if (cpu_count() > 1 && smp_threads_per_core() > 1)
            ksnprintf(line, sizeof(line), "Processor:      %d cores, %d threads (the kernel on 0, apps on all)", smp_phys_cores(), cpu_count());
        else if (cpu_count() > 1)
            ksnprintf(line, sizeof(line), "Processor:      %d cores (the kernel on core 0, apps on all)", cpu_count());
        else
            ksnprintf(line, sizeof(line), "Processor:      1 core in use (%d found)", smp_cores_found());
        gfx_draw_text(lx, ty + 112, line, C_TEXT, C_PANEL);
    }

    gfx_fill_rect(x + 3, y + H - 18, W - 6, 15, 0x00161B22u);
    if (g_status[0]) kstrlcpy(line, g_status, sizeof(line));
    else ksnprintf(line, sizeof(line), "CPU %u%%   Memory %u%%   %d window%s", g_hist_n ? g_cpu_hist[g_hist_n - 1] : 0,
                   g_hist_n ? g_mem_hist[g_hist_n - 1] : 0, g_nrows, g_nrows == 1 ? "" : "s");
    gfx_draw_text(x + 8, y + H - 14, line, C_DIM, 0x00161B22u);
    gfx_draw_grip(x + W, y + H);
    kbnav_draw(&g_nav, 0x00FFD34Eu);
}
