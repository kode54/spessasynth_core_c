/**
 * hmp.c
 * HMI Sound Operating System (HMP/HMIMIDI) parser.
 * Port of midi_processor_hmp.cpp from midi_processing.
 *
 * Two sub-variants sharing the same magic prefix:
 *   HMIMIDIP  — "classic" HMP: track count at 0x30, TPQN forced to 0xC0,
 *               32-bit track sizes (-12 body adjust), 4-byte post-track pad.
 *   HMIMIDIR  — "funky" variant: track count at 0x1A, TPQN from 0x4C/0x4D
 *               (a sparse 24-bit field), 16-bit track sizes (-4 adjust),
 *               no post-track pad.
 *
 * Emits an SMF format-1 file.  Track 0 is a conductor containing the
 * default HMP tempo (0x188000 µs/beat at TPQN 0xC0 ≈ 120 ticks/sec) and
 * an EOT.  Tracks 1..N-1 parse the HMP event stream, which uses:
 *   - A custom little-endian delta encoding (7-bit groups, LSB-first;
 *     byte with bit 7 SET terminates — the inverse of MIDI VLQ).
 *   - Explicit status byte per event (no running status).
 *   - Meta events with a standard MIDI VLQ length.  Voice messages with
 *     1 or 2 data bytes.  No SysEx support.
 */

#include <stdlib.h>
#include <string.h>

#include "parsers.h"

/* Default conductor tempo: 0x188000 µs/beat paired with TPQN 0xC0. */
static const uint8_t HMP_DEFAULT_TEMPO[3] = { 0x18, 0x80, 0x00 };

/* ── Detect HMP magic (HMIMIDI[PR]) ──────────────────────────────────────── */

bool ss_midi_is_hmp(SS_File *file, size_t size) {
	if(size < 8) return false;
	static const char magic[] = "HMIMIDI";
	for(int i = 0; i < 7; i++)
		if(ss_file_read_u8(file, (size_t)i) != (uint8_t)magic[i]) return false;
	uint8_t last = ss_file_read_u8(file, 7);
	return last == 'P' || last == 'R';
}

/* ── Push a typed message onto the track with optional data bytes ────────── */

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

/* ── HMP delta decoder ───────────────────────────────────────────────────── */

static size_t hmp_read_delta(SS_File *file, size_t *pos, size_t end) {
	size_t delta = 0;
	unsigned shift = 0;
	while(*pos < end) {
		uint8_t b = ss_file_read_u8(file, (*pos)++);
		delta += ((size_t)(b & 0x7F)) << shift;
		shift += 7;
		if(b & 0x80) break; /* High bit SET terminates (opposite of MIDI VLQ) */
	}
	return delta;
}

/* ── Parse one HMP track body into an existing SS_MIDITrack ──────────────── */

static bool parse_hmp_track(SS_File *file, size_t start, size_t end,
                            SS_MIDITrack *track) {
	size_t pos = start;
	size_t abs_tick = 0;

	while(pos < end) {
		abs_tick += hmp_read_delta(file, &pos, end);
		if(pos >= end) return false;

		uint8_t status = ss_file_read_u8(file, pos++);

		if(status == 0xFF) {
			/* Meta event. */
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
		} else if(status >= 0x80 && status <= 0xEF) {
			/* Voice message with explicit status (no running status in HMP). */
			uint8_t type = status & 0xF0;
			size_t data_bytes = (type == 0xC0 || type == 0xD0) ? 1 : 2;
			if(end - pos < data_bytes) return false;

			uint8_t *data = (uint8_t *)malloc(data_bytes);
			if(!data) return false;
			ss_file_read_bytes(file, pos, data, data_bytes);
			pos += data_bytes;

			SS_MIDIMessage msg;
			memset(&msg, 0, sizeof(msg));
			msg.ticks = abs_tick;
			msg.status_byte = status;
			msg.data = data;
			msg.data_length = (uint32_t)data_bytes;
			if(!ss_midi_track_push_event(track, msg)) {
				free(data);
				return false;
			}
		} else {
			return false; /* Unexpected status code in HMP track */
		}
	}

	return true;
}

/* ── Public HMP parser ───────────────────────────────────────────────────── */

bool ss_midi_parse_hmp(SS_MIDIFile *m, SS_File *file, size_t size) {
	if(!ss_midi_is_hmp(file, size)) return false;

	bool is_funky = ss_file_read_u8(file, 7) == 'R';
	size_t hdr_off = is_funky ? 0x1A : 0x30;
	if(hdr_off >= size) return false;

	uint8_t track_count_8 = ss_file_read_u8(file, hdr_off);
	if(track_count_8 == 0) return false;

	uint16_t dtx = 0xC0;
	if(is_funky) {
		if(size <= 0x4D) return false;
		/* Reference reads this as a sparse 24-bit field with the middle
		 * byte implicitly zero; preserve the same shape. */
		dtx = (uint16_t)(((uint32_t)ss_file_read_u8(file, 0x4C) << 16) |
		                 (uint32_t)ss_file_read_u8(file, 0x4D));
		if(dtx == 0) return false;
	}

	m->format = 1;
	m->time_division = dtx;

	m->tracks = (SS_MIDITrack *)calloc(track_count_8, sizeof(SS_MIDITrack));
	if(!m->tracks) return false;
	m->track_capacity = track_count_8;
	m->track_count = 1; /* grows as tracks are parsed */
	m->tracks[0].port = -1;

	/* Track 0 — conductor: default tempo + EOT. */
	if(!push_msg(&m->tracks[0], 0, SS_META_SET_TEMPO,
	             HMP_DEFAULT_TEMPO, sizeof(HMP_DEFAULT_TEMPO)))
		return false;
	if(!push_msg(&m->tracks[0], 0, SS_META_END_OF_TRACK, NULL, 0))
		return false;

	/* Scan past the first HMP "track" header by finding 0xFF 0x2F.
	 * The match starts at hdr_off (reading the track-count byte itself
	 * as the first candidate), matching the reference's two-byte window
	 * slide. */
	size_t pos = hdr_off;
	uint8_t prev = 0;
	bool found = false;
	while(pos < size) {
		uint8_t b = ss_file_read_u8(file, pos++);
		if(prev == 0xFF && b == 0x2F) {
			found = true;
			break;
		}
		prev = b;
	}
	if(!found) return false;

	size_t post_hdr_skip = is_funky ? 3 : 5;
	if(size - pos < post_hdr_skip) return false;
	pos += post_hdr_skip;

	/* Subsequent tracks. */
	for(uint8_t i = 1; i < track_count_8; i++) {
		size_t track_body_len;
		size_t pre_skip;

		if(is_funky) {
			if(size - pos < 4) break;
			uint16_t sz16 = (uint16_t)ss_file_read_le(file, pos, 2);
			pos += 2;
			if(sz16 < 4) return false;
			track_body_len = (size_t)sz16 - 4;
			pre_skip = 2;
		} else {
			if(size - pos < 8) break;
			uint32_t sz32 = (uint32_t)ss_file_read_le(file, pos, 4);
			pos += 4;
			if(sz32 < 12) return false;
			track_body_len = (size_t)sz32 - 12;
			pre_skip = 4;
		}

		if(size - pos < track_body_len + pre_skip) break;
		pos += pre_skip;

		size_t track_end = pos + track_body_len;

		SS_MIDITrack *track = &m->tracks[m->track_count];
		memset(track, 0, sizeof(*track));
		track->port = -1;

		if(!parse_hmp_track(file, pos, track_end, track)) return false;
		m->track_count++;

		pos = track_end + (is_funky ? 0 : 4);
	}

	return true;
}
