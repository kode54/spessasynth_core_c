/**
 * processor.c
 * Core synthesis engine — SS_Processor.
 * Port of processor.ts.
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>
#if __has_include(<spessasynth_core/spessasynth.h>)
#include <spessasynth_core/synth.h>
#else
#include "spessasynth/synthesizer/synth.h"
#endif

/* Smoothing factors tuned at 44 100 Hz, scaled linearly to target rate */
#define VOLENV_SMOOTHING_44K 0.01f
#define PAN_SMOOTHING_44K 0.05f
#define FILTER_SMOOTHING_44K 0.1f

extern void ss_channel_compute_modulators(SS_MIDIChannel *ch, double time);
extern void ss_voice_compute_modulators(SS_Voice *v, const SS_MIDIChannel *ch, double time);

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static SS_BasicPreset *find_preset_all_banks(SS_Processor *proc,
                                             uint8_t program,
                                             uint16_t bank_msb,
                                             uint16_t bank_lsb,
                                             bool is_drum) {
	for(int b = 0; b < proc->soundbank_count; b++) {
		SS_SoundBank *bank = proc->soundbanks[b];
		if(!bank) continue;
		SS_BasicPreset *p = ss_soundbank_find_preset(bank, program,
		                                             bank_msb, bank_lsb,
		                                             (int)proc->master_params.midi_system,
		                                             is_drum);
		if(p) return p;
	}
	return NULL;
}

/** Re-resolve preset for every channel after a soundbank or system change. */
static void proc_refresh_presets(SS_Processor *proc) {
	for(int i = 0; i < proc->channel_count; i++) {
		SS_MIDIChannel *ch = proc->midi_channels[i];
		if(!ch) continue;
		SS_BasicPreset *p = find_preset_all_banks(proc,
		                                          ch->program,
		                                          ch->bank_msb,
		                                          ch->bank_lsb,
		                                          ch->drum_channel || ch->is_gm_gs_drum);
		if(p) ch->preset = p;
	}
}

/* ── Create / free ───────────────────────────────────────────────────────── */

SS_Processor *ss_processor_create(uint32_t sample_rate,
                                  const SS_ProcessorOptions *opts) {
	SS_Processor *proc = (SS_Processor *)calloc(1, sizeof(SS_Processor));
	if(!proc) return NULL;

	proc->sample_rate = sample_rate;

	if(opts) {
		proc->options = *opts;
	} else {
		proc->options.enable_effects = true;
		proc->options.voice_cap = 512;
		proc->options.interpolation = SS_INTERP_LINEAR;
	}

	/* Master params defaults */
	proc->master_params.master_volume = 1.0f;
	proc->master_params.master_pan = 0.0f;
	proc->master_params.master_pitch = 0.0f;
	proc->master_params.master_tuning = 0.0f;
	proc->master_params.interpolation_type = proc->options.interpolation;
	proc->master_params.midi_system = SS_SYSTEM_GS;
	proc->master_params.reverb_enabled = true;
	proc->master_params.chorus_enabled = true;

	/* Smoothing factors — scale relative to 44 100 Hz reference */
	float sr_scale = 44100.0f / (float)sample_rate;
	proc->volume_envelope_smoothing_factor = VOLENV_SMOOTHING_44K * sr_scale;
	proc->filter_smoothing_factor = FILTER_SMOOTHING_44K * sr_scale;
	proc->pan_smoothing_factor = PAN_SMOOTHING_44K * sr_scale;

	/* Create default 16 MIDI channels for 3 ports */
	for(int i = 0; i < SS_CHANNEL_COUNT * 3; i++) {
		SS_MIDIChannel *ch = ss_channel_new(i, proc);
		if(!ch) {
			ss_processor_free(proc);
			return NULL;
		}
		proc->midi_channels[i] = ch;
		proc->channel_count++;
	}

	proc->reverb = ss_reverb_create((float)sample_rate, 512);
	proc->chorus = ss_chorus_create((float)sample_rate, 512);
	if(!proc->reverb || !proc->chorus) {
		ss_processor_free(proc);
		return NULL;
	}

	ss_processor_system_reset(proc);

	return proc;
}

