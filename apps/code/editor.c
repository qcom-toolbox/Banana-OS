/* Banana Code: text buffers - lines of bytes (UTF-8), a cursor and a
 * selection, snapshots for undo, find, and syntax colours. */
#include "code.h"
#include <ctype.h>

#define TABW 4

/* ── lines ── */
static void line_reserve(line_t* l, int n) {
    if (n + 1 <= l->cap) return;
    int c = l->cap ? l->cap : 16;
    while (c < n + 1) c *= 2;
    char* s = realloc(l->s, (size_t)c);
    if (!s) return;
    l->s = s;
    l->cap = c;
}

static void line_set(line_t* l, const char* s, int n) {
    line_reserve(l, n);
    if (l->cap < n + 1) return;
    memcpy(l->s, s, (size_t)n);
    l->len = n;
    l->s[n] = 0;
}

static void lines_reserve(doc_t* d, int n) {
    if (n <= d->cap) return;
    int c = d->cap ? d->cap : 64;
    while (c < n) c *= 2;
    line_t* nl = realloc(d->lines, sizeof(line_t) * (size_t)c);
    if (!nl) return;
    memset(nl + d->cap, 0, sizeof(line_t) * (size_t)(c - d->cap));
    d->lines = nl;
    d->cap = c;
}

/* a new empty line at index y */
static void line_insert_at(doc_t* d, int y) {
    lines_reserve(d, d->n + 1);
    if (d->cap < d->n + 1) return;
    memmove(d->lines + y + 1, d->lines + y, sizeof(line_t) * (size_t)(d->n - y));
    memset(&d->lines[y], 0, sizeof(line_t));
    d->n++;
    line_set(&d->lines[y], "", 0);
}

static void line_remove_at(doc_t* d, int y) {
    free(d->lines[y].s);
    memmove(d->lines + y, d->lines + y + 1, sizeof(line_t) * (size_t)(d->n - y - 1));
    d->n--;
    memset(&d->lines[d->n], 0, sizeof(line_t));
}

void doc_set_text(doc_t* d, const char* text) {
    for (int i = 0; i < d->n; i++) free(d->lines[i].s);
    memset(d->lines, 0, sizeof(line_t) * (size_t)d->cap);
    d->n = 0;
    const char* p = text ? text : "";
    for (;;) {
        const char* e = strchr(p, '\n');
        int n = e ? (int)(e - p) : (int)strlen(p);
        int k = n;
        if (k > 0 && p[k - 1] == '\r') k--;             /* CRLF files: the CR goes */
        line_insert_at(d, d->n);
        line_set(&d->lines[d->n - 1], p, k);
        if (!e) break;
        p = e + 1;
    }
    if (d->n == 0) { line_insert_at(d, 0); }
    d->cy = d->cx = 0;
    d->sy = -1;
    d->top = d->left = 0;
}

char* doc_text(const doc_t* d, int* len) {
    int total = 0;
    for (int i = 0; i < d->n; i++) total += d->lines[i].len + 1;
    char* t = malloc((size_t)total + 1);
    if (!t) return NULL;
    int k = 0;
    for (int i = 0; i < d->n; i++) {
        memcpy(t + k, d->lines[i].s, (size_t)d->lines[i].len);
        k += d->lines[i].len;
        if (i < d->n - 1) t[k++] = '\n';
    }
    t[k] = 0;
    if (len) *len = k;
    return t;
}

static const char* base_name(const char* p) { const char* s = strrchr(p, '/'); return s ? s + 1 : p; }

int doc_lang_for(const char* name) {
    const char* b = base_name(name);
    const char* e = strrchr(b, '.');
    if (!strcmp(b, "Makefile") || !strcmp(b, "makefile") || (e && !strcmp(e, ".mk"))) return LANG_MAKE;
    if (!e) return LANG_TEXT;
    e++;
    if (!strcasecmp(e, "c") || !strcasecmp(e, "h") || !strcasecmp(e, "cpp") || !strcasecmp(e, "cc") || !strcasecmp(e, "hpp")) return LANG_C;
    if (!strcasecmp(e, "js") || !strcasecmp(e, "mjs") || !strcasecmp(e, "ts")) return LANG_JS;
    if (!strcasecmp(e, "html") || !strcasecmp(e, "htm") || !strcasecmp(e, "svg") || !strcasecmp(e, "xml")) return LANG_HTML;
    if (!strcasecmp(e, "css")) return LANG_CSS;
    if (!strcasecmp(e, "py")) return LANG_PY;
    if (!strcasecmp(e, "json")) return LANG_JSON;
    if (!strcasecmp(e, "sh")) return LANG_SH;
    if (!strcasecmp(e, "md")) return LANG_MD;
    return LANG_TEXT;
}

const char* doc_lang_name(int lang) {
    static const char* const N[] = { "Plain Text", "C", "JavaScript", "HTML", "CSS", "Python", "JSON", "Shell Script", "Makefile", "Markdown" };
    return lang >= 0 && lang <= LANG_MD ? N[lang] : "Plain Text";
}

