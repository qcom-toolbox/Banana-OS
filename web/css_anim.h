/* SPDX-License-Identifier: GPL-3.0-or-later */
/* CSS animations: @keyframes and the animation properties. On every restyle
 * an animated element gets its keyframes' values for the current time
 * (css_anim_now): colours, lengths, percentages and numbers in matching
 * values are interpolated (background-position, color, background-color,
 * width, left...), others switch at the middle of their interval. The page
 * restyles while some animation runs (css_anim_running). Included only by
 * css.c. */
#define KF_FRAMES 16

typedef struct css_kf {
    const char* name;
    int         n;
    int         off[KF_FRAMES];          /* per mille, ascending */
    decl_t*     decls[KF_FRAMES];
    int         nd[KF_FRAMES];
    struct css_kf* next;
} css_kf_t;

uint32_t css_anim_now;                    /* page.c: the page's clock, ms */
int      css_anim_running;                /* set by a restyle that animates something */

/* "@keyframes spin { from {...} 50% {...} to {...} }" */
static void kf_parse(arena_t* A, css_sheet_t* sh, const char* head, uint32_t hn, const char* s, uint32_t n) {
    uint32_t i = 0;
    while (i < hn && head[i] != ' ' && head[i] != '\t' && head[i] != '\n') i++;   /* @keyframes / @-webkit-keyframes */
    while (i < hn && is_ws(head[i])) i++;
    uint32_t a = i;
    while (i < hn && !is_ws(head[i])) i++;
    const char* nm = head + a;
    uint32_t nl = i - a;
    if (nl && (nm[0] == '"' || nm[0] == '\'')) { nm++; nl = nl >= 2 ? nl - 2 : 0; }
    if (!nl) return;
    css_kf_t* kf = (css_kf_t*)arena_alloc(A, sizeof(css_kf_t));
    if (!kf || A->oom) return;
    memset(kf, 0, sizeof(*kf));
    char* name = (char*)arena_alloc(A, nl + 1);
    if (!name || A->oom) return;
    memcpy(name, nm, nl);                                /* names are case-sensitive */
    name[nl] = 0;
    kf->name = name;
    i = 0;
    while (i < n && kf->n < KF_FRAMES) {
        while (i < n && (is_ws(s[i]) || s[i] == ';')) i++;
        uint32_t st = i;
        while (i < n && s[i] != '{') i++;
        if (i >= n) break;
        uint32_t se = i, bs = ++i;
        while (i < n && s[i] != '}') i++;
        uint32_t be = i;
        if (i < n) i++;
        decl_t* d = NULL;
        int nd = parse_decls(A, s + bs, be - bs, &d);
        /* "0%, 50%" - the same declarations at each */
        uint32_t p = st;
        while (p < se && kf->n < KF_FRAMES) {
            while (p < se && (is_ws(s[p]) || s[p] == ',')) p++;
            if (p >= se) break;
            int off = -1;
            if (!strncasecmp(s + p, "from", 4)) off = 0;
            else if (!strncasecmp(s + p, "to", 2)) off = 1000;
            else {
                const char* q = s + p;
                int m = num_milli(&q);
                if (q != s + p) off = m / 100;              /* per mille */
            }
            while (p < se && s[p] != ',') p++;
            if (off < 0 || off > 1000) continue;
            int k = kf->n++;
            while (k > 0 && kf->off[k - 1] > off) { kf->off[k] = kf->off[k - 1]; kf->decls[k] = kf->decls[k - 1]; kf->nd[k] = kf->nd[k - 1]; k--; }
            kf->off[k] = off;
            kf->decls[k] = d;
            kf->nd[k] = nd;
        }
    }
    if (!kf->n) return;
    kf->next = sh->keyframes;
    sh->keyframes = kf;
}

/* a time: "2s", "300ms" -> ms; -1 if not one */
static int anim_time(const char* t) {
    const char* q = t;
    int m = num_milli(&q);
    if (q == t) return -1;
    if (!strcasecmp(q, "ms")) return m / 1000;
    if (!strcasecmp(q, "s")) return m;
    return -1;
}

enum { ANIM_EASE = 0, ANIM_LINEAR, ANIM_EASE_IN, ANIM_EASE_OUT, ANIM_EASE_IN_OUT, ANIM_STEP_END, ANIM_STEP_START };

