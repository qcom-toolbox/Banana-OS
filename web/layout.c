#include "layout.h"
#include "kstring.h"
#include "kheap.h"

#define GLYPH 8

/* ══ display list ═════════════════════════════════════════════════════ */

typedef struct {
    layout_t* L;
    int       dry;              /* measuring only: emit nothing */
    int       max_w;            /* widest line seen (measuring) */
    int       max_word;         /* widest unbreakable piece (measuring) */
    int       force_w, force_h; /* a flex container's size for the next layout_box (border box, px) */
    /* position: absolute boxes wait until their containing block (the
     * nearest positioned ancestor) has its size; fixed ones until the end */
    int       cb_x, cb_y, cb_w, cb_h;
    struct { dom_node_t* n; int sx, sy; } absq[64], fixq[32];
    int       nabs, nfix;
    /* overflow: hidden - items are clipped to this (page coordinates) */
    int       clip_on, clip_x0, clip_y0, clip_x1, clip_y1;
} ctx_t;

static int is_out_of_flow(const dom_node_t* e) {
    return e->type == DOM_ELEM && e->style && (e->style->position == POS_ABSOLUTE || e->style->position == POS_FIXED);
}

static int out_of_sight(const style_t* st);

/* an absolute/fixed box met at its static position (sx, sy): placed later */
static void defer_abs(ctx_t* C, dom_node_t* e, int sx, int sy) {
    if (C->dry) return;                         /* out of the flow: no size to measure */
    if (out_of_sight(e->style)) return;         /* visually hidden (flex containers come here directly) */
    if (e->style->position == POS_FIXED) {
        if (C->nfix < 32) { C->fixq[C->nfix].n = e; C->fixq[C->nfix].sx = sx; C->fixq[C->nfix].sy = sy; C->nfix++; }
    } else if (C->nabs < 64) {
        C->absq[C->nabs].n = e; C->absq[C->nabs].sx = sx; C->absq[C->nabs].sy = sy; C->nabs++;
    }
}

static dl_item_t* push_raw(ctx_t* C, int kind);

static dl_item_t* push(ctx_t* C, int kind) {
    dl_item_t* it = push_raw(C, kind);
    if (C->clip_on && !C->dry) {
        it->has_clip = 1;
        it->cx0 = C->clip_x0; it->cy0 = C->clip_y0; it->cx1 = C->clip_x1; it->cy1 = C->clip_y1;
    }
    return it;
}

static dl_item_t* push_raw(ctx_t* C, int kind) {
    static dl_item_t dummy;
    if (C->dry) { memset(&dummy, 0, sizeof(dummy)); return &dummy; }
    layout_t* L = C->L;
    if (L->n == L->cap) {
        uint32_t nc = L->cap ? L->cap * 2 : 256;
        dl_item_t* ni = (dl_item_t*)arena_alloc(L->A, nc * (uint32_t)sizeof(dl_item_t));
        if (L->A->oom) { memset(&dummy, 0, sizeof(dummy)); return &dummy; }
        if (L->n) memcpy(ni, L->items, L->n * sizeof(dl_item_t));
        L->items = ni;
        L->cap = nc;
    }
    dl_item_t* it = &L->items[L->n++];
    memset(it, 0, sizeof(*it));
    it->kind = (uint8_t)kind;
    return it;
}

static void rect(ctx_t* C, int x, int y, int w, int h, uint32_t color, dom_node_t* node) {
    if (w <= 0 || h <= 0) return;
    dl_item_t* r = push(C, DL_RECT);
    r->x = x; r->y = y; r->w = w; r->h = h;
    r->color = color;
    r->node = node;
}

static void borders(ctx_t* C, const style_t* st, int x, int y, int w, int h, dom_node_t* node) {
    if (st->radius > 0 && st->border[0] && st->border[0] == st->border[1] && st->border[0] == st->border[2] &&
        st->border[0] == st->border[3] && w > 0 && h > 0) {
        dl_item_t* r = push(C, DL_RECT);
        r->x = x; r->y = y; r->w = w; r->h = h;
        r->color = st->border_color[0];
        r->radius = (int16_t)(st->radius > 30000 ? 30000 : st->radius);
        r->ring = (uint8_t)(st->border[0] > 255 ? 255 : st->border[0]);
        r->node = node;
        return;
    }
    if (st->border[0]) rect(C, x, y, w, st->border[0], st->border_color[0], node);
    if (st->border[2]) rect(C, x, y + h - st->border[2], w, st->border[2], st->border_color[2], node);
    if (st->border[3]) rect(C, x, y, st->border[3], h, st->border_color[3], node);
    if (st->border[1]) rect(C, x + w - st->border[1], y, st->border[1], h, st->border_color[1], node);
}

/* ══ text ═════════════════════════════════════════════════════════════ */

uint32_t text_to_ascii(const char* s, uint32_t n, char* out) {
    uint32_t o = 0;
    for (uint32_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x80) { out[o++] = (c < 32 && c != '\n' && c != '\t') ? ' ' : (char)c; continue; }
        uint32_t cp = 0;
        int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
        cp = c & (0x3F >> extra);
        for (int k = 0; k < extra && i + 1 < n; k++) cp = (cp << 6) | ((unsigned char)s[++i] & 0x3F);
        const char* r = "?";
        switch (cp) {
        case 0xA0: r = " "; break;
        case 0xA9: r = "(c)"; break;
        case 0xAE: r = "(R)"; break;
        case 0x2122: r = "TM"; break;
        case 0xAB: r = "<<"; break;
        case 0xBB: r = ">>"; break;
        case 0xB0: r = "o"; break;
        case 0xB7: case 0x2022: case 0x25CF: r = "*"; break;
        case 0xD7: r = "x"; break;
        case 0xF7: r = "/"; break;
        case 0x2013: case 0x2014: case 0x2212: r = "-"; break;
        case 0x2018: case 0x2019: case 0x201A: case 0x2032: r = "'"; break;
        case 0x201C: case 0x201D: case 0x201E: r = "\""; break;
        case 0x2026: r = "..."; break;
        case 0x20AC: r = "EUR"; break;
        case 0xA3: r = "GBP"; break;
        case 0x2190: r = "<-"; break;
        case 0x2192: r = "->"; break;
        case 0x2191: r = "^"; break;
        case 0x2193: r = "v"; break;
        case 0x2665: r = "<3"; break;
        case 0x2713: case 0x2714: r = "v"; break;
        case 0xDF: r = "ss"; break;
        default:
            if ((cp >= 0xE0 && cp <= 0xE5) || cp == 0xAA) r = "a";
            else if (cp >= 0xC0 && cp <= 0xC5) r = "A";
            else if (cp >= 0xE8 && cp <= 0xEB) r = "e";
            else if (cp >= 0xC8 && cp <= 0xCB) r = "E";
            else if (cp >= 0xEC && cp <= 0xEF) r = "i";
            else if (cp >= 0xCC && cp <= 0xCF) r = "I";
            else if ((cp >= 0xF2 && cp <= 0xF6) || cp == 0xF8) r = "o";
            else if ((cp >= 0xD2 && cp <= 0xD6) || cp == 0xD8) r = "O";
            else if (cp >= 0xF9 && cp <= 0xFC) r = "u";
            else if (cp >= 0xD9 && cp <= 0xDC) r = "U";
            else if (cp == 0xE7) r = "c";
            else if (cp == 0xC7) r = "C";
            else if (cp == 0xF1) r = "n";
            else if (cp == 0xD1) r = "N";
            else if (cp == 0xFD || cp == 0xFF) r = "y";
            else if (cp == 0xFE0F || cp == 0x200B || cp == 0x200D || cp == 0xFEFF) r = "";
        }
        while (*r) out[o++] = *r++;
    }
    return o;
}

/* ══ inline formatting ════════════════════════════════════════════════ */

typedef struct {
    ctx_t*   C;
    int      x0, w;             /* line box */
    int      y;                 /* top of the current line */
    int      cx;                /* x used on the current line */
    int      line_h;
    uint32_t first;             /* first display item of the line */
    int      align;
    int      space;             /* a collapsed space is pending */
    int      empty;             /* nothing placed yet at all */
    int      height;            /* total height so far */
    int      lines;
} inl_t;

static int line_height(int scale) { return GLYPH * scale + 4 * scale; }

static void finish_line(inl_t* I, int forced) {
    if (I->line_h == 0 && forced) I->line_h = line_height(1);
    if (I->line_h == 0) return;
    ctx_t* C = I->C;
    if (C->max_w < I->cx) C->max_w = I->cx;
    if (!C->dry) {
        int shift = 0;
        if (I->align == ALIGN_CENTER) shift = (I->w - I->cx) / 2;
        else if (I->align == ALIGN_RIGHT) shift = I->w - I->cx;
        if (shift < 0) shift = 0;
        for (uint32_t i = I->first; i < C->L->n; i++) {
            dl_item_t* it = &C->L->items[i];
            it->x += shift;
            /* bottom-align everything on the line (text sits on a common baseline);
             * the parts of an atomic box keep their offsets within it */
            if (it->rel) {
                it->y = I->y + I->line_h - it->boxh - 1 + it->yoff;
                continue;
            }
            int pad = it->kind == DL_TEXT ? 2 * it->scale : 0;
            it->y = I->y + I->line_h - it->h - pad;
        }
    }
    I->y += I->line_h;
    I->height += I->line_h;
    I->cx = 0;
    I->line_h = 0;
    I->space = 0;
    I->lines++;
    if (!C->dry) I->first = C->L->n;
}

