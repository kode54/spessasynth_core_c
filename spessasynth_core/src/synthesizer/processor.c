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
#include <spessasynth_core/random.h>
#else
#include "spessasynth/midi/midi_enums.h"
#include "spessasynth/synthesizer/synth.h"
#include "spessasynth/utils/random.h"
#endif

/* 1 / cos(pi/4)^2 = 2.0: corrects insertion send levels from 0-1 to 0-2 range */
extern void ss_sinc_table_init(void);

#define EFX_SENDS_GAIN_CORRECTION 2.0f

/* Smoothing factors tuned at 44 100 Hz, scaled linearly to target rate */
#define VOLENV_SMOOTHING_44K 0.01f
#define PAN_SMOOTHING_44K 0.05f
#define FILTER_SMOOTHING_44K 0.03f

extern void ss_channel_compute_modulators(SS_MIDIChannel *ch, double time);
extern void ss_channel_compute_modulators_for(SS_MIDIChannel *ch, double time,
                                              int source_uses_cc, int source_index);
extern void ss_voice_compute_modulators(SS_Voice *v, const SS_MIDIChannel *ch, double time);
extern void ss_voice_compute_modulators_for(SS_Voice *v, const SS_MIDIChannel *ch,
                                            double time, int source_uses_cc, int source_index);
extern void ss_channel_set_tuning(SS_MIDIChannel *ch, float cents);
extern void ss_channel_set_custom_controller(SS_MIDIChannel *ch, SS_CustomController type, float val);
extern void ss_processor_init_parameters(SS_Processor *proc);

void ss_processor_set_midi_volume(SS_Processor *proc, float volume);

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
	                                                  (int)proc->midi_params.system,
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
		(int)proc->midi_params.system,
		ch->drum_channel);
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
		proc->options.interpolation = SS_INTERP_HERMITE;
		proc->options.preload_all_samples = false;
		proc->options.preload_instruments = true;
	}

	/* System and MIDI parameter defaults */
	ss_processor_init_parameters(proc);

	/* MIDI volume */
	ss_processor_set_midi_volume(proc, 1.0);
	proc->pan_left = proc->pan_right = cosf(M_PI / 4.0f); /* Center */

	/* Smoothing factors — scale relative to 44 100 Hz reference */
	float sr_scale = 44100.0f / (float)sample_rate;
	proc->volume_envelope_smoothing_factor = VOLENV_SMOOTHING_44K * sr_scale;
	proc->filter_smoothing_factor = FILTER_SMOOTHING_44K * sr_scale;
	proc->pan_smoothing_factor = PAN_SMOOTHING_44K * sr_scale;

	/* Create 16 MIDI channels for each of the SS_MIDI_PORT_COUNT ports */
	for(int i = 0; i < SS_CHANNEL_COUNT * SS_MIDI_PORT_COUNT; i++) {
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

	/* Build the sinc kernel table once, before any voice can ask for it. */
	ss_sinc_table_init();

	proc->random_state = SS_RANDOM_DEFAULT_SEED;

	ss_processor_system_reset(proc);

	return proc;
}

/* Defined with the message queue, below the dispatch entry points it calls. */
static void drain_event_queue(SS_Processor *proc);

