#ifndef AL_MATH_DEFS_H
#define AL_MATH_DEFS_H

#include <math.h>
#ifdef HAVE_FLOAT_H
#include <float.h>
#endif

#ifndef M_PI
#define M_PI (3.14159265358979323846)
#endif

#define F_PI    (3.14159265358979323846f)
#define F_PI_2  (1.57079632679489661923f)
#define F_TAU   (6.28318530717958647692f)

#ifndef FLT_EPSILON
#define FLT_EPSILON (1.19209290e-07f)
#endif

#ifndef HUGE_VALF
static const union msvc_inf_hack {
    unsigned char b[4];
    float f;
} msvc_inf_union = {{ 0x00, 0x00, 0x80, 0x7F }};
#define HUGE_VALF (msvc_inf_union.f)
#endif

/* MSVC ships log2f/cbrtf/copysignf in the CRT, and since the VS2026 toolchain
   (windows-2025-vs2026 runner images) treats them as real intrinsics: a fallback
   definition is not merely redundant there, it is a hard error -
     lib/openal/common/math_defs.h(44): error C2169: 'copysignf': intrinsic
     function, cannot be defined
   openal is compiled here without HX_WINDOWS (config.h only pulls a
   config-<platform>.h under HX_*), so HAVE_LOG2F/HAVE_CBRTF/HAVE_COPYSIGNF are
   never defined for MSVC and all three fallbacks get compiled. Skip them. */
#if !defined(HAVE_LOG2F) && !defined(_MSC_VER)
static inline float log2f(float f)
{
    return logf(f) / logf(2.0f);
}
#endif

#if !defined(HAVE_CBRTF) && !defined(_MSC_VER)
static inline float cbrtf(float f)
{
    return powf(f, 1.0f/3.0f);
}
#endif

#if !defined(HAVE_COPYSIGNF) && !defined(_MSC_VER)
static inline float copysignf(float x, float y)
{
    union {
        float f;
        unsigned int u;
    } ux = { x }, uy = { y };
    ux.u &= 0x7fffffffu;
    ux.u |= (uy.u&0x80000000u);
    return ux.f;
}
#endif

#define DEG2RAD(x)  ((float)(x) * (float)(M_PI/180.0))
#define RAD2DEG(x)  ((float)(x) * (float)(180.0/M_PI))

#endif /* AL_MATH_DEFS_H */
