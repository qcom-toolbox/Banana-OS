/* Graphics drivers: the active display, and what goes through it (see gpu.h) */
#include "gpu.h"
#include "fb.h"
#include "gfx.h"
#include "kstring.h"
#include "serial.h"
#include "task.h"
#include "terminal.h"
#include "timer.h"

#define GPU_MAX 4
static gpu_t* g_gpus[GPU_MAX];
static int    g_ngpus;
static gpu_t* g_active;

/* what was drawn since the last push to the screen (virtual GPUs) */
static volatile int g_dx0 = 1 << 30, g_dy0 = 1 << 30, g_dx1, g_dy1;
static int g_flusher;                    /* the task runs */
static int g_booting;                    /* in gpu_init: the terminal is not up yet */

gpu_t* gpu_active(void) { return g_active; }

int gpu_register(gpu_t* g) {
    if (!g || !g->set_mode || g_ngpus == GPU_MAX) return -1;
    g_gpus[g_ngpus++] = g;
    g_active = g;
    klog("gpu: %s (driver %s) on %02x:%02x.%u%s\n", g->name, g->driver, g->pci.bus, g->pci.dev, g->pci.fn,
         g->flush ? ", pushed to the screen" : "");
    return 0;
}

void gpu_set_framebuffer(uintptr_t addr, int w, int h, int pitch) {
    int had = fb_available();
    fb_set_framebuffer(addr, (uint32_t)w, (uint32_t)h, (uint32_t)pitch);
    gfx_init();
    if (!had && !g_booting) terminal_set_mode(TERMINAL_MODE_FRAMEBUFFER);
}

void gpu_flush(int x, int y, int w, int h) {
    gpu_t* g = g_active;
    if (!g || !g->flush || w <= 0 || h <= 0) return;
    if (!g_flusher) { g->flush(g, x, y, w, h); return; }      /* (at boot: right away) */
    uintptr_t fl;
    __asm__ volatile("pushf; pop %0; cli" : "=r"(fl) :: "memory");
    if (x < g_dx0) g_dx0 = x;
    if (y < g_dy0) g_dy0 = y;
    if (x + w > g_dx1) g_dx1 = x + w;
    if (y + h > g_dy1) g_dy1 = y + h;
    if (fl & 0x200) __asm__ volatile("sti" ::: "memory");
}

/* pushes what was drawn, at most 60 times a second; the whole screen
 * now and then too, for whatever draws without telling (the terminal) */
static void flusher_task(void) {
    task_set_background();
    uint32_t last_full = 0;
    for (;;) {
        gpu_t* g = g_active;
        task_sleep_ms(g && g->flush ? 16 : 500);     /* (a real sleep: the other tasks run) */
        if (!g || !g->flush) continue;
        const fb_info_t* fi = fb_info();
        uintptr_t fl;
        __asm__ volatile("pushf; pop %0; cli" : "=r"(fl) :: "memory");
        int x0 = g_dx0, y0 = g_dy0, x1 = g_dx1, y1 = g_dy1;
        g_dx0 = g_dy0 = 1 << 30;
        g_dx1 = g_dy1 = 0;
        if (fl & 0x200) __asm__ volatile("sti" ::: "memory");
        uint32_t now = timer_ms();
        if (now - last_full >= 100) {
            last_full = now;
            x0 = y0 = 0;
            x1 = (int)fi->width;
            y1 = (int)fi->height;
        }
        if (x1 > (int)fi->width) x1 = (int)fi->width;
        if (y1 > (int)fi->height) y1 = (int)fi->height;
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > x0 && y1 > y0) g->flush(g, x0, y0, x1 - x0, y1 - y0);
    }
}

void gpu_start(void) {
    if (g_flusher) return;
    g_flusher = 1;
    if (task_create("gpu", flusher_task) < 0) g_flusher = 0;
}

int gpu_cursor_image(const uint32_t* argb, int w, int h, int hx, int hy) {
    gpu_t* g = g_active;
    return g && g->cursor_image && g->cursor_move ? g->cursor_image(g, argb, w, h, hx, hy) : -1;
}
void gpu_cursor_move(int x, int y, int visible) {
    gpu_t* g = g_active;
    if (g && g->cursor_move) g->cursor_move(g, x, y, visible);
}
int gpu_has_hw_cursor(void) { return g_active && g_active->cursor_image && g_active->cursor_move; }
void gpu_wait_vblank(void) { if (g_active && g_active->wait_vblank) g_active->wait_vblank(g_active); }
int gpu_backlight(int percent) { return g_active && g_active->backlight ? g_active->backlight(g_active, percent) : -1; }

