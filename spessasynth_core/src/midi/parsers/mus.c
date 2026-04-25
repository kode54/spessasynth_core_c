/**
 * mus.c
 * DOOM/Heretic MUS (Music) format parser.
 * Port of midi_processor_mus.cpp from midi_processing.
 *
 * Layout:
 *   [ 0]  "MUS\x1A" magic
 *   [ 4]  uint16 LE  score length (bytes of event stream)
 *   [ 6]  uint16 LE  score offset (from start of file to event stream)
 *   [ 8]  uint16 LE  primary channel count
 *   [10]  uint16 LE  secondary channel count
 *   [12]  uint16 LE  instrument (patch) count
 *   [14]  uint16 LE  reserved
 *   [16]  uint16 LE  instrument list ([instrument_count] entries)
 *
 * Event byte bits:
 *   7     delta time follows (VLQ)
 *   6-4   event type
 *   3-0   MUS channel (15 → MIDI 9 drums; 9..14 → MIDI 10..15)
 *
 * Event types:
 *   0  note release           [note]
 *   1  note play              [note | 0x80? velocity]
 *   2  pitch wheel            [8-bit wheel]
 *   3  system event           [mode controller 10..14]
 *   4  controller change      [cc, value] or program change [0, program]
 *   6  score end (byte == 0x60)
 */

#include <stdlib.h>
#include <string.h>

#include "parsers.h"

/* MUS controller index → MIDI controller number.
 *   0..9  → CC change (index 0 is unused — program change uses a different path)
 *   10..14→ mode-change CCs (120, 123, 126, 127, 121) */
static const uint8_t MUS_CONTROLLERS[15] = {
	0, 0, 1, 7, 10, 11, 91, 93, 64, 67, 120, 123, 126, 127, 121
};

/* Default MUS tempo: 0x09A31A µs/beat ≈ 95.08 BPM, paired with TPQN 89
 * to approximate the 140 Hz DOOM playback rate. */
static const uint8_t MUS_DEFAULT_TEMPO[3] = { 0x09, 0xA3, 0x1A };
static const uint16_t MUS_TIME_DIVISION = 89; /* 0x59 */

/* ── Detect MUS magic ────────────────────────────────────────────────────── */

static bool mus_check_header(SS_File *file, size_t size) {
	if(size < 0x20) return false;
	if(ss_file_read_u8(file, 0) != 'M' ||
	   ss_file_read_u8(file, 1) != 'U' ||
	   ss_file_read_u8(file, 2) != 'S' ||
	   ss_file_read_u8(file, 3) != 0x1A) return false;

	uint16_t length = ss_file_read_le16(file, 4);
	uint16_t offset = ss_file_read_le16(file, 6);
	uint16_t instrument_count = ss_file_read_le16(file, 12);

	/* Offset must point past the instrument list and the combined
	 * score+offset region must fit in the file. */
	size_t min_off = (size_t)16 + (size_t)instrument_count * 2;
	size_t max_off = (size_t)16 + (size_t)instrument_count * 4;
	if(offset < min_off || offset >= max_off) return false;
	if((size_t)offset + (size_t)length > size) return false;
	return true;
}

