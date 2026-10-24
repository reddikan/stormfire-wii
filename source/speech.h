/*
 * STORMFIRE - a tiny formant speech synthesizer (arcade-style pirate voices). See speech.c for the phoneme codes.
 */
#ifndef SPEECH_H
#define SPEECH_H

#include <stdint.h>

/* render a phrase into out[] at sample rate sr; pitch 1 = a deep pirate voice, rate scales the durations,
 * growl 0..1 how gravelly. Returns the number of samples written. */
int speech_render(const char *phrase, float pitch, float rate, float growl, float *out, int maxlen, float sr);

#endif
