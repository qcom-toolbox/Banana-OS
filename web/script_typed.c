#include "script_int.h"
#include "kstring.h"

/*
 * Binary data for JavaScript: ArrayBuffer, the typed arrays (Int8Array ...
 * Float64Array), DataView, and the UTF-8 helpers TextEncoder/TextDecoder
 * use (web/prelude.js). The bytes live in the page's arena; a typed array
 * is a host object that reads and writes them with the element type's
 * conversion (a Uint8Array wraps 256 to 0, a Uint8ClampedArray clamps...),
 * which hashing, compression and challenge scripts rely on. The array-like
 * methods taking callbacks are in web/prelude.js.
 */

enum { T_I8 = 0, T_U8, T_U8C, T_I16, T_U16, T_I32, T_U32, T_F32, T_F64, T_COUNT };
static const char* const T_NAMES[T_COUNT] = {
    "Int8Array", "Uint8Array", "Uint8ClampedArray", "Int16Array", "Uint16Array",
    "Int32Array", "Uint32Array", "Float32Array", "Float64Array",
};
static const uint8_t T_SIZE[T_COUNT] = { 1, 1, 1, 2, 2, 4, 4, 4, 8 };

typedef struct { uint8_t* data; uint32_t len; obj_t* obj; } abuf_t;
typedef struct { abuf_t* buf; uint32_t off, len; uint8_t kind; } tarr_t;      /* typed array / DataView */

#define ARG(i) ((i) < argc ? argv[i] : v_undef())

static obj_t* g_buf_proto;
static obj_t* g_view_proto;
static obj_t* g_typed_proto[T_COUNT];


static obj_t* statics(interp_t* I, value_t fn) {
    if (!fn.f->statics) fn.f->statics = obj_new(I, OBJ_PLAIN);
    return fn.f->statics;
}

static int idx_of(const char* k, uint32_t* out) {
    if (!*k) return 0;
    uint32_t v = 0;
    for (const char* p = k; *p; p++) {
        if (*p < '0' || *p > '9') return 0;
        if (v > 400000000u) return 0;
        v = v * 10 + (uint32_t)(*p - '0');
    }
    if (k[0] == '0' && k[1]) return 0;
    *out = v;
    return 1;
}

/* ── number conversions (ECMAScript ToInt32 & co) ── */

static int num_finite(num_t n) { return n == n && n < 1e300L && n > -1e300L; }

static uint32_t to_u32(num_t n) {
    if (!num_finite(n)) return 0;
    num_t t = n < 0 ? -num_floor(-n) : num_floor(n);
    num_t m = t - num_floor(t / 4294967296.0L) * 4294967296.0L;
    return (uint32_t)m;
}

static uint8_t clamp_u8(num_t n) {
    if (n != n || n <= 0) return 0;
    if (n >= 255) return 255;
    num_t f = num_floor(n);
    num_t d = n - f;
    uint32_t r = (uint32_t)f;
    if (d > 0.5L || (d == 0.5L && (r & 1))) r++;       /* round half to even */
    return (uint8_t)r;
}

static num_t load(const uint8_t* p, int kind) {
    switch (kind) {
    case T_I8:  return (int8_t)p[0];
    case T_U8: case T_U8C: return p[0];
    case T_I16: { int16_t v; memcpy(&v, p, 2); return v; }
    case T_U16: { uint16_t v; memcpy(&v, p, 2); return v; }
    case T_I32: { int32_t v; memcpy(&v, p, 4); return v; }
    case T_U32: { uint32_t v; memcpy(&v, p, 4); return v; }
    case T_F32: { float v; memcpy(&v, p, 4); return v; }
    default:    { double v; memcpy(&v, p, 8); return v; }
    }
}

