/* virtio-gpu (2D): QEMU / KVM -vga virtio and -device virtio-gpu-pci,
 * crosvm, cloud hypervisors.
 *
 * A "modern" virtio device (virtio 1.x over PCI: its registers are found
 * through PCI capabilities). The screen is a host resource whose memory
 * is ours (a buffer in RAM, "attached backing"): after drawing, a
 * TRANSFER_TO_HOST_2D copies the changed rectangle over and a
 * RESOURCE_FLUSH shows it. Commands go through the control queue, the
 * pointer through the cursor queue (a 64x64 resource of its own).
 * After the virtio 1.2 specification, section 5.7. */
#include "gpu.h"
#include "fb.h"
#include "kheap.h"
#include "kstring.h"
#include "serial.h"
#include "paging.h"

/* PCI capabilities of a virtio device */
#define CAP_VENDOR       0x09
#define CFG_COMMON       1
#define CFG_NOTIFY       2
#define CFG_ISR          3
#define CFG_DEVICE       4

/* the common configuration */
#define C_DFSELECT   0x00
#define C_DF         0x04
#define C_GFSELECT   0x08
#define C_GF         0x0C
#define C_NUMQ       0x12
#define C_STATUS     0x14
#define C_QSELECT    0x16
#define C_QSIZE      0x18
#define C_QENABLE    0x1C
#define C_QNOTIFYOFF 0x1E
#define C_QDESC      0x20
#define C_QDRIVER    0x28
#define C_QDEVICE    0x30

#define S_ACK         1
#define S_DRIVER      2
#define S_DRIVER_OK   4
#define S_FEATURES_OK 8
#define S_FAILED      128

/* virtio-gpu commands */
#define GET_DISPLAY_INFO        0x0100
#define RESOURCE_CREATE_2D      0x0101
#define RESOURCE_UNREF          0x0102
#define SET_SCANOUT             0x0103
#define RESOURCE_FLUSH          0x0104
#define TRANSFER_TO_HOST_2D     0x0105
#define RESOURCE_ATTACH_BACKING 0x0106
#define RESOURCE_DETACH_BACKING 0x0107
#define UPDATE_CURSOR           0x0300
#define MOVE_CURSOR             0x0301
#define RESP_OK_NODATA          0x1100
#define RESP_OK_DISPLAY_INFO    0x1101
#define FMT_B8G8R8X8            2        /* our 0x00RRGGBB, little endian */
#define FMT_B8G8R8A8            1        /* 0xAARRGGBB */

#define QSIZE 64

typedef struct __attribute__((packed)) { uint64_t addr; uint32_t len; uint16_t flags, next; } vdesc_t;
typedef struct __attribute__((packed)) { uint32_t type, flags; uint64_t fence; uint32_t ctx, pad; } hdr_t;
typedef struct __attribute__((packed)) { uint32_t x, y, w, h; } rect_t;

typedef struct {
    vdesc_t*           desc;
    volatile uint16_t* avail;            /* flags, idx, ring[] */
    volatile uint16_t* used;             /* flags, idx, then { u32 id, u32 len }[] */
    volatile uint16_t* notify;
    uint16_t           size, avail_idx, last_used, qi;
} vq_t;

static gpu_t g_gpu;
static volatile uint8_t* g_common;
static volatile uint8_t* g_notify_base;
static uint32_t g_notify_mul;
static vq_t g_cq, g_curq;                /* control, cursor */
static uint8_t* g_req;                   /* command buffers (identity-mapped heap memory) */
static uint8_t* g_resp;
static uint8_t* g_curreq;

static uint32_t* g_fbmem;                /* the screen's memory */
static void*     g_fbraw;                 /* (its allocation) */
static int g_w, g_h, g_res = 1;          /* its size and resource id */
static int g_pref_w, g_pref_h;
static uint32_t* g_curimg;               /* 64x64 pointer image */
static int g_cursor_res;

static uint8_t  c8(int o) { return g_common[o]; }
static uint16_t c16(int o) { return *(volatile uint16_t*)(g_common + o); }
static uint32_t c32(int o) { return *(volatile uint32_t*)(g_common + o); }
static void w8(int o, uint8_t v) { g_common[o] = v; }
static void w16(int o, uint16_t v) { *(volatile uint16_t*)(g_common + o) = v; }
static void w32(int o, uint32_t v) { *(volatile uint32_t*)(g_common + o) = v; }
static void w64(int o, uint64_t v) { w32(o, (uint32_t)v); w32(o + 4, (uint32_t)(v >> 32)); }

static void* zalloc_aligned_raw(uint32_t size, uint32_t align, void** raw_out) {
    uint8_t* raw = kmalloc(size + align);
    if (raw_out) *raw_out = raw;
    if (!raw) return NULL;
    uint8_t* p = (uint8_t*)(((uintptr_t)raw + align - 1) & ~(uintptr_t)(align - 1));
    memset(p, 0, size);
    return p;
}
static void* zalloc_aligned(uint32_t size, uint32_t align) { return zalloc_aligned_raw(size, align, NULL); }

/* one operation at a time (the flusher task, the desktop) */
#define LOCK()   uintptr_t lk_fl; __asm__ volatile("pushf; pop %0; cli" : "=r"(lk_fl) :: "memory")
#define UNLOCK() do { if (lk_fl & 0x200) __asm__ volatile("sti" ::: "memory"); } while (0)

static int setup_queue(vq_t* q, int qi) {
    w16(C_QSELECT, (uint16_t)qi);
    uint16_t size = c16(C_QSIZE);
    if (!size) return -1;
    if (size > QSIZE) { size = QSIZE; w16(C_QSIZE, size); }
    q->size = size;
    q->qi = (uint16_t)qi;
    q->desc = zalloc_aligned(16u * size, 16);
    q->avail = zalloc_aligned(6u + 2u * size, 4);
    q->used = zalloc_aligned(6u + 8u * size, 4);
    if (!q->desc || !q->avail || !q->used) return -1;
    w64(C_QDESC, (uintptr_t)q->desc);
    w64(C_QDRIVER, (uintptr_t)q->avail);
    w64(C_QDEVICE, (uintptr_t)q->used);
    q->notify = (volatile uint16_t*)(g_notify_base + (uint32_t)c16(C_QNOTIFYOFF) * g_notify_mul);
    w16(C_QENABLE, 1);
    return 0;
}

/* sends a request (and waits for the device): 0 if it answered OK */
static int submit(vq_t* q, void* req, uint32_t rlen, void* resp, uint32_t slen) {
    uintptr_t fl;
    __asm__ volatile("pushf; pop %0; cli" : "=r"(fl) :: "memory");
    q->desc[0].addr = (uintptr_t)req;
    q->desc[0].len = rlen;
    q->desc[0].flags = 1;                   /* NEXT */
    q->desc[0].next = 1;
    q->desc[1].addr = (uintptr_t)resp;
    q->desc[1].len = slen;
    q->desc[1].flags = 2;                   /* WRITE: the device fills it */
    q->desc[1].next = 0;
    q->avail[2 + q->avail_idx % q->size] = 0;
    __asm__ volatile("mfence" ::: "memory");
    q->avail_idx++;
    q->avail[1] = q->avail_idx;
    __asm__ volatile("mfence" ::: "memory");
    *q->notify = q->qi;
    int ok = 0;
    for (uint32_t i = 0; i < 50000000u; i++) {
        if (q->used[1] != q->last_used) { ok = 1; break; }
        __asm__ volatile("pause");
    }
    if (ok) q->last_used++;
    if (fl & 0x200) __asm__ volatile("sti" ::: "memory");
    if (!ok) { klog("virtio-gpu: no answer from the device\n"); return -1; }
    uint32_t t = ((hdr_t*)resp)->type;
    return t == RESP_OK_NODATA || t == RESP_OK_DISPLAY_INFO ? 0 : -1;
}

static int ctrl(void* req, uint32_t rlen) { return submit(&g_cq, req, rlen, g_resp, sizeof(hdr_t)); }

static void hdr(void* p, uint32_t type) { memset(p, 0, sizeof(hdr_t)); ((hdr_t*)p)->type = type; }

