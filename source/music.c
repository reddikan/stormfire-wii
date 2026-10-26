/*
 * STORMFIRE - background music. See music.h.
 *
 * A tiny offline synth: each instrument renders whole notes into a float loop buffer (notes that run
 * past the end wrap round to the start, so every loop is seamless), then a small Schroeder reverb is
 * run round the loop twice and the result is normalised to 16-bit.
 */
#include "music.h"

#include <math.h>
#include <string.h>
#include <stdlib.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#define SR    ((float)MUS_RATE)
#define TAU_F 6.28318530718f

#define MUS_MAX (MUS_RATE * 30 + 16)
static int16_t MUS[MUS_TRACKS][MUS_MAX] __attribute__((aligned(32)));
static int     mus_len[MUS_TRACKS];
static float  *mix;                       /* the loop being built (only needed while composing) */
static int     N;                         /* length of the loop being built */
static float   BEAT;                      /* seconds per beat */
static uint32_t rng = 0x1234567u;
static int     ready;

static const uint8_t VOL[MUS_TRACKS] = { 100, 92, 96, 100, 104, 100 };

static float frnd(void)                  /* -1..1 */
{
    rng = rng * 1664525u + 1013904223u;
    return (float)(rng >> 8) / 8388608.0f - 1.0f;
}

static float mtof(float m) { return 440.0f * powf(2.0f, (m - 69.0f) / 12.0f); }

static inline void put(int i, float v)
{
    i %= N;
    if (i < 0) i += N;
    mix[i] += v;
}

/* attack / exponential decay / release envelope */
static inline float env_adr(int i, int n, float atk, float tau, float rel)
{
    float t = (float)i / SR, e = 1.0f;
    if (t < atk) e = t / atk;
    if (tau > 0) e *= expf(-t / tau);
    float left = (float)(n - i) / SR;
    if (left < rel) e *= left / rel;
    return e;
}

/* ------------------------------------------------------------------ */
/* instruments (times in beats)                                         */
/* ------------------------------------------------------------------ */

/* warm pad: two detuned band-limited saws through a 2-pole low-pass, slow attack */
static void pad(float b, float dur, float m, float vel, float cutoff)
{
    int s0 = (int)(b * BEAT * SR), n = (int)((dur * BEAT + 1.2f) * SR);
    float f = mtof(m), ph1 = 0, ph2 = 0.37f, lp1 = 0, lp2 = 0;
    float k = 1.0f - expf(-TAU_F * cutoff / SR);
    float d1 = f * 1.004f / SR, d2 = f * 0.996f / SR;
    for (int i = 0; i < n; i++) {
        float s = 0;
        /* polyBLEP saws */
        for (int o = 0; o < 2; o++) {
            float *ph = o ? &ph2 : &ph1, dt = o ? d2 : d1, x = 2.0f * *ph - 1.0f, t = *ph;
            if (t < dt) { t /= dt; x -= t + t - t * t - 1.0f; }
            else if (t > 1.0f - dt) { t = (t - 1.0f) / dt; x -= t * t + t + t + 1.0f; }
            s += x;
            *ph += dt; if (*ph >= 1.0f) *ph -= 1.0f;
        }
        lp1 += (s - lp1) * k; lp2 += (lp1 - lp2) * k;
        float e = env_adr(i, n, 0.9f, 0, 1.2f);
        put(s0 + i, lp2 * e * vel * 0.18f);
    }
}

/* electric piano: FM with a decaying index plus a soft sine body */
static void epiano(float b, float dur, float m, float vel)
{
    int s0 = (int)(b * BEAT * SR), n = (int)((dur * BEAT + 1.5f) * SR);
    float f = mtof(m), pc = 0, pm = 0;
    for (int i = 0; i < n; i++) {
        float t = (float)i / SR, idx = 1.6f * expf(-t * 3.0f);
        float e = env_adr(i, n, 0.004f, 1.4f, 0.3f);
        float v = sinf(TAU_F * pc + idx * sinf(TAU_F * pm)) + 0.35f * sinf(TAU_F * pc * 2.0f) * expf(-t * 2.0f);
        put(s0 + i, v * e * vel * 0.32f);
        pc += f / SR; pc -= floorf(pc);
        pm += f * 1.0f / SR; pm -= floorf(pm);
    }
}

/* celesta / glassy bell */
static void bell(float b, float m, float vel, float decay)
{
    int s0 = (int)(b * BEAT * SR), n = (int)(decay * 5.0f * SR);
    float f = mtof(m), pc = 0, pm = 0;
    for (int i = 0; i < n; i++) {
        float t = (float)i / SR;
        float e = env_adr(i, n, 0.002f, decay, 0.05f);
        float v = sinf(TAU_F * pc + 1.2f * expf(-t * 6.0f) * sinf(TAU_F * pm)) + 0.25f * sinf(TAU_F * pc * 4.0f) * expf(-t * 8.0f);
        put(s0 + i, v * e * vel * 0.28f);
        pc += f / SR; pc -= floorf(pc);
        pm += f * 3.5f / SR; pm -= floorf(pm);
    }
}

