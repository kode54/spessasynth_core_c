/**
 * xmi.c
 * Miles Sound System XMI (Extended MIDI) parser.
 * Port of midi_processor_xmi.cpp from midi_processing.
 *
 * Container: IFF (big-endian chunk sizes).
 *   FORM XDIR        (metadata — ignored)
 *   CAT  XMID        (collection of tracks)
 *     FORM XMID      (per track, repeated)
 *       EVNT         (event stream for this track)
 *       (TIMB, other chunks — ignored)
 *
 * Event stream:
 *   - Delta: sum of consecutive bytes < 0x80 (capped when the next byte
 *     is a status byte >= 0x80).
 *   - 0xFF meta: meta_type + (VLQ length + data) unless type == 0x2F.
 *     Tempo (0x51) values are rescaled: ppqn = tempo * 3 / 25000,
 *     tempo' = tempo * 60 / ppqn, which normalizes XMI's fixed 120 Hz
 *     / 60 TPQN clock to a standard 500000 µs/beat meta event.
 *   - 0xF0 sysex: VLQ length + data.
 *   - 0x80..0xEF voice: 1 data byte (program/aftertouch) or 2.  Note-on
 *     (0x9x) is followed by a VLQ duration; a matching note-off is
 *     synthesized at current_ticks + duration.
 *
 * Multi-track XMI files map to SMF format 2.  Our loader's format-2
 * convention concatenates tracks, so events are offset by the previous
 * track's tail tick.
 */

#include <stdlib.h>
#include <string.h>

#include "parsers.h"

static const uint8_t XMI_DEFAULT_TEMPO[3] = { 0x07, 0xA1, 0x20 }; /* 500000 µs/beat */

/* ── Detect XMI magic (FORM XDIR ... CAT  XMID) ──────────────────────────── */

bool ss_midi_is_xmi(SS_File *file, size_t size) {
	if(size < 0x22) return false;
	if(ss_file_read_u8(file, 0) != 'F' ||
	   ss_file_read_u8(file, 1) != 'O' ||
	   ss_file_read_u8(file, 2) != 'R' ||
	   ss_file_read_u8(file, 3) != 'M') return false;
	if(ss_file_read_u8(file, 8) != 'X' ||
	   ss_file_read_u8(file, 9) != 'D' ||
	   ss_file_read_u8(file, 10) != 'I' ||
	   ss_file_read_u8(file, 11) != 'R') return false;
	if(ss_file_read_u8(file, 0x1E) != 'X' ||
	   ss_file_read_u8(file, 0x1F) != 'M' ||
	   ss_file_read_u8(file, 0x20) != 'I' ||
	   ss_file_read_u8(file, 0x21) != 'D') return false;
	return true;
}

/* ── Push a typed message onto the track (takes ownership of allocated data) */

static bool push_event(SS_MIDITrack *track, size_t ticks, uint8_t status_byte,
                       uint8_t *data, size_t data_len) {
	SS_MIDIMessage msg;
	memset(&msg, 0, sizeof(msg));
	msg.ticks = ticks;
	msg.status_byte = status_byte;
	msg.data = data;
	msg.data_length = data_len;
	if(!ss_midi_track_push_event(track, msg)) {
		free(data);
		return false;
	}
	return true;
}

/* ── Read an IFF chunk header at pos (no advancement) ────────────────────── */

static bool read_iff_hdr(SS_File *file, size_t pos, size_t end,
                         char id_out[4], uint32_t *size_out) {
	if(end - pos < 8) return false;
	for(int i = 0; i < 4; i++)
		id_out[i] = (char)ss_file_read_u8(file, pos + i);
	*size_out = (uint32_t)ss_file_read_be(file, pos + 4, 4);
	return true;
}

/* ── Parse one EVNT body into a new track at start_ticks ─────────────────── */

