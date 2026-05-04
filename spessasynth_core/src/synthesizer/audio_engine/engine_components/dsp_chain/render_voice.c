/**
 * render_voice.c
 * SS_Voice rendering function.
 * Port of voice_render.ts
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>
#if __has_include(<spessasynth_core/spessasynth.h>)
#include <spessasynth_core/midi_enums.h>
#include <spessasynth_core/soundbank_enums.h>
#include <spessasynth_core/synth.h>
#else
#include "spessasynth/midi/midi_enums.h"
#include "spessasynth/soundbank/soundbank_enums.h"
#include "spessasynth/synthesizer/synth.h"
#endif

/* ── Render voice ────────────────────────────────────────────────────────── */

enum { MIN_PAN = -500 };
enum { MAX_PAN = 500 };
enum { PAN_RESOLUTION = MAX_PAN - MIN_PAN };

extern void ss_init_pan_table(void);
extern float ss_panTableLeft[PAN_RESOLUTION + 1];
extern float ss_panTableRight[PAN_RESOLUTION + 1];

extern void ss_channel_remove_finished_voices(SS_MIDIChannel *ch);
extern bool ss_wavetable_get_sample(SS_Voice *v, float *out, int count,
                                    SS_InterpolationType interp);
extern bool ss_volume_envelope_process(SS_VolumeEnvelope *env,
                                       int count, float gain_target);
extern void ss_volume_envelope_start_release(SS_Voice *v,
                                             SS_VolumeEnvelope *env,
                                             const int16_t *mod_gens,
                                             int target_key,
                                             double release_start_time,
                                             double start_time);
extern void ss_modulation_envelope_start_release(SS_ModulationEnvelope *env,
                                                 const int16_t *mod_gens,
                                                 int midi_note,
                                                 double release_start_time,
                                                 double start_time);
extern float ss_modulation_envelope_get_value(const SS_ModulationEnvelope *env,
                                              double current_time);
extern float ss_abs_cents_to_hz(int cents);
extern float ss_lfo_value(double start_time, float freq_hz, double current_time);
extern float ss_centibel_attenuation_to_gain(float db);

