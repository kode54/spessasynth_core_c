/**
 * unit_converter.c
 * Converts SoundFont units to audio values using lookup tables.
 * Direct port of unit_converter.ts.
 */

#include <math.h>
#include <stdlib.h>
#if __has_include(<spessasynth_core/spessasynth.h>)
#include <spessasynth_core/synth.h>
#else
#include "spessasynth/synthesizer/synth.h"
#endif

/* ── Timecent lookup table ─────────────────────────────────────────────────
 *   timecentsToSeconds(tc) = 2^(tc/1200)
 *   Range: -15000 to 15000
 */
#define TIMECENT_MIN (-15000)
#define TIMECENT_MAX 15000
#define TIMECENT_SIZE (TIMECENT_MAX - TIMECENT_MIN + 1)

static float timecent_table[TIMECENT_SIZE];
static bool timecent_initialized = false;

static void init_timecent_table(void) {
	if(timecent_initialized) return;
	for(int i = 0; i < TIMECENT_SIZE; i++) {
		int tc = TIMECENT_MIN + i;
		timecent_table[i] = (float)pow(2.0, tc / 1200.0);
	}
	timecent_initialized = true;
}

float ss_timecents_to_seconds(int timecents) {
	if(!timecent_initialized) init_timecent_table();
	if(timecents <= -32767) return 0.0f;
	if(timecents < TIMECENT_MIN) timecents = TIMECENT_MIN;
	if(timecents > TIMECENT_MAX) timecents = TIMECENT_MAX;
	return timecent_table[timecents - TIMECENT_MIN];
}

/* ── Absolute cents -> Hz lookup table ────────────────────────────────────
 *   absCentsToHz(c) = 440 * 2^((c - 6900) / 1200)
 *   Range: -20000 to 16500
 */
#define ABSCENT_MIN (-20000)
#define ABSCENT_MAX 16500
#define ABSCENT_SIZE (ABSCENT_MAX - ABSCENT_MIN + 1)

static float abscent_table[ABSCENT_SIZE];
static bool abscent_initialized = false;

static void init_abscent_table(void) {
	if(abscent_initialized) return;
	for(int i = 0; i < ABSCENT_SIZE; i++) {
		int c = ABSCENT_MIN + i;
		abscent_table[i] = (float)(440.0 * pow(2.0, (c - 6900) / 1200.0));
	}
	abscent_initialized = true;
}

float ss_abs_cents_to_hz(int cents) {
	if(!abscent_initialized) init_abscent_table();
	if(cents < ABSCENT_MIN || cents > ABSCENT_MAX)
		return (float)(440.0 * pow(2.0, (cents - 6900) / 1200.0));
	return abscent_table[cents - ABSCENT_MIN];
}

/* ── Centibel attenuation -> gain lookup table ──────────────────────────────
 *   centibelAttenuationToGain(db) = 10^(-cb/200)
 *   Range: -16600 to 16000, stored at 1 centibel precision
 *   i.e., index = (cb - MIN_CENTIBELS)
 */
#define MIN_CENTIBELS (-16600)
#define MAX_CENTIBELS (16000)
#define CENTIBEL_SIZE ((MAX_CENTIBELS - MIN_CENTIBELS) + 1)

static float *centibel_table = NULL;
static bool centibel_initialized = false;

static void init_centibel_table(void) {
	if(centibel_initialized) return;
	centibel_table = (float *)malloc(CENTIBEL_SIZE * sizeof(float));
	if(!centibel_table) return;
	for(int i = 0; i < CENTIBEL_SIZE; i++) {
		double cb = ((double)MIN_CENTIBELS + (double)i);
		centibel_table[i] = (float)pow(10.0, -cb / 200.0);
	}
	centibel_initialized = true;
}

float ss_centibel_attenuation_to_gain(float centibels) {
	if(!centibel_initialized) init_centibel_table();
	if(!centibel_table) return (float)pow(10.0, -centibels / 200.0);
	int idx = (int)floorf(centibels - MIN_CENTIBELS);
	if(idx < 0) idx = 0;
	if(idx >= CENTIBEL_SIZE) idx = CENTIBEL_SIZE - 1;
	return centibel_table[idx];
}

/* ── LFO ──────────────────────────────────────────────────────────────────── */

float ss_lfo_value(double start_time, float freq_hz, double current_time) {
	if(current_time < start_time) return 0.0f;
	if(freq_hz <= 0.0f) return 0.0f;
	double elapsed = current_time - start_time;
	return (float)sin(2.0 * M_PI * freq_hz * elapsed);
}

/* ── Modulator curve ─────────────────────────────────────────────────────── */

/* Convex attack table (1000 entries), computed once at init. */
static float convex_attack_table[1000];
static bool convex_initialized = false;

