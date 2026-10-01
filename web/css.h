#ifndef CSS_H
#define CSS_H

#include "types.h"
#include "arena.h"
#include "html.h"

/*
 * CSS: style sheets (<style>, a built-in default sheet) and style=""
 * attributes, selectors (tag, .class, #id, [attr], descendant and child
 * combinators, :first-child, a:link), specificity and !important, and
 * the computed style of every element - the subset the layout uses.
 */

enum { DISP_NONE = 0, DISP_INLINE, DISP_BLOCK, DISP_LIST_ITEM, DISP_INLINE_BLOCK,
       DISP_TABLE, DISP_TABLE_ROW, DISP_TABLE_CELL, DISP_TABLE_GROUP };
enum { ALIGN_LEFT = 0, ALIGN_CENTER, ALIGN_RIGHT };
enum { LIST_DISC = 0, LIST_CIRCLE, LIST_SQUARE, LIST_DECIMAL, LIST_NONE };

#define LEN_AUTO (-100000)          /* width/height not given */

typedef struct style {
    uint8_t  display;
    uint8_t  bold, italic, underline, strike, uppercase, lowercase;
    uint8_t  align, pre, nowrap, list_style, visible;
    uint8_t  scale;                 /* font: 8x8 glyphs scaled 1..4 */
    uint8_t  has_bg;
    uint32_t color, bg;
    int      margin[4];             /* top right bottom left, px */
    int      padding[4];
    int      border[4];             /* widths */
    uint32_t border_color[4];
    int      width, height;         /* px or LEN_AUTO */
    int      width_pct;             /* 1..100 if width was a percentage */
    int      max_width;             /* px or LEN_AUTO */
    int      font_px;               /* computed font size (for em) */
    int      margin_auto_lr;        /* margin: 0 auto - centered block */
} style_t;

typedef struct css_sheet css_sheet_t;

css_sheet_t* css_parse(arena_t* A, const char* src, uint32_t len, int origin);
/* computes dom->style for every element (call after any DOM/style change) */
void css_style_tree(arena_t* A, dom_node_t* doc, css_sheet_t** sheets, int nsheets);
/* the built-in default style sheet */
css_sheet_t* css_default_sheet(arena_t* A);

uint32_t css_color(const char* s, int* ok);     /* "red", "#f80", "rgb(1,2,3)" -> 0x00RRGGBB */
/* does an element match a selector list (querySelector)? */
int  css_matches(arena_t* A, dom_node_t* el, const char* selector);
dom_node_t* css_query(arena_t* A, dom_node_t* root, const char* selector);
int  css_query_all(arena_t* A, dom_node_t* root, const char* selector, dom_node_t** out, int max);

#endif
