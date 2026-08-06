#include "fb.h"

/* Multiboot2 info parsing (only what we need) */
typedef struct {
    uint32_t total_size;
    uint32_t reserved;
} __attribute__((packed)) mb2_info_t;

typedef struct {
    uint32_t type;
    uint32_t size;
} __attribute__((packed)) mb2_tag_t;

/* Tag type 8: framebuffer */
typedef struct {
    uint32_t type;   /* 8 */
    uint32_t size;
    uint64_t addr;
    uint32_t pitch;
    uint32_t width;
    uint32_t height;
    uint8_t  bpp;
    uint8_t  fb_type;
    uint16_t reserved;
    /* followed by color info; ignored */
} __attribute__((packed)) mb2_tag_fb_t;

static fb_info_t g_fb;
static int g_fb_ok = 0;
static uint32_t* g_bb = NULL;
static uint32_t g_bb_w = 0, g_bb_h = 0;

static uint32_t align_up(uint32_t v, uint32_t a) {
    return (v + (a - 1u)) & ~(a - 1u);
}

int fb_init_multiboot2(uint32_t mb2_info_addr) {
    g_fb_ok = 0;
    g_fb.width = g_fb.height = g_fb.pitch = 0;
    g_fb.bpp = 0;
    g_fb.type = 0;
    g_fb.addr = 0;

    if (!mb2_info_addr) return 0;

    mb2_info_t* info = (mb2_info_t*)(uintptr_t)mb2_info_addr;
    uint32_t total = info->total_size;
    uint32_t off = 8;

    while (off + 8 <= total) {
        mb2_tag_t* tag = (mb2_tag_t*)(uintptr_t)(mb2_info_addr + off);
        if (tag->type == 0) break;

        if (tag->type == 8 && tag->size >= sizeof(mb2_tag_fb_t)) {
            mb2_tag_fb_t* fb = (mb2_tag_fb_t*)tag;
            g_fb.addr = (uintptr_t)fb->addr; /* identity-mapped assumption */
            g_fb.pitch = fb->pitch;
            g_fb.width = fb->width;
            g_fb.height = fb->height;
            g_fb.bpp = fb->bpp;
            g_fb.type = fb->fb_type;

            if (g_fb.addr && g_fb.width && g_fb.height && (g_fb.bpp == 32) && (g_fb.type == 1)) {
                g_fb_ok = 1;
                return 1;
            }
        }

        /* Every valid tag is at least 8 bytes (its own type+size header).
         * Guard against a malformed/zero-size tag stalling the scan forever. */
        uint32_t adv = align_up(tag->size, 8);
        if (adv < 8) adv = 8;
        off += adv;
    }

    return 0;
}

int fb_available(void) {
    return g_fb_ok;
}

const fb_info_t* fb_info(void) {
    return &g_fb;
}

void fb_putpixel_direct(int x, int y, uint32_t rgb) {
    if (!g_fb_ok) return;
    if ((uint32_t)x >= g_fb.width || (uint32_t)y >= g_fb.height) return;

    uintptr_t row = g_fb.addr + (uintptr_t)((uint32_t)y * g_fb.pitch);
    uint32_t* p = (uint32_t*)(row + (uintptr_t)((uint32_t)x * 4u));
    *p = rgb; /* XRGB8888 */
}

void fb_putpixel(int x, int y, uint32_t rgb) {
    if (!g_fb_ok) return;
    if ((uint32_t)x >= g_fb.width || (uint32_t)y >= g_fb.height) return;

    if (g_bb) {
        if ((uint32_t)x < g_bb_w && (uint32_t)y < g_bb_h) {
            g_bb[(uint32_t)y * g_bb_w + (uint32_t)x] = rgb;
        }
        return;
    }

    fb_putpixel_direct(x, y, rgb);
}

void fb_fill_rect(int x, int y, int w, int h, uint32_t rgb) {
    if (!g_fb_ok) return;
    if (w <= 0 || h <= 0) return;

    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + w;
    int y1 = y + h;
    if (x1 > (int)g_fb.width) x1 = (int)g_fb.width;
    if (y1 > (int)g_fb.height) y1 = (int)g_fb.height;

    if (g_bb) {
        for (int yy = y0; yy < y1; yy++) {
            if ((uint32_t)yy >= g_bb_h) break;
            uint32_t* p = &g_bb[(uint32_t)yy * g_bb_w];
            for (int xx = x0; xx < x1; xx++) {
                if ((uint32_t)xx >= g_bb_w) break;
                p[xx] = rgb;
            }
        }
        return;
    }

    for (int yy = y0; yy < y1; yy++) {
        uintptr_t row = g_fb.addr + (uintptr_t)((uint32_t)yy * g_fb.pitch);
        uint32_t* p = (uint32_t*)row;
        for (int xx = x0; xx < x1; xx++) {
            p[xx] = rgb;
        }
    }
}

void fb_set_backbuffer(uint32_t* buf, uint32_t buf_width, uint32_t buf_height) {
    g_bb = buf;
    g_bb_w = buf_width;
    g_bb_h = buf_height;
}

void fb_clear_backbuffer(void) {
    g_bb = NULL;
    g_bb_w = g_bb_h = 0;
}

void fb_present(void) {
    if (!g_fb_ok || !g_bb) return;

    uint32_t w = g_bb_w;
    uint32_t h = g_bb_h;
    if (w > g_fb.width) w = g_fb.width;
    if (h > g_fb.height) h = g_fb.height;

    for (uint32_t y = 0; y < h; y++) {
        uintptr_t row = g_fb.addr + (uintptr_t)(y * g_fb.pitch);
        uint32_t* dst = (uint32_t*)row;
        uint32_t* src = &g_bb[y * g_bb_w];
        for (uint32_t x = 0; x < w; x++) {
            dst[x] = src[x];
        }
    }
}

