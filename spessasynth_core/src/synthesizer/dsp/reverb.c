/**
 * reverb.c
 * A complex reverb and delay filter.  Basically a port of reverb.ts.
 *
 * Imports Dattorro Reverb and Delay Line
 */

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if __has_include(<spessasynth_core/spessasynth.h>)
#include <spessasynth_core/reverb.h>
#else
#include "spessasynth/synthesizer/dsp/reverb.h"
#endif

SS_Reverb *ss_reverb_create(float sampleRate, int maxBufferSize) {
	SS_Reverb *reverb = (SS_Reverb *)calloc(1, sizeof(*reverb));
	if(!reverb) return NULL;

	reverb->maxBufferSize = maxBufferSize;
	reverb->sampleRate = sampleRate;

	/*SS_ReverbParams *p = &reverb->parameters;
	p->delayFeedback = 0;
	p->character = 0;
	p->time = 0;
	p->preDelayTime = 0;
	p->level = 0;
	p->preLowpass = 0;*/

	reverb->preLPFfc = 8000.0;
	// reverb->preLPFa = 0;
	// reverb->preLPFz = 0;

	reverb->characterTimeCoefficient = 1.0;
	reverb->characterGainCoefficient = 1.0;
	// reverb->characterLPFCoefficient = 0;

	reverb->delayGain = 1;

	// reverb->panDelayFeedback = 0;

	/*reverb->delayLeftOutput = NULL;
	reverb->delayRightOutput = NULL;
	reverb->delayLeftInput = NULL;
	reverb->delayPreLPF = NULL;*/

	/*reverb->delayLeft = NULL;
	reverb->delayRight = NULL;*/

	reverb->delayLeftOutput = (float *)calloc(maxBufferSize, sizeof(float));
	if(!reverb->delayLeftOutput) return NULL;
	reverb->delayRightOutput = (float *)calloc(maxBufferSize, sizeof(float));
	if(!reverb->delayRightOutput) goto out_of_memory;
	reverb->delayLeftInput = (float *)calloc(maxBufferSize, sizeof(float));
	if(!reverb->delayLeftInput) goto out_of_memory;
	reverb->delayPreLPF = (float *)calloc(maxBufferSize, sizeof(float));
	if(!reverb->delayPreLPF) goto out_of_memory;
	if(!(reverb->dattorro = ss_dattorro_reverb_create(sampleRate))) goto out_of_memory;
	if(!(reverb->delayLeft = ss_delay_line_create((unsigned)sampleRate))) goto out_of_memory;
	if(!(reverb->delayRight = ss_delay_line_create((unsigned)sampleRate))) goto out_of_memory;

	return reverb;

out_of_memory:
	ss_reverb_free(reverb);
	return NULL;
}

void ss_reverb_clear(SS_Reverb *reverb) {
	if(!reverb || !reverb->delayLeftOutput || !reverb->delayRightOutput || !reverb->delayLeftInput || !reverb->delayPreLPF) return;
	memset(reverb->delayLeftOutput, 0, reverb->maxBufferSize * sizeof(float));
	memset(reverb->delayRightOutput, 0, reverb->maxBufferSize * sizeof(float));
	memset(reverb->delayLeftInput, 0, reverb->maxBufferSize * sizeof(float));
	memset(reverb->delayPreLPF, 0, reverb->maxBufferSize * sizeof(float));
	ss_dattorro_reverb_clear(reverb->dattorro);
	ss_delay_line_clear(reverb->delayLeft);
	ss_delay_line_clear(reverb->delayRight);
	reverb->preLPFz = 0;
}

void ss_reverb_free(SS_Reverb *reverb) {
	if(!reverb) return;

	free(reverb->delayLeftOutput);
	free(reverb->delayRightOutput);
	free(reverb->delayLeftInput);
	free(reverb->delayPreLPF);
	ss_dattorro_reverb_free(reverb->dattorro);
	ss_delay_line_free(reverb->delayLeft);
	ss_delay_line_free(reverb->delayRight);
	free(reverb);
}

