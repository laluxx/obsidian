#include "easing.h"
#include <math.h>
#include <stddef.h>
#include <string.h>

/// SIMD Feature Detection

#if defined(__SSE2__) || (defined(_MSC_VER) && defined(_M_X64))
  #define EASE_SSE2 1
  #include <emmintrin.h>
#endif

#if defined(__AVX2__)
  #define EASE_AVX2 1
  #include <immintrin.h>
#endif

#if defined(__ARM_NEON) || defined(__aarch64__)
  #define EASE_NEON 1
  #include <arm_neon.h>
#endif

/// Lookup Tables

EaseFn   ease_table[EASE_COUNT];
EaseFn64 ease_table64[EASE_COUNT];

/// 64-bit Counterparts
// These are the non-inline double-precision versions used by ease_table64.

static double ease64_linear            (double t) { return t; }
static double ease64_sine_in           (double t) { return 1.0 - cos(t * 1.5707963267948966); }
static double ease64_sine_out          (double t) { return sin(t * 1.5707963267948966); }
static double ease64_sine_in_out       (double t) { return 0.5 * (1.0 - cos(t * 3.14159265358979324)); }
static double ease64_quad_in           (double t) { return t * t; }
static double ease64_quad_out          (double t) { return t * (2.0 - t); }
static double ease64_quad_in_out       (double t) { return (t < 0.5) ? 2.0*t*t : -1.0 + (4.0 - 2.0*t)*t; }
static double ease64_cubic_in          (double t) { return t * t * t; }
static double ease64_cubic_out         (double t) { double s = t-1.0; return s*s*s+1.0; }
static double ease64_cubic_in_out      (double t) { return (t<0.5)?4.0*t*t*t:(t-1.0)*(2.0*t-2.0)*(2.0*t-2.0)+1.0; }
static double ease64_quart_in          (double t) { double t2=t*t; return t2*t2; }
static double ease64_quart_out         (double t) { double s=t-1.0,s2=s*s; return 1.0-s2*s2; }
static double ease64_quart_in_out      (double t) { if(t<0.5){double t2=t*t;return 8.0*t2*t2;} double s=t-1.0,s2=s*s; return 1.0-8.0*s2*s2; }
static double ease64_quint_in          (double t) { double t2=t*t; return t2*t2*t; }
static double ease64_quint_out         (double t) { double s=t-1.0,s2=s*s; return s2*s2*s+1.0; }
static double ease64_quint_in_out      (double t) { if(t<0.5){double t2=t*t;return 16.0*t2*t2*t;} double s=t-1.0,s2=s*s; return 1.0+16.0*s2*s2*s; }
static double ease64_expo_in           (double t) { return (t==0.0)?0.0:pow(2.0,10.0*t-10.0); }
static double ease64_expo_out          (double t) { return (t==1.0)?1.0:1.0-pow(2.0,-10.0*t); }
static double ease64_expo_in_out       (double t) { if(t==0.0)return 0.0; if(t==1.0)return 1.0; return (t<0.5)?pow(2.0,20.0*t-10.0)*0.5:(2.0-pow(2.0,-20.0*t+10.0))*0.5; }
static double ease64_circ_in           (double t) { return 1.0-sqrt(1.0-t*t); }
static double ease64_circ_out          (double t) { return sqrt((2.0-t)*t); }
static double ease64_circ_in_out       (double t) { return (t<0.5)?0.5*(1.0-sqrt(1.0-4.0*t*t)):0.5*(sqrt(-(2.0*t-3.0)*(2.0*t-1.0))+1.0); }
static double ease64_smooth_step       (double t) { return t*t*(3.0-2.0*t); }
static double ease64_smoother_step     (double t) { return t*t*t*(t*(t*6.0-15.0)+10.0); }
static double ease64_smoothest_step    (double t) { return t*t*t*t*(t*(t*(t*-20.0+70.0)-84.0)+35.0); }