static void need_line_h(inl_t* I, int h) { if (I->line_h < h) I->line_h = h; }

/* a piece of text that may not be broken */
static void place_word(inl_t* I, const char* s, uint32_t n, const style_t* st, uint32_t bg, int has_bg,
                       dom_node_t* node) {
    int scale = st->scale ? st->scale : 1;
    int cw = GLYPH * scale;
    int ww = (int)n * cw;
    int sw = I->space && I->cx > 0 ? cw : 0;
    if (I->C->max_word < ww) I->C->max_word = ww;
    if (I->cx > 0 && I->cx + sw + ww > I->w && !st->nowrap && !st->pre) {
        finish_line(I, 0);
        sw = 0;
    }
    /* a word wider than the line: split it */
    while (ww > I->w && I->w >= cw && !st->pre) {
        /* the room left on this line - none (or less than nothing) when
         * something wider than the line came before: then a new line */
        int room = I->w - I->cx;
        if (room < cw) {
            if (I->cx > 0) { finish_line(I, 0); continue; }
            break;
        }
        uint32_t fit = (uint32_t)(room / cw);
        if (fit >= n) fit = n - 1;
        place_word(I, s, fit, st, bg, has_bg, node);
        s += fit;
        n -= fit;
        ww = (int)n * cw;
        finish_line(I, 0);
    }
    int same = !I->C->dry && sw && I->C->L->n > I->first && I->C->L->items[I->C->L->n - 1].node == node;
    dl_item_t* t = push(I->C, DL_TEXT);
    t->ulspace = (uint8_t)same;
    char* text = (char*)s;
    uint32_t len = n;
    if (sw && I->C->dry) {
        len = n + 1;                            /* measuring: only the length matters */
    } else if (sw) {
        /* the space joins the word, so underlines and backgrounds run on */
        text = (char*)arena_alloc(I->C->L->A, n + 2);
        text[0] = ' ';
        memcpy(text + 1, s, n);
        len = n + 1;
    }
    if ((st->uppercase || st->lowercase) && !I->C->dry) {
        char* u = (char*)arena_alloc(I->C->L->A, len + 1);
        for (uint32_t k = 0; k < len; k++) {
            char c = text[k];
            if (st->uppercase && c >= 'a' && c <= 'z') c = (char)(c - 32);
            if (st->lowercase && c >= 'A' && c <= 'Z') c = (char)(c + 32);
            u[k] = c;
        }
        text = u;
    }
    t->x = I->x0 + I->cx;
    t->w = (int)len * cw;
    t->h = cw;
    t->text = text;
    t->len = len;
    t->scale = (uint8_t)scale;
    t->color = st->color;
    t->bold = st->bold;
    t->italic = st->italic;
    t->underline = st->underline;
    t->strike = st->strike;
    t->bg = bg;
    t->has_bg = (uint8_t)has_bg;
    t->node = node;
    I->cx += sw + ww;
    I->space = 0;
    I->empty = 0;
    need_line_h(I, line_height(scale));
}

/* an atomic inline box (image, form control) of w x h */
static int place_box(inl_t* I, int w, int h) {
    if (I->cx > 0 && I->cx + w > I->w) finish_line(I, 0);
    int x = I->x0 + I->cx;
    I->cx += w;
    I->space = 0;
    I->empty = 0;
    need_line_h(I, h + 2);
    if (I->C->max_word < w) I->C->max_word = w;
    return x;
}

/* measuring passes (C->dry) keep nothing: their text goes into one
 * reusable buffer, not the layout arena (nested flex boxes and tables
 * measure their content many times over) */
static char*    g_dry_buf;
static uint32_t g_dry_cap;

static char* dry_buffer(uint32_t need) {
    if (need > g_dry_cap) {
        uint32_t cap = need < 4096 ? 4096 : need * 2;
        char* b = (char*)kmalloc(cap);
        if (!b) return NULL;
        if (g_dry_buf) kfree(g_dry_buf);
        g_dry_buf = b;
        g_dry_cap = cap;
    }
    return g_dry_buf;
}

static void text_run(inl_t* I, const char* s, uint32_t n, const style_t* st, uint32_t bg, int has_bg, dom_node_t* node) {
    ctx_t* C = I->C;
    char* a = C->dry && !st->pre ? dry_buffer(n * 3 + 1) : NULL;
    if (!a) a = (char*)arena_alloc(C->L->A, n * 3 + 1);
    n = text_to_ascii(s, n, a);
    if (st->pre) {
        uint32_t i = 0;
        while (i <= n) {
            uint32_t j = i;
            while (j < n && a[j] != '\n') j++;
            if (j > i) {
                /* expand tabs */
                uint32_t tabs = 0;
                for (uint32_t k = i; k < j; k++) if (a[k] == '\t') tabs++;
                if (tabs) {
                    char* e = (char*)arena_alloc(C->L->A, (j - i) + tabs * 8 + 1);
                    uint32_t o = 0;
                    for (uint32_t k = i; k < j; k++) {
                        if (a[k] == '\t') { do e[o++] = ' '; while (o % 8); }
                        else e[o++] = a[k];
                    }
                    place_word(I, e, o, st, bg, has_bg, node);
                } else {
                    place_word(I, a + i, j - i, st, bg, has_bg, node);
                }
            }
            if (j >= n) break;
            finish_line(I, 1);
            i = j + 1;
        }
        return;
    }
    uint32_t i = 0;
    while (i < n) {
        if (a[i] == ' ' || a[i] == '\n' || a[i] == '\t' || a[i] == '\r') {
            if (I->cx > 0) I->space = 1;
            i++;
            continue;
        }
        uint32_t j = i;
        while (j < n && a[j] != ' ' && a[j] != '\n' && a[j] != '\t' && a[j] != '\r') j++;
        place_word(I, a + i, j - i, st, bg, has_bg, node);
        i = j;
        if (i < n) I->space = 1;
    }
}

static int layout_box(ctx_t* C, dom_node_t* e, int x, int y, int avail);
static int layout_children(ctx_t* C, dom_node_t* parent, int x, int y, int w);
static int layout_flex(ctx_t* C, dom_node_t* e, int x, int y, int w, int hgiven);
static void measure_cell(ctx_t* C, dom_node_t* cell, int* maxw, int* minw);

static uint32_t g_layout_gen = 1;

/* elements that take no room and show nothing: skip links pushed off screen,
 * "visually hidden" text, collapsed or invisible out-of-flow boxes */
static int out_of_sight(const style_t* st) {
    if (st->position == POS_ABSOLUTE || st->position == POS_FIXED) {
        if ((st->left != LEN_AUTO && st->left < -400) || (st->top != LEN_AUTO && st->top < -400)) return 1;
        if (st->clipped || !st->visible) return 1;
        if (st->width != LEN_AUTO && st->width <= 1 && !st->width_pct) return 1;
        if (st->height != LEN_AUTO && st->height <= 1) return 1;
    }
    if (st->overflow_hidden && (st->clipped || (st->height != LEN_AUTO && st->height <= 1))) return 1;
    return 0;
}

/* max-content / min-content widths, once per element per layout */
static void measure(ctx_t* C, dom_node_t* e, int* maxw, int* minw) {
    if (e->meas_gen == g_layout_gen) { *maxw = e->meas_max; *minw = e->meas_min; return; }
    measure_cell(C, e, maxw, minw);
    e->meas_gen = g_layout_gen;
    e->meas_max = *maxw;
    e->meas_min = *minw;
}

static int is_block_level(dom_node_t* e) {
    if (e->type != DOM_ELEM || !e->style) return 0;
    uint8_t d = e->style->display;
    return d == DISP_BLOCK || d == DISP_LIST_ITEM || d == DISP_TABLE || d == DISP_TABLE_ROW ||
           d == DISP_TABLE_CELL || d == DISP_TABLE_GROUP;
}

static int has_block_child(dom_node_t* e) {
    for (dom_node_t* c = e->first; c; c = c->next) {
        if (is_block_level(c)) return 1;
        if (c->type == DOM_ELEM && c->style && c->style->display == DISP_INLINE && has_block_child(c)) return 1;
    }
    return 0;
}

static void form_text(ctx_t* C, int x, int y, int w, int h, const char* s, uint32_t n, uint32_t color,
                      int scale, dom_node_t* node, int password, int center) {
    int cw = GLYPH * scale;
    int maxc = (w - 6) / cw;
    if (maxc < 0) maxc = 0;
    char* a = (char*)arena_alloc(C->L->A, n * 3 + 1);
    n = text_to_ascii(s, n, a);
    if (password) for (uint32_t i = 0; i < n; i++) a[i] = '*';
    uint32_t start = 0;
    if ((int)n > maxc) {
        if (center) n = (uint32_t)maxc;
        else start = n - (uint32_t)maxc;   /* show the end of long input */
    }
    dl_item_t* t = push(C, DL_TEXT);
    t->text = a + start;
    t->len = n - start;
    t->scale = (uint8_t)scale;
    t->color = color;
    t->w = (int)t->len * cw;
    t->h = cw;
    t->x = center ? x + (w - t->w) / 2 : x + 3;
    t->y = y + (h - cw) / 2;
    t->node = node;
    (void)node;
}

