/**
 * sequencer.c
 * MIDI sequencer — drives SS_Processor from an SS_MIDIFile.
 * Port of sequencer.ts + process_tick.ts + process_event.ts.
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#if __has_include(<spessasynth_core/spessasynth.h>)
#include <spessasynth_core/sequencer.h>
#else
#include "spessasynth/sequencer/sequencer.h"
#endif

/* ── Helpers ─────────────────────────────────────────────────────────────── */

/** Read 3-byte big-endian µs/beat, return BPM. */
static double read_tempo_bpm(const uint8_t *d) {
	uint32_t us = ((uint32_t)d[0] << 16) | ((uint32_t)d[1] << 8) | d[2];
	if(us == 0) us = 500000;
	return 60000000.0 / (double)us;
}

/** Compute the effective channel for a message, applying the multi-port
 *  channel offset determined by the message's source track. */
static int effective_channel(const SS_MIDIFile *midi,
                             const SS_MIDIMessage *e) {
	int ch = e->status_byte & 0x0F;
	if(!midi->is_multi_port || !midi->port_channel_offset_map) return ch;
	size_t ti = e->track_index;
	if(ti >= midi->track_count) return ch;
	int port = midi->tracks[ti].port;
	if(port < 0) return ch;
	if((size_t)port >= midi->port_channel_offset_map_count) return ch;
	return ch + midi->port_channel_offset_map[port];
}

/** Compute the effective port for a sysex message. */
static int effective_port(const SS_MIDIFile *midi,
                          const SS_MIDIMessage *e) {
	if(!midi->is_multi_port || !midi->port_channel_offset_map) return 0;
	size_t ti = e->track_index;
	if(ti >= midi->track_count) return 0;
	int port = midi->tracks[ti].port;
	if(port < 0) return 0;
	if((size_t)port >= midi->port_channel_offset_map_count) return 0;
	return midi->port_channel_offset_map[port] / 16;
}

/* ── Embedded RMIDI soundbank (load/unload into processor) ───────────────── */

#define SS_SEQ_EMBEDDED_BANK_ID "embeddedBank"

/** Parse midi->embedded_soundbank and register it with the processor. */
static void load_embedded_bank(SS_Sequencer *seq, SS_MIDIFile *midi) {
	if(!seq || !seq->proc || !midi) return;
	if(!midi->embedded_soundbank || midi->embedded_soundbank_size == 0) return;

	SS_File *bank_file = ss_file_open_from_memory(midi->embedded_soundbank,
	                                              midi->embedded_soundbank_size,
	                                              false);
	if(!bank_file) return;

	SS_SoundBank *bank = ss_soundbank_load(bank_file);
	ss_file_close(bank_file);
	if(!bank) return;

	if(!ss_processor_load_soundbank(seq->proc, bank,
	                                SS_SEQ_EMBEDDED_BANK_ID,
	                                midi->bank_offset, true)) {
		ss_soundbank_free(bank);
	}
}

/** Remove the embedded bank from the processor, freeing it. */
static void unload_embedded_bank(SS_Sequencer *seq) {
	if(!seq || !seq->proc) return;
	ss_processor_remove_soundbank(seq->proc, SS_SEQ_EMBEDDED_BANK_ID, false);
}

/** Decode status_byte → voice message type and channel.
 *  Returns false if this is a meta/sysex event. */
#if 0
static bool decode_voice(uint8_t status_byte, uint8_t *type_out, int *ch_out)
{
    if (status_byte < 0x80) return false; /* meta type */
    if (status_byte >= 0xF0) return false; /* sysex / meta */
    *type_out = status_byte & 0xF0;
    *ch_out   = status_byte & 0x0F;
    return true;
}
#endif

/* ── Song helpers ────────────────────────────────────────────────────────── */

static SS_SequencerSong *current_song(const SS_Sequencer *seq) {
	if(seq->song_count == 0) return NULL;
	size_t idx = seq->current_song_index;
	if(idx >= seq->song_count) return NULL;
	return &seq->songs[idx];
}

/** Reset all per-track event indexes to 0. */
static void song_rewind(SS_SequencerSong *song) {
	for(size_t i = 0; i < song->track_count; i++)
		song->event_indexes[i] = 0;
}

/** Find the track with the earliest next event tick.
 *  Returns -1 if all tracks exhausted. */
