#include "css.h"
#include "kstring.h"

/* ══ data ═════════════════════════════════════════════════════════════ */

#define SEL_PARTS 8

typedef struct {
    char* tag;                  /* NULL: any */
    char* id;
    char* cls[6];
    int   ncls;
    char* attr;                 /* [attr] / [attr=value] */
    char* attr_val;
    int   first_child, last_child, link, never;
} simple_t;

typedef struct {
    simple_t part[SEL_PARTS];
    char     comb[SEL_PARTS];   /* combinator before part i: ' ' or '>' */
    int      n;
    uint32_t spec;
} selector_t;

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
} rule_t;

struct css_sheet {
    rule_t* rules;
    int     n, cap;
    int     origin;             /* 0 = default (user agent), 1 = page */
};

static char lc(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }
static int is_ws(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }
static int is_ident(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || (unsigned char)c >= 0x80;
}

static char* dup_lc(arena_t* A, const char* s, uint32_t n) {
    char* d = arena_strdup(A, s, n);
    for (uint32_t i = 0; i < n; i++) d[i] = lc(d[i]);
    return d;
}

/* ══ selectors ════════════════════════════════════════════════════════ */

/* parses one complex selector; returns 0 if unusable */
static int parse_selector(arena_t* A, const char* s, uint32_t n, selector_t* out) {
    memset(out, 0, sizeof(*out));
    uint32_t i = 0;
    char comb = ' ';
    while (i < n) {
        while (i < n && is_ws(s[i])) i++;
        if (i >= n) break;
        if (s[i] == '>' || s[i] == '+' || s[i] == '~') {
            comb = s[i] == '>' ? '>' : '?';      /* sibling combinators: unsupported */
            i++;
            continue;
        }
        if (out->n >= SEL_PARTS) return 0;
        simple_t* p = &out->part[out->n];
        out->comb[out->n] = comb;
        if (comb == '?') p->never = 1;
        comb = ' ';
        int any = 0;
        while (i < n && !is_ws(s[i]) && s[i] != '>' && s[i] != '+' && s[i] != '~') {
            if (s[i] == '*') { i++; any = 1; continue; }
            if (s[i] == '#' || s[i] == '.') {
                char k = s[i++];
                uint32_t st = i;
                while (i < n && is_ident(s[i])) i++;
                char* name = arena_strdup(A, s + st, i - st);
                if (k == '#') { p->id = name; out->spec += 0x10000; }
                else if (p->ncls < 6) { p->cls[p->ncls++] = name; out->spec += 0x100; }
                any = 1;
                continue;
            }
            if (s[i] == '[') {
                uint32_t st = ++i;
                while (i < n && s[i] != ']' && s[i] != '=' && s[i] != '~' && s[i] != '|' && s[i] != '^' && s[i] != '$' && s[i] != '*') i++;
                p->attr = dup_lc(A, s + st, i - st);
                if (i < n && s[i] != ']' && s[i] != '=') { p->never = 1; i++; }   /* ~= |= etc. */
                if (i < n && s[i] == '=') {
                    i++;
                    char q = (i < n && (s[i] == '"' || s[i] == '\'')) ? s[i++] : 0;
                    uint32_t vs = i;
                    while (i < n && (q ? s[i] != q : s[i] != ']')) i++;
                    p->attr_val = arena_strdup(A, s + vs, i - vs);
                    if (q && i < n) i++;
                }
                while (i < n && s[i] != ']') i++;
                if (i < n) i++;
                out->spec += 0x100;
                any = 1;
                continue;
            }
            if (s[i] == ':') {
                i++;
                if (i < n && s[i] == ':') i++;
                uint32_t st = i;
                while (i < n && (is_ident(s[i]) || s[i] == '(' || s[i] == ')')) i++;
                char* ps = dup_lc(A, s + st, i - st);
                if (strcmp(ps, "first-child") == 0) p->first_child = 1;
                else if (strcmp(ps, "last-child") == 0) p->last_child = 1;
                else if (strcmp(ps, "link") == 0 || strcmp(ps, "visited") == 0) p->link = 1;
                else if (strcmp(ps, "root") == 0) p->tag = "html";
                else p->never = 1;                   /* :hover, ::before, ... */
                out->spec += 0x100;
                any = 1;
                continue;
            }
            if (is_ident(s[i])) {
                uint32_t st = i;
                while (i < n && is_ident(s[i])) i++;
                p->tag = dup_lc(A, s + st, i - st);
                out->spec += 1;
                any = 1;
                continue;
            }
            return 0;                                /* garbage */
        }
        if (!any) return 0;
        out->n++;
    }
    return out->n > 0;
}

