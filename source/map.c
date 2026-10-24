/*
 * STORMFIRE - level data: maps, gates, tower and enemy stats, and the waves.
 *
 * Map legend
 *   .  path (enemies walk here)          F  the campfire plaza (3 x 3, always in the middle)
 *   #  wall (build on top)               t  wall with a torch
 *   ,  grass lawn (build here too)       T  forest (nothing goes here)
 *   G  graveyard, O orrery, R rune stone, S shipwreck, W open water (props: nothing goes here)
 *   r o y g m b p c   coloured blocks (build on top): red orange yellow green mint blue purple cyan
 */
#include "game_int.h"

int   MW = 19, MH = 13;
float FIRE_X = 9.5f, FIRE_Z = 6.5f;
int   FIRE_C = 9, FIRE_R = 6;

const TowerDef TOWERS[TW_TYPES] = {
    /* name       cost            range              damage          rate              extra            air  colour      glow */
    { "BLASTER", {50, 45, 80},  {2.6f, 2.9f, 3.2f}, {10, 17, 28},   {3.0f, 3.4f, 4.0f}, {0, 0, 0},       1, 0x3C8CF0FF, 0x70E8FFFF },
    { "MORTAR",  {80, 70, 120}, {3.3f, 3.7f, 4.1f}, {34, 58, 95},   {0.6f, 0.65f, 0.7f}, {0.95f, 1.1f, 1.3f}, 0, 0xF0603CFF, 0xFFA040FF },
    { "FROST",   {60, 50, 90},  {2.2f, 2.5f, 2.8f}, {4, 7, 12},     {1.0f, 1.1f, 1.25f}, {0.4f, 0.5f, 0.6f}, 1, 0x8CDCFFFF, 0x9CF4FFFF },
    { "TESLA",   {120, 100, 150}, {2.5f, 2.8f, 3.1f}, {22, 36, 55}, {0.9f, 1.0f, 1.15f}, {3, 4, 6},       1, 0xF8C838FF, 0xFFF080FF },
    /* the prism: a continuous beam that locks on and ramps up (damage is per second; extra = ramp at full lock) */
    { "PRISM",   {300, 220, 320}, {3.6f, 4.0f, 4.5f}, {34, 56, 90}, {5.0f, 5.0f, 5.0f}, {3.0f, 3.5f, 4.0f}, 1, 0xD060F0FF, 0xFF98FFFF },
};

