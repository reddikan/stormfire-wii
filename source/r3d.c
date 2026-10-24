/*
 * STORMFIRE - camera, lighting grid and 3D primitives. See r3d.h.
 */
#include "r3d.h"
#include "gfx.h"

#include <math.h>
#include <string.h>

Cam cam;
static Cam saved;
static int lt_ui;
int   r_unlit;
float r_alpha = 1.0f;
static int   flat_on;
static float flat_l[3];

/* ---- static cache storage ---- */
#define RC_MAXV 60000
#define RC_MAXP 6000
#define RC_SLOTS 32
typedef struct { float x, y, r, g, b, f, sw, dx, dy; uint16_t li; uint8_t a, pad; } CVtx;   /* sw: sway in pixels; dx,dy: stretch */
static CVtx  rcv[RC_MAXV];
static float rcp[RC_MAXP][3];          /* light sample points */
static float rcl[RC_MAXP][3];          /* their light this frame */
static float rcs[RC_MAXP];             /* their sway this frame (-1..1, times the wind) */
static float sw_amp, sw_y0, sw_y1 = 1;  /* sway being recorded: world units at the top, from y0 to y1 */
static float rc_stretch = 1.0f;          /* how long the stretchable shadows are this frame */
static float st_dx, st_dy;               /* stretch of the vertex being recorded */
static int   rc_np, rc_cur_slot;
static int   rc_start[RC_SLOTS], rc_count[RC_SLOTS];
static int   rc_flat_idx = -1;
int rc_recording, rc_verts;
#define RC_HASH 8192
static int rc_hash[RC_HASH];

static int rc_point(float x, float y, float z, int dedupe)
{
    if (dedupe) {
        int qx = (int)floorf(x * 4.0f + 0.5f), qy = (int)floorf(y * 8.0f + 0.5f), qz = (int)floorf(z * 4.0f + 0.5f);
        uint32_t h = ((uint32_t)qx * 73856093u ^ (uint32_t)qy * 19349663u ^ (uint32_t)qz * 83492791u) & (RC_HASH - 1);
        for (;;) {
            int i = rc_hash[h];
            if (i < 0) break;
            if ((int)floorf(rcp[i][0] * 4.0f + 0.5f) == qx && (int)floorf(rcp[i][1] * 8.0f + 0.5f) == qy && (int)floorf(rcp[i][2] * 4.0f + 0.5f) == qz) return i;
            h = (h + 1) & (RC_HASH - 1);
        }
        if (rc_np >= RC_MAXP) return 0;
        rc_hash[h] = rc_np;
    }
    if (rc_np >= RC_MAXP) return 0;
    rcp[rc_np][0] = x; rcp[rc_np][1] = y; rcp[rc_np][2] = z;
    return rc_np++;
}

void rc_begin(void)
{
    rc_recording = 1; rc_verts = 0; rc_np = 0; rc_cur_slot = -1; rc_flat_idx = -1;
    for (int i = 0; i < RC_HASH; i++) rc_hash[i] = -1;
    for (int i = 0; i < RC_SLOTS; i++) rc_start[i] = rc_count[i] = 0;
}
void rc_slot(int slot) { rc_cur_slot = slot; rc_start[slot] = rc_verts; rc_count[slot] = 0; }
void rc_end(void) { rc_recording = 0; }

void rc_sway(float amp, float y0, float y1) { sw_amp = amp; sw_y0 = y0; sw_y1 = y1 > y0 + 0.01f ? y1 : y0 + 0.01f; }
static void rc_vert(float sx, float sy, uint32_t c, float f, int li, float sw);
int  rc_last_point(void) { return rc_flat_idx; }
void rc_set_stretch(float k) { rc_stretch = k; }

/* a flat polygon on the ground whose corners can slide along dir (scaled by w[i]) as rc_set_stretch changes;
 * recorded at stretch 0 (the corner sits at p[i]) */
