/**
 * insertion_internal.h
 * Shared internals of the insertion effects.
 *
 * Private to src/synthesizer/audio_engine/effects/insertion/; the public
 * surface is spessasynth/synthesizer/dsp/insertion.h.  Mirrors the split
 * upstream uses under effects/insertion/, where utils.ts and convert.ts hold
 * what the individual effects share.
 */

#ifndef SS_INSERTION_INTERNAL_H
#define SS_INSERTION_INTERNAL_H

#define _USE_MATH_DEFINES
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if __has_include(<spessasynth_core/insertion.h>)
#include <spessasynth_core/insertion.h>
#else
#include "spessasynth/synthesizer/dsp/insertion.h"
#endif

/* ── convert.c — InsertionValueConverter ─────────────────────────────────── */

double ss_ivc_rate1(int v);
double ss_ivc_manual(int v);
double ss_ivc_eq_freq(int v);

/* ── utils.c — biquads, pan tables, LFOs ─────────────────────────────────── */

void ss_biquad_zero(SS_BiquadState *s);
void ss_biquad_identity(SS_Biquad *c);

/* Robert Bristow-Johnson shelf (S=1) */
void ss_compute_shelf(SS_Biquad *c, double db_gain, double f0, double fs, int is_low);
void ss_compute_peaking_eq(SS_Biquad *c, double freq, double gain_db, double Q, double fs);
void ss_compute_low_shelf(SS_Biquad *c, double freq, double gain_db, double fs);
void ss_compute_high_shelf(SS_Biquad *c, double freq, double gain_db, double fs);
void ss_compute_lpf(SS_Biquad *c, double freq, double Q, double fs);
void ss_compute_hpf(SS_Biquad *c, double freq, double Q, double fs);

/* Equal-power pan, indexed 0..127.  Filled by ss_init_insertion_pan_tables. */
extern float ss_pan_table_left[128];
extern float ss_pan_table_right[128];
void ss_init_insertion_pan_tables(void);

float ss_compute_lfo(int wave, float phase);

/* Hot-path helpers stay inline: they run per sample. */

static inline double ss_biquad_process(SS_Biquad *c, SS_BiquadState *s, double x) {
	double y = c->b0 * x + c->b1 * s->x1 + c->b2 * s->x2 - c->a1 * s->y1 - c->a2 * s->y2;
	s->x2 = s->x1;
	s->x1 = x;
	s->y2 = s->y1;
	s->y1 = y;
	return y;
}

/* Apply low shelf then high shelf inline */
static inline double ss_apply_shelves(double x,
                                      SS_Biquad *lc, SS_BiquadState *ls,
                                      SS_Biquad *hc, SS_BiquadState *hs) {
	double l = lc->b0 * x + lc->b1 * ls->x1 + lc->b2 * ls->x2 - lc->a1 * ls->y1 - lc->a2 * ls->y2;
	ls->x2 = ls->x1;
	ls->x1 = x;
	ls->y2 = ls->y1;
	ls->y1 = l;
	double h = hc->b0 * l + hc->b1 * hs->x1 + hc->b2 * hs->x2 - hc->a1 * hs->y1 - hc->a2 * hs->y2;
	hs->x2 = hs->x1;
	hs->x1 = l;
	hs->y2 = hs->y1;
	hs->y1 = h;
	return h;
}

/* ── Per-effect constructors ─────────────────────────────────────────────── */
/* One per upstream class; insertion_list.c dispatches over them. */

SS_InsertionProcessor *ss_insertion_thru_create(uint32_t type, uint32_t sample_rate, uint32_t max_buf_size);
SS_InsertionProcessor *ss_insertion_stereo_eq_create(uint32_t type, uint32_t sample_rate, uint32_t max_buf_size);
SS_InsertionProcessor *ss_insertion_phaser_create(uint32_t type, uint32_t sample_rate, uint32_t max_buf_size);
SS_InsertionProcessor *ss_insertion_auto_wah_create(uint32_t type, uint32_t sample_rate, uint32_t max_buf_size);
SS_InsertionProcessor *ss_insertion_tremolo_create(uint32_t type, uint32_t sample_rate, uint32_t max_buf_size);
SS_InsertionProcessor *ss_insertion_auto_pan_create(uint32_t type, uint32_t sample_rate, uint32_t max_buf_size);
SS_InsertionProcessor *ss_insertion_ph_auto_wah_create(uint32_t type, uint32_t sample_rate, uint32_t max_buf_size);

/* ── Phaser and AutoWah internals ────────────────────────────────────────── */
/* PhAutoWah runs a phaser and an auto-wah in parallel, embedding both by
 * value, so those two effects expose their state and entry points here. */

#define SS_PHASER_STAGES 8

typedef struct {
	SS_InsertionProcessor base;
	double sample_rate;
	double manual, manual_offset;
	double rate, depth, reso, mix, low_gain, hi_gain, level;
	double phase;
	double prev_l, prev_r;
	float prev_in_l[SS_PHASER_STAGES], prev_out_l[SS_PHASER_STAGES];
	float prev_in_r[SS_PHASER_STAGES], prev_out_r[SS_PHASER_STAGES];
	SS_Biquad ls_c, hs_c;
	SS_BiquadState ls_l, ls_r, hs_l, hs_r;
} SS_PhaserFX;

void ss_phaser_process(SS_InsertionProcessor *self,
                       const float *iL, const float *iR,
                       float *oL, float *oR,
                       float *oRev, float *oCho, float *oDel,
                       int start, int n);
void ss_phaser_set_param(SS_InsertionProcessor *self, int param, int v);
void ss_phaser_reset(SS_InsertionProcessor *self);
/** Wire the vtable and coefficients of an already-allocated phaser. */
void ss_phaser_init(SS_PhaserFX *e, uint32_t type, uint32_t sample_rate, bool owned);

typedef struct {
	SS_InsertionProcessor base;
	double sample_rate;
	int fil_type, polarity;
	double sens, manual, peak, rate, depth, pan, low_gain, hi_gain, level;
	double phase, last_fc, envelope;
	double attack_coeff, release_coeff;
	SS_Biquad coeffs, hp_coeffs;
	SS_BiquadState state, hp_state;
	SS_Biquad ls_c, hs_c;
	SS_BiquadState ls_s, hs_s;
} SS_AutoWahFX;

void ss_auto_wah_process(SS_InsertionProcessor *self,
                         const float *iL, const float *iR,
                         float *oL, float *oR,
                         float *oRev, float *oCho, float *oDel,
                         int start, int n);
void ss_auto_wah_set_param(SS_InsertionProcessor *self, int p, int v);
void ss_auto_wah_reset(SS_InsertionProcessor *self);
/** Wire the vtable and coefficients of an already-allocated auto-wah. */
void ss_auto_wah_init(SS_AutoWahFX *e, uint32_t type, uint32_t sample_rate, bool owned);

#endif /* SS_INSERTION_INTERNAL_H */
