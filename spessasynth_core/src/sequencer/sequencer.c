/**
 * sequencer.c
 * MIDI sequencer — drives SS_Processor from an SS_MIDIFile.
 * Port of sequencer.ts + process_tick.ts + process_event.ts.
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#if __has_include(<spessasynth_core/spessasynth.h>)
#include <spessasynth_core/sequencer.h>
#else
#include "spessasynth/sequencer/sequencer.h"
#endif

/* ── Helpers ─────────────────────────────────────────────────────────────── */

/** Read 3-byte big-endian µs/beat, return BPM. */
static double read_tempo_bpm(const uint8_t *d) {
	uint32_t us = ((uint32_t)d[0] << 16) | ((uint32_t)d[1] << 8) | d[2];
	if(us == 0) us = 500000;
	return 60000000.0 / (double)us;
}

/** Compute the effective channel for a message, applying the multi-port
 *  channel offset determined by the message's source track. */
static int effective_channel(const SS_MIDIFile *midi,
                             const SS_MIDIMessage *e) {
	int ch = e->status_byte & 0x0F;
	if(!midi->is_multi_port || !midi->port_channel_offset_map) return ch;
	size_t ti = e->track_index;
	if(ti >= midi->track_count) return ch;
	int port = midi->tracks[ti].port;
	if(port < 0) return ch;
	if((size_t)port >= midi->port_channel_offset_map_count) return ch;
	return ch + midi->port_channel_offset_map[port];
}

/** Compute the effective port for a sysex message. */
static int effective_port(const SS_MIDIFile *midi,
                          const SS_MIDIMessage *e) {
	if(!midi->is_multi_port || !midi->port_channel_offset_map) return 0;
	size_t ti = e->track_index;
	if(ti >= midi->track_count) return 0;
	int port = midi->tracks[ti].port;
	if(port < 0) return 0;
	if((size_t)port >= midi->port_channel_offset_map_count) return 0;
	return midi->port_channel_offset_map[port] / 16;
}

/* ── Embedded RMIDI soundbank (load/unload into processor) ───────────────── */

#define SS_SEQ_EMBEDDED_BANK_ID "embeddedBank"

/** Parse midi->embedded_soundbank and register it with the processor. */
static void load_embedded_bank(SS_Sequencer *seq, SS_MIDIFile *midi) {
	if(!seq || !seq->proc || !midi) return;
	if(!midi->embedded_soundbank || midi->embedded_soundbank_size == 0) return;

	SS_File *bank_file = ss_file_open_from_memory(midi->embedded_soundbank,
	                                              midi->embedded_soundbank_size,
	                                              false);
	if(!bank_file) return;

	SS_SoundBank *bank = ss_soundbank_load(bank_file);
	ss_file_close(bank_file);
	if(!bank) return;

	if(!ss_processor_load_soundbank(seq->proc, bank,
	                                SS_SEQ_EMBEDDED_BANK_ID,
	                                midi->bank_offset, true)) {
		ss_soundbank_free(bank);
	}
}

/** Remove the embedded bank from the processor, freeing it. */
static void unload_embedded_bank(SS_Sequencer *seq) {
	if(!seq || !seq->proc) return;
	ss_processor_remove_soundbank(seq->proc, SS_SEQ_EMBEDDED_BANK_ID, false);
}

/** Decode status_byte → voice message type and channel.
 *  Returns false if this is a meta/sysex event. */
#if 0
static bool decode_voice(uint8_t status_byte, uint8_t *type_out, int *ch_out)
{
    if (status_byte < 0x80) return false; /* meta type */
    if (status_byte >= 0xF0) return false; /* sysex / meta */
    *type_out = status_byte & 0xF0;
    *ch_out   = status_byte & 0x0F;
    return true;
}
#endif

/* ── Song helpers ────────────────────────────────────────────────────────── */

static SS_SequencerSong *current_song(const SS_Sequencer *seq) {
	if(seq->song_count == 0) return NULL;
	size_t idx = seq->current_song_index;
	if(idx >= seq->song_count) return NULL;
	return &seq->songs[idx];
}

/** Reset all per-track event indexes to 0. */
static void song_rewind(SS_SequencerSong *song) {
	song->event_index = 0;
}

/* ── Create / free ───────────────────────────────────────────────────────── */

SS_Sequencer *ss_sequencer_create(SS_Processor *proc) {
	SS_Sequencer *seq = (SS_Sequencer *)calloc(1, sizeof(SS_Sequencer));
	if(!seq) return NULL;
	seq->proc = proc;
	seq->playback_rate = 1.0;
	seq->loop_count = 1;
	seq->fade_seconds = 7.0;
	seq->loops_played = 0;
	seq->saved_master_volume = 1.0f;
	seq->skip_to_first_note_on = true;
	seq->current_song_index = ~0UL;
	return seq;
}

SS_Sequencer *ss_sequencer_create_callbacks(const SS_SequencerCallbacks *cb) {
	if(!cb) return NULL;
	SS_Sequencer *seq = (SS_Sequencer *)calloc(1, sizeof(SS_Sequencer));
	if(!seq) return NULL;
	seq->proc = NULL;
	seq->callbacks = *cb;
	seq->playback_rate = 1.0;
	seq->loop_count = 1;
	seq->fade_seconds = 7.0;
	seq->loops_played = 0;
	seq->saved_master_volume = 1.0f;
	seq->skip_to_first_note_on = true;
	seq->current_song_index = ~0UL;
	return seq;
}