void r_ground_stretch(int n, const float (*p)[3], const float *w, float dx, float dz, uint32_t c)
{
    if (!rc_recording || n > 8) return;
    float sx[8], sy[8], ox[8], oy[8];
    int li = rc_flat_idx >= 0 ? rc_flat_idx : rc_point(p[0][0], 0, p[0][2], 1);
    for (int i = 0; i < n; i++) {
        proj(p[i][0], p[i][1], p[i][2], &sx[i], &sy[i]);
        float ex, ey;
        proj(p[i][0] + dx * w[i], p[i][1], p[i][2] + dz * w[i], &ex, &ey);
        ox[i] = ex - sx[i]; oy[i] = ey - sy[i];
    }
    for (int i = 1; i + 1 < n; i++) {
        int id[3] = { 0, i, i + 1 };
        for (int k = 0; k < 3; k++) { st_dx = ox[id[k]]; st_dy = oy[id[k]]; rc_vert(sx[id[k]], sy[id[k]], c, 1.0f, li, 0); }
    }
    st_dx = st_dy = 0;
}
float rc_sway_of(int li) { return (li >= 0 && li < rc_np) ? rcs[li] : 0.0f; }

static float sway_px(float x, float y, float z)
{
    if (sw_amp <= 0) return 0;
    float u = (y - sw_y0) / (sw_y1 - sw_y0);
    if (u <= 0) return 0;
    if (u > 1) u = 1;
    (void)x;
    return sw_amp * u * u * proj_scale(y, z);
}

static void rc_vert(float sx, float sy, uint32_t c, float f, int li, float sw)
{
    if (rc_verts >= RC_MAXV || rc_cur_slot < 0) return;
    CVtx *v = &rcv[rc_verts++];
    v->x = sx; v->y = sy;
    v->r = (float)(c >> 24); v->g = (float)((c >> 16) & 255); v->b = (float)((c >> 8) & 255); v->a = (uint8_t)(c & 255);
    v->f = f; v->li = (uint16_t)li; v->sw = sw; v->dx = st_dx; v->dy = st_dy;
    rc_count[rc_cur_slot]++;
}

void rc_relight(float t, float wind)
{
    for (int i = 0; i < rc_np; i++) {
        lt_at(rcp[i][0], rcp[i][1], rcp[i][2], rcl[i]);
        float ph = (float)i;
        rcs[i] = wind * (0.65f * sinf(t * 1.1f + ph * 1.7f) + 0.35f * sinf(t * 2.3f + ph * 0.9f));
    }
}

void rc_draw(int slot, float shx, float shy)
{
    int n = rc_count[slot];
    if (n <= 0 || gfx_nseg == 0 || gfx_nv + n > GFX_MAX_VTX) return;
    const CVtx *v = &rcv[rc_start[slot]];
    GVtx *o = &gfx_v[gfx_nv];
    for (int i = 0; i < n; i++, v++, o++) {
        const float *l = rcl[v->li];
        float mr = l[0] * v->f, mg = l[1] * v->f, mb = l[2] * v->f;
        float r = v->r * mr, g = v->g * mg, b = v->b * mb;
        if (mr > 1.0f) r += (mr - 1.0f) * 40.0f;
        if (mg > 1.0f) g += (mg - 1.0f) * 40.0f;
        if (mb > 1.0f) b += (mb - 1.0f) * 40.0f;
        int ir = (int)r, ig = (int)g, ib = (int)b;
        if (ir > 255) ir = 255; if (ig > 255) ig = 255; if (ib > 255) ib = 255;
        o->x = v->x + shx + v->sw * rcs[v->li] + v->dx * rc_stretch; o->y = v->y + shy + v->dy * rc_stretch;
        o->c = ((uint32_t)ir << 24) | ((uint32_t)ig << 16) | ((uint32_t)ib << 8) | v->a;
    }
    gfx_xform_n(&gfx_v[gfx_nv], n);
    gfx_nv += n;
    gfx_seg[gfx_nseg - 1].count += n;
}

void r_light_model(float x, float y, float z)
{
    if (rc_recording) { rc_flat_idx = rc_point(x, y, z, 0); flat_on = 1; return; }
    lt_at(x, y, z, flat_l); flat_on = 1;
}
void r_light_off(void) { flat_on = 0; rc_flat_idx = -1; }

static uint32_t shade_l(uint32_t base, const float *l, float face)
{
    int out[3];
    for (int ch = 0; ch < 3; ch++) {
        float m = l[ch] * face;
        float v = (float)((base >> (24 - 8 * ch)) & 255) * m;
        if (m > 1.0f) v += (m - 1.0f) * 40.0f;
        out[ch] = (int)v;
    }
    return RGBA(out[0], out[1], out[2], (int)((float)(base & 255) * r_alpha));
}

