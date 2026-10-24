/*
 * STORMFIRE - a tiny 3D layer on top of the 2D triangle list (gfx.h).
 *
 * A fixed tilted perspective camera looks down at the map. Solid shapes are
 * convex (boxes, prisms, cones, crystals) and drop their back faces; the game
 * draws them back-to-front (painter's algorithm), so no depth buffer is needed.
 *
 * Lighting is sampled from a coarse 3D grid that is rebuilt every frame from
 * the scene's point lights (campfire, torches, towers, explosions), a dim blue
 * moonlight and the lightning flash. Every lit vertex looks the grid up.
 */
#ifndef R3D_H
#define R3D_H

#include <stdint.h>

typedef struct {
    float lx, lz;          /* world point under the screen centre */
    float cx, cy;          /* its screen position */
    float f, D, s, c;      /* focal length, camera distance, sin / cos of the pitch */
    float camx, camy, camz;
    float mx, my, mz, mk;  /* model scale about (mx, my, mz); mk = 1 when off */
} Cam;
extern Cam cam;

void cam_world(float shake_x, float shake_y);
/* draw small models in the HUD: (cx, cy) on screen, `scale` pixels per world unit, lit evenly */
void cam_ui(float cx, float cy, float scale);
void cam_restore(void);
/* draw the next models scaled by k about (x, y, z) - makes props chunkier without touching their code */
static inline void r_scale(float x, float y, float z, float k) { cam.mx = x; cam.my = y; cam.mz = z; cam.mk = k; }
static inline void r_scale_off(void) { cam.mk = 1.0f; }

static inline int proj(float x, float y, float z, float *sx, float *sy)
{
    if (cam.mk != 1.0f) { x = cam.mx + (x - cam.mx) * cam.mk; y = cam.my + (y - cam.my) * cam.mk; z = cam.mz + (z - cam.mz) * cam.mk; }
    float pz = z - cam.lz;
    float depth = cam.D - y * cam.s - pz * cam.c;
    if (depth < 0.2f) depth = 0.2f;
    float k = cam.f / depth;
    *sx = cam.cx + (x - cam.lx) * k;
    *sy = cam.cy - (y * cam.c - pz * cam.s) * k;
    return 1;
}
static inline float proj_scale(float y, float z)     /* pixels per world unit at that spot */
{
    float depth = cam.D - y * cam.s - (z - cam.lz) * cam.c;
    if (depth < 0.2f) depth = 0.2f;
    return cam.f / depth * cam.mk;
}

/* ---- lighting ---- */
void lt_begin(void);
void lt_add(float x, float y, float z, float r, float g, float b, float radius);
/* flash: lightning 0..1; amb: overall ambient scale */
void lt_build(float flash, float amb);
void lt_at(float x, float y, float z, float out[3]);
uint32_t lt_shade(uint32_t base, float x, float y, float z, float face);
extern int lt_count;

/* face brightness from its normal (unit or not): top 1.0, front ~0.62, moonlit left side brighter */
float face_f(float nx, float ny, float nz);

/* ---- solid shapes (lit) ---- */
extern int r_unlit;       /* 1: colours are emissive (no lighting) */
/* light a whole small model with one lookup at (x, y, z) instead of per vertex (much cheaper) */
void r_light_model(float x, float y, float z);
void r_light_off(void);
extern float r_alpha;     /* alpha multiplier for solids (ghost previews) */
enum { SK_LEFT = 1, SK_RIGHT = 2, SK_FRONT = 4, SK_BACK = 8, SK_TOP = 16 };
void r_face(int n, const float (*p)[3], float nx, float ny, float nz, uint32_t c);
void r_box(float x0, float y0, float z0, float x1, float y1, float z1, uint32_t top, uint32_t side, int skip);
/* box rotated about the y axis by ang, centred at (x, z) */
void r_obox(float x, float y0, float z, float hw, float hd, float y1, float ang, uint32_t top, uint32_t side);
/* prism / frustum / cone (rt = 0) / cylinder; n sides around (x, z) */
void r_prism(float x, float y0, float z, float rb, float rt, float y1, int n, float rot, uint32_t top, uint32_t side);
/* crystal: diamond with 4..8 sides, top tip at y + up, bottom tip at y - down */
void r_crystal(float x, float y, float z, float r, float up, float down, int n, float rot, uint32_t c);
/* a slab tilted towards ang (for barrels): a box from p0 along direction (dx, dy, dz) */
void r_beam(float x, float y, float z, float dx, float dy, float dz, float len, float w, float h, uint32_t c);

/* ---- static geometry cache ----
 * The scenery never moves, so it is projected once and stored. Each frame only its colours are
 * recomputed from the light grid (one light sample per corner or per model, shared between vertices). */
void rc_begin(void);                  /* start recording: r_face / r_gquad_lit go into the cache */
void rc_slot(int slot);               /* following geometry belongs to this slot */
void rc_end(void);
void rc_relight(void);                /* sample the lights for every cached point (after lt_build) */
void rc_draw(int slot, float shx, float shy);
extern int rc_recording;
extern int rc_verts;
/* a lit ground quad with a brightness factor per corner (for soft corner shadows) */
void r_ground_quad(float x0, float z0, float x1, float z1, uint32_t c, const float *ao);

/* ---- flat things on the ground / glows (unlit) ---- */
void r_gdisc(float x, float z, float y, float rad, int n, uint32_t cin, uint32_t cout);
void r_gring(float x, float z, float y, float rad, float thick, int n, uint32_t c);
void r_gquad(float x0, float z0, float x1, float z1, float y, uint32_t c);
void r_glow(float x, float y, float z, float rad, uint32_t c);           /* rad in world units */
void r_glow_cheap(float x, float y, float z, float rad, uint32_t c);
void r_line(float x0, float y0, float z0, float x1, float y1, float z1, float w, uint32_t c0, uint32_t c1);
void r_glowline(float x0, float y0, float z0, float x1, float y1, float z1, float w, uint32_t c);

#endif
