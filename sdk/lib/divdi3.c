/* 64-bit division on 32-bit x86 (what libgcc would provide): GCC calls
 * these for long long / and % in i686 builds. */
typedef unsigned long long u64;
typedef long long s64;

u64 __udivmoddi4(u64 n, u64 d, u64* rem) {
    u64 q = 0, r = 0;
    if (d == 0) { if (rem) *rem = 0; return ~0ULL; }
    for (int i = 63; i >= 0; i--) {
        r = (r << 1) | ((n >> i) & 1);
        if (r >= d) { r -= d; q |= 1ULL << i; }
    }
    if (rem) *rem = r;
    return q;
}

u64 __udivdi3(u64 n, u64 d) { return __udivmoddi4(n, d, 0); }
u64 __umoddi3(u64 n, u64 d) { u64 r; __udivmoddi4(n, d, &r); return r; }

s64 __divdi3(s64 a, s64 b) {
    int neg = (a < 0) ^ (b < 0);
    u64 q = __udivmoddi4(a < 0 ? -(u64)a : (u64)a, b < 0 ? -(u64)b : (u64)b, 0);
    return neg ? -(s64)q : (s64)q;
}

s64 __moddi3(s64 a, s64 b) {
    u64 r;
    __udivmoddi4(a < 0 ? -(u64)a : (u64)a, b < 0 ? -(u64)b : (u64)b, &r);
    return a < 0 ? -(s64)r : (s64)r;
}

/* double / float <-> 64-bit integer conversions (also libgcc's on i686),
 * through 32-bit halves the compiler converts inline */
double __floatundidf(u64 v) { return (double)(unsigned int)(v >> 32) * 4294967296.0 + (double)(unsigned int)v; }
double __floatdidf(s64 v) { return v < 0 ? -__floatundidf(-(u64)v) : __floatundidf((u64)v); }
float  __floatundisf(u64 v) { return (float)__floatundidf(v); }
float  __floatdisf(s64 v) { return (float)__floatdidf(v); }

u64 __fixunsdfdi(double d) {
    if (!(d > 0)) return 0;
    if (d >= 18446744073709551616.0) return ~0ULL;
    unsigned int hi = (unsigned int)(d / 4294967296.0);
    double rest = d - (double)hi * 4294967296.0;
    if (rest < 0) { hi--; rest += 4294967296.0; }
    return ((u64)hi << 32) | (unsigned int)rest;
}
s64 __fixdfdi(double d) { return d < 0 ? -(s64)__fixunsdfdi(-d) : (s64)__fixunsdfdi(d); }
u64 __fixunssfdi(float f) { return __fixunsdfdi(f); }
s64 __fixsfdi(float f) { return __fixdfdi(f); }
