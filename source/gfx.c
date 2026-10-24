/*
 * STORMFIRE - triangle list builder + primitives + pixel font. See gfx.h.
 */
#include "gfx.h"

#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#define TAU 6.28318530718f

GVtx gfx_v[GFX_MAX_VTX] __attribute__((aligned(32)));
GSeg gfx_seg[GFX_MAX_SEG];
int  gfx_nv, gfx_nseg;

static int   cur_mode;
static float v_ox, v_oy, v_zoom = 1.0f;

uint32_t col_mix(uint32_t a, uint32_t b, float t)
{
    if (t <= 0.0f) return a;
    if (t >= 1.0f) return b;
    uint32_t r = 0;
    for (int s = 0; s < 32; s += 8) {
        float x = (float)((a >> s) & 255), y = (float)((b >> s) & 255);
        r |= (uint32_t)(int)(x + (y - x) * t + 0.5f) << s;
    }
    return r;
}

uint32_t col_hsv(float h, float s, float v, int a)
{
    h -= floorf(h);
    float h6 = h * 6.0f;
    int   i = (int)h6;
    float f = h6 - (float)i;
    float p = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f));
    float r, g, b;
    switch (i % 6) {
    case 0: r = v; g = t; b = p; break;
    case 1: r = q; g = v; b = p; break;
    case 2: r = p; g = v; b = t; break;
    case 3: r = p; g = q; b = v; break;
    case 4: r = t; g = p; b = v; break;
    default: r = v; g = p; b = q; break;
    }
    return RGBA((int)(r * 255), (int)(g * 255), (int)(b * 255), a);
}

void gfx_begin(void)
{
    gfx_nv = 0;
    gfx_nseg = 0;
    cur_mode = -1;
    gfx_view_reset();
    gfx_blend(BL_ALPHA);
}

void gfx_blend(int mode)
{
    if (mode == cur_mode) return;
    if (gfx_nseg > 0 && gfx_seg[gfx_nseg - 1].count == 0) {   /* empty segment: just retag it */
        gfx_seg[gfx_nseg - 1].mode = mode;
        cur_mode = mode;
        return;
    }
    if (gfx_nseg >= GFX_MAX_SEG) return;
    gfx_seg[gfx_nseg].mode  = mode;
    gfx_seg[gfx_nseg].start = gfx_nv;
    gfx_seg[gfx_nseg].count = 0;
    gfx_nseg++;
    cur_mode = mode;
}

void gfx_view(float ox, float oy, float zoom) { v_ox = ox; v_oy = oy; v_zoom = zoom; }
void gfx_view_reset(void) { v_ox = 0; v_oy = 0; v_zoom = 1.0f; }

static inline void emit(float x, float y, uint32_t c)
{
    GVtx *v = &gfx_v[gfx_nv++];
    v->x = (x - VW * 0.5f) * v_zoom + VW * 0.5f + v_ox;
    v->y = (y - VH * 0.5f) * v_zoom + VH * 0.5f + v_oy;
    v->c = c;
}

void gfx_tri(float x0, float y0, uint32_t c0, float x1, float y1, uint32_t c1, float x2, float y2, uint32_t c2)
{
    if (gfx_nv + 3 > GFX_MAX_VTX || gfx_nseg == 0) return;
    if (((c0 | c1 | c2) & 255) == 0) return;                  /* fully transparent */
    emit(x0, y0, c0); emit(x1, y1, c1); emit(x2, y2, c2);
    gfx_seg[gfx_nseg - 1].count += 3;
}

void gfx_tri1(float x0, float y0, float x1, float y1, float x2, float y2, uint32_t c)
{
    gfx_tri(x0, y0, c, x1, y1, c, x2, y2, c);
}

void gfx_quad4(float x0, float y0, uint32_t c0, float x1, float y1, uint32_t c1,
               float x2, float y2, uint32_t c2, float x3, float y3, uint32_t c3)
{
    gfx_tri(x0, y0, c0, x1, y1, c1, x2, y2, c2);
    gfx_tri(x0, y0, c0, x2, y2, c2, x3, y3, c3);
}

void gfx_rect(float x, float y, float w, float h, uint32_t c)
{
    gfx_quad4(x, y, c, x + w, y, c, x + w, y + h, c, x, y + h, c);
}

void gfx_rect_v(float x, float y, float w, float h, uint32_t top, uint32_t bot)
{
    gfx_quad4(x, y, top, x + w, y, top, x + w, y + h, bot, x, y + h, bot);
}

void gfx_poly(float cx, float cy, float r, int n, float rot, uint32_t c) { gfx_poly2(cx, cy, r, n, rot, c, c); }