/* form controls and images: atomic boxes */
/* the items from index `from` make up one atomic box of height h: their y
 * are offsets from the box top, placed when the line is finished */
static void mark_box(ctx_t* C, uint32_t from, int h) {
    if (C->dry) return;
    for (uint32_t i = from; i < C->L->n; i++) {
        C->L->items[i].rel = 1;
        C->L->items[i].yoff = (int16_t)C->L->items[i].y;
        C->L->items[i].boxh = (int16_t)h;
    }
}

/* width="24" / "24px" / "1.5em" (an SVG's); "100%" and the like: 0 (unknown) */
static int attr_px(const char* s, const style_t* st) {
    int v = 0, frac = 0, div = 1;
    while (*s == ' ') s++;
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    if (*s == '.') { s++; while (*s >= '0' && *s <= '9' && div < 1000) { frac = frac * 10 + (*s++ - '0'); div *= 10; } }
    if (*s == '%') return 0;
    if (s[0] == 'e' && s[1] == 'm') return (v * div + frac) * (st && st->font_px ? st->font_px : 16) / div;
    return v;
}

static int str_to_px(const char* s) {
    int v = 0;
    while (*s == ' ') s++;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    return v;
}

/* form controls and images: atomic boxes */
static void inline_replaced(inl_t* I, dom_node_t* e, const style_t* st) {
    ctx_t* C = I->C;
    int scale = st->scale ? st->scale : 1;
    int cw = GLYPH * scale;
    const char* tag = e->tag;
    uint32_t first = C->dry ? 0 : C->L->n;
    if (strcmp(tag, "img") == 0 || strcmp(tag, "svg") == 0) {
        const char* aw = dom_attr(e, "width");
        const char* ah = dom_attr(e, "height");
        int w = aw ? attr_px(aw, st) : 0, h = ah ? attr_px(ah, st) : 0;
        if (tag[0] == 's' && ((aw && w == 0 && aw[0] == '0') || (ah && h == 0 && ah[0] == '0'))) return;   /* a hidden sprite sheet */
        if (st->width != LEN_AUTO && st->width > 0) w = st->width;
        if (st->height != LEN_AUTO && st->height > 0) h = st->height;
        img_data_t* img = e->img;
        int have = img && !img->failed && img->w > 0 && img->h > 0;
        if (have) {
            if (!w && !h) { w = img->w; h = img->h; }
            else if (!w) w = img->w * h / img->h;
            else if (!h) h = img->h * w / img->w;
        }
        if (st->width_pct) {
            w = I->w * st->width_pct / 100;
            if (have) h = img->h * w / img->w;
        }
        if (st->max_width != LEN_AUTO && st->max_width > 0 && w > st->max_width) { if (w) h = h * st->max_width / w; w = st->max_width; }
        if (w > I->w && I->w > 0) { if (w) h = h * I->w / w; w = I->w; }
        const char* alt = dom_attr(e, "alt");
        if (!have && (!w || !h)) {                   /* nothing known: the alt text in a frame */
            uint32_t al = alt ? (uint32_t)strlen(alt) : 0;
            w = (int)(al ? al : 3) * 8 + 8;
            if (w > I->w) w = I->w;
            h = 16;
        }
        int x = place_box(I, w, h);
        if (have && img->px) {
            dl_item_t* it = push(C, DL_IMG);
            it->x = x; it->w = w; it->h = h;
            it->img = img;
            it->node = e;
        } else {
            rect(C, x, 0, w, h, 0xC8C8C8, e);
            rect(C, x + 1, 1, w - 2, h - 2, 0xEEEEEE, e);
            if (alt && *alt) form_text(C, x, 0, w, h, alt, (uint32_t)strlen(alt), 0x555555, 1, e, 0, 1);
        }
        mark_box(C, first, h);
        return;
    }
    const char* type = dom_attr(e, "type");
    if (!type) type = "text";
    int is_input = strcmp(tag, "input") == 0;
    if (is_input && strcasecmp(type, "hidden") == 0) return;
    int h = cw + 8;
    if (is_input && (strcasecmp(type, "checkbox") == 0 || strcasecmp(type, "radio") == 0)) {
        int x = place_box(I, 12 + 4, 12);
        rect(C, x, 0, 12, 12, 0x707070, e);
        rect(C, x + 1, 1, 10, 10, 0xFFFFFF, e);
        if (e->form_init ? e->checked : dom_attr(e, "checked") != NULL) rect(C, x + 3, 3, 6, 6, 0x202020, e);
        mark_box(C, first, 12);
        return;
    }
    if (strcmp(tag, "textarea") == 0) {
        const char* rows = dom_attr(e, "rows");
        const char* cols = dom_attr(e, "cols");
        int r = rows ? str_to_px(rows) : 3, c = cols ? str_to_px(cols) : 30;
        if (r < 1) r = 1;
        if (c < 4) c = 4;
        int w = c * cw + 8, hh = r * (cw + 4) + 6;
        if (st->width != LEN_AUTO && st->width > 0) w = st->width;
        if (w > I->w) w = I->w;
        int x = place_box(I, w, hh);
        rect(C, x, 0, w, hh, 0x808080, e);
        rect(C, x + 1, 1, w - 2, hh - 2, 0xFFFFFF, e);
        const char* v = e->form_init ? (e->value ? e->value : "") : dom_text(C->L->A, e);
        /* one line per row, wrapped at the width */
        int perline = (w - 6) / cw;
        if (perline < 1) perline = 1;
        uint32_t vl = (uint32_t)strlen(v), pos = 0;
        for (int row = 0; row < r && pos < vl; row++) {
            uint32_t end = pos;
            while (end < vl && v[end] != '\n' && (int)(end - pos) < perline) end++;
            form_text(C, x, row * (cw + 4) + 2, w, cw + 4, v + pos, end - pos, 0x000000, scale, e, 0, 0);
            pos = end;
            if (pos < vl && v[pos] == '\n') pos++;
        }
        mark_box(C, first, hh);
        return;
    }
    int button = !is_input || strcasecmp(type, "submit") == 0 || strcasecmp(type, "button") == 0 ||
                 strcasecmp(type, "reset") == 0;
    int is_select = strcmp(tag, "select") == 0;
    if (is_select) button = 0;
    const char* label;
    if (button) {
        if (is_input) label = dom_attr(e, "value") ? dom_attr(e, "value") : (strcasecmp(type, "reset") == 0 ? "Reset" : "Submit");
        else label = dom_text(C->L->A, e);
    } else if (is_select) {
        label = "";
        for (dom_node_t* o = e->first; o; o = o->next)
            if (o->type == DOM_ELEM && strcmp(o->tag, "option") == 0) {
                if (!*label || dom_attr(o, "selected")) label = dom_text(C->L->A, o);
            }
        if (e->value) {
            /* the text of the option holding that value (values are often codes, or "") */
            label = e->value;
            for (dom_node_t* o = e->first; o; o = o->next) {
                if (o->type != DOM_ELEM || strcmp(o->tag, "option") != 0) continue;
                const char* ov = dom_attr(o, "value");
                if (ov ? strcmp(ov, e->value) == 0 : strcmp(dom_text(C->L->A, o), e->value) == 0) { label = dom_text(C->L->A, o); break; }
            }
        }
    } else {
        label = e->form_init ? (e->value ? e->value : "") : (dom_attr(e, "value") ? dom_attr(e, "value") : "");
    }
    if (button || is_select) {
        while (*label == ' ' || *label == '\n' || *label == '\t') label++;
    }
    uint32_t ll = (uint32_t)strlen(label);
    while (ll && (button || is_select) && (label[ll - 1] == ' ' || label[ll - 1] == '\n')) ll--;
    int w;
    if (button) w = (int)ll * cw + 16;
    else if (is_select) w = (int)(ll + 3) * cw + 8;
    else {
        const char* size = dom_attr(e, "size");
        int cols = size ? str_to_px(size) : 20;
        if (cols < 2) cols = 2;
        w = cols * cw + 8;
    }
    if (st->width != LEN_AUTO && st->width > 0) w = st->width;
    if (w > I->w) w = I->w;
    int x = place_box(I, w, h);
    uint32_t face = button ? (st->has_bg ? st->bg : 0xDDDDDD) : 0xFFFFFF;
    uint32_t edge = (C->L->focus == e) ? 0x3060C0 : button ? 0x777777 : 0x808080;
    rect(C, x, 0, w, h, edge, e);
    rect(C, x + 1, 1, w - 2, h - 2, face, e);
    const char* ph = (!button && !is_select && !ll && C->L->focus != e) ? dom_attr(e, "placeholder") : NULL;
    if (ph && *ph)                      /* grey hint while empty and not focused */
        form_text(C, x, 0, w, h, ph, (uint32_t)strlen(ph), 0x999999, scale, e, 0, 0);
    else
        form_text(C, x, 0, w, h, label, ll, button ? st->color : 0x000000, scale, e,
                  is_input && strcasecmp(type, "password") == 0, button);
    if (is_select) {
        dl_item_t* t = push(C, DL_TEXT);
        t->text = "v";
        t->len = 1;
        t->scale = (uint8_t)scale;
        t->x = x + w - cw - 4;
        t->w = cw;
        t->h = cw;
        t->y = (h - cw) / 2;
        t->node = e;
    }
    if (C->L->focus == e && !button && !is_select) {
        dl_item_t* caret = push(C, DL_CARET);
        int maxc = (w - 6) / cw;
        int tl = (int)ll < maxc ? (int)ll : maxc;
        caret->x = x + 3 + tl * cw;
        caret->w = 1;
        caret->h = cw + 2;
        caret->y = (h - cw) / 2 - 1;
        caret->color = 0x000000;
    }
    mark_box(C, first, h);
}