static int match_simple(const simple_t* p, dom_node_t* e) {
    if (e->type != DOM_ELEM || p->never) return 0;
    if (p->tag && strcmp(p->tag, e->tag) != 0) return 0;
    if (p->id) {
        const char* id = dom_attr(e, "id");
        if (!id || strcmp(id, p->id) != 0) return 0;
    }
    for (int i = 0; i < p->ncls; i++) if (!dom_has_class(e, p->cls[i])) return 0;
    if (p->attr) {
        const char* v = dom_attr(e, p->attr);
        if (!v) return 0;
        if (p->attr_val && strcmp(v, p->attr_val) != 0) return 0;
    }
    if (p->link && !(strcmp(e->tag, "a") == 0 && dom_attr(e, "href"))) return 0;
    if (p->first_child) {
        for (dom_node_t* s = e->prev; s; s = s->prev) if (s->type == DOM_ELEM) return 0;
    }
    if (p->last_child) {
        for (dom_node_t* s = e->next; s; s = s->next) if (s->type == DOM_ELEM) return 0;
    }
    return 1;
}

static int match_from(const selector_t* sel, int idx, dom_node_t* e) {
    if (!match_simple(&sel->part[idx], e)) return 0;
    if (idx == 0) return 1;
    if (sel->comb[idx] == '>') {
        dom_node_t* p = e->parent;
        return p && p->type == DOM_ELEM && match_from(sel, idx - 1, p);
    }
    for (dom_node_t* a = e->parent; a && a->type == DOM_ELEM; a = a->parent)
        if (match_from(sel, idx - 1, a)) return 1;
    return 0;
}

static int sel_matches(const selector_t* sel, dom_node_t* e) {
    return match_from(sel, sel->n - 1, e);
}

/* splits "a, b > c" into selectors */
static int parse_sel_list(arena_t* A, const char* s, uint32_t n, selector_t** out) {
    int count = 1;
    for (uint32_t i = 0; i < n; i++) if (s[i] == ',') count++;
    selector_t* sels = (selector_t*)arena_alloc(A, (uint32_t)count * (uint32_t)sizeof(selector_t));
    int k = 0;
    uint32_t st = 0;
    for (uint32_t i = 0; i <= n; i++) {
        if (i == n || s[i] == ',') {
            if (parse_selector(A, s + st, i - st, &sels[k])) k++;
            st = i + 1;
        }
    }
    *out = sels;
    return k;
}

/* ══ style sheet parser ═══════════════════════════════════════════════ */

