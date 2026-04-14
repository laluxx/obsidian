#pragma once

// Single-precision and double-precision implementations of all standard
// Robert Penner easing families plus higher-order, parametric, and
// engine-specific variants. Tuned for throughput on x86-64 and ARM64.

#include <math.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// SIMD Portability

#if defined(_MSC_VER)
  #define EASE_INLINE   __forceinline
  #define EASE_PURE     __declspec(noalias)
  #define EASE_RESTRICT __restrict
#elif defined(__GNUC__) || defined(__clang__)
  #define EASE_INLINE   __attribute__((always_inline)) static inline
  #define EASE_PURE     __attribute__((pure))
  #define EASE_RESTRICT __restrict__
#else
  #define EASE_INLINE   static inline
  #define EASE_PURE
  #define EASE_RESTRICT
#endif

// Branch prediction hints
#if defined(__GNUC__) || defined(__clang__)
  #define EASE_LIKELY(x)   __builtin_expect(!!(x), 1)
  #define EASE_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
  #define EASE_LIKELY(x)   (x)
  #define EASE_UNLIKELY(x) (x)
#endif

/// Constants

#define EASE_PI        3.14159265358979323846f
#define EASE_TAU       6.28318530717958647693f
#define EASE_HALF_PI   1.57079632679489661923f
#define EASE_SQRT2     1.41421356237309504880f
#define EASE_INV_SQRT2 0.70710678118654752440f

/// Types

// Function pointer matching the canonical easing signature: f(t) → [0,1]
typedef float  (*EaseFn)(float t);
typedef double (*EaseFn64)(double t);

// All easing variants enumerated for table-driven dispatch
typedef enum {
    EASE_LINEAR = 0,

    EASE_SINE_IN,    EASE_SINE_OUT,    EASE_SINE_IN_OUT,
    EASE_QUAD_IN,    EASE_QUAD_OUT,    EASE_QUAD_IN_OUT,
    EASE_CUBIC_IN,   EASE_CUBIC_OUT,   EASE_CUBIC_IN_OUT,
    EASE_QUART_IN,   EASE_QUART_OUT,   EASE_QUART_IN_OUT,
    EASE_QUINT_IN,   EASE_QUINT_OUT,   EASE_QUINT_IN_OUT,
    EASE_EXPO_IN,    EASE_EXPO_OUT,    EASE_EXPO_IN_OUT,
    EASE_CIRC_IN,    EASE_CIRC_OUT,    EASE_CIRC_IN_OUT,
    EASE_BACK_IN,    EASE_BACK_OUT,    EASE_BACK_IN_OUT,
    EASE_ELASTIC_IN, EASE_ELASTIC_OUT, EASE_ELASTIC_IN_OUT,
    EASE_BOUNCE_IN,  EASE_BOUNCE_OUT,  EASE_BOUNCE_IN_OUT,

    EASE_SMOOTH_STEP,        // Ken Perlin's smoothstep
    EASE_SMOOTHER_STEP,      // Perlin's C2-continuous variant
    EASE_SMOOTHEST_STEP,     // C3-continuous sextic
    EASE_SPRING,             // Critically-damped spring approximation
    EASE_BEZIER_EASE,        // CSS cubic-bezier(0.25, 0.1, 0.25, 1.0)
    EASE_BEZIER_EASE_IN,     // CSS cubic-bezier(0.42, 0.0, 1.0, 1.0)
    EASE_BEZIER_EASE_OUT,    // CSS cubic-bezier(0.0, 0.0, 0.58, 1.0)
    EASE_BEZIER_EASE_IN_OUT, // CSS cubic-bezier(0.42, 0.0, 0.58, 1.0)

    EASE_COUNT
} EaseType;

/// Lookup Table Dispatch

// Global table of all easing functions, indexed by EaseType.
// Populated at startup; thread-safe after ease_init().
extern EaseFn  ease_table[EASE_COUNT];
extern EaseFn64 ease_table64[EASE_COUNT];

// Initialise the lookup tables. Call once before using ease_dispatch().
void ease_init(void);

// Dispatch through the lookup table. Hot-path: single indirect branch.
EASE_INLINE float  ease_dispatch(EaseType type, float t)  { return ease_table[type](t);  }
EASE_INLINE double ease_dispatch64(EaseType type, double t) { return ease_table64[type](t); }

/// Utility: Remap