/* camera: pitched 57 degrees down, looking at the middle of the map */
#define PITCH 0.995f
#define CAM_D 30.0f
#define SCALE 35.0f

static void cam_finish(void)
{
    cam.camx = cam.lx;
    cam.camy = cam.D * cam.s;
    cam.camz = cam.lz + cam.D * cam.c;
}

void cam_world(float shx, float shy)
{
    cam.lx = 9.5f; cam.lz = 6.55f;
    cam.cx = VW * 0.5f + shx; cam.cy = VH * 0.5f + 4.0f + shy;
    cam.s = sinf(PITCH); cam.c = cosf(PITCH);
    cam.D = CAM_D; cam.f = SCALE * CAM_D;
    cam.mk = 1.0f;
    cam_finish();
    lt_ui = 0;
}

void cam_ui(float cx, float cy, float scale)
{
    saved = cam;
    cam.lx = 0; cam.lz = 0;
    cam.cx = cx; cam.cy = cy;
    cam.s = sinf(0.62f); cam.c = cosf(0.62f);
    cam.D = 40.0f; cam.f = scale * 40.0f;
    cam.mk = 1.0f;
    cam_finish();
    lt_ui = 1;
}

void cam_restore(void) { cam = saved; lt_ui = 0; }

/* ------------------------------------------------------------------ */
/* lighting                                                            */
/* ------------------------------------------------------------------ */
#define MAXL 48
typedef struct { float x, y, z, r, g, b, rad, inv; } Light;
static Light L[MAXL];
int lt_count;

#define GX0 (-4)
#define GZ0 (-4)
#define NX 28
#define NZ 22
#define NY 3
static const float LY[NY] = { 0.0f, 0.9f, 2.0f };
static float grid[NY][NZ][NX][3];
static float clampd(float v) { return v < -1.0f ? -1.0f : (v > 1.0f ? 1.0f : v); }

void lt_begin(void) { lt_count = 0; }

void lt_add(float x, float y, float z, float r, float g, float b, float radius)
{
    if (lt_count >= MAXL || radius <= 0.01f) return;
    Light *l = &L[lt_count++];
    l->x = x; l->y = y; l->z = z; l->r = r; l->g = g; l->b = b; l->rad = radius;
    l->inv = 1.0f / (radius * radius);
}

/* ambient light: night moonlight by default; lt_set_ambient switches it (golden hour) */
static float amb_base[3] = { 0.17f, 0.20f, 0.32f }, amb_up[3] = { 0.04f, 0.045f, 0.06f }, amb_dapple;
static float dap[NZ][NX];
static int   dap_ok;

void lt_set_ambient(const float base[3], const float up[3], float dapple)
{
    for (int i = 0; i < 3; i++) { amb_base[i] = base[i]; amb_up[i] = up[i]; }
    amb_dapple = dapple;
    if (!dap_ok) {
        /* sunlight through gaps in the canopy: a fixed blotchy pattern, brighter towards the sun (left) */
        for (int iz = 0; iz < NZ; iz++)
            for (int ix = 0; ix < NX; ix++) {
                uint32_t h = (uint32_t)ix * 2654435761u ^ (uint32_t)iz * 40503u;
                h = (h ^ (h >> 15)) * 2246822519u; h ^= h >> 13;
                float n = (float)(h & 1023) / 1023.0f;
                float sx = sinf((float)ix * 0.9f + (float)iz * 0.4f) * 0.5f + sinf((float)iz * 1.3f - (float)ix * 0.5f) * 0.5f;
                dap[iz][ix] = clampd(0.55f * sx + 0.45f * (n * 2.0f - 1.0f));
            }
        dap_ok = 1;
    }
}