static bool parse_xmi_track(SS_MIDIFile *m, SS_File *file, size_t evt_start,
                            size_t evt_size, size_t start_ticks,
                            size_t *end_ticks_out) {
	SS_MIDITrack *track = &m->tracks[m->track_count++];
	memset(track, 0, sizeof(*track));
	track->port = -1;

	size_t pos = evt_start;
	size_t end = evt_start + evt_size;
	size_t current_ticks = start_ticks;
	size_t last_event_ticks = start_ticks;
	bool initial_tempo_set = false;

	while(pos < end) {
		/* Delta: sum of bytes < 0x80 until the status byte appears. */
		uint32_t delta = 0;
		while(pos < end) {
			uint8_t b = ss_file_read_u8(file, pos);
			if(b & 0x80) break;
			delta += b;
			pos++;
		}
		current_ticks += delta;
		if(current_ticks > last_event_ticks) last_event_ticks = current_ticks;

		if(pos >= end) break;
		uint8_t status = ss_file_read_u8(file, pos++);

		if(status == 0xFF) {
			/* Meta event. */
			if(pos >= end) return false;
			uint8_t meta_type = ss_file_read_u8(file, pos++);
			size_t meta_len = 0;

			if(meta_type != 0x2F) {
				meta_len = ss_file_read_vlq(file, pos);
				pos = ss_file_tell(file);
				if(end - pos < meta_len) return false;
			}

			uint8_t *data = NULL;
			if(meta_len > 0) {
				data = (uint8_t *)malloc(meta_len);
				if(!data) return false;
				for(size_t k = 0; k < meta_len; k++)
					data[k] = ss_file_read_u8(file, pos + k);

				/* XMI tempo normalization. */
				if(meta_type == 0x51 && meta_len == 3) {
					uint32_t t = ((uint32_t)data[0] << 16) |
					             ((uint32_t)data[1] << 8) |
					             (uint32_t)data[2];
					uint32_t ppqn = (t * 3u) / 25000u;
					if(ppqn > 0) {
						uint32_t new_tempo = (t * 60u) / ppqn;
						data[0] = (uint8_t)(new_tempo >> 16);
						data[1] = (uint8_t)(new_tempo >> 8);
						data[2] = (uint8_t)new_tempo;
					}
					if(current_ticks == start_ticks) initial_tempo_set = true;
				}
			}
			pos += meta_len;

			/* EOT slides forward to cover pending note-offs. */
			if(meta_type == 0x2F && current_ticks < last_event_ticks)
				current_ticks = last_event_ticks;

			if(!push_event(track, current_ticks, meta_type, data, meta_len))
				return false;

			if(meta_type == 0x2F) break;

		} else if(status == 0xF0) {
			/* SysEx (continuation-capable via 0xF7 not separately handled). */
			size_t sx_len = ss_file_read_vlq(file, pos);
			pos = ss_file_tell(file);
			if(end - pos < sx_len) return false;
			uint8_t *data = NULL;
			if(sx_len > 0) {
				data = (uint8_t *)malloc(sx_len);
				if(!data) return false;
				for(size_t k = 0; k < sx_len; k++)
					data[k] = ss_file_read_u8(file, pos + k);
			}
			pos += sx_len;
			if(!push_event(track, current_ticks, status, data, sx_len))
				return false;

		} else if(status >= 0x80 && status <= 0xEF) {
			/* Voice message. */
			uint8_t type_nibble = (uint8_t)(status >> 4);
			if(pos >= end) return false;
			uint8_t d1 = ss_file_read_u8(file, pos++);
			uint8_t d2 = 0;
			uint8_t data_len = 1;
			if(type_nibble != 0xC && type_nibble != 0xD) {
				if(pos >= end) return false;
				d2 = ss_file_read_u8(file, pos++);
				data_len = 2;
			}

			uint8_t *data = (uint8_t *)malloc(data_len);
			if(!data) return false;
			data[0] = d1;
			if(data_len == 2) data[1] = d2;
			if(!push_event(track, current_ticks, status, data, data_len))
				return false;

			/* Note-on: XMI stores duration; we synthesize the matching note-off. */
			if(type_nibble == 0x9) {
				size_t duration = ss_file_read_vlq(file, pos);
				pos = ss_file_tell(file);
				size_t note_end = current_ticks + duration;
				if(note_end > last_event_ticks) last_event_ticks = note_end;

				uint8_t *off_data = (uint8_t *)malloc(2);
				if(!off_data) return false;
				off_data[0] = d1;
				off_data[1] = 0; /* vel 0 ≡ note-off */
				if(!push_event(track, note_end, status, off_data, 2))
					return false;
			}

		} else {
			return false; /* Unexpected status byte */
		}
	}

	/* Seed the default tempo at track start if the source didn't. */
	if(!initial_tempo_set) {
		uint8_t *data = (uint8_t *)malloc(sizeof(XMI_DEFAULT_TEMPO));
		if(!data) return false;
		memcpy(data, XMI_DEFAULT_TEMPO, sizeof(XMI_DEFAULT_TEMPO));
		if(!push_event(track, start_ticks, SS_META_SET_TEMPO,
		               data, sizeof(XMI_DEFAULT_TEMPO)))
			return false;
	}

	if(end_ticks_out) *end_ticks_out = last_event_ticks;
	return true;
}

