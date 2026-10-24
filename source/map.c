/*
 * STORMFIRE - level data: the map, its six gates, tower and enemy stats, and the waves.
 *
 * Map legend
 *   .  path (enemies walk here)          F  the campfire plaza (3 x 3)
 *   #  brick wall (build on top)         t  brick wall with a torch
 *   ,  grass lawn (build here too)       T  forest (nothing goes here)
 *   r o y g m b p c   coloured blocks (build on top): red orange yellow green mint blue purple cyan
 */
#include "game_int.h"

const char *const MAP_ROWS[MH] = {
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
};

/* N, S, E, W, NE, SE - "cycle" groups walk round the first `gates` of these in order, so the
 * first waves come from two sides, then four, then from every direction */
const int GATES[NUM_GATES][2] = { {5, 0}, {10, 12}, {18, 4}, {0, 8}, {14, 0}, {18, 10} };

const TowerDef TOWERS[TW_TYPES] = {
    /* name       cost            range              damage          rate              extra            air  colour      glow */
    { "BLASTER", {50, 45, 80},  {2.6f, 2.9f, 3.2f}, {10, 17, 28},   {3.0f, 3.4f, 4.0f}, {0, 0, 0},       1, 0x3C8CF0FF, 0x70E8FFFF },
    { "MORTAR",  {80, 70, 120}, {3.3f, 3.7f, 4.1f}, {34, 58, 95},   {0.6f, 0.65f, 0.7f}, {0.95f, 1.1f, 1.3f}, 0, 0xF0603CFF, 0xFFA040FF },
    { "FROST",   {60, 50, 90},  {2.2f, 2.5f, 2.8f}, {4, 7, 12},     {1.0f, 1.1f, 1.25f}, {0.4f, 0.5f, 0.6f}, 1, 0x8CDCFFFF, 0x9CF4FFFF },
    { "TESLA",   {120, 100, 150}, {2.5f, 2.8f, 3.1f}, {22, 36, 55}, {0.9f, 1.0f, 1.15f}, {3, 4, 6},       1, 0xF8C838FF, 0xFFF080FF },
};

const EnemyDef ENEMIES[EN_TYPES] = {
    /* name       hp     speed  size   fly  bounty damage */
    { "CRAWLER",   30,   1.00f, 0.26f, 0,   5,   1 },
    { "GRUNT",     85,   0.72f, 0.34f, 0,   8,   1 },
    { "BRUTE",    360,   0.48f, 0.48f, 0,  20,   3 },
    { "WISP",      50,   0.85f, 0.30f, 1,   6,   1 },
    { "STORM GOLEM", 1600, 0.30f, 0.80f, 0, 150, 10 },
};

#define C GATE_CYCLE
#define R GATE_RANDOM
#define A GATE_AIR
const WaveDef WAVES[] = {
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
const int NUM_WAVES = (int)(sizeof WAVES / sizeof WAVES[0]);
#undef C
#undef R
#undef A

void map_load(Game *g)
{
    for (int r = 0; r < MH; r++)
        for (int c = 0; c < MW; c++) {
            Tile *t = &g->tile[r][c];
            char ch = MAP_ROWS[r][c];
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
            if ((t->kind == TL_PATH || t->kind == TL_LAWN) && (hsh >> 27) < 6) t->puddle = (uint8_t)(1 + (hsh >> 8) % 3);
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