static void store(uint8_t* p, int kind, num_t n) {
    uint32_t u = to_u32(n);
    switch (kind) {
    case T_I8: case T_U8: p[0] = (uint8_t)u; break;
    case T_U8C: p[0] = clamp_u8(n); break;
    case T_I16: case T_U16: { uint16_t v = (uint16_t)u; memcpy(p, &v, 2); break; }
    case T_I32: case T_U32: memcpy(p, &u, 4); break;
    case T_F32: { float v = (float)n; memcpy(p, &v, 4); break; }
    default:    { double v = (double)n; memcpy(p, &v, 8); break; }
    }
}

/* ── ArrayBuffer ── */

static value_t buf_get(interp_t* I, obj_t* self, const char* key, int* found) {
    (void)I;
    abuf_t* b = (abuf_t*)self->host;
    *found = 1;
    if (strcmp(key, "byteLength") == 0) return v_num(b->len);
    if (strcmp(key, "__tostring") == 0) return v_str(I, "[object ArrayBuffer]");
    *found = 0;
    return v_undef();
}
static const host_class_t g_buf_class = { "ArrayBuffer", buf_get, NULL };

static abuf_t* new_buf(interp_t* I, uint32_t len) {
    if (len > (64u << 20)) len = 64u << 20;
    abuf_t* b = (abuf_t*)arena_alloc(I->A, sizeof(abuf_t));
    b->data = (uint8_t*)arena_alloc(I->A, len ? len : 1);
    b->len = len;
    obj_t* o = obj_new(I, OBJ_HOST);
    o->hc = &g_buf_class;
    o->host = b;
    o->proto = g_buf_proto;
    b->obj = o;
    return b;
}

static abuf_t* buf_of(value_t v) {
    return v.t == V_OBJ && v.o->kind == OBJ_HOST && v.o->hc == &g_buf_class ? (abuf_t*)v.o->host : NULL;
}

static value_t js_ArrayBuffer(interp_t* I, value_t self, int argc, value_t* argv) {
    (void)self;
    num_t n = argc ? v_tonum(I, argv[0]) : 0;
    return v_obj(new_buf(I, num_finite(n) && n > 0 ? (uint32_t)n : 0)->obj);
}

static int clamp_index(interp_t* I, value_t v, int len, int def) {
    if (v.t == V_UNDEF) return def;
    num_t n = v_tonum(I, v);
    if (n != n) return 0;
    if (n < 0) { n += len; if (n < 0) n = 0; }
    if (n > len) n = len;
    return (int)n;
}

static value_t buf_slice(interp_t* I, value_t self, int argc, value_t* argv) {
    abuf_t* b = buf_of(self);
    if (!b) return v_undef();
    int s = clamp_index(I, ARG(0), (int)b->len, 0), e = clamp_index(I, ARG(1), (int)b->len, (int)b->len);
    abuf_t* nb = new_buf(I, e > s ? (uint32_t)(e - s) : 0);
    if (e > s) memcpy(nb->data, b->data + s, (size_t)(e - s));
    return v_obj(nb->obj);
}

/* ── typed arrays ── */

static value_t typed_get(interp_t* I, obj_t* self, const char* key, int* found);
static int typed_set(interp_t* I, obj_t* self, const char* key, value_t v);
static const host_class_t g_typed_class = { "TypedArray", typed_get, typed_set };

static tarr_t* typed_of(value_t v) {
    return v.t == V_OBJ && v.o->kind == OBJ_HOST && v.o->hc == &g_typed_class ? (tarr_t*)v.o->host : NULL;
}

