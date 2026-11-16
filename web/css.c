/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "css.h"
#include "kstring.h"

int css_viewport_w = 800;
int css_viewport_h = 600;

/* ══ data ═════════════════════════════════════════════════════════════ */

#define SEL_PARTS 12
#define MAX_ATTRS 4

typedef struct selector selector_t;

/* Each functional pseudo is a separate condition on the same element. */
typedef struct func_cond {
    selector_t* sels;
    int nsels, negate;
    int nth_kind, nth_a, nth_b;
    struct func_cond* next;
} func_cond_t;

typedef struct {
    char* name;                 /* lowercase */
    char* val;                  /* NULL: [name] only */
    char  op;                   /* '=' '~' '|' '^' '$' '*' */
    char  icase;
} attr_cond_t;

typedef struct {
    char* tag;                  /* NULL: any */
    char* id;
    char** cls;
    int   ncls;
    attr_cond_t* attr;         /* exactly nattr of them */
    int   nattr;
    uint8_t first_child, last_child, only_child, first_type, last_type, only_type;
    uint8_t link, checked, disabled, enabled, empty, never;
    func_cond_t* funcs;         /* conjunctive :not/:is/:where and nth conditions */
} simple_t;

struct selector {
    simple_t* part;             /* exactly n of them (big style sheets have many selectors) */
    char*    comb;              /* combinator before part i: ' ' '>' '+' '~' */
    int      n;
    uint32_t spec;
};

/* one selector while it is being parsed */
typedef struct {
    simple_t part[SEL_PARTS];
    char     comb[SEL_PARTS];
    attr_cond_t attrs[SEL_PARTS][MAX_ATTRS];
} sel_tmp_t;

typedef struct {
    char* prop;
    char* val;
    int   important;
} decl_t;

typedef struct {
    selector_t* sels;
    int         nsels;
    decl_t*     decls;
    int         ndecls;
    int         media_min, media_max;   /* @media (min-width / max-width), 0 = none */
} rule_t;

/* rules by the key of their selectors' last compound (id, class, tag, or none) */
typedef struct bucket {
    const char* key;
    int*  refs;                 /* rule index << 8 | selector index */
    int   n, cap;
    struct bucket* next;
} bucket_t;

#define BUCKETS 512

struct css_kf;
struct css_sheet {
    struct css_kf* keyframes;   /* @keyframes (css_anim.h) */
    rule_t* rules;
    int     n, cap;
    int     origin;             /* 0 = default (user agent), 1 = page */
    arena_t* A;
    bucket_t* by_id[BUCKETS];
    bucket_t* by_class[BUCKETS];
    bucket_t* by_tag[BUCKETS];
    bucket_t  any;              /* selectors without id/class/tag */
    int     indexed;
};

static char lc(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }
static int is_ws(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }
static int is_ident(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || (unsigned char)c >= 0x80 || c == '\\';
}

static char* dup_lc(arena_t* A, const char* s, uint32_t n) {
    char* d = arena_strdup(A, s, n);
    for (uint32_t i = 0; i < n; i++) d[i] = lc(d[i]);
    return d;
}

static int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    c = lc(c);
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

/* Past a CSS escape, including the optional hex terminator (CRLF is one). */
static uint32_t escape_end(const char* s, uint32_t i, uint32_t n) {
    i++;
    if (i >= n) return n;
    if (hex_digit(s[i]) < 0) return i + 1;
    int digits = 0;
    while (i < n && digits < 6 && hex_digit(s[i]) >= 0) { i++; digits++; }
    if (i < n && is_ws(s[i])) {
        if (s[i++] == '\r' && i < n && s[i] == '\n') i++;
    }
    return i;
}

static uint32_t ident_end(const char* s, uint32_t i, uint32_t n) {
    while (i < n && is_ident(s[i])) {
        if (s[i] != '\\') { i++; continue; }
        if (i + 1 >= n || s[i + 1] == '\n' || s[i + 1] == '\r' || s[i + 1] == '\f') break;
        i = escape_end(s, i, n);
    }
    return i;
}

/* Decode CSS escapes to UTF-8; invalid scalar values become U+FFFD. */
static char* ident_dup(arena_t* A, const char* s, uint32_t n) {
    /* Only a two-byte escape such as \0 can expand (to three UTF-8 bytes). */
    char* d = (char*)arena_alloc(A, n + n / 2 + 1);
    uint32_t k = 0;
    for (uint32_t i = 0; i < n; ) {
        if (s[i] != '\\') { d[k++] = s[i++]; continue; }
        uint32_t end = escape_end(s, i, n);
        i++;
        if (i >= n) break;
        if (hex_digit(s[i]) < 0) {
            /* Escaped newlines are string continuations, not characters. */
            if (s[i] == '\r' && end < n && s[end] == '\n') end++;
            if (s[i] != '\n' && s[i] != '\r' && s[i] != '\f') d[k++] = s[i];
        } else {
            uint32_t cp = 0;
            int digits = 0;
            while (i < n && digits < 6 && hex_digit(s[i]) >= 0) {
                cp = cp * 16 + (uint32_t)hex_digit(s[i++]);
                digits++;
            }
            if (!cp || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) cp = 0xfffd;
            if (cp < 0x80) d[k++] = (char)cp;
            else {
                if (cp < 0x800) d[k++] = (char)(0xc0 | (cp >> 6));
                else {
                    if (cp < 0x10000) d[k++] = (char)(0xe0 | (cp >> 12));
                    else {
                        d[k++] = (char)(0xf0 | (cp >> 18));
                        d[k++] = (char)(0x80 | ((cp >> 12) & 0x3f));
                    }
                    d[k++] = (char)(0x80 | ((cp >> 6) & 0x3f));
                }
                d[k++] = (char)(0x80 | (cp & 0x3f));
            }
        }
        i = end;
    }
    d[k] = 0;
    return d;
}

static char* ident_lc(arena_t* A, const char* s, uint32_t n) {
    char* d = ident_dup(A, s, n);
    for (char* p = d; *p; p++) *p = lc(*p);
    return d;
}

static uint32_t hash_s(const char* s) {
    uint32_t h = 5381;
    while (*s) h = h * 33 + (unsigned char)*s++;
    return h;
}

/* ══ selectors ════════════════════════════════════════════════════════ */

static int parse_sel_list(arena_t* A, const char* s, uint32_t n, selector_t** out);

