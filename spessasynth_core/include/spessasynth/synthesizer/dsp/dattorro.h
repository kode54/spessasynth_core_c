#ifndef SS_DATTORRO_H
#define SS_DATTORRO_H

#ifdef _MSC_VER
#include "spessasynth_exports.h"
#else
#define SPESSASYNTH_EXPORTS
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ── Dattoro Delay Line type ─────────────────────────────────────────────── */

typedef struct {
	float *buffer;
	unsigned int writeIndex;
	unsigned int readIndex;
	unsigned int mask;
} SS_DattorroDelayLine;

SS_DattorroDelayLine SPESSASYNTH_EXPORTS *ss_dattorro_delay_line_create(
double delay, float sampleRate);
void SPESSASYNTH_EXPORTS ss_dattorro_delay_line_free(
SS_DattorroDelayLine *delayLine);
void SPESSASYNTH_EXPORTS ss_dattorro_delay_line_clear(
SS_DattorroDelayLine *delayLine);

/* ── Dattoro Reverb type ─────────────────────────────────────────────────── */

typedef struct {
	unsigned int preDelay;
	float preLPF;
	float inputDiffusion[2];
	float decay;
	float decayDiffusion[2];
	float damping;
	float excursionRate;
	float excursionDepth;
	float gain;
	float sampleRate;
	float lp[3];
	float excPhase;
	unsigned int pDWrite;
	unsigned int pDLength;
	short *taps;
	float *pDelay;
	SS_DattorroDelayLine **delays;
} SS_DattorroReverb;

SS_DattorroReverb SPESSASYNTH_EXPORTS *ss_dattorro_reverb_create(float sampleRate);
void SPESSASYNTH_EXPORTS ss_dattorro_reverb_clear(SS_DattorroReverb *reverb);
void SPESSASYNTH_EXPORTS ss_dattorro_reverb_free(SS_DattorroReverb *reverb);
void SPESSASYNTH_EXPORTS ss_dattorro_reverb_process(SS_DattorroReverb *reverb,
                                                    const float *input,
                                                    float *outputLeft, float *outputRight,
                                                    int sample_count);

#ifdef __cplusplus
}
#endif

#endif /* SS_DATTORRO_H */
