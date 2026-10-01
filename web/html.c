#include "html.h"
#include "kstring.h"

/* ══ DOM ══════════════════════════════════════════════════════════════ */

static char lc(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

dom_node_t* dom_new_element(arena_t* A, const char* tag) {
    dom_node_t* n = (dom_node_t*)arena_alloc(A, sizeof(dom_node_t));
    n->type = DOM_ELEM;
    uint32_t l = (uint32_t)strlen(tag);
    n->tag = arena_strdup(A, tag, l);
    for (uint32_t i = 0; i < l; i++) n->tag[i] = lc(n->tag[i]);
    return n;
}

dom_node_t* dom_new_text(arena_t* A, const char* s, uint32_t n) {
    dom_node_t* t = (dom_node_t*)arena_alloc(A, sizeof(dom_node_t));
    t->type = DOM_TEXT;
    t->text = arena_strdup(A, s, n);
    t->text_len = n;
    return t;
}

void dom_append(dom_node_t* parent, dom_node_t* child) {
    if (child->parent) dom_remove(child);
    child->parent = parent;
    child->prev = parent->last;
    child->next = NULL;
    if (parent->last) parent->last->next = child;
    else parent->first = child;
    parent->last = child;
}

void dom_insert_before(dom_node_t* parent, dom_node_t* child, dom_node_t* ref) {
    if (!ref) { dom_append(parent, child); return; }
    if (child->parent) dom_remove(child);
    child->parent = parent;
    child->next = ref;
    child->prev = ref->prev;
    if (ref->prev) ref->prev->next = child;
    else parent->first = child;
    ref->prev = child;
}

void dom_remove(dom_node_t* n) {
    dom_node_t* p = n->parent;
    if (!p) return;
    if (n->prev) n->prev->next = n->next;
    else p->first = n->next;
    if (n->next) n->next->prev = n->prev;
    else p->last = n->prev;
    n->parent = n->prev = n->next = NULL;
}

void dom_remove_children(dom_node_t* n) {
    for (dom_node_t* c = n->first; c; c = c->next) c->parent = NULL;
    n->first = n->last = NULL;
}

const char* dom_attr(const dom_node_t* n, const char* name) {
    for (dom_attr_t* a = n->attrs; a; a = a->next)
        if (strcmp(a->name, name) == 0) return a->value;
    return NULL;
}

void dom_set_attr(arena_t* A, dom_node_t* n, const char* name, const char* value) {
    for (dom_attr_t* a = n->attrs; a; a = a->next)
        if (strcmp(a->name, name) == 0) { a->value = arena_strdup(A, value, (uint32_t)strlen(value)); return; }
    dom_attr_t* a = (dom_attr_t*)arena_alloc(A, sizeof(dom_attr_t));
    uint32_t l = (uint32_t)strlen(name);
    a->name = arena_strdup(A, name, l);
    for (uint32_t i = 0; i < l; i++) a->name[i] = lc(a->name[i]);
    a->value = arena_strdup(A, value, (uint32_t)strlen(value));
    /* keep source order */
    dom_attr_t** pp = &n->attrs;
    while (*pp) pp = &(*pp)->next;
    *pp = a;
}

void dom_remove_attr(dom_node_t* n, const char* name) {
    for (dom_attr_t** pp = &n->attrs; *pp; pp = &(*pp)->next)
        if (strcmp((*pp)->name, name) == 0) { *pp = (*pp)->next; return; }
}

int dom_has_class(const dom_node_t* n, const char* cls) {
    const char* c = dom_attr(n, "class");
    if (!c) return 0;
    uint32_t l = (uint32_t)strlen(cls);
    while (*c) {
        while (*c == ' ' || *c == '\t' || *c == '\n') c++;
        const char* s = c;
        while (*c && *c != ' ' && *c != '\t' && *c != '\n') c++;
        if ((uint32_t)(c - s) == l && memcmp(s, cls, l) == 0) return 1;
    }
    return 0;
}

dom_node_t* dom_find_tag(dom_node_t* root, const char* tag) {
    for (dom_node_t* c = root->first; c; c = c->next) {
        if (c->type == DOM_ELEM && strcmp(c->tag, tag) == 0) return c;
        dom_node_t* r = dom_find_tag(c, tag);
        if (r) return r;
    }
    return NULL;
}

dom_node_t* dom_find_id(dom_node_t* root, const char* id) {
    for (dom_node_t* c = root->first; c; c = c->next) {
        if (c->type != DOM_ELEM) continue;
        const char* v = dom_attr(c, "id");
        if (v && strcmp(v, id) == 0) return c;
        dom_node_t* r = dom_find_id(c, id);
        if (r) return r;
    }
    return NULL;
}

dom_node_t* dom_body(dom_node_t* doc) {
    dom_node_t* b = dom_find_tag(doc, "body");
    return b ? b : doc;
}

/* ── text and markup out ── */

typedef struct { arena_t* A; char* buf; uint32_t len, cap; } sbuf_t;

static void sb_add(sbuf_t* b, const char* s, uint32_t n) {
    if (b->len + n + 1 > b->cap) {
        uint32_t nc = b->cap ? b->cap * 2 : 256;
        while (nc < b->len + n + 1) nc *= 2;
        char* nb = (char*)arena_alloc(b->A, nc);
        if (b->A->oom) return;
        if (b->len) memcpy(nb, b->buf, b->len);
        b->buf = nb;
        b->cap = nc;
    }
    memcpy(b->buf + b->len, s, n);
    b->len += n;
    b->buf[b->len] = 0;
}
static void sb_str(sbuf_t* b, const char* s) { sb_add(b, s, (uint32_t)strlen(s)); }

static void text_rec(sbuf_t* b, dom_node_t* n) {
    for (dom_node_t* c = n->first; c; c = c->next) {
        if (c->type == DOM_TEXT) sb_add(b, c->text, c->text_len);
        else if (c->type == DOM_ELEM) text_rec(b, c);
    }
}

char* dom_text(arena_t* A, dom_node_t* n) {
    sbuf_t b = { A, NULL, 0, 0 };
    sb_add(&b, "", 0);
    if (n->type == DOM_TEXT) sb_add(&b, n->text, n->text_len);
    else text_rec(&b, n);
    return b.buf;
}

static int is_void(const char* t) {
    static const char* const v[] = { "area", "base", "br", "col", "embed", "hr", "img", "input", "link",
                                     "meta", "param", "source", "track", "wbr" };
    for (uint32_t i = 0; i < sizeof(v) / sizeof(v[0]); i++) if (strcmp(t, v[i]) == 0) return 1;
    return 0;
}

static void esc(sbuf_t* b, const char* s, uint32_t n, int attr) {
    for (uint32_t i = 0; i < n; i++) {
        char c = s[i];
        if (c == '&') sb_str(b, "&amp;");
        else if (c == '<') sb_str(b, "&lt;");
        else if (c == '>') sb_str(b, "&gt;");
        else if (attr && c == '"') sb_str(b, "&quot;");
        else sb_add(b, &c, 1);
    }
}

static void html_rec(sbuf_t* b, dom_node_t* n, int self) {
    if (n->type == DOM_TEXT) {
        if (n->parent && n->parent->type == DOM_ELEM &&
            (strcmp(n->parent->tag, "script") == 0 || strcmp(n->parent->tag, "style") == 0))
            sb_add(b, n->text, n->text_len);
        else esc(b, n->text, n->text_len, 0);
        return;
    }
    if (n->type == DOM_COMMENT) {
        sb_str(b, "<!--");
        sb_add(b, n->text, n->text_len);
        sb_str(b, "-->");
        return;
    }
    if (self && n->type == DOM_ELEM) {
        sb_str(b, "<");
        sb_str(b, n->tag);
        for (dom_attr_t* a = n->attrs; a; a = a->next) {
            sb_str(b, " ");
            sb_str(b, a->name);
            sb_str(b, "=\"");
            esc(b, a->value, (uint32_t)strlen(a->value), 1);
            sb_str(b, "\"");
        }
        sb_str(b, ">");
        if (is_void(n->tag)) return;
    }
    for (dom_node_t* c = n->first; c; c = c->next) html_rec(b, c, 1);
    if (self && n->type == DOM_ELEM) {
        sb_str(b, "</");
        sb_str(b, n->tag);
        sb_str(b, ">");
    }
}

char* dom_html(arena_t* A, dom_node_t* n, int outer) {
    sbuf_t b = { A, NULL, 0, 0 };
    sb_add(&b, "", 0);
    html_rec(&b, n, outer);
    return b.buf;
}

/* ══ entities ═════════════════════════════════════════════════════════ */

static const struct { const char* name; uint32_t cp; } ENTITIES[] = {
    { "amp", '&' }, { "lt", '<' }, { "gt", '>' }, { "quot", '"' }, { "apos", '\'' }, { "nbsp", 0xA0 },
    { "copy", 0xA9 }, { "reg", 0xAE }, { "trade", 0x2122 }, { "hellip", 0x2026 }, { "mdash", 0x2014 },
    { "ndash", 0x2013 }, { "laquo", 0xAB }, { "raquo", 0xBB }, { "lsquo", 0x2018 }, { "rsquo", 0x2019 },
    { "ldquo", 0x201C }, { "rdquo", 0x201D }, { "bull", 0x2022 }, { "middot", 0xB7 }, { "deg", 0xB0 },
    { "eacute", 0xE9 }, { "egrave", 0xE8 }, { "ecirc", 0xEA }, { "agrave", 0xE0 }, { "aacute", 0xE1 },
    { "ccedil", 0xE7 }, { "uuml", 0xFC }, { "ouml", 0xF6 }, { "auml", 0xE4 }, { "szlig", 0xDF },
    { "euro", 0x20AC }, { "pound", 0xA3 }, { "times", 0xD7 }, { "divide", 0xF7 }, { "larr", 0x2190 },
    { "rarr", 0x2192 }, { "uarr", 0x2191 }, { "darr", 0x2193 }, { "hearts", 0x2665 }, { "check", 0x2713 },
};

static uint32_t put_utf8(char* o, uint32_t cp) {
    if (cp < 0x80) { o[0] = (char)cp; return 1; }
    if (cp < 0x800) { o[0] = (char)(0xC0 | (cp >> 6)); o[1] = (char)(0x80 | (cp & 63)); return 2; }
    o[0] = (char)(0xE0 | (cp >> 12));
    o[1] = (char)(0x80 | ((cp >> 6) & 63));
    o[2] = (char)(0x80 | (cp & 63));
    return 3;
}

uint32_t html_decode_entities(char* s, uint32_t n) {
    uint32_t o = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (s[i] != '&') { s[o++] = s[i]; continue; }
        uint32_t j = i + 1;
        while (j < n && j - i < 12 && s[j] != ';' && s[j] != '&' && s[j] != ' ' && s[j] != '<') j++;
        if (j >= n || s[j] != ';') { s[o++] = s[i]; continue; }
        uint32_t cp = 0;
        int ok = 0;
        if (s[i + 1] == '#') {
            int hex = (s[i + 2] == 'x' || s[i + 2] == 'X');
            for (uint32_t k = i + 2 + (uint32_t)hex; k < j; k++) {
                char c = s[k];
                int d = (c >= '0' && c <= '9') ? c - '0' : (hex && lc(c) >= 'a' && lc(c) <= 'f') ? lc(c) - 'a' + 10 : -1;
                if (d < 0) { cp = 0; break; }
                cp = cp * (hex ? 16 : 10) + (uint32_t)d;
                ok = 1;
            }
            if (cp > 0xFFFF) cp = '?';
        } else {
            for (uint32_t e = 0; e < sizeof(ENTITIES) / sizeof(ENTITIES[0]); e++) {
                uint32_t l = (uint32_t)strlen(ENTITIES[e].name);
                if (l == j - i - 1 && memcmp(s + i + 1, ENTITIES[e].name, l) == 0) { cp = ENTITIES[e].cp; ok = 1; break; }
            }
        }
        if (!ok || cp == 0) { s[o++] = s[i]; continue; }
        o += put_utf8(s + o, cp);           /* never longer than the entity text */
        i = j;
    }
    return o;
}

