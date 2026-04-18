/**
 * voice.c
 * SS_Voice creation, copying, and rendering helpers.
 * Port of voice.ts and render_voice.ts.
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

extern void ss_volume_envelope_init(SS_VolumeEnvelope *env,
                                    uint32_t sr, int initial_decay_cb);
extern void ss_lowpass_filter_init(SS_LowpassFilter *f, uint32_t sr);
extern bool ss_wavetable_get_sample(SS_Voice *v, float *out, int count,
                                    SS_InterpolationType interp);
extern bool ss_volume_envelope_process(SS_VolumeEnvelope *env,
                                       int count, float gain_target);
/*extern void ss_volume_envelope_recalculate(SS_Voice *v,
                                           SS_VolumeEnvelope *env,
                                           const int16_t *mod_gens,
                                           int target_key,
                                           bool is_in_release,
                                           double release_start_time,
                                           double start_time);*/
extern void ss_volume_envelope_start_release(SS_Voice *v,
                                             SS_VolumeEnvelope *env,
                                             const int16_t *mod_gens,
                                             int target_key,
                                             double release_start_time,
                                             double start_time);
/*extern void ss_modulation_envelope_recalculate(SS_ModulationEnvelope *env,
                                               const int16_t *mod_gens,
                                               int midi_note,
                                               bool is_in_release,
                                               double release_start_time,
                                               double start_time);*/
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

#define MIN_NOTE_LENGTH 0.05
#define MIN_EXCLUSIVE_LENGTH 0.01
#define EXCLUSIVE_CUTOFF_TIME (-2320)
#define EXCLUSIVE_MOD_CUTOFF_TIME (-1130)

/* ── Create ──────────────────────────────────────────────────────────────── */

SS_Voice *ss_voice_create(uint32_t sample_rate,
                          const SS_BasicPreset *preset,
                          const SS_AudioSample *audio_sample,
                          int midi_note, int velocity,
                          double current_time, int target_key, int real_key,
                          const int16_t *generators,
                          const SS_Modulator *modulators, size_t mod_count,
                          const SS_DynamicModulatorSystem *dms) {
	SS_Voice *v = (SS_Voice *)calloc(1, sizeof(SS_Voice));
	if(!v) return NULL;

	v->preset = preset;
	v->sample = *audio_sample;
	v->midi_note = midi_note;
	v->velocity = velocity;
	v->start_time = current_time;
	v->is_active = true;
	v->target_key = target_key;
	v->real_key = real_key;
	v->release_start_time = INFINITY;
	v->current_tuning_calculated = 1.0;
	v->portamento_from_key = -1;
	v->gain = 1.0f;
	v->pitch_offset = 0.0f;
	/* Match TypeScript Voice.setup(): vibLfoPhase = modLfoPhase = 0.25 */
	v->vib_lfo_phase = 0.25f;
	v->mod_lfo_phase = 0.25f;
	v->reverb_send = 1.0f;
	v->chorus_send = 1.0f;
	v->delay_send = 1.0f;
	v->has_rendered = false;

	if(v->sample.loop_end < v->sample.loop_start) {
		const size_t temp = v->sample.loop_start;
		v->sample.loop_start = v->sample.loop_end;
		v->sample.loop_end = temp;
	}
	if(v->sample.loop_end - v->sample.loop_start < 1) {
		/* Disable loop if enabled
		 * Don't disable on release mode. Testcase:
		 * https://github.com/spessasus/SpessaSynth/issues/174
		 */
		if(v->sample.looping_mode == SS_LOOP_LOOP || v->sample.looping_mode == SS_LOOP_LOOP_RELEASE) {
			v->sample.looping_mode = SS_LOOP_NONE;
			v->sample.is_looping = false;
		}
	}

	memcpy(v->generators, generators, SS_GEN_COUNT * sizeof(int16_t));
	memcpy(v->modulated_generators, generators, SS_GEN_COUNT * sizeof(int16_t));

	v->exclusive_class = generators[SS_GEN_EXCLUSIVE_CLASS];

	/* Copy modulators */
	if(mod_count > 0 || dms->is_active) {
		size_t adjusted_mod_count = mod_count + (dms->is_active ? dms->modulator_count : 0);
		v->modulators = (SS_Modulator *)malloc(adjusted_mod_count * sizeof(SS_Modulator));
		if(v->modulators) {
			for(size_t i = 0; i < mod_count; i++)
				v->modulators[i] = ss_modulator_copy(&modulators[i]);
		}
		if(dms->is_active) {
			for(size_t i = 0, count = dms->modulator_count; i < count; i++)
				v->modulators[i + mod_count] = ss_modulator_copy(&dms->modulators[i].modulator);
		}
		v->modulator_count = adjusted_mod_count;
	}

	ss_lowpass_filter_init(&v->filter, sample_rate);
	ss_volume_envelope_init(&v->volume_env, sample_rate,
	                        generators[SS_GEN_SUSTAIN_VOL_ENV]);

	/* Store current_pan in generator units (-500..500) to match TS smoothing behaviour */
	v->current_pan = (float)v->modulated_generators[SS_GEN_PAN];

	return v;
}

