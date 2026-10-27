/*
 * STORMFIRE - native test harness (no Wii toolchain needed).
 *
 *  - checks the map (row lengths, every gate reaches the fire)
 *  - software-rasterizes the exact triangle list the Wii's GPU gets and saves screenshots
 *  - plays the level with bots of different skill to check it can be won, that doing nothing
 *    loses, and to tune difficulty
 *  - measures CPU time and vertex counts per frame
 *
 *  usage: test <outdir> [reel]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#include "../source/game.h"
#include "../source/game_int.h"
#include "../source/gfx.h"
#include "../source/music.h"
#include "../source/audio.h"

#define OW 1280
#define OH 720
static float fb[OH][OW][3];
static const char *outdir = ".";
static int fails;

#define CHECK(c, ...) do { if (!(c)) { printf("  FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

/* ------------------------------------------------------------------ */
/* software rasterizer                                                  */
/* ------------------------------------------------------------------ */
static void raster(void)
{
    for (int y = 0; y < OH; y++) for (int x = 0; x < OW; x++) fb[y][x][0] = fb[y][x][1] = fb[y][x][2] = 0;
    const float S = (float)OW / VW;
    for (int s = 0; s < gfx_nseg; s++) {
        int add = gfx_seg[s].mode == BL_ADD;
        for (int i = gfx_seg[s].start; i + 2 < gfx_seg[s].start + gfx_seg[s].count; i += 3) {
            const GVtx *v = &gfx_v[i];
            float x0 = v[0].x * S, y0 = v[0].y * S, x1 = v[1].x * S, y1 = v[1].y * S, x2 = v[2].x * S, y2 = v[2].y * S;
            float area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
            if (fabsf(area) < 1e-6f) continue;
            int minx = (int)floorf(fminf(x0, fminf(x1, x2))), maxx = (int)ceilf(fmaxf(x0, fmaxf(x1, x2)));
            int miny = (int)floorf(fminf(y0, fminf(y1, y2))), maxy = (int)ceilf(fmaxf(y0, fmaxf(y1, y2)));
            if (minx < 0) minx = 0; if (miny < 0) miny = 0;
            if (maxx > OW - 1) maxx = OW - 1; if (maxy > OH - 1) maxy = OH - 1;
            float c[3][4];
            for (int k = 0; k < 3; k++) {
                uint32_t col = v[k].c;
                c[k][0] = (float)(col >> 24) / 255.0f; c[k][1] = (float)((col >> 16) & 255) / 255.0f;
                c[k][2] = (float)((col >> 8) & 255) / 255.0f; c[k][3] = (float)(col & 255) / 255.0f;
            }
            float inv = 1.0f / area;
            for (int py = miny; py <= maxy; py++) {
                float fy = (float)py + 0.5f;
                for (int px = minx; px <= maxx; px++) {
                    float fx = (float)px + 0.5f;
                    float w0 = ((x1 - fx) * (y2 - fy) - (x2 - fx) * (y1 - fy)) * inv;
                    float w1 = ((x2 - fx) * (y0 - fy) - (x0 - fx) * (y2 - fy)) * inv;
                    float w2 = 1.0f - w0 - w1;
                    if (w0 < 0 || w1 < 0 || w2 < 0) continue;
                    float a = c[0][3] * w0 + c[1][3] * w1 + c[2][3] * w2;
                    for (int ch = 0; ch < 3; ch++) {
                        float src = c[0][ch] * w0 + c[1][ch] * w1 + c[2][ch] * w2;
                        float d = add ? fb[py][px][ch] + src * a : fb[py][px][ch] * (1 - a) + src * a;
                        fb[py][px][ch] = d > 1 ? 1 : (d < 0 ? 0 : d);
                    }
                }
            }
        }
    }
}

static void save_ppm(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6 %d %d 255\n", OW, OH);
    for (int y = 0; y < OH; y++) for (int x = 0; x < OW; x++) {
        unsigned char p[3] = {(unsigned char)(fb[y][x][0] * 255.0f + 0.5f), (unsigned char)(fb[y][x][1] * 255.0f + 0.5f), (unsigned char)(fb[y][x][2] * 255.0f + 0.5f)};
        fwrite(p, 1, 3, f);
    }
    fclose(f);
}

