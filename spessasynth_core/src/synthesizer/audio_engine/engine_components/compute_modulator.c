/**
 * compute_modulator.c
 * SS_Voice modulator computation
 * Port of compute_modulator.ts
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>
#if __has_include(<spessasynth_core/spessasynth.h>)
#include <spessasynth_core/midi_enums.h>
#include <spessasynth_core/soundbank_enums.h>
#include <spessasynth_core/synth.h>
#else
#include "spessasynth/midi/midi_enums.h"
#include "spessasynth/soundbank/soundbank_enums.h"
#include "spessasynth/synthesizer/synth.h"
#endif

static const float EFFECT_MODULATOR_TRANSFORM_MULTIPLIER = 1000.0 / 200.0;

/* ── Compute modulators ──────────────────────────────────────────────────── */

float ss_modcurve_get_value(int transform_type, SS_ModulatorCurveType curve_type, int index_0_to_16_383);

/*
 * Evaluate the modulator source value.
 * For now, supports CC (direct), velocity, key, pressure, pitch wheel.
 */

static double get_source_value(const SS_MIDIChannel *ch, const SS_Voice *v,
                               uint16_t source_enum) {
	/* Decode packed source_enum:
	 * bits 11-10: curve (0=linear, 1=concave, 2=convex, 3=switch)
	 * bit 9: is_bipolar
	 * bit 8: is_negative
	 * bit 7: is_cc
	 * bits 6-0: index
	 */
	bool is_cc = (source_enum & 0x80) != 0;
	uint8_t idx = source_enum & 0x7F;
	bool is_negative = (source_enum & 0x100) != 0;
	bool is_bipolar = (source_enum & 0x200) != 0;
	int curve = (source_enum >> 10) & 3;

	int raw = 0;
	if(is_cc) {
		raw = ch->midi_controllers[idx];
	} else {
		switch(idx) {
			case SS_MODSRC_NO_CONTROLLER:
				raw = 16383;
				break;
			case SS_MODSRC_NOTE_ON_VELOCITY:
				raw = v->velocity << 7;
				break;
			case SS_MODSRC_NOTE_ON_KEYNUM:
				raw = v->target_key << 7;
				break;
			case SS_MODSRC_POLY_PRESSURE:
				raw = v->pressure << 7;
				break;
			/* Explicitly caught by the default case below:
			case SS_MODSRC_CHANNEL_PRESSURE:
				raw = ch->midi_controllers[NON_CC_INDEX_OFFSET + SS_MODSRC_CHANNEL_PRESSURE];
				break; */
			case SS_MODSRC_PITCH_WHEEL:
				raw = ch->per_note_pitch ? (int)ch->pitch_wheels[v->midi_note] : ch->midi_controllers[SS_MODSRC_PITCH_WHEEL + NON_CC_INDEX_OFFSET];
				break;
			/* Also caught by the default case:
			case SS_MODSRC_PITCH_WHEEL_RANGE:
				raw = ch->midi_controllers[SS_MODSRC_PITCH_WHEEL_RANGE + NON_CC_INDEX_OFFSET];
				break; */
			default:
				if(idx + NON_CC_INDEX_OFFSET >= SS_MIDI_CONTROLLER_COUNT)
					raw = 0;
				else
					raw = ch->midi_controllers[idx + NON_CC_INDEX_OFFSET];
				break;
		}
	}

	if(raw < 0)
		raw = 0;
	else if(raw > 16383)
		raw = 16383;

	const int transform = (SS_ModulatorTransformType)((is_bipolar ? 2 : 0) | (is_negative ? 1 : 0));

	return ss_modcurve_get_value(transform, (SS_ModulatorCurveType)curve, raw);
}

/* Wrap into int16 the way a JavaScript Int16Array store does, without relying
 * on implementation-defined narrowing. */
static int16_t wrap_int16(int32_t value) {
	uint16_t u = (uint16_t)((uint32_t)value & 0xFFFFu);
	return u < 0x8000u ? (int16_t)u : (int16_t)((int32_t)u - 0x10000);
}

/* Store a real into an int16 generator slot: truncate toward zero, as an
 * Int16Array store does.  Callers clamp first, so this only drops the fraction. */
static int16_t store_int16(double value) {
	if(value > 32767.0) return 32767;
	if(value < -32768.0) return -32768;
	return (int16_t)value;
}

