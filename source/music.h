/*
 * STORMFIRE - procedurally composed and synthesized background music, one loop per level.
 *
 * Every track is written as note data in music.c and rendered at start-up into a seamless
 * mono 16-bit loop at MUS_RATE Hz (host byte order, 32-byte aligned for the Wii's DMA).
 *   0  The Storm     - slow, dreary minor pads, a lonely electric piano and a soft drone
 *   1  Autumn Gold   - fresh and simple: plucked guitar arpeggios, a gentle flute, light shaker
 *   2  Hallows Eve   - fun and spooky: bouncy pizzicato bass, a wobbly theremin tune, celesta,
 *                      finger snaps and xylophone "bones"
 */
#ifndef MUSIC_H
#define MUSIC_H

#include <stdint.h>

#define MUS_RATE   22050
#define MUS_TRACKS 3

void           music_init(void);            /* compose + render every track (call once) */
const int16_t *music_data(int track);
int            music_len(int track);        /* samples, a multiple of 16 */
int            music_volume(int track);     /* suggested playback volume 0..255 */

#endif
