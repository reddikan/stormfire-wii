/*
 * STORMFIRE - a moody low-poly tower defense game for the Wii.
 *
 * game.c / draw.c / map.c / r3d.c / gfx.c / audio.c are plain C99 and know
 * nothing about the console. The game takes its input as a BTN_* bitmask plus
 * an analog stick, draws into the gfx triangle list, and emits sound events
 * that the platform layer (main.c) plays.
 */
#ifndef GAME_H
#define GAME_H

#include <stdint.h>
#include "audio.h"

/* ---- input ---- */
enum {
    BTN_UP    = 1 << 0,
    BTN_DOWN  = 1 << 1,
    BTN_LEFT  = 1 << 2,
    BTN_RIGHT = 1 << 3,
    BTN_A     = 1 << 4,     /* build / upgrade          */
    BTN_B     = 1 << 5,     /* sell                     */
    BTN_X     = 1 << 6,     /* call the next wave early */
    BTN_Y     = 1 << 7,     /* fast forward on / off    */
    BTN_L     = 1 << 8,     /* previous tower type      */
    BTN_R     = 1 << 9,     /* next tower type          */
    BTN_PLUS  = 1 << 10,    /* pause                    */
    BTN_ZL    = 1 << 11,    /* zoom out (L2)            */
    BTN_ZR    = 1 << 12     /* zoom in  (R2)            */
};

typedef struct {
    uint32_t held, pressed;
    float    sx, sy;        /* left stick, -1..1 (y down is +) */
} Input;

typedef struct Game Game;

/* sizeof(Game) for the platform layer to allocate */
extern const int GAME_SIZE;

void game_init(Game *g);                         /* once: also synthesizes the sound effects */
void game_update(Game *g, const Input *in, float dt);
void game_render(const Game *g);                 /* fills the gfx triangle list */
/* pops the next sound event, or -1. pitch: 128 = normal. pan: -1 left .. 1 right */
int  game_pop_sound(Game *g, int *pitch, float *pan);
/* how many simulation steps to run this frame (1, or 2 while fast-forwarding) */
int  game_speed(const Game *g);
/* background music track the game wants right now (music.h), or -1 for silence */
int  game_music(const Game *g);

#endif
