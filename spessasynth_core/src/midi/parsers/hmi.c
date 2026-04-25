/**
 * hmi.c
 * HMI "HMI-MIDISONG" parser (Human Machine Interfaces' newer engine).
 * Port of midi_processor_hmi.cpp from midi_processing.
 *
 * Layout:
 *   [0x00..0x0B]  "HMI-MIDISONG" magic
 *   [0xE4..0xE7]  uint32 LE track_count
 *   [0xE8..0xEB]  uint32 LE track_table_offset
 *   [track_table_offset ..]  track_count × uint32 LE track offsets
 *   Each track begins with "HMI-MIDITRACK" and contains:
 *     [+0x4B] uint32 LE meta_offset (optional track-relative; 2-byte
 *             prefix then text; trailing spaces trimmed)
 *     [+0x57] uint32 LE track_data_offset (event stream within track)
 *
 * Event stream uses standard MIDI VLQ deltas, running status, note-on
 * duration (synthesizes matching note-off), plus an 0xFE "HMI meta"
 * block with sub-types:
 *   0x10  length-prefixed block (skip)
 *   0x12  2-byte skip
 *   0x13  10-byte skip
 *   0x14  2-byte skip + emit "loopStart" marker on the conductor track
 *   0x15  6-byte skip + emit "loopEnd" marker on the conductor track
 *
 * Produces an SMF format-1 file: track 0 is a conductor seeded with the
 * HMI default tempo (0x188000 µs/beat at TPQN 0xC0 ≈ 120 ticks/sec)
 * plus any loop markers; tracks 1..N carry the decoded HMI tracks.
 */

#include <stdlib.h>
#include <string.h>

#include "parsers.h"

/* Default conductor tempo (shared value with HMP). */
static const uint8_t HMI_DEFAULT_TEMPO[3] = { 0x18, 0x80, 0x00 };
static const uint8_t HMI_LOOP_START[] = { 'l', 'o', 'o', 'p', 'S', 't', 'a', 'r', 't' };
static const uint8_t HMI_LOOP_END[] = { 'l', 'o', 'o', 'p', 'E', 'n', 'd' };

/* ── Detect HMI magic (HMI-MIDISONG) ─────────────────────────────────────── */

bool ss_midi_is_hmi(SS_File *file, size_t size) {
	if(size < 12) return false;
	static const char magic[] = "HMI-MIDISONG";
	for(int i = 0; i < 12; i++)
		if(ss_file_read_u8(file, (size_t)i) != (uint8_t)magic[i]) return false;
	return true;
}

/* ── Push a typed message onto the track (takes ownership of data) ───────── */

static bool push_msg(SS_MIDITrack *track, size_t ticks, uint8_t status_byte,
                     const uint8_t *data, size_t data_len) {
	SS_MIDIMessage msg;
	memset(&msg, 0, sizeof(msg));
	msg.ticks = ticks;
	msg.status_byte = status_byte;
	msg.data_length = data_len;
	if(data_len > 0) {
		msg.data = (uint8_t *)malloc(data_len);
		if(!msg.data) return false;
		memcpy(msg.data, data, data_len);
	}
	if(!ss_midi_track_push_event(track, msg)) {
		free(msg.data);
		return false;
	}
	return true;
}

/* ── Parse one HMI track body into an existing SS_MIDITrack ──────────────── */