static void inline_node(inl_t* I, dom_node_t* n, uint32_t bg, int has_bg);

static void inline_children(inl_t* I, dom_node_t* e, uint32_t bg, int has_bg) {
    for (dom_node_t* c = e->first; c; c = c->next) inline_node(I, c, bg, has_bg);
}

static void inline_node(inl_t* I, dom_node_t* n, uint32_t bg, int has_bg) {
    ctx_t* C = I->C;
    if (n->type == DOM_TEXT) {
        dom_node_t* p = n->parent;
        const style_t* st = p && p->style ? p->style : NULL;
        if (!st || !st->visible) return;
        text_run(I, n->text, n->text_len, st, bg, has_bg, p);
        return;
    }
    if (n->type != DOM_ELEM || !n->style || n->style->display == DISP_NONE) return;
    const style_t* st = n->style;
    const char* tag = n->tag;
    if (strcmp(tag, "br") == 0) {
        need_line_h(I, line_height(st->scale));
        finish_line(I, 1);
        return;
    }
    if (out_of_sight(st)) return;
    if (st->position == POS_ABSOLUTE || st->position == POS_FIXED) { defer_abs(C, n, I->x0 + I->cx, I->y); return; }
    n->box_x = I->x0 + I->cx;
    n->box_y = I->y;
    int replaced = strcmp(tag, "img") == 0 || strcmp(tag, "svg") == 0 || strcmp(tag, "input") == 0 || strcmp(tag, "button") == 0 ||
                   strcmp(tag, "select") == 0 || strcmp(tag, "textarea") == 0;
    if (replaced) {
        if (!st->visible) return;
        if (I->space && I->cx > 0) { I->cx += GLYPH * st->scale; I->space = 0; }
        inline_replaced(I, n, st);
        return;
    }
    if (st->display == DISP_INLINE_BLOCK || st->display == DISP_TABLE) {
        /* an atomic box: shrink-to-fit width, laid out where it lands on the line */
        int ml = st->margin[3], mr = st->margin[1];
        int extra = st->padding[1] + st->padding[3] + st->border[1] + st->border[3];
        int bw;
        if (st->width_pct) bw = I->w * st->width_pct / 100;
        else if (st->width != LEN_AUTO && st->width > 0) bw = st->width + extra;
        else {
            int maxw, minw;
            measure(C, n, &maxw, &minw);
            bw = maxw;
            if (bw > I->w - ml - mr) bw = I->w - ml - mr;
            if (bw < minw) bw = minw;
        }
        if (st->max_width != LEN_AUTO && st->max_width > 0 && bw > st->max_width + extra) bw = st->max_width + extra;
        if (bw < extra + 1) bw = extra + 1;
        int total = bw + ml + mr;
        if (I->space && I->cx > 0) { I->cx += GLYPH * st->scale; I->space = 0; }
        if (I->cx > 0 && I->cx + total > I->w) finish_line(I, 0);
        int x = I->x0 + I->cx;
        uint32_t first = C->dry ? 0 : C->L->n;
        int h = layout_box(C, n, x, 0, total);
        mark_box(C, first, h);
        I->cx += total;
        I->space = 0;
        I->empty = 0;
        need_line_h(I, h);
        if (C->max_word < total) C->max_word = total;
        if (C->max_w < I->cx) C->max_w = I->cx;
        return;
    }
    if (st->has_bg) { bg = st->bg; has_bg = 1; }
    (void)C;
    inline_children(I, n, bg, has_bg);
    n->box_w = I->x0 + I->cx - n->box_x;
    n->box_h = I->line_h;
}

/* ══ blocks ═══════════════════════════════════════════════════════════ */

/* inline content of a block, from child `from` until the next block-level child */
static dom_node_t* run_inline(ctx_t* C, dom_node_t* parent, dom_node_t* from, int x, int y, int w, int* height) {
    inl_t I;
    memset(&I, 0, sizeof(I));
    I.C = C;
    I.x0 = x;
    I.w = w;
    I.y = y;
    I.align = parent->style ? parent->style->align : ALIGN_LEFT;
    I.empty = 1;
    I.first = C->dry ? 0 : C->L->n;
    dom_node_t* c = from;
    for (; c; c = c->next) {
        if (is_block_level(c)) break;
        if (c->type == DOM_ELEM && c->style && c->style->display == DISP_INLINE && has_block_child(c)) break;
        inline_node(&I, c, 0, 0);
    }
    finish_line(&I, 0);
    *height = I.height;
    return c;
}

static int list_ordinal(dom_node_t* li) {
    int n = 1;
    dom_node_t* p = li->parent;
    if (p && p->type == DOM_ELEM) {
        const char* start = dom_attr(p, "start");
        if (start) n = (int)str_to_px(start);
    }
    for (dom_node_t* s = li->prev; s; s = s->prev)
        if (s->type == DOM_ELEM && s->style && s->style->display == DISP_LIST_ITEM) n++;
    return n;
}

static int layout_table(ctx_t* C, dom_node_t* t, int x, int y, int avail);

/* children of a block: inline runs and blocks, with margin collapsing between blocks */
static int layout_children(ctx_t* C, dom_node_t* parent, int x, int y, int w) {
    int cy = y;
    int prev_mb = 0;
    dom_node_t* c = parent->first;
    while (c) {
        if (is_out_of_flow(c) && c->style->display != DISP_NONE && (is_block_level(c) || has_block_child(c))) {
            if (!out_of_sight(c->style)) defer_abs(C, c, x, cy);
            c = c->next;
            continue;
        }
        if (is_block_level(c) || (c->type == DOM_ELEM && c->style && c->style->display == DISP_INLINE && has_block_child(c))) {
            if (c->style->display == DISP_NONE) { c = c->next; continue; }
            int mt = c->style->margin[0];
            int overlap = (prev_mb > 0 && mt > 0) ? (prev_mb < mt ? prev_mb : mt) : 0;
            int h = layout_box(C, c, x, cy - overlap, w);
            cy += h - overlap;
            prev_mb = c->style->margin[2];
            c = c->next;
            continue;
        }
        int h = 0;
        dom_node_t* next = run_inline(C, parent, c, x, cy, w, &h);
        if (h > 0) prev_mb = 0;
        cy += h;
        if (next == c) c = c->next;                  /* safety */
        else c = next;
    }
    return cy - y;
}

/* ══ flexbox ═══════════════════════════════════════════════════════════
 * display: flex - the items are measured (max-content, or flex-basis /
 * width), put on lines (flex-wrap), grown or shrunk to fill each line
 * (flex-grow / flex-shrink), spaced (justify-content, gap) and aligned
 * across (align-items / align-self, stretch by default). Rows and columns,
 * both also reversed. */

#define FLEX_MAX 96

static int is_blank_text(const dom_node_t* t) {
    for (uint32_t i = 0; i < t->text_len; i++) {
        char c = t->text[i];
        if (c != ' ' && c != '\n' && c != '\t' && c != '\r') return 0;
    }
    return 1;
}

/* form controls, images and plain inline elements are flex items too, but
 * they are laid out by the inline code (as one atomic run) */
static int flex_is_inline(const dom_node_t* e) {
    const char* t = e->tag;
    if (strcmp(t, "img") == 0 || strcmp(t, "svg") == 0 || strcmp(t, "input") == 0 || strcmp(t, "button") == 0 ||
        strcmp(t, "select") == 0 || strcmp(t, "textarea") == 0) return 1;
    return e->style->display == DISP_INLINE && !has_block_child((dom_node_t*)e);
}

static int flex_inline_item(ctx_t* C, dom_node_t* it, int x, int y, int w) {
    inl_t I;
    memset(&I, 0, sizeof(I));
    I.C = C;
    I.x0 = x;
    I.w = w;
    I.y = y;
    I.align = ALIGN_LEFT;
    I.empty = 1;
    I.first = C->dry ? 0 : C->L->n;
    inline_node(&I, it, 0, 0);
    finish_line(&I, 0);
    return I.height;
}

/* its width (as one line, at most avail) and height, measuring only */
static void flex_inline_size(ctx_t* C, dom_node_t* it, int avail, int* w, int* h) {
    int saved = C->dry, saved_w = C->max_w;
    C->dry = 1;
    C->max_w = 0;
    *h = flex_inline_item(C, it, 0, 0, avail);
    *w = C->max_w > avail ? avail : C->max_w;
    C->dry = saved;
    C->max_w = saved_w;
}

/* an item's outer height laid out at border-box width bw (measuring only) */
static int flex_measure_h(ctx_t* C, dom_node_t* it, int bw) {
    const style_t* s = it->style;
    if (it->mh_gen == g_layout_gen && it->mh_w == bw) return it->mh;
    int saved = C->dry;
    C->dry = 1;
    C->force_w = bw;
    int h = layout_box(C, it, 0, 0, bw + s->margin[1] + s->margin[3]);
    C->dry = saved;
    it->mh_gen = g_layout_gen;
    it->mh_w = bw;
    it->mh = h;
    return h;
}