static int find_first_event(const SS_SequencerSong *song,
                            const SS_MIDIFile *midi) {
	int best_track = -1;
	size_t best_tick = SIZE_MAX;

	for(size_t ti = 0; ti < song->track_count; ti++) {
		size_t ei = song->event_indexes[ti];
		if(ei >= midi->tracks[ti].event_count) continue;
		size_t t = midi->tracks[ti].events[ei].ticks;
		if(t < best_tick) {
			best_tick = t;
			best_track = (int)ti;
		}
	}
	return best_track;
}

/* ── Create / free ───────────────────────────────────────────────────────── */

SS_Sequencer *ss_sequencer_create(SS_Processor *proc) {
	SS_Sequencer *seq = (SS_Sequencer *)calloc(1, sizeof(SS_Sequencer));
	if(!seq) return NULL;
	seq->proc = proc;
	seq->playback_rate = 1.0;
	seq->loop_count = 1;
	seq->fade_seconds = 7.0;
	seq->loops_played = 0;
	seq->saved_master_volume = 1.0f;
	seq->current_song_index = ~0UL;
	return seq;
}

SS_Sequencer *ss_sequencer_create_callbacks(const SS_SequencerCallbacks *cb) {
	if(!cb) return NULL;
	SS_Sequencer *seq = (SS_Sequencer *)calloc(1, sizeof(SS_Sequencer));
	if(!seq) return NULL;
	seq->proc = NULL;
	seq->callbacks = *cb;
	seq->playback_rate = 1.0;
	seq->loop_count = 1;
	seq->fade_seconds = 7.0;
	seq->loops_played = 0;
	seq->saved_master_volume = 1.0f;
	seq->current_song_index = ~0UL;
	return seq;
}

void ss_sequencer_free(SS_Sequencer *seq) {
	if(!seq) return;
	ss_sequencer_clear(seq);
	free(seq->songs);
	free(seq);
}

/* ── Song management ─────────────────────────────────────────────────────── */

bool ss_sequencer_load_midi(SS_Sequencer *seq, SS_MIDIFile *midi) {
	if(!midi) return false;

	/* Grow song array if needed */
	if(seq->song_count >= seq->song_capacity) {
		size_t nc = seq->song_capacity ? seq->song_capacity * 2 : 4;
		SS_SequencerSong *tmp = (SS_SequencerSong *)realloc(seq->songs,
		                                                    nc * sizeof(*tmp));
		if(!tmp) return false;
		seq->songs = tmp;
		seq->song_capacity = nc;
	}

	SS_SequencerSong *song = &seq->songs[seq->song_count];
	song->midi = midi;
	song->track_count = midi->track_count;
	song->event_indexes = (size_t *)calloc(midi->track_count, sizeof(size_t));
	if(!song->event_indexes) return false;
	seq->song_count++;

	if(seq->current_song_index == ~0UL) {
		seq->current_song_index = 0;
		seq->base_time = 0.0;
		seq->current_time = 0.0;
		seq->finished = false;
		seq->loops_played = 0;
		seq->fading = false;
		/* This song is now current — attach its embedded bank, if any. */
		load_embedded_bank(seq, midi);
	}

	return true;
}

void ss_sequencer_clear(SS_Sequencer *seq) {
	unload_embedded_bank(seq);
	for(size_t i = 0; i < seq->song_count; i++)
		free(seq->songs[i].event_indexes);
	seq->song_count = 0;
	seq->current_song_index = -1;
	seq->is_playing = false;
	seq->finished = true;
}

/* ── Playback control ────────────────────────────────────────────────────── */

void ss_sequencer_play(SS_Sequencer *seq) {
	seq->is_playing = true;
	seq->is_paused = false;
	seq->finished = false;
}

void ss_sequencer_pause(SS_Sequencer *seq) {
	seq->is_paused = true;
}

/* Forward declarations of the MIDI-dispatch sink helpers defined lower
 * in this file.  They're used by the fade/stop/tick paths above. */
static void dispatch_midi(SS_Sequencer *seq, const uint8_t *data,
                          size_t length, double timestamp);
static void dispatch_master_volume(SS_Sequencer *seq, float value);
static void dispatch_voice_event(SS_Sequencer *seq, const SS_MIDIFile *midi,
                                 const SS_MIDIMessage *e, double t);
