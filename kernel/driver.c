/* Driver modules: hardware drivers made outside Banana OS (the Driver
 * Kit, sdk/driver/), loaded into the kernel. See sdk/driver/banana_driver.h
 * for what a driver sees; this is the kernel's side: the loader, the
 * table of calls, the PCI drivers they register and the displays. */
#define BANANA_KERNEL
#include "../sdk/driver/banana_driver.h"
#include "driver.h"
#include "app.h"
#include "fb.h"
#include "fs.h"
#include "gpu.h"
#include "idt.h"
#include "io.h"
#include "kheap.h"
#include "kstring.h"
#include "paging.h"
#include "pci.h"
#include "pkg.h"
#include "serial.h"
#include "task.h"
#include "terminal.h"
#include "timer.h"

#define MOD_MAX  16
#define PDRV_MAX 32

typedef struct {
    char     name[32];
    char     path[FS_PATH_LEN];
    uint8_t* image;
    int      status;                       /* what banana_driver_main returned */
} module_t;
static module_t g_mods[MOD_MAX];
static int      g_nmods;
static int      g_loading = -1;            /* the module whose main runs now */

typedef struct {
    bdrv_pci_driver_t drv;                 /* (a copy of what the module gave) */
    int               module;
    int               bound;               /* devices it took */
    char              devices[96];
} pdrv_t;
static pdrv_t g_pdrv[PDRV_MAX];
static int    g_npdrv;

/* ── the calls ────────────────────────────────────────────────────── */
static void d_vlog(const char* fmt, __builtin_va_list ap) {
    char buf[240];
    int o = 0;
    if (g_loading >= 0) o = ksnprintf(buf, sizeof(buf), "%s: ", g_mods[g_loading].name);
    kvsnprintf(buf + o, sizeof(buf) - (size_t)o, fmt, ap);
    size_t n = strlen(buf);
    if (n && buf[n - 1] != '\n' && n < sizeof(buf) - 1) { buf[n] = '\n'; buf[n + 1] = 0; }
    klog("%s", buf);
}
static void d_log(const char* fmt, ...) {
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    d_vlog(fmt, ap);
    __builtin_va_end(ap);
}

typedef struct { int index, want; bdrv_pci_t* out; } pci_get_ctx_t;
static void to_bdrv(const pci_dev_t* d, bdrv_pci_t* o) {
    memset(o, 0, sizeof(*o));
    o->bus = d->bus; o->dev = d->dev; o->fn = d->fn;
    o->irq_line = d->irq_line;
    o->vendor = d->vendor; o->device = d->device;
    o->class_code = d->class_code; o->subclass = d->subclass; o->prog_if = d->prog_if;
    o->revision = (uint8_t)pci_read32(d->bus, d->dev, d->fn, 0x08);
}
static void to_pci(const bdrv_pci_t* b, pci_dev_t* d) {
    memset(d, 0, sizeof(*d));
    d->bus = b->bus; d->dev = b->dev; d->fn = b->fn;
    d->vendor = b->vendor; d->device = b->device;
    d->class_code = b->class_code; d->subclass = b->subclass; d->prog_if = b->prog_if;
    d->irq_line = b->irq_line;
}
static int pci_get_cb(const pci_dev_t* d, void* ctx) {
    pci_get_ctx_t* c = ctx;
    if (c->index++ == c->want) { to_bdrv(d, c->out); return 1; }
    return 0;
}
static int d_pci_get(int index, bdrv_pci_t* out) {
    pci_get_ctx_t c = { 0, index, out };
    return index >= 0 && pci_scan(pci_get_cb, &c) ? 0 : -1;
}
static uint32_t d_pci_read(const bdrv_pci_t* d, int off, int width) {
    if (off < 0 || off > 255) return 0xFFFFFFFFu;
    uint32_t v = pci_read32(d->bus, d->dev, d->fn, (uint8_t)(off & ~3));
    v >>= (off & 3) * 8;
    return width == 1 ? v & 0xFF : width == 2 ? v & 0xFFFF : v;
}
static void d_pci_write(const bdrv_pci_t* d, int off, int width, uint32_t val) {
    if (off < 0 || off > 255) return;
    if (width == 4) { pci_write32(d->bus, d->dev, d->fn, (uint8_t)off, val); return; }
    uint32_t v = pci_read32(d->bus, d->dev, d->fn, (uint8_t)(off & ~3));
    int sh = (off & 3) * 8;
    uint32_t mask = (width == 1 ? 0xFFu : 0xFFFFu) << sh;
    v = (v & ~mask) | ((val << sh) & mask);
    pci_write32(d->bus, d->dev, d->fn, (uint8_t)(off & ~3), v);
}
static uint64_t d_pci_bar(const bdrv_pci_t* b, int bar, int* is_io) {
    pci_dev_t d;
    to_pci(b, &d);
    int io = 0;
    uint64_t r = bar >= 0 && bar < 6 ? (uint64_t)pci_bar(&d, bar, &io) : 0;
    if (is_io) *is_io = io;
    return r;
}
static void d_pci_enable(const bdrv_pci_t* b) { pci_dev_t d; to_pci(b, &d); pci_enable(&d); }