static value_t typed_get(interp_t* I, obj_t* self, const char* key, int* found) {
    tarr_t* t = (tarr_t*)self->host;
    uint32_t i;
    *found = 1;
    if (idx_of(key, &i)) {
        if (i >= t->len) return v_undef();
        return v_num(load(t->buf->data + t->off + (uint64_t)i * T_SIZE[t->kind], t->kind));
    }
    if (strcmp(key, "length") == 0) return v_num(t->len);
    if (strcmp(key, "byteLength") == 0) return v_num((num_t)t->len * T_SIZE[t->kind]);
    if (strcmp(key, "byteOffset") == 0) return v_num(t->off);
    if (strcmp(key, "buffer") == 0) return v_obj(t->buf->obj);
    if (strcmp(key, "BYTES_PER_ELEMENT") == 0) return v_num(T_SIZE[t->kind]);
    if (strcmp(key, "__tostring") == 0) {
        /* String(u8) is "1,2,3" like an array */
        uint32_t cap = t->len * 24 + 1, n = 0;
        char* s = (char*)arena_alloc(I->A, cap);
        for (uint32_t k = 0; k < t->len && n + 24 < cap; k++) {
            num_t v = load(t->buf->data + t->off + (uint64_t)k * T_SIZE[t->kind], t->kind);
            if (k) s[n++] = ',';
            if (t->kind == T_F32 || t->kind == T_F64) {
                str_t* vs = v_tostr(I, v_num(v));
                memcpy(s + n, vs->s, vs->len > 22 ? 22 : vs->len);
                n += vs->len > 22 ? 22 : vs->len;
            } else {
                n += (uint32_t)ksnprintf(s + n, cap - n, "%lld", (long long)v);
            }
        }
        return v_strn(I, s, n);
    }
    *found = 0;
    return v_undef();
}

static int typed_set(interp_t* I, obj_t* self, const char* key, value_t v) {
    tarr_t* t = (tarr_t*)self->host;
    uint32_t i;
    if (idx_of(key, &i)) {
        if (i < t->len) store(t->buf->data + t->off + (uint64_t)i * T_SIZE[t->kind], t->kind, v_tonum(I, v));
        return 1;                                    /* out of range: ignored, like a browser */
    }
    if (strcmp(key, "length") == 0 || strcmp(key, "byteLength") == 0) return 1;   /* read-only */
    return 0;
}

static obj_t* new_typed(interp_t* I, int kind, abuf_t* b, uint32_t off, uint32_t len) {
    tarr_t* t = (tarr_t*)arena_alloc(I->A, sizeof(tarr_t));
    t->buf = b;
    t->off = off;
    t->len = len;
    t->kind = (uint8_t)kind;
    obj_t* o = obj_new(I, OBJ_HOST);
    o->hc = &g_typed_class;
    o->host = t;
    o->proto = g_typed_proto[kind];
    return o;
}

static obj_t* alloc_typed(interp_t* I, int kind, uint32_t len) {
    if (len > (64u << 20) / T_SIZE[kind]) len = (64u << 20) / T_SIZE[kind];
    return new_typed(I, kind, new_buf(I, len * T_SIZE[kind]), 0, len);
}

/* the length and the i-th element of anything array-like */
static uint32_t like_len(interp_t* I, value_t v) {
    if (v.t == V_STR) return v.s->len;
    if (v.t != V_OBJ) return 0;
    if (v.o->kind == OBJ_ARRAY) return v.o->len;
    num_t n = v_tonum(I, obj_get(I, v.o, "length"));
    return num_finite(n) && n > 0 ? (uint32_t)n : 0;
}

static num_t like_at(interp_t* I, value_t v, uint32_t i) {
    if (v.t == V_OBJ && v.o->kind == OBJ_ARRAY) return i < v.o->len ? v_tonum(I, v.o->items[i]) : 0;
    tarr_t* t = typed_of(v);
    if (t) return i < t->len ? load(t->buf->data + t->off + (uint64_t)i * T_SIZE[t->kind], t->kind) : 0;
    char k[16];
    ksnprintf(k, sizeof(k), "%u", i);
    return v.t == V_OBJ ? v_tonum(I, obj_get(I, v.o, k)) : 0;
}

