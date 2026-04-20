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

	/* ── Looping & post-loop fade ───────────────────────────────────────── */
	/* loop_count is the total number of times the looped section plays,
	 *   counting the initial playthrough.  -1 = loop forever;
	 *   0 or 1 = no looping (single playthrough); N >= 2 = N plays of
	 *   the loop body, i.e. (N-1) jumps at the MIDI loop marker.
	 * fade_seconds is the post-loop fade duration applied when a finite
	 *   loop_count is exhausted (default 7.0 s).
	 * Infinite looping without loop markers rewinds the whole file. */
	int loop_count;
	double fade_seconds;

	/* Runtime loop/fade state (reset on song advance / seek / stop). */
	int loops_remaining;
	int loops_played;
	bool fading;
	double fade_start_time;
	float saved_master_volume;

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

/** Configure how many times the looped section plays (counting the
 *  initial pass).  Interpretation:
 *   -1    loop forever.  If the MIDI has no loop markers, rewind and
 *         play the whole file again when it reaches the end; keep
 *         running until manually advanced or stopped.
 *   0, 1  do not loop; play the song once straight through.
 *   N>=2  play the loop body N times (i.e. N-1 jumps at the markers),
 *         then continue playing to the end of the file while fading out.
 *  Default: 2. */
void ss_sequencer_set_loop_count(SS_Sequencer *seq, int count);

/** Configure the post-loop fade duration in seconds.  Only used when
 *  loop_count is finite and the MIDI has loop markers.  Default: 7.0. */
void ss_sequencer_set_fade_seconds(SS_Sequencer *seq, double seconds);

/** Manually advance to the next song, cancelling any pending loops or
 *  fade on the current one.  No-op at the end of the song list (the
 *  sequencer will report finished after the current tick instead). */
void ss_sequencer_next(SS_Sequencer *seq);

/** Must be called once per audio render quantum, BEFORE ss_processor_render(). */
void ss_sequencer_tick(SS_Sequencer *seq, uint32_t sample_count);

bool ss_sequencer_is_finished(const SS_Sequencer *seq);
double ss_sequencer_get_time(const SS_Sequencer *seq);

#ifdef __cplusplus
}
#endif

#endif /* SS_SEQUENCER_H */
