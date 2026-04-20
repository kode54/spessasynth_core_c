/**
 * midi_loader.c
 * SMF / RMIDI loader entry point and shared infrastructure.
 *
 * Format-specific parsing lives in src/midi/parsers/:
 *   - parsers/smf.c    — Standard MIDI File (format 0/1/2)
 *   - parsers/rmidi.c  — RIFF/RMID with embedded SF2/DLS soundbank
 *
 * This file handles:
 *   - SS_MIDIFile and SS_MIDITrack lifecycle
 *   - Tempo map construction and ticks→seconds conversion
 *   - Post-parse derivation of loop, duration, key range, multi-port
 *     state, karaoke flag, etc. (midi_parse_internal)
 *   - Top-level format detection and dispatch (ss_midi_load)
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if __has_include(<spessasynth_core/spessasynth.h>)
#include <spessasynth_core/midi.h>
#else
#include "spessasynth/midi/midi.h"
#endif

#include "parsers/parsers.h"

/* ── Track helpers ───────────────────────────────────────────────────────── */

SS_MIDITrack *ss_midi_track_new(void) {
	return (SS_MIDITrack *)calloc(1, sizeof(SS_MIDITrack));
}

void ss_midi_track_free(SS_MIDITrack *t) {
	if(!t) return;
	if(t->events) {
		for(size_t i = 0; i < t->event_count; i++)
			free(t->events[i].data);
	}
	free(t->events);
	free(t);
}

bool ss_midi_track_push_event(SS_MIDITrack *t, SS_MIDIMessage msg) {
	if(t->event_count >= t->event_capacity) {
		size_t nc = t->event_capacity ? t->event_capacity * 2 : 256;
		SS_MIDIMessage *tmp = (SS_MIDIMessage *)realloc(t->events,
		                                                nc * sizeof(*tmp));
		if(!tmp) return false;
		t->events = tmp;
		t->event_capacity = nc;
	}
	t->events[t->event_count++] = msg;
	return true;
}

void ss_midi_track_delete_event(SS_MIDITrack *t, size_t idx) {
	if(idx >= t->event_count) return;
	free(t->events[idx].data);
	memmove(&t->events[idx], &t->events[idx + 1],
	        (t->event_count - idx - 1) * sizeof(t->events[0]));
	t->event_count--;
}

void ss_midi_message_free_data(SS_MIDIMessage *msg) {
	if(!msg) return;
	free(msg->data);
	msg->data = NULL;
}

/* ── SS_MIDIFile lifecycle ───────────────────────────────────────────────── */

SS_MIDIFile *ss_midi_new(void) {
	SS_MIDIFile *m = (SS_MIDIFile *)calloc(1, sizeof(SS_MIDIFile));
	if(!m) return NULL;
	m->bank_offset = 0;
	return m;
}

void ss_rmidi_info_free(SS_RMIDIInfo *info) {
	if(!info) return;
	free(info->name);
	free(info->artist);
	free(info->album);
	free(info->genre);
	free(info->picture);
	free(info->comment);
	free(info->copyright);
	free(info->creation_date);
	free(info->info_encoding);
	free(info->engineer);
	free(info->software);
	free(info->subject);
	free(info->midi_encoding);
	memset(info, 0, sizeof(*info));
}

void ss_midi_free(SS_MIDIFile *m) {
	if(!m) return;
	for(size_t i = 0; i < m->track_count; i++)
		ss_midi_track_free(&m->tracks[i]);
	free(m->tracks);
	free(m->tempo_changes);
	free(m->port_channel_offset_map);
	free(m->embedded_soundbank);
	free(m->binary_name);
	ss_rmidi_info_free(&m->rmidi_info);
	free(m);
}

/* ── Tempo map ────────────────────────────────────────────────────────────── */

static bool midi_push_tempo(SS_MIDIFile *m, size_t ticks, double bpm) {
	if(m->tempo_change_count >= m->tempo_change_capacity) {
		size_t nc = m->tempo_change_capacity ? m->tempo_change_capacity * 2 : 16;
		SS_TempoChange *tmp = (SS_TempoChange *)realloc(m->tempo_changes,
		                                                nc * sizeof(*tmp));
		if(!tmp) return false;
		m->tempo_changes = tmp;
		m->tempo_change_capacity = nc;
	}
	m->tempo_changes[m->tempo_change_count].ticks = ticks;
	m->tempo_changes[m->tempo_change_count].tempo = bpm;
	m->tempo_change_count++;
	return true;
}

/* Sort tempo changes by tick descending (like TS: last → first) */
static int tempo_cmp_desc(const void *a, const void *b) {
	const SS_TempoChange *ta = (const SS_TempoChange *)a;
	const SS_TempoChange *tb = (const SS_TempoChange *)b;
	if(tb->ticks > ta->ticks) return 1;
	if(tb->ticks < ta->ticks) return -1;
	return 0;
}

