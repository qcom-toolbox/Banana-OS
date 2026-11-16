/* SPDX-License-Identifier: GPL-3.0-or-later */
/* <canvas>: the 2D context (CanvasRenderingContext2D), drawn on the
 * processor in the isolated web worker. web/prelude.js builds the
 * context object on __cv(element, op, ...); this file keeps each canvas's
 * pixels (straight alpha: XRGB + a coverage plane, as layout draws images),
 * its drawing state and its path, and rasterizes: paths filled with
 * anti-aliasing (4 sub-scanlines, exact horizontal coverage, nonzero /
 * evenodd), strokes as the union of their segments, joins and caps, text
 * through the system fonts, images scaled and transformed, linear / radial
 * gradients and patterns, clipping masks, getImageData / putImageData and
 * toDataURL (PNG). Workers only: the kernel's build has no floating point. */
#ifdef BANANA_USER_WORKER
#include "canvas.h"
#include "page.h"
#include "css.h"
#include "font.h"
#include "kheap.h"
#include "kstring.h"
#include "script_int.h"

#define CV_MAX_PIXELS (4096u * 4096u)
#define CV_STACK 32
#define CV_GRADS 256
#define SS 4                                   /* sub-scanlines a pixel row */

typedef struct { double a, b, c, d, e, f; } mat_t;   /* x' = a x + c y + e, y' = b x + d y + f */

enum { PAINT_COLOR = 0, PAINT_GRAD, PAINT_PATTERN };
typedef struct { int kind; uint32_t color; int id; } paint_t;   /* color: css_color_blend's (255 - alpha) << 24 */

typedef struct {
    int    used, radial;
    double x0, y0, r0, x1, y1, r1;
    int    n;
    double off[16];
    uint32_t col[16];
    /* patterns */
    img_data_t* img;                           /* (the page's image, not ours) */
    uint32_t* own_px;                          /* a copy of a canvas's pixels */
    uint8_t*  own_al;
    int    pw, ph, rep_x, rep_y;
} grad_t;

typedef struct {
    mat_t  m;
    paint_t fill, stroke;
    double lw, miter, alpha;
    int    cap, join, op;
    int    font_px, bold, italic, mono;
    int    align, baseline;
    double dash[16], dash_off;
    int    ndash;
    uint8_t* clip;                             /* NULL: none; else a w*h mask (owned) */
    int    smooth;
    uint32_t shadow_color;
    double shadow_blur, shadow_x, shadow_y;
} state_t;

typedef struct { double x, y; } pt_t;

struct canvas {
    struct canvas* next;
    dom_node_t* node;
    int      w, h;
    img_data_t img;                            /* what layout draws: px + alpha */
    state_t  st, stack[CV_STACK];
    int      nst;
    /* the path, in device space: points, and where each subpath starts / whether it is closed */
    pt_t*    pts;
    int      npts, cap;
    int*     subs;                             /* index of each subpath's first point */
    uint8_t* closed;
    int      nsubs, subcap;
    double   cx, cy;                           /* the current point (user space) */
    int      have_cur;                         /* 1: there is a current point; 2: just after closePath */
    uint8_t* smask;                            /* a shadow's pass: what is drawn goes here, as coverage */
};

static grad_t g_grads[CV_GRADS];
static int source_of(page_t* p, dom_node_t* n, const uint32_t** px, const uint8_t** al, int* w, int* h);

/* ── helpers ───────────────────────────────────────────────────────── */

static double fabs_(double v) { return v < 0 ? -v : v; }
static double sqrt_(double v) { return v > 0 ? __builtin_sqrt(v) : 0; }
static double floor_(double v) { double i = (double)(long long)v; return i > v ? i - 1 : i; }
static double sin_(double v) { return __builtin_sin(v); }
static double cos_(double v) { return __builtin_cos(v); }
static double atan2_(double y, double x) { return __builtin_atan2(y, x); }
static int iclamp(int v, int a, int b) { return v < a ? a : v > b ? b : v; }

#define PI 3.14159265358979323846

static void pt(const mat_t* m, double x, double y, double* ox, double* oy) {
    *ox = m->a * x + m->c * y + m->e;
    *oy = m->b * x + m->d * y + m->f;
}
static int invert(const mat_t* m, mat_t* r) {
    double det = m->a * m->d - m->b * m->c;
    if (fabs_(det) < 1e-12) return 0;
    r->a = m->d / det; r->b = -m->b / det; r->c = -m->c / det; r->d = m->a / det;
    r->e = -(r->a * m->e + r->c * m->f);
    r->f = -(r->b * m->e + r->d * m->f);
    return 1;
}
static double mat_scale(const mat_t* m) { return sqrt_(fabs_(m->a * m->d - m->b * m->c)); }

static void state_init(state_t* s) {
    memset(s, 0, sizeof(*s));
    s->m.a = s->m.d = 1;
    s->fill.color = 0x000000;
    s->stroke.color = 0x000000;
    s->lw = 1;
    s->miter = 10;
    s->alpha = 1;
    s->font_px = 10;
    s->smooth = 1;
    s->shadow_color = 0xFF000000u;
}

static int attr_int(dom_node_t* n, const char* name, int def) {
    const char* v = dom_attr(n, name);
    if (!v) return def;
    int x = 0, any = 0;
    while (*v == ' ') v++;
    while (*v >= '0' && *v <= '9') { x = x * 10 + (*v - '0'); v++; any = 1; if (x > 100000) break; }
    return any ? x : def;
}

static void free_pixels(canvas_t* c) {
    if (c->img.px) kfree(c->img.px);
    if (c->img.alpha) kfree(c->img.alpha);
    c->img.px = NULL;
    c->img.alpha = NULL;
}

/* (re)size to the element's width / height attributes; cleared, state reset */
static void resize(canvas_t* c, int w, int h) {
    if (w < 0) w = 0;
    if (h < 0) h = 0;
    if ((uint64_t)w * (uint64_t)h > CV_MAX_PIXELS) { w = 0; h = 0; }
    free_pixels(c);
    c->w = w;
    c->h = h;
    if (w && h) {
        c->img.px = kmalloc((size_t)w * (size_t)h * 4);
        c->img.alpha = kmalloc((size_t)w * (size_t)h);
        if (!c->img.px || !c->img.alpha) { free_pixels(c); c->w = c->h = 0; }
        else {
            memset(c->img.px, 0, (size_t)w * (size_t)h * 4);
            memset(c->img.alpha, 0, (size_t)w * (size_t)h);
        }
    }
    c->img.w = c->w;
    c->img.h = c->h;
    c->img.failed = 0;
    if (c->st.clip) kfree(c->st.clip);
    for (int i = 0; i < c->nst; i++) if (c->stack[i].clip) kfree(c->stack[i].clip);
    c->nst = 0;
    state_init(&c->st);
    c->npts = c->nsubs = 0;
    c->have_cur = 0;
}

canvas_t* canvas_of(page_t* p, dom_node_t* n, int create) {
    if (!n || n->type != DOM_ELEM || strcmp(n->tag, "canvas")) return NULL;
    canvas_t* c = (canvas_t*)n->canvas;
    if (!c) {
        if (!create) return NULL;
        c = kzalloc(sizeof(*c));
        if (!c) return NULL;
        c->node = n;
        c->next = p->canvases;
        p->canvases = c;
        n->canvas = c;
        n->img = &c->img;
        resize(c, attr_int(n, "width", 300), attr_int(n, "height", 150));
        p->dirty = 1;
    }
    return c;
}

