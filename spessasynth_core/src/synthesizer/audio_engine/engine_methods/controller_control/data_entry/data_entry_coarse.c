/**
 * data_entry_coarse.c
 * Per-MIDI-channel NRPN handler for data entry, coarse.
 * Port of data_entry_coarse.ts.
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

extern void ss_channel_set_generator_offset(SS_MIDIChannel *ch, SS_GeneratorType gen, int val, double time);

extern void ss_channel_reset_parameters_to_defaults(SS_MIDIChannel *ch);
extern void ss_channel_set_custom_controller(SS_MIDIChannel *ch, SS_CustomController type, float val);
extern void ss_channel_set_tuning(SS_MIDIChannel *ch, float cents);
extern void ss_channel_set_modulation_depth(SS_MIDIChannel *ch, float cents);

void ss_channel_data_entry_coarse(SS_MIDIChannel *ch, int val, double time) {
	ch->midi_controllers[SS_MIDCON_DATA_ENTRY_MSB] = val << 7;

/*
A note on this vibrato.
This is a completely custom vibrato, with its own oscillator and parameters.
It is disabled by default,
only being enabled when one of the NPRN messages changing it is received
and stays on until the next system-reset.
It was implemented very early in SpessaSynth's development,
because I wanted support for Touhou MIDIs :-)
 */
