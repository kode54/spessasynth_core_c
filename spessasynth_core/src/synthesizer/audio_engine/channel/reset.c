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

/* 1 / cos(pi/4)^2 = 2.0: corrects insertion send levels from 0-1 to 0-2 range */
#define EFX_SENDS_GAIN_CORRECTION 2.0f

extern void ss_channel_set_custom_controller(SS_MIDIChannel *ch, SS_CustomController type, float val);
extern void ss_channel_reset_midi_parameters(SS_MIDIChannel *ch);

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
	/* Reset Parameters (do not emit controller change)
	 * We reset them here since in the loop, the data entries would come before params
	 */
	ch->last_parameter_is_registered = true;
	ch->midi_controllers[SS_MIDCON_NRPN_LSB] = 127 << 7;
	ch->midi_controllers[SS_MIDCON_NRPN_MSB] = 127 << 7;
	ch->midi_controllers[SS_MIDCON_RPN_LSB] = 127 << 7;
	ch->midi_controllers[SS_MIDCON_RPN_MSB] = 127 << 7;
	ch->midi_controllers[SS_MIDCON_DATA_ENTRY_MSB] = 0;
	ch->midi_controllers[SS_MIDCON_DATA_ENTRY_LSB] = 0;
	reset_generator_overrides(ch);
	reset_generator_offsets(ch);
}

/* Values come from Falcosoft MidiPlayer 6 */
static const int16_t default_controller_values[128] = {
	[SS_MIDCON_MAIN_VOLUME] = 100 << 7,
	[SS_MIDCON_BALANCE] = 64 << 7,
	[SS_MIDCON_EXPRESSION] = 127 << 7,
	[SS_MIDCON_PAN] = 64 << 7,

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
};

enum { PORTAMENTO_CONTROL_UNSET = 1 };

static const float custom_reset_array[SS_CUSTOM_CTRL_COUNT] = {
	[SS_CUSTOM_CTRL_MODULATION_MULTIPLIER] = 1.0
};

/**
 * https://amei.or.jp/midistandardcommittee/Recommended_Practice/e/rp15.pdf
 * Reset controllers according to RP-15 Recommended Practice.
 */
void ss_channel_reset_rp15(SS_MIDIChannel *ch, double time) {
	const uint8_t rp_15_reset_cc_nums[] = {
		SS_MIDCON_MODULATION_WHEEL,
		SS_MIDCON_EXPRESSION,
		SS_MIDCON_SUSTAIN_PEDAL,
		SS_MIDCON_PORTAMENTO_ON_OFF,
		SS_MIDCON_SOSTENUTO_PEDAL,
		SS_MIDCON_SOFT_PEDAL,
		SS_MIDCON_RPN_MSB,
		SS_MIDCON_RPN_LSB
	};
	for(size_t i = 0, j = sizeof(rp_15_reset_cc_nums) / sizeof(rp_15_reset_cc_nums[0]); i < j; i++) {
		const uint8_t reset_cc = rp_15_reset_cc_nums[i];
		const int16_t reset_value = default_controller_values[reset_cc];
		if(reset_value != ch->midi_controllers[reset_cc]) {
			ss_channel_controller(ch, reset_cc, reset_value >> 7, time);
		}
	}
}

static inline float drum_params_reverb(int note) {
	if(note == 35 || note == 36) /* Kicks have no reverb */
		return 0.0;
	else
		return 1.0;
}

void ss_channel_reset_drum_params(SS_MIDIChannel *ch) {
	/* The drum parameters can be locked against MIDI/API edits. */
	if(ch->synth && ch->synth->system_params.drum_lock) return;
	/* Initialize drum params to defaults */
	bool is_xg = ch->synth && ch->synth->midi_params.system == SS_SYSTEM_XG;
	for(int k = 0; k < 128; k++) {
		ch->drum_params[k].pitch = 0.0f;
		ch->drum_params[k].gain = 1.0f;
		ch->drum_params[k].exclusive_class = 0;
		ch->drum_params[k].pan = 64;
		ch->drum_params[k].filter_cutoff = 64;
		ch->drum_params[k].filter_resonance = 0;
		ch->drum_params[k].reverb_gain = drum_params_reverb(k);
		ch->drum_params[k].chorus_gain = is_xg ? drum_params_reverb(k) : 0.0f; /* Mirror reverb on XG only, GS has no chorus by default */
		ch->drum_params[k].delay_gain = 0.0f; /* No drums have delay */
		ch->drum_params[k].rx_note_on = true;
		ch->drum_params[k].rx_note_off = false;
	}
}

