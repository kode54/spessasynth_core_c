/**
 * mids.c
 * Microsoft DirectMusic Segment (MIDS) parser.
 * Port of midi_processor_mids.cpp from midi_processing.
 *
 * Layout:
 *   RIFF  "MIDS"
 *     "fmt " chunk:
 *       uint32 LE  time_division (TPQN; must be non-zero)
 *       uint32 LE  max_buffer (ignored)
 *       uint32 LE  flags (bit 0: 8-byte event records vs 12-byte)
 *     "data" chunk:
 *       uint32 LE  segment_count
 *       segment_count × segment:
 *         uint32 LE  (ignored)
 *         uint32 LE  segment_size
 *         Records packed in segment_size bytes:
 *           uint32 LE  delta (ticks)
 *           [uint32 LE  ignored]  ← only when flags bit 0 is clear
 *           uint32 LE  event:
 *             high byte 0x01 → tempo meta (low 24 bits = µs/beat)
 *             high byte 0x00 → voice message packed as
 *               bits  0- 3 channel
 *               bits  4- 7 status high nibble (8..E)
 *               bits  8-15 data byte 1
 *               bits 16-23 data byte 2 (absent for program/aftertouch)
 */

#include <stdlib.h>
#include <string.h>

#include "parsers.h"

/* ── Detect MIDS magic (RIFF…MIDSfmt ) ───────────────────────────────────── */

bool ss_midi_is_mids(SS_File *file, size_t size) {
	if(size < 16) return false;
	if(ss_file_read_u8(file, 0) != 'R' ||
	   ss_file_read_u8(file, 1) != 'I' ||
	   ss_file_read_u8(file, 2) != 'F' ||
	   ss_file_read_u8(file, 3) != 'F') return false;
	uint32_t riff_size = ss_file_read_le32(file, 4);
	if(riff_size < 8 || size < (size_t)riff_size + 8) return false;
	if(ss_file_read_u8(file, 8) != 'M' ||
	   ss_file_read_u8(file, 9) != 'I' ||
	   ss_file_read_u8(file, 10) != 'D' ||
	   ss_file_read_u8(file, 11) != 'S') return false;
	if(ss_file_read_u8(file, 12) != 'f' ||
	   ss_file_read_u8(file, 13) != 'm' ||
	   ss_file_read_u8(file, 14) != 't' ||
	   ss_file_read_u8(file, 15) != ' ') return false;
	return true;
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
	return ss_midi_track_push_event(track, msg);
}

/* ── Public MIDS parser ──────────────────────────────────────────────────── */

bool ss_midi_parse_mids(SS_MIDIFile *m, SS_File *file, size_t size) {
	if(!ss_midi_is_mids(file, size) || size < 20) return false;

	/* ── "fmt " chunk ────────────────────────────────────────────────────── */
	size_t pos = 16; /* past RIFF header + "MIDS" + "fmt " */
	uint32_t fmt_size = ss_file_read_le32(file, pos);
	pos += 4;
	if(size - pos < fmt_size) return false;

	uint32_t time_division = 1;
	uint32_t flags = 0;
	size_t fmt_end = pos + fmt_size;

	if(fmt_size >= 4) {
		time_division = ss_file_read_le32(file, pos);
		pos += 4;
		fmt_size -= 4;
		if(time_division == 0) return false; /* avoids /0 in tempo math */
	}
	if(fmt_size >= 4) {
		/* max_buffer — unused */
		pos += 4;
		fmt_size -= 4;
	}
	if(fmt_size >= 4) {
		flags = ss_file_read_le32(file, pos);
		pos += 4;
		fmt_size -= 4;
	}

	/* Skip trailing fmt bytes + RIFF pad. */
	pos = fmt_end;
	if(pos >= size) return false;
	if(fmt_end & 1) pos++; /* odd-size pad */

	/* ── "data" chunk ────────────────────────────────────────────────────── */
	if(size - pos < 4) return false;
	if(ss_file_read_u8(file, pos) != 'd' ||
	   ss_file_read_u8(file, pos + 1) != 'a' ||
	   ss_file_read_u8(file, pos + 2) != 't' ||
	   ss_file_read_u8(file, pos + 3) != 'a') return false;
	pos += 4;

	m->format = 0;
	m->time_division = (uint16_t)time_division;

	/* Single track holds tempo meta events + voice events. */
	m->tracks = (SS_MIDITrack *)calloc(1, sizeof(SS_MIDITrack));
	if(!m->tracks) return false;
	m->track_capacity = 1;
	m->track_count = 1;
	SS_MIDITrack *track = &m->tracks[0];
	track->port = -1;

	if(size - pos < 4) return false;
	uint32_t data_size = ss_file_read_le32(file, pos);
	pos += 4;
	size_t body_end = pos + data_size;
	if(body_end > size) body_end = size;

	if(body_end - pos < 4) return false;
	uint32_t segment_count = ss_file_read_le32(file, pos);
	pos += 4;

	bool is_eight_byte = (flags & 1u) != 0;
	size_t current_ticks = 0;

	for(uint32_t i = 0; i < segment_count; i++) {
		if(size - pos < 12) return false;
		pos += 4; /* unused segment header word */
		uint32_t segment_size = ss_file_read_le32(file, pos);
		pos += 4;
		size_t segment_end = pos + segment_size;
		if(segment_end > body_end) segment_end = body_end;

		while(pos < segment_end) {
			if(segment_end - pos < 4) return false;
			uint32_t delta = ss_file_read_le32(file, pos);
			pos += 4;
			current_ticks += delta;

			if(!is_eight_byte) {
				if(segment_end - pos < 4) return false;
				pos += 4; /* unused extra word */
			}

			if(segment_end - pos < 4) return false;
			uint32_t event = ss_file_read_le32(file, pos);
			pos += 4;

			uint8_t tag = (uint8_t)(event >> 24);
			if(tag == 0x01) {
				/* Tempo meta: low 24 bits are µs/beat, big-endian order. */
				uint8_t tempo[3];
				tempo[0] = (uint8_t)(event >> 16);
				tempo[1] = (uint8_t)(event >> 8);
				tempo[2] = (uint8_t)event;
				if(!push_msg(track, current_ticks, SS_META_SET_TEMPO,
				             tempo, sizeof(tempo))) return false;
			} else if(tag == 0x00) {
				/* Voice message packed into 32-bit word. */
				uint8_t status = (uint8_t)(event & 0xFF);
				uint8_t high_nibble = (uint8_t)((status & 0xF0) >> 4);
				if(high_nibble < 0x8 || high_nibble > 0xE) continue;

				uint8_t data[2];
				size_t data_len = 1;
				data[0] = (uint8_t)(event >> 8);
				if(high_nibble != 0xC && high_nibble != 0xD) {
					data[1] = (uint8_t)(event >> 16);
					data_len = 2;
				}
				if(!push_msg(track, current_ticks, status, data, data_len))
					return false;
			}
			/* Other tags silently ignored, matching reference. */
		}
	}

	/* End-of-track meta closes the single track. */
	return push_msg(track, current_ticks, SS_META_END_OF_TRACK, NULL, 0);
}