static value_t js_typed(interp_t* I, value_t self, int argc, value_t* argv) {
    (void)self;
    int kind = (int)I->cur_native->data.n;
    value_t a = ARG(0);
    abuf_t* b = buf_of(a);
    if (b) {                                         /* a view of an ArrayBuffer */
        num_t o = argc > 1 ? v_tonum(I, argv[1]) : 0;
        uint32_t off = num_finite(o) && o > 0 ? (uint32_t)o : 0;
        if (off > b->len) off = b->len;
        uint32_t len = (b->len - off) / T_SIZE[kind];
        if (argc > 2 && argv[2].t != V_UNDEF) {
            num_t l = v_tonum(I, argv[2]);
            if (num_finite(l) && l >= 0 && (uint32_t)l < len) len = (uint32_t)l;
        }
        return v_obj(new_typed(I, kind, b, off, len));
    }
    if (a.t == V_OBJ || a.t == V_STR) {              /* a copy of an array / typed array / array-like */
        uint32_t n = like_len(I, a);
        obj_t* o = alloc_typed(I, kind, n);
        tarr_t* t = (tarr_t*)o->host;
        for (uint32_t i = 0; i < t->len; i++) store(t->buf->data + (uint64_t)i * T_SIZE[kind], kind, like_at(I, a, i));
        return v_obj(o);
    }
    num_t n = argc ? v_tonum(I, a) : 0;
    return v_obj(alloc_typed(I, kind, num_finite(n) && n > 0 ? (uint32_t)n : 0));
}

#define SELF_T tarr_t* t = typed_of(self); if (!t) return v_undef()
#define AT(t, i) ((t)->buf->data + (t)->off + (uint64_t)(i) * T_SIZE[(t)->kind])

static value_t t_set(interp_t* I, value_t self, int argc, value_t* argv) {
    SELF_T;
    value_t src = ARG(0);
    num_t o = argc > 1 ? v_tonum(I, argv[1]) : 0;
    uint32_t off = num_finite(o) && o > 0 ? (uint32_t)o : 0;
    uint32_t n = like_len(I, src);
    if (off + n > t->len) { script_throw(I, "RangeError: offset is out of bounds"); return v_undef(); }
    tarr_t* s = typed_of(src);
    if (s && s->kind == t->kind) {
        memmove(AT(t, off), AT(s, 0), (size_t)n * T_SIZE[t->kind]);   /* the same buffer is fine */
    } else {
        for (uint32_t i = 0; i < n; i++) store(AT(t, off + i), t->kind, like_at(I, src, i));
    }
    return v_undef();
}

static value_t t_subarray(interp_t* I, value_t self, int argc, value_t* argv) {
    SELF_T;
    int s = clamp_index(I, ARG(0), (int)t->len, 0), e = clamp_index(I, ARG(1), (int)t->len, (int)t->len);
    if (e < s) e = s;
    return v_obj(new_typed(I, t->kind, t->buf, t->off + (uint32_t)s * T_SIZE[t->kind], (uint32_t)(e - s)));
}

static value_t t_slice(interp_t* I, value_t self, int argc, value_t* argv) {
    SELF_T;
    int s = clamp_index(I, ARG(0), (int)t->len, 0), e = clamp_index(I, ARG(1), (int)t->len, (int)t->len);
    if (e < s) e = s;
    obj_t* o = alloc_typed(I, t->kind, (uint32_t)(e - s));
    memcpy(((tarr_t*)o->host)->buf->data, AT(t, s), (size_t)(e - s) * T_SIZE[t->kind]);
    return v_obj(o);
}

static value_t t_fill(interp_t* I, value_t self, int argc, value_t* argv) {
    SELF_T;
    num_t v = v_tonum(I, ARG(0));
    int s = clamp_index(I, ARG(1), (int)t->len, 0), e = clamp_index(I, ARG(2), (int)t->len, (int)t->len);
    for (int i = s; i < e; i++) store(AT(t, i), t->kind, v);
    return self;
}