/* plucked string (Karplus-Strong): guitar-ish when bright, pizzicato bass when dark */
static void pluck(float b, float dur, float m, float vel, float bright)
{
    static float ks[2048];
    int s0 = (int)(b * BEAT * SR), n = (int)((dur * BEAT + 0.6f) * SR);
    float f = mtof(m);
    int L = (int)(SR / f);
    if (L < 2) L = 2;
    if (L > 2047) L = 2047;
    float lp = 0;
    for (int i = 0; i < L; i++) { lp += (frnd() - lp) * bright; ks[i] = lp; }
    int p = 0;
    float damp = 0.4975f + 0.0024f * bright;
    for (int i = 0; i < n; i++) {
        int q = p + 1 == L ? 0 : p + 1;
        float y = ks[p];
        ks[p] = (ks[p] + ks[q]) * damp;
        p = q;
        float e = env_adr(i, n, 0.001f, 0, 0.08f);
        put(s0 + i, y * e * vel * 0.9f);
    }
}

/* breathy flute: sine + a little 2nd harmonic, delayed vibrato, filtered breath noise */
static void flute(float b, float dur, float m, float vel)
{
    int s0 = (int)(b * BEAT * SR), n = (int)((dur * BEAT + 0.15f) * SR);
    float f = mtof(m), ph = 0, bn = 0;
    for (int i = 0; i < n; i++) {
        float t = (float)i / SR;
        float vib = t > 0.18f ? 1.0f + 0.006f * sinf(TAU_F * 5.2f * t) * fminf((t - 0.18f) * 3.0f, 1.0f) : 1.0f;
        float e = env_adr(i, n, 0.07f, 0, 0.12f);
        bn += (frnd() - bn) * 0.15f;
        float v = sinf(TAU_F * ph) + 0.18f * sinf(TAU_F * ph * 2.0f) + bn * 0.25f;
        put(s0 + i, v * e * vel * 0.22f);
        ph += f * vib / SR; ph -= floorf(ph);
    }
}

/* theremin: pure sine that glides from the previous note, with a wide wobbly vibrato */
static float th_prev = 64.0f;
static void theremin(float b, float dur, float m, float vel)
{
    int s0 = (int)(b * BEAT * SR), n = (int)((dur * BEAT + 0.08f) * SR);
    float ph = 0, from = th_prev;
    th_prev = m;
    for (int i = 0; i < n; i++) {
        float t = (float)i / SR;
        float glide = fminf(t / 0.09f, 1.0f);
        float mm = from + (m - from) * glide + 0.32f * sinf(TAU_F * 5.6f * t) * fminf(t * 4.0f, 1.0f);
        float e = env_adr(i, n, 0.05f, 0, 0.06f);
        put(s0 + i, (sinf(TAU_F * ph) + 0.08f * sinf(TAU_F * ph * 3.0f)) * e * vel * 0.24f);
        ph += mtof(mm) / SR; ph -= floorf(ph);
    }
}

/* organ stab: a few drawbar harmonics, short */
static void organ(float b, float dur, float m, float vel)
{
    int s0 = (int)(b * BEAT * SR), n = (int)((dur * BEAT + 0.05f) * SR);
    float f = mtof(m), ph = 0;
    for (int i = 0; i < n; i++) {
        float e = env_adr(i, n, 0.006f, 0, 0.05f);
        float v = sinf(TAU_F * ph) + 0.5f * sinf(TAU_F * ph * 2.0f) + 0.3f * sinf(TAU_F * ph * 3.0f) + 0.15f * sinf(TAU_F * ph * 4.0f);
        put(s0 + i, v * e * vel * 0.12f);
        ph += f / SR; ph -= floorf(ph);
    }
}

/* soft round bass */
static void bass(float b, float dur, float m, float vel)
{
    int s0 = (int)(b * BEAT * SR), n = (int)((dur * BEAT + 0.1f) * SR);
    float f = mtof(m), ph = 0;
    for (int i = 0; i < n; i++) {
        float e = env_adr(i, n, 0.01f, dur * BEAT * 1.5f, 0.08f);
        float v = sinf(TAU_F * ph) + 0.2f * sinf(TAU_F * ph * 2.0f);
        put(s0 + i, v * e * vel * 0.4f);
        ph += f / SR; ph -= floorf(ph);
    }
}

/* low drone with a slow swell */
static void drone(float m, float vel)
{
    float f = mtof(m), ph = 0;
    for (int i = 0; i < N; i++) {
        float t = (float)i / (float)N;
        float sw = 0.6f + 0.4f * sinf(TAU_F * t * 2.0f);   /* whole cycles, so it loops cleanly */
        put(i, (sinf(TAU_F * ph) + 0.3f * sinf(TAU_F * ph * 1.5f)) * sw * vel * 0.12f);
        ph += f / SR; ph -= floorf(ph);
    }
}

static void kick(float b, float vel)
{
    int s0 = (int)(b * BEAT * SR), n = (int)(0.28f * SR);
    float ph = 0;
    for (int i = 0; i < n; i++) {
        float t = (float)i / SR, f = 45.0f + 75.0f * expf(-t * 30.0f);
        put(s0 + i, sinf(TAU_F * ph) * expf(-t * 12.0f) * vel * 0.55f);
        ph += f / SR;
    }
}

/* noise percussion: shaker (bright, short) or finger snap / clap (band-passed) */
static void noise_hit(float b, float vel, float decay, float centre, float q)
{
    int s0 = (int)(b * BEAT * SR), n = (int)(decay * 6.0f * SR);
    float w = TAU_F * centre / SR, a = sinf(w) / (2.0f * q), cw = cosf(w), a0 = 1.0f + a;
    float b0 = a / a0, b2 = -a / a0, a1 = -2.0f * cw / a0, a2 = (1.0f - a) / a0, z1 = 0, z2 = 0;
    for (int i = 0; i < n; i++) {
        float x = frnd(), y = b0 * x + z1;
        z1 = -a1 * y + z2;
        z2 = b2 * x - a2 * y;
        float t = (float)i / SR;
        put(s0 + i, y * expf(-t / decay) * vel * 0.5f);
    }
}