static void shot(const Game *g, const char *name)
{
    game_render(g);
    raster();
    char path[512];
    snprintf(path, sizeof path, "%s/%s.ppm", outdir, name);
    save_ppm(path);
    printf("  screenshot %s  (%d verts)\n", name, gfx_nv);
}

/* ------------------------------------------------------------------ */
/* bots                                                                 */
/* ------------------------------------------------------------------ */
static Game *G;
static Input NONE;
static int LEVEL;          /* level being tested */

/* fresh game sitting on the title screen of LEVEL */
static void reset_to(Game *g) { game_init(g); game_set_level(g, LEVEL); }

static void step(Game *g, const Input *in)
{
    game_update(g, in, 1.0f / 60.0f);
    while (game_pop_sound(g, NULL, NULL) >= 0) {}
}

static void press_at(Game *g, int cx, int cz, int sel, uint32_t btn)
{
    g->cx = cx; g->cz = cz; g->sel = sel;
    Input in = NONE;
    in.held = in.pressed = btn;
    step(g, &in);
}

/* how much path a tower at (c, r) with range R covers, weighted towards the fire */
static float spot_score(const Game *g, int c, int r, float R, int air)
{
    float s = 0, x = (float)c + 0.5f, z = (float)r + 0.5f;
    for (int rr = 0; rr < MH; rr++)
        for (int cc = 0; cc < MW; cc++) {
            int d = g->dist[rr][cc];
            if (d < 0) continue;
            float dx = (float)cc + 0.5f - x, dz = (float)rr + 0.5f - z;
            if (dx * dx + dz * dz > R * R) continue;
            s += 1.0f + 3.0f / (1.0f + (float)d);
        }
    if (air) {                                   /* flyers come straight at the fire */
        float dx = x - FIRE_X, dz = z - FIRE_Z;
        if (sqrtf(dx * dx + dz * dz) < R + 1.0f) s += 4.0f;
    }
    return s;
}

typedef struct { const char *name; int skill; } Bot;   /* 0 idle, 1 sloppy, 2 average, 3 keen */

static uint32_t brng = 1;
static float bfr(void) { brng ^= brng << 13; brng ^= brng >> 17; brng ^= brng << 5; return (float)(brng >> 8) / 16777216.0f; }

static void bot_think(Game *g, int skill)
{
    if (skill == 0 || g->state != ST_PLAY) return;
    /* the build order mixes types; sloppy bots think less often and pick worse spots */
    static const int ORDER[] = { TW_BLASTER, TW_BLASTER, TW_FROST, TW_MORTAR, TW_TESLA, TW_BLASTER, TW_MORTAR, TW_FROST, TW_TESLA, TW_PRISM, TW_MORTAR, TW_BLASTER, TW_TESLA, TW_PRISM };
    int ntw = 0;
    for (int r = 0; r < MH; r++) for (int c = 0; c < MW; c++) ntw += g->tw[r][c].type >= 0;
    int want = ORDER[ntw % (int)(sizeof ORDER / sizeof ORDER[0])];
    /* upgrade before building once there are enough towers */
    int up_bias = skill == 3 ? 6 : (skill == 2 ? 8 : 12);
    if (ntw >= up_bias) {
        int bc = -1, br = -1, bl = 9;
        for (int r = 0; r < MH; r++) for (int c = 0; c < MW; c++) {
            const Tower *t = &g->tw[r][c];
            if (t->type < 0 || t->lvl >= TW_MAXLVL - 1) continue;
            if (t->lvl < bl) { bl = t->lvl; bc = c; br = r; }
        }
        if (bc >= 0) {
            const Tower *t = &g->tw[br][bc];
            if (g->gold >= tower_cost(t->type, t->lvl + 1)) { press_at(g, bc, br, g->sel, BTN_A); return; }
            if (ntw >= up_bias + 4) return;                /* save up */
        }
    }
    if (g->gold < tower_cost(want, 0)) return;
    int bc = -1, br = -1;
    float bs = -1;
    for (int r = 1; r < MH - 1; r++) for (int c = 1; c < MW - 1; c++) {
        if (!can_build(g, c, r)) continue;
        float s = spot_score(g, c, r, TOWERS[want].range[0], TOWERS[want].air);
        if (skill == 1) s *= 0.4f + bfr() * 1.2f;
        if (skill == 2) s *= 0.8f + bfr() * 0.4f;
        if (s > bs) { bs = s; bc = c; br = r; }
    }
    if (bc >= 0) press_at(g, bc, br, want, BTN_A);
}

