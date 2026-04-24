/**
 * midi_channel.c
 * Per-MIDI-channel state and note management.
 * Port of midi_channel.ts.
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

extern SS_Voice *ss_voice_create(uint32_t sr,
                                 const SS_BasicPreset *preset,
                                 const SS_AudioSample *audio_sample,
                                 int midi_note, int velocity,
                                 double current_time, int target_key, int real_key,
                                 const int16_t *generators,
                                 const SS_Modulator *modulators, size_t mod_count,
                                 const SS_DynamicModulatorSystem *dms);
/*extern SS_Voice *ss_voice_copy(const SS_Voice *src, double current_time, int real_key);*/
extern void ss_voice_free(SS_Voice *v);
extern void ss_voice_release(SS_Voice *v, double current_time, double min_note_length);
extern void ss_voice_exclusive_release(SS_Voice *v, double current_time);
extern void ss_voice_compute_modulators(SS_Voice *v, const SS_MIDIChannel *ch, double time);
extern bool ss_voice_render(SS_Voice *v, const SS_MIDIChannel *ch,
                            double time_now,
                            float *ol, float *or_,
                            float *reverb,
                            float *chorus,
                            float *delay,
                            int sample_count,
                            SS_InterpolationType interp,
                            float vol_smoothing, float filter_smoothing, float pan_smoothing);
extern float ss_abs_cents_to_hz(int cents);
extern size_t ss_preset_get_synthesis_data(const SS_BasicPreset *preset,
                                           int midi_note, int velocity,
                                           SS_SynthesisData **out);
extern void ss_synthesis_data_free_array(SS_SynthesisData *data, size_t count);
extern bool ss_sample_decode(SS_BasicSample *s);

static void ss_channel_update_tuning(SS_MIDIChannel *ch);
void ss_channel_set_custom_controller(SS_MIDIChannel *ch, SS_CustomController type, float val);
void ss_channel_set_tuning(SS_MIDIChannel *ch, float cents);
static void ss_channel_set_modulation_depth(SS_MIDIChannel *ch, float cents);
static float ss_portamento_time_to_seconds(float portamento_time, float distance);
extern float ss_timecents_to_seconds(int tc);
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
void ss_channel_exclusive_release(SS_MIDIChannel *ch, int note, double time);

#define VOICE_GROW_BY 16

static signed long clamp_ssize(signed long value, signed long min, signed long max) {
	if(value < min) return min;
	if(value > max) return max;
	return value;
}

static void reset_generator_overrides(SS_MIDIChannel *ch) {
	for(int i = 0; i < SS_GEN_COUNT; i++)
		ch->generator_overrides[i] = GENERATOR_OVERRIDE_NO_CHANGE_VALUE;
	ch->generator_overrides_enabled = false;
}

static void reset_generator_offsets(SS_MIDIChannel *ch) {
	for(int i = 0; i < SS_GEN_COUNT; i++)
		ch->generator_offsets[i] = 0;
	ch->generator_offsets_enabled = false;
}

static void reset_parameters_to_defaults(SS_MIDIChannel *ch) {
	ch->data_entry_state = SS_DATAENTRY_IDLE;
	ch->midi_controllers[SS_MIDCON_NRPN_LSB] = 127 << 7;
	ch->midi_controllers[SS_MIDCON_NRPN_MSB] = 127 << 7;
	ch->midi_controllers[SS_MIDCON_RPN_LSB] = 127 << 7;
	ch->midi_controllers[SS_MIDCON_RPN_MSB] = 127 << 7;
	reset_generator_overrides(ch);
	reset_generator_offsets(ch);
}

static bool non_resettable_controllers[128] = {
	[SS_MIDCON_BANK_SELECT] = true,
	[SS_MIDCON_BANK_SELECT_LSB] = true,
	[SS_MIDCON_MAIN_VOLUME] = true,
	[SS_MIDCON_MAIN_VOLUME_LSB] = true,
	[SS_MIDCON_PAN] = true,
	[SS_MIDCON_PAN_LSB] = true,
	[SS_MIDCON_REVERB_DEPTH] = true,
	[SS_MIDCON_TREMOLO_DEPTH] = true,
	[SS_MIDCON_CHORUS_DEPTH] = true,
	[SS_MIDCON_VARIATION_DEPTH] = true,
	[SS_MIDCON_PHASER_DEPTH] = true,
	[SS_MIDCON_SOUND_VARIATION] = true,
	[SS_MIDCON_FILTER_RESONANCE] = true,
	[SS_MIDCON_RELEASE_TIME] = true,
	[SS_MIDCON_ATTACK_TIME] = true,
	[SS_MIDCON_BRIGHTNESS] = true,
	[SS_MIDCON_DECAY_TIME] = true,
	[SS_MIDCON_VIBRATO_RATE] = true,
	[SS_MIDCON_VIBRATO_DEPTH] = true,
	[SS_MIDCON_VIBRATO_DELAY] = true,
	[SS_MIDCON_SOUND_CONTROLLER_10] = true,
	[SS_MIDCON_POLY_MODE_ON] = true,
	[SS_MIDCON_MONO_MODE_ON] = true,
	[SS_MIDCON_OMNI_MODE_ON] = true,
	[SS_MIDCON_OMNI_MODE_OFF] = true,

	// RP-15: Do not reset RPN or NRPN
	[SS_MIDCON_DATA_ENTRY_MSB] = true,
	[SS_MIDCON_DATA_ENTRY_LSB] = true,
	[SS_MIDCON_NRPN_LSB] = true,
	[SS_MIDCON_NRPN_MSB] = true,
	[SS_MIDCON_RPN_LSB] = true,
	[SS_MIDCON_RPN_MSB] = true
};

/* Values come from Falcosoft MidiPlayer 6 */
static const int16_t default_controller_values[SS_MIDI_CONTROLLER_COUNT] = {
	[SS_MIDCON_MAIN_VOLUME] = 100 << 7,
	[SS_MIDCON_BALANCE] = 64 << 7,
	[SS_MIDCON_EXPRESSION] = 127 << 7,
	[SS_MIDCON_PAN] = 64 << 7,

	[SS_MIDCON_PORTAMENTO_ON_OFF] = 127 << 7,

	[SS_MIDCON_FILTER_RESONANCE] = 64 << 7,
	[SS_MIDCON_RELEASE_TIME] = 64 << 7,
	[SS_MIDCON_ATTACK_TIME] = 64 << 7,
	[SS_MIDCON_BRIGHTNESS] = 64 << 7,

	[SS_MIDCON_DECAY_TIME] = 64 << 7,
	[SS_MIDCON_VIBRATO_RATE] = 64 << 7,
	[SS_MIDCON_VIBRATO_DEPTH] = 64 << 7,
	[SS_MIDCON_VIBRATO_DELAY] = 64 << 7,
	[SS_MIDCON_GENERAL_PURPOSE_CONTROLLER_6] = 64 << 7,
	[SS_MIDCON_GENERAL_PURPOSE_CONTROLLER_8] = 64 << 7,

	[SS_MIDCON_RPN_LSB] = 127 << 7,
	[SS_MIDCON_RPN_MSB] = 127 << 7,
	[SS_MIDCON_NRPN_LSB] = 127 << 7,
	[SS_MIDCON_NRPN_MSB] = 127 << 7,

	[NON_CC_INDEX_OFFSET + SS_MODSRC_PITCH_WHEEL] = 64 << 7,
	[NON_CC_INDEX_OFFSET + SS_MODSRC_PITCH_WHEEL_RANGE] = 2 << 7
};

