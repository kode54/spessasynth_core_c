/**
 * wavetable_oscillator.c
 * Wavetable playback: linear, nearest-neighbor, and Hermite interpolation.
 * Direct port of wavetable_oscillator.ts.
 */

#include <math.h>
#include <string.h>
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
			if(floor_i >= (int)s->end) {
				memset(out + i, 0, (count - i) * sizeof(float));
				return false;
			}
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
			if(ceil_i >= (int)s->end) {
				memset(out + i, 0, (count - i) * sizeof(float));
				return false;
			}
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
				memset(out + i, 0, (count - i) * sizeof(float));
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

/* ── Sinc interpolation ──────────────────────────────────────────────────── */

enum { radius = 2 };
static inline double lanczos(double d) {
	if(d == 0.) return 1.;
	if(fabs(d) > (double)radius) return 0.;
	double dr = (d * M_PI) / (double)radius;
	return sin(d) * sin(dr) / (d * dr);
}

static float itpSinc(const float *buf, double pos, double incr, size_t loop_end, size_t loop_length) {
	size_t offset = (size_t)(pos);
	double frac = pos - (double)offset;
	double scale = 1. / incr > 1. ? 1. : 1. / incr;
	double density = 0.;
	double sample = 0.;
	double fpos = 3. + frac; // 3.5 is the center position

	int min = (double)-radius / scale + fpos - 0.5;
	int max = (double)radius / scale + fpos + 0.5;

	if(min < 0) min = 0;
	if(max > 8) max = 8;

	for(int m = min; m < max; ++m) {
		double factor = lanczos((m - fpos + 0.5) * scale);
		size_t index = offset + m;
		if(loop_length && index >= loop_end) {
			index -= loop_length;
		}
		density += factor;
		sample += buf[index] * factor;
	}
	if(density > 0.) sample /= density; // Normalize

	return (float)sample;
}

static bool get_sample_sinc(SS_Voice *v, float *out, int count, double step) {
	SS_AudioSample *s = &v->sample;
	double cur = s->cursor;
	const float *data = s->sample_data;

	if(s->is_looping) {
		int loop_len = (int)(s->loop_end - s->loop_start);
		for(int i = 0; i < count; i++) {
			while(cur >= (double)s->loop_end) cur -= (double)loop_len;
			out[i] = itpSinc(data, cur, step, s->loop_end, loop_len);
			cur += step;
		}
	} else {
		for(int i = 0; i < count; i++) {
			if(cur >= (double)s->end) {
				memset(out + i, 0, (count - i) * sizeof(float));
				return false;
			}
			out[i] = itpSinc(data, cur, step, 0, 0);
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
		case SS_INTERP_SINC:
			return get_sample_sinc(v, out, count, step);
		case SS_INTERP_HERMITE:
			return get_sample_hermite(v, out, count, step);
		case SS_INTERP_NEAREST:
			return get_sample_nearest(v, out, count, step);
		case SS_INTERP_LINEAR:
		default:
			return get_sample_linear(v, out, count, step);
	}
}