static uint32_t d_in(uint16_t port, int width) { return width == 1 ? inb(port) : width == 2 ? inw(port) : inl(port); }
static void d_out(uint16_t port, int width, uint32_t v) {
    if (width == 1) outb(port, (uint8_t)v);
    else if (width == 2) outw(port, (uint16_t)v);
    else outl(port, v);
}

static void* d_mmio_map(uint64_t phys, uint64_t size) {
#ifndef __x86_64__
    if (phys + size > (4ull << 30)) return NULL;
#endif
    return mmio_map(phys, size) ? (void*)(uintptr_t)phys : NULL;
}
static void* d_alloc(unsigned long size) { void* p = kmalloc(size); if (p) memset(p, 0, size); return p; }
static void d_free(void* p) { if (p) kfree(p); }
/* DMA memory: the heap is identity-mapped below 4 GiB, so an aligned
 * piece of it is its own physical address (never freed: drivers keep it) */
static void* d_alloc_dma(unsigned long size, unsigned long align, uint64_t* phys) {
    if (align < 16) align = 16;
    if (align & (align - 1)) return NULL;
    uint8_t* raw = kmalloc(size + align);
    if (!raw) return NULL;
    uint8_t* p = (uint8_t*)(((uintptr_t)raw + align - 1) & ~(uintptr_t)(align - 1));
    memset(p, 0, size);
    if (phys) *phys = (uint64_t)(uintptr_t)p;
    return p;
}

static uint32_t d_ticks(void) { return timer_ms(); }
static void d_delay_us(uint32_t us) {
    /* port 0x80 takes about a microsecond on every PC */
    while (us--) inb(0x80);
}
static void d_sleep_ms(uint32_t ms) { task_sleep_ms(ms); }

/* interrupts: the kernel's handlers take no argument, so one trampoline
 * per line calls the drivers' handlers of that line */
#define IRQ_SLOTS 4
static struct { void (*fn)(void*); void* ctx; } g_irq[16][IRQ_SLOTS];
static void irq_run(int line) { for (int i = 0; i < IRQ_SLOTS; i++) if (g_irq[line][i].fn) g_irq[line][i].fn(g_irq[line][i].ctx); }
#define TR(n) static void irq_tr_##n(void) { irq_run(n); }
TR(0) TR(1) TR(2) TR(3) TR(4) TR(5) TR(6) TR(7) TR(8) TR(9) TR(10) TR(11) TR(12) TR(13) TR(14) TR(15)
static void (*const IRQ_TR[16])(void) = { irq_tr_0, irq_tr_1, irq_tr_2, irq_tr_3, irq_tr_4, irq_tr_5, irq_tr_6, irq_tr_7,
                                          irq_tr_8, irq_tr_9, irq_tr_10, irq_tr_11, irq_tr_12, irq_tr_13, irq_tr_14, irq_tr_15 };
