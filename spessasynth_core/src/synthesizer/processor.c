/**
 * processor.c
 * Core synthesis engine — SS_Processor.
 * Port of processor.ts.
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

/* 1 / cos(pi/4)^2 = 2.0: corrects insertion send levels from 0-1 to 0-2 range */
#define EFX_SENDS_GAIN_CORRECTION 2.0f

/* Smoothing factors tuned at 44 100 Hz, scaled linearly to target rate */
#define VOLENV_SMOOTHING_44K 0.01f
#define PAN_SMOOTHING_44K 0.05f
#define FILTER_SMOOTHING_44K 0.03f

extern void ss_channel_compute_modulators(SS_MIDIChannel *ch, double time);
extern void ss_voice_compute_modulators(SS_Voice *v, const SS_MIDIChannel *ch, double time);
extern void ss_channel_set_tuning(SS_MIDIChannel *ch, float cents);
extern void ss_channel_set_custom_controller(SS_MIDIChannel *ch, SS_CustomController type, float val);

void ss_processor_set_midi_volume(SS_Processor *proc, float volume);

/* GS: maps part index (0-15) to MIDI channel. Part 0 → ch 9 (drums), parts 1-9 → ch 0-8, parts 10-15 → ch 10-15 */
static const int GS_PART_TO_CHANNEL[16] = { 9, 0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 11, 12, 13, 14, 15 };

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static SS_BasicPreset *find_preset_all_banks(SS_Processor *proc,
                                             uint8_t program,
                                             uint16_t bank_msb,
                                             uint16_t bank_lsb,
                                             bool is_drum) {
	for(int b = 0; b < proc->soundbank_count; b++) {
		SS_SoundBank *bank = proc->soundbanks[b];
		if(!bank) continue;
		uint16_t bank_offset = proc->soundbank_offsets[b];
		SS_BasicPreset *p = ss_soundbank_find_preset(bank, program,
		                                             bank_msb, bank_lsb, bank_offset,
		                                             (int)proc->master_params.midi_system,
		                                             is_drum, (b + 1) == proc->soundbank_count);
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
	proc->master_params.delay_gain = 1.0f;
	proc->master_params.interpolation_type = proc->options.interpolation;
	proc->master_params.midi_system = SS_SYSTEM_GS;
	proc->master_params.reverb_enabled = true;
	proc->master_params.chorus_enabled = true;
	proc->master_params.delay_enabled = true;

	/* MIDI volume */
	ss_processor_set_midi_volume(proc, 1.0);
	proc->pan_left = proc->pan_right = cos(M_PI / 4.0); /* Center */

	/* Smoothing factors — scale relative to 44 100 Hz reference */
	float sr_scale = 44100.0f / (float)sample_rate;
	proc->volume_envelope_smoothing_factor = VOLENV_SMOOTHING_44K * sr_scale;
	proc->filter_smoothing_factor = FILTER_SMOOTHING_44K * sr_scale;
	proc->pan_smoothing_factor = PAN_SMOOTHING_44K * sr_scale;

	/* Create default 16 MIDI channels for 4 ports */
	for(int i = 0; i < SS_CHANNEL_COUNT * 4; i++) {
		SS_MIDIChannel *ch = ss_channel_new(i, proc);
		if(!ch) {
			ss_processor_free(proc);
			return NULL;
		}
		proc->midi_channels[i] = ch;
		proc->channel_count++;
	}

	proc->reverb = ss_reverb_create((float)sample_rate, SS_MAX_SOUND_CHUNK);
	proc->chorus = ss_chorus_create((float)sample_rate, SS_MAX_SOUND_CHUNK);
	proc->delay = ss_delay_create((float)sample_rate, SS_MAX_SOUND_CHUNK);
	if(!proc->reverb || !proc->chorus || !proc->delay) {
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
	ss_delay_free(proc->delay);
	ss_insertion_free(proc->insertion);

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
                                 SS_SoundBank *bank, const char *id, int offset) {
	if(!bank || !id) return false;

	/* Replace if same ID already exists */
	for(int i = 0; i < proc->soundbank_count; i++) {
		if(strcmp(proc->soundbank_ids[i], id) == 0) {
			ss_soundbank_free(proc->soundbanks[i]);
			proc->soundbanks[i] = bank;
			if(offset >= 0 && offset <= 65535) {
				proc->soundbank_offsets[i] = (uint16_t)offset;
			} else {
				proc->soundbank_offsets[i] = 0;
			}
			proc_refresh_presets(proc);
			return true;
		}
	}

	if(proc->soundbank_count >= SS_MAX_SOUNDBANKS) return false;

	int idx = proc->soundbank_count++;
	proc->soundbanks[idx] = bank;
	if(offset >= 0 && offset <= 65535) {
		proc->soundbank_offsets[idx] = (uint16_t)offset;
	} else {
		proc->soundbank_offsets[idx] = 0;
	}
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
                                         float *reverb,
                                         float *chorus,
                                         float *delay,
                                         uint32_t sample_count) {
	if(!proc || sample_count == 0) return;

	/* Use silent scratch buffers when caller passes NULL for effects */
	float *rvb = reverb;
	float *chr = chorus;
	float *dly = delay;

	proc->total_voices = 0;

	double time_now = proc->current_synth_time;

	for(int i = 0; i < proc->channel_count; i++) {
		SS_MIDIChannel *ch = proc->midi_channels[i];
		if(!ch || ch->is_muted || ch->voice_count == 0) continue;

		if(proc->options.enable_effects && ch->insertion_enabled &&
		   proc->insertion_active) {
			/* Route this channel's voices into the insertion input buffers.
			 * Skip reverb/chorus/delay sends: the insertion processor handles them. */
			ss_channel_render(ch, time_now,
			                  proc->insertion_left, proc->insertion_right,
			                  NULL, NULL, NULL,
			                  sample_count);
		} else {
			ss_channel_render(ch, time_now,
			                  out_left, out_right,
			                  rvb, chr, dly,
			                  sample_count);
		}

		proc->total_voices += (int)ch->voice_count;
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

	float *reverb;
	float *chorus;
	float *delay;

	reverb = proc->reverb_buffer;
	chorus = proc->chorus_buffer;
	delay = proc->delay_buffer;

	while(sample_count) {
		const int block_count = sample_count > SS_MAX_SOUND_CHUNK ? SS_MAX_SOUND_CHUNK : sample_count;
		sample_count -= block_count;

		memset(reverb, 0, sizeof(float) * block_count);
		memset(chorus, 0, sizeof(float) * block_count);
		if(proc->delay_active) {
			memset(delay, 0, sizeof(float) * block_count);
		}
		if(proc->options.enable_effects && proc->insertion_active) {
			memset(proc->insertion_left, 0, sizeof(float) * block_count);
			memset(proc->insertion_right, 0, sizeof(float) * block_count);
		}

		ss_processor_render_internal(proc, out_left, out_right, reverb, chorus, delay, block_count);

		/* Run insertion processor first: it feeds into the stereo out and effect buses */
		if(proc->options.enable_effects && proc->insertion_active && proc->insertion) {
			proc->insertion->process(proc->insertion,
			                         proc->insertion_left, proc->insertion_right,
			                         out_left, out_right,
			                         reverb, chorus, delay,
			                         0, block_count);
		}

		/* These mix into the output, with the option of chorus and/or delay emitting into the reverb buffers */
		ss_chorus_process(proc->chorus, chorus, out_left, out_right, reverb, delay, block_count);
		if(proc->delay_active && proc->master_params.midi_system != SS_SYSTEM_XG) {
			ss_delay_process(proc->delay, delay, out_left, out_right, reverb, block_count);
		}
		ss_reverb_process(proc->reverb, reverb, out_left, out_right, block_count);

		out_left += block_count;
		out_right += block_count;
	}
}

void ss_processor_render_interleaved(SS_Processor *proc,
                                     float *out, uint32_t sample_count) {
	if(!proc || !out) return;

	float *interleave_left = proc->interleave_left;
	float *interleave_right = proc->interleave_right;

	while(sample_count) {
		uint32_t block_count = sample_count > SS_MAX_SOUND_CHUNK ? SS_MAX_SOUND_CHUNK : sample_count;
		sample_count -= block_count;

		ss_processor_render(proc, interleave_left, interleave_right, block_count);

		for(uint32_t i = 0; i < block_count; i++) {
			*out++ = interleave_left[i];
			*out++ = interleave_right[i];
		}
	}
}

/* ── MIDI event dispatch ─────────────────────────────────────────────────── */

void ss_processor_note_on(SS_Processor *proc, int ch, int note, int vel, double t) {
	if(vel == 0) {
		ss_processor_note_off(proc, ch, note, t);
		return;
	}
	ch += proc->port_select_channel_offset;
	if(ch < 0 || ch >= proc->channel_count) return;
	ss_channel_note_on(proc->midi_channels[ch], note, vel, t);
	proc_emit(proc, SS_EVENT_NOTE_ON, ch, note, vel);
}

void ss_processor_note_off(SS_Processor *proc, int ch, int note, double t) {
	ch += proc->port_select_channel_offset;
	if(ch < 0 || ch >= proc->channel_count) return;
	ss_channel_note_off(proc->midi_channels[ch], note, t);
	proc_emit(proc, SS_EVENT_NOTE_OFF, ch, note, 0);
}

void ss_processor_control_change(SS_Processor *proc, int ch, int cc, int val, double t) {
	ch += proc->port_select_channel_offset;
	if(ch < 0 || ch >= proc->channel_count) return;
	ss_channel_controller(proc->midi_channels[ch], cc, val, t);
	proc_emit(proc, SS_EVENT_CONTROLLER_CHANGE, ch, cc, val);
}

void ss_processor_program_change(SS_Processor *proc, int ch, int program, double t) {
	(void)t;
	ch += proc->port_select_channel_offset;
	if(ch < 0 || ch >= proc->channel_count) return;
	ss_channel_program_change(proc->midi_channels[ch], program);
	proc_emit(proc, SS_EVENT_PROGRAM_CHANGE, ch, program, 0);
}

void ss_processor_pitch_wheel(SS_Processor *proc, int ch, int value, int midi_note, double t) {
	(void)t;
	ch += proc->port_select_channel_offset;
	if(ch < 0 || ch >= proc->channel_count) return;
	ss_channel_pitch_wheel(proc->midi_channels[ch], value, midi_note, t);
	proc_emit(proc, SS_EVENT_PITCH_WHEEL, ch, value, midi_note);
}

void ss_processor_channel_pressure(SS_Processor *proc, int ch, int pressure, double t) {
	(void)t;
	ch += proc->port_select_channel_offset;
	if(ch < 0 || ch >= proc->channel_count) return;
	/* Store channel pressure in a dedicated controller slot above CC127.
	 * 14-bit value at NON_CC_INDEX_OFFSET + SS_MODSRC_CHANNEL_PRESSURE. */
	SS_MIDIChannel *mch = proc->midi_channels[ch];
	mch->midi_controllers[NON_CC_INDEX_OFFSET + SS_MODSRC_CHANNEL_PRESSURE] = (int16_t)(pressure << 7);
	ss_channel_compute_modulators(mch, t);
}

void ss_processor_poly_pressure(SS_Processor *proc, int ch, int note, int pressure, double t) {
	(void)t;
	ch += proc->port_select_channel_offset;
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

void sysex_handle_gm(SS_Processor *proc, const uint8_t *data, size_t len, double t, int channel_offset) {
	if(len < 4) return;
	/* data[1] = device ID (0x7f = all), data[2] = sub-ID1 */
	switch(data[2]) {
		case 0x04: /* Device Control */
			if(len < 6) break;
			switch(data[3]) {
				case 0x01: { /* Master Volume */
					uint16_t vol = (uint16_t)((data[5] << 7) | data[4]);
					ss_processor_set_midi_volume(proc, (float)vol / 16384.0);
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
}

void sysex_handle_gs(SS_Processor *proc, const uint8_t *data, size_t len, double t, int channel_offset) {
	/*  data[0]=0x41, data[1]=device_id, data[2]=0x42 (GS),
	    data[3]=0x12 (data set 1), data[4..6]=address, data[7+]=payload */
	if(len < 8) return;

	/* Some Roland */
	if(data[2] == 0x16) {
		ss_processor_set_midi_volume(proc, (float)data[5] / 100.0);
		return;
	}

	if(data[2] != 0x42 || data[3] != 0x12) return;

	uint32_t addr = ((uint32_t)data[4] << 16) |
	                ((uint32_t)data[5] << 8) |
	                (uint32_t)data[6];

	/* GS Reset */
	if(addr == 0x40007f && data[7] == 0x00) {
		proc->master_params.midi_system = SS_SYSTEM_GS;
		ss_processor_system_reset(proc);
		return;
	}

	/* System Level: 0x40_00_xx */
	if((addr & 0xFFFF00u) == 0x400000u) {
		uint8_t addr3 = (uint8_t)(addr & 0xFF);
		uint8_t val = data[7];
		switch(addr3) {
			case 0x00: /* Master tune (4-nibble BCD) */
				if(len >= 11) {
					uint32_t tune = ((uint32_t)(val & 0x0f) << 12) |
					                ((uint32_t)(data[8] & 0x0f) << 8) |
					                ((uint32_t)(data[9] & 0x0f) << 4) |
					                (uint32_t)(data[10] & 0x0f);
					proc->master_params.master_tuning = ((float)tune - 1024.0f) / 10.0f;
				}
				break;
			case 0x04: /* Master volume */
				ss_processor_set_midi_volume(proc, (float)val / 127.0f);
				break;
			case 0x05: /* Master key shift (semitones, signed) */
				proc->master_params.master_pitch = (float)((int)val - 64);
				break;
			case 0x06: /* Master pan */
				proc->master_params.master_pan = (float)((int)val - 64) / 64.0f;
				break;
			case 0x7f: /* Roland mode set: 0x7f=GS off (→GM) */
				if(val == 0x7f) {
					proc->master_params.midi_system = SS_SYSTEM_GM;
					ss_processor_system_reset(proc);
				}
				break;
			default:
				break;
		}
		return;
	}

	/* Part address: 0x40_1n_xx / 0x40_2n_xx, n = part 0-F */
	if((addr & 0xF00000u) != 0x400000u) return;

	if((addr & 0xF0F000u) == 0x400000u) {
		uint8_t addr2 = (addr >> 8) & 0xFF;
		uint8_t addr3 = (addr & 0xFF);
		uint8_t value = (data[7] > 127) ? 127 : data[7];
		switch(addr2) {
			case 0x01: {
				const bool is_reverb = addr3 >= 0x30 && addr3 <= 0x37;
				const bool is_chorus = addr3 >= 0x38 && addr3 <= 0x40;
				const bool is_delay = addr3 >= 0x50 && addr3 <= 0x5a;
				/* Disable effect editing if locked */
				if(is_reverb && !proc->master_params.reverb_enabled)
					break;
				if(is_chorus && !proc->master_params.chorus_enabled)
					break;
				if(is_delay && !proc->master_params.delay_enabled)
					break;
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
				}
				break;
			}

			/* EFX Parameter (addr2=0x03) */
			case 0x03: {
				uint8_t efx_val = (data[7] > 127) ? 127 : data[7];
				if(addr3 == 0x00) {
					/* EFX Type: 16-bit MSB<<8|LSB */
					if(len < 9) break;
					uint32_t efx_type = ((uint32_t)data[7] << 8) | (uint32_t)data[8];
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
				} else if(addr3 >= 0x03 && addr3 <= 0x16) {
					/* EFX parameters (MIDI param indices 0x03-0x16) */
					if(proc->insertion)
						proc->insertion->set_parameter(proc->insertion, (int)addr3, (int)efx_val);
				} else if(addr3 == 0x17) {
					if(proc->insertion)
						proc->insertion->send_level_to_reverb =
						((float)efx_val / 127.0f) * EFX_SENDS_GAIN_CORRECTION;
				} else if(addr3 == 0x18) {
					if(proc->insertion)
						proc->insertion->send_level_to_chorus =
						((float)efx_val / 127.0f) * EFX_SENDS_GAIN_CORRECTION;
				} else if(addr3 == 0x19) {
					proc->delay_active = true;
					if(proc->insertion)
						proc->insertion->send_level_to_delay =
						((float)efx_val / 127.0f) * EFX_SENDS_GAIN_CORRECTION;
				}
				break;
			}
		}
		return;
	}

	/* Part parameters (addr2=0x1n) and tone-map EFX assign (addr2=0x4n) */
	{
		uint8_t addr2_byte = (uint8_t)((addr >> 8) & 0xFF);
		uint8_t addr2_nibble = addr2_byte >> 4;

		/* Tone-map EFX assign: addr2=0x4n, addr3=0x22 */
		if(addr2_nibble == 4) {
			uint8_t efx_part = addr2_byte & 0x0F;
			int efx_ch = GS_PART_TO_CHANNEL[efx_part] + channel_offset;
			if(efx_ch >= 0 && efx_ch < proc->channel_count &&
			   (addr & 0xFF) == 0x22) {
				SS_MIDIChannel *efx_mch = proc->midi_channels[efx_ch];
				efx_mch->insertion_enabled = (data[7] == 1);
				if(data[7] == 1) proc->insertion_active = true;
			}
			return;
		}

		/* Patch Parameter controllers */
		if(addr2_nibble == 2) {
			/* This is an individual part (channel) parameter
			 * Determine the channel
			 * Note that: 0 means channel 9 (drums), and only then 1 means channel 0, 2 channel 1, etc.
			 * SC-88Pro manual page 196
			 */
			uint8_t part_idx = addr2_byte & 0x0F;
			int channel_idx = GS_PART_TO_CHANNEL[part_idx] + channel_offset;
			if(channel_idx < 0 || channel_idx >= proc->channel_count) return;

			SS_MIDIChannel *mch = proc->midi_channels[channel_idx];
			uint8_t gs_param = (uint8_t)(addr & 0xFF);
			uint8_t gs_val = data[7];

			switch(gs_param & 0xf0) {
				default:
					/* Not recognized */
					break;

				case 0x00: {
					/* Modulation wheel */
					if((gs_param & 0x0f) == 0x04) {
						/* LFO1 Pitch depth
						 * Special case:
						 * If the source is a mod wheel, it's a strange way of setting the modulation depth
						 * Testcase: J-Cycle.mid (it affects gm.dls which uses LFO1 for modulation)
						 */
						const float cents = ((float)gs_val / 127.0) * 600.0;
						mch->custom_controllers[SS_CUSTOM_CTRL_MODULATION_MULTIPLIER] = cents / 50.0;
						break;
					}
					ss_dynamic_modulator_system_setup_receiver(&mch->dms, gs_param, gs_val, SS_MIDCON_MODULATION_WHEEL, false);
					break;
				}

				case 0x10: {
					/* Pitch wheel */
					if((gs_param & 0x0f) == 0x00) {
						/* See https://github.com/spessasus/SpessaSynth/issues/154
						 * Pitch control
						 * Special case:
						 * If the source is a pitch wheel, it's a strange way of setting the pitch wheel range
						 * Testcase: th07_03.mid
						 */
						const int centeredValue = (int)gs_val - 64;
						mch->midi_controllers[NON_CC_INDEX_OFFSET + SS_MODSRC_PITCH_WHEEL_RANGE] = centeredValue << 7;
						break;
					}
					ss_dynamic_modulator_system_setup_receiver(&mch->dms, gs_param, gs_val, NON_CC_INDEX_OFFSET + SS_MODSRC_PITCH_WHEEL, true);
					break;
				}

				case 0x20: {
					/* Channel pressure */
					ss_dynamic_modulator_system_setup_receiver(&mch->dms, gs_param, gs_val, NON_CC_INDEX_OFFSET + SS_MODSRC_CHANNEL_PRESSURE, false);
					break;
				}

				case 0x30: {
					/* Poly pressure */
					ss_dynamic_modulator_system_setup_receiver(&mch->dms, gs_param, gs_val, NON_CC_INDEX_OFFSET + SS_MODSRC_POLY_PRESSURE, false);
					break;
				}

				case 0x40: {
					/* CC1 */
					ss_dynamic_modulator_system_setup_receiver(&mch->dms, gs_param, gs_val, mch->cc1, false);
					break;
				}

				case 0x50: {
					/* CC2 */
					ss_dynamic_modulator_system_setup_receiver(&mch->dms, gs_param, gs_val, mch->cc2, false);
					break;
				}
			}
			return;
		}

		if(addr2_nibble == 1) {
			uint8_t part_idx = addr2_byte & 0x0F;
			int channel_idx = GS_PART_TO_CHANNEL[part_idx] + channel_offset;
			if(channel_idx < 0 || channel_idx >= proc->channel_count) return;

			SS_MIDIChannel *mch = proc->midi_channels[channel_idx];
			uint8_t gs_param = (uint8_t)(addr & 0xFF);
			uint8_t gs_val = data[7];
			double t = proc->current_synth_time;

			switch(gs_param) {
				case 0x00: /* Tone number: bank select MSB + program */
					if(len < 9) break;
					ss_channel_controller(mch, SS_MIDCON_BANK_SELECT, (int)gs_val, t);
					ss_channel_program_change(mch, (int)data[8]);
					break;

				case 0x02: /* Rx. channel (0x10 = off) */
					mch->rx_channel = (gs_val == 0x10) ? -1 : (int)gs_val;
					if(mch->rx_channel != mch->channel_number)
						proc->custom_channel_numbers = true;
					break;

				case 0x13: /* Mono/poly mode */
					mch->poly_mode = (gs_val == 1);
					break;

				case 0x14: /* Assign mode */
					mch->assign_mode = gs_val;
					break;

				case 0x15: /* Use for drum part */
				{
					mch->drum_map = gs_val;
					bool is_drums = (gs_val > 0);
					if(is_drums != mch->is_gm_gs_drum) {
						mch->bank_lsb = 0;
						mch->bank_msb = 0;
						mch->is_gm_gs_drum = is_drums;
					}
					mch->drum_channel = is_drums || (mch->channel_number % 16 == 9);
					proc_emit(proc, SS_EVENT_DRUM_CHANGE, channel_idx, (int)mch->drum_channel, 0);
					break;
				}

				case 0x16: /* Pitch key shift */
					ss_channel_set_custom_controller(mch, SS_CUSTOM_CTRL_KEY_SHIFT, (float)((int)gs_val - 64));
					break;

				case 0x19: /* Part level (CC7) */
					ss_channel_controller(mch, SS_MIDCON_MAIN_VOLUME, (int)gs_val, t);
					break;

				case 0x1c: /* Pan position (0=random) */
					if(gs_val == 0) {
						mch->random_pan = true;
					} else {
						mch->random_pan = false;
						ss_channel_controller(mch, SS_MIDCON_PAN, (int)gs_val, t);
					}
					break;

				case 0x1f: /* CC1 controller number */
					mch->cc1 = gs_val;
					break;

				case 0x20: /* CC2 controller number */
					mch->cc2 = gs_val;
					break;

				case 0x21: /* Chorus send (CC93) */
					ss_channel_controller(mch, SS_MIDCON_CHORUS_DEPTH, (int)gs_val, t);
					break;

				case 0x22: /* Reverb send (CC91) */
					ss_channel_controller(mch, SS_MIDCON_REVERB_DEPTH, (int)gs_val, t);
					break;

				case 0x2a: /* Fine tune (14-bit) */
					if(len >= 9) {
						uint16_t tune_val = ((uint16_t)gs_val << 7) | (data[8] & 0x7fu);
						ss_channel_set_tuning(mch, ((float)tune_val - 8192.0f) / 81.92f);
					}
					break;

				case 0x2c: /* Delay send (CC94) */
					ss_channel_controller(mch, SS_MIDCON_VARIATION_DEPTH, (int)gs_val, t);
					break;

				case 0x30: /* Vibrato rate (CC76) */
					ss_channel_controller(mch, SS_MIDCON_VIBRATO_RATE, (int)gs_val, t);
					break;

				case 0x31: /* Vibrato depth (CC77) */
					ss_channel_controller(mch, SS_MIDCON_VIBRATO_DEPTH, (int)gs_val, t);
					break;

				case 0x32: /* Filter cutoff (CC74) */
					ss_channel_controller(mch, SS_MIDCON_BRIGHTNESS, (int)gs_val, t);
					break;

				case 0x33: /* Filter resonance (CC71) */
					ss_channel_controller(mch, SS_MIDCON_FILTER_RESONANCE, (int)gs_val, t);
					break;

				case 0x34: /* Attack time (CC73) */
					ss_channel_controller(mch, SS_MIDCON_ATTACK_TIME, (int)gs_val, t);
					break;

				case 0x35: /* Decay time (CC75) */
					ss_channel_controller(mch, SS_MIDCON_DECAY_TIME, (int)gs_val, t);
					break;

				case 0x36: /* Release time (CC72) */
					ss_channel_controller(mch, SS_MIDCON_RELEASE_TIME, (int)gs_val, t);
					break;

				case 0x37: /* Vibrato delay (CC78) */
					ss_channel_controller(mch, SS_MIDCON_VIBRATO_DELAY, (int)gs_val, t);
					break;

				case 0x40: /* Scale tuning (12 bytes, repeating for all 128 notes) */
				{
					int tuning_bytes = (int)len - 9;
					if(tuning_bytes < 0) tuning_bytes = 0;
					if(tuning_bytes > 12) tuning_bytes = 12;
					int8_t new_tuning[12] = { 0 };
					for(int i = 0; i < tuning_bytes; i++)
						new_tuning[i] = (int8_t)((data[7 + i] & 0x7f) - 64);
					for(int i = 0; i < 128; i++)
						mch->channel_octave_tuning[i] = new_tuning[i % 12];
					ss_channel_set_tuning(mch, (float)((int)gs_val - 64));
					break;
				}

				default:
					break;
			}
		}
	}
}

void sysex_handle_xg(SS_Processor *proc, const uint8_t *data, size_t len, double t, int channel_offset) {
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
				proc_emit(proc, SS_EVENT_DRUM_CHANGE, channel_idx, (int)mch->drum_channel, 0);
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

void ss_processor_sysex(SS_Processor *proc, const uint8_t *data, size_t len, double t) {
	(void)t;
	if(!data || len < 1) return;

	int channel_offset = proc->port_select_channel_offset;

	uint8_t manufacturer = data[0];

	switch(manufacturer) {
		/* ── Universal Non-Realtime / Realtime ────────────────────────────── */
		case 0x7e: /* Non-realtime */
		case 0x7f: /* Realtime     */
			sysex_handle_gm(proc, data, len, t, channel_offset);
			break;

		/* ── Roland GS ────────────────────────────────────────────────────── */
		case 0x41:
			sysex_handle_gs(proc, data, len, t, channel_offset);
			break;

		/* ── Yamaha XG ────────────────────────────────────────────────────── */
		case 0x43:
			sysex_handle_xg(proc, data, len, t, channel_offset);
			break;

		/* ── Port select (Falcosoft MIDI Player) ──────────────────────────── */
		/* https://www.vogons.org/viewtopic.php?p=1404746#p1404746 */
		case 0xf5: {
			if(len < 2) return;
			proc->port_select_channel_offset = (data[1] - 1) * 16;
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

	proc->port_select_channel_offset = 0;

	// Reset the volume
	ss_processor_set_midi_volume(proc, 1.0);

	// Reset the effects processors' buffers
	ss_reverb_clear(proc->reverb);
	ss_chorus_clear(proc->chorus);
	ss_delay_clear(proc->delay);

	// Hall2 default
	ss_reverb_set_macro(proc->reverb, 4);
	// Chorus3 default
	ss_chorus_set_macro(proc->chorus, 2);
	// Delay1 default
	ss_delay_set_macro(proc->delay, 0);
	if(proc->master_params.delay_enabled) proc->delay_active = false;

	/* Reset insertion: free old processor, create default Thru */
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
			proc->midi_channels[i]->insertion_enabled = false;
	}

	(void)t;
	proc_emit(proc, SS_EVENT_STOP_ALL, -1, 0, 0);
}

void ss_processor_set_midi_volume(SS_Processor *proc, float volume) {
	/* GM2 specification, section 4.1: volume is squared.
	 * Though, according to my own testing, Math.E seems like a better choice
	 */
	proc->midi_volume = pow(volume, M_E);
}
