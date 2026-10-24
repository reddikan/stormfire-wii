/*
 * STORMFIRE - everything you see: the rainy night map, towers, enemies, effects and the HUD.
 *
 * Draw order (painter's algorithm, back to front):
 *   1. the ground (lit per corner, with soft corner shadows), puddles, path markings, glow pools
 *   2. row by row from the far edge to the near edge: trees, walls, blocks, towers, walking enemies
 *   3. flyers, shells and bolts, particles, lightning arcs
 *   4. rain, mist, vignette, then the HUD on top
 */
#include "game_int.h"
#include "gfx.h"
#include "r3d.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define TAU 6.28318530718f

static const Game *G;

#define TOWER_K 1.3f          /* models are drawn chunkier than their footprint */
#define ENEMY_K 1.5f
static float T;               /* render time */

static uint32_t hsh(int c, int r)
{
    uint32_t h = (uint32_t)c * 374761393u + (uint32_t)r * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}
static float hf(int c, int r, int k) { return (float)((hsh(c + k * 31, r - k * 17) >> 8) & 0xFFFF) / 65535.0f; }

static uint32_t scale_rgb(uint32_t c, float k)
{
    return RGBA((int)((float)(c >> 24) * k), (int)((float)((c >> 16) & 255) * k), (int)((float)((c >> 8) & 255) * k), (int)(c & 255));
}

static const uint32_t BLOCK_COL[BC_COUNT] = {
    0xE8505AFF, 0xF5983AFF, 0xF5CE3CFF, 0x7AD452FF, 0x7EE0B8FF, 0x4C8CF0FF, 0x9A68E0FF, 0x5CC8F0FF
};

static int in_map(int c, int r) { return c >= 0 && r >= 0 && c < MW && r < MH; }
static int kind_at(int c, int r) { return in_map(c, r) ? G->tile[r][c].kind : TL_TREE; }
static int solid(int c, int r) { int k = kind_at(c, r); return k == TL_WALL || k == TL_BLOCK; }

/* off-map: keep a clear lane in front of each gate so arriving enemies are visible */
static int outside_lane(int c, int r)
{
    for (int i = 0; i < NUM_GATES; i++) {
        int gc = GATES[i][0], gr = GATES[i][1];
        if (gr == 0 && c == gc && r < 0 && r >= -3) return 1;
        if (gr == MH - 1 && c == gc && r >= MH && r <= MH + 2) return 1;
        if (gc == 0 && r == gr && c < 0 && c >= -3) return 1;
        if (gc == MW - 1 && r == gr && c >= MW && c <= MW + 2) return 1;
    }
    return 0;
}

static int on_screen(float x, float y, float z, float margin)
{
    float sx, sy;
    proj(x, y, z, &sx, &sy);
    return sx > -margin && sx < VW + margin && sy > -margin && sy < VH + margin;
}

/* ------------------------------------------------------------------ */
/* lights                                                              */
/* ------------------------------------------------------------------ */
static float flicker(float seed)
{
    return 0.86f + 0.08f * sinf(T * 11.0f + seed) + 0.06f * sinf(T * 23.7f + seed * 3.1f);
}

static void build_lights(void)
{
    lt_begin();
    float fv = G->fire_vis, fl = flicker(0.0f);
    if (fv > 0.02f)
        lt_add(FIRE_X, 0.9f, FIRE_Z, 1.35f * fv * fl, 0.62f * fv * fl, 0.24f * fv * fl, 2.6f + 2.4f * fv);
    for (int r = 0; r < MH; r++)
        for (int c = 0; c < MW; c++) {
            const Tile *t = &G->tile[r][c];
            if (t->torch) {
                float k = flicker((float)(c * 7 + r));
                lt_add((float)c + 0.5f, t->h + 0.75f, (float)r + 0.5f, 0.85f * k, 0.42f * k, 0.15f * k, 2.2f);
            }
            const Tower *w = &G->tw[r][c];
            if (w->type >= 0) {
                uint32_t gc = TOWERS[w->type].glow;
                float k = 0.35f + 0.1f * (float)w->lvl + w->pulse * 0.6f;
                lt_add((float)c + 0.5f, t->h + 1.0f, (float)r + 0.5f, (float)(gc >> 24) / 255.0f * k,
                       (float)((gc >> 16) & 255) / 255.0f * k, (float)((gc >> 8) & 255) / 255.0f * k, 1.7f + 0.2f * (float)w->lvl);
            }
        }
    for (int i = 0; i < MAX_SH; i++) {
        const Shot *s = &G->sh[i];
        if (s->alive && s->kind == SH_SHELL) lt_add(s->x, s->y, s->z, 0.8f, 0.4f, 0.1f, 1.6f);
    }
    for (int i = 0; i < MAX_EN; i++) {
        const Enemy *e = &G->en[i];
        if (!e->alive) continue;
        if (e->type == EN_WISP) lt_add(e->x, e->y, e->z, 0.25f, 0.6f, 0.45f, 1.6f);
        if (e->type == EN_GOLEM) lt_add(e->x, 1.2f, e->z, 0.2f, 0.6f, 0.9f, 2.6f);
    }
    for (int i = 0; i < MAX_FL; i++) {
        const FLight *l = &G->fl[i];
        if (l->t >= l->life) continue;
        float k = 1.0f - l->t / l->life;
        lt_add(l->x, l->y, l->z, l->r * k, l->g * k, l->b * k, l->rad);
    }
    lt_build(G->flash * 0.8f, 1.0f);
}

/* ------------------------------------------------------------------ */
/* ground                                                              */
/* ------------------------------------------------------------------ */
static uint32_t ground_col(int c, int r)
{
    int k = in_map(c, r) ? G->tile[r][c].kind : -1;
    float v = 0.92f + 0.16f * hf(c, r, 1);
    uint32_t base;
    if (k == TL_PATH || (!in_map(c, r) && outside_lane(c, r))) base = 0x4E7A3CFF;
    else if (k == TL_LAWN) base = ((c + r) & 1) ? 0x5A9444FF : 0x528A3EFF;
    else if (k == TL_FIRE) base = 0x8A8078FF;
    else if (k == TL_TREE) base = 0x3D6634FF;
    else base = 0x33582EFF;                        /* forest floor beyond the map */
    return scale_rgb(base, v);
}

#define SLOT_GROUND 0
#define SLOT_ROW0 1            /* rows -4 .. MH+2 use slots 1 .. */
#define ROW_MIN (-4)
#define ROW_MAX (MH + 3)

static void build_ground(void)
{
    for (int r = ROW_MIN; r < ROW_MAX; r++)
        for (int c = -5; c < MW + 5; c++) {
            if (in_map(c, r) && solid(c, r)) continue;     /* hidden under its wall / block */
            if (!on_screen((float)c + 0.5f, 0, (float)r + 0.5f, 70.0f)) continue;
            static const int CO[4][2] = { {0, 0}, {1, 0}, {1, 1}, {0, 1} };
            float ao[4];
            for (int k = 0; k < 4; k++) {
                int vx = c + CO[k][0], vz = r + CO[k][1];
                /* soft shadow where the corner touches walls / blocks */
                int occ = solid(vx - 1, vz - 1) + solid(vx, vz - 1) + solid(vx - 1, vz) + solid(vx, vz);
                ao[k] = 1.0f - 0.16f * (float)occ;
            }
            r_ground_quad((float)c, (float)r, (float)c + 1, (float)r + 1, ground_col(c, r), ao);
        }
}

static float SHX, SHY;
int dbg_nv[8];          /* vertex counts after each phase (for the test harness) */          /* screen shake this frame */

