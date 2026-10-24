/*
 * STORMFIRE - a tiny formant speech synthesizer for the pirates' voices, in the spirit of the speech chips in
 * old arcade cabinets (a buzzing glottal source through a cascade of formant resonators, hissing noise for the
 * fricatives, and a crunchy low sample rate at the end). See speech.h.
 *
 * A phrase is a string of phonemes (ARPAbet-like), e.g. "AA^ RR _ M EY^ T IY":
 *   ^ after a phoneme  stressed: longer, and the pitch rises on it
 *   _                  a short pause
 * Stops (P B T D K G) are expanded into closure + burst (+ aspiration for the voiceless ones).
 */
#include "speech.h"

#include <math.h>
#include <string.h>

#define PI_F 3.14159265f

enum { K_VOW, K_SON, K_NAS, K_FRIC, K_VFRIC, K_ASP, K_SIL, K_BURST, K_BAR, K_GROWL };

typedef struct {
    const char *name;
    uint8_t kind;
    float f1, f2, f3;          /* formant targets (Hz) */
    float g1, g2, g3;          /* end targets for diphthongs (0 = same) */
    float av, af, fc, fbw;     /* voicing amp, frication amp, frication centre / bandwidth */
    float dur;                 /* ms */
} Ph;

static const Ph PH[] = {
    /* vowels */
    { "AA", K_VOW, 730, 1090, 2440, 0, 0, 0,        1.0f, 0, 0, 0, 150 },
    { "AE", K_VOW, 660, 1720, 2410, 0, 0, 0,        1.0f, 0, 0, 0, 150 },
    { "AH", K_VOW, 640, 1190, 2390, 0, 0, 0,        1.0f, 0, 0, 0, 120 },
    { "AO", K_VOW, 570, 840, 2410, 0, 0, 0,         1.0f, 0, 0, 0, 150 },
    { "EH", K_VOW, 530, 1840, 2480, 0, 0, 0,        1.0f, 0, 0, 0, 120 },
    { "ER", K_VOW, 490, 1350, 1690, 0, 0, 0,        0.95f, 0, 0, 0, 140 },
    { "IH", K_VOW, 390, 1990, 2550, 0, 0, 0,        0.95f, 0, 0, 0, 100 },
    { "IY", K_VOW, 280, 2250, 2950, 0, 0, 0,        0.9f, 0, 0, 0, 150 },
    { "UH", K_VOW, 440, 1020, 2240, 0, 0, 0,        0.95f, 0, 0, 0, 110 },
    { "UW", K_VOW, 310, 870, 2240, 0, 0, 0,         0.9f, 0, 0, 0, 150 },
    { "AX", K_VOW, 500, 1450, 2450, 0, 0, 0,        0.85f, 0, 0, 0, 70 },
    { "EY", K_VOW, 480, 1760, 2500, 330, 2200, 2850, 1.0f, 0, 0, 0, 180 },
    { "AY", K_VOW, 700, 1200, 2500, 330, 2100, 2800, 1.0f, 0, 0, 0, 200 },
    { "OW", K_VOW, 560, 920, 2400, 340, 780, 2300,  1.0f, 0, 0, 0, 200 },
    /* sonorants */
    { "R",  K_SON, 420, 1250, 1600, 0, 0, 0,        0.75f, 0, 0, 0, 80 },
    { "RR", K_GROWL, 470, 1200, 1550, 0, 0, 0,      0.9f, 0, 0, 0, 420 },
    { "L",  K_SON, 360, 950, 2600, 0, 0, 0,         0.7f, 0, 0, 0, 80 },
    { "W",  K_SON, 300, 650, 2200, 0, 0, 0,         0.7f, 0, 0, 0, 80 },
    { "Y",  K_SON, 260, 2100, 3000, 0, 0, 0,        0.7f, 0, 0, 0, 80 },
    { "M",  K_NAS, 270, 1100, 2200, 0, 0, 0,        0.55f, 0, 0, 0, 90 },
    { "N",  K_NAS, 270, 1500, 2500, 0, 0, 0,        0.55f, 0, 0, 0, 80 },
    { "NG", K_NAS, 270, 2000, 2700, 0, 0, 0,        0.55f, 0, 0, 0, 90 },
    /* fricatives */
    { "S",  K_FRIC, 400, 1600, 2600, 0, 0, 0,       0, 0.75f, 6200, 2200, 130 },
    { "SH", K_FRIC, 400, 1800, 2500, 0, 0, 0,       0, 0.8f, 2900, 1400, 140 },
    { "F",  K_FRIC, 400, 1200, 2400, 0, 0, 0,       0, 0.3f, 5000, 5000, 110 },
    { "TH", K_FRIC, 400, 1400, 2600, 0, 0, 0,       0, 0.25f, 5500, 5000, 100 },
    { "V",  K_VFRIC, 300, 1100, 2300, 0, 0, 0,      0.5f, 0.22f, 4000, 4000, 80 },
    { "DH", K_VFRIC, 300, 1400, 2600, 0, 0, 0,      0.5f, 0.15f, 4500, 4000, 60 },
    { "Z",  K_VFRIC, 300, 1600, 2600, 0, 0, 0,      0.45f, 0.45f, 6000, 2400, 110 },
    { "HH", K_ASP, 0, 0, 0, 0, 0, 0,                0, 0, 0, 0, 70 },
    /* pieces of stops (made by the parser) */
    { "_",  K_SIL, 0, 0, 0, 0, 0, 0,                0, 0, 0, 0, 110 },
    { "#",  K_SIL, 0, 0, 0, 0, 0, 0,                0, 0, 0, 0, 55 },       /* closure */
    { "%",  K_BAR, 250, 1000, 2400, 0, 0, 0,        0.22f, 0, 0, 0, 45 },   /* voiced closure */
    { "pB", K_BURST, 0, 0, 0, 0, 0, 0,              0, 0.5f, 1100, 2500, 14 },
    { "tB", K_BURST, 0, 0, 0, 0, 0, 0,              0, 0.75f, 4500, 2500, 16 },
    { "kB", K_BURST, 0, 0, 0, 0, 0, 0,              0, 0.65f, 2100, 1300, 20 },
    { "hA", K_ASP, 0, 0, 0, 0, 0, 0,                0, 0, 0, 0, 40 },
};
#define NPH ((int)(sizeof PH / sizeof PH[0]))

