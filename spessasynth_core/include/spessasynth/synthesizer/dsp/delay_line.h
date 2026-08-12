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
	/* Delay in samples, never greater than bufferLength.  Assign through
	 * ss_delay_line_set_time rather than directly: a longer delay than the
	 * buffer holds would read from outside it. */
	unsigned int time;
} SS_DelayLine;

SS_DelayLine SPESSASYNTH_EXPORTS *ss_delay_line_create(unsigned int maxDelay);

/** Set the delay in samples, clamped to the buffer length and truncated to a
 *  whole sample.  Negative values clamp to zero. */
void SPESSASYNTH_EXPORTS ss_delay_line_set_time(SS_DelayLine *delayLine, float samples);
void SPESSASYNTH_EXPORTS ss_delay_line_process(SS_DelayLine *delayLine,
                                               const float *in, float *out,
                                               int sample_count);
void SPESSASYNTH_EXPORTS ss_delay_line_clear(SS_DelayLine *delayLine);
void SPESSASYNTH_EXPORTS ss_delay_line_free(SS_DelayLine *delayLine);

#ifdef __cplusplus
}
#endif

#endif /* SS_DELAY_LINE_H */