typedef struct { int won, wave, fire, towers, gold_left; float time; } Result;

static int verbose;
static Result play(int skill, uint32_t seed, int shots, const char *tag)
{
    Result res;
    memset(&res, 0, sizeof res);
    reset_to(G);
    G->rng = seed;
    brng = seed * 2654435761u + 7;
    Input start = NONE;
    start.pressed = start.held = BTN_A;
    step(G, &start);
    float t = 0;
    int think = 0, shot_wave = 0, last_wave = 0;
    while (t < 2400.0f && G->state == ST_PLAY) {
        if (++think >= (skill == 1 ? 90 : 20)) { think = 0; bot_think(G, skill); }
        /* keen bots call waves early once they are set up */
        if (skill == 3 && !G->wave_on && G->countdown < 9.0f && G->wave > 0) {
            Input in = NONE; in.pressed = in.held = BTN_X; step(G, &in);
        }
        static Enemy prev[MAX_EN];
        int fire0 = G->fire;
        if (verbose > 1) memcpy(prev, G->en, sizeof prev);
        step(G, &NONE);
        t += 1.0f / 60.0f;
        if (verbose > 1 && G->fire < fire0)
            for (int i = 0; i < MAX_EN; i++)
                if (prev[i].alive && !G->en[i].alive && hypotf(prev[i].x - FIRE_X, prev[i].z - FIRE_Z) < 1.3f)
                    printf("      leak t=%.0f %s hp %.0f/%.0f from tile %d,%d\n", t, ENEMIES[prev[i].type].name, prev[i].hp, prev[i].maxhp, prev[i].cx, prev[i].cz);
        if (verbose && G->wave != last_wave) {
            int nt = 0;
            for (int r = 0; r < MH; r++) for (int c = 0; c < MW; c++) nt += G->tw[r][c].type >= 0;
            printf("    t=%4.0f wave %2d start: fire %2d, kills %3d, leaked %2d, gold %4d, towers %d   leaks:", t, G->wave, G->fire, G->kills, G->leaked, G->gold, nt);
            for (int k = 0; k < EN_TYPES; k++) if (G->leak_by[k]) printf(" %s %d", ENEMIES[k].name, G->leak_by[k]);
            printf("\n");
            last_wave = G->wave;
        }
        if (shots && G->wave_on && G->wave != shot_wave && G->wave_t > 9.0f && (G->wave == 3 || G->wave == 8 || G->wave == 12)) {
            char nm[64];
            snprintf(nm, sizeof nm, "%s_wave%d", tag, G->wave);
            shot(G, nm);
            shot_wave = G->wave;
        }
    }
    res.won = G->state == ST_WIN;
    res.wave = G->wave;
    res.fire = G->fire;
    res.time = t;
    res.gold_left = G->gold;
    for (int r = 0; r < MH; r++) for (int c = 0; c < MW; c++) res.towers += G->tw[r][c].type >= 0;
    if (shots) {
        char nm[64];
        snprintf(nm, sizeof nm, "%s_end", tag);
        for (int i = 0; i < 150; i++) step(G, &NONE);
        shot(G, nm);
    }
    return res;
}

