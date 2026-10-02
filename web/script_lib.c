#include "script_int.h"
#include "kstring.h"
#include "random.h"
#include "timer.h"

/*
 * BananaScript built-ins: JavaScript's String/Array/Number/Date methods,
 * Math, JSON, console and friends, and the PHP function library.
 */

#define ARG(i) ((i) < argc ? argv[i] : v_undef())
#define INT64_MAX_SAFE 9000000000000000000LL
#define is_digit_c(c) ((c) >= '0' && (c) <= '9')

static int is_new_call(value_t self) { return self.t == V_NULL && self.b == 0x4E57; }

static num_t argn(interp_t* I, int argc, value_t* argv, int i, num_t def) {
    if (i >= argc || argv[i].t == V_UNDEF) return def;
    return v_tonum(I, argv[i]);
}

static int64_t to_int(num_t x) {
    if (num_isnan(x)) return 0;
    if (x > 9.0e18L) return INT64_MAX_SAFE;
    if (x < -9.0e18L) return -INT64_MAX_SAFE;
    return (int64_t)x;
}

static str_t* sv(interp_t* I, value_t v) { return v_tostr(I, v); }

static value_t ret_str(interp_t* I, const char* s, uint32_t n) { return v_strn(I, s, n); }

/* clamp a relative index like slice() does */
static int64_t rel_index(num_t x, int64_t len) {
    int64_t i = to_int(x);
    if (i < 0) { i += len; if (i < 0) i = 0; }
    if (i > len) i = len;
    return i;
}

static int find_sub(const char* h, uint32_t hl, const char* n, uint32_t nl, uint32_t from) {
    if (nl == 0) return from <= hl ? (int)from : (int)hl;
    for (uint32_t i = from; i + nl <= hl; i++)
        if (memcmp(h + i, n, nl) == 0) return (int)i;
    return -1;
}

static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }
static char upper(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }
static int is_ws(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }

/* ══ time ═════════════════════════════════════════════════════════════ */

/* days since 1970-01-01 for a civil date (Howard Hinnant's algorithm) */
static int64_t days_from_civil(int64_t y, int m, int d) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static void civil_from_days(int64_t z, int* y, int* m, int* d) {
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t yy = yoe + era * 400;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = (int)(yy + (*m <= 2));
}

static int64_t now_ms(void) {
    script_tm_t tm;
    memset(&tm, 0, sizeof(tm));
    tm.year = 2026; tm.month = 1; tm.day = 1;
    if (script_clock) script_clock(&tm);
    int64_t days = days_from_civil(tm.year, tm.month, tm.day);
    return ((days * 24 + tm.hour) * 60 + tm.minute) * 60000 + tm.second * 1000 + (int64_t)(timer_ms() % 1000);
}

typedef struct { int y, mo, d, h, mi, s, ms, wd; } tm_fields_t;

static void ms_to_fields(int64_t ms, tm_fields_t* f) {
    int64_t days = ms >= 0 ? ms / 86400000 : -((-ms + 86399999) / 86400000);
    int64_t rem = ms - days * 86400000;
    civil_from_days(days, &f->y, &f->mo, &f->d);
    f->h = (int)(rem / 3600000);
    f->mi = (int)(rem / 60000 % 60);
    f->s = (int)(rem / 1000 % 60);
    f->ms = (int)(rem % 1000);
    f->wd = (int)((days % 7 + 11) % 7);          /* 1970-01-01 was a Thursday */
}

static const char* const DAY_NAMES[] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
static const char* const MONTH_NAMES[] = { "January", "February", "March", "April", "May", "June", "July",
                                          "August", "September", "October", "November", "December" };

/* ══ JavaScript: globals ══════════════════════════════════════════════ */

static value_t js_console_log(interp_t* I, value_t self, int argc, value_t* argv) {
    (void)self;
    str_t* line = str_new(I, "", 0);
    for (int i = 0; i < argc; i++) {
        if (i) line = str_cat(I, line, str_new(I, " ", 1));
        line = str_cat(I, line, sv(I, argv[i]));
    }
    if (I->log) I->log(I->log_ctx, line->s, line->len);
    return v_undef();
}