void ss_sequencer_free(SS_Sequencer *seq) {
	if(!seq) return;
	ss_sequencer_clear(seq);
	free(seq->songs);
	free(seq);
}

/* ── Song management ─────────────────────────────────────────────────────── */

void ss_sequencer_set_tick(SS_Sequencer *seq, size_t target_tick);
static void seek_to(SS_Sequencer *seq, size_t target_tick,
                    double target_seconds, bool by_ticks);

/* ── Seek snapshot ───────────────────────────────────────────────────────── */

/* Per-channel state accumulated while scanning past events during a seek,
 * applied in one pass afterwards.  Mirrors upstream's ChannelStatus. */
typedef struct {
	int16_t controllers[128]; /* 14-bit, as stored on the channel */
	int pitch_wheel; /* 14-bit, 8192 is centre */
	int portamento_note; /* -1 when none */
} SS_SeekChannelState;

/* Controllers whose effect depends on the order they arrive in, or which
 * select what a following data entry applies to.  A snapshot cannot express
 * that, so these are dispatched as they are encountered instead. */
static bool seek_cc_is_non_skippable(uint8_t cc) {
	switch(cc) {
		case SS_MIDCON_DATA_INCREMENT:
		case SS_MIDCON_DATA_DECREMENT:
		case SS_MIDCON_DATA_ENTRY_MSB:
		case SS_MIDCON_DATA_ENTRY_LSB:
		case SS_MIDCON_RPN_LSB:
		case SS_MIDCON_RPN_MSB:
		case SS_MIDCON_NRPN_LSB:
		case SS_MIDCON_NRPN_MSB:
		case SS_MIDCON_BANK_SELECT:
		case SS_MIDCON_BANK_SELECT_LSB:
		case SS_MIDCON_RESET_ALL_CONTROLLERS:
		case SS_MIDCON_MONO_MODE_ON:
		case SS_MIDCON_POLY_MODE_ON:
			return true;
		default:
			return false;
	}
}

/* RP-15 reset applied to the snapshot rather than to the channel. */
static void seek_reset_all_controllers(SS_SeekChannelState *cs) {
	cs->pitch_wheel = 8192;
	for(size_t i = 0; i < SS_RP15_RESET_CC_COUNT; i++) {
		const uint8_t cc = ss_rp15_reset_cc_nums[i];
		cs->controllers[cc] = ss_default_controller_values[cc];
	}
}

/** Arrange to start at the first note-on rather than at tick 0.
 *
 *  Driving the built-in processor, the lead-in is replayed by a seek: its
 *  state can be set instantaneously, so there is nothing to be gained by
 *  waiting through it, and this is what upstream does.
 *
 *  Driving an external synthesizer through the callback table, a seek would
 *  deliver the whole lead-in as a burst of messages sharing one timestamp.
 *  Such a synth typically queues incoming messages into a processing buffer
 *  that only drains as it renders, so a burst delivered without any
 *  intervening render either backs up or is applied all at once.  The
 *  lead-in is played at normal speed instead, giving the synth a render
 *  quantum between messages to consume them, and the caller is told to
 *  discard what it renders until the first note sounds — see
 *  ss_sequencer_is_lead_in.
 *
 *  No-op when the song starts on a note, or has no notes at all. */
static void skip_lead_in(SS_Sequencer *seq) {
	seq->lead_in_active = false;
	if(!seq->skip_to_first_note_on) return;
	SS_SequencerSong *song = current_song(seq);
	if(!song || !song->midi) return;
	size_t first = song->midi->first_note_on;
	if(first == 0) return;

	if(seq->callbacks.midi_command) {
		seq->lead_in_active = true;
		return;
	}
	ss_sequencer_set_tick(seq, first - 1);
}

bool ss_sequencer_load_midi(SS_Sequencer *seq, SS_MIDIFile *midi) {
	if(!midi) return false;

	/* Grow song array if needed */
	if(seq->song_count >= seq->song_capacity) {
		size_t nc = seq->song_capacity ? seq->song_capacity * 2 : 4;
		SS_SequencerSong *tmp = (SS_SequencerSong *)realloc(seq->songs,
		                                                    nc * sizeof(*tmp));
		if(!tmp) return false;
		seq->songs = tmp;
		seq->song_capacity = nc;
	}

	SS_SequencerSong *song = &seq->songs[seq->song_count];
	song->midi = midi;
	if(!ss_midi_ensure_timeline(midi)) return false;
	song->event_index = 0;
	seq->song_count++;

	if(seq->current_song_index == ~0UL) {
		seq->current_song_index = 0;
		seq->base_time = 0.0;
		seq->current_tick = 0;
		seq->current_time = 0.0;
		seq->absolute_start_time = seq->engine_time;
		seq->pending_tick_samples = 0;
		seq->cursor_tick = 0;
		seq->cursor_time = 0.0;
		seq->finished = false;
		seq->loops_played = 0;
		seq->ports_active = 0;
		seq->fading = false;
		/* This song is now current — attach its embedded bank, if any. */
		load_embedded_bank(seq, midi);
		skip_lead_in(seq);
	}

	return true;
}

void ss_sequencer_clear(SS_Sequencer *seq) {
	unload_embedded_bank(seq);
	seq->song_count = 0;
	seq->current_song_index = -1;
	seq->is_playing = false;
	seq->finished = true;
}

/* ── Playback control ────────────────────────────────────────────────────── */

void ss_sequencer_play(SS_Sequencer *seq) {
	seq->is_playing = true;
	seq->is_paused = false;
	seq->finished = false;
}

void ss_sequencer_pause(SS_Sequencer *seq) {
	seq->is_paused = true;
}

