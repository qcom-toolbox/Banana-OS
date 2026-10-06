#include "font.h"
#include "ttf.h"
#include "kheap.h"
#include "kstring.h"

static ttf_t g_face[FONT_FACES];
static int   g_ok[FONT_FACES];

void font_register(int face, const uint8_t* data, uint32_t len) {
    if (face < 0 || face >= FONT_FACES) return;
    g_ok[face] = ttf_init(&g_face[face], data, len) == 0;
}

int font_available(int face) { return face >= 0 && face < FONT_FACES && g_ok[face]; }

/* a missing face falls back to its family's regular one, then to Sans */
static const ttf_t* face_of(int face) {
    if (face < 0 || face >= FONT_FACES) face = FONT_SANS;
    if (g_ok[face]) return &g_face[face];
    if (face == FONT_MONO_BOLD && g_ok[FONT_MONO]) return &g_face[FONT_MONO];
    if (face == FONT_SANS_BOLD && g_ok[FONT_SANS]) return &g_face[FONT_SANS];
    for (int i = 0; i < FONT_FACES; i++) if (g_ok[i]) return &g_face[i];
    return NULL;
}

uint32_t utf8_next(const char** sp, const char* end) {
    const unsigned char* s = (const unsigned char*)*sp;
    if ((const char*)s >= end) return 0;
    uint32_t c = *s++;
    int more = c < 0x80 ? 0 : c >= 0xF0 && c < 0xF8 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : -1;
    if (more < 0) { *sp = (const char*)s; return 0xFFFD; }
    if (more) c &= 0x3Fu >> more;
    for (int i = 0; i < more; i++) {
        if ((const char*)s >= end || (*s & 0xC0) != 0x80) { *sp = (const char*)s; return 0xFFFD; }
        c = c << 6 | (*s++ & 0x3F);
    }
    *sp = (const char*)s;
    return c;
}

/* ── the glyph cache: direct mapped by (face, size, glyph) ── */

typedef struct {
    uint32_t key;            /* face << 28 | px << 18 | glyph (0: empty slot) */
    int16_t  w, h, left, top;
    uint8_t* alpha;
} cglyph_t;

#define CACHE 4096
static cglyph_t g_cache[CACHE];

static const cglyph_t* glyph_bitmap(int face, const ttf_t* f, int px, int g) {
    uint32_t key = ((uint32_t)face + 1) << 28 | ((uint32_t)px & 0x3FF) << 18 | ((uint32_t)g & 0x3FFFF);
    uint32_t h = (key * 2654435761u) >> 20;
    cglyph_t* c = &g_cache[h % CACHE];
    if (c->key == key) return c;
    ttf_bitmap_t bm;
    if (ttf_render(f, g, px, &bm) != 0) return NULL;
    if (c->alpha) kfree(c->alpha);
    c->key = key;
    c->w = (int16_t)bm.w;
    c->h = (int16_t)bm.h;
    c->left = (int16_t)bm.left;
    c->top = (int16_t)bm.top;
    c->alpha = bm.alpha;
    return c;
}

void font_metrics(int face, int px, int* ascent, int* descent, int* line_h) {
    const ttf_t* f = face_of(face);
    if (!f) { if (ascent) *ascent = px; if (descent) *descent = px / 4; if (line_h) *line_h = px + px / 4; return; }
    int a = (f->ascent * px + f->upem - 1) / f->upem;
    int d = (-f->descent * px + f->upem - 1) / f->upem;
    if (ascent) *ascent = a;
    if (descent) *descent = d;
    if (line_h) *line_h = a + d + (f->line_gap * px) / f->upem;
}

/* pen advance of a code point after `prev` (kerning), in 1/64 px */
static int advance64(const ttf_t* f, int px, int g, int prev_g) {
    int adv = ttf_advance(f, g);
    if (prev_g) adv += ttf_kern(f, prev_g, g);
    return (adv * px * 64 + f->upem / 2) / f->upem;
}

