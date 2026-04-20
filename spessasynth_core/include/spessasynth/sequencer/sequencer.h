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

/**
 * Callback interface for driving an external synthesizer from the
 * sequencer instead of the built-in SS_Processor.  Every callback is
 * optional; a NULL entry silently disables that hook.
 *
 * Lifetime: the struct contents are copied into the sequencer at
 * ss_sequencer_create_callbacks time, so the struct itself does not
 * need to outlive the call.  The context pointer is stored and passed
 * back to every callback unchanged.
 */
typedef struct {
	/** Sample rate of the caller's custom synth.  Used by
	 *  ss_sequencer_tick to convert sample_count into a time delta.
	 *  Falls back to 44100 if zero. */
	uint32_t sample_rate;

	/** Dispatch one MIDI command.  data begins with the status byte
	 *  (e.g. 0x9n for note-on, 0xF0 for SysEx, 0xF5 for the internal
	 *  port-select message the sequencer emits ahead of each voice
	 *  event on multi-port files).  length is the total command
	 *  length in bytes, including the status byte and any payload
	 *  (including the trailing 0xF7 on SysEx).  timestamp is the
	 *  absolute time in seconds from the start of playback. */
	void (*midi_command)(void *ctx, const uint8_t *data, size_t length,
	                     double timestamp);

	/** Called during the post-loop fade (and on fade-cancel) to
	 *  request a master-volume change.  value is 0..1.  The external
	 *  synth should scale its rendered output by this factor. */
	void (*set_master_volume)(void *ctx, float value);

	/** Opaque context passed back to every callback. */
	void *context;
} SS_SequencerCallbacks;

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
	/* loop_count is the user-requested number of playthroughs.
	 *   -1  loop forever (no fade; loops_played counts up indefinitely);
	 *   0 or 1  no looping — play the song once straight through;
	 *   N>=2  target N playthroughs of the loop body.  Fade-out begins the
	 *         moment loops_played reaches loop_count (i.e. at the start of
	 *         the Nth playthrough).
	 * fade_seconds is the post-loop fade duration (default 7.0 s).
	 * Infinite looping without loop markers rewinds the whole file. */
	int loop_count;
	double fade_seconds;

	/* Runtime loop/fade state.  loops_played counts upward starting at 1
	 * (the initial playthrough) and increments each time the sequencer
	 * jumps at the MIDI loop-end marker.  Fade begins when loop_count is
	 * finite and loops_played >= loop_count.  Setting loop_count back to
	 * -1 cancels any in-progress fade.  Reset to 1 on song advance, seek,
	 * and stop. */
	int loops_played;
	bool fading;
	double fade_start_time;
	float saved_master_volume;

	bool preload; /* true once initial events have been sent */
	bool finished;

	/* When non-NULL midi_command is supplied, events are dispatched
	 * through the callbacks instead of SS_Processor.  proc is NULL
	 * in that mode and the callback's sample_rate is used for timing. */
	SS_SequencerCallbacks callbacks;
} SS_Sequencer;

/** Create a sequencer that drives the built-in SS_Processor. */
SS_Sequencer *ss_sequencer_create(SS_Processor *proc);

/** Create a sequencer that drives an external synthesizer via the
 *  callback table.  The caller is responsible for rendering audio
 *  using the dispatched MIDI commands; ss_sequencer_tick still needs
 *  to be called once per rendered quantum to advance song time. */
SS_Sequencer *ss_sequencer_create_callbacks(const SS_SequencerCallbacks *cb);

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
 *   -1    loop forever; loops_played keeps counting upward.  Setting
 *         loop_count back to -1 while a fade is in progress cancels
 *         the fade and restores the master volume.
 *   0, 1  do not loop; play the song once straight through.
 *   N>=2  target N playthroughs of the loop body.  The fade begins
 *         the moment the running loops_played count reaches N; the
 *         sequencer keeps jumping at the loop marker during the fade
 *         so the music keeps sounding rather than trailing into silence.
 *   Reducing loop_count mid-song below the current loops_played starts
 *   the fade immediately.
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
