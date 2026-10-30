/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "taskmgr.h"
#include "i18n.h"
#include "theme.h"
#include "gfx.h"
#include "kstring.h"
#include "kheap.h"
#include "meminfo.h"
#include "zswap.h"
#include "timer.h"
#include "task.h"
#include "smp.h"
#include "fs.h"
#include "winframe.h"
#include "ctxmenu.h"
#include "app.h"
#include "gui.h"
#include "audio.h"
#include "sensors.h"
#include "sysinfo.h"
#include "disk.h"
#include "blockdev.h"
#include "gpu.h"
#include "fb.h"
#include "font.h"
#include "kbnav.h"
#include "../net/net.h"

/*
 * Four looks, chosen by the theme (Settings > Themes, theme_taskmgr_style):
 *   Windows 11 - a rail of pages at the left (Processes, Performance,
 *                Details, Users), dark;
 *   Windows 10 - the same pages behind tabs under a menu bar, light;
 *   Windows    - the classic one (XP / 7): grey, Applications, Processes,
 *                Performance with green meters, Networking, Users;
 *   Banana OS  - Banana OS's own first Task Manager.
 * Processes: the apps (windows and running programs) and the background
 * processes (the kernel's tasks) with their CPU, memory, disk and network
 * use. Performance: CPU, memory, disk, Ethernet, GPU, with live graphs.
 * Sampled once a second while the window is open.
 */

#define TITLE_H  20
#define RAIL_W   150
#define ITEM_H   34
#define ROW_H    ((g_style==TM_STYLE_WIN10||g_style==TM_STYLE_WIN11)?28:20)
#define HIST     60
#define MAX_ROWS 96
#define MAX_CORE 16

/* the colours of the Windows 11 / 10 looks */
typedef struct {
    uint32_t bg, rail, panel, line, hover, text, dim, accent, sel;
    uint32_t cpu, mem, disk, net, gpu, heat0, heat_lo, heat_hi, hot_text;
} pal_t;
static pal_t g_pal;
static int   g_style = TM_STYLE_WIN11;

#define C_BG     g_pal.bg
#define C_RAIL   g_pal.rail
#define C_PANEL  g_pal.panel
#define C_LINE   g_pal.line
#define C_HOVER  g_pal.hover
#define C_TEXT   g_pal.text
#define C_DIM    g_pal.dim
#define C_ACCENT g_pal.accent
#define C_SEL    g_pal.sel
#define C_CPU    g_pal.cpu
#define C_MEM    g_pal.mem
#define C_DISK   g_pal.disk
#define C_NET    g_pal.net
#define C_GPU    g_pal.gpu

static void set_style(void) {
    g_style = theme_taskmgr_style();
    if (g_style != TM_STYLE_WIN10 && g_style != TM_STYLE_WIN11) {
        g_pal = (pal_t){ TC(0x17202C),TC(0x121923),TC(0x1E2937),TC(0x344256),TC(0x26374D),
            TC(0xE7EEF8),TC(0xA2B0C4),TC(0x80B7FF),TC(0x304D70),
            TC(0x17A2E8),TC(0xA066D0),TC(0x4DB858),TC(0xD88A3C),TC(0xD0506A),
            TC(0x17202C),TC(0x17202C),TC(0x304D70),TC(0xE7EEF8) };
        return;
    }
    /* Each explicit Windows look keeps a coherent neutral palette instead
     * of passing light surfaces through the desktop's dark colour mapping. */
    if (g_style == TM_STYLE_WIN10) {
        g_pal = (pal_t){
            .bg=0xFFFFFF,.rail=0xF3F3F3,.panel=0xFFFFFF,.line=0xE1E1E1,
            .hover=0xF0F7FD,.text=0x202020,.dim=0x656565,.accent=0x0078D4,.sel=0xD9ECFA,
            .cpu=0x117DBB,.mem=0x8B12AE,.disk=0x4DA60C,.net=0xA74F01,.gpu=0x0078D7,
            .heat0=0xFFFFFF,.heat_lo=0xFFFCF0,.heat_hi=0xFFE6A8,.hot_text=0x202020
        };
    } else {
        g_pal = (pal_t){
            .bg=0x202020,.rail=0x191919,.panel=0x252525,.line=0x363636,
            .hover=0x2D2D2D,.text=0xF3F3F3,.dim=0xACACAC,.accent=0x60CDFF,.sel=0x353F45,
            .cpu=0x60CDFF,.mem=0xC49BE8,.disk=0x83C995,.net=0xE3B775,.gpu=0xC69BD4,
            .heat0=0x202020,.heat_lo=0x23282A,.heat_hi=0x354F5C,.hot_text=0xF3F3F3
        };
    }
}
static int w10(void) { return g_style == TM_STYLE_WIN10; }

enum { P_PROCS = 0, P_PERF, P_DETAILS, P_USERS, P_COUNT };
static const char* const PAGE_NAMES[P_COUNT] = { "Processes", "Performance", "Details", "Users" };
enum { D_CPU = 0, D_MEM, D_DISK, D_NET, D_GPU, D_COUNT };

/* a row of the Processes page: a window, a running app, or a kernel task */
enum { R_WINDOW, R_APP, R_TASK, R_GROUP };
typedef struct {
    int kind, handle, pid;
    char text[48];
    char status[20];
    int cpu;                  /* % (-1: not known) */
    uint32_t mem;             /* bytes (0: not known) */
} row_t;

static int        g_open;
static win_geom_t g_win = { .x = 20, .y = 20, .w = 760, .h = 520, .min_w = 560, .min_h = 380 };
static int        g_page, g_dev;
static int        g_sel = -1, g_scroll;
static int        g_cores_view;          /* Performance > CPU: a graph per logical processor */
static uint32_t   g_gen, g_sample_ms;
static char       g_status[96];
static kbnav_t    g_nav;

/* the history, one sample a second */
static uint8_t  g_cpu_hist[HIST], g_mem_hist[HIST], g_disk_hist[HIST];
static uint32_t g_net_hist[HIST];                     /* bytes a second, in + out */
static uint8_t  g_core_hist[MAX_CORE][HIST];
static int      g_hist_n;
static uint32_t g_rd_rate, g_wr_rate, g_rx_rate, g_tx_rate;   /* bytes a second, the last second */
static uint64_t g_last_rd, g_last_wr, g_last_busy;
static uint32_t g_last_rx, g_last_tx, g_last_ms;

static row_t    g_rows[MAX_ROWS];
static int      g_nrows, g_napps, g_nbg;
static int      g_menu_row = -1, g_menu_pid = -1;

static task_info_t g_tasks[TASK_MAX];
static int         g_ntasks;

/* Proportional, anti-aliased text for the modern looks. Classic and
 * Banana layouts retain their existing fixed-cell geometry and font. */
static int modern(void) { return g_style==TM_STYLE_WIN10 || g_style==TM_STYLE_WIN11; }
static int tm_width(const char* text) {
    return modern() && font_available(FONT_SANS) ?
        font_text_width(FONT_SANS,12,text,(uint32_t)strlen(text)) : (int)strlen(text)*8;
}
static void tm_text(int x,int y,const char* text,uint32_t fg,uint32_t bg) {
    if (!modern() || !font_available(FONT_SANS)) { gfx_draw_text(x,y,text,fg,bg);return; }
    int stride,w,h,ascent;
    uint32_t* pixels=fb_target(&stride,&w,&h);
    if (!pixels) return;
    font_metrics(FONT_SANS,12,&ascent,NULL,NULL);
    int x1=g_win.x+g_win.w-2,y1=g_win.y+g_win.h-2;
    if (x1 > w) x1 = w;
    if (y1 > h) y1 = h;
    font_draw(pixels,stride,g_win.x+2,g_win.y+2,x1,y1,FONT_SANS,12,
              x,y+ascent,text,(uint32_t)strlen(text),fg,0);
}
static void tm_heading(int x,int y,const char* text,uint32_t fg,uint32_t bg) {
    int stride,w,h,ascent;
    uint32_t* pixels=fb_target(&stride,&w,&h);
    if (!pixels || !font_available(FONT_SANS_BOLD)) { gfx_draw_text(x,y,text,fg,bg);return; }
    font_metrics(FONT_SANS_BOLD,18,&ascent,NULL,NULL);
    int x1=g_win.x+g_win.w-2,y1=g_win.y+g_win.h-2;
    if (x1 > w) x1 = w;
    if (y1 > h) y1 = h;
    font_draw(pixels,stride,g_win.x+2,g_win.y+2,x1,y1,FONT_SANS_BOLD,18,
              x,y+ascent,text,(uint32_t)strlen(text),fg,0);
}

/* ── drawing helpers ── */
static void frame(int x, int y, int w, int h, uint32_t bg, uint32_t line) {
    gfx_fill_rect(x, y, w, h, bg);
    gfx_fill_rect(x, y, w, 1, line);
    gfx_fill_rect(x, y + h - 1, w, 1, line);
    gfx_fill_rect(x, y, 1, h, line);
    gfx_fill_rect(x + w - 1, y, 1, h, line);
}

static uint32_t mix(uint32_t a, uint32_t b, int t) {        /* t: 0..256 from a to b */
    if (t < 0) t = 0;
    if (t > 256) t = 256;
    uint32_t r = (((a >> 16) & 255) * (uint32_t)(256 - t) + ((b >> 16) & 255) * (uint32_t)t) >> 8;
    uint32_t g = (((a >> 8) & 255) * (uint32_t)(256 - t) + ((b >> 8) & 255) * (uint32_t)t) >> 8;
    uint32_t c = ((a & 255) * (uint32_t)(256 - t) + (b & 255) * (uint32_t)t) >> 8;
    return r << 16 | g << 8 | c;
}

/* the heat of a column's cell: deeper amber as it gets busier */
static uint32_t heat(int pct) {
    if (pct <= 0) return g_pal.heat0;
    return mix(g_pal.heat_lo, g_pal.heat_hi, pct * 256 / 100);
}

static void button(int x, int y, int w, const char* label, int enabled) {
    kbnav_add(&g_nav, x, y, w, 24);
    uint32_t bg = enabled?C_PANEL:C_BG;
    uint32_t ln = enabled?C_LINE:mix(C_BG,C_LINE,100);
    uint32_t fg = enabled ? C_TEXT : C_DIM;
    frame(x, y, w, 24, bg, ln);
    tm_text(x + (w - tm_width(label)) / 2, y + 8, label, fg, bg);
}

static int inside(int mx, int my, int x, int y, int w, int h) {
    return mx >= x && mx < x + w && my >= y && my < y + h;
}

/* (KiB: more than 4 GiB fits) */
static void human_kb(uint32_t kb, char* out, int cap) {
    if (kb >= (1u << 20)) ksnprintf(out, (size_t)cap, "%u.%u GB", kb >> 20, ((kb >> 10) & 1023) * 10 / 1024);
    else if (kb >= 1024) ksnprintf(out, (size_t)cap, "%u.%u MB", kb >> 10, (kb & 1023) * 10 / 1024);
    else ksnprintf(out, (size_t)cap, "%u KB", kb);
}

static void human_bytes64(uint64_t b, char* out, int cap) {
    if (b >= (1ull << 30)) ksnprintf(out, (size_t)cap, "%u.%u GB", (uint32_t)(b >> 30), (uint32_t)((b >> 20) & 1023) * 10 / 1024);
    else if (b >= (1ull << 20)) ksnprintf(out, (size_t)cap, "%u.%u MB", (uint32_t)(b >> 20), (uint32_t)((b >> 10) & 1023) * 10 / 1024);
    else ksnprintf(out, (size_t)cap, "%u KB", (uint32_t)(b >> 10));
}

/* a rate: "1.5 MB/s" */
static void rate(uint32_t bps, char* out, int cap) {
    if (bps >= (1u << 20)) ksnprintf(out, (size_t)cap, "%u.%u MB/s", bps >> 20, ((bps >> 10) & 1023) * 10 / 1024);
    else ksnprintf(out, (size_t)cap, "%u KB/s", bps >> 10);
}

/* network: "120 Kbps" (bits, as Windows) */
static void bits(uint32_t bps, char* out, int cap) {
    uint32_t kb = bps / 125;                          /* bytes/s -> kilobits/s */
    if (kb >= 10000) ksnprintf(out, (size_t)cap, "%u Mbps", kb / 1000);
    else if (kb >= 1000) ksnprintf(out, (size_t)cap, "%u.%u Mbps", kb / 1000, kb % 1000 / 100);
    else ksnprintf(out, (size_t)cap, "%u Kbps", kb);
}

/* ── the numbers ── */
static void snap_tasks(void) {
    g_ntasks = task_count();
    if (g_ntasks > TASK_MAX) g_ntasks = TASK_MAX;
    task_snapshot(g_tasks, g_ntasks);
}

static uint32_t cpu_now(void) {
    int cores = cpu_count() < 1 ? 1 : cpu_count();
    uint32_t sum = 0;
    for (int c = 0; c < cores && c < MAX_CORE; c++) sum += task_core_pct(c);
    return sum / (uint32_t)(cores < MAX_CORE ? cores : MAX_CORE);
}

