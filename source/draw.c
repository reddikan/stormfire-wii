/*
 * STORMFIRE - everything you see: the maps (rainy night or autumn golden hour), towers, enemies, effects and the HUD.
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
#include "fastmath.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAU 6.28318530718f

static const Game *G;
static float tw_charge;      /* tower being drawn: how close it is to firing (0..1), for charge-up glows */
#define AUTUMN (G->lv->env == ENV_AUTUMN)
#define HALLOW (G->lv->env == ENV_HALLOW)
#define ISLAND (G->lv->env == ENV_ISLAND)
#define PIRATE (G->lv->env == ENV_PIRATE)
#define FUTURE (G->lv->env == ENV_FUTURE)
#define SKYLV (G->lv->env == ENV_SKY)
#define PINBALL (FUTURE || SKYLV)
static void mortar_top(float x0, float z0, float x1, float z1, float y, uint32_t c);
static void wtri(float ax, float ay, float az, float bx, float by, float bz, float cx, float cy, float cz, uint32_t c);
static float wk_x, wk_z, wk_s = 1.0f;       /* the model scale for wtri (r_scale does not reach these) */
static void mortar_front(float x0, float y0, float x1, float y1, float z, uint32_t c);

/* level 4 props that move: orrery rings, rune glows, gears on walls; and sparkles on the sea */
#define MAX_PROP 64
enum { PR_ORRERY, PR_RUNE, PR_GEAR, PR_BUOY, PR_BARREL, PR_CLOCK, PR_LAMP, PR_PORTAL, PR_NEON, PR_BALLOON };
typedef struct { uint8_t kind, row; float x, z, y, ph; } Prop;
static Prop prop[MAX_PROP];
static int nprop;
#define MAX_GLINT 140
typedef struct { float x, z, ph; } Glint;
static Glint glint[MAX_GLINT];
static int nglint;
#define MAX_FOAM 90
typedef struct { float x0, z0, x1, z1, ph; } Foam;
static Foam foam[MAX_FOAM];
static int nfoam;

/* level 3 props that move: lanterns hanging from branches, glowing jack-o'-lantern faces */
#define MAX_LANT 26
typedef struct { float x, y, z, len, ph, amp; int pt; uint8_t row; } Lantern;   /* hook point, string length */
static Lantern lant[MAX_LANT];
static int nlant;
#define MAX_JACK 16
typedef struct { float x, z, r, ang, ph; uint8_t row; } Jack;
static Jack jack[MAX_JACK];
static int njack;

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
    for (int i = 0; i < G->lv->ngates; i++) {
        int gc = G->lv->gates[i][0], gr = G->lv->gates[i][1];
        if (gr == 0 && c == gc && r < 0 && r >= -3) return 1;
        if (gr == MH - 1 && c == gc && r >= MH && r <= MH + 2) return 1;
        if (gc == 0 && r == gr && c < 0 && c >= -3) return 1;
        if (gc == MW - 1 && r == gr && c >= MW && c <= MW + 2) return 1;
    }
    return 0;
}

static void tree_pos(int c, int r, float *x, float *z, float *s)
{
    *x = (float)c + 0.5f + (hf(c, r, 6) - 0.5f) * 0.3f; *z = (float)r + 0.5f + (hf(c, r, 7) - 0.5f) * 0.3f;
    *s = 0.8f + 0.45f * hf(c, r, 8);
    if (r >= MH - 1) *s *= 0.55f;                   /* the near row stays low so it does not hide the map */
}

