/**
 * chorus.c
 * A simple chorus filter.  Basically a port of chorus.ts.
 */

#define _USE_MATH_DEFINES
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if __has_include(<spessasynth_core/spessasynth.h>)
#include <spessasynth_core/chorus.h>
#else
#include "spessasynth/synthesizer/dsp/chorus.h"
#endif

#ifndef min
#define min(a, b) (((a) < (b)) ? (a) : (b))
#endif
#ifndef max
#define max(a, b) (((a) > (b)) ? (a) : (b))
#endif

SS_Chorus *ss_chorus_create(float sampleRate, int maxBufferSize) {
	SS_Chorus *chorus = (SS_Chorus *)calloc(1, sizeof(*chorus));
	if(!chorus) return NULL;

	/*chorus->parameters.sendLevelToReverb = 0;
	chorus->parameters.sendLevelToDelay = 0;
	chorus->parameters.preLowpass = 0;
	chorus->parameters.depth = 0;
	chorus->parameters.delay = 0;
	chorus->parameters.feedback = 0;
	chorus->parameters.rate = 0;*/
	chorus->parameters.level = 64;

	chorus->preLPFfc = 8000;
	/*chorus->preLPFa = 0;
	chorus->preLPFz = 0;*/

	/*chorus->phase = 0;
	chorus->write = 0;*/
	chorus->gain = 0.5;
	/*chorus->reverbGain = 0;
	chorus->delayGain = 0;
	chorus->depthSamples = 0;*/
	chorus->delaySamples = 1;
	/*chorus->rateInc = 0;
	chorus->feedbackGain = 0;*/

	/*chorus->leftDelayBuffer = NULL;
	chorus->rightDelayBuffer = NULL;*/

	chorus->sampleRate = sampleRate;

	// Override
	maxBufferSize = (unsigned int)round(sampleRate);

	chorus->maxBufferSize = maxBufferSize;

	chorus->leftDelayBuffer = (float *)calloc(maxBufferSize, sizeof(float));
	if(!chorus->leftDelayBuffer) return 0;
	chorus->rightDelayBuffer = (float *)calloc(maxBufferSize, sizeof(float));
	if(!chorus->rightDelayBuffer) goto out_of_memory;

	return chorus;

out_of_memory:
	ss_chorus_free(chorus);
	return NULL;
}

void ss_chorus_clear(SS_Chorus *chorus) {
	if(!chorus || !chorus->leftDelayBuffer || !chorus->rightDelayBuffer) return;
	memset(chorus->leftDelayBuffer, 0, chorus->maxBufferSize * sizeof(float));
	memset(chorus->rightDelayBuffer, 0, chorus->maxBufferSize * sizeof(float));
	chorus->preLPFz = 0;
	chorus->phase = 0;
	chorus->write = 0;
}

void ss_chorus_free(SS_Chorus *chorus) {
	if(!chorus) return;
	free(chorus->leftDelayBuffer);
	free(chorus->rightDelayBuffer);
	free(chorus);
}

void ss_chorus_set_send_level_to_reverb(SS_Chorus *chorus, unsigned char value) {
	chorus->parameters.sendLevelToReverb = value;
	chorus->reverbGain = (float)value / 127.0;
}

void ss_chorus_set_send_level_to_delay(SS_Chorus *chorus, unsigned char value) {
	chorus->parameters.sendLevelToDelay = value;
	chorus->delayGain = (float)value / 127.0;
}

void ss_chorus_set_pre_lowpass(SS_Chorus *chorus, unsigned char value) {
	chorus->parameters.preLowpass = value;
	// GS sure loves weird mappings, huh?
	// Maps to around 8000-300 Hz
	chorus->preLPFfc = 8000.0f * powf(0.63f, (float)value);
	const float decay = expf((-2.0f * M_PI * chorus->preLPFfc) / chorus->sampleRate);
	chorus->preLPFa = 1.0 - decay;
}

