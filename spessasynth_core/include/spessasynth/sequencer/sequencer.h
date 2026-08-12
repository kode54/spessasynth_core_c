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

#ifdef _MSC_VER
#include "spessasynth_exports.h"
#else
#define SPESSASYNTH_EXPORTS
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	SS_MIDIFile *midi; /* non-owning */
	size_t event_index;
} SS_SequencerSong;

/**
 * What the sequencer does once a finite loop_count is used up.
 *
 * SS_LOOP_END_FADE
 *     Keep jumping at the loop marker while fading the master volume out
 *     over fade_seconds, so the music keeps sounding rather than trailing
 *     into silence.  This is the default, and is what a music player
 *     usually wants.  Upstream has no equivalent.
 *
 * SS_LOOP_END_PLAY_OUT
 *     Stop jumping and let the song play through to its end, which is what
 *     upstream's sequencer does when its loopCount countdown reaches zero.
 *     No fade is ever started in this mode.  Use it when comparing renders
 *     against upstream, where the fade would otherwise make the last
 *     seconds of a looping file incomparable.
 */
typedef enum {
	SS_LOOP_END_FADE = 0,
	SS_LOOP_END_PLAY_OUT = 1
} SS_LoopEndBehavior;

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
	size_t current_tick; /* absolute timestamp */
	double current_time; /* seconds, same units as proc->current_time */
	/* Song time is derived from these two, not accumulated: upstream reads it
	 * as (synth.currentTime - absoluteStartTime) * playbackRate on every tick.
	 * Accumulating instead gives a different sum in the last bits, and an
	 * event landing within a part in a quadrillion of a block boundary then
	 * falls on the other side of it. */
	double engine_time; /* mirrors the engine clock, accumulated from zero */
	double absolute_start_time; /* engine time the current song time is measured from */
	double playback_rate; /* 1.0 = normal */
	double one_tick_seconds; /* current tempo ratio */
	size_t ports_active; /* bit mask */
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
	/* What happens when a finite loop_count runs out: fade and keep
	 * looping, or stop looping and play the song out as upstream does. */
	SS_LoopEndBehavior loop_end_behavior;

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

	/* Start playback at the first note-on rather than at tick 0, skipping
	 * any setup-only lead-in (SysEx, program changes, markers).  Seeking to
	 * a point before the first note lands there too.  Matches upstream's
	 * skipToFirstNoteOn, which also defaults to true.  Disable it with
	 * ss_sequencer_set_skip_to_first_note_on for exact file timing. */
	bool skip_to_first_note_on;

	/* True while the pre-first-note lead-in is being played out in real
	 * time.  Only ever set in callback mode; see ss_sequencer_is_lead_in. */
	bool lead_in_active;

	/* Exact position of the last dispatched event.
	 *
	 * current_tick/current_time track the rendered position and are rounded
	 * to it; deriving event times from them loses up to half a tick, which
	 * is enough to push an event across a render-block boundary.  This
	 * cursor advances only in exact tick deltas, so event times accumulate
	 * the same way upstream's do. */
	size_t cursor_tick;
	double cursor_time;

	/* Sample count handed to the previous ss_sequencer_tick call.
	 *
	 * Events are dispatched for the block that has already been rendered,
	 * never for the one that is about to be, so the sequencer runs exactly
	 * one render block behind the caller.  See ss_sequencer_tick. */
	uint32_t pending_tick_samples;

	/* When non-NULL midi_command is supplied, events are dispatched
	 * through the callbacks instead of SS_Processor.  proc is NULL
	 * in that mode and the callback's sample_rate is used for timing. */
	SS_SequencerCallbacks callbacks;
} SS_Sequencer;

/** Create a sequencer that drives the built-in SS_Processor. */
SS_Sequencer SPESSASYNTH_EXPORTS *ss_sequencer_create(SS_Processor *proc);

/** Create a sequencer that drives an external synthesizer via the
 *  callback table.  The caller is responsible for rendering audio
 *  using the dispatched MIDI commands; ss_sequencer_tick still needs
 *  to be called once per rendered quantum to advance song time. */
SS_Sequencer SPESSASYNTH_EXPORTS *ss_sequencer_create_callbacks(const SS_SequencerCallbacks *cb);

void SPESSASYNTH_EXPORTS ss_sequencer_free(SS_Sequencer *seq);

/** Load a single MIDI file into the song list. */
bool SPESSASYNTH_EXPORTS ss_sequencer_load_midi(SS_Sequencer *seq, SS_MIDIFile *midi);

/** Clear the song list. */
void SPESSASYNTH_EXPORTS ss_sequencer_clear(SS_Sequencer *seq);

void SPESSASYNTH_EXPORTS ss_sequencer_play(SS_Sequencer *seq);
void SPESSASYNTH_EXPORTS ss_sequencer_pause(SS_Sequencer *seq);
void SPESSASYNTH_EXPORTS ss_sequencer_stop(SS_Sequencer *seq);

/** Set playback position in seconds. */
void SPESSASYNTH_EXPORTS ss_sequencer_set_time(SS_Sequencer *seq, double seconds);

