#ifndef SS_SEQUENCER_H
#define SS_SEQUENCER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#if __has_include(<spessasynth_core/spessasynth.h>)
#include <spessasynth_core/midi.h>
#include <spessasynth_core/synth.h>
#else
#include "../midi/midi.h"
#include "../synthesizer/synth.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	SS_MIDIFile *midi; /* non-owning */
	size_t *event_indexes; /* current index per track (heap-allocated) */
	size_t track_count;
} SS_SequencerSong;

typedef struct {
	SS_Processor *proc; /* non-owning */

	SS_SequencerSong *songs;
	size_t song_count;
	size_t song_capacity;
	size_t current_song_index;

	double base_time; /* absolute time */
	double current_time; /* seconds, same units as proc->current_synth_time */
	double playback_rate; /* 1.0 = normal */
	double one_tick_seconds; /* current tempo ratio */
	bool is_playing;
	bool is_paused;
	bool loop;
	int loop_count; /* -1 = infinite */
	int loops_played;

	bool preload; /* true once initial events have been sent */
	bool finished;
} SS_Sequencer;

SS_Sequencer *ss_sequencer_create(SS_Processor *proc);
void ss_sequencer_free(SS_Sequencer *seq);

/** Load a single MIDI file into the song list. */
bool ss_sequencer_load_midi(SS_Sequencer *seq, SS_MIDIFile *midi);

/** Clear the song list. */
void ss_sequencer_clear(SS_Sequencer *seq);

void ss_sequencer_play(SS_Sequencer *seq);
void ss_sequencer_pause(SS_Sequencer *seq);
void ss_sequencer_stop(SS_Sequencer *seq);

/** Set playback position in seconds. */
void ss_sequencer_set_time(SS_Sequencer *seq, double seconds);

/** Must be called once per audio render quantum, BEFORE ss_processor_render(). */
void ss_sequencer_tick(SS_Sequencer *seq, uint32_t sample_count);

bool ss_sequencer_is_finished(const SS_Sequencer *seq);
double ss_sequencer_get_time(const SS_Sequencer *seq);

#ifdef __cplusplus
}
#endif

#endif /* SS_SEQUENCER_H */