static void draw_ground(void)
{
    rc_draw(SLOT_GROUND, SHX, SHY);

    /* puddles: dark glossy pools that pick up the lights around them */
    for (int r = 0; r < MH; r++)
        for (int c = 0; c < MW; c++) {
            const Tile *t = &G->tile[r][c];
            if (!t->puddle) continue;
            float x = (float)c + 0.3f + 0.4f * hf(c, r, 3), z = (float)r + 0.3f + 0.4f * hf(c, r, 4);
            float rad = 0.16f + 0.08f * (float)t->puddle;
            float l[3];
            lt_at(x, 0, z, l);
            uint32_t in = RGBA((int)(30 + 70 * l[0]), (int)(40 + 80 * l[1]), (int)(60 + 110 * l[2]), 215);
            uint32_t out = RGBA((int)(20 + 40 * l[0]), (int)(30 + 50 * l[1]), (int)(45 + 70 * l[2]), 120);
            r_gdisc(x, z, 0.005f, rad, 16, in, out);
            float ph = fmodf(T * 0.9f + hf(c, r, 5) * 3.0f, 1.0f);
            r_gring(x, z, 0.008f, rad * (0.2f + 0.7f * ph), 0.02f, 14, RGBA(180, 200, 240, (int)(90 * (1.0f - ph))));
        }

    gfx_blend(BL_ADD);
    /* marching red dashes show where the enemies will walk */
    for (int r = 0; r < MH; r++)
        for (int c = 0; c < MW; c++) {
            if (G->tile[r][c].kind != TL_PATH) continue;
            int nc = G->nxt[r][c][0], nr = G->nxt[r][c][1];
            if (nc < 0) continue;
            float x0 = (float)c + 0.5f, z0 = (float)r + 0.5f, dx = (float)(nc - c), dz = (float)(nr - r);
            float ph = fmodf(T * 0.55f, 0.5f);
            for (int k = -1; k < 2; k++) {
                float a = ph + 0.5f * (float)k, b = a + 0.2f;
                if (b <= 0) continue;
                if (a < 0) a = 0;
                if (b > 1) b = 1;
                if (a >= 1) continue;
                float ax = x0 + dx * a, az = z0 + dz * a, bx = x0 + dx * b, bz = z0 + dz * b;
                float w = 0.035f;
                uint32_t col = RGBA(255, 70, 50, 70);
                if (dx != 0) r_gquad(fminf(ax, bx), az - w, fmaxf(ax, bx), az + w, 0.01f, col);
                else r_gquad(ax - w, fminf(az, bz), ax + w, fmaxf(az, bz), 0.01f, col);
            }
        }
    /* warm pools of light */
    float fv = G->fire_vis, fl = flicker(0.0f);
    if (fv > 0.02f) {
        r_gdisc(FIRE_X, FIRE_Z, 0.01f, 2.7f * fv * fl, 16, RGBA(255, 130, 40, (int)(60 * fv)), 0xFF601000);
        r_gdisc(FIRE_X, FIRE_Z, 0.012f, 1.5f * fv, 16, RGBA(255, 190, 90, (int)(70 * fv)), 0xFF802000);
    }
    for (int r = 0; r < MH; r++)
        for (int c = 0; c < MW; c++)
            if (G->tile[r][c].torch) {
                float k = flicker((float)(c * 7 + r));
                /* the torch light spills onto the ground in front of the wall */
                r_gdisc((float)c + 0.5f, (float)r + 1.3f, 0.01f, 1.25f * k, 16, RGBA(255, 140, 50, (int)(45 * k)), 0xFF600000);
            }
    /* a scorch glow where lightning just hit */
    float st = G->flash_t;
    if (st < 1.5f) r_gdisc(G->strike_x, G->strike_z, 0.02f, 1.2f, 16, RGBA(150, 190, 255, (int)(120 * (1.0f - st / 1.5f))), 0x8090FF00);
    gfx_blend(BL_ALPHA);
}

/* ------------------------------------------------------------------ */
/* scenery                                                             */
/* ------------------------------------------------------------------ */
static void draw_tree(int c, int r)
{
    float x = (float)c + 0.5f + (hf(c, r, 6) - 0.5f) * 0.3f, z = (float)r + 0.5f + (hf(c, r, 7) - 0.5f) * 0.3f;
    float s = 0.8f + 0.45f * hf(c, r, 8);
    if (r >= MH - 1) s *= 0.55f;                    /* the near row stays low so it does not hide the map */
    if (!on_screen(x, 1.0f * s, z, 90.0f)) return;
    float sway = sinf(T * 1.3f + hf(c, r, 9) * 6.0f) * 0.05f * G->wind;
    float rot = hf(c, r, 10) * 3.0f;
    r_light_model(x, 0.9f * s, z);
    static const uint32_t TC[3] = { 0x2C6A48FF, 0x35795AFF, 0x40885EFF };
    float tint = 0.85f + 0.3f * hf(c, r, 11);
    int far = !in_map(c, r);
    if (!far) r_prism(x, 0, z, 0.09f * s, 0.07f * s, 0.35f * s, 5, rot, 0x6A4630FF, 0x5A3A28FF);
    for (int k = far; k < 3; k++) {
        float y0 = (0.25f + 0.38f * (float)k) * s, y1 = y0 + (0.75f - 0.1f * (float)k) * s;
        float rb = (0.55f - 0.12f * (float)k) * s;
        if (far && k == 1) { y0 = 0.1f * s; rb = 0.5f * s; }
        uint32_t col = scale_rgb(TC[k], tint);
        r_prism(x + sway * (float)(k + 1), y0, z, rb, 0.0f, y1, far ? 6 : 7, rot + (float)k * 0.4f, col, col);
    }
    r_light_off();
}

static void draw_bush(int c, int r)
{
    float x = (float)c + 0.5f, z = (float)r + 0.5f;
    if (!on_screen(x, 0.3f, z, 60.0f)) return;
    float s = 0.6f + 0.4f * hf(c, r, 12);
    r_light_model(x, 0.2f, z);
    r_crystal(x, 0.18f * s, z, 0.38f * s, 0.25f * s, 0.18f * s, 6, hf(c, r, 13) * 3.0f, 0x2E6440FF);
    r_light_off();
}

/* a thin flat strip on a box face, for brick mortar lines */
static void mortar_top(float x0, float z0, float x1, float z1, float y, uint32_t c)
{
    float p[4][3] = { {x0, y, z0}, {x1, y, z0}, {x1, y, z1}, {x0, y, z1} };
    r_face(4, (const float (*)[3])p, 0, 1, 0, c);
}
static void mortar_front(float x0, float y0, float x1, float y1, float z, uint32_t c)
{
    float p[4][3] = { {x0, y1, z}, {x1, y1, z}, {x1, y0, z}, {x0, y0, z} };
    r_face(4, (const float (*)[3])p, 0, 0, 1, c);
}

static void draw_wall(int c, int r, const Tile *t)
{
    float x0 = (float)c, z0 = (float)r, x1 = x0 + 1, z1 = z0 + 1, h = t->h;
    int skip = 0;
    if (solid(c - 1, r)) skip |= SK_LEFT;
    if (solid(c + 1, r)) skip |= SK_RIGHT;
    if (solid(c, r + 1)) skip |= SK_FRONT;
    if (solid(c, r - 1)) skip |= SK_BACK;
    float v = 0.9f + 0.2f * hf(c, r, 14);
    uint32_t top = scale_rgb(0xA05C46FF, v), side = scale_rgb(0x844A38FF, v), line = 0x4E2C22FF;
    r_box(x0, 0, z0, x1, h, z1, top, side, skip);
    r_light_model(x0 + 0.5f, h, z0 + 0.5f);
    /* bricks on the top */
    float y = h + 0.003f, w = 0.025f;
    for (int k = 1; k < 3; k++) {
        float zz = z0 + (float)k / 3.0f;
        mortar_top(x0, zz - w, x1, zz + w, y, line);
    }
    for (int k = 0; k < 3; k++) {
        float off = (k & 1) ? 0.25f : 0.5f;
        float za = z0 + (float)k / 3.0f, zb = za + 1.0f / 3.0f;
        for (float xx = x0 + off; xx < x1 - 0.05f; xx += 0.5f) mortar_top(xx - w, za, xx + w, zb, y, line);
    }
    if (!(skip & SK_FRONT)) {
        float zf = z1 + 0.003f;
        mortar_front(x0, h * 0.5f - w, x1, h * 0.5f + w, zf, line);
        for (int k = 0; k < 2; k++) {
            float off = k ? 0.25f : 0.5f, ya = k ? h * 0.5f : 0.0f, yb = ya + h * 0.5f;
            for (float xx = x0 + off; xx < x1 - 0.05f; xx += 0.5f) mortar_front(xx - w, ya, xx + w, yb, zf, line);
        }
    }
    r_light_off();
}

static void draw_torch(int c, int r, const Tile *t)
{
    float h = t->h, tx = (float)c + 0.5f, tz = (float)r + 0.5f, k = flicker((float)(c * 7 + r));
    r_light_model(tx, h + 0.4f, tz);
    r_prism(tx, h, tz, 0.07f, 0.05f, h + 0.32f, 5, 0, 0x5A4030FF, 0x4A3020FF);
    r_prism(tx, h + 0.3f, tz, 0.1f, 0.12f, h + 0.38f, 6, 0, 0x707070FF, 0x505058FF);
    r_light_off();
    gfx_blend(BL_ADD);
    r_unlit = 1;
    r_prism(tx, h + 0.36f, tz, 0.09f * k, 0.0f, h + 0.36f + 0.28f * k, 5, T * 3.0f, RGBA(255, 150, 50, 200), RGBA(255, 120, 30, 200));
    r_prism(tx, h + 0.36f, tz, 0.05f, 0.0f, h + 0.36f + 0.17f * k, 5, -T * 4.0f, RGBA(255, 230, 140, 255), RGBA(255, 220, 120, 255));
    r_unlit = 0;
    r_glow(tx, h + 0.5f, tz, 0.55f * k, RGBA(255, 140, 50, 150));
    gfx_blend(BL_ALPHA);
}

static void draw_block(int c, int r, const Tile *t)
{
    float x0 = (float)c + 0.03f, z0 = (float)r + 0.03f, x1 = (float)c + 0.97f, z1 = (float)r + 0.97f;
    uint32_t col = BLOCK_COL[t->color];
    uint32_t side = scale_rgb(col, 0.82f), top = col;
    uint32_t cap = col_mix(col, 0xFFFFFFFF, 0.22f);
    r_box(x0, 0, z0, x1, t->h - 0.06f, z1, top, side, 0);
    r_box(x0 + 0.08f, t->h - 0.06f, z0 + 0.08f, x1 - 0.08f, t->h, z1 - 0.08f, cap, col, 0);
}

