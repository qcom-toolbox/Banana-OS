/* VMware SVGA II: VMware Workstation / Player / ESXi, VirtualBox's VMSVGA
 * and VBoxSVGA adapters, QEMU -vga vmware.
 *
 * Registers through two I/O ports (BAR0: index, value); the framebuffer
 * and the command FIFO in memory (SVGA_REG_FB_START, SVGA_REG_MEM_START).
 * The host shows the framebuffer once told what changed: an UPDATE
 * command in the FIFO. The pointer is drawn by the host (an alpha cursor,
 * or the classic AND/XOR one). After the VMware SVGA Device Developer Kit
 * (svga_reg.h). */
#include "gpu.h"
#include "fb.h"
#include "io.h"
#include "kstring.h"
#include "serial.h"
#include "paging.h"

#define SVGA_INDEX       0
#define SVGA_VALUE       1
#define SVGA_ID_2        0x90000002u
#define SVGA_ID_1        0x90000001u

enum {
    R_ID = 0, R_ENABLE = 1, R_WIDTH = 2, R_HEIGHT = 3, R_MAX_WIDTH = 4, R_MAX_HEIGHT = 5, R_DEPTH = 6,
    R_BPP = 7, R_BYTES_PER_LINE = 12, R_FB_START = 13, R_FB_OFFSET = 14, R_VRAM_SIZE = 15, R_FB_SIZE = 16,
    R_CAPS = 17, R_MEM_START = 18, R_MEM_SIZE = 19, R_CONFIG_DONE = 20, R_SYNC = 21, R_BUSY = 22,
    R_GUEST_ID = 23, R_CURSOR_ID = 24, R_CURSOR_X = 25, R_CURSOR_Y = 26, R_CURSOR_ON = 27,
};
#define CAP_CURSOR          0x00000020u
#define CAP_CURSOR_BYPASS_2 0x00000080u
#define CAP_ALPHA_CURSOR    0x00000200u
#define CAP_EXTENDED_FIFO   0x00008000u

enum { F_MIN = 0, F_MAX = 1, F_NEXT_CMD = 2, F_STOP = 3 };
#define FIFO_NUM_REGS 293

#define CMD_UPDATE               1
#define CMD_DEFINE_CURSOR        19
#define CMD_DEFINE_ALPHA_CURSOR  22

static gpu_t g_gpu;
static uint16_t g_io;
static volatile uint32_t* g_fifo;
static uintptr_t g_fb;
static uint32_t g_caps, g_vram, g_fifo_size, g_max_w, g_max_h;
static int g_cursor_ok;

/* (an index then a value: the pointer moves from the timer interrupt too) */
static void wr(uint32_t r, uint32_t v) {
    uint32_t f = irq_save();
    outl(g_io + SVGA_INDEX, r);
    outl(g_io + SVGA_VALUE, v);
    irq_restore(f);
}
static uint32_t rd(uint32_t r) {
    uint32_t f = irq_save();
    outl(g_io + SVGA_INDEX, r);
    uint32_t v = inl(g_io + SVGA_VALUE);
    irq_restore(f);
    return v;
}

/* waits for the host to work the FIFO off */
static void fifo_sync(void) {
    wr(R_SYNC, 1);
    for (int i = 0; i < 10000000 && rd(R_BUSY); i++) ;
}

/* one whole command into the FIFO: the host sees it only once it is complete */
static void fifo_cmd_locked(const uint32_t* w, int n);

/* (the flusher task and the desktop both write commands: one at a time) */
static void fifo_cmd(const uint32_t* w, int n) {
    uintptr_t fl;
    __asm__ volatile("pushf; pop %0; cli" : "=r"(fl) :: "memory");
    fifo_cmd_locked(w, n);
    if (fl & 0x200) __asm__ volatile("sti" ::: "memory");
}

static void fifo_cmd_locked(const uint32_t* w, int n) {
    if (!g_fifo) return;
    uint32_t min = g_fifo[F_MIN], max = g_fifo[F_MAX];
    uint32_t bytes = (uint32_t)n * 4u;
    if (bytes > max - min - 4) return;
    for (int tries = 0;; tries++) {
        uint32_t next = g_fifo[F_NEXT_CMD], stop = g_fifo[F_STOP];
        uint32_t free_b = next >= stop ? (max - next) + (stop - min) : stop - next;
        if (free_b > bytes) break;
        fifo_sync();
        if (tries > 4) return;
    }
    uint32_t next = g_fifo[F_NEXT_CMD];
    for (int i = 0; i < n; i++) {
        g_fifo[next / 4] = w[i];
        next += 4;
        if (next >= max) next = min;
    }
    __asm__ volatile("" ::: "memory");
    g_fifo[F_NEXT_CMD] = next;
}

