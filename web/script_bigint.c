/* BigInt: integers of any size. A BigInt value is boxed - an object whose
 * prototype is I->proto_bigint, the number itself in ->host - and never
 * changes: every operation makes a new one. The operators reach here from
 * binary() / N_UNARY / N_UPDATE when an operand is a BigInt. */
#include "script_int.h"
#include "kstring.h"

typedef struct {
    int      neg;
    uint32_t n;              /* limbs in use (0: the number is 0) */
    uint32_t d[];            /* magnitude, lowest limb first */
} big_t;

#define BIG_MAX_LIMBS 32768  /* a million bits: past that, RangeError */

static big_t* big_new(interp_t* I, uint32_t n) {
    big_t* b = (big_t*)arena_alloc(I->A, (uint32_t)sizeof(big_t) + (n ? n : 1) * 4u);
    b->n = n;
    return b;
}

static big_t* big_norm(big_t* b) {
    while (b->n && !b->d[b->n - 1]) b->n--;
    if (!b->n) b->neg = 0;
    return b;
}

static big_t* big_copy(interp_t* I, const big_t* a) {
    big_t* r = big_new(I, a->n);
    r->neg = a->neg;
    for (uint32_t i = 0; i < a->n; i++) r->d[i] = a->d[i];
    return r;
}

static big_t* big_u64(interp_t* I, uint64_t v, int neg) {
    big_t* r = big_new(I, 2);
    r->d[0] = (uint32_t)v;
    r->d[1] = (uint32_t)(v >> 32);
    r->neg = neg;
    return big_norm(r);
}

static value_t box(interp_t* I, big_t* b) {
    obj_t* o = obj_new(I, OBJ_BIGINT);
    o->proto = I->proto_bigint;
    o->host = b;
    return v_obj(o);
}

static big_t* B(value_t v) { return (big_t*)v.o->host; }

static int too_big(interp_t* I, uint32_t limbs) {
    if (limbs <= BIG_MAX_LIMBS) return 0;
    script_throw(I, "RangeError: Maximum BigInt size exceeded");
    return 1;
}

/* ── magnitudes ───────────────────────────────────────────────────── */

static int mag_cmp(const big_t* a, const big_t* b) {
    if (a->n != b->n) return a->n < b->n ? -1 : 1;
    for (uint32_t i = a->n; i-- > 0; )
        if (a->d[i] != b->d[i]) return a->d[i] < b->d[i] ? -1 : 1;
    return 0;
}

static big_t* mag_add(interp_t* I, const big_t* a, const big_t* b) {
    if (a->n < b->n) { const big_t* t = a; a = b; b = t; }
    big_t* r = big_new(I, a->n + 1);
    uint64_t c = 0;
    for (uint32_t i = 0; i < a->n; i++) {
        c += (uint64_t)a->d[i] + (i < b->n ? b->d[i] : 0);
        r->d[i] = (uint32_t)c;
        c >>= 32;
    }
    r->d[a->n] = (uint32_t)c;
    return big_norm(r);
}

/* |a| - |b|, |a| >= |b| */
static big_t* mag_sub(interp_t* I, const big_t* a, const big_t* b) {
    big_t* r = big_new(I, a->n);
    int64_t br = 0;
    for (uint32_t i = 0; i < a->n; i++) {
        int64_t t = (int64_t)a->d[i] - (i < b->n ? b->d[i] : 0) - br;
        br = t < 0;
        r->d[i] = (uint32_t)(t + (br ? ((int64_t)1 << 32) : 0));
    }
    return big_norm(r);
}

static big_t* mag_mul(interp_t* I, const big_t* a, const big_t* b) {
    if (!a->n || !b->n) return big_new(I, 0);
    if (too_big(I, a->n + b->n)) return NULL;
    big_t* r = big_new(I, a->n + b->n);
    for (uint32_t i = 0; i < a->n; i++) {
        uint64_t c = 0;
        for (uint32_t j = 0; j < b->n; j++) {
            uint64_t t = (uint64_t)a->d[i] * b->d[j] + r->d[i + j] + c;
            r->d[i + j] = (uint32_t)t;
            c = t >> 32;
        }
        r->d[i + b->n] = (uint32_t)c;
    }
    return big_norm(r);
}