/* ── probing ──────────────────────────────────────────────────────── */
static int (*const BUILTIN[])(const pci_dev_t*) = {
    gpu_virtio_probe, gpu_vmsvga_probe, gpu_intel_probe, gpu_bga_probe,
};

static int probe_cb(const pci_dev_t* d, void* ctx) {
    (void)ctx;
    if (d->class_code != 0x03) return 0;               /* display controllers */
    for (unsigned i = 0; i < sizeof(BUILTIN) / sizeof(BUILTIN[0]); i++)
        if (BUILTIN[i](d) == 0) break;
    return 0;
}

void gpu_init(void) {
    g_booting = 1;
    pci_scan(probe_cb, NULL);
    g_booting = 0;
    if (!g_active) klog("gpu: no driver for the display - the firmware's framebuffer stays\n");
}

/* ── lsgpu ────────────────────────────────────────────────────────── */
static const char* vendor_name(uint16_t v) {
    switch (v) {
    case 0x8086: return "Intel";
    case 0x1002: return "AMD";
    case 0x10DE: return "NVIDIA";
    case 0x15AD: return "VMware";
    case 0x80EE: return "VirtualBox";
    case 0x1234: return "QEMU / Bochs";
    case 0x1AF4: return "virtio";
    case 0x1013: return "Cirrus Logic";
    case 0x1414: return "Microsoft (Hyper-V)";
    case 0x5333: return "S3";
    default:     return "";
    }
}

static int list_cb(const pci_dev_t* d, void* ctx) {
    (void)ctx;
    if (d->class_code != 0x03) return 0;
    gpu_t* g = NULL;
    for (int i = 0; i < g_ngpus; i++)
        if (g_gpus[i]->pci.bus == d->bus && g_gpus[i]->pci.dev == d->dev && g_gpus[i]->pci.fn == d->fn) g = g_gpus[i];
    char line[200];
    const char* vn = vendor_name(d->vendor);
    const char* fam = gpu_family_name(d->vendor, d->device);
    ksnprintf(line, sizeof(line), "%02x:%02x.%u  %04x:%04x  %s%s%s", d->bus, d->dev, d->fn, d->vendor, d->device,
              g ? g->name : fam ? fam : vn, g || fam ? "" : (vn[0] ? " display controller" : "display controller"),
              g == g_active && g ? "  [active]" : "");
    terminal_writeln(line);
    if (!g) {
        terminal_writeln("    driver: none (the firmware's framebuffer is used)");
        return 0;
    }
    const fb_info_t* fi = fb_info();
    ksnprintf(line, sizeof(line), "    driver: %s", g->driver);
    terminal_writeln(line);
    if (g == g_active) {
        ksnprintf(line, sizeof(line), "    mode: %ux%u, 32-bit, %u bytes per row", fi->width, fi->height, fi->pitch);
        terminal_writeln(line);
    }
    if (g->vram) {
        ksnprintf(line, sizeof(line), "    video memory: %u MiB", g->vram >> 20);
        terminal_writeln(line);
    }
    display_mode_t m[24];
    int n = g->modes ? g->modes(g, m, 24) : 0;
    if (n > 0) {
        int o = ksnprintf(line, sizeof(line), "    modes:");
        for (int i = 0; i < n && o < (int)sizeof(line) - 12; i++) o += ksnprintf(line + o, sizeof(line) - (size_t)o, " %dx%d", m[i].w, m[i].h);
        terminal_writeln(line);
    }
    ksnprintf(line, sizeof(line), "    hardware pointer: %s, vertical blank: %s, backlight: %s",
              g->cursor_image ? "yes" : "no", g->wait_vblank ? "yes" : "no",
              g->backlight && g->backlight(g, -1) >= 0 ? "yes" : "no");
    terminal_writeln(line);
    if (g->info) {
        char more[400];
        more[0] = 0;
        g->info(g, more, sizeof(more));
        char* s = more;
        while (*s) {                                /* one line at a time, indented */
            char* nl = strchr(s, '\n');
            if (nl) *nl = 0;
            ksnprintf(line, sizeof(line), "    %s", s);
            terminal_writeln(line);
            if (!nl) break;
            s = nl + 1;
        }
    }
    return 0;
}

void gpu_list(void) {
    pci_scan(list_cb, NULL);
    if (!fb_available()) terminal_writeln("(no framebuffer: text mode)");
}