void gfx_poly2(float cx, float cy, float r, int n, float rot, uint32_t cin, uint32_t cout)
{
    /* walk round the circle by repeated rotation: one sin/cos per call (cheap on the Wii's CPU) */
    float ca = cosf(rot), sa = sinf(rot), cs = cosf(TAU / (float)n), sn = sinf(TAU / (float)n);
    float px = cx + ca * r, py = cy + sa * r;
    for (int i = 1; i <= n; i++) {
        float t = ca * cs - sa * sn; sa = sa * cs + ca * sn; ca = t;
        float nx = cx + ca * r, ny = cy + sa * r;
        gfx_tri(cx, cy, cin, px, py, cout, nx, ny, cout);
        px = nx; py = ny;
    }
}

void gfx_ring(float cx, float cy, float r, float thick, int n, float rot, uint32_t c) { gfx_ring2(cx, cy, r, thick, n, rot, c, c); }

void gfx_ring2(float cx, float cy, float r, float thick, int n, float rot, uint32_t cin, uint32_t cout)
{
    float r0 = r - thick * 0.5f, r1 = r + thick * 0.5f;
    if (r0 < 0) r0 = 0;
    float ca = cosf(rot), sa = sinf(rot), cs = cosf(TAU / (float)n), sn = sinf(TAU / (float)n);
    for (int i = 1; i <= n; i++) {
        float cb = ca * cs - sa * sn, sb = sa * cs + ca * sn;
        gfx_quad4(cx + ca * r0, cy + sa * r0, cin, cx + ca * r1, cy + sa * r1, cout,
                  cx + cb * r1, cy + sb * r1, cout, cx + cb * r0, cy + sb * r0, cin);
        ca = cb; sa = sb;
    }
}

void gfx_arc(float cx, float cy, float r0, float r1, float a0, float a1, int n, uint32_t c0, uint32_t c1)
{
    if (n < 1) n = 1;
    uint32_t pc = c0;
    float step = (a1 - a0) / (float)n;
    float ca = cosf(a0), sa = sinf(a0), cs = cosf(step), sn = sinf(step);
    for (int i = 1; i <= n; i++) {
        float t = (float)i / (float)n;
        uint32_t c = col_mix(c0, c1, t);
        float cb = ca * cs - sa * sn, sb = sa * cs + ca * sn;
        gfx_quad4(cx + ca * r0, cy + sa * r0, pc, cx + ca * r1, cy + sa * r1, pc,
                  cx + cb * r1, cy + sb * r1, c, cx + cb * r0, cy + sb * r0, c);
        ca = cb; sa = sb; pc = c;
    }
}

/* two-band radial gradient approximating a soft gaussian bloom */
void gfx_glow(float cx, float cy, float r, uint32_t c)
{
    if (r < 1.0f || (c & 255) == 0) return;
    if (cx + r < -20 || cx - r > VW + 20 || cy + r < -20 || cy - r > VH + 20) return;
    const int n = 16;
    uint32_t cm = col_a(c, 0.30f), ce = c & 0xFFFFFF00u;
    float rm = r * 0.35f;
    float pmx = cx + rm, pmy = cy, pex = cx + r, pey = cy;
    static const float K16[17][2] = {
        {1,0},{0.92388f,0.38268f},{0.70711f,0.70711f},{0.38268f,0.92388f},{0,1},{-0.38268f,0.92388f},{-0.70711f,0.70711f},{-0.92388f,0.38268f},
        {-1,0},{-0.92388f,-0.38268f},{-0.70711f,-0.70711f},{-0.38268f,-0.92388f},{0,-1},{0.38268f,-0.92388f},{0.70711f,-0.70711f},{0.92388f,-0.38268f},{1,0} };
    for (int i = 1; i <= n; i++) {
        float ca = K16[i][0], sa = K16[i][1];
        float mx = cx + ca * rm, my = cy + sa * rm, ex = cx + ca * r, ey = cy + sa * r;
        gfx_tri(cx, cy, c, pmx, pmy, cm, mx, my, cm);
        gfx_quad4(pmx, pmy, cm, pex, pey, ce, ex, ey, ce, mx, my, cm);
        pmx = mx; pmy = my; pex = ex; pey = ey;
    }
}

void gfx_glow_cheap(float cx, float cy, float r, uint32_t c)
{
    if (r < 0.5f || (c & 255) == 0) return;
    if (cx + r < -10 || cx - r > VW + 10 || cy + r < -10 || cy - r > VH + 10) return;
    static const float K[9][2] = { {1,0},{0.707f,0.707f},{0,1},{-0.707f,0.707f},{-1,0},{-0.707f,-0.707f},{0,-1},{0.707f,-0.707f},{1,0} };
    uint32_t ce = c & 0xFFFFFF00u;
    for (int i = 0; i < 8; i++)
        gfx_tri(cx, cy, c, cx + K[i][0] * r, cy + K[i][1] * r, ce, cx + K[i + 1][0] * r, cy + K[i + 1][1] * r, ce);
}