/* a / m (m fits a limb), remainder in *rem */
static big_t* mag_divsmall(interp_t* I, const big_t* a, uint32_t m, uint32_t* rem) {
    big_t* q = big_new(I, a->n);
    uint64_t r = 0;
    for (uint32_t i = a->n; i-- > 0; ) {
        uint64_t cur = (r << 32) | a->d[i];
        q->d[i] = (uint32_t)(cur / m);
        r = cur % m;
    }
    *rem = (uint32_t)r;
    return big_norm(q);
}

static int mag_bit(const big_t* a, uint32_t bit) { return (int)((a->d[bit >> 5] >> (bit & 31)) & 1); }

static uint32_t mag_bits(const big_t* a) {
    if (!a->n) return 0;
    uint32_t top = a->d[a->n - 1], b = 0;
    while (top) { b++; top >>= 1; }
    return (a->n - 1) * 32 + b;
}

static big_t* mag_shr(interp_t* I, const big_t* a, uint32_t k);

/* |a| / |b| and |a| % |b| (b not 0) */
static void mag_divmod(interp_t* I, const big_t* a, const big_t* b, big_t** q, big_t** r) {
    if (mag_cmp(a, b) < 0) {
        *q = big_new(I, 0);
        *r = big_copy(I, a);
        (*r)->neg = 0;
        return;
    }
    if (b->n == 1) {
        uint32_t rem;
        *q = mag_divsmall(I, a, b->d[0], &rem);
        *r = big_u64(I, rem, 0);
        return;
    }
    uint32_t top = b->d[b->n - 1];
    if (!(top & (top - 1))) {                           /* a power of two (x mod 2^64): shift and mask */
        int pow2 = 1;
        for (uint32_t i = 0; i + 1 < b->n; i++) if (b->d[i]) pow2 = 0;
        if (pow2) {
            uint32_t k = mag_bits(b) - 1;
            big_t* rr = big_new(I, (k >> 5) + 1);
            for (uint32_t i = 0; i < rr->n; i++) rr->d[i] = i < a->n ? a->d[i] : 0;
            rr->d[k >> 5] &= (k & 31) ? (1u << (k & 31)) - 1 : 0;
            *q = mag_shr(I, a, k);
            (*q)->neg = 0;
            *r = big_norm(rr);
            return;
        }
    }
    /* long division, a bit at a time */
    big_t* qq = big_new(I, a->n);
    big_t* rr = big_new(I, b->n + 1);
    rr->n = 0;
    for (uint32_t bit = mag_bits(a); bit-- > 0; ) {
        uint32_t c = (uint32_t)mag_bit(a, bit);       /* rr = rr * 2 + bit */
        for (uint32_t i = 0; i < rr->n; i++) {
            uint32_t nc = rr->d[i] >> 31;
            rr->d[i] = (rr->d[i] << 1) | c;
            c = nc;
        }
        if (c) rr->d[rr->n++] = c;
        if (mag_cmp(rr, b) >= 0) {                     /* rr -= b */
            int64_t br = 0;
            for (uint32_t i = 0; i < rr->n; i++) {
                int64_t t = (int64_t)rr->d[i] - (i < b->n ? b->d[i] : 0) - br;
                br = t < 0;
                rr->d[i] = (uint32_t)(t + (br ? ((int64_t)1 << 32) : 0));
            }
            big_norm(rr);
            qq->d[bit >> 5] |= 1u << (bit & 31);
        }
    }
    *q = big_norm(qq);
    *r = rr;
}

/* ── signed arithmetic ────────────────────────────────────────────── */

static big_t* big_neg(interp_t* I, const big_t* a) {
    big_t* r = big_copy(I, a);
    if (r->n) r->neg = !r->neg;
    return r;
}