const EnemyDef ENEMIES[EN_TYPES] = {
    /* name           hp    speed  size   fly bounty dmg armor boss split            colour       smash reach cd   summon */
    { "CRAWLER",       30,  1.00f, 0.26f, 0,   5,   1,  0,   0, -1, 0,           0x3CB4E6FF,  0, 0, 0,     -1, 0, 0 },
    { "GRUNT",         85,  0.72f, 0.34f, 0,   8,   1,  0,   0, -1, 0,           0x9A5CE8FF,  0, 0, 0,     -1, 0, 0 },
    { "BRUTE",        360,  0.48f, 0.48f, 0,  20,   3,  0,   0, -1, 0,           0xC8483CFF,  0, 0, 0,     -1, 0, 0 },
    { "WISP",          50,  0.85f, 0.30f, 1,   6,   1,  0,   0, -1, 0,           0x9CFFD8FF,  0, 0, 0,     -1, 0, 0 },
    { "STORM GOLEM", 1600,  0.30f, 0.80f, 0, 150,  10,  0,   1, -1, 0,           0x7C8894FF,  0, 0, 0,     -1, 0, 0 },
    /* level 2 */
    { "BEETLE",       150,  0.62f, 0.36f, 0,  12,   1,  5,   0, -1, 0,           0x2E8C7CFF,  0, 0, 0,     -1, 0, 0 },
    { "SLIME",         95,  0.66f, 0.36f, 0,   8,   1,  0,   0, EN_SLIMELET, 2,  0x88D848FF,  0, 0, 0,     -1, 0, 0 },
    { "SLIMELET",      26,  0.72f, 0.22f, 0,   3,   1,  0,   0, -1, 0,           0xA8E860FF,  0, 0, 0,     -1, 0, 0 },
    { "FOX",           70,  0.85f, 0.30f, 0,   8,   1,  0,   0, -1, 0,           0xE8702CFF,  0, 0, 0,     -1, 0, 0 },
    { "CROW",          75,  0.95f, 0.30f, 1,   9,   1,  0,   0, -1, 0,           0x3A3044FF,  0, 0, 0,     -1, 0, 0 },
    { "PUMPKIN KING", 2000, 0.27f, 0.85f, 0, 200,  10,  4,   1, EN_GOURD, 4,     0xF08020FF,  0, 0, 0,     EN_GOURD, 2, 6.0f },
    { "GOURD",         45,  0.95f, 0.24f, 0,   3,   1,  0,   0, -1, 0,           0xF8B030FF,  0, 0, 0,     -1, 0, 0 },
    /* level 3: some of these smash towers on their way past */
    { "ZOMBIE",       200,  0.42f, 0.34f, 0,  15,   1,  0,   0, -1, 0,           0x6E9A5AFF, 10, 1.15f, 1.8f, -1, 0, 0 },
    { "GHOST",        110,  0.70f, 0.32f, 1,  14,   1,  0,   0, -1, 0,           0xD8E8F0FF,  0, 0, 0,     -1, 0, 0 },
    { "SKELETON",      80,  0.85f, 0.30f, 0,  12,   1,  0,   0, -1, 0,           0xE8E0CCFF,  0, 0, 0,     -1, 0, 0 },
    { "BAT",           34,  1.15f, 0.20f, 1,   5,   1,  0,   0, -1, 0,           0x2A2030FF,  0, 0, 0,     -1, 0, 0 },
    { "GRAVEBREAKER", 700,  0.36f, 0.50f, 0,  45,   3,  4,   0, -1, 0,           0x5C6A70FF, 40, 1.25f, 2.2f, -1, 0, 0 },
    { "GHOUL KING",  2400,  0.24f, 0.85f, 0, 250,  10,  4,   1, EN_ZOMBIE, 3,    0x7A8A5AFF, 70, 1.5f, 2.4f,  EN_ZOMBIE, 2, 7.0f },
    /* level 4: clockwork things from the old machines of the island */
    { "AUTOMATON",    260,  0.55f, 0.34f, 0,  16,   1,  6,   0, -1, 0,           0xC89A50FF,  0, 0, 0,     -1, 0, 0 },
    { "SPRINGER",     130,  0.75f, 0.28f, 0,  13,   1,  0,   0, -1, 0,           0x6AA0A0FF,  0, 0, 0,     -1, 0, 0 },
    { "BRASS MOTH",   150,  0.70f, 0.30f, 1,  15,   1,  0,   0, -1, 0,           0xD8B060FF,  0, 0, 0,     -1, 0, 0 },
    { "TINKER",       160,  0.60f, 0.30f, 0,  16,   1,  2,   0, -1, 0,           0x8A6A9AFF,  0, 0, 0,     -1, 0, 0 },
    { "SENTINEL",    1000,  0.33f, 0.55f, 0,  50,   3,  8,   0, -1, 0,           0x8A9088FF, 45, 1.3f, 2.4f,  -1, 0, 0 },
    { "ARCHITECT",   3200,  0.22f, 0.90f, 0, 300,  10,  6,   1, EN_AUTOMATON, 3, 0xB08848FF, 80, 1.6f, 2.6f,  EN_AUTOMATON, 2, 8.0f },
    /* level 5: pirates of the storm */
    { "ROWBOAT",      300,  0.55f, 0.40f, 0,  18,   1,  3,   0, -1, 0,           0x7A5434FF,  0, 0, 0,     -1, 0, 0 },
    { "SHARK",        200,  1.00f, 0.35f, 0,  16,   1,  0,   0, -1, 0,           0x6A7A88FF,  0, 0, 0,     -1, 0, 0 },
    { "SKULL PIRATE", 220,  0.70f, 0.32f, 0,  16,   1,  0,   0, -1, 0,           0xE8E0CCFF,  0, 0, 0,     -1, 0, 0 },
    { "PARROT",        90,  1.10f, 0.26f, 1,   9,   1,  0,   0, -1, 0,           0xE83A2AFF,  0, 0, 0,     -1, 0, 0 },
    { "PIRATE SHIP", 1500,  0.30f, 0.60f, 0,  70,   3,  9,   0, -1, 0,           0x6A4428FF,  0, 0, 0,     -1, 0, 0 },
    { "KRAKEN",      4500,  0.20f, 1.10f, 0, 400,  10,  8,   1, EN_SHARK, 4,     0x7E3A72FF,  0, 0, 0,     EN_SHARK, 2, 9.0f },
};

