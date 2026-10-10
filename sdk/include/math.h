#ifndef _MATH_H
#define _MATH_H

/* Banana OS SDK math library (double precision; the f variants take and
 * return float). Accurate to about 1e-15 relative for the usual ranges. */

#define M_E        2.7182818284590452354
#define M_LOG2E    1.4426950408889634074
#define M_LOG10E   0.43429448190325182765
#define M_LN2      0.69314718055994530942
#define M_LN10     2.30258509299404568402
#define M_PI       3.14159265358979323846
#define M_PI_2     1.57079632679489661923
#define M_PI_4     0.78539816339744830962
#define M_SQRT2    1.41421356237309504880
#define HUGE_VAL   (__builtin_huge_val())
#define INFINITY   (__builtin_inff())
#define NAN        (__builtin_nanf(""))
#define isnan(x)   __builtin_isnan(x)
#define isinf(x)   __builtin_isinf(x)
#define isfinite(x) __builtin_isfinite(x)
#define signbit(x) __builtin_signbit(x)
#define isnormal(x) __builtin_isnormal(x)
#define FP_NAN       0
#define FP_INFINITE  1
#define FP_ZERO      2
#define FP_SUBNORMAL 3
#define FP_NORMAL    4
#define fpclassify(x) __builtin_fpclassify(FP_NAN, FP_INFINITE, FP_NORMAL, FP_SUBNORMAL, FP_ZERO, x)
#define HUGE_VALF  (__builtin_huge_valf())
#define M_SQRT1_2  0.70710678118654752440
#define M_1_PI     0.31830988618379067154
#define M_2_PI     0.63661977236758134308
#define M_2_SQRTPI 1.12837916709551257390

double sqrt(double x);
double cbrt(double x);
double fabs(double x);
double floor(double x);
double ceil(double x);
double trunc(double x);
double round(double x);
double fmod(double x, double y);
double modf(double x, double* ip);
double frexp(double x, int* e);
double ldexp(double x, int e);
double exp(double x);
double exp2(double x);
double log(double x);
double log2(double x);
double log10(double x);
double pow(double x, double y);
double sin(double x);
double cos(double x);
double tan(double x);
double asin(double x);
double acos(double x);
double atan(double x);
double atan2(double y, double x);
double sinh(double x);
double cosh(double x);
double tanh(double x);
double hypot(double x, double y);
double fmin(double a, double b);
double fmax(double a, double b);
double scalbn(double x, int n);
double copysign(double x, double y);
double expm1(double x);
double log1p(double x);
double erf(double x);
long   lrint(double x);
long long llrint(double x);
double rint(double x);
long   lround(double x);
long long llround(double x);
double nearbyint(double x);

float sqrtf(float x);
float fabsf(float x);
float floorf(float x);
float ceilf(float x);
float roundf(float x);
float fmodf(float x, float y);
float expf(float x);
float logf(float x);
float powf(float x, float y);
float sinf(float x);
float cosf(float x);
float tanf(float x);
float atanf(float x);
float atan2f(float y, float x);
float exp2f(float x);
float log2f(float x);
float log10f(float x);
float truncf(float x);
float rintf(float x);
long  lrintf(float x);
long long llrintf(float x);
float cbrtf(float x);
float hypotf(float x, float y);
float copysignf(float x, float y);
float fminf(float a, float b);
float fmaxf(float a, float b);
float asinf(float x);
float acosf(float x);
float sinhf(float x);
float coshf(float x);
float tanhf(float x);
float ldexpf(float x, int e);
float frexpf(float x, int* e);

#endif