static int anim_timing_word(const char* t) {
    if (!strcasecmp(t, "linear")) return ANIM_LINEAR;
    if (!strcasecmp(t, "ease")) return ANIM_EASE;
    if (!strcasecmp(t, "ease-in")) return ANIM_EASE_IN;
    if (!strcasecmp(t, "ease-out")) return ANIM_EASE_OUT;
    if (!strcasecmp(t, "ease-in-out")) return ANIM_EASE_IN_OUT;
    if (!strcasecmp(t, "step-end") || !strncasecmp(t, "steps(", 6)) return ANIM_STEP_END;
    if (!strcasecmp(t, "step-start")) return ANIM_STEP_START;
    if (!strncasecmp(t, "cubic-bezier(", 13)) return ANIM_EASE;
    return -1;
}

/* the name: where token t is in v (v outlives the style) */
static void anim_name_of(style_t* st, const char* v, const char* t) {
    uint32_t n = (uint32_t)strlen(t);
    const char* p = strstr(v, t);
    while (p && ((p > v && is_ident(p[-1])) || is_ident(p[n]))) p = strstr(p + 1, t);
    if (!p || n > 255) return;
    st->anim_name = p;
    st->anim_name_len = (uint8_t)n;
}

/* the animation properties; returns 1 if prop was one */
static int anim_decl(style_t* st, const char* prop, const char* v) {
    if (strncmp(prop, "animation", 9) && strncmp(prop, "-webkit-animation", 17)) return 0;
    if (prop[0] == '-') prop += 8;
    const char* sub = prop[9] == '-' ? prop + 10 : "";
    css_value_tokens it = { v, 4096 };
    char t[CSS_VALUE_TOKEN];
    if (!*sub) {                                         /* the shorthand: the first animation */
        st->anim_name = NULL;
        st->anim_dur = 0; st->anim_delay = 0; st->anim_iter = 1; st->anim_dir = 0; st->anim_timing = ANIM_EASE; st->anim_fill = 0;
        int times = 0;
        while (css_next_value(&it, t) == 1) {
            if (!strcmp(t, ",")) break;
            int ms = anim_time(t), tw;
            if (ms >= 0) { if (times++ == 0) st->anim_dur = ms; else st->anim_delay = ms; continue; }
            if ((tw = anim_timing_word(t)) >= 0) { st->anim_timing = (uint8_t)tw; continue; }
            if (!strcasecmp(t, "infinite")) { st->anim_iter = -1; continue; }
            if (t[0] >= '0' && t[0] <= '9') { const char* q = t; int m = num_milli(&q); st->anim_iter = m / 1000 > 0 ? m / 1000 : 1; continue; }
            if (!strcasecmp(t, "alternate")) { st->anim_dir = 1; continue; }
            if (!strcasecmp(t, "reverse")) { st->anim_dir = 2; continue; }
            if (!strcasecmp(t, "alternate-reverse")) { st->anim_dir = 3; continue; }
            if (!strcasecmp(t, "normal") || !strcasecmp(t, "running") || !strcasecmp(t, "paused")) continue;
            if (!strcasecmp(t, "forwards")) { st->anim_fill |= 1; continue; }
            if (!strcasecmp(t, "backwards")) { st->anim_fill |= 2; continue; }
            if (!strcasecmp(t, "both")) { st->anim_fill = 3; continue; }
            if (!strcasecmp(t, "none")) { st->anim_name = NULL; continue; }
            anim_name_of(st, v, t);
        }
        return 1;
    }
    if (css_next_value(&it, t) != 1) return 1;
    if (!strcmp(sub, "name")) { st->anim_name = NULL; if (strcasecmp(t, "none")) anim_name_of(st, v, t); }
    else if (!strcmp(sub, "duration")) { int ms = anim_time(t); if (ms >= 0) st->anim_dur = ms; }
    else if (!strcmp(sub, "delay")) { int ms = anim_time(t); if (ms >= 0) st->anim_delay = ms; }
    else if (!strcmp(sub, "iteration-count")) {
        if (!strcasecmp(t, "infinite")) st->anim_iter = -1;
        else { const char* q = t; int m = num_milli(&q); st->anim_iter = m / 1000 > 0 ? m / 1000 : 1; }
    }
    else if (!strcmp(sub, "direction")) st->anim_dir = (uint8_t)(!strcasecmp(t, "alternate") ? 1 : !strcasecmp(t, "reverse") ? 2 : !strcasecmp(t, "alternate-reverse") ? 3 : 0);
    else if (!strcmp(sub, "timing-function")) { int tw = anim_timing_word(t); if (tw >= 0) st->anim_timing = (uint8_t)tw; }
    else if (!strcmp(sub, "fill-mode")) st->anim_fill = (uint8_t)(!strcasecmp(t, "forwards") ? 1 : !strcasecmp(t, "backwards") ? 2 : !strcasecmp(t, "both") ? 3 : 0);
    return 1;
}