static int d_irq_install(int line, void (*fn)(void*), void* ctx) {
    if (line < 1 || line > 15 || line == 2 || !fn) return -1;     /* (0: the timer, 2: the cascade) */
    for (int i = 0; i < IRQ_SLOTS; i++)
        if (!g_irq[line][i].fn) {
            g_irq[line][i].ctx = ctx;
            g_irq[line][i].fn = fn;
            irq_install(line, IRQ_TR[line]);
            return 0;
        }
    return -1;
}

/* tasks: the same, one trampoline per slot */
#define TASK_SLOTS 8
static struct { void (*fn)(void*); void* arg; } g_tasks[TASK_SLOTS];
#define TT(n) static void task_tr_##n(void) { task_set_background(); g_tasks[n].fn(g_tasks[n].arg); for (;;) task_sleep_ms(1000); }
TT(0) TT(1) TT(2) TT(3) TT(4) TT(5) TT(6) TT(7)
static void (*const TASK_TR[TASK_SLOTS])(void) = { task_tr_0, task_tr_1, task_tr_2, task_tr_3, task_tr_4, task_tr_5, task_tr_6, task_tr_7 };
static int d_task_create(const char* name, void (*fn)(void*), void* arg) {
    for (int i = 0; i < TASK_SLOTS; i++)
        if (!g_tasks[i].fn) {
            g_tasks[i].arg = arg;
            g_tasks[i].fn = fn;
            return task_create(name ? name : "driver", TASK_TR[i]) < 0 ? -1 : 0;
        }
    return -1;
}

/* ── PCI drivers ──────────────────────────────────────────────────── */
static int matches(const bdrv_pci_id_t* id, const pci_dev_t* d) {
    return (id->vendor == 0xFFFF || id->vendor == d->vendor) && (id->device == 0xFFFF || id->device == d->device) &&
           (id->class_code == 0xFF || id->class_code == d->class_code) && (id->subclass == 0xFF || id->subclass == d->subclass);
}
static int probe_cb(const pci_dev_t* d, void* ctx) {
    pdrv_t* p = ctx;
    for (int i = 0; i < p->drv.nids; i++) {
        if (!matches(&p->drv.ids[i], d)) continue;
        bdrv_pci_t b;
        to_bdrv(d, &b);
        if (p->drv.probe(&b) == 0) {
            p->bound++;
            size_t n = strlen(p->devices);
            if (n < sizeof(p->devices) - 16)
                ksnprintf(p->devices + n, sizeof(p->devices) - n, "%s%04x:%04x@%02x:%02x.%u", n ? ", " : "", d->vendor, d->device, d->bus, d->dev, d->fn);
        }
        break;
    }
    return 0;
}
static int d_register_pci_driver(const bdrv_pci_driver_t* drv) {
    if (!drv || !drv->probe || !drv->ids || drv->nids <= 0 || g_npdrv == PDRV_MAX) return -1;
    pdrv_t* p = &g_pdrv[g_npdrv++];
    memset(p, 0, sizeof(*p));
    p->drv = *drv;
    p->module = g_loading;
    pci_scan(probe_cb, p);
    return p->bound;
}

/* ── displays: a module's display seen as the kernel's gpu_t ──────── */
typedef struct { gpu_t g; bdrv_display_t* d; } mod_gpu_t;
#define MOD_GPUS 4
static mod_gpu_t g_mgpu[MOD_GPUS];
static int g_nmgpu;

