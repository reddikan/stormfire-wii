/*
 * STORMFIRE - quick sine / cosine for the drawing code (the Wii's library versions are slow, and hundreds of
 * them run every frame). Accurate to about 1e-5, far finer than a pixel. Include after <math.h>.
 */
#ifndef FASTMATH_H
#define FASTMATH_H

#include <math.h>

static inline float fm_sin(float x)
{
    /* reduce to [-pi, pi] */
    const float inv = 0.15915494f, tau = 6.2831853f;
    float k = x * inv;
    k = (float)(int)(k + (k >= 0 ? 0.5f : -0.5f));
    x -= k * tau;
    /* fold to [-pi/2, pi/2] */
    if (x > 1.5707963f) x = 3.1415927f - x;
    else if (x < -1.5707963f) x = -3.1415927f - x;
    float x2 = x * x;
    return x * (1.0f + x2 * (-0.16666667f + x2 * (0.0083333310f + x2 * (-0.00019840874f + x2 * 2.7525562e-6f))));
}
static inline float fm_cos(float x) { return fm_sin(x + 1.5707963f); }

/* e^x for envelopes (x <= ~10): split into a power of two and a small polynomial */
static inline float fm_exp(float x)
{
    if (x < -80.0f) return 0.0f;
    float t = x * 1.4426950f;
    float fi = (float)(int)t; if (fi > t) fi -= 1.0f;
    float f = t - fi;
    float p = 1.0f + f * (0.6931472f + f * (0.2402265f + f * (0.0555041f + f * (0.0096181f + f * 0.0013334f))));
    union { float f; unsigned int u; } u;
    int e = (int)fi + 127;
    if (e <= 0) return 0.0f;
    if (e >= 255) e = 254;
    u.u = (unsigned int)e << 23;
    return p * u.f;
}

#define sinf(x) fm_sin(x)
#define expf(x) fm_exp(x)
#define cosf(x) fm_cos(x)

#endif