static void dispatch_sysex_event(SS_Sequencer *seq, const SS_MIDIFile *midi,
                                 const SS_MIDIMessage *e, double t);
static void dispatch_reset(SS_Sequencer *seq);

/** Drop any active fade and restore the master volume the user had
 *  set before the fade started. */
static void end_fade(SS_Sequencer *seq) {
	if(!seq->fading) return;
	seq->fading = false;
	dispatch_master_volume(seq, seq->saved_master_volume);
}

void ss_sequencer_stop(SS_Sequencer *seq) {
	seq->is_playing = false;
	seq->is_paused = false;
	seq->base_time = 0.0;
	seq->current_time = 0.0;
	end_fade(seq);
	seq->loops_played = 0;
	SS_SequencerSong *song = current_song(seq);
	if(song) song_rewind(song);
	if(seq->proc) {
		for(int ch = 0; ch < seq->proc->channel_count; ch++)
			ss_channel_all_sound_off(seq->proc->midi_channels[ch]);
	}
}

void ss_sequencer_set_time(SS_Sequencer *seq, double seconds) {
	SS_SequencerSong *song = current_song(seq);
	if(!song) return;
	SS_MIDIFile *midi = song->midi;

	/* Manual seek cancels any active fade and restarts the loop counter. */
	end_fade(seq);
	seq->loops_played = 0;

	/* Rewind and replay non-note events up to target time */
	song_rewind(song);
	seq->base_time += seq->current_time - seconds;
	seq->current_time = seconds;

	/* Reset processor */
	dispatch_reset(seq);

	/* Fast-forward: process all events whose absolute time <= seconds without audio */
	/* We use the tempo map to convert ticks → seconds. */
	/* Replay just CC/program/pitch wheel/sysex events; skip notes. */
	bool done = false;
	double one_tick_sec = (midi->time_division > 0) ? (60.0 / (120.0 * (double)midi->time_division)) : (60.0 / (120.0 * 480.0));

	while(!done) {
		int ti = find_first_event(song, midi);
		if(ti < 0) {
			done = true;
			break;
		}

		size_t ei = song->event_indexes[ti];
		SS_MIDIMessage *e = &midi->tracks[ti].events[ei];
		double ev_time = ss_midi_ticks_to_seconds(midi, e->ticks);

		if(ev_time > seconds) break;

		song->event_indexes[ti]++;

		uint8_t sb = e->status_byte;

		/* Update tempo (so subsequent ticks-to-seconds conversions are right) */
		if(sb == SS_META_SET_TEMPO && e->data_length >= 3) {
			double bpm = read_tempo_bpm(e->data);
			if(midi->time_division > 0)
				one_tick_sec = 60.0 / (bpm * (double)midi->time_division);
		}

		/* Replay CC / Program / Pitch wheel / SysEx only; skip notes
		 * and pressure so the seek is silent. */
		if(sb >= 0x80 && sb < 0xF0) {
			uint8_t type = sb & 0xF0;
			if(type == 0x90 && e->data_length >= 2 && e->data[1] == 0)
				dispatch_voice_event(seq, midi, e, ev_time);
			else if(type == 0x80 || type == 0xB0 || type == 0xC0 || type == 0xE0)
				dispatch_voice_event(seq, midi, e, ev_time);
		} else if(sb == 0xF0) {
			dispatch_sysex_event(seq, midi, e, ev_time);
		}
	}

	seq->one_tick_seconds = one_tick_sec;
}

bool ss_sequencer_is_finished(const SS_Sequencer *seq) {
	return seq->finished;
}

double ss_sequencer_get_time(const SS_Sequencer *seq) {
	return seq->current_time;
}

/* ── MIDI dispatch sink ──────────────────────────────────────────────────── */

/**
 * Route a raw MIDI command to the active sink.  data must start with
 * the status byte; for SysEx it is 0xF0 followed by the payload and
 * terminating 0xF7.  length counts every byte.
 *
 * When callbacks are configured, the buffer is passed verbatim.  When
 * driving the built-in SS_Processor, 0xF0 SysExes are passed to
 * ss_processor_sysex with the leading 0xF0 stripped (the existing
 * contract), while channel voice messages are dispatched via the
 * typed ss_processor_* calls.  Other system-common status bytes
 * (0xF5 port-select and friends) are passed raw to ss_processor_sysex.
 */