// Normalise t from [from_min, from_max] to [0,1], apply easing, remap to
// [to_min, to_max]. Single function, zero overhead.
EASE_INLINE float ease_remap(EaseFn fn, float t,
                              float from_min, float from_max,
                              float to_min,   float to_max)
{
    float norm = (t - from_min) / (from_max - from_min);
    return to_min + fn(norm) * (to_max - to_min);
}

/// Linear

EASE_INLINE EASE_PURE float ease_linear(float t) { return t; }

/// Sine

EASE_INLINE EASE_PURE float ease_sine_in    (float t);
EASE_INLINE EASE_PURE float ease_sine_out   (float t);
EASE_INLINE EASE_PURE float ease_sine_in_out(float t);

/// Quad

EASE_INLINE EASE_PURE float ease_quad_in    (float t);
EASE_INLINE EASE_PURE float ease_quad_out   (float t);
EASE_INLINE EASE_PURE float ease_quad_in_out(float t);

/// Cubic

EASE_INLINE EASE_PURE float ease_cubic_in    (float t);
EASE_INLINE EASE_PURE float ease_cubic_out   (float t);
EASE_INLINE EASE_PURE float ease_cubic_in_out(float t);

/// Quart

EASE_INLINE EASE_PURE float ease_quart_in    (float t);
EASE_INLINE EASE_PURE float ease_quart_out   (float t);
EASE_INLINE EASE_PURE float ease_quart_in_out(float t);

/// Quint

EASE_INLINE EASE_PURE float ease_quint_in    (float t);
EASE_INLINE EASE_PURE float ease_quint_out   (float t);
EASE_INLINE EASE_PURE float ease_quint_in_out(float t);

/// Exponential

EASE_INLINE EASE_PURE float ease_expo_in    (float t);
EASE_INLINE EASE_PURE float ease_expo_out   (float t);
EASE_INLINE EASE_PURE float ease_expo_in_out(float t);

/// Circular

EASE_INLINE EASE_PURE float ease_circ_in    (float t);
EASE_INLINE EASE_PURE float ease_circ_out   (float t);
EASE_INLINE EASE_PURE float ease_circ_in_out(float t);

/// Back
// Parametric: s controls overshoot. Default Penner value = 1

EASE_INLINE EASE_PURE float ease_back_in    (float t);
EASE_INLINE EASE_PURE float ease_back_out   (float t);
EASE_INLINE EASE_PURE float ease_back_in_out(float t);

float ease_back_in_s    (float t, float s);
float ease_back_out_s   (float t, float s);
float ease_back_in_out_s(float t, float s);

/// Elastic
// Parametric: amplitude a (default 1), period p (default 0.3).

EASE_INLINE EASE_PURE float ease_elastic_in    (float t);
EASE_INLINE EASE_PURE float ease_elastic_out   (float t);
EASE_INLINE EASE_PURE float ease_elastic_in_out(float t);

float ease_elastic_in_ap    (float t, float a, float p);
float ease_elastic_out_ap   (float t, float a, float p);
float ease_elastic_in_out_ap(float t, float a, float p);

/// Bounce

EASE_INLINE EASE_PURE float ease_bounce_in    (float t);
EASE_INLINE EASE_PURE float ease_bounce_out   (float t);
EASE_INLINE EASE_PURE float ease_bounce_in_out(float t);

/// Perlin Steps

EASE_INLINE EASE_PURE float ease_smooth_step   (float t);  // 3t² − 2t³
EASE_INLINE EASE_PURE float ease_smoother_step (float t);  // 6t⁵ − 15t⁴ + 10t³
EASE_INLINE EASE_PURE float ease_smoothest_step(float t);  // C3-continuous degree-7

/// Spring

EASE_INLINE EASE_PURE float ease_spring(float t);

// Critically-damped spring with explicit stiffness and damping ratio.
float ease_spring_damp(float t, float stiffness, float damping);

/// Parametric Bezier
// Cubic Bézier compatible with CSS timing functions.
// Solve via 4-iteration Newton–Raphson on the x polynomial.

float ease_cubic_bezier(float t, float x1, float y1, float x2, float y2);

EASE_INLINE EASE_PURE float ease_bezier_ease        (float t);
EASE_INLINE EASE_PURE float ease_bezier_ease_in     (float t);
EASE_INLINE EASE_PURE float ease_bezier_ease_out    (float t);
EASE_INLINE EASE_PURE float ease_bezier_ease_in_out (float t);

