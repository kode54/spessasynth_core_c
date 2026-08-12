/**
 * thru.c
 * Thru (0x0000) — no-op insertion effect.
 *
 * Port of upstream effects/insertion/thru.ts.
 */

#include "insertion_internal.h"

/* ═══════════════════════════════════════════════════════════════════════════
 * 1.  Thru (0x0000)
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
	SS_InsertionProcessor base;
} SS_ThruFX;

static void thru_process(SS_InsertionProcessor *self,
                         const float *iL, const float *iR,
                         float *oL, float *oR,
                         float *oRev, float *oCho, float *oDel,
                         int start, int n) {
	float rev = self->send_level_to_reverb;
	float cho = self->send_level_to_chorus;
	float del = self->send_level_to_delay;
	for(int i = 0; i < n; i++) {
		float sL = iL[i], sR = iR[i];
		oL[start + i] += sL;
		oR[start + i] += sR;
		float mono = (sL + sR) * 0.5f;
		if(oRev) oRev[i] += mono * rev;
		if(oCho) oCho[i] += mono * cho;
		if(oDel) oDel[i] += mono * del;
	}
}
static void thru_set_param(SS_InsertionProcessor *self, int p, int v) {
	(void)self;
	(void)p;
	(void)v;
}
static void thru_reset(SS_InsertionProcessor *self) {
	(void)self;
}
static void thru_free(SS_InsertionProcessor *self) {
	free(self);
}

SS_InsertionProcessor *ss_insertion_thru_create(uint32_t type, uint32_t sample_rate,
                                uint32_t max_buf_size) {
	(void)sample_rate;
	(void)max_buf_size;
	SS_ThruFX *e = (SS_ThruFX *)calloc(1, sizeof(SS_ThruFX));
	if(!e) return NULL;
	e->base.type = type;
	e->base.send_level_to_reverb = 40.0f / 127.0f;
	e->base.send_level_to_chorus = 0;
	e->base.send_level_to_delay = 0;
	e->base.process = thru_process;
	e->base.set_parameter = thru_set_param;
	e->base.reset = thru_reset;
	e->base.free = thru_free;
	return &e->base;
}