static big_t* big_add(interp_t* I, const big_t* a, const big_t* b) {
    if (a->neg == b->neg) { big_t* r = mag_add(I, a, b); r->neg = r->n ? a->neg : 0; return r; }
    int c = mag_cmp(a, b);
    if (c == 0) return big_new(I, 0);
    big_t* r = c > 0 ? mag_sub(I, a, b) : mag_sub(I, b, a);
    r->neg = c > 0 ? a->neg : b->neg;
    return big_norm(r);
}

static big_t* big_sub(interp_t* I, const big_t* a, const big_t* b) { return big_add(I, a, big_neg(I, b)); }

static int big_cmp(const big_t* a, const big_t* b) {
    if (a->neg != b->neg) return a->neg ? -1 : 1;
    int c = mag_cmp(a, b);
    return a->neg ? -c : c;
}

static num_t big_tonum(const big_t* a) {
    num_t v = 0;
    for (uint32_t i = a->n; i-- > 0; ) v = v * 4294967296.0 + a->d[i];
    return a->neg ? -v : v;
}

static num_t dfloor(num_t x) {
    if (x >= 9.2e18L || x <= -9.2e18L) return x;               /* already whole (num_t: 64-bit mantissa) */
    num_t t = (num_t)(int64_t)x;
    return t > x ? t - 1 : t;
}

/* a whole, finite number; NULL if it is not one */
static big_t* big_from_num(interp_t* I, num_t x) {
    if (x != x || x - x != 0 || dfloor(x) != x) return NULL;
    int neg = x < 0;
    if (neg) x = -x;
    big_t* r = big_new(I, 34);
    uint32_t n = 0;
    while (x >= 1 && n < 34) {
        num_t q = dfloor(x / 4294967296.0);
        r->d[n++] = (uint32_t)(x - q * 4294967296.0);
        x = q;
    }
    r->n = n;
    r->neg = neg;
    return big_norm(r);
}

/* "123", "-5", "0x1f", "0b101", "0o17", "" (0); NULL if not an integer */
static big_t* big_parse(interp_t* I, const char* s, int allow_sign) {
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
    const char* e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\n' || e[-1] == '\r')) e--;
    int neg = 0, radix = 10;
    if (allow_sign && s < e && (*s == '-' || *s == '+')) { neg = *s == '-'; s++; }
    if (e - s > 2 && s[0] == '0' && ((s[1] | 32) == 'x' || (s[1] | 32) == 'o' || (s[1] | 32) == 'b')) {
        radix = (s[1] | 32) == 'x' ? 16 : (s[1] | 32) == 'o' ? 8 : 2;
        s += 2;
    }
    uint32_t cap = (uint32_t)(e - s) / 2 + 2;
    if (too_big(I, cap)) return NULL;
    big_t* r = big_new(I, cap);
    r->n = 0;
    for (const char* p = s; p < e; p++) {
        int c = *p, d = c >= '0' && c <= '9' ? c - '0' : (c | 32) >= 'a' && (c | 32) <= 'z' ? (c | 32) - 'a' + 10 : 99;
        if (d >= radix) return NULL;
        uint64_t carry = (uint64_t)d;                  /* r = r * radix + d */
        for (uint32_t i = 0; i < r->n; i++) {
            uint64_t t = (uint64_t)r->d[i] * (uint32_t)radix + carry;
            r->d[i] = (uint32_t)t;
            carry = t >> 32;
        }
        if (carry) r->d[r->n++] = (uint32_t)carry;
    }
    r->neg = neg;
    return big_norm(r);
}

static str_t* big_str(interp_t* I, const big_t* a, int radix) {
    if (!a->n) return str_new(I, "0", 1);
    uint32_t cap = mag_bits(a) + 2;
    char* buf = (char*)arena_alloc(I->A, cap + 1);
    uint32_t n = 0;
    big_t* t = big_copy(I, a);
    t->neg = 0;
    while (t->n) {
        uint32_t rem;
        t = mag_divsmall(I, t, (uint32_t)radix, &rem);
        buf[n++] = "0123456789abcdefghijklmnopqrstuvwxyz"[rem];
    }
    if (a->neg) buf[n++] = '-';
    for (uint32_t i = 0; i < n / 2; i++) { char c = buf[i]; buf[i] = buf[n - 1 - i]; buf[n - 1 - i] = c; }
    return str_new(I, buf, n);
}

