/**
 * handle_xg.c
 * Yamaha XG SysEx handler
 * Port of handle_xg.ts.
 */

#define _USE_MATH_DEFINES
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

/* ── System Exclusive: Yamaha XG ─────────────────────────────────────────── */

extern void ss_processor_set_midi_volume(SS_Processor *proc, float volume);
extern void ss_channel_set_custom_controller(SS_MIDIChannel *ch, SS_CustomController type, float val);
extern void ss_processor_event_emit(SS_Processor *proc, SS_SynthEventType type,
                                    int channel, int v1, int v2);

void ss_sysex_handle_xg(SS_Processor *proc, const uint8_t *data, size_t len, double t, int channel_offset) {
	/*  data[0]=0x43, data[1]=0x10 (parameter change), data[2]=0x4c (XG),
	          data[3]=addr1, data[4]=addr2, data[5]=addr3, data[6]=value */
	if(len < 7) return;
	if(data[1] != 0x10 || data[2] != 0x4c) return;

	uint8_t xg_addr1 = data[3];
	uint8_t xg_addr2 = data[4];
	uint8_t xg_addr3 = data[5];
	uint8_t xg_val = data[6];

	/* XG System parameters (addr1=0x00, addr2=0x00) */
	if(xg_addr1 == 0x00 && xg_addr2 == 0x00) {
		switch(xg_addr3) {
			case 0x00: /* Master tune (4-nibble) */
				if(len >= 10) {
					uint32_t tune = ((uint32_t)(data[6] & 0x0f) << 12) |
					                ((uint32_t)(data[7] & 0x0f) << 8) |
					                ((uint32_t)(data[8] & 0x0f) << 4) |
					                (uint32_t)(data[9] & 0x0f);
					proc->master_params.master_tuning = ((float)tune - 1024.0f) / 10.0f;
				}
				break;
			case 0x04: /* Master volume */
				ss_processor_set_midi_volume(proc, (float)xg_val / 127.0f);
				break;
			case 0x05: /* Master attenuation */
				ss_processor_set_midi_volume(proc, (float)(127 - xg_val) / 127.0f);
				break;
			case 0x06: /* Master transpose (semitones) */
				proc->master_params.master_pitch = (float)((int)xg_val - 64);
				break;
			case 0x7e: /* XG Reset */
			case 0x7f: /* XG System On */
				proc->master_params.midi_system = SS_SYSTEM_XG;
				ss_processor_system_reset(proc);
				break;
			default:
				break;
		}
		return;
	}

	/* XG Part parameters (addr1=0x08, addr2=channel) */
	if(xg_addr1 == 0x08) {
		if(proc->master_params.midi_system != SS_SYSTEM_XG) return;
		int channel_idx = (int)(xg_addr2) + channel_offset;
		if(channel_idx < 0 || channel_idx >= proc->channel_count) return;
		SS_MIDIChannel *mch = proc->midi_channels[channel_idx];

		switch(xg_addr3) {
			case 0x01: /* Bank select MSB (CC0) */
				ss_channel_controller(mch, SS_MIDCON_BANK_SELECT, (int)xg_val, t);
				break;
			case 0x02: /* Bank select LSB (CC32) */
				ss_channel_controller(mch, SS_MIDCON_BANK_SELECT_LSB, (int)xg_val, t);
				break;
			case 0x03: /* Program change */
				ss_channel_program_change(mch, (int)xg_val);
				break;
			case 0x04: /* Rx. channel */
				mch->rx_channel = (int)xg_val;
				if(mch->rx_channel != mch->channel_number)
					proc->custom_channel_numbers = true;
				break;
			case 0x05: /* Poly/mono mode */
				mch->poly_mode = (xg_val == 1);
				break;
			case 0x07: /* Part mode (0=normal, else=drum) */
				mch->drum_channel = (xg_val != 0);
				ss_processor_event_emit(proc, SS_EVENT_DRUM_CHANGE, channel_idx, (int)mch->drum_channel, 0);
				break;
			case 0x08: /* Note shift (key shift) — ignore on drum channels */
				if(!mch->drum_channel) {
					ss_channel_set_custom_controller(mch, SS_CUSTOM_CTRL_KEY_SHIFT, (float)((int)xg_val - 64));
				}
				break;
			case 0x0b: /* Volume (CC7) */
				ss_channel_controller(mch, SS_MIDCON_MAIN_VOLUME, (int)xg_val, t);
				break;
			case 0x0e: /* Pan (0=random) */
				if(xg_val == 0) {
					mch->random_pan = true;
				} else {
					mch->random_pan = false;
					ss_channel_controller(mch, SS_MIDCON_PAN, (int)xg_val, t);
				}
				break;
			case 0x12: /* Chorus send (CC93) */
				ss_channel_controller(mch, SS_MIDCON_CHORUS_DEPTH, (int)xg_val, t);
				break;
			case 0x13: /* Reverb send (CC91) */
				ss_channel_controller(mch, SS_MIDCON_REVERB_DEPTH, (int)xg_val, t);
				break;
			case 0x15: /* Vibrato rate (CC76) */
				ss_channel_controller(mch, SS_MIDCON_VIBRATO_RATE, (int)xg_val, t);
				break;
			case 0x16: /* Vibrato depth (CC77) */
				ss_channel_controller(mch, SS_MIDCON_VIBRATO_DEPTH, (int)xg_val, t);
				break;
			case 0x17: /* Vibrato delay (CC78) */
				ss_channel_controller(mch, SS_MIDCON_VIBRATO_DELAY, (int)xg_val, t);
				break;
			case 0x18: /* Filter cutoff (CC74) */
				ss_channel_controller(mch, SS_MIDCON_BRIGHTNESS, (int)xg_val, t);
				break;
			case 0x19: /* Filter resonance (CC71) */
				ss_channel_controller(mch, SS_MIDCON_FILTER_RESONANCE, (int)xg_val, t);
				break;
			case 0x1a: /* Attack time (CC73) */
				ss_channel_controller(mch, SS_MIDCON_ATTACK_TIME, (int)xg_val, t);
				break;
			case 0x1b: /* Decay time (CC75) */
				ss_channel_controller(mch, SS_MIDCON_DECAY_TIME, (int)xg_val, t);
				break;
			case 0x1c: /* Release time (CC72) */
				ss_channel_controller(mch, SS_MIDCON_RELEASE_TIME, (int)xg_val, t);
				break;
			default:
				break;
		}
		return;
	}

	/* XG Drum setup (addr1 high nibble = 3, e.g. 0x30-0x3F) */
	if((xg_addr1 >> 4) == 3) {
		if(proc->master_params.midi_system != SS_SYSTEM_XG) return;
		uint8_t drum_key = xg_addr2;
		/* Apply to all drum channels */
		for(int ci = 0; ci < proc->channel_count; ci++) {
			SS_MIDIChannel *mch = proc->midi_channels[ci];
			if(!mch || !mch->drum_channel) continue;
			switch(xg_addr3) {
				case 0x00: /* Drum pitch coarse */
					mch->drum_params[drum_key].pitch = (float)((int)xg_val - 64) * 100.0f;
					break;
				case 0x01: /* Drum pitch fine */
					mch->drum_params[drum_key].pitch += (float)((int)xg_val - 64);
					break;
				case 0x02: /* Drum level */
					mch->drum_params[drum_key].gain = (float)xg_val / 120.0f;
					break;
				case 0x03: /* Drum alternate group (exclusive class) */
					mch->drum_params[drum_key].exclusive_class = xg_val;
					break;
				case 0x04: /* Drum pan */
					mch->drum_params[drum_key].pan = xg_val;
					break;
				case 0x05: /* Drum reverb send */
					mch->drum_params[drum_key].reverb_gain = (float)xg_val / 127.0f;
					break;
				case 0x06: /* Drum chorus send */
					mch->drum_params[drum_key].chorus_gain = (float)xg_val / 127.0f;
					break;
				case 0x09: /* Receive note off */
					mch->drum_params[drum_key].rx_note_off = (xg_val == 1);
					break;
				case 0x0a: /* Receive note on */
					mch->drum_params[drum_key].rx_note_on = (xg_val == 1);
					break;
				default:
					break;
			}
		}
	}
}