doc_t* doc_new(const char* path) {
    doc_t* d = calloc(1, sizeof(doc_t));
    if (!d) return NULL;
    snprintf(d->path, sizeof(d->path), "%s", path ? path : "");
    snprintf(d->name, sizeof(d->name), "%s", path && *path ? base_name(path) : "Untitled");
    d->lang = doc_lang_for(d->name);
    int len = 0;
    char* t = path && *path ? read_file(path, &len) : NULL;
    doc_set_text(d, t ? t : "");
    free(t);
    return d;
}

static void free_snaps(snap_t* s, int* n) { for (int i = 0; i < *n; i++) free(s[i].text); *n = 0; }

void doc_free(doc_t* d) {
    if (!d) return;
    for (int i = 0; i < d->n; i++) free(d->lines[i].s);
    free(d->lines);
    free_snaps(d->undo, &d->nundo);
    free_snaps(d->redo, &d->nredo);
    free(d);
}

int doc_save(doc_t* d) {
    int len;
    char* t = doc_text(d, &len);
    if (!t) return -1;
    /* a text file ends with a newline */
    int rc;
    if (len && t[len - 1] != '\n') {
        char* t2 = realloc(t, (size_t)len + 2);
        if (t2) { t = t2; t[len++] = '\n'; t[len] = 0; }
    }
    rc = write_file(d->path, t, len);
    free(t);
    if (rc == 0) d->dirty = 0;
    return rc;
}

/* ── selection ── */
int doc_has_sel(const doc_t* d) { return d->sy >= 0 && (d->sy != d->cy || d->sx != d->cx); }

void doc_sel_range(const doc_t* d, int* y0, int* x0, int* y1, int* x1) {
    int ay = d->sy, ax = d->sx, by = d->cy, bx = d->cx;
    if (ay > by || (ay == by && ax > bx)) { int t = ay; ay = by; by = t; t = ax; ax = bx; bx = t; }
    *y0 = ay; *x0 = ax; *y1 = by; *x1 = bx;
}

char* doc_sel_text(const doc_t* d) {
    if (!doc_has_sel(d)) { char* e = malloc(1); if (e) e[0] = 0; return e; }
    int y0, x0, y1, x1;
    doc_sel_range(d, &y0, &x0, &y1, &x1);
    sbuf_t b;
    sb_init(&b);
    for (int y = y0; y <= y1; y++) {
        int a = y == y0 ? x0 : 0, e = y == y1 ? x1 : d->lines[y].len;
        sb_addn(&b, d->lines[y].s + a, e - a);
        if (y < y1) sb_add(&b, "\n");
    }
    if (!b.s) sb_add(&b, "");
    return b.s;
}

void doc_clear_sel(doc_t* d) { d->sy = -1; }

/* ── undo ── */
static void push_snap(snap_t* st, int* n, char* text, int cy, int cx) {
    if (*n == UNDO_MAX) { free(st[0].text); memmove(st, st + 1, sizeof(snap_t) * (UNDO_MAX - 1)); (*n)--; }
    st[*n].text = text;
    st[*n].cy = cy;
    st[*n].cx = cx;
    (*n)++;
}

void doc_snapshot(doc_t* d, int op) {
    unsigned now = banana_ticks();
    /* typing (or deleting) in a row is one step */
    if (op != OP_OTHER && op == d->last_op && now - d->last_ms < 1500) { d->last_ms = now; d->dirty = 1; return; }
    d->last_op = op;
    d->last_ms = now;
    char* t = doc_text(d, NULL);
    if (!t) return;
    push_snap(d->undo, &d->nundo, t, d->cy, d->cx);
    free_snaps(d->redo, &d->nredo);
    d->dirty = 1;
}

static void restore(doc_t* d, snap_t* from, int* nfrom, snap_t* to, int* nto) {
    if (!*nfrom) return;
    char* cur = doc_text(d, NULL);
    if (cur) push_snap(to, nto, cur, d->cy, d->cx);
    snap_t s = from[--(*nfrom)];
    int top = d->top;
    doc_set_text(d, s.text);
    free(s.text);
    d->top = top;
    d->cy = s.cy < d->n ? s.cy : d->n - 1;
    d->cx = s.cx <= d->lines[d->cy].len ? s.cx : d->lines[d->cy].len;
    d->last_op = 0;
    d->dirty = 1;
}

void doc_undo(doc_t* d) { restore(d, d->undo, &d->nundo, d->redo, &d->nredo); }
void doc_redo(doc_t* d) { restore(d, d->redo, &d->nredo, d->undo, &d->nundo); }

/* ── editing ── */
void doc_delete_sel(doc_t* d) {
    if (!doc_has_sel(d)) { d->sy = -1; return; }
    int y0, x0, y1, x1;
    doc_sel_range(d, &y0, &x0, &y1, &x1);
    line_t* a = &d->lines[y0];
    line_t* b = &d->lines[y1];
    int tail = b->len - x1;
    char* keep = malloc((size_t)tail + 1);
    if (!keep) return;
    memcpy(keep, b->s + x1, (size_t)tail);
    line_reserve(a, x0 + tail);
    memcpy(a->s + x0, keep, (size_t)tail);
    a->len = x0 + tail;
    a->s[a->len] = 0;
    free(keep);
    for (int y = y1; y > y0; y--) line_remove_at(d, y);
    d->cy = y0;
    d->cx = x0;
    d->sy = -1;
}

