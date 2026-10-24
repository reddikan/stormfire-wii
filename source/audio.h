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

#define SFX_RATE 32000

enum {
    SFX_BLASTER, SFX_MORTAR, SFX_BOOM, SFX_FROST, SFX_ZAP, SFX_HIT, SFX_POP, SFX_CRUNCH,
    SFX_BUILD, SFX_UPGRADE, SFX_SELL, SFX_ERROR, SFX_MOVE, SFX_SELECT,
    SFX_WAVE, SFX_CLEAR, SFX_HISS, SFX_THUNDER, SFX_CRACK, SFX_BOSS,
    SFX_WIN, SFX_LOSE, SFX_PAUSE, SFX_START, SFX_COIN,
    SFX_GUST, SFX_CANNON, SFX_SPLASH,
    SFX_COUNT
};

void           audio_init(void);               /* synthesize every effect (call once) */
const int16_t *audio_data(int id);             /* 32-byte aligned sample buffer */
int            audio_len(int id);              /* length in samples (multiple of 16) */
int            audio_volume(int id);           /* suggested playback volume 0..255 */

#endif
