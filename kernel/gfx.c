#include "font.h"
#include "kstring.h"
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
    if (uc >= 128 && uc < 0xA0) uc = '?';   /* a cell holds ASCII or Latin-1 */

    /* Blank cells (most of any terminal) are a plain fill. */
    if (uc == ' ' || uc == 0 || uc == 0xA0) { fb_fill_rect(x, y, 8, 8, bg); return; }
    const uint8_t* glyph = uc >= 0xA0 ? font8x8_latin1[uc - 0xA0] : font8x8_basic[uc];

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
    if (uc >= 128 && uc < 0xA0) uc = '?';
    const uint8_t* glyph = uc >= 0xA0 ? font8x8_latin1[uc - 0xA0] : font8x8_basic[uc];

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

/* ── smooth UI text: DejaVu Sans Mono, one glyph per 8-pixel cell (so every
 * layout made for the 8x8 font still fits), UTF-8 aware ── */
static int g_smooth = 1;

void gfx_set_smooth_text(int on) { g_smooth = on; }
int  gfx_smooth_text(void) { return g_smooth && font_available(FONT_MONO); }

/* cell: 8 * scale; the glyphs are a little taller than the cell, centered on it */
static void smooth_text(int x, int y, int scale, const char* s, uint32_t fg, uint32_t bg) {
    int stride, tw, th;
    uint32_t* t = fb_target(&stride, &tw, &th);
    if (!t) return;
    int cell = 8 * scale, px = 13 * scale, base = y + 8 * scale;
    const char* e = s + strlen(s);
    int cx = x;
    while (s < e) {
        const char* start = s;
        const char* line_end = s;
        while (line_end < e && *line_end != '\n') line_end++;
        int n = 0;
        for (const char* q = start; q < line_end; ) { utf8_next(&q, line_end); n++; }
        fb_fill_rect(cx, y, n * cell, cell, bg);
        while (s < line_end) {
            const char* g = s;
            uint32_t cp = utf8_next(&s, line_end);
            if (cp != ' ') {
                int adv = font_text_width(FONT_MONO, px, g, (uint32_t)(s - g));
                font_draw(t, stride, 0, 0, tw, th, FONT_MONO, px, cx + (cell - adv) / 2, base, g, (uint32_t)(s - g), fg, 0);
            }
            cx += cell;
        }
        if (s < e && *s == '\n') { s++; cx = x; y += cell; base += cell; }
    }
}

void gfx_draw_text(int x, int y, const char* s, uint32_t fg, uint32_t bg) {
    if (!g_ok || !s) return;
    if (gfx_smooth_text()) { smooth_text(x, y, 1, s, fg, bg); return; }
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
    if (gfx_smooth_text()) { smooth_text(x, y, scale, s, fg, bg); return; }
    int cx = x;
    int step = 8 * scale;
    for (int i = 0; s[i]; i++) {
        if (s[i] == '\n') { cx = x; y += step; continue; }
        gfx_draw_char_scaled(cx, y, scale, s[i], fg, bg);
        cx += step;
    }
}


void gfx_draw_grip(int right, int bottom) {
    /* three diagonal rows of dots, like most desktops */
    for (int row = 0; row < 3; row++)
        for (int k = 0; k <= row; k++)
            gfx_fill_rect(right - 5 - (row - k) * 4, bottom - 5 - k * 4, 2, 2, 0x008A96AAu);
}

/* ── desktop icon labels: no background - the text straight on the
 * wallpaper with a dark shadow, so it reads on light and dark pictures
 * alike - centred under the icon, wrapped onto up to two lines ── */

/* one line, centred on cx, drawn transparently (fg) */
static void label_line(int cx, int y, const char* s, uint32_t n, uint32_t fg, int px) {
    int stride, tw, th;
    uint32_t* t = fb_target(&stride, &tw, &th);
    if (!t || !n) return;
    if (font_available(FONT_SANS)) {
        int asc, desc, lh;
        font_metrics(FONT_SANS, px, &asc, &desc, &lh);
        int w = font_text_width(FONT_SANS, px, s, n);
        font_draw(t, stride, 0, 0, tw, th, FONT_SANS, px, cx - w / 2, y + asc, s, n, fg, 0);
        return;
    }
    /* no TrueType font: the 8x8 one, only its set pixels */
    int x = cx - (int)n * 4;
    for (uint32_t i = 0; i < n; i++, x += 8) {
        uint8_t uc = (uint8_t)s[i];
        if (uc >= 128) uc = '?';
        const uint8_t* g = font8x8_basic[uc];
        for (int gy = 0; gy < 8; gy++)
            for (int gx = 0; gx < 8; gx++)
                if (g[gy] & (1u << gx)) fb_fill_rect(x + gx, y + gy, 1, 1, fg);
    }
}

static int label_w(const char* s, uint32_t n, int px) {
    return font_available(FONT_SANS) ? font_text_width(FONT_SANS, px, s, n) : (int)n * 8;
}

int gfx_draw_label(int cx, int y, int max_w, const char* s, uint32_t fg, uint32_t highlight) {
    if (!g_ok || !s) return 0;
    const int px = 12;
    int lh = 14;
    if (font_available(FONT_SANS)) { int a, d; font_metrics(FONT_SANS, px, &a, &d, &lh); }
    /* the lines: as many words as fit, at most two (the second cut short with "..") */
    uint32_t len = (uint32_t)strlen(s), start[2] = { 0, 0 }, cnt[2] = { 0, 0 };
    int lines = 0;
    uint32_t p = 0;
    while (p < len && lines < 2) {
        while (p < len && s[p] == ' ') p++;
        uint32_t best = 0, q = p;
        while (q <= len) {
            if (q == len || s[q] == ' ') {
                if (label_w(s + p, q - p, px) <= max_w || !best) best = q - p;
                else break;
                if (q == len) break;
            }
            q++;
        }
        if (!best) break;
        start[lines] = p;
        cnt[lines] = best;
        lines++;
        p += best;
    }
    char tail[64];
    const char* l2 = s + start[1];
    if (lines == 2 && start[1] + cnt[1] < len) {          /* more than fits: ".." */
        uint32_t n = cnt[1] < sizeof(tail) - 3 ? cnt[1] : sizeof(tail) - 3;
        while (n && label_w(s + start[1], n, px) + label_w("..", 2, px) > max_w) n--;
        memcpy(tail, s + start[1], n);
        tail[n] = '.'; tail[n + 1] = '.'; tail[n + 2] = 0;
        l2 = tail;
        cnt[1] = n + 2;
    }
    for (int i = 0; i < lines; i++) {
        const char* ls = i ? l2 : s + start[0];
        int w = label_w(ls, cnt[i], px), ly = y + i * lh;
        if (highlight) gfx_fill_rect(cx - w / 2 - 2, ly - 1, w + 4, lh + 1, highlight);
        else label_line(cx + 1, ly + 1, ls, cnt[i], 0x00000000u, px);   /* the shadow */
        label_line(cx, ly, ls, cnt[i], fg, px);
    }
    return lines * lh;
}