static bool parse_hmi_track(SS_File *file, size_t start, size_t end,
                            SS_MIDITrack *track, SS_MIDITrack *conductor) {
	size_t pos = start;
	size_t abs_tick = 0;
	size_t last_event_tick = 0;
	uint8_t last_status = 0xFF; /* sentinel: no prior voice status */

	while(pos < end) {
		size_t delta = ss_file_read_vlq(file, pos);
		pos = ss_file_tell(file);

		/* Reference "shunt": treat absurd deltas (>65535) as a reset to
		 * the last known event tick.  Guards against a class of corrupt
		 * HMI files seen in the wild. */
		if(delta > 0xFFFF) {
			abs_tick = last_event_tick;
		} else {
			abs_tick += delta;
			if(abs_tick > last_event_tick) last_event_tick = abs_tick;
		}

		if(pos >= end) return false;
		uint8_t status = ss_file_read_u8(file, pos++);

		if(status == 0xFF) {
			/* Meta event. */
			last_status = 0xFF;
			if(pos >= end) return false;
			uint8_t meta_type = ss_file_read_u8(file, pos++);
			size_t meta_len = ss_file_read_vlq(file, pos);
			pos = ss_file_tell(file);
			if(end - pos < meta_len) return false;

			uint8_t *data = NULL;
			if(meta_len > 0) {
				data = (uint8_t *)malloc(meta_len);
				if(!data) return false;
				ss_file_read_bytes(file, pos, data, meta_len);
			}
			pos += meta_len;

			/* EOT slides forward to cover any pending note-off ticks. */
			if(meta_type == SS_META_END_OF_TRACK && abs_tick < last_event_tick)
				abs_tick = last_event_tick;

			SS_MIDIMessage msg;
			memset(&msg, 0, sizeof(msg));
			msg.ticks = abs_tick;
			msg.status_byte = meta_type;
			msg.data = data;
			msg.data_length = meta_len;
			if(!ss_midi_track_push_event(track, msg)) {
				free(data);
				return false;
			}

			if(meta_type == SS_META_END_OF_TRACK) break;

		} else if(status == 0xF0) {
			/* SysEx. */
			last_status = 0xFF;
			size_t sx_len = ss_file_read_vlq(file, pos);
			pos = ss_file_tell(file);
			if(end - pos < sx_len) return false;

			uint8_t *data = NULL;
			if(sx_len > 0) {
				data = (uint8_t *)malloc(sx_len);
				if(!data) return false;
				ss_file_read_bytes(file, pos, data, sx_len);
			}
			pos += sx_len;

			SS_MIDIMessage msg;
			memset(&msg, 0, sizeof(msg));
			msg.ticks = abs_tick;
			msg.status_byte = 0xF0;
			msg.data = data;
			msg.data_length = sx_len;
			if(!ss_midi_track_push_event(track, msg)) {
				free(data);
				return false;
			}

		} else if(status == 0xFE) {
			/* HMI-specific sub-event. */
			last_status = 0xFF;
			if(pos >= end) return false;
			uint8_t sub = ss_file_read_u8(file, pos++);
			switch(sub) {
				case 0x10: {
					if(end - pos < 3) return false;
					pos += 2;
					uint8_t extra = ss_file_read_u8(file, pos++);
					if(end - pos < (size_t)extra + 4) return false;
					pos += (size_t)extra + 4;
					break;
				}
				case 0x12:
					if(end - pos < 2) return false;
					pos += 2;
					break;
				case 0x13:
					if(end - pos < 10) return false;
					pos += 10;
					break;
				case 0x14:
					if(end - pos < 2) return false;
					pos += 2;
					if(!push_msg(conductor, abs_tick, SS_META_MARKER,
					             HMI_LOOP_START, sizeof(HMI_LOOP_START)))
						return false;
					break;
				case 0x15:
					if(end - pos < 6) return false;
					pos += 6;
					if(!push_msg(conductor, abs_tick, SS_META_MARKER,
					             HMI_LOOP_END, sizeof(HMI_LOOP_END)))
						return false;
					break;
				default:
					return false;
			}

		} else if(status <= 0xEF) {
			/* Voice event, possibly running status. */
			uint8_t actual_status;
			uint8_t d1;
			if(status >= 0x80) {
				actual_status = status;
				if(pos >= end) return false;
				d1 = ss_file_read_u8(file, pos++);
				last_status = status;
			} else {
				/* Running status: byte we already read IS data1. */
				if(last_status == 0xFF) return false;
				actual_status = last_status;
				d1 = status;
			}

			uint8_t type = actual_status & 0xF0;
			uint8_t data[2];
			size_t data_len = 1;
			data[0] = d1;
			if(type != 0xC0 && type != 0xD0) {
				if(pos >= end) return false;
				data[1] = ss_file_read_u8(file, pos++);
				data_len = 2;
			}

			if(!push_msg(track, abs_tick, actual_status, data, data_len))
				return false;

			/* Note-on stores a VLQ duration; synthesize the matching note-off. */
			if(type == 0x90) {
				size_t duration = ss_file_read_vlq(file, pos);
				pos = ss_file_tell(file);
				size_t note_end = abs_tick + duration;
				if(note_end > last_event_tick) last_event_tick = note_end;

				uint8_t off_data[2] = { d1, 0 };
				if(!push_msg(track, note_end, actual_status, off_data, 2))
					return false;
			}

		} else {
			return false; /* Unexpected status code */
		}
	}

	return true;
}

/* ── Public HMI parser ───────────────────────────────────────────────────── */