static void dispatch_midi(SS_Sequencer *seq, const uint8_t *data,
                          size_t length, double timestamp) {
	if(!data || length == 0) return;

	if(seq->callbacks.midi_command) {
		seq->callbacks.midi_command(seq->callbacks.context, data, length,
		                            timestamp);
		return;
	}
	if(!seq->proc) return;

	uint8_t sb = data[0];
	if(sb == 0xF0) {
		if(length >= 2)
			ss_processor_sysex(seq->proc, data + 1, length - 1, timestamp);
		return;
	}
	if(sb >= 0xF1) {
		ss_processor_sysex(seq->proc, data, length, timestamp);
		return;
	}

	uint8_t type = sb & 0xF0;
	int ch = sb & 0x0F;
	switch(type) {
		case 0x80:
			if(length >= 2)
				ss_processor_note_off(seq->proc, ch, data[1], timestamp);
			break;
		case 0x90:
			if(length >= 3) {
				if(data[2] > 0)
					ss_processor_note_on(seq->proc, ch, data[1], data[2], timestamp);
				else
					ss_processor_note_off(seq->proc, ch, data[1], timestamp);
			}
			break;
		case 0xA0:
			if(length >= 3)
				ss_processor_poly_pressure(seq->proc, ch, data[1], data[2],
				                           timestamp);
			break;
		case 0xB0:
			if(length >= 3)
				ss_processor_control_change(seq->proc, ch, data[1], data[2],
				                            timestamp);
			break;
		case 0xC0:
			if(length >= 2)
				ss_processor_program_change(seq->proc, ch, data[1], timestamp);
			break;
		case 0xD0:
			if(length >= 2)
				ss_processor_channel_pressure(seq->proc, ch, data[1], timestamp);
			break;
		case 0xE0:
			if(length >= 3)
				ss_processor_pitch_wheel(seq->proc, ch,
				                         (data[2] << 7) | data[1], -1, timestamp);
			break;
	}
}

/** Route a master-volume change to the active sink. */
static void dispatch_master_volume(SS_Sequencer *seq, float value) {
	if(seq->callbacks.set_master_volume)
		seq->callbacks.set_master_volume(seq->callbacks.context, value);
	if(seq->proc)
		seq->proc->master_params.master_volume = value;
}

/** Build the port-select SysEx and dispatch it ahead of a voice event. */
static void dispatch_port_select(SS_Sequencer *seq, int port, double t) {
	uint8_t syx[2] = { 0xF5, (uint8_t)((port & 0x0F) + 1) };
	dispatch_midi(seq, syx, 2, t);
}

/** Dispatch a voice event (non-meta, non-SysEx) via the sink. */
static void dispatch_voice_event(SS_Sequencer *seq, const SS_MIDIFile *midi,
                                 const SS_MIDIMessage *e, double t) {
	int eff = effective_channel(midi, e);
	dispatch_port_select(seq, eff >> 4, t);
	int ch = eff & 0x0F;

	uint8_t buf[3];
	buf[0] = (uint8_t)((e->status_byte & 0xF0) | ch);
	size_t len = 1;
	for(size_t i = 0; i < e->data_length && len < 3; i++)
		buf[len++] = e->data[i];
	dispatch_midi(seq, buf, len, t);
}

/** Dispatch a SysEx event via the sink, re-prepending the 0xF0 status. */
static void dispatch_sysex_event(SS_Sequencer *seq, const SS_MIDIFile *midi,
                                 const SS_MIDIMessage *e, double t) {
	if(e->data_length == 0) return;
	int port = effective_port(midi, e);
	dispatch_port_select(seq, port, t);

	/* Our SS_MIDIMessage stores the SysEx body without the leading 0xF0,
	 * but the callback contract (and MIDI byte-stream convention) wants
	 * it.  Prepend into a temp buffer. */
	uint8_t *buf = (uint8_t *)malloc(e->data_length + 1);
	if(!buf) return;
	buf[0] = 0xF0;
	memcpy(buf + 1, e->data, e->data_length);
	dispatch_midi(seq, buf, e->data_length + 1, t);
	free(buf);
}

/* ── Process a single MIDI event ─────────────────────────────────────────── */