static int find(const char *s, int n)
{
    for (int i = 0; i < NPH; i++)
        if ((int)strlen(PH[i].name) == n && !strncmp(PH[i].name, s, (size_t)n)) return i;
    return -1;
}

typedef struct { int p; float t0, t1; uint8_t stress; } Seg;
#define MAXSEG 96

static int parse(const char *s, Seg *seg, float ms_scale)
{
    int n = 0;
    float t = 0.03f;
    while (*s && n < MAXSEG - 4) {
        while (*s == ' ') s++;
        if (!*s) break;
        const char *a = s;
        while (*s && *s != ' ' && *s != '^') s++;
        int len = (int)(s - a), stress = 0;
        if (*s == '^') { stress = 1; s++; }
        int ids[3], ni = 0;
        char c0 = a[0];
        if (len == 1 && (c0 == 'P' || c0 == 'T' || c0 == 'K')) {
            ids[ni++] = find("#", 1);
            ids[ni++] = find(c0 == 'P' ? "pB" : c0 == 'T' ? "tB" : "kB", 2);
            ids[ni++] = find("hA", 2);
        } else if (len == 1 && (c0 == 'B' || c0 == 'D' || c0 == 'G')) {
            ids[ni++] = find("%", 1);
            ids[ni++] = find(c0 == 'B' ? "pB" : c0 == 'D' ? "tB" : "kB", 2);
        } else {
            int p = find(a, len);
            if (p < 0) continue;
            ids[ni++] = p;
        }
        for (int k = 0; k < ni; k++) {
            const Ph *ph = &PH[ids[k]];
            float d = ph->dur * ms_scale * (stress && ph->kind == K_VOW ? 1.35f : 1.0f) * 0.001f;
            if (ph->kind == K_BURST && k > 0 && PH[ids[0]].kind == K_BAR) d *= 0.7f;   /* voiced stops: a softer, shorter burst */
            seg[n].p = ids[k]; seg[n].t0 = t; seg[n].t1 = t + d; seg[n].stress = (uint8_t)stress;
            t += d; n++;
        }
    }
    return n;
}