static double ease64_back_in(double t)
{
    const double s = 1.70158;
    return t * t * ((s + 1.0) * t - s);
}
static double ease64_back_out(double t)
{
    const double s = 1.70158;
    double u = t - 1.0;
    return u * u * ((s + 1.0) * u + s) + 1.0;
}
static double ease64_back_in_out(double t)
{
    const double s = 1.70158 * 1.525;
    if (t < 0.5) { double u = 2.0*t; return 0.5*u*u*((s+1.0)*u-s); }
    double u = 2.0*t - 2.0;
    return 0.5*(u*u*((s+1.0)*u+s)+2.0);
}
static double ease64_elastic_out(double t)
{
    if (t == 0.0 || t == 1.0) return t;
    return pow(2.0,-10.0*t)*sin((t-0.075)*6.28318530717958648/0.3)+1.0;
}
static double ease64_elastic_in(double t)
{
    if (t == 0.0 || t == 1.0) return t;
    return -(pow(2.0,10.0*t-10.0)*sin((t-1.075)*6.28318530717958648/0.3));
}
static double ease64_elastic_in_out(double t)
{
    if (t == 0.0 || t == 1.0) return t;
    const double p = 0.45;
    if (t < 0.5) return -(0.5*pow(2.0,20.0*t-10.0)*sin((20.0*t-11.125)*6.28318530717958648/p));
    return 0.5*pow(2.0,-20.0*t+10.0)*sin((20.0*t-11.125)*6.28318530717958648/p)+1.0;
}
static double ease64_bounce_out(double t)
{
    if (t < 1.0/2.75)        return 7.5625*t*t;
    if (t < 2.0/2.75) { t -= 1.5  /2.75; return 7.5625*t*t+0.75;     }
    if (t < 2.5/2.75) { t -= 2.25 /2.75; return 7.5625*t*t+0.9375;   }
    t -= 2.625/2.75;           return 7.5625*t*t+0.984375;
}
static double ease64_bounce_in    (double t) { return 1.0 - ease64_bounce_out(1.0-t); }
static double ease64_bounce_in_out(double t)
{
    return (t<0.5) ? 0.5*ease64_bounce_in(t*2.0) : 0.5*ease64_bounce_out(t*2.0-1.0)+0.5;
}
static double ease64_spring(double t)
{
    return (1.0-(1.0-t)*(1.0-t))*(1.0+1.5*(1.0-t)*(1.0-t)*t);
}

/// Parametric Back

float ease_back_in_s(float t, float s)
{
    return t * t * ((s + 1.0f) * t - s);
}
float ease_back_out_s(float t, float s)
{
    float u = t - 1.0f;
    return u * u * ((s + 1.0f) * u + s) + 1.0f;
}
float ease_back_in_out_s(float t, float s)
{
    s *= 1.525f;
    if (t < 0.5f) { float u = 2.0f*t; return 0.5f*u*u*((s+1.0f)*u-s); }
    float u = 2.0f*t - 2.0f;
    return 0.5f*(u*u*((s+1.0f)*u+s)+2.0f);
}

/// Parametric Elastic