void ss_processor_free(SS_Processor *proc) {
	if(!proc) return;

	for(int i = 0; i < proc->channel_count; i++) {
		ss_channel_free(proc->midi_channels[i]);
		proc->midi_channels[i] = NULL;
	}

	for(int i = 0; i < proc->soundbank_count; i++) {
		ss_soundbank_free(proc->soundbanks[i]);
		proc->soundbanks[i] = NULL;
	}

	/* Free MTS tuning grid if allocated */
	if(proc->master_params.tunings) {
		for(int i = 0; i < 128; i++) free(proc->master_params.tunings[i]);
		free(proc->master_params.tunings);
	}

	ss_reverb_free(proc->reverb);
	ss_chorus_free(proc->chorus);
	free(proc->reverb_left);
	free(proc->reverb_right);
	free(proc->chorus_left);
	free(proc->chorus_right);

	free(proc);
}

/* ── Soundbank management ────────────────────────────────────────────────── */

SS_SoundBank *ss_processor_get_soundbank(SS_Processor *proc, const char *id) {
	for(int i = 0; i < proc->soundbank_count; i++) {
		if(strcmp(proc->soundbank_ids[i], id) == 0)
			return proc->soundbanks[i];
	}
	return NULL;
}

bool ss_processor_load_soundbank(SS_Processor *proc,
                                 SS_SoundBank *bank, const char *id) {
	if(!bank || !id) return false;

	/* Replace if same ID already exists */
	for(int i = 0; i < proc->soundbank_count; i++) {
		if(strcmp(proc->soundbank_ids[i], id) == 0) {
			ss_soundbank_free(proc->soundbanks[i]);
			proc->soundbanks[i] = bank;
			proc_refresh_presets(proc);
			return true;
		}
	}

	if(proc->soundbank_count >= SS_MAX_SOUNDBANKS) return false;

	int idx = proc->soundbank_count++;
	proc->soundbanks[idx] = bank;
	strncpy(proc->soundbank_ids[idx], id, sizeof(proc->soundbank_ids[idx]) - 1);

	proc_refresh_presets(proc);
	return true;
}

bool ss_processor_remove_soundbank(SS_Processor *proc, const char *id, bool dontfree) {
	for(int i = 0; i < proc->soundbank_count; i++) {
		if(strcmp(proc->soundbank_ids[i], id) == 0) {
			if(!dontfree) ss_soundbank_free(proc->soundbanks[i]);
			/* Compact the arrays */
			int last = proc->soundbank_count - 1;
			if(i != last) {
				proc->soundbanks[i] = proc->soundbanks[last];
				memcpy(proc->soundbank_ids[i], proc->soundbank_ids[last],
				       sizeof(proc->soundbank_ids[0]));
			}
			proc->soundbanks[last] = NULL;
			proc->soundbank_count--;
			proc_refresh_presets(proc);
			return true;
		}
	}
	return false;
}

/* ── Event callback ──────────────────────────────────────────────────────── */

void ss_processor_set_event_callback(SS_Processor *proc,
                                     SS_EventCallback cb, void *userdata) {
	proc->event_callback = cb;
	proc->event_userdata = userdata;
}

static void proc_emit(SS_Processor *proc, SS_SynthEventType type,
                      int channel, int v1, int v2) {
	if(!proc->event_callback) return;
	SS_SynthEvent ev;
	ev.type = type;
	ev.channel = channel;
	ev.value1 = v1;
	ev.value2 = v2;
	proc->event_callback(&ev, proc->event_userdata);
}

/* ── Render ──────────────────────────────────────────────────────────────── */

