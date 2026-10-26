/*
 * STORMFIRE - game logic: towers, enemies, waves, levels, the weather. Drawing is in draw.c.
 */
#include "game_int.h"
#include "music.h"

#include <math.h>
#include <string.h>
#include <stdio.h>

#define TAU 6.28318530718f

const int GAME_SIZE = (int)sizeof(Game);

float frand(Game *g)
{
    g->rng ^= g->rng << 13; g->rng ^= g->rng >> 17; g->rng ^= g->rng << 5;
    return (float)(g->rng >> 8) / 16777216.0f;
}
float frand2(Game *g) { return frand(g) * 2.0f - 1.0f; }

/* level 6 sounds like a pinball machine: the everyday effects swap for pinball ones */
static int pinball_sfx(int id)
{
    switch (id) {
    case SFX_POP:     return SFX_PB_BUMPER;
    case SFX_CRUNCH:  return SFX_PB_KICKER;
    case SFX_BUILD:   return SFX_PB_FLIPPER;
    case SFX_UPGRADE: return SFX_PB_COMBO;
    case SFX_SELL:    return SFX_PB_DROP;
    case SFX_ERROR:   return SFX_PB_BUZZ;
    case SFX_MOVE:    return SFX_PB_TICK;
    case SFX_SELECT:  return SFX_PB_DING;
    case SFX_WAVE:    return SFX_PB_LAUNCH;
    case SFX_CLEAR:   return SFX_PB_JACKPOT;
    case SFX_HISS:    return SFX_PB_TILT;
    case SFX_BOSS:    return SFX_PB_SCOTT;
    case SFX_WIN:     return SFX_PB_WIN;
    case SFX_LOSE:    return SFX_PB_DRAIN;
    case SFX_COIN:    return SFX_PB_DING;
    case SFX_START:   return SFX_PB_LAUNCH;
    default:          return id;
    }
}

static void snd(Game *g, int id, int pitch, float x)
{
    if (g->mute || g->snd_n >= SND_Q) return;
    if (g->lv->env == ENV_FUTURE) id = pinball_sfx(id);
    g->snd_id[g->snd_n] = id;
    g->snd_pitch[g->snd_n] = pitch;
    g->snd_pan[g->snd_n] = clampf((x - FIRE_X) / 11.0f, -0.8f, 0.8f);
    g->snd_n++;
}

int game_pop_sound(Game *g, int *pitch, float *pan)
{
    if (g->snd_n <= 0) return -1;
    int id = g->snd_id[0];
    if (pitch) *pitch = g->snd_pitch[0];
    if (pan) *pan = g->snd_pan[0];
    for (int i = 1; i < g->snd_n; i++) {
        g->snd_id[i - 1] = g->snd_id[i]; g->snd_pitch[i - 1] = g->snd_pitch[i]; g->snd_pan[i - 1] = g->snd_pan[i];
    }
    g->snd_n--;
    return id;
}

int game_speed(const Game *g) { return (g->state == ST_PLAY && g->fast) ? 2 : 1; }
int game_music(const Game *g) { return g->music_off ? -1 : g->level % MUS_TRACKS; }

/* ------------------------------------------------------------------ */
/* towers                                                              */
/* ------------------------------------------------------------------ */
int tower_cost(int type, int lvl) { return TOWERS[type].cost[lvl]; }
float tower_range(const Tower *t) { return TOWERS[t->type].range[t->lvl]; }
int tower_sell_value(const Tower *t)
{
    float v = t->spent * 0.7f * (0.4f + 0.6f * clampf(t->hp / TOWER_HP[t->lvl], 0, 1));   /* wrecked-ish towers sell for less */
    return (int)(v + 0.5f);
}
int tower_repair_cost(const Tower *t)
{
    return 5 + (int)((1.0f - clampf(t->hp / TOWER_HP[t->lvl], 0, 1)) * (float)TOWERS[t->type].cost[0] * 0.8f);
}

int can_build(const Game *g, int cx, int cz)
{
    if (cx < 0 || cz < 0 || cx >= MW || cz >= MH) return 0;
    int k = g->tile[cz][cx].kind;
    return (k == TL_WALL || k == TL_BLOCK || k == TL_LAWN) && g->tw[cz][cx].type < 0;
}

/* ------------------------------------------------------------------ */
/* effects                                                             */
/* ------------------------------------------------------------------ */
static Part *part(Game *g, int kind, float x, float y, float z)
{
    Part *p = &g->pt[g->npt_next];
    g->npt_next = (g->npt_next + 1) % MAX_PT;
    memset(p, 0, sizeof *p);
    p->kind = (uint8_t)kind; p->x = x; p->y = y; p->z = z;
    p->life = p->max = 1.0f; p->size = 0.1f; p->c = 0xFFFFFFFF;
    return p;
}

static void sparks(Game *g, float x, float y, float z, int n, uint32_t c, float spd)
{
    for (int i = 0; i < n; i++) {
        Part *p = part(g, PT_SPARK, x, y, z);
        float a = frand(g) * TAU, u = frand(g);
        p->vx = cosf(a) * spd * (0.3f + u); p->vz = sinf(a) * spd * (0.3f + u);
        p->vy = spd * (0.3f + frand(g) * 1.1f);
        p->life = p->max = 0.25f + frand(g) * 0.35f;
        p->size = 0.05f; p->c = c;
    }
}

static void debris(Game *g, float x, float y, float z, int n, uint32_t c, float size)
{
    for (int i = 0; i < n; i++) {
        Part *p = part(g, PT_DEBRIS, x, y, z);
        float a = frand(g) * TAU;
        float s = 1.0f + frand(g) * 2.2f;
        p->vx = cosf(a) * s; p->vz = sinf(a) * s; p->vy = 2.0f + frand(g) * 3.0f;
        p->life = p->max = 0.7f + frand(g) * 0.6f;
        p->size = size * (0.6f + frand(g) * 0.6f);
        p->rot = frand(g) * TAU; p->vr = frand2(g) * 12.0f;
        p->c = c;
    }
}

static void smoke(Game *g, float x, float y, float z, int n, float size, uint32_t c)
{
    for (int i = 0; i < n; i++) {
        Part *p = part(g, PT_SMOKE, x + frand2(g) * 0.2f, y, z + frand2(g) * 0.2f);
        p->vx = frand2(g) * 0.4f; p->vz = frand2(g) * 0.4f; p->vy = 0.4f + frand(g) * 0.6f;
        p->life = p->max = 0.8f + frand(g) * 0.8f;
        p->size = size * (0.7f + frand(g) * 0.6f);
        p->c = c;
    }
}

static void ring(Game *g, float x, float y, float z, float r0, float r1, float life, uint32_t c)
{
    Part *p = part(g, PT_RING, x, y, z);
    p->size = r0; p->vx = r1; p->life = p->max = life; p->c = c;
}

static void flight(Game *g, float x, float y, float z, uint32_t c, float strength, float rad, float life)
{
    int best = 0;
    for (int i = 0; i < MAX_FL; i++) {
        if (g->fl[i].t >= g->fl[i].life) { best = i; break; }
        if (g->fl[i].t / g->fl[i].life > g->fl[best].t / g->fl[best].life) best = i;
    }
    FLight *l = &g->fl[best];
    l->x = x; l->y = y; l->z = z;
    l->r = (float)(c >> 24) / 255.0f * strength;
    l->g = (float)((c >> 16) & 255) / 255.0f * strength;
    l->b = (float)((c >> 8) & 255) / 255.0f * strength;
    l->rad = rad; l->t = 0; l->life = life;
}

static void popup(Game *g, float x, float y, float z, const char *txt, uint32_t c)
{
    int best = 0;
    for (int i = 0; i < MAX_POP; i++) { if (g->pop[i].t > g->pop[best].t) best = i; }
    for (int i = 0; i < MAX_POP; i++) if (g->pop[i].t >= 1.0f) { best = i; break; }
    Pop *p = &g->pop[best];
    p->x = x; p->y = y; p->z = z; p->t = 0; p->c = c;
    snprintf(p->txt, sizeof p->txt, "%s", txt);
}

static void make_arc(Game *g, const float *a, const float *b, uint32_t c, float life, int storm, float jag)
{
    for (int i = 0; i < MAX_ARC; i++) {
        Arc *r = &g->arc[i];
        if (r->alive) continue;
        r->alive = 1; r->storm = (uint8_t)storm; r->t = 0; r->life = life; r->c = c;
        r->n = storm ? ARC_PTS : 7;
        for (int k = 0; k < r->n; k++) {
            float u = (float)k / (float)(r->n - 1);
            float j = (k == 0 || k == r->n - 1) ? 0.0f : jag;
            r->p[k][0] = a[0] + (b[0] - a[0]) * u + frand2(g) * j;
            r->p[k][1] = a[1] + (b[1] - a[1]) * u + frand2(g) * j * 0.6f;
            r->p[k][2] = a[2] + (b[2] - a[2]) * u + frand2(g) * j;
        }
        return;
    }
}

/* ------------------------------------------------------------------ */
/* enemies                                                             */
/* ------------------------------------------------------------------ */
static const float WAVE1 = 1.0f;

static float wave_hpmul(const Game *g)
{
    int w = g->wave - 1;
    if (w < 0) w = 0;
    if (w >= g->lv->nwaves) w = g->lv->nwaves - 1;
    return g->lv->waves[w].hpmul * WAVE1;
}

/* ghosts fade out of reach for a moment; a collapsed skeleton is just a heap of bones */
static int untouchable(const Enemy *e)
{
    return ((e->type == EN_GHOST || e->type == EN_SHARK || e->type == EN_KRAKEN || e->type == EN_HOLOSHARK || e->type == EN_TIMETRAIN) && e->dash_t > 0)
           || e->down_t > 0;
}

static void set_target(Game *g, Enemy *e)
{
    /* walk to the centre of tile (cx, cz), shifted sideways by the enemy's lane */
    float tx = (float)e->cx + 0.5f, tz = (float)e->cz + 0.5f;
    float dx = tx - e->x, dz = tz - e->z, l = sqrtf(dx * dx + dz * dz);
    if (g->tile[e->cz][e->cx].kind == TL_FIRE) {
        /* last stretch: stop at the stone ring round the fire */
        float fx = e->x - FIRE_X, fz = e->z - FIRE_Z, fl = sqrtf(fx * fx + fz * fz);
        if (fl < 0.01f) fl = 0.01f;
        e->tx = FIRE_X + fx / fl * 0.6f; e->tz = FIRE_Z + fz / fl * 0.6f;
        return;
    }
    if (l > 0.01f) { tx += -dz / l * e->lane; tz += dx / l * e->lane; }
    e->tx = tx; e->tz = tz;
}

static void pinball_reset(Game *g)
{
    g->score = 0; g->combo = 0; g->combo_t = 0; g->mph = 0; g->surge_t = 0; g->giga_t = 22.0f; g->giga_fx = 0;
    g->warp_cd = 0; g->tilt_cd = 0; g->dmd_t = 99.0f;
    for (int i = 0; i < MAX_TRAIL; i++) g->trail[i].t = g->trail[i].life = 0;
}

/* level 6: the dot-matrix display shows a callout */
static void dmd(Game *g, const char *l1, const char *l2, uint32_t c)
{
    snprintf(g->dmd[0], sizeof g->dmd[0], "%s", l1);
    snprintf(g->dmd[1], sizeof g->dmd[1], "%s", l2 ? l2 : "");
    g->dmd_t = 0; g->dmd_c = c;
}

static void add_trail(Game *g, float x0, float z0, float x1, float z1, float life)
{
    int slot = 0;
    for (int i = 0; i < MAX_TRAIL; i++) if (g->trail[i].t >= g->trail[i].life) { slot = i; break; } else if (g->trail[i].t > g->trail[slot].t) slot = i;
    Trail *t = &g->trail[slot];
    t->x0 = x0; t->z0 = z0; t->x1 = x1; t->z1 = z1; t->t = 0; t->life = life;
    /* flames licking up along it */
    for (int k = 0; k < 8; k++) {
        float u = (float)k / 7.0f;
        for (int s2 = -1; s2 <= 1; s2 += 2) {
            float dx = x1 - x0, dz = z1 - z0, l = sqrtf(dx * dx + dz * dz) + 1e-4f;
            Part *p = part(g, PT_FIRE, x0 + dx * u - dz / l * 0.17f * (float)s2, 0.06f, z0 + dz * u + dx / l * 0.17f * (float)s2);
            p->vy = 0.4f + frand(g) * 0.5f; p->life = p->max = 0.35f + frand(g) * 0.3f; p->size = 0.12f;
        }
    }
}

