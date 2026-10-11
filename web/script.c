#include "script_int.h"
#include "kstring.h"
#include "kheap.h"

/*
 * BananaScript core: values, the lexer and parser for both languages, and
 * the tree-walking interpreter. Built-in functions are in script_lib.c.
 */

void (*script_clock)(script_tm_t* tm) = 0;

/* ══ values ═══════════════════════════════════════════════════════════ */

value_t v_undef(void) { value_t v; memset(&v, 0, sizeof(v)); v.t = V_UNDEF; return v; }
value_t v_null(void)  { value_t v; memset(&v, 0, sizeof(v)); v.t = V_NULL; return v; }
value_t v_bool(int b) { value_t v; memset(&v, 0, sizeof(v)); v.t = V_BOOL; v.b = b ? 1 : 0; return v; }
value_t v_num(num_t n) { value_t v; memset(&v, 0, sizeof(v)); v.t = V_NUM; v.n = n; return v; }
value_t v_obj(obj_t* o) { value_t v; memset(&v, 0, sizeof(v)); v.t = V_OBJ; v.o = o; return v; }
value_t v_strv(str_t* s) { value_t v; memset(&v, 0, sizeof(v)); v.t = V_STR; v.s = s; return v; }

str_t* str_new(interp_t* I, const char* s, uint32_t n) {
    str_t* r = (str_t*)arena_alloc(I->A, (uint32_t)sizeof(str_t) + n + 1);
    if (I->A->oom) { r->len = 0; r->s[0] = 0; return r; }
    r->len = n;
    if (n) memcpy(r->s, s, n);
    r->s[n] = 0;
    return r;
}

str_t* str_cat(interp_t* I, str_t* a, str_t* b) {
    if (!a->len) return b;
    if (!b->len) return a;
    str_t* r = (str_t*)arena_alloc(I->A, (uint32_t)sizeof(str_t) + a->len + b->len + 1);
    if (I->A->oom) { r->len = 0; r->s[0] = 0; return r; }
    r->len = a->len + b->len;
    memcpy(r->s, a->s, a->len);
    memcpy(r->s + a->len, b->s, b->len);
    r->s[r->len] = 0;
    return r;
}

value_t v_str(interp_t* I, const char* s) { return v_strv(str_new(I, s, (uint32_t)strlen(s))); }
value_t v_strn(interp_t* I, const char* s, uint32_t n) { return v_strv(str_new(I, s, n)); }

int v_isfunc(value_t v) { return v.t == V_FUNC; }

value_t v_native(interp_t* I, const char* name, native_fn f) {
    func_t* fn = (func_t*)arena_alloc(I->A, sizeof(func_t));
    fn->native = 1;
    fn->nf = f;
    fn->name = name;
    value_t v = v_undef();
    v.t = V_FUNC;
    v.f = fn;
    return v;
}

/* ── numbers ── */

int num_isnan(num_t x) { return x != x; }

num_t num_floor(num_t x) {
    if (num_isnan(x)) return x;
    if (x >= 4.0e18L || x <= -4.0e18L) return x;
    int64_t i = (int64_t)x;
    if ((num_t)i > x) i--;
    return (num_t)i;
}

static num_t num_inf(void) { num_t one = 1.0L, zero = 0.0L; return one / zero; }

/* 15 significant digits, shortest form; JS-style exponent outside 1e-7..1e21 */
void num_format(num_t x, char* out, int cap) {
    if (cap < 2) return;
    if (num_isnan(x)) { kstrlcpy(out, "NaN", (size_t)cap); return; }
    if (x == num_inf()) { kstrlcpy(out, "Infinity", (size_t)cap); return; }
    if (x == -num_inf()) { kstrlcpy(out, "-Infinity", (size_t)cap); return; }
    if (x == 0) { kstrlcpy(out, "0", (size_t)cap); return; }
    char buf[64];
    int n = 0;
    if (x < 0) { buf[n++] = '-'; x = -x; }
    if (x < 1.0e15L && num_floor(x) == x) {
        char d[24];
        int k = 0;
        uint64_t u = (uint64_t)x;
        do { d[k++] = (char)('0' + (int)(u % 10)); u /= 10; } while (u);
        while (k) buf[n++] = d[--k];
        buf[n] = 0;
        kstrlcpy(out, buf, (size_t)cap);
        return;
    }
    int e = 0;
    num_t m = x;
    while (m >= 10.0L) { m /= 10.0L; e++; }
    while (m < 1.0L) { m *= 10.0L; e--; }
    char dg[20];
    for (int i = 0; i < 16; i++) {
        int d = (int)m;
        if (d > 9) d = 9;
        dg[i] = (char)('0' + d);
        m = (m - d) * 10.0L;
    }
    /* round to 15 digits */
    int nd = 15;
    if (dg[15] >= '5') {
        int i = 14;
        while (i >= 0) {
            if (dg[i] == '9') { dg[i] = '0'; i--; }
            else { dg[i]++; break; }
        }
        if (i < 0) { for (int j = 14; j > 0; j--) dg[j] = dg[j - 1]; dg[0] = '1'; e++; }
    }
    while (nd > 1 && dg[nd - 1] == '0') nd--;
    if (e >= 21 || e < -7) {
        buf[n++] = dg[0];
        if (nd > 1) { buf[n++] = '.'; for (int i = 1; i < nd; i++) buf[n++] = dg[i]; }
        buf[n++] = 'e';
        buf[n++] = e < 0 ? '-' : '+';
        int ae = e < 0 ? -e : e;
        if (ae >= 100) buf[n++] = (char)('0' + ae / 100);
        if (ae >= 10) buf[n++] = (char)('0' + (ae / 10) % 10);
        buf[n++] = (char)('0' + ae % 10);
    } else if (e < 0) {
        buf[n++] = '0';
        buf[n++] = '.';
        for (int i = -1; i > e; i--) buf[n++] = '0';
        for (int i = 0; i < nd; i++) buf[n++] = dg[i];
    } else {
        for (int i = 0; i <= e; i++) buf[n++] = i < nd ? dg[i] : '0';
        if (nd > e + 1) {
            buf[n++] = '.';
            for (int i = e + 1; i < nd; i++) buf[n++] = dg[i];
        }
    }
    buf[n] = 0;
    kstrlcpy(out, buf, (size_t)cap);
}

static int is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }
static int is_digit(char c) { return c >= '0' && c <= '9'; }

/* JS Number("...") / PHP numeric prefix */
num_t str_tonum(const char* s, int php) {
    while (is_space(*s)) s++;
    if (!*s) return 0;
    int neg = 0;
    if (*s == '-' || *s == '+') { neg = (*s == '-'); s++; }
    if (!php && strncmp(s, "Infinity", 8) == 0) return neg ? -num_inf() : num_inf();
    num_t v = 0;
    int any = 0;
    if (!php && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
        while (*s) {
            int d = is_digit(*s) ? *s - '0' : (*s >= 'a' && *s <= 'f') ? *s - 'a' + 10 :
                    (*s >= 'A' && *s <= 'F') ? *s - 'A' + 10 : -1;
            if (d < 0) break;
            v = v * 16 + d;
            s++;
            any = 1;
        }
    } else {
        while (is_digit(*s)) { v = v * 10 + (*s - '0'); s++; any = 1; }
        if (*s == '.') {
            s++;
            num_t f = 0.1L;
            while (is_digit(*s)) { v += (*s - '0') * f; f /= 10; s++; any = 1; }
        }
        if (any && (*s == 'e' || *s == 'E')) {
            const char* save = s;
            s++;
            int eneg = 0, ev = 0, edig = 0;
            if (*s == '-' || *s == '+') { eneg = (*s == '-'); s++; }
            while (is_digit(*s)) { ev = ev * 10 + (*s - '0'); s++; edig = 1; if (ev > 5000) ev = 5000; }
            if (!edig) s = save;
            else while (ev--) v = eneg ? v / 10 : v * 10;
        }
    }
    if (!any) {
        num_t z = 0;
        return php ? 0 : z / z;          /* NaN */
    }
    if (!php) {
        while (is_space(*s)) s++;
        if (*s) { num_t z = 0; return z / z; }
    }
    return neg ? -v : v;
}

/* ── conversions ── */

static value_t join_array(interp_t* I, obj_t* a, const char* sep);

str_t* v_tostr(interp_t* I, value_t v) {
    char buf[64];
    switch (v.t) {
    case V_UNDEF: return str_new(I, I->lang == LANG_PHP ? "" : "undefined", I->lang == LANG_PHP ? 0 : 9);
    case V_NULL:  return str_new(I, I->lang == LANG_PHP ? "" : "null", I->lang == LANG_PHP ? 0 : 4);
    case V_BOOL:
        if (I->lang == LANG_PHP) return str_new(I, v.b ? "1" : "", v.b ? 1 : 0);
        return str_new(I, v.b ? "true" : "false", v.b ? 4 : 5);
    case V_NUM:
        num_format(v.n, buf, sizeof(buf));
        return str_new(I, buf, (uint32_t)strlen(buf));
    case V_STR: return v.s;
    case V_FUNC: return str_new(I, "function", 8);
    case V_OBJ: {
        obj_t* o = v.o;
        if (o->kind == OBJ_BIGINT) return bi_tostr(I, v);
        if (o->kind == OBJ_ARRAY) return join_array(I, o, ",").s;
        if (o->kind == OBJ_PHPARRAY) return str_new(I, "Array", 5);
        if (o->kind == OBJ_HOST) {
            int found = 0;
            if (o->hc && o->hc->get) {
                value_t t = o->hc->get(I, o, "__tostring", &found);
                if (found) return v_tostr(I, t);
            }
            ksnprintf(buf, sizeof(buf), "[object %s]", o->hc ? o->hc->name : "Object");
            return str_new(I, buf, (uint32_t)strlen(buf));
        }
        if (I->lang != LANG_PHP) {
            /* a script-defined toString (class methods, URL...), or Date's own */
            value_t ts = obj_get(I, o, "toString");
            if (ts.t == V_FUNC && (!ts.f->native || o->proto == I->proto_date)) {
                value_t r = call_value(I, ts, v, 0, NULL);
                if (r.t == V_STR) return r.s;
            }
        }
        int f1 = 0, f2 = 0;
        value_t nm = prop_get_raw(o, "name", &f1), msg = prop_get_raw(o, "message", &f2);
        if (f1 && f2) {
            str_t* a = v_tostr(I, nm);
            str_t* m = v_tostr(I, msg);
            return str_cat(I, str_cat(I, a, str_new(I, ": ", 2)), m);
        }
        return str_new(I, "[object Object]", 15);
    }
    }
    return str_new(I, "", 0);
}

const char* v_cstr(interp_t* I, value_t v) { return v_tostr(I, v)->s; }

num_t v_tonum(interp_t* I, value_t v) {
    num_t z = 0;
    switch (v.t) {
    case V_UNDEF: return I->lang == LANG_PHP ? 0 : z / z;
    case V_NULL:  return 0;
    case V_BOOL:  return v.b;
    case V_NUM:   return v.n;
    case V_STR:   return str_tonum(v.s->s, I->lang == LANG_PHP);
    case V_OBJ:
        if (v.o->kind == OBJ_BIGINT) return bi_tonum(v);
        if (v.o->kind == OBJ_PHPARRAY) return v.o->n ? 1 : 0;
        if (v.o->kind == OBJ_ARRAY) {
            if (v.o->len == 0) return 0;
            if (v.o->len == 1) return v_tonum(I, v.o->items[0]);
        }
        return z / z;
    default: return z / z;
    }
}

int v_truthy(interp_t* I, value_t v) {
    switch (v.t) {
    case V_UNDEF: case V_NULL: return 0;
    case V_BOOL: return v.b;
    case V_NUM:  return v.n != 0 && !num_isnan(v.n);
    case V_STR:
        if (I->lang == LANG_PHP && v.s->len == 1 && v.s->s[0] == '0') return 0;
        return v.s->len > 0;
    case V_OBJ:
        if (I->lang == LANG_PHP && v.o->kind == OBJ_PHPARRAY) return v.o->n > 0;
        if (v.o->kind == OBJ_BIGINT) return !bi_zero(v);
        return 1;
    default: return 1;
    }
}

int v_strict_eq(value_t a, value_t b) {
    if (a.t != b.t) return 0;
    switch (a.t) {
    case V_UNDEF: case V_NULL: return 1;
    case V_BOOL: return a.b == b.b;
    case V_NUM:  return a.n == b.n;
    case V_STR:  return a.s->len == b.s->len && memcmp(a.s->s, b.s->s, a.s->len) == 0;
    case V_OBJ:  return a.o == b.o || (a.o->kind == OBJ_BIGINT && b.o->kind == OBJ_BIGINT && bi_eq(a, b));
    case V_FUNC: return a.f == b.f;
    }
    return 0;
}

static int str_is_numeric(const char* s) {
    num_t v = str_tonum(s, 0);
    while (is_space(*s)) s++;
    return *s && !num_isnan(v);
}

int v_loose_eq(interp_t* I, value_t a, value_t b) {
    if (a.t == b.t) {
        if (I->lang == LANG_PHP && a.t == V_STR && str_is_numeric(a.s->s) && str_is_numeric(b.s->s))
            return str_tonum(a.s->s, 1) == str_tonum(b.s->s, 1);
        return v_strict_eq(a, b);
    }
    int an = (a.t == V_UNDEF || a.t == V_NULL), bn = (b.t == V_UNDEF || b.t == V_NULL);
    if (I->lang == LANG_PHP) {
        if (an || bn || a.t == V_BOOL || b.t == V_BOOL) return v_truthy(I, a) == v_truthy(I, b);
        if (a.t == V_NUM || b.t == V_NUM) return v_tonum(I, a) == v_tonum(I, b);
        return v_strict_eq(v_strv(v_tostr(I, a)), v_strv(v_tostr(I, b)));
    }
    if (an || bn) return an && bn;
    if (a.t == V_OBJ || a.t == V_FUNC || b.t == V_OBJ || b.t == V_FUNC) {
        if ((a.t == V_OBJ || a.t == V_FUNC) && (b.t == V_OBJ || b.t == V_FUNC)) return 0;
        return v_strict_eq(v_strv(v_tostr(I, a)), v_strv(v_tostr(I, b)));
    }
    return v_tonum(I, a) == v_tonum(I, b);
}

/* ══ objects ══════════════════════════════════════════════════════════ */

obj_t* obj_new(interp_t* I, int kind) {
    obj_t* o = (obj_t*)arena_alloc(I->A, sizeof(obj_t));
    o->kind = (uint8_t)kind;
    return o;
}

/* Object.defineProperty(o, k, {value}) - enumerable left out, so false -
 * keeps k out of Object.keys, for-in, JSON and spreads: such names are
 * listed in a hidden array on the object. Names starting \x01 are the
 * engine's own (Map contents, that list) and never show. */
obj_t* prop_hidden_names(obj_t* o) {
    int f = 0;
    value_t v = prop_get_raw(o, "\x01ne", &f);
    return f && v.t == V_OBJ ? v.o : NULL;
}

int prop_enumerable(obj_t* o, obj_t* hidden, uint32_t i) {
    const char* k = o->props[i].key->s;
    if (k[0] == '\x01') return 0;
    if (hidden)
        for (uint32_t j = 0; j < hidden->len; j++)
            if (hidden->items[j].t == V_STR && strcmp(hidden->items[j].s->s, k) == 0) return 0;
    return 1;
}

void prop_set_enumerable(interp_t* I, obj_t* o, const char* key, int on) {
    obj_t* h = prop_hidden_names(o);
    for (uint32_t j = 0; h && j < h->len; j++) {
        if (h->items[j].t != V_STR || strcmp(h->items[j].s->s, key) != 0) continue;
        if (on) {
            for (uint32_t k = j + 1; k < h->len; k++) h->items[k - 1] = h->items[k];
            h->len--;
        }
        return;
    }
    if (on) return;
    if (!h) {
        h = obj_new(I, OBJ_ARRAY);
        prop_set_raw(I, o, str_new(I, "\x01ne", 3), v_obj(h));
    }
    arr_push(I, h, v_str(I, key));
}

/* buffers an array or object outgrew are reused (by power-of-two capacity)
 * instead of being left in the arena: a script growing many arrays would
 * otherwise waste as much again as it uses */
static int pow2_class(uint32_t cap) {
    if (cap < 4 || (cap & (cap - 1))) return -1;
    int b = 0;
    while ((1u << b) < cap) b++;
    return b < 24 ? b : -1;
}
static void* buf_get(interp_t* I, void** lists, uint32_t cap, uint32_t size) {
    int b = pow2_class(cap);
    if (b >= 0 && lists[b]) {
        void* p = lists[b];
        lists[b] = *(void**)p;
        memset(p, 0, (size_t)cap * size);
        return p;
    }
    return arena_alloc(I->A, cap * size);
}
static void buf_put(interp_t* I, void** lists, void* p, uint32_t cap) {
    int b = pow2_class(cap);
    if (!p || b < 0 || I->A->oom) return;           /* after oom buffers may be the shared fallback */
    *(void**)p = lists[b];
    lists[b] = p;
}

value_t prop_get_raw(obj_t* o, const char* key, int* found) {
    for (uint32_t i = 0; i < o->n; i++)
        if (strcmp(o->props[i].key->s, key) == 0) { *found = 1; return o->props[i].v; }
    *found = 0;
    return v_undef();
}

void prop_set_raw(interp_t* I, obj_t* o, str_t* key, value_t v) {
    for (uint32_t i = 0; i < o->n; i++)
        if (o->props[i].key->len == key->len && memcmp(o->props[i].key->s, key->s, key->len) == 0) {
            o->props[i].v = v;
            return;
        }
    if (o->n == o->cap) {
        uint32_t nc = o->cap ? o->cap * 2 : 4;   /* most objects are small */
        prop_t* np = (prop_t*)buf_get(I, I->free_props, nc, (uint32_t)sizeof(prop_t));
        if (I->A->oom) return;
        if (o->n) memcpy(np, o->props, o->n * sizeof(prop_t));
        buf_put(I, I->free_props, o->props, o->cap);
        o->props = np;
        o->cap = nc;
    }
    o->props[o->n].key = key;
    o->props[o->n].v = v;
    o->n++;
}

int prop_del(obj_t* o, const char* key) {
    for (uint32_t i = 0; i < o->n; i++)
        if (strcmp(o->props[i].key->s, key) == 0) {
            memmove(&o->props[i], &o->props[i + 1], (o->n - i - 1) * sizeof(prop_t));
            o->n--;
            return 1;
        }
    return 0;
}

value_t arr_get(obj_t* a, uint32_t i) {
    return i < a->len ? a->items[i] : v_undef();
}

void arr_set(interp_t* I, obj_t* a, uint32_t i, value_t v) {
    if (i > 10000000u) return;
    if (i >= a->acap) {
        uint32_t nc = a->acap ? a->acap : 4;
        while (nc <= i) nc *= 2;
        value_t* ni = (value_t*)buf_get(I, I->free_items, nc, (uint32_t)sizeof(value_t));
        if (I->A->oom) return;
        if (a->len) memcpy(ni, a->items, a->len * sizeof(value_t));
        buf_put(I, I->free_items, a->items, a->acap);
        a->items = ni;
        a->acap = nc;
    }
    for (uint32_t k = a->len; k < i; k++) a->items[k] = v_undef();
    a->items[i] = v;
    if (i >= a->len) a->len = i + 1;
}

void arr_push(interp_t* I, obj_t* a, value_t v) {
    if (a->kind == OBJ_PHPARRAY) { php_array_push(I, a, v); return; }
    arr_set(I, a, a->len, v);
}

/* "12" -> 12 if the key is a canonical array index */
static int key_index(const char* k, uint32_t* out) {
    if (!*k || (k[0] == '0' && k[1])) return 0;
    uint32_t v = 0;
    for (; *k; k++) {
        if (!is_digit(*k)) return 0;
        if (v > 100000000u) return 0;
        v = v * 10 + (uint32_t)(*k - '0');
    }
    *out = v;
    return 1;
}

value_t obj_get(interp_t* I, obj_t* o, const char* key) {
    int found = 0;
    if (o->kind == OBJ_HOST && o->hc && o->hc->get) {
        value_t v = o->hc->get(I, o, key, &found);
        if (found) return v;
    }
    if (o->kind == OBJ_ARRAY) {
        uint32_t idx;
        if (key_index(key, &idx)) return arr_get(o, idx);
        if (strcmp(key, "length") == 0) return v_num(o->len);
    }
    value_t v = prop_get_raw(o, key, &found);
    if (found) return (v.t == V_OBJ && v.o->kind == OBJ_ACCESSOR) ? accessor_get(I, v, v_obj(o)) : v;
    for (obj_t* p = o->proto; p; p = p->proto) {
        if (p->kind == OBJ_ARRAY) {                 /* Object.create(array): its elements and length show through */
            uint32_t idx;
            if (key_index(key, &idx)) return arr_get(p, idx);
            if (strcmp(key, "length") == 0) return v_num(p->len);
        }
        v = prop_get_raw(p, key, &found);
        if (found) return (v.t == V_OBJ && v.o->kind == OBJ_ACCESSOR) ? accessor_get(I, v, v_obj(o)) : v;
    }
    return v_undef();
}

/* a getter's result (undefined without one) */
value_t accessor_get(interp_t* I, value_t acc, value_t self) {
    int f = 0;
    value_t g = prop_get_raw(acc.o, "get", &f);
    if (!f || g.t != V_FUNC) return v_undef();
    return call_value(I, g, self, 0, NULL);
}

void obj_set(interp_t* I, obj_t* o, const char* key, value_t v) {
    if (o->kind == OBJ_HOST && o->hc && o->hc->set && o->hc->set(I, o, key, v)) return;
    if (o->kind == OBJ_ARRAY) {
        uint32_t idx;
        if (key_index(key, &idx)) { arr_set(I, o, idx, v); return; }
        if (strcmp(key, "length") == 0) {
            num_t n = v_tonum(I, v);
            if (n >= 0 && n < 10000000) {
                uint32_t nl = (uint32_t)n;
                if (nl < o->len) o->len = nl;
                else if (nl > o->len) arr_set(I, o, nl - 1, v_undef());
            }
            return;
        }
    }
    /* a setter (own or inherited) takes the assignment */
    if (!(v.t == V_OBJ && v.o->kind == OBJ_ACCESSOR)) {
        for (obj_t* p = o; p; p = p->proto) {
            int f = 0;
            value_t a = prop_get_raw(p, key, &f);
            if (!f) continue;
            if (a.t == V_OBJ && a.o->kind == OBJ_ACCESSOR) {
                value_t s = prop_get_raw(a.o, "set", &f);
                if (f && s.t == V_FUNC) call_value(I, s, v_obj(o), 1, &v);
                return;
            }
            break;
        }
    }
    prop_set_raw(I, o, str_new(I, key, (uint32_t)strlen(key)), v);
}

