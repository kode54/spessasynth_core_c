/**
 * controller_change.c
 * Per-MIDI-channel controller change management.
 * Port of controller_change.ts.
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>
#if __has_include(<spessasynth_core/spessasynth.h>)
#include <spessasynth_core/midi_enums.h>
#include <spessasynth_core/nrpn_enums.h>
#include <spessasynth_core/synth.h>
#else
#include "spessasynth/midi/midi_enums.h"
#include "spessasynth/midi/nrpn_enums.h"
#include "spessasynth/synthesizer/synth.h"
#endif

/* ── Controller change ───────────────────────────────────────────────────── */

extern void ss_channel_compute_modulators(SS_MIDIChannel *ch, double time);
extern void ss_voice_release(SS_Voice *v, double current_time, double min_note_length);
extern void ss_voice_compute_modulators(SS_Voice *v, const SS_MIDIChannel *ch,
                                        double time);

extern void ss_channel_reset_controllers_rp15_compliant(SS_MIDIChannel *ch, double time);

void ss_channel_data_entry_coarse(SS_MIDIChannel *ch, int val, double time);
void ss_channel_data_entry_fine(SS_MIDIChannel *ch, int val, double time);

void ss_channel_set_generator_offset(SS_MIDIChannel *ch, SS_GeneratorType gen, int val, double time) {
	ch->generator_offsets[gen] = val;
	ch->generator_offsets_enabled = true;
	ss_channel_compute_modulators(ch, time);
}

void ss_channel_set_generator_override(SS_MIDIChannel *ch, SS_GeneratorType gen, int val, bool realtime, double time) {
	ch->generator_overrides[gen] = val;
	ch->generator_overrides_enabled = true;
	if(realtime) {
		/* Patch voice->generators directly, matching TS setGeneratorOverride realtime path */
		for(size_t vi = 0; vi < ch->voice_count; vi++) {
			SS_Voice *v = ch->voices[vi];
			if(v->is_active) {
				v->generators[gen] = (int16_t)val;
				ss_voice_compute_modulators(v, ch, time);
			}
		}
	}
}

static void ss_channel_update_tuning(SS_MIDIChannel *ch) {
	ch->channel_tuning_cents =
	ch->custom_controllers[SS_CUSTOM_CTRL_TUNING] + /* RPN channel fine tuning */
	ch->custom_controllers[SS_CUSTOM_CTRL_TRANSPOSE_FINE] + /* User tuning (transpose) */
	ch->custom_controllers[SS_CUSTOM_CTRL_MASTER_TUNING] + /* Master tuning, set by sysEx */
	ch->custom_controllers[SS_CUSTOM_CTRL_TUNING_SEMITONES] *
	100; /* RPN channel coarse tuning */
}

void ss_channel_set_custom_controller(SS_MIDIChannel *ch, SS_CustomController type, float val) {
	ch->custom_controllers[type] = val;
	ss_channel_update_tuning(ch);
}

void ss_channel_set_tuning(SS_MIDIChannel *ch, float cents) {
	cents = round(cents);
	ss_channel_set_custom_controller(ch, SS_CUSTOM_CTRL_TUNING, cents);
}

void ss_channel_set_modulation_depth(SS_MIDIChannel *ch, float cents) {
	cents = round(cents);
	ss_channel_set_custom_controller(ch, SS_CUSTOM_CTRL_MODULATION_MULTIPLIER, cents / 50.0);
}