float ease_elastic_in_ap(float t, float a, float p)
{
    if (EASE_UNLIKELY(t == 0.0f || t == 1.0f)) return t;
    float s;
    if (a < 1.0f) { a = 1.0f; s = p * 0.25f; }
    else          { s = p / EASE_TAU * asinf(1.0f / a); }
    float u = t - 1.0f;
    return -(a * powf(2.0f, 10.0f * u) * sinf((u - s) * EASE_TAU / p));
}
float ease_elastic_out_ap(float t, float a, float p)
{
    if (EASE_UNLIKELY(t == 0.0f || t == 1.0f)) return t;
    float s;
    if (a < 1.0f) { a = 1.0f; s = p * 0.25f; }
    else          { s = p / EASE_TAU * asinf(1.0f / a); }
    return a * powf(2.0f, -10.0f * t) * sinf((t - s) * EASE_TAU / p) + 1.0f;
}
float ease_elastic_in_out_ap(float t, float a, float p)
{
    if (EASE_UNLIKELY(t == 0.0f || t == 1.0f)) return t;
    float s;
    if (a < 1.0f) { a = 1.0f; s = p * 0.25f; }
    else          { s = p / EASE_TAU * asinf(1.0f / a); }
    float u = 2.0f * t - 1.0f;
    if (u < 0.0f)
        return -0.5f * (a * powf(2.0f,  10.0f * u) * sinf((u - s) * EASE_TAU / p));
    return a * powf(2.0f, -10.0f * u) * sinf((u - s) * EASE_TAU / p) * 0.5f + 1.0f;
}

/// Parametric Spring

float ease_spring_damp(float t, float stiffness, float damping)
{
    // Closed-form critically-damped spring: x(t) = 1 - e^(-d*t)(cos(w*t) + (d/w)*sin(w*t))
    // When damping >= 1 the system is overdamped; we clamp to 0.9999 for safety.
    if (damping >= 1.0f) damping = 0.9999f;
    float w0  = sqrtf(stiffness);
    float zeta = damping;
    float wd   = w0 * sqrtf(1.0f - zeta * zeta);
    float decay = expf(-zeta * w0 * t);
    float damp_ratio = zeta / sqrtf(1.0f - zeta * zeta);
    return 1.0f - decay * (cosf(wd * t) + damp_ratio * sinf(wd * t));
}

/// Hermite Spline

float ease_hermite(float t, float p0, float p1, float m0, float m1)
{
    float t2 = t * t;
    float t3 = t2 * t;
    float h00 =  2.0f*t3 - 3.0f*t2 + 1.0f;
    float h10 =       t3 - 2.0f*t2 + t;
    float h01 = -2.0f*t3 + 3.0f*t2;
    float h11 =       t3 -      t2;
    return h00*p0 + h10*m0 + h01*p1 + h11*m1;
}

/// Catmull-Rom

float ease_catmull_rom(float t, float p0, float p1, float p2, float p3)
{
    float t2 = t * t;
    float t3 = t2 * t;
    return 0.5f * (
        (           -p0 + 3.0f*p1 - 3.0f*p2 + p3) * t3 +
        ( 2.0f*p0   -5.0f*p1 + 4.0f*p2 - p3) * t2 +
        (           -p0            + p2        ) * t  +
         2.0f*p1
    );
}

/// Power Generalisation

float ease_power_in    (float t, float n) { return powf(t,       n); }
float ease_power_out   (float t, float n) { return 1.0f - powf(1.0f - t, n); }
float ease_power_in_out(float t, float n)
{
    return (t < 0.5f) ? (0.5f *        powf(2.0f * t,       n))
                      : (1.0f - 0.5f * powf(2.0f * (1.0f - t), n));
}

/// Cubic Bézier Solver
// CSS-compatible: solve for parameter u from x, evaluate y(u).
// Newton–Raphson — 4 iterations is sufficient for single-precision.

EASE_INLINE float bezier_x(float u, float x1, float x2)
{
    // Bernstein form: B(u) = 3u(1-u)²x1 + 3u²(1-u)x2 + u³
    float i = 1.0f - u;
    return 3.0f*u*i*i*x1 + 3.0f*u*u*i*x2 + u*u*u;
}
EASE_INLINE float bezier_x_deriv(float u, float x1, float x2)
{
    float i = 1.0f - u;
    return 3.0f*i*i*x1 + 6.0f*u*i*(x2-x1) + 3.0f*u*u*(1.0f-x2);
}
EASE_INLINE float bezier_y(float u, float y1, float y2)
{
    float i = 1.0f - u;
    return 3.0f*u*i*i*y1 + 3.0f*u*u*i*y2 + u*u*u;
}