/* ── PHP arrays: ordered maps, integer keys as decimal strings ── */

static str_t* php_key(interp_t* I, value_t k, int64_t* as_int, int* is_int) {
    *is_int = 0;
    if (k.t == V_NUM || k.t == V_BOOL || k.t == V_NULL) {
        int64_t i = k.t == V_NUM ? (int64_t)k.n : k.t == V_BOOL ? k.b : 0;
        if (k.t == V_NULL) return str_new(I, "", 0);
        char b[24];
        ksnprintf(b, sizeof(b), "%lld", (long long)i);
        *as_int = i;
        *is_int = 1;
        return str_new(I, b, (uint32_t)strlen(b));
    }
    str_t* s = v_tostr(I, k);
    uint32_t idx;
    if (key_index(s->s, &idx)) { *as_int = idx; *is_int = 1; }
    return s;
}

void php_array_set(interp_t* I, obj_t* o, value_t key, value_t v) {
    int64_t ik = 0;
    int is_int;
    str_t* k = php_key(I, key, &ik, &is_int);
    if (is_int && ik >= o->next_index) o->next_index = ik + 1;
    prop_set_raw(I, o, k, v);
}

void php_array_push(interp_t* I, obj_t* o, value_t v) {
    php_array_set(I, o, v_num((num_t)o->next_index), v);
}

value_t php_array_get(interp_t* I, obj_t* o, value_t key, int* found) {
    int64_t ik;
    int is_int;
    str_t* k = php_key(I, key, &ik, &is_int);
    return prop_get_raw(o, k->s, found);
}

uint32_t php_array_count(obj_t* o) { return o->n; }

value_t php_array_copy(interp_t* I, value_t v) {
    if (v.t != V_OBJ || v.o->kind != OBJ_PHPARRAY) return v;
    obj_t* c = obj_new(I, OBJ_PHPARRAY);
    c->next_index = v.o->next_index;
    for (uint32_t i = 0; i < v.o->n; i++) prop_set_raw(I, c, v.o->props[i].key, v.o->props[i].v);
    return v_obj(c);
}

static value_t join_array(interp_t* I, obj_t* a, const char* sep) {
    str_t* out = str_new(I, "", 0);
    str_t* s = str_new(I, sep, (uint32_t)strlen(sep));
    for (uint32_t i = 0; i < a->len; i++) {
        if (i) out = str_cat(I, out, s);
        value_t e = a->items[i];
        if (e.t != V_UNDEF && e.t != V_NULL) out = str_cat(I, out, v_tostr(I, e));
        if (I->A->oom) break;
    }
    return v_strv(out);
}

/* ══ errors ═══════════════════════════════════════════════════════════ */

void script_throw(interp_t* I, const char* msg) {
    if (I->ctl == CTL_THROW) return;
    if (I->A->oom && I->oom_err) {
        I->ret = v_obj(I->oom_err);
        I->ctl = CTL_THROW;
        I->throw_line = I->line; I->throw_col = I->col; I->throw_src = I->src_name;
        return;
    }
    obj_t* e = obj_new(I, OBJ_PLAIN);
    const char* colon = strchr(msg, ':');
    if (colon && colon - msg < 20 && colon[1] == ' ') {
        obj_set(I, e, "name", v_strn(I, msg, (uint32_t)(colon - msg)));
        obj_set(I, e, "message", v_str(I, colon + 2));
    } else {
        obj_set(I, e, "name", v_str(I, "Error"));
        obj_set(I, e, "message", v_str(I, msg));
    }
    I->ret = v_obj(e);
    I->ctl = CTL_THROW;
    I->throw_line = I->line; I->throw_col = I->col; I->throw_src = I->src_name;
}

static void throwf(interp_t* I, const char* fmt, const char* a) {
    char buf[160];
    ksnprintf(buf, sizeof(buf), fmt, a);
    script_throw(I, buf);
}

/* ══ lexer ════════════════════════════════════════════════════════════ */

enum { T_EOF = 0, T_NUM, T_STR, T_TPL, T_ID, T_VAR, T_OP, T_HTML, T_ECHOTAG, T_REGEX };

typedef struct {
    uint8_t     t;
    uint32_t    col;         /* column (errors in minified code), capped */
    int         line;
    const char* p;
    uint32_t    len;
    num_t       n;
    str_t*      s;
} tok_t;

typedef struct {
    interp_t*   I;
    const char* src;
    uint32_t    len, pos;
    int         line;
    const char* line_start;
    int         html;        /* PHP: outside <?php ?> */
    tok_t*      toks;
    uint32_t    ntok, cap;
    const char* err;
    int         err_line;
} lexer_t;

static const char* const OPS[] = {
    ">>>=", ">>>", "||=", "&&=",
    "===", "!==", "**=", "?\?=", "...", "<=>", "<<=", ">>=",
    "==", "!=", "<=", ">=", "&&", "||", "??", "++", "--", "+=", "-=", "*=", "/=", "%=",
    ".=", "=>", "->", "::", "**", "<<", ">>", "?.", "|=", "&=", "^=", "<>",
    "{", "}", "(", ")", "[", "]", ";", ",", ".", "<", ">", "+", "-", "*", "/", "%",
    "=", "!", "?", ":", "&", "|", "^", "~", "@",
};

static tok_t* lex_push(lexer_t* L, int t, const char* p, uint32_t len) {
    if (L->ntok == L->cap) {
        /* tokens live only while parsing: on the heap, freed right after */
        uint32_t nc = L->cap ? L->cap * 2 : 256;
        tok_t* nt = (tok_t*)krealloc(L->toks, nc * (uint32_t)sizeof(tok_t));
        if (!nt) { L->err = "out of memory"; return NULL; }
        L->toks = nt;
        L->cap = nc;
    }
    tok_t* k = &L->toks[L->ntok++];
    memset(k, 0, sizeof(*k));
    k->t = (uint8_t)t;
    k->p = p;
    k->len = len;
    k->line = L->line;
    if (L->line_start && p >= L->line_start && p <= L->src + L->len)
        k->col = (uint32_t)(p - L->line_start + 1 > 0xFFFFF ? 0xFFFFF : p - L->line_start + 1);
    return k;
}

static int id_start(int c, int php) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || (!php && c == '$') || c >= 0x80;
}
static int id_char(int c, int php) { return id_start(c, php) || is_digit((char)c); }

static void count_lines(lexer_t* L, const char* a, const char* b) {
    for (; a < b; a++) if (*a == '\n') { L->line++; L->line_start = a + 1; }
}

/* JS-style string escapes into a new string */
static str_t* unescape(interp_t* I, const char* s, uint32_t n, int php_single) {
    char* buf = (char*)arena_alloc(I->A, n + 1);
    uint32_t o = 0;
    for (uint32_t i = 0; i < n; i++) {
        char c = s[i];
        if (c != '\\' || i + 1 >= n) { buf[o++] = c; continue; }
        char e = s[++i];
        if (php_single) {
            if (e == '\\' || e == '\'') buf[o++] = e;
            else { buf[o++] = '\\'; buf[o++] = e; }
            continue;
        }
        switch (e) {
        case 'n': buf[o++] = '\n'; break;
        case 't': buf[o++] = '\t'; break;
        case 'r': buf[o++] = '\r'; break;
        case '0': buf[o++] = '\0'; break;
        case 'b': buf[o++] = '\b'; break;
        case 'f': buf[o++] = '\f'; break;
        case 'v': buf[o++] = '\v'; break;
        case 'x':
            if (i + 2 < n) {
                int v = 0;
                for (int k = 1; k <= 2; k++) {
                    char h = s[i + k];
                    v = v * 16 + (is_digit(h) ? h - '0' : ((h | 32) - 'a' + 10));
                }
                buf[o++] = (char)v;
                i += 2;
            }
            break;
        case 'u':
            if (i + 1 < n && s[i + 1] == '{') {          /* \u{1F600} */
                uint32_t v = 0, k = i + 2;
                while (k < n && s[k] != '}') {
                    char h = s[k++];
                    v = v * 16 + (uint32_t)(is_digit(h) ? h - '0' : ((h | 32) - 'a' + 10));
                }
                i = k;
                if (v < 0x80) buf[o++] = (char)v;
                else if (v < 0x800) { buf[o++] = (char)(0xC0 | (v >> 6)); buf[o++] = (char)(0x80 | (v & 63)); }
                else if (v < 0x10000) { buf[o++] = (char)(0xE0 | (v >> 12)); buf[o++] = (char)(0x80 | ((v >> 6) & 63)); buf[o++] = (char)(0x80 | (v & 63)); }
                else { buf[o++] = (char)(0xF0 | (v >> 18)); buf[o++] = (char)(0x80 | ((v >> 12) & 63)); buf[o++] = (char)(0x80 | ((v >> 6) & 63)); buf[o++] = (char)(0x80 | (v & 63)); }
                break;
            }
            if (i + 4 < n) {
                int v = 0;
                for (int k = 1; k <= 4; k++) {
                    char h = s[i + k];
                    v = v * 16 + (is_digit(h) ? h - '0' : ((h | 32) - 'a' + 10));
                }
                i += 4;
                if (v < 0x80) buf[o++] = (char)v;
                else if (v < 0x800) { buf[o++] = (char)(0xC0 | (v >> 6)); buf[o++] = (char)(0x80 | (v & 63)); }
                else { buf[o++] = (char)(0xE0 | (v >> 12)); buf[o++] = (char)(0x80 | ((v >> 6) & 63)); buf[o++] = (char)(0x80 | (v & 63)); }
            }
            break;
        case '\n': break;                            /* line continuation */
        default: buf[o++] = e; break;
        }
    }
    return str_new(I, buf, o);
}

static void lex_html(lexer_t* L) {
    const char* s = L->src + L->pos;
    const char* end = L->src + L->len;
    const char* p = s;
    while (p < end) {
        if (p[0] == '<' && p + 1 < end && p[1] == '?') {
            if (p + 4 < end && strncasecmp(p + 2, "xml", 3) == 0) { p++; continue; }
            break;
        }
        p++;
    }
    if (p > s) {
        tok_t* t = lex_push(L, T_HTML, s, (uint32_t)(p - s));
        if (t) t->s = str_new(L->I, s, (uint32_t)(p - s));
        count_lines(L, s, p);
    }
    L->pos = (uint32_t)(p - L->src);
    if (p >= end) return;
    if (p + 2 < end && p[2] == '=') {
        lex_push(L, T_ECHOTAG, p, 3);
        L->pos += 3;
    } else if (p + 4 < end && strncasecmp(p + 2, "php", 3) == 0) {
        L->pos += 5;
    } else {
        L->pos += 2;
    }
    L->html = 0;
}

/* may a '/' here start a regular expression (rather than divide)? */
static int regex_ok(lexer_t* L) {
    if (!L->ntok) return 1;
    tok_t* t = &L->toks[L->ntok - 1];
    if (t->t == T_NUM || t->t == T_STR || t->t == T_TPL || t->t == T_REGEX || t->t == T_VAR) return 0;
    if (t->t == T_ID) {
        static const char* const kws[] = { "return", "typeof", "instanceof", "in", "of", "new", "delete", "void",
                                           "throw", "case", "do", "else", "yield", "await" };
        for (uint32_t i = 0; i < sizeof(kws) / sizeof(kws[0]); i++)
            if (t->len == strlen(kws[i]) && memcmp(t->p, kws[i], t->len) == 0) return 1;
        return 0;
    }
    if (t->t == T_OP) {
        if (t->len == 1 && (t->p[0] == ')' || t->p[0] == ']')) return 0;
        if (t->len == 2 && (memcmp(t->p, "++", 2) == 0 || memcmp(t->p, "--", 2) == 0)) return 0;
    }
    return 1;
}

/* one shared copy of each identifier name (big scripts repeat them a lot) */
static str_t* str_intern(interp_t* I, const char* s, uint32_t n) {
    if (I->icount * 2 >= I->icap) {
        uint32_t nc = I->icap ? I->icap * 2 : 1024;
        str_t** nt = (str_t**)arena_alloc(I->A, nc * (uint32_t)sizeof(str_t*));
        if (I->A->oom) return str_new(I, s, n);
        for (uint32_t i = 0; i < I->icap; i++) {
            str_t* e = I->itab[i];
            if (!e) continue;
            uint32_t h = 2166136261u;
            for (uint32_t k = 0; k < e->len; k++) h = (h ^ (unsigned char)e->s[k]) * 16777619u;
            uint32_t j = h & (nc - 1);
            while (nt[j]) j = (j + 1) & (nc - 1);
            nt[j] = e;
        }
        I->itab = nt;
        I->icap = nc;
    }
    uint32_t h = 2166136261u;
    for (uint32_t k = 0; k < n; k++) h = (h ^ (unsigned char)s[k]) * 16777619u;
    uint32_t j = h & (I->icap - 1);
    while (I->itab[j]) {
        str_t* e = I->itab[j];
        if (e->len == n && memcmp(e->s, s, n) == 0) return e;
        j = (j + 1) & (I->icap - 1);
    }
    str_t* e = str_new(I, s, n);
    I->itab[j] = e;
    I->icount++;
    return e;
}

/* template literals: given p just past an opening backtick, the closing one
 * (or end); `${ ... }` parts may hold strings, braces and templates of their own */
static const char* tpl_expr_end(const char* p, const char* end);
static const char* tpl_end(const char* p, const char* end) {
    while (p < end && *p != '`') {
        if (*p == '\\' && p + 1 < end) { p += 2; continue; }
        if (*p == '$' && p + 1 < end && p[1] == '{') {
            p = tpl_expr_end(p + 2, end);
            if (p < end) p++;                           /* the '}' */
            continue;
        }
        p++;
    }
    return p;
}
/* given p just past "${", the '}' that closes it (or end) */
static const char* tpl_expr_end(const char* p, const char* end) {
    int depth = 1;
    while (p < end) {
        char ch = *p;
        if (ch == '"' || ch == '\'') {
            p++;
            while (p < end && *p != ch && *p != '\n') { if (*p == '\\' && p + 1 < end) p++; p++; }
            if (p < end) p++;
            continue;
        }
        if (ch == '`') { p = tpl_end(p + 1, end); if (p < end) p++; continue; }
        if (ch == '/' && p + 1 < end && p[1] == '*') {
            p += 2;
            while (p + 1 < end && !(p[0] == '*' && p[1] == '/')) p++;
            p += 2;
            continue;
        }
        if (ch == '{') depth++;
        else if (ch == '}' && --depth == 0) return p;
        p++;
    }
    return end;
}

static void lex_all(lexer_t* L) {
    interp_t* I = L->I;
    int php = I->lang == LANG_PHP;
    while (!L->err) {
        if (L->html) {
            lex_html(L);
            if (L->pos >= L->len) break;
            continue;
        }
        if (L->pos >= L->len) break;
        const char* s = L->src + L->pos;
        const char* end = L->src + L->len;
        char c = *s;
        if (c == '\n') { L->line++; L->pos++; L->line_start = s + 1; continue; }
        if (is_space(c)) { L->pos++; continue; }
        /* comments */
        if (c == '/' && s + 1 < end && s[1] == '/') goto line_comment;
        if (php && c == '#') goto line_comment;
        if (c == '/' && s + 1 < end && s[1] == '*') {
            const char* p = s + 2;
            while (p + 1 < end && !(p[0] == '*' && p[1] == '/')) p++;
            count_lines(L, s, p);
            L->pos = (uint32_t)(p + 2 - L->src);
            if (L->pos > L->len) L->pos = L->len;
            continue;
        }
        if (php && c == '?' && s + 1 < end && s[1] == '>') {
            lex_push(L, T_OP, ";", 1);
            L->pos += 2;
            if (L->pos < L->len && L->src[L->pos] == '\n') { L->pos++; L->line++; }
            else if (L->pos + 1 < L->len && L->src[L->pos] == '\r' && L->src[L->pos + 1] == '\n') { L->pos += 2; L->line++; }
            L->html = 1;
            continue;
        }
        /* numbers */
        if (is_digit(c) || (c == '.' && s + 1 < end && is_digit(s[1]))) {
            const char* p = s;
            num_t radix_v = -1;
            if (c == '0' && s + 1 < end && (s[1] == 'x' || s[1] == 'X')) {
                p += 2;
                while (p < end && (is_digit(*p) || *p == '_' || ((*p | 32) >= 'a' && (*p | 32) <= 'f'))) p++;
            } else if (c == '0' && s + 1 < end && ((s[1] | 32) == 'b' || (s[1] | 32) == 'o')) {
                int base = (s[1] | 32) == 'b' ? 2 : 8;
                radix_v = 0;
                p += 2;
                while (p < end && ((*p >= '0' && *p < '0' + base) || *p == '_')) {
                    if (*p != '_') radix_v = radix_v * base + (*p - '0');
                    p++;
                }
            } else {
                int dot = 0;                                /* 1..toString(): the second dot is an operator */
                while (p < end && (is_digit(*p) || (*p == '.' && !dot++) || *p == '_')) p++;
                if (p < end && (*p == 'e' || *p == 'E')) {
                    p++;
                    if (p < end && (*p == '+' || *p == '-')) p++;
                    while (p < end && is_digit(*p)) p++;
                }
            }
            char tmp[64];
            uint32_t n = 0;
            for (const char* q = s; q < p && n < sizeof(tmp) - 1; q++) if (*q != '_') tmp[n++] = *q;
            tmp[n] = 0;
            int big = !php && p < end && *p == 'n';         /* BigInt literal: the n stays in the text */
            tok_t* t = lex_push(L, T_NUM, s, (uint32_t)(p - s) + (uint32_t)big);
            if (t) t->n = radix_v >= 0 ? radix_v : str_tonum(tmp, 0);
            if (big) p++;
            L->pos = (uint32_t)(p - L->src);
            continue;
        }
        /* strings */
        if (c == '"' || c == '\'' || (!php && c == '`')) {
            const char* p = s + 1;
            if (c == '`') p = tpl_end(p, end);
            else while (p < end && *p != c) {
                if (*p == '\\' && p + 1 < end) p++;
                p++;
            }
            if (p >= end) { L->err = "unterminated string"; L->err_line = L->line; break; }
            uint32_t n = (uint32_t)(p - s - 1);
            int tpl = (c == '`') || (php && c == '"');
            tok_t* t = lex_push(L, tpl ? T_TPL : T_STR, s + 1, n);
            if (t && !tpl) t->s = unescape(I, s + 1, n, php && c == '\'');
            count_lines(L, s, p);
            L->pos = (uint32_t)(p + 1 - L->src);
            continue;
        }
        /* PHP variables */
        if (php && c == '$' && s + 1 < end && id_start((unsigned char)s[1], 1)) {
            const char* p = s + 1;
            while (p < end && id_char((unsigned char)*p, 1)) p++;
            tok_t* t = lex_push(L, T_VAR, s + 1, (uint32_t)(p - s - 1));
            if (t) t->s = str_new(I, s + 1, (uint32_t)(p - s - 1));
            L->pos = (uint32_t)(p - L->src);
            continue;
        }
        /* class private names: #x */
        if (!php && c == '#' && s + 1 < end && id_start((unsigned char)s[1], 0)) {
            const char* p = s + 1;
            while (p < end && id_char((unsigned char)*p, 0)) p++;
            tok_t* t = lex_push(L, T_ID, s, (uint32_t)(p - s));
            if (t) t->s = str_new(I, s, (uint32_t)(p - s));
            L->pos = (uint32_t)(p - L->src);
            continue;
        }
        /* regular expression literals */
        if (!php && c == '/' && regex_ok(L)) {
            const char* p = s + 1;
            int cls = 0;
            while (p < end && *p != '\n') {
                if (*p == '\\' && p + 1 < end) { p += 2; continue; }
                if (*p == '[') cls = 1;
                else if (*p == ']') cls = 0;
                else if (*p == '/' && !cls) break;
                p++;
            }
            if (p < end && *p == '/') {
                const char* fs = p + 1;
                const char* fe = fs;
                while (fe < end && id_char((unsigned char)*fe, 0)) fe++;
                tok_t* t = lex_push(L, T_REGEX, s + 1, (uint32_t)(p - s - 1));
                if (t) t->s = str_new(I, fs, (uint32_t)(fe - fs));
                L->pos = (uint32_t)(fe - L->src);
                continue;
            }
        }
        if (id_start((unsigned char)c, php) || (!php && c == '\\' && s + 1 < end && s[1] == 'u')) {
            const char* p = s;
            int esc = 0;                                 /* abc: escapes in a name */
            while (p < end) {
                if (id_char((unsigned char)*p, php)) { p++; continue; }
                if (!php && *p == '\\' && p + 1 < end && p[1] == 'u') {
                    esc = 1;
                    p += 2;
                    if (p < end && *p == '{') { while (p < end && *p != '}') p++; if (p < end) p++; }
                    else p += 4;
                    if (p > end) p = end;
                    continue;
                }
                break;
            }
            tok_t* t = lex_push(L, T_ID, s, (uint32_t)(p - s));
            if (t) t->s = esc ? unescape(I, s, (uint32_t)(p - s), 0) : str_intern(I, s, (uint32_t)(p - s));
            L->pos = (uint32_t)(p - L->src);
            continue;
        }
        /* operators: longest match */
        int matched = 0;
        for (uint32_t i = 0; i < sizeof(OPS) / sizeof(OPS[0]); i++) {
            uint32_t ol = (uint32_t)strlen(OPS[i]);
            if ((uint32_t)(end - s) >= ol && memcmp(s, OPS[i], ol) == 0) {
                if (ol == 2 && s[0] == '?' && s[1] == '.' && s + 2 < end && is_digit(s[2])) continue;
                tok_t* ot = lex_push(L, T_OP, OPS[i], ol);  /* (p: the shared spelling) */
                if (ot && L->line_start && s >= L->line_start)
                    ot->col = (uint32_t)(s - L->line_start + 1 > 0xFFFFF ? 0xFFFFF : s - L->line_start + 1);
                L->pos += ol;
                matched = 1;
                break;
            }
        }
        if (!matched) { L->err = "unexpected character"; L->err_line = L->line; break; }
        continue;
line_comment:
        while (L->pos < L->len && L->src[L->pos] != '\n') {
            if (php && L->src[L->pos] == '?' && L->pos + 1 < L->len && L->src[L->pos + 1] == '>') break;
            L->pos++;
        }
    }
    lex_push(L, T_EOF, "", 0);
}

/* ══ parser ═══════════════════════════════════════════════════════════ */

