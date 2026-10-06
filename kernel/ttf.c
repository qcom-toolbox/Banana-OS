#include "ttf.h"
#include "kheap.h"
#include "kstring.h"

/* the kernel builds without SSE: the rasterizer counts in x87 long doubles */
typedef long double real;

static uint16_t u16(const ttf_t* f, uint32_t o) { return o + 2 <= f->len ? (uint16_t)(f->d[o] << 8 | f->d[o + 1]) : 0; }
static int16_t  s16(const ttf_t* f, uint32_t o) { return (int16_t)u16(f, o); }
static uint32_t u32(const ttf_t* f, uint32_t o) {
    return o + 4 <= f->len ? (uint32_t)f->d[o] << 24 | (uint32_t)f->d[o + 1] << 16 | (uint32_t)f->d[o + 2] << 8 | f->d[o + 3] : 0;
}

static uint32_t table(const ttf_t* f, const char* tag) {
    int n = u16(f, 4);
    for (int i = 0; i < n; i++) {
        uint32_t r = 12 + (uint32_t)i * 16;
        if (r + 16 > f->len) return 0;
        if (memcmp(f->d + r, tag, 4) == 0) {
            uint32_t off = u32(f, r + 8), len = u32(f, r + 12);
            return off + len <= f->len ? off : 0;
        }
    }
    return 0;
}

int ttf_init(ttf_t* f, const uint8_t* data, uint32_t len) {
    memset(f, 0, sizeof(*f));
    f->d = data;
    f->len = len;
    if (len < 12) return -1;
    uint32_t ver = u32(f, 0);
    if (ver != 0x00010000u && ver != 0x74727565u) return -1;   /* TrueType outlines only (not CFF) */
    uint32_t head = table(f, "head"), maxp = table(f, "maxp"), hhea = table(f, "hhea");
    f->cmap = table(f, "cmap");
    f->loca = table(f, "loca");
    f->glyf = table(f, "glyf");
    f->hmtx = table(f, "hmtx");
    f->kern = table(f, "kern");
    if (!head || !maxp || !hhea || !f->cmap || !f->loca || !f->glyf || !f->hmtx) return -1;
    f->upem = u16(f, head + 18);
    f->loca_long = s16(f, head + 50);
    f->nglyphs = u16(f, maxp + 4);
    f->ascent = s16(f, hhea + 4);
    f->descent = s16(f, hhea + 6);
    f->line_gap = s16(f, hhea + 8);
    f->nhmetrics = u16(f, hhea + 34);
    if (f->upem < 16 || !f->nhmetrics) return -1;
    /* the best Unicode map: format 12 (all planes), else format 4 (BMP) */
    int n = u16(f, f->cmap + 2);
    for (int i = 0; i < n; i++) {
        uint32_t r = f->cmap + 4 + (uint32_t)i * 8;
        int pid = u16(f, r), eid = u16(f, r + 2);
        uint32_t sub = f->cmap + u32(f, r + 4);
        int fmt = u16(f, sub);
        int unicode = pid == 0 || (pid == 3 && (eid == 1 || eid == 10));
        if (!unicode) continue;
        if (fmt == 12) { f->cmap_sub = sub; f->cmap_fmt = 12; break; }
        if (fmt == 4 && f->cmap_fmt != 12) { f->cmap_sub = sub; f->cmap_fmt = 4; }
    }
    return f->cmap_fmt ? 0 : -1;
}

