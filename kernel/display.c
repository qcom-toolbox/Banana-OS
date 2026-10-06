/* Screen resolution: the Bochs / QEMU "VBE DISPI" display interface (QEMU's
 * standard VGA, Bochs, VirtualBox), programmed through two I/O ports. On
 * other hardware the mode GRUB set at boot stays. */
#include "display.h"
#include "fb.h"
#include "io.h"
#include "gui.h"
#include "terminal.h"

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
#define DISPI_ENABLED     0x01
#define DISPI_GETCAPS     0x02
#define DISPI_LFB         0x40
#define DISPI_NOCLEAR     0x80

static const display_mode_t MODES[] = {
    { 800, 600 }, { 1024, 768 }, { 1280, 720 }, { 1280, 800 }, { 1280, 1024 },
    { 1366, 768 }, { 1440, 900 }, { 1600, 900 }, { 1680, 1050 }, { 1920, 1080 },
};
#define NMODES ((int)(sizeof(MODES) / sizeof(MODES[0])))

static void wr(uint16_t idx, uint16_t v) { outw(DISPI_INDEX, idx); outw(DISPI_DATA, v); }
static uint16_t rd(uint16_t idx) { outw(DISPI_INDEX, idx); return inw(DISPI_DATA); }

static int g_checked, g_bga;
static uint16_t g_max_w, g_max_h;

int display_can_change(void) {
    if (!g_checked) {
        g_checked = 1;
        uint16_t id = rd(DISPI_ID);
        g_bga = id >= 0xB0C0 && id <= 0xB0C5 && fb_available() && fb_info()->bpp == 32;
        if (g_bga) {
            uint16_t en = rd(DISPI_ENABLE);
            wr(DISPI_ENABLE, en | DISPI_GETCAPS);    /* the largest mode, then back */
            g_max_w = rd(DISPI_XRES);
            g_max_h = rd(DISPI_YRES);
            wr(DISPI_ENABLE, en);
            if (g_max_w < 800 || g_max_h < 600) { g_max_w = 1920; g_max_h = 1200; }
        }
    }
    return g_bga;
}

int display_modes(display_mode_t* out, int max) {
    int n = 0;
    if (!display_can_change()) {
        const fb_info_t* fi = fb_info();
        if (fi && max > 0) { out[0].w = (int)fi->width; out[0].h = (int)fi->height; n = 1; }
        return n;
    }
    for (int i = 0; i < NMODES && n < max; i++)
        if (MODES[i].w <= g_max_w && MODES[i].h <= g_max_h && MODES[i].w <= 2560 && MODES[i].h <= 1600) out[n++] = MODES[i];
    return n;
}

int display_set_mode(int w, int h) {
    if (!display_can_change() || w < 640 || h < 480 || w > g_max_w || h > g_max_h) return -1;
    const fb_info_t* fi = fb_info();
    if ((int)fi->width == w && (int)fi->height == h) return 0;
    wr(DISPI_ENABLE, 0);
    wr(DISPI_XRES, (uint16_t)w);
    wr(DISPI_YRES, (uint16_t)h);
    wr(DISPI_BPP, 32);
    wr(DISPI_VWIDTH, (uint16_t)w);
    wr(DISPI_VHEIGHT, (uint16_t)h);
    wr(DISPI_XOFF, 0);
    wr(DISPI_YOFF, 0);
    wr(DISPI_ENABLE, DISPI_ENABLED | DISPI_LFB);
    if (rd(DISPI_XRES) != w || rd(DISPI_YRES) != h) return -1;
    fb_reconfigure((uint32_t)w, (uint32_t)h, (uint32_t)w * 4);   /* (same linear framebuffer) */
    terminal_screen_changed();
    gui_screen_changed();
    return 0;
}