void doc_insert(doc_t* d, const char* s, int n) {
    doc_delete_sel(d);
    for (int i = 0; i < n; ) {
        int j = i;
        while (j < n && s[j] != '\n') j++;
        int k = j - i;
        if (k > 0 && j < n && s[j - 1] == '\r') k--;
        line_t* l = &d->lines[d->cy];
        line_reserve(l, l->len + k);
        memmove(l->s + d->cx + k, l->s + d->cx, (size_t)(l->len - d->cx));
        memcpy(l->s + d->cx, s + i, (size_t)k);
        l->len += k;
        l->s[l->len] = 0;
        d->cx += k;
        if (j < n) {                                   /* a line break */
            line_insert_at(d, d->cy + 1);
            l = &d->lines[d->cy];
            line_set(&d->lines[d->cy + 1], l->s + d->cx, l->len - d->cx);
            l->len = d->cx;
            l->s[l->len] = 0;
            d->cy++;
            d->cx = 0;
        }
        i = j + 1;
    }
    d->want_vx = doc_vcol(d, d->cy, d->cx);
}

static int prev_char(const char* s, int x) {
    if (x <= 0) return 0;
    x--;
    while (x > 0 && (s[x] & 0xC0) == 0x80) x--;
    return x;
}
static int next_char(const char* s, int len, int x) {
    if (x >= len) return len;
    x++;
    while (x < len && (s[x] & 0xC0) == 0x80) x++;
    return x;
}

void doc_backspace(doc_t* d) {
    if (doc_has_sel(d)) { doc_delete_sel(d); return; }
    d->sy = -1;
    line_t* l = &d->lines[d->cy];
    if (d->cx > 0) {
        int p = prev_char(l->s, d->cx);
        /* in leading spaces: back to the previous tab stop */
        int only_spaces = 1;
        for (int i = 0; i < d->cx; i++) if (l->s[i] != ' ') only_spaces = 0;
        if (only_spaces && d->cx >= 1) p = ((d->cx - 1) / TABW) * TABW;
        memmove(l->s + p, l->s + d->cx, (size_t)(l->len - d->cx));
        l->len -= d->cx - p;
        l->s[l->len] = 0;
        d->cx = p;
    } else if (d->cy > 0) {
        line_t* a = &d->lines[d->cy - 1];
        int ax = a->len;
        line_reserve(a, a->len + l->len);
        memcpy(a->s + a->len, l->s, (size_t)l->len);
        a->len += l->len;
        a->s[a->len] = 0;
        line_remove_at(d, d->cy);
        d->cy--;
        d->cx = ax;
    }
    d->want_vx = doc_vcol(d, d->cy, d->cx);
}

void doc_delete(doc_t* d) {
    if (doc_has_sel(d)) { doc_delete_sel(d); return; }
    d->sy = -1;
    line_t* l = &d->lines[d->cy];
    if (d->cx < l->len) {
        int nx = next_char(l->s, l->len, d->cx);
        memmove(l->s + d->cx, l->s + nx, (size_t)(l->len - nx));
        l->len -= nx - d->cx;
        l->s[l->len] = 0;
    } else if (d->cy + 1 < d->n) {
        line_t* b = &d->lines[d->cy + 1];
        line_reserve(l, l->len + b->len);
        memcpy(l->s + l->len, b->s, (size_t)b->len);
        l->len += b->len;
        l->s[l->len] = 0;
        line_remove_at(d, d->cy + 1);
    }
}

void doc_newline(doc_t* d) {
    doc_delete_sel(d);
    line_t* l = &d->lines[d->cy];
    char ind[128];
    int k = 0;
    while (k < l->len && k < 120 && (l->s[k] == ' ' || l->s[k] == '\t')) { ind[k] = l->s[k]; k++; }
    if (k > d->cx) k = d->cx;
    /* after an opening brace: one level more */
    int j = d->cx - 1;
    while (j >= 0 && l->s[j] == ' ') j--;
    int open = j >= 0 && (l->s[j] == '{' || l->s[j] == '(' || l->s[j] == '[' || (l->s[j] == ':' && d->lang == LANG_PY));
    int close_next = open && d->cx < l->len && (l->s[d->cx] == '}' || l->s[d->cx] == ')' || l->s[d->cx] == ']');
    doc_insert(d, "\n", 1);
    doc_insert(d, ind, k);
    if (open) {
        doc_insert(d, "    ", TABW);
        if (close_next) {
            int cy = d->cy, cx = d->cx;
            doc_insert(d, "\n", 1);
            doc_insert(d, ind, k);
            d->cy = cy;
            d->cx = cx;
        }
    }
}

