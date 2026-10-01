#ifndef LAYOUT_H
#define LAYOUT_H

#include "types.h"
#include "arena.h"
#include "html.h"
#include "css.h"

/*
 * Layout: the styled DOM becomes a display list - rectangles, text runs
 * (8x8 font, scaled) and images at page coordinates. Block formatting
 * (margins, padding, borders, widths), inline formatting with word wrap
 * and text-align, lists, simple tables, images and form controls.
 */

typedef struct img_data {
    uint32_t* px;           /* XRGB, natural size */
    int       w, h;
    int       failed;       /* could not load: draw a placeholder */
} img_data_t;

enum { DL_RECT = 1, DL_TEXT, DL_IMG, DL_CARET };

typedef struct dl_item {
    uint8_t  kind;
    uint8_t  scale, bold, underline, strike, italic, has_bg;
    int      x, y, w, h;
    uint32_t color, bg;
    const char* text;       /* ASCII (transliterated) */
    uint32_t len;
    dom_node_t* node;       /* element under this item (clicks) */
    uint8_t  rel;           /* part of an atomic inline box: y is yoff from its top */
    uint8_t  ulspace;       /* the leading space belongs to the same element: underline it too */
    int16_t  yoff, boxh;
    img_data_t* img;
} dl_item_t;

typedef struct layout {
    arena_t*   A;
    dl_item_t* items;
    uint32_t   n, cap;
    int        width, height;
    uint32_t   bg;          /* page background */
    dom_node_t* focus;      /* focused form control (gets a caret) */
} layout_t;

layout_t*   layout_build(arena_t* A, dom_node_t* doc, int width, dom_node_t* focus);
dom_node_t* layout_hit(layout_t* L, int x, int y);   /* element at page coordinates */

/* UTF-8 text in the 7-bit font: e-acute -> e, quotes -> ', dashes -> - ... */
uint32_t    text_to_ascii(const char* s, uint32_t n, char* out);

#endif
