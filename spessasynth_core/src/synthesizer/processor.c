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

/* Build a flat pointer array of every SS_FilteredBank currently
 * registered, in search order.  Caller must free() *out_fbanks. */
static size_t proc_collect_fbanks(SS_Processor *proc, SS_FilteredBank ***out_fbanks) {
	size_t total = 0;
	for(size_t g = 0; g < proc->bank_group_count; g++) {
		SS_FilteredBanks *fbs = proc->bank_groups[g].banks;
		if(fbs) total += fbs->count;
	}
	if(total == 0) {
		*out_fbanks = NULL;
		return 0;
	}
	SS_FilteredBank **arr = (SS_FilteredBank **)malloc(total * sizeof(*arr));
	if(!arr) {
		*out_fbanks = NULL;
		return 0;
	}
	size_t idx = 0;
	for(size_t g = 0; g < proc->bank_group_count; g++) {
		SS_FilteredBanks *fbs = proc->bank_groups[g].banks;
		if(!fbs) continue;
		for(size_t i = 0; i < fbs->count; i++)
			arr[idx++] = &fbs->fbanks[i];
	}
	*out_fbanks = arr;
	return total;
}

SS_BasicPreset *ss_processor_resolve_preset(SS_Processor *proc,
                                            int target_channel,
                                            uint8_t program,
                                            uint16_t bank_msb,
                                            uint16_t bank_lsb,
                                            bool is_drum) {
	SS_FilteredBank **arr = NULL;
	size_t total = proc_collect_fbanks(proc, &arr);
	SS_BasicPreset *p = ss_filtered_banks_find_preset(arr, total, target_channel,
	                                                  program, bank_msb, bank_lsb,
	                                                  (int)proc->master_params.midi_system,
	                                                  is_drum);
	free(arr);
	return p;
}

static SS_BasicPreset *find_preset_all_banks(SS_Processor *proc,
                                             uint8_t program,
                                             uint16_t bank_msb,
                                             uint16_t bank_lsb,
                                             bool is_drum) {
	return ss_processor_resolve_preset(proc, -1, program, bank_msb, bank_lsb, is_drum);
}

/** Re-resolve preset for every channel after a soundbank or system change. */
static void proc_refresh_presets(SS_Processor *proc) {
	SS_FilteredBank **arr = NULL;
	size_t total = proc_collect_fbanks(proc, &arr);
	for(int i = 0; i < proc->channel_count; i++) {
		SS_MIDIChannel *ch = proc->midi_channels[i];
		if(!ch) continue;
		SS_BasicPreset *p = ss_filtered_banks_find_preset(
		arr, total, ch->channel_number,
		ch->program, ch->bank_msb, ch->bank_lsb,
		(int)proc->master_params.midi_system,
		ch->drum_channel || ch->is_gm_gs_drum);
		if(p) ch->preset = p;
	}
	free(arr);
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

	for(size_t i = 0; i < proc->bank_group_count; i++) {
		SS_ProcessorBankGroup *g = &proc->bank_groups[i];
		ss_filtered_banks_free(g->banks, !g->external_banks);
		free(g->id);
	}
	free(proc->bank_groups);

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
	for(size_t i = 0; i < proc->bank_group_count; i++) {
		if(strcmp(proc->bank_groups[i].id, id) == 0) {
			SS_FilteredBanks *fbs = proc->bank_groups[i].banks;
			if(fbs && fbs->count > 0) return fbs->fbanks[0].parent_bank;
			return NULL;
		}
	}
	return NULL;
}

/* Grow the bank_groups array by one slot and return the index of the
 * newly inserted entry (at `insert_at_head ? 0 : count`).  Returns -1
 * on OOM. */
static int proc_bank_group_reserve(SS_Processor *proc, bool insert_at_head) {
	if(proc->bank_group_count >= proc->bank_group_allocated) {
		const size_t existing = proc->bank_group_allocated;
		const size_t new_allocated = existing ? existing * 2 : 4;
		SS_ProcessorBankGroup *next = (SS_ProcessorBankGroup *)realloc(
		proc->bank_groups, new_allocated * sizeof(*next));
		if(!next) return -1;
		memset(next + existing, 0, (new_allocated - existing) * sizeof(*next));
		proc->bank_groups = next;
		proc->bank_group_allocated = new_allocated;
	}
	if(insert_at_head && proc->bank_group_count > 0) {
		for(size_t i = proc->bank_group_count; i > 0; i--)
			proc->bank_groups[i] = proc->bank_groups[i - 1];
		proc->bank_group_count++;
		return 0;
	}
	return (int)(proc->bank_group_count++);
}

/* Locate an existing group by id; returns index or -1. */
static int proc_bank_group_find(SS_Processor *proc, const char *id) {
	for(size_t i = 0; i < proc->bank_group_count; i++) {
		if(strcmp(proc->bank_groups[i].id, id) == 0) return (int)i;
	}
	return -1;
}

/* Internal: install a pre-built SS_FilteredBanks under an id.  On
 * replacement, the previous group's banks are freed respecting the
 * previous group's external_banks flag. */
