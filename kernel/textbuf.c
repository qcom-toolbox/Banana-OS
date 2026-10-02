#include "textbuf.h"
#include "kheap.h"
#include "kstring.h"

#define MAX_LINES 200000

static int line_reserve(tb_line_t* l, uint32_t need) {
    if (need + 1 <= l->cap) return 0;
    uint32_t cap = l->cap ? l->cap : 16;
    while (cap < need + 1) cap *= 2;
    char* s = (char*)kmalloc(cap);
    if (!s) return -1;
    if (l->s) { memcpy(s, l->s, l->len); kfree(l->s); }
    s[l->len] = 0;
    l->s = s;
    l->cap = cap;
    return 0;
}

static int lines_reserve(textbuf_t* t, int need) {
    if (need <= t->cap) return 0;
    if (need > MAX_LINES) return -1;
    int cap = t->cap ? t->cap : 64;
    while (cap < need) cap *= 2;
    tb_line_t* nl = (tb_line_t*)kzalloc((size_t)cap * sizeof(tb_line_t));
    if (!nl) return -1;
    if (t->lines) { memcpy(nl, t->lines, (size_t)t->n * sizeof(tb_line_t)); kfree(t->lines); }
    t->lines = nl;
    t->cap = cap;
    return 0;
}

/* a new empty line at index i */
static int insert_line(textbuf_t* t, int i) {
    if (lines_reserve(t, t->n + 1) != 0) return -1;
    memmove(&t->lines[i + 1], &t->lines[i], (size_t)(t->n - i) * sizeof(tb_line_t));
    memset(&t->lines[i], 0, sizeof(tb_line_t));
    line_reserve(&t->lines[i], 0);
    t->n++;
    return 0;
}

static void remove_line(textbuf_t* t, int i) {
    if (t->lines[i].s) kfree(t->lines[i].s);
    memmove(&t->lines[i], &t->lines[i + 1], (size_t)(t->n - i - 1) * sizeof(tb_line_t));
    t->n--;
}

void tb_init(textbuf_t* t) {
    memset(t, 0, sizeof(*t));
    insert_line(t, 0);
}

void tb_free(textbuf_t* t) {
    for (int i = 0; i < t->n; i++) if (t->lines[i].s) kfree(t->lines[i].s);
    if (t->lines) kfree(t->lines);
    memset(t, 0, sizeof(*t));
}

static void clamp(textbuf_t* t) {
    if (t->cy < 0) t->cy = 0;
    if (t->cy >= t->n) t->cy = t->n - 1;
    if (t->cx < 0) t->cx = 0;
    if (t->cx > (int)t->lines[t->cy].len) t->cx = (int)t->lines[t->cy].len;
}

void tb_load(textbuf_t* t, const char* text, uint32_t len) {
    tb_free(t);
    tb_init(t);
    uint32_t start = 0;
    for (uint32_t i = 0; i <= len; i++) {
        if (i == len || text[i] == '\n') {
            uint32_t e = i;
            if (e > start && text[e - 1] == '\r') e--;
            tb_line_t* l = &t->lines[t->n - 1];
            if (line_reserve(l, e - start) == 0) {
                memcpy(l->s, text + start, e - start);
                l->len = e - start;
                l->s[l->len] = 0;
            }
            if (i == len) break;
            if (insert_line(t, t->n) != 0) break;
            start = i + 1;
        }
    }
    t->cx = t->cy = 0;
    t->sel = 0;
    t->dirty = 0;
}

char* tb_text(const textbuf_t* t, uint32_t* len) {
    uint32_t total = 0;
    for (int i = 0; i < t->n; i++) total += t->lines[i].len + 1;
    char* out = (char*)kmalloc(total + 1);
    if (!out) { *len = 0; return NULL; }
    uint32_t p = 0;
    for (int i = 0; i < t->n; i++) {
        memcpy(out + p, t->lines[i].s, t->lines[i].len);
        p += t->lines[i].len;
        if (i < t->n - 1) out[p++] = '\n';
    }
    out[p] = 0;
    *len = p;
    return out;
}