static void ss_reverb_update_feedback(SS_Reverb *reverb) {
	const SS_ReverbParams *p = &reverb->parameters;
	const float x = (float)p->delayFeedback / 127.0;
	const float exp = 1.0 - powf(1.0 - x, 1.9f);
	if(p->character == 6) {
		reverb->delayLeft->feedback = exp * 0.73f;
	} else {
		reverb->delayLeft->feedback = reverb->delayRight->feedback = 0;
		reverb->panDelayFeedback = exp * 0.73f;
	}
}

static void ss_reverb_update_lowpass(SS_Reverb *reverb) {
	const SS_ReverbParams *p = &reverb->parameters;
	const float preLPF = 0.1 + (float)(7 - p->preLowpass) / 14.0 + reverb->characterLPFCoefficient;
	reverb->dattorro->preLPF = (preLPF < 1.0) ? preLPF : 1.0;
}

static void ss_reverb_update_gain(SS_Reverb *reverb) {
	const SS_ReverbParams *p = &reverb->parameters;
	reverb->dattorro->gain = ((float)p->level / 348.0) * reverb->characterGainCoefficient;
	reverb->delayGain = ((float)p->level / 127.0) * 1.5;
}

static void ss_reverb_update_time(SS_Reverb *reverb) {
	const SS_ReverbParams *p = &reverb->parameters;
	const float t = (float)p->time / 127.0;
	reverb->dattorro->decay = reverb->characterTimeCoefficient * (0.05 + 0.65 * t);
	// Delay at 127 is exactly 0.4468 seconds
	// The minimum value (delay 0) seems to be 21 samples
	const unsigned int calcSamples = (unsigned int)(t * reverb->sampleRate * 0.4468);
	const unsigned int timeSamples = (calcSamples > 21) ? calcSamples : 21;
	if(p->character == 7) {
		// Half the delay time
		reverb->delayRight->time = reverb->delayLeft->time = timeSamples / 2;
	} else {
		reverb->delayLeft->time = timeSamples;
	}
}

void ss_reverb_set_delay_feedback(SS_Reverb *reverb, unsigned char delayFeedback) {
	if(!reverb) return;
	reverb->parameters.delayFeedback = delayFeedback;
	ss_reverb_update_feedback(reverb);
}

void ss_reverb_set_character(SS_Reverb *reverb, unsigned char character) {
	if(!reverb) return;
	reverb->parameters.character = character;
	reverb->dattorro->damping = 0.005;
	reverb->characterTimeCoefficient = 1;
	reverb->characterGainCoefficient = 1;
	reverb->characterLPFCoefficient = 0;

	SS_DattorroReverb *dattorro = reverb->dattorro;
	dattorro->inputDiffusion[0] = 0.75;
	dattorro->inputDiffusion[1] = 0.625;
	dattorro->decayDiffusion[0] = 0.7;
	dattorro->decayDiffusion[1] = 0.5;
	dattorro->excursionRate = 0.5;
	dattorro->excursionDepth = 0.7;

	// Tested all characters on level = 64, preset: Hall2
	// File: gs_reverb_character_test.ts, compare spessasynth to SC-VA
	// Tuned by me, though I'm not very good at it :-)
	switch(character) {
		case 0: {
			// Room1
			dattorro->damping = 0.85;
			reverb->characterTimeCoefficient = 0.9;
			reverb->characterGainCoefficient = 0.7;
			reverb->characterLPFCoefficient = 0.2;
			break;
		}

		case 1: {
			// Room2
			dattorro->damping = 0.2;
			reverb->characterGainCoefficient = 0.5;
			reverb->characterTimeCoefficient = 1;
			dattorro->decayDiffusion[1] = 0.64;
			dattorro->decayDiffusion[0] = 0.6;
			reverb->characterLPFCoefficient = 0.2;
			break;
		}

		case 2: {
			// Room3
			dattorro->damping = 0.56;
			reverb->characterGainCoefficient = 0.55;
			reverb->characterTimeCoefficient = 1;
			dattorro->decayDiffusion[1] = 0.64;
			dattorro->decayDiffusion[0] = 0.6;
			reverb->characterLPFCoefficient = 0.1;
			break;
		}

		case 3: {
			// Hall1
			dattorro->damping = 0.6;
			reverb->characterGainCoefficient = 1;
			reverb->characterLPFCoefficient = 0;
			dattorro->decayDiffusion[1] = 0.7;
			dattorro->decayDiffusion[0] = 0.66;
			break;
		}

		case 4: {
			// Hall2
			reverb->characterGainCoefficient = 0.75;
			dattorro->damping = 0.2;
			reverb->characterLPFCoefficient = 0.2;
			break;
		}

		case 5: {
			// Plate
			reverb->characterGainCoefficient = 0.55;
			dattorro->damping = 0.65;
			reverb->characterTimeCoefficient = 0.5;
			break;
		}
	}

	// Update values
	ss_reverb_update_time(reverb);
	ss_reverb_update_gain(reverb);
	ss_reverb_update_lowpass(reverb);
	ss_reverb_update_feedback(reverb);
	ss_delay_line_clear(reverb->delayLeft);
	ss_delay_line_clear(reverb->delayRight);
}

