/*
 * STORMFIRE - level data: maps, gates, tower and enemy stats, and the waves.
 *
 * Map legend
 *   .  path (enemies walk here)          F  the campfire plaza (3 x 3, always in the middle)
 *   #  wall (build on top)               t  wall with a torch
 *   ,  grass lawn (build here too)       T  forest (nothing goes here)
 *   r o y g m b p c   coloured blocks (build on top): red orange yellow green mint blue purple cyan
 */
#include "game_int.h"

const TowerDef TOWERS[TW_TYPES] = {
    /* name       cost            range              damage          rate              extra            air  colour      glow */
    { "BLASTER", {50, 45, 80},  {2.6f, 2.9f, 3.2f}, {10, 17, 28},   {3.0f, 3.4f, 4.0f}, {0, 0, 0},       1, 0x3C8CF0FF, 0x70E8FFFF },
    { "MORTAR",  {80, 70, 120}, {3.3f, 3.7f, 4.1f}, {34, 58, 95},   {0.6f, 0.65f, 0.7f}, {0.95f, 1.1f, 1.3f}, 0, 0xF0603CFF, 0xFFA040FF },
    { "FROST",   {60, 50, 90},  {2.2f, 2.5f, 2.8f}, {4, 7, 12},     {1.0f, 1.1f, 1.25f}, {0.4f, 0.5f, 0.6f}, 1, 0x8CDCFFFF, 0x9CF4FFFF },
    { "TESLA",   {120, 100, 150}, {2.5f, 2.8f, 3.1f}, {22, 36, 55}, {0.9f, 1.0f, 1.15f}, {3, 4, 6},       1, 0xF8C838FF, 0xFFF080FF },
};

const EnemyDef ENEMIES[EN_TYPES] = {
    /* name           hp    speed  size   fly bounty dmg armor boss split            colour */
    { "CRAWLER",       30,  1.00f, 0.26f, 0,   5,   1,  0,   0, -1, 0,           0x3CB4E6FF },
    { "GRUNT",         85,  0.72f, 0.34f, 0,   8,   1,  0,   0, -1, 0,           0x9A5CE8FF },
    { "BRUTE",        360,  0.48f, 0.48f, 0,  20,   3,  0,   0, -1, 0,           0xC8483CFF },
    { "WISP",          50,  0.85f, 0.30f, 1,   6,   1,  0,   0, -1, 0,           0x9CFFD8FF },
    { "STORM GOLEM", 1600,  0.30f, 0.80f, 0, 150,  10,  0,   1, -1, 0,           0x7C8894FF },
    /* level 2 */
    { "BEETLE",       150,  0.62f, 0.36f, 0,  12,   1,  5,   0, -1, 0,           0x2E8C7CFF },
    { "SLIME",         95,  0.66f, 0.36f, 0,   8,   1,  0,   0, EN_SLIMELET, 2,  0x88D848FF },
    { "SLIMELET",      26,  0.72f, 0.22f, 0,   3,   1,  0,   0, -1, 0,           0xA8E860FF },
    { "FOX",           70,  0.85f, 0.30f, 0,   8,   1,  0,   0, -1, 0,           0xE8702CFF },
    { "CROW",          75,  0.95f, 0.30f, 1,   9,   1,  0,   0, -1, 0,           0x3A3044FF },
    { "PUMPKIN KING", 2000, 0.27f, 0.85f, 0, 200,  10,  4,   1, EN_GOURD, 4,     0xF08020FF },
    { "GOURD",         45,  0.95f, 0.24f, 0,   3,   1,  0,   0, -1, 0,           0xF8B030FF },
};