/* ── Count FORM XMID sub-chunks within a CAT XMID body ───────────────────── */

static size_t count_forms(SS_File *file, size_t pos, size_t end) {
	size_t n = 0;
	while(pos + 8 <= end) {
		char id[4];
		uint32_t sz;
		if(!read_iff_hdr(file, pos, end, id, &sz)) break;
		pos += 8;
		if(sz > end - pos) sz = (uint32_t)(end - pos);
		if(memcmp(id, "FORM", 4) == 0) n++;
		pos += sz;
		if((sz & 1u) && pos < end) pos++;
	}
	return n;
}

/* ── Public XMI parser ───────────────────────────────────────────────────── */

bool ss_midi_parse_xmi(SS_MIDIFile *m, SS_File *file, size_t size) {
	if(!ss_midi_is_xmi(file, size)) return false;

	/* ── Top-level FORM XDIR: read and skip entirely. ────────────────────── */
	char id[4];
	uint32_t sz;
	size_t pos = 0;
	if(!read_iff_hdr(file, pos, size, id, &sz)) return false;
	if(memcmp(id, "FORM", 4) != 0) return false;
	pos += 8;
	size_t form_end = pos + sz;
	if(form_end > size) form_end = size;
	pos = form_end;
	if((sz & 1u) && pos < size) pos++;

	/* ── Top-level CAT  XMID ──────────────────────────────────────────────── */
	if(!read_iff_hdr(file, pos, size, id, &sz)) return false;
	if(memcmp(id, "CAT ", 4) != 0) return false;
	pos += 8;
	size_t cat_end = pos + sz;
	if(cat_end > size) cat_end = size;
	if(cat_end - pos < 4) return false;
	char type[4];
	for(int i = 0; i < 4; i++)
		type[i] = (char)ss_file_read_u8(file, pos + i);
	if(memcmp(type, "XMID", 4) != 0) return false;
	pos += 4;

	size_t track_count = count_forms(file, pos, cat_end);
	if(track_count == 0) return false;

	m->format = (track_count > 1) ? 2 : 0;
	m->time_division = 60;
	m->tracks = (SS_MIDITrack *)calloc(track_count, sizeof(SS_MIDITrack));
	if(!m->tracks) return false;
	m->track_capacity = track_count;

	/* ── Iterate each FORM XMID, find its EVNT, parse events. ─────────────── */
	size_t start_ticks = 0;
	size_t scan = pos;
	while(scan + 8 <= cat_end) {
		if(!read_iff_hdr(file, scan, cat_end, id, &sz)) break;
		size_t body = scan + 8;
		size_t body_end = body + sz;
		if(body_end > cat_end) body_end = cat_end;
		size_t next = body_end + ((sz & 1u) ? 1 : 0);

		if(memcmp(id, "FORM", 4) != 0) {
			scan = next;
			continue;
		}

		/* FORM type word. */
		if(body_end - body < 4) return false;
		for(int i = 0; i < 4; i++)
			type[i] = (char)ss_file_read_u8(file, body + i);
		if(memcmp(type, "XMID", 4) != 0) return false;
		size_t sub = body + 4;

		/* Find EVNT sub-chunk. */
		size_t evt_start = 0;
		uint32_t evt_size = 0;
		while(sub + 8 <= body_end) {
			char sid[4];
			uint32_t ssz;
			if(!read_iff_hdr(file, sub, body_end, sid, &ssz)) break;
			sub += 8;
			if(ssz > body_end - sub) ssz = (uint32_t)(body_end - sub);
			if(memcmp(sid, "EVNT", 4) == 0) {
				evt_start = sub;
				evt_size = ssz;
				break;
			}
			sub += ssz;
			if((ssz & 1u) && sub < body_end) sub++;
		}
		if(evt_start == 0) return false;

		size_t end_ticks = start_ticks;
		if(!parse_xmi_track(m, file, evt_start, evt_size, start_ticks, &end_ticks))
			return false;
		start_ticks = end_ticks;

		scan = next;
	}

	return true;
}