/* The generators a voice's modulators are applied on top of: the voice's own,
 * plus the channel's generator offsets that NRPN writes populate.  Those
 * offsets were being maintained and then never read, so every NRPN generator
 * write was silently discarded.  Upstream copies into a fresh Int16Array and
 * adds, so the sum wraps at int16 rather than saturating. */
static void resolve_base_generators(const SS_Voice *v, const SS_MIDIChannel *ch,
                                    int16_t *out) {
	if(!ch->generator_offsets_enabled) {
		memcpy(out, v->generators, SS_GEN_COUNT * sizeof(int16_t));
		return;
	}
	for(int i = 0; i < SS_GEN_COUNT; i++)
		out[i] = wrap_int16((int32_t)v->generators[i] + (int32_t)ch->generator_offsets[i]);
}

/* Truncate toward zero and wrap into int16, as a JavaScript Int16Array store
 * does, for a value of any magnitude. */
static int16_t to_int16_js(double value) {
	if(!isfinite(value)) return 0;
	double t = (value < 0.0) ? ceil(value) : floor(value);
	/* fmod keeps the wrap well-defined for magnitudes past int32. */
	t = fmod(t, 65536.0);
	return wrap_int16((int32_t)t);
}

/* Compute one modulator's own output and cache it on the modulator.  The cached
 * value is this modulator's contribution alone, never a running total: the
 * source-filtered path below sums the cached values of every modulator sharing
 * a destination, and a total stored here would be counted twice.
 *
 * It is cached truncated, because upstream caches it in an Int16Array.  That
 * makes the two recompute paths disagree by design: recomputing everything adds
 * each contribution whole, while the filtered path re-adds them after they have
 * each lost their fraction.  Caching the full value here instead left the
 * filtered path a fraction high per modulator on a shared destination. */
static double compute_one_modulator(SS_Voice *v, const SS_MIDIChannel *ch,
                                    SS_Modulator *m) {
	if(!m->transform_amount) {
		m->current_value = 0.0f;
		return 0.0f;
	}

	double src = get_source_value(ch, v, m->source_enum);
	/* The amount source is evaluated even when it is "no controller".  That
	 * source is raw 16383 against a table of 16384 steps, so it contributes
	 * 0.99993896 rather than 1 -- upstream's own comment there calls it 1, but
	 * its arithmetic does not.  Short-circuiting it to 1 here left every
	 * modulated generator a hair high, and wherever the true product sits just
	 * below an integer the truncation into the generator differs by one. */
	double asrc = get_source_value(ch, v, m->amount_source_enum);

	/* Effect modulators: scale CC91/CC93 as in spessasynth */
	double transform_amount = m->transform_amount;
	if(m->is_effect_modulator && transform_amount <= 1000) {
		transform_amount *= EFFECT_MODULATOR_TRANSFORM_MULTIPLIER;
		if(transform_amount > 1000.0) transform_amount = 1000.0;
	}

	double val = src * asrc * transform_amount;

	if(m->transform_type == SS_MODTRANS_ABSOLUTE) {
		/* Abs value */
		val = fabs(val);
	}

	/* Default resonant modulator: track separately */
	if(m->is_default_resonant_modulator) {
		/* Half the gain, negates the filter */
		v->resonance_offset = (val > 0) ? val / 2 : 0;
	}

	if(m->is_mod_wheel_modulator) {
		val *= ch->custom_controllers[SS_CUSTOM_CTRL_MODULATION_MULTIPLIER];
	}

	m->current_value = (float)to_int16_js(val);
	return val;
}

/* True if this modulator reads the source that just moved, on either input. */
static bool modulator_uses_source(const SS_Modulator *m, bool source_is_cc,
                                  int source_index) {
	const bool primary_is_cc = (m->source_enum & 0x80) != 0;
	const bool amount_is_cc = (m->amount_source_enum & 0x80) != 0;
	return (primary_is_cc == source_is_cc && (m->source_enum & 0x7F) == source_index) ||
	       (amount_is_cc == source_is_cc && (m->amount_source_enum & 0x7F) == source_index);
}

