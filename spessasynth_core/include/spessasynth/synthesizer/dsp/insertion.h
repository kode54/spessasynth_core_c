#ifndef SS_INSERTION_H
#define SS_INSERTION_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Biquad filter utilities ─────────────────────────────────────────────── */

typedef struct {
	double b0, b1, b2, a1, a2; /* normalized (a0 = 1) */
} SS_Biquad;

typedef struct {
	double x1, x2, y1, y2;
} SS_BiquadState;

/* ── Insertion processor vtable ──────────────────────────────────────────── */

typedef struct SS_InsertionProcessor {
	uint32_t type; /* MSB<<8|LSB, e.g. 0x0100 = StereoEQ */

	float send_level_to_reverb;
	float send_level_to_chorus;
	float send_level_to_delay;

	/**
	 * Process one block. Adds to output buffers.
	 * outputLeft/Right use startIndex; reverb/chorus/delay start at 0.
	 */
	void (*process)(struct SS_InsertionProcessor *self,
	                const float *inputLeft, const float *inputRight,
	                float *outputLeft, float *outputRight,
	                float *outputReverb, float *outputChorus, float *outputDelay,
	                int startIndex, int sampleCount);

	/** Set an EFX parameter (param 0x03-0x16, value 0-127). */
	void (*set_parameter)(struct SS_InsertionProcessor *self, int param, int value);

	/** Reset parameters to defaults (does not reset send levels). */
	void (*reset)(struct SS_InsertionProcessor *self);

	/** Free all resources. */
	void (*free)(struct SS_InsertionProcessor *self);
} SS_InsertionProcessor;

/**
 * Create an insertion processor for the given type code.
 * Returns NULL if type is unknown or allocation fails.
 * maxBufferSize is the maximum samples per process() call (used by PhAutoWah).
 */
SS_InsertionProcessor *ss_insertion_create(uint32_t type,
                                           uint32_t sampleRate,
                                           uint32_t maxBufferSize);

/** Convenience wrapper to free a processor. */
static inline void ss_insertion_free(SS_InsertionProcessor *p) {
	if(p && p->free) p->free(p);
}

#ifdef __cplusplus
}
#endif

#endif /* SS_INSERTION_H */