void lt_build(float flash, float amb)
{
    /* ambient first (a bit stronger higher up); lightning washes everything blue-white */
    for (int iy = 0; iy < NY; iy++) {
        float ar = (amb_base[0] + amb_up[0] * (float)iy) * amb + flash * 1.05f;
        float ag = (amb_base[1] + amb_up[1] * (float)iy) * amb + flash * 1.10f;
        float ab = (amb_base[2] + amb_up[2] * (float)iy) * amb + flash * 1.30f;
        for (int iz = 0; iz < NZ; iz++)
            for (int ix = 0; ix < NX; ix++) {
                float *o = grid[iy][iz][ix];
                float k = 1.0f + amb_dapple * dap[iz][ix];
                o[0] = ar * k; o[1] = ag * k; o[2] = ab * (1.0f + amb_dapple * 0.5f * dap[iz][ix]);
            }
    }
    /* then each light only touches the grid points inside its radius */
    for (int i = 0; i < lt_count; i++) {
        const Light *l = &L[i];
        int x0 = (int)ceilf(l->x - l->rad) - GX0, x1 = (int)floorf(l->x + l->rad) - GX0;
        int z0 = (int)ceilf(l->z - l->rad) - GZ0, z1 = (int)floorf(l->z + l->rad) - GZ0;
        if (x0 < 0) x0 = 0; if (z0 < 0) z0 = 0;
        if (x1 > NX - 1) x1 = NX - 1; if (z1 > NZ - 1) z1 = NZ - 1;
        for (int iy = 0; iy < NY; iy++) {
            float dy = LY[iy] - l->y, dy2 = dy * dy * l->inv;
            if (dy2 >= 1.0f) continue;
            for (int iz = z0; iz <= z1; iz++) {
                float dz = (float)(GZ0 + iz) - l->z, dz2 = dz * dz * l->inv + dy2;
                if (dz2 >= 1.0f) continue;
                for (int ix = x0; ix <= x1; ix++) {
                    float dx = (float)(GX0 + ix) - l->x;
                    float d2 = dx * dx * l->inv + dz2;
                    if (d2 >= 1.0f) continue;
                    float k = 1.0f - d2; k *= k;
                    float *o = grid[iy][iz][ix];
                    o[0] += l->r * k; o[1] += l->g * k; o[2] += l->b * k;
                }
            }
        }
    }
}

void lt_at(float x, float y, float z, float out[3])
{
    if (lt_ui) { out[0] = 1.0f; out[1] = 0.98f; out[2] = 0.95f; return; }
    float fx = x - (float)GX0, fz = z - (float)GZ0;
    if (fx < 0) fx = 0; if (fx > NX - 1.001f) fx = NX - 1.001f;
    if (fz < 0) fz = 0; if (fz > NZ - 1.001f) fz = NZ - 1.001f;
    int ix = (int)fx, iz = (int)fz, iy;
    float tx = fx - (float)ix, tz = fz - (float)iz, ty;
    if (y <= LY[1]) { iy = 0; ty = y / LY[1]; } else { iy = 1; ty = (y - LY[1]) / (LY[2] - LY[1]); }
    if (ty < 0) ty = 0; if (ty > 1) ty = 1;
    for (int ch = 0; ch < 3; ch++) {
        float a0 = grid[iy][iz][ix][ch] + (grid[iy][iz][ix + 1][ch] - grid[iy][iz][ix][ch]) * tx;
        float a1 = grid[iy][iz + 1][ix][ch] + (grid[iy][iz + 1][ix + 1][ch] - grid[iy][iz + 1][ix][ch]) * tx;
        float b0 = grid[iy + 1][iz][ix][ch] + (grid[iy + 1][iz][ix + 1][ch] - grid[iy + 1][iz][ix][ch]) * tx;
        float b1 = grid[iy + 1][iz + 1][ix][ch] + (grid[iy + 1][iz + 1][ix + 1][ch] - grid[iy + 1][iz + 1][ix][ch]) * tx;
        float a = a0 + (a1 - a0) * tz, b = b0 + (b1 - b0) * tz;
        out[ch] = a + (b - a) * ty;
    }
}

uint32_t lt_shade(uint32_t base, float x, float y, float z, float face)
{
    float l[3];
    lt_at(x, y, z, l);
    return shade_l(base, l, face);                          /* very bright light washes towards white */
}

float face_f(float nx, float ny, float nz)
{
    float l = sqrtf(nx * nx + ny * ny + nz * nz);
    if (l < 1e-6f) return 1.0f;
    nx /= l; ny /= l; (void)nz;
    if (ny < 0) return 0.42f;
    return 0.62f + 0.38f * ny - 0.17f * nx * (1.0f - ny);
}