/* ------------------------------------------------------------------ */
static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static void test_map(void)
{
    printf("map %d (%s)\n", LEVEL + 1, LEVELS[LEVEL].name);
    reset_to(G);
    for (int r = 0; r < MH; r++) CHECK((int)strlen(LEVELS[LEVEL].rows[r]) == MW, "row %d has %d columns", r, (int)strlen(LEVELS[LEVEL].rows[r]));
    reset_to(G);
    for (int r = 0; r < MH; r++) for (int c = 0; c < MW; c++)
        if (G->tile[r][c].kind == TL_PATH) CHECK(G->dist[r][c] > 0, "path tile (%d,%d) cannot reach the fire", c, r);
    for (int i = 0; i < G->lv->ngates; i++) {
        int c = G->lv->gates[i][0], r = G->lv->gates[i][1];
        CHECK(c == 0 || r == 0 || c == MW - 1 || r == MH - 1, "gate %d is not on the map edge", i);
        CHECK(G->dist[r][c] > 0, "gate %d (%d,%d) does not reach the fire", i, c, r);
        printf("  gate %d (%2d,%2d): %d steps to the fire\n", i, c, r, G->dist[r][c]);
    }
    int build = 0;
    for (int r = 0; r < MH; r++) for (int c = 0; c < MW; c++) build += can_build(G, c, r);
    printf("  %d buildable tiles, %d waves\n", build, G->lv->nwaves);
}

/* level 2+: place towers on the best-scoring spots, like a player would */
static void auto_build(Game *g, int n)
{
    static const int ORD[] = { TW_BLASTER, TW_MORTAR, TW_TESLA, TW_FROST, TW_PRISM, TW_MORTAR, TW_TESLA, TW_BLASTER, TW_FROST };
    for (int i = 0; i < n; i++) {
        int type = ORD[i % 9], bc = -1, br = -1;
        float bs = -1;
        for (int r = 1; r < MH - 1; r++) for (int c = 1; c < MW - 1; c++) {
            if (!can_build(g, c, r)) continue;
            float s = spot_score(g, c, r, TOWERS[type].range[0], TOWERS[type].air);
            if (s > bs) { bs = s; bc = c; br = r; }
        }
        if (bc < 0) return;
        press_at(g, bc, br, type, BTN_A);
        for (int k = 0; k < i % 3; k++) { step(g, &NONE); press_at(g, bc, br, type, BTN_A); }
    }
}

static void hand_built(Game *g)
{
    static const int T[][4] = {
        {6, 4, TW_BLASTER, 1}, {11, 5, TW_TESLA, 0}, {13, 7, TW_MORTAR, 2}, {3, 6, TW_FROST, 0}, {16, 7, TW_BLASTER, 0},
        {7, 9, TW_MORTAR, 0}, {12, 9, TW_FROST, 1}, {8, 2, TW_TESLA, 2}, {15, 5, TW_BLASTER, 2}, {11, 3, TW_PRISM, 1},
    };
    g->gold = 99999;
    if (LEVEL != 0) { auto_build(g, 9); g->gold = 340; return; }
    for (unsigned i = 0; i < sizeof T / sizeof T[0]; i++) {
        press_at(g, T[i][0], T[i][1], T[i][2], BTN_A);
        for (int k = 0; k < T[i][3]; k++) { step(g, &NONE); press_at(g, T[i][0], T[i][1], T[i][2], BTN_A); }
    }
    g->gold = 340;
}