/* ══ parser ═══════════════════════════════════════════════════════════ */

typedef struct {
    arena_t*    A;
    const char* s;
    uint32_t    n, pos;
    dom_node_t* stack[256];
    int         depth;
} hp_t;

static dom_node_t* cur(hp_t* P) { return P->stack[P->depth - 1]; }

static int in_list(const char* t, const char* const* l, int n) {
    for (int i = 0; i < n; i++) if (strcmp(t, l[i]) == 0) return 1;
    return 0;
}

/* elements whose start tag closes an open <p> */
static const char* const CLOSES_P[] = {
    "p", "div", "ul", "ol", "dl", "table", "h1", "h2", "h3", "h4", "h5", "h6", "pre", "form",
    "blockquote", "hr", "section", "article", "header", "footer", "nav", "aside", "main", "address",
    "fieldset", "figure", "details", "menu", "center",
};

static void pop_to(hp_t* P, int idx) {
    if (idx >= 1 && idx < P->depth) P->depth = idx;
}

/* index of the nearest open element with this tag, stopping at boundaries */
static int find_open(hp_t* P, const char* tag, const char* const* stops, int nstops) {
    for (int i = P->depth - 1; i >= 1; i--) {
        dom_node_t* e = P->stack[i];
        if (strcmp(e->tag, tag) == 0) return i;
        if (stops && in_list(e->tag, stops, nstops)) return -1;
    }
    return -1;
}