static value_t js_parseInt(interp_t* I, value_t self, int argc, value_t* argv) {
    (void)self;
    const char* s = sv(I, ARG(0))->s;
    int radix = (int)argn(I, argc, argv, 1, 10);
    while (is_ws(*s)) s++;
    int neg = 0;
    if (*s == '-' || *s == '+') { neg = *s == '-'; s++; }
    if ((radix == 16 || radix == 0) && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { s += 2; radix = 16; }
    if (radix == 0) radix = 10;
    num_t v = 0;
    int any = 0;
    for (;; s++) {
        int d = (*s >= '0' && *s <= '9') ? *s - '0' : (lower(*s) >= 'a' && lower(*s) <= 'z') ? lower(*s) - 'a' + 10 : 99;
        if (d >= radix) break;
        v = v * radix + d;
        any = 1;
    }
    if (!any) { num_t z = 0; return v_num(z / z); }
    return v_num(neg ? -v : v);
}

static value_t js_parseFloat(interp_t* I, value_t self, int argc, value_t* argv) {
    (void)self;
    const char* s = sv(I, ARG(0))->s;
    while (is_ws(*s)) s++;
    char buf[64];
    uint32_t n = 0;
    if (*s == '-' || *s == '+') buf[n++] = *s++;
    int dot = 0, exp = 0;
    while (*s && n < sizeof(buf) - 1) {
        if (*s >= '0' && *s <= '9') buf[n++] = *s;
        else if (*s == '.' && !dot && !exp) { dot = 1; buf[n++] = *s; }
        else if ((*s == 'e' || *s == 'E') && !exp && n) { exp = 1; buf[n++] = *s; if (s[1] == '-' || s[1] == '+') buf[n++] = *++s; }
        else break;
        s++;
    }
    buf[n] = 0;
    return v_num(str_tonum(buf, 0));
}

static value_t js_isNaN(interp_t* I, value_t self, int argc, value_t* argv) {
    (void)self;
    return v_bool(num_isnan(v_tonum(I, ARG(0))));
}

static value_t js_isFinite(interp_t* I, value_t self, int argc, value_t* argv) {
    (void)self;
    num_t x = v_tonum(I, ARG(0));
    return v_bool(!num_isnan(x) && x - x == 0);
}

static value_t js_String(interp_t* I, value_t self, int argc, value_t* argv) {
    (void)self;
    return argc ? v_strv(sv(I, argv[0])) : v_str(I, "");
}

static value_t js_Number(interp_t* I, value_t self, int argc, value_t* argv) {
    (void)self;
    return v_num(argc ? v_tonum(I, argv[0]) : 0);
}

static value_t js_Boolean(interp_t* I, value_t self, int argc, value_t* argv) {
    (void)self;
    return v_bool(v_truthy(I, ARG(0)));
}

static value_t js_Array(interp_t* I, value_t self, int argc, value_t* argv) {
    (void)self;
    obj_t* a = obj_new(I, OBJ_ARRAY);
    if (argc == 1 && argv[0].t == V_NUM) {
        uint32_t n = (uint32_t)argv[0].n;
        if (n) arr_set(I, a, n - 1, v_undef());
    } else {
        for (int i = 0; i < argc; i++) arr_push(I, a, argv[i]);
    }
    return v_obj(a);
}

static value_t js_isArray(interp_t* I, value_t self, int argc, value_t* argv) {
    (void)self; (void)I;
    return v_bool(argc && argv[0].t == V_OBJ && argv[0].o->kind == OBJ_ARRAY);
}

static value_t js_Object(interp_t* I, value_t self, int argc, value_t* argv) {
    (void)self;
    if (argc && argv[0].t == V_OBJ) return argv[0];
    return v_obj(obj_new(I, OBJ_PLAIN));
}

static value_t obj_keys_values(interp_t* I, value_t v, int what) {
    obj_t* r = obj_new(I, OBJ_ARRAY);
    if (v.t != V_OBJ) return v_obj(r);
    obj_t* o = v.o;
    if (o->kind == OBJ_ARRAY) {
        for (uint32_t i = 0; i < o->len; i++) {
            char b[16];
            ksnprintf(b, sizeof(b), "%u", i);
            if (what == 0) arr_push(I, r, v_str(I, b));
            else if (what == 1) arr_push(I, r, o->items[i]);
            else {
                obj_t* pair = obj_new(I, OBJ_ARRAY);
                arr_push(I, pair, v_str(I, b));
                arr_push(I, pair, o->items[i]);
                arr_push(I, r, v_obj(pair));
            }
        }
        return v_obj(r);
    }
    for (uint32_t i = 0; i < o->n; i++) {
        if (what == 0) arr_push(I, r, v_strv(o->props[i].key));
        else if (what == 1) arr_push(I, r, o->props[i].v);
        else {
            obj_t* pair = obj_new(I, OBJ_ARRAY);
            arr_push(I, pair, v_strv(o->props[i].key));
            arr_push(I, pair, o->props[i].v);
            arr_push(I, r, v_obj(pair));
        }
    }
    return v_obj(r);
}

static value_t js_keys(interp_t* I, value_t self, int argc, value_t* argv) { (void)self; return obj_keys_values(I, ARG(0), 0); }
static value_t js_values(interp_t* I, value_t self, int argc, value_t* argv) { (void)self; return obj_keys_values(I, ARG(0), 1); }
static value_t js_entries(interp_t* I, value_t self, int argc, value_t* argv) { (void)self; return obj_keys_values(I, ARG(0), 2); }

static value_t js_assign(interp_t* I, value_t self, int argc, value_t* argv) {
    (void)self;
    if (!argc || argv[0].t != V_OBJ) return ARG(0);
    for (int i = 1; i < argc; i++) {
        if (argv[i].t != V_OBJ) continue;
        for (uint32_t k = 0; k < argv[i].o->n; k++)
            obj_set(I, argv[0].o, argv[i].o->props[k].key->s, argv[i].o->props[k].v);
    }
    return argv[0];
}

static value_t js_Error(interp_t* I, value_t self, int argc, value_t* argv) {
    (void)self;
    obj_t* e = obj_new(I, OBJ_PLAIN);
    obj_set(I, e, "name", v_str(I, "Error"));
    obj_set(I, e, "message", argc ? v_strv(sv(I, argv[0])) : v_str(I, ""));
    return v_obj(e);
}

/* ── URI ── */

static value_t js_encodeURIComponent(interp_t* I, value_t self, int argc, value_t* argv) {
    (void)self;
    str_t* s = sv(I, ARG(0));
    char* out = (char*)arena_alloc(I->A, s->len * 3 + 1);
    uint32_t n = 0;
    static const char hx[] = "0123456789ABCDEF";
    for (uint32_t i = 0; i < s->len; i++) {
        unsigned char c = (unsigned char)s->s[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || strchr("-_.!~*'()", c)) out[n++] = (char)c;
        else { out[n++] = '%'; out[n++] = hx[c >> 4]; out[n++] = hx[c & 15]; }
    }
    return ret_str(I, out, n);
}

static int hexd(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    c = lower(c);
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static str_t* url_decode(interp_t* I, const char* s, uint32_t len, int plus) {
    char* out = (char*)arena_alloc(I->A, len + 1);
    uint32_t n = 0;
    for (uint32_t i = 0; i < len; i++) {
        if (s[i] == '%' && i + 2 < len && hexd(s[i + 1]) >= 0 && hexd(s[i + 2]) >= 0) {
            out[n++] = (char)(hexd(s[i + 1]) * 16 + hexd(s[i + 2]));
            i += 2;
        } else if (plus && s[i] == '+') {
            out[n++] = ' ';
        } else {
            out[n++] = s[i];
        }
    }
    return str_new(I, out, n);
}

static value_t js_decodeURIComponent(interp_t* I, value_t self, int argc, value_t* argv) {
    (void)self;
    str_t* s = sv(I, ARG(0));
    return v_strv(url_decode(I, s->s, s->len, 0));
}

/* ── Math ── */

static num_t x87_unary(num_t x, int op) {
    num_t r = x;
    switch (op) {
    case 0: __asm__ volatile("fsqrt" : "+t"(r)); break;
    case 1: __asm__ volatile("fsin" : "+t"(r)); break;
    case 2: __asm__ volatile("fcos" : "+t"(r)); break;
    case 3: __asm__ volatile("fldln2\n\tfxch\n\tfyl2x" : "+t"(r) :: "st(1)"); break;     /* ln */
    }
    return r;
}

static value_t m_abs(interp_t* I, value_t s, int argc, value_t* argv) { (void)s; num_t x = argn(I, argc, argv, 0, 0); return v_num(x < 0 ? -x : x); }
static value_t m_floor(interp_t* I, value_t s, int argc, value_t* argv) { (void)s; return v_num(num_floor(argn(I, argc, argv, 0, 0))); }
static value_t m_ceil(interp_t* I, value_t s, int argc, value_t* argv) { (void)s; num_t x = argn(I, argc, argv, 0, 0); return v_num(-num_floor(-x)); }
static value_t m_round(interp_t* I, value_t s, int argc, value_t* argv) { (void)s; return v_num(num_floor(argn(I, argc, argv, 0, 0) + 0.5L)); }
static value_t m_trunc(interp_t* I, value_t s, int argc, value_t* argv) { (void)s; num_t x = argn(I, argc, argv, 0, 0); return v_num(x < 0 ? -num_floor(-x) : num_floor(x)); }
static value_t m_sign(interp_t* I, value_t s, int argc, value_t* argv) { (void)s; num_t x = argn(I, argc, argv, 0, 0); return v_num(x > 0 ? 1 : x < 0 ? -1 : x); }
static value_t m_sqrt(interp_t* I, value_t s, int argc, value_t* argv) {
    (void)s;
    num_t x = argn(I, argc, argv, 0, 0);
    if (x < 0) { num_t z = 0; return v_num(z / z); }
    return v_num(x87_unary(x, 0));
}
static value_t m_sin(interp_t* I, value_t s, int argc, value_t* argv) { (void)s; return v_num(x87_unary(argn(I, argc, argv, 0, 0), 1)); }
static value_t m_cos(interp_t* I, value_t s, int argc, value_t* argv) { (void)s; return v_num(x87_unary(argn(I, argc, argv, 0, 0), 2)); }
static value_t m_log(interp_t* I, value_t s, int argc, value_t* argv) {
    (void)s;
    num_t x = argn(I, argc, argv, 0, 0);
    if (x <= 0) { num_t z = 0, one = 1; return v_num(x == 0 ? -one / z : z / z); }
    return v_num(x87_unary(x, 3));
}
static value_t m_pow(interp_t* I, value_t s, int argc, value_t* argv);
static value_t m_exp(interp_t* I, value_t s, int argc, value_t* argv) {
    (void)s;
    value_t a[2] = { v_num(2.71828182845904523536L), v_num(argn(I, argc, argv, 0, 0)) };
    return m_pow(I, v_undef(), 2, a);
}
static value_t m_random(interp_t* I, value_t s, int argc, value_t* argv) {
    (void)s; (void)I; (void)argc; (void)argv;
    return v_num((num_t)(random_u32() >> 1) / 2147483648.0L);
}
static value_t m_max(interp_t* I, value_t s, int argc, value_t* argv) {
    (void)s;
    num_t one = 1, z = 0, best = -one / z;
    for (int i = 0; i < argc; i++) {
        num_t x = v_tonum(I, argv[i]);
        if (num_isnan(x)) return v_num(x);
        if (x > best) best = x;
    }
    return v_num(best);
}
static value_t m_min(interp_t* I, value_t s, int argc, value_t* argv) {
    (void)s;
    num_t one = 1, z = 0, best = one / z;
    for (int i = 0; i < argc; i++) {
        num_t x = v_tonum(I, argv[i]);
        if (num_isnan(x)) return v_num(x);
        if (x < best) best = x;
    }
    return v_num(best);
}

/* binary() lives in script.c; ** there uses num_pow - reuse via a tiny program-free path */
static num_t pow_num(num_t a, num_t b) {
    if (num_floor(b) == b && b >= -1000 && b <= 1000) {
        num_t r = 1, base = a;
        int64_t e = (int64_t)b;
        int neg = e < 0;
        if (neg) e = -e;
        while (e) { if (e & 1) r *= base; base *= base; e >>= 1; }
        return neg ? 1 / r : r;
    }
    if (a < 0) { num_t z = 0; return z / z; }
    if (a == 0) return 0;
    /* a^b = e^(b ln a): ln via x87, e^x via 2^(x log2 e) */
    num_t y = b * x87_unary(a, 3) * 1.44269504088896340736L;    /* log2 of the result */
    num_t r;
    __asm__ volatile(
        "fld %%st(0)\n\t"
        "frndint\n\t"
        "fxch\n\t"
        "fsub %%st(1), %%st\n\t"
        "f2xm1\n\t"
        "fld1\n\t"
        "faddp\n\t"
        "fscale\n\t"
        "fstp %%st(1)\n\t"
        : "=t"(r) : "0"(y));
    return r;
}

static value_t m_pow(interp_t* I, value_t s, int argc, value_t* argv) {
    (void)s;
    return v_num(pow_num(argn(I, argc, argv, 0, 0), argn(I, argc, argv, 1, 0)));
}

/* ── JSON ── */

static void json_str(interp_t* I, str_t** out, str_t* s) {
    char* buf = (char*)arena_alloc(I->A, s->len * 6 + 3);
    uint32_t n = 0;
    buf[n++] = '"';
    for (uint32_t i = 0; i < s->len; i++) {
        unsigned char c = (unsigned char)s->s[i];
        if (c == '"' || c == '\\') { buf[n++] = '\\'; buf[n++] = (char)c; }
        else if (c == '\n') { buf[n++] = '\\'; buf[n++] = 'n'; }
        else if (c == '\t') { buf[n++] = '\\'; buf[n++] = 't'; }
        else if (c == '\r') { buf[n++] = '\\'; buf[n++] = 'r'; }
        else if (c < 0x20) { ksnprintf(buf + n, 7, "\\u%04x", c); n += 6; }
        else buf[n++] = (char)c;
    }
    buf[n++] = '"';
    *out = str_cat(I, *out, str_new(I, buf, n));
}

static void json_val(interp_t* I, str_t** out, value_t v, int depth) {
    if (depth > 30) { *out = str_cat(I, *out, str_new(I, "null", 4)); return; }
    switch (v.t) {
    case V_UNDEF: case V_FUNC: case V_NULL: *out = str_cat(I, *out, str_new(I, "null", 4)); return;
    case V_BOOL: *out = str_cat(I, *out, str_new(I, v.b ? "true" : "false", v.b ? 4 : 5)); return;
    case V_NUM: {
        if (num_isnan(v.n) || v.n - v.n != 0) { *out = str_cat(I, *out, str_new(I, "null", 4)); return; }
        *out = str_cat(I, *out, v_tostr(I, v));
        return;
    }
    case V_STR: json_str(I, out, v.s); return;
    case V_OBJ: {
        obj_t* o = v.o;
        int list = o->kind == OBJ_ARRAY;
        if (o->kind == OBJ_PHPARRAY) {               /* list if keys are 0..n-1 */
            list = 1;
            for (uint32_t i = 0; i < o->n && list; i++) {
                char b[16];
                ksnprintf(b, sizeof(b), "%u", i);
                if (strcmp(o->props[i].key->s, b) != 0) list = 0;
            }
        }
        *out = str_cat(I, *out, str_new(I, list ? "[" : "{", 1));
        if (o->kind == OBJ_ARRAY) {
            for (uint32_t i = 0; i < o->len; i++) {
                if (i) *out = str_cat(I, *out, str_new(I, ",", 1));
                json_val(I, out, o->items[i], depth + 1);
            }
        } else {
            int first = 1;
            for (uint32_t i = 0; i < o->n; i++) {
                if (!list && (o->props[i].v.t == V_FUNC || o->props[i].v.t == V_UNDEF)) continue;   /* skipped like JS */
                if (!first) *out = str_cat(I, *out, str_new(I, ",", 1));
                first = 0;
                if (!list) {
                    json_str(I, out, o->props[i].key);
                    *out = str_cat(I, *out, str_new(I, ":", 1));
                }
                json_val(I, out, o->props[i].v, depth + 1);
            }
        }
        *out = str_cat(I, *out, str_new(I, list ? "]" : "}", 1));
        return;
    }
    }
}

static value_t js_stringify(interp_t* I, value_t self, int argc, value_t* argv) {
    (void)self;
    str_t* out = str_new(I, "", 0);
    if (!argc || argv[0].t == V_UNDEF || argv[0].t == V_FUNC) return v_undef();
    json_val(I, &out, argv[0], 0);
    return v_strv(out);
}

typedef struct { const char* p; const char* end; int err; } jparse_t;

static void jws(jparse_t* J) { while (J->p < J->end && is_ws(*J->p)) J->p++; }

static value_t jvalue(interp_t* I, jparse_t* J, int php, int depth) {
    jws(J);
    if (J->p >= J->end || depth > 60) { J->err = 1; return v_undef(); }
    char c = *J->p;
    if (c == '{' || c == '[') {
        int arr = c == '[';
        J->p++;
        obj_t* o = obj_new(I, php ? OBJ_PHPARRAY : arr ? OBJ_ARRAY : OBJ_PLAIN);
        jws(J);
        if (J->p < J->end && *J->p == (arr ? ']' : '}')) { J->p++; return v_obj(o); }
        for (;;) {
            value_t key = v_undef();
            if (!arr) {
                jws(J);
                key = jvalue(I, J, php, depth + 1);
                if (J->err || key.t != V_STR) { J->err = 1; return v_undef(); }
                jws(J);
                if (J->p >= J->end || *J->p != ':') { J->err = 1; return v_undef(); }
                J->p++;
            }
            value_t v = jvalue(I, J, php, depth + 1);
            if (J->err) return v_undef();
            if (php) { if (arr) php_array_push(I, o, v); else php_array_set(I, o, key, v); }
            else if (arr) arr_push(I, o, v);
            else obj_set(I, o, key.s->s, v);
            jws(J);
            if (J->p < J->end && *J->p == ',') { J->p++; continue; }
            if (J->p < J->end && *J->p == (arr ? ']' : '}')) { J->p++; return v_obj(o); }
            J->err = 1;
            return v_undef();
        }
    }
    if (c == '"') {
        const char* s = ++J->p;
        while (J->p < J->end && *J->p != '"') { if (*J->p == '\\') J->p++; J->p++; }
        if (J->p >= J->end) { J->err = 1; return v_undef(); }
        uint32_t n = (uint32_t)(J->p - s);
        J->p++;
        char* buf = (char*)arena_alloc(I->A, n + 1);
        uint32_t o = 0;
        for (uint32_t i = 0; i < n; i++) {
            if (s[i] != '\\') { buf[o++] = s[i]; continue; }
            char e = s[++i];
            if (e == 'n') buf[o++] = '\n';
            else if (e == 't') buf[o++] = '\t';
            else if (e == 'r') buf[o++] = '\r';
            else if (e == 'u' && i + 4 < n) {
                int v = hexd(s[i + 1]) * 4096 + hexd(s[i + 2]) * 256 + hexd(s[i + 3]) * 16 + hexd(s[i + 4]);
                i += 4;
                buf[o++] = v < 128 ? (char)v : '?';
            } else buf[o++] = e;
        }
        return v_strn(I, buf, o);
    }
    if (strncmp(J->p, "true", 4) == 0) { J->p += 4; return v_bool(1); }
    if (strncmp(J->p, "false", 5) == 0) { J->p += 5; return v_bool(0); }
    if (strncmp(J->p, "null", 4) == 0) { J->p += 4; return v_null(); }
    char buf[64];
    uint32_t n = 0;
    while (J->p < J->end && n < sizeof(buf) - 1 && (strchr("+-.eE", *J->p) || (*J->p >= '0' && *J->p <= '9'))) buf[n++] = *J->p++;
    buf[n] = 0;
    if (!n) { J->err = 1; return v_undef(); }
    return v_num(str_tonum(buf, 0));
}

static value_t js_jparse(interp_t* I, value_t self, int argc, value_t* argv) {
    (void)self;
    str_t* s = sv(I, ARG(0));
    jparse_t J = { s->s, s->s + s->len, 0 };
    value_t v = jvalue(I, &J, 0, 0);
    jws(&J);
    if (J.err || J.p != J.end) { script_throw(I, "SyntaxError: bad JSON"); return v_undef(); }
    return v;
}

/* ── Date ── */

static int64_t date_ms(interp_t* I, value_t self) {
    if (self.t != V_OBJ) return 0;
    return (int64_t)v_tonum(I, obj_get(I, self.o, "__t"));
}

static value_t js_Date(interp_t* I, value_t self, int argc, value_t* argv) {
    int64_t ms = now_ms();
    if (argc == 1 && argv[0].t == V_NUM) ms = (int64_t)argv[0].n;
    else if (argc >= 3) {
        int64_t d = days_from_civil((int64_t)v_tonum(I, argv[0]), (int)v_tonum(I, argv[1]) + 1, (int)v_tonum(I, argv[2]));
        ms = d * 86400000 + (int64_t)(argn(I, argc, argv, 3, 0) * 3600000) + (int64_t)(argn(I, argc, argv, 4, 0) * 60000) +
             (int64_t)(argn(I, argc, argv, 5, 0) * 1000);
    }
    if (!is_new_call(self)) {                        /* Date() as a function: a string */
        tm_fields_t f;
        ms_to_fields(ms, &f);
        char b[64];
        ksnprintf(b, sizeof(b), "%.3s %.3s %02d %d %02d:%02d:%02d", DAY_NAMES[f.wd], MONTH_NAMES[f.mo - 1], f.d, f.y, f.h, f.mi, f.s);
        return v_str(I, b);
    }
    obj_t* o = obj_new(I, OBJ_PLAIN);
    o->proto = I->proto_date;
    obj_set(I, o, "__t", v_num((num_t)ms));
    return v_obj(o);
}

static value_t js_Date_now(interp_t* I, value_t s, int argc, value_t* argv) { (void)I; (void)s; (void)argc; (void)argv; return v_num((num_t)now_ms()); }

#define DATE_GETTER(nm, expr) \
    static value_t nm(interp_t* I, value_t self, int argc, value_t* argv) { \
        (void)argc; (void)argv; tm_fields_t f; ms_to_fields(date_ms(I, self), &f); return v_num(expr); }
DATE_GETTER(d_getFullYear, f.y)
DATE_GETTER(d_getMonth, f.mo - 1)
DATE_GETTER(d_getDate, f.d)
DATE_GETTER(d_getDay, f.wd)
DATE_GETTER(d_getHours, f.h)
DATE_GETTER(d_getMinutes, f.mi)
DATE_GETTER(d_getSeconds, f.s)
DATE_GETTER(d_getMilliseconds, f.ms)

static value_t d_getTime(interp_t* I, value_t self, int argc, value_t* argv) { (void)argc; (void)argv; return v_num((num_t)date_ms(I, self)); }

static value_t d_fmt(interp_t* I, value_t self, int which) {
    tm_fields_t f;
    ms_to_fields(date_ms(I, self), &f);
    char b[80];
    if (which == 0) ksnprintf(b, sizeof(b), "%02d:%02d:%02d", f.h, f.mi, f.s);
    else if (which == 1) ksnprintf(b, sizeof(b), "%d/%d/%d", f.mo, f.d, f.y);
    else if (which == 2) ksnprintf(b, sizeof(b), "%d-%02d-%02dT%02d:%02d:%02d.%03dZ", f.y, f.mo, f.d, f.h, f.mi, f.s, f.ms);
    else ksnprintf(b, sizeof(b), "%.3s %.3s %02d %d %02d:%02d:%02d", DAY_NAMES[f.wd], MONTH_NAMES[f.mo - 1], f.d, f.y, f.h, f.mi, f.s);
    return v_str(I, b);
}
static value_t d_toLocaleTimeString(interp_t* I, value_t self, int argc, value_t* argv) { (void)argc; (void)argv; return d_fmt(I, self, 0); }
static value_t d_toLocaleDateString(interp_t* I, value_t self, int argc, value_t* argv) { (void)argc; (void)argv; return d_fmt(I, self, 1); }
static value_t d_toISOString(interp_t* I, value_t self, int argc, value_t* argv) { (void)argc; (void)argv; return d_fmt(I, self, 2); }
static value_t d_toString(interp_t* I, value_t self, int argc, value_t* argv) { (void)argc; (void)argv; return d_fmt(I, self, 3); }

/* ══ JavaScript: string methods ═══════════════════════════════════════ */

#define SELF_STR str_t* S = sv(I, self)

static value_t s_charAt(interp_t* I, value_t self, int argc, value_t* argv) {
    SELF_STR;
    int64_t i = to_int(argn(I, argc, argv, 0, 0));
    if (i < 0 || i >= S->len) return v_str(I, "");
    return ret_str(I, S->s + i, 1);
}
static value_t s_charCodeAt(interp_t* I, value_t self, int argc, value_t* argv) {
    SELF_STR;
    int64_t i = to_int(argn(I, argc, argv, 0, 0));
    if (i < 0 || i >= S->len) { num_t z = 0; return v_num(z / z); }
    return v_num((unsigned char)S->s[i]);
}
static value_t s_at(interp_t* I, value_t self, int argc, value_t* argv) {
    SELF_STR;
    int64_t i = to_int(argn(I, argc, argv, 0, 0));
    if (i < 0) i += S->len;
    if (i < 0 || i >= S->len) return v_undef();
    return ret_str(I, S->s + i, 1);
}
static value_t s_indexOf(interp_t* I, value_t self, int argc, value_t* argv) {
    SELF_STR;
    str_t* n = sv(I, ARG(0));
    int64_t from = to_int(argn(I, argc, argv, 1, 0));
    if (from < 0) from = 0;
    if (from > S->len) from = S->len;
    return v_num(find_sub(S->s, S->len, n->s, n->len, (uint32_t)from));
}
static value_t s_lastIndexOf(interp_t* I, value_t self, int argc, value_t* argv) {
    SELF_STR;
    str_t* n = sv(I, ARG(0));
    int last = -1, p = -1;
    while ((p = find_sub(S->s, S->len, n->s, n->len, (uint32_t)(p + 1))) >= 0) {
        last = p;
        if ((uint32_t)p >= S->len) break;
    }
    return v_num(last);
}
static value_t s_includes(interp_t* I, value_t self, int argc, value_t* argv) {
    SELF_STR;
    str_t* n = sv(I, ARG(0));
    return v_bool(find_sub(S->s, S->len, n->s, n->len, 0) >= 0);
}
static value_t s_startsWith(interp_t* I, value_t self, int argc, value_t* argv) {
    SELF_STR;
    str_t* n = sv(I, ARG(0));
    return v_bool(n->len <= S->len && memcmp(S->s, n->s, n->len) == 0);
}
static value_t s_endsWith(interp_t* I, value_t self, int argc, value_t* argv) {
    SELF_STR;
    str_t* n = sv(I, ARG(0));
    return v_bool(n->len <= S->len && memcmp(S->s + S->len - n->len, n->s, n->len) == 0);
}
static value_t s_slice(interp_t* I, value_t self, int argc, value_t* argv) {
    SELF_STR;
    int64_t a = rel_index(argn(I, argc, argv, 0, 0), S->len);
    int64_t b = rel_index(argn(I, argc, argv, 1, S->len), S->len);
    if (b < a) b = a;
    return ret_str(I, S->s + a, (uint32_t)(b - a));
}
static value_t s_substring(interp_t* I, value_t self, int argc, value_t* argv) {
    SELF_STR;
    int64_t a = to_int(argn(I, argc, argv, 0, 0)), b = to_int(argn(I, argc, argv, 1, S->len));
    if (a < 0) a = 0;
    if (b < 0) b = 0;
    if (a > S->len) a = S->len;
    if (b > S->len) b = S->len;
    if (a > b) { int64_t t = a; a = b; b = t; }
    return ret_str(I, S->s + a, (uint32_t)(b - a));
}
static value_t s_substr(interp_t* I, value_t self, int argc, value_t* argv) {
    SELF_STR;
    int64_t a = rel_index(argn(I, argc, argv, 0, 0), S->len);
    int64_t n = to_int(argn(I, argc, argv, 1, S->len - a));
    if (n < 0) n = 0;
    if (a + n > S->len) n = S->len - a;
    return ret_str(I, S->s + a, (uint32_t)n);
}
static value_t s_case(interp_t* I, value_t self, int up) {
    SELF_STR;
    char* b = (char*)arena_alloc(I->A, S->len + 1);
    for (uint32_t i = 0; i < S->len; i++) b[i] = up ? upper(S->s[i]) : lower(S->s[i]);
    return ret_str(I, b, S->len);
}
static value_t s_toUpperCase(interp_t* I, value_t self, int argc, value_t* argv) { (void)argc; (void)argv; return s_case(I, self, 1); }
static value_t s_toLowerCase(interp_t* I, value_t self, int argc, value_t* argv) { (void)argc; (void)argv; return s_case(I, self, 0); }
static value_t s_trim_x(interp_t* I, value_t self, int l, int r) {
    SELF_STR;
    uint32_t a = 0, b = S->len;
    if (l) while (a < b && is_ws(S->s[a])) a++;
    if (r) while (b > a && is_ws(S->s[b - 1])) b--;
    return ret_str(I, S->s + a, b - a);
}
static value_t s_trim(interp_t* I, value_t self, int argc, value_t* argv) { (void)argc; (void)argv; return s_trim_x(I, self, 1, 1); }
static value_t s_trimStart(interp_t* I, value_t self, int argc, value_t* argv) { (void)argc; (void)argv; return s_trim_x(I, self, 1, 0); }
static value_t s_trimEnd(interp_t* I, value_t self, int argc, value_t* argv) { (void)argc; (void)argv; return s_trim_x(I, self, 0, 1); }

static obj_t* split_str(interp_t* I, str_t* S, str_t* sep, int64_t limit, int php) {
    obj_t* a = obj_new(I, php ? OBJ_PHPARRAY : OBJ_ARRAY);
    if (!sep) { arr_push(I, a, v_strv(S)); return a; }
    if (sep->len == 0) {
        for (uint32_t i = 0; i < S->len && (limit < 0 || (int64_t)i < limit); i++) arr_push(I, a, ret_str(I, S->s + i, 1));
        return a;
    }
    uint32_t start = 0;
    int count = 0;
    for (;;) {
        if (limit >= 0 && count >= limit) break;
        int p = find_sub(S->s, S->len, sep->s, sep->len, start);
        if (p < 0 || (php && limit > 0 && count == limit - 1)) {
            arr_push(I, a, ret_str(I, S->s + start, S->len - start));
            break;
        }
        arr_push(I, a, ret_str(I, S->s + start, (uint32_t)p - start));
        count++;
        start = (uint32_t)p + sep->len;
    }
    return a;
}

static value_t s_split(interp_t* I, value_t self, int argc, value_t* argv) {
    SELF_STR;
    str_t* sep = (argc && argv[0].t != V_UNDEF) ? sv(I, argv[0]) : NULL;
    int64_t limit = argc > 1 && argv[1].t != V_UNDEF ? to_int(v_tonum(I, argv[1])) : -1;
    return v_obj(split_str(I, S, sep, limit, 0));
}

static str_t* replace_str(interp_t* I, str_t* S, str_t* from, value_t to, int all) {
    if (from->len == 0) return S;
    str_t* out = str_new(I, "", 0);
    uint32_t start = 0;
    for (;;) {
        int p = find_sub(S->s, S->len, from->s, from->len, start);
        if (p < 0) break;
        out = str_cat(I, out, str_new(I, S->s + start, (uint32_t)p - start));
        if (to.t == V_FUNC) {
            value_t m = v_strv(from);
            value_t r = call_value(I, to, v_undef(), 1, &m);
            out = str_cat(I, out, sv(I, r));
        } else {
            out = str_cat(I, out, sv(I, to));
        }
        start = (uint32_t)p + from->len;
        if (!all) break;
    }
    return str_cat(I, out, str_new(I, S->s + start, S->len - start));
}

static value_t s_replace(interp_t* I, value_t self, int argc, value_t* argv) {
    SELF_STR;
    return v_strv(replace_str(I, S, sv(I, ARG(0)), ARG(1), 0));
}
static value_t s_replaceAll(interp_t* I, value_t self, int argc, value_t* argv) {
    SELF_STR;
    return v_strv(replace_str(I, S, sv(I, ARG(0)), ARG(1), 1));
}
static str_t* repeat_str(interp_t* I, str_t* S, int64_t n) {
    if (n <= 0 || S->len == 0) return str_new(I, "", 0);
    if (n * S->len > 10000000) n = 10000000 / S->len;
    char* b = (char*)arena_alloc(I->A, (uint32_t)(n * S->len) + 1);
    for (int64_t i = 0; i < n; i++) memcpy(b + i * S->len, S->s, S->len);
    return str_new(I, b, (uint32_t)(n * S->len));
}
static value_t s_repeat(interp_t* I, value_t self, int argc, value_t* argv) {
    SELF_STR;
    return v_strv(repeat_str(I, S, to_int(argn(I, argc, argv, 0, 0))));
}
static str_t* pad_str(interp_t* I, str_t* S, int64_t len, str_t* pad, int start) {
    if (len <= (int64_t)S->len || !pad->len) return S;
    uint32_t need = (uint32_t)(len - S->len);
    char* b = (char*)arena_alloc(I->A, need + 1);
    for (uint32_t i = 0; i < need; i++) b[i] = pad->s[i % pad->len];
    str_t* p = str_new(I, b, need);
    return start ? str_cat(I, p, S) : str_cat(I, S, p);
}
static value_t s_padStart(interp_t* I, value_t self, int argc, value_t* argv) {
    SELF_STR;
    return v_strv(pad_str(I, S, to_int(argn(I, argc, argv, 0, 0)), argc > 1 ? sv(I, argv[1]) : str_new(I, " ", 1), 1));
}
static value_t s_padEnd(interp_t* I, value_t self, int argc, value_t* argv) {
    SELF_STR;
    return v_strv(pad_str(I, S, to_int(argn(I, argc, argv, 0, 0)), argc > 1 ? sv(I, argv[1]) : str_new(I, " ", 1), 0));
}
static value_t s_concat(interp_t* I, value_t self, int argc, value_t* argv) {
    SELF_STR;
    for (int i = 0; i < argc; i++) S = str_cat(I, S, sv(I, argv[i]));
    return v_strv(S);
}
static value_t s_toString(interp_t* I, value_t self, int argc, value_t* argv) { (void)argc; (void)argv; return v_strv(sv(I, self)); }

static value_t js_fromCharCode(interp_t* I, value_t self, int argc, value_t* argv) {
    (void)self;
    char* b = (char*)arena_alloc(I->A, (uint32_t)argc * 3 + 1);
    uint32_t n = 0;
    for (int i = 0; i < argc; i++) {
        int c = (int)v_tonum(I, argv[i]);
        if (c < 0x80) b[n++] = (char)c;
        else if (c < 0x800) { b[n++] = (char)(0xC0 | (c >> 6)); b[n++] = (char)(0x80 | (c & 63)); }
        else { b[n++] = (char)(0xE0 | ((c >> 12) & 15)); b[n++] = (char)(0x80 | ((c >> 6) & 63)); b[n++] = (char)(0x80 | (c & 63)); }
    }
    return ret_str(I, b, n);
}

/* ══ JavaScript: array methods ════════════════════════════════════════ */

#define SELF_ARR \
    if (self.t != V_OBJ || self.o->kind != OBJ_ARRAY) { script_throw(I, "TypeError: not an array"); return v_undef(); } \
    obj_t* A = self.o

static value_t a_push(interp_t* I, value_t self, int argc, value_t* argv) {
    SELF_ARR;
    for (int i = 0; i < argc; i++) arr_push(I, A, argv[i]);
    return v_num(A->len);
}
static value_t a_pop(interp_t* I, value_t self, int argc, value_t* argv) {
    (void)argc; (void)argv;
    SELF_ARR;
    if (!A->len) return v_undef();
    return A->items[--A->len];
}
static value_t a_shift(interp_t* I, value_t self, int argc, value_t* argv) {
    (void)argc; (void)argv;
    SELF_ARR;
    if (!A->len) return v_undef();
    value_t v = A->items[0];
    memmove(A->items, A->items + 1, (A->len - 1) * sizeof(value_t));
    A->len--;
    return v;
}
static value_t a_unshift(interp_t* I, value_t self, int argc, value_t* argv) {
    SELF_ARR;
    for (int i = argc - 1; i >= 0; i--) {
        arr_push(I, A, v_undef());
        memmove(A->items + 1, A->items, (A->len - 1) * sizeof(value_t));
        A->items[0] = argv[i];
    }
    return v_num(A->len);
}
static value_t a_join(interp_t* I, value_t self, int argc, value_t* argv) {
    SELF_ARR;
    str_t* sep = (argc && argv[0].t != V_UNDEF) ? sv(I, argv[0]) : str_new(I, ",", 1);
    str_t* out = str_new(I, "", 0);
    for (uint32_t i = 0; i < A->len; i++) {
        if (i) out = str_cat(I, out, sep);
        if (A->items[i].t != V_UNDEF && A->items[i].t != V_NULL) out = str_cat(I, out, sv(I, A->items[i]));
    }
    return v_strv(out);
}
static value_t a_indexOf(interp_t* I, value_t self, int argc, value_t* argv) {
    SELF_ARR;
    for (uint32_t i = 0; i < A->len; i++) if (v_strict_eq(A->items[i], ARG(0))) return v_num(i);
    return v_num(-1);
}
static value_t a_includes(interp_t* I, value_t self, int argc, value_t* argv) {
    SELF_ARR;
    for (uint32_t i = 0; i < A->len; i++) if (v_strict_eq(A->items[i], ARG(0))) return v_bool(1);
    return v_bool(0);
}
static value_t a_slice(interp_t* I, value_t self, int argc, value_t* argv) {
    SELF_ARR;
    int64_t a = rel_index(argn(I, argc, argv, 0, 0), A->len);
    int64_t b = rel_index(argn(I, argc, argv, 1, A->len), A->len);
    obj_t* r = obj_new(I, OBJ_ARRAY);
    for (int64_t i = a; i < b; i++) arr_push(I, r, A->items[i]);
    return v_obj(r);
}
static value_t a_splice(interp_t* I, value_t self, int argc, value_t* argv) {
    SELF_ARR;
    int64_t start = rel_index(argn(I, argc, argv, 0, 0), A->len);
    int64_t del = argc > 1 ? to_int(v_tonum(I, argv[1])) : (int64_t)A->len - start;
    if (del < 0) del = 0;
    if (start + del > A->len) del = A->len - start;
    obj_t* removed = obj_new(I, OBJ_ARRAY);
    for (int64_t i = 0; i < del; i++) arr_push(I, removed, A->items[start + i]);
    int ins = argc > 2 ? argc - 2 : 0;
    uint32_t old_len = A->len;
    uint32_t new_len = (uint32_t)(old_len - del + ins);
    value_t* tmp = (value_t*)arena_alloc(I->A, (new_len + 1) * (uint32_t)sizeof(value_t));
    uint32_t k = 0;
    for (int64_t i = 0; i < start; i++) tmp[k++] = A->items[i];
    for (int i = 0; i < ins; i++) tmp[k++] = argv[2 + i];
    for (uint32_t i = (uint32_t)(start + del); i < old_len; i++) tmp[k++] = A->items[i];
    A->len = 0;
    for (uint32_t i = 0; i < k; i++) arr_push(I, A, tmp[i]);
    return v_obj(removed);
}
static value_t a_reverse(interp_t* I, value_t self, int argc, value_t* argv) {
    (void)argc; (void)argv;
    SELF_ARR;
    for (uint32_t i = 0; i < A->len / 2; i++) {
        value_t t = A->items[i];
        A->items[i] = A->items[A->len - 1 - i];
        A->items[A->len - 1 - i] = t;
    }
    return self;
}
static value_t a_concat(interp_t* I, value_t self, int argc, value_t* argv) {
    SELF_ARR;
    obj_t* r = obj_new(I, OBJ_ARRAY);
    for (uint32_t i = 0; i < A->len; i++) arr_push(I, r, A->items[i]);
    for (int k = 0; k < argc; k++) {
        if (argv[k].t == V_OBJ && argv[k].o->kind == OBJ_ARRAY)
            for (uint32_t i = 0; i < argv[k].o->len; i++) arr_push(I, r, argv[k].o->items[i]);
        else arr_push(I, r, argv[k]);
    }
    return v_obj(r);
}
static value_t a_fill(interp_t* I, value_t self, int argc, value_t* argv) {
    SELF_ARR;
    for (uint32_t i = 0; i < A->len; i++) A->items[i] = ARG(0);
    return self;
}
static value_t a_at(interp_t* I, value_t self, int argc, value_t* argv) {
    SELF_ARR;
    int64_t i = to_int(argn(I, argc, argv, 0, 0));
    if (i < 0) i += A->len;
    return (i >= 0 && i < A->len) ? A->items[i] : v_undef();
}

/* callbacks: fn(element, index, array) */
static value_t cb(interp_t* I, value_t fn, obj_t* A, uint32_t i) {
    value_t args[3] = { A->items[i], v_num(i), v_obj(A) };
    return call_value(I, fn, v_undef(), 3, args);
}

#define NEED_FN if (ARG(0).t != V_FUNC) { script_throw(I, "TypeError: callback is not a function"); return v_undef(); }

static value_t a_forEach(interp_t* I, value_t self, int argc, value_t* argv) {
    SELF_ARR; NEED_FN;
    for (uint32_t i = 0; i < A->len && !I->ctl; i++) cb(I, argv[0], A, i);
    return v_undef();
}
static value_t a_map(interp_t* I, value_t self, int argc, value_t* argv) {
    SELF_ARR; NEED_FN;
    obj_t* r = obj_new(I, OBJ_ARRAY);
    for (uint32_t i = 0; i < A->len && !I->ctl; i++) arr_push(I, r, cb(I, argv[0], A, i));
    return v_obj(r);
}
static value_t a_filter(interp_t* I, value_t self, int argc, value_t* argv) {
    SELF_ARR; NEED_FN;
    obj_t* r = obj_new(I, OBJ_ARRAY);
    for (uint32_t i = 0; i < A->len && !I->ctl; i++) if (v_truthy(I, cb(I, argv[0], A, i))) arr_push(I, r, A->items[i]);
    return v_obj(r);
}
static value_t a_find(interp_t* I, value_t self, int argc, value_t* argv) {
    SELF_ARR; NEED_FN;
    for (uint32_t i = 0; i < A->len && !I->ctl; i++) if (v_truthy(I, cb(I, argv[0], A, i))) return A->items[i];
    return v_undef();
}
static value_t a_findIndex(interp_t* I, value_t self, int argc, value_t* argv) {
    SELF_ARR; NEED_FN;
    for (uint32_t i = 0; i < A->len && !I->ctl; i++) if (v_truthy(I, cb(I, argv[0], A, i))) return v_num(i);
    return v_num(-1);
}
static value_t a_some(interp_t* I, value_t self, int argc, value_t* argv) {
    SELF_ARR; NEED_FN;
    for (uint32_t i = 0; i < A->len && !I->ctl; i++) if (v_truthy(I, cb(I, argv[0], A, i))) return v_bool(1);
    return v_bool(0);
}
static value_t a_every(interp_t* I, value_t self, int argc, value_t* argv) {
    SELF_ARR; NEED_FN;
    for (uint32_t i = 0; i < A->len && !I->ctl; i++) if (!v_truthy(I, cb(I, argv[0], A, i))) return v_bool(0);
    return v_bool(1);
}
static value_t a_reduce(interp_t* I, value_t self, int argc, value_t* argv) {
    SELF_ARR; NEED_FN;
    uint32_t i = 0;
    value_t acc;
    if (argc > 1) acc = argv[1];
    else if (A->len) acc = A->items[i++];
    else { script_throw(I, "TypeError: reduce of empty array with no initial value"); return v_undef(); }
    for (; i < A->len && !I->ctl; i++) {
        value_t args[4] = { acc, A->items[i], v_num(i), self };
        acc = call_value(I, argv[0], v_undef(), 4, args);
    }
    return acc;
}

static int sort_cmp(interp_t* I, value_t fn, value_t a, value_t b) {
    if (a.t == V_UNDEF) return b.t == V_UNDEF ? 0 : 1;
    if (b.t == V_UNDEF) return -1;
    if (fn.t == V_FUNC) {
        value_t args[2] = { a, b };
        num_t r = v_tonum(I, call_value(I, fn, v_undef(), 2, args));
        return r < 0 ? -1 : r > 0 ? 1 : 0;
    }
    return strcmp(sv(I, a)->s, sv(I, b)->s);
}

/* stable insertion sort (pages sort small lists) */
static void sort_values(interp_t* I, value_t* v, uint32_t n, value_t fn) {
    for (uint32_t i = 1; i < n && !I->ctl; i++) {
        value_t x = v[i];
        uint32_t j = i;
        while (j > 0 && sort_cmp(I, fn, v[j - 1], x) > 0 && !I->ctl) { v[j] = v[j - 1]; j--; }
        v[j] = x;
    }
}

static value_t a_sort(interp_t* I, value_t self, int argc, value_t* argv) {
    SELF_ARR;
    sort_values(I, A->items, A->len, ARG(0));
    return self;
}

/* ── numbers, objects, functions ── */

static value_t n_toFixed(interp_t* I, value_t self, int argc, value_t* argv) {
    num_t x = v_tonum(I, self);
    int d = (int)argn(I, argc, argv, 0, 0);
    if (d < 0) d = 0;
    if (d > 20) d = 20;
    if (num_isnan(x)) return v_str(I, "NaN");
    char b[80];
    int n = 0;
    if (x < 0) { b[n++] = '-'; x = -x; }
    num_t scale = 1;
    for (int i = 0; i < d; i++) scale *= 10;
    num_t r = num_floor(x * scale + 0.5L);
    num_t ip = num_floor(r / scale);
    num_t fp = r - ip * scale;
    char ib[32];
    num_format(ip, ib, sizeof(ib));
    n += ksnprintf(b + n, sizeof(b) - (size_t)n, "%s", ib);
    if (d) {
        b[n++] = '.';
        char fb[32];
        int64_t f = (int64_t)fp;
        for (int i = d - 1; i >= 0; i--) { fb[i] = (char)('0' + f % 10); f /= 10; }
        memcpy(b + n, fb, (size_t)d);
        n += d;
    }
    b[n] = 0;
    return v_str(I, b);
}

static value_t n_toString(interp_t* I, value_t self, int argc, value_t* argv) {
    int radix = (int)argn(I, argc, argv, 0, 10);
    num_t x = v_tonum(I, self);
    if (radix == 10 || radix < 2 || radix > 36 || num_isnan(x)) return v_strv(sv(I, v_num(x)));
    int64_t i = (int64_t)x;
    int neg = i < 0;
    uint64_t u = neg ? (uint64_t)-i : (uint64_t)i;
    char b[72];
    int n = 0;
    do { int d = (int)(u % (uint64_t)radix); b[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10); u /= (uint64_t)radix; } while (u);
    if (neg) b[n++] = '-';
    char r[72];
    for (int k = 0; k < n; k++) r[k] = b[n - 1 - k];
    return ret_str(I, r, (uint32_t)n);
}

static value_t o_hasOwnProperty(interp_t* I, value_t self, int argc, value_t* argv) {
    if (self.t != V_OBJ) return v_bool(0);
    int f;
    prop_get_raw(self.o, sv(I, ARG(0))->s, &f);
    return v_bool(f);
}
static value_t o_toString(interp_t* I, value_t self, int argc, value_t* argv) { (void)argc; (void)argv; return v_strv(sv(I, self)); }

static value_t f_call(interp_t* I, value_t self, int argc, value_t* argv) {
    return call_value(I, self, ARG(0), argc > 0 ? argc - 1 : 0, argv + 1);
}
static value_t f_apply(interp_t* I, value_t self, int argc, value_t* argv) {
    value_t args[16];
    int n = 0;
    if (argc > 1 && argv[1].t == V_OBJ && argv[1].o->kind == OBJ_ARRAY)
        for (uint32_t i = 0; i < argv[1].o->len && n < 16; i++) args[n++] = argv[1].o->items[i];
    return call_value(I, self, ARG(0), n, args);
}

/* ══ tables ═══════════════════════════════════════════════════════════ */

typedef struct { const char* name; native_fn f; } fn_entry_t;

static obj_t* make_table(interp_t* I, const fn_entry_t* t, int n) {
    obj_t* o = obj_new(I, OBJ_PLAIN);
    for (int i = 0; i < n; i++) obj_set(I, o, t[i].name, v_native(I, t[i].name, t[i].f));
    return o;
}

#define TABLE(I, arr) make_table(I, arr, (int)(sizeof(arr) / sizeof(arr[0])))

static const fn_entry_t STRING_METHODS[] = {
    { "charAt", s_charAt }, { "charCodeAt", s_charCodeAt }, { "at", s_at }, { "indexOf", s_indexOf },
    { "lastIndexOf", s_lastIndexOf }, { "includes", s_includes }, { "startsWith", s_startsWith },
    { "endsWith", s_endsWith }, { "slice", s_slice }, { "substring", s_substring }, { "substr", s_substr },
    { "toUpperCase", s_toUpperCase }, { "toLowerCase", s_toLowerCase }, { "trim", s_trim },
    { "trimStart", s_trimStart }, { "trimEnd", s_trimEnd }, { "split", s_split }, { "replace", s_replace },
    { "replaceAll", s_replaceAll }, { "repeat", s_repeat }, { "padStart", s_padStart }, { "padEnd", s_padEnd },
    { "concat", s_concat }, { "toString", s_toString }, { "valueOf", s_toString },
};
static const fn_entry_t ARRAY_METHODS[] = {
    { "push", a_push }, { "pop", a_pop }, { "shift", a_shift }, { "unshift", a_unshift }, { "join", a_join },
    { "indexOf", a_indexOf }, { "includes", a_includes }, { "slice", a_slice }, { "splice", a_splice },
    { "reverse", a_reverse }, { "concat", a_concat }, { "forEach", a_forEach }, { "map", a_map },
    { "filter", a_filter }, { "find", a_find }, { "findIndex", a_findIndex }, { "some", a_some },
    { "every", a_every }, { "reduce", a_reduce }, { "sort", a_sort }, { "fill", a_fill }, { "at", a_at },
    { "toString", a_join },
};
static const fn_entry_t NUMBER_METHODS[] = {
    { "toFixed", n_toFixed }, { "toString", n_toString },
};
static const fn_entry_t OBJECT_METHODS[] = {
    { "hasOwnProperty", o_hasOwnProperty }, { "toString", o_toString },
};
static const fn_entry_t DATE_METHODS[] = {
    { "getFullYear", d_getFullYear }, { "getMonth", d_getMonth }, { "getDate", d_getDate }, { "getDay", d_getDay },
    { "getHours", d_getHours }, { "getMinutes", d_getMinutes }, { "getSeconds", d_getSeconds },
    { "getMilliseconds", d_getMilliseconds }, { "getTime", d_getTime }, { "valueOf", d_getTime },
    { "toLocaleTimeString", d_toLocaleTimeString }, { "toLocaleDateString", d_toLocaleDateString },
    { "toISOString", d_toISOString }, { "toString", d_toString }, { "toLocaleString", d_toString },
};
static const fn_entry_t FUNC_METHODS[] = { { "call", f_call }, { "apply", f_apply } };
static const fn_entry_t MATH_FUNCS[] = {
    { "abs", m_abs }, { "floor", m_floor }, { "ceil", m_ceil }, { "round", m_round }, { "trunc", m_trunc },
    { "sign", m_sign }, { "sqrt", m_sqrt }, { "sin", m_sin }, { "cos", m_cos }, { "log", m_log },
    { "exp", m_exp }, { "pow", m_pow }, { "random", m_random }, { "max", m_max }, { "min", m_min },
};

value_t lib_member(interp_t* I, value_t ov, const char* key, int* found) {
    obj_t* t = NULL;
    switch (ov.t) {
    case V_STR: t = I->proto_string; break;
    case V_NUM: t = I->proto_number; break;
    case V_OBJ:
        t = ov.o->kind == OBJ_ARRAY ? I->proto_array : NULL;
        if (t) {
            value_t v = prop_get_raw(t, key, found);
            if (*found) return v;
        }
        t = I->proto_object;
        break;
    case V_BOOL: t = I->proto_object; break;
    default: break;
    }
    if (!t) { *found = 0; return v_undef(); }
    return prop_get_raw(t, key, found);
}

void lib_init(interp_t* I) {
    if (I->lang == LANG_PHP) return;               /* PHP: php_call_builtin() */
    I->proto_string = TABLE(I, STRING_METHODS);
    I->proto_array = TABLE(I, ARRAY_METHODS);
    I->proto_number = TABLE(I, NUMBER_METHODS);
    I->proto_object = TABLE(I, OBJECT_METHODS);
    I->proto_date = TABLE(I, DATE_METHODS);
    I->proto_func = TABLE(I, FUNC_METHODS);

    obj_t* console = obj_new(I, OBJ_PLAIN);
    obj_set(I, console, "log", v_native(I, "log", js_console_log));
    obj_set(I, console, "info", v_native(I, "info", js_console_log));
    obj_set(I, console, "warn", v_native(I, "warn", js_console_log));
    obj_set(I, console, "error", v_native(I, "error", js_console_log));
    script_def_global(I, "console", v_obj(console));

    obj_t* math = TABLE(I, MATH_FUNCS);
    obj_set(I, math, "PI", v_num(3.14159265358979323846L));
    obj_set(I, math, "E", v_num(2.71828182845904523536L));
    script_def_global(I, "Math", v_obj(math));

    obj_t* json = obj_new(I, OBJ_PLAIN);
    obj_set(I, json, "stringify", v_native(I, "stringify", js_stringify));
    obj_set(I, json, "parse", v_native(I, "parse", js_jparse));
    script_def_global(I, "JSON", v_obj(json));

    script_def_global(I, "parseInt", v_native(I, "parseInt", js_parseInt));
    script_def_global(I, "parseFloat", v_native(I, "parseFloat", js_parseFloat));
    script_def_global(I, "isNaN", v_native(I, "isNaN", js_isNaN));
    script_def_global(I, "isFinite", v_native(I, "isFinite", js_isFinite));
    script_def_global(I, "Boolean", v_native(I, "Boolean", js_Boolean));
    script_def_global(I, "encodeURIComponent", v_native(I, "encodeURIComponent", js_encodeURIComponent));
    script_def_global(I, "decodeURIComponent", v_native(I, "decodeURIComponent", js_decodeURIComponent));
    script_def_global(I, "Error", v_native(I, "Error", js_Error));
    script_def_global(I, "TypeError", v_native(I, "TypeError", js_Error));

    /* functions with properties: String.fromCharCode, Array.isArray, Object.keys, Date.now */
    value_t str = v_native(I, "String", js_String);
    value_t num = v_native(I, "Number", js_Number);
    value_t arr = v_native(I, "Array", js_Array);
    value_t obj = v_native(I, "Object", js_Object);
    value_t date = v_native(I, "Date", js_Date);
    script_def_global(I, "String", str);
    script_def_global(I, "Number", num);
    script_def_global(I, "Array", arr);
    script_def_global(I, "Object", obj);
    script_def_global(I, "Date", date);
    obj_t* sp = obj_new(I, OBJ_PLAIN);
    obj_set(I, sp, "fromCharCode", v_native(I, "fromCharCode", js_fromCharCode));
    obj_t* ap = obj_new(I, OBJ_PLAIN);
    obj_set(I, ap, "isArray", v_native(I, "isArray", js_isArray));
    obj_t* op = obj_new(I, OBJ_PLAIN);
    obj_set(I, op, "keys", v_native(I, "keys", js_keys));
    obj_set(I, op, "values", v_native(I, "values", js_values));
    obj_set(I, op, "entries", v_native(I, "entries", js_entries));
    obj_set(I, op, "assign", v_native(I, "assign", js_assign));
    obj_t* dp = obj_new(I, OBJ_PLAIN);
    obj_set(I, dp, "now", v_native(I, "now", js_Date_now));
    obj_t* np = obj_new(I, OBJ_PLAIN);
    obj_set(I, np, "isNaN", v_native(I, "isNaN", js_isNaN));
    obj_set(I, np, "parseFloat", v_native(I, "parseFloat", js_parseFloat));
    obj_set(I, np, "parseInt", v_native(I, "parseInt", js_parseInt));
    str.f->statics = sp;
    arr.f->statics = ap;
    obj.f->statics = op;
    date.f->statics = dp;
    num.f->statics = np;
    es_init(I);                                    /* script_es.c: the newer built-ins */
}

/* ══ PHP ══════════════════════════════════════════════════════════════ */

static value_t php_print_r(interp_t* I, value_t v, int depth, int dump);

static void out(interp_t* I, const char* s, uint32_t n) { if (I->out && n) I->out(I->out_ctx, s, n); }
static void outs(interp_t* I, const char* s) { out(I, s, (uint32_t)strlen(s)); }

static str_t* php_html_escape(interp_t* I, str_t* s) {
    char* b = (char*)arena_alloc(I->A, s->len * 6 + 1);
    uint32_t n = 0;
    for (uint32_t i = 0; i < s->len; i++) {
        const char* r = NULL;
        switch (s->s[i]) {
        case '&': r = "&amp;"; break;
        case '<': r = "&lt;"; break;
        case '>': r = "&gt;"; break;
        case '"': r = "&quot;"; break;
        case '\'': r = "&#039;"; break;
        }
        if (r) { uint32_t l = (uint32_t)strlen(r); memcpy(b + n, r, l); n += l; }
        else b[n++] = s->s[i];
    }
    return str_new(I, b, n);
}

/* sprintf: %s %d %f %.2f %05d %x %X %o %b %c %e %% with flags - + 0 space and widths */
static str_t* php_sprintf(interp_t* I, int argc, value_t* argv) {
    str_t* f = sv(I, ARG(0));
    str_t* outs_ = str_new(I, "", 0);
    int ai = 1;
    for (uint32_t i = 0; i < f->len; i++) {
        if (f->s[i] != '%') {
            uint32_t j = i;
            while (j < f->len && f->s[j] != '%') j++;
            outs_ = str_cat(I, outs_, str_new(I, f->s + i, j - i));
            i = j - 1;
            continue;
        }
        if (++i >= f->len) break;
        if (f->s[i] == '%') { outs_ = str_cat(I, outs_, str_new(I, "%", 1)); continue; }
        int left = 0, plus = 0;
        char padc = ' ';
        for (;; i++) {
            if (f->s[i] == '-') left = 1;
            else if (f->s[i] == '+') plus = 1;
            else if (f->s[i] == '0') padc = '0';
            else if (f->s[i] == ' ') padc = ' ';
            else if (f->s[i] == '\'' && i + 1 < f->len) padc = f->s[++i];
            else break;
        }
        int width = 0, prec = -1;
        while (i < f->len && is_digit_c(f->s[i])) width = width * 10 + (f->s[i++] - '0');
        if (i < f->len && f->s[i] == '.') { i++; prec = 0; while (i < f->len && is_digit_c(f->s[i])) prec = prec * 10 + (f->s[i++] - '0'); }
        if (i >= f->len) break;
        char conv = f->s[i];
        value_t a = ai < argc ? argv[ai++] : v_null();
        char buf[96];
        str_t* piece;
        switch (conv) {
        case 'd': case 'i': {
            num_t x = v_tonum(I, a);
            int64_t v = to_int(x < 0 ? -num_floor(-x) : num_floor(x));
            ksnprintf(buf, sizeof(buf), plus && v >= 0 ? "+%lld" : "%lld", (long long)v);
            piece = str_new(I, buf, (uint32_t)strlen(buf));
            break;
        }
        case 'u': ksnprintf(buf, sizeof(buf), "%llu", (unsigned long long)to_int(v_tonum(I, a))); piece = str_new(I, buf, (uint32_t)strlen(buf)); break;
        case 'f': case 'F': case 'e': {
            value_t args[1] = { v_num(prec < 0 ? 6 : prec) };
            value_t r = n_toFixed(I, v_num(v_tonum(I, a)), 1, args);
            piece = sv(I, r);
            if (plus && v_tonum(I, a) >= 0) piece = str_cat(I, str_new(I, "+", 1), piece);
            break;
        }
        case 'x': case 'X': case 'o': case 'b': {
            int radix = conv == 'o' ? 8 : conv == 'b' ? 2 : 16;
            value_t args[1] = { v_num(radix) };
            piece = sv(I, n_toString(I, v_num(num_floor(v_tonum(I, a))), 1, args));
            if (conv == 'X') piece = sv(I, s_toUpperCase(I, v_strv(piece), 0, NULL));
            break;
        }
        case 'c': buf[0] = (char)(int)v_tonum(I, a); piece = str_new(I, buf, 1); break;
        default: {
            piece = sv(I, a);
            if (prec >= 0 && (uint32_t)prec < piece->len) piece = str_new(I, piece->s, (uint32_t)prec);
            break;
        }
        }
        if (width > (int)piece->len) {
            str_t* pad = repeat_str(I, str_new(I, &padc, 1), width - (int)piece->len);
            if (left) piece = str_cat(I, piece, repeat_str(I, str_new(I, " ", 1), width - (int)piece->len));
            else if (padc == '0' && piece->len && (piece->s[0] == '-' || piece->s[0] == '+'))
                piece = str_cat(I, str_cat(I, str_new(I, piece->s, 1), pad), str_new(I, piece->s + 1, piece->len - 1));
            else piece = str_cat(I, pad, piece);
        }
        outs_ = str_cat(I, outs_, piece);
    }
    return outs_;
}

static str_t* php_date(interp_t* I, const char* fmt, int64_t ts) {
    tm_fields_t f;
    ms_to_fields(ts * 1000, &f);
    str_t* o = str_new(I, "", 0);
    char b[32];
    for (const char* p = fmt; *p; p++) {
        b[0] = 0;
        switch (*p) {
        case 'd': ksnprintf(b, sizeof(b), "%02d", f.d); break;
        case 'j': ksnprintf(b, sizeof(b), "%d", f.d); break;
        case 'D': ksnprintf(b, sizeof(b), "%.3s", DAY_NAMES[f.wd]); break;
        case 'l': kstrlcpy(b, DAY_NAMES[f.wd], sizeof(b)); break;
        case 'N': ksnprintf(b, sizeof(b), "%d", f.wd ? f.wd : 7); break;
        case 'w': ksnprintf(b, sizeof(b), "%d", f.wd); break;
        case 'm': ksnprintf(b, sizeof(b), "%02d", f.mo); break;
        case 'n': ksnprintf(b, sizeof(b), "%d", f.mo); break;
        case 'M': ksnprintf(b, sizeof(b), "%.3s", MONTH_NAMES[f.mo - 1]); break;
        case 'F': kstrlcpy(b, MONTH_NAMES[f.mo - 1], sizeof(b)); break;
        case 'Y': ksnprintf(b, sizeof(b), "%d", f.y); break;
        case 'y': ksnprintf(b, sizeof(b), "%02d", f.y % 100); break;
        case 'H': ksnprintf(b, sizeof(b), "%02d", f.h); break;
        case 'G': ksnprintf(b, sizeof(b), "%d", f.h); break;
        case 'h': ksnprintf(b, sizeof(b), "%02d", f.h % 12 ? f.h % 12 : 12); break;
        case 'g': ksnprintf(b, sizeof(b), "%d", f.h % 12 ? f.h % 12 : 12); break;
        case 'i': ksnprintf(b, sizeof(b), "%02d", f.mi); break;
        case 's': ksnprintf(b, sizeof(b), "%02d", f.s); break;
        case 'A': kstrlcpy(b, f.h < 12 ? "AM" : "PM", sizeof(b)); break;
        case 'a': kstrlcpy(b, f.h < 12 ? "am" : "pm", sizeof(b)); break;
        case 'U': ksnprintf(b, sizeof(b), "%lld", (long long)ts); break;
        case '\\': if (p[1]) { p++; b[0] = *p; b[1] = 0; } break;
        default: b[0] = *p; b[1] = 0; break;
        }
        o = str_cat(I, o, str_new(I, b, (uint32_t)strlen(b)));
    }
    return o;
}

static obj_t* need_array(interp_t* I, value_t v, const char* fn) {
    if (v.t != V_OBJ || v.o->kind != OBJ_PHPARRAY) {
        char m[96];
        ksnprintf(m, sizeof(m), "TypeError: %s(): Argument #1 must be of type array", fn);
        script_throw(I, m);
        return NULL;
    }
    return v.o;
}

static value_t php_print_r(interp_t* I, value_t v, int depth, int dump) {
    char pad[80];
    int pd = depth * 4 < 76 ? depth * 4 : 76;
    memset(pad, ' ', (size_t)pd);
    pad[pd] = 0;
    if (v.t == V_OBJ && (v.o->kind == OBJ_PHPARRAY || v.o->kind == OBJ_PLAIN)) {
        obj_t* o = v.o;
        if (dump) {
            char b[48];
            ksnprintf(b, sizeof(b), "array(%u) {\n", o->n);
            outs(I, b);
        } else {
            outs(I, "Array\n");
            outs(I, pad);
            outs(I, "(\n");
        }
        for (uint32_t i = 0; i < o->n; i++) {
            outs(I, pad);
            outs(I, dump ? "  [" : "    [");
            uint32_t idx;
            int numeric = 1;
            for (const char* k = o->props[i].key->s; *k; k++) if (!is_digit_c(*k)) numeric = 0;
            (void)idx;
            if (dump && !numeric) outs(I, "\"");
            outs(I, o->props[i].key->s);
            if (dump && !numeric) outs(I, "\"");
            outs(I, dump ? "]=>\n" : "] => ");
            if (dump) { outs(I, pad); outs(I, "  "); }
            php_print_r(I, o->props[i].v, depth + (dump ? 1 : 2), dump);
            if (!dump) outs(I, "\n");
        }
        outs(I, pad);
        outs(I, dump ? "}\n" : ")\n");
        return v_bool(1);
    }
    if (dump) {
        char b[96];
        switch (v.t) {
        case V_NULL: case V_UNDEF: outs(I, "NULL\n"); break;
        case V_BOOL: outs(I, v.b ? "bool(true)\n" : "bool(false)\n"); break;
        case V_NUM: {
            str_t* s = sv(I, v);
            int is_int = num_floor(v.n) == v.n && v.n < 9.0e18L && v.n > -9.0e18L;
            ksnprintf(b, sizeof(b), "%s(%s)\n", is_int ? "int" : "float", s->s);
            outs(I, b);
            break;
        }
        case V_STR:
            ksnprintf(b, sizeof(b), "string(%u) \"", v.s->len);
            outs(I, b);
            out(I, v.s->s, v.s->len);
            outs(I, "\"\n");
            break;
        default: outs(I, "object\n"); break;
        }
        return v_null();
    }
    str_t* s = sv(I, v);
    out(I, s->s, s->len);
    return v_bool(1);
}

static value_t php_sort(interp_t* I, obj_t* o, int reverse, int keep_keys, value_t cmp) {
    uint32_t n = o->n;
    value_t* vals = (value_t*)arena_alloc(I->A, (n + 1) * (uint32_t)sizeof(value_t));
    str_t** keys = (str_t**)arena_alloc(I->A, (n + 1) * (uint32_t)sizeof(str_t*));
    for (uint32_t i = 0; i < n; i++) { vals[i] = o->props[i].v; keys[i] = o->props[i].key; }
    /* sort indexes by value */
    uint32_t* idx = (uint32_t*)arena_alloc(I->A, (n + 1) * 4);
    for (uint32_t i = 0; i < n; i++) idx[i] = i;
    for (uint32_t i = 1; i < n; i++) {
        uint32_t x = idx[i], j = i;
        while (j > 0) {
            int c;
            value_t a = vals[idx[j - 1]], b = vals[x];
            if (cmp.t == V_FUNC) {
                value_t args[2] = { a, b };
                c = (int)v_tonum(I, call_value(I, cmp, v_undef(), 2, args));
            } else if (a.t == V_STR && b.t == V_STR) {
                c = strcmp(a.s->s, b.s->s);
            } else {
                num_t x1 = v_tonum(I, a), x2 = v_tonum(I, b);
                c = x1 < x2 ? -1 : x1 > x2 ? 1 : 0;
            }
            if (reverse) c = -c;
            if (c <= 0) break;
            idx[j] = idx[j - 1];
            j--;
        }
        idx[j] = x;
    }
    o->n = 0;
    o->next_index = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (keep_keys) prop_set_raw(I, o, keys[idx[i]], vals[idx[i]]);
        else php_array_push(I, o, vals[idx[i]]);
    }
    return v_bool(1);
}

/* ══ md5 / sha1 (PHP) ═════════════════════════════════════════════════ */

static uint32_t rol32(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

/* pads msg into a fresh block buffer: 64-byte blocks, 64-bit bit length (le or be) */
static uint8_t* hash_pad(interp_t* I, const uint8_t* m, uint32_t n, int big_endian, uint32_t* out_len) {
    uint32_t total = ((n + 8) / 64 + 1) * 64;
    uint8_t* b = (uint8_t*)arena_alloc(I->A, total);
    memcpy(b, m, n);
    b[n] = 0x80;
    uint64_t bits = (uint64_t)n * 8;
    for (int i = 0; i < 8; i++)
        b[total - 8 + i] = (uint8_t)(big_endian ? bits >> (56 - 8 * i) : bits >> (8 * i));
    *out_len = total;
    return b;
}

static void md5(interp_t* I, const uint8_t* msg, uint32_t n, uint8_t out[16]) {
    static const uint32_t K[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
        0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
        0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
        0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
        0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
        0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
        0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
        0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391 };
    static const int R[64] = { 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
                               5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
                               4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                               6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21 };
    uint32_t h[4] = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476 };
    uint32_t len;
    uint8_t* b = hash_pad(I, msg, n, 0, &len);
    for (uint32_t off = 0; off < len; off += 64) {
        uint32_t w[16];
        for (int i = 0; i < 16; i++)
            w[i] = (uint32_t)b[off + i * 4] | (uint32_t)b[off + i * 4 + 1] << 8 |
                   (uint32_t)b[off + i * 4 + 2] << 16 | (uint32_t)b[off + i * 4 + 3] << 24;
        uint32_t a = h[0], bb = h[1], c = h[2], d = h[3];
        for (int i = 0; i < 64; i++) {
            uint32_t f;
            int g;
            if (i < 16) { f = (bb & c) | (~bb & d); g = i; }
            else if (i < 32) { f = (d & bb) | (~d & c); g = (5 * i + 1) % 16; }
            else if (i < 48) { f = bb ^ c ^ d; g = (3 * i + 5) % 16; }
            else { f = c ^ (bb | ~d); g = (7 * i) % 16; }
            uint32_t t = d;
            d = c;
            c = bb;
            bb = bb + rol32(a + f + K[i] + w[g], R[i]);
            a = t;
        }
        h[0] += a; h[1] += bb; h[2] += c; h[3] += d;
    }
    for (int i = 0; i < 4; i++)
        for (int k = 0; k < 4; k++) out[i * 4 + k] = (uint8_t)(h[i] >> (8 * k));
}

static void sha1(interp_t* I, const uint8_t* msg, uint32_t n, uint8_t out[20]) {
    uint32_t h[5] = { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0 };
    uint32_t len;
    uint8_t* b = hash_pad(I, msg, n, 1, &len);
    for (uint32_t off = 0; off < len; off += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++)
            w[i] = (uint32_t)b[off + i * 4] << 24 | (uint32_t)b[off + i * 4 + 1] << 16 |
                   (uint32_t)b[off + i * 4 + 2] << 8 | b[off + i * 4 + 3];
        for (int i = 16; i < 80; i++) w[i] = rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], bb = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20) { f = (bb & c) | (~bb & d); k = 0x5A827999; }
            else if (i < 40) { f = bb ^ c ^ d; k = 0x6ED9EBA1; }
            else if (i < 60) { f = (bb & c) | (bb & d) | (c & d); k = 0x8F1BBCDC; }
            else { f = bb ^ c ^ d; k = 0xCA62C1D6; }
            uint32_t t = rol32(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rol32(bb, 30); bb = a; a = t;
        }
        h[0] += a; h[1] += bb; h[2] += c; h[3] += d; h[4] += e;
    }
    for (int i = 0; i < 5; i++)
        for (int k = 0; k < 4; k++) out[i * 4 + k] = (uint8_t)(h[i] >> (24 - 8 * k));
}