int ttf_glyph(const ttf_t* f, uint32_t cp) {
    uint32_t s = f->cmap_sub;
    if (f->cmap_fmt == 12) {
        uint32_t n = u32(f, s + 12), lo = 0, hi = n;
        while (lo < hi) {                                /* groups are sorted */
            uint32_t mid = (lo + hi) / 2, g = s + 16 + mid * 12;
            uint32_t a = u32(f, g), b = u32(f, g + 4);
            if (cp < a) hi = mid;
            else if (cp > b) lo = mid + 1;
            else return (int)(u32(f, g + 8) + (cp - a));
        }
        return 0;
    }
    if (cp > 0xFFFF) return 0;
    int segs = u16(f, s + 6) / 2;
    uint32_t ends = s + 14, starts = ends + (uint32_t)segs * 2 + 2, deltas = starts + (uint32_t)segs * 2,
             ranges = deltas + (uint32_t)segs * 2;
    int lo = 0, hi = segs - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        uint16_t end = u16(f, ends + (uint32_t)mid * 2);
        if (cp > end) { lo = mid + 1; continue; }
        uint16_t start = u16(f, starts + (uint32_t)mid * 2);
        if (cp < start) { hi = mid - 1; continue; }
        uint16_t ro = u16(f, ranges + (uint32_t)mid * 2);
        int16_t delta = s16(f, deltas + (uint32_t)mid * 2);
        if (!ro) return (int)((cp + (uint32_t)(uint16_t)delta) & 0xFFFF);
        uint32_t ga = ranges + (uint32_t)mid * 2 + ro + (cp - start) * 2;
        uint16_t g = u16(f, ga);
        return g ? (int)((g + (uint32_t)(uint16_t)delta) & 0xFFFF) : 0;
    }
    return 0;
}

int ttf_advance(const ttf_t* f, int g) {
    if (g >= f->nhmetrics) g = f->nhmetrics - 1;
    return u16(f, f->hmtx + (uint32_t)g * 4);
}

int ttf_kern(const ttf_t* f, int left, int right) {
    if (!f->kern || u16(f, f->kern + 2) < 1) return 0;
    uint32_t sub = f->kern + 4;
    if (u16(f, sub + 4) != 1) return 0;                  /* format 0, horizontal */
    int n = u16(f, sub + 6);
    uint32_t key = (uint32_t)left << 16 | (uint32_t)right;
    int lo = 0, hi = n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        uint32_t p = sub + 14 + (uint32_t)mid * 6;
        uint32_t k = u32(f, p);
        if (k < key) lo = mid + 1;
        else if (k > key) hi = mid - 1;
        else return s16(f, p + 4);
    }
    return 0;
}

/* ── outlines ─────────────────────────────────────────────────────── */

typedef struct { real x, y; } pt_t;
typedef struct { pt_t a, b; } seg_t;

typedef struct {
    seg_t* s;
    int    n, cap;
    real   scale;                                    /* font units to pixels */
    int    fail;
} segs_t;

static void add_line(segs_t* S, pt_t a, pt_t b) {
    if (S->fail) return;
    if (S->n == S->cap) {
        int nc = S->cap ? S->cap * 2 : 256;
        seg_t* ns = (seg_t*)kmalloc((uint32_t)nc * (uint32_t)sizeof(seg_t));
        if (!ns) { S->fail = 1; return; }
        if (S->s) { memcpy(ns, S->s, (size_t)S->n * sizeof(seg_t)); kfree(S->s); }
        S->s = ns;
        S->cap = nc;
    }
    S->s[S->n].a = a;
    S->s[S->n].b = b;
    S->n++;
}

static real rabs(real v) { return v < 0 ? -v : v; }

static void add_quad(segs_t* S, pt_t a, pt_t c, pt_t b) {
    real dd = rabs(a.x - 2 * c.x + b.x) + rabs(a.y - 2 * c.y + b.y);   /* how far it bends, in pixels */
    int n = 1;
    while (n < 16 && dd > (real)n * n * 0.6L) n++;
    pt_t p = a;
    for (int i = 1; i <= n; i++) {
        real t = (real)i / n, u = 1 - t;
        pt_t q = { u * u * a.x + 2 * u * t * c.x + t * t * b.x, u * u * a.y + 2 * u * t * c.y + t * t * b.y };
        add_line(S, p, q);
        p = q;
    }
}

/* an affine transform for composite glyph parts (in font units) */
typedef struct { real a, b, c, d, e, f; } xform_t;