/* two's complement, w limbs */
static void to_tc(const big_t* a, uint32_t* out, uint32_t w) {
    for (uint32_t i = 0; i < w; i++) out[i] = i < a->n ? a->d[i] : 0;
    if (a->neg) {                                      /* ~(|a| - 1) */
        for (uint32_t i = 0; i < w; i++) { if (out[i]--) break; }
        for (uint32_t i = 0; i < w; i++) out[i] = ~out[i];
    }
}

static big_t* from_tc(interp_t* I, uint32_t* v, uint32_t w) {
    big_t* r = big_new(I, w);
    int neg = (v[w - 1] >> 31) & 1;
    for (uint32_t i = 0; i < w; i++) r->d[i] = neg ? ~v[i] : v[i];
    if (neg) {                                         /* -(~v + 1) */
        for (uint32_t i = 0; i < w; i++) { if (++r->d[i]) break; }
        r->neg = 1;
    }
    return big_norm(r);
}

static big_t* big_bitop(interp_t* I, int op, const big_t* a, const big_t* b) {
    uint32_t w = (a->n > b->n ? a->n : b->n) + 1;
    uint32_t* x = (uint32_t*)arena_alloc(I->A, w * 4);
    uint32_t* y = (uint32_t*)arena_alloc(I->A, w * 4);
    to_tc(a, x, w);
    to_tc(b, y, w);
    for (uint32_t i = 0; i < w; i++)
        x[i] = op == OP_BAND ? x[i] & y[i] : op == OP_BOR ? x[i] | y[i] : x[i] ^ y[i];
    return from_tc(I, x, w);
}

static big_t* mag_shl(interp_t* I, const big_t* a, uint32_t k) {
    if (!a->n) return big_new(I, 0);
    uint32_t ls = k >> 5, bs = k & 31;
    if (too_big(I, a->n + ls + 1)) return NULL;
    big_t* r = big_new(I, a->n + ls + 1);
    for (uint32_t i = 0; i < a->n; i++) {
        uint64_t v = (uint64_t)a->d[i] << bs;
        r->d[i + ls] |= (uint32_t)v;
        r->d[i + ls + 1] |= (uint32_t)(v >> 32);
    }
    r->neg = a->neg;
    return big_norm(r);
}

static big_t* mag_shr(interp_t* I, const big_t* a, uint32_t k) {
    uint32_t ls = k >> 5, bs = k & 31;
    if (ls >= a->n) return big_new(I, 0);
    big_t* r = big_new(I, a->n - ls);
    for (uint32_t i = 0; i < r->n; i++) {
        uint64_t lo = a->d[i + ls], hi = i + ls + 1 < a->n ? a->d[i + ls + 1] : 0;
        r->d[i] = (uint32_t)(((hi << 32) | lo) >> bs);
    }
    return big_norm(r);
}

/* a >> k, rounding toward -infinity */
static big_t* big_shr(interp_t* I, const big_t* a, uint32_t k) {
    if (!a->neg) return mag_shr(I, a, k);
    big_t* one = big_u64(I, 1, 0);
    big_t* t = mag_sub(I, a, one);                    /* -((|a| - 1) >> k) - 1 */
    t = mag_shr(I, t, k);
    t = mag_add(I, t, one);
    t->neg = t->n != 0;
    return t;
}

static int64_t big_toi64(const big_t* a) {
    uint64_t v = a->n ? a->d[0] : 0;
    if (a->n > 1) v |= (uint64_t)a->d[1] << 32;
    if (a->n > 2 || v > (uint64_t)1 << 40) v = (uint64_t)1 << 40;   /* far more than any real shift */
    return a->neg ? -(int64_t)v : (int64_t)v;
}