void doc_indent(doc_t* d, int out) {
    int y0 = d->cy, y1 = d->cy, x0 = 0, x1 = 0;
    if (doc_has_sel(d)) doc_sel_range(d, &y0, &x0, &y1, &x1);
    if (doc_has_sel(d) && x1 == 0 && y1 > y0) y1--;
    for (int y = y0; y <= y1; y++) {
        line_t* l = &d->lines[y];
        if (out) {
            int k = 0;
            while (k < TABW && k < l->len && l->s[k] == ' ') k++;
            if (k == 0 && l->len && l->s[0] == '\t') k = 1;
            memmove(l->s, l->s + k, (size_t)(l->len - k));
            l->len -= k;
            l->s[l->len] = 0;
            if (y == d->cy) d->cx = d->cx >= k ? d->cx - k : 0;
            if (y == d->sy) d->sx = d->sx >= k ? d->sx - k : 0;
        } else {
            if (!l->len) continue;
            line_reserve(l, l->len + TABW);
            memmove(l->s + TABW, l->s, (size_t)l->len);
            memset(l->s, ' ', TABW);
            l->len += TABW;
            l->s[l->len] = 0;
            if (y == d->cy) d->cx += TABW;
            if (y == d->sy && d->sx > 0) d->sx += TABW;
        }
    }
}

void doc_toggle_comment(doc_t* d) {
    const char* mark = d->lang == LANG_PY || d->lang == LANG_SH || d->lang == LANG_MAKE ? "#" :
                       d->lang == LANG_C || d->lang == LANG_JS || d->lang == LANG_CSS ? "//" : NULL;
    if (!mark) return;
    int ml = (int)strlen(mark);
    int y0 = d->cy, y1 = d->cy, x0, x1;
    if (doc_has_sel(d)) { doc_sel_range(d, &y0, &x0, &y1, &x1); if (x1 == 0 && y1 > y0) y1--; }
    /* all commented: uncomment; else comment */
    int all = 1;
    for (int y = y0; y <= y1; y++) {
        line_t* l = &d->lines[y];
        int k = 0;
        while (k < l->len && (l->s[k] == ' ' || l->s[k] == '\t')) k++;
        if (k < l->len && strncmp(l->s + k, mark, (size_t)ml)) all = 0;
    }
    for (int y = y0; y <= y1; y++) {
        line_t* l = &d->lines[y];
        int k = 0;
        while (k < l->len && (l->s[k] == ' ' || l->s[k] == '\t')) k++;
        if (k == l->len) continue;
        if (all) {
            int n = ml + (l->s[k + ml] == ' ');
            memmove(l->s + k, l->s + k + n, (size_t)(l->len - k - n));
            l->len -= n;
            if (y == d->cy && d->cx > k) d->cx = d->cx - n > k ? d->cx - n : k;
        } else {
            line_reserve(l, l->len + ml + 1);
            memmove(l->s + k + ml + 1, l->s + k, (size_t)(l->len - k));
            memcpy(l->s + k, mark, (size_t)ml);
            l->s[k + ml] = ' ';
            l->len += ml + 1;
            if (y == d->cy && d->cx >= k) d->cx += ml + 1;
        }
        l->s[l->len] = 0;
    }
    d->sy = -1;
}

/* ── columns ── */
int doc_vcol(const doc_t* d, int y, int x) {
    if (y < 0 || y >= d->n) return 0;
    const line_t* l = &d->lines[y];
    int v = 0;
    for (int i = 0; i < x && i < l->len; i++) {
        unsigned char c = (unsigned char)l->s[i];
        if (c == '\t') v = (v / TABW + 1) * TABW;
        else if ((c & 0xC0) != 0x80) v++;
    }
    return v;
}

int doc_byte_at(const doc_t* d, int y, int vx) {
    if (y < 0 || y >= d->n) return 0;
    const line_t* l = &d->lines[y];
    int v = 0, i = 0;
    while (i < l->len) {
        unsigned char c = (unsigned char)l->s[i];
        int nv = c == '\t' ? (v / TABW + 1) * TABW : v + 1;
        if (nv > vx) {
            if (vx - v > nv - vx) i = next_char(l->s, l->len, i);   /* nearer the next one */
            return i;
        }
        v = nv;
        i = next_char(l->s, l->len, i);
    }
    return l->len;
}

/* ── moves ── */
static void sel_start(doc_t* d, int shift) {
    if (shift) { if (d->sy < 0) { d->sy = d->cy; d->sx = d->cx; } }
    else d->sy = -1;
}

void doc_goto(doc_t* d, int y, int x, int shift) {
    sel_start(d, shift);
    if (y < 0) y = 0;
    if (y >= d->n) y = d->n - 1;
    if (x < 0) x = 0;
    if (x > d->lines[y].len) x = d->lines[y].len;
    d->cy = y;
    d->cx = x;
    d->want_vx = doc_vcol(d, y, x);
}