static void reset_portamento(SS_MIDIChannel *ch) {
	if(ch->locked_controllers[SS_MIDCON_PORTAMENTO_CONTROL]) return;

	if(ch->synth && ch->synth->midi_params.system == SS_SYSTEM_XG) {
		ch->last_note = 60;
	} else {
		ch->last_note = -1;
	}
}

/* Default controller values per SF2 spec */
void ss_channel_reset_internal(SS_MIDIChannel *ch) {
	/* Reset MIDI controllers */
	for(int cc = 0; cc < 128; cc++) {
		const int16_t reset_value = default_controller_values[cc];
		if(ch->midi_controllers[cc] != reset_value &&
		   cc != SS_MIDCON_PORTAMENTO_CONTROL &&
		   cc != SS_MIDCON_DATA_ENTRY_MSB &&
		   cc != SS_MIDCON_RPN_MSB &&
		   cc != SS_MIDCON_RPN_LSB &&
		   cc != SS_MIDCON_NRPN_MSB &&
		   cc != SS_MIDCON_NRPN_LSB) {
			ss_channel_controller(ch, cc, reset_value >> 7, 0);
		}
	}

	/* Reset insertion: free old processor, create default Thru */
	if(ch->synth) {
		SS_Processor *proc = ch->synth;
		ss_insertion_free(proc->insertion);
		proc->insertion = ss_insertion_create(0x0000, proc->sample_rate, SS_MAX_SOUND_CHUNK);
		if(proc->insertion) {
			proc->insertion->send_level_to_reverb = (40.0f / 127.0f) * EFX_SENDS_GAIN_CORRECTION;
			proc->insertion->send_level_to_chorus = 0.0f;
			proc->insertion->send_level_to_delay = 0.0f;
		}
		proc->insertion_active = false;
		for(int i = 0; i < proc->channel_count; i++) {
			if(proc->midi_channels[i])
				proc->midi_channels[i]->midi_params.efx_assign = false;
		}
	}

	ch->midi_controllers[NON_CC_INDEX_OFFSET + SS_MODSRC_PITCH_WHEEL_RANGE] = 2 << 7; /* Default 2 semitones */
	ss_channel_pitch_wheel(ch, 8192, -1, 0);

	reset_portamento(ch);
	ss_channel_reset_drum_params(ch);
	ss_channel_reset_parameters_to_defaults(ch);

	ss_dynamic_modulator_system_init(&ch->dms);

	/* Reset the per-channel MIDI parameters (assign mode, poly mode,
	 * receive channel, random pan, CC1/CC2, ...) to their defaults.
	 * Per-channel system parameters are API-only and survive a MIDI reset. */
	ss_channel_reset_midi_parameters(ch);

	memcpy(&ch->custom_controllers, &custom_reset_array, sizeof(ch->custom_controllers));

	memset(ch->channel_octave_tuning, 0, sizeof(ch->channel_octave_tuning));
	ch->channel_tuning_cents = 0;

	/* Refresh the current_* aggregates from the parameter structs. */
	ss_channel_update_internal_params(ch);

	/* Reset program */
	const int default_bank_msb = ch->synth ? (ch->synth->midi_params.system == SS_SYSTEM_GM2 ? 121 : 0) : 0;
	ch->midi_controllers[SS_MIDCON_BANK_SELECT] = default_bank_msb << 7;
	ch->bank_msb = default_bank_msb;
	ch->bank_lsb = 0;
	ch->drum_channel = ch->channel_number % 16 == 9;
	ss_channel_program_change(ch, 0);
}

void ss_channel_reset(SS_MIDIChannel *ch) {
	ss_channel_reset_internal(ch);
}