static value_t hex_digest(interp_t* I, const uint8_t* d, int n) {
    static const char hx[] = "0123456789abcdef";
    char s[41];
    for (int i = 0; i < n; i++) { s[i * 2] = hx[d[i] >> 4]; s[i * 2 + 1] = hx[d[i] & 15]; }
    return ret_str(I, s, (uint32_t)n * 2);
}

/* methods of PHP exception objects */
static value_t ex_getMessage(interp_t* I, value_t self, int argc, value_t* argv) {
    (void)argc; (void)argv;
    return self.t == V_OBJ ? obj_get(I, self.o, "message") : v_str(I, "");
}

static value_t ex_getCode(interp_t* I, value_t self, int argc, value_t* argv) {
    (void)argc; (void)argv;
    return self.t == V_OBJ ? obj_get(I, self.o, "code") : v_num(0);
}

static obj_t* exception_proto(interp_t* I) {
    value_t p = script_get_global(I, "(__exception_proto");
    if (p.t == V_OBJ) return p.o;
    obj_t* o = obj_new(I, OBJ_PLAIN);
    obj_set(I, o, "getMessage", v_native(I, "getMessage", ex_getMessage));
    obj_set(I, o, "getCode", v_native(I, "getCode", ex_getCode));
    script_def_global(I, "(__exception_proto", v_obj(o));
    return o;
}