#define C GATE_CYCLE
#define R GATE_RANDOM
#define A GATE_AIR
static const WaveDef WAVES1[] = {
    /* n gates hp x  groups: {type, count, gate, delay, gap} */
    { 1, 2, 1.00f, { {EN_CRAWLER, 8, C, 0, 1.4f} } },
    { 2, 2, 1.10f, { {EN_CRAWLER, 10, C, 0, 1.0f}, {EN_GRUNT, 4, C, 7, 2.2f} } },
    { 2, 4, 1.30f, { {EN_GRUNT, 12, C, 0, 1.2f}, {EN_CRAWLER, 6, R, 8, 0.7f} } },
    { 2, 4, 1.55f, { {EN_CRAWLER, 24, C, 0, 0.5f}, {EN_WISP, 4, A, 6, 1.5f} } },
    { 2, 6, 1.85f, { {EN_GRUNT, 14, C, 0, 0.9f}, {EN_BRUTE, 2, C, 5, 6.0f} } },
    { 2, 6, 2.20f, { {EN_WISP, 10, A, 0, 0.8f}, {EN_CRAWLER, 20, C, 3, 0.4f} } },
    { 2, 6, 2.60f, { {EN_BRUTE, 4, C, 0, 4.0f}, {EN_GRUNT, 18, C, 2, 0.7f} } },
    { 3, 6, 3.00f, { {EN_CRAWLER, 30, C, 0, 0.3f}, {EN_WISP, 8, A, 4, 0.7f}, {EN_GRUNT, 10, C, 8, 0.8f} } },
    { 2, 6, 3.50f, { {EN_BRUTE, 7, C, 0, 2.6f}, {EN_WISP, 10, A, 5, 0.6f} } },
    { 3, 6, 4.00f, { {EN_GRUNT, 26, C, 0, 0.45f}, {EN_BRUTE, 5, C, 6, 2.5f}, {EN_CRAWLER, 20, R, 10, 0.3f} } },
    { 3, 6, 4.60f, { {EN_WISP, 16, A, 0, 0.45f}, {EN_BRUTE, 8, C, 3, 2.0f}, {EN_GRUNT, 20, C, 6, 0.5f} } },
    { 5, 6, 5.00f, { {EN_GOLEM, 1, 0, 4, 1.0f}, {EN_BRUTE, 6, C, 0, 3.0f}, {EN_GRUNT, 24, C, 2, 0.5f},
                     {EN_WISP, 12, A, 10, 0.6f}, {EN_CRAWLER, 30, R, 16, 0.25f} } },
};

/* level 2: new enemies, and the waves start tougher */
static const WaveDef WAVES2[] = {
    /* n gates hp x  groups: {type, count, gate, delay, gap} */
    { 2, 2, 1.20f, { {EN_CRAWLER, 6, C, 0, 1.2f}, {EN_SLIME, 3, C, 5, 2.5f} } },
    { 2, 2, 1.38f, { {EN_FOX, 8, C, 0, 1.4f}, {EN_GRUNT, 4, C, 6, 1.8f} } },
    { 2, 4, 1.62f, { {EN_BEETLE, 4, C, 0, 2.4f}, {EN_SLIME, 5, C, 4, 1.6f} } },
    { 2, 4, 1.92f, { {EN_CROW, 6, A, 0, 1.2f}, {EN_FOX, 10, C, 2, 0.9f} } },
    { 2, 6, 2.28f, { {EN_BEETLE, 7, C, 0, 1.6f}, {EN_GRUNT, 12, C, 3, 0.8f} } },
    { 2, 6, 2.64f, { {EN_SLIME, 12, C, 0, 0.9f}, {EN_CROW, 8, A, 5, 0.9f} } },
    { 3, 6, 3.12f, { {EN_BRUTE, 3, C, 0, 4.0f}, {EN_FOX, 16, C, 2, 0.6f}, {EN_BEETLE, 5, C, 8, 1.6f} } },
    { 3, 6, 3.60f, { {EN_CROW, 12, A, 0, 0.7f}, {EN_SLIME, 12, C, 2, 0.9f}, {EN_BEETLE, 6, C, 6, 1.4f} } },
    { 2, 6, 4.08f, { {EN_BEETLE, 12, C, 0, 1.1f}, {EN_FOX, 20, R, 4, 0.45f} } },
    { 3, 6, 4.68f, { {EN_BRUTE, 6, C, 0, 2.6f}, {EN_SLIME, 16, C, 3, 0.7f}, {EN_CROW, 10, A, 8, 0.7f} } },
    { 3, 6, 5.28f, { {EN_FOX, 26, C, 0, 0.4f}, {EN_BEETLE, 12, C, 4, 1.0f}, {EN_CROW, 14, A, 6, 0.55f} } },
    { 5, 6, 5.76f, { {EN_PUMPKIN, 1, 0, 4, 1.0f}, {EN_BEETLE, 8, C, 0, 2.2f}, {EN_SLIME, 14, C, 3, 0.8f},
                     {EN_CROW, 12, A, 10, 0.65f}, {EN_FOX, 24, R, 16, 0.35f} } },
};
#undef C
#undef R
#undef A