/* Forward declarations of the MIDI-dispatch sink helpers defined lower
 * in this file.  They're used by the fade/stop/tick paths above. */
static void dispatch_midi(SS_Sequencer *seq, const uint8_t *data,
                          size_t length, double timestamp);
static void dispatch_master_volume(SS_Sequencer *seq, float value);
static void dispatch_voice_event(SS_Sequencer *seq, const SS_MIDIFile *midi,
                                 const SS_MIDIMessage *e, double t);
static void dispatch_sysex_event(SS_Sequencer *seq, const SS_MIDIFile *midi,
                                 const SS_MIDIMessage *e, double t);
static void dispatch_reset(SS_Sequencer *seq);
static void dispatch_all_notes_off(SS_Sequencer *seq);

/** Drop any active fade and restore the master volume the user had
 *  set before the fade started. */
static void end_fade(SS_Sequencer *seq) {
	if(!seq->fading) return;
	seq->fading = false;
	dispatch_master_volume(seq, seq->saved_master_volume);
}

void ss_sequencer_stop(SS_Sequencer *seq) {
	seq->is_playing = false;
	seq->is_paused = false;
	seq->base_time = 0.0;
	seq->current_tick = 0;
	seq->current_time = 0.0;
	seq->absolute_start_time = seq->engine_time;
	seq->pending_tick_samples = 0;
	seq->cursor_tick = 0;
	seq->cursor_time = 0.0;
	end_fade(seq);
	seq->loops_played = 0;
	seq->ports_active = 0;
	SS_SequencerSong *song = current_song(seq);
	if(song) song_rewind(song);
	if(seq->proc) {
		for(int ch = 0; ch < seq->proc->channel_count; ch++)
			ss_channel_all_sound_off(seq->proc->midi_channels[ch]);
	}
}

void ss_sequencer_set_time(SS_Sequencer *seq, double seconds) {
	SS_SequencerSong *song = current_song(seq);
	if(!song) return;
	SS_MIDIFile *midi = song->midi;

	/* Upstream routes a seek to anywhere before the first note — including
	 * an out-of-range one — to the first note instead.  In callback mode the
	 * lead-in is played rather than skipped, so the seek lands where it was
	 * asked to and the lead-in flag is re-armed instead. */
	if(seq->skip_to_first_note_on && midi->first_note_on > 0 &&
	   (seconds < 0.0 || seconds > midi->duration ||
	    seconds < ss_midi_ticks_to_seconds(midi, midi->first_note_on))) {
		if(seq->callbacks.midi_command) {
			seq->lead_in_active = true;
			if(seconds < 0.0 || seconds > midi->duration) seconds = 0.0;
		} else {
			ss_sequencer_set_tick(seq, midi->first_note_on - 1);
			return;
		}
	}

	/* Manual seek cancels any active fade and restarts the loop counter. */
	end_fade(seq);
	seq->loops_played = 0;
	seq->ports_active = 0;

	seek_to(seq, 0, seconds, false);
}

/* Shared seek.  by_ticks selects the stop condition: the target tick, or the
 * accumulated time reaching target_seconds.  Upstream's setTimeTo is likewise
 * one function with those two bounds. */