value_t php_call_builtin(interp_t* I, const char* name, int argc, value_t* argv, int* found) {
    *found = 1;
    char nm[32];
    uint32_t k = 0;
    for (; name[k] && k < sizeof(nm) - 1; k++) nm[k] = lower(name[k]);
    nm[k] = 0;
#define IS(x) (strcmp(nm, x) == 0)
    if (I->php_ext) {
        int f = 0;
        value_t r = I->php_ext(I, nm, argc, argv, &f);
        if (f) return r;
    }
    /* output */
    if (IS("print")) { str_t* s = sv(I, ARG(0)); out(I, s->s, s->len); return v_num(1); }
    if (IS("printf")) { str_t* s = php_sprintf(I, argc, argv); out(I, s->s, s->len); return v_num(s->len); }
    if (IS("sprintf")) return v_strv(php_sprintf(I, argc, argv));
    if (IS("print_r")) return php_print_r(I, ARG(0), 0, 0);
    if (IS("var_dump")) { for (int i = 0; i < argc; i++) php_print_r(I, argv[i], 0, 1); return v_null(); }
    /* strings */
    if (IS("strlen")) return v_num(sv(I, ARG(0))->len);
    if (IS("strtoupper")) return s_case(I, ARG(0), 1);
    if (IS("strtolower")) return s_case(I, ARG(0), 0);
    if (IS("ucfirst") || IS("lcfirst")) {
        str_t* s = sv(I, ARG(0));
        if (!s->len) return v_strv(s);
        str_t* r = str_new(I, s->s, s->len);
        r->s[0] = IS("ucfirst") ? upper(r->s[0]) : lower(r->s[0]);
        return v_strv(r);
    }
    if (IS("ucwords")) {
        str_t* s = sv(I, ARG(0));
        str_t* r = str_new(I, s->s, s->len);
        for (uint32_t i = 0; i < r->len; i++) if (i == 0 || is_ws(r->s[i - 1])) r->s[i] = upper(r->s[i]);
        return v_strv(r);
    }
    if (IS("trim")) return s_trim_x(I, ARG(0), 1, 1);
    if (IS("ltrim")) return s_trim_x(I, ARG(0), 1, 0);
    if (IS("rtrim") || IS("chop")) return s_trim_x(I, ARG(0), 0, 1);
    if (IS("str_repeat")) return v_strv(repeat_str(I, sv(I, ARG(0)), to_int(v_tonum(I, ARG(1)))));
    if (IS("strrev")) {
        str_t* s = sv(I, ARG(0));
        str_t* r = str_new(I, s->s, s->len);
        for (uint32_t i = 0; i < r->len; i++) r->s[i] = s->s[s->len - 1 - i];
        return v_strv(r);
    }
    if (IS("str_replace")) {
        str_t* subject = sv(I, ARG(2));
        if (ARG(0).t == V_OBJ) {
            obj_t* from = ARG(0).o;
            for (uint32_t i = 0; i < from->n; i++) {
                value_t to = ARG(1).t == V_OBJ ? (i < ARG(1).o->n ? ARG(1).o->props[i].v : v_str(I, "")) : ARG(1);
                subject = replace_str(I, subject, sv(I, from->props[i].v), v_strv(sv(I, to)), 1);
            }
            return v_strv(subject);
        }
        return v_strv(replace_str(I, subject, sv(I, ARG(0)), v_strv(sv(I, ARG(1))), 1));
    }
    if (IS("substr")) {
        str_t* s = sv(I, ARG(0));
        int64_t start = to_int(v_tonum(I, ARG(1)));
        if (start < 0) { start += s->len; if (start < 0) start = 0; }
        if (start > s->len) return v_str(I, "");
        int64_t len = argc > 2 && ARG(2).t != V_NULL ? to_int(v_tonum(I, ARG(2))) : (int64_t)s->len - start;
        if (len < 0) len = (int64_t)s->len - start + len;
        if (len < 0) len = 0;
        if (start + len > s->len) len = s->len - start;
        return ret_str(I, s->s + start, (uint32_t)len);
    }
    if (IS("strpos") || IS("stripos")) {
        str_t* h = sv(I, ARG(0));
        str_t* n = sv(I, ARG(1));
        if (IS("stripos")) { h = sv(I, s_case(I, v_strv(h), 0)); n = sv(I, s_case(I, v_strv(n), 0)); }
        int p = find_sub(h->s, h->len, n->s, n->len, (uint32_t)to_int(argn(I, argc, argv, 2, 0)));
        return p < 0 ? v_bool(0) : v_num(p);
    }
    if (IS("str_contains")) { str_t* h = sv(I, ARG(0)); str_t* n = sv(I, ARG(1)); return v_bool(find_sub(h->s, h->len, n->s, n->len, 0) >= 0); }
    if (IS("str_starts_with")) return s_startsWith(I, ARG(0), 1, argv + 1);
    if (IS("str_ends_with")) return s_endsWith(I, ARG(0), 1, argv + 1);
    if (IS("str_pad")) {
        int type = (int)argn(I, argc, argv, 3, 1);          /* STR_PAD_RIGHT */
        str_t* pad = argc > 2 ? sv(I, argv[2]) : str_new(I, " ", 1);
        return v_strv(pad_str(I, sv(I, ARG(0)), to_int(v_tonum(I, ARG(1))), pad, type == 0));
    }
    if (IS("explode")) return v_obj(split_str(I, sv(I, ARG(1)), sv(I, ARG(0)), argc > 2 ? to_int(v_tonum(I, argv[2])) : -1, 1));
    if (IS("implode") || IS("join")) {
        value_t glue = ARG(0), arr = ARG(1);
        if (glue.t == V_OBJ) { value_t t = glue; glue = arr.t == V_UNDEF ? v_str(I, "") : arr; arr = t; }
        obj_t* o = need_array(I, arr, "implode");
        if (!o) return v_undef();
        str_t* g = sv(I, glue);
        str_t* r = str_new(I, "", 0);
        for (uint32_t i = 0; i < o->n; i++) {
            if (i) r = str_cat(I, r, g);
            r = str_cat(I, r, sv(I, o->props[i].v));
        }
        return v_strv(r);
    }
    if (IS("str_split")) {
        str_t* s = sv(I, ARG(0));
        int64_t n = argc > 1 ? to_int(v_tonum(I, argv[1])) : 1;
        if (n < 1) n = 1;
        obj_t* o = obj_new(I, OBJ_PHPARRAY);
        for (uint32_t i = 0; i < s->len; i += (uint32_t)n)
            php_array_push(I, o, ret_str(I, s->s + i, (uint32_t)(i + n > s->len ? s->len - i : n)));
        return v_obj(o);
    }
    if (IS("htmlspecialchars") || IS("htmlentities")) return v_strv(php_html_escape(I, sv(I, ARG(0))));
    if (IS("nl2br")) return v_strv(replace_str(I, sv(I, ARG(0)), str_new(I, "\n", 1), v_str(I, "<br />\n"), 1));
    if (IS("strip_tags")) {
        str_t* s = sv(I, ARG(0));
        char* b = (char*)arena_alloc(I->A, s->len + 1);
        uint32_t n = 0;
        int in = 0;
        for (uint32_t i = 0; i < s->len; i++) {
            if (s->s[i] == '<') in = 1;
            else if (s->s[i] == '>') in = 0;
            else if (!in) b[n++] = s->s[i];
        }
        return ret_str(I, b, n);
    }
    if (IS("urlencode") || IS("rawurlencode")) return js_encodeURIComponent(I, v_undef(), argc, argv);
    if (IS("urldecode") || IS("rawurldecode")) { str_t* s = sv(I, ARG(0)); return v_strv(url_decode(I, s->s, s->len, 1)); }
    if (IS("number_format")) {
        int dec = (int)argn(I, argc, argv, 1, 0);
        value_t a[1] = { v_num(dec) };
        str_t* f = sv(I, n_toFixed(I, v_num(v_tonum(I, ARG(0))), 1, a));
        const char* dp = argc > 2 ? sv(I, argv[2])->s : ".";
        const char* ts = argc > 3 ? sv(I, argv[3])->s : ",";
        /* group the integer part by thousands */
        const char* dot = strchr(f->s, '.');
        uint32_t il = dot ? (uint32_t)(dot - f->s) : f->len;
        uint32_t neg = f->s[0] == '-';
        str_t* r = str_new(I, f->s, neg);
        for (uint32_t i = neg; i < il; i++) {
            if (i > neg && (il - i) % 3 == 0) r = str_cat(I, r, str_new(I, ts, (uint32_t)strlen(ts)));
            r = str_cat(I, r, str_new(I, f->s + i, 1));
        }
        if (dot) r = str_cat(I, str_cat(I, r, str_new(I, dp, (uint32_t)strlen(dp))), str_new(I, dot + 1, f->len - il - 1));
        return v_strv(r);
    }
    if (IS("strcmp")) { int c = strcmp(sv(I, ARG(0))->s, sv(I, ARG(1))->s); return v_num(c < 0 ? -1 : c > 0); }
    if (IS("str_word_count")) {
        str_t* s = sv(I, ARG(0));
        int n = 0, in = 0;
        for (uint32_t i = 0; i < s->len; i++) {
            int w = (s->s[i] >= 'a' && s->s[i] <= 'z') || (s->s[i] >= 'A' && s->s[i] <= 'Z') || s->s[i] == '\'' || s->s[i] == '-';
            if (w && !in) n++;
            in = w;
        }
        return v_num(n);
    }
    if (IS("ord")) { str_t* s = sv(I, ARG(0)); return v_num(s->len ? (unsigned char)s->s[0] : 0); }
    if (IS("chr")) { char c = (char)(int)v_tonum(I, ARG(0)); return ret_str(I, &c, 1); }
    if (IS("md5")) { str_t* s = sv(I, ARG(0)); uint8_t d[16]; md5(I, (const uint8_t*)s->s, s->len, d); return hex_digest(I, d, 16); }
    if (IS("sha1")) { str_t* s = sv(I, ARG(0)); uint8_t d[20]; sha1(I, (const uint8_t*)s->s, s->len, d); return hex_digest(I, d, 20); }
    /* numbers */
    if (IS("intval")) { num_t x = v_tonum(I, ARG(0)); return v_num(num_isnan(x) ? 0 : x < 0 ? -num_floor(-x) : num_floor(x)); }
    if (IS("floatval")) return v_num(v_tonum(I, ARG(0)));
    if (IS("strval")) return v_strv(sv(I, ARG(0)));
    if (IS("boolval")) return v_bool(v_truthy(I, ARG(0)));
    if (IS("abs")) return m_abs(I, v_undef(), argc, argv);
    if (IS("floor")) return m_floor(I, v_undef(), argc, argv);
    if (IS("ceil")) return m_ceil(I, v_undef(), argc, argv);
    if (IS("sqrt")) return m_sqrt(I, v_undef(), argc, argv);
    if (IS("pow")) return m_pow(I, v_undef(), argc, argv);
    if (IS("pi")) return v_num(3.14159265358979323846L);
    if (IS("round")) {
        int d = (int)argn(I, argc, argv, 1, 0);
        num_t x = v_tonum(I, ARG(0)), scale = 1;
        for (int i = 0; i < d; i++) scale *= 10;
        num_t r = x < 0 ? -num_floor(-x * scale + 0.5L) : num_floor(x * scale + 0.5L);
        return v_num(r / scale);
    }
    if (IS("max") || IS("min")) {
        if (argc == 1 && ARG(0).t == V_OBJ) {
            obj_t* o = ARG(0).o;
            value_t vals[64];
            int n = 0;
            for (uint32_t i = 0; i < o->n && n < 64; i++) vals[n++] = o->props[i].v;
            return IS("max") ? m_max(I, v_undef(), n, vals) : m_min(I, v_undef(), n, vals);
        }
        return IS("max") ? m_max(I, v_undef(), argc, argv) : m_min(I, v_undef(), argc, argv);
    }
    if (IS("rand") || IS("mt_rand") || IS("random_int")) {
        int64_t lo = argc >= 2 ? to_int(v_tonum(I, argv[0])) : 0;
        int64_t hi = argc >= 2 ? to_int(v_tonum(I, argv[1])) : 2147483647;
        if (hi < lo) return v_num((num_t)lo);
        return v_num((num_t)(lo + (int64_t)(random_u32() % (uint32_t)(hi - lo + 1))));
    }
    if (IS("is_numeric")) {
        value_t v = ARG(0);
        if (v.t == V_NUM) return v_bool(1);
        if (v.t != V_STR) return v_bool(0);
        return v_bool(!num_isnan(str_tonum(v.s->s, 0)) && v.s->len);
    }
    if (IS("is_int") || IS("is_integer")) return v_bool(ARG(0).t == V_NUM && num_floor(ARG(0).n) == ARG(0).n);
    if (IS("is_float")) return v_bool(ARG(0).t == V_NUM && num_floor(ARG(0).n) != ARG(0).n);
    if (IS("is_string")) return v_bool(ARG(0).t == V_STR);
    if (IS("is_bool")) return v_bool(ARG(0).t == V_BOOL);
    if (IS("is_array")) return v_bool(ARG(0).t == V_OBJ && ARG(0).o->kind == OBJ_PHPARRAY);
    if (IS("is_null")) return v_bool(ARG(0).t == V_NULL || ARG(0).t == V_UNDEF);
    if (IS("gettype")) {
        static const char* const t[] = { "NULL", "NULL", "boolean", "double", "string", "array", "object" };
        value_t v = ARG(0);
        if (v.t == V_NUM && num_floor(v.n) == v.n) return v_str(I, "integer");
        return v_str(I, t[v.t]);
    }
    /* arrays */
    if (IS("count") || IS("sizeof")) {
        value_t v = ARG(0);
        if (v.t == V_OBJ) return v_num(v.o->kind == OBJ_ARRAY ? v.o->len : v.o->n);
        return v_num(v.t == V_NULL ? 0 : 1);
    }
    if (IS("array_push")) {
        obj_t* o = need_array(I, ARG(0), "array_push");
        if (!o) return v_undef();
        for (int i = 1; i < argc; i++) php_array_push(I, o, argv[i]);
        return v_num(o->n);
    }
    if (IS("array_pop") || IS("array_shift")) {
        obj_t* o = need_array(I, ARG(0), nm);
        if (!o || !o->n) return v_null();
        uint32_t at = IS("array_pop") ? o->n - 1 : 0;
        value_t v = o->props[at].v;
        prop_del(o, o->props[at].key->s);
        if (IS("array_shift")) {                    /* renumber */
            obj_t tmp = *o;
            o->n = 0; o->next_index = 0; o->props = NULL; o->cap = 0;
            for (uint32_t i = 0; i < tmp.n; i++) {
                uint32_t idx;
                int numeric = 1;
                for (const char* q = tmp.props[i].key->s; *q; q++) if (!is_digit_c(*q)) numeric = 0;
                (void)idx;
                if (numeric) php_array_push(I, o, tmp.props[i].v);
                else prop_set_raw(I, o, tmp.props[i].key, tmp.props[i].v);
            }
        }
        return v;
    }
    if (IS("array_keys") || IS("array_values")) {
        obj_t* o = need_array(I, ARG(0), nm);
        if (!o) return v_undef();
        obj_t* r = obj_new(I, OBJ_PHPARRAY);
        for (uint32_t i = 0; i < o->n; i++) {
            if (IS("array_values")) { php_array_push(I, r, o->props[i].v); continue; }
            str_t* key = o->props[i].key;
            int numeric = key->len > 0;
            for (uint32_t q = 0; q < key->len; q++) if (!is_digit_c(key->s[q])) numeric = 0;
            php_array_push(I, r, numeric ? v_num(str_tonum(key->s, 1)) : v_strv(key));
        }
        return v_obj(r);
    }
    if (IS("in_array") || IS("array_search")) {
        obj_t* o = need_array(I, ARG(1), nm);
        if (!o) return v_undef();
        int strict = argc > 2 && v_truthy(I, argv[2]);
        for (uint32_t i = 0; i < o->n; i++) {
            int eq = strict ? v_strict_eq(o->props[i].v, ARG(0)) : v_loose_eq(I, o->props[i].v, ARG(0));
            if (eq) return IS("in_array") ? v_bool(1) : v_strv(o->props[i].key);
        }
        return v_bool(0);
    }
    if (IS("array_key_exists") || IS("key_exists")) {
        obj_t* o = need_array(I, ARG(1), nm);
        if (!o) return v_undef();
        int f;
        php_array_get(I, o, ARG(0), &f);
        return v_bool(f);
    }
    if (IS("array_sum") || IS("array_product")) {
        obj_t* o = need_array(I, ARG(0), nm);
        if (!o) return v_undef();
        num_t acc = IS("array_sum") ? 0 : 1;
        for (uint32_t i = 0; i < o->n; i++) acc = IS("array_sum") ? acc + v_tonum(I, o->props[i].v) : acc * v_tonum(I, o->props[i].v);
        return v_num(acc);
    }
    if (IS("array_merge")) {
        obj_t* r = obj_new(I, OBJ_PHPARRAY);
        for (int k2 = 0; k2 < argc; k2++) {
            obj_t* o = need_array(I, argv[k2], nm);
            if (!o) return v_undef();
            for (uint32_t i = 0; i < o->n; i++) {
                str_t* key = o->props[i].key;
                int numeric = key->len > 0;
                for (uint32_t q = 0; q < key->len; q++) if (!is_digit_c(key->s[q])) numeric = 0;
                if (numeric) php_array_push(I, r, o->props[i].v);
                else prop_set_raw(I, r, key, o->props[i].v);
            }
        }
        return v_obj(r);
    }
    if (IS("array_reverse")) {
        obj_t* o = need_array(I, ARG(0), nm);
        if (!o) return v_undef();
        obj_t* r = obj_new(I, OBJ_PHPARRAY);
        for (uint32_t i = o->n; i > 0; i--) php_array_push(I, r, o->props[i - 1].v);
        return v_obj(r);
    }
    if (IS("array_slice")) {
        obj_t* o = need_array(I, ARG(0), nm);
        if (!o) return v_undef();
        int64_t off = rel_index(v_tonum(I, ARG(1)), o->n);
        int64_t len = argc > 2 && ARG(2).t != V_NULL ? to_int(v_tonum(I, argv[2])) : (int64_t)o->n - off;
        if (len < 0) len = (int64_t)o->n - off + len;
        obj_t* r = obj_new(I, OBJ_PHPARRAY);
        for (int64_t i = off; i < off + len && i < o->n; i++) php_array_push(I, r, o->props[i].v);
        return v_obj(r);
    }
    if (IS("array_map")) {
        obj_t* o = need_array(I, ARG(1), nm);
        if (!o) return v_undef();
        obj_t* r = obj_new(I, OBJ_PHPARRAY);
        for (uint32_t i = 0; i < o->n && !I->ctl; i++) {
            value_t v = o->props[i].v;
            value_t res = ARG(0).t == V_FUNC ? call_value(I, ARG(0), v_undef(), 1, &v) : v;
            prop_set_raw(I, r, o->props[i].key, res);
        }
        r->next_index = o->next_index;
        return v_obj(r);
    }
    if (IS("array_filter")) {
        obj_t* o = need_array(I, ARG(0), nm);
        if (!o) return v_undef();
        obj_t* r = obj_new(I, OBJ_PHPARRAY);
        for (uint32_t i = 0; i < o->n && !I->ctl; i++) {
            value_t v = o->props[i].v;
            value_t keep = ARG(1).t == V_FUNC ? call_value(I, ARG(1), v_undef(), 1, &v) : v;
            if (v_truthy(I, keep)) prop_set_raw(I, r, o->props[i].key, v);
        }
        return v_obj(r);
    }
    if (IS("range")) {
        num_t a = v_tonum(I, ARG(0)), b = v_tonum(I, ARG(1)), st = argn(I, argc, argv, 2, 1);
        if (st <= 0) st = 1;
        obj_t* r = obj_new(I, OBJ_PHPARRAY);
        if (a <= b) for (num_t x = a; x <= b && r->n < 100000; x += st) php_array_push(I, r, v_num(x));
        else for (num_t x = a; x >= b && r->n < 100000; x -= st) php_array_push(I, r, v_num(x));
        return v_obj(r);
    }
    if (IS("sort") || IS("rsort") || IS("asort") || IS("arsort") || IS("usort")) {
        obj_t* o = need_array(I, ARG(0), nm);
        if (!o) return v_undef();
        return php_sort(I, o, IS("rsort") || IS("arsort"), IS("asort") || IS("arsort"), IS("usort") ? ARG(1) : v_undef());
    }
    if (IS("ksort")) {
        obj_t* o = need_array(I, ARG(0), nm);
        if (!o) return v_undef();
        for (uint32_t i = 1; i < o->n; i++) {
            prop_t x = o->props[i];
            uint32_t j = i;
            while (j > 0 && strcmp(o->props[j - 1].key->s, x.key->s) > 0) { o->props[j] = o->props[j - 1]; j--; }
            o->props[j] = x;
        }
        return v_bool(1);
    }
    if (IS("array_unique")) {
        obj_t* o = need_array(I, ARG(0), nm);
        if (!o) return v_undef();
        obj_t* r = obj_new(I, OBJ_PHPARRAY);
        for (uint32_t i = 0; i < o->n; i++) {
            int dup = 0;
            for (uint32_t j = 0; j < r->n && !dup; j++) dup = v_loose_eq(I, r->props[j].v, o->props[i].v);
            if (!dup) prop_set_raw(I, r, o->props[i].key, o->props[i].v);
        }
        return v_obj(r);
    }
    if (IS("array_combine")) {
        obj_t* ks = need_array(I, ARG(0), nm);
        obj_t* vs = ks ? need_array(I, ARG(1), nm) : NULL;
        if (!vs) return v_undef();
        obj_t* r = obj_new(I, OBJ_PHPARRAY);
        for (uint32_t i = 0; i < ks->n && i < vs->n; i++) php_array_set(I, r, ks->props[i].v, vs->props[i].v);
        return v_obj(r);
    }
    if (IS("array_fill")) {
        obj_t* r = obj_new(I, OBJ_PHPARRAY);
        int64_t start = to_int(v_tonum(I, ARG(0))), n = to_int(v_tonum(I, ARG(1)));
        for (int64_t i = 0; i < n && i < 100000; i++) php_array_set(I, r, v_num((num_t)(start + i)), ARG(2));
        return v_obj(r);
    }
    /* JSON */
    if (IS("json_encode")) {
        str_t* o = str_new(I, "", 0);
        json_val(I, &o, ARG(0), 0);
        return v_strv(o);
    }
    if (IS("json_decode")) {
        str_t* s = sv(I, ARG(0));
        jparse_t J = { s->s, s->s + s->len, 0 };
        value_t v = jvalue(I, &J, 1, 0);
        return J.err ? v_null() : v;
    }
    /* time */
    if (IS("time")) return v_num((num_t)(now_ms() / 1000));
    if (IS("date")) {
        int64_t ts = argc > 1 ? to_int(v_tonum(I, argv[1])) : now_ms() / 1000;
        return v_strv(php_date(I, sv(I, ARG(0))->s, ts));
    }
    if (IS("mktime")) {
        tm_fields_t f;
        ms_to_fields(now_ms(), &f);
        int h = (int)argn(I, argc, argv, 0, f.h), mi = (int)argn(I, argc, argv, 1, f.mi), s = (int)argn(I, argc, argv, 2, f.s);
        int mo = (int)argn(I, argc, argv, 3, f.mo), d = (int)argn(I, argc, argv, 4, f.d), y = (int)argn(I, argc, argv, 5, f.y);
        return v_num((num_t)(days_from_civil(y, mo, d) * 86400 + h * 3600 + mi * 60 + s));
    }
    /* throw new Exception("...") */
    if (IS("exception") || IS("runtimeexception") || IS("invalidargumentexception") || IS("error")) {
        obj_t* e = obj_new(I, OBJ_PLAIN);
        obj_set(I, e, "name", v_str(I, name));
        obj_set(I, e, "message", argc ? v_strv(sv(I, argv[0])) : v_str(I, ""));
        obj_set(I, e, "code", argc > 1 ? v_num(v_tonum(I, argv[1])) : v_num(0));
        e->proto = exception_proto(I);
        return v_obj(e);
    }
    /* about */
    if (IS("phpversion")) return v_str(I, "8.3.0-banana");
    if (IS("php_uname")) return v_str(I, "Banana OS 0.5 banana-os " BANANA_ARCH);
    if (IS("php_sapi_name")) return v_str(I, "banana-httpd");
    if (IS("function_exists")) {
        char lname[64];
        ksnprintf(lname, sizeof(lname), "(%s", sv(I, ARG(0))->s);
        for (char* q = lname; *q; q++) *q = lower(*q);
        return v_bool(script_get_global(I, lname).t == V_FUNC);
    }
    if (IS("error_reporting") || IS("ini_set") || IS("header") || IS("set_time_limit") || IS("ob_start") ||
        IS("session_start") || IS("error_log")) return v_null();
    if (IS("die") || IS("exit")) {
        if (argc && argv[0].t == V_STR) out(I, argv[0].s->s, argv[0].s->len);
        script_throw(I, "__exit__");
        return v_undef();
    }
#undef IS
    *found = 0;
    return v_undef();
}