static void draw_campfire(void)
{
    float fv = G->fire_vis;
    r_light_model(FIRE_X, 0.6f, FIRE_Z + 1.2f);
    /* stone ring */
    for (int i = 0; i < 12; i++) {
        float a = (float)i / 12.0f * TAU + 0.13f;
        float x = FIRE_X + cosf(a) * 0.95f, z = FIRE_Z + sinf(a) * 0.95f;
        float v = 0.85f + 0.3f * hf(i, 3, 15);
        r_obox(x, 0, z, 0.2f, 0.13f, 0.17f + 0.05f * hf(i, 4, 16), a + 1.5708f, scale_rgb(0xA8A098FF, v), scale_rgb(0x8A8278FF, v));
    }
    /* logs */
    for (int i = 0; i < 4; i++) {
        float a = (float)i * 1.5708f + 0.4f;
        r_beam(FIRE_X - cosf(a) * 0.45f, 0.07f, FIRE_Z - sinf(a) * 0.45f, cosf(a), 0.32f, sinf(a), 0.62f, 0.13f, 0.13f, 0x6A4028FF);
    }
    r_light_off();
    r_gdisc(FIRE_X, FIRE_Z, 0.02f, 0.38f, 16, RGBA(255, 120, 40, (int)(255 * fv)), RGBA(120, 30, 10, (int)(200 * fv)));
    if (fv < 0.03f) return;
    /* flames: additive flickering cones */
    gfx_blend(BL_ADD);
    r_unlit = 1;
    for (int i = 0; i < 7; i++) {
        float ph = (float)i * 1.7f;
        float a = (float)i / 7.0f * TAU + T * 0.7f;
        float rr = i == 0 ? 0.0f : 0.18f;
        float x = FIRE_X + cosf(a) * rr, z = FIRE_Z + sinf(a) * rr;
        float hgt = (0.7f + 0.35f * sinf(T * 9.0f + ph) * sinf(T * 5.3f + ph * 2.0f)) * fv * (i == 0 ? 1.6f : 1.0f) * (1.0f + G->fire_hit * 0.4f);
        float w = (i == 0 ? 0.3f : 0.17f) * (0.6f + 0.4f * fv);
        float lean = G->wind * 0.12f;
        r_prism(x, 0.1f, z, w, 0.0f, 0.1f + hgt, 5, T * 2.0f + ph, RGBA(255, 110, 30, 150), RGBA(255, 90, 20, 150));
        r_prism(x + lean * 0.5f, 0.12f, z, w * 0.55f, 0.0f, 0.12f + hgt * 0.7f, 5, -T * 3.0f + ph, RGBA(255, 210, 100, 200), RGBA(255, 190, 80, 200));
    }
    r_unlit = 0;
    float fl = flicker(0.0f);
    r_glow(FIRE_X, 0.6f, FIRE_Z, 1.5f * fv * fl, RGBA(255, 140, 50, 170));
    r_glow(FIRE_X, 0.45f, FIRE_Z, 0.6f * fv, RGBA(255, 230, 160, 200));
    gfx_blend(BL_ALPHA);
}

/* ------------------------------------------------------------------ */
/* towers                                                              */
/* ------------------------------------------------------------------ */
static void pips(float x, float y, float z, int lvl, uint32_t c)
{
    for (int i = 0; i <= lvl; i++) {
        float px = x + ((float)i - (float)lvl * 0.5f) * 0.13f;
        r_glow_cheap(px, y, z + 0.34f, 0.07f, c);
    }
}

/* draws one tower model at (x, z) standing on height h */
static void tower_model(int type, int lvl, float x, float h, float z, float ang, float anim, float recoil, float pulse, float built, int glow)
{
    const TowerDef *d = &TOWERS[type];
    float s = 0.4f + 0.6f * built;            /* pops up when built */
    float bh = h + 0.13f * s;
    uint32_t base_t = 0xD8DCE6FF, base_s = 0xA8ACBAFF;
    r_prism(x, h, z, 0.38f, 0.34f, bh, 8, 0.3927f, base_t, base_s);
    float ca = cosf(ang), sa = sinf(ang);
    switch (type) {
    case TW_BLASTER: {
        float y0 = bh, y1 = bh + 0.3f * s;
        int back = sa < 0;                    /* barrel points away from the camera: draw it first */
        float bl = (0.4f + 0.06f * (float)lvl) - recoil * 0.08f, bw = 0.09f + 0.02f * (float)lvl;
        for (int pass = 0; pass < 2; pass++) {
            if ((pass == 0) == back) {
                int nb = lvl == 2 ? 2 : 1;
                for (int b = 0; b < nb; b++) {
                    float off = nb == 2 ? (b ? 0.07f : -0.07f) : 0.0f;
                    r_beam(x + ca * 0.08f - sa * off, y0 + 0.17f * s, z + sa * 0.08f + ca * off, ca, 0.0f, sa, bl * s, bw, bw, 0xE8ECF4FF);
                }
            } else {
                r_obox(x, y0, z, 0.2f * s, 0.19f * s, y1, ang, 0x4CA0FFFF, 0x3474D8FF);
                r_prism(x, y1, z, 0.17f * s, 0.09f * s, y1 + 0.1f * s, 6, ang, 0x7CC0FFFF, 0x4C8CE8FF);
            }
        }
        if (glow) {
            gfx_blend(BL_ADD);
            float mx = x + ca * (0.08f + bl), mz = z + sa * (0.08f + bl);
            r_glow_cheap(mx, y0 + 0.17f, mz, 0.12f + recoil * 0.2f, RGBA(120, 230, 255, (int)(110 + 140 * recoil)));
            r_glow_cheap(x, y1 + 0.12f, z, 0.1f, RGBA(120, 220, 255, 160));
            gfx_blend(BL_ALPHA);
        }
        break;
    }
    case TW_MORTAR: {
        float y0 = bh, y1 = bh + (0.32f + 0.04f * (float)lvl) * s;
        float rb = (0.3f + 0.02f * (float)lvl) * s;
        r_prism(x, y0, z, rb, rb + 0.06f * s, y1, 8, 0.2f, 0x3A2A2AFF, 0xE0583CFF);
        r_prism(x, y1, z, rb + 0.08f * s, rb + 0.08f * s, y1 + 0.05f, 8, 0.2f, 0xF07850FF, 0xC04830FF);
        r_gdisc(x, z, y1 + 0.052f, rb * 0.8f, 16, 0x201010FF, 0x301818FF);
        /* the stubby barrel tilts towards the target */
        float k = 0.55f;
        r_beam(x - ca * 0.05f, y1 - 0.02f - recoil * 0.06f, z - sa * 0.05f, ca * k, 0.9f, sa * k, 0.3f * s, 0.2f + 0.03f * (float)lvl, 0.2f, 0x54545CFF);
        if (glow) {
            gfx_blend(BL_ADD);
            r_glow(x, y1 + 0.08f, z, 0.32f + recoil * 0.3f, RGBA(255, 150, 60, (int)(130 + 120 * recoil)));
            gfx_blend(BL_ALPHA);
        }
        for (int i = 0; i < lvl; i++) {
            float a = (float)i * 3.14159f + 0.8f;
            r_obox(x + cosf(a) * (rb + 0.04f), y0, z + sinf(a) * (rb + 0.04f), 0.06f, 0.06f, y0 + 0.22f, a, 0xF5C040FF, 0xC89030FF);
        }
        break;
    }
    case TW_FROST: {
        float y0 = bh;
        r_prism(x, y0, z, 0.15f * s, 0.11f * s, y0 + 0.36f * s, 6, 0.0f, 0xC8E8F4FF, 0x8EB4CCFF);
        r_prism(x, y0 + 0.36f * s, z, 0.2f * s, 0.2f * s, y0 + 0.42f * s, 6, 0.0f, 0xE0F4FFFF, 0xA0C4DCFF);
        float bob = sinf(anim * 2.2f) * 0.05f;
        float cy = y0 + (0.72f + pulse * 0.08f) * s + bob;
        r_unlit = 1;
        uint32_t cc = col_mix(0x6CD8F8FF, 0xE8FFFFFF, pulse);
        r_crystal(x, cy, z, (0.15f + 0.02f * (float)lvl) * s, 0.27f * s, 0.2f * s, 6, anim * 1.3f, cc);
        for (int i = 0; i < lvl; i++) {
            float a = anim * 2.0f + (float)i * 3.14159f;
            r_crystal(x + cosf(a) * 0.32f, cy - 0.05f, z + sinf(a) * 0.32f, 0.05f, 0.1f, 0.07f, 4, anim * 3.0f, 0xA8F0FFFF);
        }
        r_unlit = 0;
        if (glow) {
            gfx_blend(BL_ADD);
            r_glow(x, cy, z, 0.45f + pulse * 0.4f, RGBA(120, 220, 255, (int)(110 + 120 * pulse)));
            gfx_blend(BL_ALPHA);
        }
        (void)d;
        break;
    }
    case TW_TESLA: {
        float y0 = bh, y1 = y0 + (0.72f + 0.06f * (float)lvl) * s;
        r_prism(x, y0, z, 0.18f * s, 0.18f * s, y0 + 0.12f, 6, 0.0f, 0xF8C838FF, 0xD0A028FF);
        r_prism(x, y0 + 0.12f, z, 0.09f * s, 0.07f * s, y1, 6, 0.0f, 0x40404CFF, 0x30303CFF);
        for (int i = 0; i < 2 + lvl; i++) {
            float ry = y0 + 0.2f + (float)i * (y1 - y0 - 0.3f) / (float)(1 + lvl);
            float rr = (0.2f - 0.03f * (float)i) * s;
            r_prism(x, ry, z, rr, rr, ry + 0.05f, 8, 0.0f, 0xFFD848FF, 0xD8A830FF);
        }
        r_unlit = 1;
        uint32_t oc = col_mix(0xFFE890FF, 0xFFFFFFFF, pulse);
        r_crystal(x, y1 + 0.12f, z, 0.12f * s, 0.12f * s, 0.12f * s, 6, anim * 2.0f, oc);
        r_unlit = 0;
        if (glow) {
            gfx_blend(BL_ADD);
            r_glow(x, y1 + 0.12f, z, 0.38f + pulse * 0.5f, RGBA(255, 240, 140, (int)(130 + 120 * pulse)));
            /* idle crackle */
            float ph = fmodf(anim * 3.7f, 1.0f);
            if (ph < 0.25f) {
                float a = anim * 13.0f;
                r_line(x, y1 + 0.12f, z, x + cosf(a) * 0.25f, y1 - 0.05f, z + sinf(a) * 0.25f, 1.6f, 0xFFF8C0E0, 0xFFF0A000);
            }
            gfx_blend(BL_ALPHA);
        }
        break;
    }
    }
    if (glow) {
        gfx_blend(BL_ADD);
        pips(x, h + 0.08f, z, lvl, d->glow & 0xFFFFFFD0);
        gfx_blend(BL_ALPHA);
    }
}

