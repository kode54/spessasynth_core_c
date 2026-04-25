#ifndef SS_REVERB_H
#define SS_REVERB_H

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#if __has_include(<spessasynth_core/spessasynth.h>)
#include <spessasynth_core/dattorro.h>
#include <spessasynth_core/delay_line.h>
#else
#include "spessasynth/synthesizer/dsp/dattorro.h"
#include "spessasynth/synthesizer/dsp/delay_line.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	unsigned char level;
	unsigned char preLowpass;
	unsigned char character;
	unsigned char time;
	unsigned char delayFeedback;
	unsigned char preDelayTime;
} SS_ReverbParams;

typedef struct {
	SS_ReverbParams parameters;
	SS_DattorroReverb *dattorro;
	SS_DelayLine *delayLeft;
	SS_DelayLine *delayRight;
	unsigned int maxBufferSize;
	float *delayLeftOutput;
	float *delayRightOutput;
	float *delayLeftInput;
	float *delayPreLPF;
	float sampleRate;
	float preLPFfc;
	float preLPFa;
	float preLPFz;
	float characterTimeCoefficient;
	float characterGainCoefficient;
	float characterLPFCoefficient;
	float delayGain;
	float panDelayFeedback;
	float delayFeedback;
} SS_Reverb;

SS_Reverb *ss_reverb_create(float sampleRate, int maxBufferSize);
void ss_reverb_clear(SS_Reverb *reverb);
void ss_reverb_free(SS_Reverb *reverb);

void ss_reverb_set_delay_feedback(SS_Reverb *reverb, unsigned char delayFeedback);
void ss_reverb_set_character(SS_Reverb *reverb, unsigned char character);
void ss_reverb_set_time(SS_Reverb *e, unsigned char time);
void ss_reverb_set_pre_delay_time(SS_Reverb *reverb, unsigned char preDelayTime);
void ss_reverb_set_level(SS_Reverb *reverb, unsigned char level);
void ss_reverb_set_pre_lowpass(SS_Reverb *reverb, unsigned char preLowpass);

void ss_reverb_set_macro(SS_Reverb *reverb, unsigned char value);

void ss_reverb_process(SS_Reverb *reverb,
                       const float *input,
                       float *outputL, float *outputR,
                       int sample_count);

#ifdef __cplusplus
}
#endif

#endif /* SS_REVERB_H */