/* ------------------------------------------------------------------ */
/* solids                                                              */
/* ------------------------------------------------------------------ */
void r_face(int n, const float (*p)[3], float nx, float ny, float nz, uint32_t c)
{
    /* back-face cull against the camera position */
    float vx = cam.camx - p[0][0], vy = cam.camy - p[0][1], vz = cam.camz - p[0][2];
    if (nx * vx + ny * vy + nz * vz <= 0.0f) return;
    float sx[8], sy[8];
    uint32_t col[8];
    float ff = r_unlit ? 1.0f : face_f(nx, ny, nz);
    if (rc_recording) {
        int li[8];
        float sw[8];
        for (int i = 0; i < n; i++) {
            proj(p[i][0], p[i][1], p[i][2], &sx[i], &sy[i]);
            li[i] = flat_on ? rc_flat_idx : rc_point(p[i][0], p[i][1], p[i][2], 1);
            sw[i] = sway_px(p[i][0], p[i][1], p[i][2]);
        }
        for (int i = 1; i + 1 < n; i++) {
            rc_vert(sx[0], sy[0], c, ff, li[0], sw[0]); rc_vert(sx[i], sy[i], c, ff, li[i], sw[i]); rc_vert(sx[i + 1], sy[i + 1], c, ff, li[i + 1], sw[i + 1]);
        }
        return;
    }
    uint32_t fc = 0;
    if (r_unlit) fc = col_a(c, r_alpha);
    else if (flat_on) fc = shade_l(c, flat_l, ff);
    for (int i = 0; i < n; i++) {
        proj(p[i][0], p[i][1], p[i][2], &sx[i], &sy[i]);
        col[i] = (r_unlit || flat_on) ? fc : lt_shade(c, p[i][0], p[i][1], p[i][2], ff);
    }
    for (int i = 1; i + 1 < n; i++)
        gfx_tri(sx[0], sy[0], col[0], sx[i], sy[i], col[i], sx[i + 1], sy[i + 1], col[i + 1]);
}

/* 8 corners indexed by bits: 1 = +a axis, 2 = +b (up-ish), 4 = +c axis. na/nb/nc are the axis directions. */
static void hexa(const float (*k)[3], const float *na, const float *nb, const float *nc, uint32_t top, uint32_t side, int skip)
{
    static const int F[6][4] = { {1, 3, 7, 5}, {0, 4, 6, 2}, {2, 6, 7, 3}, {0, 1, 5, 4}, {4, 5, 7, 6}, {0, 2, 3, 1} };
    for (int f = 0; f < 6; f++) {
        if (f == 3) continue;                                   /* bottom: never seen */
        if (f == 0 && (skip & SK_RIGHT)) continue;
        if (f == 1 && (skip & SK_LEFT)) continue;
        if (f == 2 && (skip & SK_TOP)) continue;
        if (f == 4 && (skip & SK_FRONT)) continue;
        if (f == 5 && (skip & SK_BACK)) continue;
        float n[3];
        const float *ax = f < 2 ? na : (f < 4 ? nb : nc);
        float sg = (f & 1) ? -1.0f : 1.0f;
        n[0] = ax[0] * sg; n[1] = ax[1] * sg; n[2] = ax[2] * sg;
        float p[4][3];
        for (int i = 0; i < 4; i++) memcpy(p[i], k[F[f][i]], sizeof p[i]);
        r_face(4, (const float (*)[3])p, n[0], n[1], n[2], f == 2 ? top : side);
    }
}

void r_box(float x0, float y0, float z0, float x1, float y1, float z1, uint32_t top, uint32_t side, int skip)
{
    float k[8][3];
    for (int i = 0; i < 8; i++) {
        k[i][0] = (i & 1) ? x1 : x0;
        k[i][1] = (i & 2) ? y1 : y0;
        k[i][2] = (i & 4) ? z1 : z0;
    }
    static const float A[3] = {1, 0, 0}, B[3] = {0, 1, 0}, C[3] = {0, 0, 1};
    hexa((const float (*)[3])k, A, B, C, top, side, skip);
}

void r_obox(float x, float y0, float z, float hw, float hd, float y1, float ang, uint32_t top, uint32_t side)
{
    float ca = cosf(ang), sa = sinf(ang);
    float A[3] = {ca, 0, sa}, B[3] = {0, 1, 0}, C[3] = {-sa, 0, ca};
    float k[8][3];
    for (int i = 0; i < 8; i++) {
        float a = (i & 1) ? hw : -hw, c = (i & 4) ? hd : -hd;
        k[i][0] = x + A[0] * a + C[0] * c;
        k[i][1] = (i & 2) ? y1 : y0;
        k[i][2] = z + A[2] * a + C[2] * c;
    }
    hexa((const float (*)[3])k, A, B, C, top, side, 0);
}

