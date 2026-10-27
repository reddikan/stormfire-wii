/*
 * STORMFIRE - procedurally synthesized sound effects.
 *
 * No audio files: every effect is generated at start-up from FM / band-limited
 * oscillators and filtered noise. Samples are 16-bit signed mono at SFX_RATE Hz
 * in the host's byte order (big-endian on the Wii, which is what ASND wants).
 */
#ifndef AUDIO_H
#define AUDIO_H

#include <stdint.h>

#define SFX_RATE 22050

enum {
    SFX_BLASTER, SFX_MORTAR, SFX_BOOM, SFX_FROST, SFX_ZAP, SFX_HIT, SFX_POP, SFX_CRUNCH,
    SFX_BUILD, SFX_UPGRADE, SFX_SELL, SFX_ERROR, SFX_MOVE, SFX_SELECT,
    SFX_WAVE, SFX_CLEAR, SFX_HISS, SFX_THUNDER, SFX_CRACK, SFX_BOSS,
    SFX_WIN, SFX_LOSE, SFX_PAUSE, SFX_START, SFX_COIN,
    SFX_GUST, SFX_CANNON, SFX_SPLASH,
    /* level 6: a pinball machine */
    SFX_PB_BUMPER, SFX_PB_KICKER, SFX_PB_FLIPPER, SFX_PB_COMBO, SFX_PB_DROP, SFX_PB_BUZZ, SFX_PB_TICK, SFX_PB_DING,
    SFX_PB_LAUNCH, SFX_PB_JACKPOT, SFX_PB_TILT, SFX_PB_SCOTT, SFX_PB_WIN, SFX_PB_DRAIN, SFX_PB_WARP, SFX_PB_SURGE, SFX_PB_GIGA,
    SFX_PB_PERFECT,
    /* themed sets: levels 2, 3, 4 and 7 each have their own version of the everyday sounds (TH_* slots below) */
    SFX_TH_AUTUMN,
    SFX_TH_HALLOW = SFX_TH_AUTUMN + 18,
    SFX_TH_ISLAND = SFX_TH_HALLOW + 18,
    SFX_TH_SKY    = SFX_TH_ISLAND + 18,
    /* level 7 extras: magic */
    SFX_MG_COMBO  = SFX_TH_SKY + 18, SFX_MG_SURGE, SFX_MG_BLAST, SFX_MG_WARP, SFX_MG_FIREWORK, SFX_MG_PERFECT,
    SFX_COUNT
};

/* the slots of a themed set, in order */
enum { TH_POP, TH_CRUNCH, TH_BUILD, TH_UPGRADE, TH_SELL, TH_WAVE, TH_CLEAR, TH_BOSS, TH_LEAK, TH_WIN, TH_LOSE, TH_COIN,
       TH_SELECT, TH_BLASTER, TH_MORTAR, TH_BOOM, TH_FROST, TH_ZAP, TH_SLOTS };

void           audio_init(void);               /* synthesize every effect (call once) */
extern void  (*audio_progress)(int done, int total);   /* optional: called as the effects are made (loading screen) */
const int16_t *audio_data(int id);             /* 32-byte aligned sample buffer */
int            audio_len(int id);              /* length in samples (multiple of 16) */
int            audio_volume(int id);           /* suggested playback volume 0..255 */
int            audio_pool_used(void);          /* samples of the effect pool in use (for the tests) */

#endif