void ss_chorus_set_depth(SS_Chorus *chorus, unsigned char value) {
	chorus->parameters.depth = value;
	chorus->depthSamples = (unsigned int)round(((float)value / 127.0) * 0.025 * chorus->sampleRate);
}

void ss_chorus_set_delay(SS_Chorus *chorus, unsigned char value) {
	chorus->parameters.delay = value;
	const unsigned int delaySamples = (unsigned int)round(((float)value / 127.0) * 0.025 * chorus->sampleRate);
	chorus->delaySamples = delaySamples > 1 ? delaySamples : 1;
}

void ss_chorus_set_feedback(SS_Chorus *chorus, unsigned char value) {
	chorus->parameters.feedback = value;
	chorus->feedbackGain = (float)value / 128.0;
}

void ss_chorus_set_rate(SS_Chorus *chorus, unsigned char value) {
	chorus->parameters.rate = value;
	const float rate = 15.5 * ((float)value / 127.0);
	chorus->rateInc = rate / chorus->sampleRate;
}

void ss_chorus_set_level(SS_Chorus *chorus, unsigned char value) {
	chorus->parameters.level = value;
	chorus->gain = ((float)value / 127.0) * 1.3;
}

void ss_chorus_set_macro(SS_Chorus *chorus, unsigned char value) {
	ss_chorus_set_level(chorus, 64);
	ss_chorus_set_pre_lowpass(chorus, 0);
	ss_chorus_set_delay(chorus, 127);
	ss_chorus_set_send_level_to_delay(chorus, 0);
	ss_chorus_set_send_level_to_reverb(chorus, 0);
	switch(value) {
			/**
			 * CHORUS MACRO is a macro parameter that allows global setting of chorus parameters.
			 * When you select the chorus type with CHORUS MACRO, each chorus parameter will be set to their
			 * most suitable value.
			 *
			 * Chorus1, Chorus2, Chorus3, Chorus4
			 * These are conventional chorus effects that add spaciousness and depth to the
			 * sound.
			 * Feedback Chorus
			 * This is a chorus with a flanger-like effect and a soft sound.
			 * Flanger
			 * This is an effect sounding somewhat like a jet airplane taking off and landing.
			 * Short Delay
			 * This is a delay with a short delay time.
			 * Short Delay (FB)
			 * This is a short delay with many repeats.
			 */
		case 0: {
			// Chorus1
			ss_chorus_set_feedback(chorus, 0);
			ss_chorus_set_delay(chorus, 112);
			ss_chorus_set_rate(chorus, 3);
			ss_chorus_set_depth(chorus, 5);
			break;
		}

		case 1: {
			// Chorus2
			ss_chorus_set_feedback(chorus, 5);
			ss_chorus_set_delay(chorus, 80);
			ss_chorus_set_rate(chorus, 9);
			ss_chorus_set_depth(chorus, 19);
			break;
		}

		case 2: {
			// Chorus3
			ss_chorus_set_feedback(chorus, 8);
			ss_chorus_set_delay(chorus, 80);
			ss_chorus_set_rate(chorus, 3);
			ss_chorus_set_depth(chorus, 19);
			break;
		}

		case 3: {
			// Chorus4
			ss_chorus_set_feedback(chorus, 16);
			ss_chorus_set_delay(chorus, 64);
			ss_chorus_set_rate(chorus, 9);
			ss_chorus_set_depth(chorus, 16);
			break;
		}

		case 4: {
			// FbChorus
			ss_chorus_set_feedback(chorus, 64);
			ss_chorus_set_delay(chorus, 127);
			ss_chorus_set_rate(chorus, 2);
			ss_chorus_set_depth(chorus, 24);
			break;
		}

		case 5: {
			// Flanger
			ss_chorus_set_feedback(chorus, 112);
			ss_chorus_set_delay(chorus, 127);
			ss_chorus_set_rate(chorus, 1);
			ss_chorus_set_depth(chorus, 5);
			break;
		}

		case 6: {
			// SDelay
			ss_chorus_set_feedback(chorus, 0);
			ss_chorus_set_delay(chorus, 127);
			ss_chorus_set_rate(chorus, 0);
			ss_chorus_set_depth(chorus, 127);
			break;
		}

		case 7: {
			// SDelayFb
			ss_chorus_set_feedback(chorus, 80);
			ss_chorus_set_delay(chorus, 127);
			ss_chorus_set_rate(chorus, 0);
			ss_chorus_set_depth(chorus, 127);
			break;
		}

		default: {
			return;
		}
	}
}