typedef struct {
    interp_t* I;
    tok_t*    t;
    uint32_t  i, n;
    int       php;
    const char* err;
    int       err_line;
    const char* err_at;      /* where in the source */
    node_t*   hoist;         /* JS: collects the `var` names of the function being parsed */
    int       depth;
    int       no_in;        /* for (x in y): `in` is not an operator in the head */
} parser_t;

static node_t* parse_stmt(parser_t* P);
static node_t* parse_expr(parser_t* P);
static node_t* parse_assign(parser_t* P);
static node_t* parse_block(parser_t* P);
static node_t* parse_primary(parser_t* P);
static node_t* parse_postfix(parser_t* P);

static tok_t* pk(parser_t* P) { return &P->t[P->i]; }
static tok_t* pkn(parser_t* P, uint32_t k) { return &P->t[P->i + k < P->n ? P->i + k : P->n - 1]; }
static tok_t* nx(parser_t* P) { tok_t* t = &P->t[P->i]; if (P->i < P->n - 1) P->i++; return t; }

static int tok_is(tok_t* t, int type, const char* text) {
    if (t->t != type) return 0;
    uint32_t l = (uint32_t)strlen(text);
    return t->len == l && memcmp(t->p, text, l) == 0;
}
static int is_op(parser_t* P, const char* o) { return tok_is(pk(P), T_OP, o); }
static int is_kw(parser_t* P, tok_t* t, const char* k) {
    if (t->t != T_ID) return 0;
    uint32_t l = (uint32_t)strlen(k);
    if (t->len != l) return 0;
    return P->php ? strncasecmp(t->p, k, l) == 0 : memcmp(t->p, k, l) == 0;
}
static int kw(parser_t* P, const char* k) { return is_kw(P, pk(P), k); }

static void perr(parser_t* P, const char* msg) {
    if (P->err) return;
    P->err = msg;
    P->err_line = pk(P)->line;
    P->err_at = pk(P)->p;
}
static int accept(parser_t* P, const char* o) { if (is_op(P, o)) { nx(P); return 1; } return 0; }
static void expect(parser_t* P, const char* o) {
    if (!accept(P, o)) {
        static char msg[48];
        ksnprintf(msg, sizeof(msg), "expected '%s'", o);
        perr(P, msg);
    }
}

static node_t* mk(parser_t* P, int k) {
    node_t* n = (node_t*)arena_alloc(P->I->A, sizeof(node_t));
    n->k = (uint8_t)k;
    n->line = pk(P)->line;
    n->col = pk(P)->col;
    return n;
}

static void semi(parser_t* P) { accept(P, ";"); }

static str_t* ident(parser_t* P) {
    tok_t* t = pk(P);
    if (t->t != T_ID) { perr(P, "expected a name"); return str_new(P->I, "", 0); }
    nx(P);
    return t->s;
}

/* ── templates: `a ${b}` (JS) and "a $b {$c}" (PHP) ── */

static node_t* parse_sub(parser_t* P, const char* s, uint32_t n, int line);

static node_t* tpl_join(parser_t* P, node_t* acc, node_t* part) {
    if (!acc) return part;
    node_t* b = mk(P, N_BINARY);
    b->op = P->php ? OP_CONCAT : OP_ADD;
    b->a = acc;
    b->b = part;
    return b;
}

/* tag`a${x}b${y}c` is tag(["a","b","c"] (with .raw), x, y) */
static node_t* parse_tagged(parser_t* P, tok_t* t, node_t* tag) {
    const char* s = t->p;
    uint32_t n = t->len, lit = 0, i = 0;
    node_t* arr = mk(P, N_ARRAY);
    node_t** stail = &arr->a;
    node_t* raw = mk(P, N_ARRAY);                    /* the same pieces, escapes left as written */
    node_t** rtail = &raw->a;
    node_t* vals = NULL;
    node_t** vtail = &vals;
    while (i <= n) {
        int at_end = (i >= n);
        if (!at_end && s[i] == '\\') { i += 2; continue; }
        if (!at_end && !(s[i] == '$' && i + 1 < n && s[i + 1] == '{')) { i++; continue; }
        if (i > n) i = n;
        node_t* ln = mk(P, N_STR);
        ln->s = unescape(P->I, s + lit, i - lit, 0);
        *stail = ln;
        stail = &ln->next;
        node_t* rn = mk(P, N_STR);
        rn->s = str_new(P->I, s + lit, i - lit);
        *rtail = rn;
        rtail = &rn->next;
        if (at_end) break;
        uint32_t start = i + 2;
        uint32_t stop = (uint32_t)(tpl_expr_end(s + start, s + n) - s);
        node_t* e = parse_sub(P, s + start, stop - start, t->line);
        if (P->err) break;
        *vtail = e;
        while (*vtail) vtail = &(*vtail)->next;
        i = stop + 1;
        lit = i;
    }
    node_t* mark = mk(P, N_CALL);                    /* gives the strings array its .raw */
    mark->a = mk(P, N_IDENT);
    mark->a->s = str_new(P->I, "\x01tplstrings", 11);
    mark->b = arr;
    arr->next = raw;
    mark->next = vals;
    node_t* c = mk(P, N_CALL);
    c->a = tag;
    c->b = mark;
    return c;
}

static node_t* parse_template(parser_t* P, tok_t* t) {
    const char* s = t->p;
    uint32_t n = t->len;
    node_t* acc = mk(P, N_STR);
    acc->s = str_new(P->I, "", 0);
    uint32_t lit = 0, i = 0;
    while (i <= n) {
        int at_end = (i == n);
        int js_expr = !P->php && !at_end && s[i] == '$' && i + 1 < n && s[i + 1] == '{';
        int php_brace = P->php && !at_end && s[i] == '{' && i + 1 < n && s[i + 1] == '$';
        int php_var = P->php && !at_end && s[i] == '$' && i + 1 < n && id_start((unsigned char)s[i + 1], 1);
        if (!at_end && s[i] == '\\') { i += 2; continue; }
        if (!(at_end || js_expr || php_brace || php_var)) { i++; continue; }
        if (i > lit) {
            node_t* ln = mk(P, N_STR);
            ln->s = unescape(P->I, s + lit, i - lit, 0);
            acc = tpl_join(P, acc, ln);
        }
        if (at_end) break;
        uint32_t start, stop;
        if (js_expr || php_brace) {
            start = js_expr ? i + 2 : i + 1;
            uint32_t j = start;
            if (js_expr) j = (uint32_t)(tpl_expr_end(s + start, s + n) - s);
            else {
                int depth = 1;
                while (j < n && depth) {
                    if (s[j] == '{') depth++;
                    else if (s[j] == '}') depth--;
                    if (depth) j++;
                }
            }
            stop = j;
            i = j + 1;
        } else {
            start = i;
            uint32_t j = i + 1;
            while (j < n && id_char((unsigned char)s[j], 1)) j++;
            if (j < n && s[j] == '[') {             /* $a[0], $a[key] */
                uint32_t k = j + 1;
                while (k < n && s[k] != ']') k++;
                if (k < n) {
                    /* bare words inside are string keys in PHP strings */
                    int bare = id_start((unsigned char)s[j + 1], 1);
                    if (bare) {
                        node_t* var = mk(P, N_IDENT);
                        var->s = str_new(P->I, s + i + 1, j - i - 1);
                        node_t* key = mk(P, N_STR);
                        key->s = str_new(P->I, s + j + 1, k - j - 1);
                        node_t* ix = mk(P, N_INDEX);
                        ix->a = var;
                        ix->b = key;
                        acc = tpl_join(P, acc, ix);
                        i = k + 1;
                        lit = i;
                        continue;
                    }
                    j = k + 1;
                }
            }
            stop = j;
            i = j;
        }
        node_t* e = parse_sub(P, s + start, stop - start, t->line);
        if (P->err) return acc;
        acc = tpl_join(P, acc, e);
        lit = i;
    }
    return acc;
}

/* ── expressions ── */

static node_t* parse_args0(parser_t* P);
static node_t* parse_args(parser_t* P) {
    int ni = P->no_in;
    P->no_in = 0;
    node_t* r = parse_args0(P);
    P->no_in = ni;
    return r;
}

static node_t* parse_args0(parser_t* P) {
    node_t* head = NULL;
    node_t** tail = &head;
    if (accept(P, ")")) return NULL;
    for (;;) {
        node_t* e;
        if (!P->php && accept(P, "...")) { e = mk(P, N_SPREAD); e->a = parse_assign(P); }
        else e = parse_assign(P);
        if (P->err) return head;
        *tail = e;
        tail = &e->next;
        if (accept(P, ")")) break;
        expect(P, ",");
        if (P->err) return head;
        if (accept(P, ")")) break;              /* trailing comma */
    }
    return head;
}

static node_t* parse_params(parser_t* P) {
    node_t* head = NULL;
    node_t** tail = &head;
    expect(P, "(");
    while (!P->err && !accept(P, ")")) {
        node_t* p = mk(P, N_PARAM);
        if (P->php) {
            while (pk(P)->t == T_ID || is_op(P, "?")) nx(P);   /* type hints */
            accept(P, "&");
            if (pk(P)->t != T_VAR) { perr(P, "expected a $parameter"); return head; }
            p->s = nx(P)->s;
        } else {
            if (accept(P, "...")) p->op = 'r';                     /* rest: ...args */
            if (is_op(P, "[") || is_op(P, "{")) p->c = parse_primary(P);   /* destructured */
            else p->s = ident(P);
        }
        if (accept(P, "=")) p->a = parse_assign(P);
        *tail = p;
        tail = &p->next;
        if (!is_op(P, ")")) expect(P, ",");
    }
    if (P->php && accept(P, ":")) {                     /* return type */
        accept(P, "?");
        if (pk(P)->t == T_ID) nx(P);
    }
    return head;
}

/* a function body; its `var` names are declared first thing (N_VARHOIST) */
static node_t* parse_fn_body(parser_t* P) {
    node_t* saved = P->hoist;
    node_t* h = P->php ? NULL : mk(P, N_VARHOIST);
    P->hoist = h;
    node_t* b = parse_block(P);
    P->hoist = saved;
    if (h && h->a && b) { h->next = b->a; b->a = h; }
    return b;
}

static node_t* parse_function(parser_t* P, int want_name) {
    node_t* f = mk(P, N_FUNC);
    if (pk(P)->t == T_ID) f->s = nx(P)->s;
    else if (want_name) perr(P, "expected a function name");
    f->a = parse_params(P);
    if (P->php && kw(P, "use")) {                       /* closures: use ($x) - captured anyway */
        nx(P);
        expect(P, "(");
        while (!P->err && !accept(P, ")")) nx(P);
    }
    f->b = parse_fn_body(P);
    return f;
}

/* JS arrow function at the current position? */
static int arrow_ahead(parser_t* P) {
    tok_t* t = pk(P);
    if (t->t == T_ID && tok_is(pkn(P, 1), T_OP, "=>")) return 1;
    if (!tok_is(t, T_OP, "(")) return 0;
    int depth = 0;
    for (uint32_t k = 0; P->i + k < P->n; k++) {
        tok_t* q = pkn(P, k);
        if (q->t == T_EOF) return 0;
        if (tok_is(q, T_OP, "(")) depth++;
        else if (tok_is(q, T_OP, ")")) {
            if (--depth == 0) return tok_is(pkn(P, k + 1), T_OP, "=>");
        }
    }
    return 0;
}

static node_t* parse_arrow(parser_t* P) {
    node_t* f = mk(P, N_FUNC);
    if (pk(P)->t == T_ID) {
        node_t* p = mk(P, N_PARAM);
        p->s = nx(P)->s;
        f->a = p;
    } else {
        f->a = parse_params(P);
    }
    expect(P, "=>");
    f->c = (node_t*)1;                               /* marker: arrow */
    if (is_op(P, "{")) {
        f->b = parse_fn_body(P);
    } else {
        f->op = 1;                                   /* expression body */
        f->b = parse_assign(P);
    }
    return f;
}

static node_t* parse_array_lit(parser_t* P, const char* close) {
    node_t* a = mk(P, N_ARRAY);
    node_t** tail = &a->a;
    while (!P->err && !accept(P, close)) {
        node_t* item;
        if (!P->php && is_op(P, ",")) {                     /* a hole: [a, , b] */
            nx(P);
            item = mk(P, N_EMPTY);
            *tail = item;
            tail = &item->next;
            continue;
        }
        if (!P->php && accept(P, "...")) {
            item = mk(P, N_SPREAD);
            item->a = parse_assign(P);
        } else if (P->php) {
            item = mk(P, N_PAIR);
            node_t* first = parse_assign(P);
            if (accept(P, "=>")) { item->a = first; item->b = parse_assign(P); }
            else item->b = first;
        } else {
            item = parse_assign(P);
        }
        *tail = item;
        tail = &item->next;
        if (!is_op(P, close)) expect(P, ",");
    }
    return a;
}

static node_t* parse_object_lit(parser_t* P) {
    node_t* o = mk(P, N_OBJECT);
    node_t** tail = &o->a;
    while (!P->err && !accept(P, "}")) {
        node_t* pr = mk(P, N_PAIR);
        if (accept(P, "...")) {                             /* {...other} */
            node_t* sp = mk(P, N_SPREAD);
            sp->a = parse_assign(P);
            *tail = sp;
            tail = &sp->next;
            if (!is_op(P, "}")) expect(P, ",");
            continue;
        }
        int async = 0;
        /* get x() {}, set x(v) {}, async m() {}, *gen() {} */
        if (pk(P)->t == T_ID && (is_kw(P, pk(P), "get") || is_kw(P, pk(P), "set") || is_kw(P, pk(P), "async")) &&
            !tok_is(pkn(P, 1), T_OP, ":") && !tok_is(pkn(P, 1), T_OP, "(") && !tok_is(pkn(P, 1), T_OP, ",") &&
            !tok_is(pkn(P, 1), T_OP, "}") && !tok_is(pkn(P, 1), T_OP, "=")) {
            if (is_kw(P, pk(P), "async")) async = 1;
            else pr->op = (uint8_t)pk(P)->p[0];             /* 'g' / 's' */
            nx(P);
        }
        int gen_m = accept(P, "*");
        tok_t* t = pk(P);
        if (t->t == T_ID || t->t == T_STR) {
            pr->s = t->s;
            nx(P);
        } else if (t->t == T_NUM) {
            char b[32];
            num_format(t->n, b, sizeof(b));
            pr->s = str_new(P->I, b, (uint32_t)strlen(b));
            nx(P);
        } else if (accept(P, "[")) {
            pr->a = parse_assign(P);
            expect(P, "]");
        } else {
            perr(P, "bad object literal");
            return o;
        }
        if (accept(P, ":")) {
            pr->b = parse_assign(P);
        } else if (is_op(P, "(")) {                  /* method shorthand */
            node_t* f = mk(P, N_FUNC);
            f->s = pr->s;
            f->a = parse_params(P);
            f->b = parse_fn_body(P);
            if (async) f->n = 1;
            if (gen_m) f->flags |= NF_GEN;
            pr->b = f;
        } else {                                     /* {a} shorthand */
            node_t* id = mk(P, N_IDENT);
            id->s = pr->s;
            pr->b = id;
            if (is_op(P, "=")) {                     /* {a = 1} (a destructuring default) */
                nx(P);
                node_t* as = mk(P, N_ASSIGN);
                as->op = '=';
                as->a = id;
                as->b = parse_assign(P);
                pr->b = as;
            }
        }
        *tail = pr;
        tail = &pr->next;
        if (!is_op(P, "}")) expect(P, ",");
    }
    return o;
}

/* class Name extends Base { constructor() {} m() {} static s() {} get x() {} field = 1; } */
static node_t* parse_class(parser_t* P) {
    node_t* c = mk(P, N_CLASS);
    if (pk(P)->t == T_ID && !kw(P, "extends")) c->s = nx(P)->s;
    if (kw(P, "extends")) { nx(P); c->a = parse_postfix(P); }
    expect(P, "{");
    node_t** tail = &c->b;
    while (!P->err && !accept(P, "}")) {
        if (accept(P, ";")) continue;
        node_t* m = mk(P, N_PAIR);
        m->op = 'm';
        int async = 0;
        if (kw(P, "static") && !tok_is(pkn(P, 1), T_OP, "(") && !tok_is(pkn(P, 1), T_OP, "=")) {
            nx(P);
            m->n = 1;
            if (is_op(P, "{")) {                             /* static { ... } */
                m->op = 'b';
                m->b = parse_block(P);
                *tail = m;
                tail = &m->next;
                continue;
            }
        }
        if (kw(P, "async") && !tok_is(pkn(P, 1), T_OP, "(") && !tok_is(pkn(P, 1), T_OP, "=")) { nx(P); async = 1; }
        int gen_m = accept(P, "*");
        if ((kw(P, "get") || kw(P, "set")) && !tok_is(pkn(P, 1), T_OP, "(") && !tok_is(pkn(P, 1), T_OP, "=") &&
            !tok_is(pkn(P, 1), T_OP, ";") && !tok_is(pkn(P, 1), T_OP, "}")) {
            m->op = (uint8_t)pk(P)->p[0];
            nx(P);
        }
        tok_t* t = pk(P);
        if (t->t == T_ID || t->t == T_STR) { m->s = t->s; nx(P); }
        else if (t->t == T_NUM) { char b[32]; num_format(t->n, b, sizeof(b)); m->s = str_new(P->I, b, (uint32_t)strlen(b)); nx(P); }
        else if (accept(P, "[")) { m->a = parse_assign(P); expect(P, "]"); }
        else { perr(P, "bad class member"); break; }
        if (is_op(P, "(")) {
            node_t* f = mk(P, N_FUNC);
            f->s = m->s;
            f->a = parse_params(P);
            f->b = parse_fn_body(P);
            if (async) f->n = 1;
            if (gen_m) f->flags |= NF_GEN;
            m->b = f;
        } else {
            if (m->op == 'm') m->op = 'f';                   /* a field */
            if (accept(P, "=")) m->b = parse_assign(P);
            semi(P);
        }
        *tail = m;
        tail = &m->next;
    }
    return c;
}

static node_t* parse_primary(parser_t* P) {
    tok_t* t = pk(P);
    node_t* n;
    if (++P->depth > 200) { perr(P, "expression nested too deeply"); return mk(P, N_UNDEF); }
    switch (t->t) {
    case T_NUM:
        nx(P);
        n = mk(P, N_NUM);
        n->n = t->n;
        if (!P->php && t->len > 1 && t->p[t->len - 1] == 'n') {   /* 10n: its digits, for bi_literal */
            char b[256];
            uint32_t k = 0;
            for (uint32_t i = 0; i + 1 < t->len && k < sizeof(b) - 1; i++) if (t->p[i] != '_') b[k++] = t->p[i];
            n->s = str_new(P->I, b, k);
        }
        P->depth--;
        return n;
    case T_STR: nx(P); n = mk(P, N_STR); n->s = t->s; P->depth--; return n;
    case T_TPL: nx(P); n = parse_template(P, t); P->depth--; return n;
    case T_VAR: nx(P); n = mk(P, N_IDENT); n->s = t->s; P->depth--; return n;
    case T_REGEX: {
        nx(P);
        n = mk(P, N_REGEX);
        n->s = str_new(P->I, t->p, t->len);
        node_t* fl = mk(P, N_STR);
        fl->s = t->s;
        n->a = fl;
        P->depth--;
        return n;
    }
    case T_ID:
        if (is_kw(P, t, "true"))  { nx(P); P->depth--; return mk(P, N_TRUE); }
        if (is_kw(P, t, "false")) { nx(P); P->depth--; return mk(P, N_FALSE); }
        if (is_kw(P, t, "null"))  { nx(P); P->depth--; return mk(P, N_NULL); }
        if (!P->php) {
            if (is_kw(P, t, "undefined")) { nx(P); P->depth--; return mk(P, N_UNDEF); }
            if (is_kw(P, t, "this")) { nx(P); P->depth--; return mk(P, N_THIS); }
            if (is_kw(P, t, "function")) {
                nx(P);
                int gen = accept(P, "*");
                n = parse_function(P, 0);
                if (n->s) n->flags |= NF_NAMED;
                if (gen) n->flags |= NF_GEN;
                P->depth--;
                return n;
            }
            if (is_kw(P, t, "async") && is_kw(P, pkn(P, 1), "function")) {
                nx(P); nx(P);
                int gen = accept(P, "*");
                n = parse_function(P, 0);
                n->n = 1;
                if (n->s) n->flags |= NF_NAMED;
                if (gen) n->flags |= NF_GEN;
                P->depth--;
                return n;
            }
            if (is_kw(P, t, "class")) { nx(P); n = parse_class(P); P->depth--; return n; }
            if (is_kw(P, t, "super")) {
                nx(P);
                if (accept(P, "(")) { n = mk(P, N_SUPERCALL); n->b = parse_args(P); }
                else {
                    n = mk(P, N_SUPERMEMBER);
                    if (accept(P, "[")) { n->a = parse_expr(P); expect(P, "]"); }
                    else { expect(P, "."); n->s = ident(P); }
                }
                P->depth--;
                return n;
            }
            if (is_kw(P, t, "new")) {
                nx(P);
                n = mk(P, N_NEW);
                node_t* c;
                if (accept(P, "(")) { c = parse_expr(P); expect(P, ")"); }   /* new (f())() */
                else if (kw(P, "class")) { nx(P); c = parse_class(P); }
                else if (kw(P, "this")) { nx(P); c = mk(P, N_THIS); }
                else { c = mk(P, N_IDENT); c->s = ident(P); }
                while (accept(P, ".")) {              /* new a.B() */
                    node_t* m = mk(P, N_MEMBER);
                    m->a = c;
                    m->s = ident(P);
                    c = m;
                }
                n->a = c;
                if (accept(P, "(")) n->b = parse_args(P);
                P->depth--;
                return n;
            }
        } else {
            if (is_kw(P, t, "array") && tok_is(pkn(P, 1), T_OP, "(")) {
                nx(P); nx(P);
                n = parse_array_lit(P, ")");
                P->depth--;
                return n;
            }
            if (is_kw(P, t, "function") || is_kw(P, t, "fn")) {
                int is_fn = is_kw(P, t, "fn");
                nx(P);
                if (is_fn) {
                    n = mk(P, N_FUNC);
                    n->a = parse_params(P);
                    expect(P, "=>");
                    n->op = 1;
                    n->c = (node_t*)1;
                    n->b = parse_assign(P);
                } else {
                    n = parse_function(P, 0);
                }
                P->depth--;
                return n;
            }
        }
        nx(P);
        n = mk(P, N_IDENT);
        n->s = t->s;
        if (P->php) n->op = 1;                       /* bare word: function or constant */
        P->depth--;
        return n;
    case T_OP:
        if (accept(P, "(")) {
            int ni = P->no_in;
            P->no_in = 0;
            n = parse_expr(P);
            P->no_in = ni;
            expect(P, ")");
            P->depth--;
            return n;
        }
        if (accept(P, "[")) { int ni = P->no_in; P->no_in = 0; n = parse_array_lit(P, "]"); P->no_in = ni; P->depth--; return n; }
        if (!P->php && accept(P, "{")) { int ni = P->no_in; P->no_in = 0; n = parse_object_lit(P); P->no_in = ni; P->depth--; return n; }
        break;
    default:
        break;
    }
    perr(P, t->t == T_EOF ? "unexpected end of script" : "unexpected token");
    P->depth--;
    return mk(P, N_UNDEF);
}