static void draw_tower_at(int c, int r)
{
    const Tower *t = &G->tw[r][c];
    const Tile *tl = &G->tile[r][c];
    float x = (float)c + 0.5f, z = (float)r + 0.5f;
    r_gdisc(x, z, tl->h + 0.004f, 0.5f, 16, 0x00000070, 0x00000000);
    r_scale(x, tl->h, z, TOWER_K);
    r_light_model(x, tl->h + 0.5f, z);
    tower_model(t->type, t->lvl, x, tl->h, z, t->ang, t->anim, t->recoil, t->pulse, t->built, 1);
    r_light_off();
    r_scale_off();
}

/* ------------------------------------------------------------------ */
/* enemies                                                             */
/* ------------------------------------------------------------------ */
static uint32_t en_tint(const Enemy *e, uint32_t c)
{
    if (e->slow_t > 0) c = col_mix(c, 0xB0F0FFFF, 0.5f);
    if (e->zap_t > 0) c = col_mix(c, 0xFFF6B0FF, 0.6f);
    if (e->hit_t > 0) c = col_mix(c, 0xFFFFFFFF, clampf(e->hit_t * 6.0f, 0, 0.8f));
    return c;
}

static void eyes(float x, float y, float z, float ang, float fwd, float sep, float rad, uint32_t c)
{
    float ca = cosf(ang), sa = sinf(ang);
    for (int k = -1; k <= 1; k += 2) {
        float ex = x + ca * fwd - sa * sep * (float)k, ez = z + sa * fwd + ca * sep * (float)k;
        r_glow_cheap(ex, y, ez, rad, c);
    }
}

static void draw_enemy_model(const Enemy *e);
static void draw_enemy(const Enemy *e)
{
    r_scale(e->x, 0.0f, e->z, ENEMY_K);
    r_light_model(e->x, e->y + 0.4f, e->z);
    draw_enemy_model(e);
    r_light_off();
    r_scale_off();
}

static void draw_enemy_model(const Enemy *e)
{
    const EnemyDef *d = &ENEMIES[e->type];
    float s = d->size, x = e->x, z = e->z, a = e->ang;
    float ca = cosf(a), sa = sinf(a);
    if (!on_screen(x, 0.4f, z, 60.0f)) return;
    if (!e->fly) r_gdisc(x, z, 0.006f, s * 0.95f, 16, 0x00000078, 0x00000000);
    switch (e->type) {
    case EN_CRAWLER: {
        float bob = fabsf(sinf(e->anim * 2.0f)) * 0.04f;
        float y = 0.14f + bob;
        for (int k = 0; k < 3; k++) {          /* scuttling legs */
            float o = ((float)k - 1.0f) * 0.13f, sw = sinf(e->anim * 2.0f + (float)k * 2.0f) * 0.05f;
            r_beam(x + ca * (o + sw) - sa * 0.05f, 0.03f, z + sa * (o + sw) + ca * 0.05f, -sa, 0.0f, ca, 0.0f + 0.1f, 0.04f, 0.04f, 0x203040FF);
            r_beam(x + ca * (o - sw) + sa * 0.05f, 0.03f, z + sa * (o - sw) - ca * 0.05f, sa, 0.0f, -ca, 0.1f, 0.04f, 0.04f, 0x203040FF);
        }
        r_crystal(x, y, z, s, s * 0.55f, s * 0.45f, 6, a, en_tint(e, 0x3CB4E6FF));
        gfx_blend(BL_ADD);
        eyes(x, y + 0.04f, z, a, s * 0.75f, 0.06f, 0.06f, 0xFF5040FF);
        gfx_blend(BL_ALPHA);
        break;
    }
    case EN_GRUNT: {
        float step = sinf(e->anim * 2.0f);
        for (int k = -1; k <= 1; k += 2) {     /* feet */
            float f = step * 0.08f * (float)k;
            float fx = x + ca * f - sa * 0.1f * (float)k, fz = z + sa * f + ca * 0.1f * (float)k;
            r_obox(fx, 0, fz, 0.07f, 0.06f, 0.1f + fmaxf(0, step * (float)k) * 0.05f, a, 0x40304CFF, 0x302438FF);
        }
        float y0 = 0.1f + fabsf(step) * 0.03f;
        uint32_t body = en_tint(e, 0x9A5CE8FF);
        r_obox(x, y0, z, 0.17f, 0.15f, y0 + 0.32f, a, body, scale_rgb(body, 0.85f));
        r_obox(x + ca * 0.02f, y0 + 0.32f, z + sa * 0.02f, 0.12f, 0.12f, y0 + 0.5f, a, en_tint(e, 0xB888F8FF), en_tint(e, 0x8858C8FF));
        gfx_blend(BL_ADD);
        float vx = x + ca * 0.15f, vz = z + sa * 0.15f;
        r_line(vx - sa * 0.08f, y0 + 0.42f, vz + ca * 0.08f, vx + sa * 0.08f, y0 + 0.42f, vz - ca * 0.08f, 3.0f, 0xFF4040FF, 0xFF4040FF);
        r_glow_cheap(vx, y0 + 0.42f, vz, 0.12f, 0xFF3030A0);
        gfx_blend(BL_ALPHA);
        break;
    }
    case EN_BRUTE: {
        float step = sinf(e->anim * 1.6f);
        float y0 = 0.12f + fabsf(step) * 0.04f;
        for (int k = -1; k <= 1; k += 2) {
            float f = step * 0.12f * (float)k;
            r_obox(x + ca * f - sa * 0.2f * (float)k, 0, z + sa * f + ca * 0.2f * (float)k, 0.12f, 0.11f, 0.2f, a, 0x40201CFF, 0x301814FF);
        }
        uint32_t body = en_tint(e, 0xB84034FF);
        r_obox(x, y0, z, 0.3f, 0.26f, y0 + 0.46f, a, body, scale_rgb(body, 0.82f));
        for (int k = -1; k <= 1; k += 2) {      /* shoulders + horns */
            float sx = x - sa * 0.32f * (float)k, sz = z + ca * 0.32f * (float)k;
            r_obox(sx, y0 + 0.24f, sz, 0.11f, 0.13f, y0 + 0.5f, a, en_tint(e, 0xD05844FF), en_tint(e, 0x903028FF));
            r_prism(sx, y0 + 0.5f, sz, 0.06f, 0.0f, y0 + 0.72f, 4, a, 0xE8DCC8FF, 0xC8B8A0FF);
        }
        gfx_blend(BL_ADD);
        r_glow(x + ca * 0.3f, y0 + 0.28f, z + sa * 0.3f, 0.22f, 0xFF9030C0);
        eyes(x, y0 + 0.4f, z, a, 0.3f, 0.09f, 0.07f, 0xFFC040FF);
        gfx_blend(BL_ALPHA);
        break;
    }
    case EN_WISP: {
        float y = e->y;
        r_gdisc(x, z, 0.006f, 0.3f, 16, 0x00000050, 0x00000000);
        gfx_blend(BL_ADD);
        r_unlit = 1;
        float flap = sinf(e->anim * 4.0f) * 0.7f;
        for (int k = -1; k <= 1; k += 2) {
            float px = -sa * (float)k, pz = ca * (float)k;
            float wx = x + px * 0.35f * cosf(flap), wy = y + 0.35f * sinf(flap) * 0.6f, wz = z + pz * 0.35f * cosf(flap);
            float p[3][3] = { {x + ca * 0.1f, y, z + sa * 0.1f}, {x - ca * 0.12f, y, z - sa * 0.12f}, {wx, wy, wz} };
            float sx[3], sy[3];
            for (int i = 0; i < 3; i++) proj(p[i][0], p[i][1], p[i][2], &sx[i], &sy[i]);
            gfx_tri(sx[0], sy[0], 0x80FFD0A0, sx[1], sy[1], 0x80FFD0A0, sx[2], sy[2], 0x40C0A030);
        }
        r_crystal(x, y, z, 0.1f, 0.12f, 0.12f, 5, T * 3.0f, en_tint(e, 0xC8FFE8FF));
        r_unlit = 0;
        r_glow(x, y, z, 0.45f, 0x60FFC090);
        gfx_blend(BL_ALPHA);
        break;
    }
    case EN_GOLEM: {
        float step = sinf(e->anim * 1.2f);
        float y0 = 0.25f + fabsf(step) * 0.05f;
        for (int k = -1; k <= 1; k += 2) {
            float f = step * 0.2f * (float)k;
            r_obox(x + ca * f - sa * 0.3f * (float)k, 0, z + sa * f + ca * 0.3f * (float)k, 0.17f, 0.17f, y0 + 0.1f, a, 0x5C646CFF, 0x484E56FF);
        }
        uint32_t stone = en_tint(e, 0x7C8894FF);
        r_obox(x, y0, z, 0.45f, 0.36f, y0 + 0.75f, a, stone, scale_rgb(stone, 0.8f));
        r_obox(x + ca * 0.06f, y0 + 0.75f, z + sa * 0.06f, 0.22f, 0.2f, y0 + 1.05f, a, en_tint(e, 0x8C98A4FF), scale_rgb(stone, 0.75f));
        for (int k = -1; k <= 1; k += 2) {      /* swinging arms */
            float sw = step * 0.25f * (float)k;
            float ax = x - sa * 0.58f * (float)k + ca * sw, az = z + ca * 0.58f * (float)k + sa * sw;
            r_obox(ax, y0 + 0.05f, az, 0.14f, 0.14f, y0 + 0.72f, a, stone, scale_rgb(stone, 0.78f));
        }
        gfx_blend(BL_ADD);
        eyes(x, y0 + 0.92f, z, a, 0.24f, 0.08f, 0.09f, 0x60E0FFFF);
        float pulse = 0.6f + 0.4f * sinf(T * 4.0f);
        r_glow(x + ca * 0.46f, y0 + 0.45f, z + sa * 0.46f, 0.35f, RGBA(80, 200, 255, (int)(160 * pulse)));
        gfx_blend(BL_ALPHA);
        break;
    }
    }
}