static void screenshots(void)
{
    char nm[64];
    char p[16] = "";
    if (LEVEL) snprintf(p, sizeof p, "L%d_", LEVEL + 1);
    #define SHOT(n) do { snprintf(nm, sizeof nm, "%s%s", p, n); shot(G, nm); } while (0)
    printf("screenshots level %d\n", LEVEL + 1);
    reset_to(G);
    for (int i = 0; i < 100; i++) step(G, &NONE);
    G->storm_t = 99; G->flash_t = 10;
    SHOT("title");
    Input a = NONE; a.pressed = a.held = BTN_A;
    step(G, &a);
    for (int i = 0; i < 60; i++) step(G, &NONE);
    G->storm_t = 99; G->flash_t = 10;
    SHOT("start");
    {   /* the middle of the map, close up */
        int ocx = G->cx, ocz = G->cz;
        G->cx = FIRE_C; G->cz = FIRE_R; G->cur_x = (float)G->cx; G->cur_z = (float)G->cz;
        G->zoom = G->zoom_t = ZOOM_MAX;
        for (int i = 0; i < 60; i++) game_render(G);
        SHOT("center");
        G->zoom = G->zoom_t = 1.0f; G->cx = ocx; G->cz = ocz; G->cur_x = (float)ocx; G->cur_z = (float)ocz;
        for (int i = 0; i < 60; i++) game_render(G);
    }
    hand_built(G);
    G->cx = 9; G->cz = 9; G->sel = TW_FROST;
    for (int i = 0; i < 40; i++) step(G, &NONE);
    G->storm_t = 99; G->flash_t = 10;
    SHOT("built");
    /* jump to a busy wave */
    G->countdown = 0;
    G->wave = 6;
    for (int i = 0; i < 60 * 14; i++) { step(G, &NONE); if (G->storm_t < 1) G->storm_t = 99; if (G->fire < 5) G->fire = 20; }
    G->flash_t = 10;
    SHOT("action");
    if (G->lv->env == ENV_FUTURE || G->lv->env == ENV_SKY) {
        /* 1.21 gigawatts into the clock tower (level 7: the rainbow blast from the castle) */
        G->giga_t = 0;
        for (int i = 0; i < 8; i++) step(G, &NONE);
        SHOT("giga");
        /* the 88th kill: a time surge */
        G->mph = G->lv->env == ENV_SKY ? 76 : 87;
        for (int i = 0; i < 600 && G->mph != 0; i++) step(G, &NONE);
        for (int i = 0; i < 12; i++) step(G, &NONE);
        SHOT("surge");
    }
    G->shake = 0.6f; G->punch = 1.0f;     /* a mortar impact frame: shake, punch */
    SHOT("impact");
    G->shake = 0; G->punch = 0;
    G->zoom = G->zoom_t = 2.0f;                               /* zoomed in with R2 */
    SHOT("zoom");
    G->zoom = G->zoom_t = 1.0f;
    {
        /* a line-up of this level's enemies, close up */
        int types[16], nt = 0;
        for (int w = 0; w < G->lv->nwaves; w++)
            for (int k = 0; k < G->lv->waves[w].n; k++) {
                int ty = G->lv->waves[w].g[k].type, dup = 0;
                for (int j = 0; j < nt; j++) dup |= types[j] == ty;
                if (!dup && nt < 16) types[nt++] = ty;
            }
        Enemy keep[MAX_EN];
        memcpy(keep, G->en, sizeof keep);
        memset(G->en, 0, sizeof G->en);
        for (int j = 0; j < nt; j++) {
            Enemy *e = &G->en[j];
            const EnemyDef *d = &ENEMIES[types[j]];
            e->alive = 1; e->type = (uint8_t)types[j]; e->fly = (uint8_t)d->fly;
            e->hp = e->maxhp = d->hp; e->speed = d->speed;
            static float lx; if (j == 0) lx = FIRE_X - 3.6f;
            lx += d->size * 1.6f; e->x = lx; lx += d->size * 1.6f + 0.25f;
            e->z = FIRE_Z + 2.6f; e->y = d->fly ? 1.0f : 0.0f;
            e->ang = 1.5708f - 0.5f; e->anim = (float)j; e->prog = 99; e->spawn_t = 9; e->pulse_t = 9; e->dash_t = -5;
        }
        G->cx = (int)FIRE_X; G->cz = (int)(FIRE_Z + 2.6f); G->cur_x = (float)G->cx; G->cur_z = (float)G->cz;
        G->zoom = G->zoom_t = ZOOM_MAX;
        for (int i = 0; i < 60; i++) game_render(G);              /* let the zoom pan settle on the cursor */
        SHOT("lineup");
        G->zoom = G->zoom_t = 1.0f;
        memcpy(G->en, keep, sizeof keep);
    }
    if (G->lv->env == ENV_STORM || G->lv->env == ENV_PIRATE) {
        /* a lightning strike */
        G->storm_t = 0;
        for (int i = 0; i < 2; i++) step(G, &NONE);
        G->storm_t = 99;
        SHOT("lightning");
        for (int i = 0; i < 10; i++) step(G, &NONE);
        SHOT("lightning_after");
    }
    Input pz = NONE; pz.pressed = pz.held = BTN_PLUS;
    step(G, &pz);
    SHOT("pause");
    step(G, &pz);
    /* the boss */
    G->wave = G->lv->nwaves - 1; G->wave_on = 0; G->countdown = 0;
    for (int i = 0; i < MAX_EN; i++) G->en[i].alive = 0;
    G->gold = 0;
    for (int i = 0; i < 60 * 22; i++) { step(G, &NONE); if (G->storm_t < 1) G->storm_t = 99; if (G->fire < 5) G->fire = 20; }
    G->flash_t = 10;
    SHOT("boss");
    G->state = ST_WIN; G->end_t = 3; G->fire = 17;
    SHOT("win");
    G->state = ST_LOSE; G->end_t = 3; G->fire = 0;
    for (int i = 0; i < 120; i++) step(G, &NONE);
    SHOT("lose");
    #undef SHOT
}