static int parse_decls(arena_t* A, const char* s, uint32_t n, decl_t** out) {
    int cap = 8, cnt = 0;
    decl_t* d = (decl_t*)arena_alloc(A, (uint32_t)cap * (uint32_t)sizeof(decl_t));
    uint32_t i = 0;
    while (i < n) {
        while (i < n && (is_ws(s[i]) || s[i] == ';')) i++;
        uint32_t ps = i;
        while (i < n && s[i] != ':' && s[i] != ';') i++;
        if (i >= n || s[i] != ':') { while (i < n && s[i] != ';') i++; continue; }
        uint32_t pe = i;
        while (pe > ps && is_ws(s[pe - 1])) pe--;
        i++;
        uint32_t vs = i;
        int paren = 0;
        while (i < n && (s[i] != ';' || paren)) {
            if (s[i] == '(') paren++;
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
        d[cnt].prop = dup_lc(A, s + ps, pe - ps);
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

static void parse_block_list(arena_t* A, css_sheet_t* sh, const char* s, uint32_t n) {
    uint32_t i = 0;
    while (i < n) {
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
        while (i < n && s[i] != '{' && s[i] != ';') i++;
        if (i >= n) break;
        if (s[i] == ';') { i++; continue; }                  /* @import ...; */
        uint32_t sel_end = i;
        /* the block, with nesting (@media { ... { } }) */
        uint32_t bs = ++i;
        int depth = 1;
        while (i < n && depth) {
            if (s[i] == '{') depth++;
            else if (s[i] == '}') depth--;
            if (depth) i++;
        }
        uint32_t be = i;
        if (i < n) i++;
        if (s[st] == '@') {
            /* @media screen / all: keep its rules; skip print, @font-face, @keyframes... */
            if (strncasecmp(s + st, "@media", 6) == 0) {
                const char* q = s + st + 6;
                int print_only = 0;
                for (const char* k = q; k < s + sel_end; k++)
                    if (strncasecmp(k, "print", 5) == 0) print_only = 1;
                for (const char* k = q; k < s + sel_end; k++)
                    if (strncasecmp(k, "screen", 6) == 0 || strncasecmp(k, "all", 3) == 0) print_only = 0;
                if (!print_only) parse_block_list(A, sh, s + bs, be - bs);
            }
            continue;
        }
        rule_t r;
        memset(&r, 0, sizeof(r));
        r.nsels = parse_sel_list(A, s + st, sel_end - st, &r.sels);
        if (!r.nsels) continue;
        r.ndecls = parse_decls(A, s + bs, be - bs, &r.decls);
        if (r.ndecls) sheet_add(A, sh, r);
    }
}

css_sheet_t* css_parse(arena_t* A, const char* src, uint32_t len, int origin) {
    css_sheet_t* sh = (css_sheet_t*)arena_alloc(A, sizeof(css_sheet_t));
    sh->origin = origin;
    parse_block_list(A, sh, src, len);
    return sh;
}

static const char DEFAULT_CSS[] =
    "html, body, div, p, h1, h2, h3, h4, h5, h6, ul, ol, dl, dt, dd, pre, form, blockquote, address,"
    " section, article, header, footer, nav, aside, main, figure, figcaption, fieldset, hr, center,"
    " details, summary, legend, menu, noscript { display: block }"
    "head, script, style, title, meta, link, base, template, noscript, datalist { display: none }"
    "li { display: list-item }"
    "table { display: table; border-spacing: 0 }"
    "tr { display: table-row }"
    "td, th { display: table-cell; padding: 2px 4px }"
    "thead, tbody, tfoot { display: table-row-group }"
    "caption { display: block; text-align: center }"
    "img, input, button, select, textarea { display: inline-block }"
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
    "big { font-size: 20px }"
    "small, sub, sup { font-size: 13px }"
    "code, kbd, samp, tt { color: #333 }"
    "mark { background-color: #ff0 }"
    "button { padding: 2px 6px; border: 1px solid #777; background-color: #ddd }"
    "fieldset { border: 1px solid #999; padding: 4px 8px; margin: 8px 2px }"
    "legend { font-weight: bold }";

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
};

static int hexv(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    c = lc(c);
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

uint32_t css_color(const char* s, int* ok) {
    *ok = 0;
    while (is_ws(*s)) s++;
    if (*s == '#') {
        s++;
        int n = 0;
        while (hexv(s[n]) >= 0) n++;
        if (n == 3 || n == 4) {
            *ok = 1;
            return (uint32_t)(hexv(s[0]) * 17) << 16 | (uint32_t)(hexv(s[1]) * 17) << 8 | (uint32_t)(hexv(s[2]) * 17);
        }
        if (n == 6 || n == 8) {
            *ok = 1;
            uint32_t v = 0;
            for (int i = 0; i < 6; i++) v = v * 16 + (uint32_t)hexv(s[i]);
            return v;
        }
        return 0;
    }
    if (strncasecmp(s, "rgb", 3) == 0) {
        const char* p = strchr(s, '(');
        if (!p) return 0;
        int c[3] = { 0, 0, 0 };
        p++;
        for (int i = 0; i < 3; i++) {
            while (*p && (is_ws(*p) || *p == ',')) p++;
            int v = 0, any = 0;
            while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; any = 1; }
            if (*p == '.') { p++; while (*p >= '0' && *p <= '9') p++; }
            if (*p == '%') { v = v * 255 / 100; p++; }
            if (!any) return 0;
            c[i] = v > 255 ? 255 : v;
        }
        *ok = 1;
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

/* a length in px; pct_of: base for %, -1 = percentages unknown (returns LEN_AUTO) */
static int parse_len(const char* s, int font_px, int pct_of, int* is_pct) {
    if (is_pct) *is_pct = 0;
    while (is_ws(*s)) s++;
    if (strncasecmp(s, "auto", 4) == 0) return LEN_AUTO;
    int neg = 0;
    if (*s == '-') { neg = 1; s++; } else if (*s == '+') s++;
    long whole = 0, frac = 0, fdiv = 1;
    int any = 0;
    while (*s >= '0' && *s <= '9') { whole = whole * 10 + (*s - '0'); s++; any = 1; }
    if (*s == '.') {
        s++;
        while (*s >= '0' && *s <= '9') { if (fdiv < 10000) { frac = frac * 10 + (*s - '0'); fdiv *= 10; } s++; any = 1; }
    }
    if (!any) return LEN_AUTO;
    long milli = whole * 1000 + frac * 1000 / fdiv;      /* value x 1000 */
    long px;
    if (strncasecmp(s, "em", 2) == 0) px = milli * font_px / 1000;
    else if (strncasecmp(s, "rem", 3) == 0) px = milli * 16 / 1000;
    else if (strncasecmp(s, "pt", 2) == 0) px = milli * 4 / 3000;
    else if (strncasecmp(s, "vw", 2) == 0) px = milli * 8 / 1000;
    else if (strncasecmp(s, "vh", 2) == 0) px = milli * 6 / 1000;
    else if (strncasecmp(s, "ch", 2) == 0) px = milli * 8 / 1000;
    else if (*s == '%') {
        if (is_pct) *is_pct = (int)(milli / 1000);
        if (pct_of < 0) return LEN_AUTO;
        px = milli * pct_of / 100000;
    } else px = milli / 1000;
    return (int)(neg ? -px : px);
}

static int scale_for(int px) {
    if (px <= 18) return 1;
    if (px <= 36) return 2;
    if (px <= 54) return 3;
    return 4;
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
    const char* p = v;
    while (*p && n < 4) {
        while (is_ws(*p)) p++;
        if (!*p) break;
        int l = parse_len(p, font_px, -1, NULL);
        autos[n] = (l == LEN_AUTO);
        vals[n++] = l == LEN_AUTO ? 0 : l;
        while (*p && !is_ws(*p)) p++;
    }
    if (n == 0) return;
    if (n == 1) { vals[1] = vals[2] = vals[3] = vals[0]; autos[1] = autos[2] = autos[3] = autos[0]; }
    else if (n == 2) { vals[2] = vals[0]; vals[3] = vals[1]; autos[2] = autos[0]; autos[3] = autos[1]; }
    else if (n == 3) { vals[3] = vals[1]; autos[3] = autos[1]; }
    for (int i = 0; i < 4; i++) out[i] = vals[i];
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
    const char* p = v;
    int none = has_word(v, "none") || has_word(v, "hidden");
    while (*p) {
        while (is_ws(*p)) p++;
        if (!*p) break;
        const char* tok = p;
        while (*p && !is_ws(*p)) {
            if (*p == '(') { while (*p && *p != ')') p++; }
            if (*p) p++;
        }
        if ((*tok >= '0' && *tok <= '9') || *tok == '.') { width = parse_len(tok, st->font_px, -1, NULL); have_w = 1; }
        else if (strncasecmp(tok, "thin", 4) == 0) { width = 1; have_w = 1; }
        else if (strncasecmp(tok, "medium", 6) == 0) { width = 2; have_w = 1; }
        else if (strncasecmp(tok, "thick", 5) == 0) { width = 4; have_w = 1; }
        else {
            uint32_t c = css_color(tok, &ok);
            if (ok) color = c;
        }
    }
    if (!have_w) width = none ? 0 : 2;
    if (none) width = 0;
    if (width > 20) width = 20;
    for (int i = 0; i < 4; i++) {
        if (side >= 0 && i != side) continue;
        st->border[i] = width;
        st->border_color[i] = color;
    }
}

static void apply_decl(style_t* st, const style_t* parent, const char* prop, const char* v) {
    int ok;
    if (strcmp(v, "inherit") == 0 && parent) {
        /* only the common inherited-by-request properties */
        if (strcmp(prop, "color") == 0) st->color = parent->color;
        else if (strcmp(prop, "background-color") == 0) { st->bg = parent->bg; st->has_bg = parent->has_bg; }
        return;
    }
    switch (prop[0]) {
    case 'b':
        if (strcmp(prop, "background-color") == 0 || strcmp(prop, "background") == 0) {
            if (has_word(v, "transparent") || has_word(v, "none")) { st->has_bg = 0; return; }
            /* the first color-looking token of the shorthand */
            const char* p = v;
            while (*p) {
                while (is_ws(*p)) p++;
                uint32_t c = css_color(p, &ok);
                if (ok) { st->bg = c; st->has_bg = 1; return; }
                while (*p && !is_ws(*p)) {
                    if (*p == '(') { while (*p && *p != ')') p++; }
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
        if (strcmp(prop, "border-width") == 0) { four(v, st->font_px, st->border, NULL); return; }
        if (strcmp(prop, "border-color") == 0) {
            uint32_t c = css_color(v, &ok);
            if (ok) for (int i = 0; i < 4; i++) st->border_color[i] = c;
            return;
        }
        if (strcmp(prop, "border-style") == 0) {
            if (has_word(v, "none")) for (int i = 0; i < 4; i++) st->border[i] = 0;
            else for (int i = 0; i < 4; i++) if (!st->border[i]) st->border[i] = 2;
            return;
        }
        return;
    case 'c':
        if (strcmp(prop, "color") == 0) {
            uint32_t c = css_color(v, &ok);
            if (ok) st->color = c;
        }
        return;
    case 'd':
        if (strcmp(prop, "display") == 0) {
            if (has_word(v, "none")) st->display = DISP_NONE;
            else if (has_word(v, "inline-block") || has_word(v, "inline-flex")) st->display = DISP_INLINE_BLOCK;
            else if (has_word(v, "inline")) st->display = DISP_INLINE;
            else if (has_word(v, "list-item")) st->display = DISP_LIST_ITEM;
            else if (has_word(v, "table-row-group") || has_word(v, "table-header-group") || has_word(v, "table-footer-group"))
                st->display = DISP_TABLE_GROUP;
            else if (has_word(v, "table-row")) st->display = DISP_TABLE_ROW;
            else if (has_word(v, "table-cell")) st->display = DISP_TABLE_CELL;
            else if (has_word(v, "table")) st->display = DISP_TABLE;
            else st->display = DISP_BLOCK;                /* block, flex, grid, ... */
        }
        return;
    case 'f':
        if (strcmp(prop, "font-weight") == 0) {
            st->bold = has_word(v, "bold") || has_word(v, "bolder") ||
                       (v[0] >= '6' && v[0] <= '9' && v[1] == '0' && v[2] == '0');
            return;
        }
        if (strcmp(prop, "font-style") == 0) { st->italic = has_word(v, "italic") || has_word(v, "oblique"); return; }
        if (strcmp(prop, "font-size") == 0 || strcmp(prop, "font") == 0) {
            int base = parent ? parent->font_px : 16;
            if (strcmp(prop, "font") == 0) {
                if (has_word(v, "bold")) st->bold = 1;
                if (has_word(v, "italic")) st->italic = 1;
                /* the size is the token before an optional /line-height */
                const char* p = v;
                while (*p) {
                    while (is_ws(*p)) p++;
                    if ((*p >= '0' && *p <= '9') || *p == '.') {
                        int px = parse_len(p, base, base, NULL);
                        if (px != LEN_AUTO && !(p[0] >= '1' && p[0] <= '9' && p[1] == '0' && p[2] == '0' && (is_ws(p[3]) || !p[3])))
                            set_font_px(st, px);
                        break;
                    }
                    while (*p && !is_ws(*p)) p++;
                }
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
        }
        return;
    case 'h':
        if (strcmp(prop, "height") == 0) {
            int l = parse_len(v, st->font_px, -1, NULL);
            st->height = l;
        }
        return;
    case 'l':
        if (strcmp(prop, "list-style-type") == 0 || strcmp(prop, "list-style") == 0) {
            if (has_word(v, "none")) st->list_style = LIST_NONE;
            else if (has_word(v, "decimal")) st->list_style = LIST_DECIMAL;
            else if (has_word(v, "circle")) st->list_style = LIST_CIRCLE;
            else if (has_word(v, "square")) st->list_style = LIST_SQUARE;
            else if (has_word(v, "disc")) st->list_style = LIST_DISC;
        }
        return;
    case 'm':
        if (strcmp(prop, "margin") == 0) { four(v, st->font_px, st->margin, &st->margin_auto_lr); return; }
        if (strncmp(prop, "margin-", 7) == 0) {
            int l = parse_len(v, st->font_px, -1, NULL);
            int side = strcmp(prop + 7, "top") == 0 ? 0 : strcmp(prop + 7, "right") == 0 ? 1 :
                       strcmp(prop + 7, "bottom") == 0 ? 2 : strcmp(prop + 7, "left") == 0 ? 3 : -1;
            if (side >= 0) st->margin[side] = l == LEN_AUTO ? 0 : l;
            return;
        }
        if (strcmp(prop, "max-width") == 0) {
            int pct;
            int l = parse_len(v, st->font_px, -1, &pct);
            st->max_width = l;
        }
        return;
    case 'p':
        if (strcmp(prop, "padding") == 0) { four(v, st->font_px, st->padding, NULL); return; }
        if (strncmp(prop, "padding-", 8) == 0) {
            int l = parse_len(v, st->font_px, -1, NULL);
            int side = strcmp(prop + 8, "top") == 0 ? 0 : strcmp(prop + 8, "right") == 0 ? 1 :
                       strcmp(prop + 8, "bottom") == 0 ? 2 : strcmp(prop + 8, "left") == 0 ? 3 : -1;
            if (side >= 0) st->padding[side] = l == LEN_AUTO ? 0 : l;
        }
        return;
    case 't':
        if (strcmp(prop, "text-align") == 0) {
            st->align = has_word(v, "center") ? ALIGN_CENTER : has_word(v, "right") ? ALIGN_RIGHT : ALIGN_LEFT;
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
        }
        return;
    case 'v':
        if (strcmp(prop, "visibility") == 0) st->visible = !(has_word(v, "hidden") || has_word(v, "collapse"));
        return;
    case 'w':
        if (strcmp(prop, "width") == 0) {
            int pct;
            int l = parse_len(v, st->font_px, -1, &pct);
            st->width = l;
            st->width_pct = pct;
            return;
        }
        if (strcmp(prop, "white-space") == 0) {
            st->pre = has_word(v, "pre") || has_word(v, "pre-wrap") || has_word(v, "pre-line");
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
    st->font_px = p->font_px;
    st->scale = p->scale;
}

/* old HTML attributes: bgcolor, color, align, width, border, ... */
static void presentational(arena_t* A, dom_node_t* e, style_t* st) {
    (void)A;
    const char* v;
    int ok;
    if ((v = dom_attr(e, "bgcolor"))) { uint32_t c = css_color(v, &ok); if (ok) { st->bg = c; st->has_bg = 1; } }
    if (strcmp(e->tag, "font") == 0) {
        if ((v = dom_attr(e, "color"))) { uint32_t c = css_color(v, &ok); if (ok) st->color = c; }
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
        st->width_pct = pct;
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
        if ((v = dom_attr(e, "text"))) { uint32_t c = css_color(v, &ok); if (ok) st->color = c; }
    }
    if (strcmp(e->tag, "hr") == 0 && (v = dom_attr(e, "color"))) {
        uint32_t c = css_color(v, &ok);
        if (ok) st->border_color[0] = c;
    }
}

static void style_element(arena_t* A, dom_node_t* e, const style_t* parent, css_sheet_t** sheets, int nsheets,
                          match_t* buf, int bufcap) {
    style_t* st = (style_t*)arena_alloc(A, sizeof(style_t));   /* A may be a fresh arena each restyle */
    e->style = st;
    inherit(st, parent);
    for (int i = 0; i < 4; i++) st->border_color[i] = st->color;
    int nm = 0;
    uint32_t order = 0;
    /* 1. default sheet, 2. presentational attributes, 3. page sheets, 4. style="" */
    for (int pass = 0; pass < 2; pass++) {
        if (pass == 1) {
            /* default-sheet matches first */
            for (int i = 0; i < nm; i++)
                for (int j = i + 1; j < nm; j++)
                    if (buf[j].spec < buf[i].spec || (buf[j].spec == buf[i].spec && buf[j].order < buf[i].order)) {
                        match_t t = buf[i]; buf[i] = buf[j]; buf[j] = t;
                    }
            for (int i = 0; i < nm; i++) apply_decl(st, parent, buf[i].d->prop, buf[i].d->val);
            for (int i = 0; i < 4; i++) if (!st->border[i]) st->border_color[i] = st->color;
            presentational(A, e, st);
            nm = 0;
        }
        for (int s = 0; s < nsheets; s++) {
            css_sheet_t* sh = sheets[s];
            if (!sh || (pass == 0) != (sh->origin == 0)) continue;
            for (int r = 0; r < sh->n; r++) {
                rule_t* rule = &sh->rules[r];
                uint32_t best = 0;
                int hit = 0;
                for (int k = 0; k < rule->nsels; k++)
                    if (sel_matches(&rule->sels[k], e)) { hit = 1; if (rule->sels[k].spec >= best) best = rule->sels[k].spec; }
                if (!hit) continue;
                for (int d = 0; d < rule->ndecls && nm < bufcap; d++) {
                    buf[nm].d = &rule->decls[d];
                    buf[nm].spec = best | (rule->decls[d].important ? 0x1000000u : 0);
                    buf[nm].order = order++;
                    nm++;
                }
            }
        }
    }
    /* page rules, then style="" (beats everything but !important) */
    for (int i = 0; i < nm; i++)
        for (int j = i + 1; j < nm; j++)
            if (buf[j].spec < buf[i].spec || (buf[j].spec == buf[i].spec && buf[j].order < buf[i].order)) {
                match_t t = buf[i]; buf[i] = buf[j]; buf[j] = t;
            }
    int i = 0;
    for (; i < nm && !(buf[i].spec & 0x1000000u); i++) apply_decl(st, parent, buf[i].d->prop, buf[i].d->val);
    const char* inl = dom_attr(e, "style");
    if (inl) {
        decl_t* ds;
        int nd = parse_decls(A, inl, (uint32_t)strlen(inl), &ds);
        for (int k = 0; k < nd; k++) apply_decl(st, parent, ds[k].prop, ds[k].val);
    }
    for (; i < nm; i++) apply_decl(st, parent, buf[i].d->prop, buf[i].d->val);
    if (e->tag[0] == 'a' && e->tag[1] == 0 && !dom_attr(e, "href")) { /* plain anchors stay plain */ }
}

static void style_rec(arena_t* A, dom_node_t* n, const style_t* parent, css_sheet_t** sheets, int nsheets,
                      match_t* buf, int bufcap) {
    for (dom_node_t* c = n->first; c; c = c->next) {
        if (c->type != DOM_ELEM) continue;
        style_element(A, c, parent, sheets, nsheets, buf, bufcap);
        style_rec(A, c, c->style, sheets, nsheets, buf, bufcap);
    }
}

void css_style_tree(arena_t* A, dom_node_t* doc, css_sheet_t** sheets, int nsheets) {
    match_t* buf = (match_t*)arena_alloc(A, 512 * (uint32_t)sizeof(match_t));
    style_rec(A, doc, NULL, sheets, nsheets, buf, 512);
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
