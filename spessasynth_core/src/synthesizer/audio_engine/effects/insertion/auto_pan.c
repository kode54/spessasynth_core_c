/**
 * auto_pan.c
 * AutoPan (0x0126).
 *
 * Port of upstream effects/insertion/auto_pan.ts.
 */

#include "insertion_internal.h"

/* ═══════════════════════════════════════════════════════════════════════════
 * 4.  AutoPan (0x0126)
 * ═══════════════════════════════════════════════════════════════════════════ */

#define AUTOPAN_GAIN_LVL 0.935
#define AUTOPAN_LEVEL_EXP 2.0
#define AUTOPAN_PAN_SMOOTH 0.01

typedef struct {
	SS_InsertionProcessor base;
	double sample_rate;
	int mod_wave;
	double mod_rate, mod_depth, low_gain, hi_gain, level;
	double phase, current_pan;
	SS_Biquad ls_c, hs_c;
	SS_BiquadState ls_l, ls_r, hs_l, hs_r;
} SS_AutoPanFX;

static void autopan_update_shelves(SS_AutoPanFX *e) {
	ss_compute_shelf(&e->ls_c, e->low_gain, 200.0, e->sample_rate, 1);
	ss_compute_shelf(&e->hs_c, e->hi_gain, 4000.0, e->sample_rate, 0);
}

static void autopan_process(SS_InsertionProcessor *self,
                            const float *iL, const float *iR,
                            float *oL, float *oR,
                            float *oRev, float *oCho, float *oDel,
                            int start, int n) {
	SS_AutoPanFX *e = (SS_AutoPanFX *)self;
	double rev = self->send_level_to_reverb;
	double cho = self->send_level_to_chorus;
	double del = self->send_level_to_delay;
	double depth = pow(e->mod_depth / 127.0, AUTOPAN_LEVEL_EXP);
	double scale = (2.0 / (1.0 + depth)) * AUTOPAN_GAIN_LVL;
	double rate_inc = e->mod_rate / (float)e->sample_rate;
	double phase = e->phase, cur_pan = e->current_pan;
	for(int i = 0; i < n; i++) {
		double sL = ss_apply_shelves(iL[i], &e->ls_c, &e->ls_l, &e->hs_c, &e->hs_l);
		double sR = ss_apply_shelves(iR[i], &e->ls_c, &e->ls_r, &e->hs_c, &e->hs_r);

		double lfo = ss_compute_lfo(e->mod_wave, phase);
		if((phase += rate_inc) >= 1.0) phase -= 1.0;
		cur_pan += (lfo - cur_pan) * AUTOPAN_PAN_SMOOTH;
		double pan = cur_pan * depth;
		double gainL = (1.0 - pan) * 0.5 * scale;
		double gainR = (1.0 + pan) * 0.5 * scale;

		double outL = sL * e->level * gainL;
		double outR = sR * e->level * gainR;
		oL[start + i] += outL;
		oR[start + i] += outR;
		double mono = (outL + outR) * 0.5;
		if(oRev) oRev[i] += mono * rev;
		if(oCho) oCho[i] += mono * cho;
		if(oDel) oDel[i] += mono * del;
	}
	e->phase = phase;
	e->current_pan = cur_pan;
}

static void autopan_set_param(SS_InsertionProcessor *self, int p, int v) {
	SS_AutoPanFX *e = (SS_AutoPanFX *)self;
	switch(p) {
		case 0x03:
			e->mod_wave = v;
			break;
		case 0x04:
			e->mod_rate = ss_ivc_rate1(v);
			break;
		case 0x05:
			e->mod_depth = (float)v;
			break;
		case 0x13:
			e->low_gain = (float)(v - 64);
			break;
		case 0x14:
			e->hi_gain = (float)(v - 64);
			break;
		case 0x16:
			e->level = (float)v / 127.0;
			break;
		default:
			break;
	}
	autopan_update_shelves(e);
}

static void autopan_reset(SS_InsertionProcessor *self) {
	SS_AutoPanFX *e = (SS_AutoPanFX *)self;
	e->mod_wave = 1;
	e->mod_rate = 3.05;
	e->mod_depth = 96.0;
	e->low_gain = 0;
	e->hi_gain = 0;
	e->level = 1.0;
	e->phase = 0;
	e->current_pan = 0;
	ss_biquad_zero(&e->ls_l);
	ss_biquad_zero(&e->ls_r);
	ss_biquad_zero(&e->hs_l);
	ss_biquad_zero(&e->hs_r);
	autopan_update_shelves(e);
}
static void autopan_free(SS_InsertionProcessor *self) {
	free(self);
}

SS_InsertionProcessor *ss_insertion_auto_pan_create(uint32_t type, uint32_t sample_rate,
                                uint32_t max_buf_size) {
	(void)sample_rate;
	(void)max_buf_size;
	SS_AutoPanFX *e = (SS_AutoPanFX *)calloc(1, sizeof(SS_AutoPanFX));
	if(!e) return NULL;
	e->base.type = type;
	e->base.send_level_to_reverb = 40.0 / 127.0;
	e->base.send_level_to_chorus = 0;
	e->base.send_level_to_delay = 0;
	e->base.process = autopan_process;
	e->base.set_parameter = autopan_set_param;
	e->base.reset = autopan_reset;
	e->base.free = autopan_free;
	e->sample_rate = (double)sample_rate;
	ss_biquad_identity(&e->ls_c);
	ss_biquad_identity(&e->hs_c);
	autopan_reset(&e->base);
	return &e->base;
}