static const uint32_t LEAF_COL[5] = { 0xE8782AFF, 0xF2BE3EFF, 0xC8402CFF, 0xDC5A22FF, 0xB8AE3CFF };

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
    float lk = AUTUMN ? 0.45f : (HALLOW ? 0.85f : (ISLAND ? 0.6f : 1.0f));   /* point lights matter less in daylight */
    if (SKYLV) {
        for (int i = 0; i < nprop; i++) {
            const Prop *p = &prop[i];
            if (p->kind == PR_LAMP) { float k = 0.8f + 0.2f * sinf(T * 3.0f + p->ph); lt_add(p->x, 1.1f, p->z, 0.6f * k, 0.4f * k, 0.55f * k, 1.6f); }
        }
        float fv = G->fire_vis, fx = clampf(G->giga_fx / 2.0f, 0, 1), sg = clampf(G->surge_t, 0, 1.6f);
        lt_add(FIRE_X, 2.4f, FIRE_Z, (0.6f + fx + sg) * fv + fx, (0.45f + fx * 0.8f + sg) * fv, (0.7f + fx + sg) * fv, 3.0f + 2.0f * fx);
    }
    if (FUTURE) {
        for (int i = 0; i < nprop; i++) {
            const Prop *p = &prop[i];
            if (p->kind == PR_LAMP) { float k = flicker(p->ph) * 0.9f; lt_add(p->x, 1.2f, p->z, 0.75f * k, 0.55f * k, 0.3f * k, 2.0f); }
            else if (p->kind == PR_PORTAL) { float k = 0.7f + 0.3f * sinf(T * 4.0f + p->ph); lt_add(p->x, 0.6f, p->z, 0.25f * k, 0.5f * k, 0.9f * k, 2.2f); }
            else if (p->kind == PR_NEON) {
                uint32_t c = BLOCK_COL[(int)p->y];
                float k = fmodf(T * 0.7f + p->ph, 7.0f) < 0.25f ? 0.15f : 0.6f;
                lt_add(p->x, 0.9f, p->z + 0.3f, (float)(c >> 24) / 255.0f * k, (float)((c >> 16) & 255) / 255.0f * k, (float)((c >> 8) & 255) / 255.0f * k, 1.8f);
            } else if (p->kind == PR_CLOCK && G->giga_fx > 0) {
                float k = clampf(G->giga_fx / 2.0f, 0, 1);
                lt_add(p->x, 2.4f, p->z, 1.2f * k, 1.4f * k, 2.0f * k, 4.5f);
            }
        }
        float fv = G->fire_vis, sg = clampf(G->surge_t, 0, 1.6f);
        float k = (0.55f + 0.15f * sinf(T * 9.0f)) * fv + sg;
        lt_add(FIRE_X - 0.4f, 0.9f, FIRE_Z, 0.45f * k, 0.65f * k, 1.0f * k, 2.4f + 2.0f * sg);   /* the flux capacitor */
    }
    if (PIRATE) {
        for (int i = 0; i < nprop; i++)
            if (prop[i].kind == PR_BUOY) {
                float k = flicker(prop[i].ph);
                lt_add(prop[i].x, 0.6f, prop[i].z, 0.7f * k, 0.4f * k, 0.15f * k, 1.7f);
            }
        for (int s2 = -1; s2 <= 1; s2 += 2) {                     /* the island's two torches */
            float k = flicker((float)s2 * 3.0f);
            lt_add(FIRE_X + 1.05f * (float)s2, 0.95f, FIRE_Z + 0.75f, 0.8f * k, 0.42f * k, 0.15f * k, 2.0f);
        }
    }
    if (ISLAND)
        for (int i = 0; i < nprop; i++)
            if (prop[i].kind == PR_RUNE) {
                float k = 0.55f + 0.45f * sinf(T * 1.3f + prop[i].ph);
                lt_add(prop[i].x, 0.5f, prop[i].z + 0.3f, 0.1f * k, 0.3f * k, 0.5f * k, 1.4f);
            }
    if (HALLOW) {
        for (int i = 0; i < nlant; i++) {
            float k = 0.85f + 0.15f * sinf(T * 11.0f + lant[i].ph);
            lt_add(lant[i].x, lant[i].y - lant[i].len, lant[i].z, 0.75f * k, 0.42f * k, 0.14f * k, 2.0f);
        }
        for (int i = 0; i < njack; i++) lt_add(jack[i].x, 0.25f, jack[i].z, 0.5f, 0.26f, 0.06f, 1.4f);
    }
    if (FUTURE || SKYLV) { /* the time machine (or the castle) lights itself (above) */ }
    else if (fv > 0.02f && PIRATE)                                 /* the treasure's golden glow */
        lt_add(FIRE_X, 0.9f, FIRE_Z + 0.1f, 1.2f * fv, 0.95f * fv, 0.35f * fv, 2.2f + 2.0f * fv);
    else if (fv > 0.02f)
        lt_add(FIRE_X, 0.9f, FIRE_Z, 1.35f * fv * fl * lk, 0.62f * fv * fl * lk, 0.24f * fv * fl * lk, 2.6f + 2.4f * fv);
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
                float k = (0.2f + 0.05f * (float)w->lvl + w->pulse * 0.35f) * lk;
                lt_add((float)c + 0.5f, t->h + 1.0f, (float)r + 0.5f, (float)(gc >> 24) / 255.0f * k,
                       (float)((gc >> 16) & 255) / 255.0f * k, (float)((gc >> 8) & 255) / 255.0f * k, 1.7f + 0.2f * (float)w->lvl);
            }
        }
    for (int i = 0; i < MAX_SH; i++) {
        const Shot *s = &G->sh[i];
        if (s->alive && s->kind == SH_SHELL) lt_add(s->x, s->y, s->z, 0.8f, 0.4f, 0.1f, 1.6f);
    }
    for (int r = 0; r < MH; r++)
        for (int c = 0; c < MW; c++) {
            const Tower *t = &G->tw[r][c];
            if (t->type == TW_PRISM && t->target >= 0 && G->en[t->target].alive) {
                float k = clampf(t->lock_t / 2.5f, 0, 1);
                lt_add(G->en[t->target].x, 0.6f, G->en[t->target].z, 0.6f + 0.4f * k, 0.25f + 0.4f * k, 0.6f + 0.3f * k, 1.6f);
            }
        }
    for (int i = 0; i < MAX_EN; i++) {
        const Enemy *e = &G->en[i];
        if (!e->alive) continue;
        if (e->type == EN_WISP) lt_add(e->x, e->y, e->z, 0.25f, 0.6f, 0.45f, 1.6f);
        if (e->type == EN_GOLEM) lt_add(e->x, 1.2f, e->z, 0.2f, 0.6f, 0.9f, 2.6f);
        if (e->type == EN_PUMPKIN) lt_add(e->x, 0.7f, e->z, 0.9f, 0.5f, 0.1f, 2.4f);
        if (e->type == EN_GHOULKING) lt_add(e->x, 1.2f, e->z, 0.2f, 0.7f, 0.25f, 2.6f);
        if (e->type == EN_ARCHITECT) lt_add(e->x, 1.0f, e->z, 0.2f, 0.45f, 0.7f, 2.6f);
        if (e->type == EN_GHOST && e->dash_t <= 0) lt_add(e->x, e->y, e->z, 0.15f, 0.22f, 0.4f, 1.3f);
        if (e->type == EN_HOVERCAR) lt_add(e->x + cosf(e->ang) * 0.7f, 0.5f, e->z + sinf(e->ang) * 0.7f, 0.3f, 0.6f, 0.7f, 1.8f);
        if (e->type == EN_TIMETRAIN && e->dash_t <= 0) lt_add(e->x, 0.8f, e->z, 0.9f, 0.5f, 0.2f, 2.6f);
        if (e->type == EN_HOLOSHARK && e->dash_t <= 0) lt_add(e->x, 0.5f, e->z, 0.1f, 0.3f, 0.5f, 1.4f);
        if (e->type == EN_SHIP) lt_add(e->x - cosf(e->ang) * 0.75f, 0.5f, e->z - sinf(e->ang) * 0.75f, 0.7f, 0.42f, 0.15f, 1.8f);
        if (e->type == EN_KRAKEN && e->dash_t <= 0) lt_add(e->x, 0.8f, e->z, 0.6f, 0.45f, 0.15f, 2.4f);
    }
    for (int i = 0; i < MAX_FL; i++) {
        const FLight *l = &G->fl[i];
        if (l->t >= l->life) continue;
        float k = (1.0f - l->t / l->life) * (AUTUMN ? 0.6f : 1.0f);
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
    if (ISLAND) {                                  /* dawn-lit grass, pale flagstones, the sea all round */
        if (in_map(c, r)) {
            if (k == TL_PATH) base = ((c + r) & 1) ? 0xA49A80FF : 0x9C927AFF;
            else if (k == TL_LAWN || k == TL_GRAVE) base = ((c + r) & 1) ? 0x6E8E4AFF : 0x668646FF;
            else if (k == TL_FIRE) base = 0x8E8678FF;
            else base = 0x6A7650FF;                    /* mossy rocks along the shore */
        } else {
            int near = in_map(c - 1, r) || in_map(c + 1, r) || in_map(c, r - 1) || in_map(c, r + 1);
            base = near ? 0x3E6E84FF : 0x2C5070FF;     /* shallows, then deeper water */
            v = 0.97f + 0.06f * hf(c, r, 1);
        }
        return scale_rgb(base, v);
    }
    if (SKYLV) {                                   /* clouds everywhere, a rainbow road, flower meadows, a marble courtyard */
        if (k == TL_PATH) {
            float hue = 0.03f * (float)G->dist[r][c];
            return col_hsv(hue, 0.42f, 0.98f, 255);
        }
        if (!in_map(c, r) && outside_lane(c, r)) return col_hsv(0.03f * (float)(abs(c - FIRE_C) + abs(r - FIRE_R)), 0.4f, 0.98f, 255);
        if (k == TL_FIRE) base = ((c + r) & 1) ? 0xF8E8F0FF : 0xF0D8E8FF;
        else if (k == TL_LAWN) base = ((c + r) & 1) ? 0xA8E890FF : 0xA0E088FF;
        else base = ((c * 3 + r * 5) % 7 < 3) ? 0xF4F0FCFF : 0xECE6F8FF;     /* cloud */
        return scale_rgb(base, 0.96f + 0.06f * hf(c, r, 1));
    }
    if (FUTURE) {                                  /* a town square at night: asphalt streets, pavement, a little park */
        if (k == TL_PATH || (!in_map(c, r) && outside_lane(c, r))) base = ((c + r) & 1) ? 0x3E3E48FF : 0x3A3A44FF;
        else if (k == TL_FIRE) base = ((c + r) & 1) ? 0x9898A2FF : 0x88889AFF;
        else if (k == TL_LAWN) base = ((c + r) & 1) ? 0x3E6E40FF : 0x3A683CFF;
        else if (k == TL_GRAVE) base = 0x80808AFF;
        else if (in_map(c, r)) base = 0x6A6A74FF;
        else base = ((c + r) & 1) ? 0x30303AFF : 0x2C2C36FF;
        return scale_rgb(base, v);
    }
    if (PIRATE) {                                  /* the open sea in a storm; pale sandbars; the treasure island */
        float w = 0.9f + 0.2f * hf(c, r, 1);
        if (k == TL_LAWN) return scale_rgb(((c + r) & 1) ? 0xE0BC78FF : 0xD8B472FF, v);
        if (k == TL_FIRE) return scale_rgb(0xE4C484FF, v);
        if (k == TL_PATH) base = ((c + r) & 1) ? 0x2A5466FF : 0x285062FF;      /* the shipping lanes */
        else if (in_map(c, r)) base = 0x24485AFF;
        else {
            int near = in_map(c - 1, r) || in_map(c + 1, r) || in_map(c, r - 1) || in_map(c, r + 1) || outside_lane(c, r);
            base = near ? 0x22465AFF : 0x1C3A4EFF;
        }
        return scale_rgb(base, w);
    }
    if (HALLOW) {                                  /* dead grass, dark dirt, graveyard earth */
        if (k == TL_PATH || (!in_map(c, r) && outside_lane(c, r))) base = 0x5E4C3EFF;
        else if (k == TL_LAWN) base = ((c + r) & 1) ? 0x5C5C3CFF : 0x54543AFF;
        else if (k == TL_FIRE) base = 0x6E665EFF;
        else if (k == TL_GRAVE) base = 0x4E4636FF;
        else if (k == TL_TREE) base = 0x403A30FF;
        else base = 0x36322AFF;
        return scale_rgb(base, v);
    }
    if (AUTUMN) {                                  /* golden grass, dirt paths, leaf litter */
        if (k == TL_PATH || (!in_map(c, r) && outside_lane(c, r))) base = 0x9C7A4EFF;
        else if (k == TL_LAWN) base = ((c + r) & 1) ? 0xA6A04CFF : 0x9C984AFF;
        else if (k == TL_FIRE) base = 0x9A8E80FF;
        else if (k == TL_TREE) base = 0x7E7438FF;
        else base = 0x716A36FF;
        return scale_rgb(base, v);
    }
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

/* golden hour: fallen leaves and long shadows cast away from the low sun (which sits off to the left) */
#define SUN_DX 1.5f
#define SUN_DZ 0.45f
static void shadow_quad(float ax, float az, float bx, float bz, float h)
{
    float p[4][3] = { {ax, 0.006f, az}, {bx, 0.006f, bz}, {bx + SUN_DX * h, 0.006f, bz + SUN_DZ * h}, {ax + SUN_DX * h, 0.006f, az + SUN_DZ * h} };
    /* wind the corners so the face points up */
    float ux = p[1][0] - p[0][0], uz = p[1][2] - p[0][2], vx = p[3][0] - p[0][0], vz = p[3][2] - p[0][2];
    if (uz * vx - ux * vz < 0) { float t[3]; for (int i = 0; i < 3; i++) { t[i] = p[1][i]; p[1][i] = p[3][i]; p[3][i] = t[i]; } }
    r_face(4, (const float (*)[3])p, 0, 1, 0, 0x2A100078);
}

/* level 4: piers over the water at each gate, rocks along the shore, and long dawn shadows that shorten
 * as the sun climbs (they are recorded stretchable; the stretch is set every frame) */
#define ISL_DX 0.22f
#define ISL_DZ 1.0f
static void island_shadow_edge(float ax, float az, float bx, float bz, float h)
{
    float p[4][3] = { {ax, 0.006f, az}, {bx, 0.006f, bz}, {bx, 0.006f, bz}, {ax, 0.006f, az} };
    float w[4] = { 0, 0, h, h };
    r_ground_stretch(4, (const float (*)[3])p, w, ISL_DX, ISL_DZ, 0x30182068);
}

static void build_island_ground(void)
{
    nglint = nfoam = 0;
    for (int r = ROW_MIN; r < ROW_MAX; r++)
        for (int c = -5; c < MW + 5; c++) {
            if (in_map(c, r)) continue;
            if (!on_screen((float)c + 0.5f, 0, (float)r + 0.5f, 40.0f)) continue;
            if (outside_lane(c, r)) {
                /* a wooden pier: planks on posts */
                r_light_model((float)c + 0.5f, 0.1f, (float)r + 0.5f);
                int vert = (r < 0 || r >= MH);
                for (int k = 0; k < 4; k++) {
                    float a = (float)k * 0.25f;
                    uint32_t wood = scale_rgb(0x8A6440FF, 0.85f + 0.3f * hf(c, r, 80 + k));
                    if (vert) r_box((float)c + 0.08f, 0.04f, (float)r + a + 0.01f, (float)c + 0.92f, 0.1f, (float)r + a + 0.23f, wood, scale_rgb(wood, 0.7f), 0);
                    else r_box((float)c + a + 0.01f, 0.04f, (float)r + 0.08f, (float)c + a + 0.23f, 0.1f, (float)r + 0.92f, wood, scale_rgb(wood, 0.7f), 0);
                }
                for (int k = 0; k < 2; k++) {
                    float px = vert ? (float)c + (k ? 0.88f : 0.12f) : (float)c + 0.5f, pz = vert ? (float)r + 0.5f : (float)r + (k ? 0.88f : 0.12f);
                    r_prism(px, -0.1f, pz, 0.05f, 0.05f, 0.18f, 5, 0, 0x5A4028FF, 0x4A3420FF);
                }
                r_light_off();
                continue;
            }
            int near = in_map(c - 1, r) || in_map(c + 1, r) || in_map(c, r - 1) || in_map(c, r + 1);
            if (near && hf(c, r, 90) < 0.5f) {           /* shore rocks */
                r_light_model((float)c + 0.5f, 0.1f, (float)r + 0.5f);
                for (int k = 0; k < 2; k++)
                    r_crystal((float)c + 0.2f + 0.6f * hf(c, r, 91 + k), 0.04f, (float)r + 0.2f + 0.6f * hf(c, r, 93 + k), 0.14f + 0.1f * hf(c, r, 95 + k), 0.1f, 0.05f, 5, hf(c, r, 97 + k) * 3.0f, 0x7A7A70FF);
                r_light_off();
            }
            if (nglint < MAX_GLINT && hf(c, r, 99) < 0.75f) {
                Glint *gl = &glint[nglint++];
                gl->x = (float)c + hf(c, r, 100); gl->z = (float)r + hf(c, r, 101); gl->ph = hf(c, r, 102) * 6.28f;
            }
            /* foam where the water meets the island */
            if (nfoam < MAX_FOAM) {
                if (in_map(c, r + 1)) { Foam *f = &foam[nfoam++]; f->x0 = (float)c; f->x1 = (float)c + 1; f->z0 = f->z1 = (float)r + 1; f->ph = hf(c, r, 103) * 6.28f; }
                else if (in_map(c, r - 1) && nfoam < MAX_FOAM) { Foam *f = &foam[nfoam++]; f->x0 = (float)c; f->x1 = (float)c + 1; f->z0 = f->z1 = (float)r; f->ph = hf(c, r, 104) * 6.28f; }
                if (in_map(c + 1, r) && nfoam < MAX_FOAM) { Foam *f = &foam[nfoam++]; f->x0 = f->x1 = (float)c + 1; f->z0 = (float)r; f->z1 = (float)r + 1; f->ph = hf(c, r, 105) * 6.28f; }
                else if (in_map(c - 1, r) && nfoam < MAX_FOAM) { Foam *f = &foam[nfoam++]; f->x0 = f->x1 = (float)c; f->z0 = (float)r; f->z1 = (float)r + 1; f->ph = hf(c, r, 106) * 6.28f; }
            }
        }
    /* shadows: walls and blocks, rune stones and orreries, shore pines */
    for (int r = 0; r < MH; r++)
        for (int c = 0; c < MW; c++) {
            const Tile *t = &G->tile[r][c];
            float h = 0;
            if (solid(c, r)) h = t->h;
            else if (t->kind == TL_GRAVE) h = t->color == DECO_RUNE ? 1.4f : 0.9f;
            else if (t->kind == TL_TREE) h = 1.6f;
            if (h <= 0) continue;
            r_light_model((float)c + 0.5f, 0, (float)r + 0.5f);
            if (t->kind == TL_TREE || t->kind == TL_GRAVE) {
                /* a long narrow shadow from the base */
                float x = (float)c + 0.5f, z = (float)r + 0.5f, wd = t->kind == TL_TREE ? 0.3f : 0.16f;
                float p[4][3] = { {x - wd, 0.006f, z}, {x + wd, 0.006f, z}, {x + wd * 0.4f, 0.006f, z}, {x - wd * 0.4f, 0.006f, z} };
                float w[4] = { 0, 0, h, h };
                r_ground_stretch(4, (const float (*)[3])p, w, ISL_DX, ISL_DZ, 0x30182060);
            } else {
                if (!solid(c, r + 1)) island_shadow_edge((float)c, (float)r + 1, (float)c + 1, (float)r + 1, h);
                if (!solid(c + 1, r)) island_shadow_edge((float)c + 1, (float)r, (float)c + 1, (float)r + 1, h);
            }
            r_light_off();
        }
}

/* static parts of the level 4 props, and their moving parts registered for every frame */
static void draw_island_prop(int c, int r, const Tile *t)
{
    float x = (float)c + 0.5f, z = (float)r + 0.5f;
    r_light_model(x, 0.4f, z);
    if (t->color == DECO_RUNE) {
        /* a standing stone, carved with runes that glow */
        r_prism(x, 0, z, 0.3f, 0.32f, 0.08f, 6, 0.3f, 0x8A8A80FF, 0x6E6E66FF);
        r_prism(x, 0.08f, z, 0.24f, 0.16f, 1.25f, 4, 0.785f + (hf(c, r, 5) - 0.5f) * 0.3f, 0x9A9A94FF, 0x7E7E7AFF);
        r_prism(x, 1.25f, z, 0.16f, 0.0f, 1.42f, 4, 0.785f, 0xA4A49EFF, 0x8A8A84FF);
    } else {
        /* an orrery on a stone plinth: the brass rings turn above it */
        r_prism(x, 0, z, 0.4f, 0.36f, 0.16f, 8, 0.39f, 0x9C968AFF, 0x807A70FF);
        r_prism(x, 0.16f, z, 0.1f, 0.07f, 0.62f, 6, 0, 0xB89048FF, 0x987030FF);
    }
    r_light_off();
    if (nprop < MAX_PROP) {
        Prop *p = &prop[nprop++];
        p->kind = t->color == DECO_RUNE ? PR_RUNE : PR_ORRERY;
        p->x = x; p->z = z; p->y = 0; p->ph = hf(c, r, 7) * 6.28f; p->row = (uint8_t)(r - ROW_MIN);
    }
}

/* level 5: whitecaps all over the sea, foam where the water slaps against sandbars and docks */
static void build_pirate_ground(void)
{
    nglint = nfoam = 0;
    for (int r = ROW_MIN; r < ROW_MAX; r++)
        for (int c = -5; c < MW + 5; c++) {
            int kd = kind_at(c, r);
            int water = !in_map(c, r) || kd == TL_PATH || kd == TL_TREE || kd == TL_GRAVE;
            if (!water) continue;
            if (!on_screen((float)c + 0.5f, 0, (float)r + 0.5f, 40.0f)) continue;
            if (nglint < MAX_GLINT && hf(c, r, 99) < (in_map(c, r) ? 0.45f : 0.22f)) {
                Glint *gl = &glint[nglint++];
                gl->x = (float)c + hf(c, r, 100); gl->z = (float)r + hf(c, r, 101); gl->ph = hf(c, r, 102) * 6.28f;
            }
            if (!in_map(c, r)) continue;
            #define DRY(cc, rr) (in_map(cc, rr) && (solid(cc, rr) || kind_at(cc, rr) == TL_LAWN || kind_at(cc, rr) == TL_FIRE))
            if (nfoam < MAX_FOAM && DRY(c, r + 1)) { Foam *f = &foam[nfoam++]; f->x0 = (float)c; f->x1 = (float)c + 1; f->z0 = f->z1 = (float)r + 1; f->ph = hf(c, r, 103) * 6.28f; }
            if (nfoam < MAX_FOAM && DRY(c, r - 1)) { Foam *f = &foam[nfoam++]; f->x0 = (float)c; f->x1 = (float)c + 1; f->z0 = f->z1 = (float)r; f->ph = hf(c, r, 104) * 6.28f; }
            if (nfoam < MAX_FOAM && DRY(c + 1, r)) { Foam *f = &foam[nfoam++]; f->x0 = f->x1 = (float)c + 1; f->z0 = (float)r; f->z1 = (float)r + 1; f->ph = hf(c, r, 105) * 6.28f; }
            if (nfoam < MAX_FOAM && DRY(c - 1, r)) { Foam *f = &foam[nfoam++]; f->x0 = f->x1 = (float)c; f->z0 = (float)r; f->z1 = (float)r + 1; f->ph = hf(c, r, 106) * 6.28f; }
            #undef DRY
        }
    /* lantern buoys either side of every gate, a few loose barrels drifting in the open water */
    for (int i = 0; i < G->lv->ngates; i++) {
        int gc = G->lv->gates[i][0], gr = G->lv->gates[i][1];
        float ox = 0, oz = 0;
        if (gr == 0) oz = -1; else if (gr == MH - 1) oz = 1; else if (gc == 0) ox = -1; else ox = 1;
        for (int s2 = -1; s2 <= 1; s2 += 2) {
            if (nprop >= MAX_PROP) break;
            Prop *p = &prop[nprop++];
            p->kind = PR_BUOY;
            p->x = (float)gc + 0.5f + ox * 1.4f + oz * 0.75f * (float)s2;
            p->z = (float)gr + 0.5f + oz * 1.4f + ox * 0.75f * (float)s2;
            p->y = 0; p->ph = hf(gc, gr + s2, 7) * 6.28f;
            int rr = (int)floorf(p->z); if (rr < ROW_MIN) rr = ROW_MIN; if (rr >= ROW_MAX) rr = ROW_MAX - 1;
            p->row = (uint8_t)(rr - ROW_MIN);
        }
    }
    for (int r = 0; r < MH; r++)
        for (int c = 0; c < MW; c++)
            if (G->tile[r][c].kind == TL_GRAVE && G->tile[r][c].color == DECO_WATER && nprop < MAX_PROP) {
                Prop *p = &prop[nprop++];
                p->kind = PR_BARREL; p->x = (float)c + 0.5f; p->z = (float)r + 0.5f; p->y = 0;
                p->ph = hf(c, r, 8) * 6.28f; p->row = (uint8_t)(r - ROW_MIN);
            }
}

/* a jagged sea rock (in the map's border and out in the open water) */
static void draw_sea_rock(float x, float z, float s, float rot)
{
    r_light_model(x, 0.3f, z);
    r_crystal(x, 0.06f, z, 0.42f * s, 0.32f * s, 0.12f, 6, rot, 0x5A6066FF);
    r_crystal(x + 0.12f * s, 0.18f * s, z - 0.08f * s, 0.24f * s, 0.42f * s, 0.1f, 5, rot + 1.0f, 0x6A7076FF);
    r_crystal(x - 0.16f * s, 0.05f, z + 0.12f * s, 0.18f * s, 0.16f * s, 0.06f, 5, rot + 2.0f, 0x50565CFF);
    r_light_off();
}

/* a wrecked ship half sunk, its broken mast leaning */
static void draw_wreck(int c, int r)
{
    float x = (float)c + 0.5f, z = (float)r + 0.5f, a = hf(c, r, 4) * 6.28f, ca = cosf(a), sa = sinf(a);
    r_light_model(x, 0.3f, z);
    uint32_t hull = 0x4A3424FF, hull2 = 0x3A281AFF;
    r_obox(x, -0.05f, z, 0.42f, 0.2f, 0.16f, a, 0x5E4630FF, hull2);
    r_obox(x + ca * 0.36f, -0.05f, z + sa * 0.36f, 0.12f, 0.13f, 0.32f, a + 0.6f, hull, hull2);   /* bow sticking up */
    for (int k = -2; k <= 2; k++)                                                                   /* bare ribs */
        r_beam(x + ca * 0.12f * (float)k - sa * 0.2f, 0.12f, z + sa * 0.12f * (float)k + ca * 0.2f, sa * 0.3f, 1.0f, -ca * 0.3f, 0.2f, 0.03f, 0.03f, hull2);
    r_stick(x - ca * 0.05f, 0.1f, z - sa * 0.05f, x - ca * 0.45f + sa * 0.2f, 0.75f, z - sa * 0.45f - ca * 0.2f, 0.04f, 0.025f, 0x3A2818FF);
    r_light_off();
}

/* level 5: the treasure island in the middle - a sandy mound, rocks, two leaning palms (they sway in the gale) */
static void draw_island_static(void)
{
    float x = FIRE_X, z = FIRE_Z;
    r_light_model(x, 0.3f, z);
    r_prism(x, 0, z, 1.45f, 1.05f, 0.12f, 12, 0.2f, 0xECCC8CFF, 0xC8A870FF);
    r_prism(x, 0.12f, z, 1.05f, 0.6f, 0.22f, 10, 0.5f, 0xF0D498FF, 0xD0B07CFF);
    for (int i = 0; i < 5; i++) {
        float an = 2.4f + (float)i * 0.55f;
        r_crystal(x + cosf(an) * 1.25f, 0.05f, z + sinf(an) * 1.15f, 0.16f + 0.06f * (float)(i & 1), 0.12f, 0.05f, 5, an, 0x7A7468FF);
    }
    r_light_off();
    for (int p = 0; p < 2; p++) {
        float px = x + (p ? 0.85f : -0.95f), pz = z - 0.55f + (p ? 0.1f : 0.0f), lean = p ? 0.4f : -0.35f, H = 1.5f + 0.2f * (float)p;
        float tx = px + lean, tz = pz - 0.15f;
        r_light_model(px, 0.8f, pz);
        rc_sway(0.12f, 0.3f, H + 0.3f);
        for (int k = 0; k < 5; k++) {                                          /* ringed curving trunk */
            float u0 = (float)k / 5.0f, u1 = (float)(k + 1) / 5.0f;
            float x0 = px + lean * u0 * u0, x1 = px + lean * u1 * u1, z0 = pz + (tz - pz) * u0, z1 = pz + (tz - pz) * u1;
            r_stick(x0, H * u0 + 0.2f, z0, x1, H * u1 + 0.2f, z1, 0.075f - 0.01f * (float)k, 0.065f - 0.01f * (float)k, (k & 1) ? 0x8A6A44FF : 0x7A5C3AFF);
        }
        float cy = H + 0.2f;
        for (int f = 0; f < 7; f++) {                                          /* fronds drooping out from the crown */
            float an = (float)f * 0.9f + (float)p, dx = cosf(an), dz = sinf(an);
            float mx = tx + dx * 0.38f, mz = tz + dz * 0.38f, ex = tx + dx * 0.7f, ez = tz + dz * 0.7f;
            r_stick(tx, cy, tz, mx, cy + 0.12f, mz, 0.09f, 0.08f, (f & 1) ? 0x3E7A3AFF : 0x4A8A40FF);
            r_stick(mx, cy + 0.12f, mz, ex, cy - 0.18f, ez, 0.08f, 0.02f, (f & 1) ? 0x3E7A3AFF : 0x4A8A40FF);
        }
        r_crystal(tx, cy - 0.05f, tz, 0.09f, 0.06f, 0.06f, 5, 0, 0x5A4228FF);  /* coconuts */
        rc_sway(0, 0, 1);
        r_light_off();
    }
}

/* level 6: a building (shop fronts round the square, the town beyond); near the camera they stay low */
static void draw_building(int c, int r)
{
    float x0 = (float)c + 0.04f, x1 = (float)c + 0.96f, z0 = (float)r + 0.04f, z1 = (float)r + 0.96f;
    int nearcam = r >= MH - 1;
    float h = nearcam ? 0.3f + 0.15f * hf(c, r, 30) : 0.7f + 1.2f * hf(c, r, 31) + (r < 0 ? 0.4f * (float)(-r) * hf(c, r, 32) : 0.0f);
    if (!on_screen((float)c + 0.5f, h, (float)r + 0.5f, 90.0f)) return;
    static const uint32_t BC[5] = { 0x6A4C46FF, 0x847462FF, 0x5A5A66FF, 0x426068FF, 0x6E5A7AFF };
    uint32_t col = BC[(int)(hf(c, r, 33) * 4.99f)], side = scale_rgb(col, 0.78f);
    r_box(x0, 0, z0, x1, h, z1, scale_rgb(col, 0.7f), side, 0);
    r_light_model((float)c + 0.5f, h * 0.5f, (float)r + 1.0f);
    int nearmap = c >= -2 && c <= MW + 1 && r >= -2 && r <= MH + 1;
    if (nearmap && !nearcam) {                       /* windows on the front, some lit */
        int floors = (int)(h / 0.42f);
        for (int f = 0; f < floors && f < 3; f++)
            for (int k = 0; k < 2; k++) {
                float wx = x0 + 0.16f + 0.4f * (float)k, wy = 0.18f + 0.42f * (float)f;
                uint32_t wc = hf(c + k, r + f, 34) < 0.55f ? 0xF8DC90FF : 0x283040FF;
                mortar_front(wx, wy, wx + 0.24f, wy + 0.22f, z1 + 0.004f, wc);
            }
    }
    if (h > 0.5f) mortar_top(x0, z0, x1, z0 + 0.06f, h + 0.003f, scale_rgb(col, 0.5f));   /* parapet line */
    r_light_off();
}

/* the clock tower (static part); its glowing face is drawn every frame */
static void draw_clock_tower(int c, int r)
{
    float x = (float)c + 0.5f, z = (float)r + 0.5f;
    r_light_model(x, 1.0f, z + 0.5f);
    r_box(x - 0.46f, 0, z - 0.46f, x + 0.46f, 1.0f, z + 0.46f, 0x9A7A64FF, 0x7A5C4AFF, 0);
    r_box(x - 0.36f, 1.0f, z - 0.36f, x + 0.36f, 2.0f, z + 0.36f, 0xC8C0B0FF, 0xA8A090FF, 0);
    r_box(x - 0.42f, 1.95f, z - 0.42f, x + 0.42f, 2.05f, z + 0.42f, 0xD8D0C0FF, 0xB0A898FF, 0);
    r_prism(x, 2.05f, z, 0.4f, 0.0f, 2.6f, 4, 0.785f, 0x405060FF, 0x304050FF);
    r_prism(x, 2.55f, z, 0.025f, 0.01f, 2.85f, 4, 0, 0x606060FF, 0x505050FF);       /* lightning rod */
    for (int k = 0; k < 3; k++) mortar_front(x - 0.3f + 0.22f * (float)k, 0.0f, x - 0.18f + 0.22f * (float)k, 0.6f, z + 0.464f, 0x3A2A22FF);   /* doors */
    r_light_off();
    if (nprop < MAX_PROP) { Prop *p = &prop[nprop++]; p->kind = PR_CLOCK; p->x = x; p->z = z; p->y = 1.5f; p->ph = 0; p->row = (uint8_t)(r - ROW_MIN); }
}

static void draw_lamp_post(int c, int r)
{
    float x = (float)c + 0.5f, z = (float)r + 0.5f;
    r_light_model(x, 0.6f, z);
    r_prism(x, 0, z, 0.1f, 0.08f, 0.08f, 6, 0, 0x404048FF, 0x303038FF);
    r_prism(x, 0.08f, z, 0.035f, 0.03f, 1.15f, 6, 0, 0x404048FF, 0x303038FF);
    r_light_off();
    if (nprop < MAX_PROP) { Prop *p = &prop[nprop++]; p->kind = PR_LAMP; p->x = x; p->z = z; p->y = 1.2f; p->ph = hf(c, r, 9) * 6.28f; p->row = (uint8_t)(r - ROW_MIN); }
}

/* the plaza under the time machine, and the props that move: time portals at the gates, neon signs */
static void build_future_ground(void)
{
    for (int i = 0; i < G->lv->ngates && nprop < MAX_PROP; i++) {
        int gc = G->lv->gates[i][0], gr = G->lv->gates[i][1];
        float ox = 0, oz = 0;
        if (gr == 0) oz = -1; else if (gr == MH - 1) oz = 1; else if (gc == 0) ox = -1; else ox = 1;
        Prop *p = &prop[nprop++];
        p->kind = PR_PORTAL; p->x = (float)gc + 0.5f + ox * 1.7f; p->z = (float)gr + 0.5f + oz * 1.7f;
        p->y = ox != 0 ? 1.0f : 0.0f;                        /* which way the road runs through it */
        p->ph = (float)i * 1.3f;
        int rr = (int)floorf(p->z); if (rr < ROW_MIN) rr = ROW_MIN; if (rr >= ROW_MAX) rr = ROW_MAX - 1;
        p->row = (uint8_t)(rr - ROW_MIN);
    }
    /* neon signs on a few of the shop fronts along the top edge */
    for (int c = 1; c < MW - 1 && nprop < MAX_PROP; c++) {
        if (G->tile[0][c].kind != TL_TREE || hf(c, 0, 40) > 0.4f) continue;
        Prop *p = &prop[nprop++];
        p->kind = PR_NEON; p->x = (float)c + 0.5f; p->z = 0.97f; p->y = (float)(int)(hf(c, 0, 41) * 7.99f); p->ph = hf(c, 0, 42) * 10.0f;
        p->row = (uint8_t)(0 - ROW_MIN);
    }
}

/* level 7: a heap of cloud */
static void draw_cloud_puff(float x, float z, float s, float rot)
{
    if (!on_screen(x, 0.3f * s, z, 80.0f)) return;
    r_light_model(x, 0.4f * s, z + 0.5f);
    r_crystal(x, 0.12f * s, z, 0.48f * s, 0.32f * s, 0.12f * s, 7, rot, 0xFFFFFFFF);
    r_crystal(x + 0.25f * s, 0.18f * s, z - 0.12f * s, 0.32f * s, 0.3f * s, 0.1f * s, 6, rot + 1.0f, 0xF8F4FFFF);
    r_crystal(x - 0.28f * s, 0.12f * s, z + 0.1f * s, 0.28f * s, 0.22f * s, 0.08f * s, 6, rot + 2.0f, 0xFCF6FFFF);
    r_light_off();
}

/* level 7: a pastel post with a glowing star on top (the star is drawn every frame) */
static void draw_star_post(int c, int r)
{
    float x = (float)c + 0.5f, z = (float)r + 0.5f;
    draw_cloud_puff(x, z, 0.5f, hf(c, r, 4) * 6.0f);
    r_light_model(x, 0.6f, z);
    r_prism(x, 0.05f, z, 0.04f, 0.03f, 1.0f, 6, 0, 0xF8C8E8FF, 0xE0A8D0FF);
    r_light_off();
    if (nprop < MAX_PROP) { Prop *p = &prop[nprop++]; p->kind = PR_LAMP; p->x = x; p->z = z; p->y = 1.12f; p->ph = hf(c, r, 9) * 6.28f; p->row = (uint8_t)(r - ROW_MIN); }
}

/* level 7: flowers in the meadows, twinkles on the clouds, rainbow portals at the gates, balloons drifting by */
static void build_sky_ground(void)
{
    nglint = 0;
    static const uint32_t FL[5] = { 0xFF80B0FF, 0xFFE060FF, 0xFFFFFFFF, 0xB090FFFF, 0xFF9060FF };
    for (int r = ROW_MIN; r < ROW_MAX; r++)
        for (int c = -5; c < MW + 5; c++) {
            if (!on_screen((float)c + 0.5f, 0, (float)r + 0.5f, 40.0f)) continue;
            int kd = kind_at(c, r);
            if (in_map(c, r) && kd == TL_LAWN) {
                r_light_model((float)c + 0.5f, 0, (float)r + 0.5f);
                for (int k = 0; k < 5; k++) {
                    float x = (float)c + 0.12f + 0.76f * hf(c, r, 60 + k), z = (float)r + 0.12f + 0.76f * hf(c, r, 70 + k), w = 0.05f;
                    float p[4][3] = { {x - w, 0.005f, z}, {x, 0.005f, z + w}, {x + w, 0.005f, z}, {x, 0.005f, z - w} };
                    r_face(4, (const float (*)[3])p, 0, 1, 0, FL[(int)(hf(c, r, 80 + k) * 4.99f)]);
                }
                r_light_off();
            }
            if ((!in_map(c, r) || kd == TL_TREE) && nglint < MAX_GLINT && hf(c, r, 99) < 0.4f) {
                Glint *gl = &glint[nglint++];
                gl->x = (float)c + hf(c, r, 100); gl->z = (float)r + hf(c, r, 101); gl->ph = hf(c, r, 102) * 6.28f;
            }
        }
    for (int i = 0; i < G->lv->ngates && nprop < MAX_PROP; i++) {
        int gc = G->lv->gates[i][0], gr = G->lv->gates[i][1];
        float ox = 0, oz = 0;
        if (gr == 0) oz = -1; else if (gr == MH - 1) oz = 1; else if (gc == 0) ox = -1; else ox = 1;
        Prop *p = &prop[nprop++];
        p->kind = PR_PORTAL; p->x = (float)gc + 0.5f + ox * 1.7f; p->z = (float)gr + 0.5f + oz * 1.7f;
        p->y = ox != 0 ? 1.0f : 0.0f; p->ph = (float)i * 1.3f;
        int rr = (int)floorf(p->z); if (rr < ROW_MIN) rr = ROW_MIN; if (rr >= ROW_MAX) rr = ROW_MAX - 1;
        p->row = (uint8_t)(rr - ROW_MIN);
    }
    for (int i = 0; i < 4 && nprop < MAX_PROP; i++) {        /* hot-air balloons off the edges of the kingdom */
        Prop *p = &prop[nprop++];
        p->kind = PR_BALLOON;
        p->x = i < 2 ? -2.5f - (float)i : (float)MW + 1.5f + (float)(i - 2);
        p->z = 1.5f + (float)i * 3.5f; p->y = (float)i; p->ph = (float)i * 2.1f;
        p->row = (uint8_t)((int)p->z - ROW_MIN);
    }
}

static void build_autumn_ground(void)
{
    /* leaf litter */
    for (int r = ROW_MIN; r < ROW_MAX; r++)
        for (int c = -5; c < MW + 5; c++) {
            if (in_map(c, r) && (solid(c, r) || G->tile[r][c].kind == TL_FIRE)) continue;
            if (!on_screen((float)c + 0.5f, 0, (float)r + 0.5f, 40.0f)) continue;
            int n = in_map(c, r) ? 1 + (int)(hf(c, r, 50) * 3.0f) : (int)(hf(c, r, 50) * 2.5f);
            r_light_model((float)c + 0.5f, 0, (float)r + 0.5f);
            for (int k = 0; k < n; k++) {
                float x = (float)c + 0.1f + 0.8f * hf(c, r, 60 + k), z = (float)r + 0.1f + 0.8f * hf(c, r, 70 + k);
                float a = hf(c, r, 80 + k) * 6.28f, l = 0.09f + 0.04f * hf(c, r, 90 + k), w = l * 0.5f;
                float ca = cosf(a), sa = sinf(a);
                float p[4][3] = { {x + ca * l, 0.004f, z + sa * l}, {x - sa * w, 0.004f, z + ca * w}, {x - ca * l, 0.004f, z - sa * l}, {x + sa * w, 0.004f, z - ca * w} };
                if ((p[1][2] - p[0][2]) * (p[3][0] - p[0][0]) - (p[1][0] - p[0][0]) * (p[3][2] - p[0][2]) < 0) {
                    float t[3]; for (int i = 0; i < 3; i++) { t[i] = p[1][i]; p[1][i] = p[3][i]; p[3][i] = t[i]; }
                }
                r_face(4, (const float (*)[3])p, 0, 1, 0, LEAF_COL[(int)(hf(c, r, 100 + k) * 4.99f)]);
            }
            r_light_off();
        }
    /* walls and blocks: extrude the edges facing away from the sun */
    for (int r = 0; r < MH; r++)
        for (int c = 0; c < MW; c++) {
            if (!solid(c, r)) continue;
            float h = G->tile[r][c].h;
            r_light_model((float)c + 0.5f, 0, (float)r + 0.5f);
            if (!solid(c + 1, r)) shadow_quad((float)c + 1, (float)r, (float)c + 1, (float)r + 1, h);
            if (!solid(c, r + 1)) shadow_quad((float)c, (float)r + 1, (float)c + 1, (float)r + 1, h);
            r_light_off();
        }
    /* trees: long soft ellipses */
    for (int r = ROW_MIN; r < ROW_MAX; r++)
        for (int c = -6; c < MW + 6; c++) {
            int tree = in_map(c, r) ? G->tile[r][c].kind == TL_TREE : (!outside_lane(c, r) && hf(c, r, 2) < 0.72f);
            if (!tree) continue;
            float x, z, s;
            tree_pos(c, r, &x, &z, &s);
            if (!on_screen(x, 0, z, 120.0f)) continue;
            float dl = sqrtf(SUN_DX * SUN_DX + SUN_DZ * SUN_DZ), ux = SUN_DX / dl, uz = SUN_DZ / dl;
            float L = 1.15f * s, Wd = 0.42f * s, cx = x + ux * (L * 0.85f), cz = z + uz * (L * 0.85f);
            float p[8][3];
            for (int k = 0; k < 8; k++) {
                float a = 6.2831853f * (float)k / 8.0f, u = cosf(a) * L, v = sinf(a) * Wd;
                p[k][0] = cx + ux * u - uz * v; p[k][1] = 0.006f; p[k][2] = cz + uz * u + ux * v;
            }
            float ax = p[1][0] - p[0][0], az = p[1][2] - p[0][2], bx = p[2][0] - p[0][0], bz = p[2][2] - p[0][2];
            if (az * bx - ax * bz < 0)
                for (int k = 0; k < 4; k++) for (int i = 0; i < 3; i++) { float t = p[k][i]; p[k][i] = p[7 - k][i]; p[7 - k][i] = t; }
            r_light_model(x, 0, z);
            r_face(8, (const float (*)[3])p, 0, 1, 0, 0x2A100066);
            r_light_off();
        }
}

static float SHX, SHY;
int dbg_nv[8];          /* vertex counts after each phase (for the test harness) */          /* screen shake this frame */

/* sun position on screen for level 4 (rises up out of the top of the frame as the waves go by) */
static void sun_pos(float *x, float *y)
{
    float s = G->sun;
    *x = VW * (0.30f + 0.06f * s);
    *y = 30.0f - 130.0f * s;
}

static uint32_t sun_col(int a)
{
    float s = G->sun;
    int r = 255, g = (int)(120 + 120 * fminf(s * 1.6f, 1.0f)), b = (int)(80 + 150 * fmaxf(0, s - 0.3f) / 0.7f);
    return RGBA(r, g, b, a);
}

static void draw_island_water(void)
{
    gfx_blend(BL_ADD);
    float sx, sy;
    sun_pos(&sx, &sy);
    float s = G->sun;
    /* the sun's glitter on the sea: strongest on the far water, in line with the sun */
    for (int i = 0; i < nglint; i++) {
        const Glint *gl = &glint[i];
        float px, py;
        proj(gl->x, 0.01f, gl->z, &px, &py);
        float line = 1.0f - fminf(fabsf(px - sx) / 260.0f, 1.0f);
        float far = gl->z < 0 ? 1.0f : 0.45f;
        float tw = sinf(T * 3.1f + gl->ph) * sinf(T * 1.7f + gl->ph * 2.3f);
        if (tw <= 0.2f) continue;
        float k = (tw - 0.2f) * (0.35f + 0.65f * line) * far * (0.6f + 0.4f * (1.0f - s));
        float w = (6.0f + 6.0f * line) * proj_scale(0, gl->z) / 35.0f;
        uint32_t c = sun_col((int)(230 * k));
        gfx_line(px - w, py, px + w, py, 1.6f, c, c);
        gfx_line(px, py - w * 0.35f, px, py + w * 0.35f, 1.2f, c, c);
    }
    /* foam lapping the shore */
    for (int i = 0; i < nfoam; i++) {
        const Foam *f = &foam[i];
        float k = 0.5f + 0.5f * sinf(T * 1.4f + f->ph);
        float push = 0.06f * k;
        float x0 = f->x0, x1 = f->x1, z0 = f->z0, z1 = f->z1;
        uint32_t c = RGBA(230, 235, 240, (int)(70 + 70 * k));
        if (z0 == z1) r_gquad(x0, z0 - 0.04f - push, x1, z0 + 0.04f + push, 0.012f, c);
        else r_gquad(x0 - 0.04f - push, z0, x0 + 0.04f + push, z1, 0.012f, c);
    }
    gfx_blend(BL_ALPHA);
}

/* level 5: whitecaps breaking and rolling with the wind, foam slapping the docks, the lightning on the water */
static void draw_storm_sea(void)
{
    gfx_blend(BL_ADD);
    float f = clampf(G->flash, 0, 1), wnd = G->wind + G->gust * 2.0f;
    for (int i = 0; i < nglint; i++) {
        const Glint *gl = &glint[i];
        float ph = fmodf(T * (0.32f + 0.05f * (float)(i % 4)) + gl->ph * 0.159f, 1.0f);
        float k = sinf(ph * 3.14159f);
        if (k < 0.15f) continue;
        float x = gl->x + (ph - 0.5f) * (0.3f + 0.25f * wnd), z = gl->z;
        float w = 0.12f + 0.08f * k;
        uint32_t c = RGBA(200, 222, 238, (int)((70 + 60 * f + 40 * G->gust) * k));
        r_line(x - w, 0.015f, z, x + w * 0.6f, 0.015f, z - 0.02f * k, 1.6f, c & 0xFFFFFF00, c);
    }
    for (int i = 0; i < nfoam; i++) {
        const Foam *fm = &foam[i];
        float k = 0.5f + 0.5f * sinf(T * 2.1f + fm->ph);
        float push = 0.07f * k;
        uint32_t c = RGBA(220, 232, 240, (int)(45 + 55 * k + 40 * f));
        if (fm->z0 == fm->z1) r_gquad(fm->x0, fm->z0 - 0.05f - push, fm->x1, fm->z0 + 0.05f + push, 0.012f, c);
        else r_gquad(fm->x0 - 0.05f - push, fm->z0, fm->x0 + 0.05f + push, fm->z1, 0.012f, c);
    }
    gfx_blend(BL_ALPHA);
}

/* level 6: twin trails of fire burnt into the road, and blue glow under the time portals */
static void draw_fire_trails(void)
{
    gfx_blend(BL_ADD);
    for (int i = 0; i < MAX_TRAIL; i++) {
        const Trail *t = &G->trail[i];
        if (t->t >= t->life) continue;
        float k = 1.0f - t->t / t->life, dx = t->x1 - t->x0, dz = t->z1 - t->z0, l = sqrtf(dx * dx + dz * dz) + 1e-4f;
        float px = -dz / l * 0.17f, pz = dx / l * 0.17f;
        for (int s2 = -1; s2 <= 1; s2 += 2) {
            float ax = t->x0 + px * (float)s2, az = t->z0 + pz * (float)s2, bx = t->x1 + px * (float)s2, bz = t->z1 + pz * (float)s2;
            float fl = 0.8f + 0.2f * sinf(T * 30.0f + (float)(i * 3 + s2));
            r_glowline(ax, 0.02f, az, bx, 0.02f, bz, 9.0f, RGBA(255, 120, 30, (int)(150 * k * fl)));
            r_line(ax, 0.025f, az, bx, 0.025f, bz, 2.2f, RGBA(255, 230, 140, (int)(255 * k)), RGBA(255, 160, 60, (int)(200 * k)));
        }
    }
    for (int i = 0; i < nprop; i++)
        if (prop[i].kind == PR_PORTAL) r_gdisc(prop[i].x, prop[i].z, 0.02f, 1.1f, 12, RGBA(90, 160, 255, 70), 0x4080FF00);
    gfx_blend(BL_ALPHA);
}

static void draw_ground(void)
{
    rc_draw(SLOT_GROUND, SHX, SHY);
    if (ISLAND) draw_island_water();
    if (PIRATE) draw_storm_sea();
    if (FUTURE) draw_fire_trails();
    for (int i = 0; i < MAX_PT; i++) {                   /* mortar scorch marks, fading */
        const Part *p = &G->pt[i];
        if (p->life <= 0 || p->kind != PT_SCORCH) continue;
        float k = fminf(p->life / (p->max * 0.4f), 1.0f);
        r_gdisc(p->x, p->z, 0.008f, p->size, 16, col_a(p->c, k), col_a(p->c, 0));
        r_gdisc(p->x + 0.1f * cosf(p->rot), p->z + 0.1f * sinf(p->rot), 0.009f, p->size * 0.5f, 8, col_a(p->c, k * 0.6f), col_a(p->c, 0));
    }

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
/* autumn: round low-poly crowns in oranges, yellows and reds */
static void draw_autumn_tree(int c, int r, float x, float z, float s)
{
    float rot = hf(c, r, 10) * 3.0f;
    r_light_model(x, 0.9f * s, z);
    int far = !in_map(c, r);
    int ci = (int)(hf(c, r, 11) * 4.99f);
    uint32_t col = LEAF_COL[ci], col2 = col_mix(LEAF_COL[(ci + 1) % 5], col, 0.4f);
    float tint = 0.9f + 0.2f * hf(c, r, 12);
    col = scale_rgb(col, tint); col2 = scale_rgb(col2, tint);
    if (!far) r_prism(x, 0, z, 0.09f * s, 0.07f * s, 0.55f * s, 5, rot, 0x6A4630FF, 0x5A3A28FF);
    r_crystal(x, 0.95f * s, z, 0.62f * s, 0.62f * s, 0.42f * s, 7, rot, col);
    if (!far || hf(c, r, 13) < 0.5f)
        r_crystal(x + 0.22f * s, 1.42f * s, z - 0.08f * s, 0.38f * s, 0.36f * s, 0.26f * s, 6, rot + 0.5f, col2);
    r_light_off();
}

/* end of autumn: bare, crooked trees; some carry a hanging lantern */
static void draw_bare_tree(int c, int r, float x, float z, float s)
{
    int far = !in_map(c, r) && (c < -2 || c > MW + 1 || r < -2 || r > MH + 1);
    float rot = hf(c, r, 10) * 6.28f, H = 1.55f * s;
    uint32_t bark = scale_rgb(0x4A4038FF, 0.85f + 0.3f * hf(c, r, 11)), bark2 = scale_rgb(bark, 0.8f);
    r_light_model(x, 0.9f * s, z);
    int pt = rc_last_point();
    rc_sway(0.07f, 0.4f * s, H + 0.4f * s);
    r_prism(x, 0, z, 0.11f * s, 0.0f, H, 4, rot, bark, bark2);           /* trunk tapering to a point */
    int nb = far ? 3 : 4;
    float tipx = x, tipy = H, tipz = z;
    for (int k = 0; k < nb; k++) {
        float a = rot + (float)k * (6.28f / (float)nb) + (hf(c, r, 20 + k) - 0.5f) * 0.8f;
        float by = (0.5f + 0.25f * (float)k / (float)nb + 0.1f * hf(c, r, 30 + k)) * H;
        float L = (0.5f + 0.3f * hf(c, r, 40 + k)) * s;
        float dx = cosf(a), dz = sinf(a), up = 0.7f + 0.5f * hf(c, r, 50 + k), l = sqrtf(1 + up * up);
        float bx = x + dx * 0.03f, bz = z + dz * 0.03f;
        float ex = bx + dx * L / l, ey = by + up * L / l, ez = bz + dz * L / l;
        /* a crooked branch: two segments with a kink */
        float kx = bx + dx * L * 0.5f / l + (hf(c, r, 55 + k) - 0.5f) * 0.12f * s, ky = by + up * L * 0.45f / l, kz = bz + dz * L * 0.5f / l;
        r_stick(bx, by, bz, kx, ky, kz, 0.08f * s, 0.05f * s, bark);
        r_stick(kx, ky, kz, ex, ey, ez, 0.05f * s, 0.015f * s, bark);
        if (!far) {                                           /* a twig off each branch */
            float a2 = a + 0.9f * (hf(c, r, 60 + k) < 0.5f ? 1.0f : -1.0f);
            r_stick(kx, ky, kz, kx + cosf(a2) * 0.2f * s, ky + 0.22f * s, kz + sinf(a2) * 0.2f * s, 0.035f * s, 0.01f * s, bark2);
        }
        /* remember the branch that reaches out lowest towards the camera, for a lantern */
        if (ez > tipz + 0.05f || k == 0) { tipx = kx; tipy = ky; tipz = kz; }
    }
    rc_sway(0, 0, 1);
    r_light_off();
    /* lanterns on trees near the map, but not on every one */
    int near_map = c >= -2 && c <= MW + 1 && r >= -2 && r <= MH + 1;
    if (near_map && nlant < MAX_LANT && hf(c, r, 70) < 0.30f) {
        Lantern *L = &lant[nlant++];
        L->x = tipx; L->y = tipy; L->z = tipz;
        L->len = 0.22f + 0.18f * hf(c, r, 71);
        L->ph = hf(c, r, 72) * 6.28f;
        float u = (tipy - 0.4f * s) / H;
        L->amp = 0.07f * u * u;
        L->pt = pt;
        L->row = (uint8_t)((int)floorf(z) - ROW_MIN);
    }
}

static void draw_tree(int c, int r)
{
    float x, z, s;
    tree_pos(c, r, &x, &z, &s);
    if (!on_screen(x, 1.0f * s, z, 90.0f)) return;
    if (HALLOW) { draw_bare_tree(c, r, x, z, s); return; }
    if (AUTUMN) { rc_sway(0.05f, 0.5f * s, 1.8f * s); draw_autumn_tree(c, r, x, z, s); rc_sway(0, 0, 1); return; }
    rc_sway(0.08f, 0.3f * s, 2.0f * s);
    float sway = 0;
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
    rc_sway(0, 0, 1);
}

/* level 3: a tombstone (or a cross), a little crooked */
static void draw_grave(float x, float z, float k, float tilt, int cross)
{
    uint32_t st = scale_rgb(0x8A8A92FF, 0.8f + 0.3f * k), st2 = scale_rgb(st, 0.8f);
    r_light_model(x, 0.3f, z);
    r_prism(x, 0, z + 0.08f, 0.22f, 0.16f, 0.06f, 6, 0, 0x3E3428FF, 0x342A20FF);     /* mound */
    if (cross) {
        r_obox(x, 0, z, 0.04f, 0.04f, 0.5f, tilt, st, st2);
        r_obox(x, 0.3f, z, 0.17f, 0.04f, 0.38f, tilt, st, st2);
    } else {
        r_obox(x, 0, z, 0.17f, 0.06f, 0.36f, tilt, st, st2);
        r_obox(x, 0.36f, z, 0.12f, 0.06f, 0.44f, tilt, st, st2);
        r_obox(x, 0.44f, z, 0.06f, 0.06f, 0.48f, tilt, st, st2);
    }
    r_light_off();
}

static void draw_jack_body(float x, float z, float pr, float ang)
{
    r_light_model(x, 0.2f, z);
    r_prism(x, 0, z, pr, pr * 0.8f, pr * 1.4f, 8, ang, 0xE87A24FF, 0xD06A1CFF);
    r_prism(x, pr * 1.4f, z, 0.025f, 0.02f, pr * 1.4f + 0.08f, 4, 0, 0x4A5A20FF, 0x3A4A18FF);
    r_light_off();
}

static void draw_bush(int c, int r)
{
    float x = (float)c + 0.5f, z = (float)r + 0.5f;
    if (!on_screen(x, 0.3f, z, 60.0f)) return;
    float s = 0.6f + 0.4f * hf(c, r, 12);
    if (HALLOW) {
        float h3 = hf(c, r, 3);
        if (h3 < 0.5f) draw_grave(x + (hf(c, r, 4) - 0.5f) * 0.3f, z, hf(c, r, 5), (hf(c, r, 6) - 0.5f) * 0.5f, h3 < 0.15f);
        else if (h3 < 0.75f && njack < MAX_JACK) {
            Jack *j = &jack[njack++];
            j->x = x; j->z = z; j->r = 0.15f + 0.05f * hf(c, r, 7); j->ang = 1.2f + (hf(c, r, 8) - 0.5f) * 0.8f;
            j->ph = hf(c, r, 9) * 6.28f; j->row = (uint8_t)(r - ROW_MIN);
            draw_jack_body(x, z, j->r, j->ang);
        } else {
            r_light_model(x, 0.2f, z);
            for (int k = 0; k < 4; k++) {          /* a dead thorny shrub */
                float a = (float)k * 1.57f + hf(c, r, 10 + k);
                r_beam(x, 0.02f, z, cosf(a), 1.0f, sinf(a), 0.3f * s, 0.03f, 0.03f, 0x4A3E30FF);
            }
            r_light_off();
        }
        return;
    }
    r_light_model(x, 0.2f, z);
    if (AUTUMN && hf(c, r, 3) < 0.45f) {
        /* a little pumpkin patch */
        for (int k = 0; k < 2 + (int)(hf(c, r, 4) * 2.0f); k++) {
            float px = x + (hf(c, r, 20 + k) - 0.5f) * 0.6f, pz = z + (hf(c, r, 30 + k) - 0.5f) * 0.6f, pr = 0.12f + 0.08f * hf(c, r, 40 + k);
            r_prism(px, 0, pz, pr, pr * 0.8f, pr * 1.4f, 8, (float)k, 0xF08A28FF, 0xE07020FF);
            r_prism(px, pr * 1.4f, pz, 0.025f, 0.02f, pr * 1.4f + 0.08f, 4, 0, 0x5A6A2AFF, 0x4A5A20FF);
        }
        r_light_off();
        return;
    }
    r_crystal(x, 0.18f * s, z, 0.38f * s, 0.25f * s, 0.18f * s, 6, hf(c, r, 13) * 3.0f, AUTUMN ? 0xB0482CFF : 0x2E6440FF);
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

/* level 5: a wooden dock on posts (towers go on top) */
static void draw_dock(int c, int r, const Tile *t)
{
    float x0 = (float)c, z0 = (float)r, x1 = x0 + 1, z1 = z0 + 1, h = t->h;
    int skip = 0;
    if (solid(c - 1, r)) skip |= SK_LEFT;
    if (solid(c + 1, r)) skip |= SK_RIGHT;
    if (solid(c, r + 1)) skip |= SK_FRONT;
    if (solid(c, r - 1)) skip |= SK_BACK;
    float v = 0.88f + 0.24f * hf(c, r, 14);
    uint32_t top = scale_rgb(0x8C6A46FF, v), side = scale_rgb(0x5A4028FF, v);
    r_box(x0, h - 0.1f, z0, x1, h, z1, top, side, skip);                       /* deck */
    r_light_model(x0 + 0.5f, h, z0 + 0.5f);
    int horiz = ((c + r) & 1) ? 1 : 0;                                          /* planks run either way, patchwork */
    if (solid(c - 1, r) || solid(c + 1, r)) horiz = 1;
    if (solid(c, r - 1) || solid(c, r + 1)) horiz = solid(c - 1, r) || solid(c + 1, r) ? horiz : 0;
    float y = h + 0.003f, w = 0.012f;
    uint32_t line = scale_rgb(0x3A2818FF, v);
    for (int k = 1; k < 5; k++) {
        float o = (float)k / 5.0f;
        if (horiz) mortar_top(x0, z0 + o - w, x1, z0 + o + w, y, line);
        else mortar_top(x0 + o - w, z0, x0 + o + w, z1, y, line);
    }
    r_light_off();
    /* posts at the open corners, standing in the water */
    for (int k = 0; k < 4; k++) {
        int dc = (k & 1) ? 1 : -1, dr = (k & 2) ? 1 : -1;
        if (solid(c + dc, r) || solid(c, r + dr)) continue;
        float px = (k & 1) ? x1 - 0.1f : x0 + 0.1f, pz = (k & 2) ? z1 - 0.1f : z0 + 0.1f;
        r_light_model(px, 0.2f, pz);
        r_prism(px, -0.05f, pz, 0.075f, 0.07f, h + 0.04f, 6, 0.3f, 0x6A4C30FF, 0x4A3420FF);
        r_light_off();
    }
}

/* level 5: a cargo crate, painted, with a darker frame */
static void draw_crate(int c, int r, const Tile *t)
{
    float x0 = (float)c + 0.06f, z0 = (float)r + 0.06f, x1 = (float)c + 0.94f, z1 = (float)r + 0.94f, h = t->h;
    uint32_t col = col_mix(BLOCK_COL[t->color], 0x8A6A48FF, 0.35f);
    uint32_t frame = scale_rgb(col, 0.55f);
    r_box(x0, -0.04f, z0, x1, h, z1, col, scale_rgb(col, 0.8f), 0);
    r_light_model((float)c + 0.5f, h, (float)r + 0.5f);
    float y = h + 0.003f, w = 0.05f;
    mortar_top(x0, z0, x1, z0 + w, y, frame); mortar_top(x0, z1 - w, x1, z1, y, frame);
    mortar_top(x0, z0, x0 + w, z1, y, frame); mortar_top(x1 - w, z0, x1, z1, y, frame);
    mortar_top(x0, (z0 + z1) * 0.5f - w * 0.5f, x1, (z0 + z1) * 0.5f + w * 0.5f, y, frame);   /* a plank across the lid */
    float zf = z1 + 0.003f;
    mortar_front(x0, 0, x0 + w, h, zf, frame); mortar_front(x1 - w, 0, x1, h, zf, frame);
    mortar_front(x0, h - w, x1, h, zf, frame); mortar_front(x0, h * 0.45f, x1, h * 0.45f + w, zf, frame);
    r_light_off();
}

/* level 6: a raised concrete sidewalk with paving seams and a neon strip along the kerb */
static void draw_sidewalk(int c, int r, const Tile *t)
{
    float x0 = (float)c, z0 = (float)r, x1 = x0 + 1, z1 = z0 + 1, h = t->h;
    int skip = 0;
    if (solid(c - 1, r)) skip |= SK_LEFT;
    if (solid(c + 1, r)) skip |= SK_RIGHT;
    if (solid(c, r + 1)) skip |= SK_FRONT;
    if (solid(c, r - 1)) skip |= SK_BACK;
    float v = 0.92f + 0.12f * hf(c, r, 14);
    r_box(x0, 0, z0, x1, h, z1, scale_rgb(0x9A9AA6FF, v), scale_rgb(0x74747EFF, v), skip);
    r_light_model(x0 + 0.5f, h, z0 + 0.5f);
    float y = h + 0.003f, w = 0.012f;
    uint32_t line = 0x5E5E68FF;
    mortar_top(x0 + 0.5f - w, z0, x0 + 0.5f + w, z1, y, line);
    mortar_top(x0, z0 + 0.5f - w, x1, z0 + 0.5f + w, y, line);
    /* neon kerb strip where it meets the street (cyan and magenta, alternating blocks) */
    uint32_t neon = ((c / 2 + r / 2) & 1) ? 0x60F0FFFF : 0xFF60D0FF;
    if (!solid(c, r + 1) && kind_at(c, r + 1) == TL_PATH) { mortar_front(x0, h * 0.45f, x1, h * 0.8f, z1 + 0.004f, neon); mortar_top(x0, z1 - 0.07f, x1, z1 - 0.02f, y + 0.001f, neon); }
    if (!solid(c, r - 1) && kind_at(c, r - 1) == TL_PATH) mortar_top(x0, z0 + 0.02f, x1, z0 + 0.07f, y + 0.001f, neon);
    if (!solid(c - 1, r) && kind_at(c - 1, r) == TL_PATH) mortar_top(x0 + 0.02f, z0, x0 + 0.07f, z1, y + 0.001f, neon);
    if (!solid(c + 1, r) && kind_at(c + 1, r) == TL_PATH) mortar_top(x1 - 0.07f, z0, x1 - 0.02f, z1, y + 0.001f, neon);
    r_light_off();
}

/* level 6: a gunmetal tech crate with a glowing coloured panel on top */
static void draw_tech_crate(int c, int r, const Tile *t)
{
    float x0 = (float)c + 0.06f, z0 = (float)r + 0.06f, x1 = (float)c + 0.94f, z1 = (float)r + 0.94f, h = t->h;
    uint32_t col = BLOCK_COL[t->color];
    r_box(x0, 0, z0, x1, h, z1, 0x50545EFF, 0x3E424CFF, 0);
    r_light_model((float)c + 0.5f, h, (float)r + 0.5f);
    float y = h + 0.003f;
    mortar_top(x0 + 0.12f, z0 + 0.12f, x1 - 0.12f, z1 - 0.12f, y, col_mix(col, 0xFFFFFFFF, 0.25f));
    mortar_top(x0 + 0.2f, z0 + 0.2f, x1 - 0.2f, z1 - 0.2f, y + 0.001f, col);
    mortar_front(x0 + 0.1f, h * 0.3f, x1 - 0.1f, h * 0.4f, z1 + 0.004f, col);
    mortar_front(x0 + 0.1f, h * 0.55f, x0 + 0.3f, h * 0.75f, z1 + 0.004f, 0x202428FF);
    r_light_off();
}

/* level 7: a soft bank of cloud (towers sit on top) */
static void draw_cloudbank(int c, int r, const Tile *t)
{
    float x0 = (float)c, z0 = (float)r, x1 = x0 + 1, z1 = z0 + 1, h = t->h;
    int skip = 0;
    if (solid(c - 1, r)) skip |= SK_LEFT;
    if (solid(c + 1, r)) skip |= SK_RIGHT;
    if (solid(c, r + 1)) skip |= SK_FRONT;
    if (solid(c, r - 1)) skip |= SK_BACK;
    r_box(x0, 0, z0, x1, h, z1, 0xFAF8FFFF, 0xE0DCF0FF, skip);
    r_light_model(x0 + 0.5f, h, z0 + 0.5f);
    for (int k = 0; k < 4; k++) {                          /* puffs round the rim */
        float px = x0 + ((k & 1) ? 0.82f : 0.18f) + (hf(c, r, 50 + k) - 0.5f) * 0.12f, pz = z0 + ((k & 2) ? 0.82f : 0.18f);
        if (hf(c, r, 60 + k) < 0.35f) continue;
        r_crystal(px, h - 0.02f, pz, 0.17f + 0.06f * hf(c, r, 70 + k), 0.1f, 0.05f, 6, hf(c, r, 80 + k) * 3.0f, 0xFFFFFFFF);
    }
    r_light_off();
}

/* level 7: a candy gem block, pastel, with a white sugar rim */
static void draw_candy(int c, int r, const Tile *t)
{
    float x0 = (float)c + 0.05f, z0 = (float)r + 0.05f, x1 = (float)c + 0.95f, z1 = (float)r + 0.95f, h = t->h;
    uint32_t col = col_mix(BLOCK_COL[t->color], 0xFFFFFFFF, 0.38f);
    r_box(x0, 0, z0, x1, h - 0.05f, z1, col, scale_rgb(col, 0.85f), 0);
    r_box(x0 + 0.06f, h - 0.05f, z0 + 0.06f, x1 - 0.06f, h, z1 - 0.06f, 0xFFFFFFFF, col_mix(col, 0xFFFFFFFF, 0.5f), 0);
    r_light_model((float)c + 0.5f, h, (float)r + 0.5f);
    for (int k = 0; k < 3; k++)                              /* candy stripes down the front */
        mortar_front(x0 + 0.12f + 0.28f * (float)k, 0.05f, x0 + 0.22f + 0.28f * (float)k, h - 0.08f, z1 + 0.004f, col_mix(col, 0xFFFFFFFF, 0.6f));
    r_light_off();
}

static void draw_wall(int c, int r, const Tile *t)
{
    if (SKYLV) { draw_cloudbank(c, r, t); return; }
    if (FUTURE) { draw_sidewalk(c, r, t); return; }
    if (PIRATE) { draw_dock(c, r, t); return; }
    float x0 = (float)c, z0 = (float)r, x1 = x0 + 1, z1 = z0 + 1, h = t->h;
    int skip = 0;
    if (solid(c - 1, r)) skip |= SK_LEFT;
    if (solid(c + 1, r)) skip |= SK_RIGHT;
    if (solid(c, r + 1)) skip |= SK_FRONT;
    if (solid(c, r - 1)) skip |= SK_BACK;
    float v = 0.9f + 0.2f * hf(c, r, 14);
    uint32_t top = scale_rgb(0xA05C46FF, v), side = scale_rgb(0x844A38FF, v), line = 0x4E2C22FF;
    if (AUTUMN) { top = scale_rgb(0xB4A68CFF, v); side = scale_rgb(0x968A72FF, v); line = 0x6A604EFF; }   /* old stone */
    if (HALLOW) { top = scale_rgb(0x74727CFF, v); side = scale_rgb(0x5C5A66FF, v); line = 0x3A3844FF; }   /* dark mossy stone */
    if (ISLAND) { top = scale_rgb(0xC2BCAAFF, v); side = scale_rgb(0xA09A88FF, v); line = 0x7E786AFF; }   /* pale weathered limestone */
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
    if (PIRATE) { draw_crate(c, r, t); return; }
    if (FUTURE) { draw_tech_crate(c, r, t); return; }
    if (SKYLV) { draw_candy(c, r, t); return; }
    float x0 = (float)c + 0.03f, z0 = (float)r + 0.03f, x1 = (float)c + 0.97f, z1 = (float)r + 0.97f;
    uint32_t col = BLOCK_COL[t->color];
    uint32_t side = scale_rgb(col, 0.82f), top = col;
    uint32_t cap = col_mix(col, 0xFFFFFFFF, 0.22f);
    r_box(x0, 0, z0, x1, t->h - 0.06f, z1, top, side, 0);
    r_box(x0 + 0.08f, t->h - 0.06f, z0 + 0.08f, x1 - 0.08f, t->h, z1 - 0.08f, cap, col, 0);
}

/* level 5: the treasure - a great iron-bound chest lid thrown open, heaped with gold that shines through the storm;
 * as the pirates plunder it the glow dims. Two tiki torches. */
static void draw_treasure(void)
{
    float fv = G->fire_vis, x = FIRE_X, z = FIRE_Z + 0.1f, y0 = 0.22f;
    r_light_model(x, 0.6f, z + 0.6f);
    uint32_t wood = 0x6A3E22FF, wood2 = 0x4E2C16FF, iron = 0x3A3A40FF, brass = 0xC89838FF;
    r_box(x - 0.42f, y0, z - 0.26f, x + 0.42f, y0 + 0.36f, z + 0.26f, wood, wood2, 0);                   /* the chest */
    for (int k = -1; k <= 1; k += 2)                                                                       /* iron bands */
        r_box(x + 0.25f * (float)k - 0.04f, y0 - 0.005f, z - 0.27f, x + 0.25f * (float)k + 0.04f, y0 + 0.365f, z + 0.27f, iron, iron, 0);
    r_box(x - 0.43f, y0 + 0.3f, z - 0.27f, x + 0.43f, y0 + 0.37f, z + 0.27f, brass, scale_rgb(brass, 0.8f), 0);
    r_box(x - 0.06f, y0 + 0.18f, z + 0.26f, x + 0.06f, y0 + 0.3f, z + 0.29f, brass, brass, 0);               /* lock plate */
    /* the lid, thrown back */
    float lt = 0.08f * sinf(T * 1.7f) * G->fire_hit;
    float lp[4][3] = { {x - 0.42f, y0 + 0.37f, z - 0.26f}, {x + 0.42f, y0 + 0.37f, z - 0.26f}, {x + 0.42f, y0 + 0.78f + lt, z - 0.42f}, {x - 0.42f, y0 + 0.78f + lt, z - 0.42f} };
    r_face(4, (const float (*)[3])lp, 0, 0.4f, 1.0f, 0x7A4828FF);
    r_box(x - 0.42f, y0 + 0.74f + lt, z - 0.46f, x + 0.42f, y0 + 0.8f + lt, z - 0.38f, brass, scale_rgb(brass, 0.8f), 0);
    /* the gold heaped inside (sinks as it is plundered), a few coins and a goblet spilled on the sand */
    float heap = 0.04f + 0.12f * fv;
    r_crystal(x, y0 + 0.36f, z, 0.36f, heap, 0.0f, 8, 0.3f, 0xF0C040FF);
    r_crystal(x + 0.15f, y0 + 0.36f + heap * 0.6f, z - 0.05f, 0.14f, heap * 0.8f, 0.02f, 6, 1.0f, 0xFFD860FF);
    for (int i = 0; i < 6; i++) {
        float an = (float)i * 1.1f + 0.4f;
        r_prism(x + cosf(an) * (0.55f + 0.1f * (float)(i & 1)), 0.22f, z + 0.25f + sinf(an) * 0.2f, 0.05f, 0.05f, 0.235f, 6, 0, 0xFFD050FF, 0xD0A030FF);
    }
    r_prism(x + 0.58f, 0.22f, z + 0.12f, 0.05f, 0.08f, 0.38f, 6, 0, 0xE0B848FF, 0xB89030FF);
    r_crystal(x - 0.12f, y0 + 0.42f + heap, z + 0.05f, 0.05f, 0.05f, 0.05f, 6, T, 0xFF3050FF);                   /* a ruby */
    r_crystal(x + 0.05f, y0 + 0.40f + heap, z - 0.1f, 0.045f, 0.05f, 0.04f, 6, -T, 0x40E080FF);                  /* an emerald */
    /* tiki torches */
    for (int s2 = -1; s2 <= 1; s2 += 2) {
        float tx = FIRE_X + 1.05f * (float)s2, tz = FIRE_Z + 0.75f;
        r_prism(tx, 0.1f, tz, 0.05f, 0.04f, 0.85f, 5, 0, 0x6A4A2CFF, 0x4E361EFF);
        r_prism(tx, 0.78f, tz, 0.09f, 0.11f, 0.92f, 6, 0, 0x8A6A3AFF, 0x6A4E2AFF);
    }
    r_light_off();
    gfx_blend(BL_ADD);
    r_unlit = 1;
    for (int s2 = -1; s2 <= 1; s2 += 2) {
        float tx = FIRE_X + 1.05f * (float)s2, tz = FIRE_Z + 0.75f, k = flicker((float)s2 * 3.0f);
        float lean = (G->wind + G->gust * 2.0f) * 0.08f;
        r_prism(tx, 0.9f, tz, 0.1f * k, 0.0f, 1.2f * k, 5, T * 3.0f, RGBA(255, 150, 50, 200), RGBA(255, 120, 30, 200));
        r_prism(tx + lean, 0.9f, tz, 0.05f, 0.0f, 1.05f + 0.06f * k, 5, -T * 4.0f, RGBA(255, 230, 140, 255), RGBA(255, 220, 120, 255));
        r_glow(tx, 1.0f, tz, 0.55f * k, RGBA(255, 140, 50, 150));
    }
    r_unlit = 0;
    if (fv > 0.03f) {
        float pulse = 0.85f + 0.15f * sinf(T * 2.3f);
        r_glow(x, y0 + 0.5f, z, 1.6f * fv * pulse, RGBA(255, 200, 80, 120));
        r_glow(x, y0 + 0.45f, z, 0.55f * fv, RGBA(255, 240, 170, 200));
        /* glints twinkling on the gold */
        for (int i = 0; i < 4; i++) {
            float tw = sinf(T * 3.3f + (float)i * 1.9f);
            if (tw < 0.6f) continue;
            float gx = x + cosf((float)i * 2.2f) * 0.22f, gz = z + sinf((float)i * 2.2f) * 0.12f, gy = y0 + 0.4f + heap;
            float sx, sy, k = (tw - 0.6f) / 0.4f * fv;
            proj(gx, gy, gz, &sx, &sy);
            uint32_t c = RGBA(255, 250, 220, (int)(255 * k));
            gfx_line(sx - 7 * k, sy, sx + 7 * k, sy, 1.5f, c, c);
            gfx_line(sx, sy - 7 * k, sx, sy + 7 * k, 1.5f, c, c);
        }
    }
    gfx_blend(BL_ALPHA);
}

/* level 6: the time machine - a stainless gull-wing sports car on hover pods, gull-wing doors up, time circuit
 * coils glowing along the back, and the flux capacitor pulsing in a Y */
static void draw_delorean(void)
{
    float fv = G->fire_vis, sg = clampf(G->surge_t / 1.6f, 0, 1);
    float x = FIRE_X, z = FIRE_Z + 0.05f, hy = 0.24f + 0.035f * sinf(T * 2.0f);
    float flick = fv < 0.5f && sinf(T * 37.0f) > 0.6f ? 0.3f : 1.0f;          /* low on power: it splutters */
    /* hover glow and the wheels folded flat underneath */
    gfx_blend(BL_ADD);
    r_gdisc(x, z, 0.012f, 1.5f, 16, RGBA(80, 150, 255, (int)(70 + 60 * sg)), 0x4080FF00);
    gfx_blend(BL_ALPHA);
    r_scale(x, 0, z, 1.22f);
    wk_x = x; wk_z = z; wk_s = 1.22f;
    r_light_model(x, hy + 0.3f, z + 0.6f);
    for (int k = 0; k < 4; k++) {
        float wx = x + ((k & 1) ? 0.58f : -0.58f), wz = z + ((k & 2) ? 0.36f : -0.36f);
        r_prism(wx, hy - 0.06f, wz, 0.12f, 0.12f, hy - 0.01f, 10, 0, 0x202024FF, 0x18181CFF);
    }
    uint32_t steel = 0xE0E4EAFF, steel2 = 0xA8ACB4FF, glass = 0x202838FF, dark = 0x1C1C22FF;
    /* a low wedge: the body, a tapered nose, the black stripe, a sloping bonnet up to the windscreen */
    r_obox(x - 0.16f, hy, z, 0.78f, 0.4f, hy + 0.13f, 0, steel, steel2);
    r_obox(x + 0.72f, hy, z, 0.2f, 0.34f, hy + 0.1f, 0, steel, steel2);
    r_obox(x - 0.16f, hy + 0.055f, z, 0.785f, 0.405f, hy + 0.08f, 0, dark, dark);
    r_obox(x - 0.5f, hy + 0.13f, z, 0.44f, 0.37f, hy + 0.21f, 0, steel, steel2);          /* rear deck */
    {
        float p[4][3] = { {x + 0.02f, hy + 0.21f, z - 0.37f}, {x + 0.02f, hy + 0.21f, z + 0.37f}, {x + 0.62f, hy + 0.13f, z + 0.38f}, {x + 0.62f, hy + 0.13f, z - 0.38f} };
        r_face(4, (const float (*)[3])p, 0.13f, 1.0f, 0, steel);
        float q[4][3] = { {x + 0.02f, hy + 0.13f, z + 0.4f}, {x + 0.62f, hy + 0.13f, z + 0.4f}, {x + 0.62f, hy + 0.13f, z + 0.38f}, {x + 0.02f, hy + 0.21f, z + 0.37f} };
        r_face(4, (const float (*)[3])q, 0, 0, 1, steel2);
    }
    {   /* windscreen and the low cabin roof */
        float p[4][3] = { {x + 0.02f, hy + 0.21f, z - 0.3f}, {x + 0.02f, hy + 0.21f, z + 0.3f}, {x - 0.16f, hy + 0.34f, z + 0.27f}, {x - 0.16f, hy + 0.34f, z - 0.27f} };
        r_face(4, (const float (*)[3])p, 0.6f, 1.0f, 0, glass);
    }
    r_obox(x - 0.28f, hy + 0.21f, z, 0.12f, 0.27f, hy + 0.34f, 0, steel, glass);
    for (int k = 0; k < 5; k++)                                                           /* rear louvres */
        mortar_top(x - 0.9f + 0.08f * (float)k, z - 0.3f, x - 0.87f + 0.08f * (float)k, z + 0.3f, hy + 0.212f, dark);
    r_prism(x - 0.76f, hy + 0.21f, z + 0.14f, 0.07f, 0.06f, hy + 0.33f, 8, 0, 0xF0F0F0FF, 0xD8D8DCFF);   /* the kitchen-appliance fusion reactor */
    r_prism(x - 0.76f, hy + 0.33f, z + 0.14f, 0.06f, 0.0f, hy + 0.39f, 8, 0, 0xE0E0E4FF, 0xC8C8CCFF);
    r_light_off();
    /* gull-wing doors, swung up and breathing open a little */
    float a = 1.1f + 0.1f * sinf(T * 0.8f);
    for (int s2 = -1; s2 <= 1; s2 += 2) {
        float hz = z + 0.27f * (float)s2, hyy = hy + 0.34f;
        float ez = hz + cosf(a) * 0.3f * (float)s2, ey = hyy + sinf(a) * 0.3f;
        uint32_t dc = lt_shade(s2 > 0 ? 0xB8BCC4FF : steel, x, ey, ez, 1.0f);
        wtri(x - 0.36f, hyy, hz, x + 0.06f, hyy, hz, x + 0.06f, ey, ez, dc);
        wtri(x - 0.36f, hyy, hz, x + 0.06f, ey, ez, x - 0.36f, ey, ez, dc);
    }
    gfx_blend(BL_ADD);
    /* time circuit coils down the back */
    float coil = (0.55f + 0.45f * sinf(T * 6.0f)) * flick * (0.5f + 0.5f * fv) + sg;
    for (int s2 = -1; s2 <= 1; s2 += 2) {
        r_glowline(x - 0.92f, hy + 0.19f, z + 0.38f * (float)s2, x - 0.4f, hy + 0.21f, z + 0.4f * (float)s2, 6.0f, RGBA(120, 190, 255, (int)(150 * fminf(coil, 1.0f))));
        r_line(x - 0.92f, hy + 0.19f, z + 0.38f * (float)s2, x - 0.4f, hy + 0.21f, z + 0.4f * (float)s2, 1.6f, RGBA(220, 240, 255, (int)(255 * fminf(coil, 1.0f))), RGBA(220, 240, 255, (int)(255 * fminf(coil, 1.0f))));
    }
    /* the flux capacitor: three arms of light meeting in a Y, pulses racing inwards */
    float fx = x - 0.5f, fy = hy + 0.22f, fz2 = z - 0.08f, L = 0.17f;
    float fk = (0.6f + 0.4f * sinf(T * 12.0f)) * flick * fv + sg * 1.5f;
    for (int arm = 0; arm < 3; arm++) {
        float an = -1.5708f + (float)arm * 2.0944f;
        float ex = fx + cosf(an) * L, ez = fz2 + sinf(an) * L;
        r_line(fx, fy, fz2, ex, fy, ez, 2.4f, RGBA(255, 250, 220, (int)(255 * fminf(fk, 1.0f))), RGBA(255, 220, 120, (int)(200 * fminf(fk, 1.0f))));
        for (int d = 0; d < 2; d++) {
            float u = 1.0f - fmodf(T * 3.0f + (float)d * 0.5f + (float)arm * 0.13f, 1.0f);
            r_glow_cheap(fx + cosf(an) * L * u, fy + 0.005f, fz2 + sinf(an) * L * u, 0.05f, RGBA(255, 255, 230, (int)(230 * fminf(fk, 1.0f))));
        }
    }
    r_glow(fx, fy, fz2, (0.35f + 0.5f * sg) * (0.4f + 0.6f * fv), RGBA(255, 240, 190, (int)(110 * fminf(fk, 1.0f))));
    /* head and tail lights, and the hover pods where the wheels folded down */
    for (int s2 = -1; s2 <= 1; s2 += 2) {
        r_glow_cheap(x + 0.92f, hy + 0.07f, z + 0.25f * (float)s2, 0.1f, RGBA(255, 250, 220, 230));
        r_line(x - 0.95f, hy + 0.09f, z + 0.12f * (float)s2, x - 0.95f, hy + 0.09f, z + 0.36f * (float)s2, 3.0f, RGBA(255, 50, 40, 230), RGBA(255, 50, 40, 230));
    }
    for (int k = 0; k < 4; k++)
        r_glow_cheap(x + ((k & 1) ? 0.58f : -0.58f), hy - 0.05f, z + ((k & 2) ? 0.36f : -0.36f), 0.13f, RGBA(110, 190, 255, (int)(200 * (0.7f + 0.3f * sinf(T * 5.0f + (float)k)))));
    /* sparks of loose energy crawling over the body during a surge or a lightning strike */
    if (sg > 0 || G->giga_fx > 0) {
        float k = fmaxf(sg, clampf(G->giga_fx / 2.0f, 0, 1));
        for (int a2 = 0; a2 < 3; a2++) {
            float u = fmodf(T * 2.0f + (float)a2 * 0.37f, 1.0f), px = x - 0.9f + 1.8f * u, j = sinf(T * 50.0f + (float)a2 * 2.0f) * 0.15f;
            r_line(px, hy + 0.35f, z - 0.4f, px + 0.15f, hy + 0.55f + j, z + 0.4f, 1.6f, RGBA(180, 220, 255, (int)(255 * k)), RGBA(180, 220, 255, 0));
        }
    }
    gfx_blend(BL_ALPHA);
    r_scale_off();
    wk_s = 1.0f;
}

/* level 7: the princess's castle - pink walls, four turrets with pointed roofs and flags, a tall keep crowned with a
 * glowing magic orb, warm windows, all standing on a cloud */
static void draw_castle(void)
{
    float x = FIRE_X, z = FIRE_Z, fv = G->fire_vis;
    r_light_model(x, 1.0f, z + 1.5f);
    for (int k = 0; k < 7; k++) {                                                    /* the cloud it stands on */
        float a = (float)k / 7.0f * 6.2832f + 0.3f;
        r_crystal(x + cosf(a) * 1.3f, 0.05f, z + sinf(a) * 1.2f, 0.42f, 0.18f, 0.06f, 6, a, 0xFFFFFFFF);
    }
    uint32_t wall = 0xF8C8DCFF, wall2 = 0xE0A8C4FF, roof = 0x8060E0FF, roof2 = 0x6448C0FF, gold = 0xFFD050FF;
    r_box(x - 0.95f, 0.05f, z - 0.95f, x + 0.95f, 0.6f, z + 0.95f, wall, wall2, 0);             /* curtain walls */
    for (int k = 0; k < 7; k++) {                                                    /* crenellations front and back */
        float bx = x - 0.85f + 0.283f * (float)k;
        r_box(bx - 0.06f, 0.6f, z + 0.84f, bx + 0.06f, 0.7f, z + 0.95f, wall, wall2, 0);
        r_box(bx - 0.06f, 0.6f, z - 0.95f, bx + 0.06f, 0.7f, z - 0.84f, wall, wall2, 0);
    }
    r_box(x - 0.18f, 0.05f, z + 0.951f, x + 0.18f, 0.42f, z + 0.96f, 0x6A3A50FF, 0x6A3A50FF, 0);   /* the gate */
    r_box(x - 0.45f, 0.6f, z - 0.45f, x + 0.45f, 1.25f, z + 0.45f, wall, wall2, 0);            /* keep */
    r_prism(x, 1.25f, z, 0.3f, 0.28f, 2.05f, 10, 0, wall, wall2);                               /* tall central tower */
    r_prism(x, 2.05f, z, 0.38f, 0.0f, 2.6f, 10, 0, roof, roof2);
    r_prism(x, 2.55f, z, 0.03f, 0.02f, 2.75f, 4, 0, gold, gold);
    for (int k = 0; k < 4; k++) {                                                    /* four turrets */
        float tx = x + ((k & 1) ? 0.95f : -0.95f), tz = z + ((k & 2) ? 0.95f : -0.95f);
        r_prism(tx, 0.05f, tz, 0.26f, 0.24f, 1.25f, 10, 0, wall, wall2);
        r_prism(tx, 1.25f, tz, 0.32f, 0.0f, 1.85f, 10, 0, roof, roof2);
        r_prism(tx, 1.82f, tz, 0.02f, 0.015f, 2.05f, 4, 0, gold, gold);
    }
    r_light_off();
    /* pennants fluttering on the turrets */
    for (int k = 0; k < 4; k++) {
        float tx = x + ((k & 1) ? 0.95f : -0.95f), tz = z + ((k & 2) ? 0.95f : -0.95f), fl = sinf(T * 6.0f + (float)k) * 0.04f;
        uint32_t fc = lt_shade(k & 1 ? 0xFF60A0FF : 0x60C0FFFF, tx, 2.0f, tz, 1.0f);
        wtri(tx, 2.03f, tz, tx, 1.93f, tz, tx + 0.22f, 1.98f + fl, tz + fl, fc);
    }
    gfx_blend(BL_ADD);
    for (int k = 0; k < 6; k++) {                                                    /* warm windows */
        float wx = x - 0.3f + 0.3f * (float)(k % 3), wy = k < 3 ? 0.85f : 1.6f;
        if (k >= 3 && k != 4) continue;
        r_glow_cheap(wx, wy, z + (k < 3 ? 0.46f : 0.29f), 0.06f, RGBA(255, 220, 140, (int)(200 * fv)));
    }
    for (int k = 0; k < 4; k++) {
        float tx = x + ((k & 1) ? 0.95f : -0.95f), tz = z + ((k & 2) ? 0.95f : -0.95f);
        r_glow_cheap(tx, 0.9f, tz + 0.26f, 0.05f, RGBA(255, 220, 140, (int)(200 * fv)));
    }
    /* the magic orb, pulsing pink and blue, flaring when it fires a rainbow */
    float fx = clampf(G->giga_fx / 2.0f, 0, 1), pulse = 0.75f + 0.25f * sinf(T * 3.0f);
    uint32_t oc = col_mix(RGBA(255, 140, 220, 255), RGBA(150, 200, 255, 255), 0.5f + 0.5f * sinf(T * 1.3f));
    r_glow(x, 2.85f, z, (0.35f + 0.25f * pulse) * (0.4f + 0.6f * fv) + fx * 1.4f, col_a(oc, 0.6f));
    r_glow_cheap(x, 2.85f, z, 0.1f + 0.05f * pulse, RGBA(255, 255, 255, 240));
    for (int k = 0; k < 3; k++) {                                                    /* sparkles circling the orb */
        float a = T * 2.0f + (float)k * 2.094f;
        r_glow_cheap(x + cosf(a) * 0.22f, 2.85f + sinf(a * 2.0f) * 0.06f, z + sinf(a) * 0.22f, 0.04f, RGBA(255, 250, 200, 230));
    }
    gfx_blend(BL_ALPHA);
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
            if (recoil > 0.45f) {                                           /* muzzle flash: a quick four-point star */
                float sx2, sy2, L = 6.0f + 10.0f * recoil, rot = anim * 9.0f;
                proj(mx + ca * 0.06f, y0 + 0.17f, mz + sa * 0.06f, &sx2, &sy2);
                uint32_t fc = RGBA(200, 250, 255, (int)(255 * recoil));
                for (int k = 0; k < 4; k++) {
                    float a2 = rot + (float)k * 1.5708f;
                    gfx_line(sx2, sy2, sx2 + cosf(a2) * L, sy2 + sinf(a2) * L, 2.2f, fc, fc & 0xFFFFFF00);
                }
            }
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
    case TW_PRISM: {
        /* a dark plinth with two counter-turning rings and a floating many-coloured prism */
        float y0 = bh;
        r_prism(x, y0, z, 0.22f * s, 0.16f * s, y0 + 0.22f * s, 6, 0.0f, 0x403850FF, 0x302840FF);
        r_prism(x, y0 + 0.22f * s, z, 0.1f * s, 0.07f * s, y0 + 0.5f * s, 6, 0.0f, 0x605878FF, 0x484060FF);
        float py = y0 + (0.78f + 0.04f * sinf(anim * 2.0f)) * s;
        r_unlit = 1;
        static const uint32_t PC[3] = { 0xFF80F0FF, 0x80E8FFFF, 0xFFF080FF };
        r_crystal(x, py, z, (0.14f + 0.02f * (float)lvl) * s, 0.22f * s, 0.18f * s, 3, anim * 1.6f, PC[0]);
        r_crystal(x, py, z, (0.1f + 0.02f * (float)lvl) * s, 0.26f * s, 0.2f * s, 3, anim * 1.6f + 1.047f, PC[1 + (lvl > 0)]);
        r_unlit = 0;
        for (int k = 0; k < 2; k++) {
            float rr = (0.24f + 0.06f * (float)k) * s, sp = (k ? -1.3f : 1.0f) * anim * (1.0f + tw_charge * 3.0f);
            float tl = 0.5f + 0.3f * (float)k, st = sinf(tl), ct = cosf(tl), cs = cosf(sp), ss = sinf(sp);
            float ox = 0, oy = 0, oz = 0;
            for (int i = 0; i <= 12; i++) {
                float a2 = (float)i / 12.0f * 6.2832f, c2 = cosf(a2), s2 = sinf(a2);
                float lx = c2 * rr, ly = s2 * rr * st, lz = s2 * rr * ct;
                float px = x + lx * cs - lz * ss, pyy = py + ly, pz = z + lx * ss + lz * cs;
                if (i) r_line(ox, oy, oz, px, pyy, pz, 2.0f, 0xC8A0F0FF, 0xC8A0F0FF);
                ox = px; oy = pyy; oz = pz;
            }
        }
        if (glow) {
            gfx_blend(BL_ADD);
            r_glow(x, py, z, 0.35f + 0.35f * tw_charge, RGBA(255, 140, 255, (int)(110 + 120 * tw_charge)));
            gfx_blend(BL_ALPHA);
        }
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
            r_glow(x, y1 + 0.12f, z, 0.38f + pulse * 0.5f + tw_charge * 0.18f, RGBA(255, 240, 140, (int)(130 + 120 * pulse + 60 * tw_charge)));
            if (tw_charge > 0.7f) {                                         /* charging: sparks jump down to the rings */
                for (int k = 0; k < 2; k++) {
                    float a = anim * 23.0f + (float)k * 3.1f, jx = sinf(anim * 57.0f + (float)k) * 0.04f;
                    float ex = x + cosf(a) * 0.2f, ez = z + sinf(a) * 0.2f, ey = y0 + 0.25f;
                    float mxx = (x + ex) * 0.5f + jx, myy = (y1 + 0.12f + ey) * 0.5f, mzz = (z + ez) * 0.5f - jx;
                    uint32_t ac = RGBA(255, 250, 200, (int)(255 * (tw_charge - 0.7f) / 0.3f));
                    r_line(x, y1 + 0.12f, z, mxx, myy, mzz, 1.4f, ac, ac);
                    r_line(mxx, myy, mzz, ex, ey, ez, 1.4f, ac, ac & 0xFFFFFF00);
                }
            }
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
    float wob = t->hurt_t > 0 ? sinf(T * 70.0f) * 0.05f * t->hurt_t : 0.0f;     /* shudders when smashed */
    r_scale(x + wob, tl->h, z, TOWER_K);
    r_light_model(x, tl->h + 0.5f, z);
    if (t->hurt_t > 0.2f) r_unlit = 1;                                                /* white-hot flash as the blow lands */
    if (t->stun_t > 0 && fmodf(T * 12.0f, 1.0f) < 0.3f) r_unlit = 1;                  /* jammed: flickers */
    tw_charge = t->type == TW_TESLA && t->target >= 0 ? clampf(1.0f - t->cd * TOWERS[t->type].rate[t->lvl], 0, 1) : 0.0f;
    if (t->type == TW_PRISM) tw_charge = t->target >= 0 ? clampf(t->lock_t / 2.5f, 0, 1) : 0.0f;
    r_glow_k = 0.5f;
    tower_model(t->type, t->lvl, x + wob, tl->h, z, t->ang, t->anim, t->recoil, t->pulse, t->built, 1);
    r_glow_k = 1.0f;
    tw_charge = 0;
    r_unlit = 0;
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

/* flat triangles / quads straight to the screen (sails, fins, flags): no culling, shaded at one point */
static void wtri(float ax, float ay, float az, float bx, float by, float bz, float cx, float cy, float cz, uint32_t c)
{
    float s0x, s0y, s1x, s1y, s2x, s2y;
    #define WK(x, y, z) x = wk_x + (x - wk_x) * wk_s; y *= wk_s; z = wk_z + (z - wk_z) * wk_s
    WK(ax, ay, az); WK(bx, by, bz); WK(cx, cy, cz);
    #undef WK
    proj(ax, ay, az, &s0x, &s0y); proj(bx, by, bz, &s1x, &s1y); proj(cx, cy, cz, &s2x, &s2y);
    gfx_tri(s0x, s0y, c, s1x, s1y, c, s2x, s2y, c);
}

static void draw_enemy_model(const Enemy *e);
static void draw_enemy(const Enemy *e)
{
    r_scale(e->x, 0.0f, e->z, ENEMY_K);
    r_light_model(e->x, e->y + 0.4f, e->z);
    wk_x = e->x; wk_z = e->z; wk_s = ENEMY_K;
    draw_enemy_model(e);
    wk_s = 1.0f;
    r_light_off();
    if (e->slow_t > 0 && !e->fly) {
        /* frosted over: little ice glints circling the feet */
        gfx_blend(BL_ADD);
        float sz = ENEMIES[e->type].size;
        for (int k = 0; k < 3; k++) {
            float a = T * 2.5f + (float)k * 2.09f + e->x;
            r_glow_cheap(e->x + cosf(a) * sz, 0.05f + 0.15f * fabsf(sinf(T * 3.0f + (float)k)), e->z + sinf(a) * sz, 0.05f, RGBA(200, 245, 255, 200));
        }
        r_gdisc(e->x, e->z, 0.01f, sz * 1.1f, 16, RGBA(170, 230, 255, 50), 0xA0E0FF00);
        gfx_blend(BL_ALPHA);
    }
    if (e->shield_t > 0 || e->heal_fx > 0) {
        /* inside a brass moth's shield: a shimmering bubble; just repaired: a green flash */
        gfx_blend(BL_ADD);
        float sz = ENEMIES[e->type].size;
        if (e->shield_t > 0) r_glow(e->x, e->y + sz * 0.9f, e->z, sz * 1.6f, RGBA(110, 190, 255, (int)(55 + 25 * sinf(T * 8.0f + e->x))));
        if (e->heal_fx > 0) r_glow(e->x, e->y + sz * 0.9f, e->z, sz * 1.8f, RGBA(100, 255, 130, (int)(200 * e->heal_fx)));
        gfx_blend(BL_ALPHA);
    }
    r_scale_off();
}

static void draw_enemy_model(const Enemy *e)
{
    const EnemyDef *d = &ENEMIES[e->type];
    float s = d->size, x = e->x, z = e->z, a = e->ang;
    float ca = cosf(a), sa = sinf(a);
    if (!on_screen(x, 0.4f, z, 60.0f)) return;
    if (G->lv->env == ENV_PIRATE && !e->fly) {
        /* afloat: a dark patch in the water and a ring of foam round the waterline */
        r_gdisc(x, z, 0.006f, s * 1.05f, 10, 0x0A182860, 0x0A182800);
        if (!((e->type == EN_SHARK || e->type == EN_KRAKEN) && e->dash_t > 0)) {
            gfx_blend(BL_ADD);
            float wv = 0.5f + 0.5f * sinf(T * 3.0f + x * 2.0f);
            r_gring(x + ca * s * 0.15f, z + sa * s * 0.15f, 0.01f, s * (0.85f + 0.12f * wv), 0.03f + 0.02f * wv, 10, RGBA(200, 220, 240, 70));
            gfx_blend(BL_ALPHA);
        }
    } else if (!e->fly) r_gdisc(x, z, 0.006f, s * 0.95f, 16, 0x00000078, 0x00000000);
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

    case EN_BEETLE: {
        /* armoured beetle: a low teal dome with a horn; flashes silver when hits bounce off */
        float bob = fabsf(sinf(e->anim * 2.2f)) * 0.02f;
        for (int k = 0; k < 3; k++) {
            float o = ((float)k - 1.0f) * 0.14f, sw = sinf(e->anim * 2.2f + (float)k * 2.1f) * 0.05f;
            r_beam(x + ca * (o + sw) - sa * 0.12f, 0.04f, z + sa * (o + sw) + ca * 0.12f, -sa, 0.0f, ca, 0.14f, 0.05f, 0.05f, 0x1C2A28FF);
            r_beam(x + ca * (o - sw) + sa * 0.12f, 0.04f, z + sa * (o - sw) - ca * 0.12f, sa, 0.0f, -ca, 0.14f, 0.05f, 0.05f, 0x1C2A28FF);
        }
        uint32_t shell = en_tint(e, 0x2E8C7CFF);
        if (e->armor_t > 0) shell = col_mix(shell, 0xE8F0FFFF, 0.6f);
        r_obox(x + ca * 0.24f, 0.06f + bob, z + sa * 0.24f, 0.09f, 0.1f, 0.2f + bob, a, 0x203030FF, 0x182424FF);
        r_beam(x + ca * 0.3f, 0.16f + bob, z + sa * 0.3f, ca, 0.9f, sa, 0.2f, 0.05f, 0.05f, 0xD8C8A0FF);
        r_prism(x - ca * 0.03f, 0.07f + bob, z - sa * 0.03f, 0.27f, 0.12f, 0.33f + bob, 8, a, col_mix(shell, 0xFFFFFFFF, 0.15f), shell);
        r_obox(x - ca * 0.03f, 0.33f + bob, z - sa * 0.03f, 0.14f, 0.012f, 0.345f + bob, a, 0x184840FF, 0x184840FF);
        gfx_blend(BL_ADD);
        eyes(x, 0.17f + bob, z, a, 0.33f, 0.05f, 0.05f, 0xFFE060FF);
        gfx_blend(BL_ALPHA);
        break;
    }
    case EN_SLIME: case EN_SLIMELET: {
        /* bouncy jelly: squashes on landing, stretches in the air */
        float k = e->type == EN_SLIME ? 1.0f : 0.62f;
        float ph = fmodf(e->anim * 0.55f, 1.0f), hop = sinf(ph * 3.14159f);
        float squash = ph < 0.15f ? 1.0f - (0.15f - ph) * 1.6f : 1.0f + hop * 0.12f;
        float y0 = hop * 0.12f * k;
        uint32_t body = en_tint(e, e->type == EN_SLIME ? 0x88D848E8 : 0xA8E860E8);
        r_crystal(x, y0 + 0.12f * k * squash, z, 0.3f * k / sqrtf(squash), 0.26f * k * squash, 0.12f * k * squash, 8, a, body);
        r_crystal(x, y0 + 0.14f * k * squash, z, 0.14f * k, 0.12f * k, 0.06f * k, 6, a + 0.4f, col_mix(body, 0x305818FF, 0.5f));
        gfx_blend(BL_ADD);
        eyes(x, y0 + 0.22f * k * squash, z, a, 0.22f * k, 0.08f * k, 0.06f * k, 0xFFFFFFFF);
        gfx_blend(BL_ALPHA);
        break;
    }
    case EN_FOX: {
        /* fox: trots, then sprints (dust and speed streaks) */
        int dash = e->dash_t > 0;
        float run = sinf(e->anim * (dash ? 3.5f : 2.4f));
        float y0 = 0.12f + fabsf(run) * 0.03f;
        uint32_t fur = en_tint(e, 0xE8702CFF), dark = scale_rgb(fur, 0.7f);
        for (int k = 0; k < 4; k++) {
            float fx = (k < 2 ? 0.12f : -0.12f) + run * 0.05f * ((k & 1) ? 1.0f : -1.0f), fz = (k & 1) ? 0.07f : -0.07f;
            r_obox(x + ca * fx - sa * fz, 0, z + sa * fx + ca * fz, 0.03f, 0.03f, y0 + 0.02f, a, 0x302018FF, 0x281810FF);
        }
        r_obox(x, y0, z, 0.2f, 0.09f, y0 + 0.16f, a, fur, dark);
        float hx = x + ca * 0.22f, hz = z + sa * 0.22f;
        r_obox(hx, y0 + 0.06f, hz, 0.08f, 0.08f, y0 + 0.24f, a, fur, dark);
        r_beam(hx + ca * 0.05f, y0 + 0.11f, hz + sa * 0.05f, ca, 0, sa, 0.1f, 0.06f, 0.06f, 0xF0E8D8FF);
        for (int k = -1; k <= 1; k += 2)
            r_prism(hx - sa * 0.05f * (float)k, y0 + 0.24f, hz + ca * 0.05f * (float)k, 0.04f, 0.0f, y0 + 0.36f, 4, a, fur, dark);
        float wag = sinf(T * 8.0f) * 0.3f;
        r_beam(x - ca * 0.18f, y0 + 0.1f, z - sa * 0.18f, -ca + sa * wag, 0.45f, -sa - ca * wag, 0.22f, 0.09f, 0.08f, fur);
        r_beam(x - ca * 0.37f + sa * wag * 0.2f, y0 + 0.2f, z - sa * 0.37f - ca * wag * 0.2f, -ca, 0.3f, -sa, 0.07f, 0.08f, 0.07f, 0xF8F0E8FF);
        gfx_blend(BL_ADD);
        eyes(hx, y0 + 0.18f, hz, a, 0.07f, 0.04f, 0.035f, 0xFFF080FF);
        if (dash)
            for (int k = -1; k <= 1; k++)
                r_line(x - ca * 0.3f - sa * 0.08f * (float)k, y0 + 0.1f, z - sa * 0.3f + ca * 0.08f * (float)k,
                       x - ca * 0.75f - sa * 0.08f * (float)k, y0 + 0.1f, z - sa * 0.75f + ca * 0.08f * (float)k, 2.0f, 0xFFE0B090, 0xFFE0B000);
        gfx_blend(BL_ALPHA);
        break;
    }
    case EN_CROW: {
        float y = e->y;
        r_gdisc(x, z, 0.006f, 0.28f, 16, 0x00000050, 0x00000000);
        uint32_t body = en_tint(e, 0x3A3044FF);
        r_obox(x, y - 0.06f, z, 0.15f, 0.07f, y + 0.06f, a, col_mix(body, 0x8070A0FF, 0.2f), body);
        r_obox(x + ca * 0.16f, y - 0.03f, z + sa * 0.16f, 0.06f, 0.055f, y + 0.08f, a, body, body);
        r_beam(x + ca * 0.21f, y + 0.02f, z + sa * 0.21f, ca, -0.2f, sa, 0.09f, 0.035f, 0.03f, 0xF0B030FF);
        r_beam(x - ca * 0.14f, y, z - sa * 0.14f, -ca, 0.1f, -sa, 0.13f, 0.11f, 0.02f, body);
        /* flapping wings */
        float flap = sinf(e->anim * 3.2f) * 0.8f;
        for (int k = -1; k <= 1; k += 2) {
            float px = -sa * (float)k, pz = ca * (float)k;
            float w0x = x + ca * 0.08f, w0z = z + sa * 0.08f, w1x = x - ca * 0.1f, w1z = z - sa * 0.1f;
            float tx = x + px * 0.42f * cosf(flap) - ca * 0.06f, ty = y + 0.42f * sinf(flap) * 0.7f, tz = z + pz * 0.42f * cosf(flap) - sa * 0.06f;
            float s0x, s0y, s1x, s1y, s2x, s2y;
            proj(w0x, y + 0.03f, w0z, &s0x, &s0y); proj(w1x, y + 0.03f, w1z, &s1x, &s1y); proj(tx, ty, tz, &s2x, &s2y);
            uint32_t wc = lt_shade(body, x, y, z, 0.75f + 0.25f * sinf(flap));
            gfx_tri(s0x, s0y, wc, s1x, s1y, wc, s2x, s2y, wc);
        }
        gfx_blend(BL_ADD);
        eyes(x + ca * 0.16f, y + 0.04f, z + sa * 0.16f, a, 0.04f, 0.04f, 0.035f, 0xFF4030FF);
        gfx_blend(BL_ALPHA);
        break;
    }
    case EN_ZOMBIE: case EN_GHOULKING: {
        /* shambling dead; the king is a giant with a crown and cape. Arms come up, then slam down on towers */
        int king = e->type == EN_GHOULKING;
        float k = king ? 2.2f : 1.0f;
        float step = sinf(e->anim * 1.4f), lean = sinf(e->anim * 0.7f) * 0.04f * k;
        float raise = e->act_t > 0 ? (e->act_t > 0.22f ? (0.6f - e->act_t) / 0.38f : e->act_t / 0.22f) : 0.0f;
        float px = -sa, pz = ca;                                     /* sideways */
        uint32_t skin = en_tint(e, king ? 0x7A8A5AFF : 0x7AA060FF), cloth = en_tint(e, king ? 0x4A2A5AFF : 0x4A5A78FF);
        float bx = x + px * lean, bz = z + pz * lean;
        for (int s2 = -1; s2 <= 1; s2 += 2) {                        /* stiff legs */
            float f = step * 0.07f * (float)s2 * k;
            r_obox(x + ca * f + px * 0.08f * k * (float)s2, 0, z + sa * f + pz * 0.08f * k * (float)s2, 0.045f * k, 0.045f * k, 0.2f * k, a, 0x3A3A44FF, 0x2E2E36FF);
        }
        r_obox(bx, 0.18f * k, bz, 0.13f * k, 0.09f * k, 0.46f * k, a + 0.1f * step, cloth, scale_rgb(cloth, 0.8f));
        if (king) {
            r_beam(bx - ca * 0.12f * k, 0.44f * k, bz - sa * 0.12f * k, -ca, -1.2f, -sa, 0.42f * k, 0.3f * k, 0.03f * k, en_tint(e, 0x5A2A6AFF));   /* cape */
        }
        float hx = bx + ca * 0.03f * k, hz = bz + sa * 0.03f * k, hy = 0.46f * k;
        r_obox(hx, hy, hz, 0.08f * k, 0.08f * k, hy + 0.15f * k, a + lean * 4.0f, skin, scale_rgb(skin, 0.8f));
        if (king)
            for (int i = 0; i < 5; i++) {
                float ang = a + (float)i * 1.2566f;
                r_prism(hx + cosf(ang) * 0.07f * k, hy + 0.15f * k, hz + sinf(ang) * 0.07f * k, 0.03f * k, 0.0f, hy + 0.24f * k, 4, ang, 0xFFD040FF, 0xD8A020FF);
            }
        for (int s2 = -1; s2 <= 1; s2 += 2) {                        /* arms reaching forward, or raised for a slam */
            float sx = bx + px * 0.15f * k * (float)s2, sz = bz + pz * 0.15f * k * (float)s2, sy = 0.42f * k;
            float up = -0.05f + raise * 2.2f + sinf(e->anim * 1.4f + (float)s2) * 0.06f;
            r_beam(sx, sy, sz, ca, up, sa, 0.26f * k, 0.05f * k, 0.05f * k, skin);
        }
        gfx_blend(BL_ADD);
        eyes(hx, hy + 0.1f * k, hz, a, 0.08f * k, 0.035f * k, 0.03f * k, king ? 0x80FF60FF : 0xE8F060FF);
        if (king) r_glow(hx, hy, hz, 0.7f, RGBA(80, 255, 90, 50));
        gfx_blend(BL_ALPHA);
        break;
    }
    case EN_GHOST: {
        /* a floating sheet; fades almost away while it can't be hit */
        float y = e->y, ph = e->dash_t > 0 ? 1.0f : 0.0f;
        float fade = ph > 0 ? 0.22f + 0.08f * sinf(T * 20.0f) : 0.82f;
        float sway = sinf(e->anim * 0.8f) * 0.04f;
        r_gdisc(x, z, 0.006f, 0.26f, 16, RGBA(0, 0, 0, (int)(70 * fade)), 0x00000000);
        r_alpha = fade;
        r_unlit = 1;                                                    /* ghosts glow on their own */
        uint32_t sheet = en_tint(e, 0xD8E6F2FF);
        r_prism(x + sway, y - 0.32f, z, 0.24f, 0.13f, y + 0.08f, 9, a, sheet, scale_rgb(sheet, 0.8f));
        r_crystal(x + sway * 1.5f, y + 0.14f, z, 0.14f, 0.12f, 0.08f, 8, a, sheet);
        for (int i = 0; i < 5; i++) {                                    /* ragged hem */
            float ang = a + (float)i * 1.2566f + e->anim * 0.5f;
            r_crystal(x + cosf(ang) * 0.2f, y - 0.32f, z + sinf(ang) * 0.2f, 0.06f, 0.02f, 0.1f + 0.03f * sinf(T * 4.0f + (float)i), 4, ang, sheet);
        }
        for (int s2 = -1; s2 <= 1; s2 += 2)                             /* little floating arms */
            r_beam(x - sa * 0.2f * (float)s2, y - 0.02f, z + ca * 0.2f * (float)s2, ca - sa * 0.6f * (float)s2, 0.25f + 0.2f * sinf(T * 3.0f + (float)s2), sa + ca * 0.6f * (float)s2, 0.16f, 0.06f, 0.05f, sheet);
        for (int s2 = -1; s2 <= 1; s2 += 2)                             /* hollow black eyes */
            r_crystal(x + sway * 1.5f + ca * 0.12f - sa * 0.05f * (float)s2, y + 0.17f, z + sa * 0.12f + ca * 0.05f * (float)s2, 0.03f, 0.035f, 0.035f, 4, a, 0x101018FF);
        r_unlit = 0;
        r_alpha = 1.0f;
        gfx_blend(BL_ADD);
        r_glow(x, y, z, 0.5f, RGBA(150, 190, 255, (int)(60 * fade)));
        gfx_blend(BL_ALPHA);
        break;
    }
    case EN_SKELETON: {
        uint32_t bone = en_tint(e, 0xE8E0CCFF), bone2 = scale_rgb(bone, 0.82f);
        if (e->down_t > 0) {
            /* a heap of bones, rattling as it pulls itself back together */
            float jit = e->down_t < 0.7f ? sinf(T * 40.0f) * 0.02f : 0.0f;
            for (int i = 0; i < 5; i++) {
                float ang = a + (float)i * 1.3f;
                r_beam(x + cosf(ang) * 0.08f + jit, 0.02f, z + sinf(ang) * 0.08f, cosf(ang + 1.0f), 0.05f, sinf(ang + 1.0f), 0.18f, 0.035f, 0.035f, bone);
            }
            r_crystal(x + ca * 0.1f, 0.06f + jit, z + sa * 0.1f, 0.07f, 0.06f, 0.05f, 6, a, bone);
            break;
        }
        float step = sinf(e->anim * 2.2f), px = -sa, pz = ca;
        for (int s2 = -1; s2 <= 1; s2 += 2) {
            float f = step * 0.08f * (float)s2;
            r_beam(x + ca * f + px * 0.05f * (float)s2, 0.0f, z + sa * f + pz * 0.05f * (float)s2, ca * f * 0.5f, 1.0f, sa * f * 0.5f, 0.2f, 0.03f, 0.03f, bone2);
        }
        r_obox(x, 0.19f, z, 0.07f, 0.05f, 0.23f, a, bone, bone2);                       /* pelvis */
        r_beam(x, 0.22f, z, 0, 1, 0, 0.2f, 0.025f, 0.025f, bone2);                   /* spine */
        for (int i = 0; i < 3; i++)                                                     /* ribs */
            r_obox(x, 0.29f + (float)i * 0.045f, z, 0.08f - 0.01f * (float)i, 0.06f, 0.31f + (float)i * 0.045f, a, bone, bone2);
        for (int s2 = -1; s2 <= 1; s2 += 2) {
            float sw = step * 0.4f * (float)s2;
            r_beam(x + px * 0.1f * (float)s2, 0.4f, z + pz * 0.1f * (float)s2, ca * sw, -1.0f, sa * sw, 0.2f, 0.025f, 0.025f, bone2);
        }
        r_crystal(x + ca * 0.02f, 0.5f, z + sa * 0.02f, 0.075f, 0.08f, 0.04f, 6, a, bone);   /* skull */
        r_obox(x + ca * 0.05f, 0.44f, z + sa * 0.05f, 0.04f, 0.045f, 0.47f, a, bone2, bone2);  /* jaw */
        gfx_blend(BL_ADD);
        eyes(x, 0.51f, z, a, 0.07f, 0.03f, 0.03f, 0xFF3020FF);
        gfx_blend(BL_ALPHA);
        break;
    }
    case EN_BAT: {
        float y = e->y;
        r_gdisc(x, z, 0.006f, 0.2f, 16, 0x00000040, 0x00000000);
        uint32_t body = en_tint(e, 0x2A2030FF);
        r_crystal(x, y, z, 0.07f, 0.07f, 0.06f, 5, a, body);
        for (int s2 = -1; s2 <= 1; s2 += 2)
            r_prism(x + ca * 0.03f - sa * 0.035f * (float)s2, y + 0.04f, z + sa * 0.03f + ca * 0.035f * (float)s2, 0.02f, 0.0f, y + 0.11f, 3, a, body, body);
        float flap = sinf(e->anim * 6.5f) * 0.9f;
        for (int s2 = -1; s2 <= 1; s2 += 2) {
            float px = -sa * (float)s2, pz = ca * (float)s2;
            float tx = x + px * 0.28f * cosf(flap), ty = y + 0.28f * sinf(flap) * 0.7f, tz = z + pz * 0.28f * cosf(flap);
            float mx = x + px * 0.16f * cosf(flap) - ca * 0.1f, my = y + 0.12f * sinf(flap), mz = z + pz * 0.16f * cosf(flap) - sa * 0.1f;
            float s0x, s0y, s1x, s1y, s2x, s2y, s3x, s3y;
            proj(x + ca * 0.04f, y, z + sa * 0.04f, &s0x, &s0y); proj(tx, ty, tz, &s1x, &s1y);
            proj(mx, my, mz, &s2x, &s2y); proj(x - ca * 0.05f, y, z - sa * 0.05f, &s3x, &s3y);
            uint32_t wc = lt_shade(0x3A2440FF, x, y, z, 0.8f + 0.2f * sinf(flap));
            gfx_tri(s0x, s0y, wc, s1x, s1y, wc, s2x, s2y, wc);
            gfx_tri(s0x, s0y, wc, s2x, s2y, wc, s3x, s3y, wc);
        }
        gfx_blend(BL_ADD);
        eyes(x, y + 0.02f, z, a, 0.06f, 0.025f, 0.025f, 0xFF3030FF);
        gfx_blend(BL_ALPHA);
        break;
    }
    case EN_BREAKER: {
        /* a hulking grave-robber swinging a tombstone hammer */
        float step = sinf(e->anim * 1.3f), px = -sa, pz = ca;
        float raise = e->act_t > 0 ? (e->act_t > 0.22f ? (0.6f - e->act_t) / 0.38f : e->act_t / 0.22f) : 0.0f;
        uint32_t skin = en_tint(e, 0x5C6A70FF), dark = scale_rgb(skin, 0.75f);
        if (e->armor_t > 0) skin = col_mix(skin, 0xE8F0FFFF, 0.5f);
        for (int s2 = -1; s2 <= 1; s2 += 2) {
            float f = step * 0.1f * (float)s2;
            r_obox(x + ca * f + px * 0.16f * (float)s2, 0, z + sa * f + pz * 0.16f * (float)s2, 0.09f, 0.09f, 0.22f, a, 0x30343AFF, 0x262A30FF);
        }
        r_obox(x, 0.2f, z, 0.24f, 0.2f, 0.62f, a, skin, dark);
        r_obox(x + ca * 0.14f, 0.5f, z + sa * 0.14f, 0.09f, 0.09f, 0.68f, a, skin, dark);           /* low head */
        r_beam(x - px * 0.27f, 0.5f, z - pz * 0.27f, ca, -0.4f, sa, 0.22f, 0.08f, 0.08f, skin);      /* free arm */
        /* hammer: handle from the right hand, a gravestone on the end */
        float hx = x + px * 0.28f, hz = z + pz * 0.28f, hy = 0.5f;
        float up = 0.3f + raise * 3.0f;
        float dl = sqrtf(1.0f + up * up), dx = ca / dl, dy = up / dl, dz = sa / dl;
        r_beam(hx, hy, hz, dx, dy, dz, 0.42f, 0.04f, 0.04f, 0x4A3A2AFF);
        float tx = hx + dx * 0.42f, ty = hy + dy * 0.42f, tz = hz + dz * 0.42f;
        r_obox(tx, ty - 0.12f, tz, 0.07f, 0.13f, ty + 0.12f, a, 0x9A9AA2FF, 0x7A7A82FF);
        gfx_blend(BL_ADD);
        eyes(x + ca * 0.14f, 0.62f, z + sa * 0.14f, a, 0.09f, 0.04f, 0.035f, 0x80FF80FF);
        gfx_blend(BL_ALPHA);
        break;
    }
    case EN_AUTOMATON: {
        /* a brass clockwork soldier with one glowing eye and a wind-up key turning in its back */
        float step = sinf(e->anim * 1.8f), px = -sa, pz = ca;
        uint32_t brass = en_tint(e, 0xC89A50FF), dark = scale_rgb(brass, 0.72f);
        if (e->armor_t > 0) brass = col_mix(brass, 0xFFF4D0FF, 0.5f);
        for (int s2 = -1; s2 <= 1; s2 += 2) {
            float f = step * 0.07f * (float)s2;
            r_prism(x + ca * f + px * 0.08f * (float)s2, 0, z + sa * f + pz * 0.08f * (float)s2, 0.04f, 0.04f, 0.2f, 4, a, 0x5A5048FF, 0x4A4038FF);
        }
        r_prism(x, 0.18f, z, 0.13f, 0.15f, 0.44f, 8, a, brass, dark);
        r_prism(x, 0.44f, z, 0.1f, 0.03f, 0.58f, 8, a, col_mix(brass, 0xFFFFFFFF, 0.15f), brass);
        for (int s2 = -1; s2 <= 1; s2 += 2)
            r_beam(x + px * 0.16f * (float)s2, 0.38f, z + pz * 0.16f * (float)s2, ca * step * 0.3f * (float)s2, -1.0f, sa * step * 0.3f * (float)s2, 0.18f, 0.04f, 0.04f, dark);
        /* wind-up key */
        float kx = x - ca * 0.17f, kz = z - sa * 0.17f, ka = T * 4.0f;
        r_beam(kx, 0.36f, kz, -ca, 0, -sa, 0.06f, 0.02f, 0.02f, 0x8A7040FF);
        r_obox(kx - ca * 0.07f, 0.31f + 0.0f * ka, kz - sa * 0.07f, 0.015f, 0.07f * fabsf(cosf(ka)) + 0.01f, 0.41f, a, 0xD8B060FF, 0xB89040FF);
        gfx_blend(BL_ADD);
        r_glow_cheap(x + ca * 0.1f, 0.5f, z + sa * 0.1f, 0.08f, 0x60F0FFFF);
        gfx_blend(BL_ALPHA);
        break;
    }
    case EN_SPRINGER: {
        /* a squat copper frog on coiled springs: they squash as it crouches, stretch as it leaps */
        float y0 = e->y, crouch = e->dash_t < 0 ? clampf(1.0f + e->dash_t * 2.0f, 0, 1) : 0.0f;
        float coil = 0.16f * (1.0f - 0.55f * crouch) + (e->dash_t > 0 ? 0.06f : 0.0f);
        uint32_t body = en_tint(e, 0x6AA0A0FF);
        for (int s2 = -1; s2 <= 1; s2 += 2) {
            float lx = x - sa * 0.11f * (float)s2 - ca * 0.05f, lz = z + ca * 0.11f * (float)s2 - sa * 0.05f;
            for (int k = 0; k < 4; k++) {
                float yy = y0 + coil * (float)k / 4.0f;
                r_prism(lx, yy, lz, 0.05f, 0.05f, yy + 0.02f, 6, 0, 0xB8A070FF, 0x988050FF);
            }
        }
        float by = y0 + coil;
        r_crystal(x, by + 0.08f, z, 0.17f, 0.12f, 0.07f, 7, a, body);
        r_crystal(x + ca * 0.13f, by + 0.13f, z + sa * 0.13f, 0.08f, 0.06f, 0.04f, 6, a, col_mix(body, 0xFFFFFFFF, 0.15f));
        gfx_blend(BL_ADD);
        eyes(x, by + 0.17f, z, a, 0.15f, 0.05f, 0.035f, 0xFFE070FF);
        gfx_blend(BL_ALPHA);
        break;
    }
    case EN_MOTH: {
        /* a brass moth with glassy wings; its shield bubble covers whatever walks under it */
        float y = e->y;
        r_gdisc(x, z, 0.006f, 0.26f, 16, 0x00000040, 0x00000000);
        uint32_t body = en_tint(e, 0xD8B060FF);
        r_crystal(x, y, z, 0.07f, 0.06f, 0.06f, 6, a, body);
        r_beam(x - ca * 0.12f, y, z - sa * 0.12f, ca, 0.05f, sa, 0.24f, 0.07f, 0.07f, scale_rgb(body, 0.8f));
        float flap = sinf(e->anim * 4.5f) * 0.7f;
        gfx_blend(BL_ADD);
        for (int s2 = -1; s2 <= 1; s2 += 2) {
            float px = -sa * (float)s2, pz = ca * (float)s2;
            for (int w = 0; w < 2; w++) {
                float off = w ? -0.08f : 0.05f, L = w ? 0.22f : 0.3f;
                float tx = x + px * L * cosf(flap) + ca * off, ty = y + L * sinf(flap) * 0.6f, tz = z + pz * L * cosf(flap) + sa * off;
                float s0x, s0y, s1x, s1y, s2x, s2y;
                proj(x + ca * (off + 0.06f), y, z + sa * (off + 0.06f), &s0x, &s0y);
                proj(x + ca * (off - 0.06f), y, z + sa * (off - 0.06f), &s1x, &s1y);
                proj(tx, ty, tz, &s2x, &s2y);
                gfx_tri(s0x, s0y, 0xA0E0FF70, s1x, s1y, 0xA0E0FF70, s2x, s2y, 0xFFE8B040);
            }
        }
        r_glow(x, y, z, 0.35f, 0x80D0FF50);
        r_gring(x, z, 0.02f, 1.7f, 0.02f, 32, RGBA(120, 200, 255, (int)(16 + 8 * sinf(T * 3.0f))));
        gfx_blend(BL_ALPHA);
        break;
    }
    case EN_TINKER: {
        /* a little hooded repair-bot with a spanner and a glowing green antenna */
        float step = sinf(e->anim * 2.0f), px = -sa, pz = ca;
        uint32_t robe = en_tint(e, 0x8A6A9AFF);
        r_prism(x, 0, z, 0.15f, 0.08f, 0.36f, 7, a, robe, scale_rgb(robe, 0.78f));
        r_obox(x - ca * 0.1f, 0.15f, z - sa * 0.1f, 0.06f, 0.09f, 0.32f, a, 0x7A6A50FF, 0x5A4A38FF);   /* tool pack */
        r_crystal(x + ca * 0.02f, 0.42f, z + sa * 0.02f, 0.08f, 0.07f, 0.05f, 6, a, 0xB8B0A0FF);
        float sw = sinf(T * 6.0f) * 0.5f;
        r_beam(x + px * 0.13f, 0.28f, z + pz * 0.13f, ca, 0.4f + sw, sa, 0.18f, 0.03f, 0.03f, 0x9A9AA0FF);    /* spanner */
        r_beam(x, 0.48f, z, 0, 1, 0, 0.12f, 0.015f, 0.015f, 0x606060FF);
        gfx_blend(BL_ADD);
        float pulse = clampf(1.0f - e->heal_t / 2.4f, 0, 1);
        r_glow_cheap(x, 0.62f, z, 0.07f + 0.08f * pulse, RGBA(110, 255, 140, (int)(140 + 100 * pulse)));
        eyes(x, 0.43f, z, a, 0.07f, 0.03f, 0.025f, 0x90FFB0FF);
        gfx_blend(BL_ALPHA);
        (void)step;
        break;
    }
    case EN_SENTINEL: case EN_ARCHITECT: {
        /* stone-and-brass guardians; the Architect is a towering titan with a turning gear in its chest */
        int boss = e->type == EN_ARCHITECT;
        float k = boss ? 1.75f : 1.0f;
        float step = sinf(e->anim * 1.2f), px = -sa, pz = ca;
        float raise = e->act_t > 0 ? (e->act_t > 0.22f ? (0.6f - e->act_t) / 0.38f : e->act_t / 0.22f) : 0.0f;
        uint32_t stone = en_tint(e, boss ? 0xB08848FF : 0x8A9088FF), dark = scale_rgb(stone, 0.75f);
        if (e->armor_t > 0) stone = col_mix(stone, 0xE8F0FFFF, 0.5f);
        for (int s2 = -1; s2 <= 1; s2 += 2) {
            float f = step * 0.12f * (float)s2 * k;
            r_obox(x + ca * f + px * 0.17f * k * (float)s2, 0, z + sa * f + pz * 0.17f * k * (float)s2, 0.1f * k, 0.1f * k, 0.25f * k, a, 0x50504AFF, 0x40403AFF);
        }
        r_obox(x, 0.23f * k, z, 0.26f * k, 0.2f * k, 0.62f * k, a, stone, dark);
        r_obox(x, 0.4f * k, z, 0.27f * k, 0.21f * k, 0.46f * k, a, 0xC09448FF, 0xA07830FF);          /* brass band */
        r_obox(x + ca * 0.03f * k, 0.62f * k, z + sa * 0.03f * k, 0.13f * k, 0.12f * k, 0.8f * k, a, stone, dark);
        for (int s2 = -1; s2 <= 1; s2 += 2) {
            float ax = x + px * 0.32f * k * (float)s2, az = z + pz * 0.32f * k * (float)s2;
            r_beam(ax, 0.55f * k, az, ca, -0.6f + raise * 2.6f, sa, 0.32f * k, 0.12f * k, 0.12f * k, stone);
        }
        if (boss) {                                                      /* a big gear turning in its chest */
            float gx = x + ca * 0.27f * k, gz = z + sa * 0.27f * k, gy = 0.44f * k;
            float sx[25], sy[25], cxs, cys;
            proj(gx, gy, gz, &cxs, &cys);
            for (int i = 0; i <= 24; i++) {
                float an = T * 1.5f + (float)i / 24.0f * 6.2832f, rr = (i & 1) ? 0.13f * k : 0.17f * k;
                proj(gx - sa * cosf(an) * rr, gy + sinf(an) * rr, gz + ca * cosf(an) * rr, &sx[i], &sy[i]);
            }
            uint32_t gc = lt_shade(0xE0B050FF, gx, gy, gz, 1.0f);
            if (sa > -0.3f) for (int i = 0; i < 24; i++) gfx_tri(cxs, cys, gc, sx[i], sy[i], gc, sx[i + 1], sy[i + 1], gc);
            for (int i = 0; i < 4; i++) {                                 /* little brass spires on its crown */
                float an = a + (float)i * 1.5708f + 0.785f;
                r_prism(x + cosf(an) * 0.1f * k, 0.8f * k, z + sinf(an) * 0.1f * k, 0.03f * k, 0.0f, 0.95f * k, 4, an, 0xE8C060FF, 0xC09838FF);
            }
        }
        gfx_blend(BL_ADD);
        float charge = boss ? clampf(1.0f - e->pulse_t / 2.0f, 0, 1) : 0.0f;   /* glows hotter just before its shockwave */
        eyes(x + ca * 0.03f * k, 0.73f * k, z + sa * 0.03f * k, a, 0.13f * k, 0.05f * k, 0.04f * k, boss ? 0x90E0FFFF : 0x70C8FFFF);
        if (boss) r_glow(x + ca * 0.28f * k, 0.44f * k, z + sa * 0.28f * k, 0.4f + 0.6f * charge, RGBA(120, 210, 255, (int)(70 + 150 * charge)));
        gfx_blend(BL_ALPHA);
        break;
    }
    case EN_PUMPKIN: case EN_GOURD: {
        int king = e->type == EN_PUMPKIN;
        float k = king ? 1.0f : 0.32f;
        float hop = fabsf(sinf(e->anim * (king ? 1.1f : 2.2f))) * 0.08f * k;
        uint32_t skin = en_tint(e, king ? 0xF08020FF : 0xF8B030FF);
        if (e->armor_t > 0) skin = col_mix(skin, 0xFFF0D0FF, 0.5f);
        float rot = a + 0.31f;
        r_prism(x, hop, z, 0.5f * k, 0.62f * k, 0.25f * k + hop, 10, rot, skin, scale_rgb(skin, 0.86f));
        r_prism(x, 0.25f * k + hop, z, 0.62f * k, 0.62f * k, 0.5f * k + hop, 10, rot, skin, scale_rgb(skin, 0.92f));
        r_prism(x, 0.5f * k + hop, z, 0.62f * k, 0.32f * k, 0.72f * k + hop, 10, rot, col_mix(skin, 0xFFE0A0FF, 0.2f), skin);
        r_prism(x, 0.72f * k + hop, z, 0.08f * k, 0.06f * k, 0.92f * k + hop, 5, 0, 0x5A6A2AFF, 0x4A5A20FF);
        if (king) {
            for (int i = 0; i < 5; i++) {        /* a crown of gold points */
                float ang = T * 0.6f + (float)i * 1.2566f;
                r_prism(x + cosf(ang) * 0.22f, 0.7f + hop, z + sinf(ang) * 0.22f, 0.06f, 0.0f, 0.95f + hop, 4, ang, 0xFFD040FF, 0xD8A020FF);
            }
            for (int s = -1; s <= 1; s += 2) {    /* leafy vine arms */
                float sw = sinf(e->anim * 1.1f) * 0.3f * (float)s;
                r_beam(x - sa * 0.58f * (float)s, 0.4f + hop, z + ca * 0.58f * (float)s, -sa * (float)s + ca * sw, 0.3f, ca * (float)s + sa * sw, 0.35f, 0.08f, 0.08f, 0x4E7A28FF);
            }
        }
        /* carved glowing face, only when it faces the camera */
        float face = clampf((sa + 0.3f) / 0.6f, 0, 1);
        if (face > 0) {
            float fx = x + ca * 0.62f * k, fz = z + sa * 0.62f * k, ux = -sa, uz = ca;
            uint32_t glow = RGBA(255, 190, 60, (int)(255 * face));
            gfx_blend(BL_ADD);
            float sx[3], sy[3];
            #define FPT(i, u, v) proj(fx + ux * (u) * k, (v) * k + hop, fz + uz * (u) * k, &sx[i], &sy[i])
            for (int s = -1; s <= 1; s += 2) {
                FPT(0, 0.12f * s, 0.52f); FPT(1, 0.3f * s, 0.52f); FPT(2, 0.21f * s, 0.64f);
                gfx_tri(sx[0], sy[0], glow, sx[1], sy[1], glow, sx[2], sy[2], glow);
            }
            for (int i = 0; i < 4; i++) {
                float u0 = -0.3f + 0.15f * (float)i, u1 = u0 + 0.15f;
                FPT(0, u0, 0.36f); FPT(1, u1, 0.36f); FPT(2, (u0 + u1) * 0.5f, (i & 1) ? 0.26f : 0.22f);
                gfx_tri(sx[0], sy[0], glow, sx[1], sy[1], glow, sx[2], sy[2], glow);
            }
            #undef FPT
            r_glow(fx, 0.45f * k + hop, fz, 0.5f * k, RGBA(255, 150, 40, (int)(140 * face)));
            gfx_blend(BL_ALPHA);
        }
        break;
    }
    /* ---------------- level 7: the enchanted kingdom ---------------- */
    case EN_UNICORN: {
        /* a crazy unicorn: white, rainbow mane and tail, a golden horn, galloping (charging when it dashes) */
        float gal = sinf(e->anim * 2.6f), px = -sa, pz = ca, y0 = 0.2f + fabsf(gal) * 0.04f;
        uint32_t body = en_tint(e, 0xF8F4FFFF), body2 = scale_rgb(body, 0.85f);
        static const uint32_t RB[6] = { 0xFF5060FF, 0xFFA040FF, 0xFFF060FF, 0x60F070FF, 0x60A0FFFF, 0xC070FFFF };
        for (int k = 0; k < 4; k++) {                                        /* legs */
            float o = (k & 1) ? 0.12f : -0.12f, sd = (k & 2) ? 0.07f : -0.07f, sw = gal * 0.08f * ((k == 0 || k == 3) ? 1.0f : -1.0f);
            r_beam(x + ca * (o + sw) + px * sd, y0, z + sa * (o + sw) + pz * sd, ca * sw, -1.0f, sa * sw, 0.2f, 0.035f, 0.035f, body2);
        }
        r_obox(x, y0, z, 0.2f, 0.09f, y0 + 0.16f, a, body, body2);                                      /* body */
        r_beam(x + ca * 0.16f, y0 + 0.12f, z + sa * 0.16f, ca, 0.9f, sa, 0.16f, 0.07f, 0.07f, body);      /* neck */
        r_obox(x + ca * 0.27f, y0 + 0.24f, z + sa * 0.27f, 0.09f, 0.055f, y0 + 0.32f, a, body, body2);    /* head */
        r_beam(x + ca * 0.3f, y0 + 0.32f, z + sa * 0.3f, ca * 0.5f, 1.0f, sa * 0.5f, 0.14f, 0.02f, 0.02f, 0xFFD040FF);   /* horn */
        for (int k = 0; k < 4; k++)                                          /* rainbow mane */
            r_obox(x + ca * (0.2f - 0.04f * (float)k), y0 + 0.3f - 0.04f * (float)k, z + sa * (0.2f - 0.04f * (float)k), 0.02f, 0.06f, y0 + 0.34f - 0.04f * (float)k, a, RB[k], RB[k]);
        for (int k = 0; k < 3; k++)                                          /* tail */
            r_beam(x - ca * 0.2f, y0 + 0.13f, z - sa * 0.2f, -ca + px * 0.3f * (float)(k - 1), -0.4f + 0.2f * sinf(e->anim * 3.0f), -sa + pz * 0.3f * (float)(k - 1), 0.16f, 0.03f, 0.03f, RB[(k * 2 + 1) % 6]);
        gfx_blend(BL_ADD);
        eyes(x + ca * 0.27f, y0 + 0.29f, z + sa * 0.27f, a, 0.07f, 0.04f, 0.025f, e->dash_t > 0 ? 0xFF4060FF : 0x6040FFFF);   /* wild eyes */
        if (e->dash_t > 0) r_glow(x, 0.3f, z, 0.45f, RGBA(255, 180, 240, 70));
        gfx_blend(BL_ALPHA);
        break;
    }
    case EN_LEPRECHAUN: {
        /* a little green man with a tall hat and a pot of gold, blinking ahead in a puff of clover */
        if (e->dash_t > 0) {
            gfx_blend(BL_ADD);
            r_glow(x, 0.3f, z, 0.4f, RGBA(120, 255, 120, 160));
            gfx_blend(BL_ALPHA);
            break;
        }
        float step = sinf(e->anim * 2.4f), px = -sa, pz = ca;
        uint32_t green = en_tint(e, 0x30A040FF), green2 = scale_rgb(green, 0.75f);
        for (int s2 = -1; s2 <= 1; s2 += 2) {
            float f = step * 0.06f * (float)s2;
            r_obox(x + ca * f + px * 0.05f * (float)s2, 0, z + sa * f + pz * 0.05f * (float)s2, 0.04f, 0.03f, 0.08f, a, 0x302018FF, 0x201410FF);
        }
        r_obox(x, 0.08f, z, 0.08f, 0.07f, 0.26f, a, green, green2);
        r_obox(x, 0.15f, z, 0.085f, 0.075f, 0.17f, a, 0x201810FF, 0x201810FF);                         /* belt */
        r_crystal(x + ca * 0.01f, 0.31f, z + sa * 0.01f, 0.06f, 0.05f, 0.04f, 6, a, en_tint(e, 0xF0C8A0FF));
        r_crystal(x + ca * 0.03f, 0.27f, z + sa * 0.03f, 0.05f, 0.0f, 0.04f, 6, a, 0xE06020FF);        /* ginger beard */
        r_prism(x, 0.34f, z, 0.09f, 0.09f, 0.355f, 8, a, green, green2);                                /* hat */
        r_prism(x, 0.355f, z, 0.055f, 0.05f, 0.47f, 8, a, green, green2);
        r_prism(x, 0.36f, z, 0.057f, 0.057f, 0.38f, 8, a, 0x201810FF, 0x201810FF);
        float hx = x - px * 0.11f, hz = z - pz * 0.11f;                                                  /* pot of gold */
        r_prism(hx, 0.05f, hz, 0.05f, 0.06f, 0.13f, 8, 0, 0x202020FF, 0x181818FF);
        r_crystal(hx, 0.13f, hz, 0.055f, 0.04f, 0.0f, 6, 0, 0xFFD040FF);
        break;
    }
    case EN_FROG: {
        /* a fat green frog, crouching and hopping, throat puffing */
        float y = e->y, puff = 0.5f + 0.5f * sinf(T * 6.0f + x * 3.0f), px = -sa, pz = ca;
        uint32_t skin = en_tint(e, 0x60C040FF), belly = en_tint(e, 0xD8F0A0FF);
        int air = e->dash_t > 0;
        for (int s2 = -1; s2 <= 1; s2 += 2)                                  /* back legs, stretched out mid-hop */
            r_beam(x - ca * 0.05f + px * 0.09f * (float)s2, y + 0.04f, z - sa * 0.05f + pz * 0.09f * (float)s2, -ca * (air ? 1.0f : 0.3f), air ? -0.3f : 0.0f, -sa * (air ? 1.0f : 0.3f), 0.14f, 0.05f, 0.04f, scale_rgb(skin, 0.85f));
        r_crystal(x, y + 0.1f, z, 0.15f, 0.1f, 0.08f, 7, a, skin);
        r_crystal(x + ca * 0.08f, y + 0.07f, z + sa * 0.08f, 0.06f + 0.02f * puff, 0.03f, 0.04f, 6, a, belly);   /* throat */
        for (int s2 = -1; s2 <= 1; s2 += 2) {                                /* bulging eyes */
            float ex = x + ca * 0.08f + px * 0.07f * (float)s2, ez = z + sa * 0.08f + pz * 0.07f * (float)s2;
            r_crystal(ex, y + 0.19f, ez, 0.035f, 0.03f, 0.02f, 6, a, skin);
            r_crystal(ex + ca * 0.02f, y + 0.2f, ez + sa * 0.02f, 0.015f, 0.015f, 0.01f, 4, a, 0x101010FF);
        }
        r_prism(x, y + 0.17f, z, 0.06f, 0.05f, y + 0.2f, 6, 0, 0xFFD040FF, 0xD0A020FF);              /* a tiny crown: a prince? */
        break;
    }
    case EN_PIXIE: {
        /* a mischievous pixie: a glowing little figure on shimmering wings, trailing fairy dust */
        float y = e->y, flap = sinf(e->anim * 6.0f) * 0.7f, px = -sa, pz = ca;
        r_gdisc(x, z, 0.006f, 0.16f, 10, 0x60308040, 0x00000000);
        r_unlit = 1;
        r_obox(x, y - 0.06f, z, 0.04f, 0.035f, y + 0.06f, a, en_tint(e, 0xFF90E0FF), en_tint(e, 0xE070C0FF));
        r_crystal(x, y + 0.1f, z, 0.04f, 0.04f, 0.03f, 6, a, en_tint(e, 0xFFE0D0FF));
        r_unlit = 0;
        gfx_blend(BL_ADD);
        for (int s2 = -1; s2 <= 1; s2 += 2) {
            uint32_t wc = RGBA(200, 230, 255, 150);
            float wx = x + px * 0.18f * (float)s2 * cosf(flap), wy = y + 0.1f + 0.12f * sinf(flap), wz = z + pz * 0.18f * (float)s2 * cosf(flap);
            wtri(x, y + 0.03f, z, wx + ca * 0.06f, wy, wz + sa * 0.06f, wx - ca * 0.06f, wy - 0.05f, wz - sa * 0.06f, wc);
        }
        r_glow(x, y, z, 0.3f, RGBA(255, 160, 240, 90));
        for (int k = 1; k <= 3; k++)
            r_glow_cheap(x - ca * 0.12f * (float)k, y - 0.03f * (float)k, z - sa * 0.12f * (float)k, 0.04f, RGBA(255, 240, 180, 200 - 50 * k));
        gfx_blend(BL_ALPHA);
        break;
    }
    case EN_TROLL: {
        /* a big lumbering troll: green-grey, a club over its shoulder, stomping */
        float step = sinf(e->anim * 1.2f), px = -sa, pz = ca;
        float raise = e->act_t > 0 ? (e->act_t > 0.22f ? (0.6f - e->act_t) / 0.38f : e->act_t / 0.22f) : 0.0f;
        uint32_t skin = en_tint(e, 0x7A8A5AFF), skin2 = scale_rgb(skin, 0.78f);
        if (e->armor_t > 0) skin = col_mix(skin, 0xFFFFFFFF, 0.4f);
        for (int s2 = -1; s2 <= 1; s2 += 2) {
            float f = step * 0.12f * (float)s2;
            r_obox(x + ca * f + px * 0.16f * (float)s2, 0, z + sa * f + pz * 0.16f * (float)s2, 0.1f, 0.1f, 0.22f, a, skin2, skin2);
        }
        r_obox(x, 0.2f, z, 0.26f, 0.22f, 0.58f, a, skin, skin2);
        r_obox(x, 0.22f, z, 0.27f, 0.23f, 0.34f, a, 0x6A4A30FF, 0x5A3A24FF);                            /* rough loincloth */
        r_crystal(x + ca * 0.12f, 0.66f, z + sa * 0.12f, 0.15f, 0.12f, 0.08f, 6, a, skin);              /* hunched head */
        r_crystal(x + ca * 0.24f, 0.62f, z + sa * 0.24f, 0.06f, 0.04f, 0.04f, 5, a, skin2);             /* big nose */
        r_beam(x + px * 0.3f, 0.5f, z + pz * 0.3f, ca * (0.3f + raise), 0.6f - raise * 1.4f, sa * (0.3f + raise), 0.4f, 0.09f, 0.09f, 0x6A4A2AFF);   /* club */
        gfx_blend(BL_ADD);
        eyes(x + ca * 0.12f, 0.68f, z + sa * 0.12f, a, 0.12f, 0.05f, 0.03f, 0xFFE040FF);
        gfx_blend(BL_ALPHA);
        break;
    }
    case EN_WITCH: {
        /* a witch in a pointed hat and purple robe, wand glowing green before she hexes */
        float step = sinf(e->anim * 2.0f), px = -sa, pz = ca;
        uint32_t robe = en_tint(e, 0x6A3A8AFF), robe2 = scale_rgb(robe, 0.75f);
        r_prism(x, 0, z, 0.15f, 0.09f, 0.34f, 8, a, robe, robe2);
        r_crystal(x + ca * 0.02f, 0.4f, z + sa * 0.02f, 0.06f, 0.05f, 0.04f, 6, a, en_tint(e, 0x90D080FF));   /* green face */
        r_prism(x, 0.44f, z, 0.14f, 0.13f, 0.46f, 8, 0, 0x201828FF, 0x181020FF);                       /* hat brim */
        r_prism(x - ca * 0.02f, 0.46f, z - sa * 0.02f, 0.07f, 0.0f, 0.68f, 8, 0, 0x201828FF, 0x181020FF);
        float charge = clampf(1.0f - e->pulse_t / 0.8f, 0, 1);
        float wx = x + px * 0.13f + ca * 0.1f, wz = z + pz * 0.13f + sa * 0.1f;
        r_beam(x + px * 0.1f, 0.26f, z + pz * 0.1f, ca, 0.3f + step * 0.1f, sa, 0.16f, 0.015f, 0.015f, 0x3A2A1AFF);   /* wand */
        gfx_blend(BL_ADD);
        r_glow_cheap(wx, 0.33f, wz, 0.06f + 0.1f * charge, RGBA(140, 255, 100, (int)(160 + 90 * charge)));
        if (charge > 0) r_gring(x, z, 0.03f, 0.3f + 1.6f * charge, 0.05f, 14, RGBA(170, 90, 255, (int)(180 * charge)));
        eyes(x + ca * 0.02f, 0.41f, z + sa * 0.02f, a, 0.05f, 0.025f, 0.02f, 0xFFF040FF);
        gfx_blend(BL_ALPHA);
        break;
    }
    case EN_GINGER: case EN_GINGERBIT: {
        /* a runaway gingerbread man with icing buttons (the bits are little ones) */
        float k = e->type == EN_GINGERBIT ? 0.6f : 1.0f, step = sinf(e->anim * 2.6f), px = -sa, pz = ca;
        uint32_t dough = en_tint(e, 0xC07838FF), dough2 = scale_rgb(dough, 0.8f);
        for (int s2 = -1; s2 <= 1; s2 += 2) {
            float f = step * 0.07f * (float)s2 * k;
            r_obox(x + ca * f + px * 0.06f * (float)s2 * k, 0, z + sa * f + pz * 0.06f * (float)s2 * k, 0.045f * k, 0.03f * k, 0.15f * k, a, dough, dough2);
            r_beam(x + px * 0.1f * (float)s2 * k, 0.26f * k, z + pz * 0.1f * (float)s2 * k, px * (float)s2, 0.4f - step * 0.5f * (float)s2, pz * (float)s2, 0.1f * k, 0.04f * k, 0.03f * k, dough);
        }
        r_obox(x, 0.14f * k, z, 0.09f * k, 0.035f * k, 0.32f * k, a, dough, dough2);
        r_prism(x, 0.32f * k, z, 0.08f * k, 0.08f * k, 0.36f * k, 8, a + 1.5708f, dough, dough2);
        r_crystal(x, 0.4f * k, z, 0.08f * k, 0.05f * k, 0.03f * k, 8, a, dough);
        for (int b = 0; b < 3; b++)                                          /* icing buttons */
            r_crystal(x + ca * 0.036f * k, (0.18f + 0.045f * (float)b) * k, z + sa * 0.036f * k, 0.015f * k, 0.01f, 0.01f, 4, a, b == 1 ? 0xFF4060FF : 0xFFFFFFFF);
        break;
    }
    case EN_DRAGON: {
        /* the Dragon: a great purple dragon with gold belly scales, wings beating, a long tail, smoke from its nostrils */
        float step = sinf(e->anim * 1.0f), px = -sa, pz = ca, k = 1.0f;
        uint32_t scale = en_tint(e, 0x8040C0FF), scale2 = scale_rgb(scale, 0.75f), belly = 0xF0C060FF;
        if (e->armor_t > 0) { scale = col_mix(scale, 0xFFFFFFFF, 0.4f); scale2 = col_mix(scale2, 0xFFFFFFFF, 0.3f); }
        for (int s2 = -1; s2 <= 1; s2 += 2)
            for (int f2 = -1; f2 <= 1; f2 += 2) {
                float f = step * 0.12f * (float)(s2 * f2);
                r_obox(x + ca * (0.22f * (float)f2 + f) + px * 0.2f * (float)s2, 0, z + sa * (0.22f * (float)f2 + f) + pz * 0.2f * (float)s2, 0.07f, 0.07f, 0.24f, a, scale2, scale2);
            }
        r_obox(x, 0.2f, z, 0.36f, 0.22f, 0.52f, a, scale, scale2);                                       /* body */
        r_obox(x + ca * 0.05f, 0.21f, z + sa * 0.05f, 0.3f, 0.225f, 0.3f, a, belly, belly);            /* belly scales */
        for (int sp = 0; sp < 5; sp++)                                       /* spines down the back */
            r_prism(x + ca * (0.25f - 0.13f * (float)sp), 0.52f, z + sa * (0.25f - 0.13f * (float)sp), 0.05f, 0.0f, 0.66f, 4, a, belly, scale2);
        r_beam(x + ca * 0.3f, 0.45f, z + sa * 0.3f, ca, 0.9f, sa, 0.32f, 0.13f, 0.13f, scale);          /* neck */
        r_obox(x + ca * 0.55f, 0.66f, z + sa * 0.55f, 0.17f, 0.11f, 0.82f, a, scale, scale2);           /* head */
        r_obox(x + ca * 0.72f, 0.66f, z + sa * 0.72f, 0.07f, 0.08f, 0.76f, a, scale, scale2);           /* snout */
        for (int s2 = -1; s2 <= 1; s2 += 2)                                  /* horns */
            r_beam(x + ca * 0.48f + px * 0.07f * (float)s2, 0.8f, z + sa * 0.48f + pz * 0.07f * (float)s2, -ca, 0.8f, -sa, 0.14f, 0.025f, 0.025f, 0xF0E0C0FF);
        for (int t2 = 0; t2 < 4; t2++) {                                     /* tail, swishing */
            float sw = sinf(e->anim * 1.5f + (float)t2 * 0.7f) * 0.15f * (float)t2;
            r_beam(x - ca * (0.36f + 0.14f * (float)t2) + px * sw, 0.3f - 0.05f * (float)t2, z - sa * (0.36f + 0.14f * (float)t2) + pz * sw, -ca, -0.15f, -sa, 0.16f, 0.12f - 0.025f * (float)t2, 0.1f - 0.02f * (float)t2, scale);
        }
        /* wings beating */
        float flap = sinf(e->anim * 1.6f) * 0.6f;
        for (int s2 = -1; s2 <= 1; s2 += 2) {
            float wx = x + px * 0.75f * (float)s2 * cosf(flap) * k, wy = 0.6f + 0.6f * sinf(flap) + 0.3f, wz = z + pz * 0.75f * (float)s2 * cosf(flap) * k;
            uint32_t wc = lt_shade(0xA060E0FF, x, 0.8f, z, 0.85f), wc2 = lt_shade(0x7040B0FF, x, 0.8f, z, 0.8f);
            wtri(x + ca * 0.15f + px * 0.15f * (float)s2, 0.55f, z + sa * 0.15f + pz * 0.15f * (float)s2, x - ca * 0.2f + px * 0.15f * (float)s2, 0.55f, z - sa * 0.2f + pz * 0.15f * (float)s2, wx + ca * 0.1f, wy, wz + sa * 0.1f, wc);
            wtri(x - ca * 0.2f + px * 0.15f * (float)s2, 0.55f, z - sa * 0.2f + pz * 0.15f * (float)s2, wx + ca * 0.1f, wy, wz + sa * 0.1f, wx - ca * 0.45f, wy - 0.25f, wz - sa * 0.45f, wc2);
        }
        gfx_blend(BL_ADD);
        float charge = clampf(1.0f - e->pulse_t / 1.0f, 0, 1);
        eyes(x + ca * 0.6f, 0.78f, z + sa * 0.6f, a, 0.05f, 0.07f, 0.04f, 0xFFE040FF);
        r_glow(x + ca * 0.8f, 0.7f, z + sa * 0.8f, 0.15f + 0.45f * charge, RGBA(255, 140, 40, (int)(80 + 150 * charge)));   /* fire in its throat */
        for (int p2 = 0; p2 < 2; p2++) {                                     /* smoke curling from the nostrils */
            float u = fmodf(T * 0.9f + (float)p2 * 0.5f, 1.0f);
            r_glow(x + ca * (0.8f + u * 0.2f), 0.78f + u * 0.4f, z + sa * (0.8f + u * 0.2f), 0.08f + u * 0.2f, RGBA(180, 170, 190, (int)(70 * (1.0f - u))));
        }
        gfx_blend(BL_ALPHA);
        break;
    }
    /* ---------------- level 6: visitors from the future ---------------- */
    case EN_HOVERBOARD: {
        /* a kid on a pink hoverboard, weaving and leaning */
        float weave = sinf(e->anim * 0.9f) * 0.12f, px = -sa, pz = ca;
        float bx = x + px * weave, bz = z + pz * weave, by = 0.12f + 0.03f * sinf(e->anim * 2.0f), lean = cosf(e->anim * 0.9f) * 0.25f;
        uint32_t board = en_tint(e, 0xF050A0FF);
        gfx_blend(BL_ADD);
        r_gdisc(bx, bz, 0.01f, 0.22f, 10, RGBA(120, 255, 200, 80), 0x60FFC000);
        gfx_blend(BL_ALPHA);
        r_obox(bx, by, bz, 0.22f, 0.08f, by + 0.03f, a, board, scale_rgb(board, 0.75f));
        r_obox(bx + ca * 0.16f, by + 0.01f, bz + sa * 0.16f, 0.05f, 0.075f, by + 0.04f, a, 0xFFE040FF, 0xD0B020FF);
        float hx = bx + px * lean * 0.12f, hz = bz + pz * lean * 0.12f;
        for (int s2 = -1; s2 <= 1; s2 += 2)
            r_beam(bx + ca * 0.06f * (float)s2, by + 0.03f, bz + sa * 0.06f * (float)s2, px * lean * 0.3f, 1.0f, pz * lean * 0.3f, 0.16f, 0.035f, 0.035f, 0x3A4A80FF);
        r_obox(hx, by + 0.19f, hz, 0.075f, 0.06f, by + 0.36f, a, en_tint(e, 0xE8701CFF), en_tint(e, 0xB85010FF));     /* puffy orange vest */
        r_crystal(hx, by + 0.42f, hz, 0.05f, 0.045f, 0.035f, 6, a, en_tint(e, 0xE8B890FF));
        r_prism(hx, by + 0.44f, hz, 0.055f, 0.045f, by + 0.48f, 6, a, 0xC02020FF, 0x901818FF);
        gfx_blend(BL_ADD);
        r_glow_cheap(bx - ca * 0.2f, by + 0.02f, bz - sa * 0.2f, 0.1f, RGBA(120, 255, 200, 160));
        gfx_blend(BL_ALPHA);
        break;
    }
    case EN_DRONE: {
        /* a camera drone: four spinning rotors, blinking lights, a lens looking down */
        float y = e->y;
        r_gdisc(x, z, 0.006f, 0.2f, 12, 0x00000040, 0x00000000);
        uint32_t body = en_tint(e, 0x404850FF);
        r_obox(x, y - 0.04f, z, 0.09f, 0.09f, y + 0.04f, a, col_mix(body, 0xA0A8B0FF, 0.2f), body);
        for (int k = 0; k < 4; k++) {
            float an = a + 0.785f + (float)k * 1.5708f, rx = x + cosf(an) * 0.17f, rz = z + sinf(an) * 0.17f;
            r_beam(x, y, z, cosf(an), 0, sinf(an), 0.17f, 0.025f, 0.02f, body);
            float spin = T * 40.0f + (float)k;
            uint32_t bl = lt_shade(0x8090A0FF, rx, y, rz, 0.9f);
            r_line(rx - cosf(spin) * 0.09f, y + 0.035f, rz - sinf(spin) * 0.09f, rx + cosf(spin) * 0.09f, y + 0.035f, rz + sinf(spin) * 0.09f, 1.4f, bl, bl);
        }
        gfx_blend(BL_ADD);
        int blink = fmodf(T * 2.0f + x, 1.0f) < 0.15f;
        r_glow_cheap(x + ca * 0.1f, y, z + sa * 0.1f, 0.05f, RGBA(255, 60, 40, blink ? 255 : 90));
        r_glow_cheap(x - ca * 0.1f, y, z - sa * 0.1f, 0.05f, RGBA(60, 255, 90, blink ? 90 : 255));
        r_glow_cheap(x, y - 0.06f, z, 0.04f, RGBA(120, 200, 255, 200));
        gfx_blend(BL_ALPHA);
        break;
    }
    case EN_SERVO: {
        /* a chunky chrome robot with a visor, clanking along on stubby legs */
        float step = sinf(e->anim * 1.8f), px = -sa, pz = ca;
        uint32_t chrome = en_tint(e, 0xB8C0CCFF), dk = scale_rgb(chrome, 0.7f);
        if (e->armor_t > 0) chrome = col_mix(chrome, 0xFFFFFFFF, 0.5f);
        for (int s2 = -1; s2 <= 1; s2 += 2) {
            float f = step * 0.07f * (float)s2;
            r_obox(x + ca * f + px * 0.1f * (float)s2, 0, z + sa * f + pz * 0.1f * (float)s2, 0.06f, 0.07f, 0.13f, a, 0x50545CFF, 0x40444CFF);
        }
        r_obox(x, 0.12f, z, 0.17f, 0.15f, 0.42f, a, chrome, dk);
        r_obox(x + ca * 0.15f, 0.2f, z + sa * 0.15f, 0.02f, 0.1f, 0.34f, a, 0x30343CFF, 0x30343CFF);       /* chest panel */
        for (int s2 = -1; s2 <= 1; s2 += 2) {
            float sw = step * 0.4f * (float)s2;
            r_beam(x + px * 0.2f * (float)s2, 0.36f, z + pz * 0.2f * (float)s2, ca * sw, -1.0f, sa * sw, 0.2f, 0.05f, 0.05f, dk);
        }
        r_prism(x, 0.42f, z, 0.11f, 0.09f, 0.56f, 8, a, chrome, dk);
        r_prism(x, 0.56f, z, 0.012f, 0.008f, 0.68f, 4, 0, 0x606060FF, 0x505050FF);
        gfx_blend(BL_ADD);
        float vx = x + ca * 0.1f, vz = z + sa * 0.1f;
        r_line(vx - sa * 0.07f, 0.5f, vz + ca * 0.07f, vx + sa * 0.07f, 0.5f, vz - ca * 0.07f, 3.0f, 0x60E8FFFF, 0x60E8FFFF);
        r_glow_cheap(x, 0.69f, z, 0.04f, RGBA(255, 80, 60, fmodf(T * 1.5f + x, 1.0f) < 0.3f ? 255 : 80));
        gfx_blend(BL_ALPHA);
        break;
    }
    case EN_HOLOSHARK: {
        /* a holographic shark swimming through the air: translucent blue, scanlines, glitching when out of reach */
        int phased = e->dash_t > 0;
        float fade = phased ? 0.12f + 0.1f * (sinf(T * 30.0f) > 0) : 0.6f + 0.1f * sinf(T * 9.0f);
        float y = 0.42f + 0.06f * sinf(e->anim * 0.8f), sw = sinf(e->anim * 2.2f) * 0.08f, px = -sa, pz = ca;
        float gl = phased || fmodf(T * 1.3f + x, 3.0f) < 0.08f ? sinf(T * 70.0f) * 0.06f : 0.0f;   /* glitch jump */
        float gx = x + px * gl, gz = z + pz * gl;
        r_gdisc(x, z, 0.006f, 0.3f, 12, RGBA(40, 120, 200, (int)(70 * fade)), 0x2060A000);
        r_alpha = fade;
        r_unlit = 1;
        uint32_t skin = en_tint(e, 0x70C8FFFF), belly = 0xC0F0FFFF;
        r_beam(gx - ca * 0.26f, y - 0.07f, gz - sa * 0.26f, ca, 0.0f, sa, 0.5f, 0.2f, 0.16f, skin);
        r_beam(gx + ca * 0.22f, y - 0.08f, gz + sa * 0.22f, ca, -0.15f, sa, 0.16f, 0.14f, 0.11f, belly);
        r_beam(gx - ca * 0.3f, y - 0.05f, gz - sa * 0.3f, -ca, 0.0f, -sa, 0.1f, 0.1f, 0.1f, skin);
        r_unlit = 0;
        r_alpha = 1.0f;
        gfx_blend(BL_ADD);
        uint32_t fc = RGBA(120, 210, 255, (int)(200 * fade));
        wtri(gx + ca * 0.1f, y + 0.09f, gz + sa * 0.1f, gx - ca * 0.14f, y + 0.09f, gz - sa * 0.14f, gx - ca * 0.18f, y + 0.36f, gz - sa * 0.18f, fc);
        for (int s2 = -1; s2 <= 1; s2 += 2)                 /* pectoral fins */
            wtri(gx + ca * 0.05f + px * 0.08f * (float)s2, y - 0.08f, gz + sa * 0.05f + pz * 0.08f * (float)s2, gx - ca * 0.08f + px * 0.08f * (float)s2, y - 0.08f, gz - sa * 0.08f + pz * 0.08f * (float)s2,
                 gx - ca * 0.12f + px * 0.26f * (float)s2, y - 0.16f, gz - sa * 0.12f + pz * 0.26f * (float)s2, fc);
        float tx = gx - ca * 0.38f, tz = gz - sa * 0.38f;
        wtri(tx, y, tz, tx - ca * 0.16f + px * (0.12f + sw), y + 0.14f, tz - sa * 0.16f + pz * (0.12f + sw), tx - ca * 0.16f + px * (-0.12f + sw), y - 0.12f, tz - sa * 0.16f + pz * (-0.12f + sw), fc);
        for (int k = 0; k < 4; k++) {                        /* scanlines */
            float yy = y - 0.06f + 0.04f * (float)k + fmodf(T * 0.3f, 0.04f);
            r_line(gx - ca * 0.3f, yy, gz - sa * 0.3f, gx + ca * 0.3f, yy, gz + sa * 0.3f, 1.0f, RGBA(180, 240, 255, (int)(90 * fade)), RGBA(180, 240, 255, (int)(90 * fade)));
        }
        r_glow(x, y, z, 0.5f, RGBA(80, 170, 255, (int)(60 * fade)));
        gfx_blend(BL_ALPHA);
        break;
    }
    case EN_PARADOX: case EN_ECHO: {
        /* a paradox: the same figure from two moments at once, the copies jittering apart; echoes are the leftovers */
        int echo = e->type == EN_ECHO;
        float k = echo ? 0.72f : 1.0f, step = sinf(e->anim * 2.0f), px = -sa, pz = ca;
        float jit = (echo ? 0.03f : 0.06f) + 0.03f * sinf(T * 23.0f + x * 5.0f);
        r_unlit = 1;
        for (int cp = 0; cp < (echo ? 1 : 2); cp++) {
            float off = echo ? 0.0f : (cp ? jit : -jit);
            float cx = x + px * off - ca * (cp ? 0.04f : 0.0f), cz = z + pz * off - sa * (cp ? 0.04f : 0.0f);
            uint32_t col = en_tint(e, echo ? 0xC8A0FFFF : (cp ? 0x60E0FFFF : 0xB060FFFF));
            r_alpha = echo ? 0.55f : 0.7f;
            for (int s2 = -1; s2 <= 1; s2 += 2) {
                float f = step * 0.07f * (float)s2 * k;
                r_obox(cx + ca * f + px * 0.06f * (float)s2 * k, 0, cz + sa * f + pz * 0.06f * (float)s2 * k, 0.04f * k, 0.04f * k, 0.18f * k, a, col, scale_rgb(col, 0.8f));
            }
            r_obox(cx, 0.17f * k, cz, 0.1f * k, 0.07f * k, 0.42f * k, a, col, scale_rgb(col, 0.8f));
            r_crystal(cx, 0.5f * k, cz, 0.08f * k, 0.07f * k, 0.05f * k, 6, a, col_mix(col, 0xFFFFFFFF, 0.3f));
        }
        r_alpha = 1.0f;
        r_unlit = 0;
        gfx_blend(BL_ADD);
        r_glow(x, 0.3f * k, z, 0.35f * k, RGBA(170, 110, 255, 70));
        if (!echo) {                                         /* after-images trailing behind */
            for (int t2 = 1; t2 <= 2; t2++) {
                float bx = x - ca * 0.18f * (float)t2, bz = z - sa * 0.18f * (float)t2;
                r_glow_cheap(bx, 0.3f, bz, 0.16f, RGBA(140, 90, 255, 60 / t2));
            }
        }
        gfx_blend(BL_ALPHA);
        break;
    }
    case EN_HOVERCAR: {
        /* a flying car from the future: boxy teal-and-white body with fins, wheels turned flat into glowing pods */
        float hy = 0.28f + 0.04f * sinf(T * 2.1f + x), px = -sa, pz = ca;
        uint32_t paint = en_tint(e, 0x30D0C0FF), white = en_tint(e, 0xE8ECF0FF);
        if (e->armor_t > 0) { paint = col_mix(paint, 0xFFFFFFFF, 0.4f); white = col_mix(white, 0xFFFFFFFF, 0.4f); }
        gfx_blend(BL_ADD);
        r_gdisc(x, z, 0.012f, 0.65f, 12, RGBA(90, 220, 255, 70), 0x40C0FF00);
        gfx_blend(BL_ALPHA);
        for (int k = 0; k < 4; k++) {
            float wx = x + ca * ((k & 1) ? 0.3f : -0.3f) + px * ((k & 2) ? 0.24f : -0.24f), wz = z + sa * ((k & 1) ? 0.3f : -0.3f) + pz * ((k & 2) ? 0.24f : -0.24f);
            r_prism(wx, hy - 0.06f, wz, 0.1f, 0.1f, hy - 0.02f, 8, 0, 0x202428FF, 0x181A1EFF);
        }
        r_obox(x, hy, z, 0.42f, 0.24f, hy + 0.13f, a, paint, scale_rgb(paint, 0.75f));
        r_obox(x - ca * 0.05f, hy + 0.13f, z - sa * 0.05f, 0.24f, 0.2f, hy + 0.25f, a, white, 0x283848FF);
        for (int s2 = -1; s2 <= 1; s2 += 2)                 /* tail fins */
            r_obox(x - ca * 0.36f + px * 0.2f * (float)s2, hy + 0.13f, z - sa * 0.36f + pz * 0.2f * (float)s2, 0.07f, 0.02f, hy + 0.27f, a, white, scale_rgb(white, 0.8f));
        gfx_blend(BL_ADD);
        for (int k = 0; k < 4; k++) {
            float wx = x + ca * ((k & 1) ? 0.3f : -0.3f) + px * ((k & 2) ? 0.24f : -0.24f), wz = z + sa * ((k & 1) ? 0.3f : -0.3f) + pz * ((k & 2) ? 0.24f : -0.24f);
            r_glow_cheap(wx, hy - 0.07f, wz, 0.1f, RGBA(100, 220, 255, 200));
        }
        for (int s2 = -1; s2 <= 1; s2 += 2) r_glow_cheap(x + ca * 0.43f + px * 0.15f * (float)s2, hy + 0.07f, z + sa * 0.43f + pz * 0.15f * (float)s2, 0.07f, RGBA(255, 250, 220, 230));
        float charge = clampf(1.0f - e->pulse_t / 0.8f, 0, 1);
        if (charge > 0) r_gring(x, z, 0.03f, 0.5f + 0.4f * charge, 0.05f, 14, RGBA(120, 240, 255, (int)(200 * charge)));
        gfx_blend(BL_ALPHA);
        break;
    }
    case EN_TIMETRAIN: {
        /* the Time Train: a flying steam locomotive, black and brass, on glowing hover pods, the firebox roaring */
        if (e->dash_t > 0) {
            /* mid time-jump: just a white-hot streak */
            gfx_blend(BL_ADD);
            r_glowline(e->jx, 0.5f, e->jz, x, 0.5f, z, 30.0f, RGBA(180, 220, 255, 120));
            r_line(e->jx, 0.5f, e->jz, x, 0.5f, z, 5.0f, RGBA(255, 255, 255, 230), RGBA(200, 230, 255, 120));
            r_glow(x, 0.5f, z, 1.0f, RGBA(220, 240, 255, 200));
            gfx_blend(BL_ALPHA);
            break;
        }
        float hy = 0.22f + 0.04f * sinf(T * 1.4f), px = -sa, pz = ca;
        uint32_t iron = en_tint(e, 0x30343CFF), iron2 = scale_rgb(iron, 0.75f), brass = 0xC89838FF, red = en_tint(e, 0xA02820FF);
        if (e->armor_t > 0) { iron = col_mix(iron, 0xFFFFFFFF, 0.4f); iron2 = col_mix(iron2, 0xFFFFFFFF, 0.3f); }
        gfx_blend(BL_ADD);
        r_gdisc(x, z, 0.012f, 1.0f, 14, RGBA(90, 160, 255, 80), 0x4080FF00);
        gfx_blend(BL_ALPHA);
        r_obox(x, hy, z, 0.68f, 0.22f, hy + 0.1f, a, red, scale_rgb(red, 0.7f));                 /* running board / frame */
        for (int k = -2; k <= 2; k++) {                                                          /* wheels with pistons */
            float wx = x + ca * 0.24f * (float)k, wz = z + sa * 0.24f * (float)k;
            for (int s2 = -1; s2 <= 1; s2 += 2)
                r_beam(wx + px * 0.24f * (float)s2, hy + 0.02f, wz + pz * 0.24f * (float)s2, px * (float)s2, 0, pz * (float)s2, 0.04f, 0.2f, 0.2f, 0x202024FF);
        }
        r_beam(x - ca * 0.2f, hy + 0.1f, z - sa * 0.2f, ca, 0, sa, 0.75f, 0.32f, 0.3f, iron);                 /* boiler */
        for (int k = 0; k < 3; k++)
            r_beam(x + ca * (0.0f + 0.22f * (float)k), hy + 0.095f, z + sa * (0.0f + 0.22f * (float)k), ca, 0, sa, 0.03f, 0.33f, 0.31f, brass);
        r_obox(x - ca * 0.44f, hy + 0.1f, z - sa * 0.44f, 0.2f, 0.22f, hy + 0.62f, a, iron2, iron);              /* the cab */
        r_obox(x - ca * 0.44f, hy + 0.62f, z - sa * 0.44f, 0.24f, 0.25f, hy + 0.66f, a, 0x202024FF, 0x202024FF);
        r_prism(x + ca * 0.38f, hy + 0.4f, z + sa * 0.38f, 0.07f, 0.11f, hy + 0.66f, 8, 0, iron2, iron);        /* smokestack */
        r_prism(x + ca * 0.12f, hy + 0.4f, z + sa * 0.12f, 0.07f, 0.06f, hy + 0.5f, 8, 0, brass, scale_rgb(brass, 0.8f));
        r_obox(x + ca * 0.66f, hy - 0.04f, z + sa * 0.66f, 0.06f, 0.2f, hy + 0.14f, a + 0.0f, red, scale_rgb(red, 0.7f));   /* cow-catcher */
        gfx_blend(BL_ADD);
        for (int k = -1; k <= 1; k += 2)                                                         /* hover pods */
            for (int s2 = -1; s2 <= 1; s2 += 2)
                r_glow_cheap(x + ca * 0.45f * (float)k + px * 0.25f * (float)s2, hy - 0.04f, z + sa * 0.45f * (float)k + pz * 0.25f * (float)s2, 0.14f, RGBA(110, 190, 255, 220));
        r_glow(x - ca * 0.3f, hy + 0.2f, z - sa * 0.3f, 0.35f, RGBA(255, 120, 40, 140));          /* firebox */
        r_glow_cheap(x + ca * 0.76f, hy + 0.3f, z + sa * 0.76f, 0.12f, RGBA(255, 250, 210, 240)); /* headlamp */
        for (int k = 0; k < 4; k++) {                                                            /* steam puffing from the stack */
            float u = fmodf(T * 0.8f + (float)k * 0.25f, 1.0f);
            r_glow(x + ca * (0.38f - u * 0.6f), hy + 0.7f + u * 0.6f, z + sa * (0.38f - u * 0.6f), 0.15f + u * 0.3f, RGBA(200, 200, 215, (int)(60 * (1.0f - u))));
        }
        float charge = clampf(1.0f - e->pulse_t / 1.0f, 0, 1);                                   /* about to jump: crackling */
        if (charge > 0) r_glow(x, 0.5f, z, 0.8f + 0.6f * charge, RGBA(160, 210, 255, (int)(150 * charge)));
        gfx_blend(BL_ALPHA);
        break;
    }
    /* ---------------- level 5: pirates ---------------- */
    case EN_ROWBOAT: {
        float bob = sinf(T * 2.2f + x * 1.3f + z) * 0.025f, px = -sa, pz = ca;
        uint32_t wood = en_tint(e, 0x7A5434FF), wood2 = scale_rgb(wood, 0.72f);
        if (e->armor_t > 0) wood = col_mix(wood, 0xFFF0D0FF, 0.4f);
        r_obox(x, -0.04f + bob, z, 0.34f, 0.15f, 0.12f + bob, a, wood, wood2);                          /* hull */
        r_obox(x + ca * 0.36f, -0.02f + bob, z + sa * 0.36f, 0.07f, 0.09f, 0.15f + bob, a + 0.785f, wood, wood2);   /* prow */
        r_obox(x, 0.121f + bob, z, 0.28f, 0.11f, 0.125f + bob, a, 0x3A2818FF, 0x3A2818FF);              /* inside */
        for (int k = 0; k < 2; k++) {                                                                   /* two pirates rowing */
            float o = k ? -0.14f : 0.1f, row = sinf(e->anim * 1.6f + (float)k * 0.4f);
            float bx = x + ca * o, bz = z + sa * o, lean = row * 0.03f;
            uint32_t shirt = k ? 0xE8E4DCFF : 0xC83A2EFF;
            r_obox(bx - ca * lean, 0.12f + bob, bz - sa * lean, 0.055f, 0.07f, 0.27f + bob, a, en_tint(e, shirt), scale_rgb(en_tint(e, shirt), 0.8f));
            r_crystal(bx - ca * lean, 0.31f + bob, bz - sa * lean, 0.05f, 0.04f, 0.035f, 6, a, en_tint(e, 0xD8A880FF));
            r_prism(bx - ca * lean, 0.32f + bob, bz - sa * lean, 0.055f, 0.03f, 0.36f + bob, 6, a, k ? 0x202020FF : 0xB82020FF, 0x181818FF);
            for (int s2 = -1; s2 <= 1; s2 += 2)                                                         /* oars */
                r_beam(bx + px * 0.1f * (float)s2, 0.2f + bob, bz + pz * 0.1f * (float)s2, px * (float)s2 + ca * row * 0.5f, -0.55f, pz * (float)s2 + sa * row * 0.5f, 0.32f, 0.025f, 0.02f, 0x9A7A50FF);
        }
        /* a tiny black flag at the stern */
        float fx = x - ca * 0.3f, fz = z - sa * 0.3f, fl = sinf(T * 6.0f + x) * 0.03f;
        r_beam(fx, 0.1f + bob, fz, 0, 1, 0, 0.32f, 0.015f, 0.015f, 0x4A3420FF);
        uint32_t fc = lt_shade(0x181818FF, fx, 0.4f, fz, 0.9f);
        wtri(fx, 0.42f + bob, fz, fx, 0.32f + bob, fz, fx - ca * 0.15f + px * fl, 0.37f + bob, fz - sa * 0.15f + pz * fl, fc);
        break;
    }
    case EN_SHARK: {
        float px = -sa, pz = ca;
        if (e->dash_t > 0) {
            /* under the surface: only a dark shape gliding along and a few bubbles */
            r_gdisc(x, z, 0.008f, 0.3f, 12, 0x08142070, 0x08142000);
            gfx_blend(BL_ADD);
            for (int k = 0; k < 3; k++) {
                float ph = fmodf(T * 1.5f + (float)k * 0.33f, 1.0f);
                r_gring(x - ca * 0.1f * (float)k, z - sa * 0.1f * (float)k, 0.01f, 0.03f + 0.1f * ph, 0.015f, 8, RGBA(190, 220, 240, (int)(90 * (1.0f - ph))));
            }
            gfx_blend(BL_ALPHA);
            break;
        }
        float sw = sinf(e->anim * 2.4f) * 0.06f;
        uint32_t skin = en_tint(e, 0x8A9AA8FF), belly = en_tint(e, 0xD8E0E8FF);
        r_beam(x - ca * 0.28f, 0.02f, z - sa * 0.28f, ca, 0.0f, sa, 0.56f, 0.16f, 0.08f, skin);              /* back, just awash */
        r_beam(x + ca * 0.24f, 0.03f, z + sa * 0.24f, ca, -0.15f, sa, 0.14f, 0.11f, 0.06f, belly);            /* snout */
        uint32_t fin = lt_shade(scale_rgb(skin, 0.9f), x, 0.3f, z, 1.0f);
        float fx = x - ca * 0.02f, fz = z - sa * 0.02f;
        wtri(fx + ca * 0.12f, 0.08f, fz + sa * 0.12f, fx - ca * 0.12f, 0.08f, fz - sa * 0.12f, fx - ca * 0.14f + px * sw, 0.38f, fz - sa * 0.14f + pz * sw, fin);   /* dorsal fin */
        float tx = x - ca * 0.3f, tz = z - sa * 0.3f;
        wtri(tx, 0.06f, tz, tx - ca * 0.16f + px * (0.12f + sw * 2.0f), 0.2f, tz - sa * 0.16f + pz * (0.12f + sw * 2.0f), tx - ca * 0.16f + px * (-0.12f + sw * 2.0f), 0.2f, tz - sa * 0.16f + pz * (-0.12f + sw * 2.0f), fin);
        gfx_blend(BL_ADD);
        eyes(x + ca * 0.2f, 0.09f, z + sa * 0.2f, a, 0.0f, 0.07f, 0.03f, 0xFFE070FF);
        gfx_blend(BL_ALPHA);
        break;
    }
    case EN_SKULLPIRATE: {
        float bob = sinf(T * 2.6f + x * 1.7f) * 0.03f, roll = sinf(T * 1.9f + z) * 0.03f, px = -sa, pz = ca;
        uint32_t bone = en_tint(e, 0xE8E0CCFF), bone2 = scale_rgb(bone, 0.82f);
        /* a barrel bobbing in the swell */
        uint32_t brl = en_tint(e, 0x8A6038FF);
        r_prism(x, -0.06f + bob, z, 0.15f, 0.17f, 0.06f + bob, 8, a, brl, scale_rgb(brl, 0.8f));
        r_prism(x, 0.06f + bob, z, 0.17f, 0.15f, 0.18f + bob, 8, a, brl, scale_rgb(brl, 0.8f));
        r_prism(x, 0.05f + bob, z, 0.175f, 0.175f, 0.075f + bob, 8, a, 0x505050FF, 0x404040FF);
        if (e->down_t > 0) {
            /* knocked into the sea: bones float round the barrel, then climb back aboard */
            float jit = e->down_t < 0.7f ? sinf(T * 40.0f) * 0.02f : 0.0f;
            for (int i = 0; i < 4; i++) {
                float an = a + (float)i * 1.57f + T * 0.4f;
                r_beam(x + cosf(an) * 0.26f + jit, 0.01f, z + sinf(an) * 0.26f, cosf(an + 1.2f), 0.0f, sinf(an + 1.2f), 0.14f, 0.03f, 0.03f, bone);
            }
            r_crystal(x + ca * 0.3f, 0.04f + jit, z + sa * 0.3f, 0.07f, 0.06f, 0.05f, 6, a, bone);
            break;
        }
        float y0 = 0.18f + bob, rx = px * roll, rz = pz * roll;
        r_beam(x, y0, z, rx, 1, rz, 0.16f, 0.03f, 0.03f, bone2);                                         /* spine */
        for (int i = 0; i < 3; i++)
            r_obox(x + rx * (float)i, y0 + 0.06f + (float)i * 0.045f, z + rz * (float)i, 0.075f - 0.01f * (float)i, 0.065f, y0 + 0.08f + (float)i * 0.045f, a, bone, bone2);
        r_obox(x, y0 + 0.17f, z, 0.08f, 0.09f, y0 + 0.2f, a, 0x2A3A6AFF, 0x202C50FF);                   /* tattered coat collar */
        float sw = sinf(e->anim * 1.8f);
        r_beam(x + px * 0.1f, y0 + 0.18f, z + pz * 0.1f, ca * (0.6f + sw * 0.4f), 0.3f + sw * 0.5f, sa * (0.6f + sw * 0.4f), 0.14f, 0.025f, 0.025f, bone2);
        float hx = x + px * 0.1f + ca * 0.12f, hz = z + pz * 0.1f + sa * 0.12f;                           /* cutlass */
        r_beam(hx, y0 + 0.22f + sw * 0.07f, hz, ca, 0.6f + sw * 0.4f, sa, 0.24f, 0.022f, 0.008f, 0xD0D8E0FF);
        r_beam(x - px * 0.1f, y0 + 0.18f, z - pz * 0.1f, ca * 0.3f, -1.0f, sa * 0.3f, 0.14f, 0.025f, 0.025f, bone2);
        r_crystal(x + rx * 3.0f + ca * 0.02f, y0 + 0.28f, z + rz * 3.0f + sa * 0.02f, 0.075f, 0.08f, 0.04f, 6, a, bone);   /* skull */
        r_prism(x + rx * 3.0f, y0 + 0.33f, z + rz * 3.0f, 0.13f, 0.11f, y0 + 0.35f, 3, a + 1.047f, 0x1A1A1EFF, 0x141418FF);   /* tricorn hat */
        r_prism(x + rx * 3.0f, y0 + 0.35f, z + rz * 3.0f, 0.07f, 0.055f, y0 + 0.41f, 6, a, 0x1A1A1EFF, 0x141418FF);
        gfx_blend(BL_ADD);
        eyes(x + rx * 3.0f, y0 + 0.29f, z + rz * 3.0f, a, 0.07f, 0.03f, 0.03f, 0x40FF90FF);
        gfx_blend(BL_ALPHA);
        break;
    }
    case EN_PARROT: {
        float y = e->y;
        r_gdisc(x, z, 0.006f, 0.22f, 16, 0x00000040, 0x00000000);
        uint32_t body = en_tint(e, 0xE83A2AFF);
        r_obox(x, y - 0.05f, z, 0.12f, 0.06f, y + 0.06f, a, col_mix(body, 0xFFFFFFFF, 0.15f), scale_rgb(body, 0.85f));
        r_obox(x + ca * 0.13f, y - 0.01f, z + sa * 0.13f, 0.05f, 0.05f, y + 0.09f, a, body, body);
        r_beam(x + ca * 0.18f, y + 0.03f, z + sa * 0.18f, ca, -0.5f, sa, 0.07f, 0.035f, 0.035f, 0xF0D040FF);   /* hooked beak */
        r_beam(x - ca * 0.1f, y, z - sa * 0.1f, -ca, -0.15f, -sa, 0.2f, 0.06f, 0.015f, 0x2A60D8FF);           /* long blue tail */
        float flap = sinf(e->anim * 3.6f) * 0.85f;
        for (int k = -1; k <= 1; k += 2) {
            float px = -sa * (float)k, pz = ca * (float)k;
            float tx = x + px * 0.34f * cosf(flap) - ca * 0.05f, ty = y + 0.34f * sinf(flap) * 0.7f, tz = z + pz * 0.34f * cosf(flap) - sa * 0.05f;
            uint32_t wc = lt_shade(0x38B048FF, x, y, z, 0.75f + 0.25f * sinf(flap)), wt = lt_shade(0x2A60D8FF, x, y, z, 0.8f);
            float mx = x + px * 0.18f * cosf(flap), my = y + 0.18f * sinf(flap) * 0.7f + 0.02f, mz = z + pz * 0.18f * cosf(flap);
            wtri(x + ca * 0.07f, y + 0.03f, z + sa * 0.07f, x - ca * 0.08f, y + 0.03f, z - sa * 0.08f, mx, my, mz, wc);
            wtri(mx + ca * 0.04f, my, mz + sa * 0.04f, mx - ca * 0.08f, my, mz - sa * 0.08f, tx, ty, tz, wt);
        }
        gfx_blend(BL_ADD);
        eyes(x + ca * 0.13f, y + 0.06f, z + sa * 0.13f, a, 0.04f, 0.035f, 0.025f, 0xFFF0A0FF);
        gfx_blend(BL_ALPHA);
        break;
    }
    case EN_SHIP: {
        /* a pirate ship: dark hull, raised stern, a mast with patched sails filling in the gale, the black flag */
        float rock = sinf(T * 1.3f + x * 0.7f) * 0.03f, bob = sinf(T * 1.7f + z) * 0.03f, px = -sa, pz = ca;
        uint32_t hull = en_tint(e, 0x5A3A22FF), hull2 = scale_rgb(hull, 0.7f), deck = en_tint(e, 0x9A7448FF);
        if (e->armor_t > 0) { hull = col_mix(hull, 0xFFF0D0FF, 0.4f); deck = col_mix(deck, 0xFFF0D0FF, 0.3f); }
        r_obox(x, -0.06f + bob, z, 0.5f, 0.22f, 0.2f + bob, a, deck, hull2);                                        /* hull */
        r_obox(x + ca * 0.5f, -0.04f + bob + rock, z + sa * 0.5f, 0.12f, 0.15f, 0.22f + bob + rock, a + 0.785f, deck, hull2);   /* bow */
        r_beam(x + ca * 0.58f, 0.2f + bob + rock, z + sa * 0.58f, ca, 0.35f, sa, 0.28f, 0.03f, 0.03f, 0x4A3018FF);   /* bowsprit */
        r_obox(x - ca * 0.4f, 0.2f + bob - rock, z - sa * 0.4f, 0.13f, 0.21f, 0.36f + bob - rock, a, deck, hull);    /* stern castle */
        r_obox(x, 0.2f + bob, z, 0.48f, 0.225f, 0.24f + bob, a, hull, hull);                                       /* rail */
        r_obox(x, 0.19f + bob, z, 0.44f, 0.18f, 0.205f + bob, a, deck, deck);
        for (int s2 = -1; s2 <= 1; s2 += 2)                                                                        /* gun ports */
            for (int k = -1; k <= 1; k++)
                r_obox(x + px * 0.222f * (float)s2 + ca * 0.18f * (float)k, 0.08f + bob, z + pz * 0.222f * (float)s2 + sa * 0.18f * (float)k, 0.04f, 0.006f, 0.13f + bob, a, 0x181010FF, 0x181010FF);
        float mx = x + ca * 0.05f, mz = z + sa * 0.05f, top = 1.25f + bob;
        r_beam(mx, 0.2f + bob, mz, ca * rock, 1, sa * rock, 1.08f, 0.04f, 0.04f, 0x4A3018FF);                         /* mast */
        float lean = rock * 1.05f;
        r_beam(mx + px * 0.3f + ca * lean * 0.75f, 0.92f + bob, mz + pz * 0.3f + sa * lean * 0.75f, -px, 0, -pz, 0.6f, 0.025f, 0.025f, 0x4A3018FF);   /* yards */
        r_beam(mx + px * 0.26f + ca * lean * 0.45f, 0.55f + bob, mz + pz * 0.26f + sa * lean * 0.45f, -px, 0, -pz, 0.52f, 0.025f, 0.025f, 0x4A3018FF);
        float fill = 0.12f + 0.06f * sinf(T * 1.1f + x) + G->gust * 0.1f;
        uint32_t sail = lt_shade(0xD8CCB0FF, mx, 0.8f, mz, 0.95f), sail2 = lt_shade(0xC4B898FF, mx, 0.6f, mz, 0.85f);
        for (int k = 0; k < 2; k++) {                                                                              /* two billowing sails */
            float y1 = k ? 0.9f : 0.53f, y0 = k ? 0.6f : 0.26f, w = k ? 0.28f : 0.25f;
            float cx0 = mx + ca * (lean * y1 * 0.8f), cz0 = mz + sa * (lean * y1 * 0.8f), bx2 = mx + ca * (fill + lean * y0 * 0.8f), bz2 = mz + sa * (fill + lean * y0 * 0.8f);
            uint32_t sc = k ? sail : sail2;
            wtri(cx0 + px * w, y1 + bob, cz0 + pz * w, cx0 - px * w, y1 + bob, cz0 - pz * w, bx2 + px * w * 0.9f, y0 + bob, bz2 + pz * w * 0.9f, sc);
            wtri(cx0 - px * w, y1 + bob, cz0 - pz * w, bx2 - px * w * 0.9f, y0 + bob, bz2 - pz * w * 0.9f, bx2 + px * w * 0.9f, y0 + bob, bz2 + pz * w * 0.9f, sc);
            if (k == 1) {                                                                                         /* skull on the top sail */
                float sx2 = (cx0 + bx2) * 0.5f + ca * 0.02f, sz2 = (cz0 + bz2) * 0.5f + sa * 0.02f;
                uint32_t sk = lt_shade(0x202020FF, sx2, 0.75f, sz2, 1.0f);
                wtri(sx2 + px * 0.06f, 0.8f + bob, sz2 + pz * 0.06f, sx2 - px * 0.06f, 0.8f + bob, sz2 - pz * 0.06f, sx2, 0.7f + bob, sz2, sk);
            }
        }
        float fl = sinf(T * 7.0f + x) * 0.04f;                                                                      /* the black flag streaming */
        float tx = mx + ca * lean, tz = mz + sa * lean;
        uint32_t flag = lt_shade(0x141414FF, tx, top, tz, 0.9f);
        wtri(tx, top, tz, tx, top - 0.12f, tz, tx - ca * 0.24f + px * fl, top - 0.05f, tz - sa * 0.24f + pz * fl, flag);
        gfx_blend(BL_ADD);
        float charge = clampf(1.0f - e->pulse_t / 0.8f, 0, 1);                                                     /* fuses lit just before a broadside */
        if (charge > 0)
            for (int s2 = -1; s2 <= 1; s2 += 2) for (int k = -1; k <= 1; k++)
                r_glow_cheap(x + px * 0.24f * (float)s2 + ca * 0.18f * (float)k, 0.11f + bob, z + pz * 0.24f * (float)s2 + sa * 0.18f * (float)k, 0.08f + 0.06f * charge, RGBA(255, 170, 60, (int)(220 * charge)));
        r_glow(x - ca * 0.52f, 0.34f + bob, z - sa * 0.52f, 0.35f, RGBA(255, 170, 70, 120));                           /* stern lantern */
        r_glow_cheap(x - ca * 0.52f, 0.34f + bob, z - sa * 0.52f, 0.06f, RGBA(255, 230, 160, 255));
        gfx_blend(BL_ALPHA);
        break;
    }
    case EN_KRAKEN: {
        /* the Kraken: a vast head heaving out of the sea, tentacles curling up and slapping down around it */
        float sub = e->dash_t > 0 ? 1.0f : 0.0f;
        float k = 1.0f, heave = sinf(T * 1.1f) * 0.05f;
        uint32_t skin = en_tint(e, 0x7E3A72FF), skin2 = scale_rgb(skin, 0.78f), sucker = 0xE8B8C8FF;
        if (e->armor_t > 0) skin = col_mix(skin, 0xFFE0E8FF, 0.4f);
        if (sub > 0) {
            /* gone under: a churning dark mass, bubbles, and a couple of tentacle tips breaking the surface */
            r_gdisc(x, z, 0.008f, 1.0f, 16, 0x08101C90, 0x08101C00);
            for (int t2 = 0; t2 < 3; t2++) {
                float an = a + (float)t2 * 2.1f + T * 0.3f, rr = 0.7f;
                float bx = x + cosf(an) * rr, bz = z + sinf(an) * rr, w = sinf(T * 2.0f + (float)t2) * 0.1f;
                r_stick(bx, -0.05f, bz, bx + cosf(an) * 0.12f, 0.25f + w, bz + sinf(an) * 0.12f, 0.06f, 0.015f, skin2);
            }
            gfx_blend(BL_ADD);
            for (int b = 0; b < 6; b++) {
                float ph = fmodf(T * 1.2f + (float)b * 0.17f, 1.0f), an = (float)b * 1.05f;
                r_gring(x + cosf(an) * 0.4f, z + sinf(an) * 0.4f, 0.01f, 0.05f + 0.25f * ph, 0.02f, 10, RGBA(190, 220, 240, (int)(110 * (1.0f - ph))));
            }
            gfx_blend(BL_ALPHA);
            break;
        }
        /* tentacles: eight thick arms arching up out of the sea and back down, curling at the tips */
        for (int t2 = 0; t2 < 8; t2++) {
            float an = a + 0.4f + (float)t2 * 0.785f + 0.15f * sinf(T * 0.7f + (float)t2);
            float ph = T * 1.3f + (float)t2 * 1.7f, lift = 0.55f + 0.45f * sinf(ph);
            float dx = cosf(an), dz = sinf(an);
            float px0 = x + dx * 0.4f, pz0 = z + dz * 0.4f, py0 = -0.03f, w = 0.17f;
            const int NS = 6;
            for (int sg = 1; sg <= NS; sg++) {
                float u = (float)sg / (float)NS;
                float side = sinf(ph * 0.8f + u * 3.0f) * 0.25f * u;                /* sweeps sideways as it writhes */
                float rad = 0.4f + 0.95f * u + (u > 0.8f ? -(u - 0.8f) * 1.2f * lift : 0.0f);
                float nx = x + dx * rad - dz * side, nz = z + dz * rad + dx * side;
                float ny = sinf(u * 3.14159f) * 0.75f * lift + (u > 0.8f ? (u - 0.8f) * 1.5f * lift : 0.0f) - 0.03f;
                float w2 = 0.17f * (1.0f - u * 0.82f);
                r_stick(px0, py0, pz0, nx, ny, nz, w, w2, (sg & 1) ? skin : skin2);
                if (sg <= 4) r_crystal(nx, ny - w2 * 0.6f, nz, w2 * 0.45f, w2 * 0.15f, w2 * 0.15f, 5, an, sucker);
                px0 = nx; pz0 = nz; py0 = ny; w = w2;
            }
            if (lift < 0.2f) r_gring(x + dx * 1.2f, z + dz * 1.2f, 0.01f, 0.2f, 0.04f, 10, RGBA(220, 235, 245, 120));   /* slapping the water */
        }
        /* the head: a great mottled dome with a ridge, rising and falling with its breath */
        r_crystal(x, 0.15f + heave, z, 0.55f * k, 0.25f * k, 0.15f * k, 10, a, skin2);
        r_crystal(x - ca * 0.15f, 0.42f + heave, z - sa * 0.15f, 0.45f * k, 0.55f * k, 0.25f * k, 9, a, skin);
        r_crystal(x - ca * 0.28f, 0.95f + heave, z - sa * 0.28f, 0.2f * k, 0.18f * k, 0.12f * k, 7, a, col_mix(skin, 0xFFC0D0FF, 0.15f));
        for (int s2 = -1; s2 <= 1; s2 += 2)                                                                       /* eye ridges */
            r_crystal(x + ca * 0.24f - sa * 0.2f * (float)s2, 0.52f + heave, z + sa * 0.24f + ca * 0.2f * (float)s2, 0.11f, 0.06f, 0.05f, 6, a, skin2);
        gfx_blend(BL_ADD);
        float charge = clampf(1.0f - e->spawn_t / 1.5f, 0, 1);                                                     /* eyes flare before it calls its sharks */
        eyes(x, 0.5f + heave, z, a, 0.33f, 0.2f, 0.08f + 0.05f * charge, RGBA(255, 210, 60, 255));
        r_glow(x + ca * 0.3f, 0.5f + heave, z + sa * 0.3f, 0.6f + 0.6f * charge, RGBA(255, 180, 60, (int)(60 + 100 * charge)));
        gfx_blend(BL_ALPHA);
        break;
    }
    }
}

/* ------------------------------------------------------------------ */
/* the world, back to front                                            */
/* ------------------------------------------------------------------ */
static int col_order[MAXW + 12];
static int built_level = -1;

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
/* frame the whole map: levels 1-4 are 19 x 13, bigger maps get a camera that sits further back */
static void level_camera(void)
{
    float k = fminf(19.0f / (float)MW, 13.0f / (float)MH);
    cam_config((float)MW * 0.5f, (float)MH * 0.5f + 0.05f, 35.0f * k);
}

static void build_static(void)
{
    level_camera();
    cam_world(0, 0);
    sort_columns();
    lt_begin();
    lt_build(0, 1);
    rc_begin();
    nlant = njack = nprop = 0;
    rc_slot(SLOT_GROUND);
    build_ground();
    if (AUTUMN) build_autumn_ground();
    if (ISLAND) build_island_ground();
    if (PIRATE) build_pirate_ground();
    if (FUTURE) build_future_ground();
    if (SKYLV) build_sky_ground();
    if (ISLAND)          /* brass gears on a few wall fronts */
        for (int r = 0; r < MH; r++) for (int c = 0; c < MW; c++)
            if (G->tile[r][c].kind == TL_WALL && !solid(c, r + 1) && hf(c, r, 120) < 0.16f && nprop < MAX_PROP) {
                Prop *p = &prop[nprop++];
                p->kind = PR_GEAR; p->x = (float)c + 0.3f + 0.4f * hf(c, r, 121); p->z = (float)r + 1.01f; p->y = 0.25f;
                p->ph = hf(c, r, 122) * 6.28f; p->row = (uint8_t)(r - ROW_MIN);
            }
    for (int r = ROW_MIN; r < ROW_MAX; r++) {
        rc_slot(SLOT_ROW0 + r - ROW_MIN);
        if (PIRATE && r == FIRE_R - 1) draw_island_static();
        for (int i = 0; i < MW + 12; i++) {
            int c = col_order[i];
            if (!in_map(c, r)) {
                if (outside_lane(c, r)) continue;
                if (SKYLV) {                                   /* a sea of clouds, puffs heaped up here and there */
                    if (hf(c, r, 2) < 0.4f) draw_cloud_puff((float)c + 0.5f, (float)r + 0.5f, 0.7f + 0.8f * hf(c, r, 3) * (r >= MH ? 0.4f : 1.0f), hf(c, r, 4) * 6.0f);
                    continue;
                }
                if (FUTURE) {                                  /* the town beyond the square */
                    if (hf(c, r, 2) < 0.7f) draw_building(c, r);
                    continue;
                }
                if (PIRATE) {                                  /* the open sea: a rock here and there */
                    if (hf(c, r, 2) < 0.06f && (c < -1 || c > MW || r < -1 || r > MH))
                        draw_sea_rock((float)c + 0.5f, (float)r + 0.5f, 0.8f + 0.5f * hf(c, r, 3), hf(c, r, 4) * 6.0f);
                    continue;
                }
                if (ISLAND) {                                  /* open sea: now and then a rocky islet with a pine */
                    if (hf(c, r, 2) < 0.05f && (c < -1 || c > MW || r < -1 || r > MH)) {
                        r_light_model((float)c + 0.5f, 0.2f, (float)r + 0.5f);
                        r_crystal((float)c + 0.5f, 0.05f, (float)r + 0.5f, 0.45f, 0.25f, 0.1f, 6, hf(c, r, 3) * 3.0f, 0x7A7A6EFF);
                        r_light_off();
                        draw_tree(c, r);
                    }
                    continue;
                }
                if (hf(c, r, 2) < 0.72f) draw_tree(c, r);
                else if (hf(c, r, 2) < 0.85f) draw_bush(c, r);
                continue;
            }
            const Tile *t = &G->tile[r][c];
            switch (t->kind) {
            case TL_TREE:
                if (SKYLV) { draw_cloud_puff((float)c + 0.5f, (float)r + 0.5f, r >= MH - 1 ? 0.55f : 0.9f + 0.4f * hf(c, r, 3), hf(c, r, 4) * 6.0f); break; }
                if (FUTURE) { draw_building(c, r); break; }
                if (PIRATE) { if (hf(c, r, 2) < 0.55f) draw_sea_rock((float)c + 0.5f + (hf(c, r, 5) - 0.5f) * 0.3f, (float)r + 0.5f, 0.6f + 0.5f * hf(c, r, 3), hf(c, r, 4) * 6.0f); break; }
                draw_tree(c, r); break;
            case TL_GRAVE:
                if (PIRATE) { if (t->color == DECO_WRECK) draw_wreck(c, r); break; }
                if (FUTURE) { if (t->color == DECO_CLOCK) draw_clock_tower(c, r); else draw_lamp_post(c, r); break; }
                if (SKYLV) { draw_star_post(c, r); break; }
                if (ISLAND) { draw_island_prop(c, r, t); break; }
                draw_grave((float)c + 0.5f + (hf(c, r, 4) - 0.5f) * 0.25f, (float)r + 0.45f, hf(c, r, 5), (hf(c, r, 6) - 0.5f) * 0.4f, hf(c, r, 3) < 0.3f);
                break;
            case TL_WALL: draw_wall(c, r, t); break;
            case TL_BLOCK: draw_block(c, r, t); break;
            default: break;
            }
        }
    }
    rc_end();
    built_level = G->level;
}

/* swinging lanterns and flickering pumpkin faces belonging to this row */
static void draw_row_props(int row)
{
    for (int i = 0; i < nprop; i++) {
        const Prop *p = &prop[i];
        if (p->row != row) continue;
        float x = p->x, z = p->z;
        if (p->kind == PR_CLOCK) {
            /* the clock face, stopped at 10:04, glowing; crackling blue when the lightning hits */
            float fz = z + 0.365f, cy = p->y, R = 0.24f;
            float k = clampf(G->giga_fx / 2.0f, 0, 1);
            float sx, sy;
            proj(x, cy, fz, &sx, &sy);
            float pr = R * proj_scale(cy, fz);
            gfx_poly(sx, sy, pr * 1.12f, 16, 0, lt_shade(0x6A5A40FF, x, cy, fz, 1.0f));
            gfx_poly(sx, sy, pr, 16, 0, col_mix(RGBA(250, 238, 200, 255), RGBA(200, 230, 255, 255), k));
            for (int h = 0; h < 12; h++) {
                float a = (float)h / 12.0f * 6.2832f;
                gfx_rect(sx + sinf(a) * pr * 0.82f - 1, sy - cosf(a) * pr * 0.82f - 1, 2, 2, 0x302820FF);
            }
            float ha = (10.0f + 4.0f / 60.0f) / 12.0f * 6.2832f, ma = 4.0f / 60.0f * 6.2832f;
            gfx_line(sx, sy, sx + sinf(ha) * pr * 0.5f, sy - cosf(ha) * pr * 0.5f, 2.6f, 0x201810FF, 0x201810FF);
            gfx_line(sx, sy, sx + sinf(ma) * pr * 0.78f, sy - cosf(ma) * pr * 0.78f, 1.8f, 0x201810FF, 0x201810FF);
            gfx_blend(BL_ADD);
            r_glow(x, cy, fz, 0.5f + 1.5f * k, RGBA(255, 230, 170, 60));
            if (k > 0) {
                r_glow(x, 2.6f, z, 1.6f * k, RGBA(150, 200, 255, (int)(200 * k)));
                for (int a2 = 0; a2 < 3; a2++) {                  /* electricity crawling over the tower */
                    float y0 = 0.4f + fmodf(T * 3.0f + (float)a2 * 0.7f, 2.2f), j = sinf(T * 40.0f + (float)a2) * 0.2f;
                    r_line(x - 0.4f, y0, z + 0.37f, x + j, y0 + 0.2f, z + 0.37f, 1.6f, RGBA(180, 220, 255, (int)(255 * k)), RGBA(180, 220, 255, 0));
                }
            }
            gfx_blend(BL_ALPHA);
            continue;
        }
        if (SKYLV && p->kind == PR_LAMP) {
            /* a star on a post, slowly turning and twinkling */
            float k = 0.8f + 0.2f * sinf(T * 3.0f + p->ph), spin = T * 1.2f + p->ph;
            float sx, sy;
            proj(x, p->y, z, &sx, &sy);
            float R = 0.13f * proj_scale(p->y, z) * k, rr = R * 0.45f;
            uint32_t sc = RGBA(255, 230, 120, 255);
            for (int i = 0; i < 5; i++) {
                float a0 = spin + (float)i * 1.2566f, a1 = a0 + 0.6283f, a2 = a0 - 0.6283f;
                gfx_tri(sx, sy, sc, sx + cosf(a0) * R, sy + sinf(a0) * R, sc, sx + cosf(a1) * rr, sy + sinf(a1) * rr, sc);
                gfx_tri(sx, sy, sc, sx + cosf(a0) * R, sy + sinf(a0) * R, sc, sx + cosf(a2) * rr, sy + sinf(a2) * rr, sc);
            }
            gfx_blend(BL_ADD);
            r_glow(x, p->y, z, 0.5f * k, RGBA(255, 160, 230, 110));
            gfx_blend(BL_ALPHA);
            continue;
        }
        if (p->kind == PR_BALLOON) {
            /* a hot-air balloon drifting up and down off the edge of the kingdom */
            float bob = sinf(T * 0.4f + p->ph) * 0.4f, by = 2.2f + bob, bx = x + sinf(T * 0.15f + p->ph) * 0.6f;
            static const uint32_t BC[4] = { 0xFF80B0FF, 0x80C8FFFF, 0xFFD060FF, 0xB090FFFF };
            uint32_t c1 = BC[(int)p->y & 3], c2 = col_mix(c1, 0xFFFFFFFF, 0.5f);
            r_light_model(bx, by, z + 0.5f);
            r_crystal(bx, by, z, 0.45f, 0.42f, 0.45f, 8, T * 0.2f, c1);
            r_prism(bx, by - 0.2f, z, 0.46f, 0.46f, by + 0.05f, 8, T * 0.2f + 0.39f, c2, c2);
            r_box(bx - 0.1f, by - 0.85f, z - 0.1f, bx + 0.1f, by - 0.68f, z + 0.1f, 0x8A6040FF, 0x6A4830FF, 0);
            r_light_off();
            for (int k = 0; k < 4; k++) {
                float ox = (k & 1) ? 0.09f : -0.09f, oz = (k & 2) ? 0.09f : -0.09f;
                r_line(bx + ox, by - 0.68f, z + oz, bx + ox * 3.0f, by - 0.4f, z + oz * 3.0f, 1.0f, 0x504030FF, 0x504030FF);
            }
            continue;
        }
        if (SKYLV && p->kind == PR_PORTAL) {
            /* a rainbow arch over the road, sparkling */
            int along_x = p->y > 0.5f;
            static const uint32_t RB[6] = { 0xFF5060FF, 0xFFA040FF, 0xFFF060FF, 0x60F070FF, 0x60A0FFFF, 0xC070FFFF };
            gfx_blend(BL_ADD);
            for (int band = 0; band < 6; band++) {
                float rr = 0.75f - 0.07f * (float)band, ox = 0, oy = 0, oz = 0;
                for (int k = 0; k <= 10; k++) {
                    float a = (float)k / 10.0f * 3.14159f, u = cosf(a) * rr, vv = sinf(a) * rr;
                    float px = along_x ? x : x + u, pz = along_x ? z + u : z, py = vv;
                    if (k) r_line(ox, oy, oz, px, py, pz, 3.0f, col_a(RB[band], 0.8f), col_a(RB[band], 0.8f));
                    ox = px; oy = py; oz = pz;
                }
            }
            for (int k = 0; k < 3; k++) {
                float a = fmodf(T * 0.8f + (float)k * 0.33f + p->ph, 1.0f) * 3.14159f;
                float u = cosf(a) * 0.6f, vv = sinf(a) * 0.6f;
                r_glow_cheap(along_x ? x : x + u, vv, along_x ? z + u : z, 0.07f, RGBA(255, 255, 230, 230));
            }
            r_glow(x, 0.3f, z, 0.8f, RGBA(255, 180, 240, 50));
            gfx_blend(BL_ALPHA);
            continue;
        }
        if (p->kind == PR_LAMP) {
            float k = flicker(p->ph);
            r_light_model(x, p->y, z);
            r_prism(x, p->y - 0.04f, z, 0.12f, 0.06f, p->y + 0.06f, 6, 0, 0x404048FF, 0x303038FF);
            r_light_off();
            r_unlit = 1;
            r_prism(x, p->y - 0.12f, z, 0.08f, 0.1f, p->y - 0.04f, 6, 0, RGBA(255, 230, 170, 255), RGBA((int)(255 * k), (int)(210 * k), 140, 255));
            r_unlit = 0;
            gfx_blend(BL_ADD);
            r_glow(x, p->y - 0.1f, z, 0.6f * k, RGBA(255, 200, 120, 110));
            gfx_blend(BL_ALPHA);
            continue;
        }
        if (p->kind == PR_PORTAL) {
            /* a time portal: two swirling rings standing across the road, crackling blue-white */
            int along_x = p->y > 0.5f;
            float cyy = 0.6f, R = 0.55f;
            gfx_blend(BL_ADD);
            for (int ring2 = 0; ring2 < 2; ring2++) {
                float spin = T * (ring2 ? -2.3f : 1.7f) + p->ph, rr = R * (ring2 ? 0.75f : 1.0f);
                float ox = 0, oy = 0, oz = 0;
                for (int k = 0; k <= 16; k++) {
                    float a = (float)k / 16.0f * 6.2832f + spin, wob = 1.0f + 0.08f * sinf(a * 3.0f + T * 6.0f);
                    float u = cosf(a) * rr * wob, vv = sinf(a) * rr * wob;
                    float px = along_x ? x : x + u, pz = along_x ? z + u : z, py = cyy + vv;
                    if (k) {
                        float br = 0.6f + 0.4f * sinf(a * 2.0f + T * 5.0f);
                        r_line(ox, oy, oz, px, py, pz, ring2 ? 1.6f : 2.4f, RGBA(120, 190, 255, (int)(220 * br)), RGBA(200, 240, 255, (int)(220 * br)));
                    }
                    ox = px; oy = py; oz = pz;
                }
            }
            r_glow(x, cyy, z, 0.9f, RGBA(90, 150, 255, (int)(70 + 30 * sinf(T * 4.0f + p->ph))));
            r_glow_cheap(x, cyy, z, 0.25f, RGBA(220, 240, 255, 200));
            gfx_blend(BL_ALPHA);
            continue;
        }
        if (p->kind == PR_NEON) {
            /* a neon sign on a shop front: a glowing tube rectangle, flickering now and then */
            uint32_t c = BLOCK_COL[(int)p->y];
            int off = fmodf(T * 0.7f + p->ph, 7.0f) < 0.25f && sinf(T * 60.0f) > 0;
            if (off) continue;
            gfx_blend(BL_ADD);
            float y0 = 0.5f, y1 = 0.72f, w = 0.34f;
            uint32_t cc = col_a(col_mix(c, 0xFFFFFFFF, 0.3f), 0.95f);
            r_line(x - w, y0, z, x + w, y0, z, 2.0f, cc, cc); r_line(x - w, y1, z, x + w, y1, z, 2.0f, cc, cc);
            r_line(x - w, y0, z, x - w, y1, z, 2.0f, cc, cc); r_line(x + w, y0, z, x + w, y1, z, 2.0f, cc, cc);
            r_line(x - w * 0.6f, (y0 + y1) * 0.5f, z, x + w * 0.6f, (y0 + y1) * 0.5f, z, 2.0f, cc, cc);
            r_glow(x, (y0 + y1) * 0.5f, z, 0.55f, col_a(c, 0.35f));
            gfx_blend(BL_ALPHA);
            continue;
        }
        if (p->kind == PR_BUOY || p->kind == PR_BARREL) {
            /* bobbing and tipping in the swell */
            float bob = sinf(T * 2.0f + p->ph) * 0.05f, tip = sinf(T * 1.6f + p->ph * 1.3f) * 0.12f;
            r_light_model(x, 0.3f, z);
            if (p->kind == PR_BARREL) {
                r_prism(x, -0.06f + bob, z, 0.13f, 0.15f, 0.06f + bob, 8, p->ph, 0x7A5432FF, 0x5A3C22FF);
                r_prism(x, 0.06f + bob, z, 0.15f, 0.13f, 0.16f + bob, 8, p->ph, 0x7A5432FF, 0x5A3C22FF);
                r_light_off();
                continue;
            }
            r_prism(x, -0.05f + bob, z, 0.16f, 0.12f, 0.12f + bob, 8, 0, 0xB83A2AFF, 0x8A2A1EFF);
            r_stick(x, 0.12f + bob, z, x + tip * 0.3f, 0.55f + bob, z, 0.03f, 0.02f, 0x2A2A2EFF);
            r_light_off();
            float lx = x + tip * 0.3f, ly = 0.6f + bob, k = flicker(p->ph);
            r_unlit = 1;
            r_prism(lx, ly - 0.06f, z, 0.05f, 0.05f, ly + 0.06f, 4, 0.785f, RGBA(255, 200, 110, 255), RGBA((int)(255 * k), (int)(170 * k), 70, 255));
            r_unlit = 0;
            gfx_blend(BL_ADD);
            r_glow(lx, ly, z, 0.55f * k, RGBA(255, 150, 60, 140));
            r_glow_cheap(lx, ly, z, 0.14f, RGBA(255, 230, 160, 220));
            gfx_blend(BL_ALPHA);
            continue;
        }
        if (p->kind == PR_RUNE) {
            /* runes carved down the front face, glowing in a slow breathing rhythm */
            float k = 0.55f + 0.45f * sinf(T * 1.3f + p->ph);
            gfx_blend(BL_ADD);
            uint32_t rc = RGBA(110, 210, 255, (int)(220 * k));
            float fz = z + 0.2f;
            for (int j = 0; j < 5; j++) {
                float y = 0.25f + 0.2f * (float)j, w = 0.08f;
                if ((j + (int)(p->ph * 3.0f)) & 1) { r_line(x - w, y, fz, x + w, y + 0.06f, fz, 1.6f, rc, rc); r_line(x + w, y, fz, x - w, y + 0.06f, fz, 1.6f, rc, rc); }
                else { r_line(x, y - 0.02f, fz, x, y + 0.08f, fz, 1.6f, rc, rc); r_line(x - w, y + 0.03f, fz, x + w, y + 0.03f, fz, 1.6f, rc, rc); }
            }
            r_glow(x, 0.7f, fz, 0.7f, RGBA(90, 180, 255, (int)(70 * k)));
            gfx_blend(BL_ALPHA);
        } else if (p->kind == PR_ORRERY) {
            /* three tilted brass rings turning at different speeds, a little sun in the middle */
            float cy = 0.9f;
            r_light_model(x, cy, z);
            r_crystal(x, cy, z, 0.1f, 0.1f, 0.1f, 6, T, 0xF0C060FF);
            for (int k = 0; k < 3; k++) {
                float rad = 0.24f + 0.11f * (float)k, spin = T * (0.6f - 0.18f * (float)k) + p->ph + (float)k, tilt = 0.5f + 0.35f * (float)k;
                float px = 0, py = 0, pz = 0, ox = 0, oy = 0, oz = 0;
                for (int s2 = 0; s2 <= 16; s2++) {
                    float a = (float)s2 / 16.0f * 6.2832f;
                    float lx = cosf(a) * rad, ly = sinf(a) * rad * sinf(tilt), lz = sinf(a) * rad * cosf(tilt);
                    float rx = lx * cosf(spin) - lz * sinf(spin), rz = lx * sinf(spin) + lz * cosf(spin);
                    px = x + rx; py = cy + ly; pz = z + rz;
                    if (s2) r_line(ox, oy, oz, px, py, pz, 2.6f, lt_shade(0xD8A850FF, x, cy, z, 0.9f), lt_shade(0xD8A850FF, x, cy, z, 0.9f));
                    ox = px; oy = py; oz = pz;
                }
                float pa = T * (1.2f - 0.3f * (float)k) + p->ph * (float)(k + 1);      /* a planet riding each ring */
                float lx = cosf(pa) * rad, ly = sinf(pa) * rad * sinf(tilt), lz = sinf(pa) * rad * cosf(tilt);
                r_crystal(x + lx * cosf(spin) - lz * sinf(spin), cy + ly, z + lx * sinf(spin) + lz * cosf(spin), 0.05f, 0.05f, 0.05f, 4, T, k == 1 ? 0x60A0E0FF : 0xC0705AFF);
            }
            r_light_off();
            gfx_blend(BL_ADD);
            r_glow_cheap(x, cy, z, 0.18f, RGBA(255, 210, 120, 140));
            gfx_blend(BL_ALPHA);
        } else {
            /* a brass gear turning on the face of a wall, its neighbour meshing the other way */
            for (int g2 = 0; g2 < 2; g2++) {
                float gx = x + (g2 ? 0.17f : 0.0f), gy = p->y + (g2 ? 0.1f : 0.0f), rad = g2 ? 0.07f : 0.1f;
                float rot = (g2 ? -1.43f : 1.0f) * T * 0.8f + p->ph;
                int teeth = g2 ? 7 : 10;
                float sx[34], sy[34];
                int n = teeth * 2;
                for (int k = 0; k <= n; k++) {
                    float a = rot + (float)k / (float)n * 6.2832f, rr = (k & 1) ? rad * 0.78f : rad;
                    proj(gx + cosf(a) * rr, gy + sinf(a) * rr, z, &sx[k], &sy[k]);
                }
                float cxs, cys;
                proj(gx, gy, z, &cxs, &cys);
                uint32_t col = lt_shade(g2 ? 0xA8803CFF : 0xC09448FF, gx, gy, z, 0.8f);
                for (int k = 0; k < n; k++) gfx_tri(cxs, cys, col, sx[k], sy[k], col, sx[k + 1], sy[k + 1], col);
                float hx, hy;
                proj(gx, gy, z + 0.005f, &hx, &hy);
                gfx_poly(hx, hy, rad * 0.25f * proj_scale(gy, z), 6, rot, lt_shade(0x5A4020FF, gx, gy, z, 1.0f));
            }
        }
    }
    for (int i = 0; i < njack; i++) {
        const Jack *j = &jack[i];
        if (j->row != row) continue;
        float ca = cosf(j->ang), sa = sinf(j->ang), fx = j->x + ca * j->r, fz = j->z + sa * j->r;
        float k = 0.75f + 0.25f * sinf(T * 9.0f + j->ph) * sinf(T * 5.3f + j->ph * 2.0f);
        uint32_t glow = RGBA(255, 180, 60, (int)(230 * k));
        float ux = -sa, uz = ca, sx[3], sy[3], pr = j->r;
        gfx_blend(BL_ADD);
        #define JP(i, u, v) proj(fx + ux * (u) * pr, (v) * pr, fz + uz * (u) * pr, &sx[i], &sy[i])
        for (int s = -1; s <= 1; s += 2) {
            JP(0, 0.2f * s, 0.75f); JP(1, 0.5f * s, 0.75f); JP(2, 0.35f * s, 1.0f);
            gfx_tri(sx[0], sy[0], glow, sx[1], sy[1], glow, sx[2], sy[2], glow);
        }
        JP(0, -0.45f, 0.45f); JP(1, 0.45f, 0.45f); JP(2, 0.0f, 0.25f);
        gfx_tri(sx[0], sy[0], glow, sx[1], sy[1], glow, sx[2], sy[2], glow);
        #undef JP
        r_glow(fx, pr * 0.7f, fz, 0.45f, RGBA(255, 140, 40, (int)(110 * k)));
        gfx_blend(BL_ALPHA);
    }
    for (int i = 0; i < nlant; i++) {
        const Lantern *L = &lant[i];
        if (L->row != row) continue;
        /* the hook moves with its branch; the lantern swings like a pendulum underneath */
        float hx = L->x + rc_sway_of(L->pt) * L->amp, hy = L->y, hz = L->z;
        float th = 0.25f * sinf(T * 1.6f + L->ph) + 0.08f * sinf(T * 2.9f + L->ph * 1.7f);
        float tz = 0.10f * sinf(T * 1.2f + L->ph * 0.7f);
        float lx = hx + sinf(th) * L->len, ly = hy - cosf(th) * L->len, lz = hz + tz * L->len;
        r_line(hx, hy, hz, lx, ly, lz, 1.2f, 0x1A1410FF, 0x1A1410FF);
        float k = 0.85f + 0.15f * sinf(T * 11.0f + L->ph) * sinf(T * 7.1f + L->ph * 3.0f);
        r_light_model(lx, ly, lz);
        r_prism(lx, ly - 0.04f, lz, 0.065f, 0.0f, ly + 0.03f, 4, th, 0x2A2420FF, 0x221C18FF);        /* cap */
        r_light_off();
        r_unlit = 1;
        r_prism(lx, ly - 0.2f, lz, 0.065f, 0.065f, ly - 0.04f, 4, th + 0.785f, RGBA(255, 200, 110, 255), RGBA((int)(255 * k), (int)(170 * k), 70, 255));
        r_unlit = 0;
        gfx_blend(BL_ADD);
        r_glow(lx, ly - 0.12f, lz, 0.6f * k, RGBA(255, 150, 60, 140));
        r_glow_cheap(lx, ly - 0.12f, lz, 0.18f, RGBA(255, 230, 160, 220));
        gfx_blend(BL_ALPHA);
    }
}

static void draw_world(void)
{
    int ghost = G->state == ST_PLAY && can_build(G, G->cx, G->cz);
    for (int r = ROW_MIN; r < ROW_MAX; r++) {
        rc_draw(SLOT_ROW0 + r - ROW_MIN, SHX, SHY);
        if (HALLOW || ISLAND || PIRATE || FUTURE || SKYLV) draw_row_props(r - ROW_MIN);
        if (r >= 0 && r < MH)
            for (int i = 0; i < MW + 12; i++) {
                int c = col_order[i];
                if (!in_map(c, r)) continue;
                const Tile *t = &G->tile[r][c];
                if (t->torch && G->tw[r][c].type < 0) draw_torch(c, r, t);
                if (SKYLV) { if (c == FIRE_C && r == FIRE_R + 1) draw_castle(); }
                else if (c == FIRE_C && r == FIRE_R) { if (PIRATE) draw_treasure(); else if (FUTURE) draw_delorean(); else draw_campfire(); }
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
static void draw_effects_inner(void);
static void draw_effects(void)
{
    r_glow_k = 0.6f;                     /* weapon flashes, beams and bolts: bright, but not blinding when towers bunch up */
    draw_effects_inner();
    r_glow_k = 1.0f;
}
static void draw_effects_inner(void)
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
            /* where it will land: a red ring closing in */
            float u = s->t / s->dur, rr = s->splash * (1.3f - 0.6f * u);
            gfx_blend(BL_ADD);
            uint32_t mc = RGBA(255, 70, 40, (int)(60 + 120 * u * (0.6f + 0.4f * sinf(T * 30.0f))));
            r_gring(s->ex, s->ez, 0.02f, rr, 0.04f, 14, mc);
            r_line(s->ex - rr * 0.4f, 0.02f, s->ez, s->ex + rr * 0.4f, 0.02f, s->ez, 1.5f, mc, mc);
            r_line(s->ex, 0.02f, s->ez - rr * 0.4f, s->ex, 0.02f, s->ez + rr * 0.4f, 1.5f, mc, mc);
            gfx_blend(BL_ALPHA);
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
            r_gring(p->x, p->z, p->y, rr, 0.06f + 0.06f * k, rr > 1.2f ? 28 : 18, col_a(p->c, k));
            break;
        }
        case PT_SPLASH: {
            float u = 1.0f - k;
            r_gring(p->x, p->z, p->y, p->size * (0.3f + u), 0.025f, 10, col_a(p->c, k));
            break;
        }
        case PT_FIRE: {
            uint32_t c = col_mix(0xFFE8A0FF, col_mix(0xFF7020FF, 0x602010FF, fmaxf(0, 0.6f - k) / 0.6f), 1.0f - k);
            r_glow_cheap(p->x, p->y, p->z, p->size, col_a(c, k * 0.9f));
            break;
        }
        case PT_SHARD:
            r_unlit = 1;
            r_alpha = k;
            r_crystal(p->x, p->y, p->z, p->size * 0.4f, p->size, p->size, 4, p->rot, p->c);
            r_alpha = 1.0f;
            r_unlit = 0;
            break;
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
    /* prism beams: thicker and whiter as the lock builds */
    for (int r = 0; r < MH; r++)
        for (int c = 0; c < MW; c++) {
            const Tower *t = &G->tw[r][c];
            if (t->type != TW_PRISM || t->target < 0 || t->built < 1.0f || t->stun_t > 0) continue;
            const Enemy *e = &G->en[t->target];
            if (!e->alive) continue;
            float k = clampf(t->lock_t / 2.5f, 0, 1), h = G->tile[r][c].h;
            float x0 = (float)c + 0.5f, y0 = h + 0.79f * TOWER_K, z0 = (float)r + 0.5f;
            float x1 = e->x, y1 = e->y + ENEMIES[e->type].size * 0.7f * ENEMY_K, z1 = e->z;
            float flick = 0.85f + 0.15f * sinf(T * 50.0f + (float)c);
            uint32_t outer = col_mix(0xFF60F0FF, 0xFFA8FFFF, k), inner = col_mix(0xFFA0FFFF, 0xFFE0FFFF, k);
            r_glowline(x0, y0, z0, x1, y1, z1, (8.0f + 14.0f * k) * flick, col_a(outer, 0.55f));
            r_line(x0, y0, z0, x1, y1, z1, 1.5f + 2.5f * k, inner, inner);
            r_glow(x1, y1, z1, 0.25f + 0.3f * k, col_a(outer, 0.8f));
            r_glow_cheap(x0, y0, z0, 0.2f + 0.2f * k, col_a(inner, 0.9f));
        }
    for (int i = 0; i < MAX_ARC; i++) {
        const Arc *a = &G->arc[i];
        if (!a->alive) continue;
        float k = 1.0f - a->t / a->life;
        if (a->storm) k = k * (0.5f + 0.5f * (sinf(a->t * 70.0f) > -0.3f));
        float w = a->storm ? 5.0f : 2.4f;
        r_glow_k = a->storm ? 1.0f : 0.6f;              /* the storm's own lightning stays blinding */
        for (int j = 0; j + 1 < a->n; j++) {
            r_glowline(a->p[j][0], a->p[j][1], a->p[j][2], a->p[j + 1][0], a->p[j + 1][1], a->p[j + 1][2], w * 5.0f, col_a(a->c, k * 0.45f));
            r_line(a->p[j][0], a->p[j][1], a->p[j][2], a->p[j + 1][0], a->p[j + 1][1], a->p[j + 1][2], w, col_a(0xFFFFFFFF, k), col_a(0xFFFFFFFF, k));
        }
        if (!a->storm) r_glow_cheap(a->p[a->n - 1][0], a->p[a->n - 1][1], a->p[a->n - 1][2], 0.3f, col_a(a->c, k * 0.8f));
    }
    gfx_blend(BL_ALPHA);
}

static void draw_leaves(void)
{
    for (int i = 0; i < NUM_LEAVES; i++) {
        const Drop *d = &G->drop[i];
        float sx, sy;
        proj(d->x, d->y, d->z, &sx, &sy);
        if (sx < -20 || sx > VW + 20 || sy < -20 || sy > VH + 20) continue;
        float px = proj_scale(d->y, d->z) * 0.09f;
        float spin = T * (2.0f + (float)(i % 4) * 0.6f) + d->len, flip = cosf(T * 3.1f + d->len * 2.0f);
        float ca = cosf(spin) * px, sa = sinf(spin) * px, w = 0.45f * flip;
        uint32_t c = lt_shade(LEAF_COL[(int)d->len % 5], d->x, d->y, d->z, 0.85f + 0.15f * flip);
        gfx_quad4(sx + ca, sy + sa, c, sx - sa * w, sy + ca * w, c, sx - ca, sy - sa, c, sx + sa * w, sy - ca * w, c);
    }
}

static void draw_rain(void)
{
    if (HALLOW || ISLAND || FUTURE || SKYLV) return;
    if (AUTUMN) { draw_leaves(); return; }
    float f = G->flash;
    uint32_t c = RGBA(170, 190, 235, (int)(60 + 110 * clampf(f, 0, 1)));
    for (int i = 0; i < MAX_DROP; i++) {
        const Drop *d = &G->drop[i];
        float ax, ay, bx, by;
        proj(d->x, d->y, d->z, &ax, &ay);
        proj(d->x - (G->wind + G->gust * 3.0f) * 0.16f * d->len, d->y + d->len, d->z, &bx, &by);
        gfx_line(ax, ay, bx, by, 1.3f, c, c & 0xFFFFFF00);
    }
    /* big soft streaks right in front of the camera */
    uint32_t cn = RGBA(190, 205, 240, (int)(26 + 50 * clampf(f, 0, 1)));
    for (int i = 0; i < 46; i++) {
        float sp = 1.6f + (float)(i % 5) * 0.25f;
        float ph = fmodf(T * sp + (float)i * 0.618f, 1.0f);
        float wd = G->wind + G->gust * 3.0f;
        float x = fmodf((float)i * 97.3f + T * 40.0f * G->wind + G->gust * ph * 120.0f, VW + 80.0f) - 40.0f;
        float y = ph * (VH + 200.0f) - 100.0f;
        float len = 70.0f + (float)(i % 3) * 30.0f;
        gfx_line(x, y, x - wd * 14.0f, y - len, 2.2f, cn, cn & 0xFFFFFF00);
    }
}

/* golden hour: warm haze in the distance, low fog, sunbeams slanting through the trees, floating dust */
static void draw_golden_hour(void)
{
    /* low drifting fog banks */
    for (int i = 0; i < 8; i++) {
        float x = fmodf((float)i * 3.3f + T * 0.18f * (0.6f + 0.15f * (float)i), 27.0f) - 4.0f;
        float z = (float)((i * 5) % 14) - 0.5f;
        r_glow(x, 0.45f, z, 3.6f, RGBA(255, 214, 165, 46));
    }
    /* distance haze (the top of the screen is the far side of the woods) */
    gfx_rect_v(0, 0, VW, 190, RGBA(255, 196, 128, 120), RGBA(255, 196, 128, 0));
    gfx_blend(BL_ADD);
    /* the sun just off the top-left corner */
    gfx_glow(-30, -50, 560, RGBA(255, 170, 80, 80));
    gfx_glow(-30, -50, 220, RGBA(255, 220, 150, 90));
    /* sunbeams */
    for (int i = 0; i < 7; i++) {
        float x0 = -150.0f + (float)i * 150.0f + sinf(T * 0.11f + (float)i * 2.3f) * 24.0f;
        float w0 = 30.0f + (float)((i * 37) % 3) * 26.0f, w1 = w0 * 2.6f;
        float a = 34.0f + 16.0f * sinf(T * 0.37f + (float)i * 1.7f) + 7.0f * sinf(T * 1.1f + (float)i);
        float x1 = x0 + 0.72f * (VH + 60.0f);
        uint32_t top = RGBA(255, 200, 120, (int)a), bot = RGBA(255, 190, 110, 0);
        gfx_quad4(x0 - w0 * 0.5f, -20, top, x0 + w0 * 0.5f, -20, top, x1 + w1 * 0.5f, VH + 40, bot, x1 - w1 * 0.5f, VH + 40, bot);
    }
    /* dust motes catching the light */
    for (int i = 0; i < 46; i++) {
        uint32_t h = (uint32_t)i * 2654435761u;
        float bx = (float)(h % 900), by = (float)((h >> 10) % 520);
        float x = fmodf(bx + T * (6.0f + (float)(i % 5) * 2.0f) + sinf(T * 0.7f + (float)i) * 12.0f, VW + 40.0f) - 20.0f;
        float y = fmodf(by - T * (3.0f + (float)(i % 3) * 1.5f) + VH * 4.0f, VH + 40.0f) - 20.0f;
        float tw = 0.5f + 0.5f * sinf(T * 2.3f + (float)i * 1.9f);
        gfx_glow_cheap(x, y, 2.0f + (float)(i % 3), RGBA(255, 230, 170, (int)(150 * tw)));
    }
    gfx_blend(BL_ALPHA);
    /* soft warm vignette */
    gfx_rect_v(0, VH - 80, VW, 80, 0x1E0C0000, 0x1E0C0090);
    gfx_quad4(0, 0, 0x1E0C0070, 80, 0, 0x1E0C0000, 80, VH, 0x1E0C0000, 0, VH, 0x1E0C0070);
    gfx_quad4(VW - 80, 0, 0x1E0C0000, VW, 0, 0x1E0C0070, VW, VH, 0x1E0C0070, VW - 80, VH, 0x1E0C0000);
}

/* dusk on Halloween: a rising moon, cold moonbeams, and thick fog rolling through and over the course */
static void draw_hallow_night(void)
{
    /* low fog banks hugging the ground */
    for (int i = 0; i < 12; i++) {
        float sp = 0.22f + 0.07f * (float)(i % 4);
        float x = fmodf((float)i * 2.9f + T * sp, 28.0f) - 5.0f;
        float z = (float)((i * 7) % 15) - 1.0f + sinf(T * 0.2f + (float)i) * 0.6f;
        float a = 62.0f + 20.0f * sinf(T * 0.3f + (float)i * 1.3f);
        r_glow(x, 0.3f, z, 3.4f, RGBA(150, 160, 192, (int)a));
    }
    /* a second layer drifting above the towers, slowly thickening and thinning */
    for (int i = 0; i < 12; i++) {
        float sp = 0.35f + 0.08f * (float)(i % 3);
        float x = fmodf((float)i * 3.7f + 11.0f + T * sp, 30.0f) - 6.0f;
        float z = (float)((i * 5) % 14) - 0.5f + cosf(T * 0.17f + (float)i * 2.0f) * 0.8f;
        float a = 30.0f + 34.0f * (0.5f + 0.5f * sinf(T * 0.21f + (float)i * 2.3f));
        r_glow(x, 1.7f, z, 2.8f, RGBA(176, 184, 214, (int)a));
    }
    /* dusk haze at the far edge, purple fading into the dark */
    gfx_rect_v(0, 0, VW, 170, RGBA(120, 80, 140, 120), RGBA(80, 60, 110, 0));
    gfx_blend(BL_ADD);
    /* the moon, top right */
    float mx = VW * 0.73f, my = 34.0f;
    gfx_glow(mx, my, 260, RGBA(110, 130, 200, 60));
    gfx_glow(mx, my, 70, RGBA(200, 210, 255, 90));
    gfx_blend(BL_ALPHA);
    gfx_poly(mx, my, 21, 24, 0, RGBA(236, 238, 222, 255));
    gfx_poly(mx - 6, my - 5, 5, 10, 0, RGBA(200, 202, 190, 255));
    gfx_poly(mx + 7, my + 4, 4, 10, 0, RGBA(205, 207, 195, 255));
    gfx_poly(mx + 2, my - 9, 3, 8, 0, RGBA(210, 212, 200, 255));
    /* thin clouds crossing the moon */
    for (int i = 0; i < 3; i++) {
        float cx = fmodf(T * (6.0f + 3.0f * (float)i) + (float)i * 140.0f, VW + 300.0f) - 150.0f;
        gfx_glow(cx, my + 8.0f * (float)(i - 1), 60.0f + 15.0f * (float)i, RGBA(60, 50, 80, 120));
    }
    gfx_blend(BL_ADD);
    /* moonbeams slanting down from the top right */
    for (int i = 0; i < 5; i++) {
        float x0 = VW * 0.62f + (float)i * 120.0f + sinf(T * 0.09f + (float)i * 2.1f) * 20.0f;
        float w0 = 24.0f + (float)((i * 29) % 3) * 18.0f, w1 = w0 * 2.4f;
        float a = 12.0f + 8.0f * sinf(T * 0.31f + (float)i * 1.9f);
        float x1 = x0 - 0.6f * (VH + 60.0f);
        uint32_t top = RGBA(150, 170, 230, (int)a), bot = RGBA(150, 170, 230, 0);
        gfx_quad4(x0 - w0 * 0.5f, -20, top, x0 + w0 * 0.5f, -20, top, x1 + w1 * 0.5f, VH + 40, bot, x1 - w1 * 0.5f, VH + 40, bot);
    }
    gfx_blend(BL_ALPHA);
    /* all-over mist and a deep purple vignette */
    gfx_rect(0, 0, VW, VH, RGBA(120, 130, 160, 30));
    gfx_rect_v(0, 0, VW, 60, 0x0A061280, 0x0A061200);
    gfx_rect_v(0, VH - 90, VW, 90, 0x0A061200, 0x0A0612C0);
    gfx_quad4(0, 0, 0x0A0612A0, 90, 0, 0x0A061200, 90, VH, 0x0A061200, 0, VH, 0x0A0612A0);
    gfx_quad4(VW - 90, 0, 0x0A061200, VW, 0, 0x0A0612A0, VW, VH, 0x0A0612A0, VW - 90, VH, 0x0A061200);
}

/* sunrise over the island: the sun climbing out of the top of the frame, rays fanning down through
 * the haze, mist on the water that burns off as the morning comes, gulls */
static void draw_island_dawn(void)
{
    float s = G->sun, sx, sy;
    sun_pos(&sx, &sy);
    /* sea mist, thick at first light */
    float mist = 1.0f - s * 0.85f;
    for (int i = 0; i < 10; i++) {
        float x = fmodf((float)i * 3.1f + T * 0.16f, 29.0f) - 5.0f;
        float z = (i & 1) ? -2.5f + (float)(i % 3) * 0.6f : (float)MH + 0.5f + (float)(i % 3) * 0.5f;
        if (i % 4 == 3) z = 3.0f + (float)i;
        r_glow(x, 0.3f, z, 3.2f, RGBA(255, 220, 210, (int)(55 * mist)));
    }
    /* distant haze, coloured by the hour */
    uint32_t hz = sun_col((int)(150 - 70 * s));
    gfx_rect_v(0, 0, VW, 200, hz, hz & 0xFFFFFF00);
    gfx_blend(BL_ADD);
    /* rays fanning out from the sun, longest and richest at dawn */
    for (int i = 0; i < 11; i++) {
        float base = -1.15f + (float)i * 0.23f + sinf(T * 0.07f + (float)i * 1.9f) * 0.04f + (s - 0.5f) * 0.15f;
        float w = 0.035f + 0.025f * (float)((i * 37) % 3);
        float a = (26.0f + 14.0f * sinf(T * 0.4f + (float)i * 2.1f)) * (1.0f - 0.45f * s);
        float L = VH * 1.5f;
        float a0 = base - w, a1 = base + w;
        uint32_t c0 = sun_col((int)a), c1 = sun_col(0);
        gfx_tri(sx, sy, c0, sx + sinf(a0) * L, sy + cosf(a0) * L, c1, sx + sinf(a1) * L, sy + cosf(a1) * L, c1);
    }
    /* the sun itself */
    gfx_glow(sx, sy, 420, sun_col(70));
    gfx_glow(sx, sy, 150, sun_col(110));
    gfx_blend(BL_ALPHA);
    if (sy > -30) {
        gfx_poly(sx, sy, 26, 24, 0, sun_col(255));
        gfx_blend(BL_ADD);
        gfx_poly(sx, sy, 18, 20, 0, sun_col((int)(60 + 120 * s)));
        gfx_blend(BL_ALPHA);
    }
    /* gulls wheeling over the island */
    for (int i = 0; i < 5; i++) {
        float t = T * (0.05f + 0.012f * (float)i) + (float)i * 0.37f;
        float gx = -4.0f + fmodf(t, 1.0f) * 27.0f, gz = 1.0f + (float)i * 2.2f + sinf(T * 0.3f + (float)i) * 0.8f, gy = 2.6f + 0.3f * (float)(i % 2);
        float px, py;
        proj(gx, gy, gz, &px, &py);
        float flap = sinf(T * (5.0f + (float)i) + (float)i) * 5.0f, wsz = 9.0f;
        uint32_t c = RGBA(250, 248, 240, 230);
        gfx_line(px - wsz, py - flap, px, py, 2.0f, c, c);
        gfx_line(px, py, px + wsz, py - flap, 2.0f, c, c);
        gfx_line(px - wsz * 0.5f, py + 30.0f, px + wsz * 0.5f, py + 30.0f, 3.0f, 0x00000018, 0x00000018);   /* faint shadow below */
    }
    /* soft vignette */
    gfx_rect_v(0, VH - 80, VW, 80, 0x10141C00, 0x10141C80);
    gfx_quad4(0, 0, 0x10141C60, 80, 0, 0x10141C00, 80, VH, 0x10141C00, 0, VH, 0x10141C60);
    gfx_quad4(VW - 80, 0, 0x10141C00, VW, 0, 0x10141C60, VW, VH, 0x10141C60, VW - 80, VH, 0x10141C00);
}

/* level 7 eye candy: a rainbow over the kingdom, butterflies, twinkles on the clouds, shooting stars, bubbles */
static void draw_sky_candy(void)
{
    static const uint32_t RB[6] = { 0xFF5060FF, 0xFFA040FF, 0xFFF060FF, 0x60F070FF, 0x60A0FFFF, 0xC070FFFF };
    gfx_blend(BL_ADD);
    /* a great rainbow arching behind the castle, gently shimmering */
    float sh = 0.7f + 0.3f * sinf(T * 0.6f);
    for (int band = 0; band < 6; band++) {
        float R = (float)MW * 0.62f - 0.45f * (float)band, ox = 0, oy = 0, oz = 0;
        for (int k = 0; k <= 24; k++) {
            float a = (float)k / 24.0f * 3.14159f;
            float px = FIRE_X + cosf(a) * R, py = sinf(a) * R * 0.5f, pz = -3.0f;
            if (k) r_line(ox, oy, oz, px, py, pz, 7.0f, col_a(RB[band], 0.22f * sh), col_a(RB[band], 0.22f * sh));
            ox = px; oy = py; oz = pz;
        }
    }
    /* twinkles on the clouds */
    for (int i = 0; i < nglint; i++) {
        const Glint *gl = &glint[i];
        float tw = sinf(T * 2.3f + gl->ph) * sinf(T * 1.3f + gl->ph * 1.7f);
        if (tw < 0.5f) continue;
        float px, py, k = (tw - 0.5f) * 2.0f;
        proj(gl->x, 0.25f, gl->z, &px, &py);
        uint32_t c = RGBA(255, 250, 255, (int)(230 * k));
        float L = 3.0f + 5.0f * k;
        gfx_line(px - L, py, px + L, py, 1.4f, c, c);
        gfx_line(px, py - L, px, py + L, 1.4f, c, c);
    }
    /* shooting stars across the top every few seconds */
    float st = fmodf(T, 5.0f);
    if (st < 0.9f) {
        float u = st / 0.9f, sx = VW * (0.15f + 0.8f * fmodf(floorf(T / 5.0f) * 0.37f, 1.0f)) - u * 260.0f, sy = 30.0f + u * 90.0f;
        uint32_t c = RGBA(255, 250, 220, (int)(255 * (1.0f - u)));
        gfx_line(sx, sy, sx + 70.0f, sy - 24.0f, 2.5f, c, c & 0xFFFFFF00);
        gfx_glow_cheap(sx, sy, 6.0f, c);
    }
    gfx_blend(BL_ALPHA);
    /* butterflies fluttering over the board */
    for (int i = 0; i < 7; i++) {
        float t = T * (0.07f + 0.01f * (float)i) + (float)i * 0.7f;
        float bx = FIRE_X + sinf(t * 2.0f + (float)i) * (float)MW * 0.42f, bz = FIRE_Z + cosf(t * 1.3f + (float)i * 2.0f) * (float)MH * 0.4f;
        float by = 1.4f + 0.3f * sinf(T * 1.7f + (float)i), flap = fabsf(sinf(T * 14.0f + (float)i)) * 0.9f + 0.1f;
        float px, py;
        proj(bx, by, bz, &px, &py);
        float sz = proj_scale(by, bz) * 0.035f;
        uint32_t c = RB[(i * 2) % 6] | 0xFF, c2 = col_mix(c, 0xFFFFFFFF, 0.4f);
        gfx_tri(px, py, c, px - 7.0f * sz * flap, py - 6.0f * sz, c2, px - 6.0f * sz * flap, py + 3.0f * sz, c);
        gfx_tri(px, py, c, px + 7.0f * sz * flap, py - 6.0f * sz, c2, px + 6.0f * sz * flap, py + 3.0f * sz, c);
        gfx_line(px, py - 3.0f * sz, px, py + 3.0f * sz, 1.6f * sz, 0x302030FF, 0x302030FF);
    }
    /* soap bubbles drifting up the screen */
    gfx_blend(BL_ADD);
    for (int i = 0; i < 9; i++) {
        uint32_t h = (uint32_t)i * 2654435761u;
        float bx = (float)(h % (uint32_t)(VW - 40)) + 20.0f + sinf(T * 0.8f + (float)i) * 14.0f;
        float by = VH + 30.0f - fmodf(T * (18.0f + (float)(i % 4) * 6.0f) + (float)((h >> 8) % 600), VH + 60.0f);
        float r = 5.0f + (float)(i % 3) * 3.0f;
        uint32_t c = col_mix(RGBA(255, 180, 240, 90), RGBA(160, 230, 255, 90), 0.5f + 0.5f * sinf(T + (float)i));
        for (int k = 0; k < 8; k++) {
            float a0 = (float)k * 0.7854f, a1 = a0 + 0.7854f;
            gfx_line(bx + cosf(a0) * r, by + sinf(a0) * r, bx + cosf(a1) * r, by + sinf(a1) * r, 1.2f, c, c);
        }
        gfx_glow_cheap(bx - r * 0.35f, by - r * 0.35f, 1.6f, RGBA(255, 255, 255, 160));
    }
    gfx_blend(BL_ALPHA);
    /* soft pink edges */
    gfx_rect_v(0, VH - 70, VW, 70, 0xF0A0D000, 0xF0A0D050);
    gfx_quad4(0, 0, 0xF0A0D040, 70, 0, 0xF0A0D000, 70, VH, 0xF0A0D000, 0, VH, 0xF0A0D040);
    gfx_quad4(VW - 70, 0, 0xF0A0D000, VW, 0, 0xF0A0D040, VW, VH, 0xF0A0D040, VW - 70, VH, 0xF0A0D000);
    if (G->surge_t > 0) {
        float k = clampf(G->surge_t / 1.6f, 0, 1);
        gfx_blend(BL_ADD);
        gfx_rect(0, 0, VW, VH, RGBA(255, 200, 240, (int)(60 * k * k)));
        gfx_blend(BL_ALPHA);
    }
}

static void draw_mist_vignette(void)
{
    if (ISLAND) { draw_island_dawn(); return; }
    if (HALLOW) { draw_hallow_night(); return; }
    if (AUTUMN) { draw_golden_hour(); return; }
    if (SKYLV) draw_sky_candy();
    if (FUTURE) {
        /* a hazy night over the town: purple sky glow at the far edge, low drifting steam, the time surge rippling out */
        gfx_rect_v(0, 0, VW, 150, RGBA(110, 60, 140, 110), RGBA(80, 50, 120, 0));
        for (int i = 0; i < 7; i++) {
            float xx = fmodf((float)i * 4.1f + T * 0.2f, 34.0f) - 4.0f, zz = (float)((i * 5) % 17) + 0.5f;
            r_glow(xx, 0.3f, zz, 2.6f, RGBA(140, 130, 180, 22));
        }
        if (G->surge_t > 0) {
            float k = clampf(G->surge_t / 1.6f, 0, 1);
            gfx_blend(BL_ADD);
            gfx_rect(0, 0, VW, VH, RGBA(110, 170, 255, (int)(60 * k * k)));
            gfx_blend(BL_ALPHA);
        }
    }
    if (PIRATE) {
        /* spray blown across the sea in the gusts */
        float gs = G->gust;
        for (int i = 0; i < 8; i++) {
            float x = fmodf((float)i * 3.9f + T * (0.5f + 2.5f * gs), 34.0f) - 4.0f;
            float z = (float)((i * 5) % 17) + 0.5f;
            r_glow(x, 0.35f, z, 2.8f, RGBA(150, 175, 200, (int)(22 + 40 * gs)));
        }
        if (gs > 0.05f) {
            gfx_blend(BL_ADD);
            for (int i = 0; i < 30; i++) {
                float sp = 1.0f + (float)(i % 4) * 0.3f;
                float x = fmodf((float)i * 131.7f + T * 900.0f * sp, VW + 300.0f) - 150.0f;
                float y = fmodf((float)i * 57.3f, VH);
                uint32_t c = RGBA(200, 215, 235, (int)(50 * gs));
                gfx_line(x, y, x - 90.0f * gs, y + 6.0f, 1.4f, c, c & 0xFFFFFF00);
            }
            gfx_blend(BL_ALPHA);
        }
    }
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
        if (!e->alive || e->hp >= e->maxhp || ENEMIES[e->type].boss) continue;
        const EnemyDef *d = &ENEMIES[e->type];
        float sx, sy;
        proj(e->x, e->y + d->size * 1.9f + 0.25f, e->z, &sx, &sy);
        float w = 14.0f + d->size * 30.0f, f = clampf(e->hp / e->maxhp, 0, 1);
        gfx_rect(sx - w * 0.5f - 1, sy - 1, w + 2, 5, 0x000000B0);
        gfx_rect(sx - w * 0.5f, sy, w * f, 3, f > 0.5f ? 0x60E060FF : (f > 0.25f ? 0xF0C040FF : 0xF05040FF));
    }
    /* towers that have been smashed show how much they have left */
    for (int r = 0; r < MH; r++)
        for (int c = 0; c < MW; c++) {
            const Tower *t = &G->tw[r][c];
            if (t->type < 0 || t->hp >= TOWER_HP[t->lvl] - 0.01f) continue;
            float sx, sy;
            proj((float)c + 0.5f, G->tile[r][c].h + 1.25f, (float)r + 0.5f, &sx, &sy);
            float f = clampf(t->hp / TOWER_HP[t->lvl], 0, 1), w = 30.0f;
            uint32_t col = f > 0.5f ? 0xF0D040FF : 0xFF5030FF;
            if (f <= 0.3f && fmodf(T, 0.5f) < 0.25f) col = 0xFFFFFFFF;
            gfx_rect(sx - w * 0.5f - 1, sy - 1, w + 2, 6, 0x000000C0);
            gfx_rect(sx - w * 0.5f, sy, w * f, 4, col);
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
    if (SKYLV) {                                     /* a little crown */
        uint32_t cc = col_mix(0xFFD040FF, 0xFFFFFFFF, G->fire_flash);
        gfx_rect(26, 56, 17, 6, cc);
        gfx_tri(26, 56, cc, 26, 46, cc, 31, 56, cc); gfx_tri(31, 56, cc, 34.5f, 45, cc, 38, 56, cc); gfx_tri(38, 56, cc, 43, 46, cc, 43, 56, cc);
        gfx_rect(33, 57, 3, 3, 0xFF4080FF);
    } else if (FUTURE) {                                    /* a lightning bolt */
        uint32_t bc = col_mix(0xFFD040FF, 0xFFFFFFFF, G->fire_flash);
        gfx_tri(36, 45, bc, 28, 56, bc, 34, 56, bc);
        gfx_tri(34, 54, bc, 40, 54, bc, 30, 66, bc);
    } else if (PIRATE) {                                    /* a little treasure chest */
        gfx_rect(26, 52, 17, 9, 0x8A5028FF); gfx_rect(26, 47, 17, 5, 0xB07038FF); gfx_rect(26, 51, 17, 2, 0xE0B040FF);
        gfx_rect(33, 52, 3, 4, 0xFFD860FF);
    } else flame_icon(34, 55, 9, 0.6f + 0.4f * G->fire_vis);
    float ff = clampf((float)G->fire / (float)FIRE_HP, 0, 1);
    gfx_rect(50, 50, 120, 10, 0x000000C0);
    gfx_rect_v(51, 51, 118 * ff, 8, col_mix(0xFFC050FF, 0xFFFFFFFF, G->fire_flash), col_mix(0xE05020FF, 0xFF2020FF, G->fire_flash));
    snprintf(buf, sizeof buf, "%d", G->fire);
    gfx_text_sr(buf, 168, 38, 1.4f, 0xFFD8B0FF);

    /* wave status (top right) */
    panel(VW - 214, 12, 200, 58, 0x0A0E18B8);
    int nwv = G->lv->nwaves;
    snprintf(buf, sizeof buf, "WAVE %d/%d", (!G->wave_on && G->wave < nwv) ? G->wave + 1 : G->wave, nwv);
    gfx_text_sr(buf, VW - 26, 22, 2.0f, 0xE8F0FFFF);
    if (!G->wave_on && G->wave < nwv) {
        snprintf(buf, sizeof buf, "NEXT IN %d", (int)ceilf(G->countdown));
        gfx_text_sr(buf, VW - 26, 42, 1.4f, 0xFFD070FF);
        snprintf(buf, sizeof buf, "X: CALL NOW +%d", (int)G->countdown);
        gfx_text_sr(buf, VW - 26, 55, 1.2f, 0xA8B4D0FF);
    } else {
        int left = 0;
        for (int i = 0; i < MAX_EN; i++) left += G->en[i].alive;
        const WaveDef *w = &G->lv->waves[G->wave > 0 ? G->wave - 1 : 0];
        for (int k = 0; k < w->n; k++) left += w->g[k].count - G->spawned[k];
        snprintf(buf, sizeof buf, "ENEMIES %d", left);
        gfx_text_sr(buf, VW - 26, 44, 1.4f, 0xFF9080FF);
    }
    if (G->fast) {
        float a = 0.6f + 0.4f * sinf(T * 6.0f);
        gfx_tri(VW - 252, 26, col_a(0xFFFFFFFF, a), VW - 252, 46, col_a(0xFFFFFFFF, a), VW - 240, 36, col_a(0xFFFFFFFF, a));
        gfx_tri(VW - 238, 26, col_a(0xFFFFFFFF, a), VW - 238, 46, col_a(0xFFFFFFFF, a), VW - 226, 36, col_a(0xFFFFFFFF, a));
    }

    if (PINBALL) {
        /* pinball score and the 88 MPH meter (level 7: the wishing-star meter) */
        panel(14, 76, 168, 46, 0x0A0E18B8);
        snprintf(buf, sizeof buf, "%ld", G->score);
        gfx_text_s(buf, 24, 82, 1.6f, SKYLV ? 0xFF90E0FF : 0xFFA030FF);
        float m = (float)G->mph / (SKYLV ? 77.0f : 88.0f);
        gfx_rect(24, 104, 110, 9, 0x000000C0);
        if (SKYLV) gfx_rect_v(25, 105, 108 * m, 7, 0xFFE070FF, 0xFF70C0FF);
        else gfx_rect_v(25, 105, 108 * m, 7, 0x80E0FFFF, 0x3070E0FF);
        if (SKYLV) snprintf(buf, sizeof buf, "WISH %d", G->mph);
        else snprintf(buf, sizeof buf, "%d MPH", G->mph);
        gfx_text_sr(buf, 176, 104, 1.2f, m > 0.85f && fmodf(T, 0.4f) < 0.2f ? 0xFFFFFFFF : 0x90D8FFFF);
        if (G->combo >= 3 && G->combo_t > 0) {
            snprintf(buf, sizeof buf, "COMBO %d", G->combo);
            gfx_text_s(buf, 24, 126, 1.4f, col_a(0xFF70D0FF, clampf(G->combo_t, 0, 1)));
        }
    }
    /* the pinball dot-matrix display: callouts pop up, the bulbs round it chase */
    if (PINBALL && G->dmd_t < 2.4f) {
        /* at the very top of the screen, out of the way of the board */
        float t = G->dmd_t, pop = clampf(t / 0.15f, 0, 1), fade = t > 2.0f ? (2.4f - t) / 0.4f : 1.0f;
        if (!(t > 1.9f && fmodf(t, 0.16f) < 0.08f)) {
            float px1 = 3.0f * (0.6f + 0.4f * pop), px2 = 1.9f * (0.6f + 0.4f * pop);
            float w1 = gfx_text_width(G->dmd[0], px1), w2 = gfx_text_width(G->dmd[1], px2), w = fmaxf(w1, w2) + 36.0f;
            float h = G->dmd[1][0] ? 60.0f : 40.0f, x0 = VW * 0.5f - w * 0.5f, y0 = 14.0f;
            uint32_t frame = SKYLV ? 0x4A1838FF : 0x3A1A08FF, back = SKYLV ? 0x1A0614FF : 0x120804FF;
            gfx_rect(x0 - 4, y0 - 4, w + 8, h + 8, col_a(frame, 0.85f * fade));
            gfx_rect(x0, y0, w, h, col_a(back, 0.92f * fade));
            uint32_t c = col_a(G->dmd_c, fade);
            float ty = y0 + 8.0f;
            gfx_text_c(G->dmd[0], VW * 0.5f, ty, px1, c);
            if (G->dmd[1][0]) gfx_text_c(G->dmd[1], VW * 0.5f, ty + 29.0f, px2, col_a(SKYLV ? 0xFFE0A0FF : 0xFFC060FF, fade));
            /* dot-matrix grid over the letters */
            uint32_t gc = col_a(back, 0.9f * fade);
            for (float yy = y0; yy < y0 + h; yy += 2.6f) gfx_rect(x0, yy, w, 0.8f, gc);
            for (float xx = x0; xx < x0 + w; xx += 2.6f) gfx_rect(xx, y0, 0.8f, h, gc);
            gfx_blend(BL_ADD);
            int nb = (int)(w / 18.0f);
            for (int i = 0; i < nb; i++) {                   /* chasing bulbs top and bottom */
                int on = ((i + (int)(T * 14.0f)) % 3) == 0;
                uint32_t bcl = on ? col_a(SKYLV ? 0xFFB0F0FF : 0xFFD080FF, fade) : col_a(SKYLV ? 0x602050FF : 0x603010FF, fade);
                gfx_glow_cheap(x0 + 9.0f + (float)i * 18.0f, y0 - 6.0f, on ? 5.0f : 3.0f, bcl);
                gfx_glow_cheap(x0 + w - 9.0f - (float)i * 18.0f, y0 + h + 6.0f, on ? 5.0f : 3.0f, bcl);
            }
            gfx_glow(VW * 0.5f, y0 + h * 0.5f, w * 0.6f, col_a(G->dmd_c, 0.12f * fade * (1.0f - t * 0.3f)));
            gfx_blend(BL_ALPHA);
        }
    }

    /* boss bar */
    if (G->boss >= 0 && G->en[G->boss].alive) {
        const Enemy *b = &G->en[G->boss];
        float f = clampf(b->hp / b->maxhp, 0, 1), by = PINBALL ? 128.0f : 84.0f;   /* below the dot-matrix display on the pinball levels */
        gfx_text_sc(ENEMIES[b->type].name, VW * 0.5f, by, 1.6f, b->type == EN_DRAGON ? 0xE0A0FFFF : b->type == EN_PUMPKIN ? 0xFFC060FF : (b->type == EN_GHOULKING ? 0xA0FF90FF : (b->type == EN_ARCHITECT ? 0xF0D080FF : 0xA0E8FFFF)));
        gfx_rect(VW * 0.5f - 151, by + 14, 302, 10, 0x000000C0);
        if (b->type == EN_DRAGON) gfx_rect_v(VW * 0.5f - 150, by + 15, 300 * f, 8, 0xE0A0FFFF, 0x8040C0FF);
        else if (b->type == EN_PUMPKIN) gfx_rect_v(VW * 0.5f - 150, by + 15, 300 * f, 8, 0xFFC060FF, 0xE06010FF);
        else if (b->type == EN_GHOULKING) gfx_rect_v(VW * 0.5f - 150, by + 15, 300 * f, 8, 0xA0FF80FF, 0x30A040FF);
        else if (b->type == EN_ARCHITECT) gfx_rect_v(VW * 0.5f - 150, by + 15, 300 * f, 8, 0xFFE090FF, 0xB07820FF);
        else gfx_rect_v(VW * 0.5f - 150, by + 15, 300 * f, 8, 0x80E0FFFF, 0x3080D0FF);
    }

    /* banner */
    if (G->banner_t < 4.0f && G->state == ST_PLAY && !PINBALL) {     /* (the pinball levels call it on the display) */
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
    float cfade = (csx < 360 && csy > cy - 30) ? 0.35f : 1.0f;
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
    gfx_text_s("ZL", 14 + TW_TYPES * 62, cy + 14, 1.5f, col_a(0x8090B0FF, cfade));
    gfx_text_s("ZR", 14 + TW_TYPES * 62, cy + 40, 1.5f, col_a(0x8090B0FF, cfade));
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
        int dmgd = t->hp < TOWER_HP[t->lvl] - 0.01f;
        if (dmgd) {
            snprintf(buf, sizeof buf, "HP %d/%d", (int)ceilf(t->hp), (int)TOWER_HP[t->lvl]);
            gfx_text_sr(buf, px + 228, py + 8, 1.4f, col_a(t->hp > TOWER_HP[t->lvl] * 0.5f ? 0xF0D040FF : 0xFF6040FF, ifade));
        }
        if (t->lvl + 1 >= TW_MAXLVL && dmgd) {
            int c = tower_repair_cost(t);
            snprintf(buf, sizeof buf, "A: REPAIR %d", c);
            gfx_text_s(buf, px + 10, py + 41, 1.35f, col_a(G->gold >= c ? 0xFFE070FF : 0xFF7060FF, ifade));
        } else if (t->lvl + 1 < TW_MAXLVL) {
            int c = d->cost[t->lvl + 1];
            snprintf(buf, sizeof buf, dmgd ? "A: UPGRADE+FIX %d" : "A: UPGRADE %d", c);
            gfx_text_s(buf, px + 10, py + 41, 1.35f, col_a(G->gold >= c ? 0xFFE070FF : 0xFF7060FF, ifade));
        } else gfx_text_s("MAX LEVEL", px + 10, py + 41, 1.35f, col_a(0x90F090FF, ifade));
        snprintf(buf, sizeof buf, "B: SELL %d", tower_sell_value(t));
        gfx_text_s(buf, px + 10, py + 55, 1.35f, col_a(0xA8B4D0FF, ifade));
    } else if (can_build(G, G->cx, G->cz)) {
        const TowerDef *d = &TOWERS[G->sel];
        panel(px, py, 238, 70, pbg);
        gfx_text_s(d->name, px + 10, py + 8, 1.7f, col_a(d->glow, ifade));
        static const char *DESC[TW_TYPES] = { "FAST SHOTS, HITS FLYERS", "SPLASH, GROUND ONLY", "SLOWS EVERYTHING NEARBY", "CHAIN LIGHTNING", "BEAM GROWS STRONGER" };
        gfx_text_s(DESC[G->sel], px + 10, py + 27, 1.25f, col_a(0xC8D0E0FF, ifade));
        snprintf(buf, sizeof buf, "A: BUILD %d", d->cost[0]);
        gfx_text_s(buf, px + 10, py + 46, 1.5f, col_a(G->gold >= d->cost[0] ? 0xFFE070FF : 0xFF7060FF, ifade));
    }

    /* controls reminder */
    gfx_text_sc("Y: SPEED   L/R: ZOOM   +: PAUSE", VW * 0.5f + 20, VH - 14, 1.2f, 0x8C98B4C0);
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
    gfx_text_top(s, cx - w * 0.5f, y, px, 0xFFD878FF, 2);
}

static void draw_title(void)
{
    gfx_rect(0, 0, VW, VH, AUTUMN ? 0x1A0C0060 : (HALLOW ? 0x0A061870 : (ISLAND ? 0x0A142060 : (PIRATE ? 0x04101870 : 0x02040A70))));
    fire_text("STORMFIRE", VW * 0.5f, 104, 10.0f);
    gfx_text_sc(SKYLV ? "THE PRINCESS'S CASTLE IN THE CLOUDS NEEDS YOU!" : FUTURE ? "PROTECT THE TIME MACHINE. 1.21 GIGAWATTS!" : PIRATE ? "PIRATES ARE COMING FOR THE TREASURE. HOLD THE ISLAND!" : ISLAND ? "AN ISLAND OF OLD MACHINES. HOLD IT TILL SUNRISE." : HALLOW ? "THE DEAD WALK TONIGHT. KEEP THE FIRE LIT." : (AUTUMN ? "HOLD THE FIRE TILL THE SUN GOES DOWN" : "KEEP THE CAMPFIRE BURNING THROUGH THE STORM"),
                VW * 0.5f, 190, 1.8f, SKYLV ? 0xFFE0F4FF : FUTURE ? 0xC8E8FFFF : PIRATE ? 0xC8E4F0FF : ISLAND ? 0xFFE0C8FF : HALLOW ? 0xD8C8FFFF : (AUTUMN ? 0xFFE8C8FF : 0xC8D4F0FF));
    /* level picker */
    char lb[48];
    snprintf(lb, sizeof lb, "LEVEL %d: %s", G->level + 1, G->lv->name);
    float lw = gfx_text_width(lb, 2.4f), nudge = sinf(T * 5.0f) * 3.0f;
    panel(VW * 0.5f - lw * 0.5f - 52, 216, lw + 104, 36, 0x0A0E18C8);
    gfx_text_sc(lb, VW * 0.5f, 226, 2.4f, SKYLV ? 0xFF90E0FF : FUTURE ? 0xFF9040FF : PIRATE ? 0xFFD060FF : ISLAND ? 0x90E8E0FF : HALLOW ? 0xC890FFFF : (AUTUMN ? 0xFFC060FF : 0x90D8FFFF));
    gfx_text_s("<", VW * 0.5f - lw * 0.5f - 38 - nudge, 226, 2.4f, 0xFFFFFFC0);
    gfx_text_s(">", VW * 0.5f + lw * 0.5f + 24 + nudge, 226, 2.4f, 0xFFFFFFC0);
    if (fmodf(T, 1.2f) < 0.85f) gfx_text_sc("PRESS A TO START", VW * 0.5f, 272, 2.6f, 0xFFFFFFFF);
    panel(VW * 0.5f - 230, 308, 460, 140, 0x0A0E18C0);
    static const char *L1[] = { "STICK / D-PAD", "A", "B", "ZL / ZR", "Y", "X", "L / R", "+" };
    static const char *L2[] = { "MOVE THE CURSOR", "BUILD / UPGRADE", "SELL", "CHOOSE TOWER", "FAST FORWARD", "CALL NEXT WAVE EARLY", "ZOOM OUT / IN", "PAUSE" };
    for (int i = 0; i < 8; i++) {
        float y = 318 + (float)(i % 4) * 32, x = VW * 0.5f - 214 + (float)(i / 4) * 228;
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
        fire_text(SKYLV ? "THE CASTLE IS SAFE!" : FUTURE ? "THE DELOREAN IS SAFE!" : PIRATE ? "THE TREASURE IS SAFE!" : "THE FIRE SURVIVES!", VW * 0.5f, 120, FUTURE ? 5.0f : 5.5f);
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
        gfx_text_sc(SKYLV ? "THE CASTLE HAS FALLEN" : FUTURE ? "THE TIME MACHINE IS GONE" : PIRATE ? "THE PIRATES TOOK THE TREASURE" : "THE FIRE HAS GONE OUT", VW * 0.5f, 130, PIRATE || FUTURE || SKYLV ? 3.6f : 4.5f, 0x90A8D0FF);
    }
    int nwv = G->lv->nwaves;
    snprintf(buf, sizeof buf, "WAVES SURVIVED  %d/%d", G->state == ST_WIN ? nwv : (G->wave > 0 ? G->wave - 1 : 0), nwv);
    gfx_text_sc(buf, VW * 0.5f, 280, 2.0f, 0xE0E8FFFF);
    snprintf(buf, sizeof buf, "ENEMIES DEFEATED  %d", G->kills);
    gfx_text_sc(buf, VW * 0.5f, 306, 2.0f, 0xE0E8FFFF);
    snprintf(buf, sizeof buf, SKYLV ? "CASTLE  %d/%d" : FUTURE ? "TIME MACHINE  %d/%d" : PIRATE ? "TREASURE LEFT  %d/%d" : "FIRE LEFT  %d/%d", G->fire, FIRE_HP);
    if (PINBALL) {
        char sc[48];
        snprintf(sc, sizeof sc, "SCORE %ld", G->score);
        gfx_text_sc(sc, VW * 0.5f, 358, 2.0f, 0xFFA030FF);
    }
    gfx_text_sc(buf, VW * 0.5f, 332, 2.0f, 0xFFC080FF);
    if (G->end_t > 2.0f) {
        if (G->state == ST_WIN && G->level + 1 < NUM_LEVELS) snprintf(buf, sizeof buf, "PRESS A FOR LEVEL %d", G->level + 2);
        else snprintf(buf, sizeof buf, G->state == ST_WIN ? "PRESS A TO PLAY AGAIN" : "PRESS A TO TRY AGAIN");
        if (fmodf(T, 1.2f) < 0.85f) gfx_text_sc(buf, VW * 0.5f, 380, 2.4f, 0xFFFFFFFF);
        gfx_text_sc("B: TITLE SCREEN", VW * 0.5f, 414, 1.5f, 0xA8B4D0FF);
    }
}

static void draw_pause(void)
{
    gfx_rect(0, 0, VW, VH, 0x02040AA0);
    gfx_text_sc("PAUSED", VW * 0.5f, 120, 5.0f, 0xFFFFFFFF);
    const char *M[5] = { "RESUME", G->music_off ? "MUSIC: OFF" : "MUSIC: ON", G->mute ? "SOUND: OFF" : "SOUND: ON", "RESTART", "QUIT TO TITLE" };
    for (int i = 0; i < 5; i++) {
        int s = G->menu == i;
        float y = 190 + (float)i * 40;
        if (s) panel(VW * 0.5f - 140, y - 10, 280, 38, 0xFF904060);
        gfx_text_sc(M[i], VW * 0.5f, y, 2.6f, s ? 0xFFFFFFFF : 0x8090B0FF);
    }
}

/* ------------------------------------------------------------------ */
void game_render(const Game *g)
{
    G = g;
    T = g->time;
    gfx_begin();
    if (built_level != g->level) build_static();
    /* screen shake with depth parallax (the near side of the board moves more than the far side),
     * and a tiny zoom punch on big hits */
    float sh = g->shake * sqrtf(g->shake) * 10.0f;
    float shx = (sinf(T * 53.0f) * 0.7f + sinf(T * 31.0f + 1.3f) * 0.3f) * sh;
    float shy = (cosf(T * 41.0f) * 0.7f + sinf(T * 67.0f + 0.4f) * 0.3f) * sh * 0.75f;
    SHX = SHY = 0;                       /* the offset now goes through the view transform instead */
    level_camera();
    cam_world(0, 0);
    /* L2/R2 zoom: scale the world about the screen centre and pan to follow the cursor, never past the map */
    {
        static float fxs = VW * 0.5f, fys = VH * 0.5f;
        float Z = g->zoom * (1.0f + g->punch * 0.014f), tx, ty;
        proj(g->cur_x + 0.5f, g->tile[g->cz][g->cx].h, g->cur_z + 0.5f, &tx, &ty);
        if (g->zoom < 1.01f) { tx = VW * 0.5f; ty = VH * 0.5f; }
        fxs += (tx - fxs) * 0.12f; fys += (ty - fys) * 0.12f;
        float hx = VW * 0.5f / g->zoom, hy = VH * 0.5f / g->zoom;
        float fx = clampf(fxs, hx, VW - hx), fy = clampf(fys, hy, VH - hy);
        gfx_view(shx, shy, Z);
        gfx_pan(-(fx - VW * 0.5f) * Z, -(fy - VH * 0.5f) * Z);
    }
    if (PIRATE) {
        /* the whole world rides the swell: a rolling wave plus a slow roll, stronger in the gusts */
        float amp = (2.6f + 2.0f * G->gust) * g->zoom;
        float roll = (0.007f * sinf(T * 0.55f) + 0.003f * sinf(T * 1.27f + 0.7f)) * (1.0f + G->gust);
        float bob = (2.0f * sinf(T * 0.8f + 0.4f) + 0.8f * sinf(T * 1.9f)) * g->zoom;
        gfx_sea(amp, roll, bob, T);
    } else gfx_sea(0, 0, 0, 0);
    gfx_fx(1, 1);
    if (ISLAND) {
        /* sunrise: rosy and dim at first light, golden, then a bright clear morning */
        float s = G->sun, B[3], U[3];
        static const float K[3][3] = { {0.48f, 0.33f, 0.42f}, {0.76f, 0.59f, 0.46f}, {0.84f, 0.80f, 0.72f} };
        float u = s * 2.0f;
        int k0 = u >= 1.0f ? 1 : 0;
        float f = u - (float)k0;
        if (f > 1) f = 1;
        for (int i = 0; i < 3; i++) { B[i] = K[k0][i] + (K[k0 + 1][i] - K[k0][i]) * f; U[i] = B[i] * 0.1f; }
        lt_set_ambient(B, U, 0.22f);
        rc_set_stretch(2.6f - 2.0f * s);                         /* long dawn shadows shorten as it climbs */
    } else if (SKYLV) {
        /* a bright pastel afternoon above the clouds */
        static const float B[3] = { 0.86f, 0.8f, 0.9f }, U[3] = { 0.04f, 0.04f, 0.05f };
        lt_set_ambient(B, U, 0.0f);
    } else if (FUTURE) {
        /* night in the town square: a cool violet ambience, neon and lamps do the rest */
        static const float B[3] = { 0.22f, 0.21f, 0.33f }, U[3] = { 0.05f, 0.05f, 0.07f };
        lt_set_ambient(B, U, 0.0f);
    } else if (PIRATE) {
        /* storm at sea: dark green-blue, the lightning does the rest */
        static const float B[3] = { 0.27f, 0.32f, 0.40f }, U[3] = { 0.06f, 0.07f, 0.08f };
        lt_set_ambient(B, U, 0.0f);
    } else if (HALLOW) {
        /* dusk: cool moonlight with a little purple, branches casting a patchy shade */
        static const float B[3] = { 0.30f, 0.29f, 0.44f }, U[3] = { 0.05f, 0.05f, 0.08f };
        lt_set_ambient(B, U, 0.18f);
    } else if (AUTUMN) {
        static const float B[3] = { 0.66f, 0.47f, 0.33f }, U[3] = { 0.09f, 0.055f, 0.02f };
        lt_set_ambient(B, U, 0.40f);
    } else {
        static const float B[3] = { 0.17f, 0.20f, 0.32f }, U[3] = { 0.04f, 0.045f, 0.06f };
        lt_set_ambient(B, U, 0.0f);
    }
    build_lights();
    rc_relight(T, G->lv->env == ENV_STORM ? 0.6f + 0.6f * G->wind : (PIRATE ? 0.6f + 0.6f * G->wind + 1.4f * G->gust : 0.6f + 0.8f * G->wind));

    /* sky glow behind everything (only visible at the very top) */
    if (ISLAND) gfx_rect_v(0, 0, VW, VH, 0x2C5070FF, 0x1C3850FF);
    else if (PIRATE) gfx_rect_v(0, 0, VW, VH, 0x16303EFF, 0x0C1C28FF);
    else if (FUTURE) gfx_rect_v(0, 0, VW, VH, 0x2A1C40FF, 0x100C1CFF);
    else if (SKYLV) gfx_rect_v(0, 0, VW, VH, 0xFFC8E8FF, 0xC8B8F0FF);
    else if (HALLOW) gfx_rect_v(0, 0, VW, VH, 0x2A2040FF, 0x100C18FF);
    else if (AUTUMN) gfx_rect_v(0, 0, VW, VH, 0x5A3A20FF, 0x2A1A10FF);
    else gfx_rect_v(0, 0, VW, VH, 0x0A1220FF, 0x05080EFF);

    draw_ground();        dbg_nv[0] = gfx_nv;
    r_lod = g->zoom < 1.4f && MW > 19;      /* the big map is drawn small: round shapes need fewer sides */
    draw_world();         dbg_nv[1] = gfx_nv;
    r_lod = 0;
    if (g->state == ST_PLAY) draw_cursor();
    draw_effects();       dbg_nv[2] = gfx_nv;
    draw_rain();          dbg_nv[3] = gfx_nv;
    draw_mist_vignette();
    draw_world_ui();      dbg_nv[4] = gfx_nv;

    /* the HUD drifts a little against the shake (parallax), and never zooms */
    gfx_pan(0, 0);
    gfx_view(-shx * 0.12f, -shy * 0.12f, 1.0f);
    gfx_fx(0, 0);
    switch (g->state) {
    case ST_TITLE: draw_title(); break;
    case ST_PLAY: r_lod = 1; draw_hud(); r_lod = 0; break;               /* the tower cards are small */
    case ST_PAUSE: r_lod = 1; draw_hud(); r_lod = 0; draw_pause(); break;
    case ST_WIN: case ST_LOSE: draw_end(); break;
    }
}