static void close_implied(hp_t* P, const char* tag) {
    static const char* const scope[] = { "table", "ul", "ol", "dl", "body", "html", "div", "td", "th", "button" };
    int idx;
    if (in_list(tag, CLOSES_P, (int)(sizeof(CLOSES_P) / sizeof(CLOSES_P[0])))) {
        idx = find_open(P, "p", scope, 10);
        if (idx > 0) pop_to(P, idx);
    }
    if (strcmp(tag, "li") == 0) {
        static const char* const ls[] = { "ul", "ol", "menu" };
        idx = find_open(P, "li", ls, 3);
        if (idx > 0) pop_to(P, idx);
    } else if (strcmp(tag, "dt") == 0 || strcmp(tag, "dd") == 0) {
        static const char* const ds[] = { "dl" };
        idx = find_open(P, "dt", ds, 1);
        if (idx > 0) pop_to(P, idx);
        idx = find_open(P, "dd", ds, 1);
        if (idx > 0) pop_to(P, idx);
    } else if (strcmp(tag, "tr") == 0) {
        static const char* const ts[] = { "table" };
        idx = find_open(P, "tr", ts, 1);
        if (idx > 0) pop_to(P, idx);
    } else if (strcmp(tag, "td") == 0 || strcmp(tag, "th") == 0) {
        static const char* const ts[] = { "table", "tr" };
        idx = find_open(P, "td", ts, 2);
        if (idx > 0) pop_to(P, idx);
        idx = find_open(P, "th", ts, 2);
        if (idx > 0) pop_to(P, idx);
    } else if (strcmp(tag, "option") == 0) {
        static const char* const os[] = { "select" };
        idx = find_open(P, "option", os, 1);
        if (idx > 0) pop_to(P, idx);
    } else if (strcmp(tag, "thead") == 0 || strcmp(tag, "tbody") == 0 || strcmp(tag, "tfoot") == 0) {
        static const char* const ts[] = { "table" };
        for (int k = 0; k < 3; k++) {
            static const char* const sec[] = { "thead", "tbody", "tfoot" };
            idx = find_open(P, sec[k], ts, 1);
            if (idx > 0) pop_to(P, idx);
        }
    }
}

