/**
 * handle_gs.c
 * Roland GS SysEx handler
 * Port of handle_gs.ts.
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

/* ── System Exclusive: Roland GS ─────────────────────────────────────────── */

/* 1 / cos(pi/4)^2 = 2.0: corrects insertion send levels from 0-1 to 0-2 range */
#define EFX_SENDS_GAIN_CORRECTION 2.0f

/* GS: maps part index (0-15) to MIDI channel. Part 0 → ch 9 (drums), parts 1-9 → ch 0-8, parts 10-15 → ch 10-15 */
static const uint8_t GS_PART_TO_CHANNEL[16] = { 9, 0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 11, 12, 13, 14, 15 };

extern void ss_channel_set_custom_controller(SS_MIDIChannel *ch, SS_CustomController type, float val);
extern void ss_channel_set_tuning(SS_MIDIChannel *ch, float cents);
extern void ss_processor_set_midi_volume(SS_Processor *proc, float volume);

void ss_sysex_handle_gs(SS_Processor *proc, const uint8_t *syx, size_t len, double t, int channel_offset) {
	if(len < 8) return;

	/* 0x12: DT1 (Device Transmit) */
	if(syx[3] == 0x12) {
		/* Model ID */
		switch(syx[2]) {
			/* GS */
			case 0x42: {
				const uint8_t addr1 = syx[4];
				const uint8_t addr2 = syx[5];
				const uint8_t addr3 = syx[6];

				/* Sanity check */
				const uint8_t data = syx[7] > 127 ? 127 : syx[7];

				/* SYSTEM MODE SET */
				if(
				addr1 == 0 &&
				addr2 == 0 &&
				addr3 == 0x7f &&
				data == 0x00) {
					proc->master_params.midi_system = SS_SYSTEM_GS;
					ss_processor_system_reset(proc);
					return;
				}

				/* Patch parameter */
				if(addr1 == 0x40) {
					/* System parameter */
					if(addr2 == 0x00) {
						switch(addr3) {
							/* Roland GS master tune */
							case 0x00: {
								if(len < 11) return;
								const uint32_t tune = ((uint32_t)(data & 0x0f) << 12) |
								                      ((uint32_t)(syx[8] & 0x0f) << 8) |
								                      ((uint32_t)(syx[9] & 0x0f) << 4) |
								                      (syx[10] & 0x0f);
								proc->master_params.master_tuning = ((float)tune - 1024.0f) / 10.0f;
								break;
							}

							/* Roland GS master volume */
							case 0x04:
								break;

							/* Roland GS master key shift (transpose) */
							case 0x05:
								proc->master_params.master_pitch = (float)((int)data - 64);
								break;

							/* Roland master pan */
							case 0x06:
								proc->master_params.master_pan = (float)((int)data - 64) / 64.0f;
								break;

							case 0x7f:
								/* Roland mode set */
								/* GS mode */
								if(data == 0x00) {
									/* This is a GS reset */
									proc->master_params.midi_system = SS_SYSTEM_GS;
									ss_processor_system_reset(proc);
								} else if(data == 0x7f) {
									/* GS mode off */
									proc->master_params.midi_system = SS_SYSTEM_GM;
									ss_processor_system_reset(proc);
								}
								break;
						}
						return;
					}

					/* Part Parameter, Patch Common (Effects) */
					if(addr2 == 0x01) {
						const bool is_reverb = addr3 >= 0x30 && addr3 <= 0x37;
						const bool is_chorus = addr3 >= 0x38 && addr3 <= 0x40;
						const bool is_delay = addr3 >= 0x50 && addr3 <= 0x5a;
						/* Disable effect editing if locked */
						if(is_reverb && !proc->master_params.reverb_enabled)
							return;
						if(is_chorus && !proc->master_params.chorus_enabled)
							return;
						if(is_delay && !proc->master_params.delay_enabled)
							return;
						/*
						 0x40 - chorus to delay; any delay param activates delay
						 */
						if(addr3 == 0x40 || is_delay)
							proc->delay_active = true;
						switch(addr3) {
							default:
								/* Unsupported preset */
								break;

							case 0:
								/* Patch name, ignore */
								break;

							/* Reverb */
							case 0x30: /* Reverb macro */
								ss_reverb_set_macro(proc->reverb, data);
								break;

							case 0x31: /* Reverb character */
								ss_reverb_set_character(proc->reverb, data);
								break;

							case 0x32: /* Reverb pre-LPF */
								ss_reverb_set_pre_lowpass(proc->reverb, data);
								break;

							case 0x33: /* Reverb level */
								ss_reverb_set_level(proc->reverb, data);
								break;

							case 0x34: /* Reverb time */
								ss_reverb_set_time(proc->reverb, data);
								break;

							case 0x35: /* Reverb delay feedback */
								ss_reverb_set_delay_feedback(proc->reverb, data);
								break;

							case 0x36: /* Reverb send to chorus, legacy SC-55 that's recognized by later models and unsupported. */
								break;

							case 0x37: /* Reverb pre-delay time */
								ss_reverb_set_pre_delay_time(proc->reverb, data);
								break;

							/* Chorus */
							case 0x38: /* Chorus macro */
								ss_chorus_set_macro(proc->chorus, data);
								break;

							case 0x39: /* Chorus pre-LPF */
								ss_chorus_set_pre_lowpass(proc->chorus, data);
								break;

							case 0x3a: /* Chorus level */
								ss_chorus_set_level(proc->chorus, data);
								break;

							case 0x3b: /* Chorus feedback */
								ss_chorus_set_feedback(proc->chorus, data);
								break;

							case 0x3c: /* Chorus delay */
								ss_chorus_set_delay(proc->chorus, data);
								break;

							case 0x3d: /* Chorus rate */
								ss_chorus_set_rate(proc->chorus, data);
								break;

							case 0x3e: /* Chorus depth */
								ss_chorus_set_depth(proc->chorus, data);
								break;

							case 0x3f: /* Chorus send level to reverb */
								ss_chorus_set_send_level_to_reverb(proc->chorus, data);
								break;

							case 0x40: /* Chorus send level to delay */
								ss_chorus_set_send_level_to_delay(proc->chorus, data);
								break;

							/* Delay */
							case 0x50: /* Delay macro */
								ss_delay_set_macro(proc->delay, data);
								break;

							case 0x51: /* Delay pre-LPF */
								ss_delay_set_pre_lowpass(proc->delay, data);
								break;

							case 0x52: /* Delay time center */
								ss_delay_set_time_center(proc->delay, data);
								break;

							case 0x53: /* Delay time ratio left */
								ss_delay_set_time_ratio_left(proc->delay, data);
								break;

							case 0x54: /* Delay time ratio right */
								ss_delay_set_time_ratio_right(proc->delay, data);
								break;

							case 0x55: /* Delay level center */
								ss_delay_set_level_center(proc->delay, data);
								break;

							case 0x56: /* Delay level left */
								ss_delay_set_level_left(proc->delay, data);
								break;

							case 0x57: /* Delay level right */
								ss_delay_set_level_right(proc->delay, data);
								break;

							case 0x58: /* Delay level */
								ss_delay_set_level(proc->delay, data);
								break;

							case 0x59: /* Delay feedback */
								ss_delay_set_feedback(proc->delay, data);
								break;

							case 0x5a: /* Delay send level to reverb */
								ss_delay_set_send_level_to_reverb(proc->delay, data);
								break;
						}
						return;
					}

					/* EFX Parameter */
					if(addr2 == 0x03) {
						if(addr3 >= 0x03 && addr3 <= 0x16) {
							if(proc->insertion) {
								proc->insertion->set_parameter(proc->insertion, (int)addr3, (int)data);
							}
							return;
						} else if(addr3 >= 0x17 && addr3 <= 0x19) {
							if(proc->insertion) {
								const float valueNormalized = ((float)data / 127.0f) * EFX_SENDS_GAIN_CORRECTION;
								if(addr3 == 0x17)
									proc->insertion->send_level_to_reverb = valueNormalized;
								else if(addr3 == 0x18)
									proc->insertion->send_level_to_chorus = valueNormalized;
								else if(addr3 == 0x19) {
									proc->delay_active = true;
									proc->insertion->send_level_to_delay = valueNormalized;
								}
							}
							return;
						}

						if(addr3 == 0x00) {
							/* EFX Type: 16-bit MSB<<8|LSB */
							if(len < 9) return;
							uint32_t efx_type = ((uint32_t)data << 8) | (uint32_t)syx[8];
							ss_insertion_free(proc->insertion);
							proc->insertion = ss_insertion_create(efx_type, proc->sample_rate, SS_MAX_SOUND_CHUNK);
							if(!proc->insertion)
								proc->insertion = ss_insertion_create(0x0000, proc->sample_rate, SS_MAX_SOUND_CHUNK);
							if(proc->insertion) {
								proc->insertion->reset(proc->insertion);
								proc->insertion->send_level_to_reverb = (40.0f / 127.0f) * EFX_SENDS_GAIN_CORRECTION;
								proc->insertion->send_level_to_chorus = 0.0f;
								proc->insertion->send_level_to_delay = 0.0f;
							}
							return;
						}
						return;
					}

					/* Patch Parameters */
					if(addr2 >> 4 == 1) {
						/* This is an individual part (channel) parameter
						 * Determine the channel
						 * Note that: 0 means channel 9 (drums), and only then 1 means channel 0, 2 channel 1, etc.
						 * SC-88Pro manual page 196
						 */
						uint8_t efx_part = addr2 & 0x0F;
						int efx_ch = GS_PART_TO_CHANNEL[efx_part] + channel_offset;
						if(efx_ch >= 0 && efx_ch < proc->channel_count) {
							SS_MIDIChannel *mch = proc->midi_channels[efx_ch];

							switch(addr3) {
								default:
									/* This is some other GS sysex... */
									return;

								case 0x00:
									/* Tone number (program change) */
									if(len < 9) return;
									ss_channel_controller(mch, SS_MIDCON_BANK_SELECT, data, t);
									ss_channel_program_change(mch, syx[8]);
									break;

								case 0x02:
									mch->rx_channel = (data == 0x10) ? -1 : (int)data + channel_offset;
									if(mch->rx_channel != mch->channel_number)
										proc->custom_channel_numbers = true;
									break;

								case 0x13:
									/* Mono/poly */
									mch->poly_mode = data == 1;
									break;

								case 0x14:
									/* Assign mode */
									mch->assign_mode = data;
									break;

								case 0x15: {
									/* This is the Use for Drum Part sysex (multiple drums) */
									mch->drum_map = data;
									const bool is_drums = data > 0; /* If set to other than 0, is a drum channel */
									mch->drum_channel = is_drums;
									break;
								}

								case 0x16: {
									/* This is the pitch key shift sysex */
									const int key_shift = (int)data - 64;
									ss_channel_set_custom_controller(mch, SS_CUSTOM_CTRL_KEY_SHIFT, (float)key_shift);
									break;
								}

									/* Pitch offset fine in Hz is not supported so far */

								case 0x19:
									/* Part level (cc#7) */
									ss_channel_controller(mch, SS_MIDCON_MAIN_VOLUME, data, t);
									break;

								/* Pan position */
								case 0x1c: {
									/* 0 is random */
									if(data == 0) {
										mch->random_pan = true;
									} else {
										mch->random_pan = false;
										ss_channel_controller(mch, SS_MIDCON_PAN, data, t);
									}
									break;
								}

								case 0x1f: {
									/* CC1 controller number */
									mch->cc1 = data;
									break;
								}

								case 0x20: {
									// CC2 controller number
									mch->cc2 = data;
									break;
								}

								/* Chorus send */
								case 0x21: {
									ss_channel_controller(mch, SS_MIDCON_CHORUS_DEPTH, data, t);
									break;
								}

								/* Reverb send */
								case 0x22: {
									ss_channel_controller(mch, SS_MIDCON_REVERB_DEPTH, data, t);
									break;
								}

								case 0x2a: {
									// Fine tune
									// 0-16384
									if(len < 9) return;
									const int tune = (data << 7) | (syx[8] & 0x7f);
									const float tuneCents = (float)(tune - 8192) / 81.92;
									ss_channel_set_tuning(mch, tuneCents);
									break;
								}

								/* Delay send */
								case 0x2c: {
									ss_channel_controller(mch, SS_MIDCON_VARIATION_DEPTH, data, t);
									break;
								}

								case 0x30: {
									/* Vibrato rate */
									ss_channel_controller(mch, SS_MIDCON_VIBRATO_RATE, data, t);
									break;
								}

								case 0x31: {
									/* Vibrato depth */
									ss_channel_controller(mch, SS_MIDCON_VIBRATO_DEPTH, data, t);
									break;
								}

								case 0x32: {
									/* Filter cutoff
									 * It's so out of order, Roland...
									 */
									ss_channel_controller(mch, SS_MIDCON_BRIGHTNESS, data, t);
									break;
								}

								case 0x33: {
									/* Filter resonance */
									ss_channel_controller(mch, SS_MIDCON_FILTER_RESONANCE, data, t);
									break;
								}

								case 0x34: {
									/* Attack time */
									ss_channel_controller(mch, SS_MIDCON_ATTACK_TIME, data, t);
									break;
								}

								case 0x35: {
									/* Decay time */
									ss_channel_controller(mch, SS_MIDCON_DECAY_TIME, data, t);
									break;
								}

								case 0x36: {
									/* Release time */
									ss_channel_controller(mch, SS_MIDCON_RELEASE_TIME, data, t);
									break;
								}

								case 0x37: {
									/* Vibrato delay */
									ss_channel_controller(mch, SS_MIDCON_VIBRATO_DELAY, data, t);
									break;
								}

								case 0x40: {
									/* Scale tuning: up to 12 bytes */
									long tuning_bytes = len - 8; /* Data starts at 7, minus checksum */
									if(tuning_bytes < 0)
										tuning_bytes = 0;
									else if(tuning_bytes > 12)
										tuning_bytes = 12;
									/* Read the bytes */
									int8_t new_tuning[12] = { 0 };
									for(int i = 0; i < tuning_bytes; i++)
										new_tuning[i] = (int8_t)((syx[7 + i] & 0x7f) - 64);
									for(int i = 0; i < 128; i++)
										mch->channel_octave_tuning[i] = new_tuning[i % 12];
									ss_channel_set_tuning(mch, (float)((int)data - 64));
									break;
								}
							}
						}
						return;
					}

					if(addr2 >> 4 == 2) {
						/* Patch Parameter controllers */
						uint8_t part_idx = addr2 & 0x0F;
						int channel_idx = GS_PART_TO_CHANNEL[part_idx] + channel_offset;
						if(channel_idx < 0 || channel_idx >= proc->channel_count) return;

						SS_MIDIChannel *mch = proc->midi_channels[channel_idx];

						switch(addr3 & 0xf0) {
							default:
								/* Not recognized */
								break;

							case 0x00: {
								/* Modulation wheel */
								if((addr3 & 0x0f) == 0x04) {
									/* LFO1 Pitch depth
									 * Special case:
									 * If the source is a mod wheel, it's a strange way of setting the modulation depth
									 * Testcase: J-Cycle.mid (it affects gm.dls which uses LFO1 for modulation)
									 */
									const float cents = ((float)data / 127.0) * 600.0;
									mch->custom_controllers[SS_CUSTOM_CTRL_MODULATION_MULTIPLIER] = cents / 50.0;
									break;
								}
								ss_dynamic_modulator_system_setup_receiver(&mch->dms, addr3, data, SS_MIDCON_MODULATION_WHEEL, false);
								break;
							}

							case 0x10: {
								/* Pitch wheel */
								if((addr3 & 0x0f) == 0x00) {
									/* See https://github.com/spessasus/SpessaSynth/issues/154
									 * Pitch control
									 * Special case:
									 * If the source is a pitch wheel, it's a strange way of setting the pitch wheel range
									 * Testcase: th07_03.mid
									 */
									const int centeredValue = (int)data - 64;
									mch->midi_controllers[NON_CC_INDEX_OFFSET + SS_MODSRC_PITCH_WHEEL_RANGE] = centeredValue << 7;
									break;
								}
								ss_dynamic_modulator_system_setup_receiver(&mch->dms, addr3, data, NON_CC_INDEX_OFFSET + SS_MODSRC_PITCH_WHEEL, true);
								break;
							}

							case 0x20: {
								/* Channel pressure */
								ss_dynamic_modulator_system_setup_receiver(&mch->dms, addr3, data, NON_CC_INDEX_OFFSET + SS_MODSRC_CHANNEL_PRESSURE, false);
								break;
							}

							case 0x30: {
								/* Poly pressure */
								ss_dynamic_modulator_system_setup_receiver(&mch->dms, addr3, data, NON_CC_INDEX_OFFSET + SS_MODSRC_POLY_PRESSURE, false);
								break;
							}

							case 0x40: {
								/* CC1 */
								ss_dynamic_modulator_system_setup_receiver(&mch->dms, addr3, data, mch->cc1, false);
								break;
							}

							case 0x50: {
								/* CC2 */
								ss_dynamic_modulator_system_setup_receiver(&mch->dms, addr3, data, mch->cc2, false);
								break;
							}
						}
						return;
					}

					/* Patch Parameter Tone Map */
					if(addr2 >> 4 == 4) {
						// This is an individual part (channel) parameter
						// Determine the channel
						// Note that: 0 means channel 9 (drums), and only then 1 means channel 0, 2 channel 1, etc.
						// SC-88Pro manual page 196
						uint8_t part_idx = addr2 & 0x0F;
						int channel_idx = GS_PART_TO_CHANNEL[part_idx] + channel_offset;
						if(channel_idx < 0 || channel_idx >= proc->channel_count) return;

						SS_MIDIChannel *mch = proc->midi_channels[channel_idx];

						switch(addr3) {
							default:
								/* This is some other GS sysex... */
								break;

							case 0x00:
							case 0x01:
								/* Tone map number (cc32) */
								ss_channel_controller(mch, SS_MIDCON_BANK_SELECT_LSB, data, t);
								break;

							case 0x22: {
								/* EFX assign */
								const bool efx = data == 1;
								mch->insertion_enabled = efx;
								proc->insertion_active = proc->insertion_active || efx;
								break;
							}
						}
					}
					return;
				}

				/* Drum setup */
				if(addr1 == 0x41) {
					const int map = (addr2 >> 4) + 1;
					const int drum_key = addr3;
					const int param = addr2 & 0x0f;
					switch(param) {
						default:
							/* Not recognized */
							return;

						case 0x0: {
							/* Drum map name, cool! */
							break;
						}

						case 0x1: {
							/* Here it's relative to 60, not 64 like NRPN. For some reason... */
							const int pitch = (int)data - 60;
							for(int ch = 0; ch < proc->channel_count; ch++) {
								SS_MIDIChannel *mch = proc->midi_channels[ch];
								if(!mch || mch->drum_map != map) continue;
								/* Apply same thing: SC-55 uses 100 cents, SC-88 and above is 50 */
								mch->drum_params[drum_key].pitch = pitch * (mch->bank_lsb == 1 ? 100 : 50);
							}
							break;
						}

						case 0x2: {
							/* Drum level */
							const float gain = (float)data / 120.0f;
							for(int ch = 0; ch < proc->channel_count; ch++) {
								SS_MIDIChannel *mch = proc->midi_channels[ch];
								if(!mch || mch->drum_map != map) continue;
								mch->drum_params[drum_key].gain = gain;
							}
							break;
						}

						case 0x3: {
							/* Drum Assign Group (exclusive class) */
							for(int ch = 0; ch < proc->channel_count; ch++) {
								SS_MIDIChannel *mch = proc->midi_channels[ch];
								if(!mch || mch->drum_map != map) continue;
								mch->drum_params[drum_key].exclusive_class = data;
							}
							break;
						}

						case 0x4: {
							/* Pan */
							for(int ch = 0; ch < proc->channel_count; ch++) {
								SS_MIDIChannel *mch = proc->midi_channels[ch];
								if(!mch || mch->drum_map != map) continue;
								mch->drum_params[drum_key].pan = data;
							}
							break;
						}

						case 0x5: {
							/* Reverb */
							const float gain = (float)data / 127.0f;
							for(int ch = 0; ch < proc->channel_count; ch++) {
								SS_MIDIChannel *mch = proc->midi_channels[ch];
								if(!mch || mch->drum_map != map) continue;
								mch->drum_params[drum_key].reverb_gain = gain;
							}
							break;
						}

						case 0x6: {
							/* Chorus */
							const float gain = (float)data / 127.0f;
							for(int ch = 0; ch < proc->channel_count; ch++) {
								SS_MIDIChannel *mch = proc->midi_channels[ch];
								if(!mch || mch->drum_map != map) continue;
								mch->drum_params[drum_key].chorus_gain = gain;
							}
							break;
						}

						case 0x7: {
							/* Receive Note Off */
							for(int ch = 0; ch < proc->channel_count; ch++) {
								SS_MIDIChannel *mch = proc->midi_channels[ch];
								if(!mch || mch->drum_map != map) continue;
								mch->drum_params[drum_key].rx_note_off = data == 1;
							}
							break;
						}

						case 0x8: {
							/* Receive Note On */
							for(int ch = 0; ch < proc->channel_count; ch++) {
								SS_MIDIChannel *mch = proc->midi_channels[ch];
								if(!mch || mch->drum_map != map) continue;
								mch->drum_params[drum_key].rx_note_on = data == 1;
							}
							break;
						}

						case 0x9: {
							/* Delay */
							const float gain = (float)data / 127.0f;
							for(int ch = 0; ch < proc->channel_count; ch++) {
								SS_MIDIChannel *mch = proc->midi_channels[ch];
								if(!mch || mch->drum_map != map) continue;
								mch->drum_params[drum_key].delay_gain = gain;
							}
							break;
						}
					}
					return;
				}

				/* Not recognized */
				return;
			}

			case 0x45: {
				/* Display commands! */
				break;
			}

			/* Some Roland */
			case 0x16: {
				if(syx[4] == 0x10) {
					/* This is a roland master volume message */
					ss_processor_set_midi_volume(proc, (float)syx[7] / 100.0f);
					return;
				} else {
					/* Unrecognized sysex */
				}
				break;
			}
		}
	} else {
		/* Unrecognized sysex */
		return;
	}
}