/* level 6: something arrives from another time - a white flash, a shock ring, sparks and two trails of fire */
static void time_arrive(Game *g, float x, float z, float dx, float dz, int big, float y)
{
    float k = big ? 1.6f : 1.0f;
    flight(g, x, y + 0.5f, z, 0xB0D8FFFF, 1.6f * k, 3.0f * k, 0.35f);
    ring(g, x, 0.05f, z, 0.1f, 1.3f * k, 0.45f, 0xA0D0FFE0);
    sparks(g, x, y + 0.4f, z, big ? 26 : 12, 0xD0F0FFFF, 3.0f);
    Part *p = part(g, PT_GLOW, x, y + 0.4f, z);
    p->life = p->max = 0.3f; p->size = 0.6f * k; p->c = 0xE0F4FFFF;
    if (y < 0.5f) add_trail(g, x, z, x + dx * 2.2f, z + dz * 2.2f, 2.2f);
    if (g->warp_cd <= 0) { snd(g, SFX_PB_WARP, 118 + (int)(frand(g) * 20), x); g->warp_cd = 0.45f; }
    if (big) g->shake = fmaxf(g->shake, 0.5f);
}

/* jam the towers within `rad` for `dur` seconds (they are never damaged) */
static int jam_towers(Game *g, float x, float z, float rad, float dur, uint32_t spark)
{
    int hit = 0;
    for (int r = 0; r < MH; r++) for (int c = 0; c < MW; c++) {
        Tower *t = &g->tw[r][c];
        if (t->type < 0) continue;
        float dx = (float)c + 0.5f - x, dz = (float)r + 0.5f - z;
        if (dx * dx + dz * dz > rad * rad) continue;
        t->stun_t = fmaxf(t->stun_t, dur);
        t->hurt_t = 0.4f;
        sparks(g, (float)c + 0.5f, g->tile[r][c].h + 0.5f, (float)r + 0.5f, 8, spark, 2.0f);
        hit++;
    }
    return hit;
}

static void spawn_enemy(Game *g, int type, int gate)
{
    int slot = -1;
    for (int i = 0; i < MAX_EN; i++) if (!g->en[i].alive) { slot = i; break; }
    if (slot < 0) return;
    Enemy *e = &g->en[slot];
    memset(e, 0, sizeof *e);
    const EnemyDef *d = &ENEMIES[type];
    e->alive = 1; e->type = (uint8_t)type; e->fly = (uint8_t)d->fly;
    e->maxhp = e->hp = d->hp * wave_hpmul(g);
    e->speed = d->speed * (0.94f + frand(g) * 0.12f);
    e->anim = frand(g) * 10.0f;
    e->lane = frand2(g) * (d->boss ? 0.0f : 0.17f);
    if (d->boss) g->boss = slot;
    if (type == EN_FOX) e->dash_t = -(0.5f + frand(g) * 2.0f);
    if (d->summon_n > 0) e->spawn_t = d->summon_every;
    if (type == EN_GHOST) e->dash_t = -(1.0f + frand(g) * 1.5f);
    if (type == EN_SPRINGER) e->dash_t = -(0.3f + frand(g) * 0.6f);
    if (type == EN_TINKER) e->heal_t = 1.5f + frand(g);
    if (type == EN_ARCHITECT) e->pulse_t = 6.0f;
    if (type == EN_SHARK) e->dash_t = -(1.5f + frand(g) * 2.0f);
    if (type == EN_KRAKEN) e->dash_t = -7.0f;
    if (type == EN_SHIP) e->pulse_t = 3.0f + frand(g) * 2.0f;
    if (type == EN_HOVERCAR) e->pulse_t = 4.0f + frand(g) * 2.0f;
    if (type == EN_HOLOSHARK) e->dash_t = -(1.5f + frand(g) * 1.5f);
    if (type == EN_TIMETRAIN) e->pulse_t = 9.0f;
    e->smash_t = 1.0f;
    if (d->fly) {
        float a = frand(g) * TAU;
        e->x = FIRE_X + cosf(a) * (float)MW * 0.66f; e->z = FIRE_Z + sinf(a) * (float)MH * 0.66f;
        e->y = type == EN_CROW ? 1.6f : (type == EN_GHOST ? 1.0f : 1.3f);
        e->tx = FIRE_X; e->tz = FIRE_Z;
        e->ang = atan2f(FIRE_Z - e->z, FIRE_X - e->x);
        e->prog = 20.0f;
        if (g->lv->env == ENV_FUTURE) time_arrive(g, e->x, e->z, cosf(e->ang), sinf(e->ang), 0, e->y);
        return;
    }
    int gc = g->lv->gates[gate][0], gr = g->lv->gates[gate][1];
    float ox = 0, oz = 0;
    if (gr == 0) oz = -1; else if (gr == MH - 1) oz = 1; else if (gc == 0) ox = -1; else ox = 1;
    e->x = (float)gc + 0.5f + ox * 1.8f; e->z = (float)gr + 0.5f + oz * 1.8f;
    e->entering = 1;
    e->cx = gc; e->cz = gr;
    e->tx = (float)gc + 0.5f; e->tz = (float)gr + 0.5f;
    e->ang = atan2f(-oz, -ox);
    e->prog = (float)g->dist[gr][gc] + 2.0f;
    if (g->lv->env == ENV_FUTURE) time_arrive(g, e->x, e->z, -ox, -oz, d->boss || type == EN_HOVERCAR, 0.3f);
}

static void leak(Game *g, Enemy *e, int idx)
{
    int dmg = ENEMIES[e->type].damage;
    g->fire -= dmg;
    g->leaked += dmg;
    g->leak_by[e->type] += dmg;
    g->fire_hit = 1.0f;
    g->fire_flash = 1.0f;
    g->shake = fmaxf(g->shake, dmg >= 10 ? 0.9f : 0.3f);
    e->alive = 0;
    if (idx == g->boss) g->boss = -1;
    if (g->lv->env == ENV_FUTURE) {
        /* they reached the time machine: sparks fly from the flux capacitor - TILT! */
        sparks(g, FIRE_X, 0.7f, FIRE_Z, 18, 0x90D0FFFF, 2.5f);
        ring(g, FIRE_X, 0.1f, FIRE_Z, 0.2f, 1.6f, 0.4f, 0xFF5050D0);
        flight(g, FIRE_X, 0.8f, FIRE_Z, 0xFF6060FF, 1.2f, 3.0f, 0.3f);
        g->combo = 0; g->combo_t = 0;
        if (g->tilt_cd <= 0) { dmd(g, "TILT!", NULL, 0xFF4040FF); snd(g, SFX_HISS, 128, e->x); g->tilt_cd = 3.0f; }
    } else if (g->lv->env == ENV_PIRATE) {
        /* plunder: a fistful of gold flies out of the chest */
        for (int i = 0; i < 10 + dmg; i++) {
            Part *p = part(g, PT_DEBRIS, FIRE_X + frand2(g) * 0.2f, 0.6f, FIRE_Z + frand2(g) * 0.2f);
            p->vx = frand2(g) * 1.6f; p->vz = frand2(g) * 1.6f; p->vy = 2.0f + frand(g) * 1.5f;
            p->life = p->max = 0.8f + frand(g) * 0.4f; p->size = 0.05f; p->rot = frand(g) * 6.0f; p->vr = frand2(g) * 12.0f; p->c = 0xFFD040FF;
        }
        sparks(g, FIRE_X, 0.6f, FIRE_Z, 12, 0xFFE080FF, 2.0f);
        snd(g, SFX_SELL, 100 + (int)(frand(g) * 20), e->x);
        snd(g, SFX_SPLASH, 130, e->x);
    } else {
    for (int i = 0; i < 6; i++) {
        Part *p = part(g, PT_STEAM, FIRE_X + frand2(g) * 0.4f, 0.4f, FIRE_Z + frand2(g) * 0.4f);
        p->vx = frand2(g) * 0.6f; p->vz = frand2(g) * 0.6f; p->vy = 1.2f + frand(g);
        p->life = p->max = 1.0f + frand(g) * 0.7f; p->size = 0.35f + frand(g) * 0.3f; p->c = 0xC8D0E0FF;
    }
    sparks(g, e->x, 0.5f, e->z, 10, 0x8098FFFF, 2.5f);
    snd(g, SFX_HISS, 118 + (int)(frand(g) * 20), e->x);
    }
    char t[12];
    snprintf(t, sizeof t, "-%d", dmg);
    popup(g, FIRE_X, 1.6f, FIRE_Z, t, 0xFF5050FF);
    if (g->fire <= 0) {
        g->fire = 0;
        g->state = ST_LOSE;
        g->end_t = 0;
        smoke(g, FIRE_X, 0.4f, FIRE_Z, 14, 0.7f, 0x707888FF);
        snd(g, SFX_LOSE, 128, FIRE_X);
    }
}

/* a small enemy that pops out of a bigger one (slime splitting, pumpkin king dropping gourds) */
static void spawn_child(Game *g, const Enemy *parent, int type)
{
    Enemy pc = *parent;                  /* the parent's slot may be the one we reuse */
    const Enemy *p = &pc;
    int slot = -1;
    for (int i = 0; i < MAX_EN; i++) if (!g->en[i].alive) { slot = i; break; }
    if (slot < 0) return;
    Enemy *e = &g->en[slot];
    memset(e, 0, sizeof *e);
    const EnemyDef *d = &ENEMIES[type];
    e->alive = 1; e->type = (uint8_t)type;
    e->maxhp = e->hp = d->hp * wave_hpmul(g);
    e->speed = d->speed * (0.94f + frand(g) * 0.12f);
    e->anim = frand(g) * 10.0f;
    e->lane = frand2(g) * 0.2f;
    /* pop out a little behind the parent (back the way it came), so splits near the fire stay fair */
    float bx = p->x - p->tx, bz = p->z - p->tz, bl = sqrtf(bx * bx + bz * bz);
    if (bl > 0.01f) { bx /= bl; bz /= bl; } else { bx = bz = 0; }
    e->x = p->x + bx * 0.55f + frand2(g) * 0.2f; e->z = p->z + bz * 0.55f + frand2(g) * 0.2f;
    e->cx = p->cx; e->cz = p->cz; e->entering = p->entering;
    e->ang = p->ang + frand2(g) * 0.8f;
    e->prog = p->prog;
    e->inv_t = 0.15f;
    e->hit_t = 0.2f;
    if (d->fly) {                       /* a flyer (the Time Train's drones) heads straight for the middle */
        e->fly = 1; e->y = 1.2f; e->entering = 0;
        e->tx = FIRE_X; e->tz = FIRE_Z;
        e->ang = atan2f(FIRE_Z - e->z, FIRE_X - e->x);
        e->prog = 20.0f;
        return;
    }
    if (e->entering) { e->tx = p->tx; e->tz = p->tz; } else set_target(g, e);
}

/* level 6: the time machine's flux capacitor surges - a temporal shockwave that slows everything and stings it */
static void time_surge(Game *g)
{
    g->surge_t = 1.6f;
    for (int i = 0; i < MAX_EN; i++) {
        Enemy *o = &g->en[i];
        if (!o->alive) continue;
        o->slow_t = 4.0f; o->slow_amt = ENEMIES[o->type].boss ? 0.3f : 0.6f;
        o->hp -= o->maxhp * (ENEMIES[o->type].boss ? 0.03f : 0.12f);
        if (o->hp < 1.0f) o->hp = 1.0f;
        o->hit_t = 0.2f;
    }
    g->gold += 88; g->gold_flash = 1.0f;
    g->score += 880000;
    ring(g, FIRE_X, 0.1f, FIRE_Z, 0.3f, (float)MW * 0.7f, 1.3f, 0x90D8FFE0);
    ring(g, FIRE_X, 0.6f, FIRE_Z, 0.2f, (float)MW * 0.5f, 1.0f, 0xFFFFFFB0);
    flight(g, FIRE_X, 1.0f, FIRE_Z, 0xA0D0FFFF, 3.0f, 8.0f, 0.8f);
    sparks(g, FIRE_X, 0.6f, FIRE_Z, 40, 0xC0E8FFFF, 4.0f);
    for (int k = 0; k < 4; k++) {
        float a = (float)k * 1.5708f + 0.4f;
        add_trail(g, FIRE_X + cosf(a) * 1.3f, FIRE_Z + sinf(a) * 1.3f, FIRE_X + cosf(a) * 4.0f, FIRE_Z + sinf(a) * 4.0f, 2.5f);
    }
    g->shake = fmaxf(g->shake, 0.9f);
    g->punch = 1.0f;
    dmd(g, "88 MPH!", "TIME SURGE +88", 0xFFA030FF);
    snd(g, SFX_PB_SURGE, 128, FIRE_X);
}