void gfx_line(float x0, float y0, float x1, float y1, float w, uint32_t c0, uint32_t c1)
{
    float dx = x1 - x0, dy = y1 - y0, l = sqrtf(dx * dx + dy * dy);
    if (l < 0.001f) return;
    float nx = -dy / l * w * 0.5f, ny = dx / l * w * 0.5f;
    gfx_quad4(x0 + nx, y0 + ny, c0, x1 + nx, y1 + ny, c1, x1 - nx, y1 - ny, c1, x0 - nx, y0 - ny, c0);
}

void gfx_glowline(float x0, float y0, float x1, float y1, float w, uint32_t c)
{
    float dx = x1 - x0, dy = y1 - y0, l = sqrtf(dx * dx + dy * dy);
    if (l < 0.001f) return;
    float nx = -dy / l * w * 0.5f, ny = dx / l * w * 0.5f;
    uint32_t e = c & 0xFFFFFF00u;
    gfx_quad4(x0 + nx, y0 + ny, e, x1 + nx, y1 + ny, e, x1, y1, c, x0, y0, c);
    gfx_quad4(x0, y0, c, x1, y1, c, x1 - nx, y1 - ny, e, x0 - nx, y0 - ny, e);
}

/* a streak: pointed tail behind (x,y) along -(dx,dy), round-ish head */
void gfx_capsule(float x, float y, float dx, float dy, float len, float w, uint32_t c)
{
    float l = sqrtf(dx * dx + dy * dy);
    if (l < 0.0001f) { dx = 1; dy = 0; l = 1; }
    dx /= l; dy /= l;
    float nx = -dy * w * 0.5f, ny = dx * w * 0.5f;
    float hx = x + dx * w * 0.5f, hy = y + dy * w * 0.5f;
    float tx = x - dx * len, ty = y - dy * len;
    uint32_t e = c & 0xFFFFFF00u;
    gfx_tri(hx, hy, c, x + nx, y + ny, c, x - nx, y - ny, c);
    gfx_tri(x + nx, y + ny, c, tx, ty, e, x - nx, y - ny, c);
}

/* ------------------------------------------------------------------ */
/* 5x7 pixel font                                                      */
/* ------------------------------------------------------------------ */
typedef struct { char ch; uint8_t r[7]; } Glyph;
static const Glyph FONT[] = {
    {'0',{0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}}, {'1',{0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}},
    {'2',{0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}}, {'3',{0x1F,0x02,0x04,0x02,0x01,0x11,0x0E}},
    {'4',{0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}}, {'5',{0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E}},
    {'6',{0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}}, {'7',{0x1F,0x01,0x02,0x04,0x08,0x08,0x08}},
    {'8',{0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}}, {'9',{0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}},
    {'A',{0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}}, {'B',{0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}},
    {'C',{0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}}, {'D',{0x1C,0x12,0x11,0x11,0x11,0x12,0x1C}},
    {'E',{0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}}, {'F',{0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}},
    {'G',{0x0E,0x11,0x10,0x17,0x11,0x11,0x0F}}, {'H',{0x11,0x11,0x11,0x1F,0x11,0x11,0x11}},
    {'I',{0x0E,0x04,0x04,0x04,0x04,0x04,0x0E}}, {'J',{0x07,0x02,0x02,0x02,0x02,0x12,0x0C}},
    {'K',{0x11,0x12,0x14,0x18,0x14,0x12,0x11}}, {'L',{0x10,0x10,0x10,0x10,0x10,0x10,0x1F}},
    {'M',{0x11,0x1B,0x15,0x15,0x11,0x11,0x11}}, {'N',{0x11,0x11,0x19,0x15,0x13,0x11,0x11}},
    {'O',{0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}}, {'P',{0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}},
    {'Q',{0x0E,0x11,0x11,0x11,0x15,0x12,0x0D}}, {'R',{0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}},
    {'S',{0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}}, {'T',{0x1F,0x04,0x04,0x04,0x04,0x04,0x04}},
    {'U',{0x11,0x11,0x11,0x11,0x11,0x11,0x0E}}, {'V',{0x11,0x11,0x11,0x11,0x11,0x0A,0x04}},
    {'W',{0x11,0x11,0x11,0x15,0x15,0x15,0x0A}}, {'X',{0x11,0x11,0x0A,0x04,0x0A,0x11,0x11}},
    {'Y',{0x11,0x11,0x11,0x0A,0x04,0x04,0x04}}, {'Z',{0x1F,0x01,0x02,0x04,0x08,0x10,0x1F}},
    {'x',{0x00,0x00,0x11,0x0A,0x04,0x0A,0x11}}, {'.',{0x00,0x00,0x00,0x00,0x00,0x0C,0x0C}},
    {',',{0x00,0x00,0x00,0x00,0x0C,0x04,0x08}}, {'!',{0x04,0x04,0x04,0x04,0x04,0x00,0x04}},
    {'?',{0x0E,0x11,0x01,0x02,0x04,0x00,0x04}}, {':',{0x00,0x0C,0x0C,0x00,0x0C,0x0C,0x00}},
    {'-',{0x00,0x00,0x00,0x1F,0x00,0x00,0x00}}, {'+',{0x00,0x04,0x04,0x1F,0x04,0x04,0x00}},
    {'/',{0x01,0x01,0x02,0x04,0x08,0x10,0x10}}, {'%',{0x18,0x19,0x02,0x04,0x08,0x13,0x03}},
    {'\'',{0x04,0x04,0x08,0x00,0x00,0x00,0x00}}, {'(',{0x02,0x04,0x08,0x08,0x08,0x04,0x02}},
    {')',{0x08,0x04,0x02,0x02,0x02,0x04,0x08}}, {'>',{0x08,0x04,0x02,0x01,0x02,0x04,0x08}},
    {'<',{0x02,0x04,0x08,0x10,0x08,0x04,0x02}}, {'=',{0x00,0x00,0x1F,0x00,0x1F,0x00,0x00}},
    {'*',{0x00,0x15,0x0E,0x1F,0x0E,0x15,0x00}}, {'#',{0x0A,0x0A,0x1F,0x0A,0x1F,0x0A,0x0A}},
};

