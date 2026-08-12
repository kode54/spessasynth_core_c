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

static float get_source_value(const SS_MIDIChannel *ch, const SS_Voice *v,
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

/* Compute one modulator's own output and cache it on the modulator.  The cached
 * value is this modulator's contribution alone, never a running total: the
 * source-filtered path below sums the cached values of every modulator sharing
 * a destination, and a total stored here would be counted twice. */
static float compute_one_modulator(SS_Voice *v, const SS_MIDIChannel *ch,
                                   SS_Modulator *m) {
	if(!m->transform_amount) {
		m->current_value = 0.0f;
		return 0.0f;
	}

	float src = get_source_value(ch, v, m->source_enum);
	/* The amount source is evaluated even when it is "no controller".  That
	 * source is raw 16383 against a table of 16384 steps, so it contributes
	 * 0.99993896 rather than 1 -- upstream's own comment there calls it 1, but
	 * its arithmetic does not.  Short-circuiting it to 1 here left every
	 * modulated generator a hair high, and wherever the true product sits just
	 * below an integer the truncation into the generator differs by one. */
	float asrc = get_source_value(ch, v, m->amount_source_enum);

	/* Effect modulators: scale CC91/CC93 as in spessasynth */
	float transform_amount = (float)m->transform_amount;
	if(m->is_effect_modulator && transform_amount <= 1000) {
		transform_amount *= EFFECT_MODULATOR_TRANSFORM_MULTIPLIER;
		if(transform_amount > 1000.0) transform_amount = 1000.0;
	}

	float val = src * asrc * transform_amount;

	if(m->transform_type == SS_MODTRANS_ABSOLUTE) {
		/* Abs value */
		val = fabsf(val);
	}

	/* Default resonant modulator: track separately */
	if(m->is_default_resonant_modulator) {
		/* Half the gain, negates the filter */
		v->resonance_offset = (val > 0) ? val / 2 : 0;
	}

	if(m->is_mod_wheel_modulator) {
		val *= ch->custom_controllers[SS_CUSTOM_CTRL_MODULATION_MULTIPLIER];
	}

	m->current_value = val;
	return val;
}

void ss_voice_compute_modulators(SS_Voice *v, const SS_MIDIChannel *ch,
                                 double time) {
	(void)time;

	/* Everything: lay down the base generators, then fold each modulator
	 * into its destination in turn. */
	resolve_base_generators(v, ch, v->modulated_generators);

	v->resonance_offset = 0.0f;

	for(size_t mi = 0; mi < v->modulator_count; mi++) {
		SS_Modulator *m = &v->modulators[mi];
		if(m->dest_enum >= SS_GEN_COUNT) continue;

		const float val = compute_one_modulator(v, ch, m);
		if(!m->transform_amount) continue;

		/* Add in floating point and then truncate, rather than truncating
		 * the contribution and adding an integer.  The fraction has to
		 * meet the running total before it is dropped, or a destination
		 * fed by several modulators drifts away from upstream. */
		double sum = (double)v->modulated_generators[m->dest_enum] + (double)val;
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
}

/* helper */
void ss_channel_compute_modulators(SS_MIDIChannel *ch, double time) {
	for(size_t v = 0; v < ch->voice_count; v++)
		ss_voice_compute_modulators(ch->voices[v], ch, time);
}