#define NW(a) (int)(sizeof a / sizeof a[0])
const LevelDef LEVELS[] = {
    {   "THE STORM", "THE STORM GOLEM COMES!",
        {
            "TTTTT.TTTTTTTT.TTTT",
            "T,,,b.t,,,,,,#.oooT",
            "T,,,b.#mmm#,,#...yT",
            "T,,,b.....#,,rr#.yT",
            "Tpp##g###.##,,,t...",
            "Tp...#t#FFF#oo##.#T",
            "Tp.c....FFF......bT",
            "T#.cccm#FFF..t##bbT",
            "...t,#...rr#....#,T",
            "Tgg#,#.#yyy#t##.mmT",
            "T,,,,t.....#,,#....",
            "T,,,,#ooo#.#,,#gggT",
            "TTTTTTTTTT.TTTTTTTT",
        },
        /* N, S, E, W, NE, SE: the first waves come from two sides, then four, then every direction */
        { {5, 0}, {10, 12}, {18, 4}, {0, 8}, {14, 0}, {18, 10} },
        WAVES1, NW(WAVES1), ENV_STORM, 200,
    },
    {   "AUTUMN GOLD", "THE PUMPKIN KING COMES!",
        {   /* a ring road round the fire with only two ways in (north and south) */
            "TTT.TTTTTTTTTTT.TTT",
            "Tyy.#,,,,#......#,T",
            "T...##oo##.#mmm###T",
            "T.#bb#.......#,#...",
            "T.#bb#.rr.yy.#g#.#T",
            "T....#.pFFFp.#...cT",
            "Tgg#...pFFFp...#ccT",
            "T,,#o#.#FFF#.#yy#,T",
            "T,,#o#.mm.bb....#,T",
            "T#r#.........oo.##T",
            ".....#yyy#mm###...T",
            "Tpp#.....#,,,,#gg.T",
            "TTTTTTTT.TTTTTTTT.T",
        },
        /* NW, SE, E, W, N, S */
        { {3, 0}, {17, 12}, {18, 3}, {0, 10}, {15, 0}, {8, 12} },
        WAVES2, NW(WAVES2), ENV_AUTUMN, 240,
    },
};
const int NUM_LEVELS = NW(LEVELS);

void map_load(Game *g)
{
    for (int r = 0; r < MH; r++)
        for (int c = 0; c < MW; c++) {
            Tile *t = &g->tile[r][c];
            char ch = g->lv->rows[r][c];
            t->torch = 0; t->color = 0; t->puddle = 0; t->h = 0.0f;
            switch (ch) {
            case '.': t->kind = TL_PATH; break;
            case 'F': t->kind = TL_FIRE; break;
            case ',': t->kind = TL_LAWN; break;
            case 'T': t->kind = TL_TREE; break;
            case 't': t->kind = TL_WALL; t->torch = 1; t->h = 0.5f; break;
            case '#': t->kind = TL_WALL; t->h = 0.5f; break;
            default: {
                static const char cols[] = "roygmbpc";
                t->kind = TL_BLOCK; t->h = 0.62f;
                for (int i = 0; i < BC_COUNT; i++) if (cols[i] == ch) t->color = (uint8_t)i;
                break;
            }
            }
            /* a few puddles on the paths and the grass */
            uint32_t hsh = (uint32_t)(r * 7919 + c * 104729) * 2654435761u;
            if (g->lv->env == ENV_STORM && (t->kind == TL_PATH || t->kind == TL_LAWN) && (hsh >> 27) < 6) t->puddle = (uint8_t)(1 + (hsh >> 8) % 3);
        }

    /* flow field: breadth-first search outward from the fire along walkable tiles */
    static int qx[MW * MH], qz[MW * MH];
    int qh = 0, qt = 0;
    for (int r = 0; r < MH; r++)
        for (int c = 0; c < MW; c++) {
            g->dist[r][c] = -1;
            g->nxt[r][c][0] = g->nxt[r][c][1] = -1;
            if (g->tile[r][c].kind == TL_FIRE) { g->dist[r][c] = 0; qx[qt] = c; qz[qt] = r; qt++; }
        }
    static const int D4[4][2] = { {1, 0}, {-1, 0}, {0, 1}, {0, -1} };
    while (qh < qt) {
        int c = qx[qh], r = qz[qh]; qh++;
        for (int k = 0; k < 4; k++) {
            int nc = c + D4[k][0], nr = r + D4[k][1];
            if (nc < 0 || nr < 0 || nc >= MW || nr >= MH) continue;
            if (g->dist[nr][nc] >= 0 || g->tile[nr][nc].kind != TL_PATH) continue;
            g->dist[nr][nc] = g->dist[r][c] + 1;
            g->nxt[nr][nc][0] = (int8_t)c; g->nxt[nr][nc][1] = (int8_t)r;
            qx[qt] = nc; qz[qt] = nr; qt++;
        }
    }
}