float ease_cubic_bezier(float t, float x1, float y1, float x2, float y2)
{
    if (EASE_UNLIKELY(t <= 0.0f)) return 0.0f;
    if (EASE_UNLIKELY(t >= 1.0f)) return 1.0f;

    // Linear shortcut
    if (x1 == y1 && x2 == y2) return t;

    // Solve x(u) = t for u via Newton–Raphson starting from u=t
    float u = t;
    for (int i = 0; i < 4; ++i)
    {
        float dx = bezier_x_deriv(u, x1, x2);
        if (fabsf(dx) < 1e-6f) break;
        u -= (bezier_x(u, x1, x2) - t) / dx;
    }
    return bezier_y(u, y1, y2);
}

/// Lookup Table Initialisation

void ease_init(void)
{
    ease_table[EASE_LINEAR]            = ease_linear;
    ease_table[EASE_SINE_IN]           = ease_sine_in;
    ease_table[EASE_SINE_OUT]          = ease_sine_out;
    ease_table[EASE_SINE_IN_OUT]       = ease_sine_in_out;
    ease_table[EASE_QUAD_IN]           = ease_quad_in;
    ease_table[EASE_QUAD_OUT]          = ease_quad_out;
    ease_table[EASE_QUAD_IN_OUT]       = ease_quad_in_out;
    ease_table[EASE_CUBIC_IN]          = ease_cubic_in;
    ease_table[EASE_CUBIC_OUT]         = ease_cubic_out;
    ease_table[EASE_CUBIC_IN_OUT]      = ease_cubic_in_out;
    ease_table[EASE_QUART_IN]          = ease_quart_in;
    ease_table[EASE_QUART_OUT]         = ease_quart_out;
    ease_table[EASE_QUART_IN_OUT]      = ease_quart_in_out;
    ease_table[EASE_QUINT_IN]          = ease_quint_in;
    ease_table[EASE_QUINT_OUT]         = ease_quint_out;
    ease_table[EASE_QUINT_IN_OUT]      = ease_quint_in_out;
    ease_table[EASE_EXPO_IN]           = ease_expo_in;
    ease_table[EASE_EXPO_OUT]          = ease_expo_out;
    ease_table[EASE_EXPO_IN_OUT]       = ease_expo_in_out;
    ease_table[EASE_CIRC_IN]           = ease_circ_in;
    ease_table[EASE_CIRC_OUT]          = ease_circ_out;
    ease_table[EASE_CIRC_IN_OUT]       = ease_circ_in_out;
    ease_table[EASE_BACK_IN]           = ease_back_in;
    ease_table[EASE_BACK_OUT]          = ease_back_out;
    ease_table[EASE_BACK_IN_OUT]       = ease_back_in_out;
    ease_table[EASE_ELASTIC_IN]        = ease_elastic_in;
    ease_table[EASE_ELASTIC_OUT]       = ease_elastic_out;
    ease_table[EASE_ELASTIC_IN_OUT]    = ease_elastic_in_out;
    ease_table[EASE_BOUNCE_IN]         = ease_bounce_in;
    ease_table[EASE_BOUNCE_OUT]        = ease_bounce_out;
    ease_table[EASE_BOUNCE_IN_OUT]     = ease_bounce_in_out;
    ease_table[EASE_SMOOTH_STEP]       = ease_smooth_step;
    ease_table[EASE_SMOOTHER_STEP]     = ease_smoother_step;
    ease_table[EASE_SMOOTHEST_STEP]    = ease_smoothest_step;
    ease_table[EASE_SPRING]            = ease_spring;
    ease_table[EASE_BEZIER_EASE]       = ease_bezier_ease;
    ease_table[EASE_BEZIER_EASE_IN]    = ease_bezier_ease_in;
    ease_table[EASE_BEZIER_EASE_OUT]   = ease_bezier_ease_out;
    ease_table[EASE_BEZIER_EASE_IN_OUT]= ease_bezier_ease_in_out;

    ease_table64[EASE_LINEAR]            = ease64_linear;
    ease_table64[EASE_SINE_IN]           = ease64_sine_in;
    ease_table64[EASE_SINE_OUT]          = ease64_sine_out;
    ease_table64[EASE_SINE_IN_OUT]       = ease64_sine_in_out;
    ease_table64[EASE_QUAD_IN]           = ease64_quad_in;
    ease_table64[EASE_QUAD_OUT]          = ease64_quad_out;
    ease_table64[EASE_QUAD_IN_OUT]       = ease64_quad_in_out;
    ease_table64[EASE_CUBIC_IN]          = ease64_cubic_in;
    ease_table64[EASE_CUBIC_OUT]         = ease64_cubic_out;
    ease_table64[EASE_CUBIC_IN_OUT]      = ease64_cubic_in_out;
    ease_table64[EASE_QUART_IN]          = ease64_quart_in;
    ease_table64[EASE_QUART_OUT]         = ease64_quart_out;
    ease_table64[EASE_QUART_IN_OUT]      = ease64_quart_in_out;
    ease_table64[EASE_QUINT_IN]          = ease64_quint_in;
    ease_table64[EASE_QUINT_OUT]         = ease64_quint_out;
    ease_table64[EASE_QUINT_IN_OUT]      = ease64_quint_in_out;
    ease_table64[EASE_EXPO_IN]           = ease64_expo_in;
    ease_table64[EASE_EXPO_OUT]          = ease64_expo_out;
    ease_table64[EASE_EXPO_IN_OUT]       = ease64_expo_in_out;
    ease_table64[EASE_CIRC_IN]           = ease64_circ_in;
    ease_table64[EASE_CIRC_OUT]          = ease64_circ_out;
    ease_table64[EASE_CIRC_IN_OUT]       = ease64_circ_in_out;
    ease_table64[EASE_BACK_IN]           = ease64_back_in;
    ease_table64[EASE_BACK_OUT]          = ease64_back_out;
    ease_table64[EASE_BACK_IN_OUT]       = ease64_back_in_out;
    ease_table64[EASE_ELASTIC_IN]        = ease64_elastic_in;
    ease_table64[EASE_ELASTIC_OUT]       = ease64_elastic_out;
    ease_table64[EASE_ELASTIC_IN_OUT]    = ease64_elastic_in_out;
    ease_table64[EASE_BOUNCE_IN]         = ease64_bounce_in;
    ease_table64[EASE_BOUNCE_OUT]        = ease64_bounce_out;
    ease_table64[EASE_BOUNCE_IN_OUT]     = ease64_bounce_in_out;
    ease_table64[EASE_SMOOTH_STEP]       = ease64_smooth_step;
    ease_table64[EASE_SMOOTHER_STEP]     = ease64_smoother_step;
    ease_table64[EASE_SMOOTHEST_STEP]    = ease64_smoothest_step;
    ease_table64[EASE_SPRING]            = ease64_spring;
    ease_table64[EASE_BEZIER_EASE]       = (EaseFn64)(void*)ease_bezier_ease; // f64 wraps f32
    ease_table64[EASE_BEZIER_EASE_IN]    = (EaseFn64)(void*)ease_bezier_ease_in;
    ease_table64[EASE_BEZIER_EASE_OUT]   = (EaseFn64)(void*)ease_bezier_ease_out;
    ease_table64[EASE_BEZIER_EASE_IN_OUT]= (EaseFn64)(void*)ease_bezier_ease_in_out;
}