void r_beam(float x, float y, float z, float dx, float dy, float dz, float len, float w, float h, uint32_t c)
{
    float l = sqrtf(dx * dx + dy * dy + dz * dz);
    if (l < 1e-5f) return;
    float F[3] = {dx / l, dy / l, dz / l};
    float S[3] = {F[2], 0, -F[0]};
    float sl = sqrtf(S[0] * S[0] + S[2] * S[2]);
    if (sl < 1e-4f) { S[0] = 1; S[2] = 0; sl = 1; }
    S[0] /= sl; S[2] /= sl;
    float U[3] = { F[1] * S[2] - F[2] * S[1], F[2] * S[0] - F[0] * S[2], F[0] * S[1] - F[1] * S[0] };
    if (U[1] < 0) { U[0] = -U[0]; U[1] = -U[1]; U[2] = -U[2]; }
    float k[8][3];
    for (int i = 0; i < 8; i++) {
        float a = (i & 1) ? w * 0.5f : -w * 0.5f, b = (i & 2) ? h * 0.5f : -h * 0.5f, cc = (i & 4) ? len : 0.0f;
        for (int j = 0; j < 3; j++) k[i][j] = (j == 0 ? x : (j == 1 ? y : z)) + S[j] * a + U[j] * b + F[j] * cc;
    }
    /* the hexa index layout wants: bit1 = +S, bit2 = +U, bit4 = +F */
    hexa((const float (*)[3])k, S, U, F, c, c, 0);
}

void r_stick(float x0, float y0, float z0, float x1, float y1, float z1, float w0, float w1, uint32_t c)
{
    float dx = x1 - x0, dy = y1 - y0, dz = z1 - z0;
    float vx = cam.camx - x0, vy = cam.camy - y0, vz = cam.camz - z0;     /* towards the camera */
    float sx = dy * vz - dz * vy, sy = dz * vx - dx * vz, sz = dx * vy - dy * vx;
    float sl = sqrtf(sx * sx + sy * sy + sz * sz);
    if (sl < 1e-6f) return;
    sx /= sl; sy /= sl; sz /= sl;
    float p[4][3] = { {x0 - sx * w0 * 0.5f, y0 - sy * w0 * 0.5f, z0 - sz * w0 * 0.5f}, {x0 + sx * w0 * 0.5f, y0 + sy * w0 * 0.5f, z0 + sz * w0 * 0.5f},
                      {x1 + sx * w1 * 0.5f, y1 + sy * w1 * 0.5f, z1 + sz * w1 * 0.5f}, {x1 - sx * w1 * 0.5f, y1 - sy * w1 * 0.5f, z1 - sz * w1 * 0.5f} };
    r_face(4, (const float (*)[3])p, vx, vy + 0.3f, vz, c);              /* lit like a mostly-front face */
}

