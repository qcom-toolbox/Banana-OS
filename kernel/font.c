#include "font.h"
#include "ttf.h"
#ifdef FONT_HOST                     /* built for the host tests (web/test) */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#define kmalloc(n)  malloc(n)
#define kfree(p)    free(p)
#define kzalloc(n)  calloc(1, (n))
#else
#include "kheap.h"
#include "kstring.h"
#endif

/* the built-in faces, then the user's (the glyph cache keys a face in 4 bits) */
#define MAX_FACES (FONT_FACES + FONT_USER_MAX)

static ttf_t g_face[MAX_FACES];
static int   g_ok[MAX_FACES];
static int   g_ui = FONT_MONO, g_doc = -1;

void font_register(int face, const uint8_t* data, uint32_t len) {
    if (face < 0 || face >= FONT_FACES) return;
    g_ok[face] = ttf_init(&g_face[face], data, len) == 0;
}

/* the face drawn for a face number: the choices first, then a missing
 * face falls back to its family's regular one, then to any; -1 if none */
static int resolve(int face) {
    if (face == FONT_UI) face = g_ui;
    else if ((face == FONT_SANS || face == FONT_SANS_BOLD) && g_doc >= 0) face = g_doc;
    if (face < 0 || face >= MAX_FACES) face = FONT_SANS;
    if (g_ok[face]) return face;
    if (face >= FONT_FACES && g_ok[FONT_MONO]) return FONT_MONO;     /* a removed user font */
    if (face == FONT_MONO_BOLD && g_ok[FONT_MONO]) return FONT_MONO;
    if (face == FONT_SANS_BOLD && g_ok[FONT_SANS]) return FONT_SANS;
    for (int i = 0; i < FONT_FACES; i++) if (g_ok[i]) return i;
    return -1;
}

int font_available(int face) {
    if (face == FONT_UI) return resolve(face) >= 0;
    return face >= 0 && face < MAX_FACES && g_ok[face];
}