double ss_midi_ticks_to_seconds(const SS_MIDIFile *m, size_t ticks_in) {
	size_t ticks = ticks_in;
	if(m->tempo_change_count == 0 || m->time_division == 0) return 0.0;
	double total = 0.0;
	double current_tempo = 60000000.0 / 500000.0; /* Default */
	uint32_t current_tick = 0;
	SS_TempoChange *tc;
	size_t i;
	for(i = 0, tc = m->tempo_changes; i < m->tempo_change_count && current_tick + ticks >= tc->ticks; i++, tc++) {
		size_t delta = tc->ticks - current_tick;
		total += (double)delta * 60.0 / (tc->tempo * (double)m->time_division);
		current_tick += delta;
		ticks -= delta;
		current_tempo = tc->tempo;
	}
	total += (double)ticks * 60.0 / (current_tempo * (double)m->time_division);
	return total;
}

/* ── Read 3-byte big-endian tempo (µs/beat) → BPM ──────────────────────── */

static double read_tempo_bpm(const uint8_t *d) {
	uint32_t us = ((uint32_t)d[0] << 16) | ((uint32_t)d[1] << 8) | d[2];
	if(us == 0) us = 500000;
	return 60000000.0 / (double)us;
}

/* ── Lowercase trim for marker comparison ────────────────────────────────── */

static void str_lower_trim(const char *src, size_t len,
                           char *dst, size_t dst_size) {
	size_t i = 0, j = 0;
	/* Skip leading whitespace */
	while(i < len && isspace((unsigned char)src[i])) i++;
	/* Copy and lower */
	while(i < len && j + 1 < dst_size) {
		char c = (char)tolower((unsigned char)src[i++]);
		if(!isspace((unsigned char)c))
			dst[j++] = c;
		else { /* collapse trailing space */
			dst[j++] = c;
		}
	}
	/* Trim trailing whitespace */
	while(j > 0 && isspace((unsigned char)dst[j - 1])) j--;
	dst[j] = '\0';
}

/* ── parseInternal — builds tempo map, loop, duration, key range ─────────── */

