/**
 * utils.c
 * Biquad, pan table and LFO helpers shared by the insertion effects.
 *
 * Port of upstream effects/insertion/utils.ts.
 */

#include "insertion_internal.h"

/* ── Biquad helpers ──────────────────────────────────────────────────────── */


void ss_biquad_zero(SS_BiquadState *s) {
	s->x1 = s->x2 = s->y1 = s->y2 = 0;
}

/* Identity passthrough coefficients */
void ss_biquad_identity(SS_Biquad *c) {
	c->b0 = 1;
	c->b1 = 0;
	c->b2 = 0;
	c->a1 = 0;
	c->a2 = 0;
}

/* Apply low shelf then high shelf inline */

/* Robert Bristow-Johnson shelf (S=1) */
void ss_compute_shelf(SS_Biquad *c, double db_gain, double f0, double fs, int is_low) {
	double A = pow(10.0, db_gain / 40.0);
	double w0 = 2.0 * M_PI * f0 / fs;
	double cw = cos(w0), sw = sin(w0);
	double alpha = (sw / 2.0) * sqrt((A + 1.0 / A) * (1.0 / 1.0 - 1) + 2.0);
	double b0, b1, b2, a0, a1, a2;
	double sA = sqrt(A);
	if(is_low) {
		b0 = A * (A + 1 - (A - 1) * cw + 2 * sA * alpha);
		b1 = 2 * A * (A - 1 - (A + 1) * cw);
		b2 = A * (A + 1 - (A - 1) * cw - 2 * sA * alpha);
		a0 = A + 1 + (A - 1) * cw + 2 * sA * alpha;
		a1 = -2 * (A - 1 + (A + 1) * cw);
		a2 = A + 1 + (A - 1) * cw - 2 * sA * alpha;
	} else {
		b0 = A * (A + 1 + (A - 1) * cw + 2 * sA * alpha);
		b1 = -2 * A * (A - 1 + (A + 1) * cw);
		b2 = A * (A + 1 + (A - 1) * cw - 2 * sA * alpha);
		a0 = A + 1 - (A - 1) * cw + 2 * sA * alpha;
		a1 = 2 * (A - 1 - (A + 1) * cw);
		a2 = A + 1 - (A - 1) * cw - 2 * sA * alpha;
	}
	c->b0 = b0 / a0;
	c->b1 = b1 / a0;
	c->b2 = b2 / a0;
	c->a1 = a1 / a0;
	c->a2 = a2 / a0;
}

/* Peaking EQ (used by StereoEQ mid bands) */
void ss_compute_peaking_eq(SS_Biquad *c, double freq, double gain_db, double Q, double fs) {
	double A = pow(10.0, gain_db / 40.0);
	double w0 = 2.0 * M_PI * freq / fs;
	double cw = cos(w0), sw = sin(w0);
	double alpha = sw / (2.0 * Q);
	double b0 = 1 + alpha * A;
	double b1 = -2 * cw;
	double b2 = 1 - alpha * A;
	double a0 = 1 + alpha / A;
	double a1 = -2 * cw;
	double a2 = 1 - alpha / A;
	c->b0 = b0 / a0;
	c->b1 = b1 / a0;
	c->b2 = b2 / a0;
	c->a1 = a1 / a0;
	c->a2 = a2 / a0;
}

/* Low shelf used by StereoEQ (same formula but standalone) */
void ss_compute_low_shelf(SS_Biquad *c, double freq, double gain_db, double fs) {
	ss_compute_shelf(c, gain_db, freq, fs, 1);
}

void ss_compute_high_shelf(SS_Biquad *c, double freq, double gain_db, double fs) {
	ss_compute_shelf(c, gain_db, freq, fs, 0);
}

/* Standard 2nd-order lowpass */
void ss_compute_lpf(SS_Biquad *c, double freq, double Q, double fs) {
	double w0 = 2.0 * M_PI * freq / fs;
	double cw = cos(w0), sw = sin(w0);
	double alpha = sw / (2.0 * Q);
	double b1v = 1.0 - cw;
	double b0v = b1v * 0.5;
	double a0 = 1.0 + alpha;
	double a1 = -2.0 * cw;
	double a2 = 1.0 - alpha;
	c->b0 = b0v / a0;
	c->b1 = b1v / a0;
	c->b2 = b0v / a0;
	c->a1 = a1 / a0;
	c->a2 = a2 / a0;
}

/* Standard 2nd-order highpass */
void ss_compute_hpf(SS_Biquad *c, double freq, double Q, double fs) {
	double w0 = 2.0 * M_PI * freq / fs;
	double cw = cos(w0), sw = sin(w0);
	double alpha = sw / (2.0 * Q);
	double b0v = (1.0 + cw) * 0.5;
	double b1v = -(1.0 + cw);
	double a0 = 1.0 + alpha;
	double a1 = -2.0 * cw;
	double a2 = 1.0 - alpha;
	c->b0 = b0v / a0;
	c->b1 = b1v / a0;
	c->b2 = b0v / a0;
	c->a1 = a1 / a0;
	c->a2 = a2 / a0;
}

/* Pan lookup tables (128 entries, index = pan+64, range -64..63) */
float ss_pan_table_left[128];
float ss_pan_table_right[128];
static int pan_tables_initialized = 0;

void ss_init_insertion_pan_tables(void) {
	if(pan_tables_initialized) return;
	for(int pan = -64; pan <= 63; pan++) {
		/* Computed in double and rounded on the store, as upstream does. */
		double real_pan = (double)(pan + 64) / 127.0;
		int idx = pan + 64;
		ss_pan_table_left[idx] = (float)cos((M_PI / 2.0) * real_pan);
		ss_pan_table_right[idx] = (float)sin((M_PI / 2.0) * real_pan);
	}
	pan_tables_initialized = 1;
}

/* LFO helpers (matching TS waveforms) */
static inline float lfo_triangle(float phase) {
	return 1.0f - 4.0f * fabsf(phase - 0.5f);
}
static inline float lfo_square(float phase) {
	return phase > 0.5f ? -1.0f : -(float)cos((phase - 0.75f) * 2.0 * M_PI);
}
static inline float lfo_sine(float phase) {
	return sinf(2.0f * (float)M_PI * phase);
}
static inline float lfo_saw1(float phase) {
	return 1.0f - 2.0f * phase;
}
static inline float lfo_saw2(float phase) {
	return 2.0f * phase - 1.0f;
}

float ss_compute_lfo(int wave, float phase) {
	switch(wave) {
		default:
			return lfo_triangle(phase);
		case 1:
			return lfo_square(phase);
		case 2:
			return lfo_sine(phase);
		case 3:
			return lfo_saw1(phase);
		case 4:
			return lfo_saw2(phase);
	}
}
