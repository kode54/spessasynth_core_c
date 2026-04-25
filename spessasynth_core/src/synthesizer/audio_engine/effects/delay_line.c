/**
 * delay_line.c
 * A simple delay line.  Basically a port of delay_line.ts.
 */

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if __has_include(<spessasynth_core/spessasynth.h>)
#include <spessasynth_core/delay_line.h>
#else
#include "spessasynth/synthesizer/dsp/delay_line.h"
#endif

SS_DelayLine *ss_delay_line_create(unsigned int maxDelay) {
	SS_DelayLine *delayLine = (SS_DelayLine *)calloc(1, sizeof(*delayLine));
	if(!delayLine) return NULL;

	delayLine->feedback = 0;
	delayLine->gain = 1;

	delayLine->buffer = (float *)calloc(maxDelay, sizeof(float));
	if(!delayLine->buffer) {
		free(delayLine);
		return NULL;
	}

	delayLine->bufferLength = maxDelay;
	delayLine->writeIndex = 0;
	delayLine->time = maxDelay - 5;

	return delayLine;
}

void ss_delay_line_process(SS_DelayLine *delayLine,
                           const float *in, float *out,
                           int sample_count) {
	unsigned int writeIndex = delayLine->writeIndex;
	const unsigned int delay = delayLine->time;
	float *buffer = delayLine->buffer;
	const unsigned int bufferLength = delayLine->bufferLength;
	const float feedback = delayLine->feedback;
	const float gain = delayLine->gain;

	for(int i = 0; i < sample_count; i++) {
		int readIndex = (signed)writeIndex - (signed)delay;
		if(readIndex < 0) readIndex += bufferLength;

		const float delayed = buffer[readIndex];
		out[i] = delayed * gain;

		buffer[writeIndex] = in[i] + delayed * feedback;

		if(++writeIndex >= bufferLength) writeIndex = 0;
	}

	delayLine->writeIndex = writeIndex;
}

void ss_delay_line_clear(SS_DelayLine *delayLine) {
	memset(delayLine->buffer, 0, delayLine->bufferLength * sizeof(float));
}

void ss_delay_line_free(SS_DelayLine *delayLine) {
	if(!delayLine) return;

	free(delayLine->buffer);
	free(delayLine);
}
