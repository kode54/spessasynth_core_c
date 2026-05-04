/**
 * dattorro.c
 * A complex reverb filter.  Basically a port of dattorro.ts.
 */

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if __has_include(<spessasynth_core/spessasynth.h>)
#include <spessasynth_core/dattorro.h>
#else
#include "spessasynth/synthesizer/dsp/dattorro.h"
#endif

enum { templateDelaysCount = 12 };
static const double templateDelays[] = {
	0.004771345, 0.003595309, 0.012734787, 0.009307483,
	0.022579886, 0.149625349, 0.060481839, 0.1249958,
	0.030509727, 0.141695508, 0.089244313, 0.106280031
};

enum { templateTapsCount = 14 };
static const double templateTaps[] = {
	0.008937872, 0.099929438, 0.064278754, 0.067067639,
	0.066866033, 0.006283391, 0.035818689, 0.011861161,
	0.121870905, 0.041262054, 0.08981553, 0.070931756,
	0.011256342, 0.004065724
};

SS_DattorroDelayLine *ss_dattorro_delay_line_create(
double delay, float sampleRate) {
	SS_DattorroDelayLine *delayLine =
	(SS_DattorroDelayLine *)calloc(1, sizeof(*delayLine));
	if(!delayLine) return NULL;

	const unsigned int len = (unsigned)(round(delay * (double)sampleRate));
	const unsigned int nextPow2 = (unsigned)pow(2.0, ceil(log2((float)len)));

	delayLine->buffer = (float *)calloc(nextPow2, sizeof(float));
	if(!delayLine->buffer) {
		free(delayLine);
		return NULL;
	}

	delayLine->writeIndex = len - 1;
	delayLine->readIndex = 0;
	delayLine->mask = nextPow2 - 1;

	return delayLine;
}

void ss_dattorro_delay_line_clear(SS_DattorroDelayLine *delayLine) {
	if(!delayLine || !delayLine->buffer) return;
	memset(delayLine->buffer, 0, (delayLine->mask + 1) * sizeof(float));
}

void ss_dattorro_delay_line_free(SS_DattorroDelayLine *delayLine) {
	if(!delayLine) return;

	free(delayLine->buffer);
	free(delayLine);
}

static float ss_dattorro_delay_line_read(SS_DattorroReverb *reverb, int index) {
	SS_DattorroDelayLine *delayLine = reverb->delays[index];
	return delayLine->buffer[delayLine->readIndex];
}

static float ss_dattorro_delay_line_read_at(SS_DattorroReverb *reverb, int index, int offset) {
	SS_DattorroDelayLine *delayLine = reverb->delays[index];
	return delayLine->buffer[(delayLine->readIndex + offset) & delayLine->mask];
}

static float ss_dattorro_delay_line_read_cubic_at(SS_DattorroReverb *reverb, int index, float offset) {
	SS_DattorroDelayLine *delayLine = reverb->delays[index];
	const float frac = offset - (float)((int)offset);
	const unsigned int mask = delayLine->mask;

	unsigned int intOffset = ((int)offset) + delayLine->readIndex - 1;

	const float x0 = delayLine->buffer[intOffset++ & mask],
	            x1 = delayLine->buffer[intOffset++ & mask],
	            x2 = delayLine->buffer[intOffset++ & mask],
	            x3 = delayLine->buffer[intOffset & mask];

	const float a = (3.0f * (x1 - x2) - x0 + x3) / 2.0f,
	            b = 2.0f * x2 + x0 - (5.0f * x1 + x3) / 2.0f,
	            c = (x2 - x0) / 2.0f;

	return ((a * frac + b) * frac + c) * frac + x1;
}

static float ss_dattorro_delay_line_write(SS_DattorroReverb *reverb, int index, float input) {
	SS_DattorroDelayLine *delayLine = reverb->delays[index];
	return (delayLine->buffer[delayLine->writeIndex] = input);
}

SS_DattorroReverb *ss_dattorro_reverb_create(float sampleRate) {
	SS_DattorroReverb *reverb = (SS_DattorroReverb *)calloc(1, sizeof(*reverb));
	if(!reverb) return NULL;

	int i;

	// reverb->preDelay = 0; // cleared already
	reverb->preLPF = 0.5f;
	reverb->inputDiffusion[0] = 0.75f;
	reverb->inputDiffusion[1] = 0.625f;
	reverb->decay = 0.5f;
	reverb->decayDiffusion[0] = 0.7f;
	reverb->decayDiffusion[1] = 0.5f;
	reverb->damping = 0.005f;
	reverb->excursionRate = 0.1f;
	reverb->excursionDepth = 0.2f;
	reverb->gain = 1.0f;
	reverb->sampleRate = sampleRate;
	// reverb->lp[0] = 0;
	// reverb->lp[1] = 0;
	// reverb->lp[2] = 0;
	// reverb->excPhase = 0;
	// reverb->pDWrite = 0;

	// reverb->delays = NULL;
	// reverb->taps = NULL;

	reverb->pDLength = (unsigned int)round(sampleRate);

	reverb->pDelay = (float *)calloc(reverb->pDLength, sizeof(float));
	if(!reverb->pDelay) goto out_of_memory;

	reverb->delays = (SS_DattorroDelayLine **)calloc(templateDelaysCount, sizeof(*reverb->delays));
	if(!reverb->delays) goto out_of_memory;

	for(i = 0; i < 12; i++) {
		if(!(reverb->delays[i] = ss_dattorro_delay_line_create(templateDelays[i], sampleRate)))
			goto out_of_memory;
	}

	reverb->taps = (short *)calloc(templateTapsCount, sizeof(short));
	if(!reverb->taps) goto out_of_memory;

	for(i = 0; i < templateTapsCount; i++)
		reverb->taps[i] = (short)(round(templateTaps[i] * (double)sampleRate));

	return reverb;

out_of_memory:
	ss_dattorro_reverb_free(reverb);
	return NULL;
}