static void pinball_kill(Game *g, const Enemy *e, const EnemyDef *d)
{
    g->combo = g->combo_t > 0 ? g->combo + 1 : 1;
    g->combo_t = 1.6f;
    int mult = 1 + g->combo / 5;
    g->score += (long)d->bounty * 1000L * mult;
    /* a bumper flash where it went */
    ring(g, e->x, 0.06f, e->z, 0.05f, 0.7f + d->size, 0.25f, (g->combo & 1) ? 0xFF60D0FF : 0x60F0FFFF);
    Part *p = part(g, PT_GLOW, e->x, e->y + 0.4f, e->z);
    p->life = p->max = 0.18f; p->size = 0.45f; p->c = 0xFFF0C0FF;
    if (g->combo >= 5 && g->combo % 5 == 0) {
        char l2[48];
        snprintf(l2, sizeof l2, "x%d  +%d GOLD", mult, g->combo / 5 * 5);
        dmd(g, "COMBO!", l2, 0xFF60D0FF);
        g->gold += g->combo / 5 * 5; g->gold_flash = 1.0f;
        snd(g, SFX_PB_COMBO, 120 + (g->combo / 5) * 4, e->x);
    }
    if (++g->mph >= 88) { g->mph = 0; time_surge(g); }
}

static void hurt(Game *g, int idx, float dmg);

/* level 6: lightning strikes the clock tower - 1.21 gigawatts arc out to the visitors */
static void giga_strike(Game *g)
{
    int cc = -1, cr = -1;
    for (int r = 0; r < MH; r++) for (int c = 0; c < MW; c++)
        if (g->tile[r][c].kind == TL_GRAVE && g->tile[r][c].color == DECO_CLOCK) { cc = c; cr = r; }
    if (cc < 0) return;
    float tx = (float)cc + 0.5f, tz = (float)cr + 0.5f, ty = 2.75f;
    float top[3] = { tx - 1.5f, 12.0f, tz - 5.0f }, rod[3] = { tx, ty, tz };
    make_arc(g, top, rod, 0xF0F4FFFF, 0.6f, 1, 0.9f);
    float mid[3] = { tx - 0.8f, 7.0f, tz - 2.5f }, br[3] = { tx + 1.5f, 5.0f, tz - 3.0f };
    make_arc(g, mid, br, 0xC8D8FFFF, 0.4f, 1, 0.5f);
    int zapped = 0;
    for (int k = 0; k < 6; k++) {
        int best = -1; float bd = 1e9f;
        for (int i = 0; i < MAX_EN; i++) {
            Enemy *o = &g->en[i];
            if (!o->alive || o->entering || untouchable(o) || o->zap_t > 0.5f) continue;
            float dx = o->x - tx, dz = o->z - tz, dd = dx * dx + dz * dz + frand(g) * 30.0f;
            if (dd < bd) { bd = dd; best = i; }
        }
        if (best < 0) break;
        Enemy *o = &g->en[best];
        float to[3] = { o->x, o->y + 0.4f, o->z };
        make_arc(g, rod, to, 0xB8E0FFFF, 0.5f, 0, 0.6f);
        o->zap_t = 0.6f;
        sparks(g, o->x, o->y + 0.3f, o->z, 10, 0xD0F0FFFF, 2.5f);
        hurt(g, best, o->maxhp * (ENEMIES[o->type].boss ? 0.04f : 0.3f));
        zapped++;
    }
    sparks(g, tx, ty, tz, 30, 0xE0F0FFFF, 3.5f);
    ring(g, tx, 0.05f, tz, 0.2f, 2.5f, 0.6f, 0xC0D8FFE0);
    flight(g, tx, ty, tz, 0xD0E0FFFF, 3.0f, 6.0f, 0.7f);
    g->strike_x = tx; g->strike_z = tz;
    g->flash_t = 0;
    g->giga_fx = 2.0f;
    g->shake = fmaxf(g->shake, 0.8f);
    g->thunder_t = 0.15f; g->thunder_big = 1;
    g->score += 121000L * (zapped + 1);
    dmd(g, "1.21", "GIGAWATTS!", 0x80D8FFFF);
    snd(g, SFX_PB_GIGA, 128, tx);
}

static void hurt(Game *g, int idx, float dmg)
{
    Enemy *e = &g->en[idx];
    if (!e->alive || e->inv_t > 0 || untouchable(e)) return;
    float arm = ENEMIES[e->type].armor + (e->shield_t > 0 ? 7.0f : 0.0f);   /* brass moth shields add armour */
    if (arm > 0) {
        float d2 = dmg - arm;
        if (d2 < dmg * 0.4f) d2 = dmg * 0.4f;
        if (d2 < dmg - 0.01f) {
            e->armor_t = 0.15f;
            if (frand(g) < 0.5f) sparks(g, e->x, e->y + 0.35f, e->z, 2, e->shield_t > 0 ? 0x80D0FFFF : 0xE8E8F0FF, 1.6f);
        }
        dmg = d2;
    }
    e->hp -= dmg;
    e->hit_t = 0.12f;
    if (e->hp > 0) return;
    if ((e->type == EN_SKELETON || e->type == EN_SKULLPIRATE) && !e->revived) {
        /* falls apart... and pulls itself back together a moment later */
        e->revived = 1;
        e->hp = 0;
        e->down_t = 2.6f;
        debris(g, e->x, 0.3f, e->z, 5, 0xE8E0CCFF, 0.08f);
        if (e->type == EN_SKULLPIRATE) ring(g, e->x, 0.04f, e->z, 0.1f, 0.7f, 0.5f, 0xC8E0F080);
        snd(g, SFX_CRUNCH, 170 + (int)(frand(g) * 20), e->x);
        return;
    }
    e->alive = 0;
    g->kills++;
    const EnemyDef *d = &ENEMIES[e->type];
    if (g->lv->env == ENV_FUTURE) pinball_kill(g, e, d);
    for (int k = 0; k < d->split_n; k++) spawn_child(g, e, d->split_type);
    g->gold += d->bounty;
    g->gold_flash = 1.0f;
    char t[12];
    snprintf(t, sizeof t, "+%d", d->bounty);
    popup(g, e->x, e->y + 0.9f, e->z, t, 0xFFD860FF);
    float s = d->size;
    int big = d->boss || e->type == EN_BRUTE || e->type == EN_BEETLE || e->type == EN_BREAKER || e->type == EN_ZOMBIE || e->type == EN_SENTINEL || e->type == EN_AUTOMATON
              || e->type == EN_SHIP || e->type == EN_ROWBOAT;
    if (g->lv->env == ENV_PIRATE && !e->fly) {
        /* goes down with a splash */
        for (int k = 0; k < (big ? 10 : 5); k++) {
            Part *p = part(g, PT_SPLASH, e->x + frand2(g) * s, 0.05f, e->z + frand2(g) * s);
            p->vy = 1.5f + frand(g) * 2.0f; p->vx = frand2(g) * 1.2f; p->vz = frand2(g) * 1.2f;
            p->life = p->max = 0.5f + frand(g) * 0.3f; p->size = 0.12f + frand(g) * 0.1f; p->c = 0xC8DCF0B0;
        }
        ring(g, e->x, 0.04f, e->z, 0.2f, s * 3.4f, 0.8f, 0xB0D0F0A0);
        snd(g, SFX_SPLASH, big ? 100 : 140, e->x);
    }
    if (big) g->shake = fmaxf(g->shake, d->boss ? 1.0f : (e->type == EN_BREAKER || e->type == EN_SENTINEL ? 0.55f : 0.32f));
    else g->shake = fmaxf(g->shake, 0.16f);
    if (d->boss) g->punch = 1.0f;
    debris(g, e->x, e->y + s * 0.6f, e->z, d->boss ? 26 : (big ? 12 : 6), d->color, s * 0.35f);
    sparks(g, e->x, e->y + s * 0.6f, e->z, d->boss ? 40 : 10, 0xFFD080FF, 2.6f);
    flight(g, e->x, e->y + 0.5f, e->z, 0xFFB060FF, big ? 1.6f : 0.9f, d->boss ? 5.0f : 2.0f, 0.35f);
    ring(g, e->x, e->y + 0.03f, e->z, 0.1f, s * 2.6f, 0.4f, 0xFFC08080);
    if (d->boss) {
        g->shake = 1.0f;
        smoke(g, e->x, 0.4f, e->z, 12, 0.8f, 0x50586090);
        snd(g, SFX_BOOM, 80, e->x);
        snd(g, SFX_CRUNCH, 90, e->x);
        g->boss = -1;
    } else if (big) {
        snd(g, SFX_CRUNCH, 120 + (int)(frand(g) * 16), e->x);
    } else {
        snd(g, SFX_POP, 110 + (int)(frand(g) * 40), e->x);
    }
    if (idx == g->boss) g->boss = -1;
}

/* a smasher's blow lands on the tower at (c, r); at zero health the tower is wrecked */
static void tower_smash(Game *g, const Enemy *e, int c, int r, float dmg)
{
    Tower *t = &g->tw[r][c];
    if (t->type < 0) return;
    float x = (float)c + 0.5f, z = (float)r + 0.5f, h = g->tile[r][c].h;
    int boss = ENEMIES[e->type].boss;
    t->hurt_t = 0.35f;                    /* towers can't be destroyed any more: the blow just rattles it */
    debris(g, x, h + 0.5f, z, 4, 0x8C8C9CFF, 0.08f);
    sparks(g, x, h + 0.5f, z, 8, 0xFFD8A0FF, 2.0f);
    ring(g, e->x + cosf(e->ang) * 0.6f, 0.04f, e->z + sinf(e->ang) * 0.6f, 0.1f, boss ? 1.6f : 0.9f, 0.35f, 0xC8B090C0);
    g->shake = fmaxf(g->shake, boss ? 0.75f : (dmg >= 30 ? 0.55f : 0.32f));
    if (boss || dmg >= 30) g->punch = 1.0f;
    snd(g, SFX_CRUNCH, 140 + (int)(frand(g) * 20), x);
}