/* wooden xylophone tick for the "rattling bones" */
static void bones(float b, float m, float vel)
{
    int s0 = (int)(b * BEAT * SR), n = (int)(0.25f * SR);
    float f = mtof(m), pc = 0, pm = 0;
    for (int i = 0; i < n; i++) {
        float t = (float)i / SR;
        put(s0 + i, sinf(TAU_F * pc + 2.0f * expf(-t * 40.0f) * sinf(TAU_F * pm)) * expf(-t * 22.0f) * vel * 0.3f);
        pc += f / SR; pc -= floorf(pc);
        pm += f * 3.0f / SR; pm -= floorf(pm);
    }
}

/* ------------------------------------------------------------------ */
/* reverb (Schroeder: 4 combs + 2 allpasses), run round the loop twice  */
/* ------------------------------------------------------------------ */
static void reverb(float amount, float size)
{
    static float cb[4][3000], ap[2][800];
    static const int CL[4] = { 1116, 1188, 1277, 1356 }, AL[2] = { 556, 341 };
    int cl[4], al[2];
    for (int k = 0; k < 4; k++) cl[k] = (int)((float)CL[k] * 0.69f * size);   /* tuned for 44.1k, scaled to 22k */
    for (int k = 0; k < 2; k++) al[k] = (int)((float)AL[k] * 0.69f);
    memset(cb, 0, sizeof cb); memset(ap, 0, sizeof ap);
    int cp[4] = {0}, apos[2] = {0};
    float fs[4] = {0};
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < N; i++) {
            float x = mix[i] * 0.25f, out = 0;
            for (int k = 0; k < 4; k++) {
                float y = cb[k][cp[k]];
                fs[k] = y * 0.6f + fs[k] * 0.4f;              /* damping */
                cb[k][cp[k]] = x + fs[k] * 0.82f;
                if (++cp[k] >= cl[k]) cp[k] = 0;
                out += y;
            }
            for (int k = 0; k < 2; k++) {
                float bo = ap[k][apos[k]];
                float y = -out + bo;
                ap[k][apos[k]] = out + bo * 0.5f;
                if (++apos[k] >= al[k]) apos[k] = 0;
                out = y;
            }
            /* second time round: add the reverb in place (each sample is read as dry before it is written) */
            if (pass) mix[i] += out * amount;
        }
}

static void begin(float bpm, int beats)
{
    BEAT = 60.0f / bpm;
    N = ((int)((float)beats * BEAT * SR) + 15) & ~15;
    if (N > MUS_MAX) N = MUS_MAX;
    memset(mix, 0, sizeof(float) * MUS_MAX);
}

static void finish(int track)
{
    float peak = 0;
    for (int i = 0; i < N; i++) if (fabsf(mix[i]) > peak) peak = fabsf(mix[i]);
    float k = peak > 0 ? 0.85f * 32000.0f / peak : 0;
    for (int i = 0; i < N; i++) MUS[track][i] = (int16_t)(mix[i] * k);
    mus_len[track] = N;
}

typedef struct { float b, d, m; } Note;      /* beat within the bar, length in beats, midi note */

/* ------------------------------------------------------------------ */
/* 0: The Storm - dreary, D minor, 66 bpm                               */
/* ------------------------------------------------------------------ */
static void track_storm(void)
{
    begin(66, 32);
    static const float PADS[4][4] = { {50, 53, 57, 62}, {46, 50, 53, 58}, {43, 50, 55, 58}, {45, 49, 52, 57} };
    static const float ROOT[4] = { 38, 34, 43, 45 };
    for (int bar = 0; bar < 8; bar++) {
        int c = bar & 3;
        for (int k = 0; k < 4; k++) pad((float)bar * 4, 4.2f, PADS[c][k], 0.55f, 900.0f);
        bass((float)bar * 4, 3.6f, ROOT[c], 0.5f);
    }
    static const Note MEL[8][4] = {
        { {0, 1.5f, 69}, {1.5f, 0.5f, 67}, {2, 2, 65}, {0, 0, 0} },
        { {0, 1, 62}, {1, 1, 65}, {2, 2, 70}, {0, 0, 0} },
        { {0, 1.5f, 69}, {1.5f, 0.5f, 67}, {2, 1, 65}, {3, 1, 62} },
        { {0, 3, 64}, {3, 1, 61}, {0, 0, 0}, {0, 0, 0} },
        { {0, 1, 74}, {1, 1, 72}, {2, 2, 69}, {0, 0, 0} },
        { {0, 2, 70}, {2, 1, 69}, {3, 1, 65}, {0, 0, 0} },
        { {0, 1, 67}, {1, 1, 70}, {2, 2, 74}, {0, 0, 0} },
        { {0, 2, 73}, {2, 2, 69}, {0, 0, 0}, {0, 0, 0} },
    };
    for (int bar = 0; bar < 8; bar++)
        for (int k = 0; k < 4; k++)
            if (MEL[bar][k].m > 0) epiano((float)bar * 4 + MEL[bar][k].b, MEL[bar][k].d, MEL[bar][k].m, 0.5f);
    drone(26, 0.6f);
    /* a few lonely high drops */
    static const float DROP[][2] = { {6.5f, 81}, {13, 77}, {21.5f, 84}, {27, 74}, {29.5f, 81} };
    for (int i = 0; i < 5; i++) bell(DROP[i][0], DROP[i][1], 0.22f, 0.5f);
    reverb(0.55f, 1.3f);
    finish(0);
}