static void process_event(SS_Sequencer *seq, SS_MIDIFile *midi,
                          SS_MIDIMessage *e, int track_index) {
	(void)track_index;

	uint8_t sb = e->status_byte;
	double t = seq->current_time + seq->base_time;

	/* Voice event */
	if(sb >= 0x80 && sb < 0xF0) {
		dispatch_voice_event(seq, midi, e, t);
		return;
	}

	/* SysEx */
	if(sb == 0xF0) {
		dispatch_sysex_event(seq, midi, e, t);
		return;
	}

	/* Meta events */
	switch(sb) {
		case SS_META_SET_TEMPO:
			if(e->data_length >= 3 && midi->time_division > 0) {
				double bpm = read_tempo_bpm(e->data);
				seq->one_tick_seconds = 60.0 / (bpm * (double)midi->time_division);
			}
			break;

		case SS_META_MIDI_PORT:
			/* ignore — port mapping is static */
			break;

		default:
			break;
	}
}

static bool ss_sequencer_next_song(SS_Sequencer *seq) {
	/* Detach the outgoing song's embedded bank before advancing. */
	seq->current_song_index++;
	if((size_t)seq->current_song_index < seq->song_count) {
		unload_embedded_bank(seq);
		end_fade(seq);

		seq->base_time += seq->current_time;
		seq->current_time = 0.0;
		seq->one_tick_seconds = 0.0;
		seq->loops_played = 0;
		/* Attach the new current song's embedded bank, if any. */
		load_embedded_bank(seq, seq->songs[seq->current_song_index].midi);
		return true;
	}
	return false;
}

/* ── Public configuration and manual advance ─────────────────────────────── */

/* Forward decl of the fade starter. */
static void begin_fade(SS_Sequencer *seq);

void ss_sequencer_set_loop_count(SS_Sequencer *seq, int count) {
	if(!seq) return;
	seq->loop_count = count;
	if(count < 0) {
		/* Switching to infinite cancels any pending fade so the song
		 * keeps playing at full volume. */
		end_fade(seq);
		return;
	}
	/* Finite target.  If the user requested a loop count at or below the
	 * playthrough we're already on, start the fade immediately — even
	 * mid-loop, without waiting for the next loop-end marker. */
	if(count >= 1 && seq->loops_played > count && !seq->fading)
		begin_fade(seq);
}

void ss_sequencer_set_fade_seconds(SS_Sequencer *seq, double seconds) {
	if(!seq) return;
	if(seconds < 0.0) seconds = 0.0;
	seq->fade_seconds = seconds;
}

void ss_sequencer_next(SS_Sequencer *seq) {
	if(!seq) return;
	if(!ss_sequencer_next_song(seq)) {
		/* No further song to move to; let the next tick finish up. */
		seq->finished = true;
		seq->is_playing = false;
	}
}

/* ── ss_sequencer_tick ────────────────────────────────────────────────────── */

/** Kick off a post-loop fade-out at the current absolute time. */
static void begin_fade(SS_Sequencer *seq) {
	if(seq->fading) return;
	seq->fading = true;
	seq->fade_start_time = seq->base_time + seq->current_time;
	/* Remember the master volume the synth currently holds so we can
	 * fade relative to it and restore on fade-cancel.  Callback-mode
	 * has no introspection hook, so we assume the nominal 1.0. */
	seq->saved_master_volume = seq->proc ? seq->proc->master_params.master_volume : 1.0f;
}

/** Rewind the current song's event indexes to the first event at or
 *  after target_tick and recompute one_tick_seconds from the tempo map
 *  (picks the latest SET_TEMPO at tick <= target_tick across all
 *  tracks).  Does NOT call ss_processor_system_reset — any reset that
 *  needs to happen on a loop jump should be encoded in the MIDI itself
 *  within the loop range, where it will re-fire as events dispatch. */