static void update_enemies(Game *g, float dt)
{
    /* brass moths shield everything near them */
    for (int i = 0; i < MAX_EN; i++) if (g->en[i].alive) { g->en[i].shield_t -= dt; g->en[i].heal_fx -= dt; }
    for (int m = 0; m < MAX_EN; m++) {
        const Enemy *mo = &g->en[m];
        if (!mo->alive || mo->type != EN_MOTH) continue;
        for (int i = 0; i < MAX_EN; i++) {
            Enemy *o = &g->en[i];
            if (!o->alive || i == m || o->type == EN_MOTH) continue;
            float dx = o->x - mo->x, dz = o->z - mo->z;
            if (dx * dx + dz * dz < 1.7f * 1.7f) o->shield_t = 0.25f;
        }
    }
    for (int i = 0; i < MAX_EN; i++) {
        Enemy *e = &g->en[i];
        if (!e->alive) continue;
        e->hit_t -= dt; e->zap_t -= dt; e->armor_t -= dt; e->inv_t -= dt;
        if (e->slow_t > 0) { e->slow_t -= dt; if (e->slow_t <= 0) e->slow_amt = 0; }
        float spd = e->speed * (1.0f - e->slow_amt);
        if (e->type == EN_TIMETRAIN && e->dash_t > 0) spd = e->speed * 9.0f;      /* mid time-jump */
        if (e->type == EN_FOX) {
            /* foxes trot, then sprint for a moment */
            if (e->dash_t > 0) {
                e->dash_t -= dt;
                spd *= 2.6f;
                if (frand(g) < dt * 20.0f) {
                    Part *p = part(g, PT_SMOKE, e->x, 0.05f, e->z);
                    p->vy = 0.2f; p->life = p->max = 0.4f; p->size = 0.12f; p->c = 0xB89A7090;
                }
                if (e->dash_t <= 0) e->dash_t = -(1.8f + frand(g) * 1.2f);
            } else {
                e->dash_t += dt;
                if (e->dash_t >= 0) e->dash_t = 0.55f;
            }
        }
        const EnemyDef *ed = &ENEMIES[e->type];
        if (ed->summon_n > 0 && !e->entering) {
            /* the kings drop minions as they go (gourds, or zombies clawing out of the ground) */
            e->spawn_t -= dt;
            if (e->spawn_t <= 0) {
                e->spawn_t = ed->summon_every;
                for (int k = 0; k < ed->summon_n; k++) spawn_child(g, e, ed->summon_type);
                sparks(g, e->x, 0.6f, e->z, 12, e->type == EN_GHOULKING ? 0x90FF80FF : 0xFFB040FF, 2.0f);
                debris(g, e->x, 0.1f, e->z, 6, 0x4A3A2AFF, 0.08f);
                snd(g, e->type == EN_GHOULKING ? SFX_CRUNCH : SFX_POP, 80, e->x);
            }
        }
        if (e->type == EN_SPRINGER) {
            /* crouch... then a big hop down the path */
            if (e->dash_t > 0) {
                e->dash_t -= dt;
                spd *= 3.2f;
                e->y = 0.38f * sinf(3.14159f * clampf(1.0f - e->dash_t / 0.45f, 0, 1));
                if (e->dash_t <= 0) {
                    e->dash_t = -(0.45f + frand(g) * 0.3f); e->y = 0;
                    Part *p = part(g, PT_SMOKE, e->x, 0.05f, e->z);
                    p->vy = 0.25f; p->life = p->max = 0.4f; p->size = 0.14f; p->c = 0xB0A08880;
                }
            } else {
                spd = 0;
                e->dash_t += dt;
                if (e->dash_t >= 0) e->dash_t = 0.45f;
            }
        }
        if (e->type == EN_TINKER) {
            /* repair pulse: patches up everything nearby */
            e->heal_t -= dt;
            if (e->heal_t <= 0) {
                e->heal_t = 2.4f;
                int healed = 0;
                for (int k = 0; k < MAX_EN; k++) {
                    Enemy *o = &g->en[k];
                    if (!o->alive || o->down_t > 0 || o->hp >= o->maxhp) continue;
                    float dx = o->x - e->x, dz = o->z - e->z;
                    if (dx * dx + dz * dz > 1.8f * 1.8f) continue;
                    o->hp = fminf(o->maxhp, o->hp + o->maxhp * 0.1f);
                    o->heal_fx = 0.5f;
                    healed++;
                }
                if (healed) {
                    ring(g, e->x, 0.05f, e->z, 0.2f, 1.8f, 0.5f, 0x70FF90C0);
                    snd(g, SFX_UPGRADE, 190, e->x);
                }
            }
        }
        if (e->type == EN_ARCHITECT && !e->entering) {
            /* shockwave: jams every tower close by for a few seconds */
            e->pulse_t -= dt;
            if (e->pulse_t <= 0) {
                e->pulse_t = 9.0f;
                for (int r = 0; r < MH; r++) for (int c = 0; c < MW; c++) {
                    Tower *t = &g->tw[r][c];
                    if (t->type < 0) continue;
                    float dx = (float)c + 0.5f - e->x, dz = (float)r + 0.5f - e->z;
                    if (dx * dx + dz * dz > 3.2f * 3.2f) continue;
                    t->stun_t = 2.5f;
                    sparks(g, (float)c + 0.5f, g->tile[r][c].h + 0.6f, (float)r + 0.5f, 6, 0x90E0FFFF, 1.8f);
                }
                ring(g, e->x, 0.06f, e->z, 0.3f, 3.2f, 0.7f, 0x90E0FFE0);
                ring(g, e->x, 0.9f, e->z, 0.2f, 2.4f, 0.5f, 0xC0F0FFB0);
                flight(g, e->x, 1.0f, e->z, 0x90D8FFFF, 2.0f, 4.0f, 0.5f);
                g->shake = fmaxf(g->shake, 0.7f);
                snd(g, SFX_ZAP, 70, e->x);
                snd(g, SFX_BOOM, 140, e->x);
            }
        }
        if (e->type == EN_SHIP && !e->entering) {
            /* broadside: the cannons jam every tower alongside for a moment (they never break them) */
            e->pulse_t -= dt;
            if (e->pulse_t <= 0) {
                e->pulse_t = 6.5f + frand(g) * 2.0f;
                int hit = 0;
                for (int r = 0; r < MH; r++) for (int c = 0; c < MW; c++) {
                    Tower *t = &g->tw[r][c];
                    if (t->type < 0) continue;
                    float dx = (float)c + 0.5f - e->x, dz = (float)r + 0.5f - e->z;
                    if (dx * dx + dz * dz > 2.6f * 2.6f) continue;
                    t->stun_t = fmaxf(t->stun_t, 1.6f);
                    t->hurt_t = 0.4f;
                    debris(g, (float)c + 0.5f, g->tile[r][c].h + 0.4f, (float)r + 0.5f, 4, 0x6A4A30FF, 0.06f);
                    sparks(g, (float)c + 0.5f, g->tile[r][c].h + 0.5f, (float)r + 0.5f, 8, 0xFFB050FF, 2.0f);
                    hit++;
                }
                float sx = -sinf(e->ang), sz = cosf(e->ang);
                for (int side = -1; side <= 1; side += 2) for (int k = -1; k <= 1; k++) {
                    float px = e->x + sx * side * 0.4f + cosf(e->ang) * k * 0.3f, pz = e->z + sz * side * 0.4f + sinf(e->ang) * k * 0.3f;
                    Part *p = part(g, PT_SMOKE, px, 0.45f, pz);
                    p->vx = sx * side * 1.2f; p->vz = sz * side * 1.2f; p->vy = 0.4f;
                    p->life = p->max = 1.1f; p->size = 0.25f; p->c = 0xA0A0A0A0;
                    Part *f = part(g, PT_FIRE, px, 0.45f, pz);
                    f->vx = sx * side * 2.0f; f->vz = sz * side * 2.0f;
                    f->life = f->max = 0.18f; f->size = 0.2f; f->c = 0xFFC060FF;
                }
                flight(g, e->x, 0.8f, e->z, 0xFFB060FF, 1.8f, 3.0f, 0.3f);
                g->shake = fmaxf(g->shake, hit ? 0.6f : 0.4f);
                snd(g, SFX_CANNON, 120 + (int)(frand(g) * 20), e->x);
            }
        }
        if (e->type == EN_HOVERCAR && !e->entering) {
            /* sonic boom: jams every tower close by for a moment */
            e->pulse_t -= dt;
            if (e->pulse_t <= 0) {
                e->pulse_t = 6.0f + frand(g) * 2.0f;
                int hit = jam_towers(g, e->x, e->z, 2.5f, 1.6f, 0x80F0FFFF);
                ring(g, e->x, 0.08f, e->z, 0.2f, 2.5f, 0.55f, 0x70F0FFE0);
                ring(g, e->x, 0.5f, e->z, 0.1f, 1.8f, 0.4f, 0xC0FFFFB0);
                flight(g, e->x, 0.8f, e->z, 0x80F0FFFF, 1.6f, 3.0f, 0.3f);
                g->shake = fmaxf(g->shake, hit ? 0.55f : 0.35f);
                snd(g, SFX_CANNON, 150, e->x);
            }
        }
        if (e->type == EN_HOLOSHARK) {
            /* the hologram glitches out of reach for a moment, then flickers back */
            if (e->dash_t > 0) { e->dash_t -= dt; if (e->dash_t <= 0) e->dash_t = -(2.0f + frand(g) * 1.2f); }
            else { e->dash_t += dt; if (e->dash_t >= 0) { e->dash_t = 0.9f; sparks(g, e->x, 0.4f, e->z, 6, 0x80D8FFFF, 1.4f); } }
        }
        if (e->type == EN_TIMETRAIN && !e->entering) {
            /* every so often it time-jumps a few tiles ahead, leaving twin fire trails */
            if (e->dash_t > 0) {
                e->dash_t -= dt;
                if (e->dash_t <= 0) {
                    e->dash_t = 0;
                    add_trail(g, e->jx, e->jz, e->x, e->z, 3.0f);
                    time_arrive(g, e->x, e->z, 0, 0, 1, 0.6f);
                    g->shake = fmaxf(g->shake, 0.8f);
                    snd(g, SFX_BOOM, 90, e->x);
                }
            } else {
                e->pulse_t -= dt;
                if (e->pulse_t <= 0 && e->prog > 4.0f) {
                    e->pulse_t = 10.0f + frand(g) * 3.0f;
                    e->dash_t = 0.45f;
                    e->jx = e->x; e->jz = e->z;
                    time_arrive(g, e->x, e->z, 0, 0, 1, 0.6f);
                    snd(g, SFX_PB_WARP, 90, e->x);
                }
            }
        }
        if (e->type == EN_SHARK || e->type == EN_KRAKEN) {
            /* dives out of reach for a moment, then surfaces again */
            int was = e->dash_t > 0;
            if (e->dash_t > 0) { e->dash_t -= dt; if (e->dash_t <= 0) e->dash_t = e->type == EN_KRAKEN ? -(8.0f + frand(g) * 2.0f) : -(2.2f + frand(g) * 1.2f); }
            else { e->dash_t += dt; if (e->dash_t >= 0) e->dash_t = e->type == EN_KRAKEN ? 2.2f : 1.0f; }
            if (was != (e->dash_t > 0)) {
                float s = ENEMIES[e->type].size;
                ring(g, e->x, 0.04f, e->z, 0.1f, s * 2.4f, 0.6f, 0xC0DCF0A0);
                for (int k = 0; k < (e->type == EN_KRAKEN ? 14 : 4); k++) {
                    Part *p = part(g, PT_SPLASH, e->x + frand2(g) * s, 0.05f, e->z + frand2(g) * s);
                    p->vy = 1.2f + frand(g) * 1.6f; p->vx = frand2(g); p->vz = frand2(g);
                    p->life = p->max = 0.5f; p->size = 0.1f + frand(g) * 0.08f; p->c = 0xC8DCF0B0;
                }
                if (e->type == EN_KRAKEN) { g->shake = fmaxf(g->shake, 0.5f); snd(g, SFX_SPLASH, 80, e->x); }
            }
        }
        if (e->type == EN_GHOST) {
            /* fade out of reach for a moment, then back */
            if (e->dash_t > 0) { e->dash_t -= dt; if (e->dash_t <= 0) e->dash_t = -(1.8f + frand(g) * 1.0f); }
            else { e->dash_t += dt; if (e->dash_t >= 0) e->dash_t = 1.2f; }
        }
        if (e->down_t > 0) {
            /* a heap of bones: wait, then stand back up with half health */
            e->down_t -= dt;
            if (e->down_t <= 0) {
                e->hp = e->maxhp * 0.5f;
                sparks(g, e->x, 0.3f, e->z, 6, 0xE8E0CCFF, 1.2f);
                snd(g, SFX_CRUNCH, 200, e->x);
            }
            continue;
        }
        if (ed->smash > 0 && !e->entering) {
            /* smash any tower in reach: stop, wind up, slam */
            e->smash_t -= dt;
            if (e->act_t > 0) {
                e->act_t -= dt;
                if (!e->act_hit && e->act_t < 0.22f) { e->act_hit = 1; tower_smash(g, e, e->act_c, e->act_r, ed->smash); }
                continue;
            }
            if (e->smash_t <= 0) {
                int bc = -1, br = -1;
                float bd = ed->reach * ed->reach;
                int c0 = (int)floorf(e->x), r0 = (int)floorf(e->z);
                for (int r = r0 - 2; r <= r0 + 2; r++) for (int c = c0 - 2; c <= c0 + 2; c++) {
                    if (c < 0 || r < 0 || c >= MW || r >= MH || g->tw[r][c].type < 0) continue;
                    float dx = (float)c + 0.5f - e->x, dz = (float)r + 0.5f - e->z, dd = dx * dx + dz * dz;
                    if (dd < bd) { bd = dd; bc = c; br = r; }
                }
                if (bc >= 0) {
                    e->act_t = 0.6f; e->act_hit = 0; e->act_c = (int8_t)bc; e->act_r = (int8_t)br;
                    e->smash_t = ed->smash_cd;
                    e->ang = atan2f((float)br + 0.5f - e->z, (float)bc + 0.5f - e->x);
                    continue;
                }
            }
        }
        if (ed->boss || e->type == EN_BREAKER || e->type == EN_SENTINEL) {
            /* heavy footsteps */
            int n = (int)(e->anim * 0.4f);
            if (n != e->step_n) { e->step_n = n; g->shake = fmaxf(g->shake, ed->boss ? 0.32f : 0.16f); }
        }
        e->anim += dt * spd * 6.0f;
        float dx = e->tx - e->x, dz = e->tz - e->z, d = sqrtf(dx * dx + dz * dz);
        float step = spd * dt;
        if (d > 0.001f) {
            float want = atan2f(dz, dx), da = want - e->ang;
            while (da > 3.14159f) da -= TAU;
            while (da < -3.14159f) da += TAU;
            e->ang += da * clampf(dt * 10.0f, 0, 1);
        }
        if (e->fly) {
            if (e->type == EN_CROW) e->y = 1.6f + sinf(e->anim * 0.5f) * 0.3f;
            else if (e->type == EN_GHOST) e->y = 0.9f + sinf(e->anim * 0.6f) * 0.15f;
            else if (e->type == EN_BAT) e->y = 1.3f + sinf(e->anim * 1.7f) * 0.25f + sinf(e->anim * 0.7f) * 0.15f;
            else e->y = 1.3f + sinf(e->anim * 0.9f) * 0.12f;
            if (d <= 0.6f) { leak(g, e, i); continue; }
            e->x += dx / d * step; e->z += dz / d * step;
            e->prog = d;
            continue;
        }
        if (d <= step) {
            e->x = e->tx; e->z = e->tz;
            if (g->tile[e->cz][e->cx].kind == TL_FIRE) { leak(g, e, i); if (g->state != ST_PLAY) return; continue; }
            if (e->entering) e->entering = 0;
            int nc = g->nxt[e->cz][e->cx][0], nr = g->nxt[e->cz][e->cx][1];
            if (nc < 0) { leak(g, e, i); continue; }       /* cannot happen on a valid map */
            e->cx = nc; e->cz = nr;
            set_target(g, e);
        } else {
            e->x += dx / d * step; e->z += dz / d * step;
        }
        float rem = sqrtf((e->tx - e->x) * (e->tx - e->x) + (e->tz - e->z) * (e->tz - e->z));
        e->prog = (float)g->dist[e->cz][e->cx] + rem + (e->entering ? 1.0f : 0.0f);
    }
}