static void add_text(hp_t* P, const char* s, uint32_t n) {
    if (!n) return;
    char* t = arena_strdup(P->A, s, n);
    n = html_decode_entities(t, n);
    t[n] = 0;
    dom_node_t* parent = cur(P);
    /* merge with a preceding text node */
    if (parent->last && parent->last->type == DOM_TEXT) {
        dom_node_t* l = parent->last;
        char* m = (char*)arena_alloc(P->A, l->text_len + n + 1);
        memcpy(m, l->text, l->text_len);
        memcpy(m + l->text_len, t, n);
        m[l->text_len + n] = 0;
        l->text = m;
        l->text_len += n;
        return;
    }
    dom_node_t* tn = (dom_node_t*)arena_alloc(P->A, sizeof(dom_node_t));
    tn->type = DOM_TEXT;
    tn->text = t;
    tn->text_len = n;
    dom_append(parent, tn);
}

static int is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

static void parse_tag(hp_t* P) {
    const char* s = P->s;
    uint32_t n = P->n, i = P->pos + 1;
    int end = 0;
    if (i < n && s[i] == '/') { end = 1; i++; }
    uint32_t ts = i;
    while (i < n && !is_space(s[i]) && s[i] != '>' && s[i] != '/') i++;
    char tag[32];
    uint32_t tl = i - ts < sizeof(tag) - 1 ? i - ts : (uint32_t)sizeof(tag) - 1;
    for (uint32_t k = 0; k < tl; k++) tag[k] = lc(s[ts + k]);
    tag[tl] = 0;
    if (!tl) { add_text(P, "<", 1); P->pos++; return; }

    if (end) {
        while (i < n && s[i] != '>') i++;
        P->pos = i < n ? i + 1 : n;
        int idx = find_open(P, tag, NULL, 0);
        if (idx > 0) pop_to(P, idx);
        else if (strcmp(tag, "p") == 0) {                 /* </p> without <p>: an empty paragraph */
            dom_append(cur(P), dom_new_element(P->A, "p"));
        } else if (strcmp(tag, "br") == 0) {
            dom_append(cur(P), dom_new_element(P->A, "br"));
        }
        return;
    }

    close_implied(P, tag);
    dom_node_t* el = dom_new_element(P->A, tag);
    /* attributes */
    int self_close = 0;
    while (i < n && s[i] != '>') {
        while (i < n && is_space(s[i])) i++;
        if (i < n && s[i] == '/') { self_close = 1; i++; continue; }
        if (i >= n || s[i] == '>') break;
        uint32_t ns = i;
        while (i < n && !is_space(s[i]) && s[i] != '=' && s[i] != '>' && !(s[i] == '/' && i + 1 < n && s[i + 1] == '>')) i++;
        uint32_t nl = i - ns;
        while (i < n && is_space(s[i])) i++;
        const char* vs = "";
        uint32_t vl = 0;
        if (i < n && s[i] == '=') {
            i++;
            while (i < n && is_space(s[i])) i++;
            if (i < n && (s[i] == '"' || s[i] == '\'')) {
                char q = s[i++];
                vs = s + i;
                while (i < n && s[i] != q) i++;
                vl = (uint32_t)(s + i - vs);
                if (i < n) i++;
            } else {
                vs = s + i;
                while (i < n && !is_space(s[i]) && s[i] != '>') i++;
                vl = (uint32_t)(s + i - vs);
            }
        }
        if (nl) {
            char* name = arena_strdup(P->A, s + ns, nl);
            char* val = arena_strdup(P->A, vs, vl);
            uint32_t dl = html_decode_entities(val, vl);
            val[dl] = 0;
            if (!dom_attr(el, name)) dom_set_attr(P->A, el, name, val);
        }
    }
    P->pos = i < n ? i + 1 : n;

    /* <html>, <head>, <body> appearing again just add their attributes */
    if (strcmp(tag, "html") == 0 || strcmp(tag, "body") == 0 || strcmp(tag, "head") == 0) {
        dom_node_t* existing = NULL;
        for (int k = 0; k < P->depth; k++)
            if (P->stack[k]->type == DOM_ELEM && strcmp(P->stack[k]->tag, tag) == 0) existing = P->stack[k];
        if (existing) {
            for (dom_attr_t* a = el->attrs; a; a = a->next)
                if (!dom_attr(existing, a->name)) dom_set_attr(P->A, existing, a->name, a->value);
            return;
        }
        if (strcmp(tag, "body") == 0) {                   /* leave <head> */
            int hi = find_open(P, "head", NULL, 0);
            if (hi > 0) pop_to(P, hi);
        }
    }

    dom_append(cur(P), el);
    if (self_close || is_void(tag)) return;

    /* raw text: <script>, <style>, <textarea>, <title> */
    if (strcmp(tag, "script") == 0 || strcmp(tag, "style") == 0 || strcmp(tag, "textarea") == 0 ||
        strcmp(tag, "title") == 0 || strcmp(tag, "xmp") == 0) {
        uint32_t start = P->pos, k = start;
        uint32_t tl2 = (uint32_t)strlen(tag);
        while (k < n) {
            if (s[k] == '<' && k + 1 < n && s[k + 1] == '/' && k + 2 + tl2 <= n &&
                strncasecmp(s + k + 2, tag, tl2) == 0) break;
            k++;
        }
        if (k > start) {
            int raw = strcmp(tag, "script") == 0 || strcmp(tag, "style") == 0;
            if (raw) dom_append(el, dom_new_text(P->A, s + start, k - start));
            else {
                P->stack[P->depth++] = el;
                add_text(P, s + start, k - start);
                P->depth--;
            }
        }
        while (k < n && s[k] != '>') k++;
        P->pos = k < n ? k + 1 : n;
        return;
    }
    if (P->depth < (int)(sizeof(P->stack) / sizeof(P->stack[0]))) P->stack[P->depth++] = el;
}