/* ------------------------------------------------------------------ */
/* the world, back to front                                            */
/* ------------------------------------------------------------------ */
static int col_order[MW + 12];
static int built_static;

static void sort_columns(void)
{
    /* columns from the outside in, so side faces stack correctly in the perspective */
    int n = 0;
    for (int c = -6; c < MW + 6; c++) col_order[n++] = c;
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++) {
            float di = fabsf((float)col_order[i] + 0.5f - cam.lx), dj = fabsf((float)col_order[j] + 0.5f - cam.lx);
            if (dj > di) { int t = col_order[i]; col_order[i] = col_order[j]; col_order[j] = t; }
        }
}

/* the scenery (ground, trees, walls, blocks) is projected once; only its lighting changes */
static void build_static(void)
{
    cam_world(0, 0);
    sort_columns();
    lt_begin();
    lt_build(0, 1);
    rc_begin();
    rc_slot(SLOT_GROUND);
    build_ground();
    for (int r = ROW_MIN; r < ROW_MAX; r++) {
        rc_slot(SLOT_ROW0 + r - ROW_MIN);
        for (int i = 0; i < MW + 12; i++) {
            int c = col_order[i];
            if (!in_map(c, r)) {
                if (outside_lane(c, r)) continue;
                if (hf(c, r, 2) < 0.72f) draw_tree(c, r);
                else if (hf(c, r, 2) < 0.85f) draw_bush(c, r);
                continue;
            }
            const Tile *t = &G->tile[r][c];
            switch (t->kind) {
            case TL_TREE: draw_tree(c, r); break;
            case TL_WALL: draw_wall(c, r, t); break;
            case TL_BLOCK: draw_block(c, r, t); break;
            default: break;
            }
        }
    }
    rc_end();
    built_static = 1;
}

static void draw_world(void)
{
    int ghost = G->state == ST_PLAY && can_build(G, G->cx, G->cz);
    for (int r = ROW_MIN; r < ROW_MAX; r++) {
        rc_draw(SLOT_ROW0 + r - ROW_MIN, SHX, SHY);
        if (r >= 0 && r < MH)
            for (int i = 0; i < MW + 12; i++) {
                int c = col_order[i];
                if (!in_map(c, r)) continue;
                const Tile *t = &G->tile[r][c];
                if (t->torch && G->tw[r][c].type < 0) draw_torch(c, r, t);
                if (c == 9 && r == 6) draw_campfire();
                if (G->tw[r][c].type >= 0) draw_tower_at(c, r);
                if (ghost && c == G->cx && r == G->cz) {
                    int afford = G->gold >= tower_cost(G->sel, 0);
                    r_alpha = afford ? 0.55f : 0.35f;
                    r_scale((float)c + 0.5f, t->h, (float)r + 0.5f, TOWER_K);
                    r_light_model((float)c + 0.5f, t->h + 0.5f, (float)r + 0.5f);
                    tower_model(G->sel, 0, (float)c + 0.5f, t->h, (float)r + 0.5f, -1.5708f + sinf(T) * 0.4f, T, 0, 0, 1, 0);
                    r_alpha = 1.0f;
                    r_light_off();
                    r_scale_off();
                }
            }
        /* walkers whose feet are in this row */
        for (int i = 0; i < MAX_EN; i++) {
            const Enemy *e = &G->en[i];
            if (!e->alive || e->fly) continue;
            int er = (int)floorf(e->z);
            if (er < ROW_MIN) er = ROW_MIN;
            if (er >= ROW_MAX) er = ROW_MAX - 1;
            if (er == r) draw_enemy(e);
        }
    }
    for (int i = 0; i < MAX_EN; i++) if (G->en[i].alive && G->en[i].fly) draw_enemy(&G->en[i]);
}

