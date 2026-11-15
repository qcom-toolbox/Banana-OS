/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "render.h"
#include "round_rect.h"
#include "gradient.h"
#include "font8x8.h"
#include "font.h"

typedef struct {
    uint32_t* buf;
    int stride;
    int cx0, cy0, cx1, cy1;      /* clip rectangle */
} target_t;

static void fill(target_t* T, int x, int y, int w, int h, uint32_t c) {
    int x0 = x < T->cx0 ? T->cx0 : x, y0 = y < T->cy0 ? T->cy0 : y;
    int x1 = x + w > T->cx1 ? T->cx1 : x + w, y1 = y + h > T->cy1 ? T->cy1 : y + h;
    if (c >> 24) {                       /* see-through (css_color_blend): over what is there */
        uint32_t a = 255 - (c >> 24), na = 255 - a;
        uint32_t sr = ((c >> 16) & 255) * a, sg = ((c >> 8) & 255) * a, sb = (c & 255) * a;
        for (int yy = y0; yy < y1; yy++) {
            uint32_t* p = T->buf + yy * T->stride;
            for (int xx = x0; xx < x1; xx++) {
                uint32_t d = p[xx];
                p[xx] = (sr + ((d >> 16) & 255) * na) / 255 << 16 | (sg + ((d >> 8) & 255) * na) / 255 << 8 |
                        (sb + (d & 255) * na) / 255;
            }
        }
        return;
    }
    for (int yy = y0; yy < y1; yy++) {
        uint32_t* p = T->buf + yy * T->stride;
        for (int xx = x0; xx < x1; xx++) p[xx] = c;
    }
}

/* a box with rounded corners; ring > 0: only an outline that wide */
static void round_rect(target_t* T, int x, int y, int w, int h, int r, int ring, uint32_t c) {
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    if (r < 0) r = 0;
    int ri = r - ring;
    if (ri < 0) ri = 0;
    for (int yy = 0; yy < h; yy++) {
        int py = y + yy;
        if (py < T->cy0 || py >= T->cy1) continue;
        int in = 0;
        if (yy < r) in = web_corner_inset(r, yy);
        else if (yy >= h - r) in = web_corner_inset(r, h - 1 - yy);
        if (!ring) { fill(T, x + in, py, w - 2 * in, 1, c); continue; }
        /* the hole of the ring */
        int iy = yy - ring, ih = h - 2 * ring;
        if (iy < 0 || iy >= ih) { fill(T, x + in, py, w - 2 * in, 1, c); continue; }
        int inner = 0;
        if (iy < ri) inner = web_corner_inset(ri, iy);
        else if (iy >= ih - ri) inner = web_corner_inset(ri, ih - 1 - iy);
        int hx0 = x + ring + inner, hx1 = x + w - ring - inner;
        fill(T, x + in, py, hx0 - (x + in), 1, c);
        fill(T, hx1, py, x + w - in - hx1, 1, c);
    }
}

static void glyph(target_t* T, int x, int y, unsigned char ch, int scale, uint32_t c, int bold, int italic) {
    if (ch >= 128) ch = '?';
    if (ch == ' ') return;
    const uint8_t* g = font8x8_basic[ch];
    int size = 8 * scale;
    if (x >= T->cx1 || y >= T->cy1 || x + size + 2 <= T->cx0 || y + size <= T->cy0) return;
    for (int gy = 0; gy < 8; gy++) {
        uint8_t row = g[gy];
        if (!row) continue;
        int slant = italic ? (7 - gy) / 3 : 0;          /* lean the top rows right */
        for (int gx = 0; gx < 8; gx++) {
            if (!(row & (1u << gx))) continue;
            int px = x + gx * scale + slant * scale, py = y + gy * scale;
            fill(T, px, py, scale + (bold ? 1 : 0), scale, c);
        }
    }
}

static void image_at(target_t* T, const img_data_t* im, int x, int y, int w, int h);