const float TOWER_HP[TW_MAXLVL] = { 100, 150, 200 };

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

/* level 3: the dead walk - zombies and gravebreakers smash towers, ghosts fade out of reach, skeletons get back up */
static const WaveDef WAVES3[] = {
    /* n gates hp x  groups: {type, count, gate, delay, gap} */
    { 2, 2, 1.09f, { {EN_ZOMBIE, 5, C, 0, 2.4f}, {EN_BAT, 5, A, 8, 0.8f} } },
    { 2, 2, 1.24f, { {EN_SKELETON, 10, C, 0, 1.2f}, {EN_ZOMBIE, 4, C, 6, 2.0f} } },
    { 2, 4, 1.43f, { {EN_GHOST, 6, A, 0, 1.6f}, {EN_SKELETON, 8, C, 3, 1.0f} } },
    { 2, 4, 1.66f, { {EN_ZOMBIE, 10, C, 0, 1.2f}, {EN_BAT, 14, A, 5, 0.35f} } },
    { 2, 6, 1.94f, { {EN_BREAKER, 2, C, 0, 6.0f}, {EN_SKELETON, 14, C, 2, 0.7f} } },
    { 2, 6, 2.28f, { {EN_GHOST, 10, A, 0, 1.0f}, {EN_ZOMBIE, 12, C, 2, 0.9f} } },
    { 3, 6, 2.67f, { {EN_BAT, 20, A, 0, 0.3f}, {EN_SKELETON, 16, C, 2, 0.6f}, {EN_BREAKER, 2, C, 8, 5.0f} } },
    { 3, 6, 3.09f, { {EN_ZOMBIE, 16, C, 0, 0.7f}, {EN_GHOST, 10, A, 4, 0.8f}, {EN_BEETLE, 6, C, 8, 1.2f} } },
    { 2, 6, 3.56f, { {EN_BREAKER, 5, C, 0, 3.0f}, {EN_SKELETON, 20, R, 3, 0.45f} } },
    { 3, 6, 4.09f, { {EN_GHOST, 16, A, 0, 0.6f}, {EN_ZOMBIE, 18, C, 2, 0.6f}, {EN_BAT, 16, A, 8, 0.3f} } },
    { 3, 6, 4.66f, { {EN_SKELETON, 26, C, 0, 0.4f}, {EN_BREAKER, 6, C, 4, 2.4f}, {EN_GHOST, 12, A, 6, 0.6f} } },
    { 5, 6, 5.14f, { {EN_GHOULKING, 1, 0, 4, 1.0f}, {EN_ZOMBIE, 14, C, 0, 1.0f}, {EN_SKELETON, 20, C, 3, 0.5f},
                     {EN_GHOST, 10, A, 10, 0.8f}, {EN_BAT, 12, A, 16, 0.4f} } },
};

/* level 4: clockwork - armoured automatons, hopping springers, brass moths that shield, tinkers that repair,
 * sentinels that smash towers, and the Architect, whose shockwave jams towers */