/* ------------------------------------------------------------------ */
/* effects                                                             */
/* ------------------------------------------------------------------ */
static void draw_effects(void)
{
    /* solid bits first */
    for (int i = 0; i < MAX_PT; i++) {
        const Part *p = &G->pt[i];
        if (p->life <= 0) continue;
        float k = p->life / p->max;
        if (p->kind == PT_DEBRIS) {
            uint32_t c = p->c;
            r_light_model(p->x, p->y, p->z);
            r_obox(p->x, p->y - p->size * 0.5f, p->z, p->size * 0.5f, p->size * 0.5f, p->y + p->size * 0.5f, p->rot, c, scale_rgb(c, 0.75f));
            r_light_off();
        } else if (p->kind == PT_SMOKE || p->kind == PT_STEAM) {
            uint32_t c = col_a(p->c, k * (1.0f - k) * 3.0f);
            float l[3];
            lt_at(p->x, p->y, p->z, l);
            if (p->kind == PT_SMOKE) c = (scale_rgb(c, clampf((l[0] + l[1] + l[2]) * 0.6f, 0.4f, 1.6f)) & 0xFFFFFF00) | (c & 255);
            r_glow(p->x, p->y, p->z, p->size, c);
        }
    }
    for (int i = 0; i < MAX_SH; i++) {
        const Shot *s = &G->sh[i];
        if (s->alive && s->kind == SH_SHELL) {
            r_gdisc(s->x, s->z, 0.01f, 0.18f, 16, 0x00000050, 0x00000000);
            r_prism(s->x, s->y - 0.09f, s->z, 0.08f, 0.08f, s->y + 0.09f, 6, T * 5.0f, 0x303038FF, 0x202028FF);
        }
    }

    gfx_blend(BL_ADD);
    for (int i = 0; i < MAX_PT; i++) {
        const Part *p = &G->pt[i];
        if (p->life <= 0) continue;
        float k = p->life / p->max;
        switch (p->kind) {
        case PT_SPARK:
            r_line(p->x, p->y, p->z, p->x - p->vx * 0.045f, p->y - p->vy * 0.045f, p->z - p->vz * 0.045f, 2.0f, col_a(p->c, k), col_a(p->c, 0));
            break;
        case PT_EMBER: case PT_FLAKE:
            r_glow_cheap(p->x, p->y, p->z, p->size * (0.6f + 0.6f * k) * 2.0f, col_a(p->c, k));
            break;
        case PT_RING: {
            float u = 1.0f - k, rr = lerpf(p->size, p->vx, 1.0f - (1.0f - u) * (1.0f - u));
            r_gring(p->x, p->z, p->y, rr, 0.06f + 0.06f * k, 32, col_a(p->c, k));
            break;
        }
        case PT_SPLASH: {
            float u = 1.0f - k;
            r_gring(p->x, p->z, p->y, p->size * (0.3f + u), 0.025f, 10, col_a(p->c, k));
            break;
        }
        case PT_GLOW:
            r_glow(p->x, p->y, p->z, p->size * (1.4f - k * 0.4f), col_a(p->c, k));
            break;
        default: break;
        }
    }
    for (int i = 0; i < MAX_SH; i++) {
        const Shot *s = &G->sh[i];
        if (!s->alive) continue;
        if (s->kind == SH_BOLT) {
            float sx, sy, tx, ty;
            proj(s->x, s->y, s->z, &sx, &sy);
            proj(s->x - s->vx * 0.035f, s->y - s->vy * 0.035f, s->z - s->vz * 0.035f, &tx, &ty);
            gfx_capsule(sx, sy, sx - tx, sy - ty, sqrtf((sx - tx) * (sx - tx) + (sy - ty) * (sy - ty)), 4.0f + (float)s->lvl, 0xA0F4FFFF);
            r_glow_cheap(s->x, s->y, s->z, 0.16f, 0x60D8FFB0);
        } else {
            r_glow(s->x, s->y, s->z, 0.3f, 0xFF9040D0);
            r_glow_cheap(s->x, s->y, s->z, 0.12f, 0xFFE0A0FF);
        }
    }
    for (int i = 0; i < MAX_ARC; i++) {
        const Arc *a = &G->arc[i];
        if (!a->alive) continue;
        float k = 1.0f - a->t / a->life;
        if (a->storm) k = k * (0.5f + 0.5f * (sinf(a->t * 70.0f) > -0.3f));
        float w = a->storm ? 5.0f : 2.4f;
        for (int j = 0; j + 1 < a->n; j++) {
            r_glowline(a->p[j][0], a->p[j][1], a->p[j][2], a->p[j + 1][0], a->p[j + 1][1], a->p[j + 1][2], w * 5.0f, col_a(a->c, k * 0.45f));
            r_line(a->p[j][0], a->p[j][1], a->p[j][2], a->p[j + 1][0], a->p[j + 1][1], a->p[j + 1][2], w, col_a(0xFFFFFFFF, k), col_a(0xFFFFFFFF, k));
        }
        if (!a->storm) r_glow_cheap(a->p[a->n - 1][0], a->p[a->n - 1][1], a->p[a->n - 1][2], 0.3f, col_a(a->c, k * 0.8f));
    }
    gfx_blend(BL_ALPHA);
}

static void draw_rain(void)
{
    float f = G->flash;
    uint32_t c = RGBA(170, 190, 235, (int)(60 + 110 * clampf(f, 0, 1)));
    for (int i = 0; i < MAX_DROP; i++) {
        const Drop *d = &G->drop[i];
        float ax, ay, bx, by;
        proj(d->x, d->y, d->z, &ax, &ay);
        proj(d->x - G->wind * 0.16f * d->len, d->y + d->len, d->z, &bx, &by);
        gfx_line(ax, ay, bx, by, 1.3f, c, c & 0xFFFFFF00);
    }
    /* big soft streaks right in front of the camera */
    uint32_t cn = RGBA(190, 205, 240, (int)(26 + 50 * clampf(f, 0, 1)));
    for (int i = 0; i < 46; i++) {
        float sp = 1.6f + (float)(i % 5) * 0.25f;
        float ph = fmodf(T * sp + (float)i * 0.618f, 1.0f);
        float x = fmodf((float)i * 97.3f + T * 40.0f * G->wind, VW + 80.0f) - 40.0f;
        float y = ph * (VH + 200.0f) - 100.0f;
        float len = 70.0f + (float)(i % 3) * 30.0f;
        gfx_line(x, y, x - G->wind * 14.0f, y - len, 2.2f, cn, cn & 0xFFFFFF00);
    }
}

static void draw_mist_vignette(void)
{
    /* slow drifting ground mist */
    for (int i = 0; i < 7; i++) {
        float x = fmodf((float)i * 3.7f + T * 0.25f * (0.6f + 0.2f * (float)i), 26.0f) - 4.0f;
        float z = (float)((i * 5) % 13) + 0.5f;
        r_glow(x, 0.3f, z, 2.6f, RGBA(120, 140, 170, 26));
    }
    /* dark edges */
    gfx_rect_v(0, 0, VW, 70, 0x000000C8, 0x00000000);
    gfx_rect_v(0, VH - 90, VW, 90, 0x00000000, 0x000000D0);
    gfx_quad4(0, 0, 0x000000B0, 90, 0, 0x00000000, 90, VH, 0x00000000, 0, VH, 0x000000B0);
    gfx_quad4(VW - 90, 0, 0x00000000, VW, 0, 0x000000B0, VW, VH, 0x000000B0, VW - 90, VH, 0x00000000);
    if (G->flash > 0.02f) {
        gfx_blend(BL_ADD);
        gfx_rect(0, 0, VW, VH, RGBA(150, 170, 230, (int)(70 * clampf(G->flash, 0, 1))));
        gfx_blend(BL_ALPHA);
    }
}

/* ------------------------------------------------------------------ */
/* cursor & world UI                                                   */
/* ------------------------------------------------------------------ */
static void draw_cursor(void)
{
    int c = G->cx, r = G->cz;
    float h = G->tile[r][c].h;
    float x0 = G->cur_x + 0.04f, z0 = G->cur_z + 0.04f, x1 = G->cur_x + 0.96f, z1 = G->cur_z + 0.96f;
    float y = h + 0.01f;
    const Tower *t = &G->tw[r][c];
    int buildable = can_build(G, c, r) || t->type >= 0;
    uint32_t col = G->err_t > 0 ? RGBA(255, 80, 70, 255) : (buildable ? RGBA(255, 240, 180, 255) : RGBA(170, 180, 200, 200));
    float pulse = 0.7f + 0.3f * sinf(T * 6.0f);
    gfx_blend(BL_ADD);
    r_gquad(x0, z0, x1, z1, y, col_a(col, 0.12f * pulse));
    float w = 0.05f, L = 0.3f;
    uint32_t cc = col_a(col, pulse);
    /* corner brackets */
    r_gquad(x0, z0, x0 + L, z0 + w, y, cc); r_gquad(x0, z0, x0 + w, z0 + L, y, cc);
    r_gquad(x1 - L, z0, x1, z0 + w, y, cc); r_gquad(x1 - w, z0, x1, z0 + L, y, cc);
    r_gquad(x0, z1 - w, x0 + L, z1, y, cc); r_gquad(x0, z1 - L, x0 + w, z1, y, cc);
    r_gquad(x1 - L, z1 - w, x1, z1, y, cc); r_gquad(x1 - w, z1 - L, x1, z1, y, cc);
    /* range ring for the tower under the cursor, or the one about to be built */
    float range = 0;
    uint32_t rc = 0;
    if (t->type >= 0) { range = tower_range(t); rc = TOWERS[t->type].glow; }
    else if (can_build(G, c, r)) { range = TOWERS[G->sel].range[0]; rc = TOWERS[G->sel].glow; }
    if (range > 0) {
        float cx = (float)c + 0.5f, cz = (float)r + 0.5f;
        r_gring(cx, cz, 0.03f, range, 0.05f, 40, col_a(rc, 0.75f));
        r_gdisc(cx, cz, 0.02f, range, 16, col_a(rc, 0.0f), col_a(rc, 0.16f));
    }
    gfx_blend(BL_ALPHA);
}

static void draw_world_ui(void)
{
    /* health bars */
    for (int i = 0; i < MAX_EN; i++) {
        const Enemy *e = &G->en[i];
        if (!e->alive || e->hp >= e->maxhp || e->type == EN_GOLEM) continue;
        const EnemyDef *d = &ENEMIES[e->type];
        float sx, sy;
        proj(e->x, e->y + d->size * 1.9f + 0.25f, e->z, &sx, &sy);
        float w = 14.0f + d->size * 30.0f, f = clampf(e->hp / e->maxhp, 0, 1);
        gfx_rect(sx - w * 0.5f - 1, sy - 1, w + 2, 5, 0x000000B0);
        gfx_rect(sx - w * 0.5f, sy, w * f, 3, f > 0.5f ? 0x60E060FF : (f > 0.25f ? 0xF0C040FF : 0xF05040FF));
    }
    /* floating numbers */
    for (int i = 0; i < MAX_POP; i++) {
        const Pop *p = &G->pop[i];
        if (p->t >= 1.0f) continue;
        float sx, sy;
        proj(p->x, p->y, p->z, &sx, &sy);
        float a = p->t < 0.7f ? 1.0f : (1.0f - p->t) / 0.3f;
        gfx_text_sc(p->txt, sx, sy, 2.0f, col_a(p->c, a));
    }
}