/* 0..1000 through time -> 0..1000 through the values */
static int anim_ease(int mode, int x) {
    int64_t t = x;
    switch (mode) {
    case ANIM_LINEAR: return x;
    case ANIM_EASE_IN: return (int)(t * t / 1000);
    case ANIM_EASE_OUT: return (int)(1000 - (1000 - t) * (1000 - t) / 1000);
    case ANIM_STEP_END: return x >= 1000 ? 1000 : 0;
    case ANIM_STEP_START: return x > 0 ? 1000 : 0;
    default: return (int)(t * t * (3000 - 2 * t) / 1000000);   /* ease, ease-in-out: smoothstep */
    }
}

/* a value's numbers, and its text with each number as \1 */
#define ANIM_NUMS 12
static int anim_split(const char* v, char* skel, int cap, int* nums, char units[][4]) {
    int k = 0, o = 0;
    const char* p = v;
    while (*p && o < cap - 2) {
        int start = (*p >= '0' && *p <= '9') || ((*p == '-' || *p == '.') && ((p[1] >= '0' && p[1] <= '9') || p[1] == '.'));
        if (start && (p == v || !is_ident(p[-1]) || p[-1] == '-') && k < ANIM_NUMS && !(p > v && p[-1] == '#')) {
            const char* q = p;
            int m = num_milli(&q);
            if (q != p) {
                int u = 0;
                while ((is_ident(*q) && *q != '-' && *q != '\\') || *q == '%') { if (u < 3) units[k][u++] = *q; q++; }
                units[k][u] = 0;
                nums[k++] = m;
                skel[o++] = '\1';
                p = q;
                continue;
            }
        }
        skel[o++] = *p++;
    }
    skel[o] = 0;
    return k;
}

static char* anim_mix(arena_t* A, const char* a, const char* b, int f) {
    int ok1, ok2;
    uint32_t c1 = css_color_blend(a, &ok1), c2 = css_color_blend(b, &ok2);
    if (ok1 && ok2) {                                    /* two colours */
        if (ok1 == 2) c1 = 0xFF000000u | (c2 & 0xFFFFFFu);
        if (ok2 == 2) c2 = 0xFF000000u | (c1 & 0xFFFFFFu);
        int ch[4];
        for (int sh = 0, i = 0; sh < 32; sh += 8, i++) {
            int x = (int)((c1 >> sh) & 255), y = (int)((c2 >> sh) & 255);
            ch[i] = x + (y - x) * f / 1000;
        }
        char* out = (char*)arena_alloc(A, 48);
        if (!out) return NULL;
        ksnprintf(out, 48, "rgba(%d,%d,%d,%d.%03d)", ch[2], ch[1], ch[0], (255 - ch[3]) / 255, (255 - ch[3]) * 1000 / 255 % 1000);
        return out;
    }
    char s1[512], s2[512];
    int n1[ANIM_NUMS], n2[ANIM_NUMS];
    char u1[ANIM_NUMS][4], u2[ANIM_NUMS][4];
    if (strlen(a) > 400 || strlen(b) > 400) return NULL;
    int k1 = anim_split(a, s1, sizeof(s1), n1, u1), k2 = anim_split(b, s2, sizeof(s2), n2, u2);
    if (!k1 || k1 != k2 || strcmp(s1, s2)) return NULL;
    for (int i = 0; i < k1; i++) if (strcmp(u1[i], u2[i]) && n1[i] && n2[i]) return NULL;
    char* out = (char*)arena_alloc(A, 640);
    if (!out) return NULL;
    int o = 0, i = 0;
    for (const char* p = s1; *p && o < 600; p++) {
        if (*p != '\1') { out[o++] = *p; continue; }
        int64_t m = n1[i] + (int64_t)(n2[i] - n1[i]) * f / 1000;
        const char* unit = u1[i][0] ? u1[i] : u2[i];
        int neg = m < 0;
        if (neg) m = -m;
        o += ksnprintf(out + o, 640 - o, "%s%d.%03d%s", neg ? "-" : "", (int)(m / 1000), (int)(m % 1000), unit);
        i++;
    }
    out[o] = 0;
    return out;
}

