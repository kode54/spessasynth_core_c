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
	int idx = seq->current_song_index;
	if(idx < 0 || (size_t)idx >= seq->song_count) return NULL;
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
	uint32_t best_tick = UINT32_MAX;

	for(size_t ti = 0; ti < song->track_count; ti++) {
		size_t ei = song->event_indexes[ti];
		if(ei >= midi->tracks[ti].event_count) continue;
		uint32_t t = midi->tracks[ti].events[ei].ticks;
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
	seq->loop_count = 0;
	seq->current_song_index = -1;
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

	if(seq->current_song_index < 0) {
		seq->current_song_index = 0;
		seq->current_time = 0.0;
		seq->finished = false;
	}

	return true;
}

void ss_sequencer_clear(SS_Sequencer *seq) {
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

void ss_sequencer_stop(SS_Sequencer *seq) {
	seq->is_playing = false;
	seq->is_paused = false;
	seq->current_time = 0.0;
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

	/* Rewind and replay non-note events up to target time */
	song_rewind(song);
	seq->current_time = seconds;

	/* Reset processor */
	if(seq->proc) ss_processor_system_reset(seq->proc);

	/* Fast-forward: process all events whose absolute time <= seconds without audio */
	/* We use the tempo map to convert ticks → seconds. */
	/* Replay just CC/program/pitch wheel/sysex events; skip notes. */
	bool done = false;
	/*double one_tick_sec = (midi->time_division > 0)
	    ? (60.0 / (120.0 * (double)midi->time_division))
	    : (60.0 / (120.0 * 480.0));*/

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
		/*if (sb == SS_META_SET_TEMPO && e->data_length >= 3) {
		    double bpm = read_tempo_bpm(e->data);
		    if (midi->time_division > 0)
		        one_tick_sec = 60.0 / (bpm * (double)midi->time_division);
		}*/

		/* Non-note voice events */
		if(sb >= 0x80 && sb < 0xF0 && seq->proc) {
			uint8_t type = sb & 0xF0;
			int ch = sb & 0x0F;
			switch(type) {
				case 0xB0: /* CC */
					if(e->data_length >= 2)
						ss_processor_control_change(seq->proc, ch,
						                            e->data[0], e->data[1],
						                            ev_time);
					break;
				case 0xC0: /* Program */
					if(e->data_length >= 1)
						ss_processor_program_change(seq->proc, ch,
						                            e->data[0], ev_time);
					break;
				case 0xE0: /* Pitch wheel */
					if(e->data_length >= 2)
						ss_processor_pitch_wheel(seq->proc, ch,
						                         (e->data[1] << 7) | e->data[0],
						                         ev_time);
					break;
					/* Notes are skipped during seek */
			}
		} else if(sb == 0xF0 && seq->proc && e->data_length > 0) {
			ss_processor_sysex(seq->proc, e->data, e->data_length, ev_time);
		}
	}
}

bool ss_sequencer_is_finished(const SS_Sequencer *seq) {
	return seq->finished;
}

double ss_sequencer_get_time(const SS_Sequencer *seq) {
	return seq->current_time;
}

/* ── Process a single MIDI event ─────────────────────────────────────────── */

static double g_one_tick_seconds = 0.0; /* updated by tempo events during tick */