void r_prism(float x, float y0, float z, float rb, float rt, float y1, int n, float rot, uint32_t top, uint32_t side)
{
    if (n < 3) n = 3;
    if (n > 24) n = 24;
    static float tab[25][24][2];
    static int tab_ok[25];
    if (!tab_ok[n]) {
        for (int i = 0; i < n; i++) { tab[n][i][0] = cosf(6.2831853f * (float)i / (float)n); tab[n][i][1] = sinf(6.2831853f * (float)i / (float)n); }
        tab_ok[n] = 1;
    }
    float cr = cosf(rot), sr = sinf(rot);
    float h = y1 - y0, slope = rb - rt;
    float ring_b[24][2], ring_t[24][2];
    for (int i = 0; i < n; i++) {
        float ca = tab[n][i][0] * cr - tab[n][i][1] * sr, sa = tab[n][i][1] * cr + tab[n][i][0] * sr;
        ring_b[i][0] = x + ca * rb; ring_b[i][1] = z + sa * rb;
        ring_t[i][0] = x + ca * rt; ring_t[i][1] = z + sa * rt;
    }
    for (int i = 0; i < n; i++) {
        int j = (i + 1) % n;
        float mx = (ring_b[i][0] + ring_b[j][0]) * 0.5f - x, mz = (ring_b[i][1] + ring_b[j][1]) * 0.5f - z;
        float ml = sqrtf(mx * mx + mz * mz);
        if (ml < 1e-6f) continue;
        mx /= ml; mz /= ml;
        float nx = mx * h, ny = slope, nz = mz * h;
        if (rt <= 0.0001f) {
            float p[3][3] = { {ring_b[i][0], y0, ring_b[i][1]}, {ring_b[j][0], y0, ring_b[j][1]}, {x, y1, z} };
            r_face(3, (const float (*)[3])p, nx, ny, nz, side);
        } else {
            float p[4][3] = { {ring_b[i][0], y0, ring_b[i][1]}, {ring_b[j][0], y0, ring_b[j][1]},
                              {ring_t[j][0], y1, ring_t[j][1]}, {ring_t[i][0], y1, ring_t[i][1]} };
            r_face(4, (const float (*)[3])p, nx, ny, nz, side);
        }
    }
    if (rt > 0.0001f && h > 0) {
        float p[24][3];
        for (int i = 0; i < n; i++) { p[i][0] = ring_t[i][0]; p[i][1] = y1; p[i][2] = ring_t[i][1]; }
        /* fan in chunks of 8 (r_face takes up to 8 points) */
        for (int s = 1; s + 1 < n; s += 6) {
            float q[8][3];
            int m = 0;
            memcpy(q[m++], p[0], sizeof q[0]);
            for (int i = s; i < n && m < 8; i++) memcpy(q[m++], p[i], sizeof q[0]);
            r_face(m, (const float (*)[3])q, 0, 1, 0, top);
        }
    }
}

void r_crystal(float x, float y, float z, float r, float up, float down, int n, float rot, uint32_t c)
{
    if (n > 8) n = 8;
    float e[8][3];
    for (int i = 0; i < n; i++) {
        float a = rot + 6.2831853f * (float)i / (float)n;
        e[i][0] = x + cosf(a) * r; e[i][1] = y; e[i][2] = z + sinf(a) * r;
    }
    for (int i = 0; i < n; i++) {
        int j = (i + 1) % n;
        for (int half = 0; half < 2; half++) {
            float tip[3] = { x, half ? y - down : y + up, z };
            float p[3][3];
            memcpy(p[0], tip, sizeof tip); memcpy(p[1], e[i], sizeof tip); memcpy(p[2], e[j], sizeof tip);
            float ux = p[1][0] - p[0][0], uy = p[1][1] - p[0][1], uz = p[1][2] - p[0][2];
            float vx = p[2][0] - p[0][0], vy = p[2][1] - p[0][1], vz = p[2][2] - p[0][2];
            float nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
            float cx = (p[0][0] + p[1][0] + p[2][0]) / 3 - x, cy = (p[0][1] + p[1][1] + p[2][1]) / 3 - y, cz = (p[0][2] + p[1][2] + p[2][2]) / 3 - z;
            if (nx * cx + ny * cy + nz * cz < 0) { nx = -nx; ny = -ny; nz = -nz; }
            r_face(3, (const float (*)[3])p, nx, ny, nz, c);
        }
    }
}

void r_ground_quad(float x0, float z0, float x1, float z1, uint32_t c, const float *ao)
{
    float p[4][3] = { {x0, 0, z0}, {x1, 0, z0}, {x1, 0, z1}, {x0, 0, z1} };
    float sx[4], sy[4];
    for (int i = 0; i < 4; i++) proj(p[i][0], 0, p[i][2], &sx[i], &sy[i]);
    if (rc_recording) {
        int li[4];
        for (int i = 0; i < 4; i++) li[i] = rc_point(p[i][0], 0, p[i][2], 1);
        static const int T6[6] = { 0, 1, 2, 0, 2, 3 };
        for (int k = 0; k < 6; k++) rc_vert(sx[T6[k]], sy[T6[k]], c, ao[T6[k]], li[T6[k]], 0);
        return;
    }
    uint32_t cc[4];
    for (int i = 0; i < 4; i++) cc[i] = lt_shade(c, p[i][0], 0, p[i][2], ao[i]);
    gfx_quad4(sx[0], sy[0], cc[0], sx[1], sy[1], cc[1], sx[2], sy[2], cc[2], sx[3], sy[3], cc[3]);
}