/* x mod 2^bits (0 <= result < 2^bits) */
static big_t* big_as_uint(interp_t* I, const big_t* a, uint32_t bits) {
    uint32_t w = bits / 32 + 2;
    if (a->n + 1 > w) w = a->n + 1;
    if (too_big(I, w)) return NULL;
    uint32_t* x = (uint32_t*)arena_alloc(I->A, w * 4);
    to_tc(a, x, w);
    big_t* r = big_new(I, bits / 32 + 1);
    for (uint32_t i = 0; i < r->n; i++) r->d[i] = x[i];
    if (bits & 31) r->d[bits >> 5] &= (1u << (bits & 31)) - 1;
    else r->d[bits >> 5] = 0;
    return big_norm(r);
}

/* ── the value side ───────────────────────────────────────────────── */

value_t bi_tostr_v(interp_t* I, value_t v, int radix) { return v_strv(big_str(I, B(v), radix)); }
str_t*  bi_tostr(interp_t* I, value_t v) { return big_str(I, B(v), 10); }
num_t   bi_tonum(value_t v) { return big_tonum(B(v)); }
int     bi_zero(value_t v) { return B(v)->n == 0; }
int     bi_eq(value_t a, value_t b) { return big_cmp(B(a), B(b)) == 0; }

value_t bi_add_int(interp_t* I, value_t v, int k) {
    return box(I, big_add(I, B(v), big_u64(I, (uint64_t)(k < 0 ? -k : k), k < 0)));
}

/* a BigInt literal (10n, 0xffn): parsed once, kept on its node */
value_t bi_literal(interp_t* I, node_t* n) {
    if (n->c) return v_obj((obj_t*)n->c);
    big_t* b = big_parse(I, n->s->s, 0);
    if (!b) b = big_new(I, 0);
    value_t v = box(I, b);
    n->c = (node_t*)v.o;
    return v;
}

static int cmp_mixed(interp_t* I, value_t a, value_t b, int* ok) {
    *ok = 1;
    if (bi_is(I, a) && bi_is(I, b)) return big_cmp(B(a), B(b));
    num_t x = bi_is(I, a) ? bi_tonum(a) : v_tonum(I, a);
    num_t y = bi_is(I, b) ? bi_tonum(b) : v_tonum(I, b);
    if (a.t == V_STR || b.t == V_STR) {               /* "123" vs 1n: as integers when they are */
        value_t s = a.t == V_STR ? a : b;
        big_t* p = big_parse(I, s.s->s, 1);
        if (!p) { *ok = 0; return 0; }
        return a.t == V_STR ? big_cmp(p, B(b)) : big_cmp(B(a), p);
    }
    if (x != x || y != y) { *ok = 0; return 0; }
    return x < y ? -1 : x > y ? 1 : 0;
}

