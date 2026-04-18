#ifndef SS_CHORUS_H
#define SS_CHORUS_H

#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

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
	float preLPFfc;
	float preLPFa;
	float preLPFz;
	float *leftDelayBuffer;
	float *rightDelayBuffer;
	float sampleRate;
	float phase;
	unsigned int write;
	float gain;
	float reverbGain;
	float delayGain;
	unsigned int depthSamples;
	unsigned int delaySamples;
	float rateInc;
	float feedbackGain;
} SS_Chorus;

SS_Chorus *ss_chorus_create(float sampleRate, int maxBufferSize);
void ss_chorus_clear(SS_Chorus *chorus);
void ss_chorus_free(SS_Chorus *chorus);

void ss_chorus_set_send_level_to_reverb(SS_Chorus *chorus, unsigned char value);
void ss_chorus_set_send_level_to_delay(SS_Chorus *chorus, unsigned char value);
void ss_chorus_set_pre_lowpass(SS_Chorus *e, unsigned char value);
void ss_chorus_set_depth(SS_Chorus *chorus, unsigned char value);
void ss_chorus_set_delay(SS_Chorus *chorus, unsigned char value);
void ss_chorus_set_feedback(SS_Chorus *chorus, unsigned char value);
void ss_chorus_set_rate(SS_Chorus *chorus, unsigned char value);
void ss_chorus_set_level(SS_Chorus *chorus, unsigned char value);

void ss_chorus_set_macro(SS_Chorus *chorus, unsigned char value);

void ss_chorus_process(SS_Chorus *chorus,
                       const float *input,
                       float *outputL, float *outputR,
                       float *outputReverb,
                       float *outputDelay,
                       int sample_count);

#ifdef __cplusplus
}
#endif

#endif /* SS_CHORUS_H */