static node_t* parse_postfix(parser_t* P) {
    node_t* e = parse_primary(P);
    while (!P->err) {
        if (!P->php && accept(P, ".")) {
            node_t* m = mk(P, N_MEMBER);
            m->a = e;
            m->s = ident(P);
            e = m;
        } else if (accept(P, "?.")) {
            if (accept(P, "(")) {                     /* f?.() */
                node_t* c = mk(P, N_CALL);
                c->a = e;
                c->op = 1;
                c->b = parse_args(P);
                e = c;
                continue;
            }
            if (accept(P, "[")) {                     /* a?.[i] */
                node_t* ix = mk(P, N_INDEX);
                ix->a = e;
                ix->op = 1;
                ix->b = parse_expr(P);
                expect(P, "]");
                e = ix;
                continue;
            }
            node_t* m = mk(P, N_MEMBER);
            m->a = e;
            m->op = 1;
            m->s = ident(P);
            e = m;
        } else if (P->php && accept(P, "->")) {
            node_t* m = mk(P, N_MEMBER);
            m->a = e;
            m->s = ident(P);
            e = m;
        } else if (accept(P, "[")) {
            node_t* ix = mk(P, N_INDEX);
            ix->a = e;
            int ni = P->no_in;
            P->no_in = 0;
            if (!(P->php && is_op(P, "]"))) ix->b = parse_expr(P);
            P->no_in = ni;
            expect(P, "]");
            e = ix;
        } else if (is_op(P, "{") && P->php && 0) {
            break;
        } else if (accept(P, "(")) {
            node_t* c = mk(P, N_CALL);
            c->a = e;
            c->b = parse_args(P);
            e = c;
        } else if (!P->php && pk(P)->t == T_TPL) {
            e = parse_tagged(P, nx(P), e);           /* tag`x${y}` */
        } else if ((is_op(P, "++") || is_op(P, "--")) && pk(P)->line == P->t[P->i - 1].line) {
            node_t* u = mk(P, N_UPDATE);
            u->op = (uint8_t)(nx(P)->p[0]);
            u->a = e;
            e = u;
        } else {
            break;
        }
    }
    return e;
}

static node_t* parse_unary(parser_t* P) {
    tok_t* t = pk(P);
    if (t->t == T_OP) {
        int op = 0;
        if (tok_is(t, T_OP, "!")) op = OP_NOT;
        else if (tok_is(t, T_OP, "-")) op = OP_NEG;
        else if (tok_is(t, T_OP, "+")) op = OP_PLUS;
        else if (tok_is(t, T_OP, "~")) op = OP_BITNOT;
        if (op) {
            nx(P);
            node_t* u = mk(P, N_UNARY);
            u->op = (uint8_t)op;
            u->a = parse_unary(P);
            return u;
        }
        if (tok_is(t, T_OP, "++") || tok_is(t, T_OP, "--")) {
            nx(P);
            node_t* u = mk(P, N_UPDATE);
            u->op = (uint8_t)t->p[0];
            u->n = 1;
            u->a = parse_unary(P);
            return u;
        }
        if (tok_is(t, T_OP, "@")) { nx(P); return parse_unary(P); }
        /* PHP casts: (int) $x */
        if (P->php && tok_is(t, T_OP, "(") && pkn(P, 1)->t == T_ID && tok_is(pkn(P, 2), T_OP, ")")) {
            tok_t* ty = pkn(P, 1);
            static const char* const types[] = { "int", "integer", "float", "double", "string", "bool", "boolean", "array" };
            for (uint32_t i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
                if (is_kw(P, ty, types[i])) {
                    nx(P); nx(P); nx(P);
                    node_t* c = mk(P, N_CAST);
                    c->s = ty->s;
                    c->a = parse_unary(P);
                    return c;
                }
            }
        }
    }
    if (!P->php && (is_kw(P, t, "typeof"))) {
        nx(P);
        node_t* u = mk(P, N_TYPEOF);
        u->a = parse_unary(P);
        return u;
    }
    if (!P->php && is_kw(P, t, "void")) {
        nx(P);
        node_t* s = mk(P, N_SEQ);                   /* evaluate, then undefined */
        s->a = parse_unary(P);
        s->a->next = mk(P, N_UNDEF);
        return s;
    }
    if (!P->php && is_kw(P, t, "await")) {
        nx(P);
        node_t* u = mk(P, N_AWAIT);
        u->a = parse_unary(P);
        return u;
    }
    if (!P->php && is_kw(P, t, "delete")) {
        nx(P);
        node_t* u = mk(P, N_UNARY);
        u->op = 'D';
        u->a = parse_unary(P);
        return u;
    }
    if (P->php && (is_kw(P, t, "print"))) {
        nx(P);
        node_t* c = mk(P, N_CALL);
        node_t* id = mk(P, N_IDENT);
        id->s = str_new(P->I, "print", 5);
        id->op = 1;
        c->a = id;
        c->b = parse_assign(P);
        return c;
    }
    return parse_postfix(P);
}

typedef struct { const char* op; int prec; int code; int logic; } binop_t;

static const binop_t BINOPS[] = {
    { "??", 1, OP_NULLISH, 1 },
    { "||", 2, OP_OR, 1 }, { "&&", 3, OP_AND, 1 },
    { "|", 4, OP_BOR, 0 }, { "^", 5, OP_BXOR, 0 }, { "&", 6, OP_BAND, 0 },
    { "==", 7, OP_EQ, 0 }, { "!=", 7, OP_NE, 0 }, { "===", 7, OP_SEQ, 0 }, { "!==", 7, OP_SNE, 0 }, { "<>", 7, OP_NE, 0 },
    { "<", 8, OP_LT, 0 }, { ">", 8, OP_GT, 0 }, { "<=", 8, OP_LE, 0 }, { ">=", 8, OP_GE, 0 }, { "<=>", 8, OP_SPACESHIP, 0 },
    { "<<", 9, OP_SHL, 0 }, { ">>", 9, OP_SHR, 0 }, { ">>>", 9, OP_USHR, 0 },
    { "+", 10, OP_ADD, 0 }, { "-", 10, OP_SUB, 0 }, { ".", 10, OP_CONCAT, 0 },
    { "*", 11, OP_MUL, 0 }, { "/", 11, OP_DIV, 0 }, { "%", 11, OP_MOD, 0 },
    { "**", 12, OP_POW, 0 },
};

static const binop_t* binop_at(parser_t* P) {
    tok_t* t = pk(P);
    if (t->t == T_ID && P->php) {
        static const binop_t kand = { "and", 3, OP_AND, 1 }, kor = { "or", 2, OP_OR, 1 };
        if (is_kw(P, t, "and")) return &kand;
        if (is_kw(P, t, "or")) return &kor;
        return NULL;
    }
    if (t->t == T_ID) {
        static const binop_t kin = { "in", 8, OP_IN, 0 }, kinst = { "instanceof", 8, OP_INSTANCEOF, 0 };
        if (is_kw(P, t, "in") && !P->no_in) return &kin;
        if (is_kw(P, t, "instanceof")) return &kinst;
        return NULL;
    }
    if (t->t != T_OP) return NULL;
    for (uint32_t i = 0; i < sizeof(BINOPS) / sizeof(BINOPS[0]); i++) {
        if (tok_is(t, T_OP, BINOPS[i].op)) {
            if (BINOPS[i].code == OP_CONCAT && !P->php) return NULL;
            return &BINOPS[i];
        }
    }
    return NULL;
}

static node_t* parse_binary(parser_t* P, int min_prec) {
    node_t* left = parse_unary(P);
    while (!P->err) {
        const binop_t* b = binop_at(P);
        if (!b || b->prec < min_prec) break;
        nx(P);
        node_t* right = parse_binary(P, b->code == OP_POW ? b->prec : b->prec + 1);
        node_t* n = mk(P, b->logic ? N_LOGIC : N_BINARY);
        n->op = (uint8_t)b->code;
        n->a = left;
        n->b = right;
        left = n;
    }
    return left;
}

static node_t* parse_cond(parser_t* P) {
    node_t* c = parse_binary(P, 1);
    if (is_op(P, "?")) {
        nx(P);
        node_t* n = mk(P, N_COND);
        n->a = c;
        if (accept(P, ":")) {                        /* PHP ?: */
            n->c = parse_assign(P);
            return n;
        }
        n->b = parse_assign(P);
        expect(P, ":");
        n->c = parse_assign(P);
        return n;
    }
    return c;
}

static const char* const ASSIGN_OPS[] = { "=", "+=", "-=", "*=", "/=", "%=", ".=", "**=", "?\?=", "|=", "&=", "^=",
                                          "<<=", ">>=", ">>>=", "||=", "&&=" };
static const uint8_t ASSIGN_CODES[] = { '=', OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD, OP_CONCAT, OP_POW, OP_NULLISH, OP_BOR, OP_BAND, OP_BXOR,
                                        OP_SHL, OP_SHR, OP_USHR, OP_LOGOR_ASSIGN, OP_LOGAND_ASSIGN };

static node_t* parse_assign(parser_t* P) {
    if (!P->php && arrow_ahead(P)) return parse_arrow(P);
    if (!P->php && kw(P, "async") && (pkn(P, 1)->t == T_ID || tok_is(pkn(P, 1), T_OP, "("))) {
        /* async x => ..., async (a) => ... */
        uint32_t save = P->i;
        nx(P);
        if (arrow_ahead(P)) { node_t* f = parse_arrow(P); f->n = 1; return f; }
        P->i = save;
    }
    if (!P->php && kw(P, "yield")) {
        tok_t* y = nx(P);
        node_t* n = mk(P, N_YIELD);
        if (accept(P, "*")) n->op = '*';
        tok_t* t = pk(P);
        int bare = t->t == T_EOF || t->line != y->line || tok_is(t, T_OP, ")") || tok_is(t, T_OP, "]") ||
                   tok_is(t, T_OP, "}") || tok_is(t, T_OP, ",") || tok_is(t, T_OP, ";") || tok_is(t, T_OP, ":");
        if (n->op || !bare) n->a = parse_assign(P);
        return n;
    }
    node_t* left = parse_cond(P);
    for (uint32_t i = 0; i < sizeof(ASSIGN_OPS) / sizeof(ASSIGN_OPS[0]); i++) {
        if (is_op(P, ASSIGN_OPS[i])) {
            int pattern = !P->php && i == 0 && (left->k == N_ARRAY || left->k == N_OBJECT);   /* [a, b] = ... */
            if (left->k != N_IDENT && left->k != N_MEMBER && left->k != N_INDEX && !pattern) {
                perr(P, "invalid assignment target");
                return left;
            }
            nx(P);
            node_t* n = mk(P, N_ASSIGN);
            n->op = ASSIGN_CODES[i];
            n->a = left;
            n->b = parse_assign(P);
            return n;
        }
    }
    return left;
}

static node_t* parse_expr(parser_t* P) {
    node_t* e = parse_assign(P);
    /* comma operator: evaluate all, keep the last */
    if (P->php || !is_op(P, ",")) return e;
    node_t* s = mk(P, N_SEQ);
    s->a = e;
    node_t** tail = &e->next;
    while (!P->err && accept(P, ",")) {
        node_t* x = parse_assign(P);
        *tail = x;
        tail = &x->next;
    }
    return s;
}

/* ── statements ── */

static node_t* parse_stmts_until(parser_t* P, const char* const* ends, int nends) {
    node_t* blk = mk(P, N_BLOCK);
    node_t** tail = &blk->a;
    while (!P->err && pk(P)->t != T_EOF) {
        for (int i = 0; i < nends; i++) if (kw(P, ends[i])) return blk;
        node_t* s = parse_stmt(P);
        if (!s) continue;
        *tail = s;
        while (*tail) tail = &(*tail)->next;
    }
    return blk;
}

static node_t* parse_block(parser_t* P) {
    node_t* blk = mk(P, N_BLOCK);
    int saved_no_in = P->no_in;                      /* for (var f = function () { a in b }; ...) */
    P->no_in = 0;
    expect(P, "{");
    node_t** tail = &blk->a;
    while (!P->err && !is_op(P, "}") && pk(P)->t != T_EOF) {
        node_t* s = parse_stmt(P);
        if (!s) continue;
        *tail = s;
        while (*tail) tail = &(*tail)->next;
    }
    expect(P, "}");
    P->no_in = saved_no_in;
    /* a JS block declaring nothing block-scoped (let/const/class/function)
     * runs without a scope of its own: no allocation each time a loop
     * body or if branch runs */
    if (!P->php) {
        int scoped = 0;
        for (node_t* s = blk->a; s && !scoped; s = s->next) {
            if (s->k == N_FUNCDECL || s->k == N_CLASS) scoped = 1;
            else if (s->k == N_BLOCK && s->op == 1) {
                for (node_t* v = s->a; v; v = v->next) if (v->k != N_VAR || v->op != 'v') scoped = 1;
            } else if (s->k == N_VAR && s->op != 'v') scoped = 1;
            else if (s->k != N_EXPR && s->k != N_IF && s->k != N_RETURN && s->k != N_BLOCK && s->k != N_FOR &&
                     s->k != N_WHILE && s->k != N_DOWHILE && s->k != N_BREAK && s->k != N_CONTINUE &&
                     s->k != N_THROW && s->k != N_TRY && s->k != N_EMPTY && s->k != N_FOREACH && s->k != N_SWITCH) scoped = 1;
        }
        if (!scoped) blk->op = 1;
    }
    return blk;
}

static node_t* parse_body(parser_t* P) {
    return is_op(P, "{") ? parse_block(P) : parse_stmt(P);
}

static node_t* parse_var_decl(parser_t* P, int kind) {
    node_t* blk = mk(P, N_BLOCK);
    blk->op = 1;                                     /* no scope of its own */
    node_t** tail = &blk->a;
    do {
        node_t* v = mk(P, N_VAR);
        v->op = (uint8_t)kind;
        if (!P->php && (is_op(P, "[") || is_op(P, "{"))) v->c = parse_primary(P);   /* const {a, b} = o */
        else v->s = ident(P);
        if (accept(P, "=")) v->a = parse_assign(P);
        *tail = v;
        tail = &v->next;
        if (kind == 'v' && v->s && P->hoist) {       /* usable (as undefined) from the top of the function */
            node_t* h = mk(P, N_IDENT);
            h->s = v->s;
            h->next = P->hoist->a;
            P->hoist->a = h;
        }
    } while (!P->err && accept(P, ","));
    return blk;
}

/* ── modules ──
 * N_IMPORT: s = module name, a = bindings; op 'r' = re-export (export ... from)
 * N_EXPORT: a = declaration (or, op 'd', the default value), b = bindings
 * a binding is an N_STR: s = the name in the other module (import) / exported
 * name (export), b = N_IDENT with the local name ("*": the whole namespace) */
static node_t* parse_stmt(parser_t* P);
static node_t* mk_binding(parser_t* P, str_t* name, str_t* local) {
    node_t* b = mk(P, N_STR);
    b->s = name;
    if (local) { b->b = mk(P, N_IDENT); b->b->s = local; }
    return b;
}
static str_t* name_tok(parser_t* P) {               /* an identifier, or a string: export { a as "b-c" } */
    tok_t* t = nx(P);
    if ((t->t == T_ID || t->t == T_STR) && t->s) return t->s;
    perr(P, "expected a name");
    return str_new(P->I, "", 0);
}
static str_t* module_spec(parser_t* P) {
    if (pk(P)->t != T_STR) { perr(P, "expected a module name"); return NULL; }
    str_t* s = nx(P)->s;
    if (kw(P, "with") || kw(P, "assert")) { nx(P); if (is_op(P, "{")) parse_primary(P); }
    return s;
}

static node_t* parse_import(parser_t* P) {
    nx(P);
    node_t* n = mk(P, N_IMPORT);
    node_t** tail = &n->a;
    if (pk(P)->t != T_STR) {
        if (pk(P)->t == T_ID) {                      /* import x from / import x, {...} from */
            *tail = mk_binding(P, str_new(P->I, "default", 7), nx(P)->s);
            tail = &(*tail)->next;
            accept(P, ",");
        }
        if (accept(P, "*")) {
            if (!kw(P, "as")) { perr(P, "expected 'as'"); return n; }
            nx(P);
            *tail = mk_binding(P, str_new(P->I, "*", 1), nx(P)->s);
            tail = &(*tail)->next;
        } else if (accept(P, "{")) {
            while (!P->err && !accept(P, "}")) {
                str_t* name = name_tok(P);
                str_t* local = name;
                if (kw(P, "as")) { nx(P); local = nx(P)->s; }
                *tail = mk_binding(P, name, local);
                tail = &(*tail)->next;
                if (!accept(P, ",")) { expect(P, "}"); break; }
            }
        }
        if (!kw(P, "from")) { perr(P, "expected 'from'"); return n; }
        nx(P);
    }
    n->s = module_spec(P);
    semi(P);
    return n;
}

static node_t* parse_export(parser_t* P) {
    nx(P);
    node_t* n = mk(P, N_EXPORT);
    if (kw(P, "default")) {
        nx(P);
        int fn = kw(P, "function") && pkn(P, 1)->t == T_ID;
        int afn = kw(P, "async") && is_kw(P, pkn(P, 1), "function") && pkn(P, 2)->t == T_ID;
        int cls = kw(P, "class") && pkn(P, 1)->t == T_ID && !is_kw(P, pkn(P, 1), "extends");
        if (fn || afn || cls) {                      /* export default function f() {} */
            str_t* name = pkn(P, afn ? 2 : 1)->s;
            n->a = parse_stmt(P);
            n->b = mk_binding(P, str_new(P->I, "default", 7), name);
            return n;
        }
        n->op = 'd';
        n->a = parse_assign(P);
        semi(P);
        return n;
    }
    if (accept(P, "*")) {                            /* export * [as ns] from "m" */
        node_t* imp = mk(P, N_IMPORT);
        imp->op = 'r';
        str_t* as = NULL;
        if (kw(P, "as")) { nx(P); as = name_tok(P); }
        imp->a = mk_binding(P, str_new(P->I, "*", 1), as);
        if (!kw(P, "from")) { perr(P, "expected 'from'"); return imp; }
        nx(P);
        imp->s = module_spec(P);
        semi(P);
        return imp;
    }
    if (accept(P, "{")) {                            /* export { a, b as c } [from "m"] */
        node_t* list = NULL;
        node_t** tail = &list;
        while (!P->err && !accept(P, "}")) {
            str_t* local = name_tok(P);
            str_t* ex = local;
            if (kw(P, "as")) { nx(P); ex = name_tok(P); }
            *tail = mk_binding(P, ex, local);
            tail = &(*tail)->next;
            if (!accept(P, ",")) { expect(P, "}"); break; }
        }
        if (kw(P, "from")) {
            nx(P);
            node_t* imp = mk(P, N_IMPORT);
            imp->op = 'r';
            /* re-export: the name over there, and the name to export it as */
            node_t** it = &imp->a;
            for (node_t* b = list; b; b = b->next) {
                *it = mk_binding(P, b->b->s, b->s);
                it = &(*it)->next;
            }
            imp->s = module_spec(P);
            semi(P);
            return imp;
        }
        n->b = list;
        semi(P);
        return n;
    }
    /* export var/let/const/function/class */
    node_t* d = parse_stmt(P);
    n->a = d;
    node_t** tail = &n->b;
    if (d && d->k == N_BLOCK) {
        for (node_t* v = d->a; v; v = v->next)
            if (v->k == N_VAR && v->s) { *tail = mk_binding(P, v->s, v->s); tail = &(*tail)->next; }
    } else if (d && d->k == N_FUNCDECL && d->a && d->a->s) {
        *tail = mk_binding(P, d->a->s, d->a->s);
    } else if (d && d->k == N_CLASS && d->s) {
        *tail = mk_binding(P, d->s, d->s);
    }
    return n;
}

static node_t* parse_if(parser_t* P) {
    node_t* n = mk(P, N_IF);
    expect(P, "(");
    n->a = parse_expr(P);
    expect(P, ")");
    if (P->php && accept(P, ":")) {                  /* if (): ... endif; */
        static const char* const ends[] = { "elseif", "else", "endif" };
        n->b = parse_stmts_until(P, ends, 3);
        if (kw(P, "elseif")) { nx(P); n->c = parse_if(P); return n; }
        if (kw(P, "else")) {
            nx(P);
            if (kw(P, "if")) { nx(P); n->c = parse_if(P); return n; }
            accept(P, ":");
            static const char* const end1[] = { "endif" };
            n->c = parse_stmts_until(P, end1, 1);
        }
        if (kw(P, "endif")) { nx(P); semi(P); }
        return n;
    }
    n->b = parse_body(P);
    if (P->php && kw(P, "elseif")) { nx(P); n->c = parse_if(P); return n; }
    if (kw(P, "else")) {
        nx(P);
        n->c = parse_body(P);
    }
    return n;
}

static node_t* parse_alt_body(parser_t* P, const char* endkw) {
    if (P->php && accept(P, ":")) {
        const char* ends[1] = { endkw };
        node_t* b = parse_stmts_until(P, ends, 1);
        if (kw(P, endkw)) { nx(P); semi(P); }
        return b;
    }
    return parse_body(P);
}