/* ------------------------------------------------------------------ */
/* waves                                                               */
/* ------------------------------------------------------------------ */
static void ordinal(int n, char *buf, int sz)
{
    const char *suf = "TH";
    if (n % 100 < 11 || n % 100 > 13) {
        if (n % 10 == 1) suf = "ST"; else if (n % 10 == 2) suf = "ND"; else if (n % 10 == 3) suf = "RD";
    }
    snprintf(buf, sz, "%d%s", n, suf);
}

static void start_wave(Game *g)
{
    if (g->wave >= g->lv->nwaves) return;
    g->wave++;
    g->wave_on = 1;
    g->wave_t = 0;
    memset(g->spawned, 0, sizeof g->spawned);
    g->banner_t = 0;
    snprintf(g->banner, sizeof g->banner, "WAVE %d/%d", g->wave, g->lv->nwaves);
    if (g->wave == g->lv->nwaves) snprintf(g->banner2, sizeof g->banner2, "%s", g->lv->boss_line);
    else {
        char o[8];
        ordinal(g->wave, o, sizeof o);
        snprintf(g->banner2, sizeof g->banner2, "HERE COMES THE %s WAVE!!", o);
    }
    snd(g, g->wave == g->lv->nwaves ? SFX_BOSS : SFX_WAVE, 128, FIRE_X);
    if (g->lv->env == ENV_FUTURE) {
        if (g->wave == g->lv->nwaves) dmd(g, "GREAT SCOTT!", "THE TIME TRAIN!", 0xFFC040FF);
        else { char l2[48]; snprintf(l2, sizeof l2, "WAVE %d", g->wave); dmd(g, "BALL LAUNCH", l2, 0xFFA030FF); }
    }
}

static void update_waves(Game *g, float dt)
{
    if (!g->wave_on) {
        g->countdown -= dt;
        if (g->countdown <= 0) start_wave(g);
        return;
    }
    g->wave_t += dt;
    const WaveDef *w = &g->lv->waves[g->wave - 1];
    int pending = 0;
    for (int k = 0; k < w->n; k++) {
        const Group *gr = &w->g[k];
        while (g->spawned[k] < gr->count && g->wave_t >= gr->delay + gr->gap * (float)g->spawned[k]) {
            int gate;
            if (gr->gate == GATE_CYCLE) { gate = g->gate_rr % w->gates; g->gate_rr = (g->gate_rr + 1) % w->gates; }
            else if (gr->gate == GATE_RANDOM || gr->gate == GATE_AIR) gate = (int)(frand(g) * (float)w->gates) % w->gates;
            else gate = gr->gate;
            spawn_enemy(g, gr->type, gate);
            g->spawned[k]++;
        }
        if (g->spawned[k] < gr->count) pending = 1;
    }
    if (pending) return;
    for (int i = 0; i < MAX_EN; i++) if (g->en[i].alive) return;
    /* wave cleared */
    g->wave_on = 0;
    int bonus = 15 + g->wave * 4;
    g->gold += bonus;
    g->gold_flash = 1.0f;
    if (g->lv->env == ENV_FUTURE) {
        char l2[48];
        snprintf(l2, sizeof l2, "+%d GOLD", bonus);
        g->score += 50000L * g->wave;
        dmd(g, g->wave >= g->lv->nwaves ? "SUPER JACKPOT!" : "JACKPOT!", l2, 0xFFD040FF);
        for (int k = 0; k < 24; k++) {               /* a fountain of coins from the time machine */
            Part *p = part(g, PT_DEBRIS, FIRE_X + frand2(g) * 0.3f, 0.8f, FIRE_Z + frand2(g) * 0.3f);
            p->vx = frand2(g) * 2.0f; p->vz = frand2(g) * 2.0f; p->vy = 3.0f + frand(g) * 2.0f;
            p->life = p->max = 1.0f + frand(g) * 0.4f; p->size = 0.05f; p->rot = frand(g) * 6.0f; p->vr = frand2(g) * 12.0f; p->c = 0xFFD040FF;
        }
    }
    if (g->wave >= g->lv->nwaves) {
        g->state = ST_WIN;
        g->end_t = 0;
        snd(g, SFX_WIN, 128, FIRE_X);
        return;
    }
    g->countdown = 14.0f;
    g->banner_t = 0;
    snprintf(g->banner, sizeof g->banner, "WAVE %d CLEARED", g->wave);
    snprintf(g->banner2, sizeof g->banner2, "+%d GOLD", bonus);
    snd(g, SFX_CLEAR, 128, FIRE_X);
}

/* ------------------------------------------------------------------ */
/* towers                                                              */
/* ------------------------------------------------------------------ */
static int find_target(Game *g, float x, float z, float range, int air, int skip_mask_idx)
{
    int best = -1;
    float bp = 1e9f, r2 = range * range;
    for (int i = 0; i < MAX_EN; i++) {
        const Enemy *e = &g->en[i];
        if (!e->alive || i == skip_mask_idx || untouchable(e)) continue;
        if (e->fly && !air) continue;
        if (e->entering && e->prog > 30.0f) continue;
        float dx = e->x - x, dz = e->z - z;
        if (dx * dx + dz * dz > r2) continue;
        if (e->prog < bp) { bp = e->prog; best = i; }
    }
    return best;
}

static void fire_tower(Game *g, int cx, int cz, Tower *t, int target)
{
    const TowerDef *d = &TOWERS[t->type];
    float x = (float)cx + 0.5f, z = (float)cz + 0.5f, h = g->tile[cz][cx].h;
    Enemy *e = &g->en[target];
    float dmg = d->dmg[t->lvl];
    switch (t->type) {
    case TW_BLASTER: {
        for (int s = 0; s < MAX_SH; s++) {
            Shot *b = &g->sh[s];
            if (b->alive) continue;
            memset(b, 0, sizeof *b);
            b->alive = 1; b->kind = SH_BOLT; b->target = target; b->dmg = dmg; b->lvl = t->lvl;
            float ca = cosf(t->ang), sa = sinf(t->ang);
            float side = (t->lvl == 2) ? ((g->kills + (int)(t->anim * 10)) & 1 ? 0.09f : -0.09f) : 0.0f;
            b->x = x + ca * 0.42f - sa * side; b->z = z + sa * 0.42f + ca * side; b->y = h + 0.42f;
            float ex = e->x - b->x, ey = e->y + 0.25f - b->y, ez = e->z - b->z, l = sqrtf(ex * ex + ey * ey + ez * ez) + 0.001f;
            b->vx = ex / l * 11.0f; b->vy = ey / l * 11.0f; b->vz = ez / l * 11.0f;
            b->t = 0; b->dur = 1.2f;
            break;
        }
        t->recoil = 1.0f;
        flight(g, x + cosf(t->ang) * 0.45f, h + 0.45f, z + sinf(t->ang) * 0.45f, d->glow, 0.5f, 1.4f, 0.08f);
        {   /* a brass casing flicks out of the side */
            Part *p = part(g, PT_DEBRIS, x - sinf(t->ang) * 0.12f, h + 0.45f, z + cosf(t->ang) * 0.12f);
            p->vx = -sinf(t->ang) * 1.6f + frand2(g) * 0.4f; p->vz = cosf(t->ang) * 1.6f + frand2(g) * 0.4f; p->vy = 2.0f + frand(g);
            p->life = p->max = 0.6f; p->size = 0.035f; p->rot = frand(g) * 6.0f; p->vr = 20.0f; p->c = 0xE8C060FF;
        }
        snd(g, SFX_BLASTER, 118 + (int)(frand(g) * 22) + t->lvl * 6, x);
        break;
    }
    case TW_MORTAR: {
        for (int s = 0; s < MAX_SH; s++) {
            Shot *b = &g->sh[s];
            if (b->alive) continue;
            memset(b, 0, sizeof *b);
            b->alive = 1; b->kind = SH_SHELL; b->dmg = dmg; b->splash = d->extra[t->lvl]; b->lvl = t->lvl;
            b->sx = x; b->sy = h + 0.75f; b->sz = z;
            b->dur = 1.05f;
            /* lead the target: guess where it will be when the shell lands */
            float spd = e->speed * (1.0f - e->slow_amt);
            float dx = e->tx - e->x, dz = e->tz - e->z, dl = sqrtf(dx * dx + dz * dz);
            float lead = spd * b->dur * 0.85f;
            if (dl > 0.001f) { if (lead > dl + 0.3f) lead = dl + 0.3f; b->ex = e->x + dx / dl * lead; b->ez = e->z + dz / dl * lead; }
            else { b->ex = e->x; b->ez = e->z; }
            b->x = b->sx; b->y = b->sy; b->z = b->sz;
            break;
        }
        t->recoil = 1.0f;
        smoke(g, x, h + 0.8f, z, 3, 0.25f, 0x50505070);
        flight(g, x, h + 0.9f, z, d->glow, 1.0f, 2.0f, 0.15f);
        snd(g, SFX_MORTAR, 120 + (int)(frand(g) * 16), x);
        break;
    }
    case TW_FROST: {
        float r = d->range[t->lvl], r2 = r * r, slow = d->extra[t->lvl];
        for (int i = 0; i < MAX_EN; i++) {
            Enemy *o = &g->en[i];
            if (!o->alive) continue;
            float dx = o->x - x, dz = o->z - z;
            if (dx * dx + dz * dz > r2) continue;
            if (o->slow_amt < slow) o->slow_amt = slow;
            if (ENEMIES[o->type].boss && o->slow_amt > 0.3f) o->slow_amt = 0.3f;
            o->slow_t = 1.7f;
            hurt(g, i, dmg);
        }
        ring(g, x, 0.05f, z, 0.3f, r, 0.55f, 0x90E8FFC0);
        ring(g, x, h + 0.05f, z, 0.2f, 0.9f, 0.35f, 0xD0F8FFFF);
        for (int k = 0; k < 8; k++) {                          /* ice splinters burst off the crystal */
            Part *p = part(g, PT_SHARD, x, h + 0.95f, z);
            float a = (float)k / 8.0f * 6.2832f + frand(g) * 0.5f, sp = 1.6f + frand(g) * 1.4f;
            p->vx = cosf(a) * sp; p->vz = sinf(a) * sp; p->vy = 0.6f + frand(g) * 1.2f;
            p->life = p->max = 0.5f + frand(g) * 0.3f; p->size = 0.05f + frand(g) * 0.04f; p->rot = a; p->vr = frand2(g) * 15.0f; p->c = 0xC8F4FFFF;
        }
        for (int k = 0; k < 10; k++) {
            Part *p = part(g, PT_FLAKE, x, h + 0.9f, z);
            float a = frand(g) * TAU, s = 1.0f + frand(g) * 2.0f;
            p->vx = cosf(a) * s; p->vz = sinf(a) * s; p->vy = frand2(g) * 0.5f;
            p->life = p->max = 0.6f + frand(g) * 0.4f; p->size = 0.06f; p->c = 0xD8FCFFFF;
        }
        t->pulse = 1.0f;
        flight(g, x, h + 1.0f, z, d->glow, 1.0f, r + 0.5f, 0.3f);
        snd(g, SFX_FROST, 116 + (int)(frand(g) * 20), x);
        break;
    }
    case TW_TESLA: {
        int hit[8], nh = 0, chains = (int)d->extra[t->lvl];
        int cur = target;
        float from[3] = { x, h + 1.15f, z };
        float dm = dmg;
        while (cur >= 0 && nh < chains) {
            Enemy *o = &g->en[cur];
            float to[3] = { o->x, o->y + ENEMIES[o->type].size * 0.8f, o->z };
            make_arc(g, from, to, 0xFFF4A0FF, 0.2f, 0, 0.18f);
            sparks(g, to[0], to[1], to[2], 5, 0xFFF0A0FF, 2.0f);
            o->zap_t = 0.25f;
            hit[nh++] = cur;
            memcpy(from, to, sizeof from);
            float px = o->x, pz = o->z;
            hurt(g, cur, dm);
            dm *= 0.85f;
            /* jump to the nearest enemy not hit yet */
            int nb = -1;
            float bd = 1.7f * 1.7f;
            for (int i = 0; i < MAX_EN; i++) {
                const Enemy *q = &g->en[i];
                if (!q->alive || untouchable(q)) continue;
                int used = 0;
                for (int k = 0; k < nh; k++) if (hit[k] == i) used = 1;
                if (used) continue;
                float ddx = q->x - px, ddz = q->z - pz, dd = ddx * ddx + ddz * ddz;
                if (dd < bd) { bd = dd; nb = i; }
            }
            cur = nb;
        }
        flight(g, x, h + 1.2f, z, d->glow, 1.4f, 2.6f, 0.18f);
        g->shake = fmaxf(g->shake, 0.22f);
        t->pulse = 1.0f;
        snd(g, SFX_ZAP, 112 + (int)(frand(g) * 30), x);
        break;
    }
    }
}