static void init_convex_table(void) {
	if(convex_initialized) return;
	/* Same SF2 log-based convex formula used in modulator_curves.ts / init_modcurve_table.
	 * convex[i] = 1 - ((-400/960) * log10(i / 1000))
	 * Matches: CONVEX_ATTACK[i] = getModulatorCurveValue(0, convex, i/1000)
	 */
	convex_attack_table[0] = 0.0f;
	convex_attack_table[999] = 1.0f;
	for(int i = 1; i < 999; i++) {
		float x = (float)((-400.0 / 960.0) * log10((double)i / 1000.0));
		float v = 1.0f - x;
		if(v < 0.0f) v = 0.0f;
		if(v > 1.0f) v = 1.0f;
		convex_attack_table[i] = v;
	}
	convex_initialized = true;
}

float ss_convex_attack(int index_0_to_999) {
	if(!convex_initialized) init_convex_table();
	if(index_0_to_999 < 0) return 0.0f;
	if(index_0_to_999 >= 999) return 1.0f;
	return convex_attack_table[index_0_to_999];
}

/* ── Modulator curves ───────────────────────────────────────────────────── */

static bool modcurve_initialized = false;

enum { MODULATOR_RESOLUTION = 16384 };
enum { MOD_CURVE_TYPES_AMOUNT = 4 };

/**
 * Unipolar positive
 * unipolar negative
 * bipolar positive
 * bipolar negative
 * that's 4
 */
enum { MOD_SOURCE_TRANSFORM_POSSIBILITIES = 4 };

static float concave[MODULATOR_RESOLUTION + 1];
static float convex[MODULATOR_RESOLUTION + 1];

static float precomputed_transforms[MODULATOR_RESOLUTION * MOD_SOURCE_TRANSFORM_POSSIBILITIES * MOD_CURVE_TYPES_AMOUNT];

static float get_modulator_curve_value(int transform, SS_ModulatorCurveType curve, float value) {
	const bool is_bipolar = !!(transform & 2);
	const bool is_negative = !!(transform & 1);

	if(is_negative) {
		value = 1 - value;
	}
	switch(curve) {
		case SS_MODCURVE_LINEAR:
			if(is_bipolar) {
				/* Bipolar curve */
				return value * 2.0 - 1.0;
			}
			return value;

		case SS_MODCURVE_SWITCH:
			/* Switch */
			value = value > 0.5 ? 1 : 0;
			if(is_bipolar) {
				/* Multiply */
				return value * 2.0 - 1.0;
			}
			return value;

		case SS_MODCURVE_CONCAVE:
			/* Look up the value */
			if(is_bipolar) {
				value = value * 2.0 - 1.0;
				if(value < 0) {
					return value >= -1.0 ? -concave[(int)(value * (float)-MODULATOR_RESOLUTION)] : -1.0;
				}
			}
			return value <= 1.0 ? concave[(int)(value * (float)MODULATOR_RESOLUTION)] : 1.0;

		case SS_MODCURVE_CONVEX:
			/* Look up the value */
			if(is_bipolar) {
				value = value * 2.0 - 1.0;
				if(value < 0) {
					return value >= -1.0 ? -convex[(int)(value * (float)-MODULATOR_RESOLUTION)] : -1.0;
				}
			}
			return value <= 1.0 ? convex[(int)(value * (float)MODULATOR_RESOLUTION)] : 1.0;
	}
}

void init_modcurve_table(void) {
	if(modcurve_initialized) return;
	concave[0] = 0;
	concave[MODULATOR_RESOLUTION] = 1;
	convex[0] = 0;
	convex[MODULATOR_RESOLUTION] = 1;
	for(int i = 1; i < MODULATOR_RESOLUTION; i++) {
		const float x = ((-200.0 * 2.0) / 960.0) * log10((float)i / (float)MODULATOR_RESOLUTION);
		convex[i] = 1.0 - x;
		concave[MODULATOR_RESOLUTION - i] = x;
	}
	for(int curve_type = 0; curve_type < MOD_CURVE_TYPES_AMOUNT; curve_type++) {
		for(int transform_type = 0; transform_type < MOD_SOURCE_TRANSFORM_POSSIBILITIES; transform_type++) {
			const int table_index = MODULATOR_RESOLUTION * (curve_type * MOD_CURVE_TYPES_AMOUNT + transform_type);
			for(int value = 0; value < MODULATOR_RESOLUTION; value++) {
				precomputed_transforms[table_index + value] = get_modulator_curve_value(transform_type, (SS_ModulatorCurveType)curve_type, (float)value / (float)MODULATOR_RESOLUTION);
			}
		}
	}
	modcurve_initialized = true;
}

float ss_modcurve_get_value(int transform_type, SS_ModulatorCurveType curve_type, int index_0_to_16_383) {
	if(!modcurve_initialized) init_modcurve_table();
	return precomputed_transforms[MODULATOR_RESOLUTION * (curve_type * MOD_CURVE_TYPES_AMOUNT + transform_type) + index_0_to_16_383];
}

/* ── One-time initializer for all lookup tables ─────────────────────────── */

void ss_unit_converter_init(void) {
	init_timecent_table();
	init_abscent_table();
	init_centibel_table();
	init_convex_table();
	init_modcurve_table();
}