static void parse_run(hp_t* P) {
    const char* s = P->s;
    uint32_t n = P->n;
    while (P->pos < n) {
        uint32_t i = P->pos;
        if (s[i] == '<') {
            if (i + 3 < n && s[i + 1] == '!' && s[i + 2] == '-' && s[i + 3] == '-') {
                uint32_t k = i + 4;
                while (k + 2 < n && !(s[k] == '-' && s[k + 1] == '-' && s[k + 2] == '>')) k++;
                dom_node_t* c = dom_new_text(P->A, s + i + 4, k > i + 4 ? k - i - 4 : 0);
                c->type = DOM_COMMENT;
                dom_append(cur(P), c);
                P->pos = k + 3 <= n ? k + 3 : n;
                continue;
            }
            if (i + 1 < n && (s[i + 1] == '!' || s[i + 1] == '?')) {     /* doctype, <?xml */
                while (i < n && s[i] != '>') i++;
                P->pos = i < n ? i + 1 : n;
                continue;
            }
            if (i + 1 < n && ((s[i + 1] >= 'a' && s[i + 1] <= 'z') || (s[i + 1] >= 'A' && s[i + 1] <= 'Z') || s[i + 1] == '/')) {
                parse_tag(P);
                continue;
            }
        }
        uint32_t k = i + 1;
        while (k < n && s[k] != '<') k++;
        add_text(P, s + i, k - i);
        P->pos = k;
    }
}