bool ss_voice_render(SS_Voice *v,
                     const SS_MIDIChannel *ch,
                     double time_now,
                     float *out_left, float *out_right,
                     float *reverb,
                     float *chorus,
                     float *delay,
                     int sample_count,
                     SS_InterpolationType interp,
                     float vol_smoothing,
                     float filter_smoothing,
                     float pan_smoothing) {
	/* Trigger release if needed */
	if(!v->is_in_release && time_now >= v->release_start_time) {
		v->is_in_release = true;
		ss_volume_envelope_start_release(v, &v->volume_env, v->modulated_generators,
		                                 v->target_key, v->release_start_time,
		                                 v->start_time);
		ss_modulation_envelope_start_release(&v->modulation_env, v->modulated_generators,
		                                     v->midi_note, v->release_start_time,
		                                     v->start_time);
		if(v->sample.looping_mode == SS_LOOP_LOOP_RELEASE)
			v->sample.is_looping = false;
	}

	v->has_rendered = true;
	if(!v->is_active) return v->is_active;

	/* ── TUNING ────────────────────────────────────────────────────────── */
	int target_key = v->target_key;
	float cents = (float)v->modulated_generators[SS_GEN_FINE_TUNE] + (float)ch->channel_octave_tuning[v->midi_note] + (float)ch->channel_tuning_cents + v->pitch_offset;
	float semitones = (float)v->modulated_generators[SS_GEN_COARSE_TUNE];

	/* MIDI tuning standard */
	const int program = v->preset->program;
	const SS_TuningEntry *tuning = NULL;
	if(ch->synth) {
		if(ch->synth->master_params.tunings &&
		   ch->synth->master_params.tunings[program]) {
			tuning = &ch->synth->master_params.tunings[program][v->real_key];
		}
	}
	if(tuning) {
		/* Tuning is encoded as key and cents offset
		 * Override key, otherwise -1
		 */
		if(tuning->midi_note >= 0) target_key = tuning->midi_note;
		/* Add microtonal tuning */
		cents += tuning->cent_tuning;
	}

	/* Portamento */
	if(v->portamento_from_key > -1) {
		float elapsed = (float)((time_now - v->start_time) / v->portamento_duration);
		if(elapsed > 1.0f) elapsed = 1.0f;
		float diff = (float)(target_key - v->portamento_from_key);
		semitones -= diff * (1.0f - elapsed);
	}

	/* Scale tuning */
	cents += (float)(target_key - v->sample.root_key) * (float)v->modulated_generators[SS_GEN_SCALE_TUNING];

	/* ── LFOs ─────────────────────────────────────────────────────────── */
	float lowpass_excursion = 0.0f;
	float volume_excursion_cb = 0.0f;
	float mod_mult = ch->custom_controllers[SS_CUSTOM_CTRL_MODULATION_MULTIPLIER];

	/* voice_gain: amplitude generator + LFO amplitude depths (matches TS voiceGain) */
	float voice_gain = v->gain * (1.0f + (float)v->modulated_generators[SS_GEN_AMPLITUDE] / 1000.0f);
	if(ch && ch->preset && ch->preset->parent_bank) voice_gain *= ch->preset->parent_bank->gain;
	if(voice_gain < 0.0f) voice_gain = 0.0f;

	/* Vibrato LFO — triangle wave with phase accumulator, matching TypeScript render_voice.ts.
	 * Triangle: value = 1 - 4*|phase - 0.5|, phase in [0,1).
	 * rateInc = (freqHz * sampleCount) / sampleRate
	 */
	const double vib_start = v->vib_lfo_start_time;
	if(time_now >= vib_start) {
		int vib_pitch = v->modulated_generators[SS_GEN_VIB_LFO_TO_PITCH];
		int vib_filter_depth = v->modulated_generators[SS_GEN_VIB_LFO_TO_FILTER_FC];
		int vib_amplitude_depth = v->modulated_generators[SS_GEN_VIB_LFO_AMPLITUDE_DEPTH];
		if(vib_pitch || vib_filter_depth || vib_amplitude_depth) {
			float vib_rate = (float)v->modulated_generators[SS_GEN_VIB_LFO_RATE] / 100.0f;
			float vib_freq = ss_abs_cents_to_hz(v->modulated_generators[SS_GEN_FREQ_VIB_LFO]) + vib_rate;
			if(vib_freq < 0.0f) vib_freq = 0.0f;
			float rate_inc = (vib_freq * (float)sample_count) / (float)ch->synth->sample_rate;
			float phase = v->vib_lfo_phase;
			float lfo_val = 1.0f - 4.0f * fabsf(phase - 0.5f);
			phase += rate_inc;
			if(phase >= 1.0f) phase -= 1.0f;
			v->vib_lfo_phase = phase;
			cents += lfo_val * ((float)vib_pitch * mod_mult);
			lowpass_excursion += lfo_val * (float)vib_filter_depth;
			voice_gain *= 1.0f - ((lfo_val + 1.0f) / 2.0f) * ((float)vib_amplitude_depth / 1000.0f);
		}
	}

	/* Mod LFO — same triangle wave approach */
	double mod_start = v->mod_lfo_start_time;
	if(time_now >= mod_start) {
		int mod_pitch = v->modulated_generators[SS_GEN_MOD_LFO_TO_PITCH];
		int mod_vol = v->modulated_generators[SS_GEN_MOD_LFO_TO_VOLUME];
		int mod_filter = v->modulated_generators[SS_GEN_MOD_LFO_TO_FILTER_FC];
		int mod_amplitude_depth = v->modulated_generators[SS_GEN_MOD_LFO_AMPLITUDE_DEPTH];
		if(mod_pitch || mod_vol || mod_filter || mod_amplitude_depth) {
			float mod_rate = (float)v->modulated_generators[SS_GEN_MOD_LFO_RATE] / 100.0f;
			float mod_freq = ss_abs_cents_to_hz(v->modulated_generators[SS_GEN_FREQ_MOD_LFO]) + mod_rate;
			if(mod_freq < 0.0f) mod_freq = 0.0f;
			float rate_inc = (mod_freq * (float)sample_count) / (float)ch->synth->sample_rate;
			float phase = v->mod_lfo_phase;
			float lfo_val = 1.0f - 4.0f * fabsf(phase - 0.5f);
			phase += rate_inc;
			if(phase >= 1.0f) phase -= 1.0f;
			v->mod_lfo_phase = phase;
			cents += lfo_val * ((float)mod_pitch * mod_mult);
			volume_excursion_cb += -lfo_val * (float)mod_vol;
			lowpass_excursion += lfo_val * (float)mod_filter;
			voice_gain *= 1.0f - ((lfo_val + 1.0f) / 2.0f) * ((float)mod_amplitude_depth / 1000.0f);
		}
	}

	/* Channel vibrato (GS NRPN) — sine wave, only when mod wheel == 0 (matches TS) */
	if(ch->channel_vibrato.depth > 0.0f &&
	   ch->midi_controllers[SS_MIDCON_MODULATION_WHEEL] == 0) {
		float ch_vib = ss_lfo_value(v->start_time + ch->channel_vibrato.delay,
		                            ch->channel_vibrato.rate, time_now);
		cents += ch_vib * ch->channel_vibrato.depth;
	}

	/* Mod envelope */
	int mod_env_pitch = v->modulated_generators[SS_GEN_MOD_ENV_TO_PITCH];
	int mod_env_filter = v->modulated_generators[SS_GEN_MOD_ENV_TO_FILTER_FC];
	if(mod_env_pitch || mod_env_filter) {
		float mod_env = ss_modulation_envelope_get_value(&v->modulation_env, time_now);
		lowpass_excursion += mod_env * (float)mod_env_filter;
		cents += mod_env * (float)mod_env_pitch;
	}

	/* Default resonant modulator: it does not affect the filter gain (neither XG nor GS did that) */
	volume_excursion_cb -= v->resonance_offset;

	/* ── Playback rate ────────────────────────────────────────────────── */
	int cents_total = (int)(cents + semitones * 100.0f);
	if(cents_total != v->current_tuning_cents) {
		v->current_tuning_cents = cents_total;
		v->current_tuning_calculated = pow(2.0, (double)cents_total / 1200.0);
	}

	/* ── Gain ────────────────────────────────────────────────────────── */
	const float gain_target = ss_centibel_attenuation_to_gain(v->modulated_generators[SS_GEN_INITIAL_ATTENUATION]) *
	                          ss_centibel_attenuation_to_gain(volume_excursion_cb);

	/* ── SYNTHESIS ───────────────────────────────────────────────────── */
	bool owned_buf;
	float *buf;

	if(ch->synth) {
		buf = &ch->synth->mix_buffer[0];
		memset(buf, 0, sizeof(float) * SS_MAX_SOUND_CHUNK);
		owned_buf = false;
	} else {
		buf = (float *)calloc((size_t)sample_count, sizeof(float));
		if(!buf) {
			v->is_active = false;
			return false;
		}
		owned_buf = true;
	}

	/* Looping mode 2 (start-on-release): no oscillator, only envelope */
	if(v->sample.looping_mode == SS_LOOP_START_RELEASE && !v->is_in_release) {
		bool active = ss_volume_envelope_process(&v->volume_env, sample_count,
		                                         gain_target);
		if(!active) v->is_active = false;
		if(owned_buf) free(buf);
		return v->is_active;
	}

	/* Wavetable oscillator */
	v->is_active = ss_wavetable_get_sample(v, buf, sample_count, interp);

	/* Volume envelope */
	/* Get the previous value */
	float gain = v->volume_env.output_gain;
	/* Compute the new value */
	const bool env_active = ss_volume_envelope_process(&v->volume_env, sample_count, gain_target);
	/* Calculate increase */
	const float gain_inc = (v->volume_env.output_gain - gain) / (float)sample_count;

	/* Low pass filter */
	ss_lowpass_filter_apply(&v->filter, v->modulated_generators, buf, sample_count,
	                        lowpass_excursion, filter_smoothing, gain, gain_inc);

	/* Note, we do not use &&= as it short-circuits!
	 * And we don't do = either as wavetable might've marked it as inactive (end of sample)
	 */
	v->is_active = v->is_active && env_active;

	/* ── Panning and mix ─────────────────────────────────────────────── */
	float pan_val;
	if(v->override_pan != 0.0f) {
		pan_val = v->override_pan / 500.0f;
	} else {
		/* Smooth only the generator pan (matches TS: currentPan tracks modulated[pan] only).
		 * v->current_pan is stored in the -500..500 generator range.
		 * Channel pan is already integrated into the generator by the default modulators. */
		v->current_pan += ((float)v->modulated_generators[SS_GEN_PAN] - v->current_pan) * pan_smoothing;
		pan_val = v->current_pan / 500.0f;
	}

	/* Master volume applied here */
	const float output_gain = ch->synth ? ch->synth->master_params.master_volume * ch->synth->midi_volume * voice_gain : voice_gain;
	const float reverb_amt = (float)v->modulated_generators[SS_GEN_REVERB_EFFECTS_SEND] * v->reverb_send;
	const float chorus_amt = (float)v->modulated_generators[SS_GEN_CHORUS_EFFECTS_SEND] * v->chorus_send;

	/* Equal-power panning */
	ss_init_pan_table(); /* just in case */
	int pan_index = (int)((pan_val + 1.0f) * 500.0f);
	if(pan_index < 0) pan_index = 0;
	if(pan_index > 1000) pan_index = 1000;
	const float pan_left = ss_panTableLeft[pan_index];
	const float pan_right = ss_panTableRight[pan_index];
	const float gain_left = pan_left * output_gain;
	const float gain_right = pan_right * output_gain;

	if(ch->synth && ch->synth->delay_active && delay) {
		const int delaySend = (int)(ch->midi_controllers[SS_MIDCON_VARIATION_DEPTH] * v->delay_send);
		if(delaySend > 0) {
			const float delayGain =
			output_gain *
			ch->synth->master_params.delay_gain *
			((float)(delaySend >> 7) / 127.0f);
			for(int i = 0; i < sample_count; i++) {
				const float s = delayGain * buf[i];
				delay[i] += s;
			}
		}
	}

	for(int i = 0; i < sample_count; i++) {
		float s = buf[i];
		out_left[i] += s * gain_left;
		out_right[i] += s * gain_right;
	}

	if(reverb && reverb_amt > 0) {
		const float reverb_gain = output_gain * (reverb_amt / 1000.0f);
		for(int i = 0; i < sample_count; i++) {
			float s = buf[i];
			reverb[i] += s * reverb_gain;
		}
	}

	if(chorus && chorus_amt > 0) {
		const float chorus_gain = output_gain * (chorus_amt / 1000.0f);
		for(int i = 0; i < sample_count; i++) {
			float s = buf[i];
			chorus[i] += s * chorus_gain;
		}
	}

	if(owned_buf) free(buf);
	return v->is_active;
}

/* ── Render channel ──────────────────────────────────────────────────────── */

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
	ss_channel_remove_finished_voices(ch);
	if(proc) proc->total_voices -= (proc->total_voices > (int)ch->voice_count) ? (int)(ch->voice_count - ch->voice_count) : 0;
}