static const WaveDef WAVES4[] = {
    /* n gates hp x  groups: {type, count, gate, delay, gap} */
    { 2, 2, 0.89f, { {EN_AUTOMATON, 6, C, 0, 2.0f}, {EN_SPRINGER, 6, C, 6, 1.4f} } },
    { 2, 2, 1.02f, { {EN_SPRINGER, 12, C, 0, 1.0f}, {EN_TINKER, 3, C, 6, 3.0f} } },
    { 2, 4, 1.20f, { {EN_MOTH, 4, A, 0, 2.5f}, {EN_AUTOMATON, 10, C, 2, 1.1f} } },
    { 3, 4, 1.41f, { {EN_TINKER, 5, C, 0, 2.0f}, {EN_SKELETON, 14, C, 2, 0.7f}, {EN_MOTH, 4, A, 8, 2.0f} } },
    { 2, 6, 1.68f, { {EN_SENTINEL, 2, C, 0, 7.0f}, {EN_AUTOMATON, 12, C, 2, 0.9f} } },
    { 2, 6, 1.94f, { {EN_SPRINGER, 24, C, 0, 0.5f}, {EN_MOTH, 6, A, 4, 1.2f} } },
    { 3, 6, 2.30f, { {EN_AUTOMATON, 16, C, 0, 0.7f}, {EN_TINKER, 6, C, 4, 1.8f}, {EN_GHOST, 8, A, 8, 0.9f} } },
    { 3, 6, 2.66f, { {EN_SENTINEL, 3, C, 0, 5.0f}, {EN_MOTH, 8, A, 3, 1.0f}, {EN_SPRINGER, 20, C, 6, 0.5f} } },
    { 3, 6, 3.06f, { {EN_AUTOMATON, 20, C, 0, 0.55f}, {EN_TINKER, 8, C, 3, 1.4f}, {EN_BAT, 16, A, 8, 0.3f} } },
    { 3, 6, 3.50f, { {EN_SENTINEL, 5, C, 0, 3.5f}, {EN_MOTH, 10, A, 2, 0.8f}, {EN_SKELETON, 20, C, 6, 0.45f} } },
    { 4, 6, 3.99f, { {EN_SPRINGER, 30, C, 0, 0.35f}, {EN_AUTOMATON, 16, C, 3, 0.7f}, {EN_MOTH, 10, A, 8, 0.7f}, {EN_TINKER, 6, C, 10, 2.0f} } },
    { 5, 6, 4.43f, { {EN_ARCHITECT, 1, 0, 4, 1.0f}, {EN_AUTOMATON, 14, C, 0, 1.0f}, {EN_TINKER, 6, C, 6, 2.5f},
                     {EN_MOTH, 10, A, 10, 0.9f}, {EN_SPRINGER, 24, R, 16, 0.4f} } },
};

/* level 5: the pirates come for the treasure - rowboats, sharks that dive out of reach, skull pirates that pull
 * themselves back together, parrots, pirate ships whose broadsides jam towers, and the Kraken */
