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

#define sinf(x) fm_sin(x)
#define cosf(x) fm_cos(x)

#endif