static void ss_processor_render_internal(SS_Processor *proc,
                                         float *out_left, float *out_right,
                                         float *reverb_left, float *reverb_right,
                                         float *chorus_left, float *chorus_right,
                                         uint32_t sample_count) {
	if(!proc || sample_count == 0) return;

	/* Use silent scratch buffers when caller passes NULL for effects */
	float *rl = reverb_left, *rr = reverb_right;
	float *cl = chorus_left, *cr = chorus_right;

	proc->total_voices = 0;

	double time_now = proc->current_synth_time;

	for(int i = 0; i < proc->channel_count; i++) {
		SS_MIDIChannel *ch = proc->midi_channels[i];
		if(!ch || ch->is_muted || ch->voice_count == 0) continue;

		ss_channel_render(ch, time_now,
		                  out_left, out_right,
		                  rl, rr, cl, cr,
		                  sample_count);

		proc->total_voices += (int)ch->voice_count;
	}

	/* Apply master volume */
	float mv = proc->master_params.master_volume;
	if(mv != 1.0f) {
		for(uint32_t i = 0; i < sample_count; i++) {
			out_left[i] *= mv;
			out_right[i] *= mv;
		}
	}

	/* Advance time */
	proc->current_synth_time += (double)sample_count / (double)proc->sample_rate;
}

void ss_processor_render(SS_Processor *proc,
                         float *out_left, float *out_right,
                         uint32_t sample_count) {
	if(!proc || sample_count == 0) return;

	memset(out_left, 0, sizeof(float) * sample_count);
	memset(out_right, 0, sizeof(float) * sample_count);

	float *reverb_left, *reverb_right;
	float *chorus_left, *chorus_right;

	if(proc->effects_allocated < 512) {
		reverb_left = (float *)realloc(proc->reverb_left, sizeof(float) * 512);
		if(!reverb_left) return;
		proc->reverb_left = reverb_left;
		reverb_right = (float *)realloc(proc->reverb_right, sizeof(float) * 512);
		if(!reverb_right) return;
		proc->reverb_right = reverb_right;
		chorus_left = (float *)realloc(proc->chorus_left, sizeof(float) * 512);
		if(!chorus_left) return;
		proc->chorus_left = chorus_left;
		chorus_right = (float *)realloc(proc->chorus_right, sizeof(float) * 512);
		if(!chorus_right) return;
		proc->chorus_right = chorus_right;
		proc->effects_allocated = 512;
	} else {
		reverb_left = proc->reverb_left;
		reverb_right = proc->reverb_right;
		chorus_left = proc->chorus_left;
		chorus_right = proc->chorus_right;
	}

	while(sample_count) {
		const int block_count = sample_count > 512 ? 512 : sample_count;
		sample_count -= block_count;

		memset(reverb_left, 0, sizeof(float) * block_count);
		memset(reverb_right, 0, sizeof(float) * block_count);
		memset(chorus_left, 0, sizeof(float) * block_count);
		memset(chorus_right, 0, sizeof(float) * block_count);

		ss_processor_render_internal(proc, out_left, out_right, reverb_left, reverb_right, chorus_left, chorus_right, block_count);

		/* These mix into the output, with the option of chorus emitting into the reverb buffers */
		ss_chorus_process(proc->chorus, chorus_left, chorus_right, out_left, out_right, reverb_left, reverb_right, NULL, NULL, block_count);
		ss_reverb_process(proc->reverb, reverb_left, reverb_right, out_left, out_right, block_count);

		out_left += block_count;
		out_right += block_count;
	}
}

/* ── MIDI event dispatch ─────────────────────────────────────────────────── */

void ss_processor_note_on(SS_Processor *proc, int ch, int note, int vel, double t) {
	if(ch < 0 || ch >= proc->channel_count) return;
	if(vel == 0) {
		ss_processor_note_off(proc, ch, note, t);
		return;
	}
	ss_channel_note_on(proc->midi_channels[ch], note, vel, t);
	proc_emit(proc, SS_EVENT_NOTE_ON, ch, note, vel);
}

void ss_processor_note_off(SS_Processor *proc, int ch, int note, double t) {
	if(ch < 0 || ch >= proc->channel_count) return;
	ss_channel_note_off(proc->midi_channels[ch], note, t);
	proc_emit(proc, SS_EVENT_NOTE_OFF, ch, note, 0);
}