static pt_t apply(const segs_t* S, const xform_t* m, int x, int y) {
    pt_t p;
    p.x = (m->a * x + m->c * y + m->e) * S->scale;
    p.y = (m->b * x + m->d * y + m->f) * S->scale;
    return p;
}

static uint32_t glyph_off(const ttf_t* f, int g, uint32_t* len) {
    if (g < 0 || g >= f->nglyphs) return 0;
    uint32_t a, b;
    if (f->loca_long) { a = u32(f, f->loca + (uint32_t)g * 4); b = u32(f, f->loca + (uint32_t)g * 4 + 4); }
    else { a = (uint32_t)u16(f, f->loca + (uint32_t)g * 2) * 2; b = (uint32_t)u16(f, f->loca + (uint32_t)g * 2 + 2) * 2; }
    if (b <= a || f->glyf + b > f->len) { *len = 0; return 0; }
    *len = b - a;
    return f->glyf + a;
}

static void outline(const ttf_t* f, int g, const xform_t* m, segs_t* S, int depth) {
    uint32_t len, o = glyph_off(f, g, &len);
    if (!len || depth > 6) return;
    int nc = s16(f, o);
    if (nc < 0) {                                        /* composite: parts, each moved / scaled */
        uint32_t p = o + 10;
        for (;;) {
            if (p + 4 > o + len) return;
            int flags = u16(f, p), sub = u16(f, p + 2);
            p += 4;
            int dx, dy;
            if (flags & 1) { dx = s16(f, p); dy = s16(f, p + 2); p += 4; }
            else { dx = (int8_t)f->d[p]; dy = (int8_t)f->d[p + 1]; p += 2; }
            real a = 1, b = 0, c = 0, d = 1;
            if (flags & 8) { a = d = (real)s16(f, p) / 16384; p += 2; }
            else if (flags & 0x40) { a = (real)s16(f, p) / 16384; d = (real)s16(f, p + 2) / 16384; p += 4; }
            else if (flags & 0x80) {
                a = (real)s16(f, p) / 16384; b = (real)s16(f, p + 2) / 16384;
                c = (real)s16(f, p + 4) / 16384; d = (real)s16(f, p + 6) / 16384; p += 8;
            }
            xform_t mm;
            mm.a = m->a * a + m->c * b; mm.b = m->b * a + m->d * b;
            mm.c = m->a * c + m->c * d; mm.d = m->b * c + m->d * d;
            if (flags & 2) { mm.e = m->a * dx + m->c * dy + m->e; mm.f = m->b * dx + m->d * dy + m->f; }
            else { mm.e = m->e; mm.f = m->f; }           /* (point matching: rare, not done) */
            outline(f, sub, &mm, S, depth + 1);
            if (!(flags & 0x20)) return;
        }
    }
    if (!nc) return;
    uint32_t ends = o + 10;
    int npts = u16(f, ends + (uint32_t)(nc - 1) * 2) + 1;
    if (npts <= 0 || npts > 4000) return;
    uint32_t ilen = u16(f, ends + (uint32_t)nc * 2);
    uint32_t p = ends + (uint32_t)nc * 2 + 2 + ilen;
    uint8_t* fl = (uint8_t*)kmalloc((uint32_t)npts);
    int* xs = (int*)kmalloc((uint32_t)npts * sizeof(int));
    int* ys = (int*)kmalloc((uint32_t)npts * sizeof(int));
    if (!fl || !xs || !ys) { S->fail = 1; goto done; }
    for (int i = 0; i < npts; ) {                        /* flags, with repeats */
        if (p >= f->len) { S->fail = 1; goto done; }
        uint8_t fv = f->d[p++];
        fl[i++] = fv;
        if (fv & 8) {
            int r = p < f->len ? f->d[p++] : 0;
            while (r-- > 0 && i < npts) fl[i++] = fv;
        }
    }
    int v = 0;
    for (int i = 0; i < npts; i++) {
        if (fl[i] & 2) { int dv = p < f->len ? f->d[p++] : 0; v += (fl[i] & 16) ? dv : -dv; }
        else if (!(fl[i] & 16)) { v += s16(f, p); p += 2; }
        xs[i] = v;
    }
    v = 0;
    for (int i = 0; i < npts; i++) {
        if (fl[i] & 4) { int dv = p < f->len ? f->d[p++] : 0; v += (fl[i] & 32) ? dv : -dv; }
        else if (!(fl[i] & 32)) { v += s16(f, p); p += 2; }
        ys[i] = v;
    }
    int start = 0;
    for (int c = 0; c < nc; c++) {
        int end = u16(f, ends + (uint32_t)c * 2);
        if (end < start || end >= npts) break;
        int cnt = end - start + 1;
        /* begin on an on-curve point (or the middle of two off-curve ones) */
        int first = -1;
        for (int i = 0; i < cnt; i++) if (fl[start + i] & 1) { first = i; break; }
        pt_t startp;
        if (first < 0) {
            pt_t a = apply(S, m, xs[start], ys[start]), b = apply(S, m, xs[start + 1 < end + 1 ? start + 1 : start], ys[start + 1 < end + 1 ? start + 1 : start]);
            startp.x = (a.x + b.x) / 2; startp.y = (a.y + b.y) / 2;
            first = 0;
        } else {
            startp = apply(S, m, xs[start + first], ys[start + first]);
        }
        pt_t cur = startp, ctrl;
        int have_ctrl = 0;
        for (int k = 1; k <= cnt; k++) {
            int i = start + (first + k) % cnt;
            pt_t q = apply(S, m, xs[i], ys[i]);
            if (fl[i] & 1) {
                if (have_ctrl) add_quad(S, cur, ctrl, q); else add_line(S, cur, q);
                cur = q;
                have_ctrl = 0;
            } else {
                if (have_ctrl) {                         /* two controls: an implied point between */
                    pt_t mid = { (ctrl.x + q.x) / 2, (ctrl.y + q.y) / 2 };
                    add_quad(S, cur, ctrl, mid);
                    cur = mid;
                }
                ctrl = q;
                have_ctrl = 1;
            }
        }
        if (have_ctrl) add_quad(S, cur, ctrl, startp);
        else if (cur.x != startp.x || cur.y != startp.y) add_line(S, cur, startp);
        start = end + 1;
    }
done:
    if (fl) kfree(fl);
    if (xs) kfree(xs);
    if (ys) kfree(ys);
}

