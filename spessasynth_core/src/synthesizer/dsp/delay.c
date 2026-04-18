/**
 * delay.c
 * A complex delay filter.  Basically a port of delay.ts.
 */

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if __has_include(<spessasynth_core/spessasynth.h>)
#include <spessasynth_core/delay.h>
#else
#include "spessasynth/synthesizer/dsp/delay.h"
#endif

// SC-8850 manual p.236
// How nice of Roland to provide the conversion values to ms!
typedef struct SS_DelayTimeSegment {
	uint8_t start, end;
	float time_start, resolution;
} SS_DelayTimeSegment;

static const SS_DelayTimeSegment delay_time_segments[] = {
	{ 0x01, 0x14, 0.1, 0.1 },
	{ 0x14, 0x23, 2, 0.2 },
	{ 0x23, 0x2d, 5, 0.5 },
	{ 0x2d, 0x37, 10, 1 },
	{ 0x37, 0x46, 20, 2 },
	{ 0x46, 0x50, 50, 5 },
	{ 0x50, 0x5a, 100, 10 },
	{ 0x5a, 0x69, 200, 20 },
	{ 0x69, 0x74, 500, 50 }
};
static const size_t delay_time_segments_count = sizeof(delay_time_segments) / sizeof(delay_time_segments[0]);

static const float DELAY_GAIN = 1.66;

SS_Delay *ss_delay_create(float sample_rate, int max_buffer_size) {
	SS_Delay *delay = (SS_Delay *)calloc(1, sizeof(*delay));
	if(!delay) return NULL;

	/* constructed preset */
	/*delay->parameters.send_level_to_reverb = 0;
	delay->parameters.pre_lowpass = 0;
	delay->parameters.level_right = 0;*/
	delay->parameters.level = 64;
	delay->parameters.level_center = 127;
	/*delay->parameters.level_left = 0;*/
	delay->parameters.feedback = 16;
	/*delay->parameters.time_ratio_right = 0;
	delay->parameters.time_ratio_left = 0;*/
	delay->parameters.time_center = 12;

	delay->sample_rate = sample_rate;
	delay->max_buffer_size = max_buffer_size;

	delay->delay_center_output = (float *)calloc(max_buffer_size, sizeof(float));
	if(!delay->delay_center_output) goto out_of_memory;
	delay->delay_pre_lpf = (float *)calloc(max_buffer_size, sizeof(float));
	if(!delay->delay_pre_lpf) goto out_of_memory;

	delay->delay_center_time = 0.34 * sample_rate;

	/* All delays are capped at 1s */
	const int i_sample_rate = (int)round(sample_rate);
	delay->delayCenter = ss_delay_line_create(i_sample_rate);
	if(!delay->delayCenter) goto out_of_memory;
	delay->delayLeft = ss_delay_line_create(i_sample_rate);
	if(!delay->delayLeft) goto out_of_memory;
	delay->delayRight = ss_delay_line_create(i_sample_rate);
	if(!delay->delayRight) goto out_of_memory;

	return delay;

out_of_memory:
	ss_delay_free(delay);
	return NULL;
}

void ss_delay_clear(SS_Delay *delay) {
	if(!delay || !delay->delay_center_output || !delay->delay_pre_lpf) return;
	memset(delay->delay_center_output, 0, delay->max_buffer_size * sizeof(float));
	memset(delay->delay_pre_lpf, 0, delay->max_buffer_size * sizeof(float));
	ss_delay_line_clear(delay->delayCenter);
	ss_delay_line_clear(delay->delayLeft);
	ss_delay_line_clear(delay->delayRight);
}

void ss_delay_free(SS_Delay *delay) {
	if(!delay) return;
	free(delay->delay_center_output);
	free(delay->delay_pre_lpf);
	ss_delay_line_free(delay->delayCenter);
	ss_delay_line_free(delay->delayLeft);
	ss_delay_line_free(delay->delayRight);
	free(delay);
}