/* ------------------------------------------------------------------ */
/* 1: Autumn Gold - fresh and simple, G major, 100 bpm                  */
/* ------------------------------------------------------------------ */
static void track_autumn(void)
{
    begin(100, 48);
    /* G D Em C | G D C D | Em C G D */
    static const int PROG[12] = { 0, 1, 2, 3, 0, 1, 3, 1, 2, 3, 0, 1 };
    static const float VOICE[4][5] = { {43, 50, 55, 59, 62}, {38, 45, 50, 54, 57}, {40, 47, 52, 55, 59}, {36, 43, 48, 52, 55} };
    static const int ARP[8] = { 0, 1, 2, 3, 4, 3, 2, 1 };
    for (int bar = 0; bar < 12; bar++) {
        int c = PROG[bar];
        for (int e = 0; e < 8; e++) pluck((float)bar * 4 + (float)e * 0.5f, 0.9f, VOICE[c][ARP[e]] + 12, e == 0 ? 0.55f : 0.42f, 0.55f);
        bass((float)bar * 4, 1.6f, VOICE[c][0], 0.55f);
        bass((float)bar * 4 + 2, 1.6f, VOICE[c][0] + (bar % 2 ? 7 : 0), 0.45f);
        kick((float)bar * 4, 0.35f);
        kick((float)bar * 4 + 2, 0.25f);
        for (int e = 0; e < 8; e++) noise_hit((float)bar * 4 + (float)e * 0.5f, (e & 1) ? 0.10f : 0.06f, 0.025f, 7000.0f, 0.8f);
    }
    static const Note MEL[12][4] = {
        { {0, 1.5f, 74}, {1.5f, 0.5f, 76}, {2, 2, 79}, {0, 0, 0} },
        { {0, 1, 78}, {1, 1, 76}, {2, 2, 74}, {0, 0, 0} },
        { {0, 1.5f, 76}, {1.5f, 0.5f, 74}, {2, 1, 71}, {3, 1, 74} },
        { {0, 3, 72}, {3, 1, 71}, {0, 0, 0}, {0, 0, 0} },
        { {0, 1, 71}, {1, 1, 74}, {2, 1, 79}, {3, 1, 78} },
        { {0, 2, 76}, {2, 2, 74}, {0, 0, 0}, {0, 0, 0} },
        { {0, 1, 72}, {1, 1, 76}, {2, 2, 79}, {0, 0, 0} },
        { {0, 4, 78}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0} },
        { {0, 1, 79}, {1, 1, 78}, {2, 2, 76}, {0, 0, 0} },
        { {0, 1, 76}, {1, 1, 74}, {2, 2, 72}, {0, 0, 0} },
        { {0, 1.5f, 71}, {1.5f, 0.5f, 74}, {2, 2, 79}, {0, 0, 0} },
        { {0, 2, 78}, {2, 2, 74}, {0, 0, 0}, {0, 0, 0} },
    };
    for (int bar = 0; bar < 12; bar++)
        for (int k = 0; k < 4; k++)
            if (MEL[bar][k].m > 0) flute((float)bar * 4 + MEL[bar][k].b, MEL[bar][k].d, MEL[bar][k].m, 0.5f);
    reverb(0.28f, 1.0f);
    finish(1);
}

/* ------------------------------------------------------------------ */
/* 2: Hallows Eve - fun and spooky, E minor, 128 bpm                    */
/* ------------------------------------------------------------------ */
static void track_hallow(void)
{
    begin(128, 64);
    enum { EM, CM, B7, AM };
    static const int PROG[16] = { EM, EM, CM, B7, EM, EM, AM, B7, CM, AM, EM, B7, EM, CM, B7, B7 };
    static const float BASS[4][4] = { {40, 47, 40, 47}, {36, 43, 36, 43}, {35, 42, 35, 39}, {45, 52, 45, 52} };
    static const float STAB[4][3] = { {64, 67, 71}, {64, 67, 72}, {63, 66, 69}, {64, 69, 72} };
    static const float PADN[4][2] = { {52, 59}, {48, 55}, {47, 54}, {45, 52} };
    for (int bar = 0; bar < 16; bar++) {
        int c = PROG[bar];
        float b0 = (float)bar * 4;
        for (int q = 0; q < 4; q++) pluck(b0 + (float)q, 0.45f, BASS[c][q], q == 0 ? 0.75f : 0.6f, 0.35f);   /* pizzicato oom-pah */
        for (int q = 1; q < 4; q += 2)
            for (int k = 0; k < 3; k++) organ(b0 + (float)q, 0.2f, STAB[c][k], 0.5f);
        for (int k = 0; k < 2; k++) pad(b0, 4.0f, PADN[c][k], 0.3f, 700.0f);
        kick(b0, 0.45f); kick(b0 + 2, 0.35f);
        noise_hit(b0 + 1, 0.5f, 0.04f, 1800.0f, 1.4f);       /* finger snaps */
        noise_hit(b0 + 3, 0.5f, 0.04f, 1800.0f, 1.4f);
        if ((bar & 3) == 3)                                     /* rattling bones fill */
            for (int k = 0; k < 4; k++) bones(b0 + 2.0f + (float)k * 0.5f, 79.0f - (float)k, 0.6f);
        if ((bar & 1) == 1)                                     /* celesta sparkle */
            for (int k = 0; k < 3; k++) bell(b0 + 3.25f + (float)k * 0.25f, (float[]){88, 83, 79}[k], 0.3f, 0.35f);
    }
    static const Note MEL[16][4] = {
        { {0, 1, 64}, {1, 1, 67}, {2, 1, 71}, {3, 1, 72} },
        { {0, 2, 71}, {2, 1, 70}, {3, 1, 71}, {0, 0, 0} },
        { {0, 1, 72}, {1, 1, 71}, {2, 1, 67}, {3, 1, 64} },
        { {0, 3, 66}, {3, 1, 63}, {0, 0, 0}, {0, 0, 0} },
        { {0, 1, 64}, {1, 1, 67}, {2, 1, 71}, {3, 1, 76} },
        { {0, 2, 75}, {2, 1, 74}, {3, 1, 71}, {0, 0, 0} },
        { {0, 1, 72}, {1, 1, 76}, {2, 1, 72}, {3, 1, 69} },
        { {0, 4, 71}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0} },
        { {0, 1.5f, 76}, {1.5f, 0.5f, 74}, {2, 2, 72}, {0, 0, 0} },
        { {0, 1.5f, 72}, {1.5f, 0.5f, 71}, {2, 2, 69}, {0, 0, 0} },
        { {0, 1, 67}, {1, 1, 71}, {2, 1, 76}, {3, 1, 79} },
        { {0, 2, 78}, {2, 2, 75}, {0, 0, 0}, {0, 0, 0} },
        { {0, 1, 76}, {1, 1, 71}, {2, 1, 67}, {3, 1, 64} },
        { {0, 2, 67}, {2, 1, 72}, {3, 1, 71}, {0, 0, 0} },
        { {0, 1, 69}, {1, 1, 66}, {2, 1, 63}, {3, 1, 66} },
        { {0, 3, 71}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0} },
    };
    th_prev = 71;
    for (int bar = 0; bar < 16; bar++)
        for (int k = 0; k < 4; k++)
            if (MEL[bar][k].m > 0) theremin((float)bar * 4 + MEL[bar][k].b, MEL[bar][k].d, MEL[bar][k].m, 0.55f);
    reverb(0.32f, 1.1f);
    finish(2);
}