void ss_reverb_set_time(SS_Reverb *reverb, unsigned char time) {
	if(!reverb) return;
	reverb->parameters.time = time;
	ss_reverb_update_time(reverb);
}

void ss_reverb_set_pre_delay_time(SS_Reverb *reverb, unsigned char preDelayTime) {
	if(!reverb) return;
	reverb->parameters.preDelayTime = preDelayTime;
	reverb->dattorro->preDelay = ((float)preDelayTime / 1000.0) * reverb->sampleRate;
}

void ss_reverb_set_level(SS_Reverb *reverb, unsigned char level) {
	if(!reverb) return;
	reverb->parameters.level = level;
	ss_reverb_update_gain(reverb);
}

void ss_reverb_set_pre_lowpass(SS_Reverb *reverb, unsigned char preLowpass) {
	if(!reverb) return;
	reverb->parameters.preLowpass = preLowpass;
	reverb->preLPFfc = 8000.0 * powf(0.63f, (float)preLowpass);
	const float decay = expf((-2.0 * M_PI * reverb->preLPFfc) / reverb->sampleRate);
	reverb->preLPFa = 1.0 - decay;
	ss_reverb_update_lowpass(reverb);
}

void ss_reverb_set_macro(SS_Reverb *reverb, unsigned char value) {
	if(!reverb) return;

	// SC-8850 manual page 81
	ss_reverb_set_level(reverb, 64);
	ss_reverb_set_pre_delay_time(reverb, 0);
	ss_reverb_set_character(reverb, value);
	switch(value) {
			/**
			 * REVERB MACRO is a macro parameter that allows global setting of reverb parameters.
			 * When you select the reverb type with REVERB MACRO, each reverb parameter will be set to their most
			 * suitable value.
			 *
			 * Room1, Room2, Room3
			 * These reverbs simulate the reverberation of a room. They provide a well-defined
			 * spacious reverberation.
			 * Hall1, Hall2
			 * These reverbs simulate the reverberation of a concert hall. They provide a deeper
			 * reverberation than the Room reverbs.
			 * Plate
			 * This simulates a plate reverb (a studio device using a metal plate).
			 * Delay
			 * This is a conventional delay that produces echo effects.
			 * Panning Delay
			 * This is a special delay in which the delayed sounds move left and right.
			 * It is effective when you are listening in stereo.
			 */
		case 0: {
			// Room1
			ss_reverb_set_pre_lowpass(reverb, 3);
			ss_reverb_set_time(reverb, 80);
			ss_reverb_set_delay_feedback(reverb, 0);
			ss_reverb_set_pre_delay_time(reverb, 0);
			break;
		}

		case 1: {
			// Room2
			ss_reverb_set_pre_lowpass(reverb, 4);
			ss_reverb_set_time(reverb, 56);
			ss_reverb_set_delay_feedback(reverb, 0);
			break;
		}

		case 2: {
			// Room3
			ss_reverb_set_pre_lowpass(reverb, 0);
			ss_reverb_set_time(reverb, 72);
			ss_reverb_set_delay_feedback(reverb, 0);
			break;
		}

		case 3: {
			// Hall1
			ss_reverb_set_pre_lowpass(reverb, 4);
			ss_reverb_set_time(reverb, 72);
			ss_reverb_set_delay_feedback(reverb, 0);
			break;
		}

		case 4: {
			// Hall2
			ss_reverb_set_pre_lowpass(reverb, 0);
			ss_reverb_set_time(reverb, 64);
			ss_reverb_set_delay_feedback(reverb, 0);
			break;
		}

		case 5: {
			// Plate
			ss_reverb_set_pre_lowpass(reverb, 0);
			ss_reverb_set_time(reverb, 88);
			ss_reverb_set_delay_feedback(reverb, 0);
			break;
		}

		case 6: {
			// Delay
			ss_reverb_set_pre_lowpass(reverb, 0);
			ss_reverb_set_time(reverb, 32);
			ss_reverb_set_delay_feedback(reverb, 40);
			break;
		}

		case 7: {
			// Panning delay
			ss_reverb_set_pre_lowpass(reverb, 0);
			ss_reverb_set_time(reverb, 64);
			ss_reverb_set_delay_feedback(reverb, 32);
			break;
		}

		default: {
			// Check for invalid macros
			// Testcase: 18 - Dichromatic Lotus Butterfly ~ Ancients (ZUN).mid
			return;
		}
	}
}

