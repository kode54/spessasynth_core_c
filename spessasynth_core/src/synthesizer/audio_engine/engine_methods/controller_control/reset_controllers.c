/**
 * reset_controllers.c
 * Per-MIDI-channel controller state reset functions.
 * Port of reset_controllers.ts and friends.
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>
#if __has_include(<spessasynth_core/spessasynth.h>)
#include <spessasynth_core/midi_enums.h>
#include <spessasynth_core/synth.h>
#else
#include "spessasynth/midi/midi_enums.h"
#include "spessasynth/synthesizer/synth.h"
#endif

/* ── Reset controllers ───────────────────────────────────────────────────── */

extern void ss_channel_set_custom_controller(SS_MIDIChannel *ch, SS_CustomController type, float val);

static void reset_generator_overrides(SS_MIDIChannel *ch) {
	for(int i = 0; i < SS_GEN_COUNT; i++)
		ch->generator_overrides[i] = GENERATOR_OVERRIDE_NO_CHANGE_VALUE;
	ch->generator_overrides_enabled = false;
}

static void reset_generator_offsets(SS_MIDIChannel *ch) {
	for(int i = 0; i < SS_GEN_COUNT; i++)
		ch->generator_offsets[i] = 0;
	ch->generator_offsets_enabled = false;
}

void ss_channel_reset_parameters_to_defaults(SS_MIDIChannel *ch) {
	ch->data_entry_state = SS_DATAENTRY_IDLE;
	ch->midi_controllers[SS_MIDCON_NRPN_LSB] = 127 << 7;
	ch->midi_controllers[SS_MIDCON_NRPN_MSB] = 127 << 7;
	ch->midi_controllers[SS_MIDCON_RPN_LSB] = 127 << 7;
	ch->midi_controllers[SS_MIDCON_RPN_MSB] = 127 << 7;
	reset_generator_overrides(ch);
	reset_generator_offsets(ch);
}

static bool non_resettable_controllers[128] = {
	[SS_MIDCON_BANK_SELECT] = true,
	[SS_MIDCON_BANK_SELECT_LSB] = true,
	[SS_MIDCON_MAIN_VOLUME] = true,
	[SS_MIDCON_MAIN_VOLUME_LSB] = true,
	[SS_MIDCON_PAN] = true,
	[SS_MIDCON_PAN_LSB] = true,
	[SS_MIDCON_REVERB_DEPTH] = true,
	[SS_MIDCON_TREMOLO_DEPTH] = true,
	[SS_MIDCON_CHORUS_DEPTH] = true,
	[SS_MIDCON_VARIATION_DEPTH] = true,
	[SS_MIDCON_PHASER_DEPTH] = true,
	[SS_MIDCON_SOUND_VARIATION] = true,
	[SS_MIDCON_FILTER_RESONANCE] = true,
	[SS_MIDCON_RELEASE_TIME] = true,
	[SS_MIDCON_ATTACK_TIME] = true,
	[SS_MIDCON_BRIGHTNESS] = true,
	[SS_MIDCON_DECAY_TIME] = true,
	[SS_MIDCON_VIBRATO_RATE] = true,
	[SS_MIDCON_VIBRATO_DEPTH] = true,
	[SS_MIDCON_VIBRATO_DELAY] = true,
	[SS_MIDCON_SOUND_CONTROLLER_10] = true,
	[SS_MIDCON_POLY_MODE_ON] = true,
	[SS_MIDCON_MONO_MODE_ON] = true,
	[SS_MIDCON_OMNI_MODE_ON] = true,
	[SS_MIDCON_OMNI_MODE_OFF] = true,

	// RP-15: Do not reset RPN or NRPN
	[SS_MIDCON_DATA_ENTRY_MSB] = true,
	[SS_MIDCON_DATA_ENTRY_LSB] = true,
	[SS_MIDCON_NRPN_LSB] = true,
	[SS_MIDCON_NRPN_MSB] = true,
	[SS_MIDCON_RPN_LSB] = true,
	[SS_MIDCON_RPN_MSB] = true
};

/* Values come from Falcosoft MidiPlayer 6 */
static const int16_t default_controller_values[SS_MIDI_CONTROLLER_COUNT] = {
	[SS_MIDCON_MAIN_VOLUME] = 100 << 7,
	[SS_MIDCON_BALANCE] = 64 << 7,
	[SS_MIDCON_EXPRESSION] = 127 << 7,
	[SS_MIDCON_PAN] = 64 << 7,

	[SS_MIDCON_PORTAMENTO_ON_OFF] = 127 << 7,

	[SS_MIDCON_FILTER_RESONANCE] = 64 << 7,
	[SS_MIDCON_RELEASE_TIME] = 64 << 7,
	[SS_MIDCON_ATTACK_TIME] = 64 << 7,
	[SS_MIDCON_BRIGHTNESS] = 64 << 7,

	[SS_MIDCON_DECAY_TIME] = 64 << 7,
	[SS_MIDCON_VIBRATO_RATE] = 64 << 7,
	[SS_MIDCON_VIBRATO_DEPTH] = 64 << 7,
	[SS_MIDCON_VIBRATO_DELAY] = 64 << 7,
	[SS_MIDCON_GENERAL_PURPOSE_CONTROLLER_6] = 64 << 7,
	[SS_MIDCON_GENERAL_PURPOSE_CONTROLLER_8] = 64 << 7,

	[SS_MIDCON_RPN_LSB] = 127 << 7,
	[SS_MIDCON_RPN_MSB] = 127 << 7,
	[SS_MIDCON_NRPN_LSB] = 127 << 7,
	[SS_MIDCON_NRPN_MSB] = 127 << 7,

	[NON_CC_INDEX_OFFSET + SS_MODSRC_PITCH_WHEEL] = 64 << 7,
	[NON_CC_INDEX_OFFSET + SS_MODSRC_PITCH_WHEEL_RANGE] = 2 << 7
};