/* a CSS background: tw x th tiles from (x + ox, y + oy), inside the item's area */
static void bg_image(target_t* T, const dl_item_t* it, int x, int y) {
    target_t S = *T;                    /* clipped to the area */
    if (x > S.cx0) S.cx0 = x;
    if (y > S.cy0) S.cy0 = y;
    if (x + it->w < S.cx1) S.cx1 = x + it->w;
    if (y + it->h < S.cy1) S.cy1 = y + it->h;
    if (S.cx0 >= S.cx1 || S.cy0 >= S.cy1 || it->tw <= 0 || it->th <= 0) return;
    int x0 = x + it->ox, y0 = y + it->oy;
    if (it->tile & 1) { while (x0 > S.cx0) x0 -= it->tw; while (x0 + it->tw <= S.cx0) x0 += it->tw; }
    if (it->tile & 2) { while (y0 > S.cy0) y0 -= it->th; while (y0 + it->th <= S.cy0) y0 += it->th; }
    int n = 0;
    for (int ty = y0; ty < S.cy1 && n < 4000; ty += it->th) {
        for (int tx = x0; tx < S.cx1 && n < 4000; tx += it->tw) {
            image_at(&S, it->img, tx, ty, it->tw, it->th);
            n++;
            if (!(it->tile & 1)) break;
        }
        if (!(it->tile & 2)) break;
    }
}

static void image(target_t* T, const dl_item_t* it, int x, int y) {
    if (it->tile & 4) { bg_image(T, it, x, y); return; }
    image_at(T, it->img, x, y, it->w, it->h);
}

static void image_at(target_t* T, const img_data_t* im, int x, int y, int w, int h) {
    if (!im || !im->px || w <= 0 || h <= 0) return;
    struct { int w, h; } box = { w, h };
    const struct { int w, h; }* it = (const void*)&box;
    int x0 = x < T->cx0 ? T->cx0 : x, y0 = y < T->cy0 ? T->cy0 : y;
    int x1 = x + it->w > T->cx1 ? T->cx1 : x + it->w, y1 = y + it->h > T->cy1 ? T->cy1 : y + it->h;
    for (int yy = y0; yy < y1; yy++) {
        int sy = (yy - y) * im->h / it->h;
        const uint32_t* src = im->px + sy * im->w;
        uint32_t* p = T->buf + yy * T->stride;
        if (!im->alpha) {
            for (int xx = x0; xx < x1; xx++) p[xx] = src[(xx - x) * im->w / it->w];
            continue;
        }
        const uint8_t* as = im->alpha + sy * im->w;
        for (int xx = x0; xx < x1; xx++) {
            int sx = (xx - x) * im->w / it->w;
            uint32_t a = as[sx];
            if (!a) continue;
            if (a == 255) { p[xx] = src[sx]; continue; }
            uint32_t s = src[sx], d = p[xx];
            uint32_t r = (((s >> 16) & 255) * a + ((d >> 16) & 255) * (255 - a)) / 255;
            uint32_t g = (((s >> 8) & 255) * a + ((d >> 8) & 255) * (255 - a)) / 255;
            uint32_t b = ((s & 255) * a + (d & 255) * (255 - a)) / 255;
            p[xx] = r << 16 | g << 8 | b;
        }
    }
}


/* a CSS linear gradient over the item's box, rounded by radius, tiled when
 * background-size made it smaller than the box */