/// Higher-Order

// Raised-power generalisation: ease_in(t, n) = tⁿ, ease_out = 1-(1-t)ⁿ
float ease_power_in    (float t, float n);
float ease_power_out   (float t, float n);
float ease_power_in_out(float t, float n);

// Arch: rises to 1 at t=0.5, returns to 0.  Cheap, LUT-free.
EASE_INLINE EASE_PURE float ease_arch      (float t);   // 4t(1-t)
EASE_INLINE EASE_PURE float ease_arch_cubic(float t);   // −4t³ + 6t² − 2t + …, symmetric arch

// Punch: fast in, slight overshoot, settle.  Great for UI pops.
EASE_INLINE EASE_PURE float ease_punch(float t);

// Hermite spline blend — matches smoothstep but accepts control velocities.
float ease_hermite(float t, float p0, float p1, float m0, float m1);

// Catmull-Rom blend for animation curve segments.
float ease_catmull_rom(float t, float p0, float p1, float p2, float p3);

// Stepped: quantise t to n discrete steps.
EASE_INLINE EASE_PURE float ease_stepped(float t, int n);

// Zigzag: 0→1→0→1 oscillation with frequency f.
EASE_INLINE EASE_PURE float ease_zigzag(float t, float f);

// Mirror easing: applies fn in [0,0.5] then mirrors in [0.5,1].
EASE_INLINE float ease_mirror(EaseFn fn, float t);

// Chain two easings: fn_a on [0, split], fn_b on [split, 1].
EASE_INLINE float ease_chain(EaseFn fn_a, EaseFn fn_b, float split, float t);

/// SIMD Batch API
// Process N samples of t[] through the same easing function.
// Internally uses SSE2/NEON when available.  out[] and t[] may alias
// only if out == t (in-place).  Both must be 16-byte aligned.

void ease_batch_linear    (const float* EASE_RESTRICT t, float* EASE_RESTRICT out, int n);
void ease_batch_quad_in   (const float* EASE_RESTRICT t, float* EASE_RESTRICT out, int n);
void ease_batch_quad_out  (const float* EASE_RESTRICT t, float* EASE_RESTRICT out, int n);
void ease_batch_cubic_in  (const float* EASE_RESTRICT t, float* EASE_RESTRICT out, int n);
void ease_batch_cubic_out (const float* EASE_RESTRICT t, float* EASE_RESTRICT out, int n);
void ease_batch_smooth_step(const float* EASE_RESTRICT t, float* EASE_RESTRICT out, int n);
void ease_batch_dispatch  (EaseType type,
                           const float* EASE_RESTRICT t,
                           float*       EASE_RESTRICT out, int n);

/// Inline Implementations
//// Sine

EASE_INLINE EASE_PURE float ease_sine_in(float t)
{
    return 1.0f - cosf(t * EASE_HALF_PI);
}
EASE_INLINE EASE_PURE float ease_sine_out(float t)
{
    return sinf(t * EASE_HALF_PI);
}
EASE_INLINE EASE_PURE float ease_sine_in_out(float t)
{
    return 0.5f * (1.0f - cosf(t * EASE_PI));
}

//// Quad

EASE_INLINE EASE_PURE float ease_quad_in(float t)     { return t * t; }
EASE_INLINE EASE_PURE float ease_quad_out(float t)    { return t * (2.0f - t); }
EASE_INLINE EASE_PURE float ease_quad_in_out(float t)
{
    return (t < 0.5f) ? (2.0f * t * t)
                      : (-1.0f + (4.0f - 2.0f * t) * t);
}

//// Cubic

EASE_INLINE EASE_PURE float ease_cubic_in(float t)    { return t * t * t; }
EASE_INLINE EASE_PURE float ease_cubic_out(float t)
{
    float s = t - 1.0f;
    return s * s * s + 1.0f;
}
EASE_INLINE EASE_PURE float ease_cubic_in_out(float t)
{
    return (t < 0.5f) ? (4.0f * t * t * t)
                      : ((t - 1.0f) * (2.0f * t - 2.0f) * (2.0f * t - 2.0f) + 1.0f);
}

//// Quart