int tb_sel_bounds(const textbuf_t* t, int* x0, int* y0, int* x1, int* y1) {
    if (!t->sel || (t->ax == t->cx && t->ay == t->cy)) return 0;
    if (t->ay < t->cy || (t->ay == t->cy && t->ax < t->cx)) { *x0 = t->ax; *y0 = t->ay; *x1 = t->cx; *y1 = t->cy; }
    else { *x0 = t->cx; *y0 = t->cy; *x1 = t->ax; *y1 = t->ay; }
    return 1;
}

char* tb_sel_text(const textbuf_t* t, uint32_t* len) {
    int x0, y0, x1, y1;
    *len = 0;
    if (!tb_sel_bounds(t, &x0, &y0, &x1, &y1)) return NULL;
    uint32_t total = 0;
    for (int y = y0; y <= y1; y++) total += t->lines[y].len + 1;
    char* out = (char*)kmalloc(total + 1);
    if (!out) return NULL;
    uint32_t p = 0;
    for (int y = y0; y <= y1; y++) {
        int a = y == y0 ? x0 : 0, b = y == y1 ? x1 : (int)t->lines[y].len;
        memcpy(out + p, t->lines[y].s + a, (size_t)(b - a));
        p += (uint32_t)(b - a);
        if (y < y1) out[p++] = '\n';
    }
    out[p] = 0;
    *len = p;
    return out;
}

static int has_sel(const textbuf_t* t) {
    int a, b, c, d;
    return tb_sel_bounds(t, &a, &b, &c, &d);
}

void tb_sel_delete(textbuf_t* t) {
    int x0, y0, x1, y1;
    if (!tb_sel_bounds(t, &x0, &y0, &x1, &y1)) { t->sel = 0; return; }
    tb_line_t* a = &t->lines[y0];
    tb_line_t* b = &t->lines[y1];
    uint32_t tail = b->len - (uint32_t)x1;
    if (line_reserve(a, (uint32_t)x0 + tail) == 0) {
        memmove(a->s + x0, b->s + x1, tail);
        a->len = (uint32_t)x0 + tail;
        a->s[a->len] = 0;
    }
    for (int y = y1; y > y0; y--) remove_line(t, y);
    t->cx = x0;
    t->cy = y0;
    t->sel = 0;
    t->dirty = 1;
}

void tb_insert(textbuf_t* t, const char* s, uint32_t n) {
    if (t->sel) tb_sel_delete(t);
    t->sel = 0;
    for (uint32_t i = 0; i < n; i++) {
        char c = s[i];
        if (c == '\r') continue;
        tb_line_t* l = &t->lines[t->cy];
        if (c == '\n') {
            if (insert_line(t, t->cy + 1) != 0) return;
            l = &t->lines[t->cy];                       /* the array may have moved */
            tb_line_t* nl = &t->lines[t->cy + 1];
            uint32_t rest = l->len - (uint32_t)t->cx;
            if (line_reserve(nl, rest) == 0) {
                memcpy(nl->s, l->s + t->cx, rest);
                nl->len = rest;
                nl->s[rest] = 0;
                l->len = (uint32_t)t->cx;
                l->s[l->len] = 0;
            }
            t->cy++;
            t->cx = 0;
            continue;
        }
        /* a run of ordinary characters at once */
        uint32_t j = i;
        while (j < n && s[j] != '\n' && s[j] != '\r') j++;
        uint32_t k = j - i;
        if (line_reserve(l, l->len + k) != 0) return;
        memmove(l->s + t->cx + k, l->s + t->cx, l->len - (uint32_t)t->cx);
        memcpy(l->s + t->cx, s + i, k);
        l->len += k;
        l->s[l->len] = 0;
        t->cx += (int)k;
        i = j - 1;
    }
    t->want_x = t->cx;
    t->dirty = 1;
}

