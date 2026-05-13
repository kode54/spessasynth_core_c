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
extern void ss_channel_set_pitch_wheel_range(SS_MIDIChannel *ch, int value);
extern void ss_channel_nrpn_awe32(SS_MIDIChannel *ch, int param_lsb, int data_value, double time);

void ss_channel_data_entry(SS_MIDIChannel *ch, double time) {
	const uint16_t data_value = ch->midi_controllers[SS_MIDCON_DATA_ENTRY_MSB];

#define addDefaultVibrato                  \
	if(                                    \
	ch->channel_vibrato.delay == 0 &&      \
	ch->channel_vibrato.rate == 0 &&       \
	ch->channel_vibrato.depth == 0) {      \
		ch->channel_vibrato.depth = 50.0f; \
		ch->channel_vibrato.rate = 8.0f;   \
		ch->channel_vibrato.delay = 0.6f;  \
	}

	switch(ch->data_entry_state) {
		default:
		case SS_DATAENTRY_IDLE:
			break;

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
					ss_channel_set_pitch_wheel_range(ch, data_value);
					break;

				case SS_RPN_COARSE_TUNING: {
					/* Semitones */
					const int semitones = (data_value >> 7) - 64;
					ss_channel_set_custom_controller(ch, SS_CUSTOM_CTRL_TUNING_SEMITONES, (float)semitones);
					break;
				}

					/* Fine-tuning */
				case SS_RPN_FINE_TUNING: {
					const int final_tuning = data_value - 8192;
					/* Resolution is 100/8192 cents */
					ss_channel_set_tuning(ch, (float)final_tuning / 81.92);
					break;
				}

					/* Modulation depth */
				case SS_RPN_MODULATION_DEPTH:
					/* Cents, so data / 128 * 100 is data / 1.28 */
					ss_channel_set_modulation_depth(ch, (float)data_value / 1.28f);
					break;

				case SS_RPN_RESET_PARAMETERS:
					/* Ignore */
					break;
			}
			break;
		}

		// Process GS NRPNs
		case SS_DATAENTRY_NRP_COARSE:
		case SS_DATAENTRY_NRP_FINE: {
			const int param_coarse = ch->midi_controllers[SS_MIDCON_NRPN_MSB] >> 7;
			const int param_fine = ch->midi_controllers[SS_MIDCON_NRPN_LSB] >> 7;
			const int data_coarse = data_value >> 7;
			switch(param_coarse) {
				default:
					/* Unrecognized NRPN */
					break;

				case SS_NRPN_MSB_PART_PARAMETER: {
					switch(param_fine) {
						/*
						 * A note on this vibrato.
						 * This is a completely custom vibrato, with its own oscillator and parameters.
						 * It is disabled by default,
						 * only being enabled when one of the NRPN messages changing it is received
						 * and stays on until the next system-reset.
						 * It was implemented very early in SpessaSynth's development,
						 * because I wanted support for Touhou MIDIs :-)
						 */
						default:
							/* Unsupported parameter */
							break;

						case SS_NRPN_GS_LSB_VIBRATO_RATE: {
							if(ch->dms.is_active) {
								ss_channel_controller(ch, SS_MIDCON_VIBRATO_RATE, data_coarse, time);
								return;
							}
							if(data_coarse == 64) return;
							addDefaultVibrato;
							ch->channel_vibrato.rate = ((float)data_coarse / 64.0) * 8.0;
							break;
						}

						case SS_NRPN_GS_LSB_VIBRATO_DEPTH: {
							if(data_coarse == 64) return;
							addDefaultVibrato;
							ch->channel_vibrato.depth = (float)data_coarse / 2.0;
							break;
						}

						case SS_NRPN_GS_LSB_VIBRATO_DELAY: {
							if(data_coarse == 64) return;
							addDefaultVibrato;
							ch->channel_vibrato.delay = (float)data_coarse / 64.0 / 3.0;
							break;
						}

						case SS_NRPN_GS_LSB_TVF_FILTER_CUTOFF: {
							ss_channel_controller(ch, SS_MIDCON_BRIGHTNESS, data_coarse, time);
							break;
						}

						case SS_NRPN_GS_LSB_TVF_FILTER_RESONANCE: {
							ss_channel_controller(ch, SS_MIDCON_FILTER_RESONANCE, data_coarse, time);
							break;
						}

						case SS_NRPN_GS_LSB_EG_ATTACK_TIME: {
							ss_channel_controller(ch, SS_MIDCON_ATTACK_TIME, data_coarse, time);
							break;
						}

						case SS_NRPN_GS_LSB_EG_DECAY_TIME: {
							ss_channel_controller(ch, SS_MIDCON_DECAY_TIME, data_coarse, time);
							break;
						}

						case SS_NRPN_GS_LSB_EG_RELEASE_TIME: {
							ss_channel_controller(ch, SS_MIDCON_RELEASE_TIME, data_coarse, time);
							break;
						}
					}
					break;
				}

				case SS_NRPN_GS_MSB_DRUM_FILTER_CUTOFF:
					ch->drum_params[param_fine].filter_cutoff = data_coarse;
					break;

				case SS_NRPN_GS_MSB_DRUM_FILTER_Q:
					ch->drum_params[param_fine].filter_resonance = data_coarse;
					break;

				case SS_NRPN_GS_MSB_DRUM_PITCH_COARSE: {
					/**
					 * https://github.com/spessasus/spessasynth_core/pull/58#issuecomment-3893343073
					 * it's actually 50 cents! (not for XG though)
					 * also if SC-55 preset is explicitly requested (MAP1 - LSB 1), it's 100 cents as well!
					 */
					const bool is_xg = ch->synth && ch->synth->master_params.midi_system == SS_SYSTEM_XG;
					const bool is_100cent = is_xg && ch->bank_lsb == 1;
					const float range = is_100cent ? 100.0f : 50.0f;
					ch->drum_params[param_fine].pitch = (((float)data_coarse) - 64.0f) * range;
					break;
				}

				case SS_NRPN_GS_MSB_DRUM_PITCH_FINE: {
					const int pitch = (int)data_coarse - 64;
					ch->drum_params[param_fine].pitch += (float)pitch;
					break;
				}

				case SS_NRPN_GS_MSB_DRUM_TVA_LEVEL:
					ch->drum_params[param_fine].gain = ((float)data_coarse) / 127.0f;
					break;

				case SS_NRPN_GS_MSB_DRUM_PANPOT:
					ch->drum_params[param_fine].pan = data_coarse;
					break;

				case SS_NRPN_GS_MSB_DRUM_REVERB_SEND:
					ch->drum_params[param_fine].reverb_gain = ((float)data_coarse) / 127.0f;
					break;

				case SS_NRPN_GS_MSB_DRUM_CHORUS_SEND:
					ch->drum_params[param_fine].chorus_gain = ((float)data_coarse) / 127.0f;
					break;

				case SS_NRPN_GS_MSB_DRUM_DELAY_SEND:
					ch->drum_params[param_fine].delay_gain = ((float)data_coarse) / 127.0f;
					if(ch->synth) ch->synth->delay_active = true;
					break;

				case SS_NRPN_MSB_AWE32:
					ss_channel_nrpn_awe32(ch, param_fine, data_value, time);
					break;

					/* SF2 NRPN */
				case SS_NRPN_MSB_SF2:
					if(param_fine > 100) {
						/* Sf spec:
						 * Note that NRPN Select LSB greater than 100 are for setup only,
						 * and should not be used on their own to select a
						 * Generator parameter.
						 */
						break;
					}
					const SS_GeneratorType gen = (SS_GeneratorType)ch->custom_controllers[SS_CUSTOM_CTRL_SF2_NRPN_GENERATOR_LSB];
					const int offset = (int)data_value - 8192;
					ss_channel_set_generator_offset(ch, gen, offset, time);
					break;
			}
			break;
		}
	}
}