enum { PORTAMENTO_CONTROL_UNSET = 1 };

static const float custom_reset_array[SS_CUSTOM_CTRL_COUNT] = {
	[SS_CUSTOM_CTRL_MODULATION_MULTIPLIER] = 1.0
};

static void reset_vibrato_params(SS_MIDIChannel *ch) {
	ch->channel_vibrato.rate = 0;
	ch->channel_vibrato.depth = 0;
	ch->channel_vibrato.delay = 0;
}

/**
 * https://amei.or.jp/midistandardcommittee/Recommended_Practice/e/rp15.pdf
 * Reset controllers according to RP-15 Recommended Practice.
 */
void ss_channel_reset_controllers_rp15_compliant(SS_MIDIChannel *ch, double time) {
	for(int i = 0; i < 128; i++) {
		const int16_t reset_value = default_controller_values[i];
		if(
		!non_resettable_controllers[i] &&
		reset_value != ch->midi_controllers[i] &&
		i != SS_MIDCON_PORTAMENTO_CONTROL) {
			ss_channel_controller(ch, i, reset_value >> 7, time);
		}
	}

	ss_channel_pitch_wheel(ch, 8192, -1, time);

	reset_generator_overrides(ch);
	reset_generator_offsets(ch);
}

static inline float drum_params_reverb(int note) {
	if(note == 35 || note == 36) /* Kicks have no reverb */
		return 0.0;
	else
		return 1.0;
}

void ss_channel_reset_drum_params(SS_MIDIChannel *ch) {
	/* Initialize drum params to defaults */
	for(int k = 0; k < 128; k++) {
		ch->drum_params[k].pitch = 0.0f;
		ch->drum_params[k].gain = 1.0f;
		ch->drum_params[k].exclusive_class = 0;
		ch->drum_params[k].pan = 64;
		ch->drum_params[k].filter_cutoff = 64;
		ch->drum_params[k].filter_resonance = 0;
		ch->drum_params[k].reverb_gain = drum_params_reverb(k);
		ch->drum_params[k].chorus_gain = 0.0f; /* No drums have chorus */
		ch->drum_params[k].delay_gain = 0.0f; /* No drums have delay */
		ch->drum_params[k].rx_note_on = true;
		ch->drum_params[k].rx_note_off = false;
	}
}

static void reset_portamento(SS_MIDIChannel *ch) {
	if(ch->locked_controllers[SS_MIDCON_PORTAMENTO_CONTROL]) return;

	if(ch->synth && ch->synth->master_params.midi_system == SS_SYSTEM_XG) {
		ss_channel_controller(ch, SS_MIDCON_PORTAMENTO_CONTROL, 60, 0);
	} else {
		ss_channel_controller(ch, SS_MIDCON_PORTAMENTO_CONTROL, 0, 0);
	}
}

/* Default controller values per SF2 spec */
void ss_channel_reset_controllers_to_defaults(SS_MIDIChannel *ch) {
	for(int cc = 0; cc < SS_MIDI_CONTROLLER_COUNT; cc++) {
		const int16_t reset_value = default_controller_values[cc];
		if(ch->midi_controllers[cc] != reset_value && cc < 127) {
			if(cc != SS_MIDCON_PORTAMENTO_CONTROL &&
			   cc != SS_MIDCON_DATA_ENTRY_MSB &&
			   cc != SS_MIDCON_RPN_MSB &&
			   cc != SS_MIDCON_RPN_LSB &&
			   cc != SS_MIDCON_NRPN_MSB &&
			   cc != SS_MIDCON_NRPN_LSB) {
				ss_channel_controller(ch, cc, reset_value >> 7, 0);
			}
		} else {
			/* Out of range, do a regular reset */
			ch->midi_controllers[cc] = reset_value;
		}
	}

	memset(ch->channel_octave_tuning, 0, sizeof(ch->channel_octave_tuning));
	ch->channel_tuning_cents = 0;

	ch->midi_controllers[NON_CC_INDEX_OFFSET + SS_MODSRC_PITCH_WHEEL_RANGE] = 2 << 7; /* Default 2 semitones */
	ss_channel_pitch_wheel(ch, 8192, -1, 0);

	ss_dynamic_modulator_system_init(&ch->dms);

	reset_portamento(ch);
	ss_channel_reset_drum_params(ch);
	reset_vibrato_params(ch);

	ch->assign_mode = 2;
	ch->poly_mode = true;
	ch->rx_channel = ch->channel_number;
	ch->random_pan = false;

	ss_channel_reset_parameters_to_defaults(ch);

	/* Reset custom controllers
	 * Special case: transpose does not get affected
	 */
	const float transpose =
	ch->custom_controllers[SS_CUSTOM_CTRL_TRANSPOSE_FINE];
	memcpy(&ch->custom_controllers, &custom_reset_array, sizeof(ch->custom_controllers));
	ss_channel_set_custom_controller(ch, SS_CUSTOM_CTRL_TRANSPOSE_FINE, transpose);
}

void ss_channel_reset_controllers(SS_MIDIChannel *ch) {
	ss_channel_reset_controllers_to_defaults(ch);
}