/* soft piano: like the electric piano but rounder, with a long ring */
static void piano(float b, float dur, float m, float vel)
{
    int s0 = (int)(b * BEAT * SR), n = (int)((dur * BEAT + 2.5f) * SR);
    float f = mtof(m), pc = 0, pm = 0;
    for (int i = 0; i < n; i++) {
        float t = (float)i / SR, idx = 0.9f * expf(-t * 5.0f);
        float e = env_adr(i, n, 0.003f, 2.2f, 0.5f);
        float v = sinf(TAU_F * pc + idx * sinf(TAU_F * pm)) + 0.3f * sinf(TAU_F * pc * 2.0f) * expf(-t * 1.5f) + 0.12f * sinf(TAU_F * pc * 3.0f) * expf(-t * 3.0f);
        put(s0 + i, v * e * vel * 0.3f);
        pc += f / SR; pc -= floorf(pc);
        pm += f * 2.0f / SR; pm -= floorf(pm);
    }
}

/* wind: band-passed noise that swells and fades over the loop (whole cycles, so it loops cleanly) */
static void wind(float vel)
{
    float z1 = 0, z2 = 0;
    for (int i = 0; i < N; i++) {
        float t = (float)i / (float)N;
        float centre = 500.0f + 350.0f * sinf(TAU_F * t * 3.0f);
        float w = TAU_F * centre / SR, a = sinf(w) / 1.4f, cw = cosf(w), a0 = 1.0f + a;
        float b0 = a / a0, b2 = -a / a0, a1 = -2.0f * cw / a0, a2 = (1.0f - a) / a0;
        float x = frnd(), y = b0 * x + z1;
        z1 = -a1 * y + z2;
        z2 = b2 * x - a2 * y;
        float sw = 0.35f + 0.65f * (0.5f + 0.5f * sinf(TAU_F * t * 2.0f + 1.0f));
        put(i, y * sw * vel * 0.18f);
    }
}

/* ------------------------------------------------------------------ */
/* 3: Dawn Isle - slow, mysterious, D lydian, 60 bpm                    */
/* ------------------------------------------------------------------ */
static void track_isle(void)
{
    begin(60, 28);
    /* D, G(#11), Em9, Asus, D, G, Bm */
    static const float CH[7][5] = {
        {50, 57, 61, 64, 66}, {43, 50, 54, 59, 61}, {40, 47, 54, 57, 62}, {45, 52, 57, 59, 64},
        {50, 57, 61, 66, 69}, {43, 50, 55, 59, 61}, {47, 54, 57, 62, 66} };
    for (int bar = 0; bar < 7; bar++) {
        for (int k = 0; k < 5; k++) pad((float)bar * 4, 4.6f, CH[bar][k], 0.36f, 650.0f + 80.0f * (float)k);
        bass((float)bar * 4, 3.8f, CH[bar][0] - 12, 0.32f);
    }
    static const Note MEL[7][3] = {
        { {0, 2, 74}, {2, 2, 69}, {0, 0, 0} },
        { {0, 1, 71}, {1, 3, 66}, {0, 0, 0} },
        { {0, 2, 67}, {2, 2, 71}, {0, 0, 0} },
        { {0, 4, 69}, {0, 0, 0}, {0, 0, 0} },
        { {0, 1, 74}, {1, 1, 76}, {2, 2, 73} },
        { {0, 3, 71}, {3, 1, 74}, {0, 0, 0} },
        { {0, 2, 73}, {2, 2, 69}, {0, 0, 0} },
    };
    for (int bar = 0; bar < 7; bar++)
        for (int k = 0; k < 3; k++)
            if (MEL[bar][k].m > 0) piano((float)bar * 4 + MEL[bar][k].b, MEL[bar][k].d, MEL[bar][k].m, 0.55f);
    /* a quiet echo of the tune an octave up, half a beat behind */
    for (int bar = 0; bar < 7; bar++)
        for (int k = 0; k < 3; k++)
            if (MEL[bar][k].m > 0) piano((float)bar * 4 + MEL[bar][k].b + 0.5f, MEL[bar][k].d, MEL[bar][k].m + 12, 0.14f);
    /* wind chimes */
    static const float CHIME[][2] = { {1.5f, 86}, {2.25f, 81}, {9.5f, 83}, {10.0f, 90}, {17.25f, 78}, {18.0f, 85}, {18.5f, 81}, {25.5f, 88} };
    for (int i = 0; i < 8; i++) bell(CHIME[i][0], CHIME[i][1], 0.18f, 0.7f);
    drone(26, 0.55f);
    wind(0.8f);
    reverb(0.65f, 1.45f);
    finish(3);
}