static const char* kf_value(const css_kf_t* kf, int frame, const char* prop) {
    for (int i = 0; i < kf->nd[frame]; i++) if (!strcmp(kf->decls[frame][i].prop, prop)) return kf->decls[frame][i].val;
    return NULL;
}

static void apply_decl(style_t* st, const style_t* parent, const char* prop, const char* v);

/* the element's animation at css_anim_now */
static void anim_apply(arena_t* A, dom_node_t* e, style_t* st, const style_t* parent, css_sheet_t** sheets, int nsheets) {
    if (!st->anim_name || st->anim_dur <= 0) { e->anim_key = 0; return; }
    const css_kf_t* kf = NULL;
    for (int s = 0; s < nsheets && !kf; s++)
        for (const css_kf_t* k = sheets[s] ? sheets[s]->keyframes : NULL; k; k = k->next)
            if (strlen(k->name) == st->anim_name_len && !strncmp(k->name, st->anim_name, st->anim_name_len)) { kf = k; break; }
    if (!kf) return;
    uint32_t key = 5381;
    for (int c = 0; c < st->anim_name_len; c++) key = key * 33 + (uint8_t)st->anim_name[c];
    key = key * 33 + (uint32_t)st->anim_dur;
    if (!key) key = 1;
    if (e->anim_key != key) { e->anim_key = key; e->anim_t0 = css_anim_now; }
    int64_t el = (int64_t)(css_anim_now - e->anim_t0) - st->anim_delay;
    int dur = st->anim_dur;
    if (el < 0) {
        if (!(st->anim_fill & 2)) { css_anim_running = 1; return; }
        el = 0;
    }
    int64_t cycle = el / dur;
    int x;                                               /* 0..1000 through this cycle */
    int iter = st->anim_iter ? st->anim_iter : 1;        /* -1: infinite */
    if (iter > 0 && cycle >= iter) {
        if (!(st->anim_fill & 1)) return;                /* done: the element's own style */
        cycle = iter - 1;
        x = 1000;
    } else {
        x = (int)((el % dur) * 1000 / dur);
        css_anim_running = 1;
    }
    int rev = st->anim_dir == 2 || (st->anim_dir == 1 && (cycle & 1)) || (st->anim_dir == 3 && !(cycle & 1));
    if (rev) x = 1000 - x;
    /* the frames around x */
    int a = 0, b = kf->n - 1;
    for (int i = 0; i < kf->n; i++) { if (kf->off[i] <= x) a = i; }
    for (int i = kf->n - 1; i >= 0; i--) { if (kf->off[i] >= x) b = i; }
    int span = kf->off[b] - kf->off[a];
    int f = span > 0 ? anim_ease(st->anim_timing, (x - kf->off[a]) * 1000 / span) : 0;
    for (int fr = 0; fr < 2; fr++) {
        int me = fr ? b : a, other = fr ? a : b;
        for (int i = 0; i < kf->nd[me]; i++) {
            const char* prop = kf->decls[me][i].prop;
            if (fr && kf_value(kf, other, prop)) continue;   /* done with the pair */
            const char* va = kf_value(kf, a, prop), *vb = kf_value(kf, b, prop);
            const char* v = NULL;
            if (va && vb) {
                v = a == b ? va : anim_mix(A, va, vb, f);
                if (!v) v = f < 500 ? va : vb;
            } else v = va ? (f < 500 ? va : NULL) : (f >= 500 ? vb : NULL);
            if (v && !anim_decl(st, prop, v)) apply_decl(st, parent, prop, v);
        }
    }
}
