#ifndef SS_DELAY_LINE_H
#define SS_DELAY_LINE_H

#ifdef _MSC_VER
#include "spessasynth_exports.h"
#else
#define SPESSASYNTH_EXPORTS
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ── Delay line type ─────────────────────────────────────────────────────── */

typedef struct {
	float feedback;
	float gain;
	float *buffer;
	unsigned int bufferLength;
	unsigned int writeIndex;
	unsigned int time;
} SS_DelayLine;

SS_DelayLine SPESSASYNTH_EXPORTS *ss_delay_line_create(unsigned int maxDelay);
void SPESSASYNTH_EXPORTS ss_delay_line_process(SS_DelayLine *delayLine,
                                               const float *in, float *out,
                                               int sample_count);
void SPESSASYNTH_EXPORTS ss_delay_line_clear(SS_DelayLine *delayLine);
void SPESSASYNTH_EXPORTS ss_delay_line_free(SS_DelayLine *delayLine);

#ifdef __cplusplus
}
#endif

#endif /* SS_DELAY_LINE_H */