static int create_screen(int res, int w, int h, uint32_t* mem) {
    struct __attribute__((packed)) { hdr_t h; uint32_t id, fmt, w, h2; } *c = (void*)g_req;
    hdr(c, RESOURCE_CREATE_2D);
    c->id = (uint32_t)res; c->fmt = FMT_B8G8R8X8; c->w = (uint32_t)w; c->h2 = (uint32_t)h;
    if (ctrl(c, sizeof(*c)) < 0) return -1;
    struct __attribute__((packed)) { hdr_t h; uint32_t id, n; uint64_t addr; uint32_t len, pad; } *b = (void*)g_req;
    hdr(b, RESOURCE_ATTACH_BACKING);
    b->id = (uint32_t)res; b->n = 1; b->addr = (uintptr_t)mem; b->len = (uint32_t)w * (uint32_t)h * 4u;
    if (ctrl(b, sizeof(*b)) < 0) return -1;
    struct __attribute__((packed)) { hdr_t h; rect_t r; uint32_t scanout, id; } *s = (void*)g_req;
    hdr(s, SET_SCANOUT);
    s->r = (rect_t){ 0, 0, (uint32_t)w, (uint32_t)h }; s->scanout = 0; s->id = (uint32_t)res;
    return ctrl(s, sizeof(*s));
}

static void drop_resource(int res) {
    struct __attribute__((packed)) { hdr_t h; uint32_t id, pad; } *c = (void*)g_req;
    hdr(c, RESOURCE_DETACH_BACKING);
    c->id = (uint32_t)res;
    ctrl(c, sizeof(*c));
    hdr(c, RESOURCE_UNREF);
    c->id = (uint32_t)res;
    ctrl(c, sizeof(*c));
}

static void vg_flush_locked(int x, int y, int w, int h);
static void vg_flush(gpu_t* g, int x, int y, int w, int h) {
    (void)g;
    LOCK();
    vg_flush_locked(x, y, w, h);
    UNLOCK();
}