/* ------------------------------------------------------------------ */
/* HUD                                                                 */
/* ------------------------------------------------------------------ */
static void panel(float x, float y, float w, float h, uint32_t c)
{
    float r = 5.0f;
    gfx_rect(x + r, y, w - 2 * r, h, c);
    gfx_rect(x, y + r, r, h - 2 * r, c);
    gfx_rect(x + w - r, y + r, r, h - 2 * r, c);
    gfx_poly(x + r, y + r, r, 6, 0, c); gfx_poly(x + w - r, y + r, r, 6, 0, c);
    gfx_poly(x + r, y + h - r, r, 6, 0, c); gfx_poly(x + w - r, y + h - r, r, 6, 0, c);
}

static void coin_icon(float x, float y, float s)
{
    gfx_poly(x, y, s, 12, 0, 0xB07818FF);
    gfx_poly(x, y - 1, s * 0.85f, 12, 0, 0xF8C838FF);
    gfx_poly(x, y - 1, s * 0.45f, 12, 0, 0xFFE890FF);
}

static void flame_icon(float x, float y, float s, float k)
{
    float w = sinf(T * 9.0f) * s * 0.12f;
    gfx_tri(x - s * 0.6f, y + s * 0.5f, 0xE04020FF, x + s * 0.6f, y + s * 0.5f, 0xE04020FF, x + w, y - s * 1.1f * k, 0xFFA040FF);
    gfx_tri(x - s * 0.32f, y + s * 0.5f, 0xFFB040FF, x + s * 0.32f, y + s * 0.5f, 0xFFB040FF, x - w, y - s * 0.4f * k, 0xFFF0A0FF);
}

static void draw_hud(void)
{
    char buf[64];
    /* gold + fire */
    panel(14, 12, 168, 58, 0x0A0E18B8);
    coin_icon(34, 30, 9);
    uint32_t gcol = col_mix(0xFFE070FF, 0xFFFFFFFF, G->gold_flash);
    snprintf(buf, sizeof buf, "%d", G->gold);
    gfx_text_s(buf, 50, 23, 2.4f, gcol);
    flame_icon(34, 55, 9, 0.6f + 0.4f * G->fire_vis);
    float ff = clampf((float)G->fire / (float)FIRE_HP, 0, 1);
    gfx_rect(50, 50, 120, 10, 0x000000C0);
    gfx_rect_v(51, 51, 118 * ff, 8, col_mix(0xFFC050FF, 0xFFFFFFFF, G->fire_flash), col_mix(0xE05020FF, 0xFF2020FF, G->fire_flash));
    snprintf(buf, sizeof buf, "%d", G->fire);
    gfx_text_sr(buf, 168, 38, 1.4f, 0xFFD8B0FF);

    /* wave status (top right) */
    panel(VW - 214, 12, 200, 58, 0x0A0E18B8);
    snprintf(buf, sizeof buf, "WAVE %d/%d", (!G->wave_on && G->wave < NUM_WAVES) ? G->wave + 1 : G->wave, NUM_WAVES);
    gfx_text_sr(buf, VW - 26, 22, 2.0f, 0xE8F0FFFF);
    if (!G->wave_on && G->wave < NUM_WAVES) {
        snprintf(buf, sizeof buf, "NEXT IN %d", (int)ceilf(G->countdown));
        gfx_text_sr(buf, VW - 26, 42, 1.4f, 0xFFD070FF);
        snprintf(buf, sizeof buf, "X: CALL NOW +%d", (int)G->countdown);
        gfx_text_sr(buf, VW - 26, 55, 1.2f, 0xA8B4D0FF);
    } else {
        int left = 0;
        for (int i = 0; i < MAX_EN; i++) left += G->en[i].alive;
        const WaveDef *w = &WAVES[G->wave > 0 ? G->wave - 1 : 0];
        for (int k = 0; k < w->n; k++) left += w->g[k].count - G->spawned[k];
        snprintf(buf, sizeof buf, "ENEMIES %d", left);
        gfx_text_sr(buf, VW - 26, 44, 1.4f, 0xFF9080FF);
    }
    if (G->fast) {
        float a = 0.6f + 0.4f * sinf(T * 6.0f);
        gfx_tri(VW - 252, 26, col_a(0xFFFFFFFF, a), VW - 252, 46, col_a(0xFFFFFFFF, a), VW - 240, 36, col_a(0xFFFFFFFF, a));
        gfx_tri(VW - 238, 26, col_a(0xFFFFFFFF, a), VW - 238, 46, col_a(0xFFFFFFFF, a), VW - 226, 36, col_a(0xFFFFFFFF, a));
    }

    /* boss bar */
    if (G->boss >= 0 && G->en[G->boss].alive) {
        const Enemy *b = &G->en[G->boss];
        float f = clampf(b->hp / b->maxhp, 0, 1);
        gfx_text_sc("STORM GOLEM", VW * 0.5f, 84, 1.6f, 0xA0E8FFFF);
        gfx_rect(VW * 0.5f - 151, 98, 302, 10, 0x000000C0);
        gfx_rect_v(VW * 0.5f - 150, 99, 300 * f, 8, 0x80E0FFFF, 0x3080D0FF);
    }

    /* banner */
    if (G->banner_t < 4.0f && G->state == ST_PLAY) {
        float bt = G->banner_t;
        float a = bt < 0.25f ? bt / 0.25f : (bt > 3.3f ? (4.0f - bt) / 0.7f : 1.0f);
        float slide = (1.0f - clampf(bt / 0.25f, 0, 1)) * -20.0f;
        gfx_text_sc(G->banner, VW * 0.5f, 22 + slide, 2.2f, col_a(0xF0F4FFFF, a));
        gfx_text_wave(G->banner2, VW * 0.5f + 2, 49 + slide + 3, 3.4f, col_a(0x00000000, a * 0.6f), T, 1.5f);
        gfx_text_wave(G->banner2, VW * 0.5f, 49 + slide, 3.4f, col_a(0xFFFFFFFF, a), T, 1.5f);
    }

    /* where is the cursor on screen? panels it is hiding under turn see-through */
    float csx, csy;
    proj((float)G->cx + 0.5f, G->tile[G->cz][G->cx].h, (float)G->cz + 0.5f, &csx, &csy);

    /* tower cards */
    float cy = VH - 80;
    float cfade = (csx < 300 && csy > cy - 30) ? 0.35f : 1.0f;
    for (int i = 0; i < TW_TYPES; i++) {
        int sel = i == G->sel;
        float cx = 14 + (float)i * 62, lift = sel ? -6.0f - G->sel_t * 4.0f : 0.0f;
        int afford = G->gold >= TOWERS[i].cost[0];
        uint32_t bg = sel ? col_mix(TOWERS[i].color, 0x000000FF, 0.45f) : 0x10141ED0;
        if (sel) panel(cx - 3, cy + lift - 3, 60, 76, col_a(TOWERS[i].glow, 0.9f * cfade));
        panel(cx, cy + lift, 54, 70, col_a(bg, cfade));
        r_alpha = cfade;
        cam_ui(cx + 27, cy + lift + 38, 38.0f);
        tower_model(i, 0, 0, -0.35f, 0, -0.9f + (sel ? T * 1.2f : 0.0f), T, 0, 0, 1, 1);
        cam_restore();
        r_alpha = 1.0f;
        snprintf(buf, sizeof buf, "%d", TOWERS[i].cost[0]);
        coin_icon(cx + 12, cy + lift + 60, 4.5f);
        gfx_text_s(buf, cx + 20, cy + lift + 55, 1.5f, col_a(afford ? 0xFFE070FF : 0xFF6050FF, cfade));
    }
    gfx_text_s("L", 14 + 4 * 62, cy + 14, 1.5f, col_a(0x8090B0FF, cfade));
    gfx_text_s("R", 14 + 4 * 62, cy + 40, 1.5f, col_a(0x8090B0FF, cfade));
    gfx_text_s(TOWERS[G->sel].name, 14, cy - 22, 1.8f, col_a(TOWERS[G->sel].glow, cfade));

    /* info panel for the tile under the cursor */
    const Tower *t = &G->tw[G->cz][G->cx];
    float px = VW - 252, py = VH - 80;
    float ifade = (csx > px - 30 && csy > py - 30) ? 0.4f : 1.0f;
    uint32_t pbg = col_a(0x0A0E18C8, ifade);
    if (t->type >= 0) {
        const TowerDef *d = &TOWERS[t->type];
        panel(px, py, 238, 70, pbg);
        snprintf(buf, sizeof buf, "%s  LV%d", d->name, t->lvl + 1);
        gfx_text_s(buf, px + 10, py + 8, 1.7f, col_a(d->glow, ifade));
        if (t->type == TW_FROST) snprintf(buf, sizeof buf, "SLOW %d%%  RANGE %.1f", (int)(d->extra[t->lvl] * 100), d->range[t->lvl]);
        else snprintf(buf, sizeof buf, "DMG %d  RANGE %.1f", (int)d->dmg[t->lvl], d->range[t->lvl]);
        gfx_text_s(buf, px + 10, py + 26, 1.25f, col_a(0xC8D0E0FF, ifade));
        if (t->lvl + 1 < TW_MAXLVL) {
            int c = d->cost[t->lvl + 1];
            snprintf(buf, sizeof buf, "A: UPGRADE %d", c);
            gfx_text_s(buf, px + 10, py + 41, 1.35f, col_a(G->gold >= c ? 0xFFE070FF : 0xFF7060FF, ifade));
        } else gfx_text_s("MAX LEVEL", px + 10, py + 41, 1.35f, col_a(0x90F090FF, ifade));
        snprintf(buf, sizeof buf, "B: SELL %d", tower_sell_value(t));
        gfx_text_s(buf, px + 10, py + 55, 1.35f, col_a(0xA8B4D0FF, ifade));
    } else if (can_build(G, G->cx, G->cz)) {
        const TowerDef *d = &TOWERS[G->sel];
        panel(px, py, 238, 70, pbg);
        gfx_text_s(d->name, px + 10, py + 8, 1.7f, col_a(d->glow, ifade));
        static const char *DESC[TW_TYPES] = { "FAST SHOTS, HITS FLYERS", "SPLASH, GROUND ONLY", "SLOWS EVERYTHING NEARBY", "CHAIN LIGHTNING" };
        gfx_text_s(DESC[G->sel], px + 10, py + 27, 1.25f, col_a(0xC8D0E0FF, ifade));
        snprintf(buf, sizeof buf, "A: BUILD %d", d->cost[0]);
        gfx_text_s(buf, px + 10, py + 46, 1.5f, col_a(G->gold >= d->cost[0] ? 0xFFE070FF : 0xFF7060FF, ifade));
    }

    /* controls reminder */
    gfx_text_sc("Y: SPEED   +: PAUSE", VW * 0.5f + 20, VH - 14, 1.2f, 0x8C98B4C0);
}