/* fiddle: a bowed sawtooth through two low-passes, delayed vibrato, a little bow scratch at the start */
static void fiddle(float b, float dur, float m, float vel)
{
    int s0 = (int)(b * BEAT * SR), n = (int)((dur * BEAT + 0.12f) * SR);
    float f = mtof(m), ph = 0, l1 = 0, l2 = 0, bn = 0;
    for (int i = 0; i < n; i++) {
        float t = (float)i / SR;
        float vib = 1.0f + (t > 0.12f ? 0.0045f * sinf(TAU_F * 5.8f * t) * fminf((t - 0.12f) * 4.0f, 1.0f) : 0.0f);
        float e = env_adr(i, n, 0.035f, 0, 0.09f);
        float saw = 2.0f * ph - 1.0f;
        l1 += (saw - l1) * 0.32f; l2 += (l1 - l2) * 0.4f;
        bn += (frnd() - bn) * 0.5f;
        float scratch = t < 0.05f ? bn * (1.0f - t / 0.05f) * 0.35f : 0.0f;
        put(s0 + i, (l2 + scratch) * e * vel * 0.3f);
        ph += f * vib / SR; ph -= floorf(ph);
    }
}

/* accordion: two slightly detuned reeds (soft square waves) beating against each other */
static void accordion(float b, float dur, float m, float vel)
{
    int s0 = (int)(b * BEAT * SR), n = (int)((dur * BEAT + 0.06f) * SR);
    float f = mtof(m), p1 = 0, p2 = 0, lp = 0;
    for (int i = 0; i < n; i++) {
        float e = env_adr(i, n, 0.025f, 0, 0.05f);
        float v = (p1 < 0.5f ? 1.0f : -1.0f) + (p2 < 0.45f ? 0.8f : -0.8f);
        lp += (v - lp) * 0.18f;
        put(s0 + i, lp * e * vel * 0.09f);
        p1 += f * 1.0025f / SR; p1 -= floorf(p1);
        p2 += f * 0.9975f / SR; p2 -= floorf(p2);
    }
}

/* low brass: a sawtooth whose filter opens as the note swells */
static void brass(float b, float dur, float m, float vel)
{
    int s0 = (int)(b * BEAT * SR), n = (int)((dur * BEAT + 0.2f) * SR);
    float f = mtof(m), ph = 0, z1 = 0, z2 = 0;
    for (int i = 0; i < n; i++) {
        float e = env_adr(i, n, 0.18f, 0, 0.2f);
        float k = 0.03f + 0.12f * e;
        float saw = 2.0f * ph - 1.0f;
        z1 += (saw - z1) * k; z2 += (z1 - z2) * k;
        put(s0 + i, z2 * e * vel * 0.5f);
        ph += f / SR; ph -= floorf(ph);
    }
}

/* timpani: a tuned drum - a sine that sags a little in pitch, plus a soft mallet thump */
static void timpani(float b, float m, float vel)
{
    int s0 = (int)(b * BEAT * SR), n = (int)(1.1f * SR);
    float f = mtof(m), ph = 0, lp = 0;
    for (int i = 0; i < n; i++) {
        float t = (float)i / SR;
        float ff = f * (1.0f + 0.06f * expf(-t * 25.0f));
        lp += (frnd() - lp) * 0.08f;
        float v = sinf(TAU_F * ph) * expf(-t * 3.2f) + 0.4f * sinf(TAU_F * ph * 1.5f) * expf(-t * 6.0f) + lp * 1.5f * expf(-t * 30.0f);
        put(s0 + i, v * vel * 0.5f);
        ph += ff / SR; ph -= floorf(ph);
    }
}