void ss_chorus_process(SS_Chorus *chorus,
                       const float *input,
                       float *outputL, float *outputR,
                       float *outputReverb,
                       float *outputDelay,
                       int sample_count) {
	float *bufferL = chorus->leftDelayBuffer;
	float *bufferR = chorus->rightDelayBuffer;
	const float rateInc = chorus->rateInc;
	const unsigned int bufferLen = chorus->maxBufferSize;
	const unsigned int depth = chorus->depthSamples;
	const unsigned int delay = chorus->delaySamples;
	const float gain = chorus->gain;
	const float reverbGain = chorus->reverbGain;
	const float delayGain = chorus->delayGain;
	const float feedback = chorus->feedbackGain;

	const bool preLPF = chorus->parameters.preLowpass > 0;
	float phase = chorus->phase;
	unsigned int write = chorus->write;
	float z = chorus->preLPFz;
	const float a = chorus->preLPFa;

	const bool outReverb = outputReverb && reverbGain > 0.0;
	const bool outDelay = outputDelay && delayGain > 0.0;

	int i;
	for(i = 0; i < sample_count; i++) {
		float inputSample = input[i];
		// Pre lowpass filter
		if(preLPF) {
			z += a * (inputSample - z);
			inputSample = z;
		}

		// Triangle LFO (GS uses triangle)
		const float lfo = 2.0 * fabs(phase - 0.5);

		// Read position
		const float dL = max(1.0, min(delay + lfo * depth, bufferLen));
		float readPosL = (float)write - dL;
		if(readPosL < 0.0) readPosL += (float)bufferLen;

		// Linear interpolation
		unsigned int x0 = (unsigned int)readPosL;
		unsigned int x1 = x0 + 1;
		if(x1 >= bufferLen) x1 -= bufferLen;
		float frac = readPosL - (float)x0;
		const float outL = bufferL[x0] * (1.0 - frac) + bufferL[x1] * frac;

		// Write input sample
		bufferL[write] = inputSample + outL * feedback;

		// Same for the right line (shared buffer for now for testing)
		const float dR = max(1.0, min(delay + (1.0 - lfo) * depth, bufferLen));
		float readPosR = (float)write - dR;
		if(readPosR < 0.0) readPosR += (float)bufferLen;
		readPosR = fmod(readPosR, (float)bufferLen);

		// Linear interpolation
		x0 = (unsigned int)readPosR;
		x1 = x0 + 1;
		if(x1 >= bufferLen) x1 -= bufferLen;
		frac = readPosR - (float)x0;
		const float outR = bufferR[x0] * (1.0 - frac) + bufferR[x1] * frac;

		// Write input sample
		bufferR[write] = inputSample + outR * feedback;

		// Mix outputs
		*outputL++ += outL * gain;
		*outputR++ += outR * gain;

		// Mono downmix for effects
		const float mono = (outL + outR) / 2.0;

		// Mix other effects outputs
		if(outReverb) {
			*outputReverb++ += mono * reverbGain;
		}
		if(outDelay) {
			*outputDelay++ += mono * delayGain;
		}

		// Advance pointers
		if(++write >= bufferLen) write = 0;

		if((phase += rateInc) >= 1.0) phase -= 1.0;
	}

	chorus->write = write;
	chorus->phase = phase;
	chorus->preLPFz = z;
}