/** Set playback position by MIDI tick.  Same seek as ss_sequencer_set_time,
 *  bounded by tick rather than by elapsed seconds. */
void SPESSASYNTH_EXPORTS ss_sequencer_set_tick(SS_Sequencer *seq, size_t target_tick);

/** Configure how many times the looped section plays (counting the
 *  initial pass).  Interpretation:
 *   -1    loop forever; loops_played keeps counting upward.  Setting
 *         loop_count back to -1 while a fade is in progress cancels
 *         the fade and restores the master volume.
 *   0     do not loop; play the song once straight through.
 *   N>=1  target N+1 playthroughs of the loop body.  The fade begins
 *         the moment the running loops_played count reaches N; the
 *         sequencer keeps jumping at the loop marker during the fade
 *         so the music keeps sounding rather than trailing into silence.
 *         The loops_played count only increments from 0 when a jump
 *         occurs, so loops_played indicates how many times the file
 *         has jumped backwards.
 *   Reducing loop_count mid-song below the current loops_played starts
 *   the fade immediately.
 *  Default: 1. */
void SPESSASYNTH_EXPORTS ss_sequencer_set_loop_count(SS_Sequencer *seq, int count);

/** Start playback at the first note-on instead of tick 0, skipping a
 *  setup-only lead-in.  Default: true, matching upstream.  Pass false to
 *  play files with their exact original timing.  Takes effect on the next
 *  load or seek.
 *
 *  Driving the built-in processor, the lead-in is replayed instantly by a
 *  seek.  Driving an external synthesizer through the callback table, the
 *  lead-in is instead played out at normal speed: such a synth typically
 *  queues messages into a processing buffer that only drains as it renders,
 *  so it needs a render quantum between them rather than the whole lead-in
 *  as one burst.  ss_sequencer_is_lead_in then reports which rendered audio
 *  to throw away. */
void SPESSASYNTH_EXPORTS ss_sequencer_set_skip_to_first_note_on(SS_Sequencer *seq, bool skip);

/** True while the lead-in is still playing and nothing has sounded yet.
 *
 *  Callback mode only; always false when driving the built-in processor,
 *  which skips the lead-in by seeking instead.  Check it after each
 *  ss_sequencer_tick: while it reads true, the quantum about to be rendered
 *  still precedes the first note and should be discarded.  It clears in the
 *  same tick that dispatches the first note, so the quantum carrying the
 *  note's onset is kept.
 *
 *      ss_sequencer_tick(seq, n);
 *      bool discard = ss_sequencer_is_lead_in(seq);
 *      render(n);
 *      if(!discard) write_output(n);
 */
bool SPESSASYNTH_EXPORTS ss_sequencer_is_lead_in(const SS_Sequencer *seq);

/** Configure the post-loop fade duration in seconds.  Only used when
 *  loop_count is finite, the MIDI has loop markers and the loop-end
 *  behavior is SS_LOOP_END_FADE.  Default: 7.0. */
void SPESSASYNTH_EXPORTS ss_sequencer_set_fade_seconds(SS_Sequencer *seq, double seconds);

/** Choose what happens once a finite loop count is used up.  See
 *  SS_LoopEndBehavior.  Switching to SS_LOOP_END_PLAY_OUT cancels any
 *  fade already in progress.  Default: SS_LOOP_END_FADE. */
void SPESSASYNTH_EXPORTS ss_sequencer_set_loop_end_behavior(SS_Sequencer *seq,
                                                            SS_LoopEndBehavior behavior);

/** Manually advance to the next song, cancelling any pending loops or
 *  fade on the current one.  No-op at the end of the song list (the
 *  sequencer will report finished after the current tick instead). */
void SPESSASYNTH_EXPORTS ss_sequencer_next(SS_Sequencer *seq);

/** Must be called once per audio render quantum, BEFORE ss_processor_render().
 *
 *  Events are dispatched for the quantum that has *already* been rendered
 *  rather than the one about to be, so a note at tick 0 first sounds one
 *  render block in.  This matches upstream spessasynth_core, whose sequencer
 *  dispatches against the synthesizer's elapsed time; the engine ramps
 *  parameters over a fixed block duration, so the offset cannot be expressed
 *  as a sub-block event timestamp instead.
 *
 *  sample_count is therefore consumed on the *following* call: pass the same
 *  block size every time, as the engine requires anyway. */
void SPESSASYNTH_EXPORTS ss_sequencer_tick(SS_Sequencer *seq, uint32_t sample_count);

bool SPESSASYNTH_EXPORTS ss_sequencer_is_finished(const SS_Sequencer *seq);
double SPESSASYNTH_EXPORTS ss_sequencer_get_time(const SS_Sequencer *seq);

/** Bleh, this is in case the sequencer is torn down after the processor is
 *  freed by the caller. Gotta prevent double-free. */
void SPESSASYNTH_EXPORTS ss_sequencer_set_synthesizer(SS_Sequencer *seq, SS_Processor *proc);

#ifdef __cplusplus
}
#endif

#endif /* SS_SEQUENCER_H */
