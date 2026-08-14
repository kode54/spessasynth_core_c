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
                                       int count, double gain_target);
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
extern double ss_modulation_envelope_get_value(const SS_ModulationEnvelope *env,
                                               double current_time);
extern double ss_abs_cents_to_hz(int cents);
extern double ss_centibel_attenuation_to_gain(double db);

bool ss_voice_render(SS_Voice *v,
                     const SS_MIDIChannel *ch,
                     double time_now,
                     float *out_left, float *out_right,
                     float *reverb,
                     float *chorus,
                     float *delay,
                     int sample_count,
                     SS_InterpolationType interp,
                     double vol_smoothing,
                     double filter_smoothing,
                     double pan_smoothing) {
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
	double cents = (double)v->modulated_generators[SS_GEN_FINE_TUNE] + (double)ch->channel_octave_tuning[v->midi_note] + ch->channel_tuning_cents + ch->current_tuning + v->pitch_offset;
	double semitones = (double)v->modulated_generators[SS_GEN_COARSE_TUNE];

	/* MIDI tuning standard */
	const int program = v->preset->program;
	const SS_TuningEntry *tuning = NULL;
	if(ch->synth) {
		if(ch->synth->tunings &&
		   ch->synth->tunings[program]) {
			tuning = &ch->synth->tunings[program][v->midi_note];
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
		double elapsed = (double)((time_now - v->start_time) / v->portamento_duration);
		if(elapsed > 1.0) elapsed = 1.0;
		double diff = (double)(target_key - v->portamento_from_key);
		semitones -= diff * (1.0 - elapsed);
	}

	/* Scale tuning */
	cents += (double)(target_key - v->sample.root_key) * (double)v->modulated_generators[SS_GEN_SCALE_TUNING];

	/* ── LFOs ─────────────────────────────────────────────────────────── */
	double lowpass_excursion = 0.0;
	double volume_excursion_cb = 0.0;

	/* voice_gain: amplitude generator + LFO amplitude depths (matches TS voiceGain) */
	double voice_gain = v->gain * (1.0 + (double)v->modulated_generators[SS_GEN_AMPLITUDE] / 1000.0);
	if(ch && ch->preset && ch->preset->parent_bank) voice_gain *= ch->preset->parent_bank->gain;
	if(voice_gain < 0.0) voice_gain = 0.0;

	/* Vibrato LFO — triangle wave with phase accumulator, matching TypeScript render_voice.ts.
	 * Triangle: value = 1 - 4*|phase - 0.5|, phase in [0,1).
	 * rateInc = (freqHz * sampleCount) / sampleRate
	 */
	const double vib_start = v->vib_lfo_start_time;
	if(time_now >= vib_start) {
		const int vib_pitch = v->modulated_generators[SS_GEN_VIB_LFO_TO_PITCH];
		const int vib_filter_depth = v->modulated_generators[SS_GEN_VIB_LFO_TO_FILTER_FC];
		const int vib_amplitude_depth = v->modulated_generators[SS_GEN_VIB_LFO_AMPLITUDE_DEPTH];
		if(vib_pitch || vib_filter_depth || vib_amplitude_depth) {
			const double vib_rate = (double)v->modulated_generators[SS_GEN_VIB_LFO_RATE] / 100.0;
			double vib_freq = ss_abs_cents_to_hz(v->modulated_generators[SS_GEN_FREQ_VIB_LFO]) + vib_rate;
			if(vib_freq < 0.0) vib_freq = 0.0;
			const double rate_inc = (vib_freq * (double)sample_count) / (double)ch->synth->sample_rate;
			double phase = v->vib_lfo_phase;
			const double lfo_val = 1.0 - 4.0 * fabs(phase - 0.5);
			phase += rate_inc;
			if(phase >= 1.0) phase -= 1.0;
			v->vib_lfo_phase = phase;
			/* The modulation multiplier is already folded into
			 * vib_pitch by compute_modulator, which scales the mod-wheel
			 * modulator's output by modulationDepth/50.  Applying it here
			 * as well squares it, so an RPN modulation depth other than
			 * the default 50 cents came out wrong. */
			cents += lfo_val * (double)vib_pitch;
			lowpass_excursion += lfo_val * (double)vib_filter_depth;
			voice_gain *= 1.0 - ((lfo_val + 1.0) / 2.0) * ((double)vib_amplitude_depth / 1000.0);
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
			const double mod_rate = (double)v->modulated_generators[SS_GEN_MOD_LFO_RATE] / 100.0;
			double mod_freq = ss_abs_cents_to_hz(v->modulated_generators[SS_GEN_FREQ_MOD_LFO]) + mod_rate;
			if(mod_freq < 0.0) mod_freq = 0.0;
			const double rate_inc = (mod_freq * (double)sample_count) / (double)ch->synth->sample_rate;
			double phase = v->mod_lfo_phase;
			const double lfo_val = 1.0 - 4.0 * fabs(phase - 0.5);
			phase += rate_inc;
			if(phase >= 1.0) phase -= 1.0;
			v->mod_lfo_phase = phase;
			cents += lfo_val * (double)mod_pitch;
			volume_excursion_cb += -lfo_val * (double)mod_vol;
			lowpass_excursion += lfo_val * (double)mod_filter;
			voice_gain *= 1.0 - ((lfo_val + 1.0) / 2.0) * ((double)mod_amplitude_depth / 1000.0);
		}
	}

	/* TODO: Implement proper GS vibrato. Custom vibrato used to be here. */

	/* Mod envelope */
	int mod_env_pitch = v->modulated_generators[SS_GEN_MOD_ENV_TO_PITCH];
	int mod_env_filter = v->modulated_generators[SS_GEN_MOD_ENV_TO_FILTER_FC];
	if(mod_env_pitch || mod_env_filter) {
		const double mod_env = ss_modulation_envelope_get_value(&v->modulation_env, time_now);
		lowpass_excursion += mod_env * (double)mod_env_filter;
		cents += mod_env * (double)mod_env_pitch;
	}

	/* Default resonant modulator: it does not affect the filter gain (neither XG nor GS did that) */
	volume_excursion_cb -= v->resonance_offset;

	/* ── Playback rate ────────────────────────────────────────────────── */
	const double cents_total = (double)cents + (double)semitones * 100.0;
	const int cents_rounded = (int)cents_total;
	/* Round for testing if equal,
	 * But let's allow sub-microtonal tunings, because why not? :-)
	 */
	if(cents_rounded != v->current_tuning_cents) {
		v->current_tuning_cents = cents_rounded;
		v->current_tuning_calculated = pow(2.0, cents_total / 1200.0);
	}

	/* ── Gain ────────────────────────────────────────────────────────── */
	const double gain_target = ss_centibel_attenuation_to_gain(v->modulated_generators[SS_GEN_INITIAL_ATTENUATION]) *
	                           ss_centibel_attenuation_to_gain(volume_excursion_cb);

	/* ── SYNTHESIS ───────────────────────────────────────────────────── */
	bool owned_buf;
	float *buf;

	if(ch && ch->synth) {
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
	double gain = v->volume_env.output_gain;
	/* Compute the new value */
	const bool env_active = ss_volume_envelope_process(&v->volume_env, sample_count, gain_target);
	/* Calculate increase */
	const double gain_inc = (v->volume_env.output_gain - gain) / (double)sample_count;

	/* Low pass filter */
	ss_lowpass_filter_apply(&v->filter, v->modulated_generators, buf, sample_count,
	                        lowpass_excursion, filter_smoothing, gain, gain_inc);

	/* Note, we do not use &&= as it short-circuits!
	 * And we don't do = either as wavetable might've marked it as inactive (end of sample)
	 */
	v->is_active = v->is_active && env_active;

	/* ── Panning and mix ─────────────────────────────────────────────── */
	/* Generator pan in the -500..500 range. The channel pan controller (CC#10)
	 * is already integrated into the generator by the default modulators;
	 * ch->current_pan carries the global/channel system + global MIDI pan. */
	double pan;
	if(v->override_pan_active) {
		pan = v->override_pan;
	} else {
		/* Smooth only the generator pan to prevent clicking. */
		v->current_pan += ((double)v->modulated_generators[SS_GEN_PAN] - v->current_pan) * pan_smoothing;
		pan = v->current_pan;
	}

	/* current_gain folds in the global system/MIDI gain and the channel
	 * system gain (see ss_channel_update_internal_params). */
	const double output_gain = ch->current_gain * voice_gain;
	const double reverb_amt = (double)v->modulated_generators[SS_GEN_REVERB_EFFECTS_SEND] * v->reverb_send;
	const double chorus_amt = (double)v->modulated_generators[SS_GEN_CHORUS_EFFECTS_SEND] * v->chorus_send;

	/* Equal-power panning */
	ss_init_pan_table(); /* just in case */
	double pan_combined = pan + ch->current_pan;
	if(pan_combined < (double)MIN_PAN) pan_combined = (double)MIN_PAN;
	if(pan_combined > (double)MAX_PAN) pan_combined = (double)MAX_PAN;
	int pan_index = (int)(pan_combined - (double)MIN_PAN);
	const float pan_left = ss_panTableLeft[pan_index];
	const float pan_right = ss_panTableRight[pan_index];
	const double gain_left = pan_left * output_gain;
	const double gain_right = pan_right * output_gain;

	for(int i = 0; i < sample_count; i++) {
		const double s = (double)buf[i];
		out_left[i] += gain_left * s;
		out_right[i] += gain_right * s;
	}

	/* Dry output is unconditional; the sends are not.  Upstream returns here
	 * when effects are disabled, so a voice feeds nothing into the buses and
	 * they stay silent for the block. */
	if(ch->synth && !ch->synth->system_params.effects_enabled) {
		if(owned_buf) free(buf);
		return v->is_active;
	}

	if(ch && ch->synth && ch->synth->delay_active && delay) {
		const int delaySend = (int)(ch->midi_controllers[SS_MIDCON_VARIATION_DEPTH] * v->delay_send);
		if(delaySend > 0) {
			const double delayGain =
			output_gain *
			ch->synth->system_params.delay_gain *
			((double)(delaySend >> 7) / 127.0);
			for(int i = 0; i < sample_count; i++) {
				const double s = delayGain * buf[i];
				delay[i] += s;
			}
		}
	}

	if(reverb && reverb_amt > 0) {
		const double reverb_gain = output_gain * (reverb_amt / 1000.0) *
		                           (ch && ch->synth ? ch->synth->system_params.reverb_gain : 1.0);
		for(int i = 0; i < sample_count; i++) {
			const double s = (double)buf[i];
			reverb[i] += s * reverb_gain;
		}
	}

	if(chorus && chorus_amt > 0) {
		const double chorus_gain = output_gain * (chorus_amt / 1000.0) *
		                           (ch && ch->synth ? ch->synth->system_params.chorus_gain : 1.0);
		for(int i = 0; i < sample_count; i++) {
			const double s = (double)buf[i];
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
	if(ch->system_params.is_muted) return;
	SS_Processor *proc = ch->synth;

	/* Per-channel interpolation override falls back to the global setting. */
	SS_InterpolationType interp = SS_INTERP_LINEAR;
	if(ch->system_params.interpolation_type != SS_PARAM_UNSET)
		interp = (SS_InterpolationType)ch->system_params.interpolation_type;
	else if(proc)
		interp = proc->system_params.interpolation_type;
	double vol_smoothing = proc ? proc->volume_envelope_smoothing_factor : 0.01;
	double filter_smoothing = proc ? proc->filter_smoothing_factor : 0.1;
	double pan_smoothing = proc ? proc->pan_smoothing_factor : 0.1;

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
	size_t voice_count = ch->voice_count;
	ss_channel_remove_finished_voices(ch);
	if(proc) proc->voice_count -= (proc->voice_count >= (int)ch->voice_count) ? (int)(voice_count - ch->voice_count) : 0;
}