/* ------------------------------------------------------------------ */
/* 4: Pirate Storm - an epic sea shanty, D minor, 6/8 (eighth = 200)   */
/* ------------------------------------------------------------------ */
static void track_pirate(void)
{
    begin(200, 96);                            /* 16 bars of six eighths */
    enum { DM, CM, AM, FM, BB };
    static const int PROG[16] = { DM, DM, CM, CM, DM, DM, AM, DM, FM, CM, DM, AM, BB, CM, AM, DM };
    static const float ROOT[5] = { 38, 36, 45, 41, 46 };
    static const float CH[5][3] = { {62, 65, 69}, {60, 64, 67}, {61, 64, 69}, {60, 65, 69}, {62, 65, 70} };
    for (int bar = 0; bar < 16; bar++) {
        int c = PROG[bar];
        float b0 = (float)bar * 6;
        int big = bar >= 8;
        /* oom-pah-pah: bass on 1 and 4, the accordion wheezing the chord in between */
        bass(b0, 2.5f, ROOT[c], 0.7f);
        bass(b0 + 3, 2.5f, ROOT[c] + 7, 0.55f);
        for (int q = 0; q < 6; q++) {
            if (q == 0 || q == 3) continue;
            for (int k = 0; k < 3; k++) accordion(b0 + (float)q, 0.75f, CH[c][k], q == 1 || q == 4 ? 0.75f : 0.55f);
        }
        /* brass swells: root and fifth, louder in the second half */
        brass(b0, 5.6f, ROOT[c] + 12, big ? 0.38f : 0.24f);
        brass(b0, 5.6f, ROOT[c] + 19, big ? 0.3f : 0.18f);
        if (big) brass(b0, 5.6f, CH[c][1] - 12, 0.22f);
        /* timpani and bodhran */
        float tp = ROOT[c] < 40 ? ROOT[c] + 12 : ROOT[c];
        timpani(b0, tp, 0.7f);
        timpani(b0 + 3, tp - 5, 0.45f);
        for (int q = 0; q < 6; q++)
            noise_hit(b0 + (float)q, q == 0 || q == 3 ? 0.55f : 0.25f, q == 0 || q == 3 ? 0.06f : 0.03f, 260.0f, 1.2f);
        if ((bar & 7) == 7)                    /* a timpani roll into the next phrase */
            for (int k = 0; k < 6; k++) timpani(b0 + 3.0f + (float)k * 0.5f, 45, 0.25f + 0.08f * (float)k);
        if ((bar & 3) == 1) noise_hit(b0 + 5, 0.4f, 0.12f, 5000.0f, 0.8f);   /* cymbal splash */
    }
    static const Note MEL[16][6] = {
        { {0, 2, 69}, {2, 1, 74}, {3, 2, 74}, {5, 1, 72} },
        { {0, 2, 70}, {2, 1, 69}, {3, 3, 65} },
        { {0, 2, 67}, {2, 1, 72}, {3, 2, 72}, {5, 1, 70} },
        { {0, 2, 69}, {2, 1, 67}, {3, 3, 64} },
        { {0, 2, 69}, {2, 1, 74}, {3, 2, 74}, {5, 1, 76} },
        { {0, 2, 77}, {2, 1, 76}, {3, 2, 74}, {5, 1, 72} },
        { {0, 2, 73}, {2, 1, 76}, {3, 2, 73}, {5, 1, 69} },
        { {0, 3, 74}, {3, 3, 62} },
        { {0, 1, 72}, {1, 1, 74}, {2, 1, 72}, {3, 2, 69}, {5, 1, 65} },
        { {0, 1, 67}, {1, 1, 69}, {2, 1, 67}, {3, 2, 64}, {5, 1, 60} },
        { {0, 2, 62}, {2, 1, 65}, {3, 2, 69}, {5, 1, 74} },
        { {0, 3, 73}, {3, 2, 76}, {5, 1, 73} },
        { {0, 2, 74}, {2, 1, 77}, {3, 2, 74}, {5, 1, 70} },
        { {0, 2, 72}, {2, 1, 76}, {3, 2, 72}, {5, 1, 67} },
        { {0, 2, 69}, {2, 1, 73}, {3, 1, 76}, {4, 1, 79}, {5, 1, 76} },
        { {0, 3, 74}, {3, 2, 69}, {5, 1, 73} },
    };
    for (int bar = 0; bar < 16; bar++)
        for (int k = 0; k < 6; k++) {
            const Note *nt = &MEL[bar][k];
            if (nt->m <= 0) continue;
            fiddle((float)bar * 6 + nt->b, nt->d, nt->m, 0.62f);
            if (bar >= 8) fiddle((float)bar * 6 + nt->b, nt->d, nt->m - 12, 0.3f);   /* the crew joins in an octave down */
        }
    drone(26, 0.4f);
    wind(1.1f);
    reverb(0.36f, 1.25f);
    finish(4);
}

/* synth bass: a sawtooth through a low-pass that snaps open on each note */
static void sbass(float b, float dur, float m, float vel)
{
    int s0 = (int)(b * BEAT * SR), n = (int)((dur * BEAT + 0.03f) * SR);
    float f = mtof(m), ph = 0, z1 = 0, z2 = 0;
    for (int i = 0; i < n; i++) {
        float t = (float)i / SR;
        float e = env_adr(i, n, 0.004f, 0, 0.03f);
        float k = 0.03f + 0.25f * expf(-t * 18.0f);
        float saw = 2.0f * ph - 1.0f;
        z1 += (saw - z1) * k; z2 += (z1 - z2) * k;
        put(s0 + i, z2 * e * vel * 0.55f);
        ph += f / SR; ph -= floorf(ph);
    }
}