static int box_extra_w(const style_t* s) { return s->padding[1] + s->padding[3] + s->border[1] + s->border[3]; }
static int box_extra_h(const style_t* s) { return s->padding[0] + s->padding[2] + s->border[0] + s->border[2]; }

/* main-axis start and spacing for justify-content */
static void flex_justify(int mode, int free, int n, int* start, int* spacing) {
    *start = 0;
    *spacing = 0;
    if (free <= 0 || n <= 0) return;
    switch (mode) {
    case JUSTIFY_CENTER:  *start = free / 2; break;
    case JUSTIFY_END:     *start = free; break;
    case JUSTIFY_BETWEEN: *spacing = n > 1 ? free / (n - 1) : 0; break;
    case JUSTIFY_AROUND:  *spacing = free / n; *start = *spacing / 2; break;
    case JUSTIFY_EVENLY:  *spacing = free / (n + 1); *start = *spacing; break;
    default: break;
    }
}

static int layout_flex(ctx_t* C, dom_node_t* e, int x, int y, int w, int hgiven) {
    const style_t* st = e->style;
    dom_node_t* items[FLEX_MAX];
    dom_node_t* outside[16];
    int n = 0, nout = 0;
    for (dom_node_t* c = e->first; c; c = c->next) {
        if (c->type == DOM_TEXT) {
            if (!is_blank_text(c)) return layout_children(C, e, x, y, w);   /* loose text: normal flow */
            continue;
        }
        if (c->type != DOM_ELEM || !c->style || c->style->display == DISP_NONE || out_of_sight(c->style)) continue;
        if (c->style->position == POS_ABSOLUTE || c->style->position == POS_FIXED) {
            if (nout < 16) outside[nout++] = c;      /* out of the flow: drawn at the top left */
            continue;
        }
        if (n < FLEX_MAX) items[n++] = c;
    }
    int col = st->flex_dir == FLEX_COL || st->flex_dir == FLEX_COL_REV;
    int rev = st->flex_dir == FLEX_ROW_REV || st->flex_dir == FLEX_COL_REV;
    int gap_main = col ? st->gap_row : st->gap_col, gap_cross = col ? st->gap_col : st->gap_row;
    int total = 0;

    int mainsz[FLEX_MAX], crossz[FLEX_MAX], basez[FLEX_MAX], minz[FLEX_MAX];   /* per call: flex boxes nest */
    uint8_t inl[FLEX_MAX];
    for (int i = 0; i < n; i++) inl[i] = (uint8_t)flex_is_inline(items[i]);
    if (!col) {
        /* rows: base widths (border boxes) */
        for (int i = 0; i < n; i++) {
            const style_t* s = items[i]->style;
            if (inl[i]) {
                int iw, ih;
                flex_inline_size(C, items[i], w, &iw, &ih);
                basez[i] = minz[i] = iw;
                continue;
            }
            int maxw, minw;
            measure(C, items[i], &maxw, &minw);
            int b;
            if (s->flex_basis != LEN_AUTO && s->flex_basis >= 0) b = s->flex_basis + box_extra_w(s);
            else if (s->width_pct) b = w * s->width_pct / 100;
            else if (s->width != LEN_AUTO && s->width > 0) b = s->width + box_extra_w(s);
            else b = maxw;
            if (s->max_width != LEN_AUTO && s->max_width > 0 && b > s->max_width + box_extra_w(s)) b = s->max_width + box_extra_w(s);
            basez[i] = b < 0 ? 0 : b;
            minz[i] = (s->width != LEN_AUTO && s->width > 0) ? basez[i] : minw;
            if (minz[i] > basez[i] && s->flex_basis != LEN_AUTO) minz[i] = basez[i];
        }
        int start = 0, cy = y;
        while (start < n) {
            int end = start, sum = 0;
            while (end < n) {
                const style_t* s = items[end]->style;
                int add = basez[end] + s->margin[1] + s->margin[3] + (end > start ? gap_main : 0);
                if (st->flex_wrap && end > start && sum + add > w) break;
                sum += add;
                end++;
            }
            int cnt = end - start;
            int free = w - sum;
            int grow = 0;
            long shrink = 0;
            for (int i = start; i < end; i++) { grow += items[i]->style->flex_grow; shrink += (long)items[i]->style->flex_shrink * basez[i]; }
            for (int i = start; i < end; i++) {
                const style_t* s = items[i]->style;
                int m = basez[i];
                if (free > 0 && grow > 0) m += (int)((long)free * s->flex_grow / grow);
                else if (free < 0 && shrink > 0) {
                    m -= (int)((long)(-free) * s->flex_shrink * basez[i] / shrink);
                    if (m < minz[i]) m = minz[i];
                }
                mainsz[i] = m < 1 ? 1 : m;
                if (inl[i]) { int iw; flex_inline_size(C, items[i], mainsz[i], &iw, &crossz[i]); }
                else crossz[i] = flex_measure_h(C, items[i], mainsz[i]);
            }
            int line = 0;
            for (int i = start; i < end; i++) if (crossz[i] > line) line = crossz[i];
            if (!st->flex_wrap && hgiven > line) line = hgiven;
            int used = 0;
            for (int i = start; i < end; i++) used += mainsz[i] + items[i]->style->margin[1] + items[i]->style->margin[3];
            used += gap_main * (cnt - 1);
            int off, spacing;
            flex_justify(st->justify, w - used, cnt, &off, &spacing);
            /* row-reverse: the main axis starts at the right edge */
            int pos = rev ? x + w - off : x + off;
            for (int k = 0; k < cnt; k++) {
                int i = start + k;
                const style_t* s = items[i]->style;
                int al = s->align_self ? s->align_self : st->align_items ? st->align_items : FA_STRETCH;
                int iy = cy;
                if (al == FA_CENTER) iy += (line - crossz[i]) / 2;
                else if (al == FA_END) iy += line - crossz[i];
                if (al == FA_STRETCH && s->height == LEN_AUTO && !inl[i]) C->force_h = line - s->margin[0] - s->margin[2];
                int outer = mainsz[i] + s->margin[1] + s->margin[3];
                if (rev) pos -= outer;
                if (inl[i]) flex_inline_item(C, items[i], pos, iy, outer);
                else {
                    C->force_w = mainsz[i];
                    layout_box(C, items[i], pos, iy, outer);
                }
                if (rev) pos -= gap_main + spacing;
                else pos += outer + gap_main + spacing;
            }
            cy += line + gap_cross;
            start = end;
        }
        total = cy - y - (n ? gap_cross : 0);
    } else {
        /* columns: widths across, natural heights down */
        int sum = 0;
        for (int i = 0; i < n; i++) {
            const style_t* s = items[i]->style;
            int al = s->align_self ? s->align_self : st->align_items ? st->align_items : FA_STRETCH;
            int bw;
            if (inl[i]) {
                int ih;
                flex_inline_size(C, items[i], w, &crossz[i], &ih);
                mainsz[i] = ih;
                sum += ih + (i ? gap_main : 0);
                continue;
            }
            if (s->width_pct) bw = w * s->width_pct / 100;
            else if (s->width != LEN_AUTO && s->width > 0) bw = s->width + box_extra_w(s);
            else if (al == FA_STRETCH) bw = w - s->margin[1] - s->margin[3];
            else { int maxw, minw; measure(C, items[i], &maxw, &minw); bw = maxw < w ? maxw : w; }
            if (bw < 1) bw = 1;
            crossz[i] = bw;
            int h = flex_measure_h(C, items[i], bw);
            if (s->flex_basis != LEN_AUTO && s->flex_basis >= 0) {
                int bh = s->flex_basis + box_extra_h(s) + s->margin[0] + s->margin[2];
                if (bh > h) h = bh;
            }
            mainsz[i] = h;
            sum += h + (i ? gap_main : 0);
        }
        int free = hgiven > 0 ? hgiven - sum : 0;
        int grow = 0;
        for (int i = 0; i < n; i++) grow += items[i]->style->flex_grow;
        if (free > 0 && grow > 0) {
            for (int i = 0; i < n; i++) mainsz[i] += (int)((long)free * items[i]->style->flex_grow / grow);
            free = 0;
        }
        int off, spacing;
        flex_justify(st->justify, free, n, &off, &spacing);
        int cy = y + off;
        for (int k = 0; k < n; k++) {
            int i = rev ? n - 1 - k : k;
            const style_t* s = items[i]->style;
            int al = s->align_self ? s->align_self : st->align_items ? st->align_items : FA_STRETCH;
            int outer = crossz[i] + s->margin[1] + s->margin[3];
            int ix = x;
            if (al == FA_CENTER) ix += (w - outer) / 2;
            else if (al == FA_END) ix += w - outer;
            if (inl[i]) flex_inline_item(C, items[i], ix, cy, outer > w ? w : outer);
            else {
                C->force_w = crossz[i];
                if (s->height == LEN_AUTO) C->force_h = mainsz[i] - s->margin[0] - s->margin[2];
                layout_box(C, items[i], ix, cy, outer);
            }
            cy += mainsz[i] + gap_main + spacing;
        }
        total = cy - y - (n ? gap_main + spacing : 0);
        if (hgiven > total) total = hgiven;
    }
    for (int i = 0; i < nout; i++) defer_abs(C, outside[i], x, y);
    return total < 0 ? 0 : total;
}