void ss_delay_set_send_level_to_reverb(SS_Delay *delay, unsigned char value) {
	delay->parameters.send_level_to_reverb = value;
	delay->reverb_gain = (float)value / 127.0;
}

void ss_delay_set_pre_lowpass(SS_Delay *delay, unsigned char value) {
	delay->parameters.pre_lowpass = value;

	// GS sure loves weird mappings, huh?
	// Maps to around 8000-300 Hz
	delay->preLPFfc = 8000.0 * pow(0.63, (float)value);
	const float decay = exp((-2.0 * M_PI * delay->preLPFfc) / delay->sample_rate);
	delay->preLPFa = 1.0 - decay;
}

static void ss_delay_update_gain(SS_Delay *delay) {
	delay->delayCenter->gain = (float)delay->parameters.level_center / 127.0;
	delay->delayLeft->gain = (float)delay->parameters.level_left / 127.0;
	delay->delayRight->gain = (float)delay->parameters.level_right / 127.0;
}

void ss_delay_set_level_right(SS_Delay *delay, unsigned char value) {
	delay->parameters.level_right = value;
	ss_delay_update_gain(delay);
}

void ss_delay_set_level(SS_Delay *delay, unsigned char value) {
	delay->parameters.level = value;
	delay->gain = ((float)value / 127.0) * DELAY_GAIN;
}

void ss_delay_set_level_center(SS_Delay *delay, unsigned char value) {
	delay->parameters.level_center = value;
	ss_delay_update_gain(delay);
}

void ss_delay_set_level_left(SS_Delay *delay, unsigned char value) {
	delay->parameters.level_left = value;
	ss_delay_update_gain(delay);
}

void ss_delay_set_feedback(SS_Delay *delay, unsigned char value) {
	delay->parameters.feedback = value;

	/* Only the center delay has feedback */
	delay->delayLeft->feedback = delay->delayRight->feedback = 0;
	/* -64 means max at inverted phase, so feedback of -1 it is!
	 * Use 66 for it to not be infinite
	 */
	delay->delayCenter->feedback = ((float)value - 64.0) / 66.0;
}

void ss_delay_set_time_ratio_right(SS_Delay *delay, unsigned char value) {
	delay->parameters.time_ratio_right = value;

	/* DELAY TIME RATIO LEFT and DELAY TIME RATIO RIGHT specify the ratio in relation to DELAY TIME CENTER.
	 * The resolution is 100/24(%).
	 * Turn that into multiplier
	 */
	delay->delay_right_multiplier = (float)value * (100.0 / 2400.0);
	delay->delayRight->time = delay->delay_center_time * delay->delay_right_multiplier;
}

void ss_delay_set_time_ratio_left(SS_Delay *delay, unsigned char value) {
	delay->parameters.time_ratio_left = value;

	/* DELAY TIME RATIO LEFT and DELAY TIME RATIO RIGHT specify the ratio in relation to DELAY TIME CENTER.
	 * The resolution is 100/24(%).
	 * Turn that into multiplier
	 */
	delay->delay_left_multiplier = (float)value * (100.0 / 2400.0);
	delay->delayLeft->time = delay->delay_center_time * delay->delay_left_multiplier;
}

void ss_delay_set_time_center(SS_Delay *delay, unsigned char value) {
	delay->parameters.time_center = value;

	float delayMs = 0.1;
	for(size_t i = 0; i < delay_time_segments_count; i++) {
		const SS_DelayTimeSegment *segment = &delay_time_segments[i];
		if(value >= segment->start && value < segment->end) {
			delayMs =
			segment->time_start +
			(value - segment->start) * segment->resolution;
			break;
		}
	}
	delay->delay_center_time = delay->sample_rate * (delayMs / 1000.0);
	if(delay->delay_center_time < 2.0) delay->delay_center_time = 2.0;

	delay->delayCenter->time = delay->delay_center_time;
	delay->delayLeft->time = delay->delay_center_time * delay->delay_left_multiplier;
	delay->delayRight->time = delay->delay_center_time * delay->delay_right_multiplier;
}