/* synth lead: two detuned saws, a touch of square, vibrato that comes in late - a heroic 80s brass-synth */
static void slead(float b, float dur, float m, float vel)
{
    int s0 = (int)(b * BEAT * SR), n = (int)((dur * BEAT + 0.15f) * SR);
    float f = mtof(m), p1 = 0, p2 = 0, z1 = 0, z2 = 0;
    for (int i = 0; i < n; i++) {
        float t = (float)i / SR;
        float vib = 1.0f + (t > 0.2f ? 0.005f * sinf(TAU_F * 5.5f * t) * fminf((t - 0.2f) * 3.0f, 1.0f) : 0.0f);
        float e = env_adr(i, n, 0.02f, 0, 0.12f);
        float v = (2.0f * p1 - 1.0f) + (2.0f * p2 - 1.0f) * 0.8f + (p1 < 0.5f ? 0.3f : -0.3f);
        float k = 0.12f + 0.1f * expf(-t * 6.0f);
        z1 += (v - z1) * k; z2 += (z1 - z2) * k;
        put(s0 + i, z2 * e * vel * 0.16f);
        p1 += f * vib * 1.004f / SR; p1 -= floorf(p1);
        p2 += f * vib * 0.996f / SR; p2 -= floorf(p2);
    }
}

/* ------------------------------------------------------------------ */
/* 5: 88 MPH - driving 80s synthwave in C minor, 128 bpm                */
/* ------------------------------------------------------------------ */
static void track_future(void)
{
    begin(128, 64);
    enum { CM, AB, EB, BB, G };
    static const int PROG[16] = { CM, AB, EB, BB, CM, AB, EB, BB, AB, BB, CM, CM, AB, BB, G, G };
    static const float ROOT[5] = { 36, 32, 39, 34, 31 };
    static const float CH[5][3] = { {60, 63, 67}, {60, 63, 68}, {58, 63, 67}, {58, 62, 65}, {59, 62, 67} };
    for (int bar = 0; bar < 16; bar++) {
        int c = PROG[bar];
        float b0 = (float)bar * 4;
        int big = bar >= 8;
        for (int q = 0; q < 8; q++)                                   /* pumping octave bass */
            sbass(b0 + (float)q * 0.5f, 0.42f, ROOT[c] + ((q & 1) ? 12.0f : 0.0f), (q & 1) ? 0.6f : 0.75f);
        for (int k = 0; k < 3; k++) pad(b0, 4.2f, CH[c][k], big ? 0.3f : 0.24f, 900.0f + 200.0f * (float)k);
        for (int q = 0; q < 16; q++) {                                /* sparkling sixteenth arpeggio */
            static const int ORD[4] = { 0, 1, 2, 1 };
            float m = CH[c][ORD[q & 3]] + 12.0f + ((q & 4) ? 12.0f : 0.0f);
            pluck(b0 + (float)q * 0.25f, 0.22f, m, big ? 0.32f : 0.24f, 0.85f);
        }
        kick(b0, 0.6f); kick(b0 + 1, 0.55f); kick(b0 + 2, 0.6f); kick(b0 + 3, 0.55f);
        for (int s2 = 1; s2 < 4; s2 += 2) {                           /* big gated snare */
            noise_hit(b0 + (float)s2, 0.8f, 0.09f, 1900.0f, 0.7f);
            noise_hit(b0 + (float)s2, 0.35f, 0.05f, 220.0f, 1.2f);
        }
        for (int h = 0; h < 8; h++) noise_hit(b0 + (float)h * 0.5f + 0.25f * (float)(h & 1) * 0.0f, (h & 1) ? 0.3f : 0.18f, 0.02f, 8500.0f, 1.0f);
        if ((bar & 3) == 3)                                          /* tom fill */
            for (int k = 0; k < 4; k++) kick(b0 + 3.0f + (float)k * 0.25f, 0.3f + 0.08f * (float)k);
    }
    static const Note MEL[16][4] = {
        { {0, 1.5f, 67}, {1.5f, 0.5f, 70}, {2, 2, 72} },
        { {0, 1.5f, 75}, {1.5f, 0.5f, 74}, {2, 2, 72} },
        { {0, 1, 70}, {1, 1, 67}, {2, 2, 70} },
        { {0, 3, 74}, {3, 1, 70} },
        { {0, 1.5f, 67}, {1.5f, 0.5f, 70}, {2, 2, 72} },
        { {0, 1.5f, 75}, {1.5f, 0.5f, 77}, {2, 2, 79} },
        { {0, 1, 79}, {1, 1, 77}, {2, 2, 75} },
        { {0, 4, 74} },
        { {0, 2, 72}, {2, 1, 75}, {3, 1, 79} },
        { {0, 2, 77}, {2, 1, 74}, {3, 1, 70} },
        { {0, 3, 79}, {3, 1, 84} },
        { {0, 2, 82}, {2, 2, 79} },
        { {0, 2, 80}, {2, 1, 79}, {3, 1, 77} },
        { {0, 2, 79}, {2, 2, 77} },
        { {0, 2, 74}, {2, 1, 77}, {3, 1, 79} },
        { {0, 4, 74} },
    };
    for (int bar = 0; bar < 16; bar++)
        for (int k = 0; k < 4; k++) {
            const Note *nt = &MEL[bar][k];
            if (nt->m <= 0) continue;
            slead((float)bar * 4 + nt->b, nt->d, nt->m, bar >= 8 ? 0.7f : 0.55f);
            if (bar >= 8) slead((float)bar * 4 + nt->b, nt->d, nt->m - 12, 0.3f);
        }
    reverb(0.3f, 1.2f);
    finish(5);
}

void music_init(void)
{
    if (ready) return;
    ready = 1;
    mix = (float *)malloc(sizeof(float) * MUS_MAX);
    if (!mix) return;
    track_storm();
    track_autumn();
    track_hallow();
    track_isle();
    track_pirate();
    track_future();
    free(mix); mix = NULL;
}

const int16_t *music_data(int track) { return MUS[track]; }
int            music_len(int track)  { return mus_len[track]; }
int            music_volume(int track) { return VOL[track]; }