static void process_event(SS_Sequencer *seq, SS_MIDIFile *midi,
                          SS_MIDIMessage *e, int track_index) {
	if(!seq->proc) return;
	(void)track_index;

	uint8_t sb = e->status_byte;
	double t = seq->current_time;

	/* Voice event */
	if(sb >= 0x80 && sb < 0xF0) {
		uint8_t type = sb & 0xF0;
		int ch = sb & 0x0F;
		switch(type) {
			case 0x90: /* note on */
				if(e->data_length >= 2) {
					if(e->data[1] > 0)
						ss_processor_note_on(seq->proc, ch, e->data[0], e->data[1], t);
					else
						ss_processor_note_off(seq->proc, ch, e->data[0], t);
				}
				break;
			case 0x80: /* note off */
				if(e->data_length >= 1)
					ss_processor_note_off(seq->proc, ch, e->data[0], t);
				break;
			case 0xB0: /* CC */
				if(e->data_length >= 2)
					ss_processor_control_change(seq->proc, ch, e->data[0], e->data[1], t);
				break;
			case 0xC0: /* program */
				if(e->data_length >= 1)
					ss_processor_program_change(seq->proc, ch, e->data[0], t);
				break;
			case 0xE0: /* pitch wheel */
				if(e->data_length >= 2)
					ss_processor_pitch_wheel(seq->proc, ch,
					                         (e->data[1] << 7) | e->data[0], t);
				break;
			case 0xA0: /* poly pressure */
				if(e->data_length >= 2)
					ss_processor_poly_pressure(seq->proc, ch, e->data[0], e->data[1], t);
				break;
			case 0xD0: /* channel pressure */
				if(e->data_length >= 1)
					ss_processor_channel_pressure(seq->proc, ch, e->data[0], t);
				break;
		}
		return;
	}

	/* SysEx */
	if(sb == 0xF0 && e->data_length > 0) {
		ss_processor_sysex(seq->proc, e->data, e->data_length, t);
		return;
	}

	/* Meta events */
	switch(sb) {
		case SS_META_SET_TEMPO:
			if(e->data_length >= 3 && midi->time_division > 0) {
				double bpm = read_tempo_bpm(e->data);
				g_one_tick_seconds = 60.0 / (bpm * (double)midi->time_division);
			}
			break;

		case SS_META_MIDI_PORT:
			/* ignore — port mapping is static */
			break;

		default:
			break;
	}
}

/* ── ss_sequencer_tick ────────────────────────────────────────────────────── */

void ss_sequencer_tick(SS_Sequencer *seq, uint32_t sample_count) {
	if(!seq->is_playing || seq->is_paused || seq->finished) return;

	SS_SequencerSong *song = current_song(seq);
	if(!song) {
		seq->finished = true;
		return;
	}
	SS_MIDIFile *midi = song->midi;

	/* Advance current_time by the rendered quantum */
	double dt = (seq->proc && seq->proc->sample_rate > 0) ? (double)sample_count / (double)seq->proc->sample_rate : (double)sample_count / 44100.0;
	dt *= seq->playback_rate;

	double target_time = seq->current_time + dt;

	/* Seed one_tick_seconds from the MIDI tempo map if not yet set */
	if(g_one_tick_seconds <= 0.0 && midi->time_division > 0) {
		g_one_tick_seconds = 60.0 / (120.0 * (double)midi->time_division);
	}

	/* Dispatch all events whose time <= target_time */
	while(1) {
		int ti = find_first_event(song, midi);
		if(ti < 0) {
			/* All tracks exhausted */
			seq->finished = true;
			seq->is_playing = false;
			break;
		}

		size_t ei = song->event_indexes[ti];
		SS_MIDIMessage *e = &midi->tracks[ti].events[ei];
		double ev_time = ss_midi_ticks_to_seconds(midi, e->ticks);

		if(ev_time > target_time) break;

		song->event_indexes[ti]++;
		process_event(seq, midi, e, ti);

		/* Check loop */
		if(seq->loop_count != 0 && midi->loop.end > 0 &&
		   e->ticks >= midi->loop.end) {
			if(seq->loop_count > 0) seq->loop_count--;
			seq->loops_played++;
			/* Rewind to loop start */
			double loop_start_time = ss_midi_ticks_to_seconds(midi, midi->loop.start);
			ss_sequencer_set_time(seq, loop_start_time);
			return;
		}

		/* Check end-of-sequence */
		if(e->ticks >= midi->last_voice_event_tick) {
			int next_ti = find_first_event(song, midi);
			if(next_ti < 0) {
				seq->finished = true;
				seq->is_playing = false;
				break;
			}
		}
	}

	seq->current_time = target_time;
}