void ss_reverb_process(SS_Reverb *reverb,
                       const float *input,
                       float *outputL, float *outputR,
                       int sample_count) {
	int i;
	switch(reverb->parameters.character) {
		default: {
			// Reverb
			ss_dattorro_reverb_process(reverb->dattorro, input, outputL, outputR, sample_count);
			return;
		}

		case 6: {
			// Delay
			// Process pre-lowpass
			const float *delayIn;
			if(reverb->parameters.preLowpass > 0) {
				float *preLPF = reverb->delayPreLPF;
				float z = reverb->preLPFz;
				const float a = reverb->preLPFa;
				for(i = 0; i < sample_count; i++) {
					const float x = input[i];
					z += a * (x - z);
					preLPF[i] = z;
				}
				reverb->preLPFz = z;
				delayIn = preLPF;
			} else {
				delayIn = input;
			}

			ss_delay_line_process(reverb->delayLeft, delayIn, reverb->delayLeftOutput, sample_count);

			// Mix down
			const float g = reverb->delayGain;
			const float *delay = reverb->delayLeftOutput;
			for(i = 0; i < sample_count; i++) {
				const float sample = delay[i] * g;
				*outputL++ += sample;
				*outputR++ += sample;
			}
			return;
		}

		case 7: {
			// Panning Delay
			// Process pre-lowpass
			const float *delayIn;
			if(reverb->parameters.preLowpass > 0) {
				float *preLPF = reverb->delayPreLPF;
				float z = reverb->preLPFz;
				const float a = reverb->preLPFa;
				for(i = 0; i < sample_count; i++) {
					const float x = input[i];
					z += a * (x - z);
					preLPF[i] = z;
				}
				reverb->preLPFz = z;
				delayIn = preLPF;
			} else {
				delayIn = input;
			}

			// Mix right into left
			const float fb = reverb->panDelayFeedback;
			float *delayLeftInput = reverb->delayLeftInput;
			float *delayLeftOutput = reverb->delayLeftOutput;
			float *delayRightOutput = reverb->delayRightOutput;
			for(i = 0; i < sample_count; i++) {
				delayLeftInput[i] = delayIn[i] + delayRightOutput[i] * fb;
			}
			// Process left
			ss_delay_line_process(reverb->delayLeft, delayLeftInput, delayLeftOutput, sample_count);
			// Process right
			ss_delay_line_process(reverb->delayRight, delayLeftOutput, delayRightOutput, sample_count);
			// Mix
			const float g = reverb->delayGain;
			for(i = 0; i < sample_count; i++) {
				*outputL++ += delayLeftOutput[i] * g;
				*outputR++ += delayRightOutput[i] * g;
			}
			return;
		}
	}
}