EASE_INLINE EASE_PURE float ease_quart_in(float t)
{
    float t2 = t * t;
    return t2 * t2;
}
EASE_INLINE EASE_PURE float ease_quart_out(float t)
{
    float s = t - 1.0f;
    float s2 = s * s;
    return 1.0f - s2 * s2;
}
EASE_INLINE EASE_PURE float ease_quart_in_out(float t)
{
    if (t < 0.5f) { float t2 = t * t; return 8.0f * t2 * t2; }
    float s = t - 1.0f; float s2 = s * s;
    return 1.0f - 8.0f * s2 * s2;
}

//// Quint

EASE_INLINE EASE_PURE float ease_quint_in(float t)
{
    float t2 = t * t;
    return t2 * t2 * t;
}
EASE_INLINE EASE_PURE float ease_quint_out(float t)
{
    float s = t - 1.0f; float s2 = s * s;
    return s2 * s2 * s + 1.0f;
}
EASE_INLINE EASE_PURE float ease_quint_in_out(float t)
{
    if (t < 0.5f) { float t2 = t * t; return 16.0f * t2 * t2 * t; }
    float s = t - 1.0f; float s2 = s * s;
    return 1.0f + 16.0f * s2 * s2 * s;
}

//// Exponential

EASE_INLINE EASE_PURE float ease_expo_in(float t)
{
    return EASE_UNLIKELY(t == 0.0f) ? 0.0f : powf(2.0f, 10.0f * t - 10.0f);
}
EASE_INLINE EASE_PURE float ease_expo_out(float t)
{
    return EASE_UNLIKELY(t == 1.0f) ? 1.0f : 1.0f - powf(2.0f, -10.0f * t);
}
EASE_INLINE EASE_PURE float ease_expo_in_out(float t)
{
    if (EASE_UNLIKELY(t == 0.0f)) return 0.0f;
    if (EASE_UNLIKELY(t == 1.0f)) return 1.0f;
    return (t < 0.5f) ? (powf(2.0f,  20.0f * t - 10.0f) * 0.5f)
                      : ((2.0f - powf(2.0f, -20.0f * t + 10.0f)) * 0.5f);
}

//// Circular

EASE_INLINE EASE_PURE float ease_circ_in(float t)
{
    return 1.0f - sqrtf(1.0f - t * t);
}
EASE_INLINE EASE_PURE float ease_circ_out(float t)
{
    return sqrtf((2.0f - t) * t);
}
EASE_INLINE EASE_PURE float ease_circ_in_out(float t)
{
    return (t < 0.5f)
        ? (0.5f * (1.0f - sqrtf(1.0f - 4.0f * t * t)))
        : (0.5f * (sqrtf(-(2.0f * t - 3.0f) * (2.0f * t - 1.0f)) + 1.0f));
}

//// Back  (default s = 1.70158)

EASE_INLINE EASE_PURE float ease_back_in(float t)
{
    const float s = 1.70158f;
    return t * t * ((s + 1.0f) * t - s);
}
EASE_INLINE EASE_PURE float ease_back_out(float t)
{
    const float s = 1.70158f;
    float u = t - 1.0f;
    return u * u * ((s + 1.0f) * u + s) + 1.0f;
}
EASE_INLINE EASE_PURE float ease_back_in_out(float t)
{
    const float s = 1.70158f * 1.525f;
    if (t < 0.5f) { float u = 2.0f * t; return 0.5f * u * u * ((s + 1.0f) * u - s); }
    float u = 2.0f * t - 2.0f;
    return 0.5f * (u * u * ((s + 1.0f) * u + s) + 2.0f);
}

//// Bounce  (closed-form, branch-free inner loop)

EASE_INLINE EASE_PURE float ease_bounce_out(float t)
{
    if (t < (1.0f / 2.75f))        return 7.5625f * t * t;
    if (t < (2.0f / 2.75f)) { t -= 1.5f   / 2.75f; return 7.5625f * t * t + 0.75f;   }
    if (t < (2.5f / 2.75f)) { t -= 2.25f  / 2.75f; return 7.5625f * t * t + 0.9375f; }
    t -= 2.625f / 2.75f;           return 7.5625f * t * t + 0.984375f;
}
EASE_INLINE EASE_PURE float ease_bounce_in(float t)
{
    return 1.0f - ease_bounce_out(1.0f - t);
}
EASE_INLINE EASE_PURE float ease_bounce_in_out(float t)
{
    return (t < 0.5f) ? (0.5f * ease_bounce_in(t * 2.0f))
                      : (0.5f * ease_bounce_out(t * 2.0f - 1.0f) + 0.5f);
}

//// Elastic  (default a=1, p=0.3)