static const WaveDef WAVES5[] = {
    /* n gates hp x  groups: {type, count, gate, delay, gap} */
    { 2, 2, 0.72f, { {EN_ROWBOAT, 8, C, 0, 2.2f}, {EN_PARROT, 4, A, 14, 1.5f} } },
    { 2, 2, 0.81f, { {EN_SHARK, 10, C, 0, 1.2f}, {EN_ROWBOAT, 4, C, 6, 2.0f} } },
    { 2, 4, 0.90f, { {EN_SKULLPIRATE, 10, C, 0, 1.2f}, {EN_PARROT, 8, A, 4, 0.8f} } },
    { 2, 4, 1.02f, { {EN_ROWBOAT, 12, C, 0, 1.0f}, {EN_SHARK, 8, C, 6, 1.0f} } },
    { 2, 6, 1.18f, { {EN_SHIP, 1, C, 0, 1.0f}, {EN_SKULLPIRATE, 12, C, 2, 0.8f} } },
    { 2, 6, 1.37f, { {EN_PARROT, 18, A, 0, 0.4f}, {EN_SHARK, 14, C, 2, 0.7f} } },
    { 2, 8, 1.67f, { {EN_SHIP, 2, C, 0, 7.0f}, {EN_ROWBOAT, 14, C, 2, 0.8f} } },
    { 3, 8, 1.98f, { {EN_SKULLPIRATE, 18, C, 0, 0.6f}, {EN_SHARK, 12, C, 4, 0.7f}, {EN_PARROT, 10, A, 8, 0.6f} } },
    { 2, 8, 2.33f, { {EN_ROWBOAT, 20, C, 0, 0.6f}, {EN_SHIP, 2, C, 4, 6.0f} } },
    { 3, 8, 2.73f, { {EN_SHARK, 24, C, 0, 0.45f}, {EN_SKULLPIRATE, 14, C, 4, 0.7f}, {EN_PARROT, 14, A, 8, 0.5f} } },
    { 3, 8, 3.17f, { {EN_SHIP, 4, C, 0, 4.0f}, {EN_ROWBOAT, 16, C, 2, 0.7f}, {EN_PARROT, 12, A, 6, 0.6f} } },
    { 3, 8, 3.65f, { {EN_SKULLPIRATE, 26, C, 0, 0.45f}, {EN_SHARK, 20, C, 3, 0.5f}, {EN_SHIP, 2, C, 10, 6.0f} } },
    { 4, 8, 4.14f, { {EN_ROWBOAT, 24, C, 0, 0.5f}, {EN_SHIP, 4, C, 3, 3.5f}, {EN_PARROT, 16, A, 6, 0.5f}, {EN_SHARK, 16, R, 10, 0.5f} } },
    { 3, 8, 4.58f, { {EN_SHIP, 6, C, 0, 3.0f}, {EN_SKULLPIRATE, 24, C, 2, 0.5f}, {EN_SHARK, 20, R, 6, 0.4f} } },
    { 6, 8, 4.75f, { {EN_KRAKEN, 1, 0, 4, 1.0f}, {EN_SHIP, 3, C, 0, 6.0f}, {EN_ROWBOAT, 16, C, 4, 0.8f},
                     {EN_SKULLPIRATE, 20, C, 8, 0.5f}, {EN_PARROT, 16, A, 12, 0.5f}, {EN_SHARK, 20, R, 16, 0.4f} } },
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
        6, { {5, 0}, {10, 12}, {18, 4}, {0, 8}, {14, 0}, {18, 10} },
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
        6, { {3, 0}, {17, 12}, {18, 3}, {0, 10}, {15, 0}, {8, 12} },
        WAVES2, NW(WAVES2), ENV_AUTUMN, 240,
    },
    {   "HALLOWS EVE", "THE GHOUL KING RISES!",
        {   /* long zigzag lanes into all four sides of the fire, and a graveyard in the corner */
            "TT.TTTTTTTTTTTTT.TT",
            "T#.##....#TT#....#T",
            "To....pp.#gg#.##ooT",
            "T#oo###p......#gg#T",
            "T...#,,#g.g###...pT",
            "T.#.#pp#FFF#oo.#.#T",
            "T.#.#...FFF....#...",
            "..g.#.##FFF#ppp#ooT",
            "Too...#p#..#gg##GGT",
            "T,#oo##........#GGT",
            "T,,#pp#.#oo#...#GGT",
            "T,,#....#TT#.###GGT",
            "TTTT.TTTTTTT.TTTTTT",
        },
        /* NW, S, E, W, NE, SW */
        6, { {2, 0}, {12, 12}, {18, 6}, {0, 7}, {16, 0}, {4, 12} },
        WAVES3, NW(WAVES3), ENV_HALLOW, 280,
    },
    {   "DAWN ISLE", "THE ARCHITECT AWAKENS!",
        {   /* an island of old stone and brass machines: enemies come ashore on six piers, and the fire
               has only two doors, east and west */
            "TTTTTTTTT.TTTTTTTTT",
            "T,R,#bb##.#,,R,#ccT",
            "T,O,#.....#,O,,#...",
            "T,,,#.#mmm#,#yy#.#T",
            "Too##.#rr#rr#....#T",
            "...#b.#pFFFp#.#gg#T",
            "T#.#....FFF...#oo#T",
            "Tg.g.#.pFFFp#....#T",
            "Tg...#.#yyyy#.#c.#T",
            "T#mm##.##bb##.##...",
            "T,#....#o.....#rr#T",
            "TR#.#ggg#.#####,O,T",
            "TTT.TTTTT.TTTTTTTTT",
        },
        /* N, S, W, E, NE, SW */
        6, { {9, 0}, {9, 12}, {0, 5}, {18, 9}, {18, 2}, {3, 12} },
        WAVES4, NW(WAVES4), ENV_ISLAND, 300,
    },
    {   "PIRATE STORM", "THE KRAKEN RISES FROM THE DEEP!",
        {   /* all at sea: docks and crates to build on, open-water lanes for the pirates, a treasure island in the middle */
            "TTTTT.TTTTTTTTTTTTTTT.TTTTT",
            "T,,,o.#SSS,,,,,,#bb##.#,,,T",
            "Tgg##.##rr#,,,,,#.....#,,,T",
            "...##.....#,SSS,#.##yy####T",
            "T#.#pp###.##mm###.#,,,#....",
            "T#....#,#.........#,W,#.##T",
            "Too##.###cc##.##bb#,,,#.#,T",
            "T,,,#....#r#FFF#yy#####.#,T",
            "Tgg##.##....FFF.........#,T",
            "T#....##pp##FFF#mm##.##o#,T",
            "T#.##rr,,##c#.####,#.##gg#T",
            "T#.#,,W,,#.......#,#.....#T",
            "...#,#yy##.##bb#.#,#pp##.#T",
            "T#oo,#.....#,S,#.#mm##,#...",
            "T,,,,#.##cc#,,,#.....#,#rrT",
            "T,W,,#.#,,,,,,,##gg#.#,,,,T",
            "TTTTTT.TTTTTTTTTTTTT.TTTTTT",
        },
        /* NW, S, E, W, NE, SW, SE, far NW */
        8, { {5, 0}, {20, 16}, {26, 4}, {0, 12}, {21, 0}, {6, 16}, {26, 13}, {0, 3} },
        WAVES5, NW(WAVES5), ENV_PIRATE, 340,
    },
};
const int NUM_LEVELS = NW(LEVELS);

