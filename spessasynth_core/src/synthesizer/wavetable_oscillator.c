/**
 * wavetable_oscillator.c
 * Wavetable playback: linear, nearest-neighbor, and Hermite interpolation.
 * Direct port of wavetable_oscillator.ts.
 */

#include <math.h>
#if __has_include(<spessasynth_core/spessasynth.h>)
#include <spessasynth_core/synth.h>
#else
#include "spessasynth/synthesizer/synth.h"
#endif

/* ── Nearest-neighbor ────────────────────────────────────────────────────── */

static bool get_sample_nearest(SS_Voice *v, float *out, int count, double step) {
	SS_AudioSample *s = &v->sample;
	double cur = s->cursor;
	const float *data = s->sample_data;

	if(s->is_looping) {
		double loop_len = (double)(s->loop_end - s->loop_start);
		for(int i = 0; i < count; i++) {
			while(cur >= (double)s->loop_end) cur -= loop_len;
			int floor_i = (int)cur;
			out[i] = data[floor_i];
			cur += step;
		}
	} else {
		for(int i = 0; i < count; i++) {
			int floor_i = (int)cur;
			if(floor_i >= (int)s->end) { return false; }
			out[i] = data[floor_i];
			cur += step;
		}
	}
	s->cursor = cur;
	return true;
}

/* ── Linear interpolation ────────────────────────────────────────────────── */

static bool get_sample_linear(SS_Voice *v, float *out, int count, double step) {
	SS_AudioSample *s = &v->sample;
	double cur = s->cursor;
	const float *data = s->sample_data;

	if(s->is_looping) {
		double loop_len = (double)(s->loop_end - s->loop_start);
		for(int i = 0; i < count; i++) {
			while(cur >= (double)s->loop_end) cur -= loop_len;
			int floor_i = (int)cur;
			int ceil_i = floor_i + 1;
			while(ceil_i >= (int)s->loop_end) ceil_i -= (int)loop_len;
			float frac = (float)(cur - (double)floor_i);
			float lo = data[floor_i];
			float hi = data[ceil_i];
			out[i] = lo + (hi - lo) * frac;
			cur += step;
		}
	} else {
		for(int i = 0; i < count; i++) {
			int floor_i = (int)cur;
			int ceil_i = floor_i + 1;
			if(ceil_i >= (int)s->end) { return false; }
			float frac = (float)(cur - (double)floor_i);
			float lo = data[floor_i];
			float hi = data[ceil_i];
			out[i] = lo + (hi - lo) * frac;
			cur += step;
		}
	}
	s->cursor = cur;
	return true;
}

/* ── Hermite interpolation ───────────────────────────────────────────────── */

static bool get_sample_hermite(SS_Voice *v, float *out, int count, double step) {
	SS_AudioSample *s = &v->sample;
	double cur = s->cursor;
	const float *data = s->sample_data;

	if(s->is_looping) {
		int loop_len = (int)(s->loop_end - s->loop_start);
		for(int i = 0; i < count; i++) {
			while(cur >= (double)s->loop_end) cur -= (double)loop_len;
			int y0 = (int)cur;
			int y1 = y0 + 1, y2 = y0 + 2, y3 = y0 + 3;
			float t = (float)(cur - (double)y0);
			if(y1 >= (int)s->loop_end) y1 -= loop_len;
			if(y2 >= (int)s->loop_end) y2 -= loop_len;
			if(y3 >= (int)s->loop_end) y3 -= loop_len;
			float xm1 = data[y0];
			float x0 = data[y1];
			float x1 = data[y2];
			float x2 = data[y3];
			float c = (x1 - xm1) * 0.5f;
			float vv = x0 - x1;
			float w = c + vv;
			float a = w + vv + (x2 - x0) * 0.5f;
			float b = w + a;
			out[i] = ((a * t - b) * t + c) * t + x0;
			cur += step;
		}
	} else {
		for(int i = 0; i < count; i++) {
			int y0 = (int)cur;
			int y1 = y0 + 1, y2 = y0 + 2, y3 = y0 + 3;
			float t = (float)(cur - (double)y0);
			if(y1 >= (int)s->end || y2 >= (int)s->end || y3 >= (int)s->end) {
				return false;
			}
			float xm1 = data[y0];
			float x0 = data[y1];
			float x1 = data[y2];
			float x2 = data[y3];
			float c = (x1 - xm1) * 0.5f;
			float vv = x0 - x1;
			float w = c + vv;
			float a = w + vv + (x2 - x0) * 0.5f;
			float b = w + a;
			out[i] = ((a * t - b) * t + c) * t + x0;
			cur += step;
		}
	}
	s->cursor = cur;
	return true;
}

/* ── Public dispatch ─────────────────────────────────────────────────────── */

bool ss_wavetable_get_sample(SS_Voice *v, float *out, int count,
                             SS_InterpolationType interp) {
	double step = v->current_tuning_calculated * (double)v->sample.playback_step;

	if(step == 1.0) {
		return get_sample_nearest(v, out, count, step);
	}
	switch(interp) {
		case SS_INTERP_HERMITE:
			return get_sample_hermite(v, out, count, step);
		case SS_INTERP_NEAREST:
			return get_sample_nearest(v, out, count, step);
		case SS_INTERP_LINEAR:
		default:
			return get_sample_linear(v, out, count, step);
	}
}
