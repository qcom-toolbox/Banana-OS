/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Graphics drivers: the active display, and what goes through it (see gpu.h) */
#include "gpu.h"
#include "laptop.h"
#include "fb.h"
#include "gfx.h"
#include "kstring.h"
#include "serial.h"
#include "task.h"
#include "terminal.h"
#include "timer.h"
#include "spinlock.h"
#include "gpu_surface.h"

#define GPU_MAX 4
static gpu_t* g_gpus[GPU_MAX];
static void (*g_panic[GPU_MAX])(void);
static void (*g_service[GPU_MAX])(void);
static const gpu_present_ops_t* g_present[GPU_MAX];
static void (*g_tests[GPU_MAX])(void (*)(const char*, int, const char*));
static int    g_ngpus;
static gpu_t* g_active;

/* what was drawn since the last push to the screen (virtual GPUs) */
#define DIRTY_MAX 32
static gpu_rect_t g_dirty[DIRTY_MAX];
static int g_ndirty;
static volatile int g_raw_dirty;
void gpu_dirty_raw(void) { g_raw_dirty = 1; }
static int g_flusher;                    /* the task runs */
static int g_booting;                    /* in gpu_init: the terminal is not up yet */

static gpu_stats_t g_stats;
static spinlock_t g_stats_lock = SPINLOCK_INIT;
/* ── the GPU's load ── */
static int (*volatile g_load_busy)(void);
static uint64_t (*volatile g_load_vram)(void);
static volatile uint32_t g_load_hits, g_load_all, g_load_wanted;
void gpu_register_load(int (*busy_now)(void), uint64_t (*vram_used)(void)) {
    g_load_vram = vram_used;
    g_load_busy = busy_now;
}
void gpu_load_tick(uint32_t ms) {
    int (*f)(void) = g_load_busy;
    if (!f || ms - g_load_wanted > 5000u) return;      /* (nobody looking: no register reads) */
    g_load_all++;
    if (f()) g_load_hits++;
}
int gpu_load(void) {
    if (!g_load_busy) return -1;
    uint32_t now = timer_ms();
    if (now - g_load_wanted > 5000u) { g_load_wanted = now; g_load_hits = g_load_all = 0; return 0; }
    g_load_wanted = now;
    uint32_t all = g_load_all, hits = g_load_hits;
    g_load_hits = g_load_all = 0;                       /* (a tick in between is lost: no matter) */
    return all ? (int)((uint64_t)hits * 100u / all) : 0;
}
uint64_t gpu_vram_used(void) { uint64_t (*f)(void) = g_load_vram; return f ? f() : 0; }

void gpu_stats(gpu_stats_t* out) {
    uintptr_t f = spin_lock_irq(&g_stats_lock); *out = g_stats; spin_unlock_irq(&g_stats_lock, f);
}
void gpu_note_present(uint64_t bytes, uint64_t ns) {
    uintptr_t f = spin_lock_irq(&g_stats_lock);
    g_stats.frames++; g_stats.copied_bytes += bytes; g_stats.copy_ns += ns;
    spin_unlock_irq(&g_stats_lock, f);
}
void gpu_note_damage(uint64_t candidate, uint64_t copied) {
    uintptr_t f = spin_lock_irq(&g_stats_lock);
    g_stats.damage_frames++; g_stats.candidate_bytes += candidate; g_stats.damage_bytes += copied;
    spin_unlock_irq(&g_stats_lock, f);
}
void gpu_note_command(uint64_t ns, int timeout) {
    uintptr_t f = spin_lock_irq(&g_stats_lock);
    g_stats.commands++; g_stats.command_ns += ns; g_stats.timeouts += !!timeout;
    spin_unlock_irq(&g_stats_lock, f);
}
static void flush_driver(gpu_t* g, int x, int y, int w, int h) {
    uint64_t t = timer_counter(); g->flush(g, x, y, w, h); t = timer_counter() - t;
    uintptr_t f = spin_lock_irq(&g_stats_lock);
    g_stats.flushes++; g_stats.flushed_bytes += (uint64_t)w * h * 4; g_stats.flush_ns += t;
    spin_unlock_irq(&g_stats_lock, f);
}

gpu_t* gpu_active(void) { return g_active; }

int gpu_register(gpu_t* g) {
    if (!g || !g->set_mode || g_ngpus == GPU_MAX) return -1;
    g_gpus[g_ngpus++] = g;
    g_active = g;
    klog("gpu: %s (driver %s) on %02x:%02x.%u%s\n", g->name, g->driver, g->pci.bus, g->pci.dev, g->pci.fn,
         g->flush ? ", pushed to the screen" : "");
    return 0;
}

void gpu_register_present(gpu_t* g, const gpu_present_ops_t* ops) {
    for (int i = 0; i < g_ngpus; i++) if (g_gpus[i] == g) g_present[i] = ops;
}
int gpu_present_begin(gpu_surface_t* target) {
    for (int i = 0; i < g_ngpus; i++) if (g_gpus[i] == g_active && g_present[i]) return g_present[i]->begin(target);
    return 0;
}
void gpu_present_end(gpu_surface_t source, const gpu_rect_t* changed, int count) {
    for (int i = 0; i < g_ngpus; i++) if (g_gpus[i] == g_active && g_present[i]) { g_present[i]->end(source, changed, count); return; }
}
void gpu_note_flip(void) {
    uintptr_t f = spin_lock_irq(&g_stats_lock); g_stats.flips++; spin_unlock_irq(&g_stats_lock, f);
}
void gpu_note_vblank(int timeout) {
    uintptr_t f = spin_lock_irq(&g_stats_lock);
    g_stats.vblank_waits++; g_stats.vblank_timeouts += !!timeout;
    spin_unlock_irq(&g_stats_lock, f);
}