static value_t t_indexOf(interp_t* I, value_t self, int argc, value_t* argv) {
    SELF_T;
    num_t v = v_tonum(I, ARG(0));
    int s = clamp_index(I, ARG(1), (int)t->len, 0);
    for (uint32_t i = (uint32_t)s; i < t->len; i++) if (load(AT(t, i), t->kind) == v) return v_num(i);
    return v_num(-1);
}

static value_t t_lastIndexOf(interp_t* I, value_t self, int argc, value_t* argv) {
    SELF_T;
    num_t v = v_tonum(I, ARG(0));
    for (int i = (int)t->len - 1; i >= 0; i--) if (load(AT(t, i), t->kind) == v) return v_num(i);
    return v_num(-1);
}

static value_t t_includes(interp_t* I, value_t self, int argc, value_t* argv) {
    value_t r = t_indexOf(I, self, argc, argv);
    return v_bool(r.t == V_NUM && r.n >= 0);
}

static value_t t_join(interp_t* I, value_t self, int argc, value_t* argv) {
    SELF_T;
    const char* sep = argc && argv[0].t != V_UNDEF ? v_cstr(I, argv[0]) : ",";
    uint32_t sl = (uint32_t)strlen(sep);
    uint32_t cap = t->len * (24 + sl) + 1, n = 0;
    char* s = (char*)arena_alloc(I->A, cap);
    for (uint32_t i = 0; i < t->len && n + 24 + sl < cap; i++) {
        if (i) { memcpy(s + n, sep, sl); n += sl; }
        str_t* vs = v_tostr(I, v_num(load(AT(t, i), t->kind)));
        uint32_t l = vs->len > 23 ? 23 : vs->len;
        memcpy(s + n, vs->s, l);
        n += l;
    }
    return v_strn(I, s, n);
}

static value_t t_reverse(interp_t* I, value_t self, int argc, value_t* argv) {
    (void)I; (void)argc; (void)argv;
    SELF_T;
    uint8_t tmp[8];
    int sz = T_SIZE[t->kind];
    for (uint32_t i = 0, j = t->len ? t->len - 1 : 0; i < j; i++, j--) {
        memcpy(tmp, AT(t, i), (size_t)sz);
        memcpy(AT(t, i), AT(t, j), (size_t)sz);
        memcpy(AT(t, j), tmp, (size_t)sz);
    }
    return self;
}

static value_t t_copyWithin(interp_t* I, value_t self, int argc, value_t* argv) {
    SELF_T;
    int len = (int)t->len;
    int to = clamp_index(I, ARG(0), len, 0), s = clamp_index(I, ARG(1), len, 0), e = clamp_index(I, ARG(2), len, len);
    int n = e - s;
    if (n > len - to) n = len - to;
    if (n > 0) memmove(AT(t, to), AT(t, s), (size_t)n * T_SIZE[t->kind]);
    return self;
}

static value_t t_from(interp_t* I, value_t self, int argc, value_t* argv) {
    (void)self;
    int kind = (int)I->cur_native->data.n;
    value_t a = ARG(0);
    uint32_t n = like_len(I, a);
    obj_t* o = alloc_typed(I, kind, n);
    tarr_t* t = (tarr_t*)o->host;
    value_t fn = ARG(1);
    for (uint32_t i = 0; i < t->len; i++) {
        num_t v = like_at(I, a, i);
        if (fn.t == V_FUNC) {
            value_t args[2] = { v_num(v), v_num(i) };
            v = v_tonum(I, call_value(I, fn, v_undef(), 2, args));
            if (I->ctl) return v_undef();
        }
        store(AT(t, i), kind, v);
    }
    return v_obj(o);
}

static value_t t_of(interp_t* I, value_t self, int argc, value_t* argv) {
    (void)self;
    int kind = (int)I->cur_native->data.n;
    obj_t* o = alloc_typed(I, kind, (uint32_t)argc);
    tarr_t* t = (tarr_t*)o->host;
    for (uint32_t i = 0; i < t->len; i++) store(AT(t, i), kind, v_tonum(I, argv[i]));
    return v_obj(o);
}