enum { PORTAMENTO_CONTROL_UNSET = 1 };

static const float custom_reset_array[SS_CUSTOM_CTRL_COUNT] = {
	[SS_CUSTOM_CTRL_MODULATION_MULTIPLIER] = 1.0
};

static void reset_vibrato_params(SS_MIDIChannel *ch) {
	ch->channel_vibrato.rate = 0;
	ch->channel_vibrato.depth = 0;
	ch->channel_vibrato.delay = 0;
}

/**
 * https://amei.or.jp/midistandardcommittee/Recommended_Practice/e/rp15.pdf
 * Reset controllers according to RP-15 Recommended Practice.
 */
static void reset_controllers_rp15_compliant(SS_MIDIChannel *ch, double time) {
	for(int i = 0; i < 128; i++) {
		const int16_t reset_value = default_controller_values[i];
		if(
		!non_resettable_controllers[i] &&
		reset_value != ch->midi_controllers[i] &&
		i != SS_MIDCON_PORTAMENTO_CONTROL) {
			ss_channel_controller(ch, i, reset_value >> 7, time);
		}
	}

	ss_channel_pitch_wheel(ch, 8192, -1, time);

	reset_generator_overrides(ch);
	reset_generator_offsets(ch);
}

static inline float drum_params_reverb(int note) {
	if(note == 35 || note == 36) /* Kicks have no reverb */
		return 0.0;
	else
		return 1.0;
}

static void reset_drum_params(SS_MIDIChannel *ch) {
	/* Initialize drum params to defaults */
	for(int k = 0; k < 128; k++) {
		ch->drum_params[k].pitch = 0.0f;
		ch->drum_params[k].gain = 1.0f;
		ch->drum_params[k].exclusive_class = 0;
		ch->drum_params[k].pan = 64;
		ch->drum_params[k].filter_cutoff = 64;
		ch->drum_params[k].filter_resonance = 0;
		ch->drum_params[k].reverb_gain = drum_params_reverb(k);
		ch->drum_params[k].chorus_gain = 0.0f; /* No drums have chorus */
		ch->drum_params[k].delay_gain = 0.0f; /* No drums have delay */
		ch->drum_params[k].rx_note_on = true;
		ch->drum_params[k].rx_note_off = false;
	}
}

static void reset_portamento(SS_MIDIChannel *ch) {
	if(ch->locked_controllers[SS_MIDCON_PORTAMENTO_CONTROL]) return;

	if(ch->synth && ch->synth->master_params.midi_system == SS_SYSTEM_XG) {
		ss_channel_controller(ch, SS_MIDCON_PORTAMENTO_CONTROL, 60, 0);
	} else {
		ss_channel_controller(ch, SS_MIDCON_PORTAMENTO_CONTROL, 0, 0);
	}
}

/* Default controller values per SF2 spec */
static void reset_controllers_to_defaults(SS_MIDIChannel *ch) {
	for(int cc = 0; cc < SS_MIDI_CONTROLLER_COUNT; cc++) {
		const int16_t reset_value = default_controller_values[cc];
		if(ch->midi_controllers[cc] != reset_value && cc < 127) {
			if(cc != SS_MIDCON_PORTAMENTO_CONTROL &&
			   cc != SS_MIDCON_DATA_ENTRY_MSB &&
			   cc != SS_MIDCON_RPN_MSB &&
			   cc != SS_MIDCON_RPN_LSB &&
			   cc != SS_MIDCON_NRPN_MSB &&
			   cc != SS_MIDCON_NRPN_LSB) {
				ss_channel_controller(ch, cc, reset_value >> 7, 0);
			}
		} else {
			/* Out of range, do a regular reset */
			ch->midi_controllers[cc] = reset_value;
		}
	}

	memset(ch->channel_octave_tuning, 0, sizeof(ch->channel_octave_tuning));
	ch->channel_tuning_cents = 0;

	ch->midi_controllers[NON_CC_INDEX_OFFSET + SS_MODSRC_PITCH_WHEEL_RANGE] = 2 << 7; /* Default 2 semitones */
	ss_channel_pitch_wheel(ch, 8192, -1, 0);

	ss_dynamic_modulator_system_init(&ch->dms);

	reset_portamento(ch);
	reset_drum_params(ch);
	reset_vibrato_params(ch);

	ch->assign_mode = 2;
	ch->poly_mode = true;
	ch->rx_channel = ch->channel_number;
	ch->random_pan = false;

	reset_parameters_to_defaults(ch);

	/* Reset custom controllers
	 * Special case: transpose does not get affected
	 */
	const float transpose =
	ch->custom_controllers[SS_CUSTOM_CTRL_TRANSPOSE_FINE];
	memcpy(&ch->custom_controllers, &custom_reset_array, sizeof(ch->custom_controllers));
	ss_channel_set_custom_controller(ch, SS_CUSTOM_CTRL_TRANSPOSE_FINE, transpose);
}

SS_MIDIChannel *ss_channel_new(int channel_number, struct SS_Processor *synth) {
	SS_MIDIChannel *ch = (SS_MIDIChannel *)calloc(1, sizeof(SS_MIDIChannel));
	if(!ch) return NULL;
	ch->channel_number = channel_number;
	ch->synth = synth;
	ch->drum_channel = (channel_number % 16 == 9);
	ch->poly_mode = true;
	ch->rx_channel = channel_number;
	ch->drum_map = 0;
	ch->cc1 = 0x10;
	ch->cc2 = 0x11;
	reset_drum_params(ch);
	reset_controllers_to_defaults(ch);
	return ch;
}

void ss_channel_free(SS_MIDIChannel *ch) {
	if(!ch) return;
	for(size_t i = 0; i < ch->voice_count; i++)
		ss_voice_free(ch->voices[i]);
	free(ch->voices);
	free(ch->sustained_voices);
	ss_dynamic_modulator_system_free(&ch->dms);
	free(ch);
}

/* ── Voice allocation ────────────────────────────────────────────────────── */

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

static void channel_remove_finished_sustained_voices(SS_MIDIChannel *ch) {
	size_t new_count = 0;
	for(size_t i = 0; i < ch->sustained_count; i++) {
		if(ch->sustained_voices[i]->is_active) {
			ch->sustained_voices[new_count++] = ch->sustained_voices[i];
		}
	}
	ch->sustained_count = new_count;
}