void gpu_register_service(gpu_t* g, void (*service)(void),
                          void (*tests)(void (*)(const char*, int, const char*))) {
    for (int i = 0; i < g_ngpus; i++) if (g_gpus[i] == g) { g_service[i] = service; g_tests[i] = tests; }
}
void gpu_driver_tests(void (*report)(const char*, int, const char*)) {
    for (int i = 0; i < g_ngpus; i++) if (g_gpus[i] == g_active && g_tests[i]) { g_tests[i](report); return; }
    terminal_writeln("  skip  asynchronous GPU queue - this driver has no queue test");
}

void gpu_register_panic(gpu_t* g, void (*present)(void)) {
    for(int i=0;i<g_ngpus;i++)if(g_gpus[i]==g)g_panic[i]=present;
}

void gpu_panic_present(void) {
    for(int i=0;i<g_ngpus && i<GPU_MAX;i++)if(g_gpus[i]==g_active && g_panic[i]) {
        g_panic[i]();return;
    }
}

void gpu_set_framebuffer(uintptr_t addr, int w, int h, int pitch) {
    int had = fb_available();
    fb_set_framebuffer(addr, (uint32_t)w, (uint32_t)h, (uint32_t)pitch);
    gfx_init();
    if (!had && !g_booting) terminal_set_mode(TERMINAL_MODE_FRAMEBUFFER);
}

void gpu_flush(int x, int y, int w, int h) {
    gpu_t* g = g_active;
    if (!g || !g->flush) return;
    const fb_info_t* fi = fb_info();
    gpu_rect_t r = {x, y, w, h};
    if (!gpu_rect_clip(&r, (int)fi->width, (int)fi->height)) return;
    if (!g_flusher) { flush_driver(g, r.x, r.y, r.w, r.h); return; }
    uintptr_t fl;
    __asm__ volatile("pushf; pop %0; cli" : "=r"(fl) :: "memory");
    /* Merge only when the bounding rectangle costs no more than the two
     * updates. Distant small changes must not become a full-screen copy. */
    for (int i = 0; i < g_ndirty;) {
        gpu_rect_t a = g_dirty[i];
        int x0 = a.x < r.x ? a.x : r.x, y0 = a.y < r.y ? a.y : r.y;
        int x1 = a.x + a.w > r.x + r.w ? a.x + a.w : r.x + r.w;
        int y1 = a.y + a.h > r.y + r.h ? a.y + a.h : r.y + r.h;
        if ((uint64_t)(x1 - x0) * (y1 - y0) <= (uint64_t)a.w * a.h + (uint64_t)r.w * r.h) {
            r = (gpu_rect_t){x0, y0, x1 - x0, y1 - y0};
            g_dirty[i] = g_dirty[--g_ndirty]; i = 0;
        } else i++;
    }
    if (g_ndirty == DIRTY_MAX) {
        g_ndirty = 0; r = (gpu_rect_t){0, 0, (int)fi->width, (int)fi->height};
    }
    g_dirty[g_ndirty++] = r;
    if (fl & 0x200) __asm__ volatile("sti" ::: "memory");
}

static void flusher_task(void) {
    task_set_background();
    for (;;) {
        gpu_t* g = g_active;
        task_sleep_ms(g && g->flush ? 16 : 500);
        for (int i = 0; i < g_ngpus; i++) if (g_gpus[i] == g && g_service[i]) g_service[i]();
        if (!g || !g->flush) continue;
        gpu_rect_t dirty[DIRTY_MAX];
        uintptr_t fl;
        __asm__ volatile("pushf; pop %0; cli" : "=r"(fl) :: "memory");
        int n = g_ndirty;
        memcpy(dirty, g_dirty, (size_t)n * sizeof(*dirty)); g_ndirty = 0;
        if (g_raw_dirty) {
            const fb_info_t* f = fb_info();
            dirty[0] = (gpu_rect_t){0, 0, (int)f->width, (int)f->height};
            n = 1; g_raw_dirty = 0;
        }
        if (fl & 0x200) __asm__ volatile("sti" ::: "memory");
        for (int i = 0; i < n; i++) {
            gpu_rect_t r = dirty[i];
            const fb_info_t* f = fb_info();
            if (gpu_rect_clip(&r, (int)f->width, (int)f->height))
                flush_driver(g, r.x, r.y, r.w, r.h);
        }
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
int gpu_backlight_step(int delta) {
    if(g_active && g_active->backlight) {
        int current=g_active->backlight(g_active,-1);
        if(current>=0) {
            int next=current+delta;
            return g_active->backlight(g_active,next<0?0:next>100?100:next);
        }
    }
    return laptop_backlight_step(delta);
}
int gpu_backlight(int percent) {
    int value=g_active && g_active->backlight?g_active->backlight(g_active,percent):-1;
    return value>=0?value:laptop_backlight(percent);
}

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
    gpu_diagnostics();
}