void ss_processor_control_change(SS_Processor *proc, int ch, int cc, int val, double t) {
	if(ch < 0 || ch >= proc->channel_count) return;
	ss_channel_controller(proc->midi_channels[ch], cc, val, t);
	proc_emit(proc, SS_EVENT_CONTROLLER_CHANGE, ch, cc, val);
}

void ss_processor_program_change(SS_Processor *proc, int ch, int program, double t) {
	(void)t;
	if(ch < 0 || ch >= proc->channel_count) return;
	ss_channel_program_change(proc->midi_channels[ch], program);
	proc_emit(proc, SS_EVENT_PROGRAM_CHANGE, ch, program, 0);
}

void ss_processor_pitch_wheel(SS_Processor *proc, int ch, int value, double t) {
	(void)t;
	if(ch < 0 || ch >= proc->channel_count) return;
	ss_channel_pitch_wheel(proc->midi_channels[ch], value, t);
	proc_emit(proc, SS_EVENT_PITCH_WHEEL, ch, value, 0);
}

void ss_processor_channel_pressure(SS_Processor *proc, int ch, int pressure, double t) {
	(void)t;
	if(ch < 0 || ch >= proc->channel_count) return;
	/* Store channel pressure in a dedicated controller slot above CC127.
	 * Our midi_channel uses index 128 (NON_CC_INDEX_OFFSET + channelPressure). */
	SS_MIDIChannel *mch = proc->midi_channels[ch];
	mch->midi_controllers[128] = (int16_t)pressure;
	ss_channel_compute_modulators(mch, t);
}

void ss_processor_poly_pressure(SS_Processor *proc, int ch, int note, int pressure, double t) {
	(void)t;
	if(ch < 0 || ch >= proc->channel_count) return;
	/* Apply poly pressure to matching voices */
	SS_MIDIChannel *mch = proc->midi_channels[ch];
	for(size_t i = 0; i < mch->voice_count; i++) {
		if(mch->voices[i]->midi_note == note) {
			mch->voices[i]->pressure = pressure;
			ss_voice_compute_modulators(mch->voices[i], mch, t);
		}
	}
}

/* ── System Exclusive ────────────────────────────────────────────────────── */