static void seek_to(SS_Sequencer *seq, size_t target_tick,
                    double target_seconds, bool by_ticks) {
	SS_SequencerSong *song = current_song(seq);
	if(!song) return;
	SS_MIDIFile *midi = song->midi;

	/* Rewind and replay the lead-up to the target. */
	song_rewind(song);
	const double previous_time = seq->current_time;
	/* Absolute time as the processor sees it.  process_event stamps events
	 * with current_time + base_time, so anything the seek dispatches has to
	 * use the same origin — passing the song-relative time alone lands every
	 * replayed event base_time in the past whenever a seek happens after
	 * playback has already moved the base. */
	const double now = seq->base_time + previous_time;

	/* Reset processor */
	dispatch_reset(seq);

	/* Fast-forward to the target.
	 *
	 * Controller and pitch-wheel state is accumulated into a per-channel
	 * snapshot and applied once at the end, rather than every intermediate
	 * value being dispatched on the way past.  Replaying them live leaves the
	 * channel walking through states it never actually occupied, and lands
	 * RPN/NRPN parameter selection wherever the last data entry happened to
	 * point.  This is what upstream's setTimeTo does.
	 *
	 * Program changes, channel pressure and non-controller SysEx are still
	 * dispatched live: some files edit drum parameters over SysEx, and
	 * deferring the program change past them would reset what they set.
	 *
	 * The landing time is accumulated from exact tick deltas rather than
	 * converted from the target tick.  ss_midi_ticks_to_seconds rounds
	 * through the tempo map, and half a tick of error is enough to drop the
	 * first event after the seek into the following render block.  Stopping
	 * on the tick rather than on a converted time avoids the same rounding
	 * on the loop bound. */
	const int channel_count = seq->proc ? seq->proc->channel_count : SS_CHANNEL_COUNT;
	SS_SeekChannelState *snapshot =
	(SS_SeekChannelState *)calloc((size_t)channel_count, sizeof(*snapshot));
	if(snapshot) {
		for(int i = 0; i < channel_count; i++) {
			snapshot[i].pitch_wheel = 8192;
			snapshot[i].portamento_note = -1;
			memcpy(snapshot[i].controllers, ss_default_controller_values,
			       sizeof(snapshot[i].controllers));
		}
	}

	double played = 0.0;
	double one_tick_sec = (midi->time_division > 0) ? (60.0 / (120.0 * (double)midi->time_division)) : (60.0 / (120.0 * 480.0));

	while(song->event_index < midi->timeline_count) {
		SS_MIDIMessage *e = &midi->timeline[song->event_index];
		if(by_ticks) {
			if(e->ticks >= target_tick) break;
		} else if(played >= target_seconds) {
			break;
		}

		song->event_index++;

		uint8_t sb = e->status_byte;

		/* Update tempo before measuring the delta that follows it. */
		if(sb == SS_META_SET_TEMPO && e->data_length >= 3) {
			double bpm = read_tempo_bpm(e->data);
			if(midi->time_division > 0)
				one_tick_sec = 60.0 / (bpm * (double)midi->time_division);
		}

		if(sb >= 0x80 && sb < 0xF0) {
			const uint8_t type = sb & 0xF0;
			const int chan = effective_channel(midi, e);
			SS_SeekChannelState *cs =
			(snapshot && chan >= 0 && chan < channel_count) ? &snapshot[chan] : NULL;

			if(type == 0x90) {
				/* Track the last note for portamento even while seeking.
				 * See spessasynth_core issue #77. */
				if(cs && e->data_length >= 1) cs->portamento_note = e->data[0];
			} else if(type == 0xE0) {
				if(cs && e->data_length >= 2)
					cs->pitch_wheel = ((int)e->data[1] << 7) | e->data[0];
			} else if(type == 0xB0) {
				if(cs && e->data_length >= 2) {
					const uint8_t cc = e->data[0];
					const uint8_t value = e->data[1];
					if(cc == SS_MIDCON_RESET_ALL_CONTROLLERS) {
						seek_reset_all_controllers(cs);
					} else if(seek_cc_is_non_skippable(cc)) {
						/* Bank selects, parameter selection and data entry
						 * carry sequencing that a snapshot cannot express. */
						dispatch_voice_event(seq, midi, e, now);
					} else {
						cs->controllers[cc] = (int16_t)(value << 7);
					}
				}
			} else if(type == 0xC0 || type == 0xD0) {
				/* Program change and channel pressure go through live. */
				dispatch_voice_event(seq, midi, e, now);
			}
			/* Note-off and poly pressure are simply skipped. */
		} else if(sb == 0xF0) {
			dispatch_sysex_event(seq, midi, e, now);
		}

		if(song->event_index < midi->timeline_count) {
			const SS_MIDIMessage *next = &midi->timeline[song->event_index];
			size_t d = next->ticks > e->ticks ? next->ticks - e->ticks : 0;
			played += one_tick_sec * (double)d;
		}
	}

	seq->one_tick_seconds = one_tick_sec;

	/* Apply the accumulated snapshot: pitch wheel, then the portamento note
	 * (before the controllers, since portamento control may override it),
	 * then every controller that actually changed. */
	if(snapshot && seq->proc) {
		for(int i = 0; i < channel_count; i++) {
			SS_MIDIChannel *mch = seq->proc->midi_channels[i];
			if(!mch) continue;
			const SS_SeekChannelState *cs = &snapshot[i];

			ss_channel_pitch_wheel(mch, cs->pitch_wheel, -1, now);

			if(cs->portamento_note >= 0) mch->last_note = cs->portamento_note;

			for(int cc = 0; cc < 128; cc++) {
				if(cs->controllers[cc] == ss_default_controller_values[cc]) continue;
				if(seek_cc_is_non_skippable((uint8_t)cc)) continue;
				ss_channel_controller(mch, cc, cs->controllers[cc] >> 7, now);
			}
		}
	}
	free(snapshot);

	/* Land on the first event at or after the target, timed in the same
	 * accumulated frame the dispatch loop works in — upstream likewise
	 * rebases its clock onto the accumulated playedTime, not onto the
	 * requested position. */
	seq->base_time += previous_time - played;
	seq->current_time = played;
	seq->absolute_start_time = seq->engine_time -
	                           (seq->playback_rate > 0.0 ? played / seq->playback_rate : played);
	seq->current_tick = by_ticks ? target_tick
	                             : ss_seconds_to_midi_tick(midi, played);
	/* Deliberately NOT clearing pending_tick_samples.  It holds the block
	 * the caller last rendered, and the sequencer runs one block behind by
	 * design; zeroing it makes the next tick a no-op while the caller still
	 * renders, so every seek would drop the sequencer another block further
	 * behind.  A fresh sequencer already has it at zero, which is what
	 * produces the leading silent block at load.
	 */
	seq->cursor_time = played;
	seq->cursor_tick = (song->event_index < midi->timeline_count)
	                       ? midi->timeline[song->event_index].ticks
	                       : seq->current_tick;
}

void ss_sequencer_set_tick(SS_Sequencer *seq, size_t target_tick) {
	seek_to(seq, target_tick, 0.0, true);
}

bool ss_sequencer_is_finished(const SS_Sequencer *seq) {
	return seq->finished;
}

double ss_sequencer_get_time(const SS_Sequencer *seq) {
	return seq->current_time;
}

/* ── MIDI dispatch sink ──────────────────────────────────────────────────── */

/**
 * Route a raw MIDI command to the active sink.  data must start with
 * the status byte; for SysEx it is 0xF0 followed by the payload and
 * terminating 0xF7.  length counts every byte.
 *
 * When callbacks are configured, the buffer is passed verbatim along with the
 * song-timeline timestamp.  Otherwise it goes to ss_processor_process_message
 * as due immediately, which is what every message the sequencer emits is.
 */
