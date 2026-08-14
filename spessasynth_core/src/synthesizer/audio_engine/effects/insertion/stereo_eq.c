/**
 * stereo_eq.c
 * StereoEQ (0x0100) — four-band stereo equalizer.
 *
 * Port of upstream effects/insertion/stereo_eq.ts.
 */

#include "insertion_internal.h"

/* ═══════════════════════════════════════════════════════════════════════════
 * 2.  StereoEQ (0x0100)
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
	SS_InsertionProcessor base;
	double sample_rate;
	double level;
	double low_freq, low_gain;
	double hi_freq, hi_gain;
	double m1_freq, m1_gain;
	int m1_q_idx;
	double m2_freq, m2_gain;
	int m2_q_idx;
	SS_Biquad low_c, m1_c, m2_c, hi_c;
	SS_BiquadState low_l, low_r, m1_l, m1_r, m2_l, m2_r, hi_l, hi_r;
} SS_StereoEQFX;

static const float EQ_Q_TABLE[5] = { 0.5, 1.0, 2.0, 4.0, 9.0 };

static void seq_update(SS_StereoEQFX *e) {
	double fs = e->sample_rate;
	ss_compute_low_shelf(&e->low_c, e->low_freq, e->low_gain * 0.5, fs);
	ss_compute_peaking_eq(&e->m1_c, e->m1_freq, e->m1_gain, EQ_Q_TABLE[e->m1_q_idx], fs);
	ss_compute_peaking_eq(&e->m2_c, e->m2_freq, e->m2_gain, EQ_Q_TABLE[e->m2_q_idx], fs);
	ss_compute_high_shelf(&e->hi_c, e->hi_freq, e->hi_gain * 0.5, fs);
}

static void seq_process(SS_InsertionProcessor *self,
                        const float *iL, const float *iR,
                        float *oL, float *oR,
                        float *oRev, float *oCho, float *oDel,
                        int start, int n) {
	SS_StereoEQFX *e = (SS_StereoEQFX *)self;
	double rev = self->send_level_to_reverb;
	double cho = self->send_level_to_chorus;
	double del = self->send_level_to_delay;
	double level = e->level;
	for(int i = 0; i < n; i++) {
		double sL = iL[i], sR = iR[i];
		sL = ss_biquad_process(&e->low_c, &e->low_l, sL);
		sR = ss_biquad_process(&e->low_c, &e->low_r, sR);
		sL = ss_biquad_process(&e->m1_c, &e->m1_l, sL);
		sR = ss_biquad_process(&e->m1_c, &e->m1_r, sR);
		sL = ss_biquad_process(&e->m2_c, &e->m2_l, sL);
		sR = ss_biquad_process(&e->m2_c, &e->m2_r, sR);
		sL = ss_biquad_process(&e->hi_c, &e->hi_l, sL);
		sR = ss_biquad_process(&e->hi_c, &e->hi_r, sR);
		oL[start + i] = (float)((double)oL[start + i] + sL * level);
		oR[start + i] = (float)((double)oR[start + i] + sR * level);
		double mono = 0.5 * (sL + sR);
		if(oRev) oRev[i] = (float)((double)oRev[i] + mono * rev);
		if(oCho) oCho[i] = (float)((double)oCho[i] + mono * cho);
		if(oDel) oDel[i] = (float)((double)oDel[i] + mono * del);
	}
}

static void seq_set_param(SS_InsertionProcessor *self, int p, int v) {
	SS_StereoEQFX *e = (SS_StereoEQFX *)self;
	switch(p) {
		case 0x03:
			e->low_freq = v == 1 ? 400.0 : 200.0;
			break;
		case 0x04:
			e->low_gain = (double)(v - 64);
			break;
		case 0x05:
			e->hi_freq = v == 1 ? 8000.0 : 4000.0;
			break;
		case 0x06:
			e->hi_gain = (double)(v - 64);
			break;
		case 0x07:
			e->m1_freq = ss_ivc_eq_freq(v);
			break;
		case 0x08:
			e->m1_q_idx = (v < 5 ? v : 1);
			break;
		case 0x09:
			e->m1_gain = (double)(v - 64);
			break;
		case 0x0a:
			e->m2_freq = ss_ivc_eq_freq(v);
			break;
		case 0x0b:
			e->m2_q_idx = (v < 5 ? v : 1);
			break;
		case 0x0c:
			e->m2_gain = (double)(v - 64);
			break;
		case 0x16:
			e->level = (double)v / 127.0;
			break;
		default:
			break;
	}
	seq_update(e);
}

static void seq_reset(SS_InsertionProcessor *self) {
	SS_StereoEQFX *e = (SS_StereoEQFX *)self;
	e->level = 1.0;
	e->low_freq = 400.0;
	e->low_gain = 5.0;
	e->hi_freq = 8000.0;
	e->hi_gain = -12.0;
	e->m1_freq = 1600.0;
	e->m1_gain = 8.0;
	e->m1_q_idx = 0;
	e->m2_freq = 1000.0;
	e->m2_gain = -8.0;
	e->m2_q_idx = 0;
	ss_biquad_zero(&e->low_l);
	ss_biquad_zero(&e->low_r);
	ss_biquad_zero(&e->m1_l);
	ss_biquad_zero(&e->m1_r);
	ss_biquad_zero(&e->m2_l);
	ss_biquad_zero(&e->m2_r);
	ss_biquad_zero(&e->hi_l);
	ss_biquad_zero(&e->hi_r);
	seq_update(e);
}
static void seq_free(SS_InsertionProcessor *self) {
	free(self);
}

SS_InsertionProcessor *ss_insertion_stereo_eq_create(uint32_t type, uint32_t sample_rate,
                                uint32_t max_buf_size) {
	(void)sample_rate;
	(void)max_buf_size;
	SS_StereoEQFX *e = (SS_StereoEQFX *)calloc(1, sizeof(SS_StereoEQFX));
	if(!e) return NULL;
	e->base.type = type;
	e->base.send_level_to_reverb = 0;
	e->base.send_level_to_chorus = 0;
	e->base.send_level_to_delay = 0;
	e->base.process = seq_process;
	e->base.set_parameter = seq_set_param;
	e->base.reset = seq_reset;
	e->base.free = seq_free;
	e->sample_rate = (double)sample_rate;
	ss_biquad_identity(&e->low_c);
	ss_biquad_identity(&e->m1_c);
	ss_biquad_identity(&e->m2_c);
	ss_biquad_identity(&e->hi_c);
	seq_reset(&e->base);
	return &e->base;
}
