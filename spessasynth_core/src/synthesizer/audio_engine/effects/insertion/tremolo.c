/**
 * tremolo.c
 * Tremolo (0x0125).
 *
 * Port of upstream effects/insertion/tremolo.ts.
 */

#include "insertion_internal.h"

/* ═══════════════════════════════════════════════════════════════════════════
 * 5.  Tremolo (0x0125)
 * ═══════════════════════════════════════════════════════════════════════════ */

#define TREMOLO_GAIN_SMOOTH 0.01f

typedef struct {
	SS_InsertionProcessor base;
	double sample_rate;
	int mod_wave;
	float mod_rate, mod_depth, low_gain, hi_gain, level;
	float phase, current_gain;
	SS_Biquad ls_c, hs_c;
	SS_BiquadState ls_l, ls_r, hs_l, hs_r;
} SS_TremoloFX;

static void tremolo_update_shelves(SS_TremoloFX *e) {
	ss_compute_shelf(&e->ls_c, e->low_gain, 200.0, e->sample_rate, 1);
	ss_compute_shelf(&e->hs_c, e->hi_gain, 4000.0, e->sample_rate, 0);
}

static void tremolo_process(SS_InsertionProcessor *self,
                            const float *iL, const float *iR,
                            float *oL, float *oR,
                            float *oRev, float *oCho, float *oDel,
                            int start, int n) {
	SS_TremoloFX *e = (SS_TremoloFX *)self;
	float rev = self->send_level_to_reverb;
	float cho = self->send_level_to_chorus;
	float del = self->send_level_to_delay;
	float rate_inc = e->mod_rate / (float)e->sample_rate;
	float phase = e->phase, cur_gain = e->current_gain;
	for(int i = 0; i < n; i++) {
		double sL = ss_apply_shelves(iL[i], &e->ls_c, &e->ls_l, &e->hs_c, &e->hs_l);
		double sR = ss_apply_shelves(iR[i], &e->ls_c, &e->ls_r, &e->hs_c, &e->hs_r);

		float lfo = ss_compute_lfo(e->mod_wave, phase);
		if((phase += rate_inc) >= 1.0f) phase -= 1.0f;

		float trem_level = 1.0f - (lfo * 0.5f + 0.5f) * (e->mod_depth / 127.0f);
		cur_gain += (trem_level - cur_gain) * TREMOLO_GAIN_SMOOTH;

		float outL = (float)sL * e->level * cur_gain;
		float outR = (float)sR * e->level * cur_gain;
		oL[start + i] += outL;
		oR[start + i] += outR;
		float mono = (outL + outR) * 0.5f;
		if(oRev) oRev[i] += mono * rev;
		if(oCho) oCho[i] += mono * cho;
		if(oDel) oDel[i] += mono * del;
	}
	e->phase = phase;
	e->current_gain = cur_gain;
}

static void tremolo_set_param(SS_InsertionProcessor *self, int p, int v) {
	SS_TremoloFX *e = (SS_TremoloFX *)self;
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
			e->level = (float)v / 127.0f;
			break;
		default:
			break;
	}
	tremolo_update_shelves(e);
}

static void tremolo_reset(SS_InsertionProcessor *self) {
	SS_TremoloFX *e = (SS_TremoloFX *)self;
	e->mod_wave = 1;
	e->mod_rate = 3.05f;
	e->mod_depth = 96.0f;
	e->low_gain = 0;
	e->hi_gain = 0;
	e->level = 1.0f;
	e->phase = 0;
	e->current_gain = 1.0f;
	ss_biquad_zero(&e->ls_l);
	ss_biquad_zero(&e->ls_r);
	ss_biquad_zero(&e->hs_l);
	ss_biquad_zero(&e->hs_r);
	tremolo_update_shelves(e);
}
static void tremolo_free(SS_InsertionProcessor *self) {
	free(self);
}

SS_InsertionProcessor *ss_insertion_tremolo_create(uint32_t type, uint32_t sample_rate,
                                uint32_t max_buf_size) {
	(void)sample_rate;
	(void)max_buf_size;
	SS_TremoloFX *e = (SS_TremoloFX *)calloc(1, sizeof(SS_TremoloFX));
	if(!e) return NULL;
	e->base.type = type;
	e->base.send_level_to_reverb = 40.0f / 127.0f;
	e->base.send_level_to_chorus = 0;
	e->base.send_level_to_delay = 0;
	e->base.process = tremolo_process;
	e->base.set_parameter = tremolo_set_param;
	e->base.reset = tremolo_reset;
	e->base.free = tremolo_free;
	e->sample_rate = (double)sample_rate;
	ss_biquad_identity(&e->ls_c);
	ss_biquad_identity(&e->hs_c);
	tremolo_reset(&e->base);
	return &e->base;
}