/* "2n+1", "odd", "even", "3" */
static void parse_nth(const char* s, uint32_t n, int* a, int* b) {
    char buf[32];
    uint32_t k = 0;
    for (uint32_t i = 0; i < n && k < 31; i++) if (!is_ws(s[i])) buf[k++] = lc(s[i]);
    buf[k] = 0;
    if (strcmp(buf, "odd") == 0) { *a = 2; *b = 1; return; }
    if (strcmp(buf, "even") == 0) { *a = 2; *b = 0; return; }
    char* np = strchr(buf, 'n');
    if (!np) { *a = 0; *b = 0; for (char* p = buf; *p >= '0' && *p <= '9'; p++) *b = *b * 10 + (*p - '0'); return; }
    *np = 0;
    if (!buf[0] || strcmp(buf, "+") == 0) *a = 1;
    else if (strcmp(buf, "-") == 0) *a = -1;
    else { int neg = buf[0] == '-'; const char* p = buf + (buf[0] == '-' || buf[0] == '+'); int v = 0; while (*p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0'); *a = neg ? -v : v; }
    const char* q = np + 1;
    int neg = 0;
    if (*q == '+') q++;
    else if (*q == '-') { neg = 1; q++; }
    int v = 0;
    while (*q >= '0' && *q <= '9') v = v * 10 + (*q++ - '0');
    *b = neg ? -v : v;
}

/* the end of a (...) group starting at s[i] == '(' */
static uint32_t paren_end(const char* s, uint32_t i, uint32_t n) {
    int d = 0, brackets = 0;
    char q = 0;
    for (; i < n; i++) {
        if (s[i] == '\\') { i = escape_end(s, i, n) - 1; continue; }
        if (q) { if (s[i] == q) q = 0; continue; }
        if (s[i] == '"' || s[i] == '\'') { q = s[i]; continue; }
        if (s[i] == '[') brackets++;
        else if (s[i] == ']' && brackets) brackets--;
        else if (!brackets && s[i] == '(') d++;
        else if (!brackets && s[i] == ')') { if (--d == 0) return i; }
    }
    return n;
}

/* parses one complex selector; returns 0 if unusable */
static int parse_selector_into(arena_t* A, const char* s, uint32_t n, selector_t* out, sel_tmp_t* T);

static int parse_selector(arena_t* A, const char* s, uint32_t n, selector_t* out) {
    static sel_tmp_t tmp_storage[4];                  /* :not(...) nests: a few levels of scratch */
    static int level;
    if (level >= 4) return 0;
    sel_tmp_t* t = &tmp_storage[level++];
    memset(t, 0, sizeof(*t));
    int ok = parse_selector_into(A, s, n, out, t);
    level--;
    if (!ok) return 0;
    out->part = (simple_t*)arena_alloc(A, (uint32_t)out->n * (uint32_t)sizeof(simple_t));
    out->comb = (char*)arena_alloc(A, (uint32_t)out->n);
    for (int i = 0; i < out->n; i++) {
        out->part[i] = t->part[i];
        out->comb[i] = t->comb[i];
        if (t->part[i].nattr) {
            out->part[i].attr = (attr_cond_t*)arena_alloc(A, (uint32_t)t->part[i].nattr * (uint32_t)sizeof(attr_cond_t));
            memcpy(out->part[i].attr, t->attrs[i], (size_t)t->part[i].nattr * sizeof(attr_cond_t));
        }
    }
    return 1;
}

static int parse_selector_into(arena_t* A, const char* s, uint32_t n, selector_t* out, sel_tmp_t* T) {
    memset(out, 0, sizeof(*out));
    uint32_t i = 0;
    char comb = ' ';
    while (i < n) {
        while (i < n && is_ws(s[i])) i++;
        if (i >= n) break;
        if (s[i] == '>' || s[i] == '+' || s[i] == '~') {
            comb = s[i];
            i++;
            continue;
        }
        if (out->n >= SEL_PARTS) return 0;
        simple_t* p = &T->part[out->n];
        T->comb[out->n] = comb;
        comb = ' ';
        int any = 0;
        char* cls[16];
        int ncls = 0;
        while (i < n && !is_ws(s[i]) && s[i] != '>' && s[i] != '+' && s[i] != '~') {
            if (s[i] == '*') { i++; any = 1; continue; }
            if (s[i] == '#' || s[i] == '.') {
                char k = s[i++];
                uint32_t st = i;
                i = ident_end(s, i, n);
                if (i == st) return 0;
                char* name = ident_dup(A, s + st, i - st);
                if (k == '#') { p->id = name; out->spec += 0x10000; }
                else if (ncls < 16) { cls[ncls++] = name; out->spec += 0x100; }
                any = 1;
                continue;
            }
            if (s[i] == '[') {
                uint32_t st = ++i;
                while (i < n && is_ws(s[i])) i++;
                st = i;
                i = ident_end(s, i, n);
                if (i == st) return 0;
                attr_cond_t ac;
                memset(&ac, 0, sizeof(ac));
                ac.name = ident_lc(A, s + st, i - st);
                while (i < n && is_ws(s[i])) i++;
                if (i < n && s[i] != ']') {
                    if (s[i] != '=') ac.op = s[i++]; else ac.op = '=';
                    if (i < n && s[i] == '=') i++;
                    while (i < n && is_ws(s[i])) i++;
                    char q = (i < n && (s[i] == '"' || s[i] == '\'')) ? s[i++] : 0;
                    uint32_t vs = i;
                    while (i < n && (q ? s[i] != q : (s[i] != ']' && !is_ws(s[i])))) {
                        if (s[i] == '\\') i = escape_end(s, i, n);
                        else i++;
                    }
                    ac.val = ident_dup(A, s + vs, i - vs);
                    if (q) { if (i >= n) return 0; i++; }
                    while (i < n && is_ws(s[i])) i++;
                    if (i < n && (s[i] == 'i' || s[i] == 'I')) { ac.icase = 1; i++; }
                }
                while (i < n && s[i] != ']') i++;
                if (i >= n) return 0;
                i++;
                if (p->nattr < MAX_ATTRS) T->attrs[out->n][p->nattr++] = ac;
                else p->never = 1;
                out->spec += 0x100;
                any = 1;
                continue;
            }
            if (s[i] == ':') {
                i++;
                int element = 0;
                if (i < n && s[i] == ':') { i++; element = 1; }
                uint32_t st = i;
                i = ident_end(s, i, n);
                if (i == st) return 0;
                char* ps = ident_lc(A, s + st, i - st);
                uint32_t as = 0, ae = 0;
                if (i < n && s[i] == '(') {
                    as = i + 1;
                    ae = paren_end(s, i, n);
                    if (ae == n) return 0;
                    i = ae + 1;
                }
                uint32_t ps_spec = element ? 1 : 0x100;
                if (element) p->never = 1;                              /* ::before & co: not real elements */
                else if (strcmp(ps, "first-child") == 0) p->first_child = 1;
                else if (strcmp(ps, "last-child") == 0) p->last_child = 1;
                else if (strcmp(ps, "only-child") == 0) p->only_child = 1;
                else if (strcmp(ps, "first-of-type") == 0) p->first_type = 1;
                else if (strcmp(ps, "last-of-type") == 0) p->last_type = 1;
                else if (strcmp(ps, "only-of-type") == 0) p->only_type = 1;
                else if (strcmp(ps, "link") == 0 || strcmp(ps, "visited") == 0 || strcmp(ps, "any-link") == 0) p->link = 1;
                else if (strcmp(ps, "checked") == 0) p->checked = 1;
                else if (strcmp(ps, "disabled") == 0) p->disabled = 1;
                else if (strcmp(ps, "enabled") == 0) p->enabled = 1;
                else if (strcmp(ps, "empty") == 0) p->empty = 1;
                else if (strcmp(ps, "root") == 0) p->tag = "html";
                else if (strcmp(ps, "scope") == 0) { }
                else if (as && (strcmp(ps, "nth-child") == 0 || strcmp(ps, "nth-last-child") == 0 ||
                                strcmp(ps, "nth-of-type") == 0 || strcmp(ps, "nth-last-of-type") == 0)) {
                    func_cond_t* f = (func_cond_t*)arena_alloc(A, sizeof(func_cond_t));
                    f->nth_kind = ps[4] == 'c' ? 1 : ps[4] == 'l' ? (ps[9] == 'c' ? 2 : 4) : 3;
                    uint32_t ofs = ae;
                    for (uint32_t k = as; k + 2 < ae; k++) if (s[k] == 'o' && strncmp(s + k, "of ", 3) == 0) { ofs = k; break; }
                    parse_nth(s + as, ofs - as, &f->nth_a, &f->nth_b);
                    f->next = p->funcs;
                    p->funcs = f;
                }
                else if (as && (strcmp(ps, "not") == 0 || strcmp(ps, "is") == 0 || strcmp(ps, "where") == 0 || strcmp(ps, "matches") == 0 ||
                                strcmp(ps, "-webkit-any") == 0 || strcmp(ps, "-moz-any") == 0)) {
                    func_cond_t* f = (func_cond_t*)arena_alloc(A, sizeof(func_cond_t));
                    f->nsels = parse_sel_list(A, s + as, ae - as, &f->sels);
                    f->negate = strcmp(ps, "not") == 0;
                    f->next = p->funcs;
                    p->funcs = f;
                    if (!f->nsels) p->never = 1;
                    ps_spec = 0;
                    if (strcmp(ps, "where") != 0)
                        for (int k = 0; k < f->nsels; k++)
                            if (f->sels[k].spec > ps_spec) ps_spec = f->sels[k].spec;
                }
                else p->never = 1;                   /* :hover, :focus, ... */
                out->spec += ps_spec;
                any = 1;
                continue;
            }
            if (is_ident(s[i])) {
                uint32_t st = i;
                i = ident_end(s, i, n);
                if (i == st) return 0;
                p->tag = ident_lc(A, s + st, i - st);
                out->spec += 1;
                any = 1;
                continue;
            }
            return 0;                                /* garbage */
        }
        if (!any) return 0;
        if (ncls) {
            p->cls = (char**)arena_alloc(A, (uint32_t)ncls * (uint32_t)sizeof(char*));
            memcpy(p->cls, cls, (size_t)ncls * sizeof(char*));
            p->ncls = ncls;
        }
        out->n++;
    }
    return out->n > 0;
}

static int sel_matches(const selector_t* sel, dom_node_t* e);

static int attr_ok(const attr_cond_t* c, dom_node_t* e) {
    const char* v = dom_attr(e, c->name);
    if (!v) return 0;
    if (!c->val) return 1;
    size_t vl = strlen(v), cl = strlen(c->val);
    int (*cmp)(const char*, const char*, size_t) = c->icase ? strncasecmp : strncmp;
    switch (c->op) {
    case '=': return vl == cl && cmp(v, c->val, cl) == 0;
    case '^': return cl && vl >= cl && cmp(v, c->val, cl) == 0;
    case '$': return cl && vl >= cl && cmp(v + vl - cl, c->val, cl) == 0;
    case '*':
        if (!cl) return 0;
        for (size_t i = 0; i + cl <= vl; i++) if (cmp(v + i, c->val, cl) == 0) return 1;
        return 0;
    case '|': return (vl == cl && cmp(v, c->val, cl) == 0) || (vl > cl && cmp(v, c->val, cl) == 0 && v[cl] == '-');
    case '~':
        for (const char* w = v; *w; ) {
            while (*w == ' ') w++;
            size_t l = 0;
            while (w[l] && w[l] != ' ') l++;
            if (l == cl && cmp(w, c->val, cl) == 0) return 1;
            w += l;
        }
        return 0;
    }
    return 0;
}

/* position among element siblings (1-based); of_type: same tag only; from_end: counted backwards */
static int sib_index(dom_node_t* e, int of_type, int from_end) {
    int k = 1;
    for (dom_node_t* s = from_end ? e->next : e->prev; s; s = from_end ? s->next : s->prev)
        if (s->type == DOM_ELEM && (!of_type || strcmp(s->tag, e->tag) == 0)) k++;
    return k;
}

static int nth_ok(int a, int b, int k) {
    if (a == 0) return k == b;
    int d = k - b;
    if (d % a) return 0;
    return d / a >= 0;
}

static int match_simple(const simple_t* p, dom_node_t* e) {
    if (e->type != DOM_ELEM || p->never) return 0;
    if (p->tag && strcmp(p->tag, e->tag) != 0) return 0;
    if (p->id) {
        const char* id = dom_attr(e, "id");
        if (!id || strcmp(id, p->id) != 0) return 0;
    }
    for (int i = 0; i < p->ncls; i++) if (!dom_has_class(e, p->cls[i])) return 0;
    for (int i = 0; i < p->nattr; i++) if (!attr_ok(&p->attr[i], e)) return 0;
    if (p->link && !((strcmp(e->tag, "a") == 0 || strcmp(e->tag, "area") == 0) && dom_attr(e, "href"))) return 0;
    if (p->checked) {
        int on = e->form_init ? e->checked : (dom_attr(e, "checked") || dom_attr(e, "selected"));
        if (!on) return 0;
    }
    if (p->disabled && !dom_attr(e, "disabled")) return 0;
    if (p->enabled && dom_attr(e, "disabled")) return 0;
    if (p->empty) {
        for (dom_node_t* c = e->first; c; c = c->next)
            if (c->type == DOM_ELEM || (c->type == DOM_TEXT && c->text_len)) return 0;
    }
    if (p->first_child || p->only_child) for (dom_node_t* s = e->prev; s; s = s->prev) if (s->type == DOM_ELEM) return 0;
    if (p->last_child || p->only_child) for (dom_node_t* s = e->next; s; s = s->next) if (s->type == DOM_ELEM) return 0;
    if ((p->first_type || p->only_type) && sib_index(e, 1, 0) != 1) return 0;
    if ((p->last_type || p->only_type) && sib_index(e, 1, 1) != 1) return 0;
    for (const func_cond_t* f = p->funcs; f; f = f->next) {
        if (f->nth_kind) {
            int k = sib_index(e, f->nth_kind >= 3, f->nth_kind == 2 || f->nth_kind == 4);
            if (!nth_ok(f->nth_a, f->nth_b, k)) return 0;
            continue;
        }
        int ok = 0;
        for (int i = 0; i < f->nsels && !ok; i++) ok = sel_matches(&f->sels[i], e);
        if (f->negate ? ok : !ok) return 0;
    }
    return 1;
}

static dom_node_t* prev_elem(dom_node_t* e) {
    for (dom_node_t* s = e->prev; s; s = s->prev) if (s->type == DOM_ELEM) return s;
    return NULL;
}

static int match_from(const selector_t* sel, int idx, dom_node_t* e) {
    if (!match_simple(&sel->part[idx], e)) return 0;
    if (idx == 0) return 1;
    switch (sel->comb[idx]) {
    case '>': {
        dom_node_t* p = e->parent;
        return p && p->type == DOM_ELEM && match_from(sel, idx - 1, p);
    }
    case '+': {
        dom_node_t* s = prev_elem(e);
        return s && match_from(sel, idx - 1, s);
    }
    case '~':
        for (dom_node_t* s = prev_elem(e); s; s = prev_elem(s))
            if (match_from(sel, idx - 1, s)) return 1;
        return 0;
    default:
        for (dom_node_t* a = e->parent; a && a->type == DOM_ELEM; a = a->parent)
            if (match_from(sel, idx - 1, a)) return 1;
        return 0;
    }
}

static int sel_matches(const selector_t* sel, dom_node_t* e) {
    return match_from(sel, sel->n - 1, e);
}

/* Split only unescaped commas outside strings, attributes and functions. */
static int parse_sel_list(arena_t* A, const char* s, uint32_t n, selector_t** out) {
    int count = 1;
    for (uint32_t i = 0; i < n; i++) if (s[i] == ',') count++;
    selector_t* sels = (selector_t*)arena_alloc(A, (uint32_t)count * (uint32_t)sizeof(selector_t));
    int k = 0, depth = 0;
    uint32_t st = 0;
    char q = 0;
    for (uint32_t i = 0; i <= n; i++) {
        if (i < n) {
            if (s[i] == '\\') { i = escape_end(s, i, n) - 1; continue; }
            if (q) { if (s[i] == q) q = 0; continue; }
            if (s[i] == '"' || s[i] == '\'') { q = s[i]; continue; }
            if (s[i] == '(' || s[i] == '[') depth++;
            else if (s[i] == ')' || s[i] == ']') depth--;
        }
        if (i == n || (s[i] == ',' && depth == 0)) {
            if (parse_selector(A, s + st, i - st, &sels[k])) k++;
            st = i + 1;
        }
    }
    *out = sels;
    return k;
}

/* ══ style sheet parser ═══════════════════════════════════════════════ */

static int parse_decls(arena_t* A, const char* s, uint32_t n, decl_t** out) {
    /* comments between or inside declarations (a French note with l'auteur in it
     * between two custom properties): blanked first, or they would join the next property name */
    for (uint32_t k = 0; k + 1 < n; k++) {
        if (s[k] != '/' || s[k + 1] != '*') continue;
        char* c = (char*)arena_alloc(A, n + 1);
        memcpy(c, s, n);
        c[n] = 0;
        char q = 0;
        for (uint32_t j = 0; j < n; j++) {
            if (q) { if (c[j] == '\\' && j + 1 < n) j++; else if (c[j] == q) q = 0; continue; }
            if (c[j] == '"' || c[j] == '\'') { q = c[j]; continue; }
            if (c[j] == '/' && j + 1 < n && c[j + 1] == '*') {
                uint32_t e = j + 2;
                while (e + 1 < n && !(c[e] == '*' && c[e + 1] == '/')) e++;
                e = e + 2 > n ? n : e + 2;
                while (j < e) c[j++] = ' ';
                j--;
            }
        }
        s = c;
        break;
    }
    int cap = 8, cnt = 0;
    decl_t* d = (decl_t*)arena_alloc(A, (uint32_t)cap * (uint32_t)sizeof(decl_t));
    uint32_t i = 0;
    while (i < n) {
        while (i < n && (is_ws(s[i]) || s[i] == ';')) i++;
        uint32_t ps = i;
        while (i < n && s[i] != ':' && s[i] != ';' && s[i] != '{') i++;
        if (i < n && s[i] == '{') {                          /* nested rule (CSS nesting): skip it */
            int dep = 0;
            for (; i < n; i++) { if (s[i] == '{') dep++; else if (s[i] == '}' && --dep == 0) { i++; break; } }
            continue;
        }
        if (i >= n || s[i] != ':') { while (i < n && s[i] != ';') i++; continue; }
        uint32_t pe = i;
        while (pe > ps && is_ws(s[pe - 1])) pe--;
        i++;
        uint32_t vs = i;
        int paren = 0;
        char q = 0;
        while (i < n && (s[i] != ';' || paren || q)) {
            if (q) { if (s[i] == q) q = 0; }
            else if (s[i] == '"' || s[i] == '\'') q = s[i];
            else if (s[i] == '(') paren++;
            else if (s[i] == ')') paren--;
            i++;
        }
        uint32_t ve = i;
        while (vs < ve && is_ws(s[vs])) vs++;
        while (ve > vs && is_ws(s[ve - 1])) ve--;
        int imp = 0;
        if (ve - vs >= 10) {
            const char* bang = NULL;
            for (uint32_t k = vs; k < ve; k++) if (s[k] == '!') bang = s + k;
            if (bang && strncasecmp(bang, "!important", 10) == 0) {
                imp = 1;
                ve = (uint32_t)(bang - s);
                while (ve > vs && is_ws(s[ve - 1])) ve--;
            }
        }
        if (pe == ps) continue;
        if (cnt == cap) {
            decl_t* nd = (decl_t*)arena_alloc(A, (uint32_t)cap * 2 * (uint32_t)sizeof(decl_t));
            memcpy(nd, d, (size_t)cnt * sizeof(decl_t));
            d = nd;
            cap *= 2;
        }
        /* custom properties keep their case; others are lowercased */
        d[cnt].prop = (s[ps] == '-' && s[ps + 1] == '-') ? arena_strdup(A, s + ps, pe - ps) : dup_lc(A, s + ps, pe - ps);
        d[cnt].val = arena_strdup(A, s + vs, ve - vs);
        d[cnt].important = imp;
        cnt++;
    }
    *out = d;
    return cnt;
}

static void sheet_add(arena_t* A, css_sheet_t* sh, rule_t r) {
    if (sh->n == sh->cap) {
        int nc = sh->cap ? sh->cap * 2 : 32;
        rule_t* nr = (rule_t*)arena_alloc(A, (uint32_t)nc * (uint32_t)sizeof(rule_t));
        if (sh->n) memcpy(nr, sh->rules, (size_t)sh->n * sizeof(rule_t));
        sh->rules = nr;
        sh->cap = nc;
    }
    sh->rules[sh->n++] = r;
}

/* a length in a media query (px, em, rem) */
static int media_len(const char* s) {
    while (is_ws(*s)) s++;
    int v = 0, frac = 0, fd = 1;
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    if (*s == '.') { s++; while (*s >= '0' && *s <= '9') { if (fd < 1000) { frac = frac * 10 + (*s - '0'); fd *= 10; } s++; } }
    int px = v;
    if (strncasecmp(s, "em", 2) == 0 || strncasecmp(s, "rem", 3) == 0) px = v * 16 + frac * 16 / fd;
    return px;
}

/* range syntax: (width >= 768px), (width<1012px), (400px <= width <= 800px) */
static void media_range(const char* buf, int* mn, int* mx) {
    char c[256];
    int n = 0;
    for (const char* t = buf; *t && n < 255; t++) if (*t != ' ') c[n++] = *t;
    c[n] = 0;
    for (char* k = strstr(c, "width"); k; k = strstr(k + 5, "width")) {
        if (k > c && (k[-1] == '-' || (k[-1] >= 'a' && k[-1] <= 'z'))) continue;   /* min-width, device-width */
        const char* a = k + 5;                      /* width >= V */
        if (*a == '<' || *a == '>') {
            int eq = a[1] == '=';
            int v = media_len(a + 1 + eq);
            if (*a == '>') *mn = eq ? v : v + 1;
            else *mx = eq ? v : v - 1;
        }
        if (k > c && (k[-1] == '<' || k[-1] == '>' || k[-1] == '=')) {    /* V <= width */
            const char* o = k - 1;
            int eq = 0;
            if (*o == '=' && o > c && (o[-1] == '<' || o[-1] == '>')) { eq = 1; o--; }
            if (*o != '<' && *o != '>') continue;
            const char* v0 = o;
            while (v0 > c && v0[-1] != '(' && v0[-1] != ':' && v0[-1] != ',') v0--;
            int v = media_len(v0);
            if (*o == '<') *mn = eq ? v : v + 1;
            else *mx = eq ? v : v - 1;
        }
    }
}

/* one "screen and (min-width: 40em)" alternative: 1 if it can apply, with its width range */
static int media_alt(const char* q, uint32_t n, int* mn, int* mx) {
    char buf[256];
    if (n > 255) n = 255;
    for (uint32_t i = 0; i < n; i++) buf[i] = lc(q[i]);
    buf[n] = 0;
    const char* t = buf;
    while (*t == ' ') t++;
    int neg = strncmp(t, "not ", 4) == 0;
    int ok = 1;
    if (strstr(buf, "print") || strstr(buf, "speech")) ok = 0;
    if (strstr(buf, "prefers-color-scheme") && strstr(buf, "dark")) ok = 0;
    if (strstr(buf, "orientation") && strstr(buf, "portrait")) ok = 0;
    if (strstr(buf, "hover") && strstr(buf, "none")) ok = 0;
    if (strstr(buf, "pointer") && strstr(buf, "coarse")) ok = 0;
    if (strstr(buf, "-webkit-min-device-pixel-ratio") || strstr(buf, "min-resolution")) ok = 0;
    *mn = 0;
    *mx = 0;
    const char* p;
    if ((p = strstr(buf, "min-width"))) { p = strchr(p, ':'); if (p) *mn = media_len(p + 1); }
    if ((p = strstr(buf, "max-width"))) { p = strchr(p, ':'); if (p) *mx = media_len(p + 1); }
    media_range(buf, mn, mx);
    if (neg) {
        /* "not all and (max-width: X)" = min-width X+1 */
        if (*mx && !*mn) { *mn = *mx + 1; *mx = 0; return ok; }
        if (*mn && !*mx) { *mx = *mn - 1; *mn = 0; return ok; }
        return !ok;
    }
    return ok;
}

/* past the comment starting at i */
static uint32_t skip_comment(const char* s, uint32_t n, uint32_t i) {
    i += 2;
    while (i + 1 < n && !(s[i] == '*' && s[i + 1] == '/')) i++;
    return i + 2 > n ? n : i + 2;
}

static void kf_parse(arena_t* A, css_sheet_t* sh, const char* head, uint32_t hn, const char* s, uint32_t n);

static void parse_block_list(arena_t* A, css_sheet_t* sh, const char* s, uint32_t n, int mmin, int mmax) {
    uint32_t i = 0;
    while (i < n) {
        WEB_TICK();
        while (i < n && is_ws(s[i])) i++;
        if (i + 1 < n && s[i] == '/' && s[i + 1] == '*') {
            i += 2;
            while (i + 1 < n && !(s[i] == '*' && s[i + 1] == '/')) i++;
            i += 2;
            continue;
        }
        if (i + 3 < n && memcmp(s + i, "<!--", 4) == 0) { i += 4; continue; }
        if (i + 2 < n && memcmp(s + i, "-->", 3) == 0) { i += 3; continue; }
        if (i >= n) break;
        uint32_t st = i;
        char q = 0;
        while (i < n && (q || (s[i] != '{' && s[i] != ';'))) {
            /* Selector punctuation and escaped quotes belong to the token. */
            if (s[i] == '\\') { i = escape_end(s, i, n); continue; }
            if (q) { if (s[i] == q) q = 0; }
            else if (s[i] == '/' && i + 1 < n && s[i + 1] == '*') { i = skip_comment(s, n, i); continue; }
            else if (s[i] == '"' || s[i] == '\'') q = s[i];
            i++;
        }
        if (i >= n) break;
        if (s[i] == ';') { i++; continue; }                  /* @import ...; @charset */
        uint32_t sel_end = i;
        /* the block, with nesting (@media { ... { } }) */
        uint32_t bs = ++i;
        int depth = 1;
        q = 0;
        while (i < n && depth) {
            if (q) { if (s[i] == q) q = 0; }
            else if (s[i] == '/' && i + 1 < n && s[i + 1] == '*') { i = skip_comment(s, n, i); continue; }   /* an apostrophe in a note is no string */
            else if (s[i] == '"' || s[i] == '\'') q = s[i];
            else if (s[i] == '{') depth++;
            else if (s[i] == '}') depth--;
            if (depth) i++;
        }
        uint32_t be = i;
        if (i < n) i++;
        if (s[st] == '@') {
            /* @media: its rules apply when the query can match; @supports / @layer / @container:
             * their rules apply; skip @font-face, @keyframes, @page... */
            if (strncasecmp(s + st, "@media", 6) == 0) {
                const char* qs = s + st + 6;
                int any = 0, mn = 0, mx = 0;
                /* comma-separated alternatives: the widest range of those that can apply */
                const char* a = qs;
                for (const char* k = qs; k <= s + sel_end; k++) {
                    if (k == s + sel_end || *k == ',') {
                        int amn, amx;
                        if (media_alt(a, (uint32_t)(k - a), &amn, &amx)) {
                            if (!any) { mn = amn; mx = amx; }
                            else { if (amn < mn) mn = amn; if (!amx || (mx && amx > mx)) mx = amx; }
                            any = 1;
                        }
                        a = k + 1;
                    }
                }
                if (any) {
                    if (mmin > mn) mn = mmin;
                    if (mmax && (!mx || mmax < mx)) mx = mmax;
                    parse_block_list(A, sh, s + bs, be - bs, mn, mx);
                }
            } else if (strncasecmp(s + st, "@supports", 9) == 0 || strncasecmp(s + st, "@layer", 6) == 0 ||
                       strncasecmp(s + st, "@container", 10) == 0 || strncasecmp(s + st, "@document", 9) == 0) {
                /* "@supports not (...)" usually guards fallbacks for old browsers: keep those too */
                parse_block_list(A, sh, s + bs, be - bs, mmin, mmax);
            } else if (strncasecmp(s + st, "@keyframes", 10) == 0 || strncasecmp(s + st, "@-webkit-keyframes", 18) == 0) {
                kf_parse(A, sh, s + st, sel_end - st, s + bs, be - bs);
            }
            continue;
        }
        rule_t r;
        memset(&r, 0, sizeof(r));
        r.nsels = parse_sel_list(A, s + st, sel_end - st, &r.sels);
        if (!r.nsels) continue;
        r.ndecls = parse_decls(A, s + bs, be - bs, &r.decls);
        r.media_min = mmin;
        r.media_max = mmax;
        if (r.ndecls) sheet_add(A, sh, r);
    }
}

/* ── the index ────────────────────────────────────────────────────── */

static void bucket_add(arena_t* A, bucket_t* b, int ref) {
    if (b->n == b->cap) {
        int nc = b->cap ? b->cap * 2 : 4;
        int* nr = (int*)arena_alloc(A, (uint32_t)nc * (uint32_t)sizeof(int));
        if (b->n) memcpy(nr, b->refs, (size_t)b->n * sizeof(int));
        b->refs = nr;
        b->cap = nc;
    }
    b->refs[b->n++] = ref;
}

static bucket_t* bucket_get(arena_t* A, bucket_t** table, const char* key, int create) {
    uint32_t h = hash_s(key) % BUCKETS;
    for (bucket_t* b = table[h]; b; b = b->next) if (strcmp(b->key, key) == 0) return b;
    if (!create) return NULL;
    bucket_t* b = (bucket_t*)arena_alloc(A, sizeof(bucket_t));
    b->key = key;
    b->next = table[h];
    table[h] = b;
    return b;
}

static void build_index(css_sheet_t* sh) {
    arena_t* A = sh->A;
    for (int r = 0; r < sh->n && r < (1 << 23); r++) {
        rule_t* rule = &sh->rules[r];
        for (int k = 0; k < rule->nsels && k < 256; k++) {
            simple_t* last = &rule->sels[k].part[rule->sels[k].n - 1];
            int ref = r << 8 | k;
            if (last->never) continue;
            if (last->id) bucket_add(A, bucket_get(A, sh->by_id, last->id, 1), ref);
            /* collect's class buffer holds 95 bytes; long arbitrary utility
             * names must use a tag bucket or the unkeyed bucket instead. */
            else if (last->ncls && strlen(last->cls[0]) < 96)
                bucket_add(A, bucket_get(A, sh->by_class, last->cls[0], 1), ref);
            else if (last->tag) bucket_add(A, bucket_get(A, sh->by_tag, last->tag, 1), ref);
            else bucket_add(A, &sh->any, ref);
        }
    }
    sh->indexed = 1;
}

css_sheet_t* css_parse(arena_t* A, const char* src, uint32_t len, int origin) {
    css_sheet_t* sh = (css_sheet_t*)arena_alloc(A, sizeof(css_sheet_t));
    sh->origin = origin;
    sh->A = A;
    if (A->oom && origin != 0) return sh;            /* out of page memory: no more style sheets */
    parse_block_list(A, sh, src, len, 0, 0);
    if (A->oom && origin != 0) { sh->n = 0; return sh; }
    build_index(sh);
    return sh;
}

static const char DEFAULT_CSS[] =
    "html, body, div, p, h1, h2, h3, h4, h5, h6, ul, ol, dl, dt, dd, pre, form, blockquote, address,"
    " section, article, header, footer, nav, aside, main, figure, figcaption, fieldset, hr, center,"
    " details, summary, legend, menu, hgroup, search { display: block }"
    "head, script, style, title, meta, link, base, template, noscript, datalist,"
    " object, embed, audio, dialog:not([open]), [hidden], area, map, param, source, track { display: none }"
    "iframe { display: inline-block }"
    "li { display: list-item }"
    "table { display: table; border-spacing: 0 }"
    "tr { display: table-row }"
    "td, th { display: table-cell; padding: 2px 4px }"
    "thead, tbody, tfoot { display: table-row-group }"
    "caption { display: block; text-align: center }"
    "img, input, button, select, textarea, svg { display: inline-block }"
    "body { margin: 8px; color: #000; }"
    "p { margin: 8px 0 }"
    "h1 { font-size: 32px; font-weight: bold; margin: 12px 0 }"
    "h2 { font-size: 24px; font-weight: bold; margin: 10px 0 }"
    "h3 { font-size: 18px; font-weight: bold; margin: 9px 0 }"
    "h4, h5, h6 { font-weight: bold; margin: 8px 0 }"
    "ul, ol, menu { margin: 8px 0; padding-left: 24px }"
    "ol { list-style-type: decimal }"
    "ul ul { list-style-type: circle }"
    "dd { margin-left: 24px }"
    "blockquote { margin: 8px 24px }"
    "pre { white-space: pre; margin: 8px 0 }"
    "hr { border-top: 1px solid #888; margin: 8px 0 }"
    "b, strong, th { font-weight: bold }"
    "th { text-align: center }"
    "i, em, cite, var, dfn { font-style: italic }"
    "u, ins { text-decoration: underline }"
    "s, strike, del { text-decoration: line-through }"
    "a:link { color: #0645ad; text-decoration: underline }"
    "center { text-align: center }"
    "center table { text-align: left }"   /* (quirks mode, as browsers do: HN) */
    "big { font-size: 20px }"
    "small, sub, sup { font-size: 13px }"
    "code, kbd, samp, tt { color: #333; font-family: monospace }"
    "pre, textarea { font-family: monospace }"
    "mark { background-color: #ff0 }"
    "button { padding: 2px 6px; border: 1px solid #777; background-color: #ddd }"
    "fieldset { border: 1px solid #999; padding: 4px 8px; margin: 8px 2px }"
    "legend { font-weight: bold }"
    "details:not([open]) > :not(summary) { display: none }";

css_sheet_t* css_default_sheet(arena_t* A) {
    return css_parse(A, DEFAULT_CSS, (uint32_t)sizeof(DEFAULT_CSS) - 1, 0);
}

/* ══ values ═══════════════════════════════════════════════════════════ */

static const struct { const char* name; uint32_t rgb; } COLORS[] = {
    { "black", 0x000000 }, { "white", 0xFFFFFF }, { "red", 0xFF0000 }, { "green", 0x008000 },
    { "blue", 0x0000FF }, { "yellow", 0xFFFF00 }, { "cyan", 0x00FFFF }, { "aqua", 0x00FFFF },
    { "magenta", 0xFF00FF }, { "fuchsia", 0xFF00FF }, { "gray", 0x808080 }, { "grey", 0x808080 },
    { "silver", 0xC0C0C0 }, { "maroon", 0x800000 }, { "olive", 0x808000 }, { "lime", 0x00FF00 },
    { "navy", 0x000080 }, { "teal", 0x008080 }, { "purple", 0x800080 }, { "orange", 0xFFA500 },
    { "pink", 0xFFC0CB }, { "brown", 0xA52A2A }, { "gold", 0xFFD700 }, { "beige", 0xF5F5DC },
    { "coral", 0xFF7F50 }, { "crimson", 0xDC143C }, { "darkblue", 0x00008B }, { "darkgreen", 0x006400 },
    { "darkred", 0x8B0000 }, { "darkgray", 0xA9A9A9 }, { "darkgrey", 0xA9A9A9 }, { "darkorange", 0xFF8C00 },
    { "dodgerblue", 0x1E90FF }, { "firebrick", 0xB22222 }, { "forestgreen", 0x228B22 },
    { "indigo", 0x4B0082 }, { "ivory", 0xFFFFF0 }, { "khaki", 0xF0E68C }, { "lavender", 0xE6E6FA },
    { "lightblue", 0xADD8E6 }, { "lightgray", 0xD3D3D3 }, { "lightgrey", 0xD3D3D3 }, { "lightgreen", 0x90EE90 },
    { "lightyellow", 0xFFFFE0 }, { "limegreen", 0x32CD32 }, { "linen", 0xFAF0E6 }, { "midnightblue", 0x191970 },
    { "orangered", 0xFF4500 }, { "orchid", 0xDA70D6 }, { "royalblue", 0x4169E1 }, { "salmon", 0xFA8072 },
    { "seagreen", 0x2E8B57 }, { "skyblue", 0x87CEEB }, { "slategray", 0x708090 }, { "steelblue", 0x4682B4 },
    { "tan", 0xD2B48C }, { "tomato", 0xFF6347 }, { "turquoise", 0x40E0D0 }, { "violet", 0xEE82EE },
    { "wheat", 0xF5DEB3 }, { "whitesmoke", 0xF5F5F5 }, { "yellowgreen", 0x9ACD32 }, { "aliceblue", 0xF0F8FF },
    { "ghostwhite", 0xF8F8FF }, { "honeydew", 0xF0FFF0 }, { "mintcream", 0xF5FFFA }, { "snow", 0xFFFAFA },
    { "rebeccapurple", 0x663399 }, { "chocolate", 0xD2691E }, { "darkslategray", 0x2F4F4F },
    { "dimgray", 0x696969 }, { "gainsboro", 0xDCDCDC }, { "hotpink", 0xFF69B4 }, { "deeppink", 0xFF1493 },
    { "lightcoral", 0xF08080 }, { "lightpink", 0xFFB6C1 }, { "mediumblue", 0x0000CD }, { "plum", 0xDDA0DD },
    { "sienna", 0xA0522D }, { "springgreen", 0x00FF7F }, { "cornflowerblue", 0x6495ED }, { "cadetblue", 0x5F9EA0 },
    { "darkslateblue", 0x483D8B }, { "lightslategray", 0x778899 }, { "slateblue", 0x6A5ACD },
    { "darkcyan", 0x008B8B }, { "darkmagenta", 0x8B008B }, { "darkviolet", 0x9400D3 }, { "goldenrod", 0xDAA520 },
    { "lightcyan", 0xE0FFFF }, { "lightsteelblue", 0xB0C4DE }, { "mediumseagreen", 0x3CB371 },
    { "navajowhite", 0xFFDEAD }, { "oldlace", 0xFDF5E6 }, { "papayawhip", 0xFFEFD5 }, { "peru", 0xCD853F },
    { "seashell", 0xFFF5EE }, { "antiquewhite", 0xFAEBD7 }, { "azure", 0xF0FFFF }, { "bisque", 0xFFE4C4 },
    { "blanchedalmond", 0xFFEBCD }, { "cornsilk", 0xFFF8DC }, { "floralwhite", 0xFFFAF0 },
    { "lemonchiffon", 0xFFFACD }, { "mistyrose", 0xFFE4E1 }, { "moccasin", 0xFFE4B5 }, { "palegreen", 0x98FB98 },
};

static int hexv(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    c = lc(c);
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

/* a number (with optional fraction) x 1000; advances *p */
static int num_milli(const char** p) {
    const char* s = *p;
    int neg = 0;
    if (*s == '-') { neg = 1; s++; } else if (*s == '+') s++;
    long v = 0, frac = 0, fd = 1;
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    if (*s == '.') { s++; while (*s >= '0' && *s <= '9') { if (fd < 10000) { frac = frac * 10 + (*s - '0'); fd *= 10; } s++; } }
    *p = s;
    long m = v * 1000 + frac * 1000 / fd;
    return (int)(neg ? -m : m);
}

static uint32_t hsl_rgb(int h, int s, int l) {     /* h 0..360, s/l 0..100 */
    h %= 360;
    if (h < 0) h += 360;
    int c = (100 - (2 * l - 100 < 0 ? 100 - 2 * l : 2 * l - 100)) * s / 100;   /* chroma x100 */
    int hp = h * 100 / 60;
    int xm = hp % 200 - 100;
    int x = c * (100 - (xm < 0 ? -xm : xm)) / 100;
    int r = 0, g = 0, b = 0;
    if (hp < 100) { r = c; g = x; } else if (hp < 200) { r = x; g = c; } else if (hp < 300) { g = c; b = x; }
    else if (hp < 400) { g = x; b = c; } else if (hp < 500) { r = x; b = c; } else { r = c; b = x; }
    int m = l - c / 2;
    r = (r + m) * 255 / 100; g = (g + m) * 255 / 100; b = (b + m) * 255 / 100;
    r = r < 0 ? 0 : r; g = g < 0 ? 0 : g; b = b < 0 ? 0 : b;
    r = r > 255 ? 255 : r; g = g > 255 ? 255 : g; b = b > 255 ? 255 : b;
    return (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b;
}

/* *ok: 1 a color, 0 not one, 2 transparent (alpha ~ 0); *alpha: 0..255 */
static uint32_t color_alpha(const char* s, int* ok, int* alpha);

uint32_t css_color(const char* s, int* ok) {
    int a;
    return color_alpha(s, ok, &a);
}

/* a colour that may be see-through: 1 - alpha in the top byte (0: opaque),
 * as the renderer blends it (backgrounds and borders). *ok 2 only when
 * nothing shows at all. */
uint32_t css_color_blend(const char* s, int* ok) {
    int a;
    uint32_t c = color_alpha(s, ok, &a);
    if (*ok == 0) return 0;
    if (a <= 0) { *ok = 2; return 0; }
    *ok = 1;
    return a >= 255 ? c : c | (uint32_t)(255 - a) << 24;
}

static uint32_t color_alpha(const char* s, int* ok, int* alpha) {
    *ok = 0;
    *alpha = 255;
    while (is_ws(*s)) s++;
    if (*s == '#') {
        s++;
        int n = 0;
        while (hexv(s[n]) >= 0) n++;
        if (n == 3 || n == 4) {
            if (n == 4) *alpha = hexv(s[3]) * 17;
            *ok = (n == 4 && hexv(s[3]) < 2) ? 2 : 1;
            return (uint32_t)(hexv(s[0]) * 17) << 16 | (uint32_t)(hexv(s[1]) * 17) << 8 | (uint32_t)(hexv(s[2]) * 17);
        }
        if (n == 6 || n == 8) {
            uint32_t v = 0;
            for (int i = 0; i < 6; i++) v = v * 16 + (uint32_t)hexv(s[i]);
            if (n == 8) *alpha = hexv(s[6]) * 16 + hexv(s[7]);
            *ok = (n == 8 && hexv(s[6]) * 16 + hexv(s[7]) < 26) ? 2 : 1;
            return v;
        }
        return 0;
    }
    if (strncasecmp(s, "transparent", 11) == 0) { *ok = 2; *alpha = 0; return 0; }
    int is_rgb = strncasecmp(s, "rgb", 3) == 0, is_hsl = strncasecmp(s, "hsl", 3) == 0;
    if (is_rgb || is_hsl) {
        const char* p = strchr(s, '(');
        if (!p) return 0;
        int c[4] = { 0, 0, 0, 1000 };
        p++;
        for (int i = 0; i < 4; i++) {
            while (*p && (is_ws(*p) || *p == ',' || *p == '/')) p++;
            if (*p == ')' || !*p) { if (i < 3) return 0; break; }
            if (strncasecmp(p, "none", 4) == 0) { p += 4; c[i] = 0; continue; }
            const char* b = p;
            int m = num_milli(&p);
            if (p == b) return 0;
            if (*p == '%') {
                p++;
                if (i == 3) m /= 100;
                else if (is_rgb) m = m * 255 / 100;
            } else if (strncasecmp(p, "deg", 3) == 0) p += 3;
            c[i] = m;
        }
        *alpha = c[3] <= 0 ? 0 : c[3] >= 1000 ? 255 : c[3] * 255 / 1000;
        if (c[3] < 100) { *ok = 2; return 0; }
        *ok = 1;
        if (is_hsl) return hsl_rgb(c[0] / 1000, c[1] / 1000, c[2] / 1000);
        for (int i = 0; i < 3; i++) { c[i] /= 1000; if (c[i] > 255) c[i] = 255; if (c[i] < 0) c[i] = 0; }
        return (uint32_t)c[0] << 16 | (uint32_t)c[1] << 8 | (uint32_t)c[2];
    }
    char name[24];
    int n = 0;
    while (s[n] && is_ident(s[n]) && n < 23) { name[n] = lc(s[n]); n++; }
    name[n] = 0;
    for (uint32_t i = 0; i < sizeof(COLORS) / sizeof(COLORS[0]); i++)
        if (strcmp(name, COLORS[i].name) == 0) { *ok = 1; return COLORS[i].rgb; }
    return 0;
}

#include "css_math.h"

/* a length in px; pct_of: base for %, -1 = percentages unknown (returns LEN_AUTO) */
static int parse_len_kind(const char* s, int font_px, int pct_of, int* is_pct, int* kind) {
    if (is_pct) *is_pct = 0;
    if (kind) *kind = CSS_MATH_NUMBER;
    if (!s) return LEN_AUTO;
    /* Bound token scanning as well as recursion and arithmetic work. */
    int size = 0;
    while (s[size]) { if (++size > 4096) return LEN_AUTO; }
    while (is_ws(*s)) s++;
    css_math_parser p = { s, font_px, pct_of, 0, 0, 0, 0 };
    int math = strncasecmp(s, "calc(", 5) == 0 || strncasecmp(s, "min(", 4) == 0 ||
               strncasecmp(s, "max(", 4) == 0 || strncasecmp(s, "clamp(", 6) == 0;
    css_math_value v;
    if (math) v = calc_factor(&p);
    else {
        int neg = *p.s == '-';
        if (*p.s == '+' || *p.s == '-') p.s++;
        v = len_term(&p);
        if (neg) { v.n = -v.n; p.percent = -p.percent; }
    }
    const char* end = p.s;
    css_math_ws(&p);
    if (p.bad || *p.s) return LEN_AUTO;
    if (math && v.dim == CSS_MATH_NUMBER && v.n) return LEN_AUTO;
    if (kind) *kind = v.dim;
    if (v.dim == CSS_MATH_PERCENT) {
        /* The style ABI has an integer percentage slot, not a px/% pair.
         * Keep pure percentages; never pass a mixed expression to its
         * viewport-based max-width fallback. */
        if (is_pct) *is_pct = (int)(v.n / CSS_MATH_ONE);
        return LEN_AUTO;
    }
    if (!math && end > s && end[-1] == '%' && is_pct) *is_pct = p.percent;
    return (int)(v.n / CSS_MATH_ONE);
}

static int parse_len(const char* s, int font_px, int pct_of, int* is_pct) {
    return parse_len_kind(s, font_px, pct_of, is_pct, NULL);
}

/* Shorthands need whole component values, including spaces inside functions
 * and strings. Never parse a truncated token. Separators / and , are tokens
 * only at depth zero; quoted URLs and divisions inside calc stay intact.
 * Bound the complete list to 4096 bytes and each component to 511 bytes. */
#define CSS_VALUE_TOKEN 512
typedef struct { const char* p; int left; } css_value_tokens;

/* 1 token, 0 end, -1 malformed/over limit. */
static int css_next_value(css_value_tokens* it, char* out) {
    while (is_ws(*it->p)) {
        if (--it->left < 0) return -1;
        it->p++;
    }
    int n = 0, depth = 0;
    char quote = 0;
    while (*it->p) {
        char c = *it->p;
        if (!quote && !depth && (is_ws(c) || (n && (c == '/' || c == ',')))) break;
        if (--it->left < 0 || n == CSS_VALUE_TOKEN - 1) return -1;
        out[n++] = c;
        it->p++;
        if (c == '\\') {
            if (!*it->p || --it->left < 0 || n == CSS_VALUE_TOKEN - 1) return -1;
            out[n++] = *it->p++;
        } else if (quote) {
            if (c == quote) quote = 0;
        } else if (c == '\'' || c == '"') quote = c;
        else if (c == '(') { if (++depth > CSS_MATH_DEPTH) return -1; }
        else if (c == ')') { if (!depth) return -1; depth--; }
        else if (!depth && (c == '/' || c == ',')) break;
    }
    out[n] = 0;
    return quote || depth ? -1 : n != 0;
}

static int scale_for(int px) {
    if (px <= 18) return 1;
    if (px <= 36) return 2;
    if (px <= 54) return 3;
    return 4;
}

/* font-family: a monospace one? */
static int is_mono_family(const char* v) {
    static const char* const names[] = { "monospace", "mono", "courier", "consolas", "menlo", "monaco",
                                         "lucida console", "fixed", "ui-monospace" };
    char b[160];
    uint32_t n = 0;
    for (; v[n] && n < sizeof(b) - 1; n++) b[n] = (char)(v[n] >= 'A' && v[n] <= 'Z' ? v[n] + 32 : v[n]);
    b[n] = 0;
    /* the first family listed decides (fallbacks after it rarely matter) */
    char* comma = strchr(b, ',');
    if (comma) *comma = 0;
    for (uint32_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) if (strstr(b, names[i])) return 1;
    return 0;
}

/* line-height: normal | 1.5 | 150% | 24px | 1.2em */
static void set_line_height(style_t* st, const char* v) {
    css_value_tokens it = { v, 4096 };
    char token[CSS_VALUE_TOKEN], extra[CSS_VALUE_TOKEN];
    if (css_next_value(&it, token) != 1 || css_next_value(&it, extra) != 0) return;
    if (strcasecmp(token, "normal") == 0) { st->lh_px = 0; st->lh_pct = 0; return; }
    css_math_parser p = { token, st->font_px, -1, 0, 0, 0, 0 };
    css_math_value value = calc_factor(&p);
    css_math_ws(&p);
    if (p.bad || *p.s || value.n < 0) return;
    int64_t n = value.n / (value.dim == CSS_MATH_NUMBER ? CSS_MATH_ONE / 100 : CSS_MATH_ONE);
    if (n <= 0 || n >= 1000) return;
    st->lh_px = value.dim == CSS_MATH_LENGTH ? (int16_t)n : 0;
    st->lh_pct = value.dim == CSS_MATH_LENGTH ? 0 : (int16_t)n;
}

static void set_font_px(style_t* st, int px) {
    if (px < 6) px = 6;
    if (px > 96) px = 96;
    st->font_px = px;
    st->scale = (uint8_t)scale_for(px);
}

/* "1px 2px" -> four sides */
static void four(const char* v, int font_px, int* out, int* auto_lr) {
    int vals[4], n = 0;
    int autos[4] = { 0, 0, 0, 0 };
    css_value_tokens it = { v, 4096 };
    char token[CSS_VALUE_TOKEN];
    int got;
    while ((got = css_next_value(&it, token)) == 1) {
        if (n == 4) return;
        int l = parse_len(token, font_px, -1, NULL);
        autos[n] = strcasecmp(token, "auto") == 0;
        if (l == LEN_AUTO && !autos[n]) return;
        vals[n++] = l == LEN_AUTO ? 0 : l;
    }
    if (got < 0 || n == 0) return;
    if (n == 1) { vals[1] = vals[2] = vals[3] = vals[0]; autos[1] = autos[2] = autos[3] = autos[0]; }
    else if (n == 2) { vals[2] = vals[0]; vals[3] = vals[1]; autos[2] = autos[0]; autos[3] = autos[1]; }
    else if (n == 3) { vals[3] = vals[1]; autos[3] = autos[1]; }
    for (int i = 0; i < 4; i++) out[i] = vals[i] < -2000 ? -2000 : vals[i] > 2000 ? 2000 : vals[i];
    if (auto_lr) *auto_lr = autos[1] && autos[3];
}

static int has_word(const char* v, const char* w) {
    uint32_t l = (uint32_t)strlen(w);
    for (const char* p = v; *p; p++)
        if (strncasecmp(p, w, l) == 0 && (p == v || !is_ident(p[-1])) && !is_ident(p[l])) return 1;
    return 0;
}

/* "1px solid red" for one side (or all if side < 0) */
static void border_side(style_t* st, const char* v, int side) {
    int width = 0, have_w = 0, ok = 0;
    uint32_t color = st->color;
    css_value_tokens it = { v, 4096 };
    char tok[CSS_VALUE_TOKEN];
    int none = 0, got, count = 0;
    while ((got = css_next_value(&it, tok)) == 1) {
        if (++count > 3) return;
        int l = parse_len(tok, st->font_px, -1, NULL);
        if (l != LEN_AUTO) { if (have_w || l < 0) return; width = l; have_w = 1; }
        else if (strcasecmp(tok, "thin") == 0) { width = 1; have_w = 1; }
        else if (strcasecmp(tok, "medium") == 0) { width = 2; have_w = 1; }
        else if (strcasecmp(tok, "thick") == 0) { width = 4; have_w = 1; }
        else if (strcasecmp(tok, "none") == 0 || strcasecmp(tok, "hidden") == 0) none = 1;
        else if (strcasecmp(tok, "solid") == 0 || strcasecmp(tok, "dashed") == 0 ||
                 strcasecmp(tok, "dotted") == 0 || strcasecmp(tok, "double") == 0 ||
                 strcasecmp(tok, "groove") == 0 || strcasecmp(tok, "ridge") == 0 ||
                 strcasecmp(tok, "inset") == 0 || strcasecmp(tok, "outset") == 0) {}
        else if (strcasecmp(tok, "currentcolor") == 0) color = st->color;
        else {
            uint32_t c = css_color_blend(tok, &ok);
            if (ok == 1) color = c;
            else if (ok == 2) none = 1;              /* a transparent border draws nothing */
            else return;
        }
    }
    if (got < 0 || !count) return;
    if (!have_w) width = none ? 0 : 2;
    if (none) width = 0;
    if (width > 20) width = 20;
    if (width < 0) width = 0;
    for (int i = 0; i < 4; i++) {
        if (side >= 0 && i != side) continue;
        st->border[i] = width;
        st->border_color[i] = color;
    }
}

/* ── custom properties ────────────────────────────────────────────── */

#include "css_vars.h"

/* "1", "0.5", "2" -> x100 */
static int flex_number(const char* v, int* number) {
    css_value_tokens it = { v, 4096 };
    char token[CSS_VALUE_TOKEN], extra[CSS_VALUE_TOKEN];
    if (css_next_value(&it, token) != 1 || css_next_value(&it, extra) != 0) return 0;
    css_math_parser p = { token, 16, -1, 0, 0, 0, 0 };
    css_math_value value = calc_factor(&p);
    css_math_ws(&p);
    if (p.bad || *p.s || value.dim != CSS_MATH_NUMBER || value.n < 0) return 0;
    int64_t scaled = value.n / (CSS_MATH_ONE / 100);
    if (scaled > 2147483647) return 0;
    *number = (int)scaled;
    return 1;
}

static int flex_num(const char* v) {
    int n = 0;
    flex_number(v, &n);
    return n;
}

static int flex_align(const char* v) {
    if (has_word(v, "center")) return FA_CENTER;
    if (has_word(v, "flex-end") || has_word(v, "end") || has_word(v, "self-end") || has_word(v, "last")) return FA_END;
    if (has_word(v, "flex-start") || has_word(v, "start") || has_word(v, "self-start") || has_word(v, "baseline")) return FA_START;
    if (has_word(v, "stretch") || has_word(v, "normal")) return FA_STRETCH;
    return FA_AUTO;
}

/* background-image / the shorthand: url(...) and linear-gradient(...) */
static void bg_layers(style_t* st, const char* v) {
    const char* u = strstr(v, "url(");
    if (u) {
        u += 4;
        while (is_ws(*u) || *u == '"' || *u == '\'') u++;
        const char* e = u;
        while (*e && *e != ')' && *e != '"' && *e != '\'') e++;
        if (e > u) { st->bg_url = u; st->bg_url_len = (uint16_t)(e - u > 2000 ? 2000 : e - u); }
    }
    const char* g = strstr(v, "linear-gradient(");
    if (!g) g = strstr(v, "repeating-linear-gradient(");
    if (g && !u) {
        st->grad_src = g - v >= 10 && strncmp(g - 10, "repeating-", 10) == 0 ? g - 10 : g;
        g = strchr(g, '(') + 1;
        st->grad_dir = GRAD_DOWN;
        int ncol = 0;
        /* the comma-separated parts at depth 0: a direction, then colour stops */
        const char* p = g;
        while (*p && *p != ')') {
            while (is_ws(*p)) p++;
            const char* part = p;
            int depth = 0;
            while (*p && !(depth == 0 && (*p == ',' || *p == ')'))) {
                if (*p == '(') depth++;
                else if (*p == ')') depth--;
                p++;
            }
            int ok;
            uint32_t c = css_color(part, &ok);
            if (ok == 1) {
                if (!ncol) st->grad_from = c;
                st->grad_to = c;
                ncol++;
            } else if (!ncol) {
                if (strncmp(part, "to right", 8) == 0) st->grad_dir = GRAD_RIGHT;
                else if (strncmp(part, "to left", 7) == 0) st->grad_dir = GRAD_LEFT;
                else if (strncmp(part, "to top", 6) == 0) st->grad_dir = GRAD_UP;
                else if (part[0] >= '0' && part[0] <= '9') {
                    int deg = 0;
                    for (const char* q = part; *q >= '0' && *q <= '9'; q++) deg = deg * 10 + (*q - '0');
                    deg %= 360;
                    st->grad_dir = deg >= 45 && deg < 135 ? GRAD_RIGHT : deg >= 135 && deg < 225 ? GRAD_DOWN :
                                   deg >= 225 && deg < 315 ? GRAD_LEFT : GRAD_UP;
                }
            }
            if (*p == ',') p++;
        }
        if (ncol) { st->bg_grad = 1; if (!st->has_bg) { st->bg = st->grad_from; } }
    }
}

static void bg_repeat_of(style_t* st, const char* v) {
    if (has_word(v, "no-repeat")) st->bg_repeat = BG_NO_REPEAT;
    else if (has_word(v, "repeat-x")) st->bg_repeat = BG_REPEAT_X;
    else if (has_word(v, "repeat-y")) st->bg_repeat = BG_REPEAT_Y;
    else if (has_word(v, "repeat") || has_word(v, "space") || has_word(v, "round")) st->bg_repeat = BG_REPEAT;
}

static void bg_size_of(style_t* st, const char* v, int shorthand) {
    css_value_tokens it = { v, 4096 };
    char token[CSS_VALUE_TOKEN];
    int sizes[2] = { LEN_AUTO, LEN_AUTO }, n = 0, got, mode = BG_SIZE_PX, pct = 0;
    while (n < 2 && (got = css_next_value(&it, token)) == 1) {
        if (!n && (strcasecmp(token, "cover") == 0 || strcasecmp(token, "contain") == 0)) {
            mode = strcasecmp(token, "cover") == 0 ? BG_SIZE_COVER : BG_SIZE_CONTAIN;
            n++;
            break;
        }
        int is_pct = 0, kind;
        int l = parse_len_kind(token, st->font_px, -1, &is_pct, &kind);
        if (kind == CSS_MATH_PERCENT) { l = is_pct; pct |= 1 << n; }
        if (strcasecmp(token, "auto") != 0 && (l == LEN_AUTO || l < 0)) {
            if (shorthand && n) break; /* repeat/color after position / size */
            return;
        }
        sizes[n++] = l;
    }
    if (got < 0 || !n || (!shorthand && css_next_value(&it, token) != 0)) return;
    if (mode == BG_SIZE_PX && sizes[0] == LEN_AUTO && sizes[1] == LEN_AUTO) mode = BG_SIZE_AUTO;
    st->bg_size = (uint8_t)mode;
    st->bg_size_w = sizes[0];
    st->bg_size_h = sizes[1];
    st->bg_size_pct = (uint8_t)(mode == BG_SIZE_PX ? pct : 0);
}

/* "center", "left top", "50% 20%", "-32px 0" */
static void bg_pos_of(style_t* st, const char* v, int shorthand) {
    int vals[2] = { 50, 50 }, pct[2] = { 1, 1 }, seen = 0, n = 0, got;
    css_value_tokens it = { v, 4096 };
    char t[CSS_VALUE_TOKEN];
    while ((got = css_next_value(&it, t)) == 1) {
        if (shorthand && (strcmp(t, "/") == 0 || strcmp(t, ",") == 0)) break;
        int axis = seen & 1 ? 1 : 0, value = 0, percent = 1;
        if (strcasecmp(t, "left") == 0) { axis = 0; value = 0; }
        else if (strcasecmp(t, "right") == 0) { axis = 0; value = 100; }
        else if (strcasecmp(t, "top") == 0) { axis = 1; value = 0; }
        else if (strcasecmp(t, "bottom") == 0) { axis = 1; value = 100; }
        else if (strcasecmp(t, "center") == 0) value = 50;
        else {
            int is_pct = 0, kind;
            int l = parse_len_kind(t, st->font_px, -1, &is_pct, &kind);
            if (l == LEN_AUTO && kind != CSS_MATH_PERCENT) {
                if (shorthand) continue; /* color, url(), repeat, ... */
                return;
            }
            percent = kind == CSS_MATH_PERCENT;
            value = percent ? is_pct : l;
        }
        if (n == 2 || (seen & (1 << axis))) return;
        vals[axis] = value;
        pct[axis] = percent;
        seen |= 1 << axis;
        n++;
    }
    if (got < 0 || !n) return;
    st->bg_pos_x = vals[0];
    st->bg_pos_y = vals[1];
    st->bg_pos_pct = (uint8_t)((pct[0] ? 1 : 0) | (pct[1] ? 2 : 0));
}

/* Find a top-level position/size separator, not a slash in url(), strings,
 * or calc(). The source pointer stays in the arena for background URLs. */
static void bg_geometry(style_t* st, const char* v) {
    css_value_tokens it = { v, 4096 };
    char token[CSS_VALUE_TOKEN];
    int got;
    while ((got = css_next_value(&it, token)) == 1) {
        if (strcmp(token, ",") == 0) break; /* one background layer */
        if (strcmp(token, "/") == 0) { bg_size_of(st, it.p, 1); break; }
    }
    if (got >= 0) bg_pos_of(st, v, 1);
}

static int font_size_token(const char* token, int base) {
    static const struct { const char* name; int px; } sizes[] = {
        { "xx-small", 9 }, { "x-small", 10 }, { "small", 13 }, { "medium", 16 },
        { "large", 18 }, { "x-large", 24 }, { "xx-large", 32 }, { "xxx-large", 48 }
    };
    for (uint32_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++)
        if (strcasecmp(token, sizes[i].name) == 0) return sizes[i].px;
    if (strcasecmp(token, "smaller") == 0) return base * 5 / 6;
    if (strcasecmp(token, "larger") == 0) return base * 6 / 5;
    int kind, px = parse_len_kind(token, base, base, NULL, &kind);
    return kind == CSS_MATH_LENGTH && px >= 0 ? px : LEN_AUTO;
}

static void font_shorthand(style_t* st, const char* v, int base) {
    css_value_tokens it = { v, 4096 };
    char token[CSS_VALUE_TOKEN];
    int bold = 0, italic = 0, size = LEN_AUTO, got;
    while ((got = css_next_value(&it, token)) == 1) {
        size = font_size_token(token, base);
        if (size != LEN_AUTO) break;
        int weight;
        if (strcasecmp(token, "bold") == 0 || strcasecmp(token, "bolder") == 0) bold = 1;
        else if (strcasecmp(token, "italic") == 0 || strcasecmp(token, "oblique") == 0) italic = 1;
        else if (strcasecmp(token, "normal") == 0 || strcasecmp(token, "small-caps") == 0 ||
                 strcasecmp(token, "lighter") == 0) {}
        else if (flex_number(token, &weight) && weight >= 100 && weight <= 100000) bold = weight >= 60000;
        else return;
    }
    if (got != 1 || size == LEN_AUTO) return;
    style_t next = *st;
    set_font_px(&next, size);
    next.bold = (uint8_t)bold;
    next.italic = (uint8_t)italic;
    next.lh_px = next.lh_pct = 0;
    const char* family = it.p;
    if (css_next_value(&it, token) != 1) return;
    if (strcmp(token, "/") == 0) {
        if (css_next_value(&it, token) != 1) return;
        set_line_height(&next, token);
        family = it.p;
        if (css_next_value(&it, token) != 1) return;
    }
    /* Validate the remaining quoted family list without splitting its strings. */
    while ((got = css_next_value(&it, token)) == 1) {}
    if (got < 0) return;
    next.mono = (uint8_t)is_mono_family(family);
    *st = next;
}

static void flex_shorthand(style_t* st, const char* v) {
    if (strcasecmp(v, "none") == 0) { st->flex_grow = 0; st->flex_shrink = 0; st->flex_basis = LEN_AUTO; return; }
    if (strcasecmp(v, "auto") == 0) { st->flex_grow = 100; st->flex_shrink = 100; st->flex_basis = LEN_AUTO; return; }
    css_value_tokens it = { v, 4096 };
    char token[CSS_VALUE_TOKEN];
    int grow = 100, shrink = 100, basis = 0, numbers = 0, have_basis = 0, count = 0, got;
    while ((got = css_next_value(&it, token)) == 1) {
        if (++count > 3) return;
        int num;
        if (flex_number(token, &num) && numbers < 2) {
            if (!numbers++) grow = num;
            else shrink = num;
        } else {
            if (have_basis) return;
            if (strcasecmp(token, "auto") == 0 || strcasecmp(token, "content") == 0) basis = LEN_AUTO;
            else if (strcmp(token, "0%") == 0) basis = 0; /* Tailwind flex-1 */
            else {
                int kind;
                basis = parse_len_kind(token, st->font_px, -1, NULL, &kind);
                if (basis == LEN_AUTO || basis < 0 || (kind == CSS_MATH_NUMBER && basis)) return;
            }
            have_basis = 1;
        }
    }
    if (got < 0 || !count) return;
    st->flex_grow = grow;
    st->flex_shrink = shrink;
    st->flex_basis = basis;
}

/* flexbox properties that are not keyed by the switch below; 1 if handled */
/* top / right / bottom / left: px, or a percentage of the containing block */
static int offset_of(style_t* st, const char* v, int bit) {
    st->off_pct &= (uint8_t)~bit;
    if (has_word(v, "auto")) return LEN_AUTO;
    int is_pct = 0, kind;
    int l = parse_len_kind(v, st->font_px, -1, &is_pct, &kind);
    if (kind == CSS_MATH_PERCENT) { st->off_pct |= (uint8_t)bit; return is_pct; }
    return l;
}

/* transform: the translation in it (translate / translateX / translateY / translate3d) */
static void transform_of(style_t* st, const char* v) {
    st->tx = st->ty = 0;
    st->t_pct = 0;
    const char* p = strstr(v, "translate");
    if (!p) return;
    p += 9;
    int which = 3;                                   /* x and y */
    if (*p == 'X' || *p == 'x') { which = 1; p++; }
    else if (*p == 'Y' || *p == 'y') { which = 2; p++; }
    else if (!strncmp(p, "3d", 2) || !strncmp(p, "3D", 2)) p += 2;
    if (*p != '(') return;
    p++;
    for (int axis = 0; axis < 2; axis++) {
        if (!(which & (1 << axis)) && which != 3) continue;
        while (is_ws(*p)) p++;
        char tok[64];
        int n = 0, d = 0;
        while (*p && n < 63 && !(d == 0 && (*p == ',' || *p == ')'))) { if (*p == '(') d++; else if (*p == ')') d--; tok[n++] = *p++; }
        while (n && is_ws(tok[n - 1])) n--;
        tok[n] = 0;
        int is_pct = 0, kind;
        int l = parse_len_kind(tok, st->font_px, -1, &is_pct, &kind);
        int ax = which == 3 ? axis : which - 1;
        if (kind == CSS_MATH_PERCENT) { l = is_pct; st->t_pct |= (uint8_t)(1 << ax); }
        else if (l == LEN_AUTO) l = 0;
        if (ax) st->ty = l; else st->tx = l;
        if (*p == ',') p++;
        else break;
        if (which != 3) break;
    }
}

static int flex_decl(style_t* st, const char* prop, const char* v) {
    if (strcmp(prop, "gap") == 0 || strcmp(prop, "grid-gap") == 0) {
        css_value_tokens it = { v, 4096 };
        char token[CSS_VALUE_TOKEN];
        if (css_next_value(&it, token) != 1) return 1;
        int a = parse_len(token, st->font_px, -1, NULL);
        int got = css_next_value(&it, token), b = a;
        if (got == 1) b = parse_len(token, st->font_px, -1, NULL);
        if (got < 0 || css_next_value(&it, token) != 0 || a == LEN_AUTO || b == LEN_AUTO) return 1;
        st->gap_row = a == LEN_AUTO || a < 0 ? 0 : a;
        st->gap_col = b == LEN_AUTO || b < 0 ? 0 : b;
        return 1;
    }
    if (strcmp(prop, "row-gap") == 0 || strcmp(prop, "column-gap") == 0) {
        int a = parse_len(v, st->font_px, -1, NULL);
        if (a == LEN_AUTO || a < 0) a = 0;
        if (prop[0] == 'r') st->gap_row = a; else st->gap_col = a;
        return 1;
    }
    if (strcmp(prop, "justify-content") == 0) {
        st->justify = has_word(v, "space-between") ? JUSTIFY_BETWEEN : has_word(v, "space-around") ? JUSTIFY_AROUND :
                      has_word(v, "space-evenly") ? JUSTIFY_EVENLY : has_word(v, "center") ? JUSTIFY_CENTER :
                      (has_word(v, "flex-end") || has_word(v, "end") || has_word(v, "right")) ? JUSTIFY_END : JUSTIFY_START;
        return 1;
    }
    if (strcmp(prop, "align-items") == 0) { int a = flex_align(v); st->align_items = a ? a : FA_STRETCH; return 1; }
    if (strcmp(prop, "align-self") == 0) { st->align_self = flex_align(v); return 1; }
    if (strcmp(prop, "place-items") == 0) { int a = flex_align(v); st->align_items = a ? a : FA_STRETCH; return 1; }
    if (strcmp(prop, "background-image") == 0) {
        st->bg_url = NULL;
        st->bg_grad = 0;
        st->grad_src = NULL;
        if (!has_word(v, "none")) bg_layers(st, v);
        return 1;
    }
    if (strcmp(prop, "background-repeat") == 0) { bg_repeat_of(st, v); return 1; }
    if (strcmp(prop, "object-fit") == 0) {
        st->object_fit = (uint8_t)(has_word(v, "contain") ? 1 : has_word(v, "cover") ? 2 : has_word(v, "none") ? 3 :
                                   has_word(v, "scale-down") ? 4 : 0);
        return 1;
    }
    if (strcmp(prop, "background-clip") == 0 || strcmp(prop, "-webkit-background-clip") == 0) {
        st->bg_clip_text = (uint8_t)has_word(v, "text");
        return 1;
    }
    if (strcmp(prop, "-webkit-text-fill-color") == 0) {
        int ok;
        uint32_t c = css_color(v, &ok);
        if (ok == 1) { st->color = c; st->visible = 1; st->text_clear = 0; }
        else if (ok == 2) { st->visible = 0; st->text_clear = 1; }
        return 1;
    }
    if (strcmp(prop, "background-size") == 0) { bg_size_of(st, v, 0); return 1; }
    if (strcmp(prop, "background-position") == 0) { bg_pos_of(st, v, 0); return 1; }
    if (strcmp(prop, "border-radius") == 0) {
        int is_pct = 0;
        int r = parse_len(v, st->font_px, 100, &is_pct);
        st->radius = r == LEN_AUTO || r < 0 ? 0 : is_pct ? (r >= 50 ? 100000 : r * 4) : r;
        return 1;
    }
    if (strcmp(prop, "top") == 0) { st->top = offset_of(st, v, 2); return 1; }
    if (strcmp(prop, "right") == 0) { st->right = offset_of(st, v, 4); return 1; }
    if (strcmp(prop, "bottom") == 0) { st->bottom = offset_of(st, v, 8); return 1; }
    if (strcmp(prop, "transform") == 0 || strcmp(prop, "-webkit-transform") == 0) { transform_of(st, v); return 1; }
    return 0;
}

#include "css_grid.h"
#include "css_anim.h"

static void apply_decl(style_t* st, const style_t* parent, const char* prop, const char* v) {
    int ok;
    if (strcasecmp(v, "inherit") == 0) {
        if (!parent) return;
        if (strcmp(prop, "color") == 0) st->color = parent->color;
        else if (strcmp(prop, "background-color") == 0 || strcmp(prop, "background") == 0) { st->bg = parent->bg; st->has_bg = parent->has_bg; }
        else if (strcmp(prop, "font-size") == 0) set_font_px(st, parent->font_px);
        else if (strcmp(prop, "font-weight") == 0) st->bold = parent->bold;
        else if (strcmp(prop, "text-align") == 0) st->align = parent->align;
        else if (strcmp(prop, "display") == 0) st->display = parent->display;
        else if (strcmp(prop, "visibility") == 0) st->visible = parent->visible;
        else if (strcmp(prop, "width") == 0) { st->width = parent->width; st->width_pct = parent->width_pct; }
        else if (strcmp(prop, "height") == 0) st->height = parent->height;
        return;
    }
    if (strcasecmp(v, "initial") == 0 || strcasecmp(v, "unset") == 0 || strcasecmp(v, "revert") == 0) {
        if (strcmp(prop, "color") == 0) st->color = parent && strcasecmp(v, "initial") != 0 ? parent->color : 0;
        else if (strcmp(prop, "background-color") == 0 || strcmp(prop, "background") == 0) st->has_bg = 0;
        else if (strcmp(prop, "display") == 0) st->display = DISP_INLINE;
        else if (strcmp(prop, "border") == 0) for (int i = 0; i < 4; i++) st->border[i] = 0;
        else if (strcmp(prop, "width") == 0 || strcmp(prop, "inline-size") == 0) { st->width = LEN_AUTO; st->width_pct = 0; }
        else if (strcmp(prop, "height") == 0 || strcmp(prop, "block-size") == 0) st->height = LEN_AUTO;
        else if (strcmp(prop, "max-width") == 0) st->max_width = LEN_AUTO;
        else if (strcmp(prop, "padding") == 0 || strcmp(prop, "margin") == 0) {
            int* sides = prop[0] == 'p' ? st->padding : st->margin;
            for (int i = 0; i < 4; i++) sides[i] = 0;
            if (prop[0] == 'm') st->margin_auto_lr = 0;
        }
        else if (strcmp(prop, "font-weight") == 0) st->bold = parent && strcasecmp(v, "initial") ? parent->bold : 0;
        else if (strcmp(prop, "font-size") == 0) set_font_px(st, parent && strcasecmp(v, "initial") ? parent->font_px : 16);
        else if (strcmp(prop, "visibility") == 0) st->visible = parent && strcasecmp(v, "initial") ? parent->visible : 1;
        return;
    }
    if (!strcmp(prop, "grid-template-columns")) { grid_columns(st, v); return; }
    if (flex_decl(st, prop, v)) return;
    if (prop[0] == 'a' || prop[0] == '-') { if (anim_decl(st, prop, v)) return; }
    switch (prop[0]) {
    case 'b':
        if (strcmp(prop, "background-color") == 0 || strcmp(prop, "background") == 0) {
            if (prop[10] == 0) {                     /* background: ... */
                st->bg_url = NULL;
                st->bg_grad = 0;
                st->grad_src = NULL;
                st->bg_repeat = BG_REPEAT;
                bg_layers(st, v);
                bg_repeat_of(st, v);
                bg_geometry(st, v);
                if (st->bg_grad && !strchr(v, '#') && !strstr(v, "rgb")) return;   /* the colour came from the gradient */
            }
            if (has_word(v, "none") && !strchr(v, '#') && !strstr(v, "rgb")) { st->has_bg = 0; return; }
            /* the first color-looking token of the shorthand */
            const char* p = v;
            while (*p) {
                while (is_ws(*p) || *p == ',') p++;
                uint32_t c = css_color_blend(p, &ok);
                if (ok == 1) { st->bg = c; st->has_bg = 1; return; }
                if (ok == 2) { st->has_bg = 0; return; }
                while (*p && !is_ws(*p) && *p != ',') {
                    if (*p == '(') { int d = 0; while (*p) { if (*p == '(') d++; else if (*p == ')' && --d == 0) break; p++; } }
                    if (*p) p++;
                }
            }
            return;
        }
        if (strcmp(prop, "border") == 0) { border_side(st, v, -1); return; }
        if (strcmp(prop, "border-top") == 0) { border_side(st, v, 0); return; }
        if (strcmp(prop, "border-right") == 0) { border_side(st, v, 1); return; }
        if (strcmp(prop, "border-bottom") == 0) { border_side(st, v, 2); return; }
        if (strcmp(prop, "border-left") == 0) { border_side(st, v, 3); return; }
        if (strcmp(prop, "border-width") == 0) { four(v, st->font_px, st->border, NULL); for (int i = 0; i < 4; i++) if (st->border[i] > 20) st->border[i] = 20; return; }
        if (strcmp(prop, "border-color") == 0) {
            uint32_t c = css_color_blend(v, &ok);
            if (ok == 1) for (int i = 0; i < 4; i++) st->border_color[i] = c;
            else if (ok == 2) for (int i = 0; i < 4; i++) st->border[i] = 0;
            return;
        }
        if (strcmp(prop, "border-style") == 0) {
            if (has_word(v, "none") || has_word(v, "hidden")) for (int i = 0; i < 4; i++) st->border[i] = 0;
            else for (int i = 0; i < 4; i++) if (!st->border[i]) st->border[i] = 2;
            return;
        }
        if (strncmp(prop, "border-", 7) == 0 && strstr(prop, "-width")) {
            int side = strncmp(prop + 7, "top", 3) == 0 ? 0 : strncmp(prop + 7, "right", 5) == 0 ? 1 :
                       strncmp(prop + 7, "bottom", 6) == 0 ? 2 : strncmp(prop + 7, "left", 4) == 0 ? 3 : -1;
            int l = parse_len(v, st->font_px, -1, NULL);
            if (side >= 0 && l != LEN_AUTO) st->border[side] = l < 0 ? 0 : l > 20 ? 20 : l;
            return;
        }
        return;
    case 'c':
        if (strcmp(prop, "color") == 0) {
            uint32_t c = css_color(v, &ok);
            if (ok == 1) { st->color = c; st->text_clear = 0; }
            else if (ok == 2) { st->visible = 0; st->text_clear = 1; }   /* invisible text */
            return;
        }
        if (strcmp(prop, "clip") == 0) { if (strstr(v, "rect(")) st->clipped = 1; return; }
        if (strcmp(prop, "clip-path") == 0) { if (strstr(v, "inset(50%") || strstr(v, "circle(0")) st->clipped = 1; return; }
        return;
    case 'd':
        if (strcmp(prop, "display") == 0) {
            st->flex_row = 0;
            st->flex = 0;
            st->grid = 0;
            if (has_word(v, "none")) st->display = DISP_NONE;
            else if (has_word(v, "contents")) st->display = DISP_INLINE;
            else if (has_word(v, "inline-block") || has_word(v, "inline-flex") || has_word(v, "inline-grid") ||
                     has_word(v, "inline-table")) {
                st->display = DISP_INLINE_BLOCK;
                if (has_word(v, "inline-flex")) st->flex = 1;
                else if (has_word(v, "inline-grid")) st->grid = 1;
            }
            else if (has_word(v, "inline")) st->display = DISP_INLINE;
            else if (has_word(v, "list-item")) st->display = DISP_LIST_ITEM;
            else if (has_word(v, "table-row-group") || has_word(v, "table-header-group") || has_word(v, "table-footer-group"))
                st->display = DISP_TABLE_GROUP;
            else if (has_word(v, "table-row")) st->display = DISP_TABLE_ROW;
            else if (has_word(v, "table-cell")) st->display = DISP_TABLE_CELL;
            else if (has_word(v, "table")) st->display = DISP_TABLE;
            else {
                st->display = DISP_BLOCK;                /* block, flow-root, flex, grid, ... */
                if (has_word(v, "flex")) st->flex = 1;
                else if (has_word(v, "grid")) st->grid = 1;
            }
        }
        return;
    case 'f':
        if (strcmp(prop, "font-weight") == 0) {
            st->bold = has_word(v, "bold") || has_word(v, "bolder") ||
                       (v[0] >= '6' && v[0] <= '9' && v[1] == '0' && v[2] == '0');
            return;
        }
        if (strcmp(prop, "font-style") == 0) { st->italic = has_word(v, "italic") || has_word(v, "oblique"); return; }
        if (strcmp(prop, "font-family") == 0) { st->mono = is_mono_family(v); return; }
        if (strcmp(prop, "font-size") == 0 || strcmp(prop, "font") == 0) {
            int base = parent ? parent->font_px : 16;
            if (strcmp(prop, "font") == 0) {
                font_shorthand(st, v, base);
                return;
            }
            static const struct { const char* k; int px; } sizes[] = {
                { "xx-small", 9 }, { "x-small", 10 }, { "small", 13 }, { "medium", 16 }, { "large", 18 },
                { "x-large", 24 }, { "xx-large", 32 }, { "xxx-large", 48 },
            };
            for (uint32_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++)
                if (strcasecmp(v, sizes[i].k) == 0) { set_font_px(st, sizes[i].px); return; }
            if (strcasecmp(v, "smaller") == 0) { set_font_px(st, base * 5 / 6); return; }
            if (strcasecmp(v, "larger") == 0) { set_font_px(st, base * 6 / 5); return; }
            int px = parse_len(v, base, base, NULL);
            if (px != LEN_AUTO) set_font_px(st, px);
            return;
        }
        if (strcmp(prop, "float") == 0) { st->floated = has_word(v, "left") || has_word(v, "right") || has_word(v, "inline-start") || has_word(v, "inline-end"); return; }
        if (strcmp(prop, "flex-direction") == 0 || strcmp(prop, "flex-flow") == 0) {
            if (has_word(v, "column-reverse")) st->flex_dir = FLEX_COL_REV;
            else if (has_word(v, "column")) st->flex_dir = FLEX_COL;
            else if (has_word(v, "row-reverse")) st->flex_dir = FLEX_ROW_REV;
            else if (has_word(v, "row")) st->flex_dir = FLEX_ROW;
            if (has_word(v, "column") || has_word(v, "column-reverse")) st->flex_row = 0;
            if (strcmp(prop, "flex-flow") == 0) st->flex_wrap = has_word(v, "wrap") || has_word(v, "wrap-reverse");
            return;
        }
        if (strcmp(prop, "flex-wrap") == 0) { st->flex_wrap = has_word(v, "wrap") || has_word(v, "wrap-reverse"); return; }
        if (strcmp(prop, "flex-grow") == 0) { st->flex_grow = flex_num(v); return; }
        if (strcmp(prop, "flex-shrink") == 0) { st->flex_shrink = flex_num(v); return; }
        if (strcmp(prop, "flex-basis") == 0) {
            st->flex_basis = has_word(v, "auto") || has_word(v, "content") ? LEN_AUTO : parse_len(v, st->font_px, -1, NULL);
            return;
        }
        if (strcmp(prop, "flex") == 0) {
            flex_shorthand(st, v);
            return;
        }
        return;
    case 'h':
        if (strcmp(prop, "height") == 0) {
            int is_pct = 0, kind;
            int l = parse_len_kind(v, st->font_px, -1, &is_pct, &kind);
            if (kind == CSS_MATH_PERCENT) {
                /* against the parent's definite height (the viewport for the root):
                 * html, body { height: 100% } and what is inside them */
                int base = !parent ? css_viewport_h : parent->height != LEN_AUTO && parent->height > 0 ? parent->height : -1;
                l = base > 0 && is_pct >= 0 ? (int)((int64_t)base * is_pct / 100) : LEN_AUTO;
            }
            st->height = l;
        }
        return;
    case 'i':
        if (strcmp(prop, "inset") == 0) {
            int sides[4] = { 0, 0, 0, 0 };
            four(v, st->font_px, sides, NULL);
            st->off_pct = 0;
            st->top = sides[0];
            st->right = sides[1];
            st->bottom = sides[2];
            st->left = sides[3];
        }
        return;
    case 'l':
        if (strcmp(prop, "line-height") == 0) { set_line_height(st, v); return; }
        if (strcmp(prop, "list-style-type") == 0 || strcmp(prop, "list-style") == 0) {
            if (has_word(v, "none")) st->list_style = LIST_NONE;
            else if (has_word(v, "decimal")) st->list_style = LIST_DECIMAL;
            else if (has_word(v, "circle")) st->list_style = LIST_CIRCLE;
            else if (has_word(v, "square")) st->list_style = LIST_SQUARE;
            else if (has_word(v, "disc")) st->list_style = LIST_DISC;
            return;
        }
        if (strcmp(prop, "left") == 0) { st->left = offset_of(st, v, 1); return; }
        return;
    case 'm':
        if (strcmp(prop, "margin") == 0) { four(v, st->font_px, st->margin, &st->margin_auto_lr); return; }
        if (strncmp(prop, "margin-", 7) == 0) {
            int l = parse_len(v, st->font_px, -1, NULL);
            int side = strcmp(prop + 7, "top") == 0 || strcmp(prop + 7, "block-start") == 0 ? 0 :
                       strcmp(prop + 7, "right") == 0 || strcmp(prop + 7, "inline-end") == 0 ? 1 :
                       strcmp(prop + 7, "bottom") == 0 || strcmp(prop + 7, "block-end") == 0 ? 2 :
                       strcmp(prop + 7, "left") == 0 || strcmp(prop + 7, "inline-start") == 0 ? 3 : -1;
            if (side >= 0) st->margin[side] = l == LEN_AUTO ? 0 : l < -2000 ? -2000 : l > 2000 ? 2000 : l;
            return;
        }
        if (strcmp(prop, "max-width") == 0) {
            int pct;
            int l = parse_len(v, st->font_px, -1, &pct);
            st->max_width = l;
            if (l == LEN_AUTO && pct && pct < 100) st->max_width = css_viewport_w * pct / 100;
        }
        return;
    case 'o':
        if (strcmp(prop, "opacity") == 0) {
            const char* p = v;
            int m = num_milli(&p);
            if (*p == '%') m /= 100;
            if (p != v && m < 50) st->visible = 0;
            return;
        }
        if (strcmp(prop, "overflow") == 0 || strcmp(prop, "overflow-y") == 0) { st->overflow_hidden = has_word(v, "hidden") || has_word(v, "clip"); return; }
        return;
    case 'p':
        if (strcmp(prop, "padding") == 0) { four(v, st->font_px, st->padding, NULL); for (int i = 0; i < 4; i++) if (st->padding[i] < 0) st->padding[i] = 0; return; }
        if (strncmp(prop, "padding-", 8) == 0) {
            int l = parse_len(v, st->font_px, -1, NULL);
            int side = strcmp(prop + 8, "top") == 0 || strcmp(prop + 8, "block-start") == 0 ? 0 :
                       strcmp(prop + 8, "right") == 0 || strcmp(prop + 8, "inline-end") == 0 ? 1 :
                       strcmp(prop + 8, "bottom") == 0 || strcmp(prop + 8, "block-end") == 0 ? 2 :
                       strcmp(prop + 8, "left") == 0 || strcmp(prop + 8, "inline-start") == 0 ? 3 : -1;
            if (side >= 0) st->padding[side] = l == LEN_AUTO || l < 0 ? 0 : l > 2000 ? 2000 : l;
            return;
        }
        if (strcmp(prop, "position") == 0) {
            st->position = has_word(v, "absolute") ? POS_ABSOLUTE : has_word(v, "fixed") ? POS_FIXED :
                           has_word(v, "relative") ? POS_RELATIVE : has_word(v, "sticky") ? POS_STICKY : POS_STATIC;
        }
        return;
    case 't':
        if (strcmp(prop, "text-align") == 0) {
            st->align = has_word(v, "center") ? ALIGN_CENTER : (has_word(v, "right") || has_word(v, "end")) ? ALIGN_RIGHT : ALIGN_LEFT;
            return;
        }
        if (strcmp(prop, "text-decoration") == 0 || strcmp(prop, "text-decoration-line") == 0) {
            st->underline = has_word(v, "underline");
            st->strike = has_word(v, "line-through");
            return;
        }
        if (strcmp(prop, "text-transform") == 0) {
            st->uppercase = has_word(v, "uppercase");
            st->lowercase = has_word(v, "lowercase");
            return;
        }
        if (strcmp(prop, "top") == 0) { st->top = parse_len(v, st->font_px, -1, NULL); return; }
        if (strcmp(prop, "text-indent") == 0) {
            int l = parse_len(v, st->font_px, -1, NULL);
            if (l != LEN_AUTO && l < -999) st->clipped = 1;    /* image-replacement text pushed off screen */
            return;
        }
        return;
    case 'v':
        if (strcmp(prop, "visibility") == 0) st->visible = !(has_word(v, "hidden") || has_word(v, "collapse"));
        return;
    case 'w':
        if (strcmp(prop, "width") == 0 || strcmp(prop, "inline-size") == 0) {
            int pct;
            int l = parse_len(v, st->font_px, -1, &pct);
            st->width = l;
            st->width_pct = pct > 100 ? 100 : pct;
            return;
        }
        if (strcmp(prop, "white-space") == 0) {
            st->pre = has_word(v, "pre") || has_word(v, "pre-wrap") || has_word(v, "pre-line") || has_word(v, "break-spaces");
            st->nowrap = has_word(v, "nowrap");
        }
        return;
    }
}

/* ══ cascade ══════════════════════════════════════════════════════════ */

typedef struct { const decl_t* d; uint32_t spec; uint32_t order; } match_t;

static void inherit(style_t* st, const style_t* p) {
    memset(st, 0, sizeof(*st));
    st->display = DISP_INLINE;
    st->width = st->height = st->max_width = LEN_AUTO;
    st->left = st->top = LEN_AUTO;
    st->right = st->bottom = LEN_AUTO;
    st->bg_size_w = st->bg_size_h = LEN_AUTO;
    st->flex_basis = LEN_AUTO;
    st->flex_shrink = 100;
    st->visible = 1;
    st->color = 0x000000;
    set_font_px(st, 16);
    if (!p) return;
    st->color = p->color;
    st->bold = p->bold;
    st->italic = p->italic;
    st->underline = p->underline;
    st->strike = p->strike;
    st->uppercase = p->uppercase;
    st->lowercase = p->lowercase;
    st->align = p->align;
    st->pre = p->pre;
    st->nowrap = p->nowrap;
    st->list_style = p->list_style;
    st->visible = p->visible;
    st->text_clear = p->text_clear;
    st->font_px = p->font_px;
    st->scale = p->scale;
    st->mono = p->mono;
    st->lh_px = p->lh_px;
    st->lh_pct = p->lh_pct;
    st->vars = p->vars;
}

/* old HTML attributes: bgcolor, color, align, width, border, ... */
static void presentational(arena_t* A, dom_node_t* e, style_t* st) {
    (void)A;
    const char* v;
    int ok;
    if ((v = dom_attr(e, "bgcolor"))) { uint32_t c = css_color(v, &ok); if (ok == 1) { st->bg = c; st->has_bg = 1; } }
    if (strcmp(e->tag, "font") == 0) {
        if ((v = dom_attr(e, "color"))) { uint32_t c = css_color(v, &ok); if (ok == 1) st->color = c; }
        if ((v = dom_attr(e, "size"))) {
            int sz = v[0] - '0';
            if (v[0] == '+') sz = 3 + (v[1] - '0');
            static const int px[] = { 10, 10, 13, 16, 18, 24, 32, 48 };
            if (sz >= 1 && sz <= 7) set_font_px(st, px[sz]);
        }
    }
    if ((v = dom_attr(e, "align"))) {
        if (strcasecmp(v, "center") == 0 || strcasecmp(v, "middle") == 0) {
            if (strcmp(e->tag, "table") == 0 || strcmp(e->tag, "img") == 0) st->margin_auto_lr = 1;
            else st->align = ALIGN_CENTER;
        } else if (strcasecmp(v, "right") == 0) st->align = ALIGN_RIGHT;
    }
    if ((v = dom_attr(e, "width")) && strcmp(e->tag, "img") != 0) {
        int pct;
        int l = parse_len(v, 16, -1, &pct);
        st->width = l;
        st->width_pct = pct > 100 ? 100 : pct;
    }
    if (strcmp(e->tag, "table") == 0 && (v = dom_attr(e, "border")) && v[0] != '0') {
        for (int i = 0; i < 4; i++) { st->border[i] = 1; st->border_color[i] = 0x808080; }
    }
    if ((strcmp(e->tag, "td") == 0 || strcmp(e->tag, "th") == 0)) {
        /* <table border> draws cell borders too */
        dom_node_t* t = e->parent;
        while (t && t->type == DOM_ELEM && strcmp(t->tag, "table") != 0) t = t->parent;
        if (t && t->type == DOM_ELEM && (v = dom_attr(t, "border")) && v[0] != '0')
            for (int i = 0; i < 4; i++) { st->border[i] = 1; st->border_color[i] = 0xA0A0A0; }
        if (t && t->type == DOM_ELEM && (v = dom_attr(t, "cellpadding"))) {
            int p = parse_len(v, 16, -1, NULL);
            if (p != LEN_AUTO) for (int i = 0; i < 4; i++) st->padding[i] = p;
        }
    }
    if (strcmp(e->tag, "body") == 0) {
        if ((v = dom_attr(e, "text"))) { uint32_t c = css_color(v, &ok); if (ok == 1) st->color = c; }
    }
    if (strcmp(e->tag, "hr") == 0 && (v = dom_attr(e, "color"))) {
        uint32_t c = css_color(v, &ok);
        if (ok == 1) st->border_color[0] = c;
    }
}

/* stable sort by (spec, order) - merge sort, matches can be many */
static void sort_matches(match_t* m, match_t* tmp, int n) {
    for (int w = 1; w < n; w *= 2) {
        for (int lo = 0; lo < n; lo += 2 * w) {
            int mid = lo + w < n ? lo + w : n, hi = lo + 2 * w < n ? lo + 2 * w : n;
            int a = lo, b = mid, k = lo;
            while (a < mid && b < hi) {
                if (m[b].spec < m[a].spec || (m[b].spec == m[a].spec && m[b].order < m[a].order)) tmp[k++] = m[b++];
                else tmp[k++] = m[a++];
            }
            while (a < mid) tmp[k++] = m[a++];
            while (b < hi) tmp[k++] = m[b++];
        }
        memcpy(m, tmp, (size_t)n * sizeof(match_t));
    }
}

typedef struct {
    arena_t* A;
    match_t* buf;
    match_t* tmp;
    int      cap;
    int*     seen;              /* per rule: the element serial it last matched (dedupe) */
    int      seen_cap;
    int      serial;
    int      font_pass, computed_font;
} cascade_t;

/* Process one bucket at a time, sharing the sheet's deduplication range. */
static int collect_bucket(cascade_t* K, css_sheet_t* sh, dom_node_t* e,
                          const bucket_t* bk, int base, int nm) {
    if (!bk) return nm;
    for (int k = 0; k < bk->n; k++) {
        int ref = bk->refs[k];
        int r = ref >> 8, si = ref & 255;
        rule_t* rule = &sh->rules[r];
        if (rule->media_min && css_viewport_w < rule->media_min) continue;
        if (rule->media_max && css_viewport_w > rule->media_max) continue;
        if (!sel_matches(&rule->sels[si], e)) continue;
        uint32_t spec = rule->sels[si].spec;
        if (K->seen[r] == K->serial) {
            /* the rule already matched through another selector: keep the highest specificity */
            for (int i = base; i < nm; i++)
                if (K->buf[i].order >> 12 == (uint32_t)r && (K->buf[i].spec & 0xFFFFFF) < spec)
                    K->buf[i].spec = spec | (K->buf[i].spec & 0x1000000u);
            continue;
        }
        K->seen[r] = K->serial;
        for (int d = 0; d < rule->ndecls; d++) {
            /* Many utility classes can also exceed the initial match
             * capacity. Grow both sort buffers in the arena, not on stack. */
            if (nm == K->cap) {
                int nc = K->cap * 2;
                match_t* nb = (match_t*)arena_alloc(K->A, (uint32_t)nc * (uint32_t)sizeof(match_t));
                memcpy(nb, K->buf, (size_t)nm * sizeof(match_t));
                K->buf = nb;
                K->tmp = (match_t*)arena_alloc(K->A, (uint32_t)nc * (uint32_t)sizeof(match_t));
                K->cap = nc;
            }
            K->buf[nm].d = &rule->decls[d];
            K->buf[nm].spec = spec | (rule->decls[d].important ? 0x1000000u : 0);
            K->buf[nm].order = (uint32_t)r << 12 | (uint32_t)(d & 0xFFF);
            nm++;
        }
    }
    return nm;
}

/* Every rule of one sheet that matches e, without a class-count limit. */
static int collect(cascade_t* K, css_sheet_t* sh, dom_node_t* e, int nm, uint32_t* order) {
    if (sh->n > K->seen_cap) return nm;
    int base = nm;
    const char* id = dom_attr(e, "id");
    if (id) nm = collect_bucket(K, sh, e, bucket_get(NULL, sh->by_id, id, 0), base, nm);
    const char* cls = dom_attr(e, "class");
    if (cls) {
        char word[96];
        for (const char* p = cls; *p; ) {
            while (*p == ' ' || *p == '\t' || *p == '\n') p++;
            size_t l = 0;
            while (p[l] && p[l] != ' ' && p[l] != '\t' && p[l] != '\n') l++;
            if (l && l < sizeof(word)) {
                memcpy(word, p, l);
                word[l] = 0;
                nm = collect_bucket(K, sh, e, bucket_get(NULL, sh->by_class, word, 0), base, nm);
            }
            p += l;
        }
    }
    nm = collect_bucket(K, sh, e, bucket_get(NULL, sh->by_tag, e->tag, 0), base, nm);
    nm = collect_bucket(K, sh, e, &sh->any, base, nm);
    (void)order;
    return nm;
}

static void apply_list(cascade_t* K, style_t* st, const style_t* parent, match_t* m, int from, int to) {
    for (int i = from; i < to; i++) {
        const char* prop = m[i].d->prop;
        if (prop[0] == '-' && prop[1] == '-') continue;
        if (K->font_pass && strcmp(prop, "font") && strcmp(prop, "font-size")) continue;
        const char* v = m[i].d->val;
        if (var_next(v)) {
            v = subst_vars(K->A, st, v, 0);
            if (v && !*v) v = NULL;
        }
        apply_decl(st, parent, prop, v ? v : "unset");
        if (K->computed_font) set_font_px(st, K->computed_font);
    }
}

/* custom properties first (they are inherited and used by everything else) */
static void apply_vars(cascade_t* K, style_t* st, const style_t* parent, match_t* m, int n, decl_t* inl, int ninl) {
    int important = 0;
    while (important < n && !m[important].d->important) important++;
    for (int i = 0; i < important; i++) var_store(K->A, st, parent, m[i].d);
    for (int i = 0; i < ninl; i++) if (!inl[i].important) var_store(K->A, st, parent, &inl[i]);
    for (int i = important; i < n; i++) var_store(K->A, st, parent, m[i].d);
    for (int i = 0; i < ninl; i++) if (inl[i].important) var_store(K->A, st, parent, &inl[i]);
    css_var_t* path[CSS_VAR_DEPTH];
    for (css_var_t* v = st->vars; v && (!parent || v != parent->vars); v = v->next)
        var_resolve(K->A, st, v, path, 0);
}

static void style_element(cascade_t* K, dom_node_t* e, const style_t* parent, css_sheet_t** sheets, int nsheets) {
    arena_t* A = K->A;
    K->font_pass = K->computed_font = 0;
    style_t* st = (style_t*)arena_alloc(A, sizeof(style_t));   /* A may be a fresh arena each restyle */
    e->style = st;
    inherit(st, parent);
    for (int i = 0; i < 4; i++) st->border_color[i] = st->color;
    K->serial++;
    uint32_t order = 0;
    /* 1. default sheet, 2. presentational attributes, 3. page sheets, 4. style="" */
    int nm = 0;
    for (int s = 0; s < nsheets; s++)
        if (sheets[s] && sheets[s]->origin == 0) nm = collect(K, sheets[s], e, nm, &order);
    sort_matches(K->buf, K->tmp, nm);
    apply_list(K, st, parent, K->buf, 0, nm);
    for (int i = 0; i < 4; i++) if (!st->border[i]) st->border_color[i] = st->color;
    presentational(A, e, st);

    nm = 0;
    for (int s = 0; s < nsheets; s++) {
        if (!sheets[s] || sheets[s]->origin == 0) continue;
        K->serial++;
        int before = nm;
        nm = collect(K, sheets[s], e, nm, &order);
        /* later sheets win ties: their order numbers come after */
        for (int i = before; i < nm; i++) K->buf[i].order += (uint32_t)s << 28;
    }
    sort_matches(K->buf, K->tmp, nm);
    const char* inl_src = dom_attr(e, "style");
    decl_t* inl = NULL;
    int ninl = 0;
    if (inl_src) ninl = parse_decls(A, inl_src, (uint32_t)strlen(inl_src), &inl);
    apply_vars(K, st, parent, K->buf, nm, inl, ninl);
    /* Compute the winning font size before em/ch lengths, independently of
     * declaration order. Repeat the same cascade, including inline/important. */
    for (int pass = 0; pass < 2; pass++) {
    K->font_pass = pass == 0;
    K->computed_font = pass == 1 ? st->font_px : 0;
    /* page rules, then style="" (beats everything but !important) */
    int i = 0;
    while (i < nm && !(K->buf[i].spec & 0x1000000u)) i++;
    apply_list(K, st, parent, K->buf, 0, i);
    for (int k = 0; k < ninl; k++) {
        if (inl[k].important) continue;
        if (inl[k].prop[0] == '-' && inl[k].prop[1] == '-') continue;
        if (K->font_pass && strcmp(inl[k].prop, "font") && strcmp(inl[k].prop, "font-size")) continue;
        const char* v = var_next(inl[k].val) ? subst_vars(A, st, inl[k].val, 0) : inl[k].val;
        if (v && !*v && var_next(inl[k].val)) v = NULL;
        apply_decl(st, parent, inl[k].prop, v ? v : "unset");
        if (K->computed_font) set_font_px(st, K->computed_font);
    }
    apply_list(K, st, parent, K->buf, i, nm);
    for (int k = 0; k < ninl; k++) {
        if (!inl[k].important || (inl[k].prop[0] == '-' && inl[k].prop[1] == '-')) continue;
        if (K->font_pass && strcmp(inl[k].prop, "font") && strcmp(inl[k].prop, "font-size")) continue;
        const char* v = var_next(inl[k].val) ? subst_vars(A, st, inl[k].val, 0) : inl[k].val;
        if (v && !*v && var_next(inl[k].val)) v = NULL;
        apply_decl(st, parent, inl[k].prop, v ? v : "unset");
        if (K->computed_font) set_font_px(st, K->computed_font);
    }

    }
    K->font_pass = K->computed_font = 0;
    /* gradient text: background-clip: text under a transparent colour - the
     * text is drawn, in the gradient's colours (layout.c) */
    anim_apply(K->A, e, st, parent, sheets, nsheets);
    if (st->bg_clip_text && st->bg_grad && st->text_clear) st->visible = 1;

    /* Grid owns placement. Blockify inline items so inline text bounds cannot
     * overwrite the grid's border-box geometry after layout. */
    if (parent && parent->grid && (st->display == DISP_INLINE || st->display == DISP_INLINE_BLOCK))
        st->display = DISP_BLOCK;
    if (parent && parent->flex_row && (st->display == DISP_BLOCK || st->display == DISP_INLINE ||
                                       st->display == DISP_LIST_ITEM || st->display == DISP_TABLE))
        st->display = DISP_INLINE_BLOCK;
    if (st->floated && st->display == DISP_BLOCK) st->display = DISP_INLINE_BLOCK;
}

static void style_rec(cascade_t* K, dom_node_t* n, const style_t* parent, css_sheet_t** sheets, int nsheets) {
    WEB_TICK();
    for (dom_node_t* c = n->first; c; c = c->next) {
        if (c->type != DOM_ELEM) continue;
        style_element(K, c, parent, sheets, nsheets);
        /* nothing below display:none is ever shown: no need to style it */
        if (c->style->display == DISP_NONE) { for (dom_node_t* d = c->first; d; d = d->next) if (d->type == DOM_ELEM) d->style = NULL; continue; }
        style_rec(K, c, c->style, sheets, nsheets);
    }
}

void css_style_tree(arena_t* A, dom_node_t* doc, css_sheet_t** sheets, int nsheets) {
    cascade_t K;
    K.A = A;
    K.cap = 2048;
    K.buf = (match_t*)arena_alloc(A, (uint32_t)K.cap * (uint32_t)sizeof(match_t));
    K.tmp = (match_t*)arena_alloc(A, (uint32_t)K.cap * (uint32_t)sizeof(match_t));
    K.seen_cap = 0;
    for (int s = 0; s < nsheets; s++) if (sheets[s] && sheets[s]->n > K.seen_cap) K.seen_cap = sheets[s]->n;
    K.seen = (int*)arena_alloc(A, (uint32_t)(K.seen_cap + 1) * (uint32_t)sizeof(int));
    K.serial = 0;
    style_rec(&K, doc, NULL, sheets, nsheets);
}

/* ══ querySelector ════════════════════════════════════════════════════ */

int css_matches(arena_t* A, dom_node_t* el, const char* selector) {
    selector_t* sels;
    int n = parse_sel_list(A, selector, (uint32_t)strlen(selector), &sels);
    for (int i = 0; i < n; i++) if (sel_matches(&sels[i], el)) return 1;
    return 0;
}

static int query_rec(dom_node_t* n, selector_t* sels, int ns, dom_node_t** out, int max, int count) {
    for (dom_node_t* c = n->first; c && count < max; c = c->next) {
        if (c->type != DOM_ELEM) continue;
        for (int i = 0; i < ns; i++) if (sel_matches(&sels[i], c)) { out[count++] = c; break; }
        count = query_rec(c, sels, ns, out, max, count);
    }
    return count;
}

int css_query_all(arena_t* A, dom_node_t* root, const char* selector, dom_node_t** out, int max) {
    selector_t* sels;
    int n = parse_sel_list(A, selector, (uint32_t)strlen(selector), &sels);
    if (!n) return 0;
    return query_rec(root, sels, n, out, max, 0);
}

dom_node_t* css_query(arena_t* A, dom_node_t* root, const char* selector) {
    dom_node_t* r = NULL;
    return css_query_all(A, root, selector, &r, 1) ? r : NULL;
}
