/**
 * phaser.c
 * Phaser (0x0120).
 *
 * Port of upstream effects/insertion/phaser.ts.
 */

#include "insertion_internal.h"

/* ═══════════════════════════════════════════════════════════════════════════
 * 3.  Phaser (0x0120)
 * ═══════════════════════════════════════════════════════════════════════════ */


static void phaser_set_manual(SS_PhaserFX *p, float m) {
	if(m > 1000.0f) {
		p->manual_offset = 600.0f * 1.5f * 4.0f;
		p->manual = m;
	} else {
		p->manual_offset = 600.0f;
		p->manual = m * 4.0f;
	}
}

static void phaser_update_shelves(SS_PhaserFX *p) {
	ss_compute_shelf(&p->ls_c, p->low_gain, 200.0, p->sample_rate, 1);
	ss_compute_shelf(&p->hs_c, p->hi_gain, 4000.0, p->sample_rate, 0);
}

void ss_phaser_process(SS_InsertionProcessor *self,
                           const float *iL, const float *iR,
                           float *oL, float *oR,
                           float *oRev, float *oCho, float *oDel,
                           int start, int n) {
	SS_PhaserFX *p = (SS_PhaserFX *)self;
	const double rev = self->send_level_to_reverb;
	const double cho = self->send_level_to_chorus;
	const double del = self->send_level_to_delay;
	const double rate_inc = p->rate / p->sample_rate;
	const double fb = p->reso * 0.9;
	double phase = p->phase;
	double prevL = p->prev_l, prevR = p->prev_r;
	for(int i = 0; i < n; i++) {
		double sL = ss_apply_shelves(iL[i], &p->ls_c, &p->ls_l, &p->hs_c, &p->hs_l);
		double sR = ss_apply_shelves(iR[i], &p->ls_c, &p->ls_r, &p->hs_c, &p->hs_r);

		/* Triangle LFO (TS: 2*|phase-0.5|, lfoMul = 1 - depth*lfo) */
		const double lfo = 2.0 * fabs(phase - 0.5);
		if((phase += rate_inc) >= 1.0) phase -= 1.0;
		const double lfoMul = 1.0 - p->depth * lfo;
		const double fc = p->manual_offset + p->manual * lfoMul;

		double tanT = tan(M_PI * fc / p->sample_rate);
		double a = (1.0 - tanT) / (1.0 + tanT);
		if(a < -0.9999) a = -0.9999;
		if(a > 0.9999) a = 0.9999;

		double apL = sL + fb * prevL;
		double apR = sR + fb * prevR;
		for(int s = 0; s < SS_PHASER_STAGES; s++) {
			double outL = -a * apL + p->prev_in_l[s] + a * p->prev_out_l[s];
			p->prev_in_l[s] = (float)apL;
			p->prev_out_l[s] = (float)outL;
			apL = outL;
			double outR = -a * apR + p->prev_in_r[s] + a * p->prev_out_r[s];
			p->prev_in_r[s] = (float)apR;
			p->prev_out_r[s] = (float)outR;
			apR = outR;
		}
		prevL = apL;
		prevR = apR;

		const double outL = (sL + apL * p->mix) * p->level;
		const double outR = (sR + apR * p->mix) * p->level;
		oL[start + i] += outL;
		oR[start + i] += outR;
		const double mono = (outL + outR) * 0.5;
		if(oRev) oRev[i] += mono * rev;
		if(oCho) oCho[i] += mono * cho;
		if(oDel) oDel[i] += mono * del;
	}
	p->phase = phase;
	p->prev_l = prevL;
	p->prev_r = prevR;
}

void ss_phaser_set_param(SS_InsertionProcessor *self, int param, int v) {
	SS_PhaserFX *p = (SS_PhaserFX *)self;
	switch(param) {
		case 0x03:
			phaser_set_manual(p, ss_ivc_manual(v));
			break;
		case 0x04:
			p->rate = ss_ivc_rate1(v);
			break;
		case 0x05:
			p->depth = (float)v / 128.0f;
			break;
		case 0x06:
			p->reso = (float)v / 127.0f;
			break;
		case 0x07:
			p->mix = (float)v / 127.0f;
			break;
		case 0x13:
			p->low_gain = (float)(v - 64);
			break;
		case 0x14:
			p->hi_gain = (float)(v - 64);
			break;
		case 0x16:
			p->level = (float)v / 127.0f;
			break;
		default:
			break;
	}
	phaser_update_shelves(p);
}

void ss_phaser_reset(SS_InsertionProcessor *self) {
	SS_PhaserFX *p = (SS_PhaserFX *)self;
	p->phase = 0.35f;
	phaser_set_manual(p, 620.0f);
	p->rate = 0.85f;
	p->depth = 64.0f / 128.0f;
	p->reso = 16.0f / 127.0f;
	p->mix = 1.0f;
	p->low_gain = 0;
	p->hi_gain = 0;
	p->level = 104.0f / 127.0f;
	p->prev_l = p->prev_r = 0;
	memset(p->prev_in_l, 0, sizeof(p->prev_in_l));
	memset(p->prev_out_l, 0, sizeof(p->prev_out_l));
	memset(p->prev_in_r, 0, sizeof(p->prev_in_r));
	memset(p->prev_out_r, 0, sizeof(p->prev_out_r));
	ss_biquad_zero(&p->ls_l);
	ss_biquad_zero(&p->ls_r);
	ss_biquad_zero(&p->hs_l);
	ss_biquad_zero(&p->hs_r);
	phaser_update_shelves(p);
}
static void phaser_free(SS_InsertionProcessor *self) {
	free(self);
}

void ss_phaser_init(SS_PhaserFX *e, uint32_t type, uint32_t sample_rate, bool owned) {
	e->base.type = type;
	e->base.send_level_to_reverb = owned ? 40.0f / 127.0f : 0.0f;
	e->base.send_level_to_chorus = 0;
	e->base.send_level_to_delay = 0;
	e->base.process = ss_phaser_process;
	e->base.set_parameter = ss_phaser_set_param;
	e->base.reset = ss_phaser_reset;
	e->base.free = owned ? phaser_free : NULL;
	e->sample_rate = (double)sample_rate;
	ss_biquad_identity(&e->ls_c);
	ss_biquad_identity(&e->hs_c);
	ss_phaser_reset(&e->base);
}

SS_InsertionProcessor *ss_insertion_phaser_create(uint32_t type, uint32_t sample_rate,
                                                  uint32_t max_buf_size) {
	(void)max_buf_size;
	SS_PhaserFX *e = (SS_PhaserFX *)calloc(1, sizeof(SS_PhaserFX));
	if(!e) return NULL;
	ss_phaser_init(e, type, sample_rate, true);
	return &e->base;
}