/*
 * Recompute a voice's modulated generators.
 *
 * source_uses_cc < 0 recomputes everything.  Otherwise only the modulators
 * reading the named source are recomputed and their destinations rebuilt, which
 * is all a single controller moving can actually disturb.
 *
 * The two paths round differently, and deliberately so, because upstream's do.
 * Recomputing everything accumulates through the int16 generator slot, so each
 * contribution is truncated as it lands; the filtered path sums the cached
 * contributions in floating point and truncates once at the end.  A destination
 * driven by several modulators can differ by a count between them.
 */
void ss_voice_compute_modulators_for(SS_Voice *v, const SS_MIDIChannel *ch,
                                     double time, int source_uses_cc,
                                     int source_index) {
	(void)time;

	if(source_uses_cc < 0) {
		/* Everything: lay down the base generators, then fold each modulator
		 * into its destination in turn. */
		resolve_base_generators(v, ch, v->modulated_generators);

		v->resonance_offset = 0.0f;

		for(size_t mi = 0; mi < v->modulator_count; mi++) {
			SS_Modulator *m = &v->modulators[mi];
			if(m->dest_enum >= SS_GEN_COUNT) continue;

			const double val = compute_one_modulator(v, ch, m);
			if(!m->transform_amount) continue;

			/* Add in floating point and then truncate, rather than truncating
			 * the contribution and adding an integer.  The fraction has to
			 * meet the running total before it is dropped, or a destination
			 * fed by several modulators drifts away from upstream. */
			double sum = (double)v->modulated_generators[m->dest_enum] + val;
			if(sum > 32767.0) sum = 32767.0;
			if(sum < -32768.0) sum = -32768.0;
			v->modulated_generators[m->dest_enum] = store_int16(sum);
		}

		/* Apply generator-specific limits to all modulated generators.
		 * Matches TypeScript computeModulators second pass (compute_modulator.ts lines 119-130).
		 * This clamps base generator values (e.g. sustainVolEnv = -461 from preset+inst summing)
		 * that were never touched by any modulator but still need to be in spec range. */
		for(int g = 0; g < SS_GEN_COUNT; g++) {
			v->modulated_generators[g] = ss_generator_clamp((SS_GeneratorType)g, v->modulated_generators[g]);
		}
		return;
	}

	/* Only the modulators reading the source that moved.  Each of their
	 * destinations is rebuilt from the base generator plus every modulator
	 * aimed there, cached values included, so the ones that were not
	 * recomputed keep contributing what they last produced. */
	const bool source_is_cc = (source_uses_cc != 0);
	int16_t base[SS_GEN_COUNT];
	bool base_ready = false;

	for(size_t mi = 0; mi < v->modulator_count; mi++) {
		SS_Modulator *m = &v->modulators[mi];
		if(m->dest_enum >= SS_GEN_COUNT) continue;
		if(!modulator_uses_source(m, source_is_cc, source_index)) continue;

		if(!base_ready) {
			resolve_base_generators(v, ch, base);
			base_ready = true;
		}

		const uint16_t dest = m->dest_enum;
		compute_one_modulator(v, ch, m);

		double out = (double)base[dest];
		for(size_t mj = 0; mj < v->modulator_count; mj++)
			if(v->modulators[mj].dest_enum == dest)
				out += (double)v->modulators[mj].current_value;

		/* Clamp to the generator's own limits rather than to the int16 range:
		 * this path never reaches the sweeping limit pass above. */
		const SS_GeneratorLimit *lim = &SS_GENERATOR_LIMITS[dest];
		if(out > (double)lim->max) out = (double)lim->max;
		if(out < (double)lim->min) out = (double)lim->min;
		v->modulated_generators[dest] = store_int16(out);
	}
}

void ss_voice_compute_modulators(SS_Voice *v, const SS_MIDIChannel *ch,
                                 double time) {
	ss_voice_compute_modulators_for(v, ch, time, -1, 0);
}

/* helper */
void ss_channel_compute_modulators_for(SS_MIDIChannel *ch, double time,
                                       int source_uses_cc, int source_index) {
	for(size_t v = 0; v < ch->voice_count; v++)
		ss_voice_compute_modulators_for(ch->voices[v], ch, time,
		                                source_uses_cc, source_index);
}

void ss_channel_compute_modulators(SS_MIDIChannel *ch, double time) {
	ss_channel_compute_modulators_for(ch, time, -1, 0);
}
