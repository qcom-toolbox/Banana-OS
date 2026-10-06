/* Banana OS SDK math library: portable C (argument reduction + series),
 * so it is the same on the i686 (x87) and x86_64 (SSE) builds. */
#include <math.h>

double fabs(double x) { return x < 0 ? -x : x; }
double fmin(double a, double b) { return a < b ? a : b; }
double fmax(double a, double b) { return a > b ? a : b; }

typedef union { double d; unsigned long long u; } dbits_t;

/* by clearing the fraction bits (no double -> long long conversion, which
 * would need libgcc on i686) */
double trunc(double x) {
    dbits_t b = { x };
    int e = (int)((b.u >> 52) & 0x7FF) - 1023;
    if (e >= 52) return x;                          /* whole already, or inf / NaN */
    if (e < 0) { b.u &= 1ull << 63; return b.d; }   /* |x| < 1: +-0 */
    b.u &= ~((1ull << (52 - e)) - 1);
    return b.d;
}
double floor(double x) { double t = trunc(x); return t > x ? t - 1.0 : t; }
double ceil(double x) { double t = trunc(x); return t < x ? t + 1.0 : t; }
double round(double x) { return x < 0 ? -floor(-x + 0.5) : floor(x + 0.5); }

double fmod(double x, double y) {
    if (y == 0 || isnan(x) || isnan(y) || isinf(x)) return NAN;
    double q = trunc(x / y);
    double r = x - q * y;
    /* large quotients lose precision: correct the sign drift */
    if (y > 0 ? (r >= y) : (r <= y)) r -= y;
    if (x >= 0 ? r < 0 : r > 0) r += fabs(y) * (x >= 0 ? 1 : -1);
    return r;
}

double modf(double x, double* ip) { double t = trunc(x); if (ip) *ip = t; return x - t; }

double sqrt(double x) {
    if (x < 0 || isnan(x)) return NAN;
    if (x == 0 || isinf(x)) return x;
    /* scale into [0.5, 2), Newton from a good guess */
    int e;
    double m = frexp(x, &e);
    if (e & 1) { m *= 2; e--; }
    double r = 0.5 + 0.5 * m;
    for (int i = 0; i < 6; i++) r = 0.5 * (r + m / r);
    return ldexp(r, e / 2);
}

double cbrt(double x) {
    if (x == 0 || isnan(x) || isinf(x)) return x;
    double a = fabs(x), r = exp(log(a) / 3);
    r = r - (r * r * r - a) / (3 * r * r);
    return x < 0 ? -r : r;
}

double hypot(double x, double y) {
    x = fabs(x); y = fabs(y);
    if (x < y) { double t = x; x = y; y = t; }
    if (x == 0) return 0;
    double r = y / x;
    return x * sqrt(1 + r * r);
}

/* frexp / ldexp through the bits of the double */
double frexp(double x, int* e) {
    dbits_t b = { x };
    int ex = (int)((b.u >> 52) & 0x7FF);
    if (ex == 0) {                                  /* zero or subnormal */
        if (x == 0) { *e = 0; return x; }
        x *= 18014398509481984.0;                   /* 2^54 */
        b.d = x;
        ex = (int)((b.u >> 52) & 0x7FF) - 54;
    } else if (ex == 0x7FF) { *e = 0; return x; }
    *e = ex - 1022;
    b.u = (b.u & ~(0x7FFull << 52)) | (1022ull << 52);
    return b.d;
}

double ldexp(double x, int e) {
    if (x == 0 || isinf(x) || isnan(x)) return x;
    while (e > 1023) { x *= 8.98846567431158e307; e -= 1023; }      /* 2^1023 */
    while (e < -1022) { x *= 2.2250738585072014e-308; e += 1022; }  /* 2^-1022 */
    dbits_t b;
    b.u = (unsigned long long)(e + 1023) << 52;                      /* 2^e */
    return x * b.d;
}

double exp(double x) {
    if (isnan(x)) return x;
    if (x > 709.78) return INFINITY;
    if (x < -745.2) return 0;
    /* x = k ln2 + r, |r| <= ln2/2: e^x = 2^k e^r */
    int k = (int)(x >= 0 ? x / M_LN2 + 0.5 : x / M_LN2 - 0.5);
    double r = x - k * 0.6931471803691238 - k * 1.9082149292705877e-10;
    double t = 1, s = 1;
    for (int i = 1; i < 22; i++) { t *= r / i; s += t; }
    return ldexp(s, k);
}

double exp2(double x) { return exp(x * M_LN2); }

