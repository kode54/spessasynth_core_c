#ifndef SS_DATTORRO_REVERB_H
#define SS_DATTORRO_REVERB_H

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

SS_DattorroDelayLine *ss_dattorro_delay_line_create(
double delay, float sampleRate);
void ss_dattorro_delay_line_free(
SS_DattorroDelayLine *delayLine);
void ss_dattorro_delay_line_clear(
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

SS_DattorroReverb *ss_dattorro_reverb_create(float sampleRate);
void ss_dattorro_reverb_clear(SS_DattorroReverb *reverb);
void ss_dattorro_reverb_free(SS_DattorroReverb *reverb);
void ss_dattorro_reverb_process(SS_DattorroReverb *reverb,
                                const float *input,
                                float *outputLeft, float *outputRight,
                                int sample_count);

#ifdef __cplusplus
}
#endif

#endif /* SS_DATTORRO_REVERB_H */