void doc_move(doc_t* d, int dy, int dx, int shift) {
    if (!shift && doc_has_sel(d) && dy == 0) {             /* an arrow collapses the selection to its side */
        int y0, x0, y1, x1;
        doc_sel_range(d, &y0, &x0, &y1, &x1);
        d->sy = -1;
        if (dx < 0) { d->cy = y0; d->cx = x0; } else { d->cy = y1; d->cx = x1; }
        d->want_vx = doc_vcol(d, d->cy, d->cx);
        return;
    }
    sel_start(d, shift);
    if (dy) {
        int y = d->cy + dy;
        if (y < 0) { y = 0; d->cx = 0; d->cy = 0; return; }
        if (y >= d->n) { d->cy = d->n - 1; d->cx = d->lines[d->cy].len; return; }
        d->cy = y;
        d->cx = doc_byte_at(d, y, d->want_vx);
        return;
    }
    line_t* l = &d->lines[d->cy];
    if (dx < 0) {
        if (d->cx > 0) d->cx = prev_char(l->s, d->cx);
        else if (d->cy > 0) { d->cy--; d->cx = d->lines[d->cy].len; }
    } else if (dx > 0) {
        if (d->cx < l->len) d->cx = next_char(l->s, l->len, d->cx);
        else if (d->cy + 1 < d->n) { d->cy++; d->cx = 0; }
    }
    d->want_vx = doc_vcol(d, d->cy, d->cx);
}

void doc_home(doc_t* d, int shift) {
    sel_start(d, shift);
    line_t* l = &d->lines[d->cy];
    int k = 0;
    while (k < l->len && (l->s[k] == ' ' || l->s[k] == '\t')) k++;
    d->cx = d->cx == k ? 0 : k;                     /* the first character, then the line's start */
    d->want_vx = doc_vcol(d, d->cy, d->cx);
}

void doc_end(doc_t* d, int shift) {
    sel_start(d, shift);
    d->cx = d->lines[d->cy].len;
    d->want_vx = doc_vcol(d, d->cy, d->cx);
}

static int is_word(char c) { return isalnum((unsigned char)c) || c == '_' || (unsigned char)c >= 0x80; }

void doc_word(doc_t* d, int dir, int shift) {
    sel_start(d, shift);
    line_t* l = &d->lines[d->cy];
    if (dir < 0) {
        if (d->cx == 0) { doc_move(d, 0, -1, shift); return; }
        int x = d->cx;
        while (x > 0 && !is_word(l->s[x - 1])) x--;
        while (x > 0 && is_word(l->s[x - 1])) x--;
        d->cx = x;
    } else {
        if (d->cx == l->len) { doc_move(d, 0, 1, shift); return; }
        int x = d->cx;
        while (x < l->len && !is_word(l->s[x])) x++;
        while (x < l->len && is_word(l->s[x])) x++;
        d->cx = x;
    }
    d->want_vx = doc_vcol(d, d->cy, d->cx);
}

void doc_select_all(doc_t* d) {
    d->sy = 0;
    d->sx = 0;
    d->cy = d->n - 1;
    d->cx = d->lines[d->cy].len;
}

void doc_select_word(doc_t* d) {
    line_t* l = &d->lines[d->cy];
    int a = d->cx, b = d->cx;
    while (a > 0 && is_word(l->s[a - 1])) a--;
    while (b < l->len && is_word(l->s[b])) b++;
    if (a == b) return;
    d->sy = d->cy;
    d->sx = a;
    d->cx = b;
}

/* ── find ── */
static int match_at(const char* s, int len, int x, const char* w, int wl, int mc) {
    if (x + wl > len) return 0;
    return mc ? !strncmp(s + x, w, (size_t)wl) : !strncasecmp(s + x, w, (size_t)wl);
}

int doc_find(doc_t* d, const char* what, int dir, int mc) {
    int wl = (int)strlen(what);
    if (!wl) return 0;
    int y = d->cy, x = d->cx;
    if (dir < 0 && doc_has_sel(d)) { int y0, x0, y1, x1; doc_sel_range(d, &y0, &x0, &y1, &x1); y = y0; x = x0; }
    for (int pass = 0; pass <= d->n; pass++) {
        line_t* l = &d->lines[y];
        if (dir > 0) {
            for (int i = pass == 0 ? x : 0; i + wl <= l->len; i++)
                if (match_at(l->s, l->len, i, what, wl, mc)) { d->sy = y; d->sx = i; d->cy = y; d->cx = i + wl; return 1; }
            y = (y + 1) % d->n;
        } else {
            for (int i = (pass == 0 ? x : l->len) - 1; i >= 0; i--)
                if (match_at(l->s, l->len, i, what, wl, mc)) { d->sy = y; d->sx = i; d->cy = y; d->cx = i + wl; return 1; }
            y = (y + d->n - 1) % d->n;
        }
    }
    return 0;
}

