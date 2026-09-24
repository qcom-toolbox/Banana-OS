#include "gfx.h"
#include "fb.h"
#include "font8x8.h"

static int g_ok = 0;

void gfx_init(void) {
    g_ok = fb_available();
}

int gfx_available(void) {
    return g_ok;
}

void gfx_clear(uint32_t rgb) {
    if (!g_ok) return;
    const fb_info_t* fi = fb_info();
    fb_fill_rect(0, 0, (int)fi->width, (int)fi->height, rgb);
}

void gfx_fill_rect(int x, int y, int w, int h, uint32_t rgb) {
    if (!g_ok) return;
    fb_fill_rect(x, y, w, h, rgb);
}

void gfx_draw_char(int x, int y, char c, uint32_t fg, uint32_t bg) {
    if (!g_ok) return;
    int stride, tw, th;
    uint32_t* t = fb_target(&stride, &tw, &th);
    if (!t) return;
    uint8_t uc = (uint8_t)c;
    if (uc >= 128) uc = '?'; /* font8x8_basic only covers 0-127 */

    /* Blank cells (most of any terminal) are a plain fill. */
    if (uc == ' ' || uc == 0) { fb_fill_rect(x, y, 8, 8, bg); return; }
    const uint8_t* glyph = font8x8_basic[uc];

    /* Clip once per glyph, then write rows straight into the target
     * instead of 64 bounds-checked fb_putpixel() calls. */
    int gx0 = x < 0 ? -x : 0, gx1 = (x + 8 > tw) ? tw - x : 8;
    int gy0 = y < 0 ? -y : 0, gy1 = (y + 8 > th) ? th - y : 8;
    if (gx0 >= gx1 || gy0 >= gy1) return;
    for (int gy = gy0; gy < gy1; gy++) {
        uint8_t row = glyph[gy];
        uint32_t* p = t + (uint32_t)(y + gy) * (uint32_t)stride + (uint32_t)x;
        for (int gx = gx0; gx < gx1; gx++) p[gx] = (row & (1u << gx)) ? fg : bg;
    }
}

void gfx_draw_char_scaled(int x, int y, int scale, char c, uint32_t fg, uint32_t bg) {
    if (!g_ok) return;
    if (scale <= 1) { gfx_draw_char(x, y, c, fg, bg); return; }

    uint8_t uc = (uint8_t)c;
    if (uc >= 128) uc = '?'; /* font8x8_basic only covers 0-127 */
    const uint8_t* glyph = font8x8_basic[uc];

    for (int gy = 0; gy < 8; gy++) {
        uint8_t row = glyph[gy];
        for (int gx = 0; gx < 8; gx++) {
            uint32_t col = (row & (1u << gx)) ? fg : bg;
            int px = x + gx * scale;
            int py = y + gy * scale;
            fb_fill_rect(px, py, scale, scale, col);
        }
    }
}

void gfx_draw_text(int x, int y, const char* s, uint32_t fg, uint32_t bg) {
    if (!g_ok || !s) return;
    int cx = x;
    for (int i = 0; s[i]; i++) {
        if (s[i] == '\n') { cx = x; y += 8; continue; }
        gfx_draw_char(cx, y, s[i], fg, bg);
        cx += 8;
    }
}

void gfx_draw_text_scaled(int x, int y, int scale, const char* s, uint32_t fg, uint32_t bg) {
    if (!g_ok || !s) return;
    if (scale <= 1) { gfx_draw_text(x, y, s, fg, bg); return; }
    int cx = x;
    int step = 8 * scale;
    for (int i = 0; s[i]; i++) {
        if (s[i] == '\n') { cx = x; y += step; continue; }
        gfx_draw_char_scaled(cx, y, scale, s[i], fg, bg);
        cx += step;
    }
}