static void loop_rewind_to_tick(SS_Sequencer *seq, size_t target_tick,
                                double prev_song_time) {
	SS_SequencerSong *song = current_song(seq);
	if(!song) return;
	SS_MIDIFile *midi = song->midi;

	/* Recompute tempo so the loop iteration starts at the right speed
	 * even when the loop body has no tempo meta at its head. */
	double one_tick_sec = (midi->time_division > 0) ? (60.0 / (120.0 * (double)midi->time_division)) : (60.0 / (120.0 * 480.0));
	size_t best_tick = 0;
	bool found = false;
	for(size_t ti = 0; ti < midi->track_count; ti++) {
		const SS_MIDITrack *t = &midi->tracks[ti];
		for(size_t ei = 0; ei < t->event_count; ei++) {
			const SS_MIDIMessage *e = &t->events[ei];
			if(e->ticks > target_tick) break;
			if(e->status_byte == SS_META_SET_TEMPO && e->data_length >= 3) {
				if(!found || e->ticks >= best_tick) {
					best_tick = e->ticks;
					if(midi->time_division > 0) {
						uint32_t us = ((uint32_t)e->data[0] << 16) |
						              ((uint32_t)e->data[1] << 8) | e->data[2];
						double bpm = (us > 0) ? (60000000.0 / (double)us) : 120.0;
						one_tick_sec = 60.0 / (bpm * (double)midi->time_division);
					}
					found = true;
				}
			}
		}
	}
	seq->one_tick_seconds = one_tick_sec;

	/* Rewind each track to the first event at or after target_tick. */
	for(size_t ti = 0; ti < song->track_count; ti++) {
		const SS_MIDITrack *t = &midi->tracks[ti];
		size_t idx = 0;
		while(idx < t->event_count && t->events[idx].ticks < target_tick) idx++;
		song->event_indexes[ti] = idx;
	}

	double new_song_time = ss_midi_ticks_to_seconds(midi, target_tick);

	/* The synthesizer has been running forward the entire time.  Fold
	 * the song-time we just skipped backwards into base_time so the
	 * absolute timestamps queued for the processor (base + current)
	 * remain monotonically non-decreasing across the jump. */
	if(prev_song_time > new_song_time)
		seq->base_time += prev_song_time - new_song_time;

	seq->current_time = new_song_time;
}

/** Ramp the master volume toward zero.  Returns true if the fade has
 *  completed (caller should advance the song). */
static bool apply_fade(SS_Sequencer *seq, double abs_time) {
	if(!seq->fading) return false;
	if(seq->fade_seconds <= 0.0) {
		dispatch_master_volume(seq, 0.0f);
		return true;
	}
	double elapsed = abs_time - seq->fade_start_time;
	if(elapsed < 0.0) elapsed = 0.0;
	double progress = elapsed / seq->fade_seconds;
	if(progress >= 1.0) {
		dispatch_master_volume(seq, 0.0f);
		return true;
	}
	float gain = (float)(1.0 - progress);
	dispatch_master_volume(seq, seq->saved_master_volume * gain);
	return false;
}