static uint32_t mem_pct(void) {
    meminfo_t m;
    meminfo_get(&m);
    /* (saved file data kept in RAM is a cache, given back when needed: not "in use") */
    return m.total_kb ? (uint32_t)((uint64_t)(m.used_kb - m.cached_kb) * 100 / m.total_kb) : 0;
}

static netdev_t* net_dev(void) { netif_t* n = net_if(); return n ? n->dev : NULL; }

static void push(uint8_t* h, uint32_t v) {
    if (g_hist_n == HIST) memmove(h, h + 1, HIST - 1);
    h[g_hist_n == HIST ? HIST - 1 : g_hist_n] = (uint8_t)(v > 100 ? 100 : v);
}

static void sample(void) {
    uint32_t now = timer_ms(), dt = g_last_ms ? now - g_last_ms : 0;
    uint64_t rd, wr, busy;
    disk_io_stats(&rd, &wr, &busy);
    netdev_t* nd = net_dev();
    uint32_t rx = nd ? nd->rx_bytes : 0, tx = nd ? nd->tx_bytes : 0;
    uint32_t disk = 0;
    if (dt) {
        g_rd_rate = (uint32_t)((rd - g_last_rd) * 1000u / dt);
        g_wr_rate = (uint32_t)((wr - g_last_wr) * 1000u / dt);
        uint64_t b = (busy - g_last_busy) / 10000u;          /* (ns -> hundredths of a ms: % of dt ms) */
        disk = (uint32_t)(b / dt);
        g_rx_rate = (uint32_t)((uint64_t)(rx - g_last_rx) * 1000u / dt);
        g_tx_rate = (uint32_t)((uint64_t)(tx - g_last_tx) * 1000u / dt);
    }
    g_last_rd = rd; g_last_wr = wr; g_last_busy = busy; g_last_rx = rx; g_last_tx = tx; g_last_ms = now;

    push(g_cpu_hist, cpu_now());
    push(g_mem_hist, mem_pct());
    push(g_disk_hist, disk);
    for (int c = 0; c < MAX_CORE; c++) push(g_core_hist[c], c < cpu_count() ? task_core_pct(c) : 0);
    if (g_hist_n == HIST) memmove(g_net_hist, g_net_hist + 1, (HIST - 1) * sizeof(uint32_t));
    g_net_hist[g_hist_n == HIST ? HIST - 1 : g_hist_n] = g_rx_rate + g_tx_rate;
    if (g_hist_n < HIST) g_hist_n++;
}

static int last(const uint8_t* h) { return g_hist_n ? h[g_hist_n - 1] : 0; }

/* the Processes page's rows: apps (windows, then programs), then the kernel's tasks */
static int task_cpu_of(int pid) {
    for (int i = 0; i < g_ntasks; i++) if (g_tasks[i].state != TASK_UNUSED && (int)g_tasks[i].pid == pid) return (int)g_tasks[i].cpu_pct;
    return -1;
}

static void build_rows(void) {
    snap_tasks();
    static gui_win_info_t w[64];
    int nw = gui_windows(w, 64);
    static app_info_t a[APP_MAX];
    int na = app_snapshot(a, APP_MAX);
    g_nrows = 0;
    g_napps = nw + na;
    row_t* r = &g_rows[g_nrows++];
    memset(r, 0, sizeof(*r));
    r->kind = R_GROUP;
    ksnprintf(r->text, sizeof(r->text), "%s (%d)", _("Apps"), g_napps);
    for (int i = 0; i < nw && g_nrows < MAX_ROWS; i++) {
        r = &g_rows[g_nrows++];
        memset(r, 0, sizeof(*r));
        r->kind = R_WINDOW;
        r->handle = w[i].handle;
        r->cpu = -1;
        kstrlcpy(r->text, w[i].title, sizeof(r->text));
        kstrlcpy(r->status, w[i].minimized ? _("Minimized") : w[i].focused ? _("Active") : "", sizeof(r->status));
    }
    for (int i = 0; i < na && g_nrows < MAX_ROWS; i++) {
        r = &g_rows[g_nrows++];
        memset(r, 0, sizeof(*r));
        r->kind = R_APP;
        r->handle = a[i].id;
        r->pid = a[i].pid;
        r->cpu = task_cpu_of(a[i].pid);
        r->mem = a[i].mem;
        ksnprintf(r->text, sizeof(r->text), "%s", a[i].name);
        ksnprintf(r->status, sizeof(r->status), "%d thread%s", a[i].threads, a[i].threads == 1 ? "" : "s");
    }
    int gi = g_nrows;
    if (g_nrows < MAX_ROWS) { r = &g_rows[g_nrows++]; memset(r, 0, sizeof(*r)); r->kind = R_GROUP; }
    g_nbg = 0;
    for (int i = 0; i < g_ntasks && g_nrows < MAX_ROWS; i++) {
        if (g_tasks[i].state == TASK_UNUSED) continue;
        r = &g_rows[g_nrows++];
        memset(r, 0, sizeof(*r));
        r->kind = R_TASK;
        r->pid = (int)g_tasks[i].pid;
        r->cpu = (int)g_tasks[i].cpu_pct;
        kstrlcpy(r->text, g_tasks[i].name, sizeof(r->text));
        kstrlcpy(r->status, task_state_str(g_tasks[i].state), sizeof(r->status));
        g_nbg++;
    }
    if (gi < g_nrows) ksnprintf(g_rows[gi].text, sizeof(g_rows[gi].text), "%s (%d)", _("Background processes"), g_nbg);
    if (g_sel >= g_nrows) g_sel = -1;
}

static void end_task(int row) {
    if (row < 0 || row >= g_nrows) return;
    row_t* r = &g_rows[row];
    if (r->kind == R_APP) app_kill(r->handle);
    else if (r->kind == R_WINDOW) gui_window_close(r->handle);
    else return;
    ksnprintf(g_status, sizeof(g_status), "Ended %s", r->text);
    g_gen++;
}

static void switch_to(int row) {
    if (row < 0 || row >= g_nrows || g_rows[row].kind != R_WINDOW) return;
    gui_window_activate(g_rows[row].handle);
}

/* the processor's caches (cpuid leaf 4, Intel; 0x80000005/6, AMD): KiB, 0 unknown */
static void cpuid(uint32_t leaf, uint32_t sub, uint32_t* a, uint32_t* b, uint32_t* c, uint32_t* d) {
    __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(sub));
}

static void caches(uint32_t* l1, uint32_t* l2, uint32_t* l3) {
    uint32_t a, b, c, d;
    *l1 = *l2 = *l3 = 0;
    cpuid(0, 0, &a, &b, &c, &d);
    uint32_t maxl = a;
    if (b == 0x756E6547 && maxl >= 4) {                 /* "Genu"ineIntel */
        for (uint32_t i = 0; i < 8; i++) {
            cpuid(4, i, &a, &b, &c, &d);
            uint32_t type = a & 31, level = (a >> 5) & 7;
            if (!type) break;
            uint32_t size = ((b >> 22) + 1) * (((b >> 12) & 1023) + 1) * ((b & 4095) + 1) * (c + 1) / 1024;
            if (level == 1) *l1 += size;
            else if (level == 2) *l2 = size;
            else if (level == 3) *l3 = size;
        }
        return;
    }
    cpuid(0x80000000u, 0, &a, &b, &c, &d);
    if (a >= 0x80000006u) {
        cpuid(0x80000005u, 0, &a, &b, &c, &d);
        *l1 = (c >> 24) + (d >> 24);
        cpuid(0x80000006u, 0, &a, &b, &c, &d);
        *l2 = c >> 16;
        *l3 = (d >> 18) * 512;
    }
}

static int in_vm(void) {
    uint32_t a, b, c, d;
    cpuid(1, 0, &a, &b, &c, &d);
    return (c >> 31) & 1;
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
    return g_gen * 2654435761u ^ (uint32_t)((uint32_t)g_win.x << 16 | g_win.y) ^ (uint32_t)(g_win.w << 20 | g_win.h << 4);
}

/* ── layout: Windows 11 a rail at the left; Windows 10 a menu bar and tabs on top ── */
#define W10_MENU_Y (TITLE_H + 2)
#define W10_TABS_Y (TITLE_H + 21)
#define W10_TAB_H  22
static int content_x(void) { return w10() ? g_win.x + 6 : g_win.x + RAIL_W + 14; }
static int content_w(void) { return w10() ? g_win.w - 12 : g_win.w - RAIL_W - 22; }
static int head_y(void) { return w10() ? g_win.y + W10_TABS_Y + W10_TAB_H + 4 : g_win.y + TITLE_H + 12; }
static int table_y(void) { return w10() ? head_y() : head_y() + 40; }    /* the column headers */
static int rows_y(void) { return table_y() + 34; }
static int bottom_y(void) { return w10() ? g_win.y + g_win.h - 40 : g_win.y + g_win.h - 10; }   /* (Windows 10: its button bar) */
static int rows_max(void) { int n = (bottom_y() - rows_y()) / ROW_H; return n < 1 ? 1 : n; }
static int perf_top(void) { return w10() ? head_y() + 8 : head_y() + 40; }
/* the Processes page's End task / Switch to buttons */
static void end_rect(int* x, int* y) {
    if (w10()) { *x = g_win.x + g_win.w - 106; *y = g_win.y + g_win.h - 32; }
    else { *x = content_x() + content_w() - 96; *y = head_y(); }
}
static void switch_rect(int* x, int* y) { end_rect(x, y); *x -= 100; }
/* Windows 10's tabs: x of tab i, its width */
static const char* const W10_TABS[4] = { "Processes", "Performance", "Users", "Details" };
static const int W10_PAGE[4] = { 0, 1, 3, 2 };            /* (P_PROCS, P_PERF, P_USERS, P_DETAILS) */
static int w10_tab_x(int i, int* w) {
    int x = g_win.x + 6;
    for (int k = 0; k < i; k++) x += tm_width(_(W10_TABS[k])) + 24;
    *w = tm_width(_(W10_TABS[i])) + 24;
    return x;
}

/* the Processes / Details columns, from the right: x of each */
#define COL_NET  86
#define COL_DISK 86
#define COL_MEM  96
#define COL_CPU  66
#define COL_STAT (content_w()<600?0:100)
static int col_x(int i) {        /* 0 status, 1 cpu, 2 mem, 3 disk, 4 net */
    int r = content_x() + content_w();
    int w[5] = { COL_STAT, COL_CPU, COL_MEM, COL_DISK, COL_NET };
    for (int k = 4; k >= i; k--) r -= w[k];
    return r;
}

static int row_at(int mx, int my) {
    if (mx < content_x() || mx >= content_x() + content_w() || my < rows_y()) return -1;
    int r = (my - rows_y()) / ROW_H;
    if (r >= rows_max()) return -1;
    r += g_scroll;
    return r < g_nrows ? r : -1;
}