static const uint8_t *glyph(char ch)
{
    if (ch >= 'a' && ch <= 'z' && ch != 'x') ch = (char)(ch - 32);
    for (unsigned i = 0; i < sizeof FONT / sizeof FONT[0]; i++)
        if (FONT[i].ch == ch) return FONT[i].r;
    return NULL;
}

const uint8_t *gfx_glyph(char ch) { return glyph(ch); }

float gfx_text_width(const char *s, float px)
{
    int n = (int)strlen(s);
    return n > 0 ? ((float)n * 6.0f - 1.0f) * px : 0.0f;
}

static void draw_glyph(const uint8_t *g, float x, float y, float px, uint32_t c)
{
    for (int r = 0; r < 7; r++) {
        uint8_t bits = g[r];
        int col = 0;
        while (col < 5) {
            if (!(bits & (0x10 >> col))) { col++; continue; }
            int s = col;
            while (col < 5 && (bits & (0x10 >> col))) col++;
            gfx_rect(x + (float)s * px, y + (float)r * px, (float)(col - s) * px, px, c);
        }
    }
}

void gfx_text(const char *s, float x, float y, float px, uint32_t c)
{
    for (; *s; s++, x += 6.0f * px) {
        const uint8_t *g = glyph(*s);
        if (g) draw_glyph(g, x, y, px, c);
    }
}

void gfx_text_c(const char *s, float cx, float y, float px, uint32_t c) { gfx_text(s, cx - gfx_text_width(s, px) * 0.5f, y, px, c); }
void gfx_text_r(const char *s, float rx, float y, float px, uint32_t c) { gfx_text(s, rx - gfx_text_width(s, px), y, px, c); }

void gfx_text_wave(const char *s, float cx, float y, float px, uint32_t c, float t, float amp)
{
    float x = cx - gfx_text_width(s, px) * 0.5f;
    for (int i = 0; s[i]; i++, x += 6.0f * px) {
        const uint8_t *g = glyph(s[i]);
        if (g) draw_glyph(g, x, y + sinf(t * 7.0f - (float)i * 0.55f) * amp, px, c);
    }
}

void gfx_text_s(const char *s, float x, float y, float px, uint32_t c)
{
    float o = px * 0.6f;
    if (o < 1.0f) o = 1.0f;
    if (px >= 1.75f)                      /* small text sits on dark panels and needs no shadow */
        gfx_text(s, x + o, y + o, px, RGBA(0, 0, 0, (int)((float)(c & 255) * 0.75f)));
    gfx_text(s, x, y, px, c);
}
void gfx_text_sc(const char *s, float cx, float y, float px, uint32_t c) { gfx_text_s(s, cx - gfx_text_width(s, px) * 0.5f, y, px, c); }
void gfx_text_sr(const char *s, float rx, float y, float px, uint32_t c) { gfx_text_s(s, rx - gfx_text_width(s, px), y, px, c); }