static void dispatch_midi(SS_Sequencer *seq, const uint8_t *data,
                          size_t length, double timestamp) {
	if(!data || length == 0) return;

	if(seq->callbacks.midi_command) {
		seq->callbacks.midi_command(seq->callbacks.context, data, length,
		                            timestamp);
		return;
	}
	if(!seq->proc) return;

	/* Stamp with the engine clock, not the song timeline, exactly as upstream
	 * does -- its sequencer passes synth.currentTime and never a song time.
	 *
	 * The two are different origins.  A sequencer timestamp is base_time plus
	 * song time, which a seek deliberately rebases and which a skip to the
	 * first note leaves far ahead of an engine clock still sitting at zero.
	 * Handing that to the queue would defer the whole replayed lead-in --
	 * program changes, controllers, the lot -- to some future block instead of
	 * applying it before the first note, which is not a scheduling decision the
	 * sequencer ever means to make.  Everything it emits is due now; only a
	 * host scheduling ahead of the mix has reason to say otherwise. */
	ss_processor_process_message(seq->proc, data, length, 0,
	                             seq->proc->current_time);
}

/** Route a master-volume change to the active sink. */
static void dispatch_master_volume(SS_Sequencer *seq, float value) {
	if(seq->callbacks.set_master_volume)
		seq->callbacks.set_master_volume(seq->callbacks.context, value);
	if(seq->proc)
		ss_processor_set_system_parameter(seq->proc, SS_GLOBAL_SYS_GAIN, value);
}

/** Build the port-select SysEx and dispatch it ahead of a voice event. */
static void dispatch_port_select(SS_Sequencer *seq, int port, double t) {
	uint8_t syx[2] = { 0xF5, (uint8_t)((port & 0x0F) + 1) };
	dispatch_midi(seq, syx, 2, t);
	seq->ports_active |= 1ULL << port;
}

/** Dispatch a voice event (non-meta, non-SysEx) via the sink. */
static void dispatch_voice_event(SS_Sequencer *seq, const SS_MIDIFile *midi,
                                 const SS_MIDIMessage *e, double t) {
	int eff = effective_channel(midi, e);
	dispatch_port_select(seq, eff >> 4, t);
	int ch = eff & 0x0F;

	uint8_t buf[3];
	buf[0] = (uint8_t)((e->status_byte & 0xF0) | ch);
	size_t len = 1;
	for(size_t i = 0; i < e->data_length && len < 3; i++)
		buf[len++] = e->data[i];
	dispatch_midi(seq, buf, len, t);
}

/** Dispatch a SysEx event via the sink, re-prepending the 0xF0 status. */
static void dispatch_sysex_event(SS_Sequencer *seq, const SS_MIDIFile *midi,
                                 const SS_MIDIMessage *e, double t) {
	if(e->data_length == 0) return;
	int port = effective_port(midi, e);
	dispatch_port_select(seq, port, t);

	/* Our SS_MIDIMessage stores the SysEx body without the leading 0xF0,
	 * but the callback contract (and MIDI byte-stream convention) wants
	 * it.  Prepend into a temp buffer. */
	uint8_t *buf = (uint8_t *)malloc(e->data_length + 1);
	if(!buf) return;
	buf[0] = 0xF0;
	memcpy(buf + 1, e->data, e->data_length);
	dispatch_midi(seq, buf, e->data_length + 1, t);
	free(buf);
}

/* ── Process a single MIDI event ─────────────────────────────────────────── */

static void process_event(SS_Sequencer *seq, SS_MIDIFile *midi,
                          SS_MIDIMessage *e, int track_index) {
	(void)track_index;

	uint8_t sb = e->status_byte;
	double t = seq->current_time + seq->base_time;

	/* Voice event */
	if(sb >= 0x80 && sb < 0xF0) {
		dispatch_voice_event(seq, midi, e, t);
		return;
	}

	/* SysEx */
	if(sb == 0xF0) {
		dispatch_sysex_event(seq, midi, e, t);
		return;
	}

	/* Meta events */
	switch(sb) {
		case SS_META_SET_TEMPO:
			if(e->data_length >= 3 && midi->time_division > 0) {
				double bpm = read_tempo_bpm(e->data);
				seq->one_tick_seconds = 60.0 / (bpm * (double)midi->time_division);
			}
			break;

		case SS_META_MIDI_PORT:
			/* ignore — port mapping is static */
			break;

		default:
			break;
	}
}

static bool ss_sequencer_next_song(SS_Sequencer *seq) {
	/* Detach the outgoing song's embedded bank before advancing. */
	seq->current_song_index++;
	if((size_t)seq->current_song_index < seq->song_count) {
		unload_embedded_bank(seq);
		end_fade(seq);

		seq->base_time += seq->current_time;
		seq->current_tick = 0;
		seq->current_time = 0.0;
		seq->absolute_start_time = seq->engine_time;
		seq->cursor_tick = 0;
		seq->cursor_time = 0.0;
		seq->one_tick_seconds = 0.0;
		seq->loops_played = 0;
		seq->ports_active = 0;
		/* Attach the new current song's embedded bank, if any. */
		load_embedded_bank(seq, seq->songs[seq->current_song_index].midi);
		skip_lead_in(seq);
		return true;
	}
	return false;
}

/* ── Public configuration and manual advance ─────────────────────────────── */

/* Forward decl of the fade starter. */
static void begin_fade(SS_Sequencer *seq);

