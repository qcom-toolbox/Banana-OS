#include "render.h"
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
    for (int yy = y0; yy < y1; yy++) {
        uint32_t* p = T->buf + yy * T->stride;
        for (int xx = x0; xx < x1; xx++) p[xx] = c;
    }
}

/* how far a row dy into a corner of radius r is pushed in (integer circle) */
static int corner_inset(int r, int dy) {
    int d = r - dy;                     /* distance from the corner's centre row */
    int x = 0;
    while ((x + 1) * (x + 1) + d * d <= r * r) x++;
    return r - x;
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
        if (yy < r) in = corner_inset(r, yy);
        else if (yy >= h - r) in = corner_inset(r, h - 1 - yy);
        if (!ring) { fill(T, x + in, py, w - 2 * in, 1, c); continue; }
        /* the hole of the ring */
        int iy = yy - ring, ih = h - 2 * ring;
        if (iy < 0 || iy >= ih) { fill(T, x + in, py, w - 2 * in, 1, c); continue; }
        int inner = 0;
        if (iy < ri) inner = corner_inset(ri, iy);
        else if (iy >= ih - ri) inner = corner_inset(ri, ih - 1 - iy);
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

void render_page(const layout_t* L, uint32_t* buf, int stride, int buf_w, int buf_h,
                 int vx, int vy, int vw, int vh, int scroll) {
    target_t T;
    T.buf = buf;
    T.stride = stride;
    T.cx0 = vx < 0 ? 0 : vx;
    T.cy0 = vy < 0 ? 0 : vy;
    T.cx1 = vx + vw > buf_w ? buf_w : vx + vw;
    T.cy1 = vy + vh > buf_h ? buf_h : vy + vh;
    fill(&T, vx, vy, vw, vh, L ? L->bg : 0xFFFFFF);
    if (!L) return;
    int top = scroll, bottom = scroll + vh;
    for (uint32_t i = 0; i < L->n; i++) {
        const dl_item_t* it = &L->items[i];
        if (it->y + it->h < top - 2 || it->y > bottom) continue;
        int x = vx + it->x, y = vy + it->y - scroll;
        target_t saved = T;
        if (it->has_clip) {                 /* overflow: hidden */
            int c0 = vx + it->cx0, c1 = vx + it->cx1, d0 = vy + it->cy0 - scroll, d1 = it->cy1 > 0x3FFFF000 ? T.cy1 : vy + it->cy1 - scroll;
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