/// SIMD Batch Paths
// Each batch function processes 4 (SSE2) or 8 (AVX2) floats per iteration,
// scalar tail handles remainder. NEON path mirrors SSE2 with intrinsic swap.

void ease_batch_linear(const float* EASE_RESTRICT t, float* EASE_RESTRICT out, int n)
{
    // memcpy is optimal; compiler will emit a rep movsb / SIMD move
    memcpy(out, t, (size_t)n * sizeof(float));
}

void ease_batch_quad_in(const float* EASE_RESTRICT t, float* EASE_RESTRICT out, int n)
{
    int i = 0;
#if defined(EASE_AVX2)
    for (; i <= n - 8; i += 8)
    {
        __m256 v = _mm256_loadu_ps(t + i);
        _mm256_storeu_ps(out + i, _mm256_mul_ps(v, v));
    }
#elif defined(EASE_SSE2)
    for (; i <= n - 4; i += 4)
    {
        __m128 v = _mm_loadu_ps(t + i);
        _mm_storeu_ps(out + i, _mm_mul_ps(v, v));
    }
#elif defined(EASE_NEON)
    for (; i <= n - 4; i += 4)
    {
        float32x4_t v = vld1q_f32(t + i);
        vst1q_f32(out + i, vmulq_f32(v, v));
    }
#endif
    for (; i < n; ++i) out[i] = t[i] * t[i];
}