void ss_channel_controller(SS_MIDIChannel *ch, int cc, int val, double time) {
	if(cc < 0 || cc >= SS_MIDI_CONTROLLER_COUNT) return;
	if(ch->locked_controllers[cc]) return;

	if(
	cc >= SS_MIDCON_MODULATION_WHEEL_LSB &&
	cc <= SS_MIDCON_EFFECT_CONTROL_2_LSB &&
	cc != SS_MIDCON_DATA_ENTRY_LSB) {
		const int actualCCNum = cc - 32;
		if(ch->locked_controllers[actualCCNum]) {
			return;
		}
		// Append the lower nibble to the main controller
		ch->midi_controllers[actualCCNum] =
		(ch->midi_controllers[actualCCNum] & 0x3f80) |
		(val & 0x7f);

		ss_channel_compute_modulators(ch, time);
	}

	ch->midi_controllers[cc] = (int16_t)(val << 7);

	switch(cc) {
		case SS_MIDCON_ALL_NOTES_OFF: /* all notes off */
			ss_channel_all_notes_off(ch, time);
			break;
		case SS_MIDCON_ALL_SOUND_OFF: /* all sound off */
			ss_channel_all_sound_off(ch);
			break;

		case SS_MIDCON_BANK_SELECT:
			ch->bank_msb = val;
			/* Ensure that for XG, drum channels always are 127
			 * Testcase
			 * Dave-Rodgers-D-j-Vu-Anonymous-20200419154845-nonstop2k.com.mid
			 */
			if(ch->channel_number % 16 == 9 &&
			   ch->synth && ch->synth->master_params.midi_system == SS_SYSTEM_XG) {
				ch->bank_msb = 127;
			}
			if(ch->synth && ch->synth->master_params.midi_system == SS_SYSTEM_XG) {
				ch->drum_channel = ch->bank_msb == 120 || ch->bank_msb == 127;
			}
			break;

		case SS_MIDCON_BANK_SELECT_LSB:
			ch->bank_lsb = val;
			break;

		/* Check for RPN and NPRN and data entry */
		case SS_MIDCON_RPN_LSB:
			ch->data_entry_state = SS_DATAENTRY_RP_FINE;
			break;

		case SS_MIDCON_RPN_MSB:
			ch->data_entry_state = SS_DATAENTRY_RP_COARSE;
			break;

		case SS_MIDCON_NRPN_MSB:
			ch->custom_controllers[SS_CUSTOM_CTRL_SF2_NRPN_GENERATOR_LSB] = 0;
			ch->data_entry_state = SS_DATAENTRY_NRP_COARSE;
			break;

		case SS_MIDCON_NRPN_LSB:
			if(
			ch->midi_controllers[SS_MIDCON_NRPN_MSB] >> 7 == SS_NRPN_MSB_SF2) {
				/* If a <100 value has already been sent, reset! */
				if(
				(int)ch->custom_controllers[SS_CUSTOM_CTRL_SF2_NRPN_GENERATOR_LSB] % 100 != 0) {
					ch->custom_controllers[SS_CUSTOM_CTRL_SF2_NRPN_GENERATOR_LSB] = 0;
				}

				if(val == 100) {
					ch->custom_controllers[SS_CUSTOM_CTRL_SF2_NRPN_GENERATOR_LSB] += 100.0;
				} else if(val == 101) {
					ch->custom_controllers[SS_CUSTOM_CTRL_SF2_NRPN_GENERATOR_LSB] += 1000.0;
				} else if(val == 102) {
					ch->custom_controllers[SS_CUSTOM_CTRL_SF2_NRPN_GENERATOR_LSB] += 10000.0;
				} else if(val < 100) {
					ch->custom_controllers[SS_CUSTOM_CTRL_SF2_NRPN_GENERATOR_LSB] += (float)val;
				}
			}
			ch->data_entry_state = SS_DATAENTRY_NRP_FINE;
			break;

		case SS_MIDCON_DATA_ENTRY_MSB:
			ss_channel_data_entry_coarse(ch, val, time);
			break;

		case SS_MIDCON_DATA_ENTRY_LSB:
			ss_channel_data_entry_fine(ch, val, time);
			break;

		case SS_MIDCON_SUSTAIN_PEDAL: /* sustain pedal */
			if(val < 64 && ch->sustained_count > 0) {
				/* Release all sustained voices */
				for(size_t i = 0; i < ch->sustained_count; i++) {
					SS_Voice *v = ch->sustained_voices[i];
					if(v->is_active && !v->is_in_release)
						ss_voice_release(v, time, 0.05);
				}
				ch->sustained_count = 0;
			}
			break;

		case SS_MIDCON_RESET_ALL_CONTROLLERS: /* reset all controllers */
			ss_channel_reset_controllers_rp15_compliant(ch, time);
			break;

		default: /* Compute modulators */
			ss_channel_compute_modulators(ch, time);
			break;
	}
}
