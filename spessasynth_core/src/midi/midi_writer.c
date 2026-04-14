/**
 * midi_writer.c
 * SMF serializer.  Port of midi_writer.ts.
 */

#include <stdlib.h>
#include <string.h>
#if __has_include(<spessasynth_core/spessasynth.h>)
#include <spessasynth_core/midi.h>
#else
#include "spessasynth/midi/midi.h"
#endif

/* ── Write a single track ─────────────────────────────────────────────────── */

static bool write_track(SS_File *out, const SS_MIDITrack *track) {
	size_t current_tick = 0;
	size_t running_byte = 0; /* 0 = none */

	SS_File *tb = ss_file_open_blank_memory();
	if(!tb) return false;

	for(size_t i = 0; i < track->event_count; i++) {
		const SS_MIDIMessage *e = &track->events[i];
		uint8_t sb = e->status_byte;

		/* Skip EndOfTrack here; we append it manually at the end */
		if(sb == SS_META_END_OF_TRACK) {
			size_t delta = e->ticks > current_tick ? e->ticks - current_tick : 0;
			current_tick += delta;
			continue;
		}

		size_t delta = e->ticks > current_tick ? e->ticks - current_tick : 0;
		current_tick += delta;

		if(!ss_file_write_vlq(tb, delta)) goto fail;

		/* Meta event: sb <= 0x7F (stored directly as the meta type) */
		if(sb < 0x80) {
			/* FF <type> <vlq-length> <data> */
			if(!ss_file_write_u8(tb, 0xFF)) goto fail;
			if(!ss_file_write_u8(tb, sb)) goto fail;
			if(!ss_file_write_vlq(tb, e->data_length)) goto fail;
			if(e->data_length > 0)
				if(!ss_file_write_bytes(tb, e->data, e->data_length)) goto fail;
			running_byte = 0; /* meta cancels running status */

		} else if(sb == 0xF0 || sb == 0xF7) {
			/* SysEx: F0 <vlq-length> <data> */
			if(!ss_file_write_u8(tb, sb)) goto fail;
			if(!ss_file_write_vlq(tb, e->data_length)) goto fail;
			if(e->data_length > 0)
				if(!ss_file_write_bytes(tb, e->data, e->data_length)) goto fail;
			running_byte = 0;

		} else {
			/* Voice message — use running status */
			if(running_byte != sb) {
				running_byte = sb;
				if(!ss_file_write_u8(tb, sb)) goto fail;
			}
			if(e->data_length > 0)
				if(!ss_file_write_bytes(tb, e->data, e->data_length)) goto fail;
		}
	}

	/* Write EndOfTrack: delta=0, FF 2F 00 */
	if(!ss_file_write_u8(tb, 0x00)) goto fail;
	if(!ss_file_write_u8(tb, 0xFF)) goto fail;
	if(!ss_file_write_u8(tb, SS_META_END_OF_TRACK)) goto fail;
	if(!ss_file_write_u8(tb, 0x00)) goto fail;

	uint8_t *trk_data;
	size_t trk_len;
	if(!ss_file_retrieve_memory(tb, &trk_data, &trk_len)) goto fail;

	/* Write MTrk header + track body into out */
	if(!ss_file_write_string(out, "MTrk", 4)) goto fail;
	if(!ss_file_write_be(out, trk_len, 4)) goto fail;
	if(!ss_file_write_bytes(out, trk_data, trk_len)) goto fail;

	ss_file_close(tb);
	return true;
fail:
	ss_file_close(tb);
	return false;
}

/* ── Public serializer ────────────────────────────────────────────────────── */

bool ss_midi_write(const SS_MIDIFile *midi, SS_File *file) {
	if(!midi || !file) return false;
	if(midi->track_count == 0) return false;

	/* MThd header */
	if(!ss_file_write_string(file, "MThd", 4)) return false;
	if(!ss_file_write_be(file, 6, 4)) return false; /* chunk size */
	if(!ss_file_write_be(file, (uint16_t)(0x00 | midi->format), 2)) return false;
	if(!ss_file_write_be(file, (uint16_t)midi->track_count, 2)) return false;
	if(!ss_file_write_be(file, midi->time_division, 2)) return false;

	/* Track chunks */
	for(size_t ti = 0; ti < midi->track_count; ti++) {
		if(!write_track(file, &midi->tracks[ti])) return false;
	}

	return true;
}
