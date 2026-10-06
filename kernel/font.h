#ifndef FONT_H
#define FONT_H

#include "types.h"

/*
 * The system's fonts (TrueType, ttf.c): DejaVu Sans and Sans Mono, regular
 * and bold, built into the kernel (fontbin.asm). Text is UTF-8; sizes are
 * pixels per em; glyphs are anti-aliased and cached.
 */

enum { FONT_SANS = 0, FONT_SANS_BOLD, FONT_MONO, FONT_MONO_BOLD, FONT_FACES };

void font_register(int face, const uint8_t* data, uint32_t len);   /* fonts_init() does the built-in ones */
void fonts_init(void);
int  font_available(int face);

/* ascent (above the baseline), descent (below, positive) and the line height, in pixels */
void font_metrics(int face, int px, int* ascent, int* descent, int* line_h);
/* width in pixels of n bytes of UTF-8 text */
int  font_text_width(int face, int px, const char* s, uint32_t n);
/* how many bytes of s fit in max_w pixels (whole characters) */
uint32_t font_fit(int face, int px, const char* s, uint32_t n, int max_w);
/* draws n bytes of UTF-8 text with its baseline at y, clipped to (cx0, cy0)-(cx1, cy1);
 * italic slants it; returns the x after the last character */
int  font_draw(uint32_t* buf, int stride, int cx0, int cy0, int cx1, int cy1,
               int face, int px, int x, int y, const char* s, uint32_t n, uint32_t color, int italic);

/* the next code point of UTF-8 text (bad bytes come back as U+FFFD) */
uint32_t utf8_next(const char** s, const char* end);

#endif