/* ── Copy ────────────────────────────────────────────────────────────────── */

#if 0
SS_Voice *ss_voice_copy(const SS_Voice *src, double current_time, int real_key) {
	SS_Voice *v = (SS_Voice *)calloc(1, sizeof(SS_Voice));
	if(!v) return NULL;

	*v = *src; /* shallow copy of all fields */

	/* Re-initialize filter state (don't copy x1/x2/y1/y2) */
	v->filter.x1 = v->filter.x2 = v->filter.y1 = v->filter.y2 = 0.0;
	v->filter.initialized = false;
	v->filter.last_target_cutoff = 1e38f;

	/* Re-init envelopes */
	memset(&v->volume_env, 0, sizeof(v->volume_env));
	memset(&v->modulation_env, 0, sizeof(v->modulation_env));
	v->volume_env.sample_rate = src->volume_env.sample_rate;
	v->volume_env.can_end_on_silent_sustain = src->volume_env.can_end_on_silent_sustain;

	v->start_time = current_time;
	v->real_key = real_key;
	v->release_start_time = INFINITY;
	v->is_in_release = false;
	v->is_active = true;

	/* Deep copy modulators */
	v->modulators = NULL;
	v->modulator_count = 0;
	if(src->modulator_count > 0 && src->modulators) {
		v->modulators = (SS_Modulator *)malloc(src->modulator_count * sizeof(SS_Modulator));
		if(v->modulators) {
			for(size_t i = 0; i < src->modulator_count; i++)
				v->modulators[i] = ss_modulator_copy(&src->modulators[i]);
			v->modulator_count = src->modulator_count;
		}
	}

	ss_volume_envelope_recalculate(v, &v->volume_env, v->modulated_generators,
	                               v->target_key, false, 0.0, current_time);
	ss_modulation_envelope_recalculate(&v->modulation_env, v->modulated_generators,
	                                   v->midi_note, false, 0.0, current_time);
	return v;
}
#endif

/* ── Free ────────────────────────────────────────────────────────────────── */

void ss_voice_free(SS_Voice *v) {
	if(!v) return;
	free(v->modulators);
	free(v);
}

/* ── Release ─────────────────────────────────────────────────────────────── */

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

/* ── Compute modulators ──────────────────────────────────────────────────── */

float ss_modcurve_get_value(int transform_type, SS_ModulatorCurveType curve_type, int index_0_to_16_383);

/*
 * Evaluate the modulator source value.
 * For now, supports CC (direct), velocity, key, pressure, pitch wheel.
 */

static float get_source_value(const SS_MIDIChannel *ch, const SS_Voice *v,
                              uint16_t source_enum) {
	/* Decode packed source_enum:
	 * bits 11-10: curve (0=linear, 1=concave, 2=convex, 3=switch)
	 * bit 9: is_bipolar
	 * bit 8: is_negative
	 * bit 7: is_cc
	 * bits 6-0: index
	 */
	bool is_cc = (source_enum & 0x80) != 0;
	uint8_t idx = source_enum & 0x7F;
	bool is_negative = (source_enum & 0x100) != 0;
	bool is_bipolar = (source_enum & 0x200) != 0;
	int curve = (source_enum >> 10) & 3;

	int raw = 0;
	if(is_cc) {
		raw = ch->midi_controllers[idx];
	} else {
		switch(idx) {
			case SS_MODSRC_NO_CONTROLLER:
				raw = 16383;
				break;
			case SS_MODSRC_NOTE_ON_VELOCITY:
				raw = v->velocity << 7;
				break;
			case SS_MODSRC_NOTE_ON_KEYNUM:
				raw = v->midi_note << 7;
				break;
			case SS_MODSRC_POLY_PRESSURE:
				raw = v->pressure << 7;
				break;
			case SS_MODSRC_PITCH_WHEEL:
				raw = ch->per_note_pitch ? (int)ch->pitch_wheels[v->real_key] : ch->midi_controllers[SS_MODSRC_PITCH_WHEEL + NON_CC_INDEX_OFFSET];
				break;
			default:
				if(idx + NON_CC_INDEX_OFFSET >= SS_MIDI_CONTROLLER_COUNT)
					raw = 0;
				else
					raw = ch->midi_controllers[idx + NON_CC_INDEX_OFFSET];
				break;
		}
	}

	if(raw < 0)
		raw = 0;
	else if(raw > 16383)
		raw = 16383;

	const int transform = (SS_ModulatorTransformType)((is_bipolar ? 2 : 0) | (is_negative ? 1 : 0));

	return ss_modcurve_get_value(transform, (SS_ModulatorCurveType)curve, raw);
}

static const float EFFECT_MODULATOR_TRANSFORM_MULTIPLIER = 1000 / 200;