static node_t* parse_stmt(parser_t* P) {
    WEB_TICK();
    tok_t* t = pk(P);
    node_t* n;
    if (P->err) return NULL;
    if (t->t == T_HTML) { nx(P); n = mk(P, N_HTML); n->s = t->s; return n; }
    if (t->t == T_ECHOTAG) {
        nx(P);
        n = mk(P, N_ECHO);
        n->a = parse_expr(P);
        semi(P);
        return n;
    }
    if (is_op(P, "{")) return parse_block(P);
    if (accept(P, ";")) return NULL;
    if (t->t == T_ID) {
        if (!P->php && (kw(P, "var") || kw(P, "let") || kw(P, "const"))) {
            int kind = kw(P, "var") ? 'v' : kw(P, "let") ? 'l' : 'c';
            nx(P);
            n = parse_var_decl(P, kind);
            semi(P);
            return n;
        }
        if (kw(P, "function") && (pkn(P, 1)->t == T_ID || tok_is(pkn(P, 1), T_OP, "*"))) {
            nx(P);
            int gen = accept(P, "*");
            n = mk(P, N_FUNCDECL);
            n->a = parse_function(P, 1);
            if (gen) n->a->flags |= NF_GEN;
            return n;
        }
        if (!P->php && kw(P, "async") && is_kw(P, pkn(P, 1), "function")) {
            nx(P); nx(P);
            int gen = accept(P, "*");
            n = mk(P, N_FUNCDECL);
            n->a = parse_function(P, 1);
            n->a->n = 1;
            if (gen) n->a->flags |= NF_GEN;
            return n;
        }
        if (!P->php && kw(P, "class") && pkn(P, 1)->t == T_ID) {
            nx(P);
            n = parse_class(P);
            n->op = 1;                                       /* a declaration */
            return n;
        }
        /* ES modules */
        if (!P->php && kw(P, "export")) return parse_export(P);
        if (!P->php && kw(P, "import") && !tok_is(pkn(P, 1), T_OP, "(") && !tok_is(pkn(P, 1), T_OP, "."))
            return parse_import(P);
        /* label: statement */
        if (!P->php && tok_is(pkn(P, 1), T_OP, ":") && !kw(P, "default") && !kw(P, "case")) {
            str_t* label = nx(P)->s;
            nx(P);
            n = mk(P, N_LABEL);
            n->s = label;
            n->a = parse_stmt(P);
            if (n->a && (n->a->k == N_FOR || n->a->k == N_FOREACH || n->a->k == N_WHILE || n->a->k == N_DOWHILE))
                n->a->s = label;
            return n;
        }
        if (kw(P, "if")) { nx(P); return parse_if(P); }
        if (kw(P, "while")) {
            nx(P);
            n = mk(P, N_WHILE);
            expect(P, "(");
            n->a = parse_expr(P);
            expect(P, ")");
            n->b = parse_alt_body(P, "endwhile");
            return n;
        }
        if (kw(P, "do")) {
            nx(P);
            n = mk(P, N_DOWHILE);
            n->b = parse_body(P);
            if (!kw(P, "while")) { perr(P, "expected 'while'"); return n; }
            nx(P);
            expect(P, "(");
            n->a = parse_expr(P);
            expect(P, ")");
            semi(P);
            return n;
        }
        if (kw(P, "for")) {
            nx(P);
            if (!P->php && kw(P, "await")) nx(P);    /* for await (x of y): run as a plain for-of */
            expect(P, "(");
            /* for (let x of a) / for (k in o) */
            uint32_t save = P->i;
            int decl = 0;
            if (!P->php && (kw(P, "let") || kw(P, "const") || kw(P, "var"))) { nx(P); decl = 1; }
            if (!P->php && (is_op(P, "[") || is_op(P, "{"))) {
                /* for (const [k, v] of ...) */
                uint32_t at = P->i;
                const char* oerr = P->err;
                node_t* pat = parse_primary(P);
                if (!P->err && (kw(P, "of") || kw(P, "in"))) {
                    n = mk(P, N_FOREACH);
                    n->op = kw(P, "of") ? 'o' : 'i';
                    nx(P);
                    n->b = pat;
                    n->a = parse_expr(P);
                    expect(P, ")");
                    n->d = parse_body(P);
                    return n;
                }
                P->err = oerr;
                P->i = at;
            }
            if (!P->php && pk(P)->t == T_ID && (is_kw(P, pkn(P, 1), "of") || is_kw(P, pkn(P, 1), "in"))) {
                n = mk(P, N_FOREACH);
                node_t* v = mk(P, N_IDENT);
                v->s = nx(P)->s;
                n->op = is_kw(P, pk(P), "of") ? 'o' : 'i';
                nx(P);
                n->b = v;
                n->a = parse_expr(P);
                expect(P, ")");
                n->d = parse_body(P);
                (void)decl;
                return n;
            }
            P->i = save;
            n = mk(P, N_FOR);
            if (!is_op(P, ";")) {
                if (!P->php && (kw(P, "let") || kw(P, "const") || kw(P, "var"))) {
                    int kind = kw(P, "var") ? 'v' : kw(P, "let") ? 'l' : 'c';
                    nx(P);
                    P->no_in = 1;
                    n->a = parse_var_decl(P, kind);
                    P->no_in = 0;
                } else {
                    node_t* e = mk(P, N_EXPR);
                    e->a = parse_expr(P);
                    n->a = e;
                }
            }
            expect(P, ";");
            if (!is_op(P, ";")) n->b = parse_expr(P);
            expect(P, ";");
            if (!is_op(P, ")")) {
                node_t* e = mk(P, N_EXPR);
                e->a = parse_expr(P);
                n->c = e;
                /* for (...; ...; i++, j++) */
                node_t** tail = &e->next;
                while (!P->err && accept(P, ",")) {
                    node_t* e2 = mk(P, N_EXPR);
                    e2->a = parse_expr(P);
                    *tail = e2;
                    tail = &e2->next;
                }
                if (e->next) {
                    node_t* blk = mk(P, N_BLOCK);
                    blk->op = 1;
                    blk->a = e;
                    n->c = blk;
                }
            }
            expect(P, ")");
            n->d = parse_alt_body(P, "endfor");
            return n;
        }
        if (P->php && kw(P, "foreach")) {
            nx(P);
            n = mk(P, N_FOREACH);
            n->op = 'p';
            expect(P, "(");
            n->a = parse_expr(P);
            if (!kw(P, "as")) { perr(P, "expected 'as'"); return n; }
            nx(P);
            accept(P, "&");
            node_t* first = parse_postfix(P);
            if (accept(P, "=>")) {
                n->c = first;
                accept(P, "&");
                n->b = parse_postfix(P);
            } else {
                n->b = first;
            }
            expect(P, ")");
            n->d = parse_alt_body(P, "endforeach");
            return n;
        }
        if (kw(P, "return")) {
            nx(P);
            n = mk(P, N_RETURN);
            if (!is_op(P, ";") && !is_op(P, "}") && pk(P)->t != T_EOF) n->a = parse_expr(P);
            semi(P);
            return n;
        }
        if (kw(P, "break") || kw(P, "continue")) {
            int brk = kw(P, "break");
            int line = nx(P)->line;
            n = mk(P, brk ? N_BREAK : N_CONTINUE);
            if (pk(P)->t == T_NUM) nx(P);
            else if (!P->php && pk(P)->t == T_ID && pk(P)->line == line) n->s = nx(P)->s;   /* break label */
            semi(P);
            return n;
        }
        if (kw(P, "throw")) {
            nx(P);
            n = mk(P, N_THROW);
            if (P->php && kw(P, "new")) nx(P);
            n->a = parse_expr(P);
            semi(P);
            return n;
        }
        if (kw(P, "try")) {
            nx(P);
            n = mk(P, N_TRY);
            n->a = parse_block(P);
            if (kw(P, "catch")) {
                nx(P);
                if (accept(P, "(")) {
                    if (P->php) {
                        while (pk(P)->t == T_ID) nx(P);          /* exception type */
                        if (pk(P)->t == T_VAR) n->s = nx(P)->s;
                    } else {
                        n->s = ident(P);
                    }
                    expect(P, ")");
                }
                n->b = parse_block(P);
            }
            if (kw(P, "finally")) { nx(P); n->c = parse_block(P); }
            return n;
        }
        if (kw(P, "switch")) {
            nx(P);
            n = mk(P, N_SWITCH);
            expect(P, "(");
            n->a = parse_expr(P);
            expect(P, ")");
            expect(P, "{");
            node_t** tail = &n->b;
            while (!P->err && !accept(P, "}")) {
                node_t* c = mk(P, N_CASE);
                if (kw(P, "case")) { nx(P); c->a = parse_expr(P); }
                else if (kw(P, "default")) nx(P);
                else { perr(P, "expected 'case'"); break; }
                if (!accept(P, ":")) expect(P, ";");
                node_t* blk = mk(P, N_BLOCK);
                blk->op = 1;
                node_t** st = &blk->a;
                while (!P->err && !kw(P, "case") && !kw(P, "default") && !is_op(P, "}") && pk(P)->t != T_EOF) {
                    node_t* s = parse_stmt(P);
                    if (!s) continue;
                    *st = s;
                    while (*st) st = &(*st)->next;
                }
                c->b = blk;
                *tail = c;
                tail = &c->next;
            }
            return n;
        }
        if (P->php && kw(P, "echo")) {
            nx(P);
            n = mk(P, N_ECHO);
            node_t** tail = &n->a;
            do {
                node_t* e = parse_expr(P);
                *tail = e;
                tail = &e->next;
            } while (!P->err && accept(P, ","));
            semi(P);
            return n;
        }
        if (P->php && kw(P, "global")) {
            nx(P);
            node_t* blk = mk(P, N_BLOCK);
            blk->op = 1;
            node_t** tail = &blk->a;
            do {
                if (pk(P)->t != T_VAR) { perr(P, "expected a $variable"); break; }
                node_t* g = mk(P, N_GLOBAL);
                g->s = nx(P)->s;
                *tail = g;
                tail = &g->next;
            } while (accept(P, ","));
            semi(P);
            return blk;
        }
    }
    n = mk(P, N_EXPR);
    n->a = parse_expr(P);
    semi(P);            /* semicolons are optional (automatic semicolon insertion, leniently) */
    return n;
}

static node_t* parse_sub(parser_t* P, const char* s, uint32_t n, int line) {
    lexer_t L;
    memset(&L, 0, sizeof(L));
    L.I = P->I;
    L.src = s;
    L.len = n;
    L.line = line;
    lex_all(&L);
    if (L.err) { kfree(L.toks); perr(P, L.err); return mk(P, N_UNDEF); }
    parser_t Q;
    memset(&Q, 0, sizeof(Q));
    Q.I = P->I;
    Q.t = L.toks;
    Q.n = L.ntok;
    Q.php = P->php;
    Q.depth = P->depth;
    node_t* e = parse_expr(&Q);
    if (!Q.err && pk(&Q)->t != T_EOF) perr(&Q, "bad expression in string");
    if (Q.err) { P->err = Q.err; P->err_line = Q.err_line; P->err_at = Q.err_at; }
    kfree(L.toks);
    return e;
}

/* ══ interpreter ══════════════════════════════════════════════════════ */

static value_t eval(interp_t* I, node_t* n);
static void exec(interp_t* I, node_t* n);
static void exec_list(interp_t* I, node_t* n);
static void destructure(interp_t* I, node_t* pat, value_t v, int kind);
static void init_fields(interp_t* I, func_t* cls);
static void super_call(interp_t* I, func_t* cls, int argc, value_t* argv);
static value_t key_of(interp_t* I, value_t k, char* buf, int cap, const char** out);

/* The arena frees nothing until the page goes, so a script making millions
 * of calls would fill it with dead scopes. A scope no closure captured is
 * dead once its call or block ends: its env and variables are kept on free
 * lists and reused. */
static env_t* env_new(interp_t* I, env_t* parent, int is_func) {
    env_t* e = I->free_envs;
    if (e) { I->free_envs = e->parent; memset(e, 0, sizeof(*e)); }
    else e = (env_t*)arena_alloc(I->A, sizeof(env_t));
    e->parent = parent;
    e->is_func = is_func;
    return e;
}

/* may code under n use `arguments`? (eval could too) */
static int mentions_arguments(node_t* n, int depth) {
    if ((uintptr_t)n < 4096) return 0;              /* NULL, or a marker (arrow, implicit constructor) */
    for (; n; n = n->next) {
        if (depth > 300) return 1;
        if (n->s && n->k != N_STR && (strcmp(n->s->s, "arguments") == 0 || strcmp(n->s->s, "eval") == 0)) return 1;
        if (mentions_arguments(n->a, depth + 1) || mentions_arguments(n->b, depth + 1) ||
            mentions_arguments(n->c, depth + 1) || mentions_arguments(n->d, depth + 1)) return 1;
    }
    return 0;
}

/* a closure made in scope e keeps it and every scope around it */
static void env_capture(env_t* e) {
    for (; e && !e->captured; e = e->parent) e->captured = 1;
}

static void env_release(interp_t* I, env_t* e) {
    if (!e || e->captured || I->lang != LANG_JS || e == I->global) return;
    var_t* v = e->vars;
    while (v) {
        var_t* nx = v->next;
        v->next = I->free_vars;
        I->free_vars = v;
        v = nx;
    }
    e->vars = NULL;
    e->parent = I->free_envs;
    I->free_envs = e;
}

static var_t* env_find_local(env_t* e, const char* name) {
    for (var_t* v = e->vars; v; v = v->next)
        if (strcmp(v->name->s, name) == 0) return v;
    return NULL;
}

static var_t* env_lookup(interp_t* I, env_t* e, const char* name) {
    for (; e; e = e->parent) {
        var_t* v = env_find_local(e, name);
        if (v) return v->ref ? v->ref : v;
    }
    /* PHP superglobals are visible inside functions */
    if (I->lang == LANG_PHP && name[0] == '_') return env_find_local(I->global, name);
    return NULL;
}

static var_t* env_define(interp_t* I, env_t* e, str_t* name, value_t v) {
    var_t* x = env_find_local(e, name->s);
    if (x) {
        if (x->ref) x = x->ref;
        x->v = v;
        return x;
    }
    x = I->free_vars;
    if (x) { I->free_vars = x->next; memset(x, 0, sizeof(*x)); }
    else x = (var_t*)arena_alloc(I->A, sizeof(var_t));
    x->name = name;
    x->v = v;
    x->next = e->vars;
    e->vars = x;
    return x;
}


static int step(interp_t* I) {
    if (I->ctl == CTL_THROW) return 0;
    if ((++I->steps & 0x3FF) == 0 && I->yield_fn) I->yield_fn();   /* cheap: yields only after a time slice */
    if (I->steps > I->step_limit) {
        script_throw(I, "InternalError: script took too long (endless loop?)");
        return 0;
    }
    if (I->A->oom) {
        script_throw(I, "InternalError: out of memory");
        return 0;
    }
    return 1;
}

static value_t make_func(interp_t* I, node_t* f) {
    func_t* fn = (func_t*)arena_alloc(I->A, sizeof(func_t));
    fn->decl = f;
    fn->closure = I->cur;
    env_capture(I->cur);
    fn->module_url = I->module_url;
    fn->src_name = I->src_name;
    fn->name = f->s ? f->s->s : "anonymous";
    if (f->c == (node_t*)1) { fn->has_bound = 1; fn->bound_this = I->this_v; }
    fn->is_async = f->n == 1;
    fn->parent = v_undef();
    fn->data = v_undef();
    /* code inside a class (arrows, nested functions) knows it, for super */
    if (I->cur_fn) {
        fn->cls = I->cur_fn->is_class ? I->cur_fn : I->cur_fn->cls;
        fn->home = I->cur_fn->home;
    }
    value_t v = v_undef();
    v.t = V_FUNC;
    v.f = fn;
    return v;
}

value_t call_value(interp_t* I, value_t fnv, value_t self, int argc, value_t* argv) {
    if (I->ctl == CTL_THROW) return v_undef();
    if (fnv.t != V_FUNC) { script_throw(I, "TypeError: not a function"); return v_undef(); }
    func_t* fn = fnv.f;
    if (++I->depth > I->depth_limit) {
        I->depth--;
        script_throw(I, "RangeError: too much recursion");
        return v_undef();
    }
    value_t r;
    if (fn->native) {
        func_t* saved_native = I->cur_native;
        I->cur_native = fn;
        r = fn->nf(I, self, argc, argv);
        I->cur_native = saved_native;
        I->depth--;
        return r;
    }
    node_t* d = fn->decl;
    if ((d->flags & NF_GEN) && I->lang == LANG_JS) {
        if (I->gen_force) I->gen_force = 0;              /* the generator running its body */
        else { I->depth--; return es_generator_new(I, fnv, self, argc, argv); }
    }
    env_t* saved_env = I->cur;
    env_t* saved_fn = I->fn_env;
    value_t saved_this = I->this_v;
    /* PHP functions see only their own variables (and superglobals);
     * PHP arrow functions (fn) and all JS functions close over their scope */
    env_t* e = env_new(I, (I->lang == LANG_PHP && !fn->has_bound) ? NULL : fn->closure, 1);
    I->cur = e;
    I->fn_env = e;
    I->this_v = fn->has_bound ? fn->bound_this : self;
    /* sloppy-mode JS: a plain call f() runs with this = the global object
     * (`this.x = ...`, `var g = this || self` in bundled scripts) */
    if (I->lang == LANG_JS && !fn->has_bound && (self.t == V_UNDEF || self.t == V_NULL))
        I->this_v = script_get_global(I, "globalThis");
    func_t* saved_cur_fn = I->cur_fn;
    const char* saved_module_url = I->module_url;
    if (fn->module_url) I->module_url = fn->module_url;   /* run later (timers, promises): still in its module */
    const char* saved_src = I->src_name;
    if (fn->src_name) I->src_name = fn->src_name;
    I->cur_fn = fn;
    int i = 0;
    if ((d->flags & NF_NAMED) && I->lang == LANG_JS) {    /* (function f(){ ... f() ... }) sees itself as f */
        value_t me = v_undef();
        me.t = V_FUNC;
        me.f = fn;
        env_define(I, e, d->s, me);
    }
    if (I->lang == LANG_JS && !fn->has_bound) {
        /* the arguments object, only for functions that can see it */
        if (!(d->flags & NF_CHECKED)) d->flags |= NF_CHECKED | (mentions_arguments(d->b, 0) ? NF_ARGUMENTS : 0);
        if (d->flags & NF_ARGUMENTS) {
            obj_t* args = obj_new(I, OBJ_ARRAY);
            for (int k = 0; k < argc; k++) arr_push(I, args, argv[k]);
            if (!I->s_arguments) I->s_arguments = str_new(I, "arguments", 9);
            env_define(I, e, I->s_arguments, v_obj(args));
        }
    }
    for (node_t* p = d->a; p && !I->ctl; p = p->next, i++) {
        value_t v;
        if (p->op == 'r') {                                  /* ...rest */
            obj_t* rest = obj_new(I, OBJ_ARRAY);
            for (int k = i; k < argc; k++) arr_push(I, rest, argv[k]);
            v = v_obj(rest);
        } else {
            v = i < argc ? argv[i] : v_undef();
        }
        if (I->lang == LANG_PHP) v = php_array_copy(I, v);
        if ((i >= argc || v.t == V_UNDEF) && p->a) v = eval(I, p->a);
        if (p->c) destructure(I, p->c, v, 'l');
        else env_define(I, e, p->s, v);
    }
    /* class constructors: a base class sets up its fields first; a derived
     * one when super() returns; the implicit constructor of a derived class
     * passes everything on */
    if (fn->is_class && !I->ctl) {
        if (fn->parent.t == V_UNDEF || fn->parent.t == V_NULL) init_fields(I, fn);
        else if (d->d == (node_t*)2) super_call(I, fn, argc, argv);
    }
    if (I->ctl) {
        /* a parameter default or destructuring threw */
    } else if (d->op == 1) {
        r = eval(I, d->b);
    } else {
        exec(I, d->b);
        r = v_undef();
        if (I->lang == LANG_PHP) r = v_null();
        if (I->ctl == CTL_RETURN) { r = I->ret; I->ctl = CTL_NONE; }
        else if (I->ctl == CTL_BREAK || I->ctl == CTL_CONTINUE) { I->ctl = CTL_NONE; I->label = NULL; }
    }
    if (I->ctl != CTL_THROW && d->op == 1) r = I->ctl ? v_undef() : r;
    if (fn->is_class && I->lang == LANG_JS && I->ctl != CTL_THROW && r.t != V_OBJ) r = I->this_v;
    env_release(I, e);
    I->cur = saved_env;
    I->fn_env = saved_fn;
    I->this_v = saved_this;
    I->cur_fn = saved_cur_fn;
    I->module_url = saved_module_url;
    I->src_name = saved_src;
    I->depth--;
    if (fn->is_async) {
        /* an async function returns a promise of its result (or of its exception) */
        value_t p = es_promise_new(I);
        if (I->ctl == CTL_THROW) {
            value_t ex = I->ret;
            I->ctl = CTL_NONE;
            if (!(ex.t == V_STR && strcmp(ex.s->s, "__exit__") == 0)) { es_promise_settle(I, p, 1, ex); return p; }
            I->ctl = CTL_THROW;
            I->throw_line = I->line; I->throw_col = I->col; I->throw_src = I->src_name;
            I->ret = ex;
            return p;
        }
        es_promise_settle(I, p, 0, r);
        return p;
    }
    return r;
}

/* ── classes ── */

/* runs a class's instance field initializers on this */
static void init_fields(interp_t* I, func_t* cls) {
    if (I->this_v.t != V_OBJ) return;
    for (node_t* m = cls->fields; m && !I->ctl; m = m->next) {
        value_t v = m->b ? eval(I, m->b) : v_undef();
        if (I->ctl) return;
        const char* key;
        char kb[24];
        if (m->a) { value_t k = eval(I, m->a); if (I->ctl) return; key_of(I, k, kb, sizeof(kb), &key); }
        else key = m->s->s;
        obj_set(I, I->this_v.o, key, v);
    }
}

/* super(...args) inside constructor `cls`: the parent constructor runs on this */
static void super_call(interp_t* I, func_t* cls, int argc, value_t* argv) {
    value_t parent = cls->parent;
    if (parent.t != V_FUNC) { script_throw(I, "TypeError: super() without a parent class"); return; }
    if (parent.f->native) {
        /* built-in parents (Error, Map, ...): make one, adopt what it set */
        value_t marker = v_undef();
        marker.t = V_NULL;
        marker.b = 0x4E57;
        value_t made = call_value(I, parent, marker, argc, argv);
        if (I->ctl) return;
        if (made.t == V_OBJ && made.o->kind == OBJ_HOST) {
            I->this_v = made;                            /* new MyElement(): the element itself */
        } else if (made.t == V_OBJ && I->this_v.t == V_OBJ && made.o != I->this_v.o) {
            obj_t* me = I->this_v.o;
            for (uint32_t k = 0; k < made.o->n; k++) prop_set_raw(I, me, made.o->props[k].key, made.o->props[k].v);
            if (made.o->host) me->host = made.o->host;
        }
    } else {
        value_t made = call_value(I, parent, I->this_v, argc, argv);
        if (I->ctl) return;
        if (made.t == V_OBJ && made.o->kind == OBJ_HOST) I->this_v = made;
    }
    init_fields(I, cls);
}