/* ------------------------------------------------------------------ */
/* screens                                                             */
/* ------------------------------------------------------------------ */
static void fire_text(const char *s, float cx, float y, float px)
{
    float w = gfx_text_width(s, px);
    gfx_blend(BL_ADD);
    for (int i = 0; s[i]; i++) {
        float x = cx - w * 0.5f + (float)i * 6.0f * px + 2.5f * px;
        gfx_glow(x, y + 3.5f * px, px * 6.0f, RGBA(255, 110, 30, (int)(55 + 25 * sinf(T * 5.0f + (float)i))));
    }
    gfx_blend(BL_ALPHA);
    gfx_text_c(s, cx + px * 0.5f, y + px * 0.6f, px, 0x401008E0);
    /* two passes: orange body with a yellow upper half */
    gfx_text_c(s, cx, y, px, 0xFF8030FF);
    for (int i = 0; s[i]; i++) {
        const uint8_t *g = gfx_glyph(s[i]);
        if (!g) continue;
        float x = cx - w * 0.5f + (float)i * 6.0f * px;
        for (int r = 0; r < 3; r++)
            for (int b = 0; b < 5; b++)
                if (g[r] & (0x10 >> b)) gfx_rect(x + (float)b * px, y + (float)r * px, px, px, 0xFFD878FF);
    }
}

static void draw_title(void)
{
    gfx_rect(0, 0, VW, VH, 0x02040A70);
    fire_text("STORMFIRE", VW * 0.5f, 120, 10.0f);
    gfx_text_sc("KEEP THE CAMPFIRE BURNING THROUGH THE STORM", VW * 0.5f, 212, 1.8f, 0xC8D4F0FF);
    if (fmodf(T, 1.2f) < 0.85f) gfx_text_sc("PRESS A TO START", VW * 0.5f, 268, 2.6f, 0xFFFFFFFF);
    panel(VW * 0.5f - 230, 316, 460, 112, 0x0A0E18C0);
    static const char *L1[] = { "STICK / D-PAD", "A", "B", "L / R", "Y", "X" };
    static const char *L2[] = { "MOVE THE CURSOR", "BUILD / UPGRADE", "SELL", "CHOOSE TOWER", "FAST FORWARD", "CALL NEXT WAVE EARLY" };
    for (int i = 0; i < 6; i++) {
        float y = 328 + (float)(i % 3) * 32, x = VW * 0.5f - 214 + (float)(i / 3) * 228;
        gfx_text_s(L1[i], x, y, 1.4f, 0xFFD070FF);
        gfx_text_s(L2[i], x, y + 13, 1.2f, 0xB8C4E0FF);
    }
}

static void draw_end(void)
{
    float a = clampf(G->end_t / 1.0f, 0, 1);
    gfx_rect(0, 0, VW, VH, RGBA(2, 4, 10, (int)(150 * a)));
    char buf[64];
    if (G->state == ST_WIN) {
        fire_text("THE FIRE SURVIVES!", VW * 0.5f, 120, 5.5f);
        int stars = G->fire >= 16 ? 3 : (G->fire >= 8 ? 2 : 1);
        for (int i = 0; i < 3; i++) {
            float x = VW * 0.5f + ((float)i - 1.0f) * 70.0f, y = 222;
            float pop = clampf((G->end_t - 0.6f - (float)i * 0.35f) * 4.0f, 0, 1);
            uint32_t c = i < stars ? 0xFFD040FF : 0x303848FF;
            if (pop > 0) {
                if (i < stars) { gfx_blend(BL_ADD); gfx_glow(x, y, 46 * pop, 0xFFB03060); gfx_blend(BL_ALPHA); }
                for (int k = 0; k < 5; k++) {
                    float a0 = -1.5708f + (float)k * TAU / 5.0f, a1 = a0 + TAU / 10.0f, a2 = a0 - TAU / 10.0f;
                    float R = 26 * pop, rr = 11 * pop;
                    gfx_tri(x, y, c, x + cosf(a0) * R, y + sinf(a0) * R, c, x + cosf(a1) * rr, y + sinf(a1) * rr, c);
                    gfx_tri(x, y, c, x + cosf(a0) * R, y + sinf(a0) * R, c, x + cosf(a2) * rr, y + sinf(a2) * rr, c);
                }
            }
        }
    } else {
        gfx_text_sc("THE FIRE HAS GONE OUT", VW * 0.5f, 130, 4.5f, 0x90A8D0FF);
    }
    snprintf(buf, sizeof buf, "WAVES SURVIVED  %d/%d", G->state == ST_WIN ? NUM_WAVES : (G->wave > 0 ? G->wave - 1 : 0), NUM_WAVES);
    gfx_text_sc(buf, VW * 0.5f, 280, 2.0f, 0xE0E8FFFF);
    snprintf(buf, sizeof buf, "ENEMIES DEFEATED  %d", G->kills);
    gfx_text_sc(buf, VW * 0.5f, 306, 2.0f, 0xE0E8FFFF);
    snprintf(buf, sizeof buf, "FIRE LEFT  %d/%d", G->fire, FIRE_HP);
    gfx_text_sc(buf, VW * 0.5f, 332, 2.0f, 0xFFC080FF);
    if (G->end_t > 2.0f && fmodf(T, 1.2f) < 0.85f) gfx_text_sc("PRESS A TO PLAY AGAIN", VW * 0.5f, 380, 2.4f, 0xFFFFFFFF);
}

static void draw_pause(void)
{
    gfx_rect(0, 0, VW, VH, 0x02040AA0);
    gfx_text_sc("PAUSED", VW * 0.5f, 140, 5.0f, 0xFFFFFFFF);
    const char *M[3] = { "RESUME", G->mute ? "SOUND: OFF" : "SOUND: ON", "RESTART" };
    for (int i = 0; i < 3; i++) {
        int s = G->menu == i;
        float y = 222 + (float)i * 44;
        if (s) panel(VW * 0.5f - 120, y - 10, 240, 38, 0xFF904060);
        gfx_text_sc(M[i], VW * 0.5f, y, 2.6f, s ? 0xFFFFFFFF : 0x8090B0FF);
    }
}

/* ------------------------------------------------------------------ */
void game_render(const Game *g)
{
    G = g;
    T = g->time;
    gfx_begin();
    if (!built_static) build_static();
    float sh = g->shake * g->shake;
    SHX = sinf(T * 53.0f) * 7.0f * sh; SHY = cosf(T * 41.0f) * 5.0f * sh;
    cam_world(SHX, SHY);
    build_lights();
    rc_relight();

    /* night sky glow behind everything (only visible at the very top) */
    gfx_rect_v(0, 0, VW, VH, 0x0A1220FF, 0x05080EFF);

    draw_ground();        dbg_nv[0] = gfx_nv;
    draw_world();         dbg_nv[1] = gfx_nv;
    if (g->state == ST_PLAY) draw_cursor();
    draw_effects();       dbg_nv[2] = gfx_nv;
    draw_rain();          dbg_nv[3] = gfx_nv;
    draw_mist_vignette();
    draw_world_ui();      dbg_nv[4] = gfx_nv;

    switch (g->state) {
    case ST_TITLE: draw_title(); break;
    case ST_PLAY: draw_hud(); break;
    case ST_PAUSE: draw_hud(); draw_pause(); break;
    case ST_WIN: case ST_LOSE: draw_end(); break;
    }
}
