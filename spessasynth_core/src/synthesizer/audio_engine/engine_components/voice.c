/**
 * voice.c
 * SS_Voice creation, copying, and rendering helpers.
 * Port of voice.ts
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
                                    uint32_t sr, uint32_t buffer_size);
extern void ss_lowpass_filter_init(SS_LowpassFilter *f, uint32_t sr);
/*extern void ss_volume_envelope_recalculate(SS_Voice *v,
                                           SS_VolumeEnvelope *env,
                                           const int16_t *mod_gens,
                                           int target_key,
                                           bool is_in_release,
                                           double release_start_time,
                                           double start_time);*/
/*extern void ss_modulation_envelope_recalculate(SS_ModulationEnvelope *env,
                                               const int16_t *mod_gens,
                                               int midi_note,
                                               bool is_in_release,
                                               double release_start_time,
                                               double start_time);*/

/* ── Pool ────────────────────────────────────────────────────────────────── */

/*
 * Acquires a blank voice: pops a retired structure off the processor's pool
 * (preserving its modulator buffer) or, failing that, allocates a fresh one.
 * The returned voice is fully zeroed except for the recycled modulator array.
 */
static SS_Voice *ss_voice_pool_acquire(SS_Processor *proc) {
	if(proc && proc->voice_pool.free_count > 0) {
		SS_VoicePool *pool = &proc->voice_pool;
		SS_Voice *v = pool->free_list[--pool->free_count];
		/* Recycle: keep the modulator buffer, wipe everything else. */
		SS_Modulator *mods = v->modulators;
		size_t cap = v->modulator_capacity;
		memset(v, 0, sizeof(*v));
		v->modulators = mods;
		v->modulator_capacity = cap;
		return v;
	}
	return (SS_Voice *)calloc(1, sizeof(SS_Voice));
}

void ss_voice_pool_release(SS_Processor *proc, SS_Voice *v) {
	if(!v) return;
	if(!proc) {
		ss_voice_free(v);
		return;
	}
	SS_VoicePool *pool = &proc->voice_pool;
	if(pool->free_count >= pool->free_capacity) {
		size_t nc = pool->free_capacity ? pool->free_capacity * 2 : 32;
		SS_Voice **tmp = (SS_Voice **)realloc(pool->free_list,
		                                      nc * sizeof(SS_Voice *));
		if(!tmp) {
			/* Out of memory growing the pool: free the voice outright. */
			ss_voice_free(v);
			return;
		}
		pool->free_list = tmp;
		pool->free_capacity = nc;
	}
	pool->free_list[pool->free_count++] = v;
}

void ss_voice_pool_free(SS_VoicePool *pool) {
	if(!pool) return;
	for(size_t i = 0; i < pool->free_count; i++)
		ss_voice_free(pool->free_list[i]);
	free(pool->free_list);
	pool->free_list = NULL;
	pool->free_count = 0;
	pool->free_capacity = 0;
}

/* ── Create ──────────────────────────────────────────────────────────────── */

SS_Voice *ss_voice_create(SS_Processor *proc,
                          uint32_t sample_rate,
                          const SS_BasicPreset *preset,
                          const SS_AudioSample *audio_sample,
                          int midi_note, int velocity,
                          double current_time, int target_key, int sound_bank_key,
                          const int16_t *generators,
                          const SS_Modulator *modulators, size_t mod_count,
                          const SS_DynamicModulatorSystem *dms) {
	SS_Voice *v = ss_voice_pool_acquire(proc);
	if(!v) return NULL;

	v->preset = preset;
	v->sample = *audio_sample;
	v->midi_note = midi_note;
	v->velocity = velocity;
	v->start_time = current_time;
	v->is_active = true;
	v->target_key = target_key;
	v->sound_bank_key = sound_bank_key;
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

	/* Copy modulators.  A recycled voice keeps its modulator buffer between
	 * uses; only grow it when this note needs more slots than before. */
	if(mod_count > 0 || dms->is_active) {
		size_t max_mod_count = mod_count + (dms->is_active ? dms->modulator_count : 0);
		size_t adjusted_mod_count = mod_count;
		if(v->modulator_capacity < max_mod_count) {
			SS_Modulator *tmp = (SS_Modulator *)realloc(v->modulators,
			                                            max_mod_count * sizeof(SS_Modulator));
			if(tmp) {
				v->modulators = tmp;
				v->modulator_capacity = max_mod_count;
			}
		}
		if(v->modulators && v->modulator_capacity >= max_mod_count) {
			for(size_t i = 0; i < mod_count; i++)
				v->modulators[i] = ss_modulator_copy(&modulators[i]);
			if(dms->is_active) {
				for(size_t i = 0, count = dms->modulator_count; i < count; i++) {
					signed long match = -1;
					const SS_Modulator mod = ss_modulator_copy(&dms->modulators[i].modulator);
					for(size_t ii = 0; ii < adjusted_mod_count; ii++) {
						if(ss_modulator_is_identical(&v->modulators[ii], &mod)) {
							match = (signed long)ii;
							break;
						}
					}
					if(match >= 0) {
						v->modulators[match] = mod;
					} else {
						v->modulators[adjusted_mod_count++] = mod;
					}
				}
			}
			v->modulator_count = adjusted_mod_count;
		}
	}

	ss_lowpass_filter_init(&v->filter, sample_rate);
	ss_volume_envelope_init(&v->volume_env, sample_rate, SS_MAX_SOUND_CHUNK);

	/* Store current_pan in generator units (-500..500) to match TS smoothing behaviour. */
	/* Init to 0 and init properly at the first modulated offset after computing them. */
	v->current_pan = 0;

	return v;
}

/* ── Copy ────────────────────────────────────────────────────────────────── */

#if 0
SS_Voice *ss_voice_copy(const SS_Voice *src, double current_time, int sound_bank_key) {
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
	v->sound_bank_key = sound_bank_key;
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
