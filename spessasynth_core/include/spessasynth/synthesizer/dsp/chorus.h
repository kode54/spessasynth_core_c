#ifndef SS_CHORUS_H
#define SS_CHORUS_H

#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#ifdef _MSC_VER
#include "spessasynth_exports.h"
#else
#define SPESSASYNTH_EXPORTS
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	unsigned char level;
	unsigned char preLowpass;
	unsigned char feedback;
	unsigned char delay;
	unsigned char rate;
	unsigned char depth;
	unsigned char sendLevelToReverb;
	unsigned char sendLevelToDelay;
} SS_ChorusParams;

typedef struct {
	SS_ChorusParams parameters;
	unsigned int maxBufferSize;
	double preLPFfc;
	double preLPFa;
	double preLPFz;
	float *leftDelayBuffer;
	float *rightDelayBuffer;
	double sampleRate;
	double phase;
	unsigned int write;
	double gain;
	double reverbGain;
	double delayGain;
	/* Fractional sample counts: the read position is interpolated, so
	 * rounding these to whole samples quantizes the modulation itself. */
	double depthSamples;
	double delaySamples;
	double rateInc;
	double feedbackGain;
} SS_Chorus;

SS_Chorus SPESSASYNTH_EXPORTS *ss_chorus_create(double sampleRate, int maxBufferSize);
void SPESSASYNTH_EXPORTS ss_chorus_clear(SS_Chorus *chorus);
void SPESSASYNTH_EXPORTS ss_chorus_free(SS_Chorus *chorus);

void SPESSASYNTH_EXPORTS ss_chorus_set_send_level_to_reverb(SS_Chorus *chorus, unsigned char value);
void SPESSASYNTH_EXPORTS ss_chorus_set_send_level_to_delay(SS_Chorus *chorus, unsigned char value);
void SPESSASYNTH_EXPORTS ss_chorus_set_pre_lowpass(SS_Chorus *e, unsigned char value);
void SPESSASYNTH_EXPORTS ss_chorus_set_depth(SS_Chorus *chorus, unsigned char value);
void SPESSASYNTH_EXPORTS ss_chorus_set_delay(SS_Chorus *chorus, unsigned char value);
void SPESSASYNTH_EXPORTS ss_chorus_set_feedback(SS_Chorus *chorus, unsigned char value);
void SPESSASYNTH_EXPORTS ss_chorus_set_rate(SS_Chorus *chorus, unsigned char value);
void SPESSASYNTH_EXPORTS ss_chorus_set_level(SS_Chorus *chorus, unsigned char value);

void SPESSASYNTH_EXPORTS ss_chorus_set_macro(SS_Chorus *chorus, unsigned char value);

void SPESSASYNTH_EXPORTS ss_chorus_process(SS_Chorus *chorus,
                                           const float *input,
                                           float *outputL, float *outputR,
                                           float *outputReverb,
                                           float *outputDelay,
                                           int sample_count);

#ifdef __cplusplus
}
#endif

#endif /* SS_CHORUS_H */