static void perf(void)
{
    printf("performance level %d (native CPU; the Wii is roughly 15-25x slower)\n", LEVEL + 1);
    reset_to(G);
    Input a = NONE; a.pressed = a.held = BTN_A;
    step(G, &a);
    hand_built(G);
    /* add more towers so the frame is as busy as a late game */
    static const int X[][3] = { {4, 2, TW_BLASTER}, {13, 2, TW_TESLA}, {17, 3, TW_FROST}, {1, 4, TW_MORTAR}, {11, 7, TW_TESLA}, {5, 7, TW_BLASTER}, {16, 9, TW_MORTAR}, {3, 9, TW_BLASTER}, {11, 10, TW_FROST} };
    G->gold = 99999;
    if (LEVEL == 0) for (unsigned i = 0; i < sizeof X / sizeof X[0]; i++) press_at(G, X[i][0], X[i][1], X[i][2], BTN_A);
    else auto_build(G, LEVEL == NUM_LEVELS - 1 ? 30 : 9);     /* level 5: a crowded late game */
    G->wave = LEVEL == NUM_LEVELS - 1 ? 12 : 9; G->countdown = 0;
    double tu = 0, tr = 0;
    int maxv = 0, maxseg = 0, frames = 0, maxen = 0;
    double worst = 0, worst_u = 0;
    int worst_i = 0;
    for (int i = 0; i < 60 * 40; i++) {
        double t0 = now_ms();
        step(G, &NONE);
        double t1 = now_ms();
        game_render(G);
        double t2 = now_ms();
        tu += t1 - t0; tr += t2 - t1;
        if (i > 5 && t2 - t0 > worst) { worst = t2 - t0; worst_i = i; worst_u = t1 - t0; }   /* frame 0 builds the static scenery cache */
        if (gfx_nv > maxv) maxv = gfx_nv;
        if (gfx_nseg > maxseg) maxseg = gfx_nseg;
        int ne = 0;
        for (int k = 0; k < MAX_EN; k++) ne += G->en[k].alive;
        if (ne > maxen) maxen = ne;
        frames++;
        if (G->fire < 5) G->fire = 20;
    }
    printf("  update %.3f ms, render %.3f ms avg, worst frame %.3f ms (frame %d, update part %.3f)\n", tu / frames, tr / frames, worst, worst_i, worst_u);
    { extern int dbg_nv[8]; extern int rc_verts;
      printf("  last frame: ground %d, world %d, effects %d, rain %d, ui %d, hud %d  (static cache %d)\n", dbg_nv[0], dbg_nv[1] - dbg_nv[0],
             dbg_nv[2] - dbg_nv[1], dbg_nv[3] - dbg_nv[2], dbg_nv[4] - dbg_nv[3], gfx_nv - dbg_nv[4], rc_verts); }
    printf("  max %d vertices (%d triangles), %d blend segments, %d enemies at once\n", maxv, maxv / 3, maxseg, maxen);
    CHECK(maxv < GFX_MAX_VTX * 3 / 4, "vertex budget nearly full");
    CHECK(maxseg < GFX_MAX_SEG * 3 / 4, "segment budget nearly full");
}

