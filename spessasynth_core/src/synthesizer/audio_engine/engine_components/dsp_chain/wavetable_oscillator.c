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
	bool has_looped = s->has_looped;

	if(s->is_looping) {
		double loop_len = (double)(s->loop_end - s->loop_start);
		for(int i = 0; i < count; i++) {
			while(cur >= (double)s->loop_end) {
				cur -= loop_len;
				has_looped = true;
			}
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
	s->has_looped = has_looped;
	return true;
}

/* ── Linear interpolation ────────────────────────────────────────────────── */

static bool get_sample_linear(SS_Voice *v, float *out, int count, double step) {
	SS_AudioSample *s = &v->sample;
	double cur = s->cursor;
	const float *data = s->sample_data;
	bool has_looped = s->has_looped;

	if(s->is_looping) {
		double loop_len = (double)(s->loop_end - s->loop_start);
		for(int i = 0; i < count; i++) {
			while(cur >= (double)s->loop_end) {
				cur -= loop_len;
				has_looped = true;
			}
			int floor_i = (int)cur;
			int ceil_i = floor_i + 1;
			while(ceil_i >= (int)s->loop_end) ceil_i -= (int)loop_len;
			const double frac = cur - (double)floor_i;
			const double lo = data[floor_i];
			const double hi = data[ceil_i];
			out[i] = (float)(lo + (hi - lo) * frac);
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
			const double frac = cur - (double)floor_i;
			const double lo = data[floor_i];
			const double hi = data[ceil_i];
			out[i] = (float)(lo + (hi - lo) * frac);
			cur += step;
		}
	}
	s->cursor = cur;
	s->has_looped = has_looped;
	return true;
}

/* ── Hermite interpolation ───────────────────────────────────────────────── */

static bool get_sample_hermite(SS_Voice *v, float *out, int count, double step) {
	SS_AudioSample *s = &v->sample;
	double cur = s->cursor;
	const float *data = s->sample_data;
	bool has_looped = s->has_looped;

	if(s->is_looping) {
		int loop_len = (int)(s->loop_end - s->loop_start);
		for(int i = 0; i < count; i++) {
			while(cur >= (double)s->loop_end) {
				cur -= (double)loop_len;
				has_looped = true;
			}
			int y0 = (int)cur;
			int y1 = y0 + 1, y2 = y0 + 2, y3 = y0 + 3;
			const double t = cur - (double)y0;
			if(y1 >= (int)s->loop_end) y1 -= loop_len;
			if(y2 >= (int)s->loop_end) y2 -= loop_len;
			if(y3 >= (int)s->loop_end) y3 -= loop_len;
			const double xm1 = data[y0];
			const double x0 = data[y1];
			const double x1 = data[y2];
			const double x2 = data[y3];
			const double c = (x1 - xm1) * 0.5;
			const double vv = x0 - x1;
			const double w = c + vv;
			const double a = w + vv + (x2 - x0) * 0.5;
			const double b = w + a;
			out[i] = (float)(((a * t - b) * t + c) * t + x0);
			cur += step;
		}
	} else {
		for(int i = 0; i < count; i++) {
			int y0 = (int)cur;
			int y1 = y0 + 1, y2 = y0 + 2, y3 = y0 + 3;
			const double t = cur - (double)y0;
			if(y1 >= (int)s->end || y2 >= (int)s->end || y3 >= (int)s->end) {
				memset(out + i, 0, (count - i) * sizeof(float));
				return false;
			}
			const double xm1 = data[y0];
			const double x0 = data[y1];
			const double x1 = data[y2];
			const double x2 = data[y3];
			const double c = (x1 - xm1) * 0.5;
			const double vv = x0 - x1;
			const double w = c + vv;
			const double a = w + vv + (x2 - x0) * 0.5;
			const double b = w + a;
			out[i] = (float)(((a * t - b) * t + c) * t + x0);
			cur += step;
		}
	}
	s->cursor = cur;
	s->has_looped = has_looped;
	return true;
}

/* ── Sinc interpolation ──────────────────────────────────────────────────── */

/* Ported from Tabula Sonora's SincInterpolator, which is itself derived from
 * the fixed-window version this replaces.
 *
 * The old kernel scaled its argument by min(1, 1/step) — dropping the cutoff
 * as the read sped up, which is the band-limiting — but then clamped the tap
 * window to a fixed eight samples.  The taps the stretch calls for were
 * simply never summed, so rejection collapsed at exactly the ratios the sinc
 * exists to serve.  Widening the window with the scale is the whole
 * mechanism, and it is why the tap count cannot be fixed: the kernel spans
 * radius/scale samples each side, so a 6x read wants about 50 taps, not 8.
 *
 * The count costs nothing when it is not needed.  Past the radius the kernel
 * is zero, so at unity the eight-tap and the sixty-four-tap sums are the same
 * number.
 *
 * The kernel is a windowed sinc rather than the Lanczos it replaces: a sinc
 * at SINC_CUTOFF under a Blackman window whose shoulder is left free.  Both
 * constants come from Tabula Sonora, fitted by least squares against the
 * SC-VA 4-tap bank's averaged magnitude response, so ordinary playback rates
 * keep that instrument's character while the rejection above them improves.
 */

/* Half-width in source samples at unity ratio: 8 taps. */
enum { SINC_RADIUS = 4 };
/* Largest half-width the widening may ask for, bounding a read at 8x. */
enum { SINC_MAX_RADIUS = 32 };
/* Entries per source sample in the kernel table. */
enum { SINC_RESOLUTION = 1024 };
/* Both factors of the kernel are even in d — sinc is odd over odd, and the
 * window's cosines are invariant under the t -> 1-t that d -> -d induces — so
 * only |d| in [0, SINC_RADIUS] is stored and sinc_weight folds the sign away.
 * The last entry holds kernel(SINC_RADIUS) = 0 and exists so the interpolation
 * may still read i+1 from the final real entry. */
enum { SINC_TABLE_COUNT = (SINC_RADIUS * SINC_RESOLUTION) + 1 };

/* Cutoff in cycles per sample, and the window's shoulder.  Fitted, not chosen. */
#define SINC_CUTOFF 0.29
#define SINC_SHOULDER 0.20

static double sinc_table[SINC_TABLE_COUNT];
static bool sinc_table_ready = false;

/* Tabulated rather than evaluated: the inner loop asks for up to 64 weights
 * per output sample, and a sine and a pair of cosines apiece would dominate
 * the render.  One entry per 1/1024 of a source sample, read with linear
 * interpolation, puts the table error far below the fit error.
 *
 * Racing initializers would write identical values, so no lock is needed. */
void ss_sinc_table_init(void) {
	if(sinc_table_ready) return;
	for(int i = 0; i < SINC_TABLE_COUNT; i++) {
		const double d = (double)i / (double)SINC_RESOLUTION;
		if(d >= (double)SINC_RADIUS) {
			sinc_table[i] = 0.0;
			continue;
		}
		const double x = 2.0 * SINC_CUTOFF * d;
		const double sinc = (x == 0.0) ? 1.0 : sin(M_PI * x) / (M_PI * x);
		/* A Blackman with the shoulder left free, the second fitted constant. */
		const double t = ((d / (double)SINC_RADIUS) + 1.0) / 2.0;
		const double window = ((1.0 - SINC_SHOULDER) / 2.0) -
		                      (0.5 * cos(2.0 * M_PI * t)) +
		                      ((SINC_SHOULDER / 2.0) * cos(4.0 * M_PI * t));
		sinc_table[i] = sinc * window;
	}
	sinc_table_ready = true;
}

/* kernel(|d|), linearly interpolated between table entries.  Folding the sign
 * rather than storing both halves is what the inner loop wants: it may read 64
 * weights per output sample, striding the table rather than walking it, and one
 * half is 32K where both were 64K — the difference between fitting a typical L1
 * and missing to L2 on most of those reads.
 *
 * Folding also removes an absorption the two-sided form suffered.  That version
 * indexed on (d + SINC_RADIUS) * SINC_RESOLUTION, so a d near zero lost up to
 * ten bits of itself to the addition before the scale could recover them; |d| *
 * SINC_RESOLUTION keeps them.  The mirrored halves likewise agreed only to
 * about 1.5 ulp, since their windows evaluated cos at t and at 1-t rather than
 * at one argument, and now there is one value where there were two. */
static inline double sinc_weight(double d) {
	const double at = fabs(d) * (double)SINC_RESOLUTION;
	if(at >= (double)(SINC_TABLE_COUNT - 1)) return 0.0;
	const int i = (int)at;
	const double f = at - (double)i;
	return (sinc_table[i] * (1.0 - f)) + (sinc_table[i + 1] * f);
}

/* The scale the kernel argument takes, and the half-width that scale needs.
 * Capped, because the cost is linear and a runaway ratio must not be able to
 * ask for an unbounded loop. */
static inline void sinc_span(double step, double *scale_out, int *half_out) {
	const double scale = (step > 1.0) ? (1.0 / step) : 1.0;
	int half = (int)ceil((double)SINC_RADIUS / scale);
	if(half > SINC_MAX_RADIUS) half = SINC_MAX_RADIUS;
	if(half < SINC_RADIUS) half = SINC_RADIUS;
	*scale_out = scale;
	*half_out = half;
}

/* loop_length of zero means the sample is not looping, and indices clamp to
 * the buffer instead of wrapping. */
static float itpSinc(const float *buf, double pos, double step,
                     size_t loop_start, size_t loop_end, size_t loop_length,
                     size_t buffer_length, bool has_looped) {
	const double base_f = floor(pos);
	const long base = (long)base_f;
	const double fraction = pos - base_f;

	double scale;
	int half;
	sinc_span(step, &scale, &half);

	double sum = 0.0;
	double density = 0.0;
	for(int m = -half + 1; m <= half; m++) {
		const double w = sinc_weight(((double)m - fraction) * scale);
		if(w == 0.0) continue;

		long at = base + m;
		if(loop_length) {
			/* Wrap into [loop_start, loop_end).  A wide window over a short
			 * loop can travel several loop lengths, so this is a modulo and
			 * not a single subtraction. */
			if(at >= (long)loop_end) {
				at = (long)loop_start +
				     (long)(((size_t)(at - (long)loop_start)) % loop_length);
			} else if(has_looped && at < (long)loop_start) {
				const long behind = (long)loop_start - at;
				const long wrapped = (long)(((size_t)(behind - 1)) % loop_length);
				at = (long)loop_end - 1 - wrapped;
			}
		}
		if(at < 0) continue;
		if(buffer_length && at >= buffer_length) break;

		sum += (double)buf[at] * w;
		density += w;
	}
	/* Normalised by the weights actually summed, so every fractional position
	 * has unity DC gain even where the window is truncated at an edge. */
	return density > 0.0 ? (float)(sum / density) : 0.0f;
}

static bool get_sample_sinc(SS_Voice *v, float *out, int count, double step) {
	SS_AudioSample *s = &v->sample;
	double cur = s->cursor;
	const float *data = s->sample_data;
	bool has_looped = s->has_looped;

	if(s->is_looping) {
		int loop_len = (int)(s->loop_end - s->loop_start);
		for(int i = 0; i < count; i++) {
			while(cur >= (double)s->loop_end) {
				cur -= (double)loop_len;
				has_looped = true;
			}
			out[i] = itpSinc(data, cur, step, s->loop_start, s->loop_end,
			                 (size_t)loop_len, s->sample_data_len, has_looped);
			cur += step;
		}
	} else {
		for(int i = 0; i < count; i++) {
			if(cur >= (double)s->end) {
				memset(out + i, 0, (count - i) * sizeof(float));
				return false;
			}
			out[i] = itpSinc(data, cur, step, 0, 0, 0, s->sample_data_len, false);
			cur += step;
		}
	}
	s->cursor = cur;
	s->has_looped = has_looped;
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