/* the formants a segment "wants" at fraction u through it (aspiration takes the next voiced sound's shape) */
static void targets(const Seg *seg, int n, int i, float u, float f[3])
{
    const Ph *ph = &PH[seg[i].p];
    if (ph->kind == K_ASP || ph->kind == K_SIL || ph->kind == K_BURST) {
        for (int j = i + 1; j < n; j++) {
            const Ph *q = &PH[seg[j].p];
            if (q->f1 > 0 && q->kind != K_BAR) { f[0] = q->f1; f[1] = q->f2; f[2] = q->f3; return; }
        }
        for (int j = i - 1; j >= 0; j--) {
            const Ph *q = &PH[seg[j].p];
            if (q->f1 > 0) { f[0] = q->f1; f[1] = q->f2; f[2] = q->f3; return; }
        }
        f[0] = 500; f[1] = 1500; f[2] = 2500;
        return;
    }
    f[0] = ph->f1; f[1] = ph->f2; f[2] = ph->f3;
    if (ph->g1 > 0) {                                 /* diphthong: glide in the second half */
        float w = u < 0.35f ? 0.0f : (u - 0.35f) / 0.65f;
        f[0] += (ph->g1 - f[0]) * w; f[1] += (ph->g2 - f[1]) * w; f[2] += (ph->g3 - f[2]) * w;
    }
}

typedef struct { float a, b, c, y1, y2; } Res;
static void res_set(Res *r, float f, float bw, float sr)
{
    float T = 1.0f / sr, C = -expf(-2.0f * PI_F * bw * T), B = 2.0f * expf(-PI_F * bw * T) * cosf(2.0f * PI_F * f * T);
    r->a = 1.0f - B - C; r->b = B; r->c = C;
}
static inline float res_run(Res *r, float x)
{
    float y = r->a * x + r->b * r->y1 + r->c * r->y2;
    r->y2 = r->y1; r->y1 = y;
    return y;
}