/* property read with string/array/number methods */
value_t obj_getv(interp_t* I, value_t ov, const char* key) {
    int found = 0;
    if (ov.t == V_OBJ) {
        obj_t* o = ov.o;
        if (o->kind == OBJ_PHPARRAY) {
            value_t v = php_array_get(I, o, v_str(I, key), &found);
            return v;
        }
        value_t v = obj_get(I, o, key);
        if (v.t != V_UNDEF) return v;
        int has = 0;
        prop_get_raw(o, key, &has);
        if (has) return v;
        v = lib_member(I, ov, key, &found);
        return v;
    }
    if (ov.t == V_UNDEF || ov.t == V_NULL) {
        char buf[96];
        ksnprintf(buf, sizeof(buf), "TypeError: cannot read properties of %s (reading '%s')",
                  ov.t == V_NULL ? "null" : "undefined", key);
        script_throw(I, buf);
        return v_undef();
    }
    if (ov.t == V_STR) {
        if (strcmp(key, "length") == 0) return v_num(ov.s->len);
        uint32_t idx;
        if (key_index(key, &idx)) return idx < ov.s->len ? v_strn(I, ov.s->s + idx, 1) : v_undef();
    }
    if (ov.t == V_FUNC) {
        int f = 0;
        if (ov.f->statics) {
            value_t m = prop_get_raw(ov.f->statics, key, &f);
            if (f) return m;
        }
        if (strcmp(key, "prototype") == 0 && !ov.f->native) {   /* created on first use */
            if (!ov.f->statics) ov.f->statics = obj_new(I, OBJ_PLAIN);
            value_t p = v_obj(obj_new(I, OBJ_PLAIN));
            obj_set(I, p.o, "constructor", ov);           /* F.prototype.constructor === F */
            obj_set(I, ov.f->statics, "prototype", p);
            return p;
        }
        if (I->proto_func) {
            value_t m = prop_get_raw(I->proto_func, key, &f);
            if (f) return m;
        }
        if (strcmp(key, "name") == 0) return v_str(I, ov.f->name ? ov.f->name : "");
        if (strcmp(key, "constructor") == 0) return script_get_global(I, "Function");
        return v_undef();
    }
    return lib_member(I, ov, key, &found);
}

static value_t key_of(interp_t* I, value_t k, char* buf, int cap, const char** out) {
    if (k.t == V_NUM && k.n >= 0 && k.n < 4294967295.0L && num_floor(k.n) == k.n) {
        ksnprintf(buf, (size_t)cap, "%u", (uint32_t)k.n);
        *out = buf;
        return k;
    }
    str_t* s = v_tostr(I, k);
    *out = s->s;
    return k;
}

/* where an assignment writes: variable or property */
static void assign_to(interp_t* I, node_t* target, value_t v) {
    if (target->k == N_ARRAY || target->k == N_OBJECT) { destructure(I, target, v, 0); return; }
    if (target->k == N_IDENT) {
        var_t* x = env_lookup(I, I->cur, target->s->s);
        if (x) {
            if (x->is_const) { script_throw(I, "TypeError: assignment to constant variable"); return; }
            x->v = v;
            return;
        }
        /* JS: implicit global; PHP: local to the function */
        env_define(I, I->lang == LANG_PHP ? (I->fn_env ? I->fn_env : I->global) : I->global, target->s, v);
        return;
    }
    if (target->k == N_MEMBER || target->k == N_INDEX) {
        value_t ov;
        if (I->lang == LANG_PHP && target->a->k == N_IDENT) {
            /* $a['x'] = 1 creates the array */
            var_t* x = env_lookup(I, I->cur, target->a->s->s);
            if (!x || x->v.t == V_NULL || x->v.t == V_UNDEF) {
                value_t arr = v_obj(obj_new(I, OBJ_PHPARRAY));
                if (x) x->v = arr;
                else env_define(I, I->fn_env ? I->fn_env : I->global, target->a->s, arr);
            }
        }
        if (I->lang == LANG_PHP && (target->a->k == N_INDEX || target->a->k == N_MEMBER)) {
            /* $a['x']['y'] = 1: create the inner array on the way */
            value_t inner = eval(I, target->a);
            if (I->ctl) return;
            if (inner.t != V_OBJ) {
                inner = v_obj(obj_new(I, OBJ_PHPARRAY));
                assign_to(I, target->a, inner);
            }
            ov = inner;
        } else {
            ov = eval(I, target->a);
        }
        if (I->ctl) return;
        if (ov.t == V_FUNC && target->k == N_MEMBER) {      /* F.prototype = ..., F.x = ... */
            if (!ov.f->statics) ov.f->statics = obj_new(I, OBJ_PLAIN);
            obj_set(I, ov.f->statics, target->s->s, v);
            return;
        }
        if (ov.t != V_OBJ) {
            if (ov.t == V_UNDEF || ov.t == V_NULL) {
                script_throw(I, "TypeError: cannot set properties of undefined");
            }
            return;
        }
        if (ov.o->kind == OBJ_PHPARRAY) {
            if (target->k == N_INDEX && !target->b) { php_array_push(I, ov.o, v); return; }
            value_t k = target->k == N_MEMBER ? v_strv(target->s) : eval(I, target->b);
            if (I->ctl) return;
            php_array_set(I, ov.o, k, v);
            return;
        }
        if (target->k == N_INDEX && !target->b) { arr_push(I, ov.o, v); return; }
        const char* key;
        char kb[24];
        if (target->k == N_MEMBER) key = target->s->s;
        else {
            value_t k = eval(I, target->b);
            if (I->ctl) return;
            key_of(I, k, kb, sizeof(kb), &key);
        }
        obj_set(I, ov.o, key, v);
    }
}

/* binds one destructuring target: a name, a nested pattern, a property, with an optional default */
static void bind_target(interp_t* I, node_t* t, value_t v, int kind) {
    if (t->k == N_ASSIGN && t->op == '=') {
        if (v.t == V_UNDEF) { v = eval(I, t->b); if (I->ctl) return; }
        t = t->a;
    }
    if (t->k == N_ARRAY || t->k == N_OBJECT) { destructure(I, t, v, kind); return; }
    if (t->k == N_IDENT && kind) {
        env_t* target = kind == 'v' ? I->fn_env : I->cur;
        if (!target) target = I->global;
        var_t* x = env_define(I, target, t->s, v);
        x->is_const = kind == 'c';
        return;
    }
    assign_to(I, t, v);
}

/* [a, b, ...rest] = v / {a, b: c, ...rest} = v; kind 0 assigns, 'v' 'l' 'c' declare */
static void destructure(interp_t* I, node_t* pat, value_t v, int kind) {
    if (pat->k == N_ARRAY) {
        obj_t* arr = es_to_array(I, v);
        if (!arr) { script_throw(I, "TypeError: value is not iterable"); return; }
        uint32_t i = 0;
        for (node_t* e = pat->a; e && !I->ctl; e = e->next, i++) {
            if (e->k == N_EMPTY) continue;
            if (e->k == N_SPREAD) {
                obj_t* rest = obj_new(I, OBJ_ARRAY);
                for (uint32_t k = i; k < arr->len; k++) arr_push(I, rest, arr->items[k]);
                bind_target(I, e->a, v_obj(rest), kind);
                return;
            }
            bind_target(I, e, i < arr->len ? arr->items[i] : v_undef(), kind);
        }
        return;
    }
    if (v.t == V_UNDEF || v.t == V_NULL) { script_throw(I, "TypeError: cannot destructure undefined"); return; }
    const char* used[64];
    int nused = 0;
    for (node_t* p = pat->a; p && !I->ctl; p = p->next) {
        if (p->k == N_SPREAD) {
            obj_t* rest = obj_new(I, OBJ_PLAIN);
            obj_t* hidden = v.t == V_OBJ ? prop_hidden_names(v.o) : NULL;
            if (v.t == V_OBJ)
                for (uint32_t k = 0; k < v.o->n; k++) {
                    int skip = !prop_enumerable(v.o, hidden, k);
                    for (int u = 0; u < nused; u++) if (strcmp(used[u], v.o->props[k].key->s) == 0) skip = 1;
                    if (!skip) prop_set_raw(I, rest, v.o->props[k].key, v.o->props[k].v);
                }
            bind_target(I, p->a, v_obj(rest), kind);
            return;
        }
        const char* key;
        char kb[24];
        if (p->a) { value_t k = eval(I, p->a); if (I->ctl) return; key_of(I, k, kb, sizeof(kb), &key); key = arena_strdup(I->A, key, (uint32_t)strlen(key)); }
        else key = p->s->s;
        if (nused < 64) used[nused++] = key;
        value_t pv = obj_getv(I, v, key);
        if (I->ctl) return;
        bind_target(I, p->b, pv, kind);
    }
}

static num_t num_pow(num_t a, num_t b);
static int32_t to_i32(num_t x) {
    if (num_isnan(x) || x >= 9.2e18L || x <= -9.2e18L) return 0;
    return (int32_t)(uint32_t)(int64_t)x;
}

static value_t binary(interp_t* I, int op, value_t a, value_t b) {
    int php = I->lang == LANG_PHP;
    if ((a.t == V_OBJ && a.o->kind == OBJ_BIGINT) || (b.t == V_OBJ && b.o->kind == OBJ_BIGINT)) {
        int handled;
        value_t r = bi_binary(I, op, a, b, &handled);
        if (handled) return r;
    }
    switch (op) {
    case OP_ADD:
        if (php && a.t == V_OBJ && b.t == V_OBJ && a.o->kind == OBJ_PHPARRAY) {   /* array union */
            value_t r = php_array_copy(I, a);
            for (uint32_t i = 0; i < b.o->n; i++) {
                int f;
                prop_get_raw(r.o, b.o->props[i].key->s, &f);
                if (!f) prop_set_raw(I, r.o, b.o->props[i].key, b.o->props[i].v);
            }
            return r;
        }
        if (!php && (a.t == V_STR || b.t == V_STR || a.t == V_OBJ || b.t == V_OBJ))
            return v_strv(str_cat(I, v_tostr(I, a), v_tostr(I, b)));
        return v_num(v_tonum(I, a) + v_tonum(I, b));
    case OP_CONCAT: return v_strv(str_cat(I, v_tostr(I, a), v_tostr(I, b)));
    case OP_SUB: return v_num(v_tonum(I, a) - v_tonum(I, b));
    case OP_MUL: return v_num(v_tonum(I, a) * v_tonum(I, b));
    case OP_DIV: {
        num_t d = v_tonum(I, b);
        if (php && d == 0) { script_throw(I, "DivisionByZeroError: division by zero"); return v_undef(); }
        return v_num(v_tonum(I, a) / d);
    }
    case OP_MOD: {
        num_t x = v_tonum(I, a), y = v_tonum(I, b);
        if (php) {
            int64_t xi = (int64_t)x, yi = (int64_t)y;
            if (yi == 0) { script_throw(I, "DivisionByZeroError: modulo by zero"); return v_undef(); }
            return v_num((num_t)(xi % yi));
        }
        if (y == 0 || num_isnan(x) || num_isnan(y)) { num_t z = 0; return v_num(z / z); }
        num_t q = x / y;
        q = q < 0 ? -num_floor(-q) : num_floor(q);
        return v_num(x - q * y);
    }
    case OP_POW: return v_num(num_pow(v_tonum(I, a), v_tonum(I, b)));
    case OP_EQ:  return v_bool(v_loose_eq(I, a, b));
    case OP_NE:  return v_bool(!v_loose_eq(I, a, b));
    case OP_SEQ: return v_bool(v_strict_eq(a, b));
    case OP_SNE: return v_bool(!v_strict_eq(a, b));
    case OP_LT: case OP_GT: case OP_LE: case OP_GE: case OP_SPACESHIP: {
        int c;
        if (a.t == V_STR && b.t == V_STR && !(php && str_is_numeric(a.s->s) && str_is_numeric(b.s->s))) {
            c = strcmp(a.s->s, b.s->s);
            c = c < 0 ? -1 : c > 0 ? 1 : 0;
        } else {
            num_t x = v_tonum(I, a), y = v_tonum(I, b);
            if (num_isnan(x) || num_isnan(y)) return op == OP_SPACESHIP ? v_num(1) : v_bool(0);
            c = x < y ? -1 : x > y ? 1 : 0;
        }
        if (op == OP_SPACESHIP) return v_num(c);
        if (op == OP_LT) return v_bool(c < 0);
        if (op == OP_GT) return v_bool(c > 0);
        if (op == OP_LE) return v_bool(c <= 0);
        return v_bool(c >= 0);
    }
    case OP_BAND: return v_num(to_i32(v_tonum(I, a)) & to_i32(v_tonum(I, b)));
    case OP_BOR:  return v_num(to_i32(v_tonum(I, a)) | to_i32(v_tonum(I, b)));
    case OP_BXOR: return v_num(to_i32(v_tonum(I, a)) ^ to_i32(v_tonum(I, b)));
    case OP_SHL:  return v_num((int32_t)((uint32_t)to_i32(v_tonum(I, a)) << (to_i32(v_tonum(I, b)) & 31)));
    case OP_SHR:  return v_num(to_i32(v_tonum(I, a)) >> (to_i32(v_tonum(I, b)) & 31));
    case OP_USHR: return v_num((num_t)((uint32_t)to_i32(v_tonum(I, a)) >> (to_i32(v_tonum(I, b)) & 31)));
    case OP_INSTANCEOF: return v_bool(es_instanceof(I, a, b));
    case OP_IN: {
        if (b.t != V_OBJ && b.t != V_FUNC) { script_throw(I, "TypeError: 'in' needs an object"); return v_undef(); }
        str_t* k = v_tostr(I, a);
        if (b.t == V_FUNC) return v_bool(b.f->statics && (prop_get_raw(b.f->statics, k->s, &(int){0}), 1) && obj_get(I, b.f->statics, k->s).t != V_UNDEF);
        obj_t* o = b.o;
        if (o->kind == OBJ_ARRAY) {
            uint32_t idx;
            if (key_index(k->s, &idx)) return v_bool(idx < o->len);
            if (strcmp(k->s, "length") == 0) return v_bool(1);
        }
        if (es_proxy_target(o)) return v_bool(es_proxy_has(I, o, k->s));
        if (o->kind == OBJ_HOST && o->hc && o->hc->get) { int f = 0; o->hc->get(I, o, k->s, &f); if (f) return v_bool(1); }
        for (obj_t* p = o; p; p = p->proto) { int f = 0; prop_get_raw(p, k->s, &f); if (f) return v_bool(1); }
        int f = 0;
        lib_member(I, b, k->s, &f);
        return v_bool(f);
    }
    case OP_NULLISH: return (a.t == V_UNDEF || a.t == V_NULL) ? b : a;
    }
    return v_undef();
}

static num_t num_pow(num_t a, num_t b) {
    if (num_floor(b) == b && b >= -1000 && b <= 1000) {
        num_t r = 1;
        int64_t e = (int64_t)b;
        int neg = e < 0;
        if (neg) e = -e;
        num_t base = a;
        while (e) {
            if (e & 1) r *= base;
            base *= base;
            e >>= 1;
        }
        return neg ? 1 / r : r;
    }
    /* a^b = 2^(b * log2 a) on the x87 */
    if (a < 0) { num_t z = 0; return z / z; }
    if (a == 0) return 0;
    num_t r;
    __asm__ volatile(
        "fyl2x\n\t"              /* st0 = y = b * log2(a) */
        "fld %%st(0)\n\t"
        "frndint\n\t"            /* st0 = n = round(y), st1 = y */
        "fxch\n\t"               /* st0 = y, st1 = n */
        "fsub %%st(1), %%st\n\t" /* st0 = y - n (destination st0: no AT&T operand swap) */
        "f2xm1\n\t"
        "fld1\n\t"
        "faddp\n\t"
        "fscale\n\t"
        "fstp %%st(1)\n\t"
        : "=t"(r) : "0"(a), "u"(b) : "st(1)");
    return r;
}

static value_t eval_member_value(interp_t* I, node_t* n, value_t* self_out) {
    value_t ov = eval(I, n->a);
    if (I->ctl) return v_undef();
    if (self_out) *self_out = ov;
    if (n->k == N_MEMBER) {
        if (n->op == 1 && (ov.t == V_UNDEF || ov.t == V_NULL)) return v_undef();
        return obj_getv(I, ov, n->s->s);
    }
    if (!n->b) { script_throw(I, "Error: cannot use [] for reading"); return v_undef(); }
    if (n->op == 1 && (ov.t == V_UNDEF || ov.t == V_NULL)) return v_undef();
    value_t k = eval(I, n->b);
    if (I->ctl) return v_undef();
    if (ov.t == V_OBJ && ov.o->kind == OBJ_PHPARRAY) {
        int f;
        return php_array_get(I, ov.o, k, &f);
    }
    if (ov.t == V_OBJ && ov.o->kind == OBJ_ARRAY && k.t == V_NUM && k.n >= 0 && num_floor(k.n) == k.n)
        return arr_get(ov.o, (uint32_t)k.n);
    if (I->lang == LANG_PHP && ov.t == V_STR) {
        num_t i = v_tonum(I, k);
        if (i < 0) i += ov.s->len;
        if (i >= 0 && i < ov.s->len) return v_strn(I, ov.s->s + (uint32_t)i, 1);
        return v_str(I, "");
    }
    if (I->lang == LANG_PHP && (ov.t == V_NULL || ov.t == V_UNDEF)) return v_null();
    const char* key;
    char kb[24];
    key_of(I, k, kb, sizeof(kb), &key);
    return obj_getv(I, ov, key);
}

/* evaluates call arguments (with ...spread) into *out; returns the count */
static int eval_args(interp_t* I, node_t* list, value_t* small, int small_cap, value_t** out) {
    int cnt = 0, spread = 0;
    for (node_t* a = list; a; a = a->next) { cnt++; if (a->k == N_SPREAD) spread = 1; }
    value_t* buf = small;
    int cap = small_cap;
    if (cnt > small_cap || spread) { cap = cnt + 16; buf = (value_t*)arena_alloc(I->A, (uint32_t)cap * (uint32_t)sizeof(value_t)); }
    int n = 0;
    for (node_t* a = list; a; a = a->next) {
        if (a->k == N_SPREAD) {
            value_t v = eval(I, a->a);
            if (I->ctl) return 0;
            obj_t* arr = es_to_array(I, v);
            if (!arr) { script_throw(I, "TypeError: spread of a value that is not iterable"); return 0; }
            if (n + (int)arr->len > cap) {
                int nc = n + (int)arr->len + 16;
                value_t* nb = (value_t*)arena_alloc(I->A, (uint32_t)nc * (uint32_t)sizeof(value_t));
                memcpy(nb, buf, (size_t)n * sizeof(value_t));
                buf = nb;
                cap = nc;
            }
            for (uint32_t k = 0; k < arr->len; k++) buf[n++] = arr->items[k];
            continue;
        }
        if (n >= cap) break;
        buf[n++] = eval(I, a);
        if (I->ctl) return 0;
    }
    *out = buf;
    return n;
}

/* super.x: looked up above the object the running method belongs to */
static value_t super_lookup(interp_t* I, node_t* n) {
    obj_t* home = I->cur_fn ? I->cur_fn->home : NULL;
    const char* key;
    char kb[24];
    if (n->a) { value_t k = eval(I, n->a); if (I->ctl) return v_undef(); key_of(I, k, kb, sizeof(kb), &key); }
    else key = n->s->s;
    if (!home || !home->proto) return v_undef();
    obj_t* above = home->proto;
    int f = 0;
    for (obj_t* p = above; p; p = p->proto) {
        value_t v = prop_get_raw(p, key, &f);
        if (f) return (v.t == V_OBJ && v.o->kind == OBJ_ACCESSOR) ? accessor_get(I, v, I->this_v) : v;
    }
    /* built-in parents: their methods (toString, ...) */
    return lib_member(I, I->this_v, key, &f);
}

static value_t eval_call(interp_t* I, node_t* n) {
    value_t self = v_undef();
    value_t fn;
    node_t* c = n->a;
    if (c->k == N_SUPERMEMBER) {
        fn = super_lookup(I, c);
        if (I->ctl) return v_undef();
        if (fn.t != V_FUNC) { script_throw(I, "TypeError: super method is not a function"); return v_undef(); }
        value_t small[16];
        value_t* argv;
        int argc = eval_args(I, n->b, small, 16, &argv);
        if (I->ctl) return v_undef();
        return call_value(I, fn, I->this_v, argc, argv);
    }
    if (I->lang == LANG_PHP && c->k == N_IDENT && c->op == 1) {
        const char* name = c->s->s;
        /* special forms that must not evaluate undefined variables strictly */
        if (strcasecmp(name, "isset") == 0 || strcasecmp(name, "empty") == 0) {
            int all = 1;
            for (node_t* a = n->b; a; a = a->next) {
                value_t v;
                if (a->k == N_IDENT) {
                    var_t* x = env_lookup(I, I->cur, a->s->s);
                    v = x ? x->v : v_null();
                } else {
                    v = eval(I, a);
                    if (I->ctl == CTL_THROW) { I->ctl = CTL_NONE; v = v_null(); }
                }
                if (strcasecmp(name, "empty") == 0) return v_bool(!v_truthy(I, v));
                if (v.t == V_NULL || v.t == V_UNDEF) all = 0;
            }
            return v_bool(all);
        }
        if (strcasecmp(name, "unset") == 0) {
            for (node_t* a = n->b; a; a = a->next) {
                if (a->k == N_IDENT) {
                    var_t* x = env_lookup(I, I->cur, a->s->s);
                    if (x) x->v = v_null();
                } else if (a->k == N_INDEX && a->b) {
                    value_t ov = eval(I, a->a);
                    value_t k = eval(I, a->b);
                    if (I->ctl) return v_undef();
                    if (ov.t == V_OBJ) {
                        int64_t ik;
                        int ii;
                        str_t* ks = php_key(I, k, &ik, &ii);
                        prop_del(ov.o, ks->s);
                    }
                }
            }
            return v_null();
        }
        /* user function? (PHP function names are case-insensitive) */
        char lname[64];
        uint32_t ln = 0;
        lname[ln++] = '(';
        for (const char* p = name; *p && ln < sizeof(lname) - 1; p++)
            lname[ln++] = (char)((*p >= 'A' && *p <= 'Z') ? *p + 32 : *p);
        lname[ln] = 0;
        var_t* f = env_find_local(I->global, lname);
        value_t argv[16];
        int argc = 0;
        for (node_t* a = n->b; a && argc < 16; a = a->next) {
            argv[argc++] = eval(I, a);
            if (I->ctl) return v_undef();
        }
        if (f) return call_value(I, f->v, v_undef(), argc, argv);
        int found = 0;
        value_t r = php_call_builtin(I, name, argc, argv, &found);
        if (!found && !I->ctl) throwf(I, "Error: Call to undefined function %s()", name);
        return r;
    }
    if (c->k == N_MEMBER || c->k == N_INDEX) {
        fn = eval_member_value(I, c, &self);
        if (I->ctl) return v_undef();
        if (fn.t != V_FUNC) {
            if (c->k == N_MEMBER && c->op == 1 && (self.t == V_UNDEF || self.t == V_NULL)) return v_undef();
            if (n->op == 1 && (fn.t == V_UNDEF || fn.t == V_NULL)) return v_undef();
            throwf(I, "TypeError: %s is not a function", c->k == N_MEMBER ? c->s->s : "value");
            return v_undef();
        }
    } else {
        fn = eval(I, c);
        if (I->ctl) return v_undef();
        if (fn.t != V_FUNC) {
            if (n->op == 1 && (fn.t == V_UNDEF || fn.t == V_NULL)) return v_undef();   /* f?.() */
            throwf(I, "TypeError: %s is not a function", c->k == N_IDENT ? c->s->s : "value");
            return v_undef();
        }
    }
    value_t small[16];
    value_t* argv;
    int argc = eval_args(I, n->b, small, 16, &argv);
    if (I->ctl) return v_undef();
    return call_value(I, fn, self, argc, argv);
}