/* ── rasterizer: signed area per pixel, then a running sum per row ── */

static int ifloor(real v) { int i = (int)v; return (real)i > v ? i - 1 : i; }
static int iceil(real v) { int i = (int)v; return (real)i < v ? i + 1 : i; }

static void draw_line(real* acc, int w, int h, pt_t p0, pt_t p1) {
    if (p0.y == p1.y) return;
    real dir = 1;
    if (p0.y > p1.y) { pt_t t = p0; p0 = p1; p1 = t; dir = -1; }
    real dxdy = (p1.x - p0.x) / (p1.y - p0.y);
    real x = p0.x;
    if (p0.y < 0) { x -= p0.y * dxdy; p0.y = 0; }
    if (p1.y > h) p1.y = h;
    if (p0.y >= p1.y) return;
    int y0 = ifloor(p0.y), y1 = iceil(p1.y);
    for (int y = y0; y < y1; y++) {
        real top = y > p0.y ? (real)y : p0.y, bot = (real)(y + 1) < p1.y ? (real)(y + 1) : p1.y;
        real dy = bot - top;
        real xnext = x + dxdy * dy;
        real d = dy * dir;
        real xa = x < xnext ? x : xnext, xb = x < xnext ? xnext : x;
        real* row = acc + (uint32_t)y * (uint32_t)(w + 2);
        int x0i = ifloor(xa), x1i = iceil(xb);
        if (x0i < 0) x0i = 0;
        if (x1i > w) x1i = w;
        if (x1i <= x0i + 1) {
            real xmf = (x + xnext) / 2 - x0i;
            if (xmf < 0) xmf = 0;
            if (xmf > 1) xmf = 1;
            row[x0i] += d - d * xmf;
            row[x0i + 1] += d * xmf;
        } else {
            real s = 1 / (xb - xa);
            real x0f = xa - x0i;
            if (x0f < 0) x0f = 0;
            real a0 = s * (1 - x0f) * (1 - x0f) / 2;
            real x1f = xb - x1i + 1;
            if (x1f > 1) x1f = 1;
            real am = s * x1f * x1f / 2;
            row[x0i] += d * a0;
            if (x1i == x0i + 2) {
                row[x0i + 1] += d * (1 - a0 - am);
            } else {
                real a1 = s * ((real)1.5 - x0f);
                row[x0i + 1] += d * (a1 - a0);
                for (int xi = x0i + 2; xi < x1i - 1; xi++) row[xi] += d * s;
                real a2 = a1 + (x1i - x0i - 3) * s;
                row[x1i - 1] += d * (1 - a2 - am);
            }
            row[x1i] += d * am;
        }
        x = xnext;
    }
}