static void update_towers(Game *g, float dt)
{
    for (int cz = 0; cz < MH; cz++)
        for (int cx = 0; cx < MW; cx++) {
            Tower *t = &g->tw[cz][cx];
            if (t->type < 0) continue;
            const TowerDef *d = &TOWERS[t->type];
            t->anim += dt;
            t->built += dt * 2.5f; if (t->built > 1) t->built = 1;
            t->recoil -= dt * 5.0f; if (t->recoil < 0) t->recoil = 0;
            t->hurt_t -= dt; if (t->hurt_t < 0) t->hurt_t = 0;
            if (t->stun_t > 0) {                              /* jammed: no shooting, just sparks */
                t->stun_t -= dt;
                if (frand(g) < dt * 6.0f) sparks(g, (float)cx + 0.5f, g->tile[cz][cx].h + 0.7f, (float)cz + 0.5f, 2, 0x90E0FFFF, 1.2f);
                continue;
            }
            t->pulse -= dt * 2.5f; if (t->pulse < 0) t->pulse = 0;
            t->cd -= dt;
            float x = (float)cx + 0.5f, z = (float)cz + 0.5f;
            int tg = find_target(g, x, z, d->range[t->lvl], d->air, -1);
            if (t->type == TW_PRISM) {
                /* the prism keeps its beam on the same enemy for as long as it can, so the ramp builds */
                int old = t->target;
                if (old >= 0 && g->en[old].alive && !untouchable(&g->en[old]) && (!g->en[old].fly || d->air)) {
                    float dx = g->en[old].x - x, dz = g->en[old].z - z;
                    if (dx * dx + dz * dz <= d->range[t->lvl] * d->range[t->lvl]) tg = old;
                }
                if (tg != old) t->lock_t = 0;
                t->target = tg;
                if (tg >= 0 && t->built >= 1.0f) {
                    t->lock_t += dt;
                    float ramp = 1.0f + (d->extra[t->lvl] - 1.0f) * clampf(t->lock_t / 2.5f, 0, 1);
                    t->acc += d->dmg[t->lvl] * ramp * dt;
                    t->ang = atan2f(g->en[tg].z - z, g->en[tg].x - x);
                    if (t->cd <= 0) {                                      /* damage lands in ticks, 5 a second */
                        Enemy *e = &g->en[tg];
                        float ex = e->x, ey = e->y + ENEMIES[e->type].size * 0.7f, ez = e->z;
                        hurt(g, tg, t->acc);
                        t->acc = 0;
                        t->cd = 1.0f / d->rate[t->lvl];
                        sparks(g, ex, ey, ez, 2, ramp > 2.5f ? 0xFFFFFFFF : 0xFF90FFFF, 1.6f);
                        if (ramp > 2.5f) g->shake = fmaxf(g->shake, 0.08f);
                        if (((int)(t->lock_t * 5.0f) & 3) == 0) snd(g, SFX_ZAP, 150 + (int)(ramp * 25.0f), x);
                    }
                } else { t->lock_t = 0; t->acc = 0; t->ang += dt * 0.4f; }
                continue;
            }
            t->target = tg;
            if (tg >= 0) {
                float want = atan2f(g->en[tg].z - z, g->en[tg].x - x), da = want - t->ang;
                while (da > 3.14159f) da -= TAU;
                while (da < -3.14159f) da += TAU;
                t->ang += da * clampf(dt * 12.0f, 0, 1);
                if (t->cd <= 0 && t->built >= 1.0f && (t->type != TW_BLASTER || fabsf(da) < 0.6f)) {
                    fire_tower(g, cx, cz, t, tg);
                    t->cd = 1.0f / d->rate[t->lvl];
                }
            } else {
                t->ang += dt * 0.4f;            /* idle: slowly scan */
                if (t->cd < 0) t->cd = 0;
            }
        }
}

