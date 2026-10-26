/*
 * STORMFIRE - sound effect synthesis. See audio.h.
 *
 * Every effect is built at start-up from a small synth (band-limited and FM oscillators, resonant filtered
 * noise and a short echo), so no audio files are needed. Chunky, warm tabletop sounds plus a storm:
 * rolling thunder and a sharp lightning crack.
 */
#include "audio.h"

#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define MAXLEN 80000                       /* 2.5 s per effect at 32 kHz, a multiple of 16 so rows stay 32-byte aligned */

/* every effect lives back to back in one pool (each start 32-byte aligned), sized to what they really need */
#define SFX_POOL 1600000
static int16_t SFXPOOL[SFX_POOL] __attribute__((aligned(32)));
static int     sfx_off[SFX_COUNT], sfx_len[SFX_COUNT], pool_used;
static float   mixbuf[MAXLEN];
static float   outbuf[MAXLEN];
static int     mixend;
static uint32_t rng = 0x2545F491u;
static int     ready;

/* playback volume per effect (every effect is normalised first, then scaled by this to balance loudness) */
static const uint8_t VOL[SFX_COUNT] = {
    /* BLASTER */ 60,  /* MORTAR */ 130, /* BOOM */ 190,   /* FROST */ 110,  /* ZAP */ 120,    /* HIT */ 45,
    /* POP */ 120,     /* CRUNCH */ 190, /* BUILD */ 190,  /* UPGRADE */ 200, /* SELL */ 170,   /* ERROR */ 140,
    /* MOVE */ 60,     /* SELECT */ 120, /* WAVE */ 200,   /* CLEAR */ 200,  /* HISS */ 200,   /* THUNDER */ 255,
    /* CRACK */ 230,   /* BOSS */ 240,   /* WIN */ 220,    /* LOSE */ 220,   /* PAUSE */ 120,  /* START */ 200,
    /* COIN */ 170,    /* GUST */ 150,   /* CANNON */ 220, /* SPLASH */ 160,
    /* pinball: BUMPER */ 130, /* KICKER */ 170, /* FLIPPER */ 170, /* COMBO */ 190, /* DROP */ 170, /* BUZZ */ 150,
    /* TICK */ 70, /* DING */ 150, /* LAUNCH */ 210, /* JACKPOT */ 220, /* TILT */ 210, /* SCOTT */ 210, /* WIN */ 230,
    /* DRAIN */ 220, /* WARP */ 150, /* SURGE */ 240, /* GIGA */ 240
};

/* ------------------------------------------------------------------ */
/* Building blocks                                                     */
/* ------------------------------------------------------------------ */
enum { W_SIN, W_TRI, W_SOFT, W_FM };           /* oscillator shapes */
enum { F_LP, F_BP, F_HP };                      /* noise filter types */

#define SR ((float)SFX_RATE)
#define TABN 4096
static float sine_tab[TABN + 1];

static inline float sine01(float ph)            /* ph in [0,1) */
{
    float x = ph * (float)TABN;
    int i = (int)x;
    float fr = x - (float)i;
    return sine_tab[i] + (sine_tab[i + 1] - sine_tab[i]) * fr;
}

static float frand(void)                        /* -1 .. 1 */
{
    rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
    return (float)(rng >> 8) / 8388608.0f - 1.0f;
}

/*
 * One pitched voice mixed into the effect being built.
 *   start, dur : seconds
 *   f0 -> f1   : exponential pitch glide in Hz
 *   atk        : attack time (s);  tau : decay time constant (s), 0 = sustained
 *   W_SIN      : pure sine
 *   W_TRI      : soft triangle (sine + a few odd harmonics)
 *   W_SOFT     : warm band-limited saw; a = extra high-harmonic roll-off (0 bright .. 2 dark)
 *   W_FM       : two-operator FM; a = modulator/carrier ratio, b = peak modulation index (fades with the note)
 */