static void channel_remove_finished_voices(SS_MIDIChannel *ch) {
	channel_remove_finished_sustained_voices(ch);

	size_t new_count = 0;
	for(size_t i = 0; i < ch->voice_count; i++) {
		if(ch->voices[i]->is_active) {
			ch->voices[new_count++] = ch->voices[i];
		} else {
			ss_voice_free(ch->voices[i]);
		}
	}
	ch->voice_count = new_count;
}

/* ── Note on ─────────────────────────────────────────────────────────────── */

void ss_channel_note_on(SS_MIDIChannel *ch, int note, int vel, double time) {
	if(vel < 1) {
		ss_channel_note_off(ch, note, time);
		return;
	}
	if(vel > 127) vel = 127;

	if(!ch->preset) return;
	if(ch->is_muted) return;

	const int real_key =
	note +
	ch->channel_transpose_key_shift +
	ch->custom_controllers[SS_CUSTOM_CTRL_KEY_SHIFT];
	int internal_midi_note = real_key;

	if(real_key > 127 || real_key < 0) {
		return;
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
	float drum_pan_override = 0.0f; /* 0 = not overridden */

	if(ch->drum_channel) {
		const SS_DrumParameters *dp = &ch->drum_params[internal_midi_note];
		if(!dp->rx_note_on) return;

		drum_pitch_offset = dp->pitch;
		drum_exclusive_override = (int)dp->exclusive_class;
		drum_filter_cutoff = dp->filter_cutoff;
		drum_filter_resonance = dp->filter_resonance;
		drum_reverb_send = dp->reverb_gain;
		drum_chorus_send = dp->chorus_gain;
		drum_delay_send = dp->delay_gain;
		drum_gain = dp->gain;

		/* Drum pan override: 0=random, 64=channel default, else override */
		if(dp->pan != 64) {
			if(dp->pan == 0) {
				/* Random pan [-500, 500] */
				drum_pan_override = (float)(rand() % 1001) - 500.0f;
				if(drum_pan_override == 0.0f) drum_pan_override = 1.0f;
			} else {
				float ch_pan = (float)(ch->midi_controllers[SS_MIDCON_PAN] >> 7) - 64.0f;
				float target = (float)dp->pan - 64.0f + ch_pan;
				if(target < -63.0f) target = -63.0f;
				if(target > 63.0f) target = 63.0f;
				if(target == 0.0f) target = 1.0f;
				drum_pan_override = (target / 63.0f) * 500.0f;
			}
		}
	} else if(ch->random_pan) {
		drum_pan_override = (float)(rand() % 1001) - 500.0f;
		if(drum_pan_override == 0.0f) drum_pan_override = 1.0f;
	}

	const int program = ch->preset->program;
	const int tune = ch->synth->master_params.tunings ? ch->synth->master_params.tunings[program][real_key].midi_note : 0;
	if(tune > 0) {
		internal_midi_note = tune;
	}

	if(/*this.synthCore.masterParameters.monophonicRetriggerMode note implemented ||*/ ch->assign_mode == 0) {
		ss_channel_exclusive_release(ch, note, time);
	}

	/* Get synthesis data for this (note, velocity) */
	SS_SynthesisData *synth_data = NULL;
	size_t sd_count = ss_preset_get_synthesis_data(ch->preset, internal_midi_note, vel, &synth_data);

	SS_Processor *proc = ch->synth;

	SS_DynamicModulatorSystem *dms = &ch->dms;

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

		int target_key = real_key;
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

		SS_Voice *voice = ss_voice_create(sr, ch->preset, &audio, note, voice_vel,
		                                  time, target_key, real_key,
		                                  generators,
		                                  sd->modulators, sd->mod_count, dms);
		if(!voice) continue;

		/* Portamento */
		/* Not implemented correctly */
		int portamento_from_key = -1;
		float portamento_duration = 0;
		// Note: the 14-bit value needs to go down to 7-bit
		const int portamento_time =
		ch->midi_controllers[SS_MIDCON_PORTAMENTO_TIME] >> 7;
		const int porta_control = ch->midi_controllers[SS_MIDCON_PORTAMENTO_CONTROL] >> 7;
		if(
		!ch->drum_channel && /* No portamento on drum channel */
		porta_control != internal_midi_note && /* If the same note, there's no portamento */
		ch->midi_controllers[SS_MIDCON_PORTAMENTO_ON_OFF] >= 8192 && /* (64 << 7) */
		portamento_time > 0 /* 0 duration is no portamento */
		) {
			/* A value of one means the initial portamento */
			if(porta_control > 0) {
				const int diff = abs(internal_midi_note - porta_control);
				portamento_duration = ss_portamento_time_to_seconds(portamento_time, diff);
				portamento_from_key = porta_control;
			}
			/* Set portamento control to previous value */
			ss_channel_controller(ch, SS_MIDCON_PORTAMENTO_CONTROL, internal_midi_note, time);
		}

		/* Apply drum / random-pan parameters */
		voice->pitch_offset = drum_pitch_offset;
		voice->reverb_send = drum_reverb_send;
		voice->chorus_send = drum_chorus_send;
		voice->delay_send = drum_delay_send;
		voice->gain *= drum_gain;
		if(drum_pan_override != 0.0f)
			voice->override_pan = drum_pan_override;
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
		const signed long lastSample = audio.sample_data_len - 1;
		voice->sample.cursor = clamp_ssize(cursorStartOffset, 0, lastSample);
		voice->sample.end = clamp_ssize(lastSample + endOffset, 0, lastSample);
		voice->sample.loop_start = clamp_ssize(audio.loop_start + loopStartOffset, 0, lastSample);
		voice->sample.loop_end = clamp_ssize(audio.loop_end + loopEndOffset, 0, lastSample);

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
		voice->portamento_duration = portamento_duration;
		voice->portamento_from_key = portamento_from_key;

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

		channel_add_voice(ch, voice);
		if(proc) proc->total_voices++;
	}

	ss_synthesis_data_free_array(synth_data, sd_count);
	channel_remove_finished_voices(ch);
}

/* ── Note off ────────────────────────────────────────────────────────────── */

void ss_channel_exclusive_release(SS_MIDIChannel *ch, int note, double time) {
	/* Adjust midiNote by channel key shift */
	const int real_key =
	note +
	ch->channel_transpose_key_shift +
	ch->custom_controllers[SS_CUSTOM_CTRL_KEY_SHIFT];

	for(size_t i = 0; i < ch->voice_count; i++) {
		SS_Voice *v = ch->voices[i];
		if(v->is_active &&
		   v->real_key == real_key) {
			ss_voice_exclusive_release(v, time);
		}
	}
}

