#ifndef TTF_H
#define TTF_H

#ifdef FONT_HOST
#include <stdint.h>
#else
#include "types.h"
#endif

/*
 * TrueType fonts (.ttf, glyf outlines): the tables Banana OS needs - cmap
 * (formats 4 and 12), metrics, kerning - and an anti-aliased rasterizer
 * (exact area coverage, no hinting). font.c builds the system's text on it.
 */

typedef struct {
    const uint8_t* d;
    uint32_t len;
    uint32_t cmap, loca, glyf, hmtx, kern;   /* table offsets (0: missing) */
    uint32_t cmap_sub;                       /* the subtable used */
    int      cmap_fmt;                       /* 4 or 12 */
    int      upem, loca_long, nglyphs, nhmetrics;
    int      ascent, descent, line_gap;      /* hhea, font units (descent < 0) */
} ttf_t;

typedef struct {
    int      w, h;
    int      left, top;     /* from the pen position on the baseline to the bitmap's top-left (top: up is +) */
    uint8_t* alpha;         /* w * h coverage, 0..255 (kmalloc'd; NULL for an empty glyph) */
} ttf_bitmap_t;

int  ttf_init(ttf_t* f, const uint8_t* data, uint32_t len);   /* 0, or -1 if not a usable font */
int  ttf_glyph(const ttf_t* f, uint32_t codepoint);            /* glyph index (0: missing) */
int  ttf_advance(const ttf_t* f, int glyph);                   /* font units */
int  ttf_kern(const ttf_t* f, int left, int right);            /* font units */
/* the glyph at px pixels per em; 0, or -1 (out of memory / broken glyph) */
int  ttf_render(const ttf_t* f, int glyph, int px, ttf_bitmap_t* out);

#endif