static void tone(float start, float dur, float f0, float f1, float vol, float atk, float tau, int wave, float a, float b)
{
    int s0 = (int)(start * SR), n = (int)(dur * SR);
    float ph = 0.0f, pm = 0.0f;
    float glide = (f1 == f0) ? 1.0f : powf(f1 / f0, 1.0f / (float)(n > 1 ? n : 1));
    float f = f0, hw[10];
    for (int h = 1; h <= 9; h++) hw[h] = 1.0f / powf((float)h, 1.0f + a);
    float env = 1.0f, k = (tau > 0.0f) ? expf(-1.0f / (tau * SR)) : 1.0f;
    float atk_n = atk * SR > 1.0f ? atk * SR : 1.0f;
    float rel_n = 0.006f * SR;
    for (int i = 0; i < n && s0 + i < MAXLEN; i++) {
        float amp = env;
        if ((float)i < atk_n) amp *= (float)i / atk_n;
        if ((float)(n - i) < rel_n) amp *= (float)(n - i) / rel_n;
        float v;
        switch (wave) {
        case W_TRI:
            v = sine01(ph) + 0.111f * sine01(ph * 3.0f - floorf(ph * 3.0f)) * -1.0f + 0.04f * sine01(ph * 5.0f - floorf(ph * 5.0f));
            break;
        case W_SOFT: {
            v = 0.0f;
            int nh = (int)(0.42f * SR / (f > 1.0f ? f : 1.0f));
            if (nh > 9) nh = 9;
            for (int h = 1; h <= nh; h++) {
                float x = ph * (float)h;
                v += sine01(x - floorf(x)) * hw[h];
            }
            break;
        }
        case W_FM: {
            float idx = b * env;
            float m = sine01(pm);
            float x = ph + idx * m / (2.0f * (float)M_PI) * 2.0f;
            x -= floorf(x);
            v = sine01(x);
            break;
        }
        default:
            v = sine01(ph);
            break;
        }
        mixbuf[s0 + i] += v * amp * vol;
        if (s0 + i + 1 > mixend) mixend = s0 + i + 1;
        ph += f / SR; if (ph >= 1.0f) ph -= 1.0f;
        pm += f * a / SR; pm -= floorf(pm);
        f *= glide;
        env *= k;
    }
}

/* resonant filtered noise: fc0 -> fc1 is the filter cutoff sweep in Hz, q the resonance */
static void noise(float start, float dur, float fc0, float fc1, float q, float vol, float atk, float tau, int mode)
{
    int s0 = (int)(start * SR), n = (int)(dur * SR);
    float z1 = 0.0f, z2 = 0.0f;                  /* transposed direct form II state */
    float b0 = 0, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    float env = 1.0f, k = (tau > 0.0f) ? expf(-1.0f / (tau * SR)) : 1.0f;
    float atk_n = atk * SR > 1.0f ? atk * SR : 1.0f;
    float rel_n = 0.006f * SR;
    float ratio = (fc1 == fc0) ? 1.0f : powf(fc1 / fc0, 1.0f / (float)(n > 1 ? n : 1));
    float fc = fc0;
    for (int i = 0; i < n && s0 + i < MAXLEN; i++) {
        if ((i & 7) == 0) {                       /* retune the filter every 8 samples */
            float w = 2.0f * (float)M_PI * (fc < 0.45f * SR ? fc : 0.45f * SR) / SR;
            float cw = cosf(w), alpha = sinf(w) / (2.0f * q), a0 = 1.0f + alpha;
            switch (mode) {
            case F_LP: b0 = (1 - cw) / 2; b1 = 1 - cw; b2 = (1 - cw) / 2; break;
            case F_HP: b0 = (1 + cw) / 2; b1 = -(1 + cw); b2 = (1 + cw) / 2; break;
            default:   b0 = alpha; b1 = 0; b2 = -alpha; break;
            }
            a1 = -2.0f * cw / a0; a2 = (1.0f - alpha) / a0;
            b0 /= a0; b1 /= a0; b2 /= a0;
        }
        float x = frand();
        float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        float amp = env;
        if ((float)i < atk_n) amp *= (float)i / atk_n;
        if ((float)(n - i) < rel_n) amp *= (float)(n - i) / rel_n;
        mixbuf[s0 + i] += y * amp * vol;
        if (s0 + i + 1 > mixend) mixend = s0 + i + 1;
        env *= k;
        fc *= ratio;
    }
}

static void begin(void)
{
    memset(mixbuf, 0, sizeof mixbuf);
    mixend = 0;
}

