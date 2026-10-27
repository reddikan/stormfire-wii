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
#include <malloc.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define MAXLEN 56000                       /* 2.5 s per effect, a multiple of 16 so each stays 32-byte aligned */

/* every effect lives back to back in one pool (each start 32-byte aligned), sized to what they really need */
#define SFX_POOL 2600000
static int16_t *SFXPOOL;                 /* allocated (32-byte aligned for the DSP) when the effects are made */
static int     sfx_off[SFX_COUNT], sfx_len[SFX_COUNT], pool_used, made;
void (*audio_progress)(int done, int total);
static float   mixbuf[MAXLEN];
static float   outbuf[MAXLEN];
static int     mixend;
static uint32_t rng = 0x2545F491u;
static int     ready;

/* playback volume per effect (every effect is normalised first, then scaled by this to balance loudness) */
static uint8_t VOL[SFX_COUNT] = {
    /* BLASTER */ 60,  /* MORTAR */ 130, /* BOOM */ 190,   /* FROST */ 110,  /* ZAP */ 120,    /* HIT */ 45,
    /* POP */ 120,     /* CRUNCH */ 190, /* BUILD */ 190,  /* UPGRADE */ 200, /* SELL */ 170,   /* ERROR */ 140,
    /* MOVE */ 60,     /* SELECT */ 120, /* WAVE */ 200,   /* CLEAR */ 200,  /* HISS */ 200,   /* THUNDER */ 255,
    /* CRACK */ 230,   /* BOSS */ 240,   /* WIN */ 220,    /* LOSE */ 220,   /* PAUSE */ 120,  /* START */ 200,
    /* COIN */ 170,    /* GUST */ 150,   /* CANNON */ 220, /* SPLASH */ 160,
    /* pinball: BUMPER */ 130, /* KICKER */ 170, /* FLIPPER */ 170, /* COMBO */ 190, /* DROP */ 170, /* BUZZ */ 150,
    /* TICK */ 70, /* DING */ 150, /* LAUNCH */ 210, /* JACKPOT */ 220, /* TILT */ 210, /* SCOTT */ 210, /* WIN */ 230,
    /* DRAIN */ 220, /* WARP */ 150, /* SURGE */ 240, /* GIGA */ 240, /* PERFECT */ 230
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
    while (last > 1 && fabsf(outbuf[last - 1]) < peak * 0.012f) last--;
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
    if (audio_progress) audio_progress(++made, SFX_COUNT);
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



/* ------------------------------------------------------------------ */
/* themed sets (levels 2, 3, 4, 7)                                      */
/* ------------------------------------------------------------------ */
static const uint8_t TH_VOL[TH_SLOTS] = { 120, 190, 190, 200, 170, 210, 215, 240, 210, 225, 220, 160, 110, 60, 130, 185, 110, 120 };

static void marimba(float t, float f, float vol, float tau)
{
    tone(t, tau * 4.0f, f, f, vol, 0.001f, tau, W_FM, 4.0f, 1.4f);
    tone(t, tau * 3.0f, f, f, vol * 0.6f, 0.001f, tau * 1.4f, W_SIN, 0, 0);
}
static void woodblock(float t, float f, float vol)
{
    tone(t, 0.06f, f, f * 0.9f, vol, 0.0005f, 0.018f, W_FM, 3.5f, 2.0f);
    noise(t, 0.02f, f * 2.0f, f * 2.0f, 2.0f, vol * 0.3f, 0.0005f, 0.006f, F_BP);
}
static void chirp(float t, float f0, float f1, float vol)
{
    tone(t, 0.07f, f0, f1, vol, 0.004f, 0.03f, W_SIN, 0, 0);
}
static void organ_chord(float t, float dur, const float *f, int n, float vol)
{
    for (int i = 0; i < n; i++) tone(t, dur, f[i], f[i], vol, 0.03f, dur * 0.6f, W_SOFT, 1.0f, 0);
}
static void wail(float t, float dur, float f0, float f1, float vol)       /* ghostly sine glide with a wobble */
{
    tone(t, dur, f0, f1, vol, dur * 0.25f, dur * 0.5f, W_FM, 5.5f / f0, f0 / 400.0f);
}
static void clank(float t, float f, float vol)                           /* metal */
{
    tone(t, 0.4f, f, f, vol, 0.001f, 0.12f, W_FM, 2.76f, 3.0f);
    noise(t, 0.05f, 4000, 2500, 3.0f, vol * 0.5f, 0.001f, 0.015f, F_BP);
}
static void bones_click(float t, float f, float vol)
{
    tone(t, 0.12f, f, f, vol, 0.0005f, 0.03f, W_FM, 3.0f, 2.0f);
}
static void tick(float t, float vol) { noise(t, 0.012f, 5000, 4000, 2.5f, vol, 0.0005f, 0.004f, F_BP); }
static void twinkle(float t, float f, float vol) { bell(t, f, vol, 0.12f); bell(t + 0.04f, f * 1.5f, vol * 0.6f, 0.09f); }
static void gliss(float t, float f0, int n, float step, float dt, float vol)   /* harp-ish run */
{
    for (int i = 0; i < n; i++) bell(t + dt * (float)i, f0 * powf(2.0f, step * (float)i / 12.0f), vol, 0.15f);
}

static void theme_set(int base, int th)       /* th: 0 autumn, 1 hallow, 2 island, 3 sky */
{
    static const float ch_maj[3] = { C5, E5, G5 }, ch_min[3] = { 440.0f, C5, E5 }, low_min[3] = { 110.0f, 130.8f, 164.8f };
    for (int slot = 0; slot < TH_SLOTS; slot++) {
        begin();
        float wet = 0.2f, room = 0.3f;
        switch (th * 100 + slot) {
        /* ---- autumn: wooden, folksy, birds and leaves ---- */
        case TH_POP: woodblock(0, 880, 0.8f); noise(0.01f, 0.12f, 3500, 2500, 0.8f, 0.3f, 0.01f, 0.04f, F_BP); break;
        case TH_CRUNCH: for (int i = 0; i < 6; i++) noise(0.03f * (float)i, 0.03f, 2500 - 200 * (float)i, 1800, 2.0f, 0.8f, 0.001f, 0.01f, F_BP);
                        tone(0, 0.25f, 120, 60, 0.8f, 0.001f, 0.08f, W_SIN, 0, 0); break;
        case TH_BUILD: woodblock(0, 400, 0.9f); woodblock(0.1f, 420, 0.9f); marimba(0.2f, C5, 0.4f, 0.12f); marimba(0.28f, G5, 0.4f, 0.14f); break;
        case TH_UPGRADE: for (int i = 0; i < 4; i++) marimba(0.07f * (float)i, (float[]){C5, E5, G5, C6}[i], 0.45f, 0.12f); break;
        case TH_SELL: for (int i = 0; i < 3; i++) marimba(0.06f * (float)i, (float[]){G5, E5, C5}[i], 0.45f, 0.08f); break;
        case TH_WAVE: tone(0, 0.5f, 220, 233, 0.6f, 0.04f, 0.35f, W_SOFT, 1.2f, 0); tone(0.4f, 0.7f, 330, 330, 0.6f, 0.04f, 0.45f, W_SOFT, 1.2f, 0);
                      noise(0, 0.9f, 3000, 2000, 0.6f, 0.15f, 0.2f, 0.3f, F_BP); break;
        case TH_CLEAR: for (int i = 0; i < 6; i++) marimba(0.05f * (float)i, (float[]){C5, E5, G5, C6, E5 * 2, G5 * 2}[i], 0.4f, 0.2f);
                       chirp(0.4f, 2400, 3200, 0.3f); chirp(0.5f, 2600, 3400, 0.3f); break;
        case TH_BOSS: tone(0, 1.5f, 73, 65, 0.8f, 0.1f, 0.8f, W_SOFT, 0.8f, 0); for (int i = 0; i < 4; i++) woodblock(0.3f * (float)i, 150, 0.9f); break;
        case TH_LEAK: noise(0, 0.6f, 5000, 2000, 0.6f, 0.5f, 0.01f, 0.25f, F_HP); woodblock(0, 200, 0.7f); break;
        case TH_WIN: for (int i = 0; i < 8; i++) marimba(0.11f * (float)i, (float[]){C5, D5, E5, G5, E5, G5, C6, C6}[i], 0.45f, 0.2f); break;
        case TH_LOSE: for (int i = 0; i < 4; i++) marimba(0.22f * (float)i, (float[]){E5, D5, C5, 440.0f}[i], 0.45f, 0.35f); break;
        case TH_COIN: chirp(0, 2600, 3600, 0.5f); chirp(0.08f, 3000, 4000, 0.4f); break;
        case TH_SELECT: woodblock(0, 1400, 0.6f); break;
        case TH_BLASTER: noise(0, 0.08f, 1500, 6000, 1.5f, 0.4f, 0.001f, 0.03f, F_BP); tone(0, 0.08f, 1300, 600, 0.3f, 0.001f, 0.03f, W_TRI, 0, 0); break;
        case TH_MORTAR: tone(0, 0.2f, 170, 75, 1.0f, 0.001f, 0.07f, W_FM, 2.0f, 1.5f); woodblock(0, 300, 0.5f); break;
        case TH_BOOM: tone(0, 0.5f, 130, 40, 1.0f, 0.001f, 0.15f, W_SIN, 0, 0); noise(0.02f, 0.6f, 3000, 1500, 0.6f, 0.6f, 0.02f, 0.2f, F_BP); break;
        case TH_FROST: for (int i = 0; i < 5; i++) bell(0.05f * (float)i, 2000.0f + 350.0f * (float)((i * 3) % 5), 0.25f, 0.15f); break;
        case TH_ZAP: for (int i = 0; i < 7; i++) noise(0.025f * (float)i, 0.02f, 4000, 2500, 1.5f, 0.6f, 0.001f, 0.008f, F_BP); tone(0, 0.15f, 600, 300, 0.3f, 0.001f, 0.05f, W_FM, 1.0f, 3.0f); break;
        /* ---- hallow: spooky organ, bones, wails, bells, chains ---- */
        case 100 + TH_POP: noise(0, 0.15f, 600, 2500, 0.8f, 0.6f, 0.01f, 0.05f, F_BP); bones_click(0.02f, 1200, 0.5f); break;
        case 100 + TH_CRUNCH: for (int i = 0; i < 5; i++) bones_click(0.05f * (float)i, 1500.0f - 150.0f * (float)i, 0.6f); tone(0, 0.3f, 100, 50, 0.8f, 0.001f, 0.1f, W_SIN, 0, 0); break;
        case 100 + TH_BUILD: tone(0, 0.35f, 300, 420, 0.25f, 0.05f, 0.2f, W_FM, 0.5f, 4.0f); organ_chord(0.25f, 0.5f, ch_min, 3, 0.25f); break;
        case 100 + TH_UPGRADE: for (int i = 0; i < 4; i++) tone(0.08f * (float)i, 0.35f, (float[]){440.0f, C5, E5, 880.0f}[i], (float[]){440.0f, C5, E5, 880.0f}[i], 0.3f, 0.01f, 0.2f, W_SOFT, 1.0f, 0); break;
        case 100 + TH_SELL: for (int i = 0; i < 6; i++) noise(0.04f * (float)i, 0.04f, 4500, 3500, 4.0f, 0.6f, 0.001f, 0.015f, F_BP); break;
        case 100 + TH_WAVE: bell(0, 196.0f, 0.8f, 0.8f); bell(0, 293.7f, 0.3f, 0.6f); wet = 0.5f; room = 0.8f; break;
        case 100 + TH_CLEAR: organ_chord(0, 0.5f, ch_min, 3, 0.28f); organ_chord(0.45f, 0.8f, ch_maj, 3, 0.28f); bell(0.45f, C6 * 2.0f, 0.2f, 0.3f); break;
        case 100 + TH_BOSS: organ_chord(0, 1.6f, low_min, 3, 0.4f); noise(0, 1.6f, 300, 60, 0.7f, 0.5f, 0.2f, 0.8f, F_LP); break;
        case 100 + TH_LEAK: wail(0, 0.9f, 400, 700, 0.5f); wail(0.3f, 0.7f, 700, 300, 0.4f); break;
        case 100 + TH_WIN: for (int i = 0; i < 7; i++) bell(0.12f * (float)i, (float[]){E5, G5, B5, E6, B5, E6, G6}[i], 0.35f, 0.25f); organ_chord(0.84f, 0.9f, ch_maj, 3, 0.25f); break;
        case 100 + TH_LOSE: wail(0, 1.6f, 600, 150, 0.6f); break;
        case 100 + TH_COIN: bell(0, 1760.0f, 0.4f, 0.15f); bell(0.06f, 2349.0f, 0.3f, 0.2f); break;
        case 100 + TH_SELECT: bones_click(0, 1800, 0.6f); break;
        case 100 + TH_BLASTER: wail(0, 0.12f, 1000, 550, 0.4f); noise(0, 0.05f, 2000, 1000, 1.5f, 0.2f, 0.001f, 0.02f, F_BP); break;
        case 100 + TH_MORTAR: tone(0, 0.2f, 140, 90, 0.9f, 0.001f, 0.06f, W_FM, 1.5f, 2.0f); noise(0, 0.08f, 800, 500, 2.0f, 0.5f, 0.001f, 0.03f, F_BP); break;
        case 100 + TH_BOOM: tone(0, 0.6f, 110, 35, 1.0f, 0.001f, 0.18f, W_SIN, 0, 0); noise(0.05f, 0.6f, 400, 2000, 0.8f, 0.4f, 0.1f, 0.2f, F_BP); wet = 0.4f; break;
        case 100 + TH_FROST: noise(0, 0.4f, 7000, 4000, 0.8f, 0.4f, 0.05f, 0.15f, F_HP); tone(0, 0.4f, 3000, 2200, 0.15f, 0.01f, 0.15f, W_FM, 3.0f, 1.0f); break;
        case 100 + TH_ZAP: tone(0, 0.2f, 1800, 600, 0.3f, 0.001f, 0.07f, W_FM, 1.3f, 5.0f); wail(0.02f, 0.18f, 1500, 2200, 0.2f); for (int i = 0; i < 4; i++) noise(0.04f * (float)i, 0.025f, 6000, 3000, 1.0f, 0.4f, 0.001f, 0.008f, F_HP); break;
        /* ---- island: clockwork, brass, steam, music box ---- */
        case 200 + TH_POP: clank(0, 1900, 0.4f); tick(0.01f, 0.5f); break;
        case 200 + TH_CRUNCH: for (int i = 0; i < 5; i++) clank(0.06f * (float)i, 900.0f + 230.0f * (float)((i * 2) % 5), 0.5f); break;
        case 200 + TH_BUILD: for (int i = 0; i < 5; i++) tick(0.04f * (float)i, 0.7f); clank(0.22f, 1320, 0.5f); break;
        case 200 + TH_UPGRADE: for (int i = 0; i < 6; i++) bell(0.05f * (float)i, (float[]){C6, E6, G6, C7, G6, C7}[i], 0.3f, 0.1f); break;
        case 200 + TH_SELL: clank(0, 2600, 0.35f); clank(0.07f, 2300, 0.35f); clank(0.13f, 2000, 0.35f); break;
        case 200 + TH_WAVE: tone(0, 1.0f, 147, 147, 0.6f, 0.06f, 0.6f, W_SOFT, 0.2f, 0); tone(0, 1.0f, 220, 220, 0.4f, 0.06f, 0.6f, W_SOFT, 0.3f, 0);
                            noise(0, 0.8f, 3000, 2500, 0.8f, 0.2f, 0.05f, 0.4f, F_BP); break;
        case 200 + TH_CLEAR: for (int i = 0; i < 4; i++) bell(0.12f * (float)i, (float[]){G5, B5 / 1.0f, D5 * 2.0f, G5 * 2.0f}[i], 0.4f, 0.4f); break;
        case 200 + TH_BOSS: tone(0, 1.8f, 65, 62, 0.8f, 0.01f, 0.9f, W_FM, 1.41f, 2.5f); noise(0.1f, 1.2f, 6000, 2500, 0.7f, 0.4f, 0.05f, 0.5f, F_HP); break;
        case 200 + TH_LEAK: noise(0, 0.7f, 6000, 3000, 0.7f, 0.6f, 0.01f, 0.3f, F_HP); clank(0, 300, 0.6f); break;
        case 200 + TH_WIN: for (int i = 0; i < 9; i++) bell(0.1f * (float)i, (float[]){C6, E6, G6, E6, C6, G5, C6, E6, C7}[i], 0.3f, 0.2f); break;
        case 200 + TH_LOSE: for (int i = 0; i < 6; i++) bell(0.12f * (float)i + 0.02f * (float)(i * i), (float[]){C6, B5, G5, E5, D5, C5}[i], 0.3f, 0.25f); break;
        case 200 + TH_COIN: bell(0, 3136.0f, 0.4f, 0.1f); break;
        case 200 + TH_SELECT: tick(0, 0.8f); tick(0.05f, 0.5f); break;
        case 200 + TH_BLASTER: noise(0, 0.05f, 4000, 1500, 1.0f, 0.4f, 0.001f, 0.02f, F_BP); clank(0.01f, 2400, 0.15f); break;
        case 200 + TH_MORTAR: tone(0, 0.22f, 160, 70, 0.9f, 0.001f, 0.07f, W_SIN, 0, 0); noise(0, 0.3f, 5000, 2000, 0.8f, 0.5f, 0.01f, 0.1f, F_HP); break;
        case 200 + TH_BOOM: tone(0, 0.5f, 140, 40, 1.0f, 0.001f, 0.15f, W_SIN, 0, 0); clank(0.02f, 700, 0.5f); noise(0, 0.5f, 2500, 300, 0.7f, 0.6f, 0.001f, 0.15f, F_LP); break;
        case 200 + TH_FROST: for (int i = 0; i < 4; i++) tone(0.04f * (float)i, 0.5f, 2200.0f + 470.0f * (float)i, 2200.0f + 470.0f * (float)i, 0.2f, 0.001f, 0.2f, W_FM, 2.76f, 1.0f); break;
        case 200 + TH_ZAP: tone(0, 0.25f, 300, 1200, 0.25f, 0.01f, 0.1f, W_SOFT, 0.0f, 0); for (int i = 0; i < 5; i++) noise(0.03f * (float)i, 0.02f, 7000, 4000, 1.0f, 0.5f, 0.001f, 0.008f, F_HP); break;
        /* ---- sky: sparkles, harps, bubbles, wands and fanfares ---- */
        case 300 + TH_POP: tone(0, 0.08f, 400, 1400, 0.6f, 0.001f, 0.03f, W_SIN, 0, 0); twinkle(0.03f, 2637, 0.25f); break;
        case 300 + TH_CRUNCH: for (int i = 0; i < 6; i++) twinkle(0.03f * (float)i, 1800.0f + 300.0f * (float)((i * 3) % 6), 0.2f); tone(0, 0.2f, 200, 90, 0.6f, 0.001f, 0.06f, W_SIN, 0, 0); break;
        case 300 + TH_BUILD: noise(0, 0.2f, 1000, 8000, 1.5f, 0.5f, 0.05f, 0.06f, F_BP); twinkle(0.18f, 2093, 0.35f); break;
        case 300 + TH_UPGRADE: gliss(0, C5, 8, 2.0f, 0.035f, 0.3f); break;
        case 300 + TH_SELL: gliss(0, C6 * 2.0f, 6, -2.0f, 0.035f, 0.25f); break;
        case 300 + TH_WAVE: tone(0, 0.3f, 523, 523, 0.45f, 0.02f, 0.2f, W_SOFT, 0.6f, 0); tone(0.25f, 0.6f, 784, 784, 0.45f, 0.02f, 0.35f, W_SOFT, 0.6f, 0);
                            gliss(0.2f, C5, 8, 2.0f, 0.03f, 0.2f); break;
        case 300 + TH_CLEAR: for (int r = 0; r < 3; r++) for (int i = 0; i < 4; i++) bell(0.25f * (float)r + 0.06f * (float)i, (float[]){C6, E6, G6, C7}[i], 0.3f, 0.15f);
                             gliss(0.75f, C5, 10, 2.0f, 0.03f, 0.2f); break;
        case 300 + TH_BOSS: noise(0, 1.2f, 500, 150, 2.0f, 0.8f, 0.1f, 0.5f, F_LP); tone(0, 1.2f, 90, 60, 0.6f, 0.08f, 0.5f, W_FM, 0.5f, 6.0f);
                            for (int i = 0; i < 4; i++) bell(0.3f * (float)i, i & 1 ? 1760.0f : 2093.0f, 0.3f, 0.12f); break;
        case 300 + TH_LEAK: tone(0, 0.6f, 1500, 300, 0.5f, 0.01f, 0.3f, W_SIN, 0, 0); break;
        case 300 + TH_WIN: for (int i = 0; i < 4; i++) tone(0.15f * (float)i, 0.5f, (float[]){C5, E5, G5, C6}[i], (float[]){C5, E5, G5, C6}[i], 0.35f, 0.01f, 0.3f, W_SOFT, 0.6f, 0);
                           gliss(0.6f, C5, 14, 2.0f, 0.03f, 0.22f); break;
        case 300 + TH_LOSE: gliss(0, C6, 8, -1.5f, 0.09f, 0.3f); break;
        case 300 + TH_COIN: twinkle(0, 3136, 0.4f); break;
        case 300 + TH_SELECT: tone(0, 0.06f, 600, 1200, 0.5f, 0.001f, 0.02f, W_SIN, 0, 0); break;
        case 300 + TH_BLASTER: tone(0, 0.09f, 2500, 900, 0.4f, 0.001f, 0.03f, W_FM, 2.0f, 2.0f); break;
        case 300 + TH_MORTAR: tone(0, 0.3f, 300, 120, 0.7f, 0.002f, 0.1f, W_FM, 1.5f, 3.0f); break;
        case 300 + TH_BOOM: tone(0, 0.3f, 160, 60, 0.8f, 0.001f, 0.08f, W_SIN, 0, 0); for (int i = 0; i < 6; i++) noise(0.05f + 0.04f * (float)i, 0.02f, 6000, 3000, 1.0f, 0.5f, 0.001f, 0.008f, F_HP);
                           twinkle(0.05f, 2349, 0.2f); break;
        case 300 + TH_FROST: for (int i = 0; i < 5; i++) bell(0.04f * (float)i, 2600.0f + 400.0f * (float)((i * 2) % 5), 0.22f, 0.12f); break;
        case 300 + TH_ZAP: tone(0, 0.15f, 1200, 2600, 0.3f, 0.001f, 0.06f, W_FM, 1.5f, 2.0f); twinkle(0.05f, 3136, 0.2f); break;
        default: break;
        }
        finish_fx(base + slot, wet, room);
        VOL[base + slot] = TH_VOL[slot];
    }
}

static void magic_extras(void)
{
    begin(); gliss(0, C5, 12, 2.0f, 0.03f, 0.3f); for (int i = 0; i < 3; i++) bell(0.36f + 0.08f * (float)i, C7, 0.3f, 0.1f); finish_fx(SFX_MG_COMBO, 0.25f, 0.3f);
    begin();                                                         /* wishing star: a falling whoosh, a shimmer, an "aah" chord */
    noise(0, 0.6f, 6000, 800, 1.5f, 0.6f, 0.05f, 0.3f, F_BP); tone(0, 0.6f, 2400, 600, 0.25f, 0.01f, 0.3f, W_SIN, 0, 0);
    for (int i = 0; i < 3; i++) tone(0.55f, 1.2f, (float[]){C5, E5, G5}[i], (float[]){C5, E5, G5}[i], 0.25f, 0.15f, 0.6f, W_SOFT, 1.0f, 0);
    gliss(0.55f, C6, 10, 2.0f, 0.04f, 0.22f);
    finish_fx(SFX_MG_SURGE, 0.35f, 0.6f);
    begin();                                                         /* rainbow blast: a magical swirl rising into chimes */
    tone(0, 0.8f, 300, 2400, 0.3f, 0.05f, 0.4f, W_FM, 1.01f, 3.0f); gliss(0.1f, C5, 16, 2.0f, 0.03f, 0.25f);
    for (int i = 0; i < 4; i++) bell(0.7f + 0.07f * (float)i, (float[]){C6, E6, G6, C7}[i], 0.35f, 0.14f);
    finish_fx(SFX_MG_BLAST, 0.3f, 0.5f);
    begin(); noise(0, 0.25f, 2000, 9000, 1.2f, 0.4f, 0.01f, 0.08f, F_BP); twinkle(0.05f, 2637, 0.3f); twinkle(0.12f, 3520, 0.2f); finish_fx(SFX_MG_WARP, 0.2f, 0.3f);
    begin();                                                         /* firework: whistle up, bang, crackle */
    tone(0, 0.4f, 800, 2500, 0.25f, 0.02f, 0.2f, W_SIN, 0, 0);
    tone(0.4f, 0.4f, 120, 40, 0.9f, 0.001f, 0.12f, W_SIN, 0, 0); noise(0.4f, 0.5f, 3000, 300, 0.8f, 0.7f, 0.001f, 0.12f, F_LP);
    for (int i = 0; i < 10; i++) noise(0.5f + 0.035f * (float)i, 0.015f, 7000, 4000, 1.0f, 0.4f, 0.001f, 0.006f, F_HP);
    finish_fx(SFX_MG_FIREWORK, 0.25f, 0.5f);
    begin();                                                         /* perfect wave: the alarm bells go off */
    for (int i = 0; i < 16; i++) bell(0.06f * (float)i, i & 1 ? 2093.0f : 2637.0f, 0.35f, 0.07f);
    for (int i = 0; i < 3; i++) tone(0.96f, 0.8f, (float[]){C5, E5, G5}[i] * 2.0f, (float[]){C5, E5, G5}[i] * 2.0f, 0.2f, 0.01f, 0.4f, W_FM, 2.0f, 1.0f);
    finish_fx(SFX_MG_PERFECT, 0.3f, 0.5f);
    VOL[SFX_MG_COMBO] = 200; VOL[SFX_MG_SURGE] = 240; VOL[SFX_MG_BLAST] = 240; VOL[SFX_MG_WARP] = 130; VOL[SFX_MG_FIREWORK] = 200; VOL[SFX_MG_PERFECT] = 230;
}

void audio_init(void)
{
    if (ready) return;
    ready = 1;
    SFXPOOL = (int16_t *)memalign(32, sizeof(int16_t) * SFX_POOL);
    if (!SFXPOOL) return;
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

    begin();                                                         /* perfect wave: the knocker (a free game!) and bells */
    tone(0, 0.2f, 90, 60, 1.0f, 0.001f, 0.06f, W_SIN, 0, 0); noise(0, 0.05f, 1500, 800, 1.5f, 1.0f, 0.001f, 0.015f, F_BP);
    tone(0.35f, 0.2f, 90, 60, 1.0f, 0.001f, 0.06f, W_SIN, 0, 0); noise(0.35f, 0.05f, 1500, 800, 1.5f, 1.0f, 0.001f, 0.015f, F_BP);
    for (int i = 0; i < 12; i++) bell(0.6f + 0.05f * (float)i, i & 1 ? 1568.0f : 2093.0f, 0.3f, 0.06f);
    finish_fx(SFX_PB_PERFECT, 0.2f, 0.3f);

    /* --- each level's own set of everyday sounds --- */
    theme_set(SFX_TH_AUTUMN, 0);
    theme_set(SFX_TH_HALLOW, 1);
    theme_set(SFX_TH_ISLAND, 2);
    theme_set(SFX_TH_SKY, 3);
    magic_extras();

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

const int16_t *audio_data(int id) { static const int16_t silence[16] __attribute__((aligned(32))) = {0}; return SFXPOOL ? &SFXPOOL[sfx_off[id]] : silence; }
int            audio_pool_used(void) { return pool_used; }
int            audio_len(int id)  { return sfx_len[id]; }
int            audio_volume(int id) { return VOL[id]; }