int font_text_width(int face, int px, const char* s, uint32_t n) {
    const ttf_t* f = face_of(face);
    if (!f) return (int)n * px / 2;
    const char* e = s + n;
    int pen = 0, prev = 0;
    while (s < e) {
        uint32_t cp = utf8_next(&s, e);
        if (cp == '\t') cp = ' ';
        int g = ttf_glyph(f, cp);
        pen += advance64(f, px, g, prev);
        prev = g;
    }
    return (pen + 32) >> 6;
}

uint32_t font_fit(int face, int px, const char* s, uint32_t n, int max_w) {
    const ttf_t* f = face_of(face);
    const char* start = s, *e = s + n;
    int pen = 0, prev = 0;
    while (s < e) {
        const char* before = s;
        uint32_t cp = utf8_next(&s, e);
        int g = f ? ttf_glyph(f, cp) : 0;
        pen += f ? advance64(f, px, g, prev) : px * 32;
        if ((pen + 32) >> 6 > max_w) return (uint32_t)(before - start);
        prev = g;
    }
    return n;
}

static inline uint32_t blend(uint32_t dst, uint32_t src, uint32_t a) {
    uint32_t rb = ((src & 0xFF00FF) * a + (dst & 0xFF00FF) * (255 - a)) >> 8;
    uint32_t g = ((src & 0x00FF00) * a + (dst & 0x00FF00) * (255 - a)) >> 8;
    return (rb & 0xFF00FF) | (g & 0x00FF00);
}

int font_draw(uint32_t* buf, int stride, int cx0, int cy0, int cx1, int cy1,
              int face, int px, int x, int y, const char* s, uint32_t n, uint32_t color, int italic) {
    const ttf_t* f = face_of(face);
    if (!f) return x;
    const char* e = s + n;
    int pen = x * 64, prev = 0;
    color &= 0xFFFFFF;
    while (s < e) {
        uint32_t cp = utf8_next(&s, e);
        if (cp == '\t') cp = ' ';
        int g = ttf_glyph(f, cp);
        if (prev) pen += ((ttf_kern(f, prev, g) * px * 64) + f->upem / 2) / f->upem;
        const cglyph_t* c = (cp > ' ') ? glyph_bitmap(face, f, px, g) : NULL;
        if (c && c->alpha) {
            int gx = ((pen + 32) >> 6) + c->left, gy = y - c->top;
            for (int r = 0; r < c->h; r++) {
                int yy = gy + r;
                if (yy < cy0 || yy >= cy1) continue;
                int shear = italic ? (c->top - r) / 4 : 0;      /* a slant of about 14 degrees */
                uint32_t* row = buf + (uint32_t)yy * (uint32_t)stride;
                const uint8_t* src = c->alpha + (uint32_t)r * (uint32_t)c->w;
                for (int k = 0; k < c->w; k++) {
                    uint32_t a = src[k];
                    if (!a) continue;
                    int xx = gx + k + shear;
                    if (xx < cx0 || xx >= cx1) continue;
                    row[xx] = a == 255 ? color : blend(row[xx], color, a);
                }
            }
        }
        pen += (ttf_advance(f, g) * px * 64 + f->upem / 2) / f->upem;
        prev = g;
    }
    return (pen + 32) >> 6;
}

/* ── the built-in fonts ── */
#ifndef FONT_HOST
extern const uint8_t font_sans[], font_sans_end[], font_sans_bold[], font_sans_bold_end[];
extern const uint8_t font_mono[], font_mono_end[], font_mono_bold[], font_mono_bold_end[];

void fonts_init(void) {
    font_register(FONT_SANS, font_sans, (uint32_t)(font_sans_end - font_sans));
    font_register(FONT_SANS_BOLD, font_sans_bold, (uint32_t)(font_sans_bold_end - font_sans_bold));
    font_register(FONT_MONO, font_mono, (uint32_t)(font_mono_end - font_mono));
    font_register(FONT_MONO_BOLD, font_mono_bold, (uint32_t)(font_mono_bold_end - font_mono_bold));
}
#endif
