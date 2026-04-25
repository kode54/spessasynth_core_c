/**
 * data_entry_fine.c
 * Per-MIDI-channel NRPN handler for data entry, fine.
 * Port of data_entry_fine.ts.
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

extern void ss_channel_nrpn_awe32(SS_MIDIChannel *ch, int awe_gen, int data_lsb, int data_msb, double time);

extern void ss_channel_reset_parameters_to_defaults(SS_MIDIChannel *ch);
extern void ss_channel_set_tuning(SS_MIDIChannel *ch, float cents);
extern void ss_channel_set_modulation_depth(SS_MIDIChannel *ch, float cents);

void ss_channel_data_entry_fine(SS_MIDIChannel *ch, int val, double time) {
	ch->midi_controllers[SS_MIDCON_DATA_ENTRY_LSB] = val << 7;
	switch(ch->data_entry_state) {
		default:
			break;

		case SS_DATAENTRY_RP_COARSE:
		case SS_DATAENTRY_RP_FINE: {
			const int rpn_value = ch->midi_controllers[SS_MIDCON_RPN_MSB] |
			                      (ch->midi_controllers[SS_MIDCON_RPN_LSB] >> 7);
			switch(rpn_value) {
				default:
					break;

				/* Pitch bend range fine tune */
				case SS_RPN_PITCH_WHEEL_RANGE:
					if(val == 0) {
						break;
					}
					/* 14-bit value, so upper 7 are coarse and lower 7 are fine! */
					ch->midi_controllers[NON_CC_INDEX_OFFSET + SS_MODSRC_PITCH_WHEEL_RANGE] |= val;
					break;

				/* Fine-tuning */
				case SS_RPN_FINE_TUNING: {
					/* Grab the data and shift */
					const int coarse = (int)ch->custom_controllers[SS_CUSTOM_CTRL_TUNING];
					const int final_tuning = (coarse << 7) | val;
					ss_channel_set_tuning(ch, (float)final_tuning * 0.01220703125); /* Multiply by 8192 / 100 (cent increments)) */
					break;
				}

				/* Modulation depth */
				case SS_RPN_MODULATION_DEPTH: {
					const float current_depth_cents = ch->custom_controllers[SS_CUSTOM_CTRL_MODULATION_MULTIPLIER] * 50.0;
					const float cents = current_depth_cents + ((float)val / 128.0) * 100.0;
					ss_channel_set_modulation_depth(ch, cents);
					break;
				}

				case SS_RPN_RESET_PARAMETERS:
					ss_channel_reset_parameters_to_defaults(ch);
					break;
			}
			break;
		}

		case SS_DATAENTRY_NRP_FINE: {
			const int param_coarse = ch->midi_controllers[SS_MIDCON_NRPN_MSB] >> 7;
			const int param_fine = ch->midi_controllers[SS_MIDCON_NRPN_LSB] >> 7;

			/* SF2 and GS NRPN don't use lsb (but sometimes these are still sent!) */
			if(param_coarse == SS_NRPN_MSB_SF2 ||
			   (param_coarse >= SS_NRPN_GS_MSB_DRUM_FILTER_CUTOFF &&
			    param_coarse <= SS_NRPN_GS_MSB_DRUM_DELAY_SEND) ||
			   param_coarse == SS_NRPN_MSB_PART_PARAMETER) {
				return;
			}
			switch(param_coarse) {
				default:
					/* Unsupported NRPN */
					break;

				case SS_NRPN_MSB_AWE32:
					ss_channel_nrpn_awe32(ch, param_fine, val, ch->midi_controllers[SS_MIDCON_DATA_ENTRY_MSB] >> 7, time);
					break;
			}
			break;
		}
	}
}