int doc_replace_all(doc_t* d, const char* what, const char* with, int mc) {
    int wl = (int)strlen(what), rl = (int)strlen(with), count = 0;
    if (!wl) return 0;
    doc_snapshot(d, OP_OTHER);
    for (int y = 0; y < d->n; y++) {
        line_t* l = &d->lines[y];
        for (int i = 0; i + wl <= l->len; ) {
            if (match_at(l->s, l->len, i, what, wl, mc)) {
                line_reserve(l, l->len - wl + rl);
                memmove(l->s + i + rl, l->s + i + wl, (size_t)(l->len - i - wl));
                memcpy(l->s + i, with, (size_t)rl);
                l->len += rl - wl;
                l->s[l->len] = 0;
                i += rl;
                count++;
            } else i++;
        }
    }
    d->sy = -1;
    if (d->cx > d->lines[d->cy].len) d->cx = d->lines[d->cy].len;
    return count;
}

/* ── syntax colours ── */
static const char* const C_KW[] = { "auto", "break", "case", "const", "continue", "default", "do", "else", "enum", "extern",
    "for", "goto", "if", "inline", "register", "restrict", "return", "sizeof", "static", "struct", "switch", "typedef",
    "union", "volatile", "while", "NULL", "true", "false", "class", "public", "private", "new", "delete", "namespace", 0 };
static const char* const C_TYPES[] = { "void", "char", "short", "int", "long", "float", "double", "signed", "unsigned",
    "bool", "size_t", "uint8_t", "uint16_t", "uint32_t", "uint64_t", "int8_t", "int16_t", "int32_t", "int64_t",
    "uintptr_t", "FILE", "bwin_t", "banana_event_t", 0 };
static const char* const JS_KW[] = { "var", "let", "const", "function", "return", "if", "else", "for", "while", "do",
    "break", "continue", "switch", "case", "default", "new", "this", "class", "extends", "import", "export", "from",
    "async", "await", "try", "catch", "finally", "throw", "typeof", "instanceof", "in", "of", "null", "undefined",
    "true", "false", "yield", "delete", "static", "get", "set", 0 };
static const char* const PY_KW[] = { "def", "class", "return", "if", "elif", "else", "for", "while", "in", "not", "and",
    "or", "import", "from", "as", "with", "try", "except", "finally", "raise", "pass", "break", "continue", "lambda",
    "None", "True", "False", "yield", "global", "is", "self", 0 };
static const char* const SH_KW[] = { "if", "then", "else", "elif", "fi", "for", "do", "done", "while", "case", "esac",
    "function", "return", "echo", "export", "local", "in", 0 };

static int in_list(const char* const* l, const char* s, int n) {
    for (; *l; l++) if ((int)strlen(*l) == n && !strncmp(*l, s, (size_t)n)) return 1;
    return 0;
}
static int is_control(const char* s, int n) {
    static const char* const K[] = { "if", "else", "for", "while", "do", "switch", "case", "return", "break", "continue",
        "goto", "try", "catch", "throw", "default", "elif", "except", "finally", 0 };
    return in_list(K, s, n);
}

/* the comment state at line y's start: lines are coloured in order */
static int state_at(doc_t* d, int y);

static void hl_c_like(doc_t* d, const line_t* l, unsigned char* c, int lang, int* st) {
    int n = l->len;
    const char* s = l->s;
    int i = 0;
    /* a preprocessor line */
    if (lang == LANG_C) {
        int k = 0;
        while (k < n && (s[k] == ' ' || s[k] == '\t')) k++;
        if (k < n && s[k] == '#' && !*st) {
            for (int j = 0; j < n; j++) c[j] = HL_PREPROC;
            /* <file> and "file" */
            for (int j = k; j < n; j++) if (s[j] == '<' || s[j] == '"') { for (int m = j; m < n; m++) c[m] = HL_STRING; break; }
            int cm = -1;
            for (int j = k; j + 1 < n; j++) if (s[j] == '/' && (s[j + 1] == '/' || s[j + 1] == '*')) { cm = j; break; }
            if (cm >= 0) for (int j = cm; j < n; j++) c[j] = HL_COMMENT;
            return;
        }
    }
    (void)d;
    while (i < n) {
        if (*st) {                                   /* inside a block comment */
            c[i] = HL_COMMENT;
            if (s[i] == '*' && i + 1 < n && s[i + 1] == '/') { c[i + 1] = HL_COMMENT; i += 2; *st = 0; continue; }
            i++;
            continue;
        }
        char ch = s[i];
        if (ch == '/' && i + 1 < n && s[i + 1] == '/') { for (; i < n; i++) c[i] = HL_COMMENT; break; }
        if (ch == '/' && i + 1 < n && s[i + 1] == '*') { c[i] = c[i + 1] = HL_COMMENT; i += 2; *st = 1; continue; }
        if (ch == '"' || ch == '\'' || (ch == '`' && lang == LANG_JS)) {
            char q = ch;
            c[i++] = HL_STRING;
            while (i < n) {
                c[i] = HL_STRING;
                if (s[i] == '\\' && i + 1 < n) { c[i + 1] = HL_STRING; i += 2; continue; }
                if (s[i++] == q) break;
            }
            continue;
        }
        if (isdigit((unsigned char)ch) || (ch == '.' && i + 1 < n && isdigit((unsigned char)s[i + 1]))) {
            while (i < n && (isalnum((unsigned char)s[i]) || s[i] == '.' || s[i] == '_')) c[i++] = HL_NUMBER;
            continue;
        }
        if (isalpha((unsigned char)ch) || ch == '_' || ch == '$') {
            int a = i;
            while (i < n && (isalnum((unsigned char)s[i]) || s[i] == '_' || s[i] == '$')) i++;
            int len = i - a, col = HL_TEXT;
            if (lang == LANG_JS) {
                if (is_control(s + a, len)) col = HL_CONTROL;
                else if (in_list(JS_KW, s + a, len)) col = HL_KEYWORD;
            } else {
                if (is_control(s + a, len)) col = HL_CONTROL;
                else if (in_list(C_TYPES, s + a, len)) col = HL_TYPE;
                else if (in_list(C_KW, s + a, len)) col = HL_KEYWORD;
            }
            if (col == HL_TEXT) {
                int k = i;
                while (k < n && s[k] == ' ') k++;
                if (k < n && s[k] == '(') col = HL_FUNC;
                else if (len > 2 && s[a] >= 'A' && s[a] <= 'Z' && lang == LANG_JS) col = HL_TYPE;
            }
            for (int j = a; j < i; j++) c[j] = (unsigned char)col;
            continue;
        }
        c[i++] = strchr("{}()[];,", ch) ? HL_PUNCT : HL_TEXT;
    }
}

