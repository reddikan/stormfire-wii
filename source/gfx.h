/*
 * STORMFIRE - tiny 2D triangle renderer (platform independent).
 *
 * The game draws everything as flat / gouraud-shaded triangles into a big
 * vertex list, split into segments by blend mode (normal alpha blending or
 * additive "glow" blending). On the Wii, main.c sends the list to the GPU (GX)
 * every frame; the native test harness rasterizes the same list in software.
 *
 * Virtual screen: 854 x 480 (16:9). The Wii renders it anamorphically into its
 * 640-wide framebuffer and the TV / Dolphin stretches it back to 16:9.
 */
#ifndef GFX_H
#define GFX_H

#include <stdint.h>

#define VW 854.0f
#define VH 480.0f

#define GFX_MAX_VTX 150000
#define GFX_MAX_SEG 4096

enum { BL_ALPHA, BL_ADD };

typedef struct { float x, y; uint32_t c; } GVtx;          /* c = 0xRRGGBBAA */
typedef struct { int mode, start, count; } GSeg;

extern GVtx gfx_v[GFX_MAX_VTX];
extern GSeg gfx_seg[GFX_MAX_SEG];
extern int  gfx_nv, gfx_nseg;

static inline uint32_t RGBA(int r, int g, int b, int a)
{
    if (r < 0) r = 0; if (r > 255) r = 255;
    if (g < 0) g = 0; if (g > 255) g = 255;
    if (b < 0) b = 0; if (b > 255) b = 255;
    if (a < 0) a = 0; if (a > 255) a = 255;
    return ((uint32_t)r << 24) | ((uint32_t)g << 16) | ((uint32_t)b << 8) | (uint32_t)a;
}
static inline uint32_t col_a(uint32_t c, float a)          /* scale alpha */
{
    int al = (int)((float)(c & 255) * a);
    if (al < 0) al = 0; if (al > 255) al = 255;
    return (c & 0xFFFFFF00u) | (uint32_t)al;
}
uint32_t col_mix(uint32_t a, uint32_t b, float t);           /* lerp all four channels */
uint32_t col_hsv(float h, float s, float v, int a);          /* h in turns (0..1) */

void gfx_begin(void);
void gfx_blend(int mode);
/* view transform applied to everything: zoom about the screen centre, then offset (screen shake) */
void gfx_view(float ox, float oy, float zoom);
void gfx_view_reset(void);

void gfx_tri(float x0, float y0, uint32_t c0, float x1, float y1, uint32_t c1, float x2, float y2, uint32_t c2);
void gfx_tri1(float x0, float y0, float x1, float y1, float x2, float y2, uint32_t c);
void gfx_quad4(float x0, float y0, uint32_t c0, float x1, float y1, uint32_t c1,
               float x2, float y2, uint32_t c2, float x3, float y3, uint32_t c3);   /* corners in order */
void gfx_rect(float x, float y, float w, float h, uint32_t c);
void gfx_rect_v(float x, float y, float w, float h, uint32_t top, uint32_t bot);   /* vertical gradient */
void gfx_poly(float cx, float cy, float r, int n, float rot, uint32_t c);          /* filled regular polygon */
void gfx_poly2(float cx, float cy, float r, int n, float rot, uint32_t cin, uint32_t cout);
void gfx_ring(float cx, float cy, float r, float thick, int n, float rot, uint32_t c);
void gfx_ring2(float cx, float cy, float r, float thick, int n, float rot, uint32_t cin, uint32_t cout);
void gfx_arc(float cx, float cy, float r0, float r1, float a0, float a1, int n, uint32_t c0, uint32_t c1); /* c0 at a0 -> c1 at a1 */
void gfx_glow(float cx, float cy, float r, uint32_t c);   /* soft radial blob: c at the centre, fading to 0 */
void gfx_glow_cheap(float cx, float cy, float r, uint32_t c);
void gfx_line(float x0, float y0, float x1, float y1, float w, uint32_t c0, uint32_t c1);
void gfx_glowline(float x0, float y0, float x1, float y1, float w, uint32_t c);   /* soft-edged line */
void gfx_capsule(float x, float y, float dx, float dy, float len, float w, uint32_t c); /* tapered streak */

/* 5x7 pixel font: uppercase, digits and a little punctuation */
const uint8_t *gfx_glyph(char ch);                 /* 7 rows of 5 bits, or NULL */
float gfx_text_width(const char *s, float px);
void  gfx_text(const char *s, float x, float y, float px, uint32_t c);            /* top-left anchored */
void  gfx_text_c(const char *s, float cx, float y, float px, uint32_t c);         /* centred */
void  gfx_text_r(const char *s, float rx, float y, float px, uint32_t c);         /* right aligned */
/* fancy text: each letter bounces/scales individually; t = animation time */
void  gfx_text_wave(const char *s, float cx, float y, float px, uint32_t c, float t, float amp);
/* text with a soft dark drop shadow for readability over the scene */
void  gfx_text_s(const char *s, float x, float y, float px, uint32_t c);
void  gfx_text_sc(const char *s, float cx, float y, float px, uint32_t c);
void  gfx_text_sr(const char *s, float rx, float y, float px, uint32_t c);

#endif