static void gradient(target_t* T, const dl_item_t* it, int x, int y) {
    const web_grad_t* g = it->grad;
    if (!g || it->w <= 0 || it->h <= 0) return;
    int r = it->radius;
    if (r * 2 > it->w) r = it->w / 2;
    if (r * 2 > it->h) r = it->h / 2;
    if (r < 0) r = 0;
    int y0 = y < T->cy0 ? T->cy0 : y, y1 = y + it->h > T->cy1 ? T->cy1 : y + it->h;
    if (y0 >= y1) return;
    /* the colour along the line, 512 steps (repeating: over one period) */
    uint32_t lut[512];
    int a = 0, b = 65536;
    if (g->repeating && g->n > 1 && g->pos[g->n - 1] > g->pos[0]) { a = g->pos[0]; b = g->pos[g->n - 1]; }
    for (int i = 0; i < 512; i++) lut[i] = web_grad_at(g, a + (int)((int64_t)(b - a) * i / 511));
    int s = web_sin1024(g->deg), c = web_cos1024(g->deg);
    int64_t len = (int64_t)web_grad_len(g->deg, g->gw, g->gh) * 1024;
    int gx = x + g->ox, gy = y + g->oy;
    for (int py = y0; py < y1; py++) {
        int yy = py - y, in = 0;
        if (yy < r) in = web_corner_inset(r, yy);
        else if (yy >= it->h - r) in = web_corner_inset(r, it->h - 1 - yy);
        int x0 = x + in, x1 = x + it->w - in;
        if (x0 < T->cx0) x0 = T->cx0;
        if (x1 > T->cx1) x1 = T->cx1;
        if (x0 >= x1) continue;
        int ly = py - gy;
        if (g->tile & 2) { ly %= g->gh; if (ly < 0) ly += g->gh; }
        else if (ly < 0 || ly >= g->gh) continue;
        int64_t row = -(int64_t)(ly - g->gh / 2) * c;         /* y down, the angle's 0 is up */
        uint32_t* p = T->buf + py * T->stride;
        for (int px = x0; px < x1; px++) {
            int lx = px - gx;
            if (g->tile & 1) { if (lx < 0 || lx >= g->gw) { lx %= g->gw; if (lx < 0) lx += g->gw; } }
            else if (lx < 0 || lx >= g->gw) continue;
            int64_t proj = row + (int64_t)(lx - g->gw / 2) * s;
            int t = 32768 + (int)(proj * 65536 / len);           /* 0..65536 along the line */
            int k;
            if (b > a && (g->repeating)) { int q = (t - a) % (b - a); if (q < 0) q += b - a; k = (int)((int64_t)q * 511 / (b - a)); }
            else { k = t <= 0 ? 0 : t >= 65536 ? 511 : t * 511 / 65536; }
            uint32_t col = lut[k];
            if (!(col >> 24)) { p[px] = col; continue; }
            uint32_t al = 255 - (col >> 24), na = 255 - al, d = p[px];
            p[px] = ((((col >> 16) & 255) * al + ((d >> 16) & 255) * na) / 255) << 16 |
                    ((((col >> 8) & 255) * al + ((d >> 8) & 255) * na) / 255) << 8 |
                    (((col & 255) * al + (d & 255) * na) / 255);
        }
    }
}

/* draws L's display list into the clip already set in T; the document's
 * (0,0) maps to (ox, oy), scrolled down by `scroll`. Recurses for iframes. */