/* ── DataView ── */

static value_t view_get_prop(interp_t* I, obj_t* self, const char* key, int* found) {
    (void)I;
    tarr_t* t = (tarr_t*)self->host;
    *found = 1;
    if (strcmp(key, "byteLength") == 0) return v_num(t->len);
    if (strcmp(key, "byteOffset") == 0) return v_num(t->off);
    if (strcmp(key, "buffer") == 0) return v_obj(t->buf->obj);
    *found = 0;
    return v_undef();
}
static const host_class_t g_view_class = { "DataView", view_get_prop, NULL };

static value_t js_DataView(interp_t* I, value_t self, int argc, value_t* argv) {
    (void)self;
    abuf_t* b = buf_of(ARG(0));
    if (!b) { script_throw(I, "TypeError: DataView needs an ArrayBuffer"); return v_undef(); }
    num_t o = argc > 1 ? v_tonum(I, argv[1]) : 0;
    uint32_t off = num_finite(o) && o > 0 ? (uint32_t)o : 0;
    if (off > b->len) off = b->len;
    uint32_t len = b->len - off;
    if (argc > 2 && argv[2].t != V_UNDEF) {
        num_t l = v_tonum(I, argv[2]);
        if (num_finite(l) && l >= 0 && (uint32_t)l < len) len = (uint32_t)l;
    }
    tarr_t* t = (tarr_t*)arena_alloc(I->A, sizeof(tarr_t));
    t->buf = b;
    t->off = off;
    t->len = len;
    obj_t* v = obj_new(I, OBJ_HOST);
    v->hc = &g_view_class;
    v->host = t;
    v->proto = g_view_proto;
    return v_obj(v);
}

/* data.n: the element kind; getters/setters share one native each */
static value_t view_access(interp_t* I, value_t self, int argc, value_t* argv, int set) {
    if (self.t != V_OBJ || self.o->kind != OBJ_HOST || self.o->hc != &g_view_class) return v_undef();
    tarr_t* t = (tarr_t*)self.o->host;
    int kind = (int)I->cur_native->data.n;
    int sz = T_SIZE[kind];
    num_t o = v_tonum(I, ARG(0));
    if (!num_finite(o) || o < 0 || (uint32_t)o + (uint32_t)sz > t->len) {
        script_throw(I, "RangeError: offset is outside the bounds of the DataView");
        return v_undef();
    }
    uint8_t* p = t->buf->data + t->off + (uint32_t)o;
    int little = v_truthy(I, ARG(set ? 2 : 1));
    uint8_t tmp[8];
    if (set) {
        store(tmp, kind, v_tonum(I, ARG(1)));
        for (int i = 0; i < sz; i++) p[i] = little ? tmp[i] : tmp[sz - 1 - i];
        return v_undef();
    }
    for (int i = 0; i < sz; i++) tmp[i] = little ? p[i] : p[sz - 1 - i];
    return v_num(load(tmp, kind));
}
static value_t view_get(interp_t* I, value_t self, int argc, value_t* argv) { return view_access(I, self, argc, argv, 0); }
static value_t view_set(interp_t* I, value_t self, int argc, value_t* argv) { return view_access(I, self, argc, argv, 1); }

