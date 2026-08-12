/**
 * midi_emidi.c
 * EMIDI (Extended MIDI) track-designation scanning and filtering.
 *
 * Tracks in EMIDI-aware files carry CC 110 ("Track Designation") events
 * naming the sound cards the track is authored for.  Apogee's AudioLib
 * numbers them (audiolib/_midi.h):
 *
 *    0  General MIDI          5  Pro Audio Spectrum
 *    1  Roland Sound Canvas   6  Sound Man 16
 *    2  Sound Blaster AWE32   7  Adlib
 *    3  Wave Blaster          8  Ensoniq Soundscape
 *    4  Sound Blaster         9  Gravis Ultrasound
 *
 *  127  every card (EMIDI_ALL_CARDS, the wildcard — 0 is *not* a wildcard)
 *
 * A song built for several cards duplicates its content across tracks and
 * marks each copy with the designations it belongs to, so playing the file
 * without filtering doubles (or triples) the voices.
 *
 * A track carrying designations plays if *any* one of them names the card
 * we are: AudioLib's _MIDI_InitEMIDI latches EMIDI_IncludeTrack on the
 * first match and never clears it (its `IncludeFound` guard only lets the
 * first designation seen decide against us).  A track listing several
 * cards including ours therefore plays — matching on all of them is not
 * required, and demanding that drops parts that should sound.
 *
 * Derived from the EMIDI "clean" behavior in midi_processing's
 * midi_container::serialize_as_stream, corrected against AudioLib: that
 * implementation drops a track on its first designation outside
 * {0, 1, 127} without regard to the rest.
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

/* The card we play as.  AudioLib hardcodes type = EMIDI_GeneralMIDI, and
 * we are a General MIDI receiver, so device 0 is ours and 127 matches
 * everyone.  Device 1 is the Sound Canvas — a *different* card, however
 * GM-compatible it is in practice.  Accepting it as well would keep both
 * halves of a Sound-Canvas/General-MIDI pair, which is exactly the
 * doubling the filter exists to prevent. */
#define SS_EMIDI_ALL_CARDS 127
#define SS_EMIDI_OUR_CARD 0

static bool designates_our_card(uint8_t device) {
	return device == SS_EMIDI_OUR_CARD || device == SS_EMIDI_ALL_CARDS;
}

SS_EMIDIKind ss_midi_track_emidi_kind(const SS_MIDITrack *track) {
	if(!track) return SS_EMIDI_KIND_ANY;
	bool designated = false;
	for(size_t i = 0; i < track->event_count; i++) {
		const SS_MIDIMessage *e = &track->events[i];
		if(!is_track_designation(e)) continue;
		/* Any single match carries the whole track. */
		if(designates_our_card(e->data[1])) return SS_EMIDI_KIND_GM;
		designated = true;
	}
	/* Designated, but never for us — this copy belongs to another card. */
	return designated ? SS_EMIDI_KIND_OTHER : SS_EMIDI_KIND_ANY;
}

/* ── File-level scan ─────────────────────────────────────────────────────── */

/* Controllers whose presence marks a file as EMIDI.
 *
 * CC 110 is the track designation; 112 through 119 are the rest of the
 * block Apogee's Extended MIDI claims.  CC 111 is deliberately not in the
 * set: RPG Maker writes it as a loop start and LeapFrog as a loop end, so
 * on its own it indicates nothing — it is the ambiguous controller that
 * the others are used to disambiguate.  libmidi tests the same set. */
static bool is_emidi_indication(const SS_MIDIMessage *e) {
	if((e->status_byte & 0xF0) != 0xB0 || e->data_length < 2) return false;
	const uint8_t cc = e->data[0];
	return cc == 110 || (cc >= 112 && cc <= 119);
}

bool ss_midi_has_emidi(const SS_MIDIFile *midi) {
	if(!midi) return false;
	for(size_t ti = 0; ti < midi->track_count; ti++) {
		const SS_MIDITrack *t = &midi->tracks[ti];
		for(size_t ei = 0; ei < t->event_count; ei++) {
			if(is_emidi_indication(&t->events[ei])) return true;
		}
	}
	return false;
}

/* ── Filtering: drop non-GM tracks in place ──────────────────────────────── */

size_t ss_midi_remove_emidi_non_gm(SS_MIDIFile *midi) {
	if(!midi || midi->track_count == 0) return 0;

	size_t out = 0;
	size_t dropped = 0;
	for(size_t in = 0; in < midi->track_count; in++) {
		if(ss_midi_track_emidi_kind(&midi->tracks[in]) == SS_EMIDI_KIND_OTHER) {
			ss_midi_track_clear(&midi->tracks[in]);
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
	if(dropped) ss_midi_flush(midi);
	return dropped;
}