double log(double x) {
    if (x < 0 || isnan(x)) return NAN;
    if (x == 0) return -INFINITY;
    if (isinf(x)) return x;
    int e;
    double m = frexp(x, &e);                        /* m in [0.5, 1) */
    if (m < M_SQRT2 / 2) { m *= 2; e--; }
    /* log(m) = 2 atanh((m-1)/(m+1)) */
    double z = (m - 1) / (m + 1), z2 = z * z, t = z, s = 0;
    for (int i = 1; i < 40; i += 2) { s += t / i; t *= z2; }
    return 2 * s + e * M_LN2;
}

double log2(double x) { return log(x) / M_LN2; }
double log10(double x) { return log(x) / M_LN10; }

double pow(double x, double y) {
    if (y == 0) return 1;
    if (isnan(x) || isnan(y)) return NAN;
    if (x == 0) return y > 0 ? 0 : INFINITY;
    double yi = trunc(y);
    if (yi == y && fabs(y) < 1e9) {                 /* whole powers: exact by squaring */
        int n = (int)y;
        int neg = n < 0;
        if (neg) n = -n;
        double r = 1, b = x;
        while (n) { if (n & 1) r *= b; b *= b; n >>= 1; }
        return neg ? 1 / r : r;
    }
    if (x < 0) return NAN;
    return exp(y * log(x));
}

/* sin / cos: reduce to [-pi/4, pi/4] around a multiple of pi/2 */
static double sin_k(double x) {
    double x2 = x * x, t = x, s = x;
    for (int i = 1; i < 12; i++) { t *= -x2 / ((2 * i) * (2 * i + 1)); s += t; }
    return s;
}
static double cos_k(double x) {
    double x2 = x * x, t = 1, s = 1;
    for (int i = 1; i < 12; i++) { t *= -x2 / ((2 * i - 1) * (2 * i)); s += t; }
    return s;
}
static double reduce(double x, int* quad) {
    double k = round(x / M_PI_2);
    *quad = (int)(k - 4 * floor(k / 4));            /* k mod 4, for any size of k */
    /* pi/2 in three parts for an accurate subtraction */
    return ((x - k * 1.5707963267341256) - k * 6.077100506506192e-11) - k * 2.0222662487959506e-21;
}

double sin(double x) {
    if (isnan(x) || isinf(x)) return NAN;
    int q;
    double r = reduce(x, &q);
    switch (q) { case 0: return sin_k(r); case 1: return cos_k(r); case 2: return -sin_k(r); default: return -cos_k(r); }
}

double cos(double x) {
    if (isnan(x) || isinf(x)) return NAN;
    int q;
    double r = reduce(x, &q);
    switch (q) { case 0: return cos_k(r); case 1: return -sin_k(r); case 2: return -cos_k(r); default: return sin_k(r); }
}

double tan(double x) { return sin(x) / cos(x); }

double atan(double x) {
    if (isnan(x)) return x;
    int neg = x < 0;
    if (neg) x = -x;
    int inv = x > 1;
    if (inv) x = 1 / x;
    /* atan(x) = 2 atan(x / (1 + sqrt(1 + x^2))): brings x below 0.42 */
    double y = x / (1 + sqrt(1 + x * x));
    double y2 = y * y, t = y, s = 0;
    for (int i = 1; i < 60; i += 2) { s += t / i; t *= -y2; }
    double r = 2 * s;
    if (inv) r = M_PI_2 - r;
    return neg ? -r : r;
}

double atan2(double y, double x) {
    if (x > 0) return atan(y / x);
    if (x < 0) return y >= 0 ? atan(y / x) + M_PI : atan(y / x) - M_PI;
    if (y > 0) return M_PI_2;
    if (y < 0) return -M_PI_2;
    return 0;
}

double asin(double x) { if (x < -1 || x > 1) return NAN; return atan2(x, sqrt(1 - x * x)); }
double acos(double x) { if (x < -1 || x > 1) return NAN; return atan2(sqrt(1 - x * x), x); }
double sinh(double x) { double e = exp(x); return (e - 1 / e) / 2; }
double cosh(double x) { double e = exp(x); return (e + 1 / e) / 2; }
double tanh(double x) {
    if (x > 20) return 1;
    if (x < -20) return -1;
    double e = exp(2 * x);
    return (e - 1) / (e + 1);
}

float sqrtf(float x) { return (float)sqrt(x); }
float fabsf(float x) { return x < 0 ? -x : x; }
float floorf(float x) { return (float)floor(x); }
float ceilf(float x) { return (float)ceil(x); }
float roundf(float x) { return (float)round(x); }
float fmodf(float x, float y) { return (float)fmod(x, y); }
float expf(float x) { return (float)exp(x); }
float logf(float x) { return (float)log(x); }
float powf(float x, float y) { return (float)pow(x, y); }
float sinf(float x) { return (float)sin(x); }
float cosf(float x) { return (float)cos(x); }
float tanf(float x) { return (float)tan(x); }
float atanf(float x) { return (float)atan(x); }
float atan2f(float y, float x) { return (float)atan2(y, x); }