void ss_delay_set_macro(SS_Delay *delay, unsigned char value) {
	// SC-8850 manual page 85
	ss_delay_set_level(delay, 64);
	ss_delay_set_pre_lowpass(delay, 0);
	ss_delay_set_send_level_to_reverb(delay, 0);
	ss_delay_set_level_right(delay, 0);
	ss_delay_set_level_left(delay, 0);
	ss_delay_set_level_center(delay, 127);
	switch(value) {
		/**
		 * DELAY MACRO is a macro parameter that allows global setting of delay parameters. When you select the delay type with DELAY MACRO, each delay parameter will be set to their most
		 * suitable value.
		 *
		 * Delay1, Delay2, Delay3
		 * These are conventional delays. 1, 2 and 3 have progressively longer delay times.
		 * Delay4
		 * This is a delay with a rather short delay time.
		 * Pan Delay1. Pan Delay2. Pan Delay3
		 * The delay sound moves between left and right. This is effective when listening in
		 * stereo. 1, 2 and 3 have progressively longer delay times.
		 * Pan Delay4
		 * This is a rather short delay with the delayed sound moving between left and
		 * right.
		 * It is effective when listening in stereo.
		 * Dly To Rev
		 * Reverb is added to the delay sound, which moves between left and right.
		 * It is effective when listening in stereo.
		 * PanRepeat
		 * The delay sound moves between left and right,
		 * but the pan positioning is different from the effects listed above.
		 * It is effective when listening in stereo.
		 */
		case 0: {
			/* Delay1 */
			ss_delay_set_time_center(delay, 97);
			ss_delay_set_time_ratio_right(delay, 1);
			ss_delay_set_time_ratio_left(delay, 1);
			ss_delay_set_feedback(delay, 80);
			break;
		}

		case 1: {
			/* Delay2 */
			ss_delay_set_time_center(delay, 106);
			ss_delay_set_time_ratio_right(delay, 1);
			ss_delay_set_time_ratio_left(delay, 1);
			ss_delay_set_feedback(delay, 80);
			break;
		}

		case 2: {
			/* Delay3 */
			ss_delay_set_time_center(delay, 115);
			ss_delay_set_time_ratio_right(delay, 1);
			ss_delay_set_time_ratio_left(delay, 1);
			ss_delay_set_feedback(delay, 72);
			break;
		}

		case 3: {
			/* Delay4 */
			ss_delay_set_time_center(delay, 83);
			ss_delay_set_time_ratio_right(delay, 1);
			ss_delay_set_time_ratio_left(delay, 1);
			ss_delay_set_feedback(delay, 72);
			break;
		}

		case 4: {
			/* PanDelay1 */
			ss_delay_set_time_center(delay, 105);
			ss_delay_set_time_ratio_left(delay, 12);
			ss_delay_set_time_ratio_right(delay, 24);
			ss_delay_set_level_center(delay, 0);
			ss_delay_set_level_left(delay, 125);
			ss_delay_set_level_right(delay, 60);
			ss_delay_set_feedback(delay, 74);
			break;
		}

		case 5: {
			/* PanDelay2 */
			ss_delay_set_time_center(delay, 109);
			ss_delay_set_time_ratio_left(delay, 12);
			ss_delay_set_time_ratio_right(delay, 24);
			ss_delay_set_level_center(delay, 0);
			ss_delay_set_level_left(delay, 125);
			ss_delay_set_level_right(delay, 60);
			ss_delay_set_feedback(delay, 71);
			break;
		}

		case 6: {
			/* PanDelay3 */
			ss_delay_set_time_center(delay, 115);
			ss_delay_set_time_ratio_left(delay, 12);
			ss_delay_set_time_ratio_right(delay, 24);
			ss_delay_set_level_center(delay, 0);
			ss_delay_set_level_left(delay, 120);
			ss_delay_set_level_right(delay, 64);
			ss_delay_set_feedback(delay, 73);
			break;
		}

		case 7: {
			/* PanDelay4 */
			ss_delay_set_time_center(delay, 93);
			ss_delay_set_time_ratio_left(delay, 12);
			ss_delay_set_time_ratio_right(delay, 24);
			ss_delay_set_level_center(delay, 0);
			ss_delay_set_level_left(delay, 120);
			ss_delay_set_level_right(delay, 64);
			ss_delay_set_feedback(delay, 72);
			break;
		}

		case 8: {
			/* DelayToReverb */
			ss_delay_set_time_center(delay, 109);
			ss_delay_set_time_ratio_left(delay, 12);
			ss_delay_set_time_ratio_right(delay, 24);
			ss_delay_set_level_center(delay, 0);
			ss_delay_set_level_left(delay, 114);
			ss_delay_set_level_right(delay, 60);
			ss_delay_set_feedback(delay, 61);
			ss_delay_set_send_level_to_reverb(delay, 36);
			break;
		}

		case 9: {
			/* PanRepeat */
			ss_delay_set_time_center(delay, 110);
			ss_delay_set_time_ratio_left(delay, 21);
			ss_delay_set_time_ratio_right(delay, 32);
			ss_delay_set_level_center(delay, 97);
			ss_delay_set_level_left(delay, 127);
			ss_delay_set_level_right(delay, 67);
			ss_delay_set_feedback(delay, 40);
			break;
		}

		default: {
			/* Check for invalid macros
			 * Testcase: 18 - Dichromatic Lotus Butterfly ~ Ancients (ZUN).mid
			 */
			return;
		}
	}
}