void ss_sequencer_set_loop_count(SS_Sequencer *seq, int count) {
	if(!seq) return;
	seq->loop_count = count;
	if(count < 0) {
		/* Switching to infinite cancels any pending fade so the song
		 * keeps playing at full volume. */
		end_fade(seq);
		return;
	}
	/* Finite target.  If the user requested a loop count at or below the
	 * playthrough we're already on, start the fade immediately — even
	 * mid-loop, without waiting for the next loop-end marker. */
	if(count >= 1 && seq->loops_played > count && !seq->fading)
		begin_fade(seq);
}

void ss_sequencer_set_skip_to_first_note_on(SS_Sequencer *seq, bool skip) {
	seq->skip_to_first_note_on = skip;
	if(!skip) seq->lead_in_active = false;
}

bool ss_sequencer_is_lead_in(const SS_Sequencer *seq) {
	return seq->lead_in_active;
}

void ss_sequencer_set_fade_seconds(SS_Sequencer *seq, double seconds) {
	if(!seq) return;
	if(seconds < 0.0) seconds = 0.0;
	seq->fade_seconds = seconds;
}

void ss_sequencer_next(SS_Sequencer *seq) {
	if(!seq) return;
	if(!ss_sequencer_next_song(seq)) {
		/* No further song to move to; let the next tick finish up. */
		seq->finished = true;
		seq->is_playing = false;
	}
}

/* ── ss_sequencer_tick ────────────────────────────────────────────────────── */

/** Kick off a post-loop fade-out at the current absolute time. */
static void begin_fade(SS_Sequencer *seq) {
	if(seq->fading) return;
	seq->fading = true;
	seq->fade_start_time = seq->base_time + seq->current_time;
	/* Remember the master volume the synth currently holds so we can
	 * fade relative to it and restore on fade-cancel.  Callback-mode
	 * has no introspection hook, so we assume the nominal 1.0. */
	seq->saved_master_volume = seq->proc ? seq->proc->system_params.gain : 1.0f;
}

/** Rewind the current song's event indexes to the first event at or
 *  after target_tick and recompute one_tick_seconds from the tempo map
 *  (picks the latest SET_TEMPO at tick <= target_tick across all
 *  tracks).  Does NOT call ss_processor_system_reset — any reset that
 *  needs to happen on a loop jump should be encoded in the MIDI itself
 *  within the loop range, where it will re-fire as events dispatch. */
static void loop_rewind_to_tick(SS_Sequencer *seq, size_t target_tick,
                                double prev_song_time) {
	SS_SequencerSong *song = current_song(seq);
	if(!song) return;
	SS_MIDIFile *midi = song->midi;

	/* Recompute tempo so the loop iteration starts at the right speed
	 * even when the loop body has no tempo meta at its head. */
	size_t event_index = 0;
	double one_tick_sec = (midi->time_division > 0) ? (60.0 / (120.0 * (double)midi->time_division)) : (60.0 / (120.0 * 480.0));
	for(size_t ei = 0; ei < midi->timeline_count; ei++) {
		const SS_MIDIMessage *e = &midi->timeline[ei];
		if(e->ticks >= target_tick) {
			event_index = ei;
			break;
		}
		if(e->status_byte == SS_META_SET_TEMPO && e->data_length >= 3) {
			if(midi->time_division > 0) {
				uint32_t us = ((uint32_t)e->data[0] << 16) |
				              ((uint32_t)e->data[1] << 8) | e->data[2];
				double bpm = (us > 0) ? (60000000.0 / (double)us) : 120.0;
				one_tick_sec = 60.0 / (bpm * (double)midi->time_division);
			}
		}
	}
	seq->one_tick_seconds = one_tick_sec;

	/* Rewind the timeline to the first event at or after target_tick. */
	song->event_index = event_index;

	double new_song_time = ss_midi_ticks_to_seconds(midi, target_tick);

	/* The synthesizer has been running forward the entire time.  Fold
	 * the song-time we just skipped backwards into base_time so the
	 * absolute timestamps queued for the processor (base + current)
	 * remain monotonically non-decreasing across the jump. */
	if(prev_song_time > new_song_time)
		seq->base_time += prev_song_time - new_song_time;

	seq->current_tick = target_tick;
	seq->current_time = new_song_time;
	seq->absolute_start_time = seq->engine_time -
	                           (seq->playback_rate > 0.0 ? new_song_time / seq->playback_rate
	                                                     : new_song_time);
	seq->cursor_tick = target_tick;
	seq->cursor_time = new_song_time;

	/* Kill any hanging notes */
	dispatch_all_notes_off(seq);
}

/** Ramp the master volume toward zero.  Returns true if the fade has
 *  completed (caller should advance the song). */
static bool apply_fade(SS_Sequencer *seq, double abs_time) {
	if(!seq->fading) return false;
	if(seq->fade_seconds <= 0.0) {
		dispatch_master_volume(seq, 0.0f);
		return true;
	}
	double elapsed = abs_time - seq->fade_start_time;
	if(elapsed < 0.0) elapsed = 0.0;
	double progress = elapsed / seq->fade_seconds;
	if(progress >= 1.0) {
		dispatch_master_volume(seq, 0.0f);
		return true;
	}
	float gain = (float)(1.0 - progress);
	dispatch_master_volume(seq, seq->saved_master_volume * gain);
	return false;
}