static void update_shots(Game *g, float dt)
{
    for (int s = 0; s < MAX_SH; s++) {
        Shot *b = &g->sh[s];
        if (!b->alive) continue;
        b->t += dt;
        if (b->kind == SH_BOLT) {
            Enemy *e = (b->target >= 0) ? &g->en[b->target] : NULL;
            if (e && e->alive) {
                float tx = e->x - b->x, ty = e->y + ENEMIES[e->type].size * 0.6f - b->y, tz = e->z - b->z;
                float l = sqrtf(tx * tx + ty * ty + tz * tz);
                if (l < 0.22f + ENEMIES[e->type].size * 0.4f) {
                    hurt(g, b->target, b->dmg);
                    sparks(g, b->x, b->y, b->z, 4, TOWERS[TW_BLASTER].glow, 1.6f);
                    snd(g, SFX_HIT, 120 + (int)(frand(g) * 30), b->x);
                    b->alive = 0;
                    continue;
                }
                float sp = 11.0f;
                b->vx += (tx / l * sp - b->vx) * clampf(dt * 14.0f, 0, 1);
                b->vy += (ty / l * sp - b->vy) * clampf(dt * 14.0f, 0, 1);
                b->vz += (tz / l * sp - b->vz) * clampf(dt * 14.0f, 0, 1);
            }
            b->x += b->vx * dt; b->y += b->vy * dt; b->z += b->vz * dt;
            if (frand(g) < 0.3f) {                              /* a fizzing trail */
                Part *p = part(g, PT_EMBER, b->x, b->y, b->z);
                p->vx = frand2(g) * 0.3f; p->vy = frand2(g) * 0.3f; p->vz = frand2(g) * 0.3f;
                p->life = p->max = 0.18f; p->size = 0.04f; p->c = 0x80E8FFFF;
            }
            if (b->t > b->dur || b->y < 0.0f) { sparks(g, b->x, fmaxf(b->y, 0.05f), b->z, 3, 0x70E8FFFF, 1.0f); b->alive = 0; }
        } else {
            float u = b->t / b->dur;
            if (u >= 1.0f) {
                /* boom */
                float r2 = b->splash * b->splash;
                for (int i = 0; i < MAX_EN; i++) {
                    Enemy *e = &g->en[i];
                    if (!e->alive || e->fly) continue;
                    float dx = e->x - b->ex, dz = e->z - b->ez, d2 = dx * dx + dz * dz;
                    if (d2 > r2) continue;
                    hurt(g, i, b->dmg * (1.0f - 0.5f * d2 / r2));
                }
                sparks(g, b->ex, 0.2f, b->ez, 16, 0xFFB050FF, 3.2f);
                for (int k = 0; k < 7; k++) {                  /* a rolling fireball */
                    Part *p = part(g, PT_FIRE, b->ex + frand2(g) * 0.25f, 0.15f + frand(g) * 0.2f, b->ez + frand2(g) * 0.25f);
                    p->vx = frand2(g) * 0.8f; p->vz = frand2(g) * 0.8f; p->vy = 0.8f + frand(g) * 1.2f;
                    p->life = p->max = 0.45f + frand(g) * 0.3f; p->size = 0.25f + frand(g) * 0.2f; p->c = 0xFFA040FF;
                }
                debris(g, b->ex, 0.1f, b->ez, 6, 0x5A4430FF, 0.07f);      /* clods of earth */
                {
                    Part *p = part(g, PT_SCORCH, b->ex, 0.008f, b->ez);
                    p->life = p->max = 5.0f; p->size = b->splash * 0.75f; p->rot = frand(g) * 6.28f; p->c = 0x140C08B0;
                }
                smoke(g, b->ex, 0.2f, b->ez, 5, 0.45f, 0x40404880);
                ring(g, b->ex, 0.04f, b->ez, 0.2f, b->splash * 1.15f, 0.4f, 0xFFA040E0);
                Part *p = part(g, PT_GLOW, b->ex, 0.35f, b->ez);
                p->life = p->max = 0.3f; p->size = b->splash * 1.2f; p->c = 0xFF9030FF;
                flight(g, b->ex, 0.6f, b->ez, 0xFF9040FF, 2.2f, 3.2f, 0.4f);
                g->shake = fmaxf(g->shake, 0.48f);
                g->punch = 1.0f;
                snd(g, SFX_BOOM, 116 + (int)(frand(g) * 24), b->ex);
                b->alive = 0;
                continue;
            }
            b->x = lerpf(b->sx, b->ex, u); b->z = lerpf(b->sz, b->ez, u);
            b->y = lerpf(b->sy, 0.1f, u) + 2.3f * 4.0f * u * (1.0f - u);
            if (frand(g) < 0.7f) {
                Part *p = part(g, PT_EMBER, b->x, b->y, b->z);
                p->vx = frand2(g) * 0.3f; p->vy = frand2(g) * 0.3f; p->vz = frand2(g) * 0.3f;
                p->life = p->max = 0.35f; p->size = 0.07f; p->c = 0xFF8030FF;
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* particles, rain, storm                                              */
/* ------------------------------------------------------------------ */
static float surface_h(const Game *g, float x, float z)
{
    int c = (int)floorf(x), r = (int)floorf(z);
    if (c < 0 || r < 0 || c >= MW || r >= MH) return 0.0f;
    return g->tile[r][c].h;
}

static void update_particles(Game *g, float dt)
{
    for (int i = 0; i < MAX_PT; i++) {
        Part *p = &g->pt[i];
        if (p->life <= 0) continue;
        p->life -= dt;
        switch (p->kind) {
        case PT_SPARK:
            p->vy -= 9.0f * dt;
            p->x += p->vx * dt; p->y += p->vy * dt; p->z += p->vz * dt;
            if (p->y < 0) { p->y = 0; p->vy *= -0.3f; p->vx *= 0.6f; p->vz *= 0.6f; }
            break;
        case PT_DEBRIS: {
            p->vy -= 11.0f * dt;
            p->x += p->vx * dt; p->y += p->vy * dt; p->z += p->vz * dt;
            p->rot += p->vr * dt;
            float h = surface_h(g, p->x, p->z);
            if (p->y < h + p->size * 0.5f) { p->y = h + p->size * 0.5f; p->vy *= -0.35f; p->vx *= 0.6f; p->vz *= 0.6f; p->vr *= 0.6f; }
            break;
        }
        case PT_EMBER:
            p->vy += 0.8f * dt;
            p->x += (p->vx + g->wind * 0.4f) * dt; p->y += p->vy * dt; p->z += p->vz * dt;
            break;
        case PT_SMOKE: case PT_STEAM:
            p->x += (p->vx + g->wind * 0.3f) * dt; p->y += p->vy * dt; p->z += p->vz * dt;
            p->size += dt * 0.35f;
            break;
        case PT_FLAKE:
            p->vx *= 1.0f - dt * 3.0f; p->vz *= 1.0f - dt * 3.0f; p->vy -= 0.8f * dt;
            p->x += p->vx * dt; p->y += p->vy * dt; p->z += p->vz * dt;
            break;
        case PT_FIRE:
            p->vx *= 1.0f - dt * 2.0f; p->vz *= 1.0f - dt * 2.0f; p->vy += 0.6f * dt;
            p->x += p->vx * dt; p->y += p->vy * dt; p->z += p->vz * dt;
            p->size += dt * 0.6f;
            break;
        case PT_SHARD: {
            p->vy -= 9.0f * dt;
            p->x += p->vx * dt; p->y += p->vy * dt; p->z += p->vz * dt;
            p->rot += p->vr * dt;
            float h = surface_h(g, p->x, p->z);
            if (p->y < h) { p->y = h; p->vy *= -0.25f; p->vx *= 0.5f; p->vz *= 0.5f; }
            break;
        }
        default: break;
        }
    }
    for (int i = 0; i < MAX_POP; i++) if (g->pop[i].t < 1.0f) { g->pop[i].t += dt * 0.9f; g->pop[i].y += dt * 0.8f; }
    for (int i = 0; i < MAX_FL; i++) if (g->fl[i].t < g->fl[i].life) g->fl[i].t += dt;
    for (int i = 0; i < MAX_ARC; i++) {
        Arc *a = &g->arc[i];
        if (!a->alive) continue;
        a->t += dt;
        if (a->t >= a->life) { a->alive = 0; continue; }
        if (!a->storm && frand(g) < 0.5f)             /* tesla arcs crackle */
            for (int k = 1; k < a->n - 1; k++) { a->p[k][0] += frand2(g) * 0.06f; a->p[k][1] += frand2(g) * 0.04f; a->p[k][2] += frand2(g) * 0.06f; }
    }
}

static void reset_drop(Game *g, Drop *d, int top)
{
    d->x = -4.0f + frand(g) * (float)(MW + 8);
    d->z = -4.0f + frand(g) * (float)(MH + 8);
    d->y = top ? 6.0f + frand(g) * 2.0f : frand(g) * 8.0f;
    d->len = 0.35f + frand(g) * 0.35f;
}

/* autumn: the drop slots are falling leaves instead (fewer of them, drifting and spinning) */
static void reset_leaf(Game *g, Drop *d, int top)
{
    d->x = -3.0f + frand(g) * (float)(MW + 6);
    d->z = -3.0f + frand(g) * (float)(MH + 5);
    d->y = top ? 4.5f + frand(g) * 1.5f : frand(g) * 5.0f;
    d->len = frand(g) * 100.0f;               /* phase + colour seed */
}

static void update_leaves(Game *g, float dt)
{
    for (int i = 0; i < NUM_LEAVES; i++) {
        Drop *d = &g->drop[i];
        d->y -= (0.55f + 0.25f * (float)(i % 3)) * dt;
        d->x += (g->wind * 0.9f + sinf(g->time * 1.3f + d->len) * 0.45f) * dt;
        d->z += cosf(g->time * 0.9f + d->len * 1.7f) * 0.2f * dt;
        if (d->y <= surface_h(g, d->x, d->z)) reset_leaf(g, d, 1);
    }
}

static void update_rain(Game *g, float dt)
{
    if (g->lv->env == ENV_HALLOW || g->lv->env == ENV_ISLAND || g->lv->env == ENV_FUTURE) return;
    if (g->lv->env == ENV_AUTUMN) { update_leaves(g, dt); return; }
    float push = (g->wind + g->gust * 3.0f) * 2.2f;
    for (int i = 0; i < MAX_DROP; i++) {
        Drop *d = &g->drop[i];
        d->y -= 14.0f * dt;
        d->x += push * dt;
        if (d->x > MW + 5) d->x -= (float)(MW + 9);
        float h = surface_h(g, d->x, d->z);
        if (d->y <= h) {
            if (d->x > -1 && d->x < MW + 1 && d->z > -1 && d->z < MH + 1 && (i & 1)) {
                Part *p = part(g, PT_SPLASH, d->x, h + 0.01f, d->z);
                p->life = p->max = 0.22f; p->size = 0.11f + frand(g) * 0.06f; p->c = 0xB4C8F060;
            }
            reset_drop(g, d, 1);
        }
    }
}

static void lightning_strike(Game *g)
{
    float x, z;
    int target = -1;
    if (g->state == ST_PLAY && frand(g) < 0.45f) {
        int n = 0;
        for (int i = 0; i < MAX_EN; i++) if (g->en[i].alive && !g->en[i].entering && !untouchable(&g->en[i])) n++;
        if (n > 0) {
            int pick = (int)(frand(g) * n) % n;
            for (int i = 0; i < MAX_EN; i++) if (g->en[i].alive && !g->en[i].entering && !untouchable(&g->en[i])) { if (pick-- == 0) { target = i; break; } }
        }
    }
    if (target >= 0) { x = g->en[target].x; z = g->en[target].z; }
    else {
        /* somewhere in the woods round the edge */
        float a = frand(g) * TAU;
        x = FIRE_X + cosf(a) * ((float)MW * 0.45f + frand(g) * 3.0f);
        z = FIRE_Z + sinf(a) * ((float)MH * 0.45f + frand(g) * 2.0f);
    }
    float top[3] = { x + frand2(g) * 3.0f - 2.0f, 11.0f, z - 4.0f - frand(g) * 3.0f };
    float bot[3] = { x, surface_h(g, x, z) + 0.05f, z };
    make_arc(g, top, bot, 0xE8F0FFFF, 0.45f, 1, 0.9f);
    /* a side branch */
    float mid[3] = { (top[0] + bot[0]) * 0.5f, (top[1] + bot[1]) * 0.55f, (top[2] + bot[2]) * 0.5f };
    float br[3] = { mid[0] + frand2(g) * 2.5f, mid[1] - 2.5f - frand(g) * 2.0f, mid[2] + frand2(g) * 1.5f };
    make_arc(g, mid, br, 0xC8D8FFFF, 0.3f, 1, 0.5f);
    sparks(g, x, bot[1], z, 24, 0xD8E8FFFF, 3.5f);
    ring(g, x, bot[1] + 0.02f, z, 0.2f, 1.8f, 0.5f, 0xC0D8FFE0);
    smoke(g, x, bot[1] + 0.1f, z, 4, 0.4f, 0x30304060);
    flight(g, x, 1.2f, z, 0xD0E0FFFF, 1.7f, 4.5f, 0.6f);
    g->strike_x = x; g->strike_z = z;
    g->flash_t = 0;
    g->shake = fmaxf(g->shake, 0.55f);
    g->thunder_t = 0.12f + frand(g) * 0.35f;
    g->thunder_big = 1;
    if (target >= 0) {
        float dmg = ENEMIES[g->en[target].type].boss ? 120.0f : 90.0f;
        hurt(g, target, dmg);
    }
}

static void update_storm(Game *g, float dt)
{
    if (g->lv->env == ENV_PIRATE) {
        /* squalls: every so often a gust of wind tears across the sea */
        g->gust_t -= dt;
        if (g->gust_t <= 0) {
            g->gust_t = (g->state == ST_TITLE ? 5.0f : 7.0f) + frand(g) * 7.0f;
            g->gust_on = 2.6f;
            snd(g, SFX_GUST, 110 + (int)(frand(g) * 30), FIRE_X + frand2(g) * 6.0f);
        }
        if (g->gust_on > 0) { g->gust_on -= dt; g->gust += (1.0f - g->gust) * clampf(dt * 2.5f, 0, 1); }
        else g->gust -= dt * 0.45f;
        if (g->gust < 0) g->gust = 0;
    }
    if (g->lv->env == ENV_FUTURE) {
        /* the town square: a dry thunderstorm, the clock tower drawing a 1.21 gigawatt bolt now and then */
        g->combo_t -= dt; if (g->combo_t < 0) g->combo_t = 0;
        g->surge_t -= dt; g->giga_fx -= dt; g->warp_cd -= dt; g->tilt_cd -= dt;
        g->dmd_t += dt;
        for (int i = 0; i < MAX_TRAIL; i++) if (g->trail[i].t < g->trail[i].life) g->trail[i].t += dt;
        if (g->state == ST_PLAY && g->wave_on) {
            g->giga_t -= dt;
            if (g->giga_t <= 0) { g->giga_t = 26.0f + frand(g) * 12.0f; giga_strike(g); }
        }
    }
    if (g->lv->env != ENV_STORM && g->lv->env != ENV_PIRATE && g->lv->env != ENV_FUTURE) {
        /* a calm autumn evening: a light breeze, no lightning */
        g->wind = 0.25f + 0.15f * sinf(g->time * 0.27f);
        g->flash = 0;
        g->shake -= dt * 2.2f;
        if (g->shake < 0) g->shake = 0;
        return;
    }
    g->wind = 0.55f + 0.35f * sinf(g->time * 0.31f) + 0.15f * sinf(g->time * 1.7f);
    g->flash_t += dt;
    float t = g->flash_t;
    /* the classic double / triple flicker of a nearby strike */
    float f = expf(-t * 10.0f) + 0.75f * expf(-fabsf(t - 0.14f) * 30.0f) + 0.45f * expf(-fabsf(t - 0.32f) * 22.0f);
    g->flash = clampf(f, 0, 1.1f);
    g->storm_t -= dt;
    if (g->storm_t <= 0) {
        if (frand(g) < 0.3f) {
            /* distant sheet lightning: a soft flash and a far rumble, no bolt */
            g->flash_t = 0.22f;
            g->thunder_t = 0.6f + frand(g) * 0.8f;
            g->thunder_big = 0;
            g->storm_t = 3.0f + frand(g) * 4.0f;
        } else {
            lightning_strike(g);
            g->storm_t = (g->state == ST_TITLE ? 4.0f : 7.0f) + frand(g) * 8.0f;
        }
    }
    if (g->thunder_t > 0) {
        g->thunder_t -= dt;
        if (g->thunder_t <= 0) {
            if (g->thunder_big) snd(g, SFX_CRACK, 120 + (int)(frand(g) * 16), g->strike_x);
            snd(g, SFX_THUNDER, (g->thunder_big ? 118 : 100) + (int)(frand(g) * 20), g->strike_x);
        }
    }
    g->shake -= dt * 2.2f;
    if (g->shake < 0) g->shake = 0;
}

static void update_fire(Game *g, float dt)
{
    float want = g->state == ST_LOSE ? 0.0f : clampf((float)g->fire / (float)FIRE_HP, 0.0f, 1.0f) * 0.75f + 0.25f;
    if (g->state == ST_TITLE) want = 1.0f;
    g->fire_vis += (want - g->fire_vis) * clampf(dt * 2.0f, 0, 1);
    g->fire_hit -= dt * 1.5f; if (g->fire_hit < 0) g->fire_hit = 0;
    if (g->lv->env == ENV_PIRATE) {
        /* the treasure: glints of gold sparkle up out of the open chest (fewer as it is plundered) */
        if (g->fire_vis > 0.05f && frand(g) < dt * 14.0f * g->fire_vis) {
            Part *p = part(g, PT_EMBER, FIRE_X + frand2(g) * 0.25f, 0.55f, FIRE_Z + 0.05f + frand2(g) * 0.15f);
            p->vx = frand2(g) * 0.25f; p->vz = frand2(g) * 0.25f; p->vy = 0.6f + frand(g) * 0.8f;
            p->life = p->max = 0.6f + frand(g) * 0.7f; p->size = 0.035f + frand(g) * 0.03f; p->c = frand(g) < 0.5f ? 0xFFE070FF : 0xFFF8D0FF;
        }
        return;
    }
    if (g->fire_vis > 0.05f && frand(g) < dt * 22.0f * g->fire_vis) {
        Part *p = part(g, PT_EMBER, FIRE_X + frand2(g) * 0.35f, 0.5f, FIRE_Z + frand2(g) * 0.35f);
        p->vx = frand2(g) * 0.4f; p->vz = frand2(g) * 0.4f; p->vy = 1.0f + frand(g) * 1.4f;
        p->life = p->max = 0.9f + frand(g) * 1.0f; p->size = 0.045f + frand(g) * 0.04f; p->c = 0xFFA040FF;
    }
    if (g->fire_vis > 0.05f && frand(g) < dt * 3.0f)
        smoke(g, FIRE_X, 1.1f + g->fire_vis * 0.5f, FIRE_Z, 1, 0.35f, 0x30303850);
}

/* ------------------------------------------------------------------ */
/* game flow                                                           */
/* ------------------------------------------------------------------ */
static void new_game(Game *g)
{
    map_load(g);
    for (int r = 0; r < MH; r++) for (int c = 0; c < MW; c++) { memset(&g->tw[r][c], 0, sizeof(Tower)); g->tw[r][c].type = -1; }
    memset(g->en, 0, sizeof g->en);
    memset(g->sh, 0, sizeof g->sh);
    memset(g->arc, 0, sizeof g->arc);
    g->gold = g->lv->start_gold;
    g->fire = FIRE_HP;
    g->cx = 6; g->cz = 4;
    g->cur_x = (float)g->cx; g->cur_z = (float)g->cz;
    g->sel = 0;
    g->fast = 0;
    g->wave = 0; g->wave_on = 0; g->countdown = 30.0f;
    g->gate_rr = 0;
    g->leaked = 0; g->kills = 0;
    memset(g->leak_by, 0, sizeof g->leak_by);
    g->boss = -1;
    g->t = 0;
    g->banner_t = 0;
    snprintf(g->banner, sizeof g->banner, "WAVE 1/%d", g->lv->nwaves);
    snprintf(g->banner2, sizeof g->banner2, "PREPARE FOR 1ST WAVE!!");
    g->state = ST_PLAY;
    pinball_reset(g);
    if (g->lv->env == ENV_FUTURE) dmd(g, "INSERT COIN", "1.21 GIGAWATTS!", 0xFFA030FF);
}

/* switch to a level and show it behind the title screen (with a few towers on it) */
void game_set_level(Game *g, int level)
{
    if (level < 0) level = NUM_LEVELS - 1;
    if (level >= NUM_LEVELS) level = 0;
    g->level = level;
    g->lv = &LEVELS[level];
    map_load(g);
    for (int r = 0; r < MH; r++) for (int c = 0; c < MW; c++) { memset(&g->tw[r][c], 0, sizeof(Tower)); g->tw[r][c].type = -1; }
    memset(g->en, 0, sizeof g->en);
    memset(g->sh, 0, sizeof g->sh);
    memset(g->arc, 0, sizeof g->arc);
    memset(g->pt, 0, sizeof g->pt);
    for (int i = 0; i < MAX_POP; i++) g->pop[i].t = 1.0f;
    for (int i = 0; i < MAX_FL; i++) g->fl[i].t = g->fl[i].life = 0;
    if (g->lv->env == ENV_STORM || g->lv->env == ENV_PIRATE) for (int i = 0; i < MAX_DROP; i++) reset_drop(g, &g->drop[i], 0);
    else for (int i = 0; i < NUM_LEAVES; i++) reset_leaf(g, &g->drop[i], 0);
    g->gust = 0; g->gust_on = 0; g->gust_t = 4.0f;
    pinball_reset(g);
    g->state = ST_TITLE;
    g->storm_t = 1.5f;
    g->flash_t = 10.0f;
    g->flash = 0;
    g->fire_vis = 1.0f;
    g->boss = -1;
    g->wave = 0; g->wave_on = 0;
    g->sun = 0.12f;
    g->zoom = g->zoom_t = 1.0f;
    static const int demo1[][3] = { {6, 4, TW_BLASTER}, {11, 5, TW_TESLA}, {13, 7, TW_MORTAR}, {3, 7, TW_FROST}, {16, 7, TW_BLASTER}, {7, 9, TW_MORTAR} };
    static const int demo2[][3] = { {7, 4, TW_BLASTER}, {11, 5, TW_TESLA}, {14, 7, TW_MORTAR}, {4, 7, TW_FROST}, {13, 9, TW_BLASTER}, {7, 8, TW_MORTAR} };
    const int (*demo)[3] = level == 0 ? demo1 : demo2;
    for (int i = 0; i < 6; i++) {
        int c = demo[i][0], r = demo[i][1];
        if (!can_build(g, c, r)) continue;
        Tower *t = &g->tw[r][c];
        t->type = (int8_t)demo[i][2]; t->built = 1; t->ang = (float)i * 1.3f; t->lvl = (uint8_t)(i % 3);
    }
}

void game_init(Game *g)
{
    memset(g, 0, sizeof *g);
    g->rng = 0x9E3779B9u;
    audio_init();
    music_init();
    game_set_level(g, 0);
}

static void move_cursor(Game *g, int dx, int dz)
{
    int nx = g->cx + dx, nz = g->cz + dz;
    if (nx < 1) nx = 1; if (nx > MW - 2) nx = MW - 2;
    if (nz < 1) nz = 1; if (nz > MH - 2) nz = MH - 2;
    if (nx != g->cx || nz != g->cz) { g->cx = nx; g->cz = nz; snd(g, SFX_MOVE, 128, (float)nx); }
}

static void play_input(Game *g, const Input *in, float dt)
{
    /* cursor: d-pad with key repeat, or the stick */
    uint32_t dirs = in->held & (BTN_UP | BTN_DOWN | BTN_LEFT | BTN_RIGHT);
    int dx = 0, dz = 0;
    if (in->pressed & BTN_LEFT) dx = -1;
    if (in->pressed & BTN_RIGHT) dx = 1;
    if (in->pressed & BTN_UP) dz = -1;
    if (in->pressed & BTN_DOWN) dz = 1;
    if (dx || dz) { move_cursor(g, dx, dz); g->rep_t = 0.28f; g->rep_dir = dirs; }
    else if (dirs) {
        g->rep_t -= dt;
        if (g->rep_t <= 0) {
            g->rep_t = 0.085f;
            move_cursor(g, (dirs & BTN_LEFT) ? -1 : ((dirs & BTN_RIGHT) ? 1 : 0), (dirs & BTN_UP) ? -1 : ((dirs & BTN_DOWN) ? 1 : 0));
        }
    }
    float m = sqrtf(in->sx * in->sx + in->sy * in->sy);
    if (m > 0.45f) {
        g->stick_t -= dt;
        if (g->stick_t <= 0) {
            int sx = fabsf(in->sx) > 0.38f ? (in->sx > 0 ? 1 : -1) : 0;
            int sz = fabsf(in->sy) > 0.38f ? (in->sy > 0 ? 1 : -1) : 0;
            move_cursor(g, sx, sz);
            g->stick_t = 0.17f - 0.08f * clampf((m - 0.45f) / 0.5f, 0, 1);
        }
    } else g->stick_t = 0;

    if (in->pressed & BTN_L) { g->sel = (g->sel + TW_TYPES - 1) % TW_TYPES; g->sel_t = 1.0f; snd(g, SFX_SELECT, 120, FIRE_X); }
    if (in->pressed & BTN_R) { g->sel = (g->sel + 1) % TW_TYPES; g->sel_t = 1.0f; snd(g, SFX_SELECT, 136, FIRE_X); }
    if (in->pressed & BTN_Y) { g->fast = !g->fast; snd(g, SFX_SELECT, g->fast ? 150 : 105, FIRE_X); }

    Tower *t = &g->tw[g->cz][g->cx];
    float tx = (float)g->cx + 0.5f, tz = (float)g->cz + 0.5f, th = g->tile[g->cz][g->cx].h;
    if (in->pressed & BTN_A) {
        if (t->type >= 0) {
            int damaged = t->hp < TOWER_HP[t->lvl] - 0.01f;
            if (t->lvl + 1 >= TW_MAXLVL && damaged && g->gold >= tower_repair_cost(t)) {
                g->gold -= tower_repair_cost(t);
                t->hp = TOWER_HP[t->lvl];
                sparks(g, tx, th + 0.6f, tz, 12, 0xA0FFA0FF, 2.0f);
                ring(g, tx, th + 0.03f, tz, 0.2f, 0.9f, 0.4f, 0x90FF90C0);
                snd(g, SFX_BUILD, 150, tx);
            } else if (t->lvl + 1 < TW_MAXLVL && g->gold >= tower_cost(t->type, t->lvl + 1)) {
                int c = tower_cost(t->type, t->lvl + 1);
                g->gold -= c; t->spent += (float)c; t->lvl++; t->built = 0.35f; t->hp = TOWER_HP[t->lvl];   /* upgrading also repairs */
                sparks(g, tx, th + 0.6f, tz, 18, TOWERS[t->type].glow, 2.4f);
                ring(g, tx, th + 0.03f, tz, 0.2f, 1.0f, 0.45f, TOWERS[t->type].glow);
                flight(g, tx, th + 1.0f, tz, TOWERS[t->type].glow, 1.5f, 2.5f, 0.4f);
                snd(g, SFX_UPGRADE, 128, tx);
            } else { g->err_t = 1.0f; snd(g, SFX_ERROR, 128, tx); }
        } else if (can_build(g, g->cx, g->cz) && g->gold >= tower_cost(g->sel, 0)) {
            int c = tower_cost(g->sel, 0);
            g->gold -= c;
            memset(t, 0, sizeof *t);
            t->type = (int8_t)g->sel; t->spent = (float)c; t->target = -1; t->ang = -1.5708f; t->hp = TOWER_HP[0];
            debris(g, tx, th + 0.2f, tz, 6, 0x8C8C9CFF, 0.1f);
            ring(g, tx, th + 0.03f, tz, 0.2f, 0.9f, 0.4f, 0xFFFFFFA0);
            smoke(g, tx, th + 0.1f, tz, 4, 0.3f, 0x60607080);
            snd(g, SFX_BUILD, 128, tx);
        } else { g->err_t = 1.0f; snd(g, SFX_ERROR, 128, tx); }
    }
    if ((in->pressed & BTN_B) && t->type >= 0) {
        int v = tower_sell_value(t);
        g->gold += v;
        char s[12];
        snprintf(s, sizeof s, "+%d", v);
        popup(g, tx, th + 1.0f, tz, s, 0xFFD860FF);
        debris(g, tx, th + 0.4f, tz, 8, TOWERS[t->type].color, 0.12f);
        smoke(g, tx, th + 0.2f, tz, 5, 0.35f, 0x60607080);
        t->type = -1;
        snd(g, SFX_SELL, 128, tx);
    }
    if ((in->pressed & BTN_X) && !g->wave_on && g->wave < g->lv->nwaves) {
        int bonus = (int)g->countdown;
        if (bonus > 0) {
            g->gold += bonus;
            char s[12];
            snprintf(s, sizeof s, "+%d", bonus);
            popup(g, FIRE_X, 1.8f, FIRE_Z, s, 0xFFD860FF);
            snd(g, SFX_COIN, 128, FIRE_X);
        }
        g->countdown = 0;
        start_wave(g);
    }
    if (in->held & BTN_ZR) g->zoom_t += dt * 1.6f;          /* R: zoom in on the action, L: back out */
    if (in->held & BTN_ZL) g->zoom_t -= dt * 1.6f;
    g->zoom_t = clampf(g->zoom_t, 1.0f, ZOOM_MAX);
    if (in->pressed & BTN_PLUS) { g->state = ST_PAUSE; g->menu = 0; snd(g, SFX_PAUSE, 128, FIRE_X); }
}

void game_update(Game *g, const Input *in, float dt)
{
    g->time += dt;
    g->gold_flash -= dt * 2.0f; if (g->gold_flash < 0) g->gold_flash = 0;
    g->fire_flash -= dt * 1.5f; if (g->fire_flash < 0) g->fire_flash = 0;
    g->err_t -= dt * 2.5f; if (g->err_t < 0) g->err_t = 0;
    g->sel_t -= dt * 3.0f; if (g->sel_t < 0) g->sel_t = 0;
    g->banner_t += dt;
    g->cur_x += ((float)g->cx - g->cur_x) * clampf(dt * 20.0f, 0, 1);
    g->cur_z += ((float)g->cz - g->cur_z) * clampf(dt * 20.0f, 0, 1);
    if (g->state == ST_TITLE) g->zoom_t = 1.0f;
    g->zoom += (g->zoom_t - g->zoom) * clampf(dt * 8.0f, 0, 1);

    if (g->state != ST_PAUSE) {
        g->punch -= dt * 6.0f; if (g->punch < 0) g->punch = 0;
        if (g->lv->env == ENV_ISLAND) {
            /* the sun climbs as the waves go by */
            float want = g->state == ST_TITLE ? 0.12f : clampf(((float)g->wave + (g->wave_on ? clampf(g->wave_t / 60.0f, 0, 0.9f) : 0.0f)) / (float)g->lv->nwaves, 0, 1);
            g->sun += (want - g->sun) * clampf(dt * 0.25f, 0, 1);
        }
        update_storm(g, dt);
        update_rain(g, dt);
        update_fire(g, dt);
        update_particles(g, dt);
    }

    switch (g->state) {
    case ST_TITLE:
        for (int r = 0; r < MH; r++) for (int c = 0; c < MW; c++) if (g->tw[r][c].type >= 0) g->tw[r][c].ang += dt * 0.5f;
        if (in->pressed & BTN_LEFT) { game_set_level(g, g->level - 1); snd(g, SFX_SELECT, 120, FIRE_X); }
        else if (in->pressed & BTN_RIGHT) { game_set_level(g, g->level + 1); snd(g, SFX_SELECT, 136, FIRE_X); }
        else if (in->pressed & (BTN_A | BTN_PLUS)) { new_game(g); snd(g, SFX_START, 128, FIRE_X); }
        break;
    case ST_PLAY:
        g->t += dt;
        play_input(g, in, dt);
        if (g->state != ST_PLAY) break;
        update_waves(g, dt);
        if (g->state != ST_PLAY) break;
        update_towers(g, dt);
        update_shots(g, dt);
        update_enemies(g, dt);
        break;
    case ST_PAUSE:
        if (in->pressed & BTN_UP) { g->menu = (g->menu + 4) % 5; snd(g, SFX_MOVE, 128, FIRE_X); }
        if (in->pressed & BTN_DOWN) { g->menu = (g->menu + 1) % 5; snd(g, SFX_MOVE, 128, FIRE_X); }
        if ((in->pressed & BTN_PLUS) || ((in->pressed & BTN_A) && g->menu == 0)) { g->state = ST_PLAY; snd(g, SFX_PAUSE, 150, FIRE_X); }
        else if ((in->pressed & BTN_A) && g->menu == 1) { g->music_off = !g->music_off; snd(g, SFX_SELECT, 136, FIRE_X); }
        else if ((in->pressed & BTN_A) && g->menu == 2) { g->mute = !g->mute; snd(g, SFX_SELECT, 136, FIRE_X); }
        else if ((in->pressed & BTN_A) && g->menu == 3) { new_game(g); snd(g, SFX_START, 128, FIRE_X); }
        else if ((in->pressed & BTN_A) && g->menu == 4) { game_set_level(g, g->level); snd(g, SFX_PAUSE, 128, FIRE_X); }
        break;
    case ST_WIN: case ST_LOSE:
        g->end_t += dt;
        update_shots(g, dt);
        for (int i = 0; i < MAX_EN; i++) if (g->en[i].alive) g->en[i].anim += dt * 3.0f;
        if (g->end_t > 2.0f && (in->pressed & (BTN_A | BTN_PLUS))) {
            /* A: next level after a win (or retry after a loss); B: back to the title */
            if (g->state == ST_WIN && g->level + 1 < NUM_LEVELS) game_set_level(g, g->level + 1);
            new_game(g);
            snd(g, SFX_START, 128, FIRE_X);
        } else if (g->end_t > 2.0f && (in->pressed & BTN_B)) { game_set_level(g, g->level); snd(g, SFX_PAUSE, 128, FIRE_X); }
        break;
    }
}