static bdrv_display_t* disp(gpu_t* g) { return ((mod_gpu_t*)g)->d; }
static int mg_modes(gpu_t* g, display_mode_t* out, int max) {
    bdrv_display_t* d = disp(g);
    if (!d->modes || max <= 0) return 0;
    bdrv_mode_t m[32];
    int n = d->modes(d, m, max < 32 ? max : 32);
    for (int i = 0; i < n; i++) { out[i].w = m[i].w; out[i].h = m[i].h; }
    return n < 0 ? 0 : n;
}
static int mg_set_mode(gpu_t* g, int w, int h) {
    bdrv_display_t* d = disp(g);
    uint64_t fb = 0;
    int pitch = 0;
    if (d->set_mode(d, w, h, &fb, &pitch) != 0 || !fb || pitch < w * 4) return -1;
#ifndef __x86_64__
    if (fb >> 32) return -1;
#endif
    gpu_set_framebuffer((uintptr_t)fb, w, h, pitch);
    return 0;
}
static void mg_flush(gpu_t* g, int x, int y, int w, int h) { bdrv_display_t* d = disp(g); d->flush(d, x, y, w, h); }
static int mg_cursor_image(gpu_t* g, const uint32_t* a, int w, int h, int hx, int hy) { bdrv_display_t* d = disp(g); return d->cursor_image(d, a, w, h, hx, hy); }
static void mg_cursor_move(gpu_t* g, int x, int y, int v) { bdrv_display_t* d = disp(g); d->cursor_move(d, x, y, v); }
static void mg_vblank(gpu_t* g) { bdrv_display_t* d = disp(g); d->wait_vblank(d); }
static int mg_backlight(gpu_t* g, int p) { bdrv_display_t* d = disp(g); return d->backlight(d, p); }
static void mg_info(gpu_t* g, char* buf, int cap) {
    (void)g;
    ksnprintf(buf, (size_t)cap, "a driver module (Banana OS Driver Kit)");
}

static int d_register_display(bdrv_display_t* d, const bdrv_pci_t* dev) {
    if (!d || !d->set_mode || g_nmgpu == MOD_GPUS) return -1;
    mod_gpu_t* m = &g_mgpu[g_nmgpu];
    memset(m, 0, sizeof(*m));
    m->d = d;
    kstrlcpy(m->g.name, d->name ? d->name : "display", sizeof(m->g.name));
    m->g.driver = g_loading >= 0 ? g_mods[g_loading].name : "module";
    if (dev) to_pci(dev, &m->g.pci);
    m->g.vram = d->vram;
    m->g.modes = mg_modes;
    m->g.set_mode = mg_set_mode;
    if (d->flush) m->g.flush = mg_flush;
    if (d->cursor_image && d->cursor_move) { m->g.cursor_image = mg_cursor_image; m->g.cursor_move = mg_cursor_move; }
    if (d->wait_vblank) m->g.wait_vblank = mg_vblank;
    if (d->backlight) m->g.backlight = mg_backlight;
    m->g.info = mg_info;
    /* the screen at its current size (or 1024x768) from the new driver */
    const fb_info_t* fi = fb_info();
    int w = fb_available() ? (int)fi->width : 1024, h = fb_available() ? (int)fi->height : 768;
    if (mg_set_mode(&m->g, w, h) < 0 && mg_set_mode(&m->g, 1024, 768) < 0) { klog("driver: %s: no mode it can show\n", m->g.name); return -1; }
    g_nmgpu++;
    gpu_register(&m->g);
    terminal_screen_changed();
    gpu_flush(0, 0, (int)fb_info()->width, (int)fb_info()->height);
    return 0;
}

static banana_driver_api_t g_api;

static void api_init(void) {
    if (g_api.magic) return;
    g_api.magic = BANANA_DRV_MAGIC;
    g_api.version = BANANA_DRV_VERSION;
    g_api.size = sizeof(g_api);
    g_api.arch = BANANA_ARCH;
    g_api.os_version = "0.5";
    g_api.log = d_log;
    g_api.vlog = d_vlog;
    g_api.pci_get = d_pci_get;
    g_api.pci_read = d_pci_read;
    g_api.pci_write = d_pci_write;
    g_api.pci_bar = d_pci_bar;
    g_api.pci_enable = d_pci_enable;
    g_api.port_in = d_in;
    g_api.port_out = d_out;
    g_api.mmio_map = d_mmio_map;
    g_api.alloc = d_alloc;
    g_api.free = d_free;
    g_api.alloc_dma = d_alloc_dma;
    g_api.ticks_ms = d_ticks;
    g_api.delay_us = d_delay_us;
    g_api.sleep_ms = d_sleep_ms;
    g_api.irq_install = d_irq_install;
    g_api.task_create = d_task_create;
    g_api.register_pci_driver = d_register_pci_driver;
    g_api.register_display = d_register_display;
}

