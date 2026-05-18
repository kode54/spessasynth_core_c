/**
 * note_on.c
 * Per-MIDI-channel note on handler
 * Port of note_on.ts, separated from midi_channel.c.
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

extern float ss_portamento_time_to_seconds(float portamento_time, float distance);
extern void ss_channel_exclusive_release(SS_MIDIChannel *ch, int note, double time);
extern void ss_channel_remove_finished_voices(SS_MIDIChannel *ch);
extern void ss_voice_compute_modulators(SS_Voice *v, const SS_MIDIChannel *ch,
                                        double time);
extern void ss_volume_envelope_recalculate(SS_Voice *v,
                                           SS_VolumeEnvelope *env,
                                           const int16_t *mod_gens,
                                           int target_key,
                                           bool is_in_release,
                                           double release_start_time,
                                           double start_time);
extern void ss_modulation_envelope_recalculate(SS_ModulationEnvelope *env,
                                               const int16_t *mod_gens,
                                               int midi_note,
                                               bool is_in_release,
                                               double release_start_time,
                                               double start_time);
extern void ss_voice_exclusive_release(SS_Voice *v, double current_time);
extern float ss_timecents_to_seconds(int tc);

static long clamp_long(long val, long min, long max) {
	if(val < min) return min;
	if(val > max) return max;
	return val;
}

/* ── Voice allocation ────────────────────────────────────────────────────── */

#define VOICE_GROW_BY 16

static bool channel_add_voice(SS_MIDIChannel *ch, SS_Voice *v) {
	if(ch->voice_count >= ch->voice_capacity) {
		size_t new_cap = ch->voice_capacity + VOICE_GROW_BY;
		SS_Voice **tmp = (SS_Voice **)realloc(ch->voices,
		                                      new_cap * sizeof(SS_Voice *));
		if(!tmp) return false;
		ch->voices = tmp;
		ch->voice_capacity = new_cap;
	}
	ch->voices[ch->voice_count++] = v;
	return true;
}

/* ── Voice cap / stealing ────────────────────────────────────────────────── */

/*
 * Stealing priority for a voice: higher means more important / keep longer.
 * Mirrors SynthesizerCore.assignVoicePriorities() in spessasynth_core: drums
 * are favored, released and quiet voices are the first to go, while louder
 * and younger voices (earlier volume-envelope stage) are kept.
 */
static float voice_steal_priority(const SS_Voice *v, bool drum_channel) {
	float p = 0.0f;
	if(drum_channel) p += 5.0f;          /* percussion is important */
	if(v->is_in_release) p -= 10.0f;     /* already released: expendable */
	p += (float)v->velocity / 25.0f;     /* louder note (0..~5): keep */
	p -= (float)v->volume_env.state;     /* later envelope stage: drop sooner */
	p -= (float)v->volume_env.attenuation_cb / 200.0f; /* quieter: drop sooner */
	return p;
}

/*
 * Enforces the global voice cap.  Once the number of active voices across all
 * channels has reached the cap, the lowest-priority voice is silenced to make
 * room for the one about to start.  The layers already placed by the current
 * note-on (own_ch->voices[own_start..]) are spared so a multi-layer note-on
 * cannot cannibalize itself; voices from earlier note-ons — even at the same
 * timestamp — remain fair game.  With auto-allocation enabled the cap is
 * advisory and nothing is stolen.
 */
static void enforce_voice_cap(SS_Processor *proc, double time,
                              const SS_MIDIChannel *own_ch, size_t own_start) {
	(void)time;
	const int cap = proc->system_params.voice_cap;
	if(cap <= 0 || proc->system_params.auto_allocate_voices) return;

	int active = 0;
	SS_Voice *lowest = NULL;
	float lowest_priority = 0.0f;
	for(int ci = 0; ci < proc->channel_count; ci++) {
		SS_MIDIChannel *ch = proc->midi_channels[ci];
		if(!ch) continue;
		for(size_t vi = 0; vi < ch->voice_count; vi++) {
			SS_Voice *v = ch->voices[vi];
			if(!v->is_active) continue;
			active++;
			/* Never steal a layer started by the in-progress note-on. */
			if(ch == own_ch && vi >= own_start) continue;
			const float p = voice_steal_priority(v, ch->drum_channel);
			if(!lowest || p < lowest_priority) {
				lowest = v;
				lowest_priority = p;
			}
		}
	}
	/* Silenced voices stop rendering immediately and are reaped by
	 * ss_channel_remove_finished_voices(), which returns them to the pool. */
	if(active >= cap && lowest)
		lowest->is_active = false;
}

/* ── Note on ─────────────────────────────────────────────────────────────── */

