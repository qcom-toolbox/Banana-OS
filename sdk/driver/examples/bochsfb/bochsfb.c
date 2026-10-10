/* bochsfb - a Driver Kit example of a display driver: the Bochs / QEMU
 * standard VGA ("VBE DISPI", PCI 1234:1111). Banana OS has a driver of
 * its own for it; a display module replaces the built-in driver of its
 * card, so this one shows how a graphics card maker's driver plugs in:
 * modes, setting one, and handing the framebuffer to the system.
 *
 * Test it in QEMU (-vga std): pkg install bochsfb.bpk, then lsgpu. */
#include "banana_driver.h"

#define DISPI_INDEX 0x01CE
#define DISPI_DATA  0x01CF
enum { ID = 0, XRES = 1, YRES = 2, BPP = 3, ENABLE = 4, VWIDTH = 6, VHEIGHT = 7, XOFF = 8, YOFF = 9 };

static uint64_t g_lfb;
static bdrv_display_t g_disp;

static void wr(uint16_t i, uint16_t v) { bdrv->port_out(DISPI_INDEX, 2, i); bdrv->port_out(DISPI_DATA, 2, v); }
static uint16_t rd(uint16_t i) { bdrv->port_out(DISPI_INDEX, 2, i); return (uint16_t)bdrv->port_in(DISPI_DATA, 2); }

static const bdrv_mode_t MODES[] = { { 800, 600 }, { 1024, 768 }, { 1280, 720 }, { 1280, 1024 }, { 1600, 900 }, { 1920, 1080 } };

static int modes(bdrv_display_t* d, bdrv_mode_t* out, int max) {
    (void)d;
    int n = 0;
    for (unsigned i = 0; i < sizeof(MODES) / sizeof(MODES[0]) && n < max; i++) out[n++] = MODES[i];
    return n;
}

static int set_mode(bdrv_display_t* d, int w, int h, uint64_t* fb, int* pitch) {
    (void)d;
    wr(ENABLE, 0);
    wr(XRES, (uint16_t)w);
    wr(YRES, (uint16_t)h);
    wr(BPP, 32);
    wr(VWIDTH, (uint16_t)w);
    wr(VHEIGHT, (uint16_t)h);
    wr(XOFF, 0);
    wr(YOFF, 0);
    wr(ENABLE, 0x01 | 0x40);                 /* enabled, linear framebuffer */
    if (rd(XRES) != w || rd(YRES) != h) return -1;
    *fb = g_lfb;
    *pitch = w * 4;
    return 0;
}

static int probe(const bdrv_pci_t* dev) {
    int io = 0;
    g_lfb = bdrv->pci_bar(dev, 0, &io);
    uint16_t id = rd(ID);
    if (!g_lfb || io || id < 0xB0C0 || id > 0xB0C5) return -1;
    bdrv->log("Bochs VBE DISPI 0x%x, framebuffer at 0x%llx", id, (unsigned long long)g_lfb);
    g_disp.name = "Bochs display (Driver Kit example)";
    g_disp.vram = 16u << 20;
    g_disp.modes = modes;
    g_disp.set_mode = set_mode;
    return bdrv->register_display(&g_disp, dev);
}

static const bdrv_pci_id_t IDS[] = { { 0x1234, 0x1111, 0xFF, 0xFF } };
static const bdrv_pci_driver_t DRV = { "bochsfb", "Bochs / QEMU standard VGA display", IDS, 1, probe };

int banana_driver_main(const banana_driver_api_t* api) {
    return api->register_pci_driver(&DRV) > 0 ? 0 : 1;
}
