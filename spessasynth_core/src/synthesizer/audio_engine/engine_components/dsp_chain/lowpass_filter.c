/**
 * lowpass_filter.c
 * SoundFont2 biquad lowpass filter.  Direct port of lowpass_filter.ts.
 *
 * Original coefficient calculation ported from meltysynth by sinshu.
 */

#define _USE_MATH_DEFINES
#include <math.h>
#include <stdlib.h>
#include <string.h>
#if __has_include(<spessasynth_core/spessasynth.h>)
#include <spessasynth_core/synth.h>
#else
#include "spessasynth/synthesizer/synth.h"
#endif

#define FILTER_SMOOTHING_FACTOR 0.03f

extern float ss_abs_cents_to_hz(int cents);
extern float ss_centibel_attenuation_to_gain(float cb);

/* ── Cached coefficient table ───────────────────────────────────────────────
 * Indexed as cache[resonance_cb][cutoff_cents_floor].
 * resonance_cb: 0..960 (limited by SS_GEN_INITIAL_FILTER_Q max)
 * cutoff_cents: 1500..13500
 * We pre-allocate a flat array sized [961][12001].
 */
#define CACHE_RES_MAX 961
#define CACHE_CENTS_MIN 1500
#define CACHE_CENTS_MAX 13500
#define CACHE_CENTS_SIZE (CACHE_CENTS_MAX - CACHE_CENTS_MIN + 1) /* 12001 */

typedef struct {
	double a0, a1, a2, a3, a4;
	bool valid;
} CachedCoeff;

/* Lazily allocated.  NULL = not yet allocated. */
static CachedCoeff *coeff_cache = NULL;
static SS_Mutex *coeff_mutex = NULL;

/* These won't be freed until process termination */
void ss_lowpass_ensure_cache(void) {
	if(coeff_cache) return;
	size_t total = (size_t)CACHE_RES_MAX * (size_t)CACHE_CENTS_SIZE;
	coeff_cache = (CachedCoeff *)calloc(total, sizeof(CachedCoeff));
	coeff_mutex = ss_mutex_create();
}

static CachedCoeff *get_cached(int resonance_cb, int cutoff_cents) {
	if(!coeff_cache) return NULL;
	if(resonance_cb < 0 || resonance_cb >= CACHE_RES_MAX) return NULL;
	if(cutoff_cents < CACHE_CENTS_MIN || cutoff_cents > CACHE_CENTS_MAX) return NULL;
	int ci = cutoff_cents - CACHE_CENTS_MIN;
	return &coeff_cache[(size_t)resonance_cb * CACHE_CENTS_SIZE + ci];
}

/* ── Coefficient calculation ─────────────────────────────────────────────── */

