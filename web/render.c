#include "render.h"
#include "font8x8.h"

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

static void image(target_t* T, const dl_item_t* it, int x, int y) {
    const img_data_t* im = it->img;
    if (!im || !im->px || it->w <= 0 || it->h <= 0) return;
    int x0 = x < T->cx0 ? T->cx0 : x, y0 = y < T->cy0 ? T->cy0 : y;
    int x1 = x + it->w > T->cx1 ? T->cx1 : x + it->w, y1 = y + it->h > T->cy1 ? T->cy1 : y + it->h;
    for (int yy = y0; yy < y1; yy++) {
        int sy = (yy - y) * im->h / it->h;
        const uint32_t* src = im->px + sy * im->w;
        uint32_t* p = T->buf + yy * T->stride;
        for (int xx = x0; xx < x1; xx++) p[xx] = src[(xx - x) * im->w / it->w];
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
        switch (it->kind) {
        case DL_RECT:
        case DL_CARET:
            fill(&T, x, y, it->w, it->h, it->color);
            break;
        case DL_IMG:
            image(&T, it, x, y);
            break;
        case DL_TEXT: {
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
    }
}