/* getBigInt64 / getBigUint64 / setBigInt64 / setBigUint64: data.n = 1 signed, + 2 to set */
static value_t view_big(interp_t* I, value_t self, int argc, value_t* argv) {
    if (self.t != V_OBJ || self.o->kind != OBJ_HOST || self.o->hc != &g_view_class) return v_undef();
    tarr_t* t = (tarr_t*)self.o->host;
    int mode = (int)I->cur_native->data.n, set = mode >= 2;
    num_t o = v_tonum(I, ARG(0));
    if (!num_finite(o) || o < 0 || (uint32_t)o + 8 > t->len) {
        script_throw(I, "RangeError: offset is outside the bounds of the DataView");
        return v_undef();
    }
    uint8_t* p = t->buf->data + t->off + (uint32_t)o;
    int little = v_truthy(I, ARG(set ? 2 : 1));
    if (set) {
        uint64_t v = bi_to_u64(I, ARG(1));
        for (int i = 0; i < 8; i++) p[little ? i : 7 - i] = (uint8_t)(v >> (8 * i));
        return v_undef();
    }
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)p[little ? i : 7 - i] << (8 * i);
    return bi_from_u64(I, v, mode & 1);
}

/* ── UTF-8 (TextEncoder / TextDecoder in prelude.js) ── */

static value_t js_utf8_encode(interp_t* I, value_t self, int argc, value_t* argv) {
    (void)self;
    str_t* s = v_tostr(I, ARG(0));                   /* strings are UTF-8 already */
    obj_t* o = alloc_typed(I, T_U8, s->len);
    memcpy(((tarr_t*)o->host)->buf->data, s->s, s->len);
    return v_obj(o);
}

static value_t js_utf8_decode(interp_t* I, value_t self, int argc, value_t* argv) {
    (void)self;
    value_t a = ARG(0);
    tarr_t* t = typed_of(a);
    abuf_t* b = buf_of(a);
    if (t) return v_strn(I, (const char*)AT(t, 0), t->len * T_SIZE[t->kind]);
    if (b) return v_strn(I, (const char*)b->data, b->len);
    uint32_t n = like_len(I, a);
    char* s = (char*)arena_alloc(I->A, n + 1);
    for (uint32_t i = 0; i < n; i++) s[i] = (char)(uint8_t)to_u32(like_at(I, a, i));
    return v_strn(I, s, n);
}

/* a Uint8Array holding a copy of d */
value_t typed_u8_new(interp_t* I, const uint8_t* d, uint32_t n) {
    obj_t* o = alloc_typed(I, T_U8, n);
    if (n) memcpy(((tarr_t*)o->host)->buf->data, d, n);
    return v_obj(o);
}

/* the bytes of a typed array, DataView or ArrayBuffer: 1, or 0 if v is none */
int typed_bytes(value_t v, const uint8_t** d, uint32_t* n) {
    tarr_t* t = typed_of(v);
    abuf_t* b = buf_of(v);
    if (t) { *d = AT(t, 0); *n = t->len * T_SIZE[t->kind]; return 1; }
    if (b) { *d = b->data; *n = b->len; return 1; }
    return 0;
}

static value_t js_isView(interp_t* I, value_t self, int argc, value_t* argv) {
    (void)I; (void)self;
    value_t a = ARG(0);
    return v_bool(typed_of(a) || (a.t == V_OBJ && a.o->kind == OBJ_HOST && a.o->hc == &g_view_class));
}

/* `x instanceof Uint8Array` and friends (script_es.c) */
int typed_instanceof(value_t v, const char* ctor) {
    if (v.t != V_OBJ || v.o->kind != OBJ_HOST) return -1;
    if (strcmp(ctor, "ArrayBuffer") == 0) return v.o->hc == &g_buf_class;
    if (strcmp(ctor, "DataView") == 0) return v.o->hc == &g_view_class;
    for (int k = 0; k < T_COUNT; k++)
        if (strcmp(ctor, T_NAMES[k]) == 0) return v.o->hc == &g_typed_class && ((tarr_t*)v.o->host)->kind == k;
    return -1;
}

static void method(interp_t* I, obj_t* o, const char* name, native_fn f) { obj_set(I, o, name, v_native(I, name, f)); }