/* ------------------------------------------------------------------ */
/* flat & glowing                                                      */
/* ------------------------------------------------------------------ */
static const float K16[17][2] = {
    {1,0},{0.92388f,0.38268f},{0.70711f,0.70711f},{0.38268f,0.92388f},{0,1},{-0.38268f,0.92388f},{-0.70711f,0.70711f},{-0.92388f,0.38268f},
    {-1,0},{-0.92388f,-0.38268f},{-0.70711f,-0.70711f},{-0.38268f,-0.92388f},{0,-1},{0.38268f,-0.92388f},{0.70711f,-0.70711f},{0.92388f,-0.38268f},{1,0} };

void r_gdisc(float x, float z, float y, float rad, int n, uint32_t cin, uint32_t cout)
{
    float cx, cy;
    proj(x, y, z, &cx, &cy);
    int step = n >= 16 ? 1 : 2;
    float px, py;
    proj(x + rad, y, z, &px, &py);
    for (int i = step; i <= 16; i += step) {
        float nx, ny;
        proj(x + K16[i][0] * rad, y, z + K16[i][1] * rad, &nx, &ny);
        gfx_tri(cx, cy, cin, px, py, cout, nx, ny, cout);
        px = nx; py = ny;
    }
}

void r_gring(float x, float z, float y, float rad, float thick, int n, uint32_t c)
{
    if (n < 6) n = 6;
    if (n > 48) n = 48;
    float r0 = rad - thick * 0.5f, r1 = rad + thick * 0.5f;
    if (r0 < 0) r0 = 0;
    float cs = cosf(6.2831853f / (float)n), sn = sinf(6.2831853f / (float)n), ca = 1, sa = 0;
    float ax, ay, bx, by;
    proj(x + r0, y, z, &ax, &ay);
    proj(x + r1, y, z, &bx, &by);
    for (int i = 1; i <= n; i++) {
        float t = ca * cs - sa * sn; sa = sa * cs + ca * sn; ca = t;
        float cx2, cy2, dx2, dy2;
        proj(x + ca * r0, y, z + sa * r0, &cx2, &cy2);
        proj(x + ca * r1, y, z + sa * r1, &dx2, &dy2);
        gfx_quad4(ax, ay, c, bx, by, c, dx2, dy2, c, cx2, cy2, c);
        ax = cx2; ay = cy2; bx = dx2; by = dy2;
    }
}

void r_gquad(float x0, float z0, float x1, float z1, float y, uint32_t c)
{
    float a[4][2];
    proj(x0, y, z0, &a[0][0], &a[0][1]);
    proj(x1, y, z0, &a[1][0], &a[1][1]);
    proj(x1, y, z1, &a[2][0], &a[2][1]);
    proj(x0, y, z1, &a[3][0], &a[3][1]);
    gfx_quad4(a[0][0], a[0][1], c, a[1][0], a[1][1], c, a[2][0], a[2][1], c, a[3][0], a[3][1], c);
}

void r_glow(float x, float y, float z, float rad, uint32_t c)
{
    float sx, sy;
    proj(x, y, z, &sx, &sy);
    float pr = rad * proj_scale(y, z);
    if (pr > 7.0f) gfx_glow(sx, sy, pr, c); else gfx_glow_cheap(sx, sy, pr, c);
}

void r_glow_cheap(float x, float y, float z, float rad, uint32_t c)
{
    float sx, sy;
    proj(x, y, z, &sx, &sy);
    gfx_glow_cheap(sx, sy, rad * proj_scale(y, z), c);
}

void r_line(float x0, float y0, float z0, float x1, float y1, float z1, float w, uint32_t c0, uint32_t c1)
{
    float ax, ay, bx, by;
    proj(x0, y0, z0, &ax, &ay);
    proj(x1, y1, z1, &bx, &by);
    gfx_line(ax, ay, bx, by, w, c0, c1);
}

void r_glowline(float x0, float y0, float z0, float x1, float y1, float z1, float w, uint32_t c)
{
    float ax, ay, bx, by;
    proj(x0, y0, z0, &ax, &ay);
    proj(x1, y1, z1, &bx, &by);
    gfx_glowline(ax, ay, bx, by, w, c);
}