EASE_INLINE EASE_PURE float ease_elastic_out(float t)
{
    if (EASE_UNLIKELY(t == 0.0f || t == 1.0f)) return t;
    return powf(2.0f, -10.0f * t) * sinf((t - 0.075f) * EASE_TAU / 0.3f) + 1.0f;
}
EASE_INLINE EASE_PURE float ease_elastic_in(float t)
{
    if (EASE_UNLIKELY(t == 0.0f || t == 1.0f)) return t;
    return -(powf(2.0f, 10.0f * t - 10.0f) * sinf((t - 1.075f) * EASE_TAU / 0.3f));
}
EASE_INLINE EASE_PURE float ease_elastic_in_out(float t)
{
    if (EASE_UNLIKELY(t == 0.0f || t == 1.0f)) return t;
    const float p = 0.45f;
    if (t < 0.5f)
        return -(0.5f * powf(2.0f,  20.0f * t - 10.0f) * sinf((20.0f * t - 11.125f) * EASE_TAU / p));
    return 0.5f *  powf(2.0f, -20.0f * t + 10.0f) * sinf((20.0f * t - 11.125f) * EASE_TAU / p) + 1.0f;
}

//// Perlin Steps

EASE_INLINE EASE_PURE float ease_smooth_step(float t)
{
    return t * t * (3.0f - 2.0f * t);
}
EASE_INLINE EASE_PURE float ease_smoother_step(float t)
{
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}
EASE_INLINE EASE_PURE float ease_smoothest_step(float t)
{
    // Degree-7: C3-continuous — no visible derivative kinks at t=0 or t=1
    return t * t * t * t * (t * (t * (t * -20.0f + 70.0f) - 84.0f) + 35.0f);
}

//// Spring (fast analytical approximation)

EASE_INLINE EASE_PURE float ease_spring(float t)
{
    // Approximates a critically-damped spring settling: overshoot ~0.15
    return (1.0f - (1.0f - t) * (1.0f - t))
         * (1.0f + 1.5f * (1.0f - t) * (1.0f - t) * t);
}

//// Arch

EASE_INLINE EASE_PURE float ease_arch(float t)
{
    return 4.0f * t * (1.0f - t);
}
EASE_INLINE EASE_PURE float ease_arch_cubic(float t)
{
    return t * (1.0f - t) * (4.0f - 4.0f * t * (1.0f - t));
}

//// Punch  (quintic with overshoot)

EASE_INLINE EASE_PURE float ease_punch(float t)
{
    if (t < 0.5f)
    {
        float u = 2.0f * t;
        return 0.5f * ease_quint_out(u) * 1.1f; // slight overshoot
    }
    float u = 2.0f * t - 1.0f;
    return 0.5f + 0.5f * ease_quint_in(u);
}

//// Stepped

EASE_INLINE EASE_PURE float ease_stepped(float t, int n)
{
    return (float)((int)(t * (float)n)) / (float)n;
}

//// Zigzag

EASE_INLINE EASE_PURE float ease_zigzag(float t, float f)
{
    float v = t * f;
    float frac = v - (float)(int)v;
    return ((int)v & 1) ? (1.0f - frac) : frac;
}

//// Mirror / Chain

EASE_INLINE float ease_mirror(EaseFn fn, float t)
{
    return (t < 0.5f) ? fn(t * 2.0f) : fn(2.0f - t * 2.0f);
}
EASE_INLINE float ease_chain(EaseFn fn_a, EaseFn fn_b, float split, float t)
{
    return (t < split) ? fn_a(t / split)
                       : fn_b((t - split) / (1.0f - split));
}

//// CSS Bezier presets

EASE_INLINE EASE_PURE float ease_bezier_ease        (float t) { return ease_cubic_bezier(t, 0.25f, 0.10f, 0.25f, 1.00f); }
EASE_INLINE EASE_PURE float ease_bezier_ease_in     (float t) { return ease_cubic_bezier(t, 0.42f, 0.00f, 1.00f, 1.00f); }
EASE_INLINE EASE_PURE float ease_bezier_ease_out    (float t) { return ease_cubic_bezier(t, 0.00f, 0.00f, 0.58f, 1.00f); }
EASE_INLINE EASE_PURE float ease_bezier_ease_in_out (float t) { return ease_cubic_bezier(t, 0.42f, 0.00f, 0.58f, 1.00f); }

#ifdef __cplusplus
}
#endif