void ss_voice_compute_modulators(SS_Voice *v, const SS_MIDIChannel *ch,
                                 double time) {
	/* Reset modulated generators to base values */
	memcpy(v->modulated_generators, v->generators, SS_GEN_COUNT * sizeof(int16_t));

	v->resonance_offset = 0.0f;

	for(size_t mi = 0; mi < v->modulator_count; mi++) {
		const SS_Modulator *m = &v->modulators[mi];
		if(m->dest_enum >= SS_GEN_COUNT) continue;

		if(!m->transform_amount) continue;

		float src = get_source_value(ch, v, m->source_enum);
		float asrc = (m->amount_source_enum != 0) ? get_source_value(ch, v, m->amount_source_enum) : 1.0f;

		/* Effect modulators: scale CC91/CC93 as in spessasynth */
		float transform_amount = (float)m->transform_amount;
		if(m->is_effect_modulator && transform_amount <= 1000) {
			transform_amount *= EFFECT_MODULATOR_TRANSFORM_MULTIPLIER;
			if(transform_amount > 1000.0) transform_amount = 1000.0;
		}

		float val = src * asrc * transform_amount;

		if(m->transform_type == SS_MODTRANS_ABSOLUTE) {
			/* Abs value */
			val = fabs(val);
		}

		/* Default resonant modulator: track separately */
		if(m->is_default_resonant_modulator) {
			/* Half the gain, negates the filter */
			v->resonance_offset = (val > 0) ? val / 2 : 0;
		}

		if(m->is_mod_wheel_modulator) {
			val *= ch->custom_controllers[SS_CUSTOM_CTRL_MODULATION_MULTIPLIER];
		}

		{
			int16_t g = v->modulated_generators[m->dest_enum];
			int32_t new_val = (int32_t)g + (int32_t)val;
			new_val = ss_generator_clamp((SS_GeneratorType)m->dest_enum, (int16_t)new_val);
			v->modulated_generators[m->dest_enum] = (int16_t)new_val;
			val = new_val;
		}
		/* Update stored current_value (for snapshot purposes) */
		((SS_Modulator *)m)->current_value = val;
	}

	/* Apply generator-specific limits to all modulated generators.
	 * Matches TypeScript computeModulators second pass (compute_modulator.ts lines 119-130).
	 * This clamps base generator values (e.g. sustainVolEnv = -461 from preset+inst summing)
	 * that were never touched by any modulator but still need to be in spec range. */
	for(int g = 0; g < SS_GEN_COUNT; g++) {
		v->modulated_generators[g] = ss_generator_clamp((SS_GeneratorType)g, v->modulated_generators[g]);
	}
}

/* ── Render voice ────────────────────────────────────────────────────────── */

enum { MIN_PAN = -500 };
enum { MAX_PAN = 500 };
enum { PAN_RESOLUTION = MAX_PAN - MIN_PAN };

extern void ss_init_pan_table(void);
extern float ss_panTableLeft[PAN_RESOLUTION + 1];
extern float ss_panTableRight[PAN_RESOLUTION + 1];

bool ss_voice_render(SS_Voice *v,
                     const SS_MIDIChannel *ch,
                     double time_now,
                     float *out_left, float *out_right,
                     float *reverb_left, float *reverb_right,
                     float *chorus_left, float *chorus_right,
                     float *delay_left, float *delay_right,
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
		 * Channel pan is added at use time, not during smoothing. */
		v->current_pan += ((float)v->modulated_generators[SS_GEN_PAN] - v->current_pan) * pan_smoothing;
		float ch_pan = (float)ch->midi_controllers[SS_MIDCON_PAN] / (63.5f * 128.0f) - 1.0f;
		pan_val = v->current_pan / 500.0f + ch_pan;
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
	float gain_left = pan_left * output_gain;
	float gain_right = pan_right * output_gain;

	if(ch->synth && ch->synth->delay_active && delay_left && delay_right) {
		const int delaySend = ch->midi_controllers[SS_MIDCON_VARIATION_DEPTH] * v->delay_send;
		if(delaySend > 0) {
			const float delayGain =
			output_gain *
			ch->synth->master_params.delay_gain *
			((float)(delaySend >> 7) / 127.0);
			for(int i = 0; i < sample_count; i++) {
				const float delaySample = delayGain * buf[i];
				delay_left[i] += delaySample;
				delay_right[i] += delaySample;
			}
		}
	}

	for(int i = 0; i < sample_count; i++) {
		float s = buf[i];
		out_left[i] += s * gain_left;
		out_right[i] += s * gain_right;
	}

	if(reverb_left && reverb_right && reverb_amt > 0) {
		const float reverb_gain = output_gain * (reverb_amt / 1000.0);
		gain_left = pan_left * reverb_gain;
		gain_right = pan_right * reverb_gain;
		for(int i = 0; i < sample_count; i++) {
			float s = buf[i];
			reverb_left[i] += s * gain_left;
			reverb_right[i] += s * gain_right;
		}
	}

	if(chorus_left && chorus_right && chorus_amt > 0) {
		const float chorus_gain = output_gain * (reverb_amt / 1000.0);
		gain_left = pan_left * chorus_gain;
		gain_right = pan_right * chorus_gain;
		for(int i = 0; i < sample_count; i++) {
			float s = buf[i];
			chorus_left[i] += s * gain_left;
			chorus_right[i] += s * gain_right;
		}
	}

	if(owned_buf) free(buf);
	return v->is_active;
}