#define addDefaultVibrato                \
	if(                                  \
	ch->channel_vibrato.delay == 0 &&    \
	ch->channel_vibrato.rate == 0 &&     \
	ch->channel_vibrato.depth == 0) {    \
		ch->channel_vibrato.depth = 50;  \
		ch->channel_vibrato.rate = 8;    \
		ch->channel_vibrato.delay = 0.6; \
	}

	switch(ch->data_entry_state) {
		default:
		case SS_DATAENTRY_IDLE:
			break;

		// Process GS NRPNs
		case SS_DATAENTRY_NRP_COARSE:
		case SS_DATAENTRY_NRP_FINE: {
			const int nrpn_coarse = ch->midi_controllers[SS_MIDCON_NRPN_MSB] >> 7;
			const int nrpn_fine = ch->midi_controllers[SS_MIDCON_NRPN_LSB] >> 7;
			const int data_entry_fine = ch->midi_controllers[SS_MIDCON_DATA_ENTRY_LSB] >> 7;
			switch(nrpn_coarse) {
				default:
					if(val == 64) {
						/* Default value */
						return;
					}
					/* Unrecognized NRPN */
					break;

				case SS_NRPN_GS_MSB_DRUM_FILTER_CUTOFF:
					ch->drum_params[nrpn_fine].filter_cutoff = val;
					break;

				case SS_NRPN_GS_MSB_DRUM_FILTER_Q:
					ch->drum_params[nrpn_fine].filter_resonance = val;
					break;

				case SS_NRPN_GS_MSB_DRUM_PITCH_COARSE: {
					/* Pitch range */
					const bool is_gs = ch->synth && ch->synth->master_params.midi_system == SS_SYSTEM_GS;
					const bool is_50cent = is_gs && ch->bank_lsb != 1;
					const float range = is_50cent ? 50.0 : 100.0;
					ch->drum_params[nrpn_fine].pitch = (((float)val) - 64.0) * range;
					break;
				}

				case SS_NRPN_GS_MSB_DRUM_PITCH_FINE: {
					ch->drum_params[nrpn_fine].pitch += ((float)val) - 64.0;
					break;
				}

				case SS_NRPN_GS_MSB_DRUM_TVA_LEVEL:
					ch->drum_params[nrpn_fine].gain = ((float)val) / 127.0;
					break;

				case SS_NRPN_GS_MSB_DRUM_PANPOT:
					ch->drum_params[nrpn_fine].pan = val;
					break;

				case SS_NRPN_GS_MSB_DRUM_REVERB_SEND:
					ch->drum_params[nrpn_fine].reverb_gain = ((float)val) / 127.0;
					break;

				case SS_NRPN_GS_MSB_DRUM_CHORUS_SEND:
					ch->drum_params[nrpn_fine].chorus_gain = ((float)val) / 127.0;
					break;

				case SS_NRPN_GS_MSB_DRUM_DELAY_SEND:
					ch->drum_params[nrpn_fine].delay_gain = ((float)val) / 127.0;
					if(ch->synth) ch->synth->delay_active = true;
					break;

				case SS_NRPN_MSB_PART_PARAMETER:
					switch(nrpn_fine) {
						default:
							if(val == 64) {
								/* Default value */
								return;
							}
							/* Unrecognized NRPN */
							break;

						case SS_NRPN_GS_LSB_VIBRATO_RATE:
							if(ch->dms.is_active) {
								ss_channel_controller(ch, SS_MIDCON_VIBRATO_RATE, val, time);
								return;
							}
							if(val == 64) {
								/* Default value */
								return;
							}
							addDefaultVibrato;
							ch->channel_vibrato.rate = ((float)val / 64.0) * 8.0;
							break;

						case SS_NRPN_GS_LSB_VIBRATO_DEPTH:
							if(val == 64) {
								/* Default value */
								return;
							}
							addDefaultVibrato;
							ch->channel_vibrato.depth = (float)val / 2.0;
							break;

						case SS_NRPN_GS_LSB_VIBRATO_DELAY:
							if(val == 64) {
								/* Default value */
								return;
							}
							addDefaultVibrato;
							ch->channel_vibrato.delay = (float)val / 64.0 / 3.0;
							break;

						case SS_NRPN_GS_LSB_TVF_FILTER_CUTOFF:
							ss_channel_controller(ch, SS_MIDCON_BRIGHTNESS, val, time);
							break;

						case SS_NRPN_GS_LSB_TVF_FILTER_RESONANCE:
							ss_channel_controller(ch, SS_MIDCON_FILTER_RESONANCE, val, time);
							break;

						case SS_NRPN_GS_LSB_EG_ATTACK_TIME:
							ss_channel_controller(ch, SS_MIDCON_ATTACK_TIME, val, time);
							break;

						case SS_NRPN_GS_LSB_EG_DECAY_TIME:
							ss_channel_controller(ch, SS_MIDCON_DECAY_TIME, val, time);
							break;

						case SS_NRPN_GS_LSB_EG_RELEASE_TIME:
							ss_channel_controller(ch, SS_MIDCON_RELEASE_TIME, val, time);
							break;
					}
					break;

				case SS_NRPN_MSB_AWE32:
					break;

					/* SF2 NRPN */
				case SS_NRPN_MSB_SF2:
					if(nrpn_fine > 100) {
						/* Sf spec:
						 * Note that NRPN Select LSB greater than 100 are for setup only,
						 * and should not be used on their own to select a
						 * Generator parameter.
						 */
						break;
					}
					const SS_GeneratorType gen = (SS_GeneratorType)ch->custom_controllers[SS_CUSTOM_CTRL_SF2_NRPN_GENERATOR_LSB];
					const int offset = (val << 7) | data_entry_fine;
					ss_channel_set_generator_offset(ch, gen, offset, time);
					break;
			}
			break;
		}

		case SS_DATAENTRY_RP_COARSE:
		case SS_DATAENTRY_RP_FINE: {
			const int rpn_value = ch->midi_controllers[SS_MIDCON_RPN_MSB] |
			                      ch->midi_controllers[SS_MIDCON_RPN_LSB] >> 7;
			switch(rpn_value) {
				default:
					/* Unsupported RPN */
					break;

					/* Pitch bend range */
				case SS_RPN_PITCH_WHEEL_RANGE:
					ch->midi_controllers[NON_CC_INDEX_OFFSET + SS_MODSRC_PITCH_WHEEL_RANGE] = val << 7;
					break;

				case SS_RPN_COARSE_TUNING: {
					/* Semitones */
					const int semitones = val - 64;
					ss_channel_set_custom_controller(ch, SS_CUSTOM_CTRL_TUNING_SEMITONES, semitones);
					break;
				}

					/* Fine-tuning */
				case SS_RPN_FINE_TUNING:
					/* Note: this will not work properly unless the lsb is sent!
					 * Here we store the raw value to then adjust in fine
					 */
					ss_channel_set_tuning(ch, (float)(val - 64));
					break;

					/* Modulation depth */
				case SS_RPN_MODULATION_DEPTH:
					ss_channel_set_modulation_depth(ch, (float)val * 100.0);
					break;

				case SS_RPN_RESET_PARAMETERS:
					ss_channel_reset_parameters_to_defaults(ch);
					break;
			}
			break;
		}
	}
}