/* ══ positioning ═══════════════════════════════════════════════════════ */

static void layout_abs(ctx_t* C, dom_node_t* e, int sx, int sy, int cbx, int cby, int cbw, int cbh) {
    const style_t* s = e->style;
    int ml = s->margin[3], mr = s->margin[1], mt = s->margin[0], mb = s->margin[2];
    int L = s->left, R = s->right, T = s->top, B = s->bottom;
    int bw;
    if (s->width_pct) bw = cbw * s->width_pct / 100;
    else if (s->width != LEN_AUTO && s->width > 0) bw = s->width + box_extra_w(s);
    else if (L != LEN_AUTO && R != LEN_AUTO) bw = cbw - L - R - ml - mr;
    else {                                      /* shrink to fit */
        int maxw, minw;
        measure(C, e, &maxw, &minw);
        bw = maxw;
        if (bw > cbw - ml - mr) bw = cbw - ml - mr;
        if (bw < minw) bw = minw;
    }
    if (bw < 1) bw = 1;
    int x = L != LEN_AUTO ? cbx + L : R != LEN_AUTO ? cbx + cbw - R - bw - ml - mr : sx;
    int y;
    if (T != LEN_AUTO) y = cby + T;
    else if (B != LEN_AUTO) y = cby + cbh - B - flex_measure_h(C, e, bw);
    else y = sy;
    if (T != LEN_AUTO && B != LEN_AUTO && s->height == LEN_AUTO) C->force_h = cbh - T - B - mt - mb;
    C->force_w = bw;
    layout_box(C, e, x, y, bw + ml + mr);
}

/* the absolute boxes queued since `from`, inside the current containing block */
static void place_abs(ctx_t* C, int from) {
    for (int i = from; i < C->nabs; i++)
        layout_abs(C, C->absq[i].n, C->absq[i].sx, C->absq[i].sy, C->cb_x, C->cb_y, C->cb_w, C->cb_h);
    C->nabs = from;
}

static int layout_box(ctx_t* C, dom_node_t* e, int x, int y, int avail) {
    WEB_TICK();
    int force_w = C->force_w, force_h = C->force_h;   /* set by a flex container for this box only */
    C->force_w = C->force_h = 0;
    const style_t* st = e->style;
    if (!st || st->display == DISP_NONE || out_of_sight(st)) return 0;
    /* a form control or image styled display:block is still drawn as one
     * (its options/children are not content) - on a line of its own */
    {
        const char* t = e->tag;
        if (strcmp(t, "select") == 0 || strcmp(t, "input") == 0 || strcmp(t, "button") == 0 ||
            strcmp(t, "textarea") == 0 || strcmp(t, "img") == 0 || strcmp(t, "svg") == 0)
            return flex_inline_item(C, e, x + st->margin[3], y + st->margin[0], avail - st->margin[1] - st->margin[3]) +
                   st->margin[0] + st->margin[2];
    }
    if (st->display == DISP_TABLE) return layout_table(C, e, x, y, avail);
    int ml = st->margin[3], mr = st->margin[1], mt = st->margin[0], mb = st->margin[2];
    int bl = st->border[3], br = st->border[1], bt = st->border[0], bb = st->border[2];
    int pl = st->padding[3], pr = st->padding[1], pt = st->padding[0], pb = st->padding[2];
    int cw;
    int explicit_w = 0;
    if (st->width_pct) { cw = avail * st->width_pct / 100 - (bl + br + pl + pr); explicit_w = 1; }
    else if (st->width != LEN_AUTO && st->width > 0) { cw = st->width; explicit_w = 1; }
    else cw = avail - ml - mr - bl - br - pl - pr;
    if (st->max_width != LEN_AUTO && st->max_width > 0 && cw > st->max_width) { cw = st->max_width; explicit_w = 1; }
    if (force_w > 0) { cw = force_w - (bl + br + pl + pr); explicit_w = 1; }
    if (cw < 8) cw = 8;
    int bw = cw + bl + br + pl + pr;
    if (explicit_w && st->margin_auto_lr && bw < avail) ml = (avail - bw) / 2;
    if (bw > avail - ml && !explicit_w) bw = avail - ml;
    int bx = x + ml, by = y + mt;
    uint32_t bg_index = C->dry ? 0 : C->L->n;
    int body_like = strcmp(e->tag, "body") == 0 || strcmp(e->tag, "html") == 0;
    if (st->has_bg && !body_like && st->visible) {
        rect(C, bx, by, bw, 1, st->bg, e);
        if (!C->dry && C->L->n) C->L->items[C->L->n - 1].radius = (int16_t)(st->radius > 30000 ? 30000 : st->radius);
    }
    /* gradient bands and the background image: reserved now (under the
     * content), sized once the box height is known */
    #define GRAD_BANDS 24
    uint32_t grad_index = 0, bgimg_index = 0;
    int has_grad = st->bg_grad && st->visible && !C->dry;
    int has_bgimg = e->bg_img && !e->bg_img->failed && e->bg_img->px && st->visible && !C->dry;
    if (has_grad) {
        grad_index = C->L->n;
        for (int k = 0; k < GRAD_BANDS; k++) rect(C, bx, by, 1, 1, st->grad_from, e);
    }
    if (has_bgimg) {
        bgimg_index = C->L->n;
        dl_item_t* it = push(C, DL_IMG);
        it->img = e->bg_img;
        it->node = e;
    }
    /* overflow: hidden - the children stay inside the padding box */
    int clip_saved = C->clip_on, csx0 = C->clip_x0, csy0 = C->clip_y0, csx1 = C->clip_x1, csy1 = C->clip_y1;
    if (st->overflow_hidden && !C->dry && !body_like) {
        int x0 = bx + bl, y0 = by + bt, x1 = bx + bw - br;
        int y1 = st->height != LEN_AUTO && st->height > 0 ? by + bt + pt + st->height + pb : 0x3FFFFFFF;
        if (C->clip_on) {
            if (x0 < C->clip_x0) x0 = C->clip_x0;
            if (y0 < C->clip_y0) y0 = C->clip_y0;
            if (x1 > C->clip_x1) x1 = C->clip_x1;
            if (y1 > C->clip_y1) y1 = C->clip_y1;
        }
        C->clip_on = 1;
        C->clip_x0 = x0; C->clip_y0 = y0; C->clip_x1 = x1; C->clip_y1 = y1;
    }
    int content_y = by + bt + pt;
    int positioned = !C->dry && st->position != POS_STATIC;
    int abs_from = C->nabs;
    uint32_t first_item = C->dry ? 0 : C->L->n;
    int hgiven = st->height != LEN_AUTO && st->height > 0 ? st->height : force_h > 0 ? force_h - (bt + pt + pb + bb) : 0;
    int ch = st->flex ? layout_flex(C, e, bx + bl + pl, content_y, cw, hgiven) : layout_children(C, e, bx + bl + pl, content_y, cw);
    if (st->height != LEN_AUTO && st->height > 0) ch = st->height;
    else if (force_h > 0 && force_h - (bt + pt + pb + bb) > ch) ch = force_h - (bt + pt + pb + bb);   /* stretched */
    int bh = bt + pt + ch + pb + bb;
    if (st->has_bg && !body_like && st->visible && !C->dry) C->L->items[bg_index].h = bh;
    C->clip_on = clip_saved; C->clip_x0 = csx0; C->clip_y0 = csy0; C->clip_x1 = csx1; C->clip_y1 = csy1;
    if (has_grad) {
        /* bands across the padding box, from one colour to the other */
        int vertical = st->grad_dir == GRAD_DOWN || st->grad_dir == GRAD_UP;
        int len = vertical ? bh : bw;
        for (int k = 0; k < GRAD_BANDS; k++) {
            dl_item_t* it = &C->L->items[grad_index + (uint32_t)k];
            int a = len * k / GRAD_BANDS, b = len * (k + 1) / GRAD_BANDS;
            int t = (k * 2 + 1) * 128 / GRAD_BANDS;            /* 0..255 */
            if (st->grad_dir == GRAD_UP || st->grad_dir == GRAD_LEFT) t = 255 - t;
            uint32_t f = st->grad_from, g = st->grad_to;
            uint32_t r = (((f >> 16) & 255) * (uint32_t)(255 - t) + ((g >> 16) & 255) * (uint32_t)t) / 255;
            uint32_t gg = (((f >> 8) & 255) * (uint32_t)(255 - t) + ((g >> 8) & 255) * (uint32_t)t) / 255;
            uint32_t bb2 = ((f & 255) * (uint32_t)(255 - t) + (g & 255) * (uint32_t)t) / 255;
            it->color = r << 16 | gg << 8 | bb2;
            if (vertical) { it->x = bx; it->w = bw; it->y = by + a; it->h = b - a; }
            else { it->x = bx + a; it->w = b - a; it->y = by; it->h = bh; }
        }
    }
    if (has_bgimg) {
        dl_item_t* it = &C->L->items[bgimg_index];
        img_data_t* im = e->bg_img;
        int iw = im->w, ih = im->h;
        if (st->bg_size == BG_SIZE_COVER || st->bg_size == BG_SIZE_CONTAIN) {
            long sx = (long)bw * 1000 / (iw ? iw : 1), sy = (long)bh * 1000 / (ih ? ih : 1);
            long sc = st->bg_size == BG_SIZE_COVER ? (sx > sy ? sx : sy) : (sx < sy ? sx : sy);
            iw = (int)((long)im->w * sc / 1000);
            ih = (int)((long)im->h * sc / 1000);
        } else if (st->bg_size == BG_SIZE_PX) {
            int w2 = st->bg_size_w, h2 = st->bg_size_h;
            if (w2 != LEN_AUTO && h2 == LEN_AUTO) { h2 = im->w ? im->h * w2 / im->w : w2; }
            else if (w2 == LEN_AUTO && h2 != LEN_AUTO) { w2 = im->h ? im->w * h2 / im->h : h2; }
            if (w2 != LEN_AUTO) iw = w2;
            if (h2 != LEN_AUTO) ih = h2;
        }
        if (iw < 1) iw = 1;
        if (ih < 1) ih = 1;
        int px = st->bg_pos_pct & 1 ? (bw - iw) * st->bg_pos_x / 100 : st->bg_pos_x;
        int py = st->bg_pos_pct & 2 ? (bh - ih) * st->bg_pos_y / 100 : st->bg_pos_y;
        it->x = bx; it->y = by; it->w = bw; it->h = bh;      /* the area painted */
        it->tw = (int16_t)(iw > 30000 ? 30000 : iw);
        it->th = (int16_t)(ih > 30000 ? 30000 : ih);
        it->ox = (int16_t)px;
        it->oy = (int16_t)py;
        it->tile = (uint8_t)(st->bg_repeat == BG_REPEAT ? 3 : st->bg_repeat == BG_REPEAT_X ? 1 : st->bg_repeat == BG_REPEAT_Y ? 2 : 0) | 4;
    }
    if (positioned) {
        /* its absolute descendants are placed against its padding box */
        int scx = C->cb_x, scy = C->cb_y, scw = C->cb_w, sch = C->cb_h;
        C->cb_x = bx + bl; C->cb_y = by + bt; C->cb_w = bw - bl - br; C->cb_h = bh - bt - bb;
        place_abs(C, abs_from);
        C->cb_x = scx; C->cb_y = scy; C->cb_w = scw; C->cb_h = sch;
        if (st->position == POS_RELATIVE || st->position == POS_STICKY) {
            int dx = st->left != LEN_AUTO ? st->left : st->right != LEN_AUTO ? -st->right : 0;
            int dy = st->top != LEN_AUTO ? st->top : st->bottom != LEN_AUTO ? -st->bottom : 0;
            if (dx || dy)
                for (uint32_t i = first_item; i < C->L->n; i++) {
                    dl_item_t* it = &C->L->items[i];
                    it->x += dx; it->y += dy;
                    if (it->has_clip) { it->cx0 += dx; it->cx1 += dx; it->cy0 += dy; it->cy1 += dy; }
                }
        }
    }
    if (st->visible) borders(C, st, bx, by, bw, bh, e);
    if (st->display == DISP_LIST_ITEM && st->list_style != LIST_NONE && st->visible) {
        int scale = st->scale ? st->scale : 1;
        char m[16];
        if (st->list_style == LIST_DECIMAL) ksnprintf(m, sizeof(m), "%d.", list_ordinal(e));
        else kstrlcpy(m, st->list_style == LIST_CIRCLE ? "o" : st->list_style == LIST_SQUARE ? "#" : "*", sizeof(m));
        uint32_t ml2 = (uint32_t)strlen(m);
        dl_item_t* t = push(C, DL_TEXT);
        char* mt2 = (char*)arena_alloc(C->L->A, ml2 + 1);
        memcpy(mt2, m, ml2);
        t->text = mt2;
        t->len = ml2;
        t->scale = (uint8_t)scale;
        t->color = st->color;
        t->w = (int)ml2 * GLYPH * scale;
        t->h = GLYPH * scale;
        t->x = bx + bl + pl - t->w - 6;
        t->y = content_y + line_height(scale) - t->h - 2 * scale;
        t->node = e;
    }
    e->box_x = bx;
    e->box_y = by;
    e->box_w = bw;
    e->box_h = bh;
    if (C->max_w < ml + bw) C->max_w = ml + bw;
    return mt + bh + mb;
}