void ss_sequencer_tick(SS_Sequencer *seq, uint32_t sample_count) {
	if(!seq->is_playing || seq->is_paused || seq->finished) return;

	/* Dispatch for the block that has already been rendered, not the one
	 * about to be.  Upstream's sequencer compares event times against the
	 * synthesizer's *elapsed* time, which only advances once a block has
	 * been rendered, so an event at tick 0 is not seen until the second
	 * quantum.  Deferring the caller's sample_count by one call reproduces
	 * that without disturbing the tick/tempo/loop bookkeeping below, all of
	 * which stays internally consistent — it simply runs one block behind.
	 *
	 * Sub-block event timestamps are not an option here: the engine ramps
	 * gain, pan and filter parameters across a whole block, so event timing
	 * is quantized to the block grid by design. */
	const uint32_t elapsed_samples = seq->pending_tick_samples;
	seq->pending_tick_samples = sample_count;
	if(elapsed_samples == 0) return;
	sample_count = elapsed_samples;

	SS_SequencerSong *song;
try_again:
	song = current_song(seq);
	if(!song) {
		seq->finished = true;
		return;
	}
	SS_MIDIFile *midi = song->midi;

	/* Advance current_time by the rendered quantum.  Sample rate comes
	 * from the built-in processor in proc mode, or from the caller's
	 * SS_SequencerCallbacks in callback mode; fall back to 44100. */
	uint32_t sr = 0;
	if(seq->proc && seq->proc->sample_rate > 0)
		sr = seq->proc->sample_rate;
	else if(seq->callbacks.sample_rate > 0)
		sr = seq->callbacks.sample_rate;
	if(sr == 0) sr = 44100;
	seq->engine_time += (double)sample_count * (1.0 / (double)sr);
	double target_time = (seq->engine_time - seq->absolute_start_time) *
	                     seq->playback_rate;
	double current_time = seq->current_time;

	/* Apply fade for this block up-front.  If the fade has run its
	 * course we advance the song immediately and retry. */
	if(seq->fading && apply_fade(seq, seq->base_time + target_time)) {
		if(ss_sequencer_next_song(seq)) goto try_again;
		seq->finished = true;
		seq->is_playing = false;
		return;
	}

	/* Seed one_tick_seconds from the MIDI tempo map if not yet set */
	if(seq->one_tick_seconds <= 0.0 && midi->time_division > 0) {
		seq->one_tick_seconds = 60.0 / (120.0 * (double)midi->time_division);
	}

	const bool has_markers = midi->loop.end > 0;
	const bool infinite = seq->loop_count < 0;

	/* Dispatch all events whose time <= target_time */
	while(1) {
		if(song->event_index >= song->midi->timeline_count) {
			/* End of events.  Behavior depends on loop config: */
			if(infinite && !has_markers) {
				/* Infinite + no markers: loop the whole file. */
				ss_sequencer_set_tick(seq, 0);
				target_time -= current_time;
				current_time = 0;
				seq->loops_played++;
				continue;
			}
			if(seq->fading) {
				/* Let the fade timer finish even after the track runs
				 * out so we never cut off mid-fade. */
				break;
			}
			if(ss_sequencer_next_song(seq)) goto try_again;
			/* Upstream's songIsFinished pauses, and pausing sends its
			 * all-off: sustain down on every channel, then a graceful
			 * all-notes-off.  Without it the last notes of a song hang
			 * for as long as the caller keeps rendering. */
			dispatch_all_notes_off(seq);
			seq->finished = true;
			seq->is_playing = false;
			break;
		}

		size_t ei = song->event_index;
		SS_MIDIMessage *e = &midi->timeline[ei];
		/* Accumulate from the exact event cursor, never from the rendered
		 * position: current_tick is re-derived from target_time at the end
		 * of every tick, and that seconds→tick rounding loses up to half a
		 * tick — enough to drop an event into the following render block. */
		size_t delta_ticks = 0;
		if(e->ticks > seq->cursor_tick) delta_ticks = e->ticks - seq->cursor_tick;
		double delta_time = (double)delta_ticks * seq->one_tick_seconds;
		double ev_time = delta_time + seq->cursor_time;

		/* Strictly before the clock, as upstream's `playedTime < currentTime`
		 * is: an event landing exactly on the boundary belongs to the next
		 * block, not this one. */
		if(ev_time >= target_time) break;

		seq->cursor_time = ev_time;
		seq->cursor_tick = e->ticks;
		/* The lead-in ends the moment the first note is dispatched, so the
		 * block about to be rendered is the first one worth keeping. */
		if(seq->lead_in_active && e->ticks >= midi->first_note_on)
			seq->lead_in_active = false;
		current_time = ev_time;
		seq->current_tick = e->ticks;

		song->event_index++;
		process_event(seq, midi, e, e->track_index);

		/* Loop-jump check at the MIDI loop marker.
		 *
		 * loops_played counts upward starting at 1 on the initial pass.
		 * Each time we reach the loop-end marker we've finished one more
		 * playthrough of the loop body; if the sequencer keeps going,
		 * increment loops_played and jump.
		 *
		 * Finite loop_count: we want loops_played to reach loop_count
		 * exactly before the fade kicks in.  When incrementing would
		 * take loops_played past loop_count we first start the fade and
		 * then keep jumping — that way the music keeps sounding all the
		 * way through the fade instead of trailing into silence.
		 *
		 * loop_count <= 1: no looping at all; let the song play through.
		 * loop_count < 0:  infinite — always jump, never fade. */
		if(has_markers && e->ticks >= midi->loop.end) {
			bool do_jump = false;
			if(infinite || seq->fading) {
				/* Infinite looping, or fade-in-progress where we keep
				 * jumping so the music continues to play rather than
				 * trailing into silence after the final loop body. */
				do_jump = true;
			} else if(seq->loop_count >= 1) {
				/* Finite target.  Incrementing loops_played brings it to
				 * the count of the playthrough we're about to begin;
				 * when that hits loop_count we are starting the final
				 * iteration, which is also the fade iteration. */
				if(seq->loops_played >= seq->loop_count)
					begin_fade(seq);
				do_jump = true;
			}
			/* loop_count in {0, 1}: looping disabled; fall through and
			 * let the song play out. */
			if(do_jump) {
				seq->loops_played++;
				double loop_end_time = ss_midi_ticks_to_seconds(midi,
				                                                midi->loop.end);
				double loop_start_time = ss_midi_ticks_to_seconds(midi,
				                                                  midi->loop.start);
				if(midi->loop.type == SS_LOOP_TYPE_SOFT)
					loop_rewind_to_tick(seq, midi->loop.start, loop_end_time);
				else
					ss_sequencer_set_tick(seq, midi->loop.start);
				/* No origin adjustment here: both branches above land through
				 * a seek, and a seek already rebases absolute_start_time onto
				 * the position it lands on.  Shifting it again moved the
				 * derived clock a whole loop further on, so the rest of the
				 * song was consumed in one tick and the file fell silent.
				 *
				 * The tick ends here, as upstream's returns after a jump.
				 * Carrying on would dispatch the start of the new pass using
				 * a window whose far edge is still the old timeline's, and
				 * would then store that back over the position the seek just
				 * established -- putting the whole second pass a fraction of
				 * a block early. */
				(void)loop_end_time;
				(void)loop_start_time;
				return;
			}
		}

		/* End-of-sequence check (for tracks without a loop.end). */
		if(e->ticks >= midi->last_voice_event_tick) {
			if(song->event_index >= song->midi->timeline_count) {
				if(infinite && !has_markers) {
					ss_sequencer_set_tick(seq, 0);
					target_time -= current_time;
					current_time = 0;
					continue;
				}
				if(seq->fading) break;
				if(ss_sequencer_next_song(seq)) goto try_again;
				dispatch_all_notes_off(seq);
				seq->finished = true;
				seq->is_playing = false;
				break;
			}
		}
	}
	seq->current_tick = ss_seconds_to_midi_tick(midi, target_time);
	seq->current_time = target_time;
}