int ttf_render(const ttf_t* f, int g, int px, ttf_bitmap_t* out) {
    memset(out, 0, sizeof(*out));
    if (px < 1) return 0;
    if (px > 400) px = 400;
    segs_t S;
    memset(&S, 0, sizeof(S));
    S.scale = (real)px / f->upem;
    xform_t m = { 1, 0, 0, 1, 0, 0 };
    outline(f, g, &m, &S, 0);
    if (S.fail) { if (S.s) kfree(S.s); return -1; }
    if (!S.n) return 0;                                  /* a space */
    real minx = S.s[0].a.x, maxx = minx, miny = S.s[0].a.y, maxy = miny;
    for (int i = 0; i < S.n; i++) {
        pt_t q[2] = { S.s[i].a, S.s[i].b };
        for (int k = 0; k < 2; k++) {
            if (q[k].x < minx) minx = q[k].x;
            if (q[k].x > maxx) maxx = q[k].x;
            if (q[k].y < miny) miny = q[k].y;
            if (q[k].y > maxy) maxy = q[k].y;
        }
    }
    int left = ifloor(minx), right = iceil(maxx), bottom = ifloor(miny), top = iceil(maxy);
    int w = right - left, h = top - bottom;
    if (w <= 0 || h <= 0 || w > 1000 || h > 1000) { kfree(S.s); return 0; }
    real* acc = (real*)kzalloc((uint32_t)(w + 2) * (uint32_t)h * sizeof(real));
    uint8_t* a = (uint8_t*)kmalloc((uint32_t)w * (uint32_t)h);
    if (!acc || !a) { if (acc) kfree(acc); if (a) kfree(a); kfree(S.s); return -1; }
    for (int i = 0; i < S.n; i++) {                      /* bitmap rows run downward: flip y */
        pt_t p0 = { S.s[i].a.x - left, top - S.s[i].a.y }, p1 = { S.s[i].b.x - left, top - S.s[i].b.y };
        draw_line(acc, w, h, p0, p1);
    }
    for (int y = 0; y < h; y++) {
        real sum = 0;
        real* row = acc + (uint32_t)y * (uint32_t)(w + 2);
        for (int x = 0; x < w; x++) {
            sum += row[x];
            real c = rabs(sum);
            if (c > 1) c = 1;
            a[(uint32_t)y * (uint32_t)w + (uint32_t)x] = (uint8_t)(c * 255 + (real)0.5);
        }
    }
    kfree(acc);
    kfree(S.s);
    out->w = w;
    out->h = h;
    out->left = left;
    out->top = top;
    out->alpha = a;
    return 0;
}