/* ══ tables ═══════════════════════════════════════════════════════════ */

#define MAX_COLS 32
#define MAX_ROWS 512

static int collect_rows(dom_node_t* t, dom_node_t** rows, int max) {
    int n = 0;
    for (dom_node_t* c = t->first; c && n < max; c = c->next) {
        if (c->type != DOM_ELEM || !c->style) continue;
        if (c->style->display == DISP_TABLE_ROW) rows[n++] = c;
        else if (c->style->display == DISP_TABLE_GROUP) n += collect_rows(c, rows + n, max - n);
    }
    return n;
}

static int span_of(dom_node_t* cell) {
    const char* s = dom_attr(cell, "colspan");
    int v = s ? (int)str_to_px(s) : 1;
    return v < 1 ? 1 : v > MAX_COLS ? MAX_COLS : v;
}

/* content widths of a cell: max-content and the widest word */
static void measure_cell(ctx_t* C, dom_node_t* cell, int* maxw, int* minw) {
    ctx_t M;
    memset(&M, 0, sizeof(M));               /* every field: forced sizes, clips, queues */
    M.L = C->L;
    M.dry = 1;
    layout_children(&M, cell, 0, 0, 100000);
    const style_t* st = cell->style;
    int extra = st->padding[1] + st->padding[3] + st->border[1] + st->border[3];
    *maxw = M.max_w + extra;
    *minw = M.max_word + extra;
    if (st->width != LEN_AUTO && st->width > 0) { *maxw = *minw = st->width + extra; }
}

static int layout_table(ctx_t* C, dom_node_t* t, int x, int y, int avail) {
    const style_t* st = t->style;
    dom_node_t* rows[MAX_ROWS];                /* on the stack: tables are measured over and over */
    int nrows = collect_rows(t, rows, MAX_ROWS);
    int ncols = 0;
    for (int r = 0; r < nrows; r++) {
        int c = 0;
        for (dom_node_t* cell = rows[r]->first; cell; cell = cell->next)
            if (cell->type == DOM_ELEM && cell->style && cell->style->display == DISP_TABLE_CELL) c += span_of(cell);
        if (c > ncols) ncols = c;
    }
    if (ncols > MAX_COLS) ncols = MAX_COLS;
    int mt = st->margin[0], mb = st->margin[2], ml = st->margin[3];
    int bl = st->border[3], br = st->border[1], bt = st->border[0], bb = st->border[2];
    int spacing = 2;
    if (dom_attr(t, "cellspacing")) spacing = (int)str_to_px(dom_attr(t, "cellspacing"));
    if (ncols == 0) return mt + bt + bb + mb;

    int colmax[MAX_COLS], colmin[MAX_COLS];
    memset(colmax, 0, sizeof(colmax));
    memset(colmin, 0, sizeof(colmin));
    for (int r = 0; r < nrows; r++) {
        int c = 0;
        for (dom_node_t* cell = rows[r]->first; cell && c < ncols; cell = cell->next) {
            if (cell->type != DOM_ELEM || !cell->style || cell->style->display != DISP_TABLE_CELL) continue;
            int sp = span_of(cell);
            int mx, mn;
            measure_cell(C, cell, &mx, &mn);
            if (sp == 1) {
                if (mx > colmax[c]) colmax[c] = mx;
                if (mn > colmin[c]) colmin[c] = mn;
            }
            c += sp;
        }
    }
    int sum_max = 0, sum_min = 0;
    for (int c = 0; c < ncols; c++) {
        if (colmin[c] < 8) colmin[c] = 8;
        if (colmax[c] < colmin[c]) colmax[c] = colmin[c];
        sum_max += colmax[c];
        sum_min += colmin[c];
    }
    int frame = bl + br + spacing * (ncols + 1);
    int tw;
    if (st->width_pct) tw = avail * st->width_pct / 100;
    else if (st->width != LEN_AUTO && st->width > 0) tw = st->width;
    else tw = sum_max + frame < avail - ml ? sum_max + frame : avail - ml;
    if (tw > avail - ml && avail - ml > 0) tw = avail - ml;
    int inner = tw - frame;
    if (inner < ncols * 8) inner = ncols * 8;
    int colw[MAX_COLS];
    if (sum_max <= inner) {
        int extra = inner - sum_max;
        for (int c = 0; c < ncols; c++) colw[c] = colmax[c] + (sum_max ? extra * colmax[c] / sum_max : extra / ncols);
    } else if (sum_min >= inner) {
        for (int c = 0; c < ncols; c++) colw[c] = inner * colmin[c] / (sum_min ? sum_min : 1);
    } else {
        int room = inner - sum_min, want = sum_max - sum_min;
        for (int c = 0; c < ncols; c++) colw[c] = colmin[c] + (want ? room * (colmax[c] - colmin[c]) / want : 0);
    }
    if (st->margin_auto_lr && tw < avail) ml = (avail - tw) / 2;
    int tx = x + ml, ty = y + mt;
    uint32_t bg_index = C->dry ? 0 : C->L->n;
    if (st->has_bg) rect(C, tx, ty, tw, 1, st->bg, t);
    int cy = ty + bt + spacing;
    for (int r = 0; r < nrows; r++) {
        dom_node_t* row = rows[r];
        uint32_t row_bg_index = C->dry ? 0 : C->L->n;
        int row_bg = row->style && row->style->has_bg;
        if (row_bg) rect(C, tx + bl, cy, tw - bl - br, 1, row->style->bg, row);
        int cx = tx + bl + spacing;
        int c = 0;
        int row_h = 0;
        uint32_t cell_bg[MAX_COLS];
        dom_node_t* cells[MAX_COLS];
        int cellx[MAX_COLS], cellw[MAX_COLS], ncells = 0;
        for (dom_node_t* cell = row->first; cell && c < ncols; cell = cell->next) {
            if (cell->type != DOM_ELEM || !cell->style || cell->style->display != DISP_TABLE_CELL) continue;
            const style_t* cs = cell->style;
            int sp = span_of(cell);
            if (c + sp > ncols) sp = ncols - c;
            int w = 0;
            for (int k = 0; k < sp; k++) w += colw[c + k] + (k ? spacing : 0);
            cell_bg[ncells] = C->dry ? 0 : C->L->n;
            if (cs->has_bg) rect(C, cx, cy, w, 1, cs->bg, cell);
            int inner_w = w - cs->padding[1] - cs->padding[3] - cs->border[1] - cs->border[3];
            int h = layout_children(C, cell, cx + cs->border[3] + cs->padding[3], cy + cs->border[0] + cs->padding[0], inner_w);
            h += cs->padding[0] + cs->padding[2] + cs->border[0] + cs->border[2];
            if (cs->height != LEN_AUTO && cs->height > h) h = cs->height;
            if (h > row_h) row_h = h;
            cells[ncells] = cell;
            cellx[ncells] = cx;
            cellw[ncells] = w;
            ncells++;
            cx += w + spacing;
            c += sp;
        }
        if (row_h == 0) row_h = line_height(1);
        if (!C->dry) {
            if (row_bg) C->L->items[row_bg_index].h = row_h;
            for (int k = 0; k < ncells; k++) {
                if (cells[k]->style->has_bg) C->L->items[cell_bg[k]].h = row_h;
                borders(C, cells[k]->style, cellx[k], cy, cellw[k], row_h, cells[k]);
                cells[k]->box_x = cellx[k];
                cells[k]->box_y = cy;
                cells[k]->box_w = cellw[k];
                cells[k]->box_h = row_h;
            }
        }
        cy += row_h + spacing;
    }
    int th = cy - ty + bb;
    if (st->has_bg && !C->dry) C->L->items[bg_index].h = th;
    borders(C, st, tx, ty, tw, th, t);
    t->box_x = tx;
    t->box_y = ty;
    t->box_w = tw;
    t->box_h = th;
    if (C->max_w < ml + tw) C->max_w = ml + tw;
    return mt + th + mb;
}