void ss_channel_note_off(SS_MIDIChannel *ch, int note, double time) {
	const int real_key = note +
	                     ch->channel_transpose_key_shift +
	                     (int)ch->custom_controllers[SS_CUSTOM_CTRL_KEY_SHIFT];

	/* Drum rx_note_off: if enabled, do a fast exclusive release */
	if(ch->drum_channel && real_key >= 0 && real_key < 128) {
		if(ch->drum_params[real_key].rx_note_off) {
			ss_channel_exclusive_release(ch, note, time);
			return;
		}
	}

	bool sustained = ch->midi_controllers[64] >= 64 << 7; /* CC64 sustain pedal */
	for(size_t i = 0; i < ch->voice_count; i++) {
		SS_Voice *v = ch->voices[i];
		if(!v->is_active || v->is_in_release) continue;
		if(v->real_key != real_key) continue;
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
	channel_remove_finished_voices(ch);
	ch->sustained_count = 0;
}

/* ── Controller change ───────────────────────────────────────────────────── */

enum {
	SS_RPN_PITCH_WHEEL_RANGE = 0x0000,
	SS_RPN_FINE_TUNING = 0x0001,
	SS_RPN_COARSE_TUNING = 0x0002,
	SS_RPN_MODULATION_DEPTH = 0x0005,
	SS_RPN_RESET_PARAMETERS = 0x3fff
};

enum {
	SS_NRPN_MSB_PART_PARAMETER = 0x01,
	SS_NRPN_MSB_AWE32 = 0x7f,
	SS_NRPN_MSB_SF2 = 120
};

/**
 * https://cdn.roland.com/assets/media/pdf/SC-88PRO_OM.pdf
 * http://hummer.stanford.edu/sig/doc/classes/MidiOutput/rpn.html
 * @enum {number}
 */
enum {
	SS_NRPN_GS_LSB_VIBRATO_RATE = 0x08,
	SS_NRPN_GS_LSB_VIBRATO_DEPTH = 0x09,
	SS_NRPN_GS_LSB_VIBRATO_DELAY = 0x0a,

	SS_NRPN_GS_LSB_TVF_FILTER_CUTOFF = 0x20,
	SS_NRPN_GS_LSB_TVF_FILTER_RESONANCE = 0x21,

	SS_NRPN_GS_LSB_EG_ATTACK_TIME = 0x63,
	SS_NRPN_GS_LSB_EG_DECAY_TIME = 0x64,
	SS_NRPN_GS_LSB_EG_RELEASE_TIME = 0x66,

	SS_NRPN_GS_MSB_DRUM_FILTER_CUTOFF = 0x14,
	SS_NRPN_GS_MSB_DRUM_FILTER_Q = 0x15,
	SS_NRPN_GS_MSB_DRUM_PITCH_COARSE = 0x18,
	SS_NRPN_GS_MSB_DRUM_PITCH_FINE = 0x19,
	SS_NRPN_GS_MSB_DRUM_TVA_LEVEL = 0x1a,
	SS_NRPN_GS_MSB_DRUM_PANPOT = 0x1c,
	SS_NRPN_GS_MSB_DRUM_REVERB_SEND = 0x1d,
	SS_NRPN_GS_MSB_DRUM_CHORUS_SEND = 0x1e,
	SS_NRPN_GS_MSB_DRUM_DELAY_SEND = 0x1f
};

void ss_channel_compute_modulators(SS_MIDIChannel *ch, double time) {
	for(size_t v = 0; v < ch->voice_count; v++) {
		ss_voice_compute_modulators(ch->voices[v], ch, time);
	}
}

static void ss_channel_set_generator_offset(SS_MIDIChannel *ch, SS_GeneratorType gen, int val, double time) {
	ch->generator_offsets[gen] = val;
	ch->generator_offsets_enabled = true;
	ss_channel_compute_modulators(ch, time);
}

static void ss_channel_set_generator_override(SS_MIDIChannel *ch, SS_GeneratorType gen, int val, bool realtime, double time) {
	ch->generator_overrides[gen] = val;
	ch->generator_overrides_enabled = true;
	if(realtime) {
		/* Patch voice->generators directly, matching TS setGeneratorOverride realtime path */
		for(size_t vi = 0; vi < ch->voice_count; vi++) {
			SS_Voice *v = ch->voices[vi];
			if(v->is_active) {
				v->generators[gen] = (int16_t)val;
				ss_voice_compute_modulators(v, ch, time);
			}
		}
	}
}

/**
 * SoundBlaster AWE32 NRPN generator mappings.
 * http://archive.gamedev.net/archive/reference/articles/article445.html
 * https://github.com/user-attachments/files/15757220/adip301.pdf
 */
static const SS_GeneratorType AWE_NRPN_GENERATOR_MAPPINGS[] = {
	SS_GEN_DELAY_MOD_LFO,
	SS_GEN_FREQ_MOD_LFO,

	SS_GEN_DELAY_VIB_LFO,
	SS_GEN_FREQ_VIB_LFO,

	SS_GEN_DELAY_MOD_ENV,
	SS_GEN_ATTACK_MOD_ENV,
	SS_GEN_HOLD_MOD_ENV,
	SS_GEN_DECAY_MOD_ENV,
	SS_GEN_SUSTAIN_MOD_ENV,
	SS_GEN_RELEASE_MOD_ENV,

	SS_GEN_DELAY_VOL_ENV,
	SS_GEN_ATTACK_VOL_ENV,
	SS_GEN_HOLD_VOL_ENV,
	SS_GEN_DECAY_VOL_ENV,
	SS_GEN_SUSTAIN_VOL_ENV,
	SS_GEN_RELEASE_VOL_ENV,

	SS_GEN_FINE_TUNE,

	SS_GEN_MOD_LFO_TO_PITCH,
	SS_GEN_VIB_LFO_TO_PITCH,
	SS_GEN_MOD_ENV_TO_PITCH,
	SS_GEN_MOD_LFO_TO_VOLUME,

	SS_GEN_INITIAL_FILTER_FC,
	SS_GEN_INITIAL_FILTER_Q,

	SS_GEN_MOD_LFO_TO_FILTER_FC,
	SS_GEN_MOD_ENV_TO_FILTER_FC,

	SS_GEN_CHORUS_EFFECTS_SEND,
	SS_GEN_REVERB_EFFECTS_SEND
};
static const int AWE_NRPN_GENERATOR_MAPPINGS_COUNT = sizeof(AWE_NRPN_GENERATOR_MAPPINGS) / sizeof(AWE_NRPN_GENERATOR_MAPPINGS[0]);

/* helpers */
static float clip(float v, float min, float max) {
	if(v < min)
		return min;
	else if(v > max)
		return max;
	else
		return v;
}

static double msecToTimecents(double ms) {
	const float cents = 1200.0 * log2(ms / 1000.0);
	if(cents < -32768)
		return -32768;
	else
		return cents;
}

static double hzToCents(double hz) {
	return 6900.0 + 1200 * log2(hz / 440.0);
}

/**
 * Function that emulates AWE32 similarly to fluidsynth
 * https://github.com/FluidSynth/fluidsynth/wiki/FluidFeatures
 *
 * Note: This makes use of findings by mrbumpy409:
 * https://github.com/fluidSynth/fluidsynth/issues/1473
 *
 * The excellent test files are available here, also collected and converted by mrbumpy409:
 * https://github.com/mrbumpy409/AWE32-midi-conversions
 */
static void ss_channel_nrpn_awe32(SS_MIDIChannel *ch, int awe_gen, int data_lsb, int data_msb, double time) {
	int data_value = (data_msb << 7) | data_lsb;
	/* Center the value
	 * Though ranges reported as 0 to 127 only use LSB */
	data_value -= 8192;
	if(awe_gen >= AWE_NRPN_GENERATOR_MAPPINGS_COUNT) return; /* Protect against crash */

	const SS_GeneratorType generator = AWE_NRPN_GENERATOR_MAPPINGS[awe_gen];

	float milliseconds, hertz, centibels, cents;
	switch(generator) {
		default:
			/* This should not happen */
			break;

		/* Delays */
		case SS_GEN_DELAY_MOD_LFO:
		case SS_GEN_DELAY_VIB_LFO:
		case SS_GEN_DELAY_VOL_ENV:
		case SS_GEN_DELAY_MOD_ENV:
			milliseconds = 4 * clip(data_value, 0, 5900);
			// Convert to timecents
			ss_channel_set_generator_override(ch, generator, msecToTimecents(milliseconds), false, time);
			break;

		/* Attacks */
		case SS_GEN_ATTACK_VOL_ENV:
		case SS_GEN_ATTACK_MOD_ENV:
			milliseconds = clip(data_value, 0, 5940);
			/* Convert to timecents */
			ss_channel_set_generator_override(ch, generator, msecToTimecents(milliseconds), false, time);
			break;

		/* Holds */
		case SS_GEN_HOLD_VOL_ENV:
		case SS_GEN_HOLD_MOD_ENV:
			milliseconds = clip(data_value, 0, 8191);
			/* Convert to timecents */
			ss_channel_set_generator_override(ch, generator, msecToTimecents(milliseconds), false, time);
			break;

		/* Decays and releases (share clips and units) */
		case SS_GEN_DECAY_VOL_ENV:
		case SS_GEN_DECAY_MOD_ENV:
		case SS_GEN_RELEASE_VOL_ENV:
		case SS_GEN_RELEASE_MOD_ENV:
			milliseconds = 4 * clip(data_value, 0, 5940);
			/* Convert to timecents */
			ss_channel_set_generator_override(ch, generator, msecToTimecents(milliseconds), false, time);
			break;

		/* LFO frequencies */
		case SS_GEN_FREQ_VIB_LFO:
		case SS_GEN_FREQ_MOD_LFO:
			hertz = 0.084 * (float)data_lsb;
			/* Convert to abs cents */
			ss_channel_set_generator_override(ch, generator, hzToCents(hertz), true, time);
			break;

		/* Sustains */
		case SS_GEN_SUSTAIN_VOL_ENV:
		case SS_GEN_SUSTAIN_MOD_ENV:
			/* 0.75 dB is 7.5 cB */
			centibels = (float)data_lsb * 7.5;
			ss_channel_set_generator_override(ch, generator, centibels, false, time);
			break;

		/* Pitch */
		case SS_GEN_FINE_TUNE:
			/* Data is already centered */
			ss_channel_set_generator_override(ch, generator, data_value, true, time);
			break;

		/* LFO to pitch */
		case SS_GEN_MOD_LFO_TO_PITCH:
		case SS_GEN_VIB_LFO_TO_PITCH:
			cents = clip(data_value, -127, 127) * 9.375;
			ss_channel_set_generator_override(ch, generator, cents, true, time);
			break;

		/* Env to pitch */
		case SS_GEN_MOD_ENV_TO_PITCH:
			cents = clip(data_value, -127, 127) * 9.375;
			ss_channel_set_generator_override(ch, generator, cents, false, time);
			break;

		/* Mod LFO to vol */
		case SS_GEN_MOD_LFO_TO_VOLUME:
			/* 0.1875 dB is 1.875 cB */
			centibels = 1.875 * (float)data_lsb;
			ss_channel_set_generator_override(ch, generator, centibels, true, time);
			break;

		/* Filter Fc */
		case SS_GEN_INITIAL_FILTER_FC: {
			/* Minimum: 100 Hz -> 4335 cents */
			const float fc_cents = 4335.0 + 59 * (float)data_lsb;
			ss_channel_set_generator_override(ch, generator, fc_cents, true, time);
			break;
		}

		/* Filter Q */
		case SS_GEN_INITIAL_FILTER_Q:
			/* Note: this uses the "modulator-ish" approach proposed by mrbumpy409
			 * Here https://github.com/FluidSynth/fluidsynth/issues/1473
			 */
			centibels = 215.0 * ((float)data_lsb / 127.0);
			ss_channel_set_generator_override(ch, generator, centibels, true, time);
			break;

		/* To filter Fc */
		case SS_GEN_MOD_LFO_TO_FILTER_FC:
			cents = clip(data_value, -64, 63) * 56.25;
			ss_channel_set_generator_override(ch, generator, cents, true, time);
			break;

		case SS_GEN_MOD_ENV_TO_FILTER_FC:
			cents = clip(data_value, -64, 63) * 56.25;
			ss_channel_set_generator_override(ch, generator, cents, false, time);
			break;

		/* Effects */
		case SS_GEN_CHORUS_EFFECTS_SEND:
		case SS_GEN_REVERB_EFFECTS_SEND:
			ss_channel_set_generator_override(ch, generator, clip(data_value, 0, 255) * (1000.0 / 255.0), false, time);
			break;
	}
}

static void ss_channel_data_entry_fine(SS_MIDIChannel *ch, int val, double time) {
	ch->midi_controllers[SS_MIDCON_DATA_ENTRY_LSB] = val << 7;
	switch(ch->data_entry_state) {
		default:
			break;

		case SS_DATAENTRY_RP_COARSE:
		case SS_DATAENTRY_RP_FINE: {
			const int rpn_value = ch->midi_controllers[SS_MIDCON_RPN_MSB] |
			                      (ch->midi_controllers[SS_MIDCON_RPN_LSB] >> 7);
			switch(rpn_value) {
				default:
					break;

				/* Pitch bend range fine tune */
				case SS_RPN_PITCH_WHEEL_RANGE:
					if(val == 0) {
						break;
					}
					/* 14-bit value, so upper 7 are coarse and lower 7 are fine! */
					ch->midi_controllers[NON_CC_INDEX_OFFSET + SS_MODSRC_PITCH_WHEEL_RANGE] |= val;
					break;

				/* Fine-tuning */
				case SS_RPN_FINE_TUNING: {
					/* Grab the data and shift */
					const int coarse = (int)ch->custom_controllers[SS_CUSTOM_CTRL_TUNING];
					const int final_tuning = (coarse << 7) | val;
					ss_channel_set_tuning(ch, (float)final_tuning * 0.01220703125); /* Multiply by 8192 / 100 (cent increments)) */
					break;
				}

				/* Modulation depth */
				case SS_RPN_MODULATION_DEPTH: {
					const float current_depth_cents = ch->custom_controllers[SS_CUSTOM_CTRL_MODULATION_MULTIPLIER] * 50.0;
					const float cents = current_depth_cents + ((float)val / 128.0) * 100.0;
					ss_channel_set_modulation_depth(ch, cents);
					break;
				}

				case SS_RPN_RESET_PARAMETERS:
					reset_parameters_to_defaults(ch);
					break;
			}
			break;
		}

		case SS_DATAENTRY_NRP_FINE: {
			const int param_coarse = ch->midi_controllers[SS_MIDCON_NRPN_MSB] >> 7;
			const int param_fine = ch->midi_controllers[SS_MIDCON_NRPN_LSB] >> 7;

			/* SF2 and GS NRPN don't use lsb (but sometimes these are still sent!) */
			if(param_coarse == SS_NRPN_MSB_SF2 ||
			   (param_coarse >= SS_NRPN_GS_MSB_DRUM_FILTER_CUTOFF &&
			    param_coarse <= SS_NRPN_GS_MSB_DRUM_DELAY_SEND) ||
			   param_coarse == SS_NRPN_MSB_PART_PARAMETER) {
				return;
			}
			switch(param_coarse) {
				default:
					/* Unsupported NRPN */
					break;

				case SS_NRPN_MSB_AWE32:
					ss_channel_nrpn_awe32(ch, param_fine, val, ch->midi_controllers[SS_MIDCON_DATA_ENTRY_MSB] >> 7, time);
					break;
			}
			break;
		}
	}
}

static void ss_channel_data_entry_coarse(SS_MIDIChannel *ch, int val, double time) {
	ch->midi_controllers[SS_MIDCON_DATA_ENTRY_MSB] = val << 7;

/*
A note on this vibrato.
This is a completely custom vibrato, with its own oscillator and parameters.
It is disabled by default,
only being enabled when one of the NPRN messages changing it is received
and stays on until the next system-reset.
It was implemented very early in SpessaSynth's development,
because I wanted support for Touhou MIDIs :-)
 */
#define addDefaultVibrato                \
	if(                                  \
	ch->channel_vibrato.delay == 0 &&    \
	ch->channel_vibrato.rate == 0 &&     \
	ch->channel_vibrato.depth == 0) {    \
		ch->channel_vibrato.depth = 50;  \
		ch->channel_vibrato.rate = 8;    \
		ch->channel_vibrato.delay = 0.6; \
	}

	switch(ch->data_entry_state) {
		default:
		case SS_DATAENTRY_IDLE:
			break;

		// Process GS NRPNs
		case SS_DATAENTRY_NRP_COARSE:
		case SS_DATAENTRY_NRP_FINE: {
			const int nrpn_coarse = ch->midi_controllers[SS_MIDCON_NRPN_MSB] >> 7;
			const int nrpn_fine = ch->midi_controllers[SS_MIDCON_NRPN_LSB] >> 7;
			const int data_entry_fine = ch->midi_controllers[SS_MIDCON_DATA_ENTRY_LSB] >> 7;
			switch(nrpn_coarse) {
				default:
					if(val == 64) {
						/* Default value */
						return;
					}
					/* Unrecognized NRPN */
					break;

				case SS_NRPN_GS_MSB_DRUM_FILTER_CUTOFF:
					ch->drum_params[nrpn_fine].filter_cutoff = val;
					break;

				case SS_NRPN_GS_MSB_DRUM_FILTER_Q:
					ch->drum_params[nrpn_fine].filter_resonance = val;
					break;

				case SS_NRPN_GS_MSB_DRUM_PITCH_COARSE: {
					/* Pitch range */
					const bool is_gs = ch->synth && ch->synth->master_params.midi_system == SS_SYSTEM_GS;
					const bool is_50cent = is_gs && ch->bank_lsb != 1;
					const float range = is_50cent ? 50.0 : 100.0;
					ch->drum_params[nrpn_fine].pitch = (((float)val) - 64.0) * range;
					break;
				}

				case SS_NRPN_GS_MSB_DRUM_PITCH_FINE: {
					ch->drum_params[nrpn_fine].pitch += ((float)val) - 64.0;
					break;
				}

				case SS_NRPN_GS_MSB_DRUM_TVA_LEVEL:
					ch->drum_params[nrpn_fine].gain = ((float)val) / 127.0;
					break;

				case SS_NRPN_GS_MSB_DRUM_PANPOT:
					ch->drum_params[nrpn_fine].pan = val;
					break;

				case SS_NRPN_GS_MSB_DRUM_REVERB_SEND:
					ch->drum_params[nrpn_fine].reverb_gain = ((float)val) / 127.0;
					break;

				case SS_NRPN_GS_MSB_DRUM_CHORUS_SEND:
					ch->drum_params[nrpn_fine].chorus_gain = ((float)val) / 127.0;
					break;

				case SS_NRPN_GS_MSB_DRUM_DELAY_SEND:
					ch->drum_params[nrpn_fine].delay_gain = ((float)val) / 127.0;
					if(ch->synth) ch->synth->delay_active = true;
					break;

				case SS_NRPN_MSB_PART_PARAMETER:
					switch(nrpn_fine) {
						default:
							if(val == 64) {
								/* Default value */
								return;
							}
							/* Unrecognized NRPN */
							break;

						case SS_NRPN_GS_LSB_VIBRATO_RATE:
							if(ch->dms.is_active) {
								ss_channel_controller(ch, SS_MIDCON_VIBRATO_RATE, val, time);
								return;
							}
							if(val == 64) {
								/* Default value */
								return;
							}
							addDefaultVibrato;
							ch->channel_vibrato.rate = ((float)val / 64.0) * 8.0;
							break;

						case SS_NRPN_GS_LSB_VIBRATO_DEPTH:
							if(val == 64) {
								/* Default value */
								return;
							}
							addDefaultVibrato;
							ch->channel_vibrato.depth = (float)val / 2.0;
							break;

						case SS_NRPN_GS_LSB_VIBRATO_DELAY:
							if(val == 64) {
								/* Default value */
								return;
							}
							addDefaultVibrato;
							ch->channel_vibrato.delay = (float)val / 64.0 / 3.0;
							break;

						case SS_NRPN_GS_LSB_TVF_FILTER_CUTOFF:
							ss_channel_controller(ch, SS_MIDCON_BRIGHTNESS, val, time);
							break;

						case SS_NRPN_GS_LSB_TVF_FILTER_RESONANCE:
							ss_channel_controller(ch, SS_MIDCON_FILTER_RESONANCE, val, time);
							break;

						case SS_NRPN_GS_LSB_EG_ATTACK_TIME:
							ss_channel_controller(ch, SS_MIDCON_ATTACK_TIME, val, time);
							break;

						case SS_NRPN_GS_LSB_EG_DECAY_TIME:
							ss_channel_controller(ch, SS_MIDCON_DECAY_TIME, val, time);
							break;

						case SS_NRPN_GS_LSB_EG_RELEASE_TIME:
							ss_channel_controller(ch, SS_MIDCON_RELEASE_TIME, val, time);
							break;
					}
					break;

				case SS_NRPN_MSB_AWE32:
					break;

					/* SF2 NRPN */
				case SS_NRPN_MSB_SF2:
					if(nrpn_fine > 100) {
						/* Sf spec:
						 * Note that NRPN Select LSB greater than 100 are for setup only,
						 * and should not be used on their own to select a
						 * Generator parameter.
						 */
						break;
					}
					const SS_GeneratorType gen = (SS_GeneratorType)ch->custom_controllers[SS_CUSTOM_CTRL_SF2_NRPN_GENERATOR_LSB];
					const int offset = (val << 7) | data_entry_fine;
					ss_channel_set_generator_offset(ch, gen, offset, time);
					break;
			}
			break;
		}

		case SS_DATAENTRY_RP_COARSE:
		case SS_DATAENTRY_RP_FINE: {
			const int rpn_value = ch->midi_controllers[SS_MIDCON_RPN_MSB] |
			                      ch->midi_controllers[SS_MIDCON_RPN_LSB] >> 7;
			switch(rpn_value) {
				default:
					/* Unsupported RPN */
					break;

					/* Pitch bend range */
				case SS_RPN_PITCH_WHEEL_RANGE:
					ch->midi_controllers[NON_CC_INDEX_OFFSET + SS_MODSRC_PITCH_WHEEL_RANGE] = val << 7;
					break;

				case SS_RPN_COARSE_TUNING: {
					/* Semitones */
					const int semitones = val - 64;
					ss_channel_set_custom_controller(ch, SS_CUSTOM_CTRL_TUNING_SEMITONES, semitones);
					break;
				}

					/* Fine-tuning */
				case SS_RPN_FINE_TUNING:
					/* Note: this will not work properly unless the lsb is sent!
					 * Here we store the raw value to then adjust in fine
					 */
					ss_channel_set_tuning(ch, (float)(val - 64));
					break;

					/* Modulation depth */
				case SS_RPN_MODULATION_DEPTH:
					ss_channel_set_modulation_depth(ch, (float)val * 100.0);
					break;

				case SS_RPN_RESET_PARAMETERS:
					reset_parameters_to_defaults(ch);
					break;
			}
			break;
		}
	}
}

void ss_channel_controller(SS_MIDIChannel *ch, int cc, int val, double time) {
	if(cc < 0 || cc >= SS_MIDI_CONTROLLER_COUNT) return;
	if(ch->locked_controllers[cc]) return;

	if(
	cc >= SS_MIDCON_MODULATION_WHEEL_LSB &&
	cc <= SS_MIDCON_EFFECT_CONTROL_2_LSB &&
	cc != SS_MIDCON_DATA_ENTRY_LSB) {
		const int actualCCNum = cc - 32;
		if(ch->locked_controllers[actualCCNum]) {
			return;
		}
		// Append the lower nibble to the main controller
		ch->midi_controllers[actualCCNum] =
		(ch->midi_controllers[actualCCNum] & 0x3f80) |
		(val & 0x7f);

		ss_channel_compute_modulators(ch, time);
	}

	ch->midi_controllers[cc] = (int16_t)(val << 7);

	switch(cc) {
		case SS_MIDCON_ALL_NOTES_OFF: /* all notes off */
			ss_channel_all_notes_off(ch, time);
			break;
		case SS_MIDCON_ALL_SOUND_OFF: /* all sound off */
			ss_channel_all_sound_off(ch);
			break;

		case SS_MIDCON_BANK_SELECT:
			ch->bank_msb = val;
			/* Ensure that for XG, drum channels always are 127
			 * Testcase
			 * Dave-Rodgers-D-j-Vu-Anonymous-20200419154845-nonstop2k.com.mid
			 */
			if(ch->channel_number % 16 == 9 &&
			   ch->synth && ch->synth->master_params.midi_system == SS_SYSTEM_XG) {
				ch->bank_msb = 127;
			}
			if(ch->synth && ch->synth->master_params.midi_system == SS_SYSTEM_XG) {
				ch->drum_channel = ch->bank_msb == 120 || ch->bank_msb == 127;
			}
			break;

		case SS_MIDCON_BANK_SELECT_LSB:
			ch->bank_lsb = val;
			break;

		/* Check for RPN and NPRN and data entry */
		case SS_MIDCON_RPN_LSB:
			ch->data_entry_state = SS_DATAENTRY_RP_FINE;
			break;

		case SS_MIDCON_RPN_MSB:
			ch->data_entry_state = SS_DATAENTRY_RP_COARSE;
			break;

		case SS_MIDCON_NRPN_MSB:
			ch->custom_controllers[SS_CUSTOM_CTRL_SF2_NRPN_GENERATOR_LSB] = 0;
			ch->data_entry_state = SS_DATAENTRY_NRP_COARSE;
			break;

		case SS_MIDCON_NRPN_LSB:
			if(
			ch->midi_controllers[SS_MIDCON_NRPN_MSB] >> 7 == SS_NRPN_MSB_SF2) {
				/* If a <100 value has already been sent, reset! */
				if(
				(int)ch->custom_controllers[SS_CUSTOM_CTRL_SF2_NRPN_GENERATOR_LSB] % 100 != 0) {
					ch->custom_controllers[SS_CUSTOM_CTRL_SF2_NRPN_GENERATOR_LSB] = 0;
				}

				if(val == 100) {
					ch->custom_controllers[SS_CUSTOM_CTRL_SF2_NRPN_GENERATOR_LSB] += 100.0;
				} else if(val == 101) {
					ch->custom_controllers[SS_CUSTOM_CTRL_SF2_NRPN_GENERATOR_LSB] += 1000.0;
				} else if(val == 102) {
					ch->custom_controllers[SS_CUSTOM_CTRL_SF2_NRPN_GENERATOR_LSB] += 10000.0;
				} else if(val < 100) {
					ch->custom_controllers[SS_CUSTOM_CTRL_SF2_NRPN_GENERATOR_LSB] += (float)val;
				}
			}
			ch->data_entry_state = SS_DATAENTRY_NRP_FINE;
			break;

		case SS_MIDCON_DATA_ENTRY_MSB:
			ss_channel_data_entry_coarse(ch, val, time);
			break;

		case SS_MIDCON_DATA_ENTRY_LSB:
			ss_channel_data_entry_fine(ch, val, time);
			break;

		case SS_MIDCON_SUSTAIN_PEDAL: /* sustain pedal */
			if(val < 64 && ch->sustained_count > 0) {
				/* Release all sustained voices */
				for(size_t i = 0; i < ch->sustained_count; i++) {
					SS_Voice *v = ch->sustained_voices[i];
					if(v->is_active && !v->is_in_release)
						ss_voice_release(v, time, 0.05);
				}
				ch->sustained_count = 0;
			}
			break;

		case SS_MIDCON_RESET_ALL_CONTROLLERS: /* reset all controllers */
			reset_controllers_rp15_compliant(ch, time);
			break;

		default: /* Compute modulators */
			ss_channel_compute_modulators(ch, time);
			break;
	}
}

/* ── Program change ──────────────────────────────────────────────────────── */

void ss_channel_program_change(SS_MIDIChannel *ch, int program) {
	if(ch->lock_preset) return;
	ch->program = (uint8_t)(program & 0x7F);
	/* Look up preset in the soundbank(s) via processor */
	struct SS_Processor *proc = ch->synth;
	if(!proc) return;
	SS_BasicPreset *p = ss_processor_resolve_preset(proc, ch->channel_number,
	                                                ch->program, ch->bank_msb, ch->bank_lsb,
	                                                ch->drum_channel);
	if(p) {
		ch->preset = p;
		bool is_drum_preset = p->is_gm_gs_drum || p->is_xg_drum;
		if(ch->drum_channel != is_drum_preset) {
			ch->drum_channel = is_drum_preset;
		}
		reset_drum_params(ch);
		return;
	}
}

/* ── Pitch wheel ─────────────────────────────────────────────────────────── */

void ss_channel_pitch_wheel(SS_MIDIChannel *ch, int value, int midi_note, double time) {
	/* value: 0..16383, 8192 = center; midi_note == -1 for channel-wide pitch wheel */
	if(ch->locked_controllers[NON_CC_INDEX_OFFSET + SS_MODSRC_PITCH_WHEEL]) return;

	if(midi_note == -1) {
		/* Global pitch wheel: disable per-note mode */
		ch->per_note_pitch = false;
		ch->midi_controllers[NON_CC_INDEX_OFFSET + SS_MODSRC_PITCH_WHEEL] = (int16_t)value;
		ss_channel_compute_modulators(ch, time);
	} else {
		/* Per-note pitch wheel */
		if(!ch->per_note_pitch) {
			/* Entering per-note mode: seed all notes with the current global value */
			int16_t current = ch->midi_controllers[NON_CC_INDEX_OFFSET + SS_MODSRC_PITCH_WHEEL];
			for(int i = 0; i < 128; i++) ch->pitch_wheels[i] = current;
		}
		ch->per_note_pitch = true;
		ch->pitch_wheels[midi_note] = (int16_t)value;
		/* Recompute modulators only for active voices on this note */
		for(size_t i = 0; i < ch->voice_count; i++) {
			SS_Voice *v = ch->voices[i];
			if(v && v->is_active && v->midi_note == midi_note) {
				ss_voice_compute_modulators(v, ch, time);
			}
		}
	}
}

/* ── Reset controllers ───────────────────────────────────────────────────── */

void ss_channel_reset_controllers(SS_MIDIChannel *ch) {
	reset_controllers_to_defaults(ch);
}

/* ── Render ──────────────────────────────────────────────────────────────── */

void ss_channel_render(SS_MIDIChannel *ch,
                       double time_now,
                       float *out_left, float *out_right,
                       float *reverb,
                       float *chorus,
                       float *delay,
                       uint32_t sample_count) {
	if(ch->is_muted) return;
	SS_Processor *proc = ch->synth;

	SS_InterpolationType interp = proc ? proc->master_params.interpolation_type : SS_INTERP_LINEAR;
	float vol_smoothing = proc ? proc->volume_envelope_smoothing_factor : 0.01f;
	float filter_smoothing = proc ? proc->filter_smoothing_factor : 0.1f;
	float pan_smoothing = proc ? proc->pan_smoothing_factor : 0.1f;

	for(size_t i = 0; i < ch->voice_count; i++) {
		SS_Voice *v = ch->voices[i];
		if(!v->is_active) continue;

		ss_voice_render(v, ch, time_now,
		                out_left, out_right,
		                reverb,
		                chorus,
		                delay,
		                (int)sample_count, interp,
		                vol_smoothing, filter_smoothing, pan_smoothing);
	}
	channel_remove_finished_voices(ch);
	if(proc) proc->total_voices -= (proc->total_voices > (int)ch->voice_count) ? (int)(ch->voice_count - ch->voice_count) : 0;
}

static void ss_channel_update_tuning(SS_MIDIChannel *ch) {
	ch->channel_tuning_cents =
	ch->custom_controllers[SS_CUSTOM_CTRL_TUNING] + /* RPN channel fine tuning */
	ch->custom_controllers[SS_CUSTOM_CTRL_TRANSPOSE_FINE] + /* User tuning (transpose) */
	ch->custom_controllers[SS_CUSTOM_CTRL_MASTER_TUNING] + /* Master tuning, set by sysEx */
	ch->custom_controllers[SS_CUSTOM_CTRL_TUNING_SEMITONES] *
	100; /* RPN channel coarse tuning */
}

void ss_channel_set_custom_controller(SS_MIDIChannel *ch, SS_CustomController type, float val) {
	ch->custom_controllers[type] = val;
	ss_channel_update_tuning(ch);
}

void ss_channel_set_tuning(SS_MIDIChannel *ch, float cents) {
	cents = round(cents);
	ss_channel_set_custom_controller(ch, SS_CUSTOM_CTRL_TUNING, cents);
}

static void ss_channel_set_modulation_depth(SS_MIDIChannel *ch, float cents) {
	cents = round(cents);
	ss_channel_set_custom_controller(ch, SS_CUSTOM_CTRL_MODULATION_MULTIPLIER, cents / 50.0);
}

typedef struct SS_PortamentoLookup {
	uint8_t key;
	float value;
} SS_PortamentoLookup;

static const SS_PortamentoLookup portamento_lookup_table[] = {
	{ 0, 0.0 },
	{ 1, 0.006 },
	{ 2, 0.023 },
	{ 4, 0.05 },
	{ 8, 0.11 },
	{ 16, 0.25 },
	{ 32, 0.5 },
	{ 64, 2.06 },
	{ 80, 4.2 },
	{ 96, 8.4 },
	{ 112, 19.5 },
	{ 116, 26.7 },
	{ 120, 40.0 },
	{ 124, 80.0 },
	{ 127, 480.0 }
};
static const int portamento_lookup_table_count = sizeof(portamento_lookup_table) / sizeof(portamento_lookup_table[0]);

static float ss_portamento_get_lookup(int time) {
	const int count = portamento_lookup_table_count;
	for(int i = 0; i < count; i++) {
		if(portamento_lookup_table[i].key == time)
			return portamento_lookup_table[i].value;
	}

	/* The slow path */
	int lower = -1;
	int lower_index = 0;
	int upper = -1;
	int upper_index = 0;
	for(int i = 0; i < count; i++) {
		int key = portamento_lookup_table[i].key;
		if(key < time && (lower == -1 || key > lower)) {
			lower = key;
			lower_index = i;
		}
		if(key > time && (upper == -1 || key < upper)) {
			upper = key;
			upper_index = i;
		}
	}

	if(lower != -1 && upper != -1) {
		const float lowerTime = portamento_lookup_table[lower_index].value;
		const float upperTime = portamento_lookup_table[upper_index].value;

		return (lowerTime + ((float)(time - lower) * (upperTime - lowerTime)) / (float)(upper - lower));
	}

	return 0;
}

static float ss_portamento_time_to_seconds(float portamento_time, float distance) {
	return ss_portamento_get_lookup(portamento_time) * (distance / 36.0);
}
