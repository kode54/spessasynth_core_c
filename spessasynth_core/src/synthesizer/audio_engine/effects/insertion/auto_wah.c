/**
 * auto_wah.c
 * AutoWah (0x0121).
 *
 * Port of upstream effects/insertion/auto_wah.ts.
 */

#include "insertion_internal.h"

/* ═══════════════════════════════════════════════════════════════════════════
 * 6.  AutoWah (0x0121)
 * ═══════════════════════════════════════════════════════════════════════════ */

#define AW_SENS_COEFF 27.0f
#define AW_PEAK_DB 28.0f
#define AW_HPF_Q_DB -28.0f
#define AW_HPF_FC 400.0f
#define AW_MANUAL_SCALE 0.62f
#define AW_FC_SMOOTH 0.005f
#define AW_DEPTH_MUL 5.0f
#define AW_LFO_SMOOTH (AW_DEPTH_MUL * 0.5f)


static void aw_set_manual(SS_AutoWahFX *e, int value) {
	float target = value * AW_MANUAL_SCALE;
	int fl = (int)target, cl = fl + 1;
	if(fl < 0) fl = 0;
	if(fl > 127) fl = 127;
	if(cl > 127) cl = 127;
	float frac = target - fl;
	e->manual = ss_ivc_manual(fl) + (ss_ivc_manual(cl) - ss_ivc_manual(fl)) * frac;
}

static void aw_update_shelves(SS_AutoWahFX *e) {
	ss_compute_shelf(&e->ls_c, e->low_gain, 200.0, e->sample_rate, 1);
	ss_compute_shelf(&e->hs_c, e->hi_gain, 4000.0, e->sample_rate, 0);
}

void ss_auto_wah_process(SS_InsertionProcessor *self,
                       const float *iL, const float *iR,
                       float *oL, float *oR,
                       float *oRev, float *oCho, float *oDel,
                       int start, int n) {
	SS_AutoWahFX *e = (SS_AutoWahFX *)self;
	float rev = self->send_level_to_reverb;
	float cho = self->send_level_to_chorus;
	float del = self->send_level_to_delay;

	float rate_inc = e->rate / (float)e->sample_rate;
	float peak = powf(10.0f, (e->peak / 127.0f * AW_PEAK_DB) / 20.0f);
	float hpf_peak = powf(10.0f, (e->peak / 127.0f * AW_HPF_Q_DB) / 20.0f);
	float pol = (e->polarity == 0) ? -1.0f : AW_DEPTH_MUL;
	float depth = (e->depth / 127.0f) * pol;
	float sens = e->sens / 127.0f;

	int pan_idx = (int)(e->pan + 64);
	if(pan_idx < 0) pan_idx = 0;
	if(pan_idx > 127) pan_idx = 127;
	float gainL = ss_pan_table_left[pan_idx];
	float gainR = ss_pan_table_right[pan_idx];

	float phase = e->phase;
	float last_fc = e->last_fc;
	double env = e->envelope;
	double atk = e->attack_coeff, rel = e->release_coeff;

	for(int i = 0; i < n; i++) {
		/* Mono: average L+R */
		double s = ss_apply_shelves((iL[i] + iR[i]) * 0.5f,
		                         &e->ls_c, &e->ls_s,
		                         &e->hs_c, &e->hs_s);

		double rect = fabs(s);
		if(rect > env)
			env = atk * env + (1.0 - atk) * rect;
		else
			env = rel * env + (1.0 - rel) * rect;

		float lfo = 2.0f * fabsf(phase - 0.5f) * depth;
		if((phase += rate_inc) >= 1.0f) phase -= 1.0f;

		float lfo_mul;
		if(lfo >= AW_LFO_SMOOTH || pol < 0)
			lfo_mul = 1.0f;
		else
			lfo_mul = sinf(lfo * (float)M_PI / (2.0f * AW_LFO_SMOOTH));

		float base = e->manual * (1.0f + sens * (float)env * AW_SENS_COEFF);
		float fc = base * (1.0f + lfo_mul * lfo);
		if(fc < 20.0f) fc = 20.0f;
		float target = fc < 10.0f ? 10.0f : fc;
		last_fc += (target - last_fc) * AW_FC_SMOOTH;

		ss_compute_lpf(&e->coeffs, last_fc, peak, e->sample_rate);

		double proc = s;
		if(e->fil_type == 1) {
			ss_compute_hpf(&e->hp_coeffs, AW_HPF_FC, hpf_peak, e->sample_rate);
			proc = ss_biquad_process(&e->hp_coeffs, &e->hp_state, proc);
		}
		float mono = (float)ss_biquad_process(&e->coeffs, &e->state, proc) * e->level;

		oL[start + i] += mono * gainL;
		oR[start + i] += mono * gainR;
		if(oRev) oRev[i] += mono * rev;
		if(oCho) oCho[i] += mono * cho;
		if(oDel) oDel[i] += mono * del;
	}
	e->phase = phase;
	e->last_fc = last_fc;
	e->envelope = (float)env;
}

