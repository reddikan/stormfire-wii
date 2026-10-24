/*
 * STORMFIRE - internal game state shared by game.c, draw.c, map.c and the test harness.
 *
 * World units: one map tile = 1.0. x runs left -> right across the screen,
 * z runs from the far (top) edge of the map to the near (bottom) edge, y is up.
 * Tile (c, r) covers x in [c, c+1], z in [r, r+1].
 */
#ifndef GAME_INT_H
#define GAME_INT_H

#include "game.h"

#define MW 19
#define MH 13

/* tile kinds */
enum { TL_PATH, TL_WALL, TL_BLOCK, TL_LAWN, TL_TREE, TL_FIRE };
/* block colours (TL_BLOCK) */
enum { BC_RED, BC_ORANGE, BC_YELLOW, BC_GREEN, BC_MINT, BC_BLUE, BC_PURPLE, BC_CYAN, BC_COUNT };

typedef struct {
    uint8_t kind, color, torch, puddle;
    float   h;              /* surface height a tower stands on */
} Tile;

/* towers */
enum { TW_BLASTER, TW_MORTAR, TW_FROST, TW_TESLA, TW_TYPES };
#define TW_MAXLVL 3
typedef struct {
    const char *name;
    int   cost[TW_MAXLVL];  /* build cost, then upgrade costs */
    float range[TW_MAXLVL];
    float dmg[TW_MAXLVL];
    float rate[TW_MAXLVL];  /* shots per second */
    float extra[TW_MAXLVL]; /* mortar: splash radius; frost: slow fraction; tesla: chain count */
    int   air;              /* can hit flyers */
    uint32_t color, glow;
} TowerDef;
extern const TowerDef TOWERS[TW_TYPES];

typedef struct {
    int8_t type;            /* -1 = none */
    uint8_t lvl;            /* 0..2 */
    float cd, ang, recoil, anim, built, spent, pulse;
    int   target;
} Tower;

/* enemies */
enum { EN_CRAWLER, EN_GRUNT, EN_BRUTE, EN_WISP, EN_GOLEM,                    /* level 1 */
       EN_BEETLE, EN_SLIME, EN_SLIMELET, EN_FOX, EN_CROW, EN_PUMPKIN, EN_GOURD,  /* level 2 */
       EN_TYPES };
typedef struct {
    const char *name;
    float hp, speed, size, fly;
    int   bounty, damage;
    float armor;            /* flat damage taken off every hit (never below 40% of the hit) */
    int   boss;             /* gets the big health bar, resists slowing */
    int   split_type, split_n;   /* spawns these where it dies (-1 = none) */
    uint32_t color;         /* debris colour */
} EnemyDef;
extern const EnemyDef ENEMIES[EN_TYPES];

#define MAX_EN 120
typedef struct {
    uint8_t alive, type, fly, entering;
    float x, z, y;
    float hp, maxhp, speed;
    float slow_t, slow_amt;
    float ang, anim, hit_t, lane;
    float tx, tz;           /* point currently walking to */
    int   cx, cz;           /* tile it is heading for */
    float prog;             /* distance left to the fire (smaller = closer) */
    float zap_t;
    float dash_t;           /* fox: > 0 while sprinting; counts down to the next sprint when < 0 */
    float spawn_t;          /* pumpkin king: until it drops the next gourds */
    float armor_t;          /* flashes when a hit bounces off the armour */
    float inv_t;            /* just split off a parent: can't be hurt yet */
} Enemy;

/* projectiles */
enum { SH_BOLT, SH_SHELL };
#define MAX_SH 96
typedef struct {
    uint8_t alive, kind, lvl;
    int   target;
    float x, y, z, vx, vy, vz;
    float sx, sy, sz, ex, ez, t, dur;   /* shell flight */
    float dmg, splash;
} Shot;

/* lightning arcs (tesla chains and the storm's own bolts) */
#define MAX_ARC 24
#define ARC_PTS 12
typedef struct {
    uint8_t alive, storm;
    int     n;
    float   p[ARC_PTS][3];
    float   t, life;
    uint32_t c;
} Arc;

/* particles */
enum { PT_SPARK, PT_EMBER, PT_DEBRIS, PT_SMOKE, PT_SPLASH, PT_RING, PT_FLAKE, PT_GLOW, PT_STEAM };
#define MAX_PT 900
typedef struct {
    uint8_t kind;
    float x, y, z, vx, vy, vz;
    float life, max, size, rot, vr;
    uint32_t c;
} Part;