static void hl_hash(const line_t* l, unsigned char* c, const char* const* kw) {
    int n = l->len, i = 0;
    const char* s = l->s;
    while (i < n) {
        char ch = s[i];
        if (ch == '#') { for (; i < n; i++) c[i] = HL_COMMENT; break; }
        if (ch == '"' || ch == '\'') {
            char q = ch;
            c[i++] = HL_STRING;
            while (i < n) { c[i] = HL_STRING; if (s[i] == '\\' && i + 1 < n) { c[++i] = HL_STRING; i++; continue; } if (s[i++] == q) break; }
            continue;
        }
        if (ch == '$') { c[i++] = HL_TYPE; while (i < n && (isalnum((unsigned char)s[i]) || s[i] == '_' || s[i] == '{' || s[i] == '}' || s[i] == '(' || s[i] == ')')) c[i++] = HL_TYPE; continue; }
        if (isdigit((unsigned char)ch)) { while (i < n && isalnum((unsigned char)s[i])) c[i++] = HL_NUMBER; continue; }
        if (isalpha((unsigned char)ch) || ch == '_') {
            int a = i;
            while (i < n && (isalnum((unsigned char)s[i]) || s[i] == '_')) i++;
            int col = kw && in_list(kw, s + a, i - a) ? (is_control(s + a, i - a) ? HL_CONTROL : HL_KEYWORD) : HL_TEXT;
            if (col == HL_TEXT && i < n && s[i] == '(') col = HL_FUNC;
            for (int j = a; j < i; j++) c[j] = (unsigned char)col;
            continue;
        }
        c[i++] = HL_TEXT;
    }
}

static void hl_html(const line_t* l, unsigned char* c, int* st) {
    int n = l->len, i = 0;
    const char* s = l->s;
    while (i < n) {
        if (*st == 1) {                                /* <!-- ... --> */
            c[i] = HL_COMMENT;
            if (!strncmp(s + i, "-->", 3) && i + 3 <= n) { c[i + 1] = c[i + 2] = HL_COMMENT; i += 3; *st = 0; continue; }
            i++;
            continue;
        }
        if (s[i] == '<' && i + 3 < n && !strncmp(s + i, "<!--", 4)) { *st = 1; continue; }
        if (s[i] == '<') {
            c[i++] = HL_PUNCT;
            if (i < n && s[i] == '/') c[i++] = HL_PUNCT;
            while (i < n && (isalnum((unsigned char)s[i]) || s[i] == '-' || s[i] == '!')) c[i++] = HL_TAG;
            while (i < n && s[i] != '>') {
                if (s[i] == '"' || s[i] == '\'') {
                    char q = s[i];
                    c[i++] = HL_STRING;
                    while (i < n) { c[i] = HL_STRING; if (s[i++] == q) break; }
                } else if (isalpha((unsigned char)s[i])) {
                    while (i < n && (isalnum((unsigned char)s[i]) || s[i] == '-' || s[i] == ':')) c[i++] = HL_ATTR;
                } else c[i++] = HL_TEXT;
            }
            if (i < n) c[i++] = HL_PUNCT;
            continue;
        }
        if (s[i] == '&') { while (i < n && s[i] != ';' && s[i] != ' ') c[i++] = HL_NUMBER; if (i < n && s[i] == ';') c[i++] = HL_NUMBER; continue; }
        c[i++] = HL_TEXT;
    }
}

