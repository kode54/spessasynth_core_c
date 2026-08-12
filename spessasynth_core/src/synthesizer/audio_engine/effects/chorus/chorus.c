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
	chorus->delaySamples = 1.0f;
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
	chorus->reverbGain = (float)value / 127.0f;
}

void ss_chorus_set_send_level_to_delay(SS_Chorus *chorus, unsigned char value) {
	chorus->parameters.sendLevelToDelay = value;
	chorus->delayGain = (double)value / 127.0;
}

void ss_chorus_set_pre_lowpass(SS_Chorus *chorus, unsigned char value) {
	chorus->parameters.preLowpass = value;
	// GS sure loves weird mappings, huh?
	// Maps to around 8000-300 Hz
	chorus->preLPFfc = 8000.0 * pow(0.63, (double)value);
	const double decay = exp((-2.0 * M_PI * chorus->preLPFfc) / chorus->sampleRate);
	chorus->preLPFa = 1.0 - decay;
}

void ss_chorus_set_depth(SS_Chorus *chorus, unsigned char value) {
	chorus->parameters.depth = value;
	chorus->depthSamples = ((double)value / 127.0) * 0.025 * chorus->sampleRate;
}

void ss_chorus_set_delay(SS_Chorus *chorus, unsigned char value) {
	chorus->parameters.delay = value;
	const double delaySamples = ((double)value / 127.0) * 0.025 * chorus->sampleRate;
	chorus->delaySamples = delaySamples > 1.0 ? delaySamples : 1.0;
}

void ss_chorus_set_feedback(SS_Chorus *chorus, unsigned char value) {
	chorus->parameters.feedback = value;
	/* GM2 section 4.5.4 */
	chorus->feedbackGain = (double)value * 0.00763;
}

void ss_chorus_set_rate(SS_Chorus *chorus, unsigned char value) {
	chorus->parameters.rate = value;
	/* GS Advanced Editor actually specifies the rate!
	 * 127 - 15.50Hz, 1 - 0.12 Hz
	 * And GM2 section 4.5.2 actually specifies the equation!
	 */
	const double rate = (double)value * 0.122;
	chorus->rateInc = rate / chorus->sampleRate;
}

void ss_chorus_set_level(SS_Chorus *chorus, unsigned char value) {
	chorus->parameters.level = value;
	chorus->gain = ((double)value / 127.0) * 1.3;
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
	const double rateInc = chorus->rateInc;
	const unsigned int bufferLen = chorus->maxBufferSize;
	const double depth = chorus->depthSamples;
	const double delay = chorus->delaySamples;
	const double gain = chorus->gain;
	const double reverbGain = chorus->reverbGain;
	const double delayGain = chorus->delayGain;
	const double feedback = chorus->feedbackGain;

	const bool preLPF = chorus->parameters.preLowpass > 0;
	double phase = chorus->phase;
	unsigned int write = chorus->write;
	double z = chorus->preLPFz;
	const double a = chorus->preLPFa;

	const bool outReverb = outputReverb && reverbGain > 0.0;
	const bool outDelay = outputDelay && delayGain > 0.0;

	int i;
	for(i = 0; i < sample_count; i++) {
		double inputSample = input[i];
		// Pre lowpass filter
		if(preLPF) {
			z += a * (inputSample - z);
			inputSample = z;
		}

		// Triangle LFO (GS uses triangle)
		const double lfo = 2.0 * fabs(phase - 0.5);

		// Read position
		const double dL = max(1.0, min(delay + lfo * depth, (double)bufferLen));
		double readPosL = (double)write - dL;
		if(readPosL < 0.0) readPosL += (double)bufferLen;

		// Linear interpolation
		unsigned int x0 = (unsigned int)readPosL;
		unsigned int x1 = x0 + 1;
		if(x1 >= bufferLen) x1 -= bufferLen;
		double frac = readPosL - (double)x0;
		const double outL = bufferL[x0] * (1.0 - frac) + bufferL[x1] * frac;

		// Write input sample
		bufferL[write] = (float)(inputSample + outL * feedback);

		// Same for the right line (shared buffer for now for testing)
		const double dR = max(1.0, min(delay + (1.0 - lfo) * depth, (double)bufferLen));
		double readPosR = (double)write - dR;
		if(readPosR < 0.0) readPosR += (double)bufferLen;
		readPosR = fmod(readPosR, (double)bufferLen);

		// Linear interpolation
		x0 = (unsigned int)readPosR;
		x1 = x0 + 1;
		if(x1 >= bufferLen) x1 -= bufferLen;
		frac = readPosR - (double)x0;
		const double outR = bufferR[x0] * (1.0 - frac) + bufferR[x1] * frac;

		// Write input sample
		bufferR[write] = (float)(inputSample + outR * feedback);

		// Mix outputs
		*outputL++ += outL * gain;
		*outputR++ += outR * gain;

		// Mono downmix for effects
		const double mono = (outL + outR) / 2.0;

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
