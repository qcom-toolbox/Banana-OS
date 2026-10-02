#include "regex.h"
#include "kstring.h"

/*
 * The pattern compiles to a tree of nodes; matching walks it with
 * explicit continuations ("match this node, then the rest"), which gives
 * correct backtracking through groups, alternation and quantifiers.
 */

enum {
    R_CHAR = 1,     /* c */
    R_ANY,          /* . */
    R_CLASS,        /* [..] - set[] bitmap, neg */
    R_BOL, R_EOL,   /* ^ $ */
    R_WORDB, R_NWORDB,
    R_GROUP,        /* (...) - group index (0 = non-capturing), child */
    R_ALT,          /* a | b - child list via alt */
    R_REPEAT,       /* child {min,max} greedy/lazy */
    R_LOOK,         /* (?=...) / (?!...) - neg */
    R_BACKREF,      /* \n */
};

typedef struct rnode {
    uint8_t  k;
    uint8_t  neg, lazy;
    char     c;
    int      group;
    int      min, max;              /* max -1 = unbounded */
    uint8_t* set;                   /* 32 bytes: 256-bit class */
    struct rnode* child;            /* sequence inside a group/repeat/look */
    struct rnode* alt;              /* next alternative (R_ALT) */
    struct rnode* next;             /* next in sequence */
} rnode_t;

struct regex {
    rnode_t* root;
    int      ngroups;
    int      icase, multiline, dotall;
    const char* names[RX_MAX_GROUPS + 1];
};

typedef struct {
    arena_t* A;
    const char* p;
    const char* end;
    regex_t* re;
    const char* err;
    int depth;
} rparse_t;

static rnode_t* rn(rparse_t* P, int k) {
    rnode_t* n = (rnode_t*)arena_alloc(P->A, sizeof(rnode_t));
    n->k = (uint8_t)k;
    return n;
}

static void set_add(uint8_t* set, int c) { set[(c & 255) >> 3] |= (uint8_t)(1u << (c & 7)); }
static int set_has(const uint8_t* set, int c) { return set[(c & 255) >> 3] & (1u << (c & 7)); }