static void hl_css(const line_t* l, unsigned char* c, int* st) {
    int n = l->len, i = 0, in_block = 0;
    const char* s = l->s;
    for (int k = 0; k < n; k++) if (s[k] == ':' ) in_block = 1;
    while (i < n) {
        if (*st) { c[i] = HL_COMMENT; if (s[i] == '*' && i + 1 < n && s[i + 1] == '/') { c[i + 1] = HL_COMMENT; i += 2; *st = 0; continue; } i++; continue; }
        if (s[i] == '/' && i + 1 < n && s[i + 1] == '*') { *st = 1; continue; }
        if (s[i] == '"' || s[i] == '\'') { char q = s[i]; c[i++] = HL_STRING; while (i < n) { c[i] = HL_STRING; if (s[i++] == q) break; } continue; }
        if (isdigit((unsigned char)s[i]) || s[i] == '#') { while (i < n && (isalnum((unsigned char)s[i]) || s[i] == '.' || s[i] == '#' || s[i] == '%')) c[i++] = HL_NUMBER; continue; }
        if (isalpha((unsigned char)s[i]) || s[i] == '-' || s[i] == '.') {
            int a = i;
            while (i < n && (isalnum((unsigned char)s[i]) || s[i] == '-' || s[i] == '.' || s[i] == '_')) i++;
            int k = i;
            while (k < n && s[k] == ' ') k++;
            int col = (k < n && s[k] == ':' && in_block) ? HL_ATTR : in_block ? HL_STRING : HL_TAG;
            for (int j = a; j < i; j++) c[j] = (unsigned char)col;
            continue;
        }
        char ch = s[i];
        c[i++] = strchr("{};:", ch) ? HL_PUNCT : HL_TEXT;
    }
}

static void hl_json(const line_t* l, unsigned char* c) {
    int n = l->len, i = 0;
    const char* s = l->s;
    while (i < n) {
        if (s[i] == '"') {
            int a = i++;
            while (i < n && s[i] != '"') { if (s[i] == '\\') i++; i++; }
            if (i < n) i++;
            int k = i;
            while (k < n && s[k] == ' ') k++;
            int col = k < n && s[k] == ':' ? HL_ATTR : HL_STRING;
            for (int j = a; j < i && j < n; j++) c[j] = (unsigned char)col;
            continue;
        }
        if (isdigit((unsigned char)s[i]) || s[i] == '-') { while (i < n && (isalnum((unsigned char)s[i]) || s[i] == '.' || s[i] == '-' || s[i] == '+')) c[i++] = HL_NUMBER; continue; }
        if (isalpha((unsigned char)s[i])) { while (i < n && isalpha((unsigned char)s[i])) c[i++] = HL_KEYWORD; continue; }
        c[i++] = HL_PUNCT;
    }
}

static void hl_md(const line_t* l, unsigned char* c) {
    int n = l->len;
    const char* s = l->s;
    int col = HL_TEXT;
    if (n && s[0] == '#') col = HL_KEYWORD;
    else if (n > 1 && (s[0] == '-' || s[0] == '*') && s[1] == ' ') { c[0] = HL_PUNCT; }
    for (int i = (col == HL_TEXT && n > 1 && (s[0] == '-' || s[0] == '*') && s[1] == ' ') ? 1 : 0; i < n; i++) c[i] = (unsigned char)col;
    for (int i = 0; i < n; i++) if (s[i] == '`') { int j = i + 1; while (j < n && s[j] != '`') j++; for (int k = i; k <= j && k < n; k++) c[k] = HL_STRING; i = j; }
}

static void hl_line(doc_t* d, int y, unsigned char* c, int* st) {
    const line_t* l = &d->lines[y];
    memset(c, HL_TEXT, (size_t)l->len + 1);
    switch (d->lang) {
    case LANG_C: case LANG_JS: hl_c_like(d, l, c, d->lang, st); break;
    case LANG_PY: hl_hash(l, c, PY_KW); break;
    case LANG_SH: case LANG_MAKE: hl_hash(l, c, SH_KW); break;
    case LANG_HTML: hl_html(l, c, st); break;
    case LANG_CSS: hl_css(l, c, st); break;
    case LANG_JSON: hl_json(l, c); break;
    case LANG_MD: hl_md(l, c); break;
    default: break;
    }
}

/* comment state at y's start: from the nearest line above that knows */
static int state_at(doc_t* d, int y) {
    int st = 0;
    int from = y - 400 < 0 ? 0 : y - 400;            /* far enough for any block comment shown */
    static unsigned char tmp[65536];
    for (int k = from; k < y; k++) {
        if (d->lines[k].len >= (int)sizeof(tmp)) continue;
        hl_line(d, k, tmp, &st);
    }
    return st;
}

void doc_highlight(doc_t* d, int y, unsigned char* c) {
    static doc_t* last_doc;
    static int last_y = -2, last_st;
    int st;
    /* drawing goes top to bottom: carry the state from the line above */
    if (last_doc == d && last_y == y - 1) st = last_st;
    else st = state_at(d, y);
    hl_line(d, y, c, &st);
    last_doc = d;
    last_y = y;
    last_st = st;
}