/* Details: the task on list row r */
static int detail_pid_at(int mx, int my) {
    if (mx < content_x() || my < rows_y()) return -1;
    int r = (my - rows_y()) / ROW_H + g_scroll;
    for (int i = 0; i < g_ntasks; i++) {
        if (g_tasks[i].state == TASK_UNUSED) continue;
        if (r-- == 0) return (int)g_tasks[i].pid;
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
    else if (id == 3) { g_page = P_DETAILS; g_scroll = 0; }
    else if (id >= 10 && id < 15 && g_menu_pid >= 0) {
        int k = id - 10;
        if (task_set_nice(g_menu_pid, PRIOS[k].nice) == 0)
            ksnprintf(g_status, sizeof(g_status), "Task %d: priority %s (nice %d)", g_menu_pid, PRIOS[k].name, PRIOS[k].nice);
    }
    g_gen++;
}

static void prio_menu(int mx, int my, int pid) {
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
}

static void classic_click(int mx, int my);
static void classic_rclick(int mx, int my);
static void banana_click(int mx, int my);
static void banana_rclick(int mx, int my);

void taskmgr_rclick(int mx, int my) {
    g_gen++;
    if (g_style == TM_STYLE_CLASSIC) { classic_rclick(mx, my); return; }
    if (g_style == TM_STYLE_BANANA) { banana_rclick(mx, my); return; }
    if (g_page == P_DETAILS) {
        int pid = detail_pid_at(mx, my);
        if (pid >= 0) prio_menu(mx, my, pid);
        return;
    }
    if (g_page == P_PERF) {
        if (g_dev == D_CPU) g_cores_view = !g_cores_view;     /* (Windows: "Change graph to") */
        return;
    }
    if (g_page != P_PROCS) return;
    int r = row_at(mx, my);
    if (r < 0 || g_rows[r].kind == R_GROUP) return;
    g_sel = r;
    g_menu_row = r;
    if (g_rows[r].kind == R_TASK) { prio_menu(mx, my, g_rows[r].pid); return; }
    ctx_item_t items[] = {
        { _("Switch to"), 1, g_rows[r].kind != R_WINDOW },
        { _("End task"), 2, 0 },
        { CTX_SEP, 0, 0 },
        { _("Go to details"), 3, 0 },
    };
    ctxmenu_open(mx, my, items, 4, menu_cb, NULL);
}

void taskmgr_click(int mx, int my) {
    if (!taskmgr_contains(mx, my)) return;
    int ly = my - g_win.y;
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
    if (g_style == TM_STYLE_CLASSIC) { classic_click(mx, my); return; }
    if (g_style == TM_STYLE_BANANA) { banana_click(mx, my); return; }
    if (w10()) {
        /* the tabs */
        if (ly >= W10_TABS_Y && ly < W10_TABS_Y + W10_TAB_H) {
            for (int i = 0; i < 4; i++) {
                int tw, tx = w10_tab_x(i, &tw);
                if (mx >= tx && mx < tx + tw) { g_page = W10_PAGE[i]; g_sel = -1; g_scroll = 0; g_status[0] = 0; }
            }
            return;
        }
        if (ly < W10_TABS_Y) return;                      /* (the menu bar: its words only) */
    } else if (mx < g_win.x + RAIL_W) {
        /* the rail */
        int i = (my - (g_win.y + TITLE_H + 10)) / ITEM_H;
        if (my >= g_win.y + TITLE_H + 10 && i >= 0 && i < P_COUNT) { g_page = i; g_sel = -1; g_scroll = 0; g_status[0] = 0; }
        return;
    }
    int cx = content_x();
    if (g_page == P_PROCS) {
        int ex, ey, sx, sy;
        end_rect(&ex, &ey);
        switch_rect(&sx, &sy);
        if (inside(mx, my, ex, ey, 96, 24)) { end_task(g_sel); return; }
        if (inside(mx, my, sx, sy, 92, 24)) { switch_to(g_sel); return; }
        int r = row_at(mx, my);
        static uint32_t last_ms;
        static int last_row = -1;
        if (r >= 0 && g_rows[r].kind != R_GROUP) {
            uint32_t now = timer_ms();
            if (r == last_row && now - last_ms < 450) switch_to(r);   /* double-click */
            last_row = r;
            last_ms = now;
            g_sel = r;
        } else g_sel = -1;
    } else if (g_page == P_PERF) {
        int ty = perf_top();
        if (mx < cx + 200) {
            int d = (my - ty) / 62;
            if (my >= ty && d >= 0 && d < D_COUNT) g_dev = d;
        } else if (g_dev == D_CPU && cpu_count() > 1 && my >= ty + 20 && my < ty + 220) {
            g_cores_view = !g_cores_view;
        }
    }
}

int taskmgr_navkey(int code) {
    if (!g_open) return 0;
    g_gen++;
    int own = g_style == TM_STYLE_WIN11 || g_style == TM_STYLE_WIN10;   /* (the other looks: Tab, the arrows and Enter only) */
    if (own && code == KB_DEL && g_page == P_PROCS && g_sel >= 0) { end_task(g_sel); return 1; }
    if (own && g_page == P_PROCS && (code == KB_UP || code == KB_DOWN) && g_nrows) {
        int s = g_sel < 0 ? 0 : g_sel;
        do { s += code == KB_UP ? -1 : 1; } while (s >= 0 && s < g_nrows && g_rows[s].kind == R_GROUP);
        if (s >= 0 && s < g_nrows) g_sel = s;
        if (g_sel < g_scroll) g_scroll = g_sel;
        if (g_sel >= g_scroll + rows_max()) g_scroll = g_sel - rows_max() + 1;
        return 1;
    }
    if (own && g_page == P_PROCS && code == KB_ENTER && g_sel >= 0) { switch_to(g_sel); return 1; }
    if (own && g_page == P_PERF && (code == KB_UP || code == KB_DOWN)) {
        g_dev = (g_dev + (code == KB_UP ? D_COUNT - 1 : 1)) % D_COUNT;
        return 1;
    }
    int cx, cy;
    int r = kbnav_key(&g_nav, code, &cx, &cy);
    if (r == 1) {
        int shown = g_nav.shown, focus = g_nav.focus;
        taskmgr_click(cx, cy);
        g_nav.shown = shown;
        g_nav.focus = focus;
    }
    return r != 0;
}

void taskmgr_mouse(int mx, int my, int left) {
    if (win_mouse(&g_win, mx, my, left)) g_gen++;
}

void taskmgr_wheel(int mx, int my, int dz) {
    (void)mx; (void)my;
    int total = g_page == P_DETAILS ? g_ntasks : g_nrows;
    g_scroll += dz * 3;
    if (g_scroll > total - rows_max()) g_scroll = total - rows_max();
    if (g_scroll < 0) g_scroll = 0;
    g_gen++;
}

/* ── the pages ── */

/* the rail's icons: small line drawings */
static void rail_icon(int page, int x, int y, uint32_t c) {
    switch (page) {
    case P_PROCS:            /* a grid of four boxes */
        for (int i = 0; i < 4; i++) frame(x + (i & 1) * 8, y + (i >> 1) * 8, 7, 7, C_RAIL, c);
        break;
    case P_PERF:             /* a pulse line */
        gfx_fill_rect(x, y + 8, 4, 1, c); gfx_fill_rect(x + 4, y + 3, 1, 6, c); gfx_fill_rect(x + 5, y + 3, 1, 1, c);
        gfx_fill_rect(x + 6, y + 3, 1, 10, c); gfx_fill_rect(x + 7, y + 12, 1, 1, c); gfx_fill_rect(x + 8, y + 6, 1, 7, c);
        gfx_fill_rect(x + 9, y + 6, 1, 1, c); gfx_fill_rect(x + 10, y + 6, 1, 3, c); gfx_fill_rect(x + 11, y + 8, 4, 1, c);
        break;
    case P_DETAILS:          /* a bulleted list */
        for (int i = 0; i < 3; i++) { gfx_fill_rect(x, y + 2 + i * 5, 2, 2, c); gfx_fill_rect(x + 4, y + 2 + i * 5, 10, 1, c); }
        break;
    default:                 /* a person */
        frame(x + 4, y, 7, 7, C_RAIL, c);
        frame(x + 1, y + 8, 13, 7, C_RAIL, c);
        break;
    }
}

static void modern_title_buttons(void) {
    int right=g_win.x+g_win.w, y=g_win.y+6;
    int close=right-28, max=close-24, min=max-24;
    gfx_fill_rect(min+6,y+8,8,1,C_DIM);
    frame(max+6,y,8,8,C_RAIL,C_DIM);
    if(g_win.maxed)frame(max+8,y-2,8,8,C_RAIL,C_DIM);
    for(int i=0;i<7;i++) {
        gfx_fill_rect(close+6+i,y+i,1,1,C_DIM);
        gfx_fill_rect(close+12-i,y+i,1,1,C_DIM);
    }
}

static void draw_rail(void) {
    int x = g_win.x + 3, y = g_win.y + TITLE_H + 2, h = g_win.h - TITLE_H - 5;
    gfx_fill_rect(x, y, RAIL_W - 3, h, C_RAIL);
    gfx_fill_rect(x+RAIL_W-2,y,1,h,C_LINE);
    for (int i = 0; i < P_COUNT; i++) {
        int iy = g_win.y + TITLE_H + 10 + i * ITEM_H, on = i == g_page;
        kbnav_add(&g_nav, x + 4, iy, RAIL_W - 12, ITEM_H - 4);
        if (on) {
            gfx_fill_rect(x + 4, iy, RAIL_W - 12, ITEM_H - 4, C_HOVER);
            gfx_fill_rect(x + 4, iy + 8, 3, ITEM_H - 20, C_ACCENT);
        }
        uint32_t bg = on ? C_HOVER : C_RAIL;
        rail_icon(i, x + 16, iy + 7, on ? C_ACCENT : C_DIM);
        (void)bg;
        tm_text(x + 42, iy + 11, _(PAGE_NAMES[i]), on ? C_TEXT : C_DIM, bg);
    }
}

static void page_title(const char* t) {
    if (w10()) return;                       /* (Windows 10: the tab says it) */
    tm_heading(content_x(), head_y() + 3, t, C_TEXT, C_BG);
}

/* a column header: the total over its name (Windows: "12%" over "CPU") */
static void col_head(int x, int w, const char* total, const char* name, int pct) {
    uint32_t bg = pct >= 0 ? heat(pct / 2) : C_BG;
    gfx_fill_rect(x, table_y(), w - 2, 32, bg);
    if (total) tm_text(x + w - 10 - tm_width(total), table_y() + 5, total, C_TEXT, bg);
    tm_text(x + w - 10 - tm_width(name), table_y() + 19, name, C_DIM, bg);
}

static void draw_processes(void) {
    int cx = content_x(), cw = content_w();
    page_title(_("Processes"));
    int sel_ok = g_sel >= 0 && g_sel < g_nrows;
    int bx, by;
    end_rect(&bx, &by);
    button(bx, by, 96, _("End task"), sel_ok && g_rows[g_sel].kind != R_TASK);
    switch_rect(&bx, &by);
    button(bx, by, 92, _("Switch to"), sel_ok && g_rows[g_sel].kind == R_WINDOW);

    char t[24];
    meminfo_t memory; meminfo_get(&memory);
    int cpu = last(g_cpu_hist), mem = last(g_mem_hist), dsk = last(g_disk_hist);
    tm_text(cx + 8, table_y() + 19, _("Name"), C_DIM, C_BG);
    if(COL_STAT)col_head(col_x(0), COL_STAT, NULL, _("Status"), -1);
    ksnprintf(t, sizeof(t), "%d%%", cpu);       col_head(col_x(1), COL_CPU, t, _("CPU"), cpu);
    ksnprintf(t, sizeof(t), "%d%%", mem);       col_head(col_x(2), COL_MEM, t, _("Memory"), mem);
    ksnprintf(t, sizeof(t), "%d%%", dsk);       col_head(col_x(3), COL_DISK, t, _("Disk"), dsk);
    netdev_t* nd = net_dev();
    int netp = nd ? (int)((g_rx_rate + g_tx_rate) / 125000u) : 0;   /* % of a 100 Mbps link (approximate) */
    ksnprintf(t, sizeof(t), "%d%%", netp > 100 ? 100 : netp);
    col_head(col_x(4), COL_NET, t, _("Network"), netp);
    gfx_fill_rect(cx, rows_y() - 2, cw, 1, C_LINE);

    int maxr = rows_max();
    if (g_scroll > g_nrows - maxr) g_scroll = g_nrows - maxr;
    if (g_scroll < 0) g_scroll = 0;
    for (int i = 0; i < maxr && i + g_scroll < g_nrows; i++) {
        int ri = i + g_scroll, ry = rows_y() + i * ROW_H;
        row_t* r = &g_rows[ri];
        if (r->kind == R_GROUP) {
            tm_text(cx + 8, ry + 8, r->text, C_ACCENT, C_BG);
            continue;
        }
        int sel = ri == g_sel;
        uint32_t bg = sel ? C_SEL : C_BG;
        kbnav_add(&g_nav, cx, ry, cw, ROW_H);
        if (sel) {gfx_fill_rect(cx, ry, cw, ROW_H, bg);gfx_fill_rect(cx,ry,2,ROW_H,C_ACCENT);}
        else gfx_fill_rect(cx,ry+ROW_H-1,cw,1,mix(C_BG,C_LINE,80));
        /* its icon: blue a window, amber an app, grey a kernel task */
        uint32_t ic = r->kind == R_WINDOW ? TC(0x3A7BD5) : r->kind == R_APP ? TC(0xD0A030) : TC(0x707A88);
        frame(cx + 10, ry + 5, 10, 10, ic, mix(ic, TC(0xFFFFFF), 80));
        char nm[48];
        kstrlcpy(nm, r->text, sizeof(nm));
        int maxc = (col_x(0) - (cx + 30) - 6) / 8;
        if (maxc > 0 && maxc < (int)sizeof(nm) && (int)strlen(nm) > maxc) { nm[maxc] = 0; if (maxc > 1) nm[maxc - 1] = '.'; }
        if (modern() && font_available(FONT_SANS)) {
            uint32_t n=font_fit(FONT_SANS,12,nm,(uint32_t)strlen(nm),col_x(0)-cx-38);
            nm[n]=0;
        }
        tm_text(cx + 30, ry + 6, nm, C_TEXT, bg);
        if(COL_STAT)tm_text(col_x(0) + 8, ry + 6, r->status, C_DIM, bg);
        /* the busy columns: tinted by how much */
        uint32_t hb;
        hb = sel ? bg : heat(r->cpu < 0 ? 0 : r->cpu);
        gfx_fill_rect(col_x(1), ry, COL_CPU - 2, ROW_H - 1, hb);
        if (r->cpu >= 0) { ksnprintf(t, sizeof(t), "%d%%", r->cpu); tm_text(col_x(1) + COL_CPU - 10 - tm_width(t), ry + 6, t, C_TEXT, hb); }
        int memp = memory.total_kb ? (int)((uint64_t)r->mem * 100u / ((uint64_t)memory.total_kb * 1024u)) : 0;
        hb = sel ? bg : heat(memp > 100 ? 100 : memp);
        gfx_fill_rect(col_x(2), ry, COL_MEM - 2, ROW_H - 1, hb);
        if (r->mem) { human_kb(r->mem / 1024, t, sizeof(t)); tm_text(col_x(2) + COL_MEM - 10 - tm_width(t), ry + 6, t, C_TEXT, hb); }
        hb = sel ? bg : heat(0);
        gfx_fill_rect(col_x(3), ry, COL_DISK - 2, ROW_H - 1, hb);
        gfx_fill_rect(col_x(4), ry, COL_NET - 2, ROW_H - 1, hb);
        /* (the kernel does every disk and network transfer: shown on its first task) */
        if (r->kind == R_TASK && r->pid == 0) {
            rate(g_rd_rate + g_wr_rate, t, sizeof(t));
            tm_text(col_x(3) + COL_DISK - 10 - tm_width(t), ry + 6, t, C_TEXT, hb);
            bits(g_rx_rate + g_tx_rate, t, sizeof(t));
            tm_text(col_x(4) + COL_NET - 10 - tm_width(t), ry + 6, t, C_TEXT, hb);
        }
    }
}

/* a graph of a history: values 0..max, the newest at the right, filled under its line */
static void big_graph(int x, int y, int w, int h, const uint8_t* h8, const uint32_t* h32, uint32_t max, uint32_t color) {
    uint32_t fill = mix(C_PANEL, color, 36), grid = mix(C_PANEL, C_LINE, 100);
    frame(x, y, w, h, C_PANEL, C_LINE);
    for (int g = 1; g < 5; g++) gfx_fill_rect(x + 1, y + h * g / 5, w - 2, 1, grid);
    for (int g = 1; g < 6; g++) gfx_fill_rect(x + w * g / 6, y + 1, 1, h - 2, grid);
    if (!g_hist_n || !max) return;
    int prev = -1;
    for (int px = 1; px < w - 1; px++) {
        /* where px falls in the last HIST samples (the history fills from the right) */
        int pos = (px - 1) * (HIST - 1) * 16 / (w - 3);          /* 1/16ths of a sample */
        int i = pos / 16, fr = pos % 16, start = HIST - g_hist_n;
        if (i < start) continue;
        int k = i - start;
        uint32_t v0 = h8 ? h8[k] : h32[k];
        uint32_t v1 = k + 1 < g_hist_n ? (h8 ? h8[k + 1] : h32[k + 1]) : v0;
        uint32_t v = (v0 * (uint32_t)(16 - fr) + v1 * (uint32_t)fr) / 16;
        if (v > max) v = max;
        int vh = (int)((uint64_t)v * (uint32_t)(h - 2) / max);
        int top = y + h - 1 - vh;
        if (vh > 0) gfx_fill_rect(x + px, top, 1, vh, fill);
        if (prev >= 0) {
            int a = prev < top ? prev : top, b = prev < top ? top : prev;
            gfx_fill_rect(x + px, a, 1, b - a + 1, color);
        } else gfx_fill_rect(x + px, top, 1, 1, color);
        prev = top;
    }
}

static uint32_t net_max(void) {
    uint32_t m = 12500;                                    /* at least 100 Kbps across */
    for (int i = 0; i < g_hist_n; i++) if (g_net_hist[i] > m) m = g_net_hist[i];
    return m + m / 5;
}

static void dev_name(int d, char* out, int cap) {
    static const char* const N[D_COUNT] = { "CPU", "Memory", "Disk", "Ethernet", "GPU" };
    kstrlcpy(out, N[d], (size_t)cap);
}

static void dev_value(int d, char* out, int cap) {
    char a[24];
    meminfo_t m;
    switch (d) {
    case D_CPU: ksnprintf(out, (size_t)cap, "%d%%", last(g_cpu_hist)); break;
    case D_MEM:
        meminfo_get(&m);
        human_kb(m.used_kb - m.cached_kb, a, sizeof(a));
        ksnprintf(out, (size_t)cap, "%d%% %s", last(g_mem_hist),a);
        break;
    case D_DISK: ksnprintf(out, (size_t)cap, "%d%%", last(g_disk_hist)); break;
    case D_NET:
        if (!net_dev()) { kstrlcpy(out, "Not connected", (size_t)cap); break; }
        bits(g_tx_rate+g_rx_rate,out,cap);
        break;
    default: { gpu_t* g = gpu_active(); kstrlcpy(out, g ? g->driver : "framebuffer", (size_t)cap); }
    }
}

static uint32_t dev_color(int d) {
    const uint32_t C[D_COUNT] = { C_CPU, C_MEM, C_DISK, C_NET, C_GPU };
    return C[d];
}

/* a stat: its label small, its value under it */
static void stat(int x, int y, const char* label, const char* value) {
    tm_text(x, y, label, C_DIM, C_BG);
    tm_heading(x, y + 12, value, C_TEXT, C_BG);
}

static void kv(int x, int y, int kw, const char* k, const char* v) {
    tm_text(x, y, k, C_DIM, C_BG);
    tm_text(x + kw, y, v, C_TEXT, C_BG);
}

/* the chosen device's numbers: big ones in a grid (two or three to a row),
 * then the details as "key: value" lines - beside them in a wide window,
 * under them in a narrow one */
static int g_px, g_pw, g_sy, g_sc, g_kx, g_ky, g_kw;

static void layout_stats(int px, int pw, int sy, int big_rows) {
    g_px = px; g_pw = pw; g_sy = sy;
    g_sc = pw >= 720 ? 170 : pw >= 470 ? pw / 3 : pw / 2;
    if (pw >= 720) { g_kx = px + pw - 260; g_ky = sy; }
    else { g_kx = px; g_ky = sy + big_rows * 44 + 6; }
    g_kw = 172;
}
static void bigstat(int col, int row, const char* label, const char* value) {
    int per = g_pw >= 720 ? 3 : g_pw >= 470 ? 3 : 2;
    if (col >= per) { row += col / per; col %= per; }
    stat(g_px + col * g_sc, g_sy + row * 44, label, value);
}
static void kvl(int i, const char* k, const char* v) {
    if (g_ky + i * 14 > g_win.y + g_win.h - 16) return;        /* (no room left) */
    kv(g_kx, g_ky + i * 14, g_kw, k, v);
}

static void draw_performance(void) {
    int cx = content_x(), cw = content_w(), ty = perf_top();
    page_title(_("Performance"));
    /* the tiles */
    for (int d = 0; d < D_COUNT; d++) {
        int y = ty + d * 62, on = d == g_dev;
        uint32_t bg = on ? C_HOVER : C_BG;
        kbnav_add(&g_nav, cx, y, 196, 56);
        gfx_fill_rect(cx, y, 196, 56, bg);
        if (on) gfx_fill_rect(cx, y + 14, 3, 28, C_ACCENT);
        uint32_t c = dev_color(d);
        if (d == D_GPU) frame(cx + 8, y + 8, 64, 40, C_PANEL, C_LINE);
        else if (d == D_NET) big_graph(cx + 8, y + 8, 64, 40, NULL, g_net_hist, net_max(), c);
        else big_graph(cx + 8, y + 8, 64, 40, d == D_CPU ? g_cpu_hist : d == D_MEM ? g_mem_hist : g_disk_hist, NULL, 100, c);
        char n[24], v[48];
        dev_name(d, n, sizeof(n));
        dev_value(d, v, sizeof(v));
        if ((int)strlen(v) > 14) {v[11]=v[12]=v[13]='.';v[14]=0;}
        tm_text(cx + 80, y + 16, n, C_TEXT, bg);
        tm_text(cx + 80, y + 32, v, C_DIM, bg);
    }

    /* the one chosen: its name and model, its graph, its numbers */
    int px = cx + 210, pw = cw - 210;
    int gy = ty + 36, gh = (g_win.y + g_win.h - gy) * 2 / 5;
    if (gh < 70) gh = 70;
    char line[96], a[24], b[24];
    char nm[24];
    dev_name(g_dev, nm, sizeof(nm));
    tm_heading(px, ty - 6, nm, C_TEXT, C_BG);
    const sysinfo_t* si = sysinfo_get();
    const char* sub = "";
    meminfo_t m;
    meminfo_get(&m);
    char subbuf[64];
    if (g_dev == D_CPU) sub = si && si->cpu_brand[0] ? si->cpu_brand : "Processor";
    else if (g_dev == D_MEM) { human_kb(m.installed_kb ? m.installed_kb : m.total_kb, a, sizeof(a)); ksnprintf(subbuf, sizeof(subbuf), "%s installed", a); sub = subbuf; }
    else if (g_dev == D_NET) sub = net_dev() ? net_dev()->model : "No adapter";
    else if (g_dev == D_GPU) { gpu_t* g = gpu_active(); sub = g ? g->name : "VBE / UEFI framebuffer"; }
    else sub = "All disks";
    char subt[64];
    kstrlcpy(subt, sub, sizeof(subt));
    while (subt[0] == ' ') memmove(subt, subt + 1, strlen(subt));
    int maxc = (pw - 110) / 8;
    if (maxc > 0 && (int)strlen(subt) > maxc) subt[maxc] = 0;
    tm_text(px + pw - tm_width(subt), ty + 2, subt, C_DIM, C_BG);

    uint32_t col = dev_color(g_dev);
    int sy = gy + gh + 22;
    uint32_t up = timer_ms() / 1000;
    if (g_dev == D_CPU) {
        int cores = cpu_count() < 1 ? 1 : cpu_count();
        if (g_cores_view && cores > 1) {
            tm_text(px, gy - 14, _("Logical processors"), C_DIM, C_BG);
            int n = cores > MAX_CORE ? MAX_CORE : cores, per = n <= 4 ? n : (n + 1) / 2, rows = (n + per - 1) / per;
            int cw2 = (pw - (per - 1) * 6) / per, ch2 = (gh - (rows - 1) * 6) / rows;
            for (int i = 0; i < n; i++) big_graph(px + (i % per) * (cw2 + 6), gy + (i / per) * (ch2 + 6), cw2, ch2, g_core_hist[i], NULL, 100, col);
        } else {
            tm_text(px, gy - 14, "% Utilization", C_DIM, C_BG);
            tm_text(px + pw - 32, gy - 14, "100%", C_DIM, C_BG);
            big_graph(px, gy, pw, gh, g_cpu_hist, NULL, 100, col);
        }
        if (cores > 1) tm_text(px, gy + gh + 2, g_cores_view ? "Click the graph: overall" : "Click the graph: each core", TC(0x707070), C_BG);
        int threads = 0;
        snap_tasks();
        for (int i = 0; i < g_ntasks; i++) if (g_tasks[i].state != TASK_UNUSED) threads++;
        layout_stats(px, pw, sy, 2);
        ksnprintf(a, sizeof(a), "%d%%", last(g_cpu_hist));
        bigstat(0, 0, _("Utilization"), a);
        ksnprintf(a, sizeof(a), "%d", app_count());
        bigstat(1, 0, _("Apps"), a);
        ksnprintf(a, sizeof(a), "%d", threads);
        bigstat(2, 0, _("Tasks"), a);
        ksnprintf(a, sizeof(a), "%u:%02u:%02u:%02u", up / 86400, up / 3600 % 24, up / 60 % 60, up % 60);
        bigstat(0, g_pw >= 470 ? 1 : 2, _("Up time"), a);
        uint32_t l1, l2, l3;
        caches(&l1, &l2, &l3);
        if (g_pw < 470) g_ky += 44;
        int k = 0;
        ksnprintf(a, sizeof(a), "%d", smp_phys_cores() > 0 ? smp_phys_cores() : cores);
        kvl(k++, _("Cores:"), a);
        ksnprintf(a, sizeof(a), "%d", cores);
        kvl(k++, _("Logical processors:"), a);
        kvl(k++, _("Virtual machine:"), in_vm() ? "Yes" : "No");
        if (l1) { ksnprintf(a, sizeof(a), "%u KB", l1); kvl(k++, _("L1 cache:"), a); }
        if (l2) { human_kb(l2, a, sizeof(a)); kvl(k++, _("L2 cache:"), a); }
        if (l3) { human_kb(l3, a, sizeof(a)); kvl(k++, _("L3 cache:"), a); }
        int temp = sensors_cpu_temp();
        if (temp != SENSOR_NONE) { ksnprintf(a, sizeof(a), "%d C", temp); kvl(k++, _("Temperature:"), a); }
    } else if (g_dev == D_MEM) {
        tm_text(px, gy - 14, _("Memory usage"), C_DIM, C_BG);
        human_kb(m.total_kb, a, sizeof(a));
        tm_text(px + pw - tm_width(a), gy - 14, a, C_DIM, C_BG);
        big_graph(px, gy, pw, gh - 34, g_mem_hist, NULL, 100, col);
        /* the composition: in use | cached files | free */
        int by = gy + gh - 18;
        tm_text(px, by - 12, _("Memory composition"), C_DIM, C_BG);
        frame(px, by, pw, 18, C_PANEL, col);
        int u = m.total_kb ? (int)((uint64_t)(m.used_kb - m.cached_kb) * (uint32_t)(pw - 2) / m.total_kb) : 0;
        int kc = m.total_kb ? (int)((uint64_t)m.cached_kb * (uint32_t)(pw - 2) / m.total_kb) : 0;
        gfx_fill_rect(px + 1, by + 1, u, 16, mix(C_PANEL, col, 150));
        gfx_fill_rect(px + 1 + u, by + 1, kc, 16, mix(C_PANEL, col, 60));
        zswap_stats_t zs;
        zswap_stats(&zs);
        layout_stats(px, pw, sy, 2);
        human_kb(m.used_kb - m.cached_kb, a, sizeof(a));
        bigstat(0, 0, _("In use"), a);
        human_kb(m.avail_kb, a, sizeof(a));
        bigstat(1, 0, _("Available"), a);
        human_kb(m.cached_kb, a, sizeof(a));
        bigstat(2, 0, _("Cached"), a);
        int k = 0;
        human_kb(zs.pages * 4u, a, sizeof(a));
        human_kb((uint32_t)(zs.bytes / 1024u), b, sizeof(b));
        ksnprintf(line, sizeof(line), "%s in %s", a, b);
        kvl(k++, _("Compressed:"), line);
        human_kb(m.kernel_kb, a, sizeof(a));
        kvl(k++, _("Kernel:"), a);
        human_kb(m.files_kb, a, sizeof(a));
        kvl(k++, _("File data:"), a);
        human_kb(m.free_kb, a, sizeof(a));
        kvl(k++, _("Free:"), a);
        human_kb(m.largest_kb, a, sizeof(a));
        kvl(k++, _("Largest block:"), a);
        human_kb(zs.disk_pages * 4u, a, sizeof(a));
        kvl(k++, _("Swapped out:"), zswap_disk_bytes() ? a : "no swap area");
    } else if (g_dev == D_DISK) {
        tm_text(px, gy - 14, _("Active time"), C_DIM, C_BG);
        tm_text(px + pw - 32, gy - 14, "100%", C_DIM, C_BG);
        big_graph(px, gy, pw, gh, g_disk_hist, NULL, 100, col);
        layout_stats(px, pw, sy, 2);
        ksnprintf(a, sizeof(a), "%d%%", last(g_disk_hist));
        bigstat(0, 0, _("Active time"), a);
        rate(g_rd_rate, a, sizeof(a));
        bigstat(1, 0, _("Read speed"), a);
        rate(g_wr_rate, a, sizeof(a));
        bigstat(2, 0, _("Write speed"), a);
        uint64_t rd, wr;
        disk_io_stats(&rd, &wr, NULL);
        int k = 0;
        human_bytes64(rd, a, sizeof(a));
        kvl(k++, _("Read since start:"), a);
        human_bytes64(wr, a, sizeof(a));
        kvl(k++, _("Written:"), a);
        for (int i = 0; i < BLOCKDEV_MAX && k < 8; i++) {
            blockdev_t* bd = blockdev_get(i);
            if (!bd || !bd->present) continue;
            human_bytes64((uint64_t)bd->sectors * (bd->sector_size ? bd->sector_size : 512u), a, sizeof(a));
            char model[20];
            kstrlcpy(model, bd->model, sizeof(model));
            ksnprintf(line, sizeof(line), "%s%s", a, bd->system ? ", system" : "");
            char key[24];
            ksnprintf(key, sizeof(key), "%s:", bd->name);
            kvl(k++, key, line);
        }
    } else if (g_dev == D_NET) {
        netdev_t* nd = net_dev();
        tm_text(px, gy - 14, _("Throughput"), C_DIM, C_BG);
        bits(net_max(), a, sizeof(a));
        tm_text(px + pw - tm_width(a), gy - 14, a, C_DIM, C_BG);
        big_graph(px, gy, pw, gh, NULL, g_net_hist, net_max(), col);
        layout_stats(px, pw, sy, 1);
        bits(g_tx_rate, a, sizeof(a));
        bigstat(0, 0, _("Send"), a);
        bits(g_rx_rate, a, sizeof(a));
        bigstat(1, 0, _("Receive"), a);
        netif_t* nif = net_if();
        int k = 0;
        kvl(k++, _("Adapter name:"), nd ? nd->ifname : "none");
        char ip[16] = "-";
        if (nd && nif->configured) ip4_to_str(nif->ip, ip);
        kvl(k++, _("IPv4 address:"), ip);
        if (nd) {
            ksnprintf(a, sizeof(a), "%02x:%02x:%02x:%02x:%02x:%02x", nd->mac[0], nd->mac[1], nd->mac[2], nd->mac[3], nd->mac[4], nd->mac[5]);
            kvl(k++, _("MAC address:"), a);
            human_kb(nd->tx_bytes / 1024, a, sizeof(a));
            kvl(k++, _("Sent:"), a);
            human_kb(nd->rx_bytes / 1024, a, sizeof(a));
            kvl(k++, _("Received:"), a);
            ksnprintf(a, sizeof(a), "%u errors, %u dropped", nd->rx_errors + nd->tx_errors, nd->rx_dropped);
            kvl(k++, _("Problems:"), a);
        }
    } else {
        gpu_t* g = gpu_active();
        const fb_info_t* fi = fb_info();
        frame(px, gy, pw, gh, C_PANEL, col);
        tm_text(px + 16, gy + gh / 2 - 4, _("Its load is not measured."), C_DIM, C_PANEL);
        layout_stats(px, pw, sy, 0);
        g_kx = px; g_ky = sy;
        int k = 0;
        kvl(k++, _("Adapter:"), g ? g->name : "VBE / UEFI framebuffer");
        kvl(k++, _("Driver:"), g ? g->driver : "none (firmware's mode)");
        if (fi) {
            ksnprintf(a, sizeof(a), "%u x %u, %u-bit", fi->width, fi->height, fi->bpp);
            kvl(k++, _("Screen:"), a);
            human_kb(fi->pitch * fi->height / 1024, a, sizeof(a));
            kvl(k++, _("Framebuffer:"), a);
        }
        kvl(k++, _("Sound:"), audio_device_name());
    }
}

static void draw_details(void) {
    int cx = content_x(), cw = content_w();
    page_title(_("Details"));
    if (!w10()) tm_text(cx + cw - 39 * 8, head_y() + 10, _("Right-click a task: its priority"), C_DIM, C_BG);
    snap_tasks();
    gfx_fill_rect(cx, table_y() + 12, cw, ROW_H, C_PANEL);
    int compact=cw<580;
    tm_text(cx + 8, table_y() + 18, compact?_("Name              PID Status      CPU  CPU time Prio"):_("Name                    PID  Status     Core  CPU   CPU time  Prio"), C_DIM, C_PANEL);
    int row = 0, maxr = rows_max(), skip = g_scroll;
    char line[128];
    for (int i = 0; i < g_ntasks && row < maxr; i++) {
        if (g_tasks[i].state == TASK_UNUSED) continue;
        if (skip > 0) { skip--; continue; }
        uint32_t s = g_tasks[i].ticks_total / 100;
        char st[16], core[8];
        kstrlcpy(st, g_tasks[i].state == TASK_AWAY ? "Running" : task_state_str(g_tasks[i].state), sizeof(st));
        if (g_tasks[i].cpu >= 0) ksnprintf(core, sizeof(core), "%d", g_tasks[i].cpu); else kstrlcpy(core, "-", sizeof(core));
        ksnprintf(line, sizeof(line), "%-23s %-4u %-10s %-4s %3u%%  %3u:%02u:%02u  %3d", g_tasks[i].name, g_tasks[i].pid, st, core,
                  g_tasks[i].cpu_pct, s / 3600, s / 60 % 60, s % 60, g_tasks[i].nice);
        if(compact)ksnprintf(line,sizeof(line),"%-17.17s %-4u %-10.10s %3u%% %3u:%02u:%02u %3d",g_tasks[i].name,g_tasks[i].pid,st,g_tasks[i].cpu_pct,s/3600,s/60%60,s%60,g_tasks[i].nice);
        int limit=(cw-16)/8;if(limit>0&&limit<(int)sizeof(line))line[limit]=0;
        int ry = rows_y() + row * ROW_H;
        if (g_tasks[i].cpu_pct) gfx_fill_rect(cx, ry, cw, ROW_H - 1, heat((int)g_tasks[i].cpu_pct));
        tm_text(cx + 8, ry + 6, line, g_tasks[i].cpu_pct >= 50 ? g_pal.hot_text : C_TEXT, g_tasks[i].cpu_pct ? heat((int)g_tasks[i].cpu_pct) : C_BG);
        row++;
    }
}

static void draw_users(void) {
    int cx = content_x(), cw = content_w();
    page_title(_("Users"));
    gfx_fill_rect(cx, table_y() + 12, cw, ROW_H, C_PANEL);
    tm_text(cx + 8, table_y() + 18, _("User"), C_DIM, C_PANEL);
    tm_text(cx + cw - 300, table_y() + 18, _("Status"), C_DIM, C_PANEL);
    tm_text(cx + cw - 180, table_y() + 18, _("CPU"), C_DIM, C_PANEL);
    tm_text(cx + cw - 100, table_y() + 18, _("Memory"), C_DIM, C_PANEL);
    int ry = rows_y();
    frame(cx + 10, ry + 2, 16, 16, TC(0x405478), TC(0x90B8F0));
    tm_text(cx + 14, ry + 6, "b", C_TEXT, TC(0x405478));
    tm_text(cx + 34, ry + 6, _("banana (you)"), C_TEXT, C_BG);
    char a[24];
    ksnprintf(a, sizeof(a), "%d app%s", g_napps, g_napps == 1 ? "" : "s");
    tm_text(cx + cw - 300, ry + 6, a, C_DIM, C_BG);
    ksnprintf(a, sizeof(a), "%d%%", last(g_cpu_hist));
    gfx_fill_rect(cx + cw - 186, ry, 72, ROW_H - 1, heat(last(g_cpu_hist)));
    tm_text(cx + cw - 180, ry + 6, a, C_TEXT, heat(last(g_cpu_hist)));
    meminfo_t m;
    meminfo_get(&m);
    human_kb(m.used_kb - m.cached_kb, a, sizeof(a));
    gfx_fill_rect(cx + cw - 106, ry, 100, ROW_H - 1, heat(last(g_mem_hist)));
    tm_text(cx + cw - 100, ry + 6, a, C_TEXT, heat(last(g_mem_hist)));
    tm_text(cx + 34, ry + 30, _("Banana OS has one user; SSH sessions run as it too."), C_DIM, C_BG);
}

/* ══ the classic Windows look (XP / 7): grey, tabs, green meters ═════════ */

#define K_FACE   0x00D4D0C8u
#define K_HI     0x00FFFFFFu
#define K_SHADOW 0x00808080u
#define K_DARK   0x00404040u
#define K_TEXT   0x00000000u
#define K_SEL    0x000A246Au
#define K_GREEN  0x0000FF00u
#define K_GRID   0x00008040u
#define K_YELLOW 0x00FFFF00u
#define K_ROW_H  16

enum { K_APPS = 0, K_PROCS, K_PERF, K_NET, K_USERS, K_TABS };
static const char* const K_TAB_NAMES[K_TABS] = { "Applications", "Processes", "Performance", "Networking", "Users" };
static int g_ktab, g_ksel = -1;

static void k_raised(int x, int y, int w, int h) {
    gfx_fill_rect(x, y, w, h, K_FACE);
    gfx_fill_rect(x, y, w, 1, K_HI);
    gfx_fill_rect(x, y, 1, h, K_HI);
    gfx_fill_rect(x, y + h - 1, w, 1, K_DARK);
    gfx_fill_rect(x + w - 1, y, 1, h, K_DARK);
    gfx_fill_rect(x + 1, y + h - 2, w - 2, 1, K_SHADOW);
    gfx_fill_rect(x + w - 2, y + 1, 1, h - 2, K_SHADOW);
}
static void k_sunken(int x, int y, int w, int h, uint32_t fill) {
    gfx_fill_rect(x, y, w, h, fill);
    gfx_fill_rect(x, y, w, 1, K_SHADOW);
    gfx_fill_rect(x, y, 1, h, K_SHADOW);
    gfx_fill_rect(x + 1, y + 1, w - 2, 1, K_DARK);
    gfx_fill_rect(x + 1, y + 1, 1, h - 2, K_DARK);
    gfx_fill_rect(x, y + h - 1, w, 1, K_HI);
    gfx_fill_rect(x + w - 1, y, 1, h, K_HI);
}
static void k_button(int x, int y, int w, const char* label, int enabled) {
    kbnav_add(&g_nav, x, y, w, 22);
    k_raised(x, y, w, 22);
    tm_text(x + (w - tm_width(label)) / 2, y + 7, label, enabled ? K_TEXT : K_SHADOW, K_FACE);
}
static void k_group(int x, int y, int w, int h, const char* title) {
    gfx_fill_rect(x, y + 4, w, 1, K_SHADOW);
    gfx_fill_rect(x, y + 4, 1, h - 4, K_SHADOW);
    gfx_fill_rect(x, y + h - 1, w, 1, K_HI);
    gfx_fill_rect(x + w - 1, y + 4, 1, h - 4, K_HI);
    gfx_fill_rect(x + 6, y, (int)strlen(title) * 8 + 6, 9, K_FACE);
    tm_text(x + 9, y, title, K_TEXT, K_FACE);
}

/* the page's box, the buttons' row */
static int k_page_x(void) { return g_win.x + 8; }
static int k_page_y(void) { return g_win.y + TITLE_H + 44; }
static int k_page_w(void) { return g_win.w - 16; }
static int k_page_h(void) { return g_win.h - TITLE_H - 44 - 30; }
static int k_list_bottom(void) { return k_page_y() + k_page_h() - 40; }
static int k_tab_x(int i, int* w) {
    int x = g_win.x + 10;
    for (int k = 0; k < i; k++) x += (int)strlen(_(K_TAB_NAMES[k])) * 8 + 16;
    *w = (int)strlen(_(K_TAB_NAMES[i])) * 8 + 16;
    return x;
}

/* the list rows of Applications and Processes: windows and apps, then (Processes) the tasks */
static int k_rows(void) {
    int n = 0;
    for (int i = 0; i < g_nrows; i++)
        if (g_rows[i].kind == R_WINDOW || g_rows[i].kind == R_APP || (g_ktab == K_PROCS && g_rows[i].kind == R_TASK)) n++;
    return n;
}
static int k_row(int k) {                              /* the k-th listed row's index in g_rows */
    for (int i = 0; i < g_nrows; i++)
        if (g_rows[i].kind == R_WINDOW || g_rows[i].kind == R_APP || (g_ktab == K_PROCS && g_rows[i].kind == R_TASK))
            if (k-- == 0) return i;
    return -1;
}
static int k_list_max(void) { int n = (k_list_bottom() - (k_page_y() + 30)) / K_ROW_H; return n < 1 ? 1 : n; }

static void k_meter(int x, int y, int w, int h, int pct, const char* label, uint32_t c) {
    k_sunken(x, y, w, h, 0x00000000u);
    int bars = (h - 26) / 3, lit = bars * pct / 100;
    for (int i = 0; i < bars; i++) {
        uint32_t on = i < lit ? c : 0x00004000u;
        int by = y + h - 20 - (i + 1) * 3;
        gfx_fill_rect(x + 10, by, w / 2 - 12, 2, on);
        gfx_fill_rect(x + w / 2 + 2, by, w / 2 - 12, 2, on);
    }
    tm_text(x + (w - tm_width(label)) / 2, y + h - 13, label, c, 0x00000000u);
}

static void k_history(int x, int y, int w, int h, const uint8_t* hist, const uint32_t* h32, uint32_t max, uint32_t c) {
    k_sunken(x, y, w, h, 0x00000000u);
    for (int gx = x + 2 + (12 - (int)(timer_ms() / 1000 % 12)); gx < x + w - 2; gx += 12) gfx_fill_rect(gx, y + 2, 1, h - 4, K_GRID);
    for (int gy = y + 2 + 11; gy < y + h - 2; gy += 12) gfx_fill_rect(x + 2, gy, w - 4, 1, K_GRID);
    if (!g_hist_n || !max) return;
    int prev = -1;
    for (int i = 0; i < g_hist_n; i++) {
        int px = x + w - 3 - (g_hist_n - 1 - i) * (w - 6) / (HIST - 1);
        uint32_t v = hist ? hist[i] : h32[i];
        if (v > max) v = max;
        int py = y + h - 3 - (int)((uint64_t)v * (uint32_t)(h - 6) / max);
        if (prev >= 0) {
            int ppx = x + w - 3 - (g_hist_n - i) * (w - 6) / (HIST - 1);
            for (int sx = ppx; sx <= px; sx++) {
                int t = px > ppx ? (sx - ppx) * 256 / (px - ppx) : 256;
                int sy = prev + (py - prev) * t / 256;
                gfx_fill_rect(sx, sy, 1, 2, c);
            }
        }
        prev = py;
    }
}

static void classic_draw(void) {
    int x = g_win.x, y = g_win.y, W = g_win.w, H = g_win.h;
    char a[48], b[48], line[96];
    /* the window: grey, a blue title bar */
    k_raised(x, y, W, H);
    for (int i = 0; i < W - 6; i++) gfx_fill_rect(x + 3 + i, y + 3, 1, TITLE_H - 2, mix(0x000A246Au, 0x00A6CAF0u, i * 256 / (W - 6)));
    tm_text(x + 8, y + 8, _("Windows Task Manager"), 0x00FFFFFFu, 0x000A246Au);
    win_draw_buttons(&g_win, 4, 12);
    static const char* const MENU[5] = { "File", "Options", "View", "Windows", "Help" };
    for (int i = 0, mx = x + 8; i < 5; mx += (int)strlen(_(MENU[i])) * 8 + 16, i++) tm_text(mx, y + TITLE_H + 6, _(MENU[i]), K_TEXT, K_FACE);
    /* the tabs, the page under them */
    int px = k_page_x(), py = k_page_y(), pw = k_page_w(), ph = k_page_h();
    k_raised(px, py, pw, ph);
    for (int i = 0; i < K_TABS; i++) {
        int tw, tx = k_tab_x(i, &tw), on = i == g_ktab, ty = py - 20 + (on ? -2 : 0);
        kbnav_add(&g_nav, tx, py - 20, tw, 20);
        k_raised(tx, ty, tw, 22 + (on ? 2 : 0));
        if (on) gfx_fill_rect(tx + 1, py - 1, tw - 2, 3, K_FACE);
        tm_text(tx + 8, ty + 7, _(K_TAB_NAMES[i]), K_TEXT, K_FACE);
    }
    meminfo_t m;
    meminfo_get(&m);
    int lx = px + 8, lw = pw - 16, ly = py + 10;
    if (g_ktab == K_APPS || g_ktab == K_PROCS) {
        k_sunken(lx, ly, lw, k_list_bottom() - ly, 0x00FFFFFFu);
        k_raised(lx + 2, ly + 2, lw - 4, 18);
        if (g_ktab == K_APPS) {
            tm_text(lx + 8, ly + 7, _("Task"), K_TEXT, K_FACE);
            tm_text(lx + lw - 110, ly + 7, _("Status"), K_TEXT, K_FACE);
        } else {
            tm_text(lx + 8, ly + 7, _("Image Name"), K_TEXT, K_FACE);
            tm_text(lx + lw - 330, ly + 7, _("User Name"), K_TEXT, K_FACE);
            tm_text(lx + lw - 220, ly + 7, _("PID"), K_TEXT, K_FACE);
            tm_text(lx + lw - 160, ly + 7, _("CPU"), K_TEXT, K_FACE);
            tm_text(lx + lw - 100, ly + 7, _("Mem Usage"), K_TEXT, K_FACE);
        }
        int n = k_rows(), maxr = k_list_max();
        if (g_scroll > n - maxr) g_scroll = n - maxr;
        if (g_scroll < 0) g_scroll = 0;
        for (int r = 0; r < maxr && r + g_scroll < n; r++) {
            int ri = k_row(r + g_scroll), ry = ly + 22 + r * K_ROW_H, sel = ri == g_ksel;
            row_t* rw = &g_rows[ri];
            uint32_t bg = sel ? K_SEL : 0x00FFFFFFu, fg = sel ? 0x00FFFFFFu : K_TEXT;
            kbnav_add(&g_nav, lx + 2, ry, lw - 4, K_ROW_H);
            if (sel) gfx_fill_rect(lx + 2, ry, lw - 4, K_ROW_H, bg);
            char nm[40];
            kstrlcpy(nm, rw->text, sizeof(nm));
            if (g_ktab == K_APPS) {
                if ((int)strlen(nm) > (lw - 140) / 8) nm[(lw - 140) / 8] = 0;
                gfx_fill_rect(lx + 6, ry + 4, 8, 8, rw->kind == R_WINDOW ? 0x003A7BD5u : 0x00D0A030u);
                tm_text(lx + 20, ry + 4, nm, fg, bg);
                tm_text(lx + lw - 110, ry + 4, _("Running"), fg, bg);
            } else {
                if (rw->kind == R_TASK) ksnprintf(a, sizeof(a), "%s", nm);
                else ksnprintf(a, sizeof(a), "%s.exe", nm);
                for (char* p = a; *p; p++) if (*p == ' ') *p = '_';
                if ((int)strlen(a) > (lw - 350) / 8) a[(lw - 350) / 8] = 0;
                tm_text(lx + 8, ry + 4, a, fg, bg);
                tm_text(lx + lw - 330, ry + 4, rw->kind == R_TASK ? "SYSTEM" : "banana", fg, bg);
                ksnprintf(b, sizeof(b), "%d", rw->pid);
                tm_text(lx + lw - 220, ry + 4, rw->kind == R_WINDOW ? "" : b, fg, bg);
                ksnprintf(b, sizeof(b), "%02d", rw->cpu < 0 ? 0 : rw->cpu > 99 ? 99 : rw->cpu);
                tm_text(lx + lw - 160, ry + 4, b, fg, bg);
                if (rw->mem) { ksnprintf(b, sizeof(b), "%u K", rw->mem / 1024); tm_text(lx + lw - 100, ry + 4, b, fg, bg); }
            }
        }
        int by = k_list_bottom() + 10, sel_ok = g_ksel >= 0 && g_ksel < g_nrows;
        if (g_ktab == K_APPS) {
            k_button(px + pw - 300, by, 92, _("End Task"), sel_ok);
            k_button(px + pw - 200, by, 92, _("Switch To"), sel_ok && g_rows[g_ksel].kind == R_WINDOW);
            k_button(px + pw - 100, by, 92, _("New Task..."), 0);
        } else {
            tm_text(px + 12, by + 7, _("Show processes from all users"), K_TEXT, K_FACE);
            k_button(px + pw - 120, by, 112, _("End Process"), sel_ok && g_rows[g_ksel].kind != R_TASK);
        }
    } else if (g_ktab == K_PERF) {
        int col = 100, gx = lx + col + 10, gw = lw - col - 10, gh = (ph - 120) / 2 - 18;
        if (gh < 40) gh = 40;
        int cpu = last(g_cpu_hist), mem = last(g_mem_hist);
        k_group(lx, ly, col, gh + 18, _("CPU Usage"));
        ksnprintf(a, sizeof(a), "%d %%", cpu);
        k_meter(lx + 8, ly + 14, col - 16, gh - 2, cpu, a, K_GREEN);
        k_group(gx, ly, gw, gh + 18, _("CPU Usage History"));
        int cores = cpu_count() < 1 ? 1 : cpu_count();
        if (cores > MAX_CORE) cores = MAX_CORE;
        int each = (gw - 16 - (cores - 1) * 4) / cores;
        for (int c = 0; c < cores; c++) k_history(gx + 8 + c * (each + 4), ly + 14, each, gh - 2, g_core_hist[c], NULL, 100, K_GREEN);
        int my2 = ly + gh + 24;
        k_group(lx, my2, col, gh + 18, _("Mem Usage"));
        ksnprintf(a, sizeof(a), "%u MB", (m.used_kb - m.cached_kb) / 1024);
        k_meter(lx + 8, my2 + 14, col - 16, gh - 2, mem, a, K_GREEN);
        k_group(gx, my2, gw, gh + 18, _("Physical Memory Usage History"));
        k_history(gx + 8, my2 + 14, gw - 16, gh - 2, g_mem_hist, NULL, 100, K_YELLOW);
        /* the numbers */
        int ty = my2 + gh + 26, cw3 = (lw - 16) / 3;
        snap_tasks();
        int threads = 0;
        for (int i = 0; i < g_ntasks; i++) if (g_tasks[i].state != TASK_UNUSED) threads++;
        uint32_t up = timer_ms() / 1000;
        k_group(lx, ty, cw3, 74, _("Totals"));
        ksnprintf(line, sizeof(line), "Tasks       %6d", threads);       tm_text(lx + 8, ty + 16, line, K_TEXT, K_FACE);
        ksnprintf(line, sizeof(line), "Apps        %6d", app_count());   tm_text(lx + 8, ty + 30, line, K_TEXT, K_FACE);
        ksnprintf(line, sizeof(line), "Windows     %6d", g_napps - app_count() > 0 ? g_napps - app_count() : 0); tm_text(lx + 8, ty + 44, line, K_TEXT, K_FACE);
        ksnprintf(line, sizeof(line), "Up Time %u:%02u:%02u", up / 3600, up / 60 % 60, up % 60); tm_text(lx + 8, ty + 58, line, K_TEXT, K_FACE);
        k_group(lx + cw3 + 8, ty, cw3, 74, _("Physical Memory (K)"));
        ksnprintf(line, sizeof(line), "Total     %9u", m.total_kb);  tm_text(lx + cw3 + 16, ty + 16, line, K_TEXT, K_FACE);
        ksnprintf(line, sizeof(line), "Available %9u", m.avail_kb);  tm_text(lx + cw3 + 16, ty + 30, line, K_TEXT, K_FACE);
        ksnprintf(line, sizeof(line), "Sys Cache %9u", m.cached_kb); tm_text(lx + cw3 + 16, ty + 44, line, K_TEXT, K_FACE);
        k_group(lx + 2 * (cw3 + 8), ty, cw3, 74, _("Kernel Memory (K)"));
        ksnprintf(line, sizeof(line), "Total     %9u", m.kernel_kb); tm_text(lx + 2 * (cw3 + 8) + 8, ty + 16, line, K_TEXT, K_FACE);
        ksnprintf(line, sizeof(line), "Files     %9u", m.files_kb);  tm_text(lx + 2 * (cw3 + 8) + 8, ty + 30, line, K_TEXT, K_FACE);
        ksnprintf(line, sizeof(line), "Free      %9u", m.free_kb);   tm_text(lx + 2 * (cw3 + 8) + 8, ty + 44, line, K_TEXT, K_FACE);
    } else if (g_ktab == K_NET) {
        netdev_t* nd = net_dev();
        int gh = ph - 130;
        k_group(lx, ly, lw, gh, nd ? nd->ifname : "No network adapter");
        uint32_t max = 12500000u;                       /* % of 100 Mbps */
        k_history(lx + 8, ly + 14, lw - 16, gh - 22, NULL, g_net_hist, max / 20, K_GREEN);
        tm_text(lx + 12, ly + 18, "5 %", K_GREEN, 0x00000000u);
        int ty = ly + gh + 8;
        k_sunken(lx, ty, lw, 60, 0x00FFFFFFu);
        k_raised(lx + 2, ty + 2, lw - 4, 18);
        tm_text(lx + 8, ty + 7, _("Adapter Name"), K_TEXT, K_FACE);
        tm_text(lx + lw - 330, ty + 7, _("Network Utilization"), K_TEXT, K_FACE);
        tm_text(lx + lw - 160, ty + 7, _("Link Speed"), K_TEXT, K_FACE);
        tm_text(lx + lw - 70, ty + 7, _("State"), K_TEXT, K_FACE);
        if (nd) {
            kstrlcpy(a, nd->model ? nd->model : nd->ifname, 28);
            tm_text(lx + 8, ty + 26, a, K_TEXT, 0x00FFFFFFu);
            uint32_t pc100 = (g_rx_rate + g_tx_rate) / 1250u;      /* hundredths of a % of 100 Mbps */
            ksnprintf(b, sizeof(b), "%u.%02u %%", pc100 / 100, pc100 % 100);
            tm_text(lx + lw - 330, ty + 26, b, K_TEXT, 0x00FFFFFFu);
            tm_text(lx + lw - 160, ty + 26, _("100 Mbps"), K_TEXT, 0x00FFFFFFu);
            tm_text(lx + lw - 70, ty + 26, net_if()->configured ? "Connected" : "Waiting", K_TEXT, 0x00FFFFFFu);
        }
    } else {
        k_sunken(lx, ly, lw, ph - 30, 0x00FFFFFFu);
        k_raised(lx + 2, ly + 2, lw - 4, 18);
        static const char* const UH[5] = { "User", "ID", "Status", "Client Name", "Session" };
        static const int UX[5] = { 8, 140, 190, 280, 400 };
        for (int i = 0; i < 5; i++) tm_text(lx + UX[i], ly + 7, _(UH[i]), K_TEXT, K_FACE);
        static const char* const UV[5] = { "banana", "0", "Active", "", "Console" };
        for (int i = 0; i < 5; i++) tm_text(lx + UX[i], ly + 26, UV[i], K_TEXT, 0x00FFFFFFu);
    }
    /* the status bar */
    int sy = y + H - 22;
    int nproc = 0;
    for (int i = 0; i < g_nrows; i++) if (g_rows[i].kind == R_APP || g_rows[i].kind == R_TASK) nproc++;
    ksnprintf(a, sizeof(a), "Processes: %d", nproc);
    k_sunken(x + 4, sy, 140, 18, K_FACE);
    tm_text(x + 10, sy + 5, a, K_TEXT, K_FACE);
    ksnprintf(a, sizeof(a), "CPU Usage: %d%%", last(g_cpu_hist));
    k_sunken(x + 148, sy, 140, 18, K_FACE);
    tm_text(x + 154, sy + 5, a, K_TEXT, K_FACE);
    ksnprintf(a, sizeof(a), "Physical Memory: %d%%", last(g_mem_hist));
    k_sunken(x + 292, sy, W - 300, 18, K_FACE);
    tm_text(x + 298, sy + 5, g_status[0] ? g_status : a, K_TEXT, K_FACE);
    gfx_draw_grip(x + W, y + H);
    kbnav_draw(&g_nav, K_SEL);
}

static void classic_click(int mx, int my) {
    int py = k_page_y(), px = k_page_x(), pw = k_page_w();
    if (my >= py - 22 && my < py) {
        for (int i = 0; i < K_TABS; i++) {
            int tw, tx = k_tab_x(i, &tw);
            if (mx >= tx && mx < tx + tw) { g_ktab = i; g_ksel = -1; g_scroll = 0; g_status[0] = 0; }
        }
        return;
    }
    if (g_ktab != K_APPS && g_ktab != K_PROCS) return;
    int by = k_list_bottom() + 10, sel_ok = g_ksel >= 0 && g_ksel < g_nrows;
    if (my >= by && my < by + 22) {
        if (g_ktab == K_APPS) {
            if (mx >= px + pw - 300 && mx < px + pw - 208 && sel_ok) end_task(g_ksel);
            else if (mx >= px + pw - 200 && mx < px + pw - 108 && sel_ok) switch_to(g_ksel);
        } else if (mx >= px + pw - 120 && sel_ok) end_task(g_ksel);
        return;
    }
    int ly = py + 10 + 22;
    if (my < ly || my >= k_list_bottom()) return;
    int r = (my - ly) / K_ROW_H + g_scroll;
    int ri = r < k_rows() ? k_row(r) : -1;
    static uint32_t last_ms;
    static int last_ri = -1;
    if (ri >= 0 && ri == last_ri && timer_ms() - last_ms < 450 && g_ktab == K_APPS) switch_to(ri);
    last_ri = ri;
    last_ms = timer_ms();
    g_ksel = ri;
}

static void classic_rclick(int mx, int my) {
    if (g_ktab != K_PROCS && g_ktab != K_APPS) return;
    int ly = k_page_y() + 10 + 22;
    if (my < ly || my >= k_list_bottom()) return;
    int r = (my - ly) / K_ROW_H + g_scroll;
    int ri = r < k_rows() ? k_row(r) : -1;
    if (ri < 0) return;
    g_ksel = ri;
    if (g_rows[ri].kind == R_TASK) { prio_menu(mx, my, g_rows[ri].pid); return; }
    g_menu_row = ri;
    ctx_item_t items[] = { { _("Switch To"), 1, g_rows[ri].kind != R_WINDOW }, { _("End Task"), 2, 0 } };
    ctxmenu_open(mx, my, items, 2, menu_cb, NULL);
}

/* ══ Banana OS's own first Task Manager ══════════════════════════════════ */

#define B_TABS_Y   24
#define B_TAB_H    20
#define B_BODY_Y   (B_TABS_Y + B_TAB_H + 6)
#define B_ROW_H    14
#define B_PANEL    TC(0x1D232C)
#define B_TITLE    TC(0x384562)
#define B_TEXT     TC(0xE8EEF6)
#define B_DIM      TC(0xAAB6C6)
#define B_LIST     TC(0x141920)
#define B_SEL      TC(0x3A5A8A)
static const char* const B_TAB_NAMES[3] = { "Apps & windows", "Processes", "Performance" };
static int g_btab, g_bsel = -1;

static void b_bevel(int x, int y, int w, int h, uint32_t base, uint32_t hi, uint32_t lo) {
    gfx_fill_rect(x, y, w, h, base);
    gfx_fill_rect(x, y, w, 1, hi);
    gfx_fill_rect(x, y, 1, h, hi);
    gfx_fill_rect(x, y + h - 1, w, 1, lo);
    gfx_fill_rect(x + w - 1, y, 1, h, lo);
}
static void b_button(int x, int y, int w, const char* label, int enabled) {
    kbnav_add(&g_nav, x, y, w, 18);
    uint32_t base = enabled ? TC(0x303740) : TC(0x262B33);
    b_bevel(x, y, w, 18, base, TC(0x535D6E), TC(0x15191F));
    tm_text(x + (w - tm_width(label)) / 2, y + 5, label, enabled ? B_TEXT : TC(0x707A88), base);
}
static int b_list_top(void) { return g_win.y + B_BODY_Y + B_ROW_H + 2; }
static int b_apps(void) { int n = 0; for (int i = 0; i < g_nrows; i++) if (g_rows[i].kind == R_WINDOW || g_rows[i].kind == R_APP) n++; return n; }
static int b_app(int k) {
    for (int i = 0; i < g_nrows; i++) if ((g_rows[i].kind == R_WINDOW || g_rows[i].kind == R_APP) && k-- == 0) return i;
    return -1;
}
static void b_graph(int x, int y, int w, int h, const uint8_t* hist, uint32_t color, const char* label) {
    b_bevel(x, y, w, h, B_LIST, TC(0x10141C), TC(0x404B5C));
    for (int g = 1; g < 4; g++) gfx_fill_rect(x + 1, y + h * g / 4, w - 2, 1, TC(0x232A35));
    int bw = (w - 4) / HIST;
    if (bw < 1) bw = 1;
    for (int i = 0; i < g_hist_n; i++) {
        int bh = (h - 4) * hist[i] / 100;
        if (bh < 1 && hist[i]) bh = 1;
        gfx_fill_rect(x + 2 + (HIST - g_hist_n + i) * bw, y + h - 2 - bh, bw > 1 ? bw - 1 : 1, bh, color);
    }
    tm_text(x + 6, y + 5, label, B_TEXT, B_LIST);
}

static void banana_draw(void) {
    int x = g_win.x, y = g_win.y, W = g_win.w, H = g_win.h;
    b_bevel(x, y, W, H, B_PANEL, TC(0x505D72), TC(0x10141C));
    b_bevel(x + 3, y + 3, W - 6, TITLE_H - 1, B_TITLE, TC(0x647692), TC(0x111923));
    tm_text(x + 10, y + 7, _("Task Manager"), TC(0xFFFFFF), B_TITLE);
    win_draw_buttons(&g_win, 4, 12);
    int tx = x + 6;
    for (int t = 0; t < 3; t++) {
        int w = (int)strlen(_(B_TAB_NAMES[t])) * 8 + 20;
        uint32_t bg = t == g_btab ? TC(0xF2F2F2) : TC(0x3A4250);
        b_bevel(tx, y + B_TABS_Y, w, B_TAB_H, bg, TC(0x808A9A), TC(0x202630));
        kbnav_add(&g_nav, tx, y + B_TABS_Y, w, B_TAB_H);
        tm_text(tx + 10, y + B_TABS_Y + 6, _(B_TAB_NAMES[t]), t == g_btab ? TC(0x101010) : B_TEXT, bg);
        tx += w + 4;
    }
    char line[128];
    int lx = x + 8, lw = W - 16;
    if (g_btab == 0) {
        int lh = H - B_BODY_Y - 56;
        b_bevel(lx, y + B_BODY_Y, lw, lh, B_LIST, TC(0x10141C), TC(0x404B5C));
        gfx_fill_rect(lx + 1, y + B_BODY_Y + 1, lw - 2, B_ROW_H, TC(0x252C37));
        tm_text(lx + 8, y + B_BODY_Y + 4, _("Name"), B_DIM, TC(0x252C37));
        tm_text(lx + lw - 200, y + B_BODY_Y + 4, _("Status"), B_DIM, TC(0x252C37));
        int maxr = (lh - B_ROW_H - 4) / B_ROW_H, n = b_apps();
        for (int i = 0; i < n && i < maxr; i++) {
            int ri = b_app(i), ry = b_list_top() + i * B_ROW_H;
            uint32_t bg = ri == g_bsel ? B_SEL : B_LIST;
            kbnav_add(&g_nav, lx + 1, ry, lw - 2, B_ROW_H);
            if (ri == g_bsel) gfx_fill_rect(lx + 1, ry, lw - 2, B_ROW_H, bg);
            char t[48];
            if (g_rows[ri].kind == R_APP) ksnprintf(t, sizeof(t), "%s (app)", g_rows[ri].text);
            else kstrlcpy(t, g_rows[ri].text, sizeof(t));
            int maxc = (lw - 220) / 8;
            if (maxc > 0 && maxc < (int)sizeof(t)) t[maxc] = 0;
            gfx_fill_rect(lx + 8, ry + 3, 8, 8, g_rows[ri].kind == R_APP ? TC(0xD0A030) : TC(0x3A7BD5));
            tm_text(lx + 22, ry + 3, t, B_TEXT, bg);
            if (g_rows[ri].kind == R_APP) {
                char info[40];
                ksnprintf(info, sizeof(info), "%s, %u KiB", g_rows[ri].status, g_rows[ri].mem / 1024);
                tm_text(lx + lw - 200, ry + 3, info, B_DIM, bg);
            } else tm_text(lx + lw - 200, ry + 3, g_rows[ri].status[0] ? g_rows[ri].status : "window", B_DIM, bg);
        }
        if (!n) tm_text(lx + 8, b_list_top() + 4, _("Nothing is open."), B_DIM, B_LIST);
        int by = y + H - 44, sel_ok = g_bsel >= 0 && g_bsel < g_nrows;
        b_button(x + W - 208, by, 100, _("Switch to"), sel_ok && g_rows[g_bsel].kind == R_WINDOW);
        b_button(x + W - 100, by, 92, _("End task"), sel_ok);
    } else if (g_btab == 1) {
        snap_tasks();
        int lh = H - B_BODY_Y - 30;
        b_bevel(lx, y + B_BODY_Y, lw, lh, B_LIST, TC(0x10141C), TC(0x404B5C));
        gfx_fill_rect(lx + 1, y + B_BODY_Y + 1, lw - 2, B_ROW_H, TC(0x252C37));
        tm_text(lx + 8, y + B_BODY_Y + 4, _("PID  Name                    State     CPU   Time   Prio"), B_DIM, TC(0x252C37));
        int row = 0, maxr = (lh - B_ROW_H - 4) / B_ROW_H;
        for (int i = 0; i < g_ntasks && row < maxr; i++) {
            if (g_tasks[i].state == TASK_UNUSED) continue;
            uint32_t s = g_tasks[i].ticks_total / 100;
            char st[16];
            if (g_tasks[i].state == TASK_AWAY && g_tasks[i].cpu > 0) ksnprintf(st, sizeof(st), "core %d", g_tasks[i].cpu);
            else kstrlcpy(st, g_tasks[i].state == TASK_AWAY ? "wait core" : task_state_str(g_tasks[i].state), sizeof(st));
            ksnprintf(line, sizeof(line), "%-4u %-23s %-9s %3u%%  %u:%02u   %3d", g_tasks[i].pid, g_tasks[i].name,
                      st, g_tasks[i].cpu_pct, s / 60, s % 60, g_tasks[i].nice);
            tm_text(lx + 8, b_list_top() + row * B_ROW_H + 3, line, g_tasks[i].cpu_pct >= 50 ? TC(0xF0A060) : B_TEXT, B_LIST);
            row++;
        }
    } else {
        int gw = (W - 24) / 2, gh = 120, gy = y + B_BODY_Y + 4;
        char lab[48];
        ksnprintf(lab, sizeof(lab), "CPU %u%%", last(g_cpu_hist));
        b_graph(lx, gy, gw, gh, g_cpu_hist, TC(0x57B65A), lab);
        ksnprintf(lab, sizeof(lab), "Memory %u%%", last(g_mem_hist));
        b_graph(lx + gw + 8, gy, gw, gh, g_mem_hist, TC(0x3A7BD5), lab);
        int ty = gy + gh + 14;
        meminfo_t mi;
        meminfo_get(&mi);
        char a[24], b[24], c[24];
        human_kb(mi.used_kb - mi.cached_kb, a, sizeof(a));
        human_kb(mi.total_kb, b, sizeof(b));
        human_kb(mi.cached_kb, c, sizeof(c));
        ksnprintf(line, sizeof(line), "Memory in use:  %s of %s (+ %s of cached files)", a, b, c);
        tm_text(lx, ty, line, B_TEXT, B_PANEL);
        uint32_t up = timer_ms() / 1000;
        snap_tasks();
        int ntask = 0;
        for (int i = 0; i < g_ntasks; i++) if (g_tasks[i].state != TASK_UNUSED) ntask++;
        ksnprintf(line, sizeof(line), "Uptime:         %u:%02u:%02u", up / 3600, up / 60 % 60, up % 60);
        tm_text(lx, ty + 16, line, B_TEXT, B_PANEL);
        ksnprintf(line, sizeof(line), "Tasks:          %d kernel tasks, %d app%s", ntask, app_count(), app_count() == 1 ? "" : "s");
        tm_text(lx, ty + 32, line, B_TEXT, B_PANEL);
        ksnprintf(line, sizeof(line), "Files:          %u files, %u folders", fs_used_files(), fs_used_dirs());
        tm_text(lx, ty + 48, line, B_TEXT, B_PANEL);
        netif_t* nif = net_if();
        char ip[16] = "-";
        if (nif->dev && nif->configured) ip4_to_str(nif->ip, ip);
        ksnprintf(line, sizeof(line), "Network:        %s %s", nif->dev ? nif->dev->ifname : "none", ip);
        tm_text(lx, ty + 64, line, B_TEXT, B_PANEL);
        ksnprintf(line, sizeof(line), "Sound:          %s", audio_device_name());
        tm_text(lx, ty + 80, line, B_TEXT, B_PANEL);
        ksnprintf(line, sizeof(line), "Processor:      %d core%s", cpu_count(), cpu_count() == 1 ? "" : "s");
        tm_text(lx, ty + 96, line, B_TEXT, B_PANEL);
    }
    gfx_fill_rect(x + 3, y + H - 18, W - 6, 15, TC(0x161B22));
    if (g_status[0]) kstrlcpy(line, g_status, sizeof(line));
    else ksnprintf(line, sizeof(line), "CPU %u%%   Memory %u%%   %d window%s", last(g_cpu_hist), last(g_mem_hist), b_apps(), b_apps() == 1 ? "" : "s");
    tm_text(x + 8, y + H - 14, line, B_DIM, TC(0x161B22));
    gfx_draw_grip(x + W, y + H);
    kbnav_draw(&g_nav, TC(0xFFD34E));
}

static void banana_click(int mx, int my) {
    int lx = mx - g_win.x, ly = my - g_win.y;
    if (ly >= B_TABS_Y && ly < B_TABS_Y + B_TAB_H) {
        int x = 6;
        for (int t = 0; t < 3; t++) {
            int w = (int)strlen(_(B_TAB_NAMES[t])) * 8 + 20;
            if (lx >= x && lx < x + w) { g_btab = t; g_bsel = -1; g_status[0] = 0; }
            x += w + 4;
        }
        return;
    }
    if (g_btab != 0) return;
    int by = g_win.h - 44, sel_ok = g_bsel >= 0 && g_bsel < g_nrows;
    if (ly >= by && ly < by + 18) {
        if (lx >= g_win.w - 208 && lx < g_win.w - 108 && sel_ok) switch_to(g_bsel);
        else if (lx >= g_win.w - 100 && lx < g_win.w - 8 && sel_ok) end_task(g_bsel);
        return;
    }
    if (my < b_list_top() || lx < 8 || lx >= g_win.w - 8) return;
    int r = (my - b_list_top()) / B_ROW_H;
    int ri = r < b_apps() ? b_app(r) : -1;
    static uint32_t last_ms;
    static int last_ri = -1;
    if (ri >= 0 && ri == last_ri && timer_ms() - last_ms < 450) switch_to(ri);
    last_ri = ri;
    last_ms = timer_ms();
    g_bsel = ri;
}

static void banana_rclick(int mx, int my) {
    if (g_btab == 1) {
        if (my < b_list_top()) return;
        int r = (my - b_list_top()) / B_ROW_H;
        snap_tasks();
        for (int i = 0; i < g_ntasks; i++) {
            if (g_tasks[i].state == TASK_UNUSED) continue;
            if (r-- == 0) { prio_menu(mx, my, (int)g_tasks[i].pid); return; }
        }
        return;
    }
    if (g_btab != 0 || my < b_list_top()) return;
    int r = (my - b_list_top()) / B_ROW_H;
    int ri = r < b_apps() ? b_app(r) : -1;
    if (ri < 0) return;
    g_bsel = g_menu_row = ri;
    ctx_item_t items[] = { { _("Switch to"), 1, g_rows[ri].kind != R_WINDOW }, { CTX_SEP, 0, 0 }, { _("End task"), 2, 0 } };
    ctxmenu_open(mx, my, items, 3, menu_cb, NULL);
}

/* Windows 10: the menu bar and the tabs */
static void draw_w10_bars(void) {
    int x = g_win.x, y = g_win.y, W = g_win.w;
    gfx_fill_rect(x + 1, y + W10_MENU_Y, W - 2, 19, C_BG);
    static const char* const MENU[3] = { "File", "Options", "View" };
    for (int i = 0, mx = x + 8; i < 3; mx += (int)strlen(_(MENU[i])) * 8 + 16, i++) tm_text(mx, y + W10_MENU_Y + 6, _(MENU[i]), C_TEXT, C_BG);
    gfx_fill_rect(x + 1, y + W10_TABS_Y + W10_TAB_H - 1, W - 2, 1, C_LINE);
    int sel_tab = 0;
    for (int i = 0; i < 4; i++) if (W10_PAGE[i] == g_page) sel_tab = i;
    for (int i = 0; i < 4; i++) {
        int tw, tx = w10_tab_x(i, &tw), on = i == sel_tab;
        kbnav_add(&g_nav, tx, y + W10_TABS_Y, tw, W10_TAB_H);
        if (on) {
            gfx_fill_rect(tx, y + W10_TABS_Y, tw, W10_TAB_H, C_BG);
            gfx_fill_rect(tx, y + W10_TABS_Y, tw, 1, C_LINE);
            gfx_fill_rect(tx, y + W10_TABS_Y, 1, W10_TAB_H, C_LINE);
            gfx_fill_rect(tx + tw - 1, y + W10_TABS_Y, 1, W10_TAB_H, C_LINE);
        }
        tm_text(tx + 10, y + W10_TABS_Y + 8, _(W10_TABS[i]), C_TEXT, C_BG);
    }
    /* the button bar at the bottom */
    int by = g_win.y + g_win.h - 38;
    gfx_fill_rect(x + 1, by, W - 2, 37, C_RAIL);
    gfx_fill_rect(x + 1, by, W - 2, 1, C_LINE);
    char summary[64];
    ksnprintf(summary,sizeof(summary),"%d %s",g_napps+g_nbg,_("processes"));
    tm_text(x + 12, by + 13, summary, C_DIM, C_RAIL);
}


void taskmgr_draw(const fb_info_t* fi) {
    (void)fi;
    if (!g_open) return;
    set_style();
    g_win.min_w=g_style==TM_STYLE_WIN11?680:g_style==TM_STYLE_WIN10?640:560;
    win_clamp(&g_win);
    kbnav_begin(&g_nav);
    if (g_style == TM_STYLE_CLASSIC) { classic_draw(); return; }
    if (g_style == TM_STYLE_BANANA) { banana_draw(); return; }
    int x = g_win.x, y = g_win.y, W = g_win.w, H = g_win.h;
    frame(x, y, W, H, C_BG, w10() ? TCL(0xA0A0A0) : TC(0x454545));
    gfx_fill_rect(x + 1, y + 1, W - 2, TITLE_H + 1, w10() ? C_BG : C_RAIL);
    /* the title: a small chart icon, the name */
    gfx_fill_rect(x + 10, y + 13, 3, 4, C_ACCENT);
    gfx_fill_rect(x + 14, y + 9, 3, 8, C_ACCENT);
    gfx_fill_rect(x + 18, y + 6, 3, 11, C_ACCENT);
    tm_text(x + 28, y + 8, _("Task Manager"), C_TEXT, w10() ? C_BG : C_RAIL);
    modern_title_buttons();
    if (w10()) draw_w10_bars();
    else draw_rail();
    if (g_page == P_PROCS) draw_processes();
    else if (g_page == P_PERF) draw_performance();
    else if (g_page == P_DETAILS) draw_details();
    else draw_users();
    if (g_status[0]) {
        int sw = tm_width(g_status) + 16;
        gfx_fill_rect(x + W - sw - 10, y + H - 26, sw, 18, C_PANEL);
        tm_text(x + W - sw - 2, y + H - 21, g_status, C_DIM, C_PANEL);
    }
    gfx_draw_grip(x + W, y + H);
    kbnav_draw(&g_nav, C_ACCENT);
}

/* the window manager (kernel/gui.c) moves, snaps and maximizes it through its geometry */
win_geom_t* taskmgr_geom(void) { return &g_win; }