void typed_init(interp_t* I) {
    g_buf_proto = obj_new(I, OBJ_PLAIN);
    method(I, g_buf_proto, "slice", buf_slice);
    value_t ab = v_native(I, "ArrayBuffer", js_ArrayBuffer);
    obj_set(I, statics(I, ab), "prototype", v_obj(g_buf_proto));
    method(I, statics(I, ab), "isView", js_isView);
    obj_set(I, g_buf_proto, "constructor", ab);
    script_def_global(I, "ArrayBuffer", ab);

    obj_t* common = obj_new(I, OBJ_PLAIN);           /* %TypedArray%.prototype */
    method(I, common, "set", t_set);
    method(I, common, "subarray", t_subarray);
    method(I, common, "slice", t_slice);
    method(I, common, "fill", t_fill);
    method(I, common, "indexOf", t_indexOf);
    method(I, common, "lastIndexOf", t_lastIndexOf);
    method(I, common, "includes", t_includes);
    method(I, common, "join", t_join);
    method(I, common, "reverse", t_reverse);
    method(I, common, "copyWithin", t_copyWithin);
    script_def_global(I, "__TypedArrayProto", v_obj(common));   /* prelude.js adds the rest */
    for (int k = 0; k < T_COUNT; k++) {
        g_typed_proto[k] = obj_new(I, OBJ_PLAIN);
        g_typed_proto[k]->proto = common;
        value_t c = v_native(I, T_NAMES[k], js_typed);
        c.f->data = v_num(k);
        obj_t* st = statics(I, c);
        obj_set(I, st, "prototype", v_obj(g_typed_proto[k]));
        obj_set(I, st, "BYTES_PER_ELEMENT", v_num(T_SIZE[k]));
        value_t f = v_native(I, "from", t_from);
        f.f->data = v_num(k);
        obj_set(I, st, "from", f);
        value_t of = v_native(I, "of", t_of);
        of.f->data = v_num(k);
        obj_set(I, st, "of", of);
        obj_set(I, g_typed_proto[k], "constructor", c);
        obj_set(I, g_typed_proto[k], "BYTES_PER_ELEMENT", v_num(T_SIZE[k]));
        script_def_global(I, T_NAMES[k], c);
    }

    g_view_proto = obj_new(I, OBJ_PLAIN);
    static const struct { const char* get; const char* set; int kind; } VM[] = {
        { "getInt8", "setInt8", T_I8 }, { "getUint8", "setUint8", T_U8 },
        { "getInt16", "setInt16", T_I16 }, { "getUint16", "setUint16", T_U16 },
        { "getInt32", "setInt32", T_I32 }, { "getUint32", "setUint32", T_U32 },
        { "getFloat32", "setFloat32", T_F32 }, { "getFloat64", "setFloat64", T_F64 },
    };
    for (uint32_t i = 0; i < sizeof(VM) / sizeof(VM[0]); i++) {
        value_t g = v_native(I, VM[i].get, view_get);
        g.f->data = v_num(VM[i].kind);
        obj_set(I, g_view_proto, VM[i].get, g);
        value_t s = v_native(I, VM[i].set, view_set);
        s.f->data = v_num(VM[i].kind);
        obj_set(I, g_view_proto, VM[i].set, s);
    }
    static const char* const BIG[] = { "getBigUint64", "getBigInt64", "setBigUint64", "setBigInt64" };
    for (int i = 0; i < 4; i++) {
        value_t f = v_native(I, BIG[i], view_big);
        f.f->data = v_num(i);
        obj_set(I, g_view_proto, BIG[i], f);
    }
    value_t dv = v_native(I, "DataView", js_DataView);
    obj_set(I, statics(I, dv), "prototype", v_obj(g_view_proto));
    obj_set(I, g_view_proto, "constructor", dv);
    script_def_global(I, "DataView", dv);

    script_def_global(I, "__utf8_encode", v_native(I, "__utf8_encode", js_utf8_encode));
    script_def_global(I, "__utf8_decode", v_native(I, "__utf8_decode", js_utf8_decode));
}