void ss_sequencer_tick(SS_Sequencer *seq, uint32_t sample_count) {
	if(!seq->is_playing || seq->is_paused || seq->finished) return;

	SS_SequencerSong *song;
try_again:
	song = current_song(seq);
	if(!song) {
		seq->finished = true;
		return;
	}
	SS_MIDIFile *midi = song->midi;

	/* Advance current_time by the rendered quantum.  Sample rate comes
	 * from the built-in processor in proc mode, or from the caller's
	 * SS_SequencerCallbacks in callback mode; fall back to 44100. */
	uint32_t sr = 0;
	if(seq->proc && seq->proc->sample_rate > 0)
		sr = seq->proc->sample_rate;
	else if(seq->callbacks.sample_rate > 0)
		sr = seq->callbacks.sample_rate;
	if(sr == 0) sr = 44100;
	double dt = (double)sample_count / (double)sr;
	dt *= seq->playback_rate;
	double target_time = seq->current_time + dt;

	/* Apply fade for this block up-front.  If the fade has run its
	 * course we advance the song immediately and retry. */
	if(seq->fading && apply_fade(seq, seq->base_time + target_time)) {
		if(ss_sequencer_next_song(seq)) goto try_again;
		seq->finished = true;
		seq->is_playing = false;
		return;
	}

	/* Seed one_tick_seconds from the MIDI tempo map if not yet set */
	if(seq->one_tick_seconds <= 0.0 && midi->time_division > 0) {
		seq->one_tick_seconds = 60.0 / (120.0 * (double)midi->time_division);
	}

	const bool has_markers = midi->loop.end > 0;
	const bool infinite = seq->loop_count < 0;

	/* Dispatch all events whose time <= target_time */
	while(1) {
		int ti = find_first_event(song, midi);
		if(ti < 0) {
			/* End of events.  Behavior depends on loop config: */
			if(infinite && !has_markers) {
				/* Infinite + no markers: loop the whole file. */
				double current_time = seq->current_time;
				loop_rewind_to_tick(seq, 0, target_time);
				target_time -= current_time;
				seq->loops_played++;
				continue;
			}
			if(seq->fading) {
				/* Let the fade timer finish even after the track runs
				 * out so we never cut off mid-fade. */
				break;
			}
			if(ss_sequencer_next_song(seq)) goto try_again;
			seq->finished = true;
			seq->is_playing = false;
			break;
		}

		size_t ei = song->event_indexes[ti];
		SS_MIDIMessage *e = &midi->tracks[ti].events[ei];
		double ev_time = (double)e->ticks * seq->one_tick_seconds;

		if(ev_time > target_time) break;

		song->event_indexes[ti]++;
		process_event(seq, midi, e, ti);

		/* Loop-jump check at the MIDI loop marker.
		 *
		 * loops_played counts upward starting at 1 on the initial pass.
		 * Each time we reach the loop-end marker we've finished one more
		 * playthrough of the loop body; if the sequencer keeps going,
		 * increment loops_played and jump.
		 *
		 * Finite loop_count: we want loops_played to reach loop_count
		 * exactly before the fade kicks in.  When incrementing would
		 * take loops_played past loop_count we first start the fade and
		 * then keep jumping — that way the music keeps sounding all the
		 * way through the fade instead of trailing into silence.
		 *
		 * loop_count <= 1: no looping at all; let the song play through.
		 * loop_count < 0:  infinite — always jump, never fade. */
		if(has_markers && e->ticks >= midi->loop.end) {
			bool do_jump = false;
			if(infinite || seq->fading) {
				/* Infinite looping, or fade-in-progress where we keep
				 * jumping so the music continues to play rather than
				 * trailing into silence after the final loop body. */
				do_jump = true;
			} else if(seq->loop_count >= 1) {
				/* Finite target.  Incrementing loops_played brings it to
				 * the count of the playthrough we're about to begin;
				 * when that hits loop_count we are starting the final
				 * iteration, which is also the fade iteration. */
				if(seq->loops_played >= seq->loop_count)
					begin_fade(seq);
				do_jump = true;
			}
			/* loop_count in {0, 1}: looping disabled; fall through and
			 * let the song play out. */
			if(do_jump) {
				seq->loops_played++;
				double loop_end_time = ss_midi_ticks_to_seconds(midi,
				                                                midi->loop.end);
				double loop_start_time = ss_midi_ticks_to_seconds(midi,
				                                                  midi->loop.start);
				loop_rewind_to_tick(seq, midi->loop.start, loop_end_time);
				target_time -= loop_end_time - loop_start_time;
				continue;
			}
		}

		/* End-of-sequence check (for tracks without a loop.end). */
		if(e->ticks >= midi->last_voice_event_tick) {
			int next_ti = find_first_event(song, midi);
			if(next_ti < 0) {
				if(infinite && !has_markers) {
					double current_time = seq->current_time;
					loop_rewind_to_tick(seq, 0, target_time);
					target_time -= current_time;
					continue;
				}
				if(seq->fading) break;
				if(ss_sequencer_next_song(seq)) goto try_again;
				seq->finished = true;
				seq->is_playing = false;
				break;
			}
		}
	}
	seq->current_time = target_time;
}

void ss_sequencer_set_synthesizer(SS_Sequencer *seq, SS_Processor *proc) {
	seq->proc = proc;
}

const uint8_t syx_reset_gm[] = { 0xF0, 0x7E, 0x7F, 0x09, 0x01, 0xF7 };

static void dispatch_reset(SS_Sequencer *seq) {
	if(seq->proc)
		ss_processor_system_reset(seq->proc);
	if(seq->callbacks.midi_command) {
		for(int i = 0; i < 4; i++) {
			dispatch_port_select(seq, i, seq->base_time);
			dispatch_midi(seq, syx_reset_gm, sizeof(syx_reset_gm), seq->base_time);
		}
	}
}

