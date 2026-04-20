/**
 * smf.c
 * Standard MIDI File (SMF) parser.  Handles MThd/MTrk chunks and
 * format 0/1/2 track layouts.
 */

#include <stdlib.h>
#include <string.h>

#include "parsers.h"

/* ── Data-bytes-per-message-type table ───────────────────────────────────── */
/* Indexed by (status >> 4), values 8..E */
static const int DATA_BYTES[16] = {
	0, 0, 0, 0, 0, 0, 0, 0, /* 0-7: unused */
	2, /* 8x note off        */
	2, /* 9x note on         */
	2, /* Ax poly pressure   */
	2, /* Bx controller      */
	1, /* Cx program change  */
	1, /* Dx channel pressure*/
	2, /* Ex pitch wheel     */
	0 /* Fx sys / meta      */
};

/* ── Parse a single MTrk chunk ───────────────────────────────────────────── */

static bool parse_track(SS_MIDIFile *m, SS_File *file, size_t start_ticks) {
	size_t track_index = m->track_count;
	SS_MIDITrack *track = &m->tracks[m->track_count++];
	memset(track, 0, sizeof(*track));
	track->port = -1;

	size_t buf_len = ss_file_remaining(file);

	size_t pos = 0;
	size_t abs_tick = start_ticks;
	uint8_t running = 0; /* running status byte */

	while(pos < buf_len) {
		/* Delta time */
		abs_tick += ss_file_read_vlq(file, pos);
		pos = ss_file_tell(file);

		if(pos >= buf_len) break;

		uint8_t status_check = ss_file_read_u8(file, pos);
		uint8_t status_byte;

		if(status_check >= 0x80) {
			status_byte = status_check;
			pos++;

			if(status_byte == 0xFF) {
				/* Meta event: next byte is the meta type */
				if(pos >= buf_len) break;
				uint8_t meta_type = ss_file_read_u8(file, pos++);
				size_t meta_len = ss_file_read_vlq(file, pos);
				pos = ss_file_tell(file);

				SS_MIDIMessage msg;
				msg.ticks = abs_tick;
				msg.status_byte = meta_type;
				msg.track_index = (uint16_t)track_index;
				msg.data_length = meta_len;
				msg.data = NULL;

				if(meta_len > 0 && pos + meta_len <= buf_len) {
					msg.data = (uint8_t *)malloc(meta_len);
					if(msg.data) {
						ss_file_read_bytes(file, pos, msg.data, meta_len);
					}
				}
				pos += meta_len;
				ss_midi_track_push_event(track, msg);
				/* Don't update running status for meta events */
				continue;

			} else if(status_byte == 0xF0 || status_byte == 0xF7) {
				/* SysEx */
				size_t slen = ss_file_read_vlq(file, pos);
				pos = ss_file_tell(file);
				SS_MIDIMessage msg;
				msg.ticks = abs_tick;
				msg.status_byte = status_byte;
				msg.track_index = (uint16_t)track_index;
				msg.data_length = slen;
				msg.data = NULL;
				if(slen > 0 && pos + slen <= buf_len) {
					msg.data = (uint8_t *)malloc(slen);
					if(msg.data) {
						ss_file_read_bytes(file, pos, msg.data, slen);
					}
				}
				pos += slen;
				ss_midi_track_push_event(track, msg);
				running = 0; /* clear running status */
				continue;

			} else {
				/* Voice message — update running status */
				running = status_byte;
			}
		} else {
			/* Use running status */
			if(running == 0) {
				/* No running status — corrupt file, skip */
				continue;
			}
			status_byte = running;
			/* Do NOT advance pos — the byte at pos is data */
		}

		/* Voice message data */
		int data_bytes = DATA_BYTES[(status_byte >> 4) & 0xF];
		if(pos + (size_t)data_bytes > buf_len) break;

		SS_MIDIMessage msg;
		msg.ticks = abs_tick;
		msg.status_byte = status_byte;
		msg.track_index = (uint16_t)track_index;
		msg.data_length = (uint32_t)data_bytes;
		msg.data = NULL;

		if(data_bytes > 0) {
			msg.data = (uint8_t *)malloc((size_t)data_bytes);
			if(msg.data) {
				ss_file_read_bytes(file, pos, msg.data, data_bytes);
			}
		}
		pos += (size_t)data_bytes;
		ss_midi_track_push_event(track, msg);
	}

	return true;
}

/* ── Public SMF parser ───────────────────────────────────────────────────── */

bool ss_midi_parse_smf(SS_MIDIFile *m, SS_File *smf_data, size_t smf_size) {
	size_t pos = 0;

	/* MThd */
	char mthd_id[5];
	if(pos + 8 > smf_size) return false;
	ss_file_read_string(smf_data, 0, mthd_id, 4);
	if(strcmp(mthd_id, "MThd") != 0) return false;
	size_t hdr_size = ss_file_read_be(smf_data, 4, 4);
	pos += 8;
	if(hdr_size < 6 || pos + 6 > smf_size) return false;

	m->format = ss_file_read_u8(smf_data, pos + 1); /* low byte of big-endian uint16 */
	uint16_t n_tracks = (uint16_t)ss_file_read_be(smf_data, pos + 2, 2);
	m->time_division = (uint16_t)ss_file_read_be(smf_data, pos + 4, 2);
	pos += hdr_size;

	/* Allocate track array */
	m->tracks = (SS_MIDITrack *)calloc(n_tracks, sizeof(SS_MIDITrack));
	if(!m->tracks) return false;
	m->track_capacity = n_tracks;

	for(uint16_t ti = 0; ti < n_tracks; ti++) {
		if(pos + 8 > smf_size) break;
		char mtrk_id[5];
		ss_file_read_string(smf_data, pos, mtrk_id, 4);
		if(strcmp(mtrk_id, "MTrk") != 0) {
			/* Skip unrecognized chunk */
			size_t skip_sz = ss_file_read_be(smf_data, pos + 4, 4);
			pos += 8 + skip_sz;
			continue;
		}
		size_t trk_sz = ss_file_read_be(smf_data, pos + 4, 4);
		pos += 8;

		size_t start_ticks = 0;
		/* Format 2: tracks play sequentially; each track starts after the previous */
		if(m->format == 2 && ti > 0) {
			SS_MIDITrack *prev = &m->tracks[ti - 1];
			if(prev->event_count > 0)
				start_ticks = prev->events[prev->event_count - 1].ticks;
		}

		size_t trk_end = pos + trk_sz;
		if(trk_end > smf_size) trk_end = smf_size;
		SS_File *trk_data = ss_file_slice(smf_data, pos, trk_end - pos);
		if(!trk_data) return false;
		parse_track(m, trk_data, start_ticks);
		ss_file_close(trk_data);
		pos = trk_end;
	}

	return true;
}