/* a WebGL frame (RGBA, top row first, premultiplied or not) as the canvas's pixels */
void canvas_put_rgba(page_t* p, dom_node_t* n, int w, int h, const uint8_t* rgba, int premultiplied) {
    canvas_t* c = canvas_of(p, n, 1);
    if (!c || w <= 0 || h <= 0) return;
    if (c->w != w || c->h != h) { resize(c, w, h); p->dirty = 1; }
    if (!c->img.px) return;
    size_t np = (size_t)w * (size_t)h;
    for (size_t i = 0; i < np; i++) {
        const uint8_t* q = rgba + i * 4;
        int a = q[3], r = q[0], g = q[1], b = q[2];
        if (premultiplied && a && a < 255) { r = r * 255 / a; g = g * 255 / a; b = b * 255 / a; if (r > 255) r = 255; if (g > 255) g = 255; if (b > 255) b = 255; }
        c->img.px[i] = (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b;
        c->img.alpha[i] = (uint8_t)a;
    }
    p->canvas_dirty = 1;
}

/* an element's pixels (an <img>, <video> or <canvas>) as RGBA, kmalloc'd */
uint8_t* canvas_element_rgba(page_t* p, dom_node_t* n, int* w, int* h) {
    const uint32_t* px; const uint8_t* al; int sw, sh;
    if (!source_of(p, n, &px, &al, &sw, &sh)) return NULL;
    uint8_t* o = kmalloc((size_t)sw * sh * 4);
    if (!o) return NULL;
    for (size_t i = 0; i < (size_t)sw * sh; i++) {
        o[i * 4] = (uint8_t)(px[i] >> 16); o[i * 4 + 1] = (uint8_t)(px[i] >> 8); o[i * 4 + 2] = (uint8_t)px[i];
        o[i * 4 + 3] = al ? al[i] : 255;
    }
    *w = sw; *h = sh;
    return o;
}

void canvas_attr_changed(page_t* p, dom_node_t* n) {
    canvas_t* c = canvas_of(p, n, 0);
    if (c) { resize(c, attr_int(n, "width", 300), attr_int(n, "height", 150)); p->dirty = 1; }
}

void canvas_free_all(page_t* p) {
    for (canvas_t* c = p->canvases; c;) {
        canvas_t* next = c->next;
        free_pixels(c);
        if (c->st.clip) kfree(c->st.clip);
        for (int i = 0; i < c->nst; i++) if (c->stack[i].clip) kfree(c->stack[i].clip);
        if (c->pts) kfree(c->pts);
        if (c->subs) kfree(c->subs);
        if (c->closed) kfree(c->closed);
        if (c->node) { c->node->canvas = NULL; c->node->img = NULL; }
        kfree(c);
        c = next;
    }
    p->canvases = NULL;
    for (int i = 0; i < CV_GRADS; i++) {
        if (g_grads[i].own_px) kfree(g_grads[i].own_px);
        if (g_grads[i].own_al) kfree(g_grads[i].own_al);
    }
    memset(g_grads, 0, sizeof(g_grads));
}

/* ── the path ──────────────────────────────────────────────────────── */

static int grow(canvas_t* c, int more) {
    if (c->npts + more > c->cap) {
        int nc = c->cap ? c->cap * 2 : 256;
        while (nc < c->npts + more) nc *= 2;
        if (nc > 4000000) return 0;
        pt_t* np = kmalloc(sizeof(pt_t) * (size_t)nc);
        if (!np) return 0;
        if (c->npts) memcpy(np, c->pts, sizeof(pt_t) * (size_t)c->npts);
        if (c->pts) kfree(c->pts);
        c->pts = np;
        c->cap = nc;
    }
    return 1;
}

static void sub_begin(canvas_t* c) {
    if (c->nsubs == c->subcap) {
        int nc = c->subcap ? c->subcap * 2 : 32;
        int* ns = kmalloc(sizeof(int) * (size_t)nc);
        uint8_t* ncl = kmalloc((size_t)nc);
        if (!ns || !ncl) { if (ns) kfree(ns); if (ncl) kfree(ncl); return; }
        if (c->nsubs) { memcpy(ns, c->subs, sizeof(int) * (size_t)c->nsubs); memcpy(ncl, c->closed, (size_t)c->nsubs); }
        if (c->subs) kfree(c->subs);
        if (c->closed) kfree(c->closed);
        c->subs = ns;
        c->closed = ncl;
        c->subcap = nc;
    }
    c->subs[c->nsubs] = c->npts;
    c->closed[c->nsubs] = 0;
    c->nsubs++;
}

static void add_dev(canvas_t* c, double x, double y) {
    if (!grow(c, 1)) return;
    c->pts[c->npts].x = x;
    c->pts[c->npts].y = y;
    c->npts++;
}

static void move_to(canvas_t* c, double x, double y) {
    double dx, dy;
    pt(&c->st.m, x, y, &dx, &dy);
    sub_begin(c);
    add_dev(c, dx, dy);
    c->cx = x; c->cy = y; c->have_cur = 1;
}

static void line_to(canvas_t* c, double x, double y) {
    if (!c->have_cur || !c->nsubs) { move_to(c, x, y); return; }
    if (c->have_cur == 2) { double sx = c->cx, sy = c->cy; move_to(c, sx, sy); }
    double dx, dy;
    pt(&c->st.m, x, y, &dx, &dy);
    add_dev(c, dx, dy);
    c->cx = x; c->cy = y;
}

static void close_path(canvas_t* c) {
    if (!c->nsubs) return;
    c->closed[c->nsubs - 1] = 1;
    /* the next segment starts from the subpath's first point */
    int s = c->subs[c->nsubs - 1];
    mat_t inv;
    if (invert(&c->st.m, &inv)) {
        double ux, uy;
        pt(&inv, c->pts[s].x, c->pts[s].y, &ux, &uy);
        c->cx = ux; c->cy = uy;
    }
    c->have_cur = 2;                           /* the next segment starts a subpath there */
}

/* curves: flattened into lines in user space, enough segments for their size on screen */
static int curve_steps(canvas_t* c, double len) {
    int n = (int)(len * mat_scale(&c->st.m) / 3) + 4;
    return n > 256 ? 256 : n;
}

static void quad_to(canvas_t* c, double x1, double y1, double x, double y) {
    if (!c->have_cur) move_to(c, x1, y1);
    double x0 = c->cx, y0 = c->cy;
    int n = curve_steps(c, fabs_(x1 - x0) + fabs_(y1 - y0) + fabs_(x - x1) + fabs_(y - y1));
    for (int i = 1; i <= n; i++) {
        double t = (double)i / n, u = 1 - t;
        line_to(c, u * u * x0 + 2 * u * t * x1 + t * t * x, u * u * y0 + 2 * u * t * y1 + t * t * y);
    }
}

static void cubic_to(canvas_t* c, double x1, double y1, double x2, double y2, double x, double y) {
    if (!c->have_cur) move_to(c, x1, y1);
    double x0 = c->cx, y0 = c->cy;
    int n = curve_steps(c, fabs_(x1 - x0) + fabs_(y1 - y0) + fabs_(x2 - x1) + fabs_(y2 - y1) + fabs_(x - x2) + fabs_(y - y2));
    for (int i = 1; i <= n; i++) {
        double t = (double)i / n, u = 1 - t;
        double a = u * u * u, b = 3 * u * u * t, cc = 3 * u * t * t, d = t * t * t;
        line_to(c, a * x0 + b * x1 + cc * x2 + d * x, a * y0 + b * y1 + cc * y2 + d * y);
    }
}

static void ellipse(canvas_t* c, double x, double y, double rx, double ry, double rot, double a0, double a1, int ccw) {
    if (rx < 0 || ry < 0) return;
    double sweep = a1 - a0;
    if (!ccw) {
        if (sweep >= 2 * PI) sweep = 2 * PI;
        else { while (sweep < 0) sweep += 2 * PI; }
    } else {
        if (sweep <= -2 * PI) sweep = -2 * PI;
        else { while (sweep > 0) sweep -= 2 * PI; }
    }
    double r = rx > ry ? rx : ry;
    int n = (int)(fabs_(sweep) * r * mat_scale(&c->st.m) / 2) + 8;
    if (n > 512) n = 512;
    double cr = cos_(rot), sr = sin_(rot);
    for (int i = 0; i <= n; i++) {
        double t = a0 + sweep * i / n;
        double ex = rx * cos_(t), ey = ry * sin_(t);
        double px = x + ex * cr - ey * sr, py = y + ex * sr + ey * cr;
        if (i == 0 && !c->have_cur) move_to(c, px, py);
        else line_to(c, px, py);
    }
}

static void arc_to(canvas_t* c, double x1, double y1, double x2, double y2, double r) {
    if (!c->have_cur) { move_to(c, x1, y1); return; }
    double x0 = c->cx, y0 = c->cy;
    double ax = x0 - x1, ay = y0 - y1, bx = x2 - x1, by = y2 - y1;
    double la = sqrt_(ax * ax + ay * ay), lb = sqrt_(bx * bx + by * by);
    if (r <= 0 || la < 1e-9 || lb < 1e-9) { line_to(c, x1, y1); return; }
    ax /= la; ay /= la; bx /= lb; by /= lb;
    double cosang = ax * bx + ay * by;
    if (cosang > 0.999999 || cosang < -0.999999) { line_to(c, x1, y1); return; }
    double ang = __builtin_acos(cosang);
    double dist = r / __builtin_tan(ang / 2);
    double tx0 = x1 + ax * dist, ty0 = y1 + ay * dist, tx1 = x1 + bx * dist, ty1 = y1 + by * dist;
    double bisx = ax + bx, bisy = ay + by, bl = sqrt_(bisx * bisx + bisy * bisy);
    bisx /= bl; bisy /= bl;
    double cd = r / sin_(ang / 2);
    double ccx = x1 + bisx * cd, ccy = y1 + bisy * cd;
    double s0 = atan2_(ty0 - ccy, tx0 - ccx), s1 = atan2_(ty1 - ccy, tx1 - ccx);
    int ccw = (ax * by - ay * bx) > 0;          /* turning left: the arc runs counter-clockwise */
    line_to(c, tx0, ty0);
    ellipse(c, ccx, ccy, r, r, 0, s0, s1, ccw);
}

/* ── colours and painting ──────────────────────────────────────────── */

static uint32_t mix(uint32_t c0, uint32_t c1, double t) {
    if (t <= 0) return c0;
    if (t >= 1) return c1;
    uint32_t out = 0;
    for (int sh = 0; sh < 32; sh += 8) {
        int a = (int)((c0 >> sh) & 255), b = (int)((c1 >> sh) & 255);
        out |= (uint32_t)(a + (int)((b - a) * t)) << sh;
    }
    return out;
}

static uint32_t grad_color(const grad_t* g, double t) {
    if (g->n <= 0) return 0xFF000000u;
    if (t <= g->off[0]) return g->col[0];
    for (int i = 1; i < g->n; i++)
        if (t <= g->off[i]) {
            double span = g->off[i] - g->off[i - 1];
            return span <= 0 ? g->col[i] : mix(g->col[i - 1], g->col[i], (t - g->off[i - 1]) / span);
        }
    return g->col[g->n - 1];
}

/* the paint's colour at device pixel (x, y) (centre), in css_color_blend form */
typedef struct { const paint_t* p; const grad_t* g; mat_t inv; int ok; } painter_t;

static void painter(canvas_t* c, const paint_t* p, painter_t* out) {
    out->p = p;
    out->g = p->kind != PAINT_COLOR && p->id >= 0 && p->id < CV_GRADS && g_grads[p->id].used ? &g_grads[p->id] : NULL;
    out->ok = invert(&c->st.m, &out->inv);
}

static uint32_t sample_img(const uint32_t* px, const uint8_t* al, int w, int h, int x, int y) {
    if (x < 0 || y < 0 || x >= w || y >= h) return 0xFF000000u;
    uint32_t col = px[(size_t)y * w + x] & 0xFFFFFFu;
    int a = al ? al[(size_t)y * w + x] : 255;
    return col | (uint32_t)(255 - a) << 24;
}

static uint32_t paint_at(const painter_t* pp, double x, double y) {
    const paint_t* p = pp->p;
    if (p->kind == PAINT_COLOR || !pp->g || !pp->ok) return p->kind == PAINT_COLOR ? p->color : 0xFF000000u;
    const grad_t* g = pp->g;
    double ux, uy;
    pt(&pp->inv, x, y, &ux, &uy);
    if (p->kind == PAINT_PATTERN) {
        int sx = (int)floor_(ux), sy = (int)floor_(uy);
        if (g->rep_x) { sx %= g->pw; if (sx < 0) sx += g->pw; }
        if (g->rep_y) { sy %= g->ph; if (sy < 0) sy += g->ph; }
        const uint32_t* px = g->own_px ? g->own_px : g->img ? g->img->px : NULL;
        const uint8_t* al = g->own_px ? g->own_al : g->img ? g->img->alpha : NULL;
        return px ? sample_img(px, al, g->pw, g->ph, sx, sy) : 0xFF000000u;
    }
    double t;
    if (g->radial == 2) {                      /* conic: the angle around (x0, y0), from r0 */
        t = (atan2_(uy - g->y0, ux - g->x0) - g->r0) / (2 * PI);
        t -= floor_(t);
    } else if (!g->radial) {
        double dx = g->x1 - g->x0, dy = g->y1 - g->y0, l2 = dx * dx + dy * dy;
        t = l2 > 0 ? ((ux - g->x0) * dx + (uy - g->y0) * dy) / l2 : 0;
    } else {
        /* two circles: t where the point lies on the circle interpolated between them */
        double cdx = g->x1 - g->x0, cdy = g->y1 - g->y0, dr = g->r1 - g->r0;
        double pdx = ux - g->x0, pdy = uy - g->y0;
        double a = cdx * cdx + cdy * cdy - dr * dr;
        double b = pdx * cdx + pdy * cdy + g->r0 * dr;
        double cc = pdx * pdx + pdy * pdy - g->r0 * g->r0;
        if (fabs_(a) < 1e-9) t = b != 0 ? cc / (2 * b) : 0;
        else {
            double disc = b * b - a * cc;
            if (disc < 0) return 0xFF000000u;
            double sq = sqrt_(disc);
            t = (b + sq) / a;
            if (g->r0 + t * dr < 0) t = (b - sq) / a;
        }
    }
    return grad_color(g, t);
}

/* src (css_color_blend form) over pixel i with coverage cov (0..256) */
static void blend(canvas_t* c, size_t i, uint32_t src, int cov, int op) {
    int sa = (255 - (int)(src >> 24)) * cov >> 8;
    sa = (int)(sa * c->st.alpha);
    if (c->smask) {                            /* (the clip applies when the shadow is laid down) */
        if (sa > 0) c->smask[i] = (uint8_t)(sa + c->smask[i] * (255 - sa) / 255);
        return;
    }
    if (c->st.clip) sa = sa * c->st.clip[i] / 255;
    if (sa <= 0 && op != 3) return;
    uint32_t d = c->img.px[i];
    int da = c->img.alpha[i];
    int sr = (src >> 16) & 255, sg = (src >> 8) & 255, sb = src & 255;
    int dr = (d >> 16) & 255, dg = (d >> 8) & 255, db = d & 255;
    int oa, orr, og, ob;
    switch (op) {
    case 1:                                    /* destination-out */
        oa = da * (255 - sa) / 255;
        c->img.alpha[i] = (uint8_t)oa;
        return;
    case 2:                                    /* lighter */
        oa = sa + da > 255 ? 255 : sa + da;
        if (!oa) return;
        orr = (sr * sa + dr * da) / oa; og = (sg * sa + dg * da) / oa; ob = (sb * sa + db * da) / oa;
        break;
    case 3:                                    /* copy (inside the shape) */
        oa = sa; orr = sr; og = sg; ob = sb;
        break;
    case 4:                                    /* destination-over */
        oa = da + sa * (255 - da) / 255;
        if (!oa) return;
        orr = (dr * da + sr * sa * (255 - da) / 255) / oa;
        og = (dg * da + sg * sa * (255 - da) / 255) / oa;
        ob = (db * da + sb * sa * (255 - da) / 255) / oa;
        break;
    case 5:                                    /* source-atop */
        if (!da) return;
        oa = da;
        orr = (sr * sa + dr * (255 - sa)) / 255; og = (sg * sa + dg * (255 - sa)) / 255; ob = (sb * sa + db * (255 - sa)) / 255;
        break;
    case 6:                                    /* destination-in */
        oa = da * sa / 255;
        c->img.alpha[i] = (uint8_t)oa;
        return;
    case 7:                                    /* source-in */
        oa = sa * da / 255; orr = sr; og = sg; ob = sb;
        break;
    default:                                   /* source-over */
        oa = sa + da * (255 - sa) / 255;
        if (!oa) return;
        if (sa == 255 || !da) { orr = sr; og = sg; ob = sb; }
        else {
            int k = da * (255 - sa) / 255;
            orr = (sr * sa + dr * k) / oa; og = (sg * sa + dg * k) / oa; ob = (sb * sa + db * k) / oa;
        }
        break;
    }
    if (orr > 255) orr = 255;
    if (og > 255) og = 255;
    if (ob > 255) ob = 255;
    c->img.px[i] = (uint32_t)orr << 16 | (uint32_t)og << 8 | (uint32_t)ob;
    c->img.alpha[i] = (uint8_t)(oa > 255 ? 255 : oa);
}

/* ── rasterizing ───────────────────────────────────────────────────── */

typedef struct { double x0, y0, x1, y1; int dir; } edge_t;
typedef struct { edge_t* e; int n, cap; double miny, maxy, minx, maxx; } edges_t;

static void edge_add(edges_t* E, double x0, double y0, double x1, double y1) {
    if (y0 == y1) return;
    if (E->n == E->cap) {
        int nc = E->cap ? E->cap * 2 : 256;
        if (nc > 8000000) return;
        edge_t* ne = kmalloc(sizeof(edge_t) * (size_t)nc);
        if (!ne) return;
        if (E->n) memcpy(ne, E->e, sizeof(edge_t) * (size_t)E->n);
        if (E->e) kfree(E->e);
        E->e = ne;
        E->cap = nc;
    }
    edge_t* e = &E->e[E->n++];
    if (y0 < y1) { e->x0 = x0; e->y0 = y0; e->x1 = x1; e->y1 = y1; e->dir = 1; }
    else { e->x0 = x1; e->y0 = y1; e->x1 = x0; e->y1 = y0; e->dir = -1; }
    if (E->n == 1) { E->miny = e->y0; E->maxy = e->y1; E->minx = E->maxx = x0; }
    if (e->y0 < E->miny) E->miny = e->y0;
    if (e->y1 > E->maxy) E->maxy = e->y1;
    double lo = x0 < x1 ? x0 : x1, hi = x0 < x1 ? x1 : x0;
    if (lo < E->minx) E->minx = lo;
    if (hi > E->maxx) E->maxx = hi;
}

static void edges_free(edges_t* E) { if (E->e) kfree(E->e); memset(E, 0, sizeof(*E)); }

static void path_edges(canvas_t* c, edges_t* E) {
    for (int s = 0; s < c->nsubs; s++) {
        int a = c->subs[s], b = s + 1 < c->nsubs ? c->subs[s + 1] : c->npts;
        if (b - a < 2) continue;
        for (int i = a; i + 1 < b; i++) edge_add(E, c->pts[i].x, c->pts[i].y, c->pts[i + 1].x, c->pts[i + 1].y);
        edge_add(E, c->pts[b - 1].x, c->pts[b - 1].y, c->pts[a].x, c->pts[a].y);   /* filled: closed */
    }
}

typedef struct { double x; int dir; } cross_t;

static void sort_cross(cross_t* a, int n) {
    for (int i = 1; i < n; i++) {
        cross_t v = a[i];
        int j = i - 1;
        while (j >= 0 && a[j].x > v.x) { a[j + 1] = a[j]; j--; }
        a[j + 1] = v;
    }
}

/* coverage of the edges, one row at a time, handed to fn (0..256 a pixel) */
typedef void (*span_fn)(canvas_t* c, int y, int x0, int x1, const float* cov, void* ctx);

static void raster(canvas_t* c, edges_t* E, int evenodd, span_fn fn, void* ctx) {
    if (!E->n || !c->w || !c->h) return;
    int y0 = (int)floor_(E->miny), y1 = (int)floor_(E->maxy) + 1;
    int x0 = (int)floor_(E->minx), x1 = (int)floor_(E->maxx) + 2;
    y0 = iclamp(y0, 0, c->h); y1 = iclamp(y1, 0, c->h);
    x0 = iclamp(x0, 0, c->w); x1 = iclamp(x1, 0, c->w);
    if (y0 >= y1 || x0 >= x1) return;
    float* cov = kmalloc(sizeof(float) * (size_t)(c->w + 2));
    cross_t* cr = kmalloc(sizeof(cross_t) * (size_t)E->n);
    if (!cov || !cr) { if (cov) kfree(cov); if (cr) kfree(cr); return; }
    /* edges sorted by their top, an active window moving down */
    for (int y = y0; y < y1; y++) {
        for (int x = x0; x < x1; x++) cov[x] = 0;
        int any = 0;
        for (int s = 0; s < SS; s++) {
            double sy = y + (s + 0.5) / SS;
            int n = 0;
            for (int i = 0; i < E->n; i++) {
                const edge_t* e = &E->e[i];
                if (sy < e->y0 || sy >= e->y1) continue;
                double t = (sy - e->y0) / (e->y1 - e->y0);
                cr[n].x = e->x0 + (e->x1 - e->x0) * t;
                cr[n].dir = e->dir;
                n++;
            }
            if (n < 2) continue;
            sort_cross(cr, n);
            int wind = 0;
            for (int k = 0; k + 1 < n; k++) {
                wind += evenodd ? 1 : cr[k].dir;
                int inside = evenodd ? (wind & 1) : wind != 0;
                if (!inside) continue;
                double a = cr[k].x, b = cr[k + 1].x;
                if (b <= a) continue;
                if (a < x0) a = x0;
                if (b > x1) b = x1;
                if (b <= a) continue;
                int ia = (int)a, ib = (int)b;
                const float wgt = 1.0f / SS;
                if (ia == ib) cov[ia] += (float)(b - a) * wgt;
                else {
                    cov[ia] += (float)(ia + 1 - a) * wgt;
                    for (int x = ia + 1; x < ib; x++) cov[x] += wgt;
                    if (ib < x1) cov[ib] += (float)(b - ib) * wgt;
                }
                any = 1;
            }
        }
        if (any) fn(c, y, x0, x1, cov, ctx);
    }
    kfree(cov);
    kfree(cr);
}

typedef struct { painter_t pp; int op; } fill_ctx_t;

static void fill_span(canvas_t* c, int y, int x0, int x1, const float* cov, void* ctx) {
    fill_ctx_t* f = ctx;
    size_t row = (size_t)y * (size_t)c->w;
    int solid = f->pp.p->kind == PAINT_COLOR;
    uint32_t col = f->pp.p->color;
    for (int x = x0; x < x1; x++) {
        float v = cov[x];
        if (v <= 0.002f) continue;
        int k = v >= 1 ? 256 : (int)(v * 256);
        blend(c, row + (size_t)x, solid ? col : paint_at(&f->pp, x + 0.5, y + 0.5), k, f->op);
    }
}

static void fill_edges(canvas_t* c, edges_t* E, const paint_t* p, int evenodd) {
    fill_ctx_t f;
    painter(c, p, &f.pp);
    f.op = c->st.op;
    raster(c, E, evenodd, fill_span, &f);
}

/* ── strokes: segments, joins and caps as polygons, filled as one ─── */

static void poly(edges_t* E, const double* xy, int n) {
    /* every piece wound the same way, so nonzero makes their union */
    double area = 0;
    for (int i = 0; i < n; i++) {
        int j = (i + 1) % n;
        area += xy[2 * i] * xy[2 * j + 1] - xy[2 * j] * xy[2 * i + 1];
    }
    for (int i = 0; i < n; i++) {
        int a = area >= 0 ? i : n - 1 - i, b = area >= 0 ? (i + 1) % n : (2 * n - 2 - i) % n;
        edge_add(E, xy[2 * a], xy[2 * a + 1], xy[2 * b], xy[2 * b + 1]);
    }
}

static void disc(edges_t* E, double x, double y, double r) {
    double xy[64];
    int n = r < 2 ? 8 : r < 8 ? 16 : 32;
    for (int i = 0; i < n; i++) { xy[2 * i] = x + r * cos_(2 * PI * i / n); xy[2 * i + 1] = y + r * sin_(2 * PI * i / n); }
    poly(E, xy, n);
}

static void seg_poly(edges_t* E, double x0, double y0, double x1, double y1, double hw, int sq0, int sq1) {
    double dx = x1 - x0, dy = y1 - y0, l = sqrt_(dx * dx + dy * dy);
    if (l < 1e-9) return;
    double ux = dx / l, uy = dy / l, nx = -uy * hw, ny = ux * hw;
    if (sq0) { x0 -= ux * hw; y0 -= uy * hw; }
    if (sq1) { x1 += ux * hw; y1 += uy * hw; }
    double xy[8] = { x0 + nx, y0 + ny, x1 + nx, y1 + ny, x1 - nx, y1 - ny, x0 - nx, y0 - ny };
    poly(E, xy, 4);
}

static void join(edges_t* E, const pt_t* a, const pt_t* b, const pt_t* cpt, double hw, int kind, double miter) {
    double d0x = b->x - a->x, d0y = b->y - a->y, d1x = cpt->x - b->x, d1y = cpt->y - b->y;
    double l0 = sqrt_(d0x * d0x + d0y * d0y), l1 = sqrt_(d1x * d1x + d1y * d1y);
    if (l0 < 1e-9 || l1 < 1e-9) return;
    d0x /= l0; d0y /= l0; d1x /= l1; d1y /= l1;
    if (kind == 1) { disc(E, b->x, b->y, hw); return; }     /* round */
    double cross = d0x * d1y - d0y * d1x;
    if (fabs_(cross) < 1e-9) return;
    double s = cross > 0 ? -1 : 1;                           /* the outer side */
    double p0x = b->x + s * -d0y * hw, p0y = b->y + s * d0x * hw;
    double p1x = b->x + s * -d1y * hw, p1y = b->y + s * d1x * hw;
    if (kind == 0) {                                         /* miter, if within the limit */
        double cosang = -(d0x * d1x + d0y * d1y);
        double ratio = 1 / sqrt_((1 - cosang) / 2 > 1e-12 ? (1 - cosang) / 2 : 1e-12);
        if (ratio <= miter) {
            double mx = (p0x + p1x) / 2 - b->x, my = (p0y + p1y) / 2 - b->y, ml = sqrt_(mx * mx + my * my);
            if (ml > 1e-9) {
                double len = hw * ratio;
                double tipx = b->x + mx / ml * len, tipy = b->y + my / ml * len;
                double xy[8] = { b->x, b->y, p0x, p0y, tipx, tipy, p1x, p1y };
                poly(E, xy, 4);
                return;
            }
        }
    }
    double xy[6] = { b->x, b->y, p0x, p0y, p1x, p1y };       /* bevel */
    poly(E, xy, 3);
}

/* one polyline (device space) as stroke polygons */
static void stroke_line(canvas_t* c, edges_t* E, const pt_t* p, int n, int closed, double hw) {
    if (n < 2) {
        if (n == 1 && c->st.cap == 1) disc(E, p[0].x, p[0].y, hw);
        return;
    }
    int sq = c->st.cap == 2;
    for (int i = 0; i + 1 < n; i++)
        seg_poly(E, p[i].x, p[i].y, p[i + 1].x, p[i + 1].y, hw, !closed && sq && i == 0, !closed && sq && i + 2 == n);
    if (closed) seg_poly(E, p[n - 1].x, p[n - 1].y, p[0].x, p[0].y, hw, 0, 0);
    for (int i = 1; i + 1 < n; i++) join(E, &p[i - 1], &p[i], &p[i + 1], hw, c->st.join, c->st.miter);
    if (closed) {
        join(E, &p[n - 2], &p[n - 1], &p[0], hw, c->st.join, c->st.miter);
        join(E, &p[n - 1], &p[0], &p[1], hw, c->st.join, c->st.miter);
    } else if (c->st.cap == 1) {
        disc(E, p[0].x, p[0].y, hw);
        disc(E, p[n - 1].x, p[n - 1].y, hw);
    }
}

/* the subpath cut by the dash pattern into pieces, each stroked */
static void stroke_sub(canvas_t* c, edges_t* E, const pt_t* p, int n, int closed, double hw, double scale) {
    if (!c->st.ndash) { stroke_line(c, E, p, n, closed, hw); return; }
    double total = 0;
    for (int i = 0; i < c->st.ndash; i++) total += c->st.dash[i] * scale;
    if (total <= 0) { stroke_line(c, E, p, n, closed, hw); return; }
    int di = 0;
    double left = c->st.dash[0] * scale, off = c->st.dash_off * scale;
    while (off > 0) {                                   /* the offset into the pattern */
        double step = off < left ? off : left;
        left -= step; off -= step;
        if (left <= 0) { di = (di + 1) % c->st.ndash; left = c->st.dash[di] * scale; }
        if (total <= 0) break;
    }
    pt_t piece[2];
    int segs = closed ? n : n - 1;
    for (int i = 0; i < segs; i++) {
        pt_t a = p[i], b = p[(i + 1) % n];
        double dx = b.x - a.x, dy = b.y - a.y, l = sqrt_(dx * dx + dy * dy), pos = 0;
        while (l > 0 && pos < l) {
            double step = left < l - pos ? left : l - pos;
            if (!(di & 1)) {
                piece[0].x = a.x + dx * pos / l; piece[0].y = a.y + dy * pos / l;
                piece[1].x = a.x + dx * (pos + step) / l; piece[1].y = a.y + dy * (pos + step) / l;
                stroke_line(c, E, piece, 2, 0, hw);
            }
            pos += step;
            left -= step;
            if (left <= 1e-9) { di = (di + 1) % c->st.ndash; left = c->st.dash[di] * scale; if (left <= 0) left = 1e-3; }
        }
    }
}

static void stroke_path(canvas_t* c) {
    edges_t E;
    memset(&E, 0, sizeof(E));
    double scale = mat_scale(&c->st.m), hw = c->st.lw * scale / 2;
    if (hw < 0.25) hw = 0.25;                            /* hairlines stay visible, a little faint */
    for (int s = 0; s < c->nsubs; s++) {
        int a = c->subs[s], b = s + 1 < c->nsubs ? c->subs[s + 1] : c->npts;
        if (b - a < 1) continue;
        /* a closed subpath ends where it began: drop that repeated point */
        int n = b - a;
        if (c->closed[s] && n > 2 && c->pts[a].x == c->pts[b - 1].x && c->pts[a].y == c->pts[b - 1].y) n--;
        stroke_sub(c, &E, c->pts + a, n, c->closed[s] && n > 2, hw, scale);
    }
    fill_edges(c, &E, &c->st.stroke, 0);
    edges_free(&E);
}

static void fill_path(canvas_t* c, int evenodd) {
    edges_t E;
    memset(&E, 0, sizeof(E));
    path_edges(c, &E);
    fill_edges(c, &E, &c->st.fill, evenodd);
    edges_free(&E);
}

/* the user-space rectangle as a 4-point polygon in device space */
static void rect_edges(canvas_t* c, edges_t* E, double x, double y, double w, double h) {
    double px[4], py[4];
    pt(&c->st.m, x, y, &px[0], &py[0]);
    pt(&c->st.m, x + w, y, &px[1], &py[1]);
    pt(&c->st.m, x + w, y + h, &px[2], &py[2]);
    pt(&c->st.m, x, y + h, &px[3], &py[3]);
    for (int i = 0; i < 4; i++) edge_add(E, px[i], py[i], px[(i + 1) % 4], py[(i + 1) % 4]);
}

static void clear_span(canvas_t* c, int y, int x0, int x1, const float* cov, void* ctx) {
    (void)ctx;
    size_t row = (size_t)y * (size_t)c->w;
    for (int x = x0; x < x1; x++) {
        float v = cov[x];
        if (v <= 0.002f) continue;
        int keep = v >= 1 ? 0 : (int)((1 - v) * 256);
        if (c->st.clip) keep = 256 - (256 - keep) * c->st.clip[row + x] / 255;
        c->img.alpha[row + x] = (uint8_t)(c->img.alpha[row + x] * keep >> 8);
    }
}

static void clip_span(canvas_t* c, int y, int x0, int x1, const float* cov, void* ctx) {
    uint8_t* mask = ctx;
    size_t row = (size_t)y * (size_t)c->w;
    for (int x = x0; x < x1; x++) {
        float v = cov[x];
        mask[row + x] = (uint8_t)(v >= 1 ? 255 : v <= 0 ? 0 : (int)(v * 255));
    }
}

static void clip_path(canvas_t* c, int evenodd) {
    size_t n = (size_t)c->w * (size_t)c->h;
    if (!n) return;
    uint8_t* mask = kzalloc(n);
    if (!mask) return;
    edges_t E;
    memset(&E, 0, sizeof(E));
    path_edges(c, &E);
    raster(c, &E, evenodd, clip_span, mask);
    edges_free(&E);
    if (c->st.clip) {
        for (size_t i = 0; i < n; i++) mask[i] = (uint8_t)(mask[i] * c->st.clip[i] / 255);
        kfree(c->st.clip);
    }
    c->st.clip = mask;
}

/* ── text ──────────────────────────────────────────────────────────── */

static int face_of(const state_t* s) {
    if (s->mono) return s->bold ? FONT_MONO_BOLD : FONT_MONO;
    return s->bold ? FONT_SANS_BOLD : FONT_SANS;
}

/* "italic bold 16px Arial" */
static void set_font(state_t* s, const char* v) {
    int px = 0;
    s->bold = s->italic = s->mono = 0;
    for (const char* p = v; *p; p++) {
        if (!strncasecmp(p, "bold", 4) || !strncmp(p, "600", 3) || !strncmp(p, "700", 3) || !strncmp(p, "800", 3) || !strncmp(p, "900", 3)) s->bold = 1;
        if (!strncasecmp(p, "italic", 6) || !strncasecmp(p, "oblique", 7)) s->italic = 1;
        if (!strncasecmp(p, "monospace", 9) || !strncasecmp(p, "courier", 7) || !strncasecmp(p, "consolas", 8)) s->mono = 1;
    }
    for (const char* p = v; *p; p++) {
        if (*p >= '0' && *p <= '9' && (p == v || p[-1] == ' ')) {
            const char* q = p;
            double n = 0, frac = 0, scale = 1;
            while (*q >= '0' && *q <= '9') { n = n * 10 + (*q - '0'); q++; }
            if (*q == '.') { q++; while (*q >= '0' && *q <= '9') { scale /= 10; frac += (*q - '0') * scale; q++; } }
            n += frac;
            if (!strncmp(q, "px", 2)) { px = (int)(n + 0.5); break; }
            if (!strncmp(q, "pt", 2)) { px = (int)(n * 4 / 3 + 0.5); break; }
            if (!strncmp(q, "em", 2) || !strncmp(q, "rem", 3)) { px = (int)(n * 16 + 0.5); break; }
        }
    }
    if (px > 0) s->font_px = px > 400 ? 400 : px;
}

static double text_width(const state_t* s, const char* t) {
    return font_text_width(face_of(s), s->font_px, t, (uint32_t)strlen(t));
}

static void draw_text(canvas_t* c, const char* t, double x, double y, double maxw, const paint_t* paint) {
    uint32_t len = (uint32_t)strlen(t);
    if (!len || !c->w) return;
    double scale = mat_scale(&c->st.m);
    int px = (int)(c->st.font_px * scale + 0.5);
    if (px < 1) return;
    if (px > 400) px = 400;
    int face = face_of(&c->st);
    int tw = font_text_width(face, px, t, len);
    double squeeze = 1;
    if (maxw > 0 && tw > maxw * scale) squeeze = maxw * scale / tw;
    int asc, desc, lh;
    font_metrics(face, px, &asc, &desc, &lh);
    /* where the text begins, against textAlign / textBaseline (device space) */
    double dx, dy;
    pt(&c->st.m, x, y, &dx, &dy);
    double w = tw * squeeze;
    if (c->st.align == 1) dx -= w / 2;               /* center */
    else if (c->st.align == 2) dx -= w;              /* right / end */
    if (c->st.baseline == 1) dy += asc;              /* top / hanging */
    else if (c->st.baseline == 2) dy += (asc - desc) / 2.0;   /* middle */
    else if (c->st.baseline == 3) dy -= desc;        /* bottom / ideographic */
    /* the glyphs as coverage: white text on black, then painted through it */
    int bw = tw + px / 2 + 4, bh = asc + desc + 4;
    if ((uint64_t)bw * bh > 16u << 20) return;
    uint32_t* buf = kzalloc((size_t)bw * (size_t)bh * 4);
    if (!buf) return;
    font_draw(buf, bw, 0, 0, bw, bh, face, px, 1, asc + 2, t, len, 0xFFFFFF, c->st.italic);
    painter_t pp;
    painter(c, paint, &pp);
    const mat_t* m = &c->st.m;
    if (m->b != 0 || m->c != 0 || m->a <= 0 || m->d <= 0 || fabs_(m->a - m->d) > 1e-6 * m->a) {
        /* rotated, skewed, mirrored or stretched: each device pixel looks
         * back into the glyphs through the transform. Glyph pixel (bx, by)
         * is user point (ux + (bx - 1) sq / scale, uy + (by - asc - 2) / scale) */
        double ux = x, uy = y, wu = w / scale;
        if (c->st.align == 1) ux -= wu / 2;
        else if (c->st.align == 2) ux -= wu;
        if (c->st.baseline == 1) uy += asc / scale;
        else if (c->st.baseline == 2) uy += (asc - desc) / 2.0 / scale;
        else if (c->st.baseline == 3) uy -= desc / scale;
        mat_t g;                                    /* glyph pixel -> device */
        double kx = squeeze / scale, ky = 1 / scale;
        double tx = ux - kx, ty = uy - (asc + 2) * ky;
        g.a = m->a * kx; g.b = m->b * kx; g.c = m->c * ky; g.d = m->d * ky;
        g.e = m->a * tx + m->c * ty + m->e; g.f = m->b * tx + m->d * ty + m->f;
        mat_t gi;
        if (!invert(&g, &gi)) { kfree(buf); return; }
        double xs[4], ys[4];
        pt(&g, 0, 0, &xs[0], &ys[0]); pt(&g, bw, 0, &xs[1], &ys[1]);
        pt(&g, bw, bh, &xs[2], &ys[2]); pt(&g, 0, bh, &xs[3], &ys[3]);
        double minx = xs[0], maxx = xs[0], miny = ys[0], maxy = ys[0];
        for (int i = 1; i < 4; i++) {
            if (xs[i] < minx) minx = xs[i];
            if (xs[i] > maxx) maxx = xs[i];
            if (ys[i] < miny) miny = ys[i];
            if (ys[i] > maxy) maxy = ys[i];
        }
        int x0 = iclamp((int)floor_(minx), 0, c->w), x1 = iclamp((int)floor_(maxx) + 1, 0, c->w);
        int y0 = iclamp((int)floor_(miny), 0, c->h), y1 = iclamp((int)floor_(maxy) + 1, 0, c->h);
        for (int cy = y0; cy < y1; cy++)
            for (int cx = x0; cx < x1; cx++) {
                double bx, by;
                pt(&gi, cx + 0.5, cy + 0.5, &bx, &by);
                bx -= 0.5; by -= 0.5;                   /* (bilinear, between glyph pixel centres) */
                int ix = (int)floor_(bx), iy = (int)floor_(by);
                if (ix < -1 || iy < -1 || ix >= bw || iy >= bh) continue;
                double fx = bx - ix, fy = by - iy, k = 0;
                for (int j = 0; j < 4; j++) {
                    int sx = ix + (j & 1), sy = iy + (j >> 1);
                    if (sx < 0 || sy < 0 || sx >= bw || sy >= bh) continue;
                    double wt = ((j & 1) ? fx : 1 - fx) * ((j >> 1) ? fy : 1 - fy);
                    k += wt * (double)((buf[(size_t)sy * bw + sx] >> 8) & 255);
                }
                int ki = (int)(k + 0.5);
                if (ki <= 0) continue;
                blend(c, (size_t)cy * c->w + cx, paint->kind == PAINT_COLOR ? paint->color : paint_at(&pp, cx + 0.5, cy + 0.5),
                      ki * 256 / 255, c->st.op);
            }
        kfree(buf);
        return;
    }
    int ox = (int)floor_(dx + 0.5) - 1, oy = (int)floor_(dy + 0.5) - (asc + 2);
    for (int yy = 0; yy < bh; yy++) {
        int cy = oy + yy;
        if (cy < 0 || cy >= c->h) continue;
        for (int xx = 0; xx < (int)(bw * squeeze); xx++) {
            int sx = squeeze < 1 ? (int)(xx / squeeze) : xx;
            if (sx >= bw) break;
            int k = (int)((buf[(size_t)yy * bw + sx] >> 8) & 255);
            if (!k) continue;
            int cx = ox + xx;
            if (cx < 0 || cx >= c->w) continue;
            blend(c, (size_t)cy * c->w + cx, paint->kind == PAINT_COLOR ? paint->color : paint_at(&pp, cx + 0.5, cy + 0.5),
                  k * 256 / 255, c->st.op);
        }
    }
    kfree(buf);
}

/* ── images ────────────────────────────────────────────────────────── */

static int source_of(page_t* p, dom_node_t* n, const uint32_t** px, const uint8_t** al, int* w, int* h) {
    if (!n || n->type != DOM_ELEM) return 0;
    if (!strcmp(n->tag, "canvas")) {
        canvas_t* s = canvas_of(p, n, 0);
        if (!s || !s->img.px) return 0;
        *px = s->img.px; *al = s->img.alpha; *w = s->w; *h = s->h;
        return 1;
    }
    img_data_t* im = n->img;
    if (!im || im->failed || !im->px || im->w <= 0 || im->h <= 0) return 0;
    *px = im->px; *al = im->alpha; *w = im->w; *h = im->h;
    return 1;
}

static void draw_image(canvas_t* c, const uint32_t* spx, const uint8_t* sal, int sw0, int sh0,
                       double sx, double sy, double sw, double sh, double dx, double dy, double dw, double dh) {
    if (sw <= 0 || sh <= 0 || dw == 0 || dh == 0) return;
    /* the destination's corners on the canvas, and the way back */
    mat_t m = c->st.m;
    m.e += m.a * dx + m.c * dy;
    m.f += m.b * dx + m.d * dy;
    m.a *= dw / sw; m.b *= dw / sw; m.c *= dh / sh; m.d *= dh / sh;
    /* (now source pixel (u - sx, v - sy) maps through m) */
    mat_t inv;
    if (!invert(&m, &inv)) return;
    double xs[4], ys[4];
    pt(&m, 0, 0, &xs[0], &ys[0]);
    pt(&m, sw, 0, &xs[1], &ys[1]);
    pt(&m, sw, sh, &xs[2], &ys[2]);
    pt(&m, 0, sh, &xs[3], &ys[3]);
    double minx = xs[0], maxx = xs[0], miny = ys[0], maxy = ys[0];
    for (int i = 1; i < 4; i++) {
        if (xs[i] < minx) minx = xs[i];
        if (xs[i] > maxx) maxx = xs[i];
        if (ys[i] < miny) miny = ys[i];
        if (ys[i] > maxy) maxy = ys[i];
    }
    int x0 = iclamp((int)floor_(minx), 0, c->w), x1 = iclamp((int)floor_(maxx) + 1, 0, c->w);
    int y0 = iclamp((int)floor_(miny), 0, c->h), y1 = iclamp((int)floor_(maxy) + 1, 0, c->h);
    int smooth = c->st.smooth && (fabs_(m.a) > 1.01 || fabs_(m.d) > 1.01 || m.b != 0 || m.c != 0);
    for (int y = y0; y < y1; y++) {
        for (int x = x0; x < x1; x++) {
            double u, v;
            pt(&inv, x + 0.5, y + 0.5, &u, &v);
            if (u < 0 || v < 0 || u >= sw || v >= sh) continue;
            u += sx; v += sy;
            uint32_t col;
            if (!smooth) col = sample_img(spx, sal, sw0, sh0, (int)u, (int)v);
            else {
                double fu = u - 0.5, fv = v - 0.5;
                int iu = (int)floor_(fu), iv = (int)floor_(fv);
                double tu = fu - iu, tv = fv - iv;
                int u0 = iclamp(iu, 0, sw0 - 1), u1 = iclamp(iu + 1, 0, sw0 - 1);
                int v0 = iclamp(iv, 0, sh0 - 1), v1 = iclamp(iv + 1, 0, sh0 - 1);
                uint32_t a = sample_img(spx, sal, sw0, sh0, u0, v0), b = sample_img(spx, sal, sw0, sh0, u1, v0);
                uint32_t cc = sample_img(spx, sal, sw0, sh0, u0, v1), d = sample_img(spx, sal, sw0, sh0, u1, v1);
                col = mix(mix(a, b, tu), mix(cc, d, tu), tv);
            }
            blend(c, (size_t)y * c->w + x, col, 256, c->st.op);
        }
    }
}


/* ── shadows ───────────────────────────────────────────────────────── */

static int shadow_on(const canvas_t* c) {
    return (c->st.shadow_color >> 24) != 255 &&
           (c->st.shadow_blur > 0 || c->st.shadow_x != 0 || c->st.shadow_y != 0);
}

/* one pass of a box blur of radius r over the rows (step 1) or columns (step
 * w) of the box x0..x1, y0..y1 */
static void box_pass(uint8_t* m, uint8_t* tmp, int w, int x0, int y0, int x1, int y1, int r, int vertical) {
    int n = vertical ? y1 - y0 : x1 - x0, lines = vertical ? x1 - x0 : y1 - y0;
    int div = 2 * r + 1;
    for (int l = 0; l < lines; l++) {
        uint8_t* base = vertical ? m + (size_t)y0 * w + x0 + l : m + (size_t)(y0 + l) * w + x0;
        size_t step = vertical ? (size_t)w : 1;
        for (int k = 0; k < n; k++) tmp[k] = base[k * step];
        int sum = 0;
        for (int k = -r; k <= r; k++) sum += k >= 0 && k < n ? tmp[k] : 0;
        for (int k = 0; k < n; k++) {
            base[k * step] = (uint8_t)((sum + div / 2) / div);
            int add = k + r + 1, sub = k - r;
            sum += (add < n ? tmp[add] : 0) - (sub >= 0 ? tmp[sub] : 0);
        }
    }
}

/* the shadow of what the capture pass drew: blurred (three box blurs make
 * about the Gaussian of sigma = blur / 2), moved by the offset (not
 * transformed), and laid down in the shadow's colour under the clip and the
 * compositing operation */
static void shadow_draw(canvas_t* c, uint8_t* m) {
    int w = c->w, h = c->h;
    int x0 = w, y0 = h, x1 = 0, y1 = 0;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            if (m[(size_t)y * w + x]) {
                if (x < x0) x0 = x;
                if (x >= x1) x1 = x + 1;
                if (y < y0) y0 = y;
                if (y >= y1) y1 = y + 1;
            }
    if (x0 >= x1) return;
    double sigma = c->st.shadow_blur / 2;
    int r = sigma > 0 ? (int)(sqrt_(sigma * sigma + 0.25) - 0.5 + 0.5) : 0;
    if (r > 0) {
        x0 = iclamp(x0 - 3 * r, 0, w); x1 = iclamp(x1 + 3 * r, 0, w);
        y0 = iclamp(y0 - 3 * r, 0, h); y1 = iclamp(y1 + 3 * r, 0, h);
        uint8_t* tmp = kmalloc((size_t)(w > h ? w : h));
        if (tmp) {
            for (int k = 0; k < 3; k++) {
                box_pass(m, tmp, w, x0, y0, x1, y1, r, 0);
                box_pass(m, tmp, w, x0, y0, x1, y1, r, 1);
            }
            kfree(tmp);
        }
    }
    int ox = (int)floor_(c->st.shadow_x + 0.5), oy = (int)floor_(c->st.shadow_y + 0.5);
    double alpha = c->st.alpha;
    c->st.alpha = 1;                           /* (the capture held it already) */
    for (int y = y0; y < y1; y++) {
        int dy = y + oy;
        if (dy < 0 || dy >= h) continue;
        for (int x = x0; x < x1; x++) {
            int dx = x + ox, k = m[(size_t)y * w + x];
            if (!k || dx < 0 || dx >= w) continue;
            blend(c, (size_t)dy * w + dx, c->st.shadow_color, k * 256 / 255, c->st.op);
        }
    }
    c->st.alpha = alpha;
}

/* ── image data, PNG ───────────────────────────────────────────────── */

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static char* b64_encode(const uint8_t* d, size_t n, size_t* out_len) {
    size_t m = (n + 2) / 3 * 4;
    char* o = kmalloc(m + 1);
    if (!o) return NULL;
    size_t k = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)d[i] << 16 | (i + 1 < n ? (uint32_t)d[i + 1] << 8 : 0) | (i + 2 < n ? d[i + 2] : 0);
        o[k++] = B64[v >> 18 & 63];
        o[k++] = B64[v >> 12 & 63];
        o[k++] = i + 1 < n ? B64[v >> 6 & 63] : '=';
        o[k++] = i + 2 < n ? B64[v & 63] : '=';
    }
    o[k] = 0;
    *out_len = k;
    return o;
}