/* mix down: a clean two-tap echo for a little space (no reverb noise), normalise, 16-bit. No clipping or saturation. */
static void finish_fx(int id, float wet, float room)
{
    int d1 = (int)((0.045f + 0.07f * room) * SR), d2 = d1 * 2 + (int)(0.013f * SR);
    int tail = (wet > 0.0f) ? d2 + (int)(0.05f * SR) : (int)(0.03f * SR);
    int len = mixend + tail;
    if (len > MAXLEN) len = MAXLEN;
    float peak = 0.0f;
    for (int i = 0; i < len; i++) {
        float x = (i < mixend) ? mixbuf[i] : 0.0f;
        if (i >= d1 && i - d1 < mixend) x += wet * 0.6f * mixbuf[i - d1];
        if (i >= d2 && i - d2 < mixend) x += wet * 0.3f * mixbuf[i - d2];
        outbuf[i] = x;
        if (fabsf(x) > peak) peak = fabsf(x);
    }
    int last = len;                                    /* trim the silent tail */
    while (last > 1 && fabsf(outbuf[last - 1]) < peak * 0.003f) last--;
    last += (int)(0.02f * SR);
    if (last < len) len = last;
    int fade = (int)(0.02f * SR);
    float scale = peak > 0.0f ? 0.85f * 32000.0f / peak : 0.0f;
    int outlen = (len + 15) & ~15;
    if (outlen > MAXLEN) outlen = MAXLEN;
    if (pool_used + outlen > SFX_POOL) { sfx_off[id] = 0; sfx_len[id] = 16; return; }   /* (cannot happen: the pool has room) */
    int16_t *dst = &SFXPOOL[pool_used];
    sfx_off[id] = pool_used;
    pool_used += outlen;
    memset(dst, 0, (size_t)outlen * sizeof *dst);
    for (int i = 0; i < len && i < outlen; i++) {
        float x = outbuf[i];
        if (len - i < fade) x *= (float)(len - i) / (float)fade;
        dst[i] = (int16_t)(x * scale);
    }
    sfx_len[id] = outlen;
}

#define C4 261.63f
#define D4 293.66f
#define EB4 311.13f
#define F4 349.23f
#define G4 392.00f
#define A4 440.00f
#define C5 523.25f
#define D5 587.33f
#define E5 659.25f
#define G5 783.99f
#define A5 880.00f
#define B5 987.77f
#define C6 1046.50f
#define E6 1318.51f
#define G6 1567.98f
#define C7 2093.00f

/* a bell/ping: FM with a slightly inharmonic ratio */
static void bell(float t, float f, float vol, float tau)
{
    tone(t, tau * 5.0f, f, f, vol, 0.002f, tau, W_FM, 3.0f, 1.1f);
    tone(t, tau * 3.0f, f * 2.0f, f * 2.0f, vol * 0.25f, 0.002f, tau * 0.5f, W_SIN, 0, 0);
}