#define MAX_DROP 230
#define NUM_LEAVES 70     /* autumn uses the first drops as falling leaves */
typedef struct { float x, y, z, len; } Drop;

#define MAX_POP 24
typedef struct { float x, y, z, t; char txt[12]; uint32_t c; } Pop;

/* transient point lights (explosions, muzzle flashes, strikes) */
#define MAX_FL 24
typedef struct { float x, y, z, r, g, b, rad, t, life; } FLight;

/* waves */
#define GATE_CYCLE 250      /* each enemy of the group uses the next gate in turn */
#define GATE_RANDOM 251
#define GATE_AIR 252        /* flyers: from any direction around the map */
typedef struct { uint8_t type, count, gate; float delay, gap; } Group;
#define MAX_GROUPS 8
typedef struct { int n, gates; float hpmul; Group g[MAX_GROUPS]; } WaveDef;   /* gates: how many gates this wave uses */

#define NUM_GATES 6
enum { ENV_STORM, ENV_AUTUMN };
typedef struct {
    const char *name;               /* shown on the title screen */
    const char *boss_line;          /* last-wave banner */
    const char *rows[MH];
    int   gates[NUM_GATES][2];      /* tiles on the map edge, in the order waves open them */
    const WaveDef *waves;
    int   nwaves;
    int   env;
    int   start_gold;
} LevelDef;
extern const LevelDef LEVELS[];
extern const int NUM_LEVELS;

#define FIRE_X 9.5f
#define FIRE_Z 6.5f
#define FIRE_HP 20

enum { ST_TITLE, ST_PLAY, ST_PAUSE, ST_WIN, ST_LOSE };

#define SND_Q 32

struct Game {
    int   state;
    int   level;            /* index into LEVELS */
    const LevelDef *lv;
    float time;             /* real seconds since boot (never pauses) */
    float t;                /* game seconds */
    uint32_t rng;

    Tile  tile[MH][MW];
    int   dist[MH][MW];     /* steps to the fire along the paths (-1 = not walkable) */
    int8_t nxt[MH][MW][2];  /* next tile towards the fire */
    Tower tw[MH][MW];

    /* player */
    int   gold, fire, cx, cz, sel, fast;
    float cur_x, cur_z;     /* smoothed cursor (for drawing) */
    float rep_t; uint32_t rep_dir;
    float stick_t;
    float gold_flash, fire_flash, err_t, sel_t;
    int   menu;             /* pause menu item: 0 resume, 1 sound, 2 restart, 3 quit to title */
    int   mute;             /* sound effects off */

    /* waves */
    int   wave;             /* waves started so far (1-based display) */
    int   wave_on;          /* spawning or enemies still alive */
    float wave_t;           /* time since this wave started */
    float countdown;        /* until the next wave starts by itself */
    int   spawned[MAX_GROUPS];
    int   gate_rr;
    float banner_t; char banner[40]; char banner2[40];
    int   leaked, kills;
    int   leak_by[EN_TYPES];      /* stats for the test harness */
    float end_t;
    int   boss;             /* index of the golem, -1 */

    Enemy en[MAX_EN];
    Shot  sh[MAX_SH];
    Arc   arc[MAX_ARC];
    Part  pt[MAX_PT];
    int   npt_next;
    Drop  drop[MAX_DROP];
    Pop   pop[MAX_POP];
    FLight fl[MAX_FL];

    /* storm */
    float flash;            /* 0..1 lightning flash brightness */
    float storm_t;          /* until the next strike */
    float flash_t;          /* time since the strike */
    float thunder_t;        /* > 0: thunder plays when it reaches 0 */
    int   thunder_big;
    float strike_x, strike_z;
    float shake;
    float wind;

    /* fire */
    float fire_vis;         /* smoothed fire strength 0..1 */
    float fire_hit;

    int   snd_id[SND_Q], snd_pitch[SND_Q];
    float snd_pan[SND_Q];
    int   snd_n;
};

/* helpers shared between the game files */
static inline float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }
static inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }
float frand(Game *g);                       /* 0..1 */
float frand2(Game *g);                      /* -1..1 */
void  map_load(Game *g);                    /* loads g->level */
void  game_set_level(Game *g, int level);   /* switch level (title screen backdrop) */
int   tower_cost(int type, int lvl);        /* cost to reach lvl (0 = build) */
float tower_range(const Tower *t);
int   tower_sell_value(const Tower *t);
int   can_build(const Game *g, int cx, int cz);

#endif