bool ss_midi_parse_hmi(SS_MIDIFile *m, SS_File *file, size_t size) {
	if(!ss_midi_is_hmi(file, size) || size < 0xEC) return false;

	uint32_t track_count = ss_file_read_le32(file, 0xE4);
	uint32_t track_table_offset = ss_file_read_le32(file, 0xE8);
	if(track_count == 0) return false;
	if(track_table_offset >= size) return false;
	if((uint64_t)track_table_offset + (uint64_t)track_count * 4 > size) return false;

	uint32_t *track_offsets = (uint32_t *)malloc(track_count * sizeof(uint32_t));
	if(!track_offsets) return false;
	for(uint32_t i = 0; i < track_count; i++)
		track_offsets[i] = ss_file_read_le32(file,
		                                     track_table_offset + i * 4);

	m->format = 1;
	m->time_division = 0xC0;

	size_t total_tracks = (size_t)track_count + 1;
	m->tracks = (SS_MIDITrack *)calloc(total_tracks, sizeof(SS_MIDITrack));
	if(!m->tracks) {
		free(track_offsets);
		return false;
	}
	m->track_capacity = total_tracks;
	m->track_count = 1;
	m->tracks[0].port = -1;

	/* Conductor: default tempo + EOT.  Loop markers (if any) get added
	 * later when the track parser encounters 0xFE 0x14 / 0xFE 0x15. */
	if(!push_msg(&m->tracks[0], 0, SS_META_SET_TEMPO,
	             HMI_DEFAULT_TEMPO, sizeof(HMI_DEFAULT_TEMPO))) {
		free(track_offsets);
		return false;
	}
	if(!push_msg(&m->tracks[0], 0, SS_META_END_OF_TRACK, NULL, 0)) {
		free(track_offsets);
		return false;
	}

	static const char track_magic[] = "HMI-MIDITRACK";

	for(uint32_t i = 0; i < track_count; i++) {
		uint32_t t_off = track_offsets[i];
		uint32_t t_len;
		if(i + 1 < track_count) {
			if(track_offsets[i + 1] <= t_off) goto fail;
			t_len = track_offsets[i + 1] - t_off;
		} else {
			if(t_off >= size) goto fail;
			t_len = (uint32_t)(size - t_off);
		}
		if((uint64_t)t_off + t_len > size) goto fail;
		if(t_len < 13) goto fail;

		for(int k = 0; k < 13; k++)
			if(ss_file_read_u8(file, t_off + (size_t)k) != (uint8_t)track_magic[k])
				goto fail;

		SS_MIDITrack *track = &m->tracks[m->track_count];
		memset(track, 0, sizeof(*track));
		track->port = -1;

		/* Optional text metadata at meta_offset (track-relative). */
		if(t_len < 0x4B + 4) goto fail;
		uint32_t meta_offset = ss_file_read_le32(file, t_off + 0x4B);
		if(meta_offset != 0 && (uint64_t)meta_offset + 1 < t_len) {
			uint8_t meta_size = ss_file_read_u8(file, t_off + meta_offset + 1);
			if((uint64_t)meta_offset + 2 + (uint64_t)meta_size > t_len) goto fail;
			if(meta_size > 0) {
				uint8_t *data = (uint8_t *)malloc(meta_size);
				if(!data) goto fail;
				ss_file_read_bytes(file, t_off + meta_offset + 2, data, meta_size);
				size_t trimmed = meta_size;
				while(trimmed > 0 && data[trimmed - 1] == ' ') trimmed--;
				if(trimmed > 0) {
					SS_MIDIMessage msg;
					memset(&msg, 0, sizeof(msg));
					msg.ticks = 0;
					msg.status_byte = SS_META_TEXT;
					msg.data = data;
					msg.data_length = trimmed;
					if(!ss_midi_track_push_event(track, msg)) {
						free(data);
						goto fail;
					}
				} else {
					free(data);
				}
			}
		}

		/* Event stream. */
		if(t_len < 0x57 + 4) goto fail;
		uint32_t data_off = ss_file_read_le32(file, t_off + 0x57);
		size_t events_start = (size_t)t_off + data_off;
		size_t events_end = (size_t)t_off + t_len;
		if(events_start > events_end) goto fail;

		if(!parse_hmi_track(file, events_start, events_end, track, &m->tracks[0]))
			goto fail;
		m->track_count++;
	}

	free(track_offsets);
	return true;

fail:
	free(track_offsets);
	return false;
}