void audio_init(void)
{
    if (ready) return;
    ready = 1;
    for (int i = 0; i <= TABN; i++) sine_tab[i] = sinf((float)i * 2.0f * (float)M_PI / (float)TABN);

    /* --- towers --- */
    begin();                                                         /* blaster: soft round "pew" */
    tone(0, 0.1f, 1500, 420, 0.5f, 0.001f, 0.035f, W_FM, 1.0f, 1.6f);
    tone(0, 0.08f, 800, 260, 0.35f, 0.001f, 0.03f, W_SIN, 0, 0);
    noise(0, 0.03f, 3000, 3000, 1.0f, 0.15f, 0.001f, 0.01f, F_BP);
    finish_fx(SFX_BLASTER, 0.15f, 0.1f);

    begin();                                                         /* mortar launch: hollow thoomp */
    tone(0, 0.25f, 180, 70, 1.0f, 0.001f, 0.08f, W_SIN, 0, 0);
    noise(0, 0.2f, 900, 300, 0.8f, 0.5f, 0.001f, 0.06f, F_LP);
    tone(0.02f, 0.25f, 500, 900, 0.15f, 0.01f, 0.1f, W_SIN, 0, 0);
    finish_fx(SFX_MORTAR, 0.25f, 0.3f);

    begin();                                                         /* shell explosion: boom + crumble */
    tone(0, 0.6f, 150, 35, 1.0f, 0.001f, 0.18f, W_SIN, 0, 0);
    noise(0, 0.7f, 2200, 150, 0.7f, 0.8f, 0.001f, 0.17f, F_LP);
    noise(0.05f, 0.4f, 4000, 800, 1.2f, 0.25f, 0.01f, 0.1f, F_BP);
    finish_fx(SFX_BOOM, 0.4f, 0.6f);

    begin();                                                         /* frost pulse: glassy shimmer + whoosh */
    tone(0, 0.5f, 2600, 1800, 0.3f, 0.005f, 0.2f, W_FM, 3.01f, 1.4f);
    tone(0.03f, 0.5f, 3500, 2400, 0.2f, 0.005f, 0.18f, W_FM, 2.0f, 1.0f);
    noise(0, 0.45f, 5000, 1500, 2.0f, 0.4f, 0.02f, 0.15f, F_BP);
    finish_fx(SFX_FROST, 0.35f, 0.4f);

    begin();                                                         /* tesla zap: buzzy electric crackle */
    tone(0, 0.22f, 120, 90, 0.6f, 0.001f, 0.08f, W_SOFT, 0.0f, 0);
    tone(0, 0.2f, 2400, 900, 0.35f, 0.001f, 0.07f, W_FM, 1.5f, 6.0f);
    for (int i = 0; i < 5; i++) noise(0.03f * (float)i, 0.03f, 6000, 3000, 1.0f, 0.5f, 0.001f, 0.01f, F_HP);
    finish_fx(SFX_ZAP, 0.2f, 0.2f);

    begin();                                                         /* bolt hit: tiny tick */
    tone(0, 0.04f, 1400, 800, 0.5f, 0.001f, 0.012f, W_SIN, 0, 0);
    noise(0, 0.03f, 3500, 3500, 1.0f, 0.3f, 0.001f, 0.01f, F_BP);
    finish_fx(SFX_HIT, 0.0f, 0.0f);

    /* --- enemies --- */
    begin();                                                         /* small enemy pops: bubbly "plip" */
    tone(0, 0.12f, 500, 1500, 0.6f, 0.001f, 0.04f, W_SIN, 0, 0);
    noise(0, 0.12f, 2000, 600, 0.8f, 0.4f, 0.001f, 0.04f, F_LP);
    tone(0.04f, 0.1f, 1800, 2200, 0.2f, 0.002f, 0.03f, W_FM, 2.0f, 0.8f);
    finish_fx(SFX_POP, 0.2f, 0.2f);

    begin();                                                         /* brute falls apart: crunchy rubble */
    tone(0, 0.5f, 110, 40, 0.9f, 0.001f, 0.15f, W_SIN, 0, 0);
    for (int i = 0; i < 6; i++) noise(0.04f * (float)i, 0.12f, 1600 - 150 * (float)i, 400, 1.2f, 0.5f, 0.002f, 0.04f, F_BP);
    finish_fx(SFX_CRUNCH, 0.3f, 0.4f);

    begin();                                                         /* fire hit: steamy hiss + sputter */
    noise(0, 0.8f, 7000, 2500, 0.6f, 0.7f, 0.01f, 0.3f, F_HP);
    tone(0, 0.4f, 300, 90, 0.4f, 0.002f, 0.15f, W_FM, 1.0f, 2.0f);
    for (int i = 0; i < 4; i++) noise(0.05f + 0.07f * (float)i, 0.04f, 1200, 1200, 1.5f, 0.4f, 0.001f, 0.015f, F_BP);
    finish_fx(SFX_HISS, 0.25f, 0.3f);

    /* --- building --- */
    begin();                                                         /* build: chunky clunk + rising chime */
    tone(0, 0.15f, 220, 90, 0.8f, 0.001f, 0.05f, W_SIN, 0, 0);
    noise(0, 0.1f, 1500, 500, 1.0f, 0.5f, 0.001f, 0.03f, F_LP);
    bell(0.08f, C5, 0.3f, 0.06f);
    bell(0.15f, G5, 0.3f, 0.08f);
    finish_fx(SFX_BUILD, 0.3f, 0.3f);

    begin();                                                         /* upgrade: sparkling arpeggio */
    for (int i = 0; i < 4; i++) {
        static const float nf[4] = {C5, E5, G5, C6};
        bell(0.06f * (float)i, nf[i], 0.35f, 0.08f);
    }
    tone(0.2f, 0.4f, C6, C6, 0.2f, 0.01f, 0.2f, W_FM, 2.0f, 1.0f);
    finish_fx(SFX_UPGRADE, 0.4f, 0.4f);

    begin();                                                         /* sell: coins tumbling */
    for (int i = 0; i < 4; i++) bell(0.05f * (float)i, 2800.0f + 300.0f * (float)(i & 1), 0.3f, 0.04f);
    tone(0, 0.12f, 300, 120, 0.4f, 0.001f, 0.04f, W_SIN, 0, 0);
    finish_fx(SFX_SELL, 0.2f, 0.2f);

    begin();                                                         /* error: low double buzz */
    tone(0, 0.12f, 150, 140, 0.7f, 0.003f, 0.05f, W_SOFT, 0.0f, 0);
    tone(0.1f, 0.14f, 120, 110, 0.7f, 0.003f, 0.05f, W_SOFT, 0.0f, 0);
    finish_fx(SFX_ERROR, 0.0f, 0.0f);

    begin();                                                         /* cursor move: soft wooden tick */
    tone(0, 0.05f, 1100, 900, 0.6f, 0.001f, 0.012f, W_SIN, 0, 0);
    finish_fx(SFX_MOVE, 0.0f, 0.0f);

    begin();                                                         /* choose tower */
    tone(0.00f, 0.07f, 700, 900, 0.5f, 0.003f, 0.03f, W_SIN, 0, 0);
    tone(0.05f, 0.12f, 1200, 1400, 0.4f, 0.003f, 0.05f, W_FM, 2.0f, 0.6f);
    finish_fx(SFX_SELECT, 0.25f, 0.2f);

    begin();                                                         /* coin bonus for calling a wave early */
    bell(0, 1975.0f, 0.4f, 0.05f);
    bell(0.07f, 2637.0f, 0.4f, 0.1f);
    finish_fx(SFX_COIN, 0.3f, 0.3f);

    /* --- flow --- */
    begin();                                                         /* wave start: a war horn through the rain */
    tone(0.00f, 0.9f, 98, 110, 0.6f, 0.08f, 0.5f, W_SOFT, 0.6f, 0);
    tone(0.00f, 0.9f, 147, 165, 0.4f, 0.08f, 0.5f, W_SOFT, 0.8f, 0);
    tone(0.45f, 0.8f, 110, 110, 0.5f, 0.05f, 0.4f, W_SOFT, 0.6f, 0);
    tone(0.45f, 0.8f, 165, 165, 0.35f, 0.05f, 0.4f, W_SOFT, 0.8f, 0);
    finish_fx(SFX_WAVE, 0.5f, 0.8f);

    begin();                                                         /* wave cleared: warm chord */
    for (int i = 0; i < 4; i++) {
        static const float nf[4] = {G4, C5, E5, G5};
        tone(0.07f * (float)i, 0.8f, nf[i], nf[i], 0.3f, 0.005f, 0.35f, W_FM, 1.0f, 1.0f);
    }
    finish_fx(SFX_CLEAR, 0.45f, 0.5f);

    begin();                                                         /* boss: deep horn + rumble */
    tone(0, 1.6f, 55, 49, 0.8f, 0.1f, 0.9f, W_SOFT, 0.4f, 0);
    tone(0, 1.6f, 82, 73, 0.5f, 0.1f, 0.9f, W_SOFT, 0.6f, 0);
    noise(0, 1.6f, 300, 80, 0.7f, 0.4f, 0.2f, 0.8f, F_LP);
    finish_fx(SFX_BOSS, 0.4f, 0.8f);

    begin();                                                         /* victory fanfare */
    for (int i = 0; i < 6; i++) {
        static const float nf[6] = {C5, E5, G5, C6, G5, C6};
        static const float tt[6] = {0, 0.12f, 0.24f, 0.36f, 0.6f, 0.72f};
        tone(tt[i], 0.5f, nf[i], nf[i], 0.35f, 0.005f, 0.25f, W_FM, 2.0f, 1.0f);
    }
    tone(0.72f, 1.2f, C4, C4, 0.3f, 0.02f, 0.6f, W_SOFT, 0.8f, 0);
    finish_fx(SFX_WIN, 0.5f, 0.6f);

    begin();                                                         /* defeat: the fire hisses out */
    noise(0, 1.8f, 6000, 800, 0.5f, 0.7f, 0.02f, 0.7f, F_LP);
    tone(0.1f, 1.5f, 330, 110, 0.5f, 0.02f, 0.6f, W_FM, 1.0f, 1.2f);
    tone(0.1f, 1.5f, 262, 87, 0.4f, 0.02f, 0.6f, W_FM, 1.0f, 1.2f);
    finish_fx(SFX_LOSE, 0.4f, 0.6f);

    begin();                                                         /* pause */
    tone(0, 0.18f, 1200, 500, 0.5f, 0.004f, 0.07f, W_SIN, 0, 0);
    finish_fx(SFX_PAUSE, 0.3f, 0.3f);

    begin();                                                         /* start: whoosh of flame + chime */
    noise(0, 0.6f, 300, 3000, 1.0f, 0.5f, 0.15f, 0.2f, F_BP);
    tone(0.3f, 0.7f, C5, C5, 0.3f, 0.01f, 0.3f, W_FM, 2.0f, 1.0f);
    tone(0.3f, 0.7f, G5, G5, 0.25f, 0.01f, 0.3f, W_FM, 2.0f, 1.0f);
    finish_fx(SFX_START, 0.4f, 0.5f);

    /* --- storm --- */
    begin();                                                         /* thunder: long rolling rumble */
    noise(0.0f, 2.3f, 260, 60, 0.7f, 1.0f, 0.03f, 0.75f, F_LP);
    noise(0.15f, 1.6f, 500, 90, 0.9f, 0.5f, 0.05f, 0.4f, F_LP);
    noise(0.55f, 1.6f, 300, 70, 0.9f, 0.55f, 0.08f, 0.5f, F_LP);
    tone(0.0f, 2.2f, 42, 30, 0.5f, 0.05f, 0.8f, W_SIN, 0, 0);
    finish_fx(SFX_THUNDER, 0.0f, 0.0f);

    begin();                                                         /* lightning crack: sharp tearing snap */
    noise(0.0f, 0.35f, 9000, 1500, 0.6f, 1.0f, 0.001f, 0.06f, F_HP);
    noise(0.02f, 0.5f, 3000, 400, 0.8f, 0.7f, 0.001f, 0.12f, F_LP);
    tone(0.0f, 0.3f, 180, 50, 0.6f, 0.001f, 0.1f, W_SIN, 0, 0);
    finish_fx(SFX_CRACK, 0.3f, 0.5f);

    /* --- level 6: a pinball machine --- */
    begin();                                                         /* pop bumper: a solenoid thwack and a bright ding */
    tone(0, 0.08f, 200, 80, 0.9f, 0.001f, 0.03f, W_SIN, 0, 0);
    noise(0, 0.03f, 2500, 1500, 1.2f, 0.6f, 0.001f, 0.01f, F_BP);
    bell(0.005f, 1568.0f, 0.4f, 0.07f);
    finish_fx(SFX_PB_BUMPER, 0.1f, 0.1f);

    begin();                                                         /* kicker: a heavier thunk, a lower ding */
    tone(0, 0.14f, 150, 55, 1.0f, 0.001f, 0.05f, W_SIN, 0, 0);
    noise(0, 0.06f, 1800, 600, 1.0f, 0.8f, 0.001f, 0.02f, F_BP);
    bell(0.01f, 1047.0f, 0.45f, 0.1f);
    finish_fx(SFX_PB_KICKER, 0.15f, 0.15f);

    begin();                                                         /* flipper: clack-clack of the coil */
    noise(0, 0.035f, 1600, 900, 1.6f, 1.0f, 0.001f, 0.012f, F_BP);
    tone(0, 0.07f, 140, 70, 0.7f, 0.001f, 0.025f, W_SIN, 0, 0);
    noise(0.07f, 0.03f, 2200, 1200, 1.6f, 0.6f, 0.001f, 0.01f, F_BP);
    bell(0.08f, 2093.0f, 0.25f, 0.06f);
    finish_fx(SFX_PB_FLIPPER, 0.1f, 0.1f);

    begin();                                                         /* combo: a fast rising chime run */
    for (int i = 0; i < 6; i++) {
        static const float nf[6] = {C6, E6, G6, C7, E6 * 2.0f, G6 * 2.0f};
        bell(0.05f * (float)i, nf[i], 0.35f, 0.07f);
    }
    finish_fx(SFX_PB_COMBO, 0.25f, 0.2f);

    begin();                                                         /* drop target: clack and a falling blip */
    noise(0, 0.04f, 1400, 800, 1.4f, 1.0f, 0.001f, 0.012f, F_BP);
    tone(0.02f, 0.18f, 1400, 300, 0.4f, 0.002f, 0.06f, W_TRI, 0, 0);
    finish_fx(SFX_PB_DROP, 0.1f, 0.1f);

    begin();                                                         /* buzzer */
    tone(0, 0.26f, 110, 108, 0.7f, 0.002f, 0, W_SOFT, 0.0f, 0);
    tone(0, 0.26f, 165, 163, 0.35f, 0.002f, 0, W_SOFT, 0.0f, 0);
    finish_fx(SFX_PB_BUZZ, 0.0f, 0.0f);

    begin();                                                         /* relay tick */
    noise(0, 0.012f, 4000, 3000, 1.5f, 1.0f, 0.0005f, 0.004f, F_BP);
    finish_fx(SFX_PB_TICK, 0.0f, 0.0f);

    begin();                                                         /* ding */
    bell(0, 2093.0f, 0.5f, 0.12f);
    bell(0.002f, 2637.0f, 0.2f, 0.08f);
    finish_fx(SFX_PB_DING, 0.2f, 0.2f);

    begin();                                                         /* plunger launch: spring boing, a whoosh up the lane, a ding */
    tone(0, 0.35f, 320, 120, 0.7f, 0.002f, 0.12f, W_FM, 1.5f, 3.0f);
    noise(0.08f, 0.35f, 400, 4000, 1.0f, 0.6f, 0.05f, 0.12f, F_BP);
    tone(0.08f, 0.35f, 200, 900, 0.25f, 0.02f, 0.15f, W_TRI, 0, 0);
    bell(0.42f, 1568.0f, 0.4f, 0.1f);
    bell(0.5f, 2093.0f, 0.4f, 0.14f);
    finish_fx(SFX_PB_LAUNCH, 0.25f, 0.3f);

    begin();                                                         /* jackpot: bells ringing up and the score tallying */
    for (int r = 0; r < 2; r++)
        for (int i = 0; i < 4; i++) {
            static const float nf[4] = {C6, E6, G6, C7};
            bell(0.3f * (float)r + 0.07f * (float)i, nf[i] * (r ? 1.0f : 0.5f) * 2.0f, 0.35f, 0.12f);
        }
    for (int i = 0; i < 14; i++) noise(0.62f + 0.04f * (float)i, 0.012f, 4000, 3000, 1.5f, 0.35f, 0.0005f, 0.004f, F_BP);
    bell(0.62f, C7, 0.5f, 0.35f);
    tone(0.62f, 0.8f, C5, C5, 0.25f, 0.01f, 0.35f, W_FM, 2.0f, 1.0f);
    tone(0.62f, 0.8f, G5, G5, 0.2f, 0.01f, 0.35f, W_FM, 2.0f, 1.0f);
    finish_fx(SFX_PB_JACKPOT, 0.3f, 0.4f);

    begin();                                                         /* TILT: a nasty double buzz and a clank */
    tone(0, 0.22f, 90, 88, 0.8f, 0.002f, 0, W_SOFT, 0.0f, 0);
    tone(0.3f, 0.3f, 90, 86, 0.8f, 0.002f, 0, W_SOFT, 0.0f, 0);
    noise(0, 0.08f, 1200, 400, 1.0f, 0.7f, 0.001f, 0.03f, F_BP);
    finish_fx(SFX_PB_TILT, 0.1f, 0.2f);

    begin();                                                         /* boss: an alarm klaxon */
    for (int i = 0; i < 3; i++) {
        tone(0.4f * (float)i, 0.2f, 620, 600, 0.6f, 0.01f, 0, W_SOFT, 0.3f, 0);
        tone(0.4f * (float)i + 0.2f, 0.2f, 470, 450, 0.6f, 0.01f, 0, W_SOFT, 0.3f, 0);
    }
    tone(0, 1.3f, 55, 50, 0.6f, 0.05f, 0.7f, W_SOFT, 0.5f, 0);
    finish_fx(SFX_PB_SCOTT, 0.2f, 0.4f);

    begin();                                                         /* win: a long run of bells over a fanfare */
    for (int i = 0; i < 12; i++) {
        static const float nf[4] = {C6, E6, G6, C7};
        bell(0.08f * (float)i, nf[i & 3] * (i >= 8 ? 2.0f : 1.0f), 0.3f, 0.12f);
    }
    for (int i = 0; i < 4; i++) {
        static const float nf[4] = {C5, E5, G5, C6};
        tone(0.96f, 1.1f, nf[i], nf[i], 0.22f, 0.01f, 0.5f, W_FM, 2.0f, 1.0f);
    }
    finish_fx(SFX_PB_WIN, 0.3f, 0.5f);

    begin();                                                         /* drain: wah... wah... wahhh */
    for (int i = 0; i < 3; i++) {
        float f0 = 392.0f * powf(0.89f, (float)i);
        tone(0.32f * (float)i, i == 2 ? 0.9f : 0.3f, f0, f0 * (i == 2 ? 0.7f : 0.95f), 0.5f, 0.01f, i == 2 ? 0.5f : 0.15f, W_FM, 1.0f, 2.5f);
    }
    finish_fx(SFX_PB_DRAIN, 0.2f, 0.4f);

    begin();                                                         /* time warp: a rising whoosh into a sonic boom */
    noise(0, 0.32f, 200, 5000, 1.5f, 0.6f, 0.2f, 0.1f, F_BP);
    tone(0, 0.32f, 90, 1600, 0.35f, 0.15f, 0.1f, W_FM, 2.0f, 2.0f);
    tone(0.3f, 0.35f, 120, 40, 0.9f, 0.001f, 0.1f, W_SIN, 0, 0);
    noise(0.3f, 0.3f, 2000, 200, 0.8f, 0.8f, 0.001f, 0.08f, F_LP);
    finish_fx(SFX_PB_WARP, 0.15f, 0.3f);

    begin();                                                         /* time surge: whoosh, zap, boom and bells */
    noise(0, 0.6f, 150, 6000, 1.2f, 0.6f, 0.35f, 0.2f, F_BP);
    tone(0, 0.6f, 60, 2000, 0.3f, 0.3f, 0.2f, W_FM, 1.5f, 3.0f);
    tone(0.55f, 0.8f, 110, 30, 1.0f, 0.001f, 0.25f, W_SIN, 0, 0);
    noise(0.55f, 0.7f, 2500, 150, 0.7f, 0.8f, 0.001f, 0.2f, F_LP);
    for (int i = 0; i < 5; i++) noise(0.55f + 0.04f * (float)i, 0.03f, 6000, 3000, 1.0f, 0.5f, 0.001f, 0.01f, F_HP);
    for (int i = 0; i < 6; i++) bell(0.65f + 0.06f * (float)i, (float[]){C6, E6, G6, C7, G6, C7}[i], 0.35f, 0.12f);
    finish_fx(SFX_PB_SURGE, 0.3f, 0.5f);

    begin();                                                         /* 1.21 gigawatts: crackle, a rising power-up chord, bells */
    for (int i = 0; i < 8; i++) noise(0.03f * (float)i, 0.03f, 7000, 3000, 1.0f, 0.7f, 0.001f, 0.01f, F_HP);
    tone(0, 0.3f, 2400, 900, 0.3f, 0.001f, 0.1f, W_FM, 1.5f, 6.0f);
    tone(0.2f, 0.9f, 110, 440, 0.35f, 0.05f, 0.4f, W_SOFT, 0.3f, 0);
    tone(0.2f, 0.9f, 165, 660, 0.25f, 0.05f, 0.4f, W_SOFT, 0.4f, 0);
    tone(0.2f, 0.9f, 220, 880, 0.2f, 0.05f, 0.4f, W_SOFT, 0.5f, 0);
    for (int i = 0; i < 4; i++) bell(0.9f + 0.07f * (float)i, (float[]){C6, E6, G6, C7}[i], 0.4f, 0.14f);
    finish_fx(SFX_PB_GIGA, 0.25f, 0.4f);

    /* --- the sea --- */
    begin();                                                         /* wind gust: a swelling, whistling howl */
    noise(0.0f, 2.4f, 400, 1400, 3.0f, 0.8f, 0.7f, 0.9f, F_BP);
    noise(0.2f, 2.1f, 900, 2600, 6.0f, 0.35f, 0.6f, 0.7f, F_BP);
    noise(0.0f, 2.4f, 250, 180, 0.7f, 0.5f, 0.5f, 1.0f, F_LP);
    finish_fx(SFX_GUST, 0.0f, 0.0f);

    begin();                                                         /* cannon broadside: three booms rolling down the deck */
    for (int i = 0; i < 3; i++) {
        float t0 = 0.13f * (float)i;
        tone(t0, 0.7f, 110, 32, 0.9f, 0.001f, 0.2f, W_SIN, 0, 0);
        noise(t0, 0.6f, 2600, 120, 0.7f, 0.8f, 0.001f, 0.14f, F_LP);
        noise(t0, 0.08f, 5000, 2000, 1.0f, 0.4f, 0.001f, 0.02f, F_BP);
    }
    finish_fx(SFX_CANNON, 0.35f, 0.7f);

    begin();                                                         /* splash: a slap and a wash of spray */
    tone(0, 0.12f, 260, 90, 0.6f, 0.001f, 0.04f, W_SIN, 0, 0);
    noise(0.0f, 0.6f, 3000, 900, 0.8f, 0.8f, 0.005f, 0.18f, F_LP);
    noise(0.03f, 0.4f, 6000, 2500, 1.2f, 0.35f, 0.01f, 0.1f, F_HP);
    for (int i = 0; i < 4; i++) tone(0.08f + 0.06f * (float)i, 0.08f, 600 + 300 * (float)i, 1400 + 200 * (float)i, 0.15f, 0.002f, 0.03f, W_SIN, 0, 0);
    finish_fx(SFX_SPLASH, 0.2f, 0.3f);
}

const int16_t *audio_data(int id) { return &SFXPOOL[sfx_off[id]]; }
int            audio_pool_used(void) { return pool_used; }
int            audio_len(int id)  { return sfx_len[id]; }
int            audio_volume(int id) { return VOL[id]; }