/* ── loading ──────────────────────────────────────────────────────── */
typedef int (*driver_entry_t)(const banana_driver_api_t* api);

int driver_load(const char* path, const char* name, char* err, int ecap) {
    api_init();
    if (g_nmods == MOD_MAX) { kstrlcpy(err, "too many driver modules", (size_t)ecap); return -1; }
    for (int i = 0; i < g_nmods; i++)
        if (!strcmp(g_mods[i].name, name)) { ksnprintf(err, (size_t)ecap, "%s is loaded already (restart to load a new version)", name); return -1; }
    int fi = fs_find_file(path);
    if (fi < 0) { kstrlcpy(err, "no such file", (size_t)ecap); return -1; }
    fs_file_t* f = fs_get_file(fi);
    if (!f) { kstrlcpy(err, "cannot read it", (size_t)ecap); return -1; }
    fs_pin(fi);
    uint8_t* image = NULL;
    uintptr_t entry = 0;
    int r = app_load_image((const uint8_t*)f->content, f->size, &image, &entry, err, ecap);
    fs_unpin(fi);
    if (r < 0) return -1;
    module_t* m = &g_mods[g_nmods];
    memset(m, 0, sizeof(*m));
    kstrlcpy(m->name, name, sizeof(m->name));
    kstrlcpy(m->path, path, sizeof(m->path));
    m->image = image;
    g_loading = g_nmods++;
    klog("driver: loading %s (%s)\n", name, path);
    m->status = ((driver_entry_t)entry)(&g_api);
    g_loading = -1;
    if (m->status != 0) klog("driver: %s returned %d\n", name, m->status);
    /* (its memory stays: registered callbacks point into it) */
    return 0;
}

int driver_load_package(const char* name, char* err, int ecap) {
    char path[FS_PATH_LEN];
    ksnprintf(path, sizeof(path), "%s/%s/driver-%s", PKG_DIR, name, BANANA_ARCH);
    return driver_load(path, name, err, ecap);
}

void drivers_load_installed(void) {
    pkg_info_t list[32];
    int n = pkg_list_drivers(list, 32);
    for (int i = 0; i < n && i < 32; i++) {
        char err[96];
        if (driver_load_package(list[i].name, err, sizeof(err)) < 0) klog("driver: %s: %s\n", list[i].name, err);
    }
}

void drivers_list(void) {
    char line[200];
    gpu_t* g = gpu_active();
    ksnprintf(line, sizeof(line), "Display: %s (driver %s)", g ? g->name : "the firmware's framebuffer", g ? g->driver : "none");
    terminal_writeln(line);
    terminal_writeln("Built-in drivers also run sound, network, USB and storage: see lsaudio, ifconfig, lsusb, disks");
    terminal_writeln(g_nmods ? "Driver modules:" : "Driver modules: none (pkg install a driver .bpk; see sdk/driver)");
    for (int i = 0; i < g_nmods; i++) {
        ksnprintf(line, sizeof(line), "  %-14s %s%s", g_mods[i].name, g_mods[i].status == 0 ? "loaded" : "failed to start",
                  g_mods[i].status == 0 ? "" : " (see the kernel log)");
        terminal_writeln(line);
        for (int k = 0; k < g_npdrv; k++) {
            if (g_pdrv[k].module != i) continue;
            ksnprintf(line, sizeof(line), "      %s: %s", g_pdrv[k].drv.description ? g_pdrv[k].drv.description : g_pdrv[k].drv.name,
                      g_pdrv[k].bound ? g_pdrv[k].devices : "no device found");
            terminal_writeln(line);
        }
        for (int k = 0; k < g_nmgpu; k++)
            if (!strcmp(g_mgpu[k].g.driver, g_mods[i].name)) {
                ksnprintf(line, sizeof(line), "      display: %s%s", g_mgpu[k].g.name, gpu_active() == &g_mgpu[k].g ? " (active)" : "");
                terminal_writeln(line);
            }
    }
}
