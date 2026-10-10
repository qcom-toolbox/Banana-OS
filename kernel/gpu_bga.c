/* The Bochs "VBE DISPI" display: QEMU's standard VGA (-vga std) and
 * bochs-display, Bochs, VirtualBox's VBoxVGA. Its registers are behind
 * two I/O ports - or, on bochs-display and newer QEMU VGAs, in memory
 * (BAR2 + 0x500) - and the framebuffer is BAR0. */
#include "gpu.h"
#include "fb.h"
#include "io.h"
#include "kstring.h"
#include "serial.h"

#define DISPI_INDEX   0x01CE
#define DISPI_DATA    0x01CF
#define DISPI_ID      0
#define DISPI_XRES    1
#define DISPI_YRES    2
#define DISPI_BPP     3
#define DISPI_ENABLE  4
#define DISPI_VWIDTH  6
#define DISPI_VHEIGHT 7
#define DISPI_XOFF    8
#define DISPI_YOFF    9
#define DISPI_VRAM64K 10                     /* video memory in 64 KiB units (ID 0xB0C5) */
#define DISPI_ENABLED 0x01
#define DISPI_GETCAPS 0x02
#define DISPI_LFB     0x40

static const display_mode_t MODES[] = {
    { 800, 600 }, { 1024, 768 }, { 1280, 720 }, { 1280, 800 }, { 1280, 1024 }, { 1366, 768 },
    { 1440, 900 }, { 1600, 900 }, { 1680, 1050 }, { 1920, 1080 }, { 1920, 1200 }, { 2560, 1440 },
};
#define NMODES ((int)(sizeof(MODES) / sizeof(MODES[0])))

typedef struct {
    volatile uint16_t* mmio;               /* the registers in memory, or NULL: the ports */
    uintptr_t lfb;
    uint16_t  max_w, max_h;
} bga_t;

static gpu_t g_gpu;
static bga_t g_bga;

static void wr(uint16_t idx, uint16_t v) {
    if (g_bga.mmio) g_bga.mmio[idx] = v;
    else { outw(DISPI_INDEX, idx); outw(DISPI_DATA, v); }
}
static uint16_t rd(uint16_t idx) {
    if (g_bga.mmio) return g_bga.mmio[idx];
    outw(DISPI_INDEX, idx);
    return inw(DISPI_DATA);
}

static int bga_modes(gpu_t* g, display_mode_t* out, int max) {
    (void)g;
    int n = 0;
    uint32_t vram = g_gpu.vram ? g_gpu.vram : 16u << 20;
    for (int i = 0; i < NMODES && n < max; i++)
        if (MODES[i].w <= g_bga.max_w && MODES[i].h <= g_bga.max_h && (uint32_t)MODES[i].w * (uint32_t)MODES[i].h * 4u <= vram)
            out[n++] = MODES[i];
    return n;
}

static int program(int w, int h) {
    wr(DISPI_ENABLE, 0);
    wr(DISPI_XRES, (uint16_t)w);
    wr(DISPI_YRES, (uint16_t)h);
    wr(DISPI_BPP, 32);
    wr(DISPI_VWIDTH, (uint16_t)w);
    wr(DISPI_VHEIGHT, (uint16_t)h);
    wr(DISPI_XOFF, 0);
    wr(DISPI_YOFF, 0);
    wr(DISPI_ENABLE, DISPI_ENABLED | DISPI_LFB);
    return rd(DISPI_XRES) == w && rd(DISPI_YRES) == h ? 0 : -1;
}

static int bga_set_mode(gpu_t* g, int w, int h) {
    (void)g;
    if (w < 640 || h < 480 || w > g_bga.max_w || h > g_bga.max_h) return -1;
    const fb_info_t* fi = fb_info();
    if (fb_available() && (int)fi->width == w && (int)fi->height == h) return 0;
    if (program(w, h) < 0) return -1;
    if (fb_available() && fi->addr == g_bga.lfb) fb_reconfigure((uint32_t)w, (uint32_t)h, (uint32_t)w * 4);
    else gpu_set_framebuffer(g_bga.lfb, w, h, w * 4);
    return 0;
}

static void bga_info(gpu_t* g, char* buf, int cap) {
    (void)g;
    ksnprintf(buf, (size_t)cap, "interface: VBE DISPI 0x%x (%s), largest mode %ux%u", rd(DISPI_ID),
              g_bga.mmio ? "memory-mapped registers" : "I/O ports 0x1CE/0x1CF", g_bga.max_w, g_bga.max_h);
}

int gpu_bga_probe(const pci_dev_t* d) {
    int ok = (d->vendor == 0x1234 && d->device == 0x1111) ||       /* QEMU std VGA, bochs-display */
             (d->vendor == 0x80EE && d->device == 0xBEEF);          /* VirtualBox VBoxVGA */
    if (!ok) return -1;
    memset(&g_bga, 0, sizeof(g_bga));
    pci_enable(d);
    int io = 0;
    g_bga.lfb = pci_bar(d, 0, &io);
    if (io || !g_bga.lfb) return -1;
    uint16_t id = rd(DISPI_ID);
    if (id < 0xB0C0 || id > 0xB0C5) {
        /* no ports (bochs-display): the registers at BAR2 + 0x500 */
        uintptr_t mm = pci_bar(d, 2, &io);
        if (!mm || io) return -1;
        g_bga.mmio = (volatile uint16_t*)(mm + 0x500);
        id = rd(DISPI_ID);
        if (id < 0xB0C0 || id > 0xB0C5) return -1;
    }
    uint16_t en = rd(DISPI_ENABLE);
    wr(DISPI_ENABLE, en | DISPI_GETCAPS);        /* the largest mode, then back */
    g_bga.max_w = rd(DISPI_XRES);
    g_bga.max_h = rd(DISPI_YRES);
    wr(DISPI_ENABLE, en);
    if (g_bga.max_w < 800 || g_bga.max_h < 600) { g_bga.max_w = 1920; g_bga.max_h = 1200; }
    if (g_bga.max_w > 2560) g_bga.max_w = 2560;
    if (g_bga.max_h > 1600) g_bga.max_h = 1600;

    memset(&g_gpu, 0, sizeof(g_gpu));
    kstrlcpy(g_gpu.name, d->vendor == 0x80EE ? "VirtualBox Graphics Adapter (VBoxVGA)" :
             d->class_code == 0x03 && d->subclass == 0x80 ? "QEMU bochs-display" : "QEMU / Bochs standard VGA", sizeof(g_gpu.name));
    g_gpu.driver = "bga";
    g_gpu.pci = *d;
    g_gpu.vram = id >= 0xB0C5 ? (uint32_t)rd(DISPI_VRAM64K) << 16 : 0;
    g_gpu.modes = bga_modes;
    g_gpu.set_mode = bga_set_mode;
    g_gpu.info = bga_info;
    /* the 32-bit framebuffer the firmware left must be this one; else (none,
     * or text mode) a mode of our own */
    const fb_info_t* fi = fb_info();
    if (!fb_available() || fi->bpp != 32) {
        if (program(1024, 768) < 0) return -1;
        gpu_set_framebuffer(g_bga.lfb, 1024, 768, 1024 * 4);
    }
    return gpu_register(&g_gpu);
}
