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

void ss_midi_track_clear(SS_MIDITrack *t) {
	if(!t) return;
	if(t->events) {
		for(size_t i = 0; i < t->event_count; i++)
			free(t->events[i].data);
	}
	free(t->events);
	memset(t, 0, sizeof(*t));
	t->port = -1;
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
	size_t i;
	for(i = t->event_count; i > 0; i--) {
		SS_MIDIMessage *m = &t->events[i - 1];
		if(m->ticks <= msg.ticks) break;
	}
	if(i < t->event_count) {
		/* Insert the event */
		for(size_t ii = t->event_count; ii > i; ii--) {
			t->events[ii] = t->events[ii - 1];
		}
		t->events[i] = msg;
		t->event_count++;
		return true;
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
	free(m->timeline);
	for(size_t i = 0; i < m->track_count; i++)
		ss_midi_track_clear(&m->tracks[i]);
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
	size_t i;
	for(i = m->tempo_change_count; i > 0; i--) {
		SS_TempoChange *tc = &m->tempo_changes[i - 1];
		if(tc->ticks <= ticks) break;
	}
	if(i < m->tempo_change_count) {
		/* Insert the event */
		for(size_t ii = m->tempo_change_count; ii > i; ii--) {
			m->tempo_changes[ii] = m->tempo_changes[ii - 1];
		}
	}
	m->tempo_changes[i].ticks = ticks;
	m->tempo_changes[i].tempo = bpm;
	m->tempo_change_count++;
	return true;
}

size_t ss_seconds_to_midi_tick(const SS_MIDIFile *m, double seconds_in) {
	double total = 0.0;
	double current_tempo = 60000000.0 / 500000.0; /* Default */
	size_t current_tick = 0;
	SS_TempoChange *tc;
	size_t i;
	if(m->tempo_change_count > 0 && m->time_division > 0) {
		for(i = 0, tc = m->tempo_changes; i < m->tempo_change_count; i++, tc++) {
			size_t delta = tc->ticks - current_tick;
			if(delta > 0) {
				double delta_time = (double)delta * 60.0 / (current_tempo * (double)m->time_division);
				if(total + delta_time > seconds_in) break;
				total += delta_time;
				current_tick += delta;
			}
			current_tempo = tc->tempo;
		}
	}
	size_t delta = (size_t)((seconds_in - total) / 60.0 * (current_tempo * (double)m->time_division));
	return current_tick + delta;
}

double ss_midi_ticks_to_seconds(const SS_MIDIFile *m, size_t ticks_in) {
	size_t ticks = ticks_in;
	if(m->tempo_change_count == 0 || m->time_division == 0) return 0.0;
	double total = 0.0;
	double current_tempo = 60000000.0 / 500000.0; /* Default */
	size_t current_tick = 0;
	SS_TempoChange *tc;
	size_t i;
	for(i = 0, tc = m->tempo_changes; i < m->tempo_change_count && current_tick + ticks >= tc->ticks; i++, tc++) {
		size_t delta = tc->ticks - current_tick;
		total += (double)delta * 60.0 / (current_tempo * (double)m->time_division);
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

/* ── Loop scanning ───────────────────────────────────────────────────────── */

/* Ported from midi_processing's midi_container::scan_for_loops.  Runs the
 * four independent loop-marker scanners (Touhou, RPG Maker, XMI, Marker)
 * and takes the outermost surviving start/end across them.  Must be
 * called after m->last_voice_event_tick has been computed so the sanity
 * check at the end can compare against song end. */

#define LOOP_UNSET ((size_t)-1)

static bool is_cc(const SS_MIDIMessage *e) {
	return (e->status_byte & 0xF0) == 0xB0 && e->data_length >= 2;
}

static void scan_loops(SS_MIDIFile *m) {
	size_t loop_start = LOOP_UNSET;
	size_t loop_end = LOOP_UNSET;
	SS_MIDILoopType loop_type = SS_LOOP_TYPE_HARD;

	/* Pre-pass: work out which of the three conventions that write CC 110
	 * and CC 111 this file is using, because they collide.
	 *
	 *   EMIDI     CC 110 designates a track for a sound card, CC 111
	 *             excludes it from one.  Neither is a loop marker.
	 *   LeapFrog  CC 110 begins a loop, CC 111 ends it.
	 *   RPG Maker CC 111 with value 0 starts a loop.  No CC 110.
	 *
	 * A CC 112-119 anywhere settles it as EMIDI: no other convention
	 * touches that part of the block.  Failing that, the two readings of
	 * CC 110 are told apart by where it sits.  A designation declares
	 * what a track is, so it is written at the head of the track, before
	 * any of its content; a LeapFrog loop begins somewhere inside the
	 * song.  Across the 36 EMIDI-ish files to hand the split is total:
	 * every one of the 32 EMIDI files puts all its designations at tick
	 * 0 or 1, and the four LeapFrog files (Shattered Steel's MISS5,
	 * MISSA, MISSB and SPACE) put their lone CC 110 at tick 189 or
	 * later.
	 *
	 * midi_processing and libmidi instead stop looking for a LeapFrog
	 * loop only at a CC 112-119, so they read the tick-0 designations in
	 * BRIEFING.MID and APOGEE.MID as a loop that begins and ends before
	 * the first note. */
	const bool any_emidi = ss_midi_has_emidi(m);

	bool any_designation = false; /* CC 110 at the head of a track */
	size_t leapfrog_start = LOOP_UNSET; /* earliest CC 110 within the song */
	if(!any_emidi) {
		for(size_t ti = 0; ti < m->track_count; ti++) {
			const SS_MIDITrack *t = &m->tracks[ti];
			for(size_t ei = 0; ei < t->event_count; ei++) {
				const SS_MIDIMessage *e = &t->events[ei];
				if(!is_cc(e) || e->data[0] != 110) continue;
				if(e->ticks <= 1) {
					any_designation = true;
				} else if(leapfrog_start == LOOP_UNSET ||
				          e->ticks < leapfrog_start) {
					leapfrog_start = e->ticks;
				}
			}
		}
	}
	const bool leapfrog = leapfrog_start != LOOP_UNSET;

	/* Scan 1 — Touhou (format 0 only): CC 2 = start, CC 4 = end.  A
	 * non-zero value on either voids the entire Touhou result. */
	if(m->format == 0) {
		size_t t_start = LOOP_UNSET;
		size_t t_end = LOOP_UNSET;
		bool errored = false;
		for(size_t ti = 0; ti < m->track_count && !errored; ti++) {
			const SS_MIDITrack *t = &m->tracks[ti];
			for(size_t ei = 0; ei < t->event_count && !errored; ei++) {
				const SS_MIDIMessage *e = &t->events[ei];
				if(!is_cc(e)) continue;
				if(e->data[0] == 2) {
					if(e->data[1] != 0) {
						errored = true;
						break;
					}
					t_start = e->ticks;
				} else if(e->data[0] == 4) {
					if(e->data[1] != 0) {
						errored = true;
						break;
					}
					loop_type = SS_LOOP_TYPE_SOFT;
					t_end = e->ticks;
				}
			}
		}
		if(!errored) {
			if(t_start != LOOP_UNSET &&
			   (loop_start == LOOP_UNSET || t_start < loop_start))
				loop_start = t_start;
			if(t_end != LOOP_UNSET &&
			   (loop_end == LOOP_UNSET || t_end > loop_end))
				loop_end = t_end;
		}
	}

	/* Scan 2 — RPG Maker: CC 111 with value 0 = start.  Only when no
	 * CC 110 has claimed the pair for one of the other two conventions,
	 * in either of its readings. */
	if(!any_emidi && !any_designation && !leapfrog) {
		for(size_t ti = 0; ti < m->track_count; ti++) {
			const SS_MIDITrack *t = &m->tracks[ti];
			for(size_t ei = 0; ei < t->event_count; ei++) {
				const SS_MIDIMessage *e = &t->events[ei];
				if(!is_cc(e) || e->data[0] != 111 || e->data[1] != 0) continue;
				if(loop_start == LOOP_UNSET || e->ticks < loop_start)
					loop_start = e->ticks;
			}
		}
	}

	/* Scan 2b — LeapFrog: CC 110 = begin, CC 111 = end.  The begin was
	 * found in the pre-pass; the end is the last CC 111 at or after it,
	 * whatever its value.  An end marker makes the loop soft, as the
	 * equivalent XMI and Touhou markers do below. */
	if(leapfrog) {
		if(loop_start == LOOP_UNSET || leapfrog_start < loop_start)
			loop_start = leapfrog_start;
		size_t lf_end = LOOP_UNSET;
		for(size_t ti = 0; ti < m->track_count; ti++) {
			const SS_MIDITrack *t = &m->tracks[ti];
			for(size_t ei = 0; ei < t->event_count; ei++) {
				const SS_MIDIMessage *e = &t->events[ei];
				if(!is_cc(e) || e->data[0] != 111) continue;
				if(e->ticks < leapfrog_start) continue;
				if(lf_end == LOOP_UNSET || e->ticks > lf_end)
					lf_end = e->ticks;
			}
		}
		if(lf_end != LOOP_UNSET) {
			if(loop_end == LOOP_UNSET || lf_end > loop_end)
				loop_end = lf_end;
			loop_type = SS_LOOP_TYPE_SOFT;
		}
	}

	/* Scan 3 — XMI / EMIDI: CC 0x74/0x76 = start, CC 0x75/0x77 = end. */
	for(size_t ti = 0; ti < m->track_count; ti++) {
		const SS_MIDITrack *t = &m->tracks[ti];
		for(size_t ei = 0; ei < t->event_count; ei++) {
			const SS_MIDIMessage *e = &t->events[ei];
			if(!is_cc(e)) continue;
			uint8_t cc = e->data[0];
			if(cc == 0x74 || cc == 0x76) {
				if(loop_start == LOOP_UNSET || e->ticks < loop_start)
					loop_start = e->ticks;
			} else if(cc == 0x75 || cc == 0x77) {
				if(loop_end == LOOP_UNSET || e->ticks > loop_end)
					loop_end = e->ticks;
				loop_type = SS_LOOP_TYPE_SOFT;
			}
		}
	}

	/* Scan 4 — Marker meta events: "loopStart" / "loopEnd" (case
	 * insensitive).  "start" is also accepted as a loop-start alias. */
	for(size_t ti = 0; ti < m->track_count; ti++) {
		const SS_MIDITrack *t = &m->tracks[ti];
		for(size_t ei = 0; ei < t->event_count; ei++) {
			const SS_MIDIMessage *e = &t->events[ei];
			if(e->status_byte != SS_META_MARKER) continue;
			char lower[64];
			str_lower_trim((const char *)e->data,
			               e->data_length < 63 ? e->data_length : 63,
			               lower, sizeof(lower));
			if(strcmp(lower, "loopstart") == 0 || strcmp(lower, "start") == 0) {
				if(loop_start == LOOP_UNSET || e->ticks < loop_start)
					loop_start = e->ticks;
			} else if(strcmp(lower, "loopend") == 0) {
				if(loop_end == LOOP_UNSET || e->ticks > loop_end)
					loop_end = e->ticks;
			}
		}
	}

	/* Sanity: degenerate loops (empty range, or "loop start" sitting on
	 * the song's final tick) get dropped entirely. */
	if(loop_start != LOOP_UNSET) {
		if(loop_start == loop_end ||
		   loop_start == m->last_voice_event_tick) {
			loop_start = LOOP_UNSET;
			loop_end = LOOP_UNSET;
		}
	}

	/* Fallback: start-only loops extend to the last voice event. */
	if(loop_start != LOOP_UNSET && loop_end == LOOP_UNSET)
		loop_end = m->last_voice_event_tick;

	m->loop.start = (loop_start != LOOP_UNSET) ? loop_start : 0;
	m->loop.end = (loop_end != LOOP_UNSET) ? loop_end : 0;
	m->loop.type = loop_type;
}

/* ── Port resolution ─────────────────────────────────────────────────────── */

/*
 * A track's port can be stated three ways, in descending order of trust:
 *
 *   0x21 MIDI Port       — a literal port number.  The modern, unambiguous
 *                          form; always wins when present.
 *   0x09 Device Name     — names the destination device.  Distinct names are
 *                          interned in encounter order and become ports
 *                          0, 1, 2, …
 *   0x04 Instrument Name — genuinely means "instrument" in the SMF spec, but
 *                          some older authoring tools reused it as a device
 *                          name.  Only consulted as a last resort, and only
 *                          when it looks like device naming rather than
 *                          instrument labelling (see instrument_names_are_ports).
 *
 * Whatever the source, the resulting port is folded into
 * [0, SS_MIDI_PORT_COUNT) so that the channel offsets derived from it can
 * never index past the processor's channel array.
 */

/* Maximum distinct device names tracked. Names beyond this are unresolvable
 * and leave their track unassigned; the limit only has to exceed
 * SS_MIDI_PORT_COUNT by enough to make the "too many names to be devices"
 * test below meaningful. */
#define PORT_NAME_MAX_ENTRIES 32
/* Device names are compared on their first PORT_NAME_MAX_LEN bytes. */
#define PORT_NAME_MAX_LEN 64

typedef struct {
	char names[PORT_NAME_MAX_ENTRIES][PORT_NAME_MAX_LEN];
	size_t count;
} PortNameTable;

/**
 * Normalise a name-meta payload for comparison: trim leading/trailing spaces
 * and control bytes, truncate, and fold to lower case. Returns the length,
 * which is 0 if nothing printable remained.
 */
static size_t port_name_normalize(const uint8_t *data, size_t len,
                                  char *out, size_t out_size) {
	size_t start = 0, end = len;
	while(start < end && (unsigned char)data[start] <= ' ') start++;
	while(end > start && (unsigned char)data[end - 1] <= ' ') end--;

	size_t n = end - start;
	if(n > out_size - 1) n = out_size - 1;
	for(size_t i = 0; i < n; i++)
		out[i] = (char)tolower((unsigned char)data[start + i]);
	out[n] = '\0';
	return n;
}

/** Look up name, appending it if new. Returns its index, or -1 if the table
 *  is full. */
static int port_name_intern(PortNameTable *t, const char *name) {
	for(size_t i = 0; i < t->count; i++) {
		if(strcmp(t->names[i], name) == 0) return (int)i;
	}
	if(t->count >= PORT_NAME_MAX_ENTRIES) return -1;
	strcpy(t->names[t->count], name);
	return (int)t->count++;
}

/**
 * Decide whether this file's 0x04 events are device names rather than
 * instrument labels. Two signals have to agree:
 *
 *   - There are few enough distinct names to be plausible devices. A file
 *     labelling instruments typically has one name per part, far more than
 *     the handful of ports any device chain has.
 *   - Two different names claim the same MIDI channel. That collision is the
 *     only reason port separation would matter: without it, a single-port
 *     reading of the file plays correctly and splitting it would just scatter
 *     the parts across ports for no gain.
 *
 * track_names holds each track's interned 0x04 index (-1 for none) and
 * track_channels its channel-usage bitmask.
 */
static bool instrument_names_are_ports(const int *track_names,
                                       const uint16_t *track_channels,
                                       size_t track_count, size_t name_count) {
	if(name_count < 2 || name_count > SS_MIDI_PORT_COUNT) return false;

	uint16_t per_name[SS_MIDI_PORT_COUNT];
	memset(per_name, 0, sizeof(per_name));
	for(size_t ti = 0; ti < track_count; ti++) {
		if(track_names[ti] >= 0)
			per_name[track_names[ti]] |= track_channels[ti];
	}

	for(size_t i = 1; i < name_count; i++) {
		for(size_t j = 0; j < i; j++) {
			if((per_name[i] & per_name[j]) != 0) return true;
		}
	}
	return false;
}

/**
 * Assign SS_MIDITrack.port for every track from the file's port metadata, and
 * set m->is_multi_port. Ports are folded into [0, SS_MIDI_PORT_COUNT).
 */
static void resolve_track_ports(SS_MIDIFile *m) {
	/* ── Survey pass: what port metadata does this file actually carry? ─── */
	bool has_explicit = false; /* any usable 0x21 */
	bool has_device = false;   /* any usable 0x09 */

	int *inst_name = (int *)malloc(m->track_count * sizeof(int));
	uint16_t *inst_channels = (uint16_t *)calloc(m->track_count,
	                                             sizeof(uint16_t));
	PortNameTable inst_table;
	inst_table.count = 0;

	for(size_t ti = 0; ti < m->track_count; ti++) {
		if(inst_name) inst_name[ti] = -1;
		SS_MIDITrack *track = &m->tracks[ti];

		for(size_t ei = 0; ei < track->event_count; ei++) {
			SS_MIDIMessage *e = &track->events[ei];
			uint8_t sb = e->status_byte;

			if(sb >= 0x80 && sb < 0xF0) {
				if(inst_channels) inst_channels[ti] |= (uint16_t)(1u << (sb & 0x0F));
				continue;
			}
			if(e->data_length == 0) continue;

			if(sb == SS_META_MIDI_PORT) {
				has_explicit = true;
			} else if(sb == SS_META_DEVICE_NAME) {
				has_device = true;
			} else if(sb == SS_META_INSTRUMENT_NAME && inst_name &&
			          inst_name[ti] < 0) {
				char name[PORT_NAME_MAX_LEN];
				if(port_name_normalize(e->data, e->data_length, name,
				                       sizeof(name)) > 0)
					inst_name[ti] = port_name_intern(&inst_table, name);
			}
		}
	}

	bool use_inst = !has_explicit && !has_device && inst_name && inst_channels &&
	                instrument_names_are_ports(inst_name, inst_channels,
	                                           m->track_count,
	                                           inst_table.count);

	/* ── Assignment pass ────────────────────────────────────────────────── */
	PortNameTable device_table;
	device_table.count = 0;

	for(size_t ti = 0; ti < m->track_count; ti++) {
		SS_MIDITrack *track = &m->tracks[ti];
		int port = -1;             /* from 0x21; last one in the track wins */
		char device[PORT_NAME_MAX_LEN]; /* from 0x09; first one in the track wins */
		device[0] = '\0';

		/* This function is the only writer of track->port, and it can run
		 * again after the caller edits the track, so start from unassigned. */
		track->port = -1;

		for(size_t ei = 0; ei < track->event_count; ei++) {
			SS_MIDIMessage *e = &track->events[ei];
			if(e->data_length == 0) continue;

			if(e->status_byte == SS_META_MIDI_PORT) {
				port = (int)e->data[0];
			} else if(e->status_byte == SS_META_DEVICE_NAME && device[0] == '\0') {
				port_name_normalize(e->data, e->data_length, device,
				                    sizeof(device));
			}
		}

		/* Intern only once the track's source is settled, so a name that an
		 * explicit 0x21 overrode doesn't consume a port number and shift the
		 * numbering of every device named after it. */
		if(port < 0 && device[0] != '\0')
			port = port_name_intern(&device_table, device);
		if(port < 0 && use_inst)
			port = inst_name[ti];

		if(port >= 0) {
			track->port = port % SS_MIDI_PORT_COUNT;
			if(track->port > 0) m->is_multi_port = true;
		}
	}

	free(inst_name);
	free(inst_channels);
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

	bool first_note_set = false;
	bool karaoke_has_title = false;

	/* Resolve every track's port up front: the main loop below needs the
	 * final, folded numbers to size the channel-offset map. */
	resolve_track_ports(m);

	int max_port = 0;

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
					/* Note-on.  The earliest one in the file, across every
					 * track, not the first one this scan happens to reach:
					 * tracks are walked in file order, so latching on the
					 * first track that carries a note would pick that track's
					 * entry rather than the song's.  A file whose first track
					 * rests until the second section then starts there, which
					 * for a looping arrangement means skipping the whole intro
					 * and landing on the loop.  Upstream takes the minimum over
					 * each track's first note-on for the same reason.
					 *
					 * Velocity is deliberately not checked, matching upstream,
					 * which tests the status byte alone and so counts a
					 * velocity-zero note-off as marking the start. */
					uint8_t note = e->data[0];
					uint8_t vel = e->data[1];
					if(!first_note_set || e->ticks < m->first_note_on) {
						m->first_note_on = e->ticks;
						first_note_set = true;
					}
					if(vel > 0) {
						if(note < m->key_range.min) m->key_range.min = note;
						if(note > m->key_range.max) m->key_range.max = note;
					}
				} else if(type == 0xB0 && e->data_length >= 2) {
					/* Controller change — DLS RMIDI bank-offset hint only.
					 * Loop markers are handled in scan_loops below. */
					if(e->data[0] == 0 && m->is_dls_rmidi &&
					   e->data[1] != 0 && e->data[1] != 127) {
						m->bank_offset = 1;
					}
				}
			}

			/* ── Meta event ─────────────────────────────────────────────── */
			switch(sb) {
				case SS_META_END_OF_TRACK:
					/* Remove mid-track End of Track events.
					 *
					 * A trailing one does NOT extend last_voice_event_tick:
					 * upstream counts voice messages only, and a file whose
					 * tracks end on a bar line rather than on their last note
					 * would otherwise report a longer duration, loop later and
					 * finish later than upstream does.  Trailing silence is
					 * the caller's business -- that is what a render tail is
					 * for. */
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

	/* Run the four loop scanners now that last_voice_event_tick is known. */
	scan_loops(m);

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
	} else if(ss_midi_is_xmi(file, size)) {
		/* Miles Sound System XMI */
		ok = ss_midi_parse_xmi(m, file, size);
	} else if(ss_midi_is_gmf(file, size)) {
		/* General MIDI Format (GMF) */
		ok = ss_midi_parse_gmf(m, file, size);
	} else if(ss_midi_is_hmp(file, size)) {
		/* HMI Sound Operating System (HMP) */
		ok = ss_midi_parse_hmp(m, file, size);
	} else if(ss_midi_is_hmi(file, size)) {
		/* HMI-MIDISONG */
		ok = ss_midi_parse_hmi(m, file, size);
	} else if(ss_midi_is_lds(file, size, file_name)) {
		/* AdLib Loudness Sound System (LDS) tracker */
		ok = ss_midi_parse_lds(m, file, size);
	} else if(ss_midi_is_xmf(file, size)) {
		/* eXtensible Music Format (XMF / Mobile XMF) */
		ok = ss_midi_parse_xmf(m, file, size);
	} else {
		/* Plain SMF: parse directly. */
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

/* ── GS detection ─────────────────────────────────────────────────────────── */

bool ss_midi_has_gs(const SS_MIDIFile *m) {
	if(!m) return false;
	for(size_t ti = 0; ti < m->track_count; ti++) {
		const SS_MIDITrack *t = &m->tracks[ti];
		for(size_t ei = 0; ei < t->event_count; ei++) {
			const SS_MIDIMessage *msg = &t->events[ei];
			if(msg->status_byte == 0xf0) {
				if(msg->data_length < 8) continue;
				const uint8_t *data = msg->data;
				if(data[3] != 0x12) continue;
				if(data[2] == 0x42) {
					if(data[4] == 0x00 && data[5] == 0x00 && data[6] == 0x7f && data[7] == 0)
						return true;
					if(data[4] == 0x40 && data[5] == 0x00 && data[6] == 0x7f && data[7] == 0)
						return true;
				}
			}
		}
	}
	return false;
}

/* ── GM2 detection ────────────────────────────────────────────────────────── */

bool ss_midi_has_gm2(const SS_MIDIFile *m) {
	if(!m) return false;
	for(size_t ti = 0; ti < m->track_count; ti++) {
		const SS_MIDITrack *t = &m->tracks[ti];
		for(size_t ei = 0; ei < t->event_count; ei++) {
			const SS_MIDIMessage *msg = &t->events[ei];
			if(msg->status_byte == 0xf0) {
				if(msg->data_length < 4) continue;
				const uint8_t *data = msg->data;
				if(data[0] == 0x7e && data[1] == 0x7f && data[2] == 9 && data[3] == 3) {
					return true;
				}
			}
		}
	}
	return false;
}

/* ── Timeline ────────────────────────────────────────────────────────────── */

bool ss_midi_ensure_timeline(SS_MIDIFile *m) {
	free(m->timeline);
	m->timeline = NULL;

	size_t total_events = 0;
	for(size_t ti = 0; ti < m->track_count; ti++) {
		const SS_MIDITrack *t = &m->tracks[ti];
		total_events += t->event_count;
	}

	m->timeline = (SS_MIDIMessage *) calloc(total_events, sizeof(SS_MIDIMessage));
	if(!m->timeline) return false;

	size_t *current_track_event = calloc(m->track_count, sizeof(size_t));
	if(!current_track_event) return false;

	size_t current_output_event = 0;

	while(current_output_event < total_events) {
		const SS_MIDIMessage *msg = NULL;
		size_t best_ticks = ~0UL;
		for(size_t ti = 0; ti < m->track_count; ti++) {
			const SS_MIDITrack *t = &m->tracks[ti];
			const size_t current_event = current_track_event[ti];
			if(current_event >= t->event_count) continue;
			const SS_MIDIMessage *_msg = &t->events[current_event];
			if(_msg->ticks < best_ticks) {
				best_ticks = _msg->ticks;
				msg = _msg;
				continue;
			}
		}
		if(!msg) break;
		m->timeline[current_output_event++] = *msg;
		current_track_event[msg->track_index]++;
	}
	m->timeline_count = current_output_event;

	free(current_track_event);

	return true;
}