void ss_processor_free(SS_Processor *proc) {
	if(!proc) return;

	for(int i = 0; i < proc->channel_count; i++) {
		ss_channel_free(proc->midi_channels[i]);
		proc->midi_channels[i] = NULL;
	}

	for(size_t i = 0; i < proc->event_queue_count; i++)
		free(proc->event_queue[i].data);
	free(proc->event_queue);
	proc->event_queue = NULL;
	proc->event_queue_count = 0;
	proc->event_queue_allocated = 0;

	/* Channels retire their voices into the pool; free it once they're gone. */
	ss_voice_pool_free(&proc->voice_pool);

	for(size_t i = 0; i < proc->bank_group_count; i++) {
		SS_ProcessorBankGroup *g = &proc->bank_groups[i];
		ss_filtered_banks_free(g->banks, !g->external_banks);
		free(g->id);
	}
	free(proc->bank_groups);

	/* Free MTS tuning grid if allocated */
	if(proc->tunings) {
		for(int i = 0; i < 128; i++) free(proc->tunings[i]);
		free(proc->tunings);
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
	if(proc->options.preload_all_samples)
		preload_filtered_banks_samples(fbs);
	return true;
}

bool ss_processor_load_filtered_banks(SS_Processor *proc,
                                      SS_FilteredBanks *banks, const char *id,
                                      bool insert) {
	if(!proc || !banks || !id) return false;
	if(!proc_install_group(proc, banks, id, insert, /*external_banks=*/false))
		return false;
	if(proc->options.preload_all_samples)
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

void ss_processor_event_emit(SS_Processor *proc, SS_SynthEventType type,
                             int channel, int v1, int v2) {
	if(!proc->event_callback || !proc->system_params.events_enabled) return;
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

	/* Queued messages are applied against the clock of the block about to be
	 * rendered, matching upstream, which drains its queue at the top of
	 * renderAudio before any voice is touched. */
	drain_event_queue(proc);

	proc->voice_count = 0;

	double time_now = proc->current_time;

	for(int i = 0; i < proc->channel_count; i++) {
		SS_MIDIChannel *ch = proc->midi_channels[i];
		if(!ch || ch->system_params.is_muted || ch->voice_count == 0) continue;

		if(proc->system_params.effects_enabled && ch->midi_params.efx_assign &&
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

		proc->voice_count += (int)ch->voice_count;
	}

	/* Advance time */
	proc->current_time += (double)sample_count / (double)proc->sample_rate;
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
		if(proc->system_params.effects_enabled && proc->insertion_active) {
			memset(proc->insertion_left, 0, sizeof(float) * block_count);
			memset(proc->insertion_right, 0, sizeof(float) * block_count);
		}

		ss_processor_render_internal(proc, out_left, out_right, reverb, chorus, delay, block_count);

		/* Run insertion processor first: it feeds into the stereo out and effect buses */
		if(proc->system_params.effects_enabled && proc->insertion_active && proc->insertion) {
			proc->insertion->process(proc->insertion,
			                         proc->insertion_left, proc->insertion_right,
			                         out_left, out_right,
			                         reverb, chorus, delay,
			                         0, block_count);
		}

		/* These mix into the output, with the option of chorus and/or delay emitting into the reverb buffers */
		ss_chorus_process(proc->chorus, chorus, out_left, out_right, reverb, delay, block_count);
		if(proc->delay_active && proc->midi_params.system != SS_SYSTEM_XG) {
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

/* ── Message queue ───────────────────────────────────────────────────────── */

/* Apply a message now, decoding it exactly as upstream's processMessageInternal
 * does.  The typed entry points below take a timestamp of their own, which they
 * ignore for voice timing; the engine clock is the only voice clock. */
static void process_message_internal(SS_Processor *proc, const uint8_t *data,
                                     size_t length, int channel_offset) {
	if(!proc || !data || length == 0) return;

	const double t = proc->current_time;
	const uint8_t sb = data[0];

	/* SysEx keeps the existing contract: 0xF0 is stripped, other system-common
	 * status bytes are passed through whole. */
	if(sb == 0xF0) {
		if(length >= 2) ss_processor_sysex(proc, data + 1, length - 1, t);
		return;
	}
	if(sb >= 0xF1) {
		ss_processor_sysex(proc, data, length, t);
		return;
	}

	const int ch = (sb & 0x0F) + channel_offset;
	switch(sb & 0xF0) {
		case 0x80:
			if(length >= 2) ss_processor_note_off(proc, ch, data[1], t);
			break;
		case 0x90:
			if(length >= 3) {
				if(data[2] > 0)
					ss_processor_note_on(proc, ch, data[1], data[2], t);
				else
					ss_processor_note_off(proc, ch, data[1], t);
			}
			break;
		case 0xA0:
			if(length >= 3)
				ss_processor_poly_pressure(proc, ch, data[1], data[2], t);
			break;
		case 0xB0:
			if(length >= 3)
				ss_processor_control_change(proc, ch, data[1], data[2], t);
			break;
		case 0xC0:
			if(length >= 2) ss_processor_program_change(proc, ch, data[1], t);
			break;
		case 0xD0:
			if(length >= 2) ss_processor_channel_pressure(proc, ch, data[1], t);
			break;
		case 0xE0:
			if(length >= 3)
				ss_processor_pitch_wheel(proc, ch, (data[2] << 7) | data[1], -1, t);
			break;
		default:
			break;
	}
}

void ss_processor_process_message(SS_Processor *proc, const uint8_t *data,
                                  size_t length, int channel_offset,
                                  double time) {
	if(!proc || !data || length == 0) return;

	if(time <= proc->current_time) {
		process_message_internal(proc, data, length, channel_offset);
		return;
	}

	if(proc->event_queue_count == proc->event_queue_allocated) {
		size_t want = proc->event_queue_allocated ? proc->event_queue_allocated * 2 : 16;
		SS_QueuedMessage *grown = realloc(proc->event_queue, want * sizeof(*grown));
		/* Dropping a scheduled message would desynchronize the song far worse
		 * than applying it a block early, so fall back to immediate dispatch. */
		if(!grown) { process_message_internal(proc, data, length, channel_offset); return; }
		proc->event_queue = grown;
		proc->event_queue_allocated = want;
	}

	uint8_t *copy = malloc(length);
	if(!copy) { process_message_internal(proc, data, length, channel_offset); return; }
	memcpy(copy, data, length);

	/* Insert in time order.  Upstream sorts the whole queue on every push;
	 * messages arrive in time order in practice, so an insertion from the back
	 * settles in one comparison and keeps equal timestamps in arrival order. */
	size_t at = proc->event_queue_count;
	while(at > 0 && proc->event_queue[at - 1].time > time) {
		proc->event_queue[at] = proc->event_queue[at - 1];
		at--;
	}
	proc->event_queue[at].data = copy;
	proc->event_queue[at].length = length;
	proc->event_queue[at].channel_offset = channel_offset;
	proc->event_queue[at].time = time;
	proc->event_queue_count++;
}

/** Apply every queued message the engine clock has now reached. */
static void drain_event_queue(SS_Processor *proc) {
	if(proc->event_queue_count == 0) return;

	size_t done = 0;
	while(done < proc->event_queue_count &&
	      proc->event_queue[done].time <= proc->current_time) {
		SS_QueuedMessage *q = &proc->event_queue[done];
		process_message_internal(proc, q->data, q->length, q->channel_offset);
		free(q->data);
		done++;
	}
	if(done == 0) return;

	proc->event_queue_count -= done;
	if(proc->event_queue_count)
		memmove(proc->event_queue, proc->event_queue + done,
		        proc->event_queue_count * sizeof(*proc->event_queue));
}

/* ── MIDI event dispatch ─────────────────────────────────────────────────── */

/* Upstream treats a message's timestamp purely as a scheduling key:
 * processMessage() queues anything stamped later than the synthesizer clock and
 * otherwise runs it immediately, and every voice it then touches is stamped with
 * synthCore.currentTime.  The event's own time never reaches a voice.
 *
 * Threading the caller's timestamp down instead put voice clocks on the
 * sequencer's song timeline, which the block-deferred tick leaves one quantum
 * behind the block being rendered.  Anything comparing a voice time against the
 * render clock then fired a block early -- the LFO delay gates most visibly,
 * since a sub-block delay had always elapsed by the voice's first render, so
 * vibrato ran one LFO step ahead of upstream for the life of the note.
 *
 * These entry points therefore ignore their timestamp for voice timing.
 * Scheduling lives one level up, in ss_processor_process_message. */
void ss_processor_note_on(SS_Processor *proc, int ch, int note, int vel, double t) {
	if(vel == 0) {
		ss_processor_note_off(proc, ch, note, t);
		return;
	}
	ch += proc->port_select_channel_offset;
	if(ch < 0 || ch >= proc->channel_count) return;
	ss_channel_note_on(proc->midi_channels[ch], note, vel, proc->current_time);
	ss_processor_event_emit(proc, SS_EVENT_NOTE_ON, ch, note, vel);
}

void ss_processor_note_off(SS_Processor *proc, int ch, int note, double t) {
	(void)t;
	ch += proc->port_select_channel_offset;
	if(ch < 0 || ch >= proc->channel_count) return;
	ss_channel_note_off(proc->midi_channels[ch], note, proc->current_time);
	ss_processor_event_emit(proc, SS_EVENT_NOTE_OFF, ch, note, 0);
}

void ss_processor_control_change(SS_Processor *proc, int ch, int cc, int val, double t) {
	(void)t;
	ch += proc->port_select_channel_offset;
	if(ch < 0 || ch >= proc->channel_count) return;
	ss_channel_controller(proc->midi_channels[ch], cc, val, proc->current_time);
	ss_processor_event_emit(proc, SS_EVENT_CONTROLLER_CHANGE, ch, cc, val);
}

void ss_processor_program_change(SS_Processor *proc, int ch, int program, double t) {
	(void)t;
	ch += proc->port_select_channel_offset;
	if(ch < 0 || ch >= proc->channel_count) return;
	ss_channel_program_change(proc->midi_channels[ch], program);
	ss_processor_event_emit(proc, SS_EVENT_PROGRAM_CHANGE, ch, program, 0);
}

void ss_processor_pitch_wheel(SS_Processor *proc, int ch, int value, int midi_note, double t) {
	(void)t;
	ch += proc->port_select_channel_offset;
	if(ch < 0 || ch >= proc->channel_count) return;
	ss_channel_pitch_wheel(proc->midi_channels[ch], value, midi_note, proc->current_time);
	ss_processor_event_emit(proc, SS_EVENT_PITCH_WHEEL, ch, value, midi_note);
}

void ss_processor_channel_pressure(SS_Processor *proc, int ch, int pressure, double t) {
	(void)t;
	ch += proc->port_select_channel_offset;
	if(ch < 0 || ch >= proc->channel_count) return;
	/* Store channel pressure in a dedicated controller slot above CC127.
	 * 14-bit value at NON_CC_INDEX_OFFSET + SS_MODSRC_CHANNEL_PRESSURE. */
	SS_MIDIChannel *mch = proc->midi_channels[ch];
	mch->midi_controllers[NON_CC_INDEX_OFFSET + SS_MODSRC_CHANNEL_PRESSURE] = (int16_t)(pressure << 7);
	ss_channel_compute_modulators_for(mch, proc->current_time, 0,
	                                  SS_MODSRC_CHANNEL_PRESSURE);
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
			ss_voice_compute_modulators_for(mch->voices[i], mch, proc->current_time,
			                                0, SS_MODSRC_POLY_PRESSURE);
		}
	}
}

/* ── System reset ────────────────────────────────────────────────────────── */

void ss_processor_update_active_effects(SS_Processor *proc) {
	if(!proc) return;

	if(!proc->system_params.insertion_effect_lock) {
		bool any_efx = false;
		for(int i = 0; i < proc->channel_count; i++) {
			const SS_MIDIChannel *ch = proc->midi_channels[i];
			if(ch && ch->midi_params.efx_assign) { any_efx = true; break; }
		}
		proc->insertion_active = any_efx;
	}

	if(!proc->system_params.delay_lock) {
		bool active = false;
		if(proc->midi_params.system != SS_SYSTEM_XG) {
			active = (proc->chorus && proc->chorus->delayGain > 0.0f) ||
			         (proc->insertion && proc->insertion->send_level_to_delay > 0.0f);
			for(int i = 0; !active && i < proc->channel_count; i++) {
				const SS_MIDIChannel *ch = proc->midi_channels[i];
				if(ch && ch->midi_controllers[SS_MIDCON_VARIATION_DEPTH] > 0)
					active = true;
			}
		}
		proc->delay_active = active;
	}
}

void ss_processor_system_reset(SS_Processor *proc) {
	if(!proc) return;

	/* Global state first, channels after.  A channel reset reads the MIDI
	 * system to pick its drum map, its portamento default and its default
	 * bank, so resetting channels before the system would have them adopt
	 * the outgoing mode.  Upstream orders it the same way. */
	ss_processor_set_midi_parameter(proc, SS_GLOBAL_MIDI_SYSTEM,
	                                (double)SS_SYSTEM_GS);
	ss_processor_set_midi_volume(proc, 1.0);
	ss_processor_set_midi_parameter(proc, SS_GLOBAL_MIDI_PAN, 0.0);
	ss_processor_set_midi_parameter(proc, SS_GLOBAL_MIDI_KEY_SHIFT, 0.0);
	ss_processor_set_midi_parameter(proc, SS_GLOBAL_MIDI_FINE_TUNE, 0.0);

	/* Drop any MIDI Tuning Standard grid back to "no change". */
	if(proc->tunings) {
		for(int i = 0; i < 128; i++) {
			if(!proc->tunings[i]) continue;
			for(int n = 0; n < 128; n++) {
				proc->tunings[i][n].midi_note = -1;
				proc->tunings[i][n].cent_tuning = 0.0f;
			}
		}
	}

	proc->port_select_channel_offset = 0;

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
		SS_MIDIChannel *ch = proc->midi_channels[i];
		if(!ch) continue;
		ch->midi_params.efx_assign = false;
		ss_channel_all_sound_off(ch);
		ss_channel_reset(ch);
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

	ss_processor_update_active_effects(proc);

	ss_processor_event_emit(proc, SS_EVENT_STOP_ALL, -1, 0, 0);
}

void ss_processor_set_midi_volume(SS_Processor *proc, float volume) {
	/* GM2 specification, section 4.1: volume is squared.
	 * Though, according to my own testing, Math.E seems like a better choice.
	 * The curved value is stored as the global MIDI gain parameter.
	 */
	ss_processor_set_midi_parameter(proc, SS_GLOBAL_MIDI_GAIN,
	                                powf(volume, (float)M_E));
}
