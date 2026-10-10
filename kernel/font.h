#ifndef FONT_H
#define FONT_H

#ifdef FONT_HOST
#include <stdint.h>
#else
#include "types.h"
#endif

/*
 * The system's fonts (TrueType, ttf.c): DejaVu Sans and Sans Mono, regular
 * and bold, built into the kernel (fontbin.asm). Text is UTF-8; sizes are
 * pixels per em; glyphs are anti-aliased and cached.
 */

enum { FONT_SANS = 0, FONT_SANS_BOLD, FONT_MONO, FONT_MONO_BOLD, FONT_FACES };

/*
 * Fonts the user installs (Settings > Fonts, or "Install font" in Files):
 * .ttf files kept in FONT_DIR, loaded at boot. Each gets a face number
 * (FONT_FACES and up). Two choices use them:
 *   the interface font  - the desktop's text (FONT_UI: one glyph per cell,
 *                         gfx.c), DejaVu Sans Mono unless chosen
 *   the documents font  - what FONT_SANS (and its bold) draws: web pages,
 *                         apps, desktop icon names; DejaVu Sans unless chosen
 */
#define FONT_UI        64          /* a face number: the interface font */
#define FONT_DIR       "/usr/share/fonts"
#define FONT_USER_MAX  11

/* loads a .ttf file as a user font: its face, or -1 with the reason in err */
int  font_user_load(const char* path, char* err, int cap);
void font_user_unload(int face);
int  font_user_count(void);
int  font_user_face(int i);                 /* the i-th user font's face */
const char* font_user_name(int face);       /* its name (from the font), "" if none */
const char* font_user_path(int face);       /* its file */
int  font_user_find(const char* path);      /* the face loaded from path, -1 */

void font_set_ui(int face);                 /* FONT_MONO: the default */
int  font_ui(void);
void font_set_doc(int face);                /* -1: the default (DejaVu Sans) */
int  font_doc(void);

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