static int is_dig(int c) { return c >= '0' && c <= '9'; }
static int is_wordc(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || is_dig(c) || c == '_'; }
static int is_spc(int c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v' || c == 0xA0; }
static int lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

/* \d \w \s and friends into a set */
static void class_escape(uint8_t* set, char e) {
    int neg = e == 'D' || e == 'W' || e == 'S';
    char b = (char)lower(e);
    for (int c = 0; c < 256; c++) {
        int in = b == 'd' ? is_dig(c) : b == 'w' ? is_wordc(c) : is_spc(c);
        if (b == 's' && c >= 0x80) in = 0;
        if (in != neg) set_add(set, c);
    }
}

static int hexd(char c) {
    if (is_dig(c)) return c - '0';
    c = (char)lower(c);
    return (c >= 'a' && c <= 'f') ? c - 'a' + 10 : -1;
}

/* a single escaped character (\n, \x41, A - as UTF-8 only for < 0x80) */
static int char_escape(rparse_t* P, char e) {
    switch (e) {
    case 'n': return '\n';
    case 't': return '\t';
    case 'r': return '\r';
    case 'f': return '\f';
    case 'v': return '\v';
    case '0': return 0;
    case 'x':
        if (P->end - P->p >= 2 && hexd(P->p[0]) >= 0 && hexd(P->p[1]) >= 0) {
            int v = hexd(P->p[0]) * 16 + hexd(P->p[1]);
            P->p += 2;
            return v;
        }
        return 'x';
    case 'u':
        if (P->end - P->p >= 4 && hexd(P->p[0]) >= 0 && hexd(P->p[1]) >= 0 && hexd(P->p[2]) >= 0 && hexd(P->p[3]) >= 0) {
            int v = (hexd(P->p[0]) << 12) | (hexd(P->p[1]) << 8) | (hexd(P->p[2]) << 4) | hexd(P->p[3]);
            P->p += 4;
            return v < 256 ? v : '?';
        }
        if (P->p < P->end && *P->p == '{') {               /* \u{...} */
            while (P->p < P->end && *P->p != '}') P->p++;
            if (P->p < P->end) P->p++;
            return '?';
        }
        return 'u';
    case 'c':
        if (P->p < P->end) return *P->p++ & 31;
        return 'c';
    default: return (unsigned char)e;
    }
}

static rnode_t* parse_alt(rparse_t* P);

static rnode_t* parse_class(rparse_t* P) {
    rnode_t* n = rn(P, R_CLASS);
    n->set = (uint8_t*)arena_alloc(P->A, 32);
    if (P->p < P->end && *P->p == '^') { n->neg = 1; P->p++; }
    int first = 1;
    while (P->p < P->end && (*P->p != ']' || first)) {
        first = 0;
        int lo;
        char c = *P->p++;
        if (c == '\\' && P->p < P->end) {
            char e = *P->p++;
            if (e == 'd' || e == 'D' || e == 'w' || e == 'W' || e == 's' || e == 'S') { class_escape(n->set, e); continue; }
            if (e == 'b') lo = '\b';
            else lo = char_escape(P, e);
        } else {
            lo = (unsigned char)c;
        }
        int hi = lo;
        if (P->p + 1 < P->end && *P->p == '-' && P->p[1] != ']') {
            P->p++;
            char c2 = *P->p++;
            if (c2 == '\\' && P->p < P->end) { char e = *P->p++; hi = char_escape(P, e); }
            else hi = (unsigned char)c2;
        }
        if (hi < lo) { int t = lo; lo = hi; hi = t; }
        for (int k = lo; k <= hi && k < 256; k++) set_add(n->set, k);
        /* non-ASCII ranges (UTF-8 text): accept all continuation/lead bytes */
        if (hi >= 0x80) for (int k = 0x80; k < 256; k++) set_add(n->set, k);
    }
    if (P->p >= P->end) { P->err = "unterminated character class"; return n; }
    P->p++;
    if (P->re->icase)
        for (int c = 'a'; c <= 'z'; c++) {
            if (set_has(n->set, c)) set_add(n->set, c - 32);
            if (set_has(n->set, c - 32)) set_add(n->set, c);
        }
    return n;
}

static int parse_int(rparse_t* P, int* out) {
    if (P->p >= P->end || !is_dig(*P->p)) return 0;
    int v = 0;
    while (P->p < P->end && is_dig(*P->p)) { v = v * 10 + (*P->p - '0'); if (v > 100000) v = 100000; P->p++; }
    *out = v;
    return 1;
}

static rnode_t* parse_atom(rparse_t* P) {
    char c = *P->p++;
    rnode_t* n;
    switch (c) {
    case '.': return rn(P, R_ANY);
    case '^': return rn(P, R_BOL);
    case '$': return rn(P, R_EOL);
    case '[': return parse_class(P);
    case '(': {
        if (++P->depth > 100) { P->err = "pattern nested too deeply"; return rn(P, R_ANY); }
        n = rn(P, R_GROUP);
        if (P->p < P->end && *P->p == '?') {
            P->p++;
            char t = P->p < P->end ? *P->p++ : 0;
            if (t == ':') n->group = 0;
            else if (t == '=' || t == '!') { n->k = R_LOOK; n->neg = t == '!'; }
            else if (t == '<' && P->p < P->end && (*P->p == '=' || *P->p == '!')) {
                /* lookbehind: unsupported - treated as "always true" */
                n->k = R_LOOK; n->neg = 2; P->p++;
            } else if (t == '<') {
                const char* ns = P->p;
                while (P->p < P->end && *P->p != '>') P->p++;
                n->group = ++P->re->ngroups;
                if (n->group <= RX_MAX_GROUPS) P->re->names[n->group] = arena_strdup(P->A, ns, (uint32_t)(P->p - ns));
                if (P->p < P->end) P->p++;
            } else { P->err = "unsupported group"; }
        } else {
            n->group = ++P->re->ngroups;
        }
        if (n->group > RX_MAX_GROUPS) n->group = 0;
        n->child = parse_alt(P);
        if (P->p >= P->end || *P->p != ')') { P->err = "missing )"; return n; }
        P->p++;
        P->depth--;
        return n;
    }
    case '\\': {
        if (P->p >= P->end) { n = rn(P, R_CHAR); n->c = '\\'; return n; }
        char e = *P->p++;
        if (e == 'd' || e == 'D' || e == 'w' || e == 'W' || e == 's' || e == 'S') {
            n = rn(P, R_CLASS);
            n->set = (uint8_t*)arena_alloc(P->A, 32);
            class_escape(n->set, e);
            return n;
        }
        if (e == 'b') return rn(P, R_WORDB);
        if (e == 'B') return rn(P, R_NWORDB);
        if (e >= '1' && e <= '9') {
            n = rn(P, R_BACKREF);
            n->group = e - '0';
            while (P->p < P->end && is_dig(*P->p) && n->group * 10 + (*P->p - '0') <= P->re->ngroups) n->group = n->group * 10 + (*P->p++ - '0');
            return n;
        }
        if (e == 'k' && P->p < P->end && *P->p == '<') {
            const char* ns = ++P->p;
            while (P->p < P->end && *P->p != '>') P->p++;
            uint32_t nl = (uint32_t)(P->p - ns);
            if (P->p < P->end) P->p++;
            n = rn(P, R_BACKREF);
            for (int g = 1; g <= P->re->ngroups && g <= RX_MAX_GROUPS; g++)
                if (P->re->names[g] && strlen(P->re->names[g]) == nl && memcmp(P->re->names[g], ns, nl) == 0) n->group = g;
            return n;
        }
        n = rn(P, R_CHAR);
        n->c = (char)char_escape(P, e);
        return n;
    }
    default:
        n = rn(P, R_CHAR);
        n->c = c;
        return n;
    }
}

/* atom + quantifier */
static rnode_t* parse_piece(rparse_t* P) {
    rnode_t* a = parse_atom(P);
    if (P->err || P->p >= P->end) return a;
    int min = -1, max = -1;
    char q = *P->p;
    if (q == '*') { min = 0; max = -1; P->p++; }
    else if (q == '+') { min = 1; max = -1; P->p++; }
    else if (q == '?') { min = 0; max = 1; P->p++; }
    else if (q == '{') {
        const char* save = P->p++;
        int lo, hi;
        if (parse_int(P, &lo)) {
            hi = lo;
            if (P->p < P->end && *P->p == ',') {
                P->p++;
                if (!parse_int(P, &hi)) hi = -1;
            }
            if (P->p < P->end && *P->p == '}') { P->p++; min = lo; max = hi; }
            else P->p = save;
        } else {
            P->p = save;
        }
        if (min < 0) {                       /* a literal '{' */
            P->p = save + 1;
            rnode_t* lit = rn(P, R_CHAR);
            lit->c = '{';
            a->next = lit;
            return a;
        }
    }
    if (min < 0) return a;
    rnode_t* r = rn(P, R_REPEAT);
    r->min = min;
    r->max = max;
    r->child = a;
    if (P->p < P->end && *P->p == '?') { r->lazy = 1; P->p++; }
    return r;
}

/* a sequence up to | or ) */
static rnode_t* parse_seq(rparse_t* P) {
    rnode_t* head = NULL;
    rnode_t** tail = &head;
    while (!P->err && P->p < P->end && *P->p != '|' && *P->p != ')') {
        rnode_t* n = parse_piece(P);
        *tail = n;
        while (*tail) tail = &(*tail)->next;
    }
    return head;
}

static rnode_t* parse_alt(rparse_t* P) {
    rnode_t* first = parse_seq(P);
    if (P->p >= P->end || *P->p != '|') return first;
    rnode_t* alt = rn(P, R_ALT);
    rnode_t* opt = rn(P, R_GROUP);         /* each alternative wrapped, chained through ->alt */
    opt->child = first;
    alt->child = opt;
    rnode_t* last = opt;
    while (!P->err && P->p < P->end && *P->p == '|') {
        P->p++;
        rnode_t* o = rn(P, R_GROUP);
        o->child = parse_seq(P);
        last->alt = o;
        last = o;
    }
    return alt;
}

regex_t* rx_compile(arena_t* A, const char* pattern, uint32_t len, const char* flags, const char** err) {
    regex_t* re = (regex_t*)arena_alloc(A, sizeof(regex_t));
    for (const char* f = flags ? flags : ""; *f; f++) {
        if (*f == 'i') re->icase = 1;
        else if (*f == 'm') re->multiline = 1;
        else if (*f == 's') re->dotall = 1;
    }
    rparse_t P;
    memset(&P, 0, sizeof(P));
    P.A = A;
    P.p = pattern;
    P.end = pattern + len;
    P.re = re;
    re->root = parse_alt(&P);
    if (!P.err && P.p < P.end) P.err = "unmatched )";
    if (P.err) { *err = P.err; return NULL; }
    return re;
}

int rx_groups(const regex_t* re) { return re->ngroups > RX_MAX_GROUPS ? RX_MAX_GROUPS : re->ngroups; }
const char* rx_group_name(const regex_t* re, int i) { return i >= 1 && i <= RX_MAX_GROUPS ? re->names[i] : NULL; }

/* ── matching ─────────────────────────────────────────────────────── */

typedef struct cont {
    rnode_t* node;               /* continue with this sequence... */
    const struct cont* next;     /* ...then this */
    /* a pending repeat: the remaining count after node (a REPEAT node) */
    int      rep_count;
    int      rep_pos;            /* position where this iteration started (empty-loop guard) */
} cont_t;

typedef struct {
    regex_t* re;
    const char* s;
    int n;
    int* caps;
    uint32_t steps;
    int depth;
} mstate_t;

#define STEP_LIMIT 2000000u
#define DEPTH_LIMIT 4000

static int match_here(mstate_t* M, rnode_t* node, int pos, const cont_t* k);

/* the rest after a node sequence ended: pop continuations */
static int match_cont(mstate_t* M, int pos, const cont_t* k) {
    if (!k) return pos;
    return match_here(M, k->node, pos, k->next);
}

static int repeat_iter(mstate_t* M, rnode_t* r, int count, int pos, int start_pos, const cont_t* k);

/* after one iteration of r's child: try more, or stop */
static int match_after_iter(mstate_t* M, rnode_t* r, int count, int pos, int start_pos, const cont_t* k) {
    /* start_pos: where the iteration that just ended began */
    return repeat_iter(M, r, count, pos, start_pos, k);
}

static int repeat_iter(mstate_t* M, rnode_t* r, int count, int pos, int start_pos, const cont_t* k) {
    if (++M->steps > STEP_LIMIT || ++M->depth > DEPTH_LIMIT) { M->depth--; return -2; }
    int res = -1;
    int can_more = (r->max < 0 || count < r->max);
    int can_stop = count >= r->min;
    /* an iteration that matched nothing would loop forever: stop there */
    if (count > r->min && pos == start_pos && count > 0) can_more = 0;
    /* one more iteration: match the child, then come back here through a
     * marker continuation (node = the repeat, rep_count = the new count) */
    if (!r->lazy) {
        if (can_more) {
            cont_t mark = { r, k, count + 1, pos };
            res = match_here(M, r->child, pos, &mark);
            if (res >= 0 || res == -2) { M->depth--; return res; }
        }
        if (can_stop) res = match_cont(M, pos, k);
    } else {
        if (can_stop) {
            res = match_cont(M, pos, k);
            if (res >= 0 || res == -2) { M->depth--; return res; }
        }
        if (can_more) {
            cont_t mark = { r, k, count + 1, pos };
            res = match_here(M, r->child, pos, &mark);
        }
    }
    M->depth--;
    return res;
}

static int word_at(mstate_t* M, int i) { return i >= 0 && i < M->n && is_wordc((unsigned char)M->s[i]); }

static int match_here(mstate_t* M, rnode_t* node, int pos, const cont_t* k) {
    for (;;) {
        if (++M->steps > STEP_LIMIT) return -2;
        if (!node) {
            /* end of a sequence: a repeat marker continuation goes back to its repeat */
            if (!k) return pos;
            if (k->node && k->node->k == R_REPEAT && k->rep_count > 0) {
                const cont_t* kk = k;
                return match_after_iter(M, kk->node, kk->rep_count, pos, kk->rep_pos, kk->next);
            }
            node = k->node;
            k = k->next;
            continue;
        }
        switch (node->k) {
        case R_CHAR: {
            if (pos >= M->n) return -1;
            int a = (unsigned char)M->s[pos], b = (unsigned char)node->c;
            if (a != b && !(M->re->icase && lower(a) == lower(b))) return -1;
            pos++;
            node = node->next;
            continue;
        }
        case R_ANY:
            if (pos >= M->n) return -1;
            if (!M->re->dotall && (M->s[pos] == '\n' || M->s[pos] == '\r')) return -1;
            pos++;
            node = node->next;
            continue;
        case R_CLASS: {
            if (pos >= M->n) return -1;
            int in = set_has(node->set, (unsigned char)M->s[pos]) != 0;
            if (in == node->neg) return -1;
            pos++;
            node = node->next;
            continue;
        }
        case R_BOL:
            if (!(pos == 0 || (M->re->multiline && M->s[pos - 1] == '\n'))) return -1;
            node = node->next;
            continue;
        case R_EOL:
            if (!(pos == M->n || (M->re->multiline && M->s[pos] == '\n'))) return -1;
            node = node->next;
            continue;
        case R_WORDB: case R_NWORDB: {
            int b = word_at(M, pos - 1) != word_at(M, pos);
            if (b != (node->k == R_WORDB)) return -1;
            node = node->next;
            continue;
        }
        case R_BACKREF: {
            int g = node->group;
            if (g > 0 && g <= RX_MAX_GROUPS && M->caps[2 * g] >= 0) {
                int s0 = M->caps[2 * g], len = M->caps[2 * g + 1] - s0;
                if (pos + len > M->n) return -1;
                for (int i = 0; i < len; i++) {
                    int a = (unsigned char)M->s[pos + i], b = (unsigned char)M->s[s0 + i];
                    if (a != b && !(M->re->icase && lower(a) == lower(b))) return -1;
                }
                pos += len;
            }
            node = node->next;
            continue;
        }
        case R_GROUP: {
            int g = node->group;
            if (!g) {
                cont_t c = { node->next, k, 0, 0 };
                return match_here(M, node->child, pos, &c);
            }
            /* capture: remember the old values to restore on failure */
            int o0 = M->caps[2 * g], o1 = M->caps[2 * g + 1];
            M->caps[2 * g] = pos;
            /* an end marker: a tiny continuation that records the end, then goes on */
            static rnode_t endmark[RX_MAX_GROUPS + 1];     /* k = 0xFF, group = g */
            endmark[g].k = 0xFF;
            endmark[g].group = g;
            endmark[g].next = NULL;
            cont_t after = { node->next, k, 0, 0 };
            cont_t c = { &endmark[g], &after, 0, 0 };
            int r = match_here(M, node->child, pos, &c);
            if (r < 0) { M->caps[2 * g] = o0; M->caps[2 * g + 1] = o1; }
            return r;
        }
        case 0xFF: {                                    /* the end of capture group */
            int g = node->group;
            int old = M->caps[2 * g + 1];
            M->caps[2 * g + 1] = pos;
            int r = match_here(M, NULL, pos, k);
            if (r < 0) M->caps[2 * g + 1] = old;
            return r;
        }
        case R_ALT: {
            for (rnode_t* o = node->child; o; o = o->alt) {
                cont_t c = { node->next, k, 0, 0 };
                int r = match_here(M, o->child, pos, &c);
                if (r >= 0 || r == -2) return r;
            }
            return -1;
        }
        case R_REPEAT: {
            cont_t c = { node->next, k, 0, 0 };
            return repeat_iter(M, node, 0, pos, -1, &c);
        }
        case R_LOOK: {
            if (node->neg == 2) { node = node->next; continue; }   /* lookbehind: ignored */
            int saved[2 * (RX_MAX_GROUPS + 1)];
            memcpy(saved, M->caps, sizeof(saved));
            int r = match_here(M, node->child, pos, NULL);
            if (r == -2) return -2;
            int ok = r >= 0;
            if (node->neg) { memcpy(M->caps, saved, sizeof(saved)); if (ok) return -1; }
            else if (!ok) return -1;
            node = node->next;
            continue;
        }
        default:
            return -1;
        }
    }
}

int rx_exec(regex_t* re, const char* s, uint32_t n, uint32_t start, int sticky, int* caps) {
    mstate_t M;
    M.re = re;
    M.s = s;
    M.n = (int)n;
    M.caps = caps;
    M.steps = 0;
    M.depth = 0;
    for (uint32_t p = start; p <= n; p++) {
        for (int i = 0; i < 2 * (RX_MAX_GROUPS + 1); i++) caps[i] = -1;
        int end = match_here(&M, re->root, (int)p, NULL);
        if (end == -2) return 0;                        /* too much backtracking: no match */
        if (end >= 0) {
            caps[0] = (int)p;
            caps[1] = end;
            return 1;
        }
        if (sticky) return 0;
        /* a pattern anchored with ^ (and not multiline) can only match at 0 */
        if (re->root && re->root->k == R_BOL && !re->multiline) return 0;
    }
    return 0;
}