bool ss_midi_is_mus(SS_File *file, size_t size) {
	return mus_check_header(file, size);
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

/* ── Public MUS parser ───────────────────────────────────────────────────── */

bool ss_midi_parse_mus(SS_MIDIFile *m, SS_File *file, size_t size) {
	if(!mus_check_header(file, size)) return false;

	uint16_t length = ss_file_read_le16(file, 4);
	uint16_t offset = ss_file_read_le16(file, 6);

	m->format = 0;
	m->time_division = MUS_TIME_DIVISION;

	/* Single track: tempo meta at tick 0, then voice events, then end-of-track. */
	m->tracks = (SS_MIDITrack *)calloc(1, sizeof(SS_MIDITrack));
	if(!m->tracks) return false;
	m->track_capacity = 1;
	m->track_count = 1;
	SS_MIDITrack *track = &m->tracks[0];
	track->port = -1;

	/* Set-tempo meta event. */
	if(!push_msg(track, 0, SS_META_SET_TEMPO,
	             MUS_DEFAULT_TEMPO, sizeof(MUS_DEFAULT_TEMPO)))
		return false;

	/* Slice the event stream so VLQ reads can't overrun into trailing data. */
	SS_File *evt = ss_file_slice(file, offset, length);
	if(!evt) return false;

	size_t pos = 0;
	size_t end = length;
	size_t current_ticks = 0;
	uint8_t velocity_levels[16] = { 0 };
	bool ok = true;

	while(pos < end) {
		uint8_t ev = ss_file_read_u8(evt, pos++);
		/* Score end marker (exact match, like the reference implementation). */
		if(ev == 0x60) break;

		uint8_t channel = ev & 0x0F;
		/* MUS drum channel (15) → MIDI 9; MIDI 9 slot is shifted up. */
		if(channel == 0x0F)
			channel = 9;
		else if(channel >= 9)
			channel++;

		uint8_t data[2];
		uint8_t data_len = 0;
		uint8_t status_byte = 0;

		switch(ev & 0x70) {
			case 0x00: { /* Note release → note-on with velocity 0 */
				if(pos >= end) {
					ok = false;
					break;
				}
				data[0] = ss_file_read_u8(evt, pos++);
				data[1] = 0;
				data_len = 2;
				status_byte = (uint8_t)(0x90 | channel);
				break;
			}

			case 0x10: { /* Note play */
				if(pos >= end) {
					ok = false;
					break;
				}
				uint8_t note = ss_file_read_u8(evt, pos++);
				uint8_t vel;
				if(note & 0x80) {
					if(pos >= end) {
						ok = false;
						break;
					}
					vel = ss_file_read_u8(evt, pos++);
					velocity_levels[channel] = vel;
					note &= 0x7F;
				} else {
					vel = velocity_levels[channel];
				}
				data[0] = note;
				data[1] = vel;
				data_len = 2;
				status_byte = (uint8_t)(0x90 | channel);
				break;
			}

			case 0x20: { /* Pitch wheel: 8-bit MUS → 14-bit MIDI */
				if(pos >= end) {
					ok = false;
					break;
				}
				uint8_t w = ss_file_read_u8(evt, pos++);
				/* Scale by 64: MIDI_value = w * 64, so 0x80 → 0x2000 (center).
				 * LSB = (w << 6) & 0x7F carries only w's bit 0 into bit 6. */
				data[0] = (uint8_t)((w << 6) & 0x7F);
				data[1] = (uint8_t)(w >> 1);
				data_len = 2;
				status_byte = (uint8_t)(0xE0 | channel);
				break;
			}

			case 0x30: { /* System/mode-change controller (10..14) */
				if(pos >= end) {
					ok = false;
					break;
				}
				uint8_t cc = ss_file_read_u8(evt, pos++);
				if(cc < 10 || cc > 14) {
					ok = false;
					break;
				}
				data[0] = MUS_CONTROLLERS[cc];
				data[1] = 1;
				data_len = 2;
				status_byte = (uint8_t)(0xB0 | channel);
				break;
			}

			case 0x40: { /* Controller change (cc=1..9) or program change (cc=0) */
				if(pos >= end) {
					ok = false;
					break;
				}
				uint8_t cc = ss_file_read_u8(evt, pos++);
				if(cc == 0) {
					if(pos >= end) {
						ok = false;
						break;
					}
					data[0] = ss_file_read_u8(evt, pos++);
					data_len = 1;
					status_byte = (uint8_t)(0xC0 | channel);
				} else if(cc < 10) {
					if(pos >= end) {
						ok = false;
						break;
					}
					data[0] = MUS_CONTROLLERS[cc];
					data[1] = ss_file_read_u8(evt, pos++);
					data_len = 2;
					status_byte = (uint8_t)(0xB0 | channel);
				} else {
					ok = false;
				}
				break;
			}

			default:
				ok = false;
				break;
		}

		if(!ok) break;

		if(!push_msg(track, current_ticks, status_byte, data, data_len)) {
			ok = false;
			break;
		}

		/* Delta-time follows when bit 7 of the event byte is set. */
		if(ev & 0x80) {
			size_t before = pos;
			current_ticks += ss_file_read_vlq(evt, pos);
			pos = ss_file_tell(evt);
			if(pos <= before) { /* VLQ made no progress — corrupt */
				ok = false;
				break;
			}
		}
	}

	ss_file_close(evt);

	if(!ok) return false;

	/* End-of-track meta event closes the single track. */
	return push_msg(track, current_ticks, SS_META_END_OF_TRACK, NULL, 0);
}