static void bots(void)
{
    printf("bots level %d\n", LEVEL + 1);
    static const Bot B[] = { {"idle", 0}, {"sloppy", 1}, {"average", 2}, {"keen", 3} };
    for (unsigned b = 0; b < sizeof B / sizeof B[0]; b++) {
        int wins = 0, n = B[b].skill == 0 ? 1 : 4;
        float fire = 0, wave = 0;
        for (int s = 0; s < n; s++) {
            char tag[32];
            if (LEVEL) snprintf(tag, sizeof tag, "L%d_%s", LEVEL + 1, B[b].name); else snprintf(tag, sizeof tag, "%s", B[b].name);
            Result r = play(B[b].skill, 1000u + (uint32_t)s * 77u, s == 0 && B[b].skill >= 2, tag);
            wins += r.won; fire += (float)r.fire; wave += (float)r.wave;
            printf("  %-8s seed %d: %s wave %2d, fire %2d, %2d towers, %4d gold left, %.0fs\n", B[b].name, s,
                   r.won ? "WON " : "lost", r.wave, r.fire, r.towers, r.gold_left, r.time);
        }
        printf("  %-8s => %d/%d wins, avg wave %.1f, avg fire %.1f\n", B[b].name, wins, n, wave / (float)n, fire / (float)n);
        if (B[b].skill == 0) CHECK(wins == 0, "doing nothing should lose");
        if (B[b].skill == 2) CHECK(wins >= 3, "an average player should usually win");
        if (B[b].skill == 3) CHECK(wins >= (LEVEL == 0 ? 4 : 3), "a keen player should (almost) always win");
    }
}

