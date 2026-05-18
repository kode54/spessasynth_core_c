/**
 * note_off.c
 * Per-MIDI-channel note off handlers.
 * Port of note_off.ts and friends, separated from midi_channel.c.
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>
#if __has_include(<spessasynth_core/spessasynth.h>)
#include <spessasynth_core/midi_enums.h>
#include <spessasynth_core/synth.h>
#else
#include "spessasynth/midi/midi_enums.h"
#include "spessasynth/synthesizer/synth.h"
#endif

#define MIN_NOTE_LENGTH 0.05
#define MIN_EXCLUSIVE_LENGTH 0.01
#define EXCLUSIVE_CUTOFF_TIME (-2320)
#define EXCLUSIVE_MOD_CUTOFF_TIME (-1130)

/* ── Voice allocation ────────────────────────────────────────────────────── */

static void channel_remove_finished_sustained_voices(SS_MIDIChannel *ch) {
	size_t new_count = 0;
	for(size_t i = 0; i < ch->sustained_count; i++) {
		if(ch->sustained_voices[i]->is_active) {
			ch->sustained_voices[new_count++] = ch->sustained_voices[i];
		}
	}
	ch->sustained_count = new_count;
}

void ss_channel_remove_finished_voices(SS_MIDIChannel *ch) {
	channel_remove_finished_sustained_voices(ch);

	size_t new_count = 0;
	for(size_t i = 0; i < ch->voice_count; i++) {
		if(ch->voices[i]->is_active) {
			ch->voices[new_count++] = ch->voices[i];
		} else {
			/* Retire the finished voice into the processor's pool
			 * so the structure can be recycled by a later note-on. */
			ss_voice_pool_release(ch->synth, ch->voices[i]);
		}
	}
	ch->voice_count = new_count;
}

/* ── Voice Release ───────────────────────────────────────────────────────── */

void ss_voice_release(SS_Voice *v, double current_time, double min_note_length) {
	v->release_start_time = current_time;
	if(v->release_start_time - v->start_time < min_note_length)
		v->release_start_time = v->start_time + min_note_length;
}

void ss_voice_exclusive_release(SS_Voice *v, double current_time) {
	v->override_release_vol_env = EXCLUSIVE_CUTOFF_TIME; /* Make the release nearly instant */
	v->is_in_release = false;
	ss_voice_release(v, current_time, MIN_EXCLUSIVE_LENGTH);
}

/* ── Note off ────────────────────────────────────────────────────────────── */

void ss_channel_exclusive_release(SS_MIDIChannel *ch, int note, double time) {
	for(size_t i = 0; i < ch->voice_count; i++) {
		SS_Voice *v = ch->voices[i];
		if(v->is_active && v->midi_note == note) {
			ss_voice_exclusive_release(v, time);
		}
	}
}

void ss_channel_note_off(SS_MIDIChannel *ch, int note, double time) {
	/* Drum rx_note_off: if enabled, do a fast exclusive release */
	if(ch->drum_channel && note >= 0 && note < 128) {
		if(ch->drum_params[note].rx_note_off) {
			ss_channel_exclusive_release(ch, note, time);
			return;
		}
	}

	bool sustained = ch->midi_controllers[64] >= 64 << 7; /* CC64 sustain pedal */
	for(size_t i = 0; i < ch->voice_count; i++) {
		SS_Voice *v = ch->voices[i];
		if(!v->is_active || v->is_in_release) continue;
		if(v->midi_note != note) continue;
		if(sustained) {
			/* Add to sustained list */
			if(ch->sustained_count >= ch->sustained_capacity) {
				size_t nc = ch->sustained_capacity + 8;
				SS_Voice **tmp = (SS_Voice **)realloc(ch->sustained_voices,
				                                      nc * sizeof(SS_Voice *));
				if(tmp) {
					ch->sustained_voices = tmp;
					ch->sustained_capacity = nc;
				}
			}
			if(ch->sustained_count < ch->sustained_capacity)
				ch->sustained_voices[ch->sustained_count++] = v;
		} else {
			ss_voice_release(v, time, 0.05);
		}
	}
}

void ss_channel_all_notes_off(SS_MIDIChannel *ch, double time) {
	for(size_t i = 0; i < ch->voice_count; i++) {
		SS_Voice *v = ch->voices[i];
		if(v->is_active && !v->is_in_release)
			ss_voice_release(v, time, 0.05);
	}
	ch->sustained_count = 0;
}

void ss_channel_all_sound_off(SS_MIDIChannel *ch) {
	for(size_t i = 0; i < ch->voice_count; i++)
		ch->voices[i]->is_active = false;
	ss_channel_remove_finished_voices(ch);
	ch->sustained_count = 0;
}