static node_t g_default_ctor_body;                 /* an empty block */

/* class X extends Y { ... }: a constructor function with a prototype of methods */
static value_t eval_class(interp_t* I, node_t* n) {
    value_t parent = v_undef();
    if (n->a) {
        parent = eval(I, n->a);
        if (I->ctl) return v_undef();
        if (parent.t != V_FUNC && parent.t != V_NULL) { script_throw(I, "TypeError: class extends a value that is not a constructor"); return v_undef(); }
    }
    node_t* ctor = NULL;
    for (node_t* m = n->b; m; m = m->next)
        if (m->op == 'm' && !m->n && m->s && strcmp(m->s->s, "constructor") == 0) ctor = m->b;
    if (!ctor) {
        ctor = (node_t*)arena_alloc(I->A, sizeof(node_t));
        ctor->k = N_FUNC;
        g_default_ctor_body.k = N_BLOCK;
        ctor->b = &g_default_ctor_body;
        if (parent.t == V_FUNC) ctor->d = (node_t*)2;       /* pass the arguments to super() */
    }
    ctor->s = n->s;
    value_t F = make_func(I, ctor);
    func_t* fn = F.f;
    fn->is_class = 1;
    fn->parent = parent;
    fn->cls = NULL;
    if (n->s) fn->name = n->s->s;
    obj_t* proto = obj_new(I, OBJ_PLAIN);
    fn->statics = obj_new(I, OBJ_PLAIN);
    if (parent.t == V_FUNC) {
        value_t pp = obj_getv(I, parent, "prototype");
        if (pp.t == V_OBJ) proto->proto = pp.o;
        if (parent.f->statics) fn->statics->proto = parent.f->statics;    /* static inheritance */
    }
    fn->home = proto;
    obj_set(I, fn->statics, "prototype", v_obj(proto));
    prop_set_raw(I, proto, str_new(I, "constructor", 11), F);
    /* the class's own name is visible inside it */
    env_t* saved = I->cur;
    I->cur = env_new(I, saved, 0);
    if (n->s) env_define(I, I->cur, n->s, F);
    fn->closure = I->cur;
    env_capture(I->cur);
    func_t* saved_fn = I->cur_fn;
    value_t saved_this = I->this_v;
    node_t* fields = NULL;
    node_t** ftail = &fields;
    for (node_t* m = n->b; m && !I->ctl; m = m->next) {
        obj_t* target = m->n ? fn->statics : proto;
        if (m->op == 'b') {                                   /* static { } */
            I->cur_fn = fn;
            I->this_v = F;
            exec(I, m->b);
            continue;
        }
        if (m->op == 'f') {
            if (!m->n) {                                      /* instance field: copied per object */
                node_t* copy = (node_t*)arena_alloc(I->A, sizeof(node_t));
                *copy = *m;
                copy->next = NULL;
                *ftail = copy;
                ftail = &copy->next;
                continue;
            }
            I->cur_fn = fn;
            I->this_v = F;
            value_t v = m->b ? eval(I, m->b) : v_undef();
            if (I->ctl) break;
            obj_set(I, target, m->s ? m->s->s : "", v);
            continue;
        }
        if (m->b == ctor) continue;
        const char* key;
        char kb[24];
        if (m->a) { value_t k = eval(I, m->a); if (I->ctl) break; key_of(I, k, kb, sizeof(kb), &key); key = arena_strdup(I->A, key, (uint32_t)strlen(key)); }
        else key = m->s->s;
        I->cur_fn = fn;
        value_t mv = make_func(I, m->b);
        mv.f->home = target;
        mv.f->cls = fn;
        if (m->op == 'g' || m->op == 's') {
            int f = 0;
            value_t acc = prop_get_raw(target, key, &f);
            if (!f || acc.t != V_OBJ || acc.o->kind != OBJ_ACCESSOR) {
                acc = v_obj(obj_new(I, OBJ_PLAIN));
                acc.o->kind = OBJ_ACCESSOR;
                prop_set_raw(I, target, str_new(I, key, (uint32_t)strlen(key)), acc);
            }
            prop_set_raw(I, acc.o, str_new(I, m->op == 'g' ? "get" : "set", 3), mv);
        } else {
            prop_set_raw(I, target, str_new(I, key, (uint32_t)strlen(key)), mv);
        }
    }
    fn->fields = fields;
    I->cur_fn = saved_fn;
    I->this_v = saved_this;
    I->cur = saved;
    if (I->ctl) return v_undef();
    if (n->op == 1 && n->s) {                                 /* a declaration: let-like binding */
        var_t* x = env_define(I, I->cur ? I->cur : I->global, n->s, F);
        (void)x;
    }
    return F;
}

static value_t eval(interp_t* I, node_t* n) {
    if (!n || !step(I)) return v_undef();
    I->col = n->col;
    I->line = n->line;
    switch (n->k) {
    case N_NUM: return n->s ? bi_literal(I, n) : v_num(n->n);
    case N_STR: return v_strv(n->s);
    case N_TRUE: return v_bool(1);
    case N_FALSE: return v_bool(0);
    case N_NULL: return v_null();
    case N_UNDEF: return v_undef();
    case N_THIS: return I->this_v;
    case N_IDENT: {
        if (I->lang == LANG_PHP && n->op == 1) {            /* constant */
            const char* c = n->s->s;
            if (strcmp(c, "PHP_EOL") == 0) return v_str(I, "\n");
            if (strcmp(c, "PHP_VERSION") == 0) return v_str(I, "8.3.0-banana");
            if (strcmp(c, "M_PI") == 0) return v_num(3.14159265358979323846L);
            if (strcmp(c, "FILE_APPEND") == 0) return v_num(8);
            if (strcmp(c, "LOCK_EX") == 0) return v_num(2);
            if (strcmp(c, "PHP_INT_MAX") == 0) return v_num(9223372036854775807.0L);
            throwf(I, "Error: Undefined constant \"%s\"", c);
            return v_undef();
        }
        var_t* x = env_lookup(I, I->cur, n->s->s);
        if (x) return x->v;
        if (I->lang == LANG_PHP) return v_null();     /* warning in PHP, not fatal */
        if (strcmp(n->s->s, "NaN") == 0) { num_t z = 0; return v_num(z / z); }
        if (strcmp(n->s->s, "Infinity") == 0) return v_num(num_inf());
        throwf(I, "ReferenceError: %s is not defined", n->s->s);
        return v_undef();
    }
    case N_ARRAY: {
        if (I->lang == LANG_PHP) {
            obj_t* o = obj_new(I, OBJ_PHPARRAY);
            for (node_t* p = n->a; p; p = p->next) {
                value_t v = eval(I, p->b);
                if (I->ctl) return v_undef();
                if (p->a) {
                    value_t k = eval(I, p->a);
                    if (I->ctl) return v_undef();
                    php_array_set(I, o, k, v);
                } else {
                    php_array_push(I, o, v);
                }
            }
            return v_obj(o);
        }
        obj_t* o = obj_new(I, OBJ_ARRAY);
        o->proto = NULL;
        for (node_t* p = n->a; p; p = p->next) {
            if (p->k == N_EMPTY) { arr_push(I, o, v_undef()); continue; }
            if (p->k == N_SPREAD) {
                value_t sv = eval(I, p->a);
                if (I->ctl) return v_undef();
                obj_t* items = es_to_array(I, sv);
                if (!items) { script_throw(I, "TypeError: spread of a value that is not iterable"); return v_undef(); }
                for (uint32_t k = 0; k < items->len; k++) arr_push(I, o, items->items[k]);
                continue;
            }
            value_t v = eval(I, p);
            if (I->ctl) return v_undef();
            arr_push(I, o, v);
        }
        return v_obj(o);
    }
    case N_OBJECT: {
        obj_t* o = obj_new(I, OBJ_PLAIN);
        for (node_t* p = n->a; p; p = p->next) {
            const char* key;
            char kb[24];
            if (p->k == N_SPREAD) {                             /* {...other} */
                value_t sv = eval(I, p->a);
                if (I->ctl) return v_undef();
                obj_t* pt = sv.t == V_OBJ ? es_proxy_target(sv.o) : NULL;
                if (pt) {
                    while (es_proxy_target(pt)) pt = es_proxy_target(pt);
                    for (uint32_t k = 0; k < pt->n; k++) prop_set_raw(I, o, pt->props[k].key, obj_get(I, sv.o, pt->props[k].key->s));
                } else if (sv.t == V_OBJ) {
                    if (sv.o->kind == OBJ_ARRAY)
                        for (uint32_t k = 0; k < sv.o->len; k++) { char b[16]; ksnprintf(b, sizeof(b), "%u", k); obj_set(I, o, b, sv.o->items[k]); }
                    obj_t* hidden = prop_hidden_names(sv.o);
                    for (uint32_t k = 0; k < sv.o->n; k++) {
                        if (!prop_enumerable(sv.o, hidden, k)) continue;
                        value_t pv = sv.o->props[k].v;
                        if (pv.t == V_OBJ && pv.o->kind == OBJ_ACCESSOR) pv = accessor_get(I, pv, sv);
                        prop_set_raw(I, o, sv.o->props[k].key, pv);
                    }
                }
                continue;
            }
            if (p->a) {
                value_t k = eval(I, p->a);
                if (I->ctl) return v_undef();
                key_of(I, k, kb, sizeof(kb), &key);
            } else {
                key = p->s->s;
            }
            value_t v = eval(I, p->b);
            if (I->ctl) return v_undef();
            if (v.t == V_FUNC && !v.f->native && p->b->k == N_FUNC) v.f->home = o;   /* super inside methods */
            if (p->op == 'g' || p->op == 's') {
                /* get x() {} / set x(v) {}: one accessor holds both */
                int f = 0;
                value_t acc = prop_get_raw(o, key, &f);
                if (!f || acc.t != V_OBJ || acc.o->kind != OBJ_ACCESSOR) {
                    acc = v_obj(obj_new(I, OBJ_PLAIN));
                    acc.o->kind = OBJ_ACCESSOR;
                    prop_set_raw(I, o, str_new(I, key, (uint32_t)strlen(key)), acc);
                }
                prop_set_raw(I, acc.o, str_new(I, p->op == 'g' ? "get" : "set", 3), v);
                continue;
            }
            obj_set(I, o, key, v);
        }
        return v_obj(o);
    }
    case N_REGEX: return es_regexp_new(I, n->s->s, n->s->len, n->a->s->s);
    case N_SEQ: {
        value_t v = v_undef();
        for (node_t* e = n->a; e; e = e->next) { v = eval(I, e); if (I->ctl) return v_undef(); }
        return v;
    }
    case N_CLASS: return eval_class(I, n);
    case N_SUPERCALL: {
        func_t* cls = I->cur_fn ? (I->cur_fn->is_class ? I->cur_fn : I->cur_fn->cls) : NULL;
        if (!cls) { script_throw(I, "SyntaxError: super() outside a constructor"); return v_undef(); }
        value_t small[16];
        value_t* argv;
        int argc = eval_args(I, n->b, small, 16, &argv);
        if (I->ctl) return v_undef();
        super_call(I, cls, argc, argv);
        return v_undef();
    }
    case N_SUPERMEMBER: return super_lookup(I, n);
    case N_YIELD: {
        value_t v = n->a ? eval(I, n->a) : v_undef();
        if (I->ctl || !I->gen_out) return v_undef();
        if (n->op == '*') {
            obj_t* a = es_to_array(I, v);
            if (I->ctl) return v_undef();
            if (!a) { script_throw(I, "TypeError: yield* of a value that is not iterable"); return v_undef(); }
            for (uint32_t i = 0; i < a->len; i++) arr_push(I, I->gen_out, a->items[i]);
        } else {
            arr_push(I, I->gen_out, v);
        }
        if (I->gen_out->len >= 10000) { I->ret = v_undef(); I->ctl = CTL_RETURN; }   /* endless generator: stop it */
        return v_undef();
    }
    case N_AWAIT: {
        value_t v = eval(I, n->a);
        if (I->ctl) return v_undef();
        value_t r;
        int st = es_promise_state(I, v, &r);
        if (st < 0) return v;                                 /* not a promise */
        if (st == 0) { es_run_jobs(I); st = es_promise_state(I, v, &r); }
        if (st == 2) { I->ret = r; I->ctl = CTL_THROW; I->throw_line = I->line; I->throw_col = I->col; I->throw_src = I->src_name; return v_undef(); }
        return st == 1 ? r : v_undef();                       /* still pending: cannot wait here */
    }
    case N_FUNC: return make_func(I, n);
    case N_MEMBER: case N_INDEX: return eval_member_value(I, n, NULL);
    case N_CALL: return eval_call(I, n);
    case N_NEW: {
        value_t fn = eval(I, n->a);
        if (I->ctl) return v_undef();
        value_t small[16];
        value_t* argv;
        int argc = eval_args(I, n->b, small, 16, &argv);
        if (I->ctl) return v_undef();
        if (fn.t != V_FUNC) {
            throwf(I, "TypeError: %s is not a constructor",
                   n->a->k == N_IDENT || (n->a->k == N_MEMBER && n->a->s) ? n->a->s->s : "value");
            return v_undef();
        }
        if (fn.f->native) {
            value_t marker = v_undef();
            marker.t = V_NULL;
            marker.b = 0x4E57;                           /* "new" */
            return call_value(I, fn, marker, argc, argv);
        }
        obj_t* o = obj_new(I, OBJ_PLAIN);
        value_t proto = obj_getv(I, fn, "prototype");
        if (proto.t == V_OBJ) o->proto = proto.o;
        value_t saved_nt = I->new_target;
        I->new_target = fn;
        value_t r = call_value(I, fn, v_obj(o), argc, argv);
        I->new_target = saved_nt;
        return r.t == V_OBJ ? r : v_obj(o);
    }
    case N_TYPEOF: {
        value_t v;
        if (n->a->k == N_IDENT) {
            var_t* x = env_lookup(I, I->cur, n->a->s->s);
            v = x ? x->v : v_undef();
        } else {
            v = eval(I, n->a);
            if (I->ctl) return v_undef();
        }
        static const char* const names[] = { "undefined", "object", "boolean", "number", "string", "object", "function" };
        if (bi_is(I, v)) return v_str(I, "bigint");
        return v_str(I, names[v.t]);
    }
    case N_UNARY: {
        if (n->op == 'D') {                              /* delete o.x */
            if (n->a->k == N_MEMBER) {
                value_t ov = eval(I, n->a->a);
                if (ov.t == V_OBJ) prop_del(ov.o, n->a->s->s);
            }
            return v_bool(1);
        }
        value_t v = eval(I, n->a);
        if (I->ctl) return v_undef();
        if (n->op != OP_NOT && bi_is(I, v)) return bi_unary(I, n->op, v);
        switch (n->op) {
        case OP_NOT: return v_bool(!v_truthy(I, v));
        case OP_NEG: return v_num(-v_tonum(I, v));
        case OP_PLUS: return v_num(v_tonum(I, v));
        case OP_BITNOT: return v_num(~to_i32(v_tonum(I, v)));
        }
        return v_undef();
    }
    case N_BINARY: {
        value_t a = eval(I, n->a);
        if (I->ctl) return v_undef();
        value_t b = eval(I, n->b);
        if (I->ctl) return v_undef();
        return binary(I, n->op, a, b);
    }
    case N_LOGIC: {
        value_t a = eval(I, n->a);
        if (I->ctl) return v_undef();
        int php = I->lang == LANG_PHP;
        if (n->op == OP_AND) {
            if (!v_truthy(I, a)) return php ? v_bool(0) : a;
            value_t b = eval(I, n->b);
            return php ? v_bool(v_truthy(I, b)) : b;
        }
        if (n->op == OP_OR) {
            if (v_truthy(I, a)) return php ? v_bool(1) : a;
            value_t b = eval(I, n->b);
            return php ? v_bool(v_truthy(I, b)) : b;
        }
        if (a.t != V_UNDEF && a.t != V_NULL) return a;      /* ?? */
        return eval(I, n->b);
    }
    case N_COND: {
        value_t c = eval(I, n->a);
        if (I->ctl) return v_undef();
        if (!n->b) return v_truthy(I, c) ? c : eval(I, n->c);   /* ?: */
        return v_truthy(I, c) ? eval(I, n->b) : eval(I, n->c);
    }
    case N_ASSIGN: {
        value_t v;
        if (n->op == '=') {
            v = eval(I, n->b);
            if (I->ctl) return v_undef();
            if (I->lang == LANG_PHP) v = php_array_copy(I, v);
        } else {
            value_t old;
            if (n->a->k == N_IDENT) {
                var_t* x = env_lookup(I, I->cur, n->a->s->s);
                if (x) old = x->v;
                else if (I->lang == LANG_PHP) old = v_null();
                else { throwf(I, "ReferenceError: %s is not defined", n->a->s->s); return v_undef(); }
            } else {
                old = eval(I, n->a);
            }
            if (I->ctl) return v_undef();
            if (n->op == OP_NULLISH && old.t != V_UNDEF && old.t != V_NULL) return old;
            if (n->op == OP_LOGOR_ASSIGN || n->op == OP_LOGAND_ASSIGN) {
                int t = v_truthy(I, old);
                if ((n->op == OP_LOGOR_ASSIGN) == (t != 0)) return old;
                v = eval(I, n->b);
                if (I->ctl) return v_undef();
                assign_to(I, n->a, v);
                return v;
            }
            value_t r = eval(I, n->b);
            if (I->ctl) return v_undef();
            v = n->op == OP_NULLISH ? r : binary(I, n->op, old, r);
            if (I->ctl) return v_undef();
        }
        assign_to(I, n->a, v);
        return v;
    }
    case N_UPDATE: {
        value_t old;
        if (n->a->k == N_IDENT) {
            var_t* x = env_lookup(I, I->cur, n->a->s->s);
            if (x) old = x->v;
            else if (I->lang == LANG_PHP) old = v_null();
            else { throwf(I, "ReferenceError: %s is not defined", n->a->s->s); return v_undef(); }
        } else {
            old = eval(I, n->a);
        }
        if (I->ctl) return v_undef();
        if (bi_is(I, old)) {
            value_t bv = bi_add_int(I, old, n->op == '+' ? 1 : -1);
            assign_to(I, n->a, bv);
            return n->n ? bv : old;
        }
        num_t o = v_tonum(I, old);
        value_t nv = v_num(n->op == '+' ? o + 1 : o - 1);
        assign_to(I, n->a, nv);
        return n->n ? nv : v_num(o);
    }
    case N_CAST: {
        value_t v = eval(I, n->a);
        if (I->ctl) return v_undef();
        const char* t = n->s->s;
        if (strncasecmp(t, "int", 3) == 0) {
            num_t x = v_tonum(I, v);
            return v_num(num_isnan(x) ? 0 : (x < 0 ? -num_floor(-x) : num_floor(x)));
        }
        if (strcasecmp(t, "float") == 0 || strcasecmp(t, "double") == 0) return v_num(v_tonum(I, v));
        if (strcasecmp(t, "string") == 0) return v_strv(v_tostr(I, v));
        if (strncasecmp(t, "bool", 4) == 0) return v_bool(v_truthy(I, v));
        if (strcasecmp(t, "array") == 0) {
            if (v.t == V_OBJ) return v;
            obj_t* o = obj_new(I, OBJ_PHPARRAY);
            if (v.t != V_NULL && v.t != V_UNDEF) php_array_push(I, o, v);
            return v_obj(o);
        }
        return v;
    }
    default:
        script_throw(I, "InternalError: bad expression");
        return v_undef();
    }
}

/* function declarations are usable before they appear */
static void hoist(interp_t* I, node_t* list, env_t* e) {
    for (node_t* s = list; s; s = s->next) {
        if (s->k == N_FUNCDECL) {
            node_t* f = s->a;
            env_t* saved = I->cur;
            I->cur = e;
            value_t fv = make_func(I, f);
            I->cur = saved;
            if (I->lang == LANG_PHP) {
                char lname[64];
                uint32_t ln = 0;
                lname[ln++] = '(';
                for (const char* p = f->s->s; *p && ln < sizeof(lname) - 1; p++)
                    lname[ln++] = (char)((*p >= 'A' && *p <= 'Z') ? *p + 32 : *p);
                lname[ln] = 0;
                env_define(I, I->global, str_new(I, lname, ln), fv);
            } else {
                env_define(I, e, f->s, fv);
            }
        } else if (I->lang == LANG_PHP && s->k == N_BLOCK && s->op == 1) {
            hoist(I, s->a, e);
        } else if (s->k == N_EXPORT && s->op != 'd' && s->a && s->a->k == N_FUNCDECL) {
            hoist(I, s->a, e);                       /* export function f() {} */
        }
    }
}

static void exec_list(interp_t* I, node_t* n) {
    for (; n && !I->ctl; n = n->next) exec(I, n);
}

static void out_str(interp_t* I, str_t* s) {
    if (I->out && s->len) I->out(I->out_ctx, s->s, s->len);
}

/* after a loop body ran: 1 = leave the loop (break, a labelled break/continue for an
 * outer loop, return, throw), 0 = go on (ctl cleared) */
/* does code under n make closures (which could capture a loop variable)? */
static int makes_closure(node_t* n, int depth) {
    if ((uintptr_t)n < 4096) return 0;
    for (; n; n = n->next) {
        if (n->k == N_FUNC || n->k == N_FUNCDECL || n->k == N_CLASS) return 1;
        if (depth > 200) return 1;
        if (makes_closure(n->a, depth + 1) || makes_closure(n->b, depth + 1) ||
            makes_closure(n->c, depth + 1) || makes_closure(n->d, depth + 1)) return 1;
    }
    return 0;
}

