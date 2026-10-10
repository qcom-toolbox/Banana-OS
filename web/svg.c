#include "svg.h"
#include "html.h"
#include "css.h"
#include "kstring.h"

/*
 * A small SVG renderer for logos and icons: <svg> with viewBox / width /
 * height, <g> (transforms, inherited paint), <path> (M L H V C S Q T A Z,
 * absolute and relative), <rect> (rx), <circle>, <ellipse>, <line>,
 * <polyline>, <polygon> and <use>; fill / stroke / stroke-width / opacity
 * from attributes, style="" and simple class / tag rules in <style>;
 * nonzero and evenodd fill rules; gradients are drawn in their first
 * stop's colour. Shapes are flattened to line segments and scan-converted
 * with 4x4 supersampling into an image with an alpha channel.
 * Not supported: text, masks, clip paths, patterns, filters.
 */

typedef long double real;

static real r_sqrt(real x) { if (x <= 0) return 0; real r; __asm__("fsqrt" : "=t"(r) : "0"(x)); return r; }
static real r_sin(real x) { real r; __asm__("fsin" : "=t"(r) : "0"(x)); return r; }
static real r_cos(real x) { real r; __asm__("fcos" : "=t"(r) : "0"(x)); return r; }
static real r_atan2(real y, real x) { real r; __asm__("fpatan" : "=t"(r) : "0"(x), "u"(y) : "st(1)"); return r; }
static real r_abs(real x) { return x < 0 ? -x : x; }
#define R_PI 3.14159265358979323846L

/* ── output image and rasterizer ── */

#define SS 4                       /* subsamples per pixel in each direction */
#define MAX_SEGS 8000

typedef struct { real x0, y0, x1, y1; } seg_t;

typedef struct {
    int       w, h;
    uint32_t* rgb;                 /* 0x00RRGGBB */
    uint8_t*  a;                   /* coverage 0..255 */
    seg_t*    segs;                /* the shape being built */
    int       nsegs;
    uint8_t*  cov;                 /* one row: 0..SS*SS per pixel */
    real      cx, cy, sx0, sy0;    /* path pen and subpath start (user space) */
    real      m[6];                /* current transform: x' = a x + c y + e, y' = b x + d y + f */
    arena_t*  A;
    dom_node_t* root;
    /* <style> rules: ".cls { decls }" / "tag { decls }" */
    struct { const char* sel; const char* decls; } rules[64];
    int       nrules;
    int       keep_flat;           /* add_seg() keeps horizontal segments (a stroke's outline) */
    /* scratch space of fill_segs() and make_stroke() - per drawing, not
     * static: two tasks may draw at once, preempted in between */
    real*     xs;
    int*      dirs;
    seg_t*    tmp;
} rc_t;

static void xform(const rc_t* R, real x, real y, real* ox, real* oy) {
    *ox = R->m[0] * x + R->m[2] * y + R->m[4];
    *oy = R->m[1] * x + R->m[3] * y + R->m[5];
}

/* add_seg() drops horizontal segments, which a fill does not need but a
 * stroke does: shapes that are stroked collect their outline with
 * R->keep_flat on. (No mutable globals in this file: it is preemptible
 * kernel code - see kernel/task.h - and two tasks may draw at once.) */
static void add_seg(rc_t* R, real x0, real y0, real x1, real y1) {
    if (R->nsegs >= MAX_SEGS || (y0 == y1 && !R->keep_flat)) return;
    seg_t* s = &R->segs[R->nsegs++];
    s->x0 = x0; s->y0 = y0; s->x1 = x1; s->y1 = y1;
}

/* a line in user space, transformed to device space */
static void user_line(rc_t* R, real x0, real y0, real x1, real y1) {
    real a, b, c, d;
    xform(R, x0, y0, &a, &b);
    xform(R, x1, y1, &c, &d);
    add_seg(R, a, b, c, d);
}