int speech_render(const char *phrase, float pitch, float rate, float growl, float *out, int maxlen, float sr)
{
    static Seg seg[MAXSEG];
    int n = parse(phrase, seg, rate);
    if (n == 0) return 0;
    float T = seg[n - 1].t1 + 0.06f;
    int len = (int)(T * sr);
    if (len > maxlen) len = maxlen;
    Res r1 = {0}, r2 = {0}, r3 = {0}, r4 = {0}, rn = {0}, rf = {0};
    res_set(&r4, 3300, 250, sr);
    uint32_t rng = 0x9E3779B9u;
    float ph = 0, gprev = 0, f1 = 500, f2 = 1500, f3 = 2500, av = 0, af = 0, ah = 0, fc = 4000, fbw = 2000, nas = 0, gr = 0;
    float hp = 0, hp_prev = 0, pulse_amp = 1.0f, nzp = 0;
    int pulse_n = 0, si = 0;
    for (int i = 0; i < len; i++) {
        float t = (float)i / sr;
        while (si < n - 1 && t >= seg[si].t1) si++;
        if ((i & 31) == 0) {
            /* control rate: every 1 ms work out where the articulators should be */
            const Seg *s = &seg[si];
            const Ph *p = &PH[s->p];
            float u = (t - s->t0) / (s->t1 - s->t0 > 1e-4f ? s->t1 - s->t0 : 1e-4f);
            if (u < 0) u = 0; if (u > 1) u = 1;
            float tf[3], pf[3], nf[3];
            targets(seg, n, si, u, tf);
            /* smooth formant transitions across the boundaries (40 ms either side) */
            float tr = 0.04f, d0 = t - s->t0, d1 = s->t1 - t;
            if (si > 0 && d0 < tr) {
                targets(seg, n, si - 1, 1.0f, pf);
                float w = 0.5f + 0.5f * d0 / tr;
                for (int k = 0; k < 3; k++) tf[k] = tf[k] * w + pf[k] * (1.0f - w);
            } else if (si < n - 1 && d1 < tr) {
                targets(seg, n, si + 1, 0.0f, nf);
                float w = 0.5f + 0.5f * d1 / tr;
                for (int k = 0; k < 3; k++) tf[k] = tf[k] * w + nf[k] * (1.0f - w);
            }
            float k1 = 0.25f;                       /* a little lag so nothing clicks */
            f1 += (tf[0] - f1) * k1; f2 += (tf[1] - f2) * k1; f3 += (tf[2] - f3) * k1;
            /* source amplitudes, with quicker 8 ms ramps */
            float tav = p->av, taf = p->af, tah = p->kind == K_ASP ? (p->name[0] == 'h' ? 0.45f : 0.7f) : 0.0f;
            if (p->kind == K_VOW && s->stress) tav *= 1.1f;
            float ka = 0.12f;
            av += (tav - av) * ka; af += (taf - af) * (p->kind == K_BURST ? 0.6f : ka); ah += (tah - ah) * ka;
            if (p->af > 0) { fc = p->fc; fbw = p->fbw; }
            nas += ((p->kind == K_NAS ? 1.0f : 0.0f) - nas) * 0.2f;
            gr += ((p->kind == K_GROWL ? 1.0f : 0.0f) - gr) * 0.1f;
            res_set(&r1, f1 * (1.0f - 0.05f * gr), 70.0f + 60.0f * nas, sr);
            res_set(&r2, f2, 100.0f + 80.0f * nas, sr);
            res_set(&r3, f3, 160.0f, sr);
            res_set(&rn, 270.0f, 100.0f, sr);
            res_set(&rf, fc, fbw, sr);
        }
        /* pitch: falling through the phrase, rising on stressed vowels, dropping and rasping in a growl */
        float f0 = 95.0f * pitch * (1.12f - 0.24f * t / T);
        const Seg *s = &seg[si];
        if (s->stress) {
            float u = (t - s->t0) / (s->t1 - s->t0);
            f0 *= 1.0f + 0.28f * sinf(PI_F * (u < 0 ? 0 : u > 1 ? 1 : u));
        }
        f0 *= 1.0f - 0.18f * gr;
        rng = rng * 1664525u + 1013904223u;
        float nz = (float)(rng >> 9) / 4194304.0f - 1.0f;
        f0 *= 1.0f + 0.012f * nz;                           /* jitter */
        /* glottal pulse (Rosenberg), differentiated for the lip radiation */
        ph += f0 / sr;
        if (ph >= 1.0f) {
            ph -= 1.0f; pulse_n++;
            /* a rough, creaky pirate voice: alternate pulses weaker, much more so in the growl */
            float creak = growl * (0.25f + 0.55f * gr);
            pulse_amp = (pulse_n & 1) ? 1.0f - creak : 1.0f;
            pulse_amp *= 1.0f + 0.08f * nz;
        }
        float g;
        if (ph < 0.4f) g = 0.5f * (1.0f - cosf(PI_F * ph / 0.4f));
        else if (ph < 0.62f) g = cosf(0.5f * PI_F * (ph - 0.4f) / 0.22f);
        else g = 0.0f;
        g *= pulse_amp;
        float src = (g - gprev) * 8.0f;
        gprev = g;
        float trill = 1.0f - gr * growl * 0.5f * (0.5f + 0.5f * sinf(2.0f * PI_F * 27.0f * t));   /* the rolling rrr */
        float voiced = src * av * trill + nz * (0.004f * av + 0.12f * ah);   /* a little breath in every voiced sound */
        float v = res_run(&r1, voiced);
        v = res_run(&r2, v);
        v = res_run(&r3, v);
        v = res_run(&r4, v);
        if (nas > 0.01f) v = v * (1.0f - 0.5f * nas) + res_run(&rn, src * av) * 0.6f * nas;   /* nasal murmur */
        float fr = res_run(&rf, nz - nzp) * af;      /* differenced noise: hiss without rumble */
        nzp = nz;
        float x = v * 0.55f + fr * 0.9f;
        /* high-pass at ~100 Hz */
        hp = 0.98f * (hp + x - hp_prev); hp_prev = x;
        out[i] = hp;
    }
    return len;
}