value_t bi_binary(interp_t* I, int op, value_t a, value_t b, int* handled) {
    *handled = 1;
    int ok;
    switch (op) {
    case OP_SEQ: return v_bool(bi_is(I, a) && bi_is(I, b) && big_cmp(B(a), B(b)) == 0);
    case OP_SNE: return v_bool(!(bi_is(I, a) && bi_is(I, b) && big_cmp(B(a), B(b)) == 0));
    case OP_EQ: case OP_NE: {
        value_t o = bi_is(I, a) ? b : a;
        int eq;
        if (o.t == V_UNDEF || o.t == V_NULL || o.t == V_FUNC || (o.t == V_OBJ && !bi_is(I, o))) eq = 0;
        else { int c = cmp_mixed(I, a, b, &ok); eq = ok && c == 0; }
        return v_bool(op == OP_EQ ? eq : !eq);
    }
    case OP_LT: { int c = cmp_mixed(I, a, b, &ok); return v_bool(ok && c < 0); }
    case OP_GT: { int c = cmp_mixed(I, a, b, &ok); return v_bool(ok && c > 0); }
    case OP_LE: { int c = cmp_mixed(I, a, b, &ok); return v_bool(ok && c <= 0); }
    case OP_GE: { int c = cmp_mixed(I, a, b, &ok); return v_bool(ok && c >= 0); }
    case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV: case OP_MOD: case OP_POW:
    case OP_BAND: case OP_BOR: case OP_BXOR: case OP_SHL: case OP_SHR: case OP_USHR:
        break;
    default:
        *handled = 0;                                  /* in, instanceof, ??: as for any object */
        return v_undef();
    }
    if (op == OP_ADD && (a.t == V_STR || b.t == V_STR))
        return v_strv(str_cat(I, v_tostr(I, a), v_tostr(I, b)));
    if (!bi_is(I, a) || !bi_is(I, b)) {
        script_throw(I, "TypeError: Cannot mix BigInt and other types, use explicit conversions");
        return v_undef();
    }
    big_t* x = B(a);
    big_t* y = B(b);
    big_t* r = NULL;
    switch (op) {
    case OP_ADD: r = big_add(I, x, y); break;
    case OP_SUB: r = big_sub(I, x, y); break;
    case OP_MUL: r = mag_mul(I, x, y); if (r) { r->neg = r->n && (x->neg != y->neg); } break;
    case OP_DIV: case OP_MOD: {
        if (!y->n) { script_throw(I, "RangeError: Division by zero"); return v_undef(); }
        big_t *q, *m;
        mag_divmod(I, x, y, &q, &m);
        if (op == OP_DIV) { r = q; r->neg = r->n && (x->neg != y->neg); }
        else { r = m; r->neg = r->n && x->neg; }
        break;
    }
    case OP_POW: {
        if (y->neg) { script_throw(I, "RangeError: Exponent must be non-negative"); return v_undef(); }
        int64_t e = big_toi64(y);
        if ((uint64_t)e * mag_bits(x) > (uint64_t)BIG_MAX_LIMBS * 32 && mag_bits(x) > 1) { too_big(I, BIG_MAX_LIMBS + 1); return v_undef(); }
        r = big_u64(I, 1, 0);
        big_t* base = x;
        while (e > 0 && r && base) {
            if (e & 1) r = mag_mul(I, r, base);
            e >>= 1;
            if (e && r) base = mag_mul(I, base, base);
        }
        if (r) r->neg = r->n && x->neg && (big_toi64(y) & 1);
        break;
    }
    case OP_BAND: case OP_BOR: case OP_BXOR: r = big_bitop(I, op, x, y); break;
    case OP_SHL: case OP_SHR: {
        int64_t k = big_toi64(y);
        int left = (op == OP_SHL) == (k >= 0);
        uint64_t m = (uint64_t)(k < 0 ? -k : k);
        if (left) { if (m > (uint64_t)BIG_MAX_LIMBS * 32) { too_big(I, BIG_MAX_LIMBS + 1); return v_undef(); } r = mag_shl(I, x, (uint32_t)m); }
        else r = big_shr(I, x, m > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)m);
        break;
    }
    case OP_USHR:
        script_throw(I, "TypeError: BigInts have no unsigned right shift, use >> instead");
        return v_undef();
    }
    if (!r) return v_undef();                          /* (too big: thrown) */
    return box(I, r);
}

value_t bi_unary(interp_t* I, int op, value_t v) {
    if (op == OP_NEG) return box(I, big_neg(I, B(v)));
    if (op == OP_BITNOT) return box(I, big_sub(I, big_neg(I, B(v)), big_u64(I, 1, 0)));   /* ~x = -x - 1 */
    script_throw(I, "TypeError: Cannot convert a BigInt value to a number");
    return v_undef();
}

/* 64 bits (DataView, BigInt64Array) */
value_t bi_from_u64(interp_t* I, uint64_t v, int is_signed) {
    if (is_signed && (int64_t)v < 0) return box(I, big_u64(I, (uint64_t)(-(int64_t)v), 1));
    return box(I, big_u64(I, v, 0));
}

uint64_t bi_to_u64(interp_t* I, value_t v) {
    if (!bi_is(I, v)) return (uint64_t)(int64_t)v_tonum(I, v);
    big_t* u = big_as_uint(I, B(v), 64);
    if (!u) return 0;
    uint64_t r = u->n ? u->d[0] : 0;
    if (u->n > 1) r |= (uint64_t)u->d[1] << 32;
    return r;
}

/* ── BigInt(), BigInt.asUintN / asIntN, prototype ─────────────────── */