/* ══ entry points ═════════════════════════════════════════════════════ */

static int inline_elem(dom_node_t* e) {
    return e && e->type == DOM_ELEM && e->style &&
           (e->style->display == DISP_INLINE || e->style->display == DISP_INLINE_BLOCK);
}

/* inline elements get their final boxes (after line alignment) from the
 * items they produced: the union of them */
static void inline_boxes(layout_t* L) {
    for (uint32_t i = 0; i < L->n; i++)
        for (dom_node_t* e = L->items[i].node; inline_elem(e); e = e->parent) e->box_w = -1;
    for (uint32_t i = 0; i < L->n; i++) {
        dl_item_t* it = &L->items[i];
        for (dom_node_t* e = it->node; inline_elem(e); e = e->parent) {
            if (e->box_w < 0) { e->box_x = it->x; e->box_y = it->y; e->box_w = it->w; e->box_h = it->h; continue; }
            int x1 = e->box_x + e->box_w, y1 = e->box_y + e->box_h;
            if (it->x < e->box_x) e->box_x = it->x;
            if (it->y < e->box_y) e->box_y = it->y;
            if (it->x + it->w > x1) x1 = it->x + it->w;
            if (it->y + it->h > y1) y1 = it->y + it->h;
            e->box_w = x1 - e->box_x;
            e->box_h = y1 - e->box_y;
        }
    }
}

/* position: relative on inline elements (spans, links): their pieces move
 * once the lines are done (block boxes move in layout_box) */
static void inline_relative(layout_t* L) {
    for (uint32_t i = 0; i < L->n; i++) {
        dl_item_t* it = &L->items[i];
        int dx = 0, dy = 0;
        for (dom_node_t* e = it->node; e && e->type == DOM_ELEM && e->style && e->style->display == DISP_INLINE; e = e->parent) {
            const style_t* st = e->style;
            if (st->position != POS_RELATIVE && st->position != POS_STICKY) continue;
            dx += st->left != LEN_AUTO ? st->left : st->right != LEN_AUTO ? -st->right : 0;
            dy += st->top != LEN_AUTO ? st->top : st->bottom != LEN_AUTO ? -st->bottom : 0;
        }
        it->x += dx;
        it->y += dy;
    }
}

layout_t* layout_build(arena_t* A, dom_node_t* doc, int width, dom_node_t* focus) {
    g_layout_gen++;
    layout_t* L = (layout_t*)arena_alloc(A, sizeof(layout_t));
    L->A = A;
    L->width = width;
    L->bg = 0xFFFFFF;
    L->focus = focus;
    dom_node_t* html = dom_find_tag(doc, "html");
    dom_node_t* body = dom_find_tag(doc, "body");
    if (html && html->style && html->style->has_bg) L->bg = html->style->bg;
    if (body && body->style && body->style->has_bg) L->bg = body->style->bg;
    ctx_t C;
    memset(&C, 0, sizeof(C));
    C.L = L;
    int h = 0;
    if (html && html->style) h = layout_box(&C, html, 0, 0, width);
    C.cb_x = 0; C.cb_y = 0; C.cb_w = width; C.cb_h = h > 480 ? h : 480;
    place_abs(&C, 0);
    for (int i = 0; i < C.nfix; i++)
        layout_abs(&C, C.fixq[i].n, C.fixq[i].sx, C.fixq[i].sy, 0, 0, width, h > 480 ? h : 480);
    L->height = h;
    inline_relative(L);
    inline_boxes(L);
    return L;
}

dom_node_t* layout_hit(layout_t* L, int x, int y) {
    for (uint32_t i = L->n; i > 0; i--) {
        dl_item_t* it = &L->items[i - 1];
        if (!it->node) continue;
        if (x >= it->x && x < it->x + it->w && y >= it->y && y < it->y + it->h) return it->node;
    }
    return NULL;
}

/* ══ text selection ═══════════════════════════════════════════════════ */

static int char_w(const dl_item_t* it) { return 8 * (it->scale ? it->scale : 1); }

int layout_text_pos(layout_t* L, int x, int y, int* item, int* off) {
    int best = -1, best_off = 0;
    int line_hit = -1;              /* last text item on the point's line that starts left of it */
    int after = -1;                 /* first text item below the point */
    int last = -1;
    for (uint32_t i = 0; i < L->n; i++) {
        dl_item_t* it = &L->items[i];
        if (it->kind != DL_TEXT || !it->len) continue;
        last = (int)i;
        int top = it->y - 2, bottom = it->y + char_w(it) + 3;
        if (y >= top && y < bottom) {
            if (x >= it->x && x < it->x + it->w) {
                int o = (x - it->x + char_w(it) / 2) / char_w(it);
                if (o > (int)it->len) o = (int)it->len;
                best = (int)i;
                best_off = o;
                break;
            }
            if (it->x <= x) line_hit = (int)i;
            else if (after < 0 || L->items[after].y > it->y) after = (int)i;
        } else if (it->y > y && after < 0) {
            after = (int)i;
        }
    }
    if (best < 0 && line_hit >= 0) { best = line_hit; best_off = (int)L->items[line_hit].len; }
    if (best < 0 && after >= 0) { best = after; best_off = 0; }
    if (best < 0 && last >= 0) { best = last; best_off = (int)L->items[last].len; }
    if (best < 0) return 0;
    *item = best;
    *off = best_off;
    return 1;
}

uint32_t layout_text_range(layout_t* L, int i0, int o0, int i1, int o1, char* out, uint32_t cap) {
    if (i1 < i0 || (i1 == i0 && o1 < o0)) { int t = i0; i0 = i1; i1 = t; t = o0; o0 = o1; o1 = t; }
    uint32_t n = 0;
    int prev_y = -1, prev_end = 0;
    for (int i = i0; i <= i1 && i < (int)L->n; i++) {
        dl_item_t* it = &L->items[i];
        if (it->kind != DL_TEXT) continue;
        int a = i == i0 ? o0 : 0, b = i == i1 ? o1 : (int)it->len;
        if (a > (int)it->len) a = (int)it->len;
        if (b > (int)it->len) b = (int)it->len;
        if (prev_y >= 0 && n + 1 < cap) {
            if (it->y > prev_y + 2) out[n++] = '\n';              /* a new line */
            else if (it->x > prev_end + 1 && n && out[n - 1] != ' ' && (b > a && it->text[a] != ' ')) out[n++] = ' ';
        }
        for (int k = a; k < b && n + 1 < cap; k++) out[n++] = it->text[k];
        prev_y = it->y;
        prev_end = it->x + it->w;
    }
    out[n] = 0;
    return n;
}