void ease_batch_quad_out(const float* EASE_RESTRICT t, float* EASE_RESTRICT out, int n)
{
    int i = 0;
#if defined(EASE_AVX2)
    __m256 two = _mm256_set1_ps(2.0f);
    for (; i <= n - 8; i += 8)
    {
        __m256 v = _mm256_loadu_ps(t + i);
        // t*(2-t)
        _mm256_storeu_ps(out + i, _mm256_mul_ps(v, _mm256_sub_ps(two, v)));
    }
#elif defined(EASE_SSE2)
    __m128 two = _mm_set1_ps(2.0f);
    for (; i <= n - 4; i += 4)
    {
        __m128 v = _mm_loadu_ps(t + i);
        _mm_storeu_ps(out + i, _mm_mul_ps(v, _mm_sub_ps(two, v)));
    }
#elif defined(EASE_NEON)
    float32x4_t two = vdupq_n_f32(2.0f);
    for (; i <= n - 4; i += 4)
    {
        float32x4_t v = vld1q_f32(t + i);
        vst1q_f32(out + i, vmulq_f32(v, vsubq_f32(two, v)));
    }
#endif
    for (; i < n; ++i) out[i] = t[i] * (2.0f - t[i]);
}

void ease_batch_cubic_in(const float* EASE_RESTRICT t, float* EASE_RESTRICT out, int n)
{
    int i = 0;
#if defined(EASE_AVX2)
    for (; i <= n - 8; i += 8)
    {
        __m256 v = _mm256_loadu_ps(t + i);
        __m256 v2 = _mm256_mul_ps(v, v);
        _mm256_storeu_ps(out + i, _mm256_mul_ps(v2, v));
    }
#elif defined(EASE_SSE2)
    for (; i <= n - 4; i += 4)
    {
        __m128 v  = _mm_loadu_ps(t + i);
        __m128 v2 = _mm_mul_ps(v, v);
        _mm_storeu_ps(out + i, _mm_mul_ps(v2, v));
    }
#elif defined(EASE_NEON)
    for (; i <= n - 4; i += 4)
    {
        float32x4_t v  = vld1q_f32(t + i);
        float32x4_t v2 = vmulq_f32(v, v);
        vst1q_f32(out + i, vmulq_f32(v2, v));
    }
#endif
    for (; i < n; ++i) { float v = t[i]; out[i] = v*v*v; }
}