void ss_processor_sysex(SS_Processor *proc, const uint8_t *data, size_t len, double t) {
	(void)t;
	if(!data || len < 3) return;

	uint8_t manufacturer = data[0];

	switch(manufacturer) {
		/* ── Universal Non-Realtime / Realtime ────────────────────────────── */
		case 0x7e: /* Non-realtime */
		case 0x7f: /* Realtime     */
		{
			if(len < 4) break;
			/* data[1] = device ID (0x7f = all), data[2] = sub-ID1 */
			switch(data[2]) {
				case 0x04: /* Device Control */
					if(len < 6) break;
					switch(data[3]) {
						case 0x01: { /* Master Volume */
							uint16_t vol = (uint16_t)((data[5] << 7) | data[4]);
							proc->master_params.master_volume =
							powf((float)vol / 16384.0f, (float)M_E);
							break;
						}
						case 0x02: { /* Master Balance / Pan */
							uint16_t bal = (uint16_t)((data[5] << 7) | data[4]);
							proc->master_params.master_pan =
							((float)bal - 8192.0f) / 8192.0f;
							break;
						}
						case 0x03: { /* Fine Tuning */
							if(len < 7) break;
							int raw = (int)(((data[5] << 7) | data[6]) - 8192);
							proc->master_params.master_tuning =
							(float)(raw / 81.92);
							break;
						}
						case 0x04: { /* Coarse Tuning */
							int semitones = (int)data[5] - 64;
							proc->master_params.master_tuning =
							(float)(semitones * 100);
							break;
						}
					}
					break;

				case 0x09: /* GM system */
					if(len < 4) break;
					if(data[3] == 0x01) {
						proc->master_params.midi_system = SS_SYSTEM_GM;
					} else if(data[3] == 0x03) {
						proc->master_params.midi_system = SS_SYSTEM_GM;
					} else {
						proc->master_params.midi_system = SS_SYSTEM_GS;
					}
					ss_processor_system_reset(proc);
					break;

				case 0x08: /* MIDI Tuning Standard */
				{
					if(len < 5) break;
					size_t idx = 4;
					switch(data[3]) {
						case 0x01: { /* Bulk Tuning Dump */
							if(len < 384 + 4) break;
							int program = (int)data[idx++];
							/* skip 16-byte name */
							idx += 16;
							/* Ensure tuning grid exists */
							if(!proc->master_params.tunings) {
								proc->master_params.tunings =
								(SS_TuningEntry **)calloc(128, sizeof(SS_TuningEntry *));
							}
							if(!proc->master_params.tunings[program]) {
								proc->master_params.tunings[program] =
								(SS_TuningEntry *)calloc(128, sizeof(SS_TuningEntry));
							}
							for(int n = 0; n < 128 && idx + 2 < len; n++) {
								uint8_t b1 = data[idx++];
								uint8_t b2 = data[idx++];
								uint8_t b3 = data[idx++];
								if(b1 == 0x7f && b2 == 0x7f && b3 == 0x7f) {
									proc->master_params.tunings[program][n].midi_note = n;
									proc->master_params.tunings[program][n].cent_tuning = 0.0f;
								} else {
									int fraction = (b2 << 7) | b3;
									proc->master_params.tunings[program][n].midi_note = (int)b1;
									proc->master_params.tunings[program][n].cent_tuning =
									(float)(fraction * 0.0061);
								}
							}
							break;
						}
						case 0x02: { /* Single Note Tuning Change */
							if(len < 6) break;
							int program = (int)data[idx++];
							int num_notes = (int)data[idx++];
							if(!proc->master_params.tunings) {
								proc->master_params.tunings =
								(SS_TuningEntry **)calloc(128, sizeof(SS_TuningEntry *));
							}
							if(!proc->master_params.tunings[program]) {
								proc->master_params.tunings[program] =
								(SS_TuningEntry *)calloc(128, sizeof(SS_TuningEntry));
							}
							for(int ni = 0; ni < num_notes && idx + 3 <= len; ni++) {
								uint8_t key = data[idx++];
								uint8_t b1 = data[idx++];
								uint8_t b2 = data[idx++];
								uint8_t b3 = data[idx++];
								if(b1 == 0x7f && b2 == 0x7f && b3 == 0x7f) continue;
								int fraction = (b2 << 7) | b3;
								proc->master_params.tunings[program][key].midi_note = (int)b1;
								proc->master_params.tunings[program][key].cent_tuning =
								(float)(fraction * 0.0061);
							}
							break;
						}
					}
					break;
				}
			}
			break;
		}

		/* ── Roland GS ────────────────────────────────────────────────────── */
		case 0x41: {
			/*  data[0]=0x41, data[1]=device_id, data[2]=0x42 (GS),
			    data[3]=0x12 (data set 1), data[4..6]=address, data[7+]=payload */
			if(len < 8) break;
			if(data[2] != 0x42 || data[3] != 0x12) break;

			uint32_t addr = ((uint32_t)data[4] << 16) |
			                ((uint32_t)data[5] << 8) |
			                (uint32_t)data[6];

			/* GS Reset */
			if(addr == 0x40007f && data[7] == 0x00) {
				proc->master_params.midi_system = SS_SYSTEM_GS;
				ss_processor_system_reset(proc);
				break;
			}

			/* System Level: master volume 0x40_00_04 */
			if((addr & 0xFFFF00u) == 0x400000u) {
				uint8_t param = (uint8_t)(addr & 0xFF);
				if(param == 0x04 && len >= 8) {
					proc->master_params.master_volume =
					powf((float)data[7] / 127.0f, (float)M_E);
				} else if(param == 0x06 && len >= 8) {
					/* master key shift in semitones, signed */
					proc->master_params.master_pitch = (float)((int8_t)data[7]);
				} else if(param == 0x05 && len >= 8) {
					/* master tune: -100..+100 cents stored as 0..200 */
					proc->master_params.master_tuning = (float)data[7] - 64.0f;
				}
				break;
			}

			/* Part address: 0x40_1n_xx / 0x40_2n_xx, n = part 0-F */
			if((addr & 0xF00000u) != 0x400000u) break;

			if((addr & 0xF0F000u) == 0x400000u) {
				uint8_t addr2 = (addr >> 8) & 0xFF;
				uint8_t addr3 = (addr & 0xFF);
				uint8_t value = (data[7] > 127) ? 127 : data[7];
				switch(addr2) {
					case 0x01: {
						const bool is_reverb = addr3 >= 0x30 && addr3 <= 0x37;
						const bool is_chorus = addr3 >= 0x38 && addr3 <= 0x40;
						/* const bool is_delay = addr3 >= 0x50 && addr3 <= 0x5a; */
						/* Disable effect editing if locked */
						if(is_reverb && !proc->master_params.reverb_enabled)
							return;
						if(is_chorus && !proc->master_params.chorus_enabled)
							return;
						/*
						 0x40 - chorus to delay
						 enable delay that way
						 */
						/* proc->delay_active ||= addr3 == 0x40 && is_delay; */
						switch(addr3) {
							default:
								/* Unsupported preset */
								break;

							case 0:
								/* Patch name, ignore */
								break;

							/* Reverb */
							case 0x30: /* Reverb macro */
								ss_reverb_set_macro(proc->reverb, value);
								break;

							case 0x31: /* Reverb character */
								ss_reverb_set_character(proc->reverb, value);
								break;

							case 0x32: /* Reverb pre-LPF */
								ss_reverb_set_pre_lowpass(proc->reverb, value);
								break;

							case 0x33: /* Reverb level */
								ss_reverb_set_level(proc->reverb, value);
								break;

							case 0x34: /* Reverb time */
								ss_reverb_set_time(proc->reverb, value);
								break;

							case 0x35: /* Reverb delay feedback */
								ss_reverb_set_delay_feedback(proc->reverb, value);
								break;

							case 0x36: /* Reverb send to chorus, legacy SC-55 that's recognized by later models and unsupported. */
								break;

							case 0x37: /* Reverb pre-delay time */
								ss_reverb_set_pre_delay_time(proc->reverb, value);
								break;

							/* Chorus */
							case 0x38: /* Chorus macro */
								ss_chorus_set_macro(proc->chorus, value);
								break;

							case 0x39: /* Chorus pre-LPF */
								ss_chorus_set_pre_lowpass(proc->chorus, value);
								break;

							case 0x3a: /* Chorus level */
								ss_chorus_set_level(proc->chorus, value);
								break;

							case 0x3b: /* Chorus feedback */
								ss_chorus_set_feedback(proc->chorus, value);
								break;

							case 0x3c: /* Chorus delay */
								ss_chorus_set_delay(proc->chorus, value);
								break;

							case 0x3d: /* Chorus rate */
								ss_chorus_set_rate(proc->chorus, value);
								break;

							case 0x3e: /* Chorus depth */
								ss_chorus_set_depth(proc->chorus, value);
								break;

							case 0x3f: /* Chorus send level to reverb */
								ss_chorus_set_send_level_to_reverb(proc->chorus, value);
								break;

							case 0x40: /* Chorus send level to delay */
								ss_chorus_set_send_level_to_delay(proc->chorus, value);
								break;

#if 0
							/* Delay */
							case 0x50: /* Delay macro */
								ss_delay_set_macro(proc->delay, value);
								break;

							case 0x51: /* Delay pre-LPF */
								ss_delay_set_pre_lowpass(proc->delay, value);
								break;

							case 0x52: /* Delay time center */
								ss_delay_set_time_center(proc->delay, value);
								break;

							case 0x53: /* Delay time ratio left */
								ss_delay_set_time_ratio_left(proc->delay, value);
								break;

							case 0x54: /* Delay time ratio right */
								ss_delay_set_time_ratio_right(proc->delay, value);
								break;

							case 0x55: /* Delay level center */
								ss_delay_set_level_center(proc->delay, value);
								break;

							case 0x56: /* Delay level left */
								ss_delay_set_level_left(proc->delay, value);
								break;

							case 0x57: /* Delay level right */
								ss_delay_set_level_right(proc->delay, value);
								break;

							case 0x58: /* Delay level */
								ss_delay_set_level(proc->delay, value);
								break;

							case 0x59: /* Delay feedback */
								ss_delay_set_feedback(proc->delay, value);
								break;

							case 0x5a: /* Delay send level to reverb */
								ss_delay_set_send_level_to_reverb(proc->delay, value);
								break;
#endif
						}
					}

						/* EFX Parameter */
					case 0x03: {
						break;
					}
				}
				break;
			}

			uint8_t part_idx = (uint8_t)((addr >> 8) & 0x0F);
			/* GS maps part 10 (0-indexed) to channel 9 (drum) */
			int channel_idx = (part_idx == 9) ? 9 : (int)part_idx;
			if(channel_idx < 0 || channel_idx >= proc->channel_count) break;

			SS_MIDIChannel *mch = proc->midi_channels[channel_idx];
			uint8_t gs_param = (uint8_t)(addr & 0xFF);
			uint8_t gs_val = data[7];

			switch(gs_param) {
				case 0x02: /* Receive channel — ignore */
					break;
				case 0x11: /* Vibrato rate */
					mch->channel_vibrato.rate = (float)(gs_val - 64) * 0.15f;
					break;
				case 0x12: /* Vibrato depth */
					mch->channel_vibrato.depth = (float)(gs_val - 64) * 0.15f;
					break;
				case 0x13: /* Vibrato delay */
					mch->channel_vibrato.delay = (float)(gs_val - 64) * 0.015f;
					break;
				case 0x15: /* Drum part */
					mch->drum_channel = (gs_val != 0);
					proc_emit(proc, SS_EVENT_DRUM_CHANGE, channel_idx, mch->drum_channel, 0);
					break;
				case 0x1a: /* Pan — CC10 */
					ss_channel_controller(mch, 10, (int)gs_val, proc->current_synth_time);
					break;
				case 0x1c: /* Chorus send */
					ss_channel_controller(mch, 93, (int)gs_val, proc->current_synth_time);
					break;
				case 0x1d: /* Reverb send */
					ss_channel_controller(mch, 91, (int)gs_val, proc->current_synth_time);
					break;
			}
			break;
		}

		/* ── Yamaha XG ────────────────────────────────────────────────────── */
		case 0x43: {
			if(len < 5) break;
			/* data[1] = device (0x10 = parameter change), data[2] = model ID */
			if(data[1] != 0x10) break;

			/* XG System On: 0x43 0x10 0x4c 0x00 0x00 0x7e 0x00 */
			if(len >= 7 && data[2] == 0x4c &&
			   data[3] == 0x00 && data[4] == 0x00 && data[5] == 0x7e) {
				proc->master_params.midi_system = SS_SYSTEM_XG;
				ss_processor_system_reset(proc);
			}
			break;
		}
	}

	proc_emit(proc, SS_EVENT_SYSEX, -1, 0, 0);
}

/* ── System reset ────────────────────────────────────────────────────────── */

void ss_processor_system_reset(SS_Processor *proc) {
	if(!proc) return;

	double t = proc->current_synth_time;

	for(int i = 0; i < proc->channel_count; i++) {
		SS_MIDIChannel *ch = proc->midi_channels[i];
		if(!ch) continue;
		ss_channel_all_sound_off(ch);
		ss_channel_reset_controllers(ch);
		ch->drum_channel = (i % 16 == 9);
		/* Reset bank/program */
		ch->bank_msb = 0;
		ch->bank_lsb = 0;
		ch->program = 0;
		/* Look up the default preset */
		SS_BasicPreset *p = find_preset_all_banks(proc, 0, 0, 0,
		                                          ch->drum_channel);
		if(p) ch->preset = p;
	}

	// Hall2 default
	ss_reverb_set_macro(proc->reverb, 4);
	// Chorus3 default
	ss_chorus_set_macro(proc->chorus, 2);

	(void)t;
	proc_emit(proc, SS_EVENT_STOP_ALL, -1, 0, 0);
}