static const ttf_t* face_of(int face) {
    int r = resolve(face);
    return r >= 0 ? &g_face[r] : NULL;
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
        const cglyph_t* c = (cp > ' ') ? glyph_bitmap(resolve(face), f, px, g) : NULL;
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

/* ── the user's fonts ── */
#ifndef FONT_HOST
#include "fs.h"

static uint8_t* g_udata[MAX_FACES];          /* the file, kept (the face points into it) */
static char     g_uname[MAX_FACES][48];
static char     g_upath[MAX_FACES][FS_PATH_LEN];

/* drops a face's glyphs from the cache */
static void cache_drop(int face) {
    for (int i = 0; i < CACHE; i++)
        if (g_cache[i].key && (g_cache[i].key >> 28) == (uint32_t)face + 1) {
            kfree(g_cache[i].alpha);
            g_cache[i].alpha = NULL;
            g_cache[i].key = 0;
        }
}

static uint32_t be16(const uint8_t* p) { return (uint32_t)p[0] << 8 | p[1]; }
static uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

/* the font's full name (name table, ID 4; else the family, ID 1), ASCII */
static void font_name(const uint8_t* d, uint32_t len, char* out, int cap) {
    out[0] = 0;
    if (len < 12) return;
    uint32_t nt = be16(d + 4), name = 0, nlen = 0;
    for (uint32_t i = 0; i < nt && 12 + i * 16 + 16 <= len; i++) {
        const uint8_t* r = d + 12 + i * 16;
        if (r[0] == 'n' && r[1] == 'a' && r[2] == 'm' && r[3] == 'e') { name = be32(r + 8); nlen = be32(r + 12); }
    }
    if (!name || name + 6 > len || nlen > len - name) return;
    uint32_t count = be16(d + name + 2), strs = name + be16(d + name + 4);
    int best = 0;
    for (uint32_t i = 0; i < count && name + 6 + i * 12 + 12 <= len; i++) {
        const uint8_t* r = d + name + 6 + i * 12;
        uint32_t pid = be16(r), id = be16(r + 6), l = be16(r + 8), o = strs + be16(r + 10);
        if ((id != 4 && id != 1) || (pid != 3 && pid != 1 && pid != 0) || o + l > len) continue;
        int score = (id == 4 ? 2 : 1) * 2 + (pid != 1);
        if (score <= best) continue;
        int n = 0, wide = pid != 1;
        for (uint32_t k = wide; k < l && n < cap - 1; k += wide ? 2 : 1) {
            uint8_t c = d[o + k];
            if (wide && d[o + k - 1]) c = '?';
            out[n++] = (char)(c >= 32 && c < 127 ? c : '?');
        }
        out[n] = 0;
        if (n) best = score;
    }
}

int font_user_find(const char* path) {
    for (int f = FONT_FACES; f < MAX_FACES; f++) if (g_ok[f] && !strcmp(g_upath[f], path)) return f;
    return -1;
}

int font_user_load(const char* path, char* err, int cap) {
    int have = font_user_find(path);
    if (have >= 0) return have;
    int face = -1;
    for (int f = FONT_FACES; f < MAX_FACES; f++) if (!g_ok[f] && !g_udata[f]) { face = f; break; }
    if (face < 0) { ksnprintf(err, (uint32_t)cap, "%d fonts at most", FONT_USER_MAX); return -1; }
    int idx = fs_find_file(path);
    fs_file_t* fl = idx >= 0 ? fs_get_file(idx) : NULL;
    if (!fl || !fl->content || fl->size < 12) { ksnprintf(err, (uint32_t)cap, "cannot read %s", path); return -1; }
    uint8_t* d = (uint8_t*)kmalloc(fl->size);
    if (!d) { ksnprintf(err, (uint32_t)cap, "out of memory"); return -1; }
    memcpy(d, fl->content, fl->size);
    if (ttf_init(&g_face[face], d, fl->size) != 0) {
        kfree(d);
        ksnprintf(err, (uint32_t)cap, "not a TrueType font (.ttf with TrueType outlines)");
        return -1;
    }
    /* it must draw: a few letters */
    const char* probe = "Aag0";
    for (const char* p = probe; *p; p++) {
        ttf_bitmap_t bm;
        int g = ttf_glyph(&g_face[face], (uint32_t)*p);
        if (!g || ttf_render(&g_face[face], g, 16, &bm) != 0) {
            kfree(d);
            ksnprintf(err, (uint32_t)cap, "the font has no usable letters");
            return -1;
        }
        kfree(bm.alpha);
    }
    g_udata[face] = d;
    font_name(d, fl->size, g_uname[face], sizeof(g_uname[face]));
    if (!g_uname[face][0]) {
        const char* b = strrchr(path, '/');
        kstrlcpy(g_uname[face], b ? b + 1 : path, sizeof(g_uname[face]));
    }
    kstrlcpy(g_upath[face], path, sizeof(g_upath[face]));
    cache_drop(face);
    g_ok[face] = 1;
    return face;
}

void font_user_unload(int face) {
    if (face < FONT_FACES || face >= MAX_FACES || !g_ok[face]) return;
    if (g_ui == face) g_ui = FONT_MONO;
    if (g_doc == face) g_doc = -1;
    g_ok[face] = 0;
    cache_drop(face);
    kfree(g_udata[face]);
    g_udata[face] = NULL;
    g_uname[face][0] = g_upath[face][0] = 0;
}

int font_user_count(void) {
    int n = 0;
    for (int f = FONT_FACES; f < MAX_FACES; f++) n += g_ok[f];
    return n;
}

int font_user_face(int i) {
    for (int f = FONT_FACES; f < MAX_FACES; f++) if (g_ok[f] && i-- == 0) return f;
    return -1;
}

const char* font_user_name(int face) { return face >= FONT_FACES && face < MAX_FACES && g_ok[face] ? g_uname[face] : ""; }
const char* font_user_path(int face) { return face >= FONT_FACES && face < MAX_FACES && g_ok[face] ? g_upath[face] : ""; }

void font_set_ui(int face) { g_ui = face >= 0 && face < MAX_FACES && g_ok[face] ? face : FONT_MONO; }
int  font_ui(void) { return g_ui; }
void font_set_doc(int face) { g_doc = face >= 0 && face < MAX_FACES && g_ok[face] ? face : -1; }
int  font_doc(void) { return g_doc; }
#endif

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