void ss_delay_process(SS_Delay *delay,
                      const float *input,
                      float *outputL, float *outputR,
                      float *reverb,
                      int sample_count) {
	const float *delayIn;
	if(delay->parameters.pre_lowpass > 0) {
		float *preLPF = delay->delay_pre_lpf;
		float z = delay->preLPFz;
		const float a = delay->preLPFa;
		for(int i = 0; i < sample_count; i++) {
			const float x = input[i];
			z += a * (x - z);
			preLPF[i] = z;
		}
		delay->preLPFz = z;
		delayIn = preLPF;
	} else {
		delayIn = input;
	}

	/*
	Connections are:
	Input connects to all delays,
	center connects to both output and stereo delays,
	stereo delays only connect to the output.
	Also level is separate from reverb send level,
	i.e. level = 0 and reverb send level = 127 will still send sound to reverb.
	 */
	const float gain = delay->gain;
	const float reverbGain = delay->reverb_gain;

	/* Process center first */
	ss_delay_line_process(delay->delayCenter, delayIn, delay->delay_center_output, sample_count);

	/* Mix into output */
	float *center = delay->delay_center_output;
	for(int i = 0; i < sample_count; i++) {
		const float sample = center[i];
		reverb[i] += sample * reverbGain;
		const float outSample = sample * gain;
		outputL[i] += outSample;
		outputR[i] += outSample;
	}

	/* Add input into delay (stereo delays take input from both) */
	for(int i = 0; i < sample_count; i++) {
		center[i] += input[i];
	}

	/* Process stereo delays (reuse preLPF array as delays overwrite samples) */
	float *stereoOut = delay->delay_pre_lpf;
	/* Left */
	ss_delay_line_process(delay->delayLeft, center, stereoOut, sample_count);
	for(int i = 0; i < sample_count; i++) {
		const float sample = stereoOut[i];
		outputL[i] += sample * gain;
		reverb[i] += sample * reverbGain;
	}
	/* Right */
	ss_delay_line_process(delay->delayRight, center, stereoOut, sample_count);
	for(int i = 0; i < sample_count; i++) {
		const float sample = stereoOut[i];
		outputR[i] += sample * gain;
		reverb[i] += sample * reverbGain;
	}
}