static void calculate_coefficients(SS_LowpassFilter *f, float cutoff_cents) {
	int ci = (int)cutoff_cents; /* floor */
	ss_lowpass_ensure_cache();
	ss_mutex_enter(coeff_mutex);
	CachedCoeff *cached = get_cached(f->resonance_cb, ci);
	if(cached && cached->valid) {
		f->a0 = cached->a0;
		f->a1 = cached->a1;
		f->a2 = cached->a2;
		f->a3 = cached->a3;
		f->a4 = cached->a4;
		ss_mutex_leave(coeff_mutex);
		return;
	}
	ss_mutex_leave(coeff_mutex);

	float cutoff_hz = ss_abs_cents_to_hz(ci);
	if(cutoff_hz > f->max_cutoff) cutoff_hz = f->max_cutoff;

	float q_cb = (float)f->resonance_cb;
	float res_gain = ss_centibel_attenuation_to_gain(-(q_cb - 3.01f));
	float q_gain = 1.0f / sqrtf(ss_centibel_attenuation_to_gain(-q_cb));

	double w = (2.0 * M_PI * (double)cutoff_hz) / (double)f->sample_rate;
	double cosw = cos(w);
	double alpha = sin(w) / (2.0 * (double)res_gain);

	double b1 = (1.0 - cosw) * (double)q_gain;
	double b0 = b1 / 2.0;
	double b2 = b0;
	double a0 = 1.0 + alpha;
	double a1 = -2.0 * cosw;
	double a2 = 1.0 - alpha;

	double ra0 = b0 / a0;
	double ra1 = b1 / a0;
	double ra2 = b2 / a0;
	double ra3 = a1 / a0;
	double ra4 = a2 / a0;

	f->a0 = ra0;
	f->a1 = ra1;
	f->a2 = ra2;
	f->a3 = ra3;
	f->a4 = ra4;

	if(cached) {
		ss_mutex_enter(coeff_mutex);
		cached->a0 = ra0;
		cached->a1 = ra1;
		cached->a2 = ra2;
		cached->a3 = ra3;
		cached->a4 = ra4;
		cached->valid = true;
		ss_mutex_leave(coeff_mutex);
	}
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void ss_lowpass_filter_init(SS_LowpassFilter *f, uint32_t sample_rate) {
	memset(f, 0, sizeof(*f));
	f->sample_rate = sample_rate;
	f->current_initial_fc = 13500.0f;
	f->last_target_cutoff = 1e38f; /* effectively Infinity */
	f->max_cutoff = (float)sample_rate * 0.45f;
	f->initialized = false;
}

/* Pre-warm the coefficient cache for q=0, fc=1500..13500 (most common). */
void ss_lowpass_filter_prewarm(uint32_t sample_rate) {
	SS_LowpassFilter dummy;
	ss_lowpass_filter_init(&dummy, sample_rate);
	dummy.resonance_cb = 0;
	for(int i = CACHE_CENTS_MIN; i <= CACHE_CENTS_MAX; i++) {
		calculate_coefficients(&dummy, (float)i);
	}
}

void ss_lowpass_filter_apply(SS_LowpassFilter *f,
                             const int16_t *mod_gens,
                             float *buffer, int count,
                             float fc_excursion, float smoothing,
                             float gain, float gain_inc) {
	int initial_fc = mod_gens[SS_GEN_INITIAL_FILTER_FC];

	if(f->initialized) {
		/* Note:
		 * We only smooth out the initialFc part,
		 * the modulation envelope and LFO excursions are not smoothed.
		 */
		f->current_initial_fc += ((float)initial_fc - f->current_initial_fc) * smoothing;
	} else {
		/* Filter initialization, set the current fc to target */
		f->initialized = true;
		f->current_initial_fc = (float)initial_fc;
	}

	/* The final cutoff for this calculation */
	float target_cutoff = f->current_initial_fc + fc_excursion;
	int mod_resonance = mod_gens[SS_GEN_INITIAL_FILTER_Q];

	/* Note:
	 * the check for initialFC is because of the filter optimization
	 * (if cents are the maximum then the filter is open)
	 * filter cannot use this optimization if it's dynamic (see #53), and
	 * the filter can only be dynamic if the initial filter is not open
	 */
	if(f->current_initial_fc > 13499.0f && target_cutoff > 13499.0f && mod_resonance == 0) {
		f->current_initial_fc = 13500.0f;
		/* Filter is open, apply gain */
		for(int i = 0; i < count; i++) {
			buffer[i] *= gain;
			gain += gain_inc;
		}
		return;
	}

	/* Check if the frequency has changed. if so, calculate new coefficients */
	if(fabsf(f->last_target_cutoff - target_cutoff) > 1.0f || f->resonance_cb != mod_resonance) {
		f->last_target_cutoff = target_cutoff;
		f->resonance_cb = mod_resonance;
		calculate_coefficients(f, target_cutoff);
	}

	/* Apply biquad IIR filter */
	double x1 = f->x1, x2 = f->x2;
	double y1 = f->y1, y2 = f->y2;
	const double a0 = f->a0, a1 = f->a1, a2 = f->a2, a3 = f->a3, a4 = f->a4;

	for(int i = 0; i < count; i++) {
		double input = (double)buffer[i];
		double filtered = a0 * input + a1 * x1 + a2 * x2 - a3 * y1 - a4 * y2;
		x2 = x1;
		x1 = input;
		y2 = y1;
		y1 = filtered;

		/* Apply filter and THEN gain */
		/* Per SF2 spec apply order, also see */
		/* https://github.com/FluidSynth/fluidsynth/issues/1427 */
		buffer[i] = (float)filtered * gain;
		gain += gain_inc;
	}

	f->x1 = x1;
	f->x2 = x2;
	f->y1 = y1;
	f->y2 = y2;
}