static void render_into(const layout_t* L, target_t T, int ox, int oy, int scroll) {
    fill(&T, T.cx0, T.cy0, T.cx1 - T.cx0, T.cy1 - T.cy0, L ? L->bg : 0xFFFFFF);
    if (!L) return;
    for (uint32_t i = 0; i < L->n; i++) {
        const dl_item_t* it = &L->items[i];
        int x = ox + it->x, y = oy + it->y - scroll;
        if (y + it->h < T.cy0 - 2 || y > T.cy1) continue;
        target_t saved = T;
        if (it->has_clip) {                 /* overflow: hidden */
            int c0 = ox + it->cx0, c1 = ox + it->cx1, d0 = oy + it->cy0 - scroll, d1 = it->cy1 > 0x3FFFF000 ? T.cy1 : oy + it->cy1 - scroll;
            if (c0 > T.cx0) T.cx0 = c0;
            if (c1 < T.cx1) T.cx1 = c1;
            if (d0 > T.cy0) T.cy0 = d0;
            if (d1 < T.cy1) T.cy1 = d1;
            if (T.cx0 >= T.cx1 || T.cy0 >= T.cy1) { T = saved; continue; }
        }
        switch (it->kind) {
        case DL_RECT:
        case DL_CARET:
            if (it->radius > 0 || it->ring) round_rect(&T, x, y, it->w, it->h, it->radius, it->ring, it->color);
            else fill(&T, x, y, it->w, it->h, it->color);
            break;
        case DL_IMG:
            image(&T, it, x, y);
            break;
        case DL_GRAD:
            gradient(&T, it, x, y);
            break;
        case DL_IFRAME: {
            int bw = 2;                              /* a thin border, then the frame's page inside */
            fill(&T, x, y, it->w, it->h, 0xB0B0B0);
            int cx = x + bw, cy = y + bw, cw = it->w - 2 * bw, ch = it->h - 2 * bw;
            if (cw > 0 && ch > 0) {
                target_t C = T;
                if (cx > C.cx0) C.cx0 = cx;
                if (cy > C.cy0) C.cy0 = cy;
                if (cx + cw < C.cx1) C.cx1 = cx + cw;
                if (cy + ch < C.cy1) C.cy1 = cy + ch;
                /* the child's layout is read live from the node: the frame
                 * re-lays-out on its own (timers, typing) between our paints */
                const layout_t* child = it->node && it->node->frame_layout
                                        ? (const layout_t*)it->node->frame_layout : (const layout_t*)it->frame_l;
                int fs = it->node ? it->node->frame_scroll : it->fscroll;
                if (C.cx0 < C.cx1 && C.cy0 < C.cy1)
                    render_into(child, C, cx, cy, fs);
            }
            break;
        }
        case DL_TEXT: {
            if (it->px) {                        /* the system fonts */
                int px = it->px, base = y + it->asc;
                if (it->has_bg) fill(&T, x, y, it->w, it->h, it->bg);
                if (L->sel_on) {
                    int i0 = L->sel_i0, o0 = L->sel_o0, i1 = L->sel_i1, o1 = L->sel_o1;
                    if (i1 < i0 || (i1 == i0 && o1 < o0)) { int t = i0; i0 = i1; i1 = t; t = o0; o0 = o1; o1 = t; }
                    int me = (int)i;
                    if (me >= i0 && me <= i1) {
                        int a = me == i0 ? o0 : 0, b = me == i1 ? o1 : (int)it->len;
                        if (a > (int)it->len) a = (int)it->len;
                        if (b > (int)it->len) b = (int)it->len;
                        if (b > a) {
                            int xa = font_text_width(it->face, px, it->text, (uint32_t)a);
                            int xb = font_text_width(it->face, px, it->text, (uint32_t)b);
                            fill(&T, x + xa, y, xb - xa, it->h, 0xB4D5FE);
                        }
                    }
                }
                font_draw(T.buf, T.stride, T.cx0, T.cy0, T.cx1, T.cy1, it->face, px, x, base,
                          it->text, it->len, it->color, it->italic);
                int th = px >= 20 ? px / 14 : 1;
                if (it->underline) {
                    int sx = x, sw = it->w;
                    if (it->len && it->text[0] == ' ' && !it->ulspace) {   /* not under a space before the element */
                        int spw = font_text_width(it->face, px, " ", 1);
                        sx += spw; sw -= spw;
                    }
                    fill(&T, sx, base + 1 + px / 10, sw, th, it->color);
                }
                if (it->strike) fill(&T, x, base - px * 3 / 10, it->w, th, it->color);
                break;
            }
            int s = it->scale ? it->scale : 1;
            int cw = 8 * s;
            if (it->has_bg) fill(&T, x, y - s, it->w, cw + 2 * s, it->bg);
            if (L->sel_on) {
                /* the selected part of this run */
                int i0 = L->sel_i0, o0 = L->sel_o0, i1 = L->sel_i1, o1 = L->sel_o1;
                if (i1 < i0 || (i1 == i0 && o1 < o0)) { int t = i0; i0 = i1; i1 = t; t = o0; o0 = o1; o1 = t; }
                int me = (int)i;
                if (me >= i0 && me <= i1) {
                    int a = me == i0 ? o0 : 0, b = me == i1 ? o1 : (int)it->len;
                    if (b > a) fill(&T, x + a * cw, y - s, (b - a) * cw, cw + 2 * s, 0xB4D5FE);
                }
            }
            for (uint32_t k = 0; k < it->len; k++)
                glyph(&T, x + (int)k * cw, y, (unsigned char)it->text[k], s, it->color, it->bold, it->italic);
            if (it->underline) {
                int sx = x, sw = it->w;
                if (it->len && it->text[0] == ' ' && !it->ulspace) { sx += cw; sw -= cw; }   /* not under a space before the element */
                fill(&T, sx, y + cw, sw, s, it->color);
            }
            if (it->strike) fill(&T, x, y + cw / 2, it->w, s, it->color);
            break;
        }
        }
        T = saved;
    }
}

void render_page(const layout_t* L, uint32_t* buf, int stride, int buf_w, int buf_h,
                 int vx, int vy, int vw, int vh, int scroll) {
    target_t T;
    T.buf = buf;
    T.stride = stride;
    T.cx0 = vx < 0 ? 0 : vx;
    T.cy0 = vy < 0 ? 0 : vy;
    T.cx1 = vx + vw > buf_w ? buf_w : vx + vw;
    T.cy1 = vy + vh > buf_h ? buf_h : vy + vh;
    render_into(L, T, vx, vy, scroll);
}