void html_parse_into(arena_t* A, dom_node_t* parent, const char* src, uint32_t len) {
    hp_t* P = (hp_t*)arena_alloc(A, sizeof(hp_t));
    P->A = A;
    P->s = src;
    P->n = len;
    P->stack[0] = parent;
    P->depth = 1;
    parse_run(P);
}

dom_node_t* html_parse(arena_t* A, const char* src, uint32_t len) {
    dom_node_t* doc = (dom_node_t*)arena_alloc(A, sizeof(dom_node_t));
    doc->type = DOM_DOC;
    doc->tag = arena_strdup(A, "#document", 9);
    html_parse_into(A, doc, src, len);

    /* normalize to document > html > (head, body) */
    dom_node_t* html = NULL;
    for (dom_node_t* c = doc->first; c; c = c->next)
        if (c->type == DOM_ELEM && strcmp(c->tag, "html") == 0) html = c;
    if (!html) {
        html = dom_new_element(A, "html");
        while (doc->first) {
            dom_node_t* c = doc->first;
            dom_remove(c);
            dom_append(html, c);
        }
        dom_append(doc, html);
    } else {
        /* stray nodes outside <html> move inside it */
        dom_node_t* c = doc->first;
        while (c) {
            dom_node_t* nx = c->next;
            if (c != html && c->type != DOM_COMMENT) { dom_remove(c); dom_append(html, c); }
            c = nx;
        }
    }
    dom_node_t *head = NULL, *body = NULL;
    for (dom_node_t* c = html->first; c; c = c->next) {
        if (c->type != DOM_ELEM) continue;
        if (!head && strcmp(c->tag, "head") == 0) head = c;
        if (!body && strcmp(c->tag, "body") == 0) body = c;
    }
    if (!head) {
        head = dom_new_element(A, "head");
        dom_insert_before(html, head, html->first);
    }
    if (!body) {
        body = dom_new_element(A, "body");
        dom_node_t* c = html->first;
        while (c) {
            dom_node_t* nx = c->next;
            if (c != head) {
                int head_stuff = c->type == DOM_ELEM && (strcmp(c->tag, "title") == 0 || strcmp(c->tag, "meta") == 0 ||
                                                         strcmp(c->tag, "link") == 0 || strcmp(c->tag, "base") == 0);
                dom_remove(c);
                if (head_stuff) dom_append(head, c);
                else dom_append(body, c);
            }
            c = nx;
        }
        dom_append(html, body);
    } else {
        /* content after </body> belongs in it */
        dom_node_t* c = body->next;
        while (c) {
            dom_node_t* nx = c->next;
            dom_remove(c);
            dom_append(body, c);
            c = nx;
        }
    }
    return doc;
}