/* fills the collected segments with colour / opacity, then clears them */
static void fill_segs(rc_t* R, uint32_t color, real opacity, int evenodd) {
    if (!R->nsegs || opacity <= 0) { R->nsegs = 0; return; }
    real ymin = 1e30L, ymax = -1e30L;
    for (int i = 0; i < R->nsegs; i++) {
        seg_t* s = &R->segs[i];
        if (s->y0 < ymin) ymin = s->y0;
        if (s->y1 < ymin) ymin = s->y1;
        if (s->y0 > ymax) ymax = s->y0;
        if (s->y1 > ymax) ymax = s->y1;
    }
    int row0 = ymin < 0 ? 0 : (int)ymin, row1 = ymax >= R->h ? R->h - 1 : (int)ymax;
    real* xs = R->xs;
    int* dirs = R->dirs;
    uint32_t cr = (color >> 16) & 255, cg = (color >> 8) & 255, cb = color & 255;
    int op = (int)(opacity * 255);
    if (op > 255) op = 255;
    for (int row = row0; row <= row1; row++) {
        memset(R->cov, 0, (size_t)R->w);
        int any = 0;
        for (int sub = 0; sub < SS; sub++) {
            real sy = row + (sub + 0.5L) / SS;
            int n = 0;
            for (int i = 0; i < R->nsegs && n < 2048; i++) {
                seg_t* s = &R->segs[i];
                real ya = s->y0, yb = s->y1;
                int dir = 1;
                if (ya > yb) { real t = ya; ya = yb; yb = t; dir = -1; }
                if (sy < ya || sy >= yb) continue;
                real t = (sy - s->y0) / (s->y1 - s->y0);
                xs[n] = s->x0 + t * (s->x1 - s->x0);
                dirs[n] = dir;
                n++;
            }
            /* sort the crossings (insertion: there are few) */
            for (int i = 1; i < n; i++) {
                real x = xs[i];
                int d = dirs[i], j = i;
                while (j > 0 && xs[j - 1] > x) { xs[j] = xs[j - 1]; dirs[j] = dirs[j - 1]; j--; }
                xs[j] = x;
                dirs[j] = d;
            }
            int wind = 0;
            for (int i = 0; i + 1 < n; i++) {
                wind += dirs[i];
                int inside = evenodd ? (i & 1) == 0 : wind != 0;
                if (!inside) continue;
                real xa = xs[i], xb = xs[i + 1];
                if (xb <= 0 || xa >= R->w) continue;
                if (xa < 0) xa = 0;
                if (xb > R->w) xb = R->w;
                /* coverage in SS horizontal steps */
                int sa = (int)(xa * SS + 0.5L), sb = (int)(xb * SS + 0.5L);
                for (int sx = sa; sx < sb; ) {
                    int px = sx / SS;
                    int end = (px + 1) * SS;
                    if (end > sb) end = sb;
                    R->cov[px] = (uint8_t)(R->cov[px] + (end - sx));
                    sx = end;
                    any = 1;
                }
            }
        }
        if (!any) continue;
        for (int x = 0; x < R->w; x++) {
            int c = R->cov[x];
            if (!c) continue;
            int alpha = c * op / (SS * SS);                  /* 0..255 */
            if (alpha > 255) alpha = 255;
            int i = row * R->w + x;
            int da = R->a[i];
            uint32_t d = R->rgb[i];
            /* "over" onto what is there (straight alpha) */
            int oa = alpha + da * (255 - alpha) / 255;
            if (!oa) continue;
            int r = ((int)cr * alpha + (int)((d >> 16) & 255) * da * (255 - alpha) / 255) / oa;
            int g = ((int)cg * alpha + (int)((d >> 8) & 255) * da * (255 - alpha) / 255) / oa;
            int b = ((int)cb * alpha + (int)(d & 255) * da * (255 - alpha) / 255) / oa;
            R->rgb[i] = (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b;
            R->a[i] = (uint8_t)oa;
        }
    }
    R->nsegs = 0;
}

/* ── paths (user space, flattened) ── */

static void bezier3(rc_t* R, real x0, real y0, real x1, real y1, real x2, real y2, real x3, real y3) {
    real len = r_abs(x1 - x0) + r_abs(y1 - y0) + r_abs(x2 - x1) + r_abs(y2 - y1) + r_abs(x3 - x2) + r_abs(y3 - y2);
    real scale = r_sqrt(r_abs(R->m[0] * R->m[3] - R->m[1] * R->m[2]));
    int n = (int)(len * scale / 3);
    if (n < 4) n = 4;
    if (n > 48) n = 48;
    real px = x0, py = y0;
    for (int i = 1; i <= n; i++) {
        real t = (real)i / n, u = 1 - t;
        real x = u * u * u * x0 + 3 * u * u * t * x1 + 3 * u * t * t * x2 + t * t * t * x3;
        real y = u * u * u * y0 + 3 * u * u * t * y1 + 3 * u * t * t * y2 + t * t * t * y3;
        user_line(R, px, py, x, y);
        px = x; py = y;
    }
}

static void arc_to(rc_t* R, real x0, real y0, real rx, real ry, real phi_deg, int large, int sweep, real x1, real y1) {
    if (rx == 0 || ry == 0) { user_line(R, x0, y0, x1, y1); return; }
    rx = r_abs(rx); ry = r_abs(ry);
    real phi = phi_deg * R_PI / 180, cp = r_cos(phi), sp = r_sin(phi);
    real dx = (x0 - x1) / 2, dy = (y0 - y1) / 2;
    real x1p = cp * dx + sp * dy, y1p = -sp * dx + cp * dy;
    real lam = x1p * x1p / (rx * rx) + y1p * y1p / (ry * ry);
    if (lam > 1) { real s = r_sqrt(lam); rx *= s; ry *= s; }
    real num = rx * rx * ry * ry - rx * rx * y1p * y1p - ry * ry * x1p * x1p;
    real den = rx * rx * y1p * y1p + ry * ry * x1p * x1p;
    real co = den > 0 && num > 0 ? r_sqrt(num / den) : 0;
    if (large == sweep) co = -co;
    real cxp = co * rx * y1p / ry, cyp = -co * ry * x1p / rx;
    real cx = cp * cxp - sp * cyp + (x0 + x1) / 2, cy = sp * cxp + cp * cyp + (y0 + y1) / 2;
    real th1 = r_atan2((y1p - cyp) / ry, (x1p - cxp) / rx);
    real th2 = r_atan2((-y1p - cyp) / ry, (-x1p - cxp) / rx);
    real dth = th2 - th1;
    if (sweep && dth < 0) dth += 2 * R_PI;
    if (!sweep && dth > 0) dth -= 2 * R_PI;
    int n = (int)(r_abs(dth) * 8) + 2;
    if (n > 64) n = 64;
    real px = x0, py = y0;
    for (int i = 1; i <= n; i++) {
        real t = th1 + dth * i / n;
        real x = cx + rx * r_cos(t) * cp - ry * r_sin(t) * sp;
        real y = cy + rx * r_cos(t) * sp + ry * r_sin(t) * cp;
        if (i == n) { x = x1; y = y1; }
        user_line(R, px, py, x, y);
        px = x; py = y;
    }
}

/* number parsing in path data and attributes */
static const char* skip_sep(const char* p) { while (*p == ' ' || *p == ',' || *p == '\n' || *p == '\t' || *p == '\r') p++; return p; }

static int read_num(const char** pp, real* out) {
    const char* p = skip_sep(*pp);
    int neg = 0;
    if (*p == '-') { neg = 1; p++; } else if (*p == '+') p++;
    if (!((*p >= '0' && *p <= '9') || *p == '.')) return 0;
    real v = 0;
    while (*p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
    if (*p == '.') {
        p++;
        real f = 0.1L;
        while (*p >= '0' && *p <= '9') { v += (*p++ - '0') * f; f /= 10; }
    }
    if (*p == 'e' || *p == 'E') {
        const char* q = p + 1;
        int en = 0, e = 0;
        if (*q == '-') { en = 1; q++; } else if (*q == '+') q++;
        if (*q >= '0' && *q <= '9') {
            while (*q >= '0' && *q <= '9') e = e * 10 + (*q++ - '0');
            while (e-- > 0) v = en ? v / 10 : v * 10;
            p = q;
        }
    }
    *out = neg ? -v : v;
    *pp = p;
    return 1;
}

static int read_flag(const char** pp, int* out) {
    const char* p = skip_sep(*pp);
    if (*p != '0' && *p != '1') return 0;
    *out = *p == '1';
    *pp = p + 1;
    return 1;
}

static void path_data(rc_t* R, const char* d) {
    const char* p = d;
    char cmd = 0;
    real cx = 0, cy = 0, sx = 0, sy = 0, lcx = 0, lcy = 0;   /* pen, subpath start, last control point */
    char last = 0;
    for (;;) {
        p = skip_sep(p);
        if (!*p) break;
        if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z')) cmd = *p++;
        else if (!cmd) break;
        int rel = cmd >= 'a';
        char C = (char)(rel ? cmd - 32 : cmd);
        real a[7];
        if (C == 'Z') {
            user_line(R, cx, cy, sx, sy);
            cx = sx; cy = sy;
            last = 'Z';
            cmd = 0;
            continue;
        }
        int need = C == 'M' || C == 'L' || C == 'T' ? 2 : C == 'H' || C == 'V' ? 1 : C == 'C' ? 6 : C == 'S' || C == 'Q' ? 4 : C == 'A' ? 7 : 0;
        if (!need) break;
        int ok = 1;
        for (int i = 0; i < need && ok; i++) {
            if (C == 'A' && (i == 3 || i == 4)) { int f; ok = read_flag(&p, &f); a[i] = f; }
            else ok = read_num(&p, &a[i]);
        }
        if (!ok) break;
        real ox = rel ? cx : 0, oy = rel ? cy : 0;
        switch (C) {
        case 'M':
            cx = a[0] + ox; cy = a[1] + oy;
            sx = cx; sy = cy;
            cmd = rel ? 'l' : 'L';                         /* further pairs are lines */
            break;
        case 'L': user_line(R, cx, cy, a[0] + ox, a[1] + oy); cx = a[0] + ox; cy = a[1] + oy; break;
        case 'H': { real nx = a[0] + (rel ? cx : 0); user_line(R, cx, cy, nx, cy); cx = nx; break; }
        case 'V': { real ny = a[0] + (rel ? cy : 0); user_line(R, cx, cy, cx, ny); cy = ny; break; }
        case 'C':
            bezier3(R, cx, cy, a[0] + ox, a[1] + oy, a[2] + ox, a[3] + oy, a[4] + ox, a[5] + oy);
            lcx = a[2] + ox; lcy = a[3] + oy;
            cx = a[4] + ox; cy = a[5] + oy;
            break;
        case 'S': {
            real x1 = cx, y1 = cy;
            if (last == 'C' || last == 'S') { x1 = 2 * cx - lcx; y1 = 2 * cy - lcy; }
            bezier3(R, cx, cy, x1, y1, a[0] + ox, a[1] + oy, a[2] + ox, a[3] + oy);
            lcx = a[0] + ox; lcy = a[1] + oy;
            cx = a[2] + ox; cy = a[3] + oy;
            break;
        }
        case 'Q': case 'T': {
            real qx, qy, ex, ey;
            if (C == 'Q') { qx = a[0] + ox; qy = a[1] + oy; ex = a[2] + ox; ey = a[3] + oy; }
            else {
                qx = (last == 'Q' || last == 'T') ? 2 * cx - lcx : cx;
                qy = (last == 'Q' || last == 'T') ? 2 * cy - lcy : cy;
                ex = a[0] + ox; ey = a[1] + oy;
            }
            /* a quadratic as a cubic */
            bezier3(R, cx, cy, cx + 2 * (qx - cx) / 3, cy + 2 * (qy - cy) / 3, ex + 2 * (qx - ex) / 3, ey + 2 * (qy - ey) / 3, ex, ey);
            lcx = qx; lcy = qy;
            cx = ex; cy = ey;
            break;
        }
        case 'A':
            arc_to(R, cx, cy, a[0], a[1], a[2], (int)a[3], (int)a[4], a[5] + ox, a[6] + oy);
            cx = a[5] + ox; cy = a[6] + oy;
            break;
        }
        last = C;
    }
}

/* ── strokes: each segment as a thick quad, round-ish joins ── */

/* a closed polygon, always added with the same orientation: the pieces of
 * a stroke overlap, and with the nonzero rule they must add up, not cancel */
static void add_poly(rc_t* R, const real* xs, const real* ys, int n) {
    real area = 0;
    for (int i = 0; i < n; i++) { int j = (i + 1) % n; area += xs[i] * ys[j] - xs[j] * ys[i]; }
    for (int i = 0; i < n; i++) {
        int a = area >= 0 ? i : n - 1 - i, b = area >= 0 ? (i + 1) % n : (n - 2 - i + n) % n;
        add_seg(R, xs[a], ys[a], xs[b], ys[b]);
    }
}

static void stroke_quad(rc_t* R, real x0, real y0, real x1, real y1, real hw) {
    real dx = x1 - x0, dy = y1 - y0, len = r_sqrt(dx * dx + dy * dy);
    if (len <= 0) return;
    real nx = -dy / len * hw, ny = dx / len * hw;
    real xs[4] = { x0 + nx, x1 + nx, x1 - nx, x0 - nx }, ys[4] = { y0 + ny, y1 + ny, y1 - ny, y0 - ny };
    add_poly(R, xs, ys, 4);
}

static void stroke_dot(rc_t* R, real x, real y, real hw) {
    real xs[8], ys[8];
    for (int i = 0; i < 8; i++) { real t = i * R_PI / 4; xs[i] = x + hw * r_cos(t); ys[i] = y + hw * r_sin(t); }
    add_poly(R, xs, ys, 8);
}

/* the segments of the current outline become a stroke outline (device space) */
static void make_stroke(rc_t* R, int start, real width) {
    real scale = r_sqrt(r_abs(R->m[0] * R->m[3] - R->m[1] * R->m[2]));
    real hw = width * scale / 2;
    if (hw < 0.35L) hw = 0.35L;
    int end = R->nsegs;
    seg_t* tmp = R->tmp;
    int n = end - start;
    if (n > MAX_SEGS) n = MAX_SEGS;
    memcpy(tmp, R->segs + start, (size_t)n * sizeof(seg_t));
    R->nsegs = start;
    for (int i = 0; i < n; i++) {
        stroke_quad(R, tmp[i].x0, tmp[i].y0, tmp[i].x1, tmp[i].y1, hw);
        if (hw > 1) stroke_dot(R, tmp[i].x1, tmp[i].y1, hw);
    }
}


/* ── paint ── */

typedef struct {
    uint32_t fill, stroke;
    int      fill_none, stroke_none;
    real     stroke_w, opacity, fill_op, stroke_op;
    int      evenodd, hidden;
    uint32_t color;                /* currentColor */
} paint_t;

static int parse_color(rc_t* R, const char* v, const paint_t* ps, uint32_t* out, int* none) {
    while (*v == ' ') v++;
    if (!*v) return 0;
    if (strncasecmp(v, "none", 4) == 0 || strncasecmp(v, "transparent", 11) == 0) { *none = 1; return 1; }
    if (strncasecmp(v, "currentcolor", 12) == 0) { *out = ps->color; *none = 0; return 1; }
    if (strncmp(v, "url(", 4) == 0) {
        /* a gradient or pattern: its first stop's colour */
        const char* h = strchr(v, '#');
        if (h) {
            char id[64];
            int n = 0;
            h++;
            while (*h && *h != ')' && *h != '"' && *h != '\'' && n < 63) id[n++] = *h++;
            id[n] = 0;
            dom_node_t* g = dom_find_id(R->root, id);
            for (int depth = 0; g && depth < 4; depth++) {
                for (dom_node_t* s = g->first; s; s = s->next) {
                    if (s->type != DOM_ELEM || strcmp(s->tag, "stop") != 0) continue;
                    const char* sc = dom_attr(s, "stop-color");
                    const char* st = dom_attr(s, "style");
                    char buf[48];
                    if (!sc && st && (sc = strstr(st, "stop-color:"))) {
                        sc += 11;
                        int k = 0;
                        while (*sc && *sc != ';' && k < 47) buf[k++] = *sc++;
                        buf[k] = 0;
                        sc = buf;
                    }
                    int ok;
                    uint32_t c = css_color(sc ? sc : "black", &ok);
                    *out = ok ? c : 0;
                    *none = 0;
                    return 1;
                }
                /* href="#other": the stops are there */
                const char* ref = dom_attr(g, "href");
                if (!ref) ref = dom_attr(g, "xlink:href");
                g = ref && ref[0] == '#' ? dom_find_id(R->root, ref + 1) : NULL;
            }
        }
        *out = 0;
        *none = 0;
        return 1;
    }
    int ok;
    uint32_t c = css_color(v, &ok);
    if (!ok) return 0;
    *out = c;
    *none = 0;
    return 1;
}

static real parse_real(const char* v, real def) {
    real r;
    const char* p = v;
    if (!v || !read_num(&p, &r)) return def;
    if (*p == '%') r /= 100;
    return r;
}

/* one "prop: value" for the paint */
static void paint_prop(rc_t* R, paint_t* ps, const char* k, int kl, const char* v) {
    char key[24];
    if (kl > 23) kl = 23;
    memcpy(key, k, (size_t)kl);
    key[kl] = 0;
    char val[96];
    int n = 0;
    while (*v == ' ') v++;
    while (*v && *v != ';' && n < 95) val[n++] = *v++;
    while (n && val[n - 1] == ' ') n--;
    val[n] = 0;
    if (strcmp(key, "fill") == 0) parse_color(R, val, ps, &ps->fill, &ps->fill_none);
    else if (strcmp(key, "stroke") == 0) parse_color(R, val, ps, &ps->stroke, &ps->stroke_none);
    else if (strcmp(key, "stroke-width") == 0) ps->stroke_w = parse_real(val, ps->stroke_w);
    else if (strcmp(key, "opacity") == 0) ps->opacity *= parse_real(val, 1);
    else if (strcmp(key, "fill-opacity") == 0) ps->fill_op = parse_real(val, 1);
    else if (strcmp(key, "stroke-opacity") == 0) ps->stroke_op = parse_real(val, 1);
    else if (strcmp(key, "fill-rule") == 0) ps->evenodd = strcmp(val, "evenodd") == 0;
    else if (strcmp(key, "color") == 0) { int none; parse_color(R, val, ps, &ps->color, &none); }
    else if (strcmp(key, "display") == 0) { if (strcmp(val, "none") == 0) ps->hidden = 1; }
    else if (strcmp(key, "visibility") == 0) { if (strcmp(val, "hidden") == 0) ps->hidden = 1; }
}

static void paint_decls(rc_t* R, paint_t* ps, const char* s) {
    while (s && *s) {
        while (*s == ' ' || *s == ';' || *s == '\n') s++;
        const char* k = s;
        while (*s && *s != ':' && *s != ';') s++;
        if (*s != ':') break;
        int kl = (int)(s - k);
        while (kl && k[kl - 1] == ' ') kl--;
        paint_prop(R, ps, k, kl, s + 1);
        while (*s && *s != ';') s++;
    }
}

static void paint_of(rc_t* R, dom_node_t* e, paint_t* ps) {
    static const char* const attrs[] = { "fill", "stroke", "stroke-width", "opacity", "fill-opacity", "stroke-opacity",
                                         "fill-rule", "color", "display", "visibility" };
    /* rules from <style>, then attributes, then style="" */
    const char* cls = dom_attr(e, "class");
    for (int i = 0; i < R->nrules; i++) {
        const char* sel = R->rules[i].sel;
        int match = 0;
        if (sel[0] == '.' && cls) {
            int sl = (int)strlen(sel + 1);
            for (const char* c = cls; *c; ) {
                while (*c == ' ') c++;
                const char* st = c;
                while (*c && *c != ' ') c++;
                if ((int)(c - st) == sl && strncmp(st, sel + 1, (size_t)sl) == 0) match = 1;
            }
        } else if (strcmp(sel, e->tag) == 0) match = 1;
        if (match) paint_decls(R, ps, R->rules[i].decls);
    }
    for (uint32_t i = 0; i < sizeof(attrs) / sizeof(attrs[0]); i++) {
        const char* v = dom_attr(e, attrs[i]);
        if (v) paint_prop(R, ps, attrs[i], (int)strlen(attrs[i]), v);
    }
    const char* st = dom_attr(e, "style");
    if (st) paint_decls(R, ps, st);
}

/* transform="translate(..) scale(..) rotate(..) matrix(..)" onto R->m */
static void apply_transform(rc_t* R, const char* t) {
    while (t && *t) {
        while (*t == ' ' || *t == ',') t++;
        const char* name = t;
        while (*t && *t != '(') t++;
        if (*t != '(') break;
        int nl = (int)(t - name);
        while (nl && name[nl - 1] == ' ') nl--;
        t++;
        real v[6] = { 0, 0, 0, 0, 0, 0 };
        int n = 0;
        while (n < 6 && read_num(&t, &v[n])) n++;
        while (*t && *t != ')') t++;
        if (*t == ')') t++;
        real a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;
        if (nl == 9 && strncmp(name, "translate", 9) == 0) { e = v[0]; f = n > 1 ? v[1] : 0; }
        else if (nl == 5 && strncmp(name, "scale", 5) == 0) { a = v[0]; d = n > 1 ? v[1] : v[0]; }
        else if (nl == 6 && strncmp(name, "matrix", 6) == 0 && n == 6) { a = v[0]; b = v[1]; c = v[2]; d = v[3]; e = v[4]; f = v[5]; }
        else if (nl == 6 && strncmp(name, "rotate", 6) == 0) {
            real th = v[0] * R_PI / 180, co = r_cos(th), si = r_sin(th);
            a = co; b = si; c = -si; d = co;
            if (n == 3) { e = v[1] - co * v[1] + si * v[2]; f = v[2] - si * v[1] - co * v[2]; }
        } else continue;
        real* m = R->m;
        real na = m[0] * a + m[2] * b, nb = m[1] * a + m[3] * b;
        real nc = m[0] * c + m[2] * d, nd = m[1] * c + m[3] * d;
        real ne = m[0] * e + m[2] * f + m[4], nf = m[1] * e + m[3] * f + m[5];
        m[0] = na; m[1] = nb; m[2] = nc; m[3] = nd; m[4] = ne; m[5] = nf;
    }
}

static real attr_real(dom_node_t* e, const char* name, real def) { return parse_real(dom_attr(e, name), def); }

/* the outline of a basic shape (user space); 0 if e is not one */
static int shape_outline(rc_t* R, dom_node_t* e) {
    const char* t = e->tag;
    if (strcmp(t, "path") == 0) { const char* d = dom_attr(e, "d"); if (d) path_data(R, d); return 1; }
    if (strcmp(t, "rect") == 0) {
        real x = attr_real(e, "x", 0), y = attr_real(e, "y", 0), w = attr_real(e, "width", 0), h = attr_real(e, "height", 0);
        real rx = attr_real(e, "rx", -1), ry = attr_real(e, "ry", -1);
        if (rx < 0) rx = ry;
        if (ry < 0) ry = rx;
        if (rx < 0) rx = ry = 0;
        if (rx > w / 2) rx = w / 2;
        if (ry > h / 2) ry = h / 2;
        if (w <= 0 || h <= 0) return 1;
        if (rx <= 0 || ry <= 0) {
            user_line(R, x, y, x + w, y); user_line(R, x + w, y, x + w, y + h);
            user_line(R, x + w, y + h, x, y + h); user_line(R, x, y + h, x, y);
        } else {
            user_line(R, x + rx, y, x + w - rx, y);
            arc_to(R, x + w - rx, y, rx, ry, 0, 0, 1, x + w, y + ry);
            user_line(R, x + w, y + ry, x + w, y + h - ry);
            arc_to(R, x + w, y + h - ry, rx, ry, 0, 0, 1, x + w - rx, y + h);
            user_line(R, x + w - rx, y + h, x + rx, y + h);
            arc_to(R, x + rx, y + h, rx, ry, 0, 0, 1, x, y + h - ry);
            user_line(R, x, y + h - ry, x, y + ry);
            arc_to(R, x, y + ry, rx, ry, 0, 0, 1, x + rx, y);
        }
        return 1;
    }
    if (strcmp(t, "circle") == 0 || strcmp(t, "ellipse") == 0) {
        real cx = attr_real(e, "cx", 0), cy = attr_real(e, "cy", 0);
        real rx = t[0] == 'c' ? attr_real(e, "r", 0) : attr_real(e, "rx", 0);
        real ry = t[0] == 'c' ? rx : attr_real(e, "ry", 0);
        if (rx <= 0 || ry <= 0) return 1;
        real px = cx + rx, py = cy;
        for (int i = 1; i <= 48; i++) {
            real th = i * 2 * R_PI / 48, nx = cx + rx * r_cos(th), ny = cy + ry * r_sin(th);
            user_line(R, px, py, nx, ny);
            px = nx; py = ny;
        }
        return 1;
    }
    if (strcmp(t, "line") == 0) {
        user_line(R, attr_real(e, "x1", 0), attr_real(e, "y1", 0), attr_real(e, "x2", 0), attr_real(e, "y2", 0));
        return 1;
    }
    if (strcmp(t, "polyline") == 0 || strcmp(t, "polygon") == 0) {
        const char* p = dom_attr(e, "points");
        real x0 = 0, y0 = 0, px = 0, py = 0, x, y;
        int n = 0;
        while (p && read_num(&p, &x) && read_num(&p, &y)) {
            if (n) user_line(R, px, py, x, y);
            else { x0 = x; y0 = y; }
            px = x; py = y;
            n++;
        }
        if (t[4] == 'g' && n > 2) user_line(R, px, py, x0, y0);   /* polygon: closed */
        return 1;
    }
    return 0;
}

static void draw_node(rc_t* R, dom_node_t* e, const paint_t* parent, int depth);

static void draw_children(rc_t* R, dom_node_t* e, const paint_t* ps, int depth) {
    for (dom_node_t* c = e->first; c; c = c->next)
        if (c->type == DOM_ELEM) draw_node(R, c, ps, depth + 1);
}

static void draw_node(rc_t* R, dom_node_t* e, const paint_t* parent, int depth) {
    if (depth > 40) return;
    const char* t = e->tag;
    if (strcmp(t, "defs") == 0 || strcmp(t, "clippath") == 0 || strcmp(t, "mask") == 0 || strcmp(t, "symbol") == 0 ||
        strcmp(t, "lineargradient") == 0 || strcmp(t, "radialgradient") == 0 || strcmp(t, "pattern") == 0 ||
        strcmp(t, "title") == 0 || strcmp(t, "desc") == 0 || strcmp(t, "metadata") == 0 || strcmp(t, "style") == 0 ||
        strcmp(t, "filter") == 0 || strcmp(t, "text") == 0 || strcmp(t, "script") == 0)
        return;
    paint_t ps = *parent;
    ps.hidden = 0;
    paint_of(R, e, &ps);
    if (ps.hidden) return;
    real saved[6];
    memcpy(saved, R->m, sizeof(saved));
    const char* tr = dom_attr(e, "transform");
    if (tr) apply_transform(R, tr);

    if (strcmp(t, "use") == 0) {
        const char* ref = dom_attr(e, "href");
        if (!ref) ref = dom_attr(e, "xlink:href");
        dom_node_t* target = ref && ref[0] == '#' ? dom_find_id(R->root, ref + 1) : NULL;
        if (target && target != e) {
            real ux = attr_real(e, "x", 0), uy = attr_real(e, "y", 0);
            R->m[4] += R->m[0] * ux + R->m[2] * uy;
            R->m[5] += R->m[1] * ux + R->m[3] * uy;
            if (strcmp(target->tag, "symbol") == 0) draw_children(R, target, &ps, depth);
            else draw_node(R, target, &ps, depth + 1);
        }
    } else if (strcmp(t, "g") == 0 || strcmp(t, "a") == 0 || strcmp(t, "svg") == 0 || strcmp(t, "switch") == 0) {
        draw_children(R, e, &ps, depth);
    } else {
        int start = R->nsegs;
        R->keep_flat = 0;
        if (shape_outline(R, e)) {
            int is_line = strcmp(t, "line") == 0 || strcmp(t, "polyline") == 0;
            if (!ps.fill_none && !is_line) fill_segs(R, ps.fill, ps.opacity * ps.fill_op, ps.evenodd);
            else R->nsegs = start;
            if (!ps.stroke_none && ps.stroke_w > 0) {
                /* the outline again, now kept for the stroke */
                R->keep_flat = 1;
                shape_outline(R, e);
                R->keep_flat = 0;
                make_stroke(R, start, ps.stroke_w);
                fill_segs(R, ps.stroke, ps.opacity * ps.stroke_op, 0);
            }
            R->nsegs = 0;
        }
    }
    memcpy(R->m, saved, sizeof(saved));
}

/* <style> text: ".a, .b { fill: red } path { ... }" (no nesting, no combinators) */
static void collect_rules(rc_t* R, dom_node_t* n) {
    for (dom_node_t* c = n->first; c; c = c->next) {
        if (c->type != DOM_ELEM) continue;
        if (strcmp(c->tag, "style") == 0 && c->first && c->first->type == DOM_TEXT) {
            const char* s = c->first->text;
            const char* end = s + c->first->text_len;
            while (s < end && R->nrules < 64) {
                const char* sel = s;
                while (s < end && *s != '{') s++;
                if (s >= end) break;
                const char* body = ++s;
                while (s < end && *s != '}') s++;
                char* decls = arena_strdup(R->A, body, (uint32_t)(s - body));
                if (s < end) s++;
                /* each selector of the list */
                const char* p = sel;
                while (p < body - 1 && R->nrules < 64) {
                    while (p < body - 1 && (*p == ' ' || *p == ',' || *p == '\n' || *p == '\r' || *p == '\t')) p++;
                    const char* q = p;
                    while (q < body - 1 && *q != ',') q++;
                    int l = (int)(q - p);
                    while (l && (p[l - 1] == ' ' || p[l - 1] == '\n' || p[l - 1] == '\r' || p[l - 1] == '\t')) l--;
                    if (l > 0) {
                        R->rules[R->nrules].sel = arena_strdup(R->A, p, (uint32_t)l);
                        R->rules[R->nrules].decls = decls;
                        R->nrules++;
                    }
                    p = q;
                }
            }
        }
        collect_rules(R, c);
    }
}

int svg_sniff(const uint8_t* d, uint32_t len) {
    uint32_t n = len < 512 ? len : 512;
    for (uint32_t i = 0; i + 4 <= n; i++)
        if (d[i] == '<' && (d[i + 1] == 's' || d[i + 1] == 'S') && (d[i + 2] == 'v' || d[i + 2] == 'V') && (d[i + 3] == 'g' || d[i + 3] == 'G'))
            return 1;
    return 0;
}

static dom_node_t* find_svg(dom_node_t* n) {
    for (dom_node_t* c = n->first; c; c = c->next) {
        if (c->type != DOM_ELEM) continue;
        if (strcmp(c->tag, "svg") == 0) return c;
        dom_node_t* r = find_svg(c);
        if (r) return r;
    }
    return NULL;
}

int svg_render(const char* src, uint32_t len, int want_w, int want_h, img_data_t* out, arena_t* A) {
    return svg_render_color(src, len, want_w, want_h, out, A, 0);
}

int svg_render_color(const char* src, uint32_t len, int want_w, int want_h, img_data_t* out, arena_t* A,
                     uint32_t current_color) {
    dom_node_t* doc = html_parse(A, src, len);
    dom_node_t* svg = doc ? find_svg(doc) : NULL;
    if (!svg) return -1;
    /* the canvas: viewBox, else width/height */
    real vx = 0, vy = 0, vw = 0, vh = 0;
    const char* vb = dom_attr(svg, "viewbox");
    if (vb) { const char* p = vb; read_num(&p, &vx); read_num(&p, &vy); read_num(&p, &vw); read_num(&p, &vh); }
    const char* aw = dom_attr(svg, "width");
    const char* ah = dom_attr(svg, "height");
    real w = aw && !strchr(aw, '%') ? parse_real(aw, 0) : 0;
    real h = ah && !strchr(ah, '%') ? parse_real(ah, 0) : 0;
    if (w <= 0 && h > 0 && vw > 0 && vh > 0) w = h * vw / vh;
    if (h <= 0 && w > 0 && vw > 0 && vh > 0) h = w * vh / vw;
    if (w <= 0) w = vw > 0 ? vw : 300;
    if (h <= 0) h = vh > 0 ? vh : 150;
    if (vw <= 0 || vh <= 0) { vw = w; vh = h; }
    if (want_w > 0 && want_h > 0) { w = want_w; h = want_h; }
    else if (want_w > 0) { h = h * want_w / w; w = want_w; }
    /* small icons are drawn at twice their size: they get scaled up often */
    if (w < 64 && h < 64 && want_w <= 0) { w *= 2; h *= 2; }
    if (w > 1024) { h = h * 1024 / w; w = 1024; }
    if (h > 1024) { w = w * 1024 / h; h = 1024; }
    int W = (int)(w + 0.5L), H = (int)(h + 0.5L);
    if (W < 1) W = 1;
    if (H < 1) H = 1;

    rc_t* R = (rc_t*)arena_alloc(A, sizeof(rc_t));
    if (!R) return -1;
    memset(R, 0, sizeof(*R));
    R->w = W;
    R->h = H;
    R->A = A;
    R->root = doc;
    R->rgb = (uint32_t*)arena_alloc(A, (uint32_t)(W * H) * 4);
    R->a = (uint8_t*)arena_alloc(A, (uint32_t)(W * H));
    R->segs = (seg_t*)arena_alloc(A, MAX_SEGS * (uint32_t)sizeof(seg_t));
    R->cov = (uint8_t*)arena_alloc(A, (uint32_t)W + 1);
    R->xs = (real*)arena_alloc(A, 2048u * (uint32_t)sizeof(real));
    R->dirs = (int*)arena_alloc(A, 2048u * (uint32_t)sizeof(int));
    R->tmp = (seg_t*)arena_alloc(A, MAX_SEGS * (uint32_t)sizeof(seg_t));
    if (A->oom) return -1;
    /* viewBox -> pixels (preserveAspectRatio xMidYMid meet) */
    real sx = W / vw, sy = H / vh, s = sx < sy ? sx : sy;
    R->m[0] = s; R->m[1] = 0; R->m[2] = 0; R->m[3] = s;
    R->m[4] = (W - vw * s) / 2 - vx * s;
    R->m[5] = (H - vh * s) / 2 - vy * s;
    collect_rules(R, svg);
    paint_t ps;
    memset(&ps, 0, sizeof(ps));
    ps.fill = 0x000000;
    ps.stroke_none = 1;
    ps.stroke_w = 1;
    ps.opacity = ps.fill_op = ps.stroke_op = 1;
    ps.color = current_color;
    paint_of(R, svg, &ps);
    draw_children(R, svg, &ps, 0);
    out->px = R->rgb;
    out->alpha = R->a;
    out->w = W;
    out->h = H;
    out->failed = 0;
    return 0;
}