static void vg_flush_locked(int x, int y, int w, int h) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > g_w) w = g_w - x;
    if (y + h > g_h) h = g_h - y;
    if (w <= 0 || h <= 0) return;
    struct __attribute__((packed)) { hdr_t h; rect_t r; uint64_t off; uint32_t id, pad; } *t = (void*)g_req;
    hdr(t, TRANSFER_TO_HOST_2D);
    t->r = (rect_t){ (uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h };
    t->off = (uint64_t)y * (uint64_t)g_w * 4u + (uint64_t)x * 4u;
    t->id = (uint32_t)g_res;
    if (ctrl(t, sizeof(*t)) < 0) return;
    struct __attribute__((packed)) { hdr_t h; rect_t r; uint32_t id, pad; } *f = (void*)g_req;
    hdr(f, RESOURCE_FLUSH);
    f->r = (rect_t){ (uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h };
    f->id = (uint32_t)g_res;
    ctrl(f, sizeof(*f));
}

static const display_mode_t MODES[] = {
    { 800, 600 }, { 1024, 768 }, { 1280, 720 }, { 1280, 800 }, { 1280, 1024 }, { 1366, 768 },
    { 1440, 900 }, { 1600, 900 }, { 1680, 1050 }, { 1920, 1080 }, { 1920, 1200 }, { 2560, 1440 },
};
#define NMODES ((int)(sizeof(MODES) / sizeof(MODES[0])))

static int vg_modes(gpu_t* g, display_mode_t* out, int max) {
    (void)g;
    int n = 0, pref = 0;
    for (int i = 0; i < NMODES && n < max; i++) {
        if (MODES[i].w == g_pref_w && MODES[i].h == g_pref_h) pref = 1;
        out[n++] = MODES[i];
    }
    if (!pref && g_pref_w >= 640 && n < max) out[n++] = (display_mode_t){ g_pref_w, g_pref_h };
    return n;
}

static int vg_set_mode(gpu_t* g, int w, int h) {
    (void)g;
    if (w < 640 || h < 480 || w > 4096 || h > 2160) return -1;
    void* raw;
    uint32_t* mem = zalloc_aligned_raw((uint32_t)w * (uint32_t)h * 4u, 4096, &raw);
    if (!mem) return -1;
    LOCK();
    int res = g_res == 1 ? 2 : 1;
    if (create_screen(res, w, h, mem) < 0) { drop_resource(res); UNLOCK(); kfree(raw); return -1; }
    int old = g_res;
    void* old_raw = g_fbraw;
    g_res = res;
    g_w = w;
    g_h = h;
    g_fbmem = mem;
    g_fbraw = raw;
    gpu_set_framebuffer((uintptr_t)mem, w, h, w * 4);
    drop_resource(old);
    UNLOCK();
    if (old_raw) kfree(old_raw);
    return 0;
}

/* the pointer: its own 64x64 resource, moved on the cursor queue */
static int vg_cursor_image_locked(const uint32_t* argb, int w, int h, int hx, int hy);
static int vg_cursor_image(gpu_t* g, const uint32_t* argb, int w, int h, int hx, int hy) {
    (void)g;
    LOCK();
    int r = vg_cursor_image_locked(argb, w, h, hx, hy);
    UNLOCK();
    return r;
}

static int vg_cursor_image_locked(const uint32_t* argb, int w, int h, int hx, int hy) {
    if (w > 64 || h > 64 || !g_curq.size) return -1;
    if (!g_curimg) {
        g_curimg = zalloc_aligned(64 * 64 * 4, 4096);
        if (!g_curimg) return -1;
        g_cursor_res = 3;
        struct __attribute__((packed)) { hdr_t h; uint32_t id, fmt, w, h2; } *c = (void*)g_req;
        hdr(c, RESOURCE_CREATE_2D);
        c->id = 3; c->fmt = FMT_B8G8R8A8; c->w = 64; c->h2 = 64;
        if (ctrl(c, sizeof(*c)) < 0) return -1;
        struct __attribute__((packed)) { hdr_t h; uint32_t id, n; uint64_t addr; uint32_t len, pad; } *b = (void*)g_req;
        hdr(b, RESOURCE_ATTACH_BACKING);
        b->id = 3; b->n = 1; b->addr = (uintptr_t)g_curimg; b->len = 64 * 64 * 4;
        if (ctrl(b, sizeof(*b)) < 0) return -1;
    }
    memset(g_curimg, 0, 64 * 64 * 4);
    for (int y = 0; y < h; y++) memcpy(g_curimg + y * 64, argb + y * w, (size_t)w * 4);
    struct __attribute__((packed)) { hdr_t h; rect_t r; uint64_t off; uint32_t id, pad; } *t = (void*)g_req;
    hdr(t, TRANSFER_TO_HOST_2D);
    t->r = (rect_t){ 0, 0, 64, 64 };
    t->off = 0;
    t->id = 3;
    if (ctrl(t, sizeof(*t)) < 0) return -1;
    struct __attribute__((packed)) { hdr_t h; uint32_t scanout, x, y, pad, id, hx, hy, pad2; } *u = (void*)g_curreq;
    hdr(u, UPDATE_CURSOR);
    u->scanout = 0; u->x = 0; u->y = 0; u->id = 3; u->hx = (uint32_t)hx; u->hy = (uint32_t)hy;
    return submit(&g_curq, u, sizeof(*u), g_resp, sizeof(hdr_t));
}

static void vg_cursor_move(gpu_t* g, int x, int y, int visible) {
    (void)g;
    if (!g_curimg) return;
    LOCK();
    struct __attribute__((packed)) { hdr_t h; uint32_t scanout, x, y, pad, id, hx, hy, pad2; } *u = (void*)g_curreq;
    /* hidden: the cursor without a resource */
    hdr(u, visible ? MOVE_CURSOR : UPDATE_CURSOR);
    u->scanout = 0; u->x = (uint32_t)(x < 0 ? 0 : x); u->y = (uint32_t)(y < 0 ? 0 : y);
    u->id = visible ? 3u : 0u;
    submit(&g_curq, u, sizeof(*u), g_resp, sizeof(hdr_t));
    UNLOCK();
}

static void vg_info(gpu_t* g, char* buf, int cap) {
    (void)g;
    ksnprintf(buf, (size_t)cap, "virtio 1.x 2D, screen resource %d, host's preferred size %dx%d", g_res, g_pref_w, g_pref_h);
}

/* the virtio capabilities: where the common, notify and device areas are */
static int find_caps(const pci_dev_t* d) {
    if (!(pci_read16(d->bus, d->dev, d->fn, 0x06) & 0x10)) return -1;    /* no capability list */
    uint8_t p = (uint8_t)(pci_read32(d->bus, d->dev, d->fn, 0x34) & 0xFC);
    for (int guard = 0; p && guard < 48; guard++) {
        uint32_t w0 = pci_read32(d->bus, d->dev, d->fn, p);
        uint8_t id = (uint8_t)w0, next = (uint8_t)(w0 >> 8), type = (uint8_t)(w0 >> 24);
        if (id == CAP_VENDOR) {
            uint8_t bar = (uint8_t)pci_read32(d->bus, d->dev, d->fn, (uint8_t)(p + 4));
            uint32_t off = pci_read32(d->bus, d->dev, d->fn, (uint8_t)(p + 8));
            uint32_t len = pci_read32(d->bus, d->dev, d->fn, (uint8_t)(p + 12));
            int io = 0;
            uintptr_t base = bar < 6 ? pci_bar(d, bar, &io) : 0;
            if (base && !io && mmio_map(base + off, len ? len : 4096)) {
                if (type == CFG_COMMON) g_common = (volatile uint8_t*)(base + off);
                else if (type == CFG_NOTIFY) {
                    g_notify_base = (volatile uint8_t*)(base + off);
                    g_notify_mul = pci_read32(d->bus, d->dev, d->fn, (uint8_t)(p + 16));
                }
            }
        }
        p = next & 0xFC;
    }
    return g_common && g_notify_base ? 0 : -1;
}

int gpu_virtio_probe(const pci_dev_t* d) {
    if (d->vendor != 0x1AF4 || d->device != 0x1050) return -1;
    pci_enable(d);
    if (find_caps(d) < 0) { klog("virtio-gpu: no modern virtio interface\n"); return -1; }
    w8(C_STATUS, 0);                                   /* reset */
    for (int i = 0; i < 100000 && c8(C_STATUS); i++) ;
    w8(C_STATUS, S_ACK);
    w8(C_STATUS, S_ACK | S_DRIVER);
    w32(C_DFSELECT, 1);
    uint32_t hi = c32(C_DF);
    if (!(hi & 1)) { w8(C_STATUS, S_FAILED); return -1; }   /* VIRTIO_F_VERSION_1 */
    w32(C_GFSELECT, 0);
    w32(C_GF, 0);                                      /* no extra features (2D only) */
    w32(C_GFSELECT, 1);
    w32(C_GF, 1);
    w8(C_STATUS, S_ACK | S_DRIVER | S_FEATURES_OK);
    if (!(c8(C_STATUS) & S_FEATURES_OK)) { w8(C_STATUS, S_FAILED); return -1; }
    g_req = zalloc_aligned(4096, 64);
    g_resp = zalloc_aligned(4096, 64);
    g_curreq = zalloc_aligned(256, 64);
    if (!g_req || !g_resp || !g_curreq || setup_queue(&g_cq, 0) < 0) { w8(C_STATUS, S_FAILED); return -1; }
    if (c16(C_NUMQ) > 1 && setup_queue(&g_curq, 1) < 0) g_curq.size = 0;
    w8(C_STATUS, S_ACK | S_DRIVER | S_FEATURES_OK | S_DRIVER_OK);

    /* the size the host would like (its window) */
    hdr(g_req, GET_DISPLAY_INFO);
    g_pref_w = 1024;
    g_pref_h = 768;
    if (submit(&g_cq, g_req, sizeof(hdr_t), g_resp, sizeof(hdr_t) + 16 * 24) == 0) {
        const uint32_t* pm = (const uint32_t*)(g_resp + sizeof(hdr_t));
        if (pm[4] && pm[2] >= 640 && pm[3] >= 480) { g_pref_w = (int)pm[2]; g_pref_h = (int)pm[3]; }
    }
    /* the mode the firmware set (if any), else the host's */
    const fb_info_t* fi = fb_info();
    int w = fb_available() && fi->bpp == 32 ? (int)fi->width : g_pref_w;
    int h = fb_available() && fi->bpp == 32 ? (int)fi->height : g_pref_h;
    g_fbmem = zalloc_aligned_raw((uint32_t)w * (uint32_t)h * 4u, 4096, &g_fbraw);
    if (!g_fbmem || create_screen(g_res, w, h, g_fbmem) < 0) { klog("virtio-gpu: the screen could not be set up\n"); return -1; }
    g_w = w;
    g_h = h;

    memset(&g_gpu, 0, sizeof(g_gpu));
    kstrlcpy(g_gpu.name, d->class_code == 0x03 && d->subclass == 0x00 ? "virtio-gpu (virtio-vga)" : "virtio-gpu", sizeof(g_gpu.name));
    g_gpu.driver = "virtio";
    g_gpu.pci = *d;
    g_gpu.modes = vg_modes;
    g_gpu.set_mode = vg_set_mode;
    g_gpu.flush = vg_flush;
    if (g_curq.size) { g_gpu.cursor_image = vg_cursor_image; g_gpu.cursor_move = vg_cursor_move; }
    g_gpu.info = vg_info;
    gpu_set_framebuffer((uintptr_t)g_fbmem, w, h, w * 4);
    gpu_register(&g_gpu);
    vg_flush_locked(0, 0, w, h);
    return 0;
}
