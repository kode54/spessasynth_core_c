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
				raw = v->midi_note << 7;
				break;
			case SS_MODSRC_POLY_PRESSURE:
				raw = v->pressure << 7;
				break;
			case SS_MODSRC_PITCH_WHEEL:
				raw = ch->per_note_pitch ? (int)ch->pitch_wheels[v->real_key] : ch->midi_controllers[SS_MODSRC_PITCH_WHEEL + NON_CC_INDEX_OFFSET];
				break;
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

void ss_voice_compute_modulators(SS_Voice *v, const SS_MIDIChannel *ch,
                                 double time) {
	/* Reset modulated generators to base values */
	memcpy(v->modulated_generators, v->generators, SS_GEN_COUNT * sizeof(int16_t));

	v->resonance_offset = 0.0f;

	for(size_t mi = 0; mi < v->modulator_count; mi++) {
		const SS_Modulator *m = &v->modulators[mi];
		if(m->dest_enum >= SS_GEN_COUNT) continue;

		if(!m->transform_amount) continue;

		float src = get_source_value(ch, v, m->source_enum);
		float asrc = (m->amount_source_enum != 0) ? get_source_value(ch, v, m->amount_source_enum) : 1.0f;

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

		{
			int16_t g = v->modulated_generators[m->dest_enum];
			int32_t new_val = (int32_t)g + (int32_t)val;
			if(new_val > 32767) new_val = 32767;
			if(new_val < -32768) new_val = -32768;
			v->modulated_generators[m->dest_enum] = (int16_t)new_val;
			val = (float)new_val;
		}
		/* Update stored current_value (for snapshot purposes) */
		((SS_Modulator *)m)->current_value = val;
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
	for(size_t v = 0; v < ch->voice_count; v++) {
		ss_voice_compute_modulators(ch->voices[v], ch, time);
	}
}
