/**
 * ph_auto_wah.c
 * PhAutoWah (0x1108) — parallel Phaser and AutoWah.
 *
 * Port of upstream effects/insertion/ph_auto_wah.ts.
 */

#include "insertion_internal.h"

/* ═══════════════════════════════════════════════════════════════════════════
 * 7.  PhAutoWah (0x1108) — parallel Phaser + AutoWah
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
	SS_InsertionProcessor base;
	SS_PhaserFX phaser;
	SS_AutoWahFX auto_wah;
	double ph_pan; /* 0..127, index into pan_table */
	double aw_pan;
	double level;
	float *buf_ph;
	float *buf_aw;
	uint32_t buf_size;
} SS_PhAutoWahFX;

static void phaw_process(SS_InsertionProcessor *self,
                         const float *iL, const float *iR,
                         float *oL, float *oR,
                         float *oRev, float *oCho, float *oDel,
                         int start, int n) {
	SS_PhAutoWahFX *e = (SS_PhAutoWahFX *)self;
	double rev = self->send_level_to_reverb;
	double cho = self->send_level_to_chorus;
	double del = self->send_level_to_delay;

	/* Process phaser (only left input) into buf_ph */
	memset(e->buf_ph, 0, sizeof(float) * n);
	ss_phaser_process(&e->phaser.base, iL, iL,
	               e->buf_ph, e->buf_ph,
	               e->buf_ph, e->buf_ph, e->buf_ph, /* sends = 0, ignored */
	               0, n);

	/* Process auto-wah (only right input) into buf_aw */
	memset(e->buf_aw, 0, sizeof(float) * n);
	ss_auto_wah_process(&e->auto_wah.base, iR, iR,
	           e->buf_aw, e->buf_aw,
	           e->buf_aw, e->buf_aw, e->buf_aw,
	           0, n);

	int ph_idx = (int)e->ph_pan;
	if(ph_idx < 0) ph_idx = 0;
	if(ph_idx > 127) ph_idx = 127;
	int aw_idx = (int)e->aw_pan;
	if(aw_idx < 0) aw_idx = 0;
	if(aw_idx > 127) aw_idx = 127;
	double phL = ss_pan_table_left[ph_idx], phR = ss_pan_table_right[ph_idx];
	double awL = ss_pan_table_left[aw_idx], awR = ss_pan_table_right[aw_idx];

	for(int i = 0; i < n; i++) {
		/* Divide by 2: each processor mixed both L+R into one buffer */
		double out_ph = e->buf_ph[i] * 0.5 * e->level;
		double out_aw = e->buf_aw[i] * 0.5 * e->level;
		double outL = out_ph * phL + out_aw * awL;
		double outR = out_ph * phR + out_aw * awR;
		oL[start + i] += outL;
		oR[start + i] += outR;
		double mono = (outL + outR) * 0.5;
		if(oRev) oRev[i] += mono * rev;
		if(oCho) oCho[i] += mono * cho;
		if(oDel) oDel[i] += mono * del;
	}
}

static void phaw_set_param(SS_InsertionProcessor *self, int p, int v) {
	SS_PhAutoWahFX *e = (SS_PhAutoWahFX *)self;
	if(p >= 0x03 && p <= 0x07) {
		ss_phaser_set_param(&e->phaser.base, p, v);
		return;
	}
	if(p >= 0x08 && p <= 0x0e) {
		ss_auto_wah_set_param(&e->auto_wah.base, p - 5, v);
		return;
	}
	switch(p) {
		case 0x12:
			e->ph_pan = (float)v;
			break;
		case 0x13:
			ss_phaser_set_param(&e->phaser.base, 0x16, v);
			break;
		case 0x14:
			e->aw_pan = (float)v;
			break;
		case 0x15:
			ss_auto_wah_set_param(&e->auto_wah.base, 0x16, v);
			break;
		case 0x16:
			e->level = (float)v / 127.0;
			break;
		default:
			break;
	}
}

static void phaw_reset(SS_InsertionProcessor *self) {
	SS_PhAutoWahFX *e = (SS_PhAutoWahFX *)self;
	e->ph_pan = 0;
	e->aw_pan = 127;
	e->level = 1.0;
	ss_phaser_reset(&e->phaser.base);
	ss_auto_wah_reset(&e->auto_wah.base);
	/* Override sub-processor levels to full */
	ss_phaser_set_param(&e->phaser.base, 0x16, 127);
	ss_auto_wah_set_param(&e->auto_wah.base, 0x16, 127);
}

static void phaw_free(SS_InsertionProcessor *self) {
	SS_PhAutoWahFX *e = (SS_PhAutoWahFX *)self;
	free(e->buf_ph);
	free(e->buf_aw);
	free(e);
}

SS_InsertionProcessor *ss_insertion_ph_auto_wah_create(uint32_t type, uint32_t sample_rate,
                                                       uint32_t max_buf_size) {
	SS_PhAutoWahFX *e = (SS_PhAutoWahFX *)calloc(1, sizeof(SS_PhAutoWahFX));
	if(!e) return NULL;
	if(max_buf_size == 0) max_buf_size = 512;
	e->buf_ph = (float *)calloc(max_buf_size, sizeof(float));
	e->buf_aw = (float *)calloc(max_buf_size, sizeof(float));
	if(!e->buf_ph || !e->buf_aw) {
		free(e->buf_ph);
		free(e->buf_aw);
		free(e);
		return NULL;
	}
	e->buf_size = max_buf_size;

	e->base.type = type;
	e->base.send_level_to_reverb = 40.0 / 127.0;
	e->base.send_level_to_chorus = 0;
	e->base.send_level_to_delay = 0;
	e->base.process = phaw_process;
	e->base.set_parameter = phaw_set_param;
	e->base.reset = phaw_reset;
	e->base.free = phaw_free;

	/* The two sub-processors are embedded by value and owned by this one,
	 * so they get no send levels and no free of their own. */
	ss_phaser_init(&e->phaser, 0x0120, sample_rate, false);
	ss_auto_wah_init(&e->auto_wah, 0x0121, sample_rate, false);

	phaw_reset(&e->base);
	return &e->base;
}