static void reel(void)
{
    /* 20 s of gameplay at 30 fps for a preview video (frames as ppm, piped to ffmpeg by the script) */
    char path[512];
    reset_to(G);
    Input a = NONE; a.pressed = a.held = BTN_A;
    for (int i = 0; i < 90; i++) step(G, &NONE);
    step(G, &a);
    hand_built(G);
    G->wave = 7; G->countdown = 0; G->gold = 400;
    for (int i = 0; i < 60 * 8; i++) { step(G, &NONE); if (G->fire < 5) G->fire = 20; }
    G->storm_t = 3.5f;
    int fr = 0;
    if (G->lv->env == ENV_FUTURE || G->lv->env == ENV_SKY) {
        /* level 6: 1.21 gigawatts (level 7: the rainbow blast), a zoom on the time machine, a time surge, then the Time Train time-jumping */
        for (int i = 0; i < 60 * 22; i++) {
            if (i == 60 * 2) G->giga_t = 0;
            if (i == 60 * 5) { G->cx = FIRE_C; G->cz = FIRE_R + 1; G->zoom_t = 2.0f; G->mph = G->lv->env == ENV_SKY ? 75 : 86; }
            if (i == 60 * 10) G->zoom_t = 1.0f;
            if (i == 60 * 12) {
                for (int k = 0; k < MAX_EN; k++) G->en[k].alive = 0;
                G->wave = G->lv->nwaves - 1; G->wave_on = 0; G->countdown = 0;
                for (int k = 0; k < 60 * 45; k++) { step(G, &NONE); if (G->fire < 5) G->fire = 20; if (G->boss >= 0) G->en[G->boss].hp = G->en[G->boss].maxhp; }
                if (G->boss >= 0) {
                    G->en[G->boss].pulse_t = 2.5f;
                    G->cx = (int)G->en[G->boss].x; G->cz = (int)G->en[G->boss].z;
                    if (G->cx < 0) G->cx = 0; if (G->cz < 0) G->cz = 0; if (G->cx >= MW) G->cx = MW - 1; if (G->cz >= MH) G->cz = MH - 1;
                }
                G->zoom_t = 1.7f;
            }
            if (i == 60 * 19) G->zoom_t = 1.0f;
            step(G, &NONE);
            if (G->fire < 5) G->fire = 20;
            if (i & 1) continue;
            game_render(G);
            raster();
            snprintf(path, sizeof path, "%s/reel/f%04d.ppm", outdir, fr++);
            save_ppm(path);
        }
    } else
    if (G->lv->env == ENV_PIRATE) {
        /* level 5: the storm and the swell, a gust, a zoom in on the action, then the Kraken */
        G->gust_t = 2.5f;
        for (int i = 0; i < 60 * 24; i++) {
            if (i == 60 * 7) { G->cx = (int)FIRE_X - 4; G->cz = (int)FIRE_Z + 2; G->zoom_t = 2.0f; }
            if (i == 60 * 9) G->cx += 2;
            if (i == 60 * 11) G->zoom_t = 1.0f;
            if (i == 60 * 13) {
                /* skip ahead to the last wave, with the Kraken already halfway in */
                for (int k = 0; k < MAX_EN; k++) G->en[k].alive = 0;
                G->wave = G->lv->nwaves - 1; G->wave_on = 0; G->countdown = 0;
                for (int k = 0; k < 60 * 40; k++) { step(G, &NONE); if (G->fire < 5) G->fire = 20; if (G->boss >= 0) G->en[G->boss].hp = G->en[G->boss].maxhp; }
                if (G->boss >= 0) { G->cx = (int)G->en[G->boss].x; G->cz = (int)G->en[G->boss].z; if (G->cx < 0) G->cx = 0; if (G->cz < 0) G->cz = 0; if (G->cx >= MW) G->cx = MW - 1; if (G->cz >= MH) G->cz = MH - 1; }
                G->storm_t = 2.0f; G->gust_t = 4.0f;
            }
            if (i == 60 * 16) G->zoom_t = 1.7f;
            if (i == 60 * 21) G->zoom_t = 1.0f;
            step(G, &NONE);
            if (G->fire < 5) G->fire = 20;
            if (i & 1) continue;
            game_render(G);
            raster();
            snprintf(path, sizeof path, "%s/reel/f%04d.ppm", outdir, fr++);
            save_ppm(path);
        }
    } else
    for (int i = 0; i < 60 * 20; i++) {
        if (i == 60 * 6) { G->cx = 10; G->cz = 9; }
        if (i == 60 * 9) { G->sel = TW_TESLA; }
        if (i == 60 * 10) press_at(G, 6, 9, TW_TESLA, BTN_A);
        else step(G, &NONE);
        if (G->fire < 5) G->fire = 20;
        if (i & 1) continue;
        game_render(G);
        raster();
        snprintf(path, sizeof path, "%s/reel/f%04d.ppm", outdir, fr++);
        save_ppm(path);
    }
    printf("  reel: %d frames\n", fr);
    /* the level's music for the same length, for the preview video */
    snprintf(path, sizeof path, "%s/reel/music.wav", outdir);
    FILE *f = fopen(path, "wb");
    if (f) {
        int tr = game_music(G) < 0 ? LEVEL % MUS_TRACKS : game_music(G), n = MUS_RATE * (fr / 30), sr = MUS_RATE, br = MUS_RATE * 2, ds = n * 2, cs = 36 + ds, fmt = 16;
        short one = 1, ba = 2, bits = 16;
        fwrite("RIFF", 1, 4, f); fwrite(&cs, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f); fwrite(&fmt, 4, 1, f);
        fwrite(&one, 2, 1, f); fwrite(&one, 2, 1, f); fwrite(&sr, 4, 1, f); fwrite(&br, 4, 1, f); fwrite(&ba, 2, 1, f); fwrite(&bits, 2, 1, f);
        fwrite("data", 1, 4, f); fwrite(&ds, 4, 1, f);
        for (int i = 0; i < n; i++) { short v = music_data(tr)[i % music_len(tr)]; fwrite(&v, 2, 1, f); }
        fclose(f);
    }
}

int main(int argc, char **argv)
{
    /* usage: test <outdir> [all|reel|shots|perf|bots] [level 1..N] [v] */
    if (argc > 1) outdir = argv[1];
    G = (Game *)calloc(1, (size_t)GAME_SIZE);
    music_init();
    const char *what = argc > 2 ? argv[2] : "all";
    int only = argc > 3 ? atoi(argv[3]) - 1 : -1;
    verbose = argc > 4 ? (int)strlen(argv[4]) : 0;
    for (LEVEL = 0; LEVEL < NUM_LEVELS; LEVEL++) {
        if (only >= 0 && LEVEL != only) continue;
        if (!strcmp(what, "reel")) { reel(); continue; }
        if (!strcmp(what, "shots")) { screenshots(); continue; }
        if (!strcmp(what, "perf")) { perf(); continue; }
        if (!strcmp(what, "bots")) { bots(); continue; }
        test_map();
        screenshots();
        perf();
        bots();
    }
    printf(fails ? "\n%d FAILURES\n" : "\nall good\n", fails);
    return fails ? 1 : 0;
}