void tb_backspace(textbuf_t* t) {
    if (has_sel(t)) { tb_sel_delete(t); return; }
    t->sel = 0;
    if (t->cx > 0) {
        tb_line_t* l = &t->lines[t->cy];
        memmove(l->s + t->cx - 1, l->s + t->cx, l->len - (uint32_t)t->cx + 1);
        l->len--;
        t->cx--;
    } else if (t->cy > 0) {
        tb_line_t* p = &t->lines[t->cy - 1];
        tb_line_t* l = &t->lines[t->cy];
        int px = (int)p->len;
        if (line_reserve(p, p->len + l->len) != 0) return;
        memcpy(p->s + p->len, l->s, l->len);
        p->len += l->len;
        p->s[p->len] = 0;
        remove_line(t, t->cy);
        t->cy--;
        t->cx = px;
    } else {
        return;
    }
    t->want_x = t->cx;
    t->dirty = 1;
}

void tb_delete(textbuf_t* t) {
    if (has_sel(t)) { tb_sel_delete(t); return; }
    t->sel = 0;
    tb_line_t* l = &t->lines[t->cy];
    if (t->cx < (int)l->len) {
        memmove(l->s + t->cx, l->s + t->cx + 1, l->len - (uint32_t)t->cx);
        l->len--;
        t->dirty = 1;
    } else if (t->cy < t->n - 1) {
        t->cy++;
        t->cx = 0;
        tb_backspace(t);
    }
}

static void start_sel(textbuf_t* t, int select) {
    if (select && !t->sel) { t->sel = 1; t->ax = t->cx; t->ay = t->cy; }
    if (!select) t->sel = 0;
}

void tb_move(textbuf_t* t, int dx, int dy, int select) {
    start_sel(t, select);
    if (dy) {
        t->cy += dy;
        if (t->cy < 0) { t->cy = 0; t->want_x = 0; }
        if (t->cy >= t->n) { t->cy = t->n - 1; t->want_x = (int)t->lines[t->cy].len; }
        t->cx = t->want_x;
        clamp(t);
        return;
    }
    if (dx < 0) {
        if (t->cx > 0) t->cx--;
        else if (t->cy > 0) { t->cy--; t->cx = (int)t->lines[t->cy].len; }
    } else if (dx > 0) {
        if (t->cx < (int)t->lines[t->cy].len) t->cx++;
        else if (t->cy < t->n - 1) { t->cy++; t->cx = 0; }
    }
    t->want_x = t->cx;
}

void tb_home(textbuf_t* t, int select) { start_sel(t, select); t->cx = 0; t->want_x = 0; }
void tb_end(textbuf_t* t, int select) { start_sel(t, select); t->cx = (int)t->lines[t->cy].len; t->want_x = t->cx; }

void tb_goto(textbuf_t* t, int x, int y, int select) {
    start_sel(t, select);
    t->cy = y;
    t->cx = x;
    clamp(t);
    t->want_x = t->cx;
}

void tb_select_all(textbuf_t* t) {
    t->sel = 1;
    t->ax = 0;
    t->ay = 0;
    t->cy = t->n - 1;
    t->cx = (int)t->lines[t->cy].len;
}

char* tb_cut_line(textbuf_t* t, uint32_t* len) {
    tb_line_t* l = &t->lines[t->cy];
    char* out = (char*)kmalloc(l->len + 2);
    if (!out) { *len = 0; return NULL; }
    memcpy(out, l->s, l->len);
    out[l->len] = '\n';
    out[l->len + 1] = 0;
    *len = l->len + 1;
    if (t->n > 1) remove_line(t, t->cy);
    else { l->len = 0; l->s[0] = 0; }
    t->sel = 0;
    clamp(t);
    t->cx = 0;
    t->dirty = 1;
    return out;
}

int tb_find(textbuf_t* t, const char* needle) {
    uint32_t nl = (uint32_t)strlen(needle);
    if (!nl) return 0;
    for (int pass = 0; pass <= t->n; pass++) {
        int y = (t->cy + pass) % t->n;
        tb_line_t* l = &t->lines[y];
        uint32_t from = pass == 0 ? (uint32_t)t->cx : 0;
        for (uint32_t x = from; x + nl <= l->len; x++) {
            if (strncasecmp(l->s + x, needle, nl) == 0) {
                t->sel = 1;
                t->ay = y;
                t->ax = (int)x;
                t->cy = y;
                t->cx = (int)(x + nl);
                return 1;
            }
        }
    }
    return 0;
}