static void svga_flush(gpu_t* g, int x, int y, int w, int h) {
    (void)g;
    uint32_t c[5] = { CMD_UPDATE, (uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h };
    fifo_cmd(c, 5);
}

static const display_mode_t MODES[] = {
    { 800, 600 }, { 1024, 768 }, { 1152, 864 }, { 1280, 720 }, { 1280, 800 }, { 1280, 1024 }, { 1366, 768 },
    { 1440, 900 }, { 1600, 900 }, { 1600, 1200 }, { 1680, 1050 }, { 1920, 1080 }, { 1920, 1200 }, { 2560, 1440 },
};
#define NMODES ((int)(sizeof(MODES) / sizeof(MODES[0])))

static int svga_modes(gpu_t* g, display_mode_t* out, int max) {
    (void)g;
    int n = 0;
    for (int i = 0; i < NMODES && n < max; i++)
        if ((uint32_t)MODES[i].w <= g_max_w && (uint32_t)MODES[i].h <= g_max_h &&
            (uint32_t)MODES[i].w * (uint32_t)MODES[i].h * 4u <= g_vram) out[n++] = MODES[i];
    return n;
}

static int program(int w, int h) {
    wr(R_WIDTH, (uint32_t)w);
    wr(R_HEIGHT, (uint32_t)h);
    wr(R_BPP, 32);
    wr(R_ENABLE, 1);
    if (rd(R_WIDTH) != (uint32_t)w || rd(R_HEIGHT) != (uint32_t)h || rd(R_BPP) != 32) return -1;
    return 0;
}

static int svga_set_mode(gpu_t* g, int w, int h) {
    (void)g;
    if (w < 640 || h < 480 || (uint32_t)w > g_max_w || (uint32_t)h > g_max_h) return -1;
    if (program(w, h) < 0) return -1;
    uint32_t pitch = rd(R_BYTES_PER_LINE);
    gpu_set_framebuffer(g_fb + rd(R_FB_OFFSET), w, h, (int)pitch);
    svga_flush(g, 0, 0, w, h);
    return 0;
}

/* the pointer: ARGB with alpha when the host takes it, else AND/XOR masks */
static int svga_cursor_image(gpu_t* g, const uint32_t* argb, int w, int h, int hx, int hy) {
    (void)g;
    if (!g_cursor_ok || w > 64 || h > 64 || w <= 0 || h <= 0) return -1;
    static uint32_t cmd[6 + 64 * 64 * 2 + 64 * 2];
    int n = 0;
    if (g_caps & CAP_ALPHA_CURSOR) {
        cmd[n++] = CMD_DEFINE_ALPHA_CURSOR;
        cmd[n++] = 0; cmd[n++] = (uint32_t)hx; cmd[n++] = (uint32_t)hy; cmd[n++] = (uint32_t)w; cmd[n++] = (uint32_t)h;
        for (int i = 0; i < w * h; i++) {             /* premultiplied alpha */
            uint32_t p = argb[i], a = p >> 24;
            uint32_t r = ((p >> 16) & 255) * a / 255, gg = ((p >> 8) & 255) * a / 255, b = (p & 255) * a / 255;
            cmd[n++] = a << 24 | r << 16 | gg << 8 | b;
        }
    } else {
        int words = (w + 31) / 32;
        cmd[n++] = CMD_DEFINE_CURSOR;
        cmd[n++] = 0; cmd[n++] = (uint32_t)hx; cmd[n++] = (uint32_t)hy; cmd[n++] = (uint32_t)w; cmd[n++] = (uint32_t)h;
        cmd[n++] = 1;                                 /* AND mask: 1 bit per pixel */
        cmd[n++] = 32;                                /* XOR mask: 32-bit pixels */
        for (int y = 0; y < h; y++)
            for (int k = 0; k < words; k++) {
                uint32_t m = 0;
                for (int b = 0; b < 32; b++) {
                    int x = k * 32 + b;
                    /* bit 31 is the leftmost pixel; 1 keeps the screen */
                    if (x >= w || (argb[y * w + x] >> 24) < 128) m |= 0x80000000u >> b;
                }
                cmd[n++] = m;
            }
        for (int i = 0; i < w * h; i++) cmd[n++] = (argb[i] >> 24) >= 128 ? argb[i] & 0xFFFFFFu : 0;
    }
    fifo_cmd(cmd, n);
    return 0;
}

static void svga_cursor_move(gpu_t* g, int x, int y, int visible) {
    (void)g;
    if (!g_cursor_ok) return;
    wr(R_CURSOR_ID, 0);
    wr(R_CURSOR_X, (uint32_t)(x < 0 ? 0 : x));
    wr(R_CURSOR_Y, (uint32_t)(y < 0 ? 0 : y));
    wr(R_CURSOR_ON, visible ? 1 : 0);
}

static void svga_info(gpu_t* g, char* buf, int cap) {
    (void)g;
    ksnprintf(buf, (size_t)cap, "SVGA II, I/O 0x%x, framebuffer 0x%x, command FIFO %u KiB, largest mode %ux%u\ncapabilities 0x%x%s",
              g_io, (uint32_t)g_fb, g_fifo_size >> 10, g_max_w, g_max_h, g_caps,
              g_caps & CAP_ALPHA_CURSOR ? " (alpha cursor)" : g_caps & CAP_CURSOR ? " (cursor)" : "");
}

int gpu_vmsvga_probe(const pci_dev_t* d) {
    int vmware = d->vendor == 0x15AD && d->device == 0x0405;
    int vbox = d->vendor == 0x80EE && d->device == 0xBEEF;     /* VBoxSVGA (VBoxVGA has no I/O BAR) */
    if (!vmware && !vbox) return -1;
    int io = 0;
    uintptr_t b0 = pci_bar(d, 0, &io);
    if (!io || !b0) return -1;
    pci_enable(d);
    g_io = (uint16_t)b0;
    wr(R_ID, SVGA_ID_2);
    if (rd(R_ID) != SVGA_ID_2) {
        wr(R_ID, SVGA_ID_1);
        if (rd(R_ID) != SVGA_ID_1) return -1;
    }
    g_fb = (uintptr_t)rd(R_FB_START);
    g_vram = rd(R_VRAM_SIZE);
    g_caps = rd(R_CAPS);
    g_max_w = rd(R_MAX_WIDTH);
    g_max_h = rd(R_MAX_HEIGHT);
    uintptr_t fifo = (uintptr_t)rd(R_MEM_START);
    g_fifo_size = rd(R_MEM_SIZE);
    if (!g_fb || !fifo || g_fifo_size < 4096 || !g_max_w) return -1;
    if (!mmio_map(g_fb, g_vram ? g_vram : 16u << 20) || !mmio_map(fifo, g_fifo_size)) return -1;
    if (g_max_w > 2560) g_max_w = 2560;
    if (g_max_h > 1600) g_max_h = 1600;

    /* the command FIFO */
    g_fifo = (volatile uint32_t*)fifo;
    uint32_t min = (g_caps & CAP_EXTENDED_FIFO) ? FIFO_NUM_REGS * 4 : 16;
    g_fifo[F_MIN] = min;
    g_fifo[F_MAX] = g_fifo_size;
    g_fifo[F_NEXT_CMD] = min;
    g_fifo[F_STOP] = min;
    wr(R_GUEST_ID, 0x5010);                /* "other 64-bit" (an OS the host does not know) */
    wr(R_CONFIG_DONE, 1);
    g_cursor_ok = (g_caps & (CAP_ALPHA_CURSOR | CAP_CURSOR)) && (g_caps & CAP_CURSOR_BYPASS_2);

    memset(&g_gpu, 0, sizeof(g_gpu));
    kstrlcpy(g_gpu.name, vbox ? "VirtualBox SVGA (VBoxSVGA)" : "VMware SVGA II", sizeof(g_gpu.name));
    g_gpu.driver = "vmsvga";
    g_gpu.pci = *d;
    g_gpu.vram = g_vram;
    g_gpu.modes = svga_modes;
    g_gpu.set_mode = svga_set_mode;
    g_gpu.flush = svga_flush;
    if (g_cursor_ok) { g_gpu.cursor_image = svga_cursor_image; g_gpu.cursor_move = svga_cursor_move; g_gpu.cursor_irq_safe = 1; }
    g_gpu.info = svga_info;

    /* the mode the firmware set (same width / height), or one of ours */
    const fb_info_t* fi = fb_info();
    int w = fb_available() && fi->bpp == 32 ? (int)fi->width : 1024;
    int h = fb_available() && fi->bpp == 32 ? (int)fi->height : 768;
    if (program(w, h) < 0 && (w = 1024, h = 768, program(w, h) < 0)) return -1;
    gpu_set_framebuffer(g_fb + rd(R_FB_OFFSET), w, h, (int)rd(R_BYTES_PER_LINE));
    gpu_register(&g_gpu);
    svga_flush(&g_gpu, 0, 0, w, h);
    return 0;
}