void ss_auto_wah_set_param(SS_InsertionProcessor *self, int p, int v) {
	SS_AutoWahFX *e = (SS_AutoWahFX *)self;
	switch(p) {
		case 0x03:
			e->fil_type = v;
			break;
		case 0x04:
			e->sens = (float)v;
			break;
		case 0x05:
			aw_set_manual(e, v);
			break;
		case 0x06:
			e->peak = (float)v;
			break;
		case 0x07:
			e->rate = ss_ivc_rate1(v);
			break;
		case 0x08:
			e->depth = (float)v;
			break;
		case 0x09:
			e->polarity = v;
			break;
		case 0x13:
			e->low_gain = (float)(v - 64);
			break;
		case 0x14:
			e->hi_gain = (float)(v - 64);
			break;
		case 0x15:
			e->pan = (float)(v - 64);
			break;
		case 0x16:
			e->level = (float)v / 127.0f;
			break;
		default:
			break;
	}
	aw_update_shelves(e);
}

static void aw_reset_impl(SS_AutoWahFX *e) {
	e->fil_type = 1;
	e->sens = 0;
	e->peak = 62;
	e->rate = 2.05f;
	e->depth = 72;
	e->polarity = 1;
	e->low_gain = 0;
	e->hi_gain = 0;
	e->pan = 0;
	e->level = 96.0f / 127.0f;
	e->phase = 0.2f;
	aw_set_manual(e, 68);
	e->last_fc = e->manual;
	/* Upstream's reset zeroes the filter states but leaves the envelope
	 * follower and the coefficients alone — reset runs on a live effect
	 * every time the EFX type changes, and the wah carries its envelope
	 * across rather than restarting from silence.  Both are initialized
	 * at construction instead. */
	ss_biquad_zero(&e->state);
	ss_biquad_zero(&e->hp_state);
	ss_biquad_zero(&e->ls_s);
	ss_biquad_zero(&e->hs_s);
	aw_update_shelves(e);
}

void ss_auto_wah_reset(SS_InsertionProcessor *self) {
	aw_reset_impl((SS_AutoWahFX *)self);
}
static void aw_free(SS_InsertionProcessor *self) {
	free(self);
}

void ss_auto_wah_init(SS_AutoWahFX *e, uint32_t type, uint32_t sample_rate, bool owned) {
	e->base.type = type;
	e->base.send_level_to_reverb = owned ? 40.0f / 127.0f : 0.0f;
	e->base.send_level_to_chorus = 0;
	e->base.send_level_to_delay = 0;
	e->base.process = ss_auto_wah_process;
	e->base.set_parameter = ss_auto_wah_set_param;
	e->base.reset = ss_auto_wah_reset;
	e->base.free = owned ? aw_free : NULL;
	e->sample_rate = (double)sample_rate;
	e->attack_coeff = exp(-1.0 / (0.1 * sample_rate));
	e->release_coeff = exp(-1.0 / (0.1 * sample_rate));
	ss_biquad_identity(&e->coeffs);
	ss_biquad_identity(&e->hp_coeffs);
	ss_biquad_identity(&e->ls_c);
	ss_biquad_identity(&e->hs_c);
	aw_reset_impl(e);
}

SS_InsertionProcessor *ss_insertion_auto_wah_create(uint32_t type, uint32_t sample_rate,
                                                    uint32_t max_buf_size) {
	(void)max_buf_size;
	SS_AutoWahFX *e = (SS_AutoWahFX *)calloc(1, sizeof(SS_AutoWahFX));
	if(!e) return NULL;
	ss_auto_wah_init(e, type, sample_rate, true);
	return &e->base;
}