void ss_sequencer_set_synthesizer(SS_Sequencer *seq, SS_Processor *proc) {
	seq->proc = proc;
}

/* GS Reset: Roland DT1 to System Mode Set, 40 00 7F 00, checksum 0x41.
 * Upstream sends this rather than a GM reset when driving an external
 * synthesizer, so the device lands in GS mode as the corpus expects. */
const uint8_t syx_reset_gs[] = {
	0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x7F, 0x00, 0x41, 0xF7
};

static void dispatch_reset(SS_Sequencer *seq) {
	/* Stop everything sounding before resetting, as upstream's
	 * sendMIDIReset does; otherwise held and sustained notes outlive the
	 * reset that is supposed to clear them. */
	dispatch_all_notes_off(seq);

	if(seq->proc) {
		ss_processor_system_reset(seq->proc);
		return;
	}
	if(seq->callbacks.midi_command) {
		for(int i = 0; i < SS_MIDI_PORT_COUNT; i++) {
			if((seq->ports_active & (1ULL << i)) != 0) {
				dispatch_port_select(seq, i, seq->base_time);
				dispatch_midi(seq, syx_reset_gs, sizeof(syx_reset_gs), seq->base_time);
			}
		}
	}
}

static void dispatch_all_notes_off(SS_Sequencer *seq) {
	/* Release sustain first, on every channel, or held notes survive the
	 * all-notes-off that follows.  Upstream's sendMIDIAllOff does this
	 * unconditionally, in both playback modes. */
	if(seq->proc) {
		for(int ch = 0; ch < seq->proc->channel_count; ch++) {
			SS_MIDIChannel *c = seq->proc->midi_channels[ch];
			if(c) ss_channel_controller(c, SS_MIDCON_SUSTAIN_PEDAL, 0,
			                            seq->base_time);
		}
	}
	if(seq->callbacks.midi_command) {
		for(int i = 0; i < SS_MIDI_PORT_COUNT; i++) {
			if((seq->ports_active & (1ULL << i)) == 0) continue;
			dispatch_port_select(seq, i, seq->base_time);
			for(int ch = 0; ch < SS_CHANNEL_COUNT; ch++) {
				const uint8_t msg[3] = { (uint8_t)(0xB0 | ch),
					                     SS_MIDCON_SUSTAIN_PEDAL, 0 };
				dispatch_midi(seq, msg, 3, seq->base_time);
			}
		}
	}

	if(seq->proc) {
		for(int ch = 0; ch < seq->proc->channel_count; ch++) {
			int port = ch >> 4;
			if((seq->ports_active & (1ULL << port)) != 0) {
				SS_MIDIChannel *c = seq->proc->midi_channels[ch];
				if(c) ss_channel_all_notes_off(c, seq->base_time);
			}
		}
	}
	if(seq->callbacks.midi_command) {
		for(int i = 0; i < SS_MIDI_PORT_COUNT; i++) {
			if((seq->ports_active & (1ULL << i)) != 0) {
				dispatch_port_select(seq, i, seq->base_time);
				for(int ch = 0; ch < SS_CHANNEL_COUNT; ch++) {
					uint8_t msg[3];
					msg[0] = 0xB0 | ch;
					msg[1] = 123; /* All notes off */
					msg[2] = 0;
					dispatch_midi(seq, msg, 3, seq->base_time);
				}
			}
		}
	}
}
