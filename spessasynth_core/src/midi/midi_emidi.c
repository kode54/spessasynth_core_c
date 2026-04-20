/**
 * midi_emidi.c
 * EMIDI (Extended MIDI) track-designation scanning and filtering.
 *
 * Tracks in EMIDI-aware files carry CC 110 ("Track Designation") events
 * that tell the sequencer which synthesizer target the track is authored
 * for:
 *
 *   value  0       plays only on General MIDI receivers
 *   value  1       plays only on General MIDI receivers (alternate)
 *   value  127     plays on every receiver (universal / fallback)
 *   any other      targets a specific non-GM device (MT-32, LAPC, SCC-1, …)
 *
 * A song built for multiple synths duplicates its content across tracks
 * and marks each copy with the appropriate CC 110 value.  Playing the
 * file as plain GM therefore doubles (or triples) the voices unless the
 * non-GM copies are removed.
 *
 * Port of the EMIDI "clean" behavior from midi_processing's
 * midi_container::serialize_as_stream.
 */

#include <stdlib.h>
#include <string.h>

#if __has_include(<spessasynth_core/spessasynth.h>)
#include <spessasynth_core/midi.h>
#else
#include "spessasynth/midi/midi.h"
#endif

/* ── Per-track classification ────────────────────────────────────────────── */

static bool is_track_designation(const SS_MIDIMessage *e) {
	return (e->status_byte & 0xF0) == 0xB0 &&
	       e->data_length >= 2 &&
	       e->data[0] == 110;
}

SS_EMIDIKind ss_midi_track_emidi_kind(const SS_MIDITrack *track) {
	if(!track) return SS_EMIDI_KIND_ANY;
	SS_EMIDIKind kind = SS_EMIDI_KIND_ANY;
	for(size_t i = 0; i < track->event_count; i++) {
		const SS_MIDIMessage *e = &track->events[i];
		if(!is_track_designation(e)) continue;
		uint8_t v = e->data[1];
		if(v == 0 || v == 1 || v == 127) {
			if(kind == SS_EMIDI_KIND_ANY) kind = SS_EMIDI_KIND_GM;
			/* Scan to the end — any later non-GM designation downgrades
			 * the verdict. */
		} else {
			return SS_EMIDI_KIND_OTHER;
		}
	}
	return kind;
}

/* ── File-level scan ─────────────────────────────────────────────────────── */

bool ss_midi_has_emidi(const SS_MIDIFile *midi) {
	if(!midi) return false;
	for(size_t ti = 0; ti < midi->track_count; ti++) {
		const SS_MIDITrack *t = &midi->tracks[ti];
		for(size_t ei = 0; ei < t->event_count; ei++) {
			if(is_track_designation(&t->events[ei])) return true;
		}
	}
	return false;
}

/* ── Filtering: drop non-GM tracks in place ──────────────────────────────── */

static void track_free_contents(SS_MIDITrack *t) {
	if(!t) return;
	if(t->events) {
		for(size_t i = 0; i < t->event_count; i++)
			free(t->events[i].data);
		free(t->events);
	}
	memset(t, 0, sizeof(*t));
	t->port = -1;
}

size_t ss_midi_remove_emidi_non_gm(SS_MIDIFile *midi) {
	if(!midi || midi->track_count == 0) return 0;

	size_t out = 0;
	size_t dropped = 0;
	for(size_t in = 0; in < midi->track_count; in++) {
		if(ss_midi_track_emidi_kind(&midi->tracks[in]) == SS_EMIDI_KIND_OTHER) {
			track_free_contents(&midi->tracks[in]);
			dropped++;
			continue;
		}
		if(out != in) {
			midi->tracks[out] = midi->tracks[in];
			memset(&midi->tracks[in], 0, sizeof(midi->tracks[in]));
			midi->tracks[in].port = -1;
		}
		out++;
	}
	midi->track_count = out;
	return dropped;
}