static void midi_parse_internal(SS_MIDIFile *m) {
	/* Reset all derived values */
	m->tempo_change_count = 0;
	m->first_note_on = 0;
	m->last_voice_event_tick = 0;
	m->loop.start = 0;
	m->loop.end = 0;
	m->key_range.min = 127;
	m->key_range.max = 0;
	m->is_karaoke = false;
	m->is_multi_port = false;

	/* Seed tempo map with 120 BPM at tick 0 */
	midi_push_tempo(m, 0, 120.0);

	bool loop_start_found = false;
	int loop_end_count = 0;
	bool first_note_set = false;
	bool karaoke_has_title = false;

	/* Port tracking: map port_num → channel_offset */
	int max_port = 0;
	int port_offsets[64];
	memset(port_offsets, -1, sizeof(port_offsets));
	port_offsets[0] = 0;

	for(size_t ti = 0; ti < m->track_count; ti++) {
		SS_MIDITrack *track = &m->tracks[ti];
		int track_port = (track->port >= 0) ? track->port : 0;
		if(track_port > max_port) max_port = track_port;

		for(size_t ei = 0; ei < track->event_count; ei++) {
			SS_MIDIMessage *e = &track->events[ei];
			e->track_index = (uint16_t)ti;
			uint8_t sb = e->status_byte;

			/* ── Voice message ──────────────────────────────────────────── */
			if(sb >= 0x80 && sb < 0xF0) {
				if(e->ticks > m->last_voice_event_tick)
					m->last_voice_event_tick = e->ticks;

				uint8_t type = sb & 0xF0;
				uint8_t channel = sb & 0x0F;
				(void)channel;

				if(type == 0x90 && e->data_length >= 2) {
					/* Note-on */
					uint8_t note = e->data[0];
					uint8_t vel = e->data[1];
					if(vel > 0) {
						if(!first_note_set) {
							m->first_note_on = e->ticks;
							first_note_set = true;
						}
						if(note < m->key_range.min) m->key_range.min = note;
						if(note > m->key_range.max) m->key_range.max = note;
					}
				} else if(type == 0xB0 && e->data_length >= 2) {
					/* Controller change — loop detection */
					uint8_t cc = e->data[0];
					uint8_t val = e->data[1];
					(void)val;
					switch(cc) {
						case 2: /* Touhou loop start */
						case 111: /* RPG Maker */
						case 116: /* EMIDI/XMI loop start */
							m->loop.start = e->ticks;
							loop_start_found = true;
							break;
						case 4: /* Touhou loop end */
						case 117: /* EMIDI/XMI loop end */
							if(loop_end_count == 0) {
								m->loop.end = e->ticks;
							} else {
								m->loop.end = 0; /* duplicate → not a real loop end */
							}
							loop_end_count++;
							break;
						case 0: /* Bank select MSB — detect DLS RMIDI bank offset */
							if(m->is_dls_rmidi && e->data_length >= 2 &&
							   e->data[1] != 0 && e->data[1] != 127) {
								m->bank_offset = 1;
							}
							break;
					}
				}
			}

			/* ── Meta event ─────────────────────────────────────────────── */
			switch(sb) {
				case SS_META_END_OF_TRACK:
					/* Remove mid-track End of Track events */
					if(ei != track->event_count - 1) {
						ss_midi_track_delete_event(track, ei);
						ei--;
					}
					break;

				case SS_META_SET_TEMPO:
					if(e->data_length >= 3)
						midi_push_tempo(m, e->ticks, read_tempo_bpm(e->data));
					break;

				case SS_META_TRACK_NAME:
					if(e->data_length > 0 && track->name[0] == '\0') {
						size_t copy = e->data_length < 255 ? e->data_length : 255;
						memcpy(track->name, e->data, copy);
						track->name[copy] = '\0';
						/* Use track name as MIDI name if none found yet */
						if(m->binary_name == NULL) {
							m->binary_name = (uint8_t *)malloc(copy);
							if(m->binary_name) {
								memcpy(m->binary_name, e->data, copy);
								m->binary_name_length = copy;
							}
						}
					}
					break;

				case SS_META_MIDI_PORT:
					if(e->data_length >= 1) {
						int port = (int)e->data[0];
						track->port = port;
						if(port > 0) m->is_multi_port = true;
						if(port > max_port) max_port = port;
					}
					break;

				case SS_META_MARKER: {
					/* Loop-point markers: "loopstart" / "loopend" / "start" */
					char lower[64];
					str_lower_trim((const char *)e->data,
					               e->data_length < 63 ? e->data_length : 63,
					               lower, sizeof(lower));
					if(strcmp(lower, "loopstart") == 0 ||
					   strcmp(lower, "start") == 0) {
						m->loop.start = e->ticks;
						loop_start_found = true;
					} else if(strcmp(lower, "loopend") == 0) {
						m->loop.end = e->ticks;
						loop_end_count++;
					}
					break;
				}

				case SS_META_TEXT: {
					if(e->data_length == 0) break;
					char buf[32];
					size_t clen = e->data_length < 31 ? e->data_length : 31;
					memcpy(buf, e->data, clen);
					buf[clen] = '\0';
					/* Karaoke detection */
					if(strstr(buf, "@KMIDI") || strstr(buf, "KARAOKE")) {
						m->is_karaoke = true;
					}
					break;
				}

				case SS_META_LYRIC: {
					if(e->data_length == 0) break;
					char buf[32];
					size_t clen = e->data_length < 31 ? e->data_length : 31;
					memcpy(buf, e->data, clen);
					buf[clen] = '\0';
					if(strstr(buf, "@KMIDI") || strstr(buf, "KARAOKE")) {
						m->is_karaoke = true;
					}
					break;
				}
			}
		}
	}

	/* If only loop start found, loop end = last voice event */
	if(loop_start_found && m->loop.end == 0)
		m->loop.end = m->last_voice_event_tick;

	/* Sort tempo changes descending by tick (last → first) */
	qsort(m->tempo_changes, m->tempo_change_count,
	      sizeof(m->tempo_changes[0]), tempo_cmp_desc);

	/* Compute duration */
	m->duration = ss_midi_ticks_to_seconds(m, m->last_voice_event_tick);

	/* Build port_channel_offset_map */
	if(m->is_multi_port) {
		size_t map_size = (size_t)(max_port + 1);
		free(m->port_channel_offset_map);
		m->port_channel_offset_map = (int *)calloc(map_size, sizeof(int));
		if(m->port_channel_offset_map) {
			for(int p = 0; p <= max_port; p++)
				m->port_channel_offset_map[p] = p * 16;
			m->port_channel_offset_map_count = map_size;
		}
	}

	(void)karaoke_has_title;
}

/* ── Public loader ───────────────────────────────────────────────────────── */

SS_MIDIFile *ss_midi_load(SS_File *file, const char *file_name) {
	if(!file || ss_file_size(file) < 14) return NULL;

	SS_MIDIFile *m = ss_midi_new();
	if(!m) return NULL;

	if(file_name && *file_name)
		strncpy(m->file_name, file_name, sizeof(m->file_name) - 1);

	size_t size = ss_file_size(file);
	char header[5];
	ss_file_read_string(file, 0, header, 4);

	bool ok;
	if(header[0] == 'R' && header[1] == 'I' && header[2] == 'F' && header[3] == 'F') {
		/* Disambiguate RIFF variants by inner 4CC */
		if(ss_midi_is_mids(file, size)) {
			/* Microsoft DirectMusic Segment */
			ok = ss_midi_parse_mids(m, file, size);
		} else {
			/* RIFF-MIDI wrapper */
			ok = ss_midi_parse_rmidi(m, file, size);
		}
	} else if(ss_midi_is_mus(file, size)) {
		/* DOOM/Heretic MUS */
		ok = ss_midi_parse_mus(m, file, size);
	} else {
		/* XMF: not implemented — treat as plain SMF.
		 * Plain SMF: parse directly. */
		ok = ss_midi_parse_smf(m, file, size);
	}

	if(!ok) {
		ss_midi_free(m);
		return NULL;
	}

	midi_parse_internal(m);
	return m;
}

/* ── flush ────────────────────────────────────────────────────────────────── */

void ss_midi_flush(SS_MIDIFile *m) {
	if(!m) return;
	midi_parse_internal(m);
}
