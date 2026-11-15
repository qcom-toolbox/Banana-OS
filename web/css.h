/* SPDX-License-Identifier: GPL-3.0-or-later */
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
#define GRID_MAX_COLS 16
enum { GRID_PX, GRID_PERCENT, GRID_FR };
typedef struct { int value, min; uint8_t kind; } grid_track_t;

typedef struct style {
    uint8_t  display;
    uint8_t  bold, italic, underline, strike, uppercase, lowercase;
    uint8_t  align, pre, nowrap, list_style, visible;
    uint8_t  scale;                 /* font: 8x8 glyphs scaled 1..4 */
    uint8_t  has_bg;
    uint8_t  mono;                  /* font-family: monospace (Courier, Consolas, ...) */
    int16_t  lh_px, lh_pct;         /* line-height: px, or a factor x100 (unitless / %); 0 0: normal */
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
    uint8_t  position;              /* POS_* */
    uint8_t  overflow_hidden;
    uint8_t  clipped;               /* clip: rect(0 0 0 0) / clip-path: inset(50%) - visually hidden */
    uint8_t  flex_row;              /* display: grid (or flex): children side by side as inline-blocks */
    uint8_t  flex;                  /* display: flex - a real flex container (layout_flex) */
    uint8_t  grid, grid_ncols;      /* explicit columns, row-major automatic placement */
    grid_track_t grid_cols[GRID_MAX_COLS];
    uint8_t  flex_dir;              /* FLEX_ROW / FLEX_ROW_REV / FLEX_COL / FLEX_COL_REV */
    uint8_t  flex_wrap;
    uint8_t  justify;               /* JUSTIFY_* (main axis) */
    uint8_t  align_items;           /* FA_* (cross axis) */
    uint8_t  align_self;            /* FA_AUTO or FA_* */
    int      gap_row, gap_col;      /* px */
    int      flex_grow, flex_shrink;   /* x100 (flex-grow: 1 -> 100) */
    int      flex_basis;            /* px or LEN_AUTO */
    uint8_t  floated;
    int      left, top;             /* px or LEN_AUTO (position offsets) */
    int      right, bottom;
    /* backgrounds beyond a colour */
    const char* bg_url;             /* background-image: url(...) - not NUL-terminated, bg_url_len long */
    uint16_t bg_url_len;
    uint8_t  bg_grad;               /* linear-gradient: grad_from -> grad_to */
    uint8_t  grad_dir;              /* GRAD_* */
    uint32_t grad_from, grad_to;
    uint8_t  bg_repeat;             /* BG_REPEAT* */
    uint8_t  bg_size;               /* BG_SIZE_* */
    int      bg_size_w, bg_size_h;  /* BG_SIZE_PX: px (LEN_AUTO: from the other one) */
    int      bg_pos_x, bg_pos_y;    /* px, or a percentage when bg_pos_pct has the bit */
    uint8_t  bg_pos_pct;            /* bit 0: x is %, bit 1: y is % */
    uint8_t  bg_size_pct;           /* bit 0: bg_size_w is %, bit 1: bg_size_h is % */
    uint8_t  bg_clip_text;          /* background-clip: text - the gradient colours the text */
    uint8_t  off_pct;               /* bits: left 1, top 2, right 4, bottom 8 hold a percentage */
    int      tx, ty;                /* transform: translate(), px or % (t_pct) of the own box */
    uint8_t  t_pct;                 /* bit 0: tx is %, bit 1: ty is % */
    uint8_t  object_fit;            /* <img> / <video>: 0 fill, 1 contain, 2 cover, 3 none, 4 scale-down */
    uint8_t  text_clear;            /* color / -webkit-text-fill-color: transparent */
    const char* grad_src;           /* "linear-gradient(...)": its stops, read by layout */
    /* animation (css_anim.h): the first one */
    const char* anim_name;          /* not NUL-terminated: anim_name_len long */
    uint8_t  anim_name_len;
    uint8_t  anim_dir, anim_timing, anim_fill;
    int      anim_dur, anim_delay;  /* ms */
    int      anim_iter;             /* 0 = 1, -1 = infinite */
    int      radius;                /* border-radius, px (large: a pill / circle) */
    struct css_var* vars;           /* custom properties (--x), inherited */
} style_t;

enum { POS_STATIC = 0, POS_RELATIVE, POS_ABSOLUTE, POS_FIXED, POS_STICKY };
enum { FLEX_ROW = 0, FLEX_ROW_REV, FLEX_COL, FLEX_COL_REV };
enum { GRAD_DOWN = 0, GRAD_RIGHT, GRAD_UP, GRAD_LEFT };
enum { BG_REPEAT = 0, BG_NO_REPEAT, BG_REPEAT_X, BG_REPEAT_Y };
enum { BG_SIZE_AUTO = 0, BG_SIZE_COVER, BG_SIZE_CONTAIN, BG_SIZE_PX };
enum { JUSTIFY_START = 0, JUSTIFY_CENTER, JUSTIFY_END, JUSTIFY_BETWEEN, JUSTIFY_AROUND, JUSTIFY_EVENLY };
enum { FA_AUTO = 0, FA_STRETCH, FA_START, FA_CENTER, FA_END };   /* flex alignment */

typedef struct css_var {
    const char* name;
    const char* raw;
    const char* value;
    uint8_t state;                 /* raw / resolving / computed / invalid */
    struct css_var* next;
} css_var_t;

/* the viewport width @media queries are evaluated against (page.c sets it) */
extern int css_viewport_w;
extern int css_viewport_h;
extern uint32_t css_anim_now;       /* page.c: the clock CSS animations are computed at, ms */
extern int css_anim_running;        /* a restyle left some animation running */          /* viewport units; defaults to 600 until page.c sets it */

typedef struct css_sheet css_sheet_t;

css_sheet_t* css_parse(arena_t* A, const char* src, uint32_t len, int origin);
/* computes dom->style for every element (call after any DOM/style change) */
void css_style_tree(arena_t* A, dom_node_t* doc, css_sheet_t** sheets, int nsheets);
/* the built-in default style sheet */
css_sheet_t* css_default_sheet(arena_t* A);

uint32_t css_color(const char* s, int* ok);     /* "red", "#f80", "rgb(1,2,3)" -> 0x00RRGGBB */
/* the same with its transparency: 255 - alpha in the top byte (0 = opaque),
 * which the renderer blends: backgrounds and borders */
uint32_t css_color_blend(const char* s, int* ok);
/* does an element match a selector list (querySelector)? */
int  css_matches(arena_t* A, dom_node_t* el, const char* selector);
dom_node_t* css_query(arena_t* A, dom_node_t* root, const char* selector);
int  css_query_all(arena_t* A, dom_node_t* root, const char* selector, dom_node_t** out, int max);

#endif