static uint32_t crc_table[256];
static uint32_t crc32_(const uint8_t* d, size_t n, uint32_t c) {
    if (!crc_table[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t v = i;
            for (int k = 0; k < 8; k++) v = v & 1 ? 0xEDB88320u ^ (v >> 1) : v >> 1;
            crc_table[i] = v;
        }
    c = ~c;
    for (size_t i = 0; i < n; i++) c = crc_table[(c ^ d[i]) & 255] ^ (c >> 8);
    return ~c;
}

static void be32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

/* the canvas as a PNG (RGBA, deflate with stored blocks: no compression) */
static uint8_t* png_encode(canvas_t* c, size_t* out_len) {
    size_t raw = (size_t)c->h * ((size_t)c->w * 4 + 1);
    size_t blocks = (raw + 65534) / 65535;
    size_t zlen = 2 + raw + blocks * 5 + 4;
    size_t total = 8 + 25 + 12 + zlen + 12;
    uint8_t* o = kmalloc(total);
    uint8_t* r = kmalloc(raw ? raw : 1);
    if (!o || !r) { if (o) kfree(o); if (r) kfree(r); return NULL; }
    size_t k = 0;
    for (int y = 0; y < c->h; y++) {
        r[k++] = 0;
        for (int x = 0; x < c->w; x++) {
            uint32_t p = c->img.px[(size_t)y * c->w + x];
            r[k++] = (uint8_t)(p >> 16); r[k++] = (uint8_t)(p >> 8); r[k++] = (uint8_t)p;
            r[k++] = c->img.alpha[(size_t)y * c->w + x];
        }
    }
    static const uint8_t sig[8] = { 137, 'P', 'N', 'G', 13, 10, 26, 10 };
    memcpy(o, sig, 8);
    size_t p = 8;
    be32(o + p, 13); memcpy(o + p + 4, "IHDR", 4);
    be32(o + p + 8, (uint32_t)c->w); be32(o + p + 12, (uint32_t)c->h);
    o[p + 16] = 8; o[p + 17] = 6; o[p + 18] = 0; o[p + 19] = 0; o[p + 20] = 0;
    be32(o + p + 21, crc32_(o + p + 4, 17, 0));
    p += 25;
    be32(o + p, (uint32_t)zlen); memcpy(o + p + 4, "IDAT", 4);
    size_t z = p + 8;
    o[z++] = 0x78; o[z++] = 0x01;
    uint32_t a1 = 1, a2 = 0;
    for (size_t off = 0; off < raw || (raw == 0 && off == 0); ) {
        size_t n = raw - off > 65535 ? 65535 : raw - off;
        o[z++] = off + n >= raw ? 1 : 0;
        o[z++] = (uint8_t)n; o[z++] = (uint8_t)(n >> 8);
        o[z++] = (uint8_t)~n; o[z++] = (uint8_t)(~n >> 8);
        memcpy(o + z, r + off, n);
        for (size_t i = 0; i < n; i++) { a1 = (a1 + r[off + i]) % 65521; a2 = (a2 + a1) % 65521; }
        z += n;
        off += n;
        if (!n) break;
    }
    be32(o + z, a2 << 16 | a1); z += 4;
    be32(o + z, crc32_(o + p + 4, zlen + 4, 0)); z += 4;
    be32(o + z, 0); memcpy(o + z + 4, "IEND", 4); be32(o + z + 8, crc32_(o + z + 4, 4, 0)); z += 12;
    kfree(r);
    *out_len = z;
    return o;
}

/* ── the JavaScript side: __cv(element, op, ...) ───────────────────── */

enum {
    OP_FILLRECT = 1, OP_STROKERECT, OP_CLEARRECT, OP_BEGIN, OP_MOVE, OP_LINE, OP_CLOSE, OP_RECT, OP_ARC,
    OP_ARCTO, OP_QUAD, OP_CUBIC, OP_ELLIPSE, OP_FILL, OP_STROKE, OP_CLIP, OP_SAVE, OP_RESTORE,
    OP_TRANSFORM, OP_SETTRANSFORM, OP_RESET_TRANSFORM, OP_STYLE, OP_GRAD_NEW, OP_GRAD_STOP, OP_PATTERN,
    OP_LINEWIDTH, OP_CAP, OP_JOIN, OP_MITER, OP_ALPHA, OP_COMPOSITE, OP_FONT, OP_ALIGN, OP_BASELINE,
    OP_FILLTEXT, OP_STROKETEXT, OP_MEASURE, OP_DRAWIMAGE, OP_GETDATA, OP_PUTDATA, OP_DATAURL, OP_SIZE,
    OP_DASH, OP_DASHOFF, OP_INPATH, OP_ROUNDRECT, OP_SMOOTH, OP_GETTRANSFORM, OP_RESET, OP_INSTROKE,
    OP_SHADOW_COLOR, OP_SHADOW                 /* (color) / (0 blur, 1 offset x, 2 offset y; value) */
};

static page_t* host_page(interp_t* I) { return (page_t*)script_host(I); }

static double num(interp_t* I, int argc, value_t* argv, int i) {
    if (i >= argc) return 0;
    num_t v = v_tonum(I, argv[i]);
    return v != v ? 0 : (double)v;
}
static int finite_(double v) { return v == v && v < 1e300 && v > -1e300; }

static paint_t* style_slot(canvas_t* c, int which) { return which ? &c->st.stroke : &c->st.fill; }

static value_t cv_call(interp_t* I, value_t self, int argc, value_t* argv) {
    (void)self;
    page_t* p = host_page(I);
    if (argc < 2) return v_undef();
    dom_node_t* n = jsdom_node_of(argv[0]);
    int op = (int)num(I, argc, argv, 1);
    if (op == OP_GRAD_NEW) {                           /* (no canvas needed) */
        for (int i = 0; i < CV_GRADS; i++) {
            grad_t* g = &g_grads[i];
            if (g->used) continue;
            memset(g, 0, sizeof(*g));
            g->used = 1;
            g->radial = (int)num(I, argc, argv, 2);
            g->x0 = num(I, argc, argv, 3); g->y0 = num(I, argc, argv, 4);
            if (g->radial == 2) g->r0 = num(I, argc, argv, 5);         /* conic: start angle */
            else if (g->radial) {
                g->r0 = num(I, argc, argv, 5); g->x1 = num(I, argc, argv, 6); g->y1 = num(I, argc, argv, 7); g->r1 = num(I, argc, argv, 8);
            } else { g->x1 = num(I, argc, argv, 5); g->y1 = num(I, argc, argv, 6); }
            return v_num(i);
        }
        return v_num(-1);
    }
    if (op == OP_GRAD_STOP) {
        int id = (int)num(I, argc, argv, 2);
        if (id < 0 || id >= CV_GRADS || !g_grads[id].used) return v_undef();
        grad_t* g = &g_grads[id];
        double off = num(I, argc, argv, 3);
        int ok;
        uint32_t col = css_color_blend(argc > 4 ? v_cstr(I, argv[4]) : "", &ok);
        if (!ok || off < 0 || off > 1) { script_throw(I, "SyntaxError: bad color stop"); return v_undef(); }
        if (ok == 2) col = 0xFF000000u;
        if (g->n < 16) {
            int k = g->n++;
            while (k > 0 && g->off[k - 1] > off) { g->off[k] = g->off[k - 1]; g->col[k] = g->col[k - 1]; k--; }
            g->off[k] = off;
            g->col[k] = col;
        }
        return v_undef();
    }
    canvas_t* c = canvas_of(p, n, 1);
    if (!c) return v_undef();
    if (c->w != attr_int(n, "width", 300) || c->h != attr_int(n, "height", 150))   /* setAttribute('width', ...) */
        resize(c, attr_int(n, "width", 300), attr_int(n, "height", 150));
    double a = num(I, argc, argv, 2), b = num(I, argc, argv, 3), cc = num(I, argc, argv, 4), d = num(I, argc, argv, 5);
    double e = num(I, argc, argv, 6), f = num(I, argc, argv, 7);
    int draws = 0;
    if (!c->smask && shadow_on(c) && (op == OP_FILLRECT || op == OP_STROKERECT || op == OP_FILL || op == OP_STROKE ||
                                    op == OP_FILLTEXT || op == OP_STROKETEXT || op == OP_DRAWIMAGE)) {
        /* the shape once into a mask, for its shadow; then itself */
        uint8_t* m = kzalloc((size_t)c->w * (size_t)c->h);
        if (m) {
            c->smask = m;
            cv_call(I, self, argc, argv);
            c->smask = NULL;
            shadow_draw(c, m);
            kfree(m);
        }
    }
    switch (op) {
    case OP_SIZE: {
        obj_t* o = obj_new(I, OBJ_PLAIN);
        obj_set(I, o, "width", v_num(c->w));
        obj_set(I, o, "height", v_num(c->h));
        return v_obj(o);
    }
    case OP_FILLRECT: case OP_STROKERECT: case OP_CLEARRECT: {
        if (!finite_(a) || !finite_(b) || !finite_(cc) || !finite_(d)) return v_undef();
        if (op == OP_STROKERECT) {
            /* a path of its own: the current one is set aside, then back */
            pt_t* sp = c->pts; int sn = c->npts, scap = c->cap;
            int* ssub = c->subs; uint8_t* scl = c->closed; int sns = c->nsubs, sscap = c->subcap;
            double scx = c->cx, scy = c->cy;
            int shc = c->have_cur;
            c->pts = NULL; c->npts = c->cap = 0; c->subs = NULL; c->closed = NULL; c->nsubs = c->subcap = 0; c->have_cur = 0;
            move_to(c, a, b); line_to(c, a + cc, b); line_to(c, a + cc, b + d); line_to(c, a, b + d); close_path(c);
            stroke_path(c);
            if (c->pts) kfree(c->pts);
            if (c->subs) kfree(c->subs);
            if (c->closed) kfree(c->closed);
            c->pts = sp; c->npts = sn; c->cap = scap; c->subs = ssub; c->closed = scl; c->nsubs = sns; c->subcap = sscap;
            c->cx = scx; c->cy = scy; c->have_cur = shc;
        } else {
            edges_t E;
            memset(&E, 0, sizeof(E));
            rect_edges(c, &E, a, b, cc, d);
            if (op == OP_FILLRECT) fill_edges(c, &E, &c->st.fill, 0);
            else raster(c, &E, 0, clear_span, NULL);
            edges_free(&E);
        }
        draws = 1;
        break;
    }
    case OP_BEGIN: c->npts = c->nsubs = 0; c->have_cur = 0; break;
    case OP_MOVE: if (finite_(a) && finite_(b)) move_to(c, a, b); break;
    case OP_LINE: if (finite_(a) && finite_(b)) line_to(c, a, b); break;
    case OP_CLOSE: close_path(c); break;
    case OP_RECT:
        if (finite_(a) && finite_(b) && finite_(cc) && finite_(d)) {
            move_to(c, a, b); line_to(c, a + cc, b); line_to(c, a + cc, b + d); line_to(c, a, b + d); close_path(c);
        }
        break;
    case OP_ROUNDRECT: {
        double r = e < 0 ? 0 : e;
        if (r > fabs_(cc) / 2) r = fabs_(cc) / 2;
        if (r > fabs_(d) / 2) r = fabs_(d) / 2;
        if (!finite_(a) || !finite_(b) || !finite_(cc) || !finite_(d)) break;
        move_to(c, a + r, b);
        line_to(c, a + cc - r, b);
        ellipse(c, a + cc - r, b + r, r, r, 0, -PI / 2, 0, 0);
        line_to(c, a + cc, b + d - r);
        ellipse(c, a + cc - r, b + d - r, r, r, 0, 0, PI / 2, 0);
        line_to(c, a + r, b + d);
        ellipse(c, a + r, b + d - r, r, r, 0, PI / 2, PI, 0);
        line_to(c, a, b + r);
        ellipse(c, a + r, b + r, r, r, 0, PI, 3 * PI / 2, 0);
        close_path(c);
        break;
    }
    case OP_ARC: if (finite_(a) && finite_(b) && finite_(cc)) {
        if (cc < 0) { script_throw(I, "IndexSizeError: negative radius"); return v_undef(); }
        ellipse(c, a, b, cc, cc, 0, d, e, (int)f);
    } break;
    case OP_ELLIPSE: if (finite_(a) && finite_(b)) {
        double g = num(I, argc, argv, 8);
        int ccw = (int)num(I, argc, argv, 9);
        if (cc < 0 || d < 0) { script_throw(I, "IndexSizeError: negative radius"); return v_undef(); }
        ellipse(c, a, b, cc, d, e, f, g, ccw);
    } break;
    case OP_ARCTO: arc_to(c, a, b, cc, d, e); break;
    case OP_QUAD: quad_to(c, a, b, cc, d); break;
    case OP_CUBIC: cubic_to(c, a, b, cc, d, e, f); break;
    case OP_FILL: fill_path(c, (int)a); draws = 1; break;
    case OP_STROKE: stroke_path(c); draws = 1; break;
    case OP_CLIP: clip_path(c, (int)a); break;
    case OP_SAVE:
        if (c->nst < CV_STACK) {
            c->stack[c->nst] = c->st;
            if (c->st.clip) {                          /* each level its own mask */
                size_t sz = (size_t)c->w * (size_t)c->h;
                uint8_t* m = kmalloc(sz);
                if (m) memcpy(m, c->st.clip, sz);
                c->stack[c->nst].clip = m;
            }
            c->nst++;
        }
        break;
    case OP_RESTORE:
        if (c->nst > 0) {
            if (c->st.clip) kfree(c->st.clip);
            c->st = c->stack[--c->nst];
        }
        break;
    case OP_RESET:
        resize(c, c->w, c->h);
        draws = 1;
        break;
    case OP_TRANSFORM: {
        mat_t m = c->st.m, r;
        r.a = m.a * a + m.c * b; r.b = m.b * a + m.d * b;
        r.c = m.a * cc + m.c * d; r.d = m.b * cc + m.d * d;
        r.e = m.a * e + m.c * f + m.e; r.f = m.b * e + m.d * f + m.f;
        if (finite_(r.a) && finite_(r.b) && finite_(r.c) && finite_(r.d) && finite_(r.e) && finite_(r.f)) c->st.m = r;
        break;
    }
    case OP_SETTRANSFORM:
        if (finite_(a) && finite_(b) && finite_(cc) && finite_(d) && finite_(e) && finite_(f)) {
            c->st.m.a = a; c->st.m.b = b; c->st.m.c = cc; c->st.m.d = d; c->st.m.e = e; c->st.m.f = f;
        }
        break;
    case OP_RESET_TRANSFORM: memset(&c->st.m, 0, sizeof(c->st.m)); c->st.m.a = c->st.m.d = 1; break;
    case OP_GETTRANSFORM: {
        obj_t* o = obj_new(I, OBJ_PLAIN);
        obj_set(I, o, "a", v_num(c->st.m.a)); obj_set(I, o, "b", v_num(c->st.m.b));
        obj_set(I, o, "c", v_num(c->st.m.c)); obj_set(I, o, "d", v_num(c->st.m.d));
        obj_set(I, o, "e", v_num(c->st.m.e)); obj_set(I, o, "f", v_num(c->st.m.f));
        return v_obj(o);
    }
    case OP_STYLE: {                                   /* (which, kind, color string | gradient id) */
        paint_t* s = style_slot(c, (int)a);
        int kind = (int)b;
        if (kind == PAINT_COLOR) {
            int ok;
            uint32_t col = css_color_blend(argc > 4 ? v_cstr(I, argv[4]) : "", &ok);
            if (!ok) return v_bool(0);
            s->kind = PAINT_COLOR;
            s->color = ok == 2 ? 0xFF000000u : col;
        } else { s->kind = kind; s->id = (int)cc; }
        return v_bool(1);
    }
    case OP_PATTERN: {                                 /* (image element, repeat x, repeat y) -> id */
        dom_node_t* src = argc > 2 ? jsdom_node_of(argv[2]) : NULL;
        const uint32_t* spx; const uint8_t* sal; int sw, sh;
        if (!source_of(p, src, &spx, &sal, &sw, &sh)) return v_num(-1);
        for (int i = 0; i < CV_GRADS; i++) {
            grad_t* g = &g_grads[i];
            if (g->used) continue;
            memset(g, 0, sizeof(*g));
            g->used = 1;
            g->pw = sw; g->ph = sh; g->rep_x = (int)b; g->rep_y = (int)cc;
            if (!strcmp(src->tag, "canvas")) {             /* a copy: the canvas may change */
                g->own_px = kmalloc((size_t)sw * sh * 4);
                g->own_al = kmalloc((size_t)sw * sh);
                if (!g->own_px || !g->own_al) { g->used = 0; return v_num(-1); }
                memcpy(g->own_px, spx, (size_t)sw * sh * 4);
                memcpy(g->own_al, sal, (size_t)sw * sh);
            } else g->img = src->img;
            return v_num(i);
        }
        return v_num(-1);
    }
    case OP_LINEWIDTH: if (finite_(a) && a > 0) c->st.lw = a; break;
    case OP_CAP: c->st.cap = (int)a; break;
    case OP_JOIN: c->st.join = (int)a; break;
    case OP_MITER: if (finite_(a) && a > 0) c->st.miter = a; break;
    case OP_ALPHA: if (finite_(a) && a >= 0 && a <= 1) c->st.alpha = a; break;
    case OP_COMPOSITE: c->st.op = (int)a; break;
    case OP_FONT: if (argc > 2) set_font(&c->st, v_cstr(I, argv[2])); break;
    case OP_ALIGN: c->st.align = (int)a; break;
    case OP_BASELINE: c->st.baseline = (int)a; break;
    case OP_SMOOTH: c->st.smooth = (int)a; break;
    case OP_SHADOW_COLOR: {
        int ok;
        uint32_t col = css_color_blend(argc > 2 ? v_cstr(I, argv[2]) : "", &ok);
        if (!ok) return v_bool(0);
        c->st.shadow_color = ok == 2 ? 0xFF000000u : col;
        return v_bool(1);
    }
    case OP_SHADOW:
        if (!finite_(b)) break;
        if ((int)a == 0) { if (b >= 0) c->st.shadow_blur = b; }
        else if ((int)a == 1) c->st.shadow_x = b;
        else c->st.shadow_y = b;
        break;
    case OP_FILLTEXT: case OP_STROKETEXT:
        if (argc > 4) {
            const char* t = v_cstr(I, argv[2]);
            double maxw = argc > 5 && argv[5].t != V_UNDEF ? num(I, argc, argv, 5) : -1;
            draw_text(c, t, num(I, argc, argv, 3), num(I, argc, argv, 4), maxw, op == OP_FILLTEXT ? &c->st.fill : &c->st.stroke);
            draws = 1;
        }
        break;
    case OP_MEASURE: {
        int asc, desc, lh;
        font_metrics(face_of(&c->st), c->st.font_px, &asc, &desc, &lh);
        obj_t* o = obj_new(I, OBJ_PLAIN);
        double w = argc > 2 ? text_width(&c->st, v_cstr(I, argv[2])) : 0;
        obj_set(I, o, "width", v_num(w));
        obj_set(I, o, "actualBoundingBoxAscent", v_num(asc));
        obj_set(I, o, "actualBoundingBoxDescent", v_num(desc));
        obj_set(I, o, "fontBoundingBoxAscent", v_num(asc));
        obj_set(I, o, "fontBoundingBoxDescent", v_num(desc));
        obj_set(I, o, "actualBoundingBoxLeft", v_num(0));
        obj_set(I, o, "actualBoundingBoxRight", v_num(w));
        return v_obj(o);
    }
    case OP_DRAWIMAGE: {                               /* (src, nargs, a..h) */
        dom_node_t* src = argc > 2 ? jsdom_node_of(argv[2]) : NULL;
        const uint32_t* spx; const uint8_t* sal; int sw, sh;
        if (!source_of(p, src, &spx, &sal, &sw, &sh)) return v_bool(0);
        int na = (int)num(I, argc, argv, 3);
        double v[8];
        for (int i = 0; i < 8; i++) v[i] = num(I, argc, argv, 4 + i);
        if (na == 2) draw_image(c, spx, sal, sw, sh, 0, 0, sw, sh, v[0], v[1], sw, sh);
        else if (na == 4) draw_image(c, spx, sal, sw, sh, 0, 0, sw, sh, v[0], v[1], v[2], v[3]);
        else if (na == 8) draw_image(c, spx, sal, sw, sh, v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]);
        draws = 1;
        return v_bool(1);
    }
    case OP_GETDATA: {                                 /* (x, y, w, h) -> base64 RGBA */
        int x0 = (int)a, y0 = (int)b, w = (int)cc, h = (int)d;
        if (w <= 0 || h <= 0 || (uint64_t)w * h > CV_MAX_PIXELS) { script_throw(I, "IndexSizeError: bad size"); return v_undef(); }
        uint8_t* buf = kzalloc((size_t)w * h * 4);
        if (!buf) return v_null();
        for (int y = 0; y < h; y++) {
            int sy = y0 + y;
            if (sy < 0 || sy >= c->h) continue;
            for (int x = 0; x < w; x++) {
                int sx = x0 + x;
                if (sx < 0 || sx >= c->w) continue;
                size_t i = (size_t)sy * c->w + sx, o = ((size_t)y * w + x) * 4;
                uint32_t px = c->img.px[i];
                int al = c->img.alpha[i];
                buf[o] = al ? (uint8_t)(px >> 16) : 0; buf[o + 1] = al ? (uint8_t)(px >> 8) : 0; buf[o + 2] = al ? (uint8_t)px : 0;
                buf[o + 3] = (uint8_t)al;
            }
        }
        size_t len;
        char* s = b64_encode(buf, (size_t)w * h * 4, &len);
        kfree(buf);
        if (!s) return v_null();
        value_t r = v_strn(I, s, (uint32_t)len);
        kfree(s);
        return r;
    }
    case OP_PUTDATA: {                                 /* (hex RGBA, w, h, dx, dy, sx, sy, sw, sh) */
        if (argc < 5) return v_undef();
        size_t n2 = 0;
        const char* hex = v_cstr(I, argv[2]);
        size_t hl = strlen(hex);
        uint8_t* dat = kmalloc(hl / 2 + 1);
        if (!dat) return v_undef();
        for (size_t i = 0; i + 1 < hl; i += 2) {
            int hi = hex[i] <= '9' ? hex[i] - '0' : (hex[i] | 32) - 'a' + 10;
            int lo = hex[i + 1] <= '9' ? hex[i + 1] - '0' : (hex[i + 1] | 32) - 'a' + 10;
            dat[n2++] = (uint8_t)(hi << 4 | lo);
        }
        int w = (int)num(I, argc, argv, 3), h = (int)num(I, argc, argv, 4);
        int dx = (int)num(I, argc, argv, 5), dy = (int)num(I, argc, argv, 6);
        int sx = (int)num(I, argc, argv, 7), sy = (int)num(I, argc, argv, 8);
        int sw = argc > 9 ? (int)num(I, argc, argv, 9) : w, sh = argc > 10 ? (int)num(I, argc, argv, 10) : h;
        if ((size_t)w * h * 4 <= n2) {
            for (int y = sy; y < sy + sh && y < h; y++) {
                if (y < 0) continue;
                int ty = dy + y;
                if (ty < 0 || ty >= c->h) continue;
                for (int x = sx; x < sx + sw && x < w; x++) {
                    if (x < 0) continue;
                    int tx = dx + x;
                    if (tx < 0 || tx >= c->w) continue;
                    const uint8_t* q = dat + ((size_t)y * w + x) * 4;
                    size_t i = (size_t)ty * c->w + tx;
                    c->img.px[i] = (uint32_t)q[0] << 16 | (uint32_t)q[1] << 8 | q[2];
                    c->img.alpha[i] = q[3];
                }
            }
        }
        kfree(dat);
        draws = 1;
        break;
    }
    case OP_DATAURL: {
        size_t n2, len;
        uint8_t* png = c->w && c->h ? png_encode(c, &n2) : NULL;
        if (!png) return v_str(I, "data:,");
        char* s = b64_encode(png, n2, &len);
        kfree(png);
        if (!s) return v_str(I, "data:,");
        char* full = kmalloc(len + 32);
        value_t r = v_str(I, "data:,");
        if (full) {
            memcpy(full, "data:image/png;base64,", 22);
            memcpy(full + 22, s, len + 1);
            r = v_strn(I, full, (uint32_t)(len + 22));
            kfree(full);
        }
        kfree(s);
        return r;
    }
    case OP_DASH: {                                    /* (n, values...) */
        int nd = (int)a;
        if (nd < 0 || nd > 16) break;
        int ok = 1;
        double v[16];
        for (int i = 0; i < nd; i++) { v[i] = num(I, argc, argv, 3 + i); if (!finite_(v[i]) || v[i] < 0) ok = 0; }
        if (!ok) break;
        c->st.ndash = nd;
        for (int i = 0; i < nd; i++) c->st.dash[i] = v[i];
        if (nd & 1) {                                  /* an odd list repeats */
            for (int i = 0; i < nd && nd + i < 16; i++) c->st.dash[nd + i] = v[i];
            c->st.ndash = nd * 2 > 16 ? 16 : nd * 2;
        }
        int allzero = 1;
        for (int i = 0; i < c->st.ndash; i++) if (c->st.dash[i] > 0) allzero = 0;
        if (allzero) c->st.ndash = 0;
        break;
    }
    case OP_DASHOFF: if (finite_(a)) c->st.dash_off = a; break;
    case OP_INPATH: case OP_INSTROKE: {                /* (x, y, evenodd) in device space after the transform */
        edges_t E;
        memset(&E, 0, sizeof(E));
        if (op == OP_INPATH) path_edges(c, &E);
        else {
            double scale = mat_scale(&c->st.m), hw = c->st.lw * scale / 2;
            for (int s = 0; s < c->nsubs; s++) {
                int s0 = c->subs[s], s1 = s + 1 < c->nsubs ? c->subs[s + 1] : c->npts;
                stroke_line(c, &E, c->pts + s0, s1 - s0, c->closed[s] && s1 - s0 > 2, hw);
            }
        }
        /* (a, b) are already canvas coordinates for isPointInPath: not transformed */
        int wind = 0, cross = 0;
        for (int i = 0; i < E.n; i++) {
            const edge_t* ed = &E.e[i];
            if (b < ed->y0 || b >= ed->y1) continue;
            double x = ed->x0 + (ed->x1 - ed->x0) * (b - ed->y0) / (ed->y1 - ed->y0);
            if (x > a) { wind += ed->dir; cross++; }
        }
        edges_free(&E);
        return v_bool(op == OP_INPATH && (int)cc ? (cross & 1) : wind != 0);
    }
    default: break;
    }
    if (draws) p->canvas_dirty = 1;
    return v_undef();
}

void canvas_install(interp_t* I) {
    script_def_global(I, "__cv", v_native(I, "__cv", cv_call));
}
#endif