static bool proc_install_group(SS_Processor *proc, SS_FilteredBanks *banks,
                               const char *id, bool insert, bool external_banks) {
	int existing = proc_bank_group_find(proc, id);
	if(existing >= 0) {
		SS_ProcessorBankGroup *g = &proc->bank_groups[existing];
		ss_filtered_banks_free(g->banks, !g->external_banks);
		g->banks = banks;
		g->external_banks = external_banks;
		proc_refresh_presets(proc);
		return true;
	}

	int idx = proc_bank_group_reserve(proc, insert);
	if(idx < 0) return false;

	SS_ProcessorBankGroup *g = &proc->bank_groups[idx];
	size_t len = strlen(id);
	g->id = (char *)malloc(len + 1);
	if(!g->id) {
		/* Roll back the reservation. */
		if(insert) {
			for(size_t i = 0; i + 1 < proc->bank_group_count; i++)
				proc->bank_groups[i] = proc->bank_groups[i + 1];
		}
		proc->bank_group_count--;
		return false;
	}
	memcpy(g->id, id, len);
	g->id[len] = '\0';
	g->banks = banks;
	g->external_banks = external_banks;

	proc_refresh_presets(proc);
	return true;
}

/* Walk every instrument zone reachable from the filtered banks' parent
 * banks and eagerly decode its per-zone sample, so the audio thread
 * never triggers a Vorbis/FLAC decode.  Parent banks are deduplicated
 * so a bank shared by multiple fbanks is only walked once.  Individual
 * decode failures are ignored — the bank is still considered loaded. */
static void preload_filtered_banks_samples(SS_FilteredBanks *fbs) {
	if(!fbs || fbs->count == 0) return;

	SS_SoundBank **seen = (SS_SoundBank **)malloc(fbs->count * sizeof(*seen));
	if(!seen) return;
	size_t seen_count = 0;

	for(size_t i = 0; i < fbs->count; i++) {
		SS_SoundBank *bank = fbs->fbanks[i].parent_bank;
		if(!bank) continue;
		bool already = false;
		for(size_t j = 0; j < seen_count; j++) {
			if(seen[j] == bank) {
				already = true;
				break;
			}
		}
		if(already) continue;
		seen[seen_count++] = bank;

		for(size_t ii = 0; ii < bank->instrument_count; ii++) {
			SS_BasicInstrument *inst = &bank->instruments[ii];
			for(size_t zi = 0; zi < inst->zone_count; zi++) {
				SS_BasicSample *s = inst->zones[zi].sample;
				if(s) ss_sample_decode(s);
			}
		}
	}

	free(seen);
}

bool ss_processor_load_soundbank(SS_Processor *proc,
                                 SS_SoundBank *bank, const char *id, int offset,
                                 bool insert) {
	if(!proc || !bank || !id) return false;

	int clamped_offset = (offset >= 0 && offset <= 65535) ? offset : 0;
	SS_FilteredBankRule rule = {
		.source_program = -1,
		.source_bank = -1,
		.destination_program = 0,
		.destination_bank = clamped_offset,
		.minimum_channel = 0,
		.channel_count = 0
	};
	SS_FilteredBanks *fbs = ss_filtered_banks_build(bank, &rule, 1);
	if(!fbs) return false;

	if(!proc_install_group(proc, fbs, id, insert, /*external_banks=*/false)) {
		/* Install failed — caller retains bank ownership (consistent with
		 * the prior API contract), so free only the filtered-preset
		 * shell, not the underlying SS_SoundBank. */
		ss_filtered_banks_free(fbs, /*free_banks=*/false);
		return false;
	}
	if(proc->options.preload_samples)
		preload_filtered_banks_samples(fbs);
	return true;
}

bool ss_processor_load_filtered_banks(SS_Processor *proc,
                                      SS_FilteredBanks *banks, const char *id,
                                      bool insert) {
	if(!proc || !banks || !id) return false;
	if(!proc_install_group(proc, banks, id, insert, /*external_banks=*/false))
		return false;
	if(proc->options.preload_samples)
		preload_filtered_banks_samples(banks);
	return true;
}

bool ss_processor_remove_soundbank(SS_Processor *proc, const char *id, bool dontfree) {
	int idx = proc_bank_group_find(proc, id);
	if(idx < 0) return false;

	SS_ProcessorBankGroup *g = &proc->bank_groups[idx];
	ss_filtered_banks_free(g->banks, !dontfree && !g->external_banks);
	free(g->id);

	size_t last = proc->bank_group_count - 1;
	if((size_t)idx != last) {
		for(size_t ii = (size_t)idx + 1; ii <= last; ii++)
			proc->bank_groups[ii - 1] = proc->bank_groups[ii];
	}
	memset(&proc->bank_groups[last], 0, sizeof(proc->bank_groups[last]));
	proc->bank_group_count--;
	proc_refresh_presets(proc);
	return true;
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

void sysex_handle_gs(SS_Processor *proc, const uint8_t *syx, size_t len, double t, int channel_offset) {
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

	proc_emit(proc, SS_EVENT_STOP_ALL, -1, 0, 0);
}

void ss_processor_set_midi_volume(SS_Processor *proc, float volume) {
	/* GM2 specification, section 4.1: volume is squared.
	 * Though, according to my own testing, Math.E seems like a better choice
	 */
	proc->midi_volume = pow(volume, M_E);
}