static value_t to_big(interp_t* I, value_t v) {
    if (bi_is(I, v)) return v;
    big_t* b = NULL;
    if (v.t == V_NUM) {
        b = big_from_num(I, v.n);
        if (!b) {
            char msg[96];
            ksnprintf(msg, sizeof(msg), "RangeError: The number %s cannot be converted to a BigInt because it is not an integer", v_cstr(I, v));
            script_throw(I, msg);
            return v_undef();
        }
    } else if (v.t == V_BOOL) {
        b = big_u64(I, v.b ? 1 : 0, 0);
    } else if (v.t == V_STR) {
        b = big_parse(I, v.s->s, 1);
        if (!b) {
            if (I->ctl) return v_undef();
            char msg[120];
            ksnprintf(msg, sizeof(msg), "SyntaxError: Cannot convert %.60s to a BigInt", v.s->s);
            script_throw(I, msg);
            return v_undef();
        }
    } else {
        char msg[120];
        ksnprintf(msg, sizeof(msg), "TypeError: Cannot convert %.60s to a BigInt", v_cstr(I, v));
        script_throw(I, msg);
        return v_undef();
    }
    return box(I, b);
}

static value_t js_BigInt(interp_t* I, value_t self, int argc, value_t* argv) {
    (void)self;
    return to_big(I, argc ? argv[0] : v_undef());
}

static value_t big_asN(interp_t* I, int argc, value_t* argv, int is_signed) {
    num_t bits = argc ? v_tonum(I, argv[0]) : 0;
    if (bits != bits || bits < 0 || bits > 1048576) { script_throw(I, "RangeError: Invalid value: not (convertible to) a safe integer"); return v_undef(); }
    value_t x = to_big(I, argc > 1 ? argv[1] : v_undef());
    if (I->ctl) return v_undef();
    uint32_t n = (uint32_t)bits;
    big_t* u = big_as_uint(I, B(x), n);
    if (!u) return v_undef();
    if (is_signed && n && u->n && mag_bits(u) == n) {   /* top bit set: minus 2^n */
        big_t* p = mag_shl(I, big_u64(I, 1, 0), n);
        if (!p) return v_undef();
        u = big_sub(I, u, p);
    }
    return box(I, u);
}
static value_t js_asUintN(interp_t* I, value_t self, int argc, value_t* argv) { (void)self; return big_asN(I, argc, argv, 0); }
static value_t js_asIntN(interp_t* I, value_t self, int argc, value_t* argv) { (void)self; return big_asN(I, argc, argv, 1); }

static value_t bp_toString(interp_t* I, value_t self, int argc, value_t* argv) {
    if (!bi_is(I, self)) { script_throw(I, "TypeError: BigInt.prototype.toString requires that 'this' be a BigInt"); return v_undef(); }
    int radix = argc && argv[0].t != V_UNDEF ? (int)v_tonum(I, argv[0]) : 10;
    if (radix < 2 || radix > 36) { script_throw(I, "RangeError: toString() radix must be between 2 and 36"); return v_undef(); }
    return bi_tostr_v(I, self, radix);
}
static value_t bp_valueOf(interp_t* I, value_t self, int argc, value_t* argv) { (void)I; (void)argc; (void)argv; return self; }

void bi_init(interp_t* I) {
    I->proto_bigint = obj_new(I, OBJ_PLAIN);
    obj_set(I, I->proto_bigint, "toString", v_native(I, "toString", bp_toString));
    obj_set(I, I->proto_bigint, "toLocaleString", v_native(I, "toLocaleString", bp_toString));
    obj_set(I, I->proto_bigint, "valueOf", v_native(I, "valueOf", bp_valueOf));
    value_t c = v_native(I, "BigInt", js_BigInt);
    c.f->statics = obj_new(I, OBJ_PLAIN);
    obj_set(I, c.f->statics, "prototype", v_obj(I->proto_bigint));
    obj_set(I, c.f->statics, "asUintN", v_native(I, "asUintN", js_asUintN));
    obj_set(I, c.f->statics, "asIntN", v_native(I, "asIntN", js_asIntN));
    obj_set(I, I->proto_bigint, "constructor", c);
    script_def_global(I, "BigInt", c);
}
