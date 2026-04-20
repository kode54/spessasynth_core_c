/**
 * gmf.c
 * General MIDI Format (GMF) parser.
 * Port of midi_processor_gmf.cpp from midi_processing.
 *
 * Layout:
 *   [0..3]  "GMF\x01" magic
 *   [4..5]  uint16 BE tempo (multiplied by 100000 → µs/beat)
 *   [6]     unused
 *   [7..]   raw MIDI event stream (like an MTrk body, no chunk header)
 *
 * Produces a format-0 file with two tracks:
 *   Track 0  tempo meta + a Roland GS-style reset SysEx + EOT
 *   Track 1  parsed event stream (pitch-wheel events are dropped, matching
 *            the reference C++ port's is_gmf behavior)
 */

#include <stdlib.h>
#include <string.h>

#include "parsers.h"

/* Reset SysEx emitted on the conductor track (F0 and payload; F7 closes
 * the message).  Kept verbatim from the reference port. */
static const uint8_t GMF_RESET_SYSEX[] = {
	0x41, 0x10, 0x16, 0x12, 0x7F, 0x00, 0x00, 0x01, 0xF7
};

/* ── Detect GMF magic ────────────────────────────────────────────────────── */

bool ss_midi_is_gmf(SS_File *file, size_t size) {
	if(size < 32) return false;
	return ss_file_read_u8(file, 0) == 'G' &&
	       ss_file_read_u8(file, 1) == 'M' &&
	       ss_file_read_u8(file, 2) == 'F' &&
	       ss_file_read_u8(file, 3) == 1;
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

/* ── Public GMF parser ───────────────────────────────────────────────────── */

extern bool ss_midi_smf_parse_track(SS_MIDITrack *track, size_t track_index, SS_File *file, size_t start_ticks);

bool ss_midi_parse_gmf(SS_MIDIFile *m, SS_File *file, size_t size) {
	if(!ss_midi_is_gmf(file, size)) return false;

	uint32_t tempo_raw = ((uint32_t)ss_file_read_u8(file, 4) << 8) |
	                     (uint32_t)ss_file_read_u8(file, 5);
	uint32_t tempo_us = tempo_raw * 100000u;

	m->format = 0;
	m->time_division = 0xC0; /* 192 TPQN */

	m->tracks = (SS_MIDITrack *)calloc(2, sizeof(SS_MIDITrack));
	if(!m->tracks) return false;
	m->track_capacity = 2;
	m->track_count = 2;
	m->tracks[0].port = -1;
	m->tracks[1].port = -1;

	/* Track 0 — conductor: tempo meta, reset SysEx, EOT. */
	uint8_t tempo_bytes[3] = {
		(uint8_t)(tempo_us >> 16),
		(uint8_t)(tempo_us >> 8),
		(uint8_t)tempo_us
	};
	if(!push_msg(&m->tracks[0], 0, SS_META_SET_TEMPO, tempo_bytes, 3))
		return false;
	if(!push_msg(&m->tracks[0], 0, 0xF0,
	             GMF_RESET_SYSEX, sizeof(GMF_RESET_SYSEX)))
		return false;
	if(!push_msg(&m->tracks[0], 0, SS_META_END_OF_TRACK, NULL, 0))
		return false;

	/* Track 1 — event stream from offset 7 to end. */
	SS_File *body = ss_file_slice(file, 7, size - 7);
	if(!body) return false;
	bool ok = ss_midi_smf_parse_track(&m->tracks[1], 1, body, 0);
	ss_file_close(body);
	return ok;
}