void map_load(Game *g)
{
    /* size and fire position come from the level's rows */
    MW = 0; MH = 0;
    while (MH < MAXH && g->lv->rows[MH]) MH++;
    for (const char *p = g->lv->rows[0]; *p && MW < MAXW; p++) MW++;
    float fx = 0, fz = 0;
    int nf = 0;
    for (int r = 0; r < MH; r++)
        for (int c = 0; c < MW; c++)
            if (g->lv->rows[r][c] == 'F') { fx += (float)c + 0.5f; fz += (float)r + 0.5f; nf++; }
    if (nf) { FIRE_X = fx / (float)nf; FIRE_Z = fz / (float)nf; }
    FIRE_C = (int)FIRE_X; FIRE_R = (int)FIRE_Z;

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
            case 'G': t->kind = TL_GRAVE; t->color = DECO_GRAVE; break;
            case 'O': t->kind = TL_GRAVE; t->color = DECO_ORRERY; break;
            case 'R': t->kind = TL_GRAVE; t->color = DECO_RUNE; break;
            case 'S': t->kind = TL_GRAVE; t->color = DECO_WRECK; break;
            case 'W': t->kind = TL_GRAVE; t->color = DECO_WATER; break;
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
    static int qx[MAXW * MAXH], qz[MAXW * MAXH];
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