void ss_channel_note_on(SS_MIDIChannel *ch, int note, int vel, double time) {
	if(vel < 1) {
		ss_channel_note_off(ch, note, time);
		return;
	}
	if(vel > 127) vel = 127;

	if(!ch->preset) return;
	if(ch->system_params.is_muted) return;

	/* current_key_shift folds in the global system/MIDI and channel
	 * system/MIDI key shifts (see ss_channel_update_internal_params). The
	 * GS/XG SysEx key shift is the channel MIDI key shift parameter. */
	int sound_bank_note = (int)(note + ch->current_key_shift);

	if(sound_bank_note > 127 || sound_bank_note < 0) {
		return;
	}

	/* Portamento */
	/* Not implemented correctly */
	const int previous_note = ch->last_note;
	const bool portamento_enabled = ch->portamento_force || ch->midi_controllers[SS_MIDCON_PORTAMENTO_ON_OFF] >= 8192;

	/* 14-bit MIDI CC -> 7-bit value */
	const int portamento_time = ch->midi_controllers[SS_MIDCON_PORTAMENTO_TIME] >> 7;

	const bool can_apply_portamento = portamento_enabled && /* Enabled? */
		!ch->drum_channel && /* Not a drum channel? */
		previous_note >= 0 && /* Valid note? */
		previous_note != note && /* Not the same note? */
		portamento_time > 0; /* Non-instant time? */

	int porta_from_key = -1;
	float porta_time = 0;

	if(can_apply_portamento) {
		const int key_distance = abs(note - previous_note);
		porta_from_key = previous_note;
		porta_time = ss_portamento_time_to_seconds((float)portamento_time, (float)key_distance);
		ch->portamento_force = false;
	}

	/* Always track the last note, even if portamento isn't applied.
	 * See: https://github.com/spessasus/spessasynth_core/issues/77
	 */
	ch->last_note = note;

	if(!ch->midi_params.poly_mode) {
		ss_channel_exclusive_release(ch, -1, time);
	}

	/* Drum parameter checks */
	int drum_filter_cutoff = -1;
	int drum_filter_resonance = -1;
	float drum_pitch_offset = 0.0f;
	float drum_reverb_send = 1.0f;
	float drum_chorus_send = 1.0f;
	float drum_delay_send = 1.0f;
	float drum_gain = 1.0f;
	int drum_exclusive_override = 0;
	bool pan_override_active = false;
	float pan_override;

	if(ch->drum_channel) {
		const SS_DrumParameters *dp = &ch->drum_params[note];
		if(!dp->rx_note_on) return;

		drum_pitch_offset = dp->pitch;
		drum_exclusive_override = (int)dp->exclusive_class;
		drum_filter_cutoff = dp->filter_cutoff;
		drum_filter_resonance = dp->filter_resonance;
		drum_reverb_send = dp->reverb_gain;
		drum_chorus_send = dp->chorus_gain;
		drum_delay_send = dp->delay_gain;
		drum_gain = dp->gain;
		const int drum_pan = dp->pan - 64;

		/* Drum pan override: 0=random, 64=channel default, else override */
		if(drum_pan != 0) {
			if(drum_pan == -64) {
				/* Random pan [-500, 500] */
				pan_override = (float)(rand() % 1001) - 500.0f;
				pan_override_active = true;
			} else {
				float ch_pan = (float)(ch->midi_controllers[SS_MIDCON_PAN] >> 7) - 64.0f;
				float target = (float)drum_pan + ch_pan;
				if(target < -63.0f) target = -63.0f;
				if(target > 63.0f) target = 63.0f;
				pan_override = (target / 63.0f) * 500.0f;
				pan_override_active = true;
			}
		}
	} else if(ch->midi_params.random_pan) {
		pan_override = (float)(rand() % 1001) - 500.0f;
		pan_override_active = true;
	}

	const int program = ch->preset->program;
	int tune = -1;
	if(ch->synth &&
	   ch->synth->tunings &&
	   ch->synth->tunings[program]) {
		tune = ch->synth->tunings[program][note].midi_note;
	}
	if(tune >= 0) {
		sound_bank_note = tune;
	}

	/* Monophonic retrigger: the per-channel system override falls back to
	 * the global system parameter when unset. */
	const int8_t mono_retrig = ch->system_params.monophonic_retrigger;
	const bool monophonic_retrigger = (mono_retrig != SS_PARAM_UNSET)
	                                  ? (mono_retrig != 0)
	                                  : (ch->synth && ch->synth->system_params.monophonic_retrigger);
	if(monophonic_retrigger || ch->midi_params.assign_mode == 0) {
		ss_channel_exclusive_release(ch, note, time);
	}

	/* Get synthesis data for this (note, velocity) */
	SS_SynthesisData *synth_data = NULL;
	size_t sd_count = ss_preset_get_synthesis_data(ch->preset, sound_bank_note, vel, &synth_data);

	SS_Processor *proc = ch->synth;

	SS_DynamicModulatorSystem *dms = &ch->dms;

	/* Voices appended below belong to this note-on; the cap enforcement
	 * must never steal them back. */
	const size_t own_voices_start = ch->voice_count;

	for(size_t si = 0; si < sd_count; si++) {
		SS_SynthesisData *sd = &synth_data[si];
		SS_BasicSample *samp = sd->sample;
		if(!samp) continue;

		/* Decode sample data lazily */
		if(!ss_sample_decode(samp)) continue;
		if(!samp->audio_data || samp->audio_data_length == 0) continue;

		/* Generator array is fully resolved by ss_preset_get_synthesis_data:
		 * defaults + inst layer + preset summed + EMU attenuation applied. */
		int16_t generators[SS_GEN_COUNT];
		memcpy(generators, sd->generators, sizeof(generators));

		int target_key = sound_bank_note;
		if(generators[SS_GEN_KEYNUM] > -1)
			target_key = generators[SS_GEN_KEYNUM];

		/* Build AudioSample.
		 * sample_start / end_adjustment are pre-baked address-offset fixups
		 * applied at zone construction time (SF2 gens 0/4 and 1/12). */
		SS_AudioSample audio;
		memset(&audio, 0, sizeof(audio));
		audio.sample_data = samp->audio_data + samp->sample_start;
		audio.sample_data_len = (samp->audio_data_length > samp->sample_start) ? samp->audio_data_length - samp->sample_start : 0;
		audio.loop_start = samp->loop_start;
		audio.loop_end = samp->loop_end;

		int root_key = samp->original_key;
		if(generators[SS_GEN_OVERRIDING_ROOT_KEY] > -1) {
			root_key = generators[SS_GEN_OVERRIDING_ROOT_KEY];
		}
		audio.root_key = root_key;

		{
			int32_t end_frame = (samp->audio_data_length > 0) ? (int32_t)(samp->audio_data_length - 1) + samp->end_adjustment : 0;
			audio.end = (end_frame > 0) ? (size_t)end_frame : 0;
		}
		audio.looping_mode = (SS_SampleLoopingMode)generators[SS_GEN_SAMPLE_MODES];
		audio.is_looping = (audio.looping_mode == SS_LOOP_LOOP ||
		                    audio.looping_mode == SS_LOOP_LOOP_RELEASE);

		/* Playback step */
		uint32_t sr = proc ? proc->sample_rate : 44100;
		audio.playback_step = (float)samp->sample_rate / (float)sr * powf(2.0f, (float)samp->pitch_correction / 1200.0f);

		/* Velocity override */
		int voice_vel = vel;
		if(generators[SS_GEN_VELOCITY] > -1)
			voice_vel = generators[SS_GEN_VELOCITY];

		SS_Voice *voice = ss_voice_create(proc, sr, ch->preset, &audio, note, voice_vel,
		                                  time, target_key, sound_bank_note,
		                                  generators,
		                                  sd->modulators, sd->mod_count, dms);
		if(!voice) continue;

		/* Apply drum / random-pan parameters */
		voice->pitch_offset = drum_pitch_offset;
		voice->reverb_send = drum_reverb_send;
		voice->chorus_send = drum_chorus_send;
		voice->delay_send = drum_delay_send;
		voice->gain *= drum_gain;
		voice->override_pan_active = pan_override_active;
		if(pan_override_active) {
			voice->override_pan = pan_override;
		} else {
			voice->override_pan = 0;
		}
		if(drum_exclusive_override != 0)
			voice->exclusive_class = drum_exclusive_override;

		/* Apply channel generator overrides (AWE32/sysex) to voice base generators */
		if(ch->generator_overrides_enabled) {
			for(int g = 0; g < SS_GEN_COUNT; g++) {
				if(ch->generator_overrides[g] != GENERATOR_OVERRIDE_NO_CHANGE_VALUE)
					voice->generators[g] = ch->generator_overrides[g];
			}
		}

		/* Apply the drum overrides */
		int16_t backup_brightness = ch->midi_controllers[SS_MIDCON_BRIGHTNESS];
		int16_t backup_filter_resonance = ch->midi_controllers[SS_MIDCON_FILTER_RESONANCE];

		if(drum_filter_cutoff != -1) {
			ch->midi_controllers[SS_MIDCON_BRIGHTNESS] = drum_filter_cutoff << 7;
		}
		if(drum_filter_resonance != -1) {
			ch->midi_controllers[SS_MIDCON_FILTER_RESONANCE] = drum_filter_resonance << 7;
		}

		/* Compute initial modulators */
		ss_voice_compute_modulators(voice, ch, time);

		ch->midi_controllers[SS_MIDCON_BRIGHTNESS] = backup_brightness;
		ch->midi_controllers[SS_MIDCON_FILTER_RESONANCE] = backup_filter_resonance;

		/* Initialize panning now that modulators have calculated it. */
		voice->current_pan = (float)voice->modulated_generators[SS_GEN_PAN];

		/* Recalculate the envelopes */
		ss_volume_envelope_recalculate(voice, &voice->volume_env, voice->modulated_generators, voice->target_key, voice->is_in_release, voice->release_start_time, time);
		ss_modulation_envelope_recalculate(&voice->modulation_env, voice->modulated_generators, voice->target_key, voice->is_in_release, voice->release_start_time, time);

		/* Calculate LFO start times */
		voice->vib_lfo_start_time = time + ss_timecents_to_seconds(voice->modulated_generators[SS_GEN_DELAY_VIB_LFO]);
		voice->mod_lfo_start_time = time + ss_timecents_to_seconds(voice->modulated_generators[SS_GEN_DELAY_MOD_LFO]);

		/* Modulate sample offsets (these are not real time) */
		const signed long cursorStartOffset =
		voice->modulated_generators[SS_GEN_START_ADDRS_OFFSET] +
		voice->modulated_generators[SS_GEN_START_ADDRS_COARSE_OFFSET] *
		32768;
		const signed long endOffset =
		voice->modulated_generators[SS_GEN_END_ADDR_OFFSET] +
		voice->modulated_generators[SS_GEN_END_ADDRS_COARSE_OFFSET] *
		32768;
		const signed long loopStartOffset =
		voice->modulated_generators[SS_GEN_STARTLOOP_ADDRS_OFFSET] +
		voice->modulated_generators[SS_GEN_STARTLOOP_ADDRS_COARSE_OFFSET] *
		32768;
		const signed long loopEndOffset =
		voice->modulated_generators[SS_GEN_ENDLOOP_ADDRS_OFFSET] +
		voice->modulated_generators[SS_GEN_ENDLOOP_ADDRS_COARSE_OFFSET] *
		32768;

		/* Clamp the sample offsets */
		const signed long lastSample = (long)(audio.sample_data_len - 1);
		voice->sample.cursor = clamp_long(cursorStartOffset, 0, lastSample);
		voice->sample.end = clamp_long(lastSample + endOffset, 0, lastSample);
		voice->sample.loop_start = clamp_long((long)audio.loop_start + loopStartOffset, 0, lastSample);
		voice->sample.loop_end = clamp_long((long)audio.loop_end + loopEndOffset, 0, lastSample);

		// Swap loops if needed
		if(voice->sample.loop_end < voice->sample.loop_start) {
			const size_t temp = voice->sample.loop_start;
			voice->sample.loop_start = voice->sample.loop_end;
			voice->sample.loop_end = temp;
		}
		if(
		voice->sample.loop_end - voice->sample.loop_start < 1 && /* Disable loop if enabled */
		/* Don't disable on release mode. Testcase:
		 * https://github.com/spessasus/SpessaSynth/issues/174
		 */
		(voice->sample.looping_mode == SS_LOOP_LOOP || voice->sample.looping_mode == SS_LOOP_LOOP_RELEASE)) {
			voice->sample.looping_mode = SS_LOOP_NONE;
		}
		voice->sample.is_looping = (voice->sample.looping_mode == SS_LOOP_LOOP ||
		                            voice->sample.looping_mode == SS_LOOP_LOOP_RELEASE);

		/* Apply portamento */
		voice->portamento_duration = porta_time;
		voice->portamento_from_key = porta_from_key;

		/* Handle exclusive class: cut other voices in same class.
		 * Only kill voices that have already been rendered in a prior
		 * quantum — otherwise a stereo pair triggered by the same note-on
		 * (e.g. hard-panned L/R drum samples sharing one exclusive class)
		 * would release itself. */
		if(voice->exclusive_class > 0) {
			for(size_t vi = 0; vi < ch->voice_count; vi++) {
				SS_Voice *ov = ch->voices[vi];
				if(ov->is_active && ov->has_rendered &&
				   ov->exclusive_class == voice->exclusive_class) {
					ss_voice_exclusive_release(ov, time);
				}
			}
		}

		/* Enforce the global voice cap before the new voice joins. */
		if(proc) enforce_voice_cap(proc, time, ch, own_voices_start);

		channel_add_voice(ch, voice);
		if(proc) proc->voice_count++;
	}

	ss_synthesis_data_free_array(synth_data, sd_count);
	ss_channel_remove_finished_voices(ch);
}