void ss_dattorro_reverb_clear(SS_DattorroReverb *reverb) {
	if(!reverb || !reverb->delays || !reverb->pDelay) return;
	memset(reverb->pDelay, 0, reverb->pDLength * sizeof(float));
	memset(reverb->lp, 0, sizeof(reverb->lp));
	for(size_t i = 0; i < templateDelaysCount; i++)
		ss_dattorro_delay_line_clear(reverb->delays[i]);
	reverb->excPhase = 0;
	reverb->pDWrite = 0;
}

void ss_dattorro_reverb_free(SS_DattorroReverb *reverb) {
	if(!reverb) return;

	if(reverb->delays)
		for(int i = 0; i < templateDelaysCount; i++)
			ss_dattorro_delay_line_free(reverb->delays[i]);
	free(reverb->delays);
	free(reverb->pDelay);
	free(reverb->taps);
	free(reverb);
}

void ss_dattorro_reverb_process(SS_DattorroReverb *reverb, const float *input, float *outputLeft, float *outputRight, int sample_count) {
	const unsigned int pd = reverb->preDelay;
	const float fi = reverb->inputDiffusion[0];
	const float si = reverb->inputDiffusion[1];
	const float dc = reverb->decay;
	const float ft = reverb->decayDiffusion[0];
	const float st = reverb->decayDiffusion[1];
	const float dp = 1.0f - reverb->damping;
	const float ex = reverb->excursionRate / reverb->sampleRate;
	const float ed = (reverb->excursionDepth * reverb->sampleRate) / 1000.0f;
	const short *taps = reverb->taps;

	const unsigned int blockStart = reverb->pDWrite;
	const unsigned int blockLength = reverb->pDLength;

	const float gain = reverb->gain;

	int i, j;

	for(i = 0; i < sample_count; i++) {
		reverb->pDelay[(blockStart + i) % blockLength] = input[i];
	}

	for(i = 0; i < sample_count; i++) {
		reverb->lp[0] +=
		reverb->preLPF *
		(reverb->pDelay[(blockLength + blockStart - pd + i) % blockLength] -
		 reverb->lp[0]);

#define delayWrite(n, v) ss_dattorro_delay_line_write(reverb, n, v)
#define delayRead(n) ss_dattorro_delay_line_read(reverb, n)
#define delayReadAt(n, p) ss_dattorro_delay_line_read_at(reverb, n, p)
#define delayReadCAt(n, p) ss_dattorro_delay_line_read_cubic_at(reverb, n, p)

		// Pre-tank
		float pre = delayWrite(0, reverb->lp[0] - fi * delayRead(0));
		pre = delayWrite(1, fi * (pre - delayRead(1)) + delayRead(0));
		pre = delayWrite(2, fi * pre + delayRead(1) - si * delayRead(2));
		pre = delayWrite(3, si * (pre - delayRead(3)) + delayRead(2));

		const float split = si * pre + delayRead(3);

		// Excursions
		// Could be optimized?
		const float exc = ed * (1.0f + cosf(reverb->excPhase * 6.28f));
		const float exc2 = ed * (1.0f + sinf(reverb->excPhase * 6.2847f));

		// Left loop
		float temp = delayWrite(4, split + dc * delayRead(11) + ft * delayReadCAt(4, exc)); // Tank diffuse 1
		delayWrite(5, delayReadCAt(4, exc) - ft * temp); // Long delay 1
		reverb->lp[1] += dp * (delayRead(5) - reverb->lp[1]); // Damp 1
		temp = delayWrite(6, dc * reverb->lp[1] - st * delayRead(6)); // Tank diffuse 2
		delayWrite(7, delayRead(6) + st * temp); // Long delay 2

		// Right loop
		temp = delayWrite(8, split + dc * delayRead(7) + ft * delayReadCAt(8, exc2)); // Tank diffuse 3
		delayWrite(9, delayReadCAt(8, exc2) - ft * temp); // Long delay 3
		reverb->lp[2] += dp * (delayRead(9) - reverb->lp[2]); // Damp 2
		temp = delayWrite(10, dc * reverb->lp[2] - st * delayRead(10)); // Tank diffuse 4
		delayWrite(11, delayRead(10) + st * temp); // Long delay 4

		// Mix down
		const float leftSample =
		delayReadAt(9, taps[0]) +
		delayReadAt(9, taps[1]) -
		delayReadAt(10, taps[2]) +
		delayReadAt(11, taps[3]) -
		delayReadAt(5, taps[4]) -
		delayReadAt(6, taps[5]) -
		delayReadAt(7, taps[6]);

		const float rightSample =
		delayReadAt(5, taps[7]) +
		delayReadAt(5, taps[8]) -
		delayReadAt(6, taps[9]) +
		delayReadAt(7, taps[10]) -
		delayReadAt(9, taps[11]) -
		delayReadAt(10, taps[12]) -
		delayReadAt(11, taps[13]);

		*outputLeft++ += leftSample * gain;
		*outputRight++ += rightSample * gain;

#undef delayWrite
#undef delayRead
#undef delayReadAt
#undef delayReadCAt

		reverb->excPhase += ex;

		// Advance delays
		for(j = 0; j < templateDelaysCount; j++) {
			SS_DattorroDelayLine *delayLine = reverb->delays[j];
			delayLine->writeIndex = (delayLine->writeIndex + 1) & delayLine->mask;
			delayLine->readIndex = (delayLine->readIndex + 1) & delayLine->mask;
		}
	}

	reverb->pDWrite = (blockStart + sample_count) % blockLength;
}