void ease_batch_cubic_out(const float* EASE_RESTRICT t, float* EASE_RESTRICT out, int n)
{
    int i = 0;
#if defined(EASE_AVX2)
    __m256 one = _mm256_set1_ps(1.0f);
    for (; i <= n - 8; i += 8)
    {
        __m256 s  = _mm256_sub_ps(_mm256_loadu_ps(t + i), one);
        __m256 s2 = _mm256_mul_ps(s, s);
        // s³ + 1
        _mm256_storeu_ps(out + i, _mm256_add_ps(_mm256_mul_ps(s2, s), one));
    }
#elif defined(EASE_SSE2)
    __m128 one = _mm_set1_ps(1.0f);
    for (; i <= n - 4; i += 4)
    {
        __m128 s  = _mm_sub_ps(_mm_loadu_ps(t + i), one);
        __m128 s2 = _mm_mul_ps(s, s);
        _mm_storeu_ps(out + i, _mm_add_ps(_mm_mul_ps(s2, s), one));
    }
#elif defined(EASE_NEON)
    float32x4_t one = vdupq_n_f32(1.0f);
    for (; i <= n - 4; i += 4)
    {
        float32x4_t s  = vsubq_f32(vld1q_f32(t + i), one);
        float32x4_t s2 = vmulq_f32(s, s);
        vst1q_f32(out + i, vaddq_f32(vmulq_f32(s2, s), one));
    }
#endif
    for (; i < n; ++i) { float s = t[i]-1.0f; out[i] = s*s*s+1.0f; }
}

void ease_batch_smooth_step(const float* EASE_RESTRICT t, float* EASE_RESTRICT out, int n)
{
    // 3t² - 2t³  =  t²(3 - 2t)
    int i = 0;
#if defined(EASE_AVX2)
    __m256 three = _mm256_set1_ps(3.0f);
    __m256 two   = _mm256_set1_ps(2.0f);
    for (; i <= n - 8; i += 8)
    {
        __m256 v  = _mm256_loadu_ps(t + i);
        __m256 v2 = _mm256_mul_ps(v, v);
        _mm256_storeu_ps(out + i,
            _mm256_mul_ps(v2, _mm256_sub_ps(three, _mm256_mul_ps(two, v))));
    }
#elif defined(EASE_SSE2)
    __m128 three = _mm_set1_ps(3.0f);
    __m128 two   = _mm_set1_ps(2.0f);
    for (; i <= n - 4; i += 4)
    {
        __m128 v  = _mm_loadu_ps(t + i);
        __m128 v2 = _mm_mul_ps(v, v);
        _mm_storeu_ps(out + i,
            _mm_mul_ps(v2, _mm_sub_ps(three, _mm_mul_ps(two, v))));
    }
#elif defined(EASE_NEON)
    float32x4_t three = vdupq_n_f32(3.0f);
    float32x4_t two   = vdupq_n_f32(2.0f);
    for (; i <= n - 4; i += 4)
    {
        float32x4_t v  = vld1q_f32(t + i);
        float32x4_t v2 = vmulq_f32(v, v);
        vst1q_f32(out + i,
            vmulq_f32(v2, vsubq_f32(three, vmulq_f32(two, v))));
    }
#endif
    for (; i < n; ++i) { float v = t[i]; out[i] = v*v*(3.0f - 2.0f*v); }
}

void ease_batch_dispatch(EaseType type,
                         const float* EASE_RESTRICT t,
                         float*       EASE_RESTRICT out,
                         int n)
{
    // Fast paths for the hot polynomial families
    switch (type)
    {
        case EASE_LINEAR:      ease_batch_linear    (t, out, n); return;
        case EASE_QUAD_IN:     ease_batch_quad_in   (t, out, n); return;
        case EASE_QUAD_OUT:    ease_batch_quad_out  (t, out, n); return;
        case EASE_CUBIC_IN:    ease_batch_cubic_in  (t, out, n); return;
        case EASE_CUBIC_OUT:   ease_batch_cubic_out (t, out, n); return;
        case EASE_SMOOTH_STEP: ease_batch_smooth_step(t,out, n); return;
        default: break;
    }

    // Scalar fallback via function pointer table
    EaseFn fn = ease_table[type];
    for (int i = 0; i < n; ++i) out[i] = fn(t[i]);
}