static int loop_exit(interp_t* I, node_t* loop) {
    if (I->ctl == CTL_BREAK || I->ctl == CTL_CONTINUE) {
        int mine = !I->label || (loop->s && strcmp(loop->s->s, I->label->s) == 0);
        if (!mine) return 1;
        int brk = I->ctl == CTL_BREAK;
        I->ctl = CTL_NONE;
        I->label = NULL;
        return brk;
    }
    return I->ctl != CTL_NONE;
}

static void foreach_body(interp_t* I, node_t* n, value_t key, value_t val, int* stop) {
    if (n->c) assign_to(I, n->c, key);
    if (n->b->k == N_ARRAY || n->b->k == N_OBJECT) {
        destructure(I, n->b, val, I->lang == LANG_JS ? 'l' : 0);
    } else if (n->b->k == N_IDENT) {
        if (I->lang == LANG_JS) env_define(I, I->cur, n->b->s, val);
        else assign_to(I, n->b, val);
    } else {
        assign_to(I, n->b, val);
    }
    if (I->ctl) { *stop = 1; return; }
    exec(I, n->d);
    if (loop_exit(I, n)) *stop = 1;
}

/* modules: the exported names' current values into the namespace object */
static void export_copy(interp_t* I, node_t* n) {
    if (!I->module_ns || n->op == 'd') return;
    for (node_t* b = n->b; b; b = b->next) {
        if (!b->b) continue;
        var_t* x = env_lookup(I, I->cur, b->b->s->s);
        if (x) obj_set(I, I->module_ns, b->s->s, x->v);
    }
}
static void export_pass(interp_t* I, node_t* list) {
    for (node_t* s = list; s; s = s->next)
        if (s->k == N_EXPORT) export_copy(I, s);
}

static void exec_import(interp_t* I, node_t* n) {
    if (!n->s) return;
    if (!I->import_fn) { script_throw(I, "SyntaxError: import is only available in pages"); return; }
    value_t ns = I->import_fn(I, I->import_ctx, n->s->s, I->module_url);
    if (I->ctl) return;
    if (ns.t != V_OBJ) ns = v_obj(obj_new(I, OBJ_PLAIN));
    for (node_t* b = n->a; b; b = b->next) {
        int all = b->s->len == 1 && b->s->s[0] == '*';
        if (n->op == 'r') {                          /* export ... from */
            if (!I->module_ns) continue;
            if (all && !b->b) {
                for (uint32_t i = 0; i < ns.o->n; i++)
                    if (strcmp(ns.o->props[i].key->s, "default") != 0)
                        prop_set_raw(I, I->module_ns, ns.o->props[i].key, ns.o->props[i].v);
            } else if (b->b) {
                obj_set(I, I->module_ns, b->b->s->s, all ? ns : obj_get(I, ns.o, b->s->s));
            }
        } else if (b->b) {
            env_define(I, I->cur, b->b->s, all ? ns : obj_get(I, ns.o, b->s->s));
        }
    }
}

static void exec(interp_t* I, node_t* n) {
    if (!n || !step(I)) return;
    I->line = n->line;
    I->col = n->col;
    switch (n->k) {
    case N_EMPTY: case N_FUNCDECL: return;
    case N_IMPORT: exec_import(I, n); return;
    case N_EXPORT:
        if (n->op == 'd') {
            value_t v = eval(I, n->a);
            if (!I->ctl && I->module_ns) obj_set(I, I->module_ns, "default", v);
            return;
        }
        if (n->a) exec(I, n->a);
        if (!I->ctl) export_copy(I, n);
        return;
    case N_VARHOIST: {
        env_t* target = I->fn_env ? I->fn_env : I->global;
        for (node_t* x = n->a; x; x = x->next)
            if (!env_find_local(target, x->s->s)) env_define(I, target, x->s, v_undef());
        return;
    }
    case N_EXPR: eval(I, n->a); return;
    case N_VAR: {
        value_t v = n->a ? eval(I, n->a) : v_undef();
        if (I->ctl) return;
        if (n->c) { destructure(I, n->c, v, n->op); return; }
        env_t* target = (n->op == 'v') ? I->fn_env : I->cur;
        if (!target) target = I->global;
        /* `var x;` with no value leaves x alone (bundles declare at the end
         * of a scope what they assigned higher up) */
        if (n->op == 'v' && !n->a && env_find_local(target, n->s->s)) return;
        var_t* x = env_define(I, target, n->s, v);
        x->is_const = (n->op == 'c');
        return;
    }
    case N_BLOCK: {
        if (n->op == 1 || I->lang == LANG_PHP) { exec_list(I, n->a); return; }
        env_t* saved = I->cur;
        I->cur = env_new(I, saved, 0);
        hoist(I, n->a, I->cur);
        exec_list(I, n->a);
        env_release(I, I->cur);
        I->cur = saved;
        return;
    }
    case N_IF: {
        value_t c = eval(I, n->a);
        if (I->ctl) return;
        if (v_truthy(I, c)) exec(I, n->b);
        else if (n->c) exec(I, n->c);
        return;
    }
    case N_WHILE:
        for (;;) {
            value_t c = eval(I, n->a);
            if (I->ctl || !v_truthy(I, c)) return;
            exec(I, n->b);
            if (loop_exit(I, n)) return;
        }
    case N_DOWHILE:
        for (;;) {
            exec(I, n->b);
            if (loop_exit(I, n)) return;
            value_t c = eval(I, n->a);
            if (I->ctl || !v_truthy(I, c)) return;
        }
    case N_FOR: {
        env_t* saved = I->cur;
        if (I->lang == LANG_JS) I->cur = env_new(I, saved, 0);
        if (n->a) exec(I, n->a);
        /* op bit 1: checked, bit 2: the body makes closures. Only then does
         * each iteration need its own copy of the let variables - copying
         * them every time would fill the page's memory in a long loop */
        if (I->lang == LANG_JS && !(n->op & 1)) n->op |= (uint8_t)(1 | (makes_closure(n->d, 0) ? 2 : 0));
        int per_iter = I->lang == LANG_JS && (n->op & 2) && I->cur->vars;
        while (!I->ctl) {
            if (n->b) {
                value_t c = eval(I, n->b);
                if (I->ctl || !v_truthy(I, c)) break;
            }
            if (per_iter) {
                /* a fresh copy of the loop variables per iteration (closures) */
                env_t* it = env_new(I, I->cur, 0);
                for (var_t* v = I->cur->vars; v; v = v->next) env_define(I, it, v->name, v->v);
                env_t* outer = I->cur;
                I->cur = it;
                exec(I, n->d);
                for (var_t* v = it->vars; v; v = v->next) {
                    var_t* o = env_find_local(outer, v->name->s);
                    if (o) o->v = v->v;
                }
                env_release(I, it);
                I->cur = outer;
            } else {
                exec(I, n->d);
            }
            if (loop_exit(I, n)) break;
            if (n->c) exec(I, n->c);
        }
        if (I->cur != saved) env_release(I, I->cur);
        I->cur = saved;
        return;
    }
    case N_FOREACH: {
        value_t it = eval(I, n->a);
        if (I->ctl) return;
        env_t* saved = I->cur;
        if (I->lang == LANG_JS) I->cur = env_new(I, saved, 0);
        int stop = 0;
        if (it.t == V_OBJ) {
            obj_t* o = it.o;
            if (n->op == 'i') while (es_proxy_target(o)) o = es_proxy_target(o);
            obj_t* iter = (n->op == 'o' && o->kind != OBJ_ARRAY) ? es_to_array(I, it) : NULL;
            if (iter) {
                for (uint32_t i = 0; i < iter->len && !stop; i++) foreach_body(I, n, v_num(i), iter->items[i], &stop);
            } else if (n->op == 'o' && o->kind == OBJ_ARRAY) {
                for (uint32_t i = 0; i < o->len && !stop; i++) foreach_body(I, n, v_num(i), o->items[i], &stop);
            } else if (n->op == 'i' && o->kind == OBJ_ARRAY) {
                for (uint32_t i = 0; i < o->len && !stop; i++) {
                    char b[16];
                    ksnprintf(b, sizeof(b), "%u", i);
                    foreach_body(I, n, v_undef(), v_str(I, b), &stop);
                }
            } else if (o->kind == OBJ_HOST && o->hc && o->hc->get) {
                int f = 0;
                value_t len = o->hc->get(I, o, "length", &f);
                uint32_t L = f ? (uint32_t)v_tonum(I, len) : 0;
                for (uint32_t i = 0; i < L && !stop; i++) {
                    char b[16];
                    ksnprintf(b, sizeof(b), "%u", i);
                    value_t item = obj_get(I, o, b);
                    foreach_body(I, n, v_num(i), n->op == 'i' ? v_str(I, b) : item, &stop);
                }
            } else {
                uint32_t cnt = o->n;
                obj_t* hidden = prop_hidden_names(o);
                for (uint32_t i = 0; i < cnt && i < o->n && !stop; i++) {
                    if (!prop_enumerable(o, hidden, i)) continue;
                    value_t key = v_strv(o->props[i].key);
                    if (o->kind == OBJ_PHPARRAY) {
                        uint32_t idx;
                        if (key_index(key.s->s, &idx)) key = v_num(idx);
                        else if (key.s->s[0] == '-' && key_index(key.s->s + 1, &idx)) key = v_num(-(num_t)idx);
                    }
                    if (n->op == 'i') foreach_body(I, n, v_undef(), key, &stop);
                    else foreach_body(I, n, key, o->props[i].v, &stop);
                }
            }
        } else if (it.t == V_STR && n->op == 'o') {
            for (uint32_t i = 0; i < it.s->len && !stop; i++)
                foreach_body(I, n, v_num(i), v_strn(I, it.s->s + i, 1), &stop);
        } else if (I->lang == LANG_PHP && it.t != V_NULL) {
            /* PHP warns and skips */
        } else if (I->lang == LANG_JS && n->op == 'o') {
            script_throw(I, "TypeError: value is not iterable");
        }
        if (I->cur != saved) env_release(I, I->cur);
        I->cur = saved;
        return;
    }
    case N_RETURN: {
        value_t r = n->a ? eval(I, n->a) : (I->lang == LANG_PHP ? v_null() : v_undef());
        if (I->ctl == CTL_THROW) return;             /* I->ret holds the exception */
        I->ret = r;
        I->ctl = CTL_RETURN;
        return;
    }
    case N_BREAK: I->ctl = CTL_BREAK; I->label = n->s; return;
    case N_CONTINUE: I->ctl = CTL_CONTINUE; I->label = n->s; return;
    case N_LABEL:
        exec(I, n->a);
        if ((I->ctl == CTL_BREAK || I->ctl == CTL_CONTINUE) && I->label && strcmp(I->label->s, n->s->s) == 0) {
            I->ctl = CTL_NONE;
            I->label = NULL;
        }
        return;
    case N_CLASS: eval(I, n); return;
    case N_THROW: {
        value_t v = eval(I, n->a);
        if (I->ctl) return;
        I->ret = v;
        I->ctl = CTL_THROW;
        I->throw_line = I->line; I->throw_col = I->col; I->throw_src = I->src_name;
        return;
    }
    case N_TRY: {
        exec(I, n->a);
        if (I->ctl == CTL_THROW && n->b) {
            value_t ex = I->ret;
            I->ctl = CTL_NONE;
            env_t* saved = I->cur;
            I->cur = env_new(I, saved, 0);
            if (n->s) env_define(I, I->cur, n->s, ex);
            exec(I, n->b);
            env_release(I, I->cur);
            I->cur = saved;
        }
        if (n->c) {
            int ctl = I->ctl;
            value_t ret = I->ret;
            I->ctl = CTL_NONE;
            exec(I, n->c);
            if (!I->ctl) { I->ctl = ctl; I->ret = ret; }
        }
        return;
    }
    case N_SWITCH: {
        value_t d = eval(I, n->a);
        if (I->ctl) return;
        node_t* start = NULL;
        for (node_t* c = n->b; c && !start; c = c->next) {
            if (!c->a) continue;
            value_t v = eval(I, c->a);
            if (I->ctl) return;
            if (I->lang == LANG_PHP ? v_loose_eq(I, d, v) : v_strict_eq(d, v)) start = c;
        }
        if (!start) for (node_t* c = n->b; c; c = c->next) if (!c->a) { start = c; break; }
        for (node_t* c = start; c && !I->ctl; c = c->next) exec(I, c->b);
        if (I->ctl == CTL_BREAK && !I->label) I->ctl = CTL_NONE;
        return;
    }
    case N_ECHO:
        for (node_t* e = n->a; e && !I->ctl; e = e->next) {
            value_t v = eval(I, e);
            if (I->ctl) return;
            out_str(I, v_tostr(I, v));
        }
        return;
    case N_HTML: out_str(I, n->s); return;
    case N_GLOBAL: {
        var_t* g = env_find_local(I->global, n->s->s);
        if (!g) g = env_define(I, I->global, n->s, v_null());
        if (I->cur && I->cur != I->global) {
            /* the local name becomes an alias of the global variable */
            var_t* x = (var_t*)arena_alloc(I->A, sizeof(var_t));
            x->name = n->s;
            x->ref = g;
            x->next = I->cur->vars;
            I->cur->vars = x;
        }
        return;
    }
    default:
        eval(I, n);
        return;
    }
}

/* ══ public API ═══════════════════════════════════════════════════════ */

interp_t* script_new(arena_t* A, int lang) {
    interp_t* I = (interp_t*)arena_alloc(A, sizeof(interp_t));
    I->A = A;
    I->lang = lang;
    I->global = env_new(I, NULL, 1);
    I->fn_env = I->global;
    I->step_limit = 20000000u;
    I->depth_limit = 150;
    I->this_v = v_undef();
    lib_init(I);
    /* made now: when memory runs out there is none left to make it */
    obj_t* oe = obj_new(I, OBJ_PLAIN);
    obj_set(I, oe, "name", v_str(I, "InternalError"));
    obj_set(I, oe, "message", v_str(I, "out of memory"));
    I->oom_err = oe;
    return I;
}

void script_set_output(interp_t* I, script_out_fn out, void* ctx) { I->out = out; I->out_ctx = ctx; }
void script_set_log(interp_t* I, script_out_fn log, void* ctx) { I->log = log; I->log_ctx = ctx; }
void script_set_host(interp_t* I, void* host) { I->host = host; }
void script_set_yield(interp_t* I, void (*fn)(void)) { I->yield_fn = fn; }
void* script_host(interp_t* I) { return I->host; }
void script_set_php_ext(interp_t* I, script_ext_fn fn) { I->php_ext = fn; }
void script_set_limits(interp_t* I, uint32_t steps, uint32_t depth) {
    if (steps) I->step_limit = steps;
    if (depth) I->depth_limit = depth;
}
const char* script_error(interp_t* I) { return I->err; }

void script_def_global(interp_t* I, const char* name, value_t v) {
    env_define(I, I->global, str_new(I, name, (uint32_t)strlen(name)), v);
}

value_t script_get_global(interp_t* I, const char* name) {
    var_t* x = env_find_local(I->global, name);
    return x ? x->v : v_undef();
}

/* PHP exit()/die() unwinds as an exception with this message */
static int is_exit(interp_t* I) {
    if (I->ret.t != V_OBJ) return 0;
    value_t m = obj_get(I, I->ret.o, "message");
    return m.t == V_STR && strcmp(m.s->s, "__exit__") == 0;
}

static void set_uncaught(interp_t* I) {
    if (is_exit(I)) { I->err[0] = 0; return; }
    str_t* s = v_tostr(I, I->ret);
    const char* where = I->throw_src ? I->throw_src : I->src_name ? I->src_name : "script";
    if (I->lang == LANG_PHP)
        ksnprintf(I->err, sizeof(I->err), "Fatal error: Uncaught %s in %s on line %d", s->s,
                  I->src_name ? I->src_name : "script", I->line);
    else if (I->throw_col > 1)                             /* minified code: where on the line */
        ksnprintf(I->err, sizeof(I->err), "Uncaught %s (%s, line %d:%d)", s->s,
                  where, I->throw_line, I->throw_col);
    else
        ksnprintf(I->err, sizeof(I->err), "Uncaught %s (%s, line %d)", s->s,
                  where, I->throw_line);
}

/* script_run/script_call may be re-entered from a native (a DOM method
 * firing event handlers): the outer run's state is saved around it */
typedef struct {
    env_t* cur; env_t* fn_env; value_t this_v, ret;
    int line; uint32_t steps, depth; const char* src_name;
} run_state_t;

static int enter(interp_t* I, run_state_t* s) {
    s->cur = I->cur; s->fn_env = I->fn_env; s->this_v = I->this_v; s->ret = I->ret;
    s->line = I->line; s->steps = I->steps; s->depth = I->depth; s->src_name = I->src_name;
    int nested = I->running++ > 0;
    if (!nested) { I->steps = 0; I->depth = 0; }
    return nested;
}

static void leave(interp_t* I, run_state_t* s, int nested) {
    I->running--;
    I->cur = s->cur; I->fn_env = s->fn_env; I->this_v = s->this_v;
    if (nested) { I->ret = s->ret; I->line = s->line; I->depth = s->depth; I->src_name = s->src_name; }
}

/* add the source around a syntax error to the message: minified code is all on one line */
static void near_text(interp_t* I, const char* src, uint32_t len, const char* at) {
    if (!at || at < src || at > src + len) return;
    const char* s = at - 20 < src ? src : at - 20;
    const char* e = at + 20 > src + len ? src + len : at + 20;
    char snip[48];
    int n = 0;
    for (const char* q = s; q < e && n < (int)sizeof(snip) - 1; q++) snip[n++] = ((unsigned char)*q < ' ') ? ' ' : *q;
    snip[n] = 0;
    uint32_t used = (uint32_t)strlen(I->err);
    ksnprintf(I->err + used, sizeof(I->err) - used, " near `%s`", snip);
}

/* a program (ns NULL) or a module (its own scope; exports into ns) */
static int run_program(interp_t* I, const char* src, uint32_t len, const char* name, const char* url, obj_t* ns) {
    I->err[0] = 0;
    lexer_t L;
    memset(&L, 0, sizeof(L));
    L.I = I;
    L.src = src;
    L.len = len;
    L.line = 1;
    L.line_start = src;
    L.html = (I->lang == LANG_PHP);
    lex_all(&L);
    if (L.err) {
        ksnprintf(I->err, sizeof(I->err), "%s: %s in %s on line %d",
                  I->lang == LANG_PHP ? "Parse error" : "SyntaxError", L.err, name ? name : "script", L.err_line);
        near_text(I, src, len, src + (L.pos < len ? L.pos : len));
        kfree(L.toks);
        return -1;
    }
    parser_t P;
    memset(&P, 0, sizeof(P));
    P.I = I;
    P.t = L.toks;
    P.n = L.ntok;
    P.php = (I->lang == LANG_PHP);
    node_t* prog = mk(&P, N_BLOCK);
    prog->op = 1;
    node_t** tail = &prog->a;
    node_t* hoisted = P.php ? NULL : mk(&P, N_VARHOIST);
    P.hoist = hoisted;
    while (!P.err && pk(&P)->t != T_EOF) {
        node_t* s = parse_stmt(&P);
        if (!s) continue;
        *tail = s;
        while (*tail) tail = &(*tail)->next;
    }
    if (hoisted && hoisted->a) { hoisted->next = prog->a; prog->a = hoisted; }
    kfree(L.toks);                                   /* the tree does not point into them */
    if (P.err) {
        ksnprintf(I->err, sizeof(I->err), "%s: %s in %s on line %d",
                  I->lang == LANG_PHP ? "Parse error" : "SyntaxError", P.err, name ? name : "script", P.err_line);
        near_text(I, src, len, P.err_at);
        return -1;
    }
    if (ns) {                                        /* imports are done before the rest */
        node_t* imps = NULL;
        node_t** it = &imps;
        node_t* rest = NULL;
        node_t** rt = &rest;
        for (node_t* s = prog->a; s; ) {
            node_t* nxt = s->next;
            s->next = NULL;
            if (s->k == N_IMPORT || s->k == N_VARHOIST) { *it = s; it = &s->next; }
            else { *rt = s; rt = &s->next; }
            s = nxt;
        }
        *it = rest;
        prog->a = imps;
    }
    run_state_t st;
    int nested = enter(I, &st);
    I->src_name = name;                              /* after enter(): it saves the caller's name */
    obj_t* saved_ns = I->module_ns;
    const char* saved_url = I->module_url;
    if (ns) {
        env_t* m = env_new(I, I->global, 1);
        I->cur = m;
        I->fn_env = m;
        I->module_ns = ns;
        I->module_url = url;
    } else {
        I->cur = I->global;
        I->fn_env = I->global;
    }
    /* top-level `this` is the global object (window) in a script, undefined in a module */
    I->this_v = (I->lang == LANG_JS && !ns) ? script_get_global(I, "globalThis") : v_undef();
    I->ctl = CTL_NONE;
    hoist(I, prog->a, I->cur);
    if (ns) export_pass(I, prog->a);                 /* functions, for modules importing this one back */
    exec_list(I, prog->a);
    if (ns && !I->ctl) export_pass(I, prog->a);
    I->module_ns = saved_ns;
    I->module_url = saved_url;
    int rc = 0;
    if (I->ctl == CTL_THROW) {
        set_uncaught(I);
        rc = I->err[0] ? -1 : 0;
    }
    I->ctl = CTL_NONE;
    leave(I, &st, nested);
    if (!nested && I->jobs && I->lang == LANG_JS) es_run_jobs(I);   /* promise reactions */
    return rc;
}

int script_run(interp_t* I, const char* src, uint32_t len, const char* name) {
    return run_program(I, src, len, name, NULL, NULL);
}

int script_run_module(interp_t* I, const char* src, uint32_t len, const char* name, const char* url, obj_t* ns) {
    return run_program(I, src, len, name, url, ns);
}

void script_set_import(interp_t* I, script_import_fn fn, void* ctx) {
    I->import_fn = fn;
    I->import_ctx = ctx;
}

int script_call(interp_t* I, value_t fn, value_t self, int argc, value_t* argv, value_t* ret) {
    run_state_t st;
    int nested = enter(I, &st);
    if (!I->cur) I->cur = I->global;
    I->err[0] = 0;
    I->ctl = CTL_NONE;
    value_t r = call_value(I, fn, self, argc, argv);
    int rc = 0;
    if (I->ctl == CTL_THROW) {
        set_uncaught(I);
        rc = -1;
    } else if (ret) {
        *ret = r;
    }
    I->ctl = CTL_NONE;
    leave(I, &st, nested);
    if (!nested && I->jobs && I->lang == LANG_JS) es_run_jobs(I);   /* promise reactions */
    return rc;
}
