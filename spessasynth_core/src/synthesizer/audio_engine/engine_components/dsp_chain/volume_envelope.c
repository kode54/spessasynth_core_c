/**
 * volume_envelope.c
 * SoundFont2 volume envelope.  Direct port of volume_envelope.ts.
 *
 * States:
 *   0 delay  1 attack  2 hold  3 decay  4 sustain
 * Release is a separate path triggered by voice.is_in_release.
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>
#if __has_include(<spessasynth_core/spessasynth.h>)
#include <spessasynth_core/synth.h>
#else
#include "spessasynth/synthesizer/synth.h"
#endif

/* Forward declarations of unit converter functions */
extern float ss_timecents_to_seconds(int tc);
extern float ss_centibel_attenuation_to_gain(float db);

#define CB_SILENCE 960.0f
#define PERCEIVED_CB_SILENCE 900.0f

#define DB_SILENCE 100.0f
#define PERCEIVED_DB_SILENCE 90.0f
#define PERCEIVED_GAIN_SILENCE 0.000015f
#define VOLUME_ENVELOPE_SMOOTHING_FACTOR 0.01f

/* Gain smoothing for rapid volume changes. Must be run EVERY SAMPLE */

#define GAIN_SMOOTHING_FACTOR 0.01f

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static inline uint64_t timecents_to_samples(int tc, uint32_t sample_rate) {
	float secs = ss_timecents_to_seconds(tc);
	if(secs < 0.0f) secs = 0.0f;
	return (uint64_t)(secs * (float)sample_rate);
}

/* ── ss_volume_envelope_init ─────────────────────────────────────────────── */

void ss_volume_envelope_init(SS_VolumeEnvelope *env,
                             uint32_t sample_rate, int initial_decay_cb) {
	/* Constructor parameters */
	memset(env, 0, sizeof(*env));
	env->sample_rate = sample_rate;
}

/* ── ss_volume_envelope_recalculate ──────────────────────────────────────── */

void ss_volume_envelope_recalculate(SS_Voice *v,
                                    SS_VolumeEnvelope *env,
                                    const int16_t *mod_gens,
                                    int target_key,
                                    bool is_in_release,
                                    double release_start_time,
                                    double start_time) {
	(void)start_time;
	(void)release_start_time;

	env->entered_release = false;

	env->can_end_on_silent_sustain = mod_gens[SS_GEN_SUSTAIN_VOL_ENV] >= PERCEIVED_CB_SILENCE;

	/* Start at silence */
	env->output_gain = 0;

	/* Sustain */
	int sustain_raw = mod_gens[SS_GEN_SUSTAIN_VOL_ENV];
	if(sustain_raw > CB_SILENCE) sustain_raw = CB_SILENCE;
	env->sustain_cb = (double)sustain_raw;

	/* Attack duration */
	env->attack_duration = timecents_to_samples(mod_gens[SS_GEN_ATTACK_VOL_ENV], env->sample_rate);

	/* Decay: sf spec page 35: the time is for change from attenuation to -100dB,
	 * Therefore, we need to calculate the real time
	 * (changing from attenuation to sustain instead of -100dB)
	 */
	int key_num_addition = (60 - v->target_key) *
	                       mod_gens[SS_GEN_KEYNUM_TO_VOL_ENV_DECAY];
	const double fraction = env->sustain_cb / CB_SILENCE;
	env->decay_duration = timecents_to_samples(mod_gens[SS_GEN_DECAY_VOL_ENV] + key_num_addition, env->sample_rate) * fraction;

	/* Calculate absolute end times for the values */
	env->delay_end = timecents_to_samples(mod_gens[SS_GEN_DELAY_VOL_ENV], env->sample_rate);
	env->attack_end = env->attack_duration + env->delay_end;

	/* Make sure to take keyNumToVolEnvHold into account! */
	const int hold_excursion = (60 - v->target_key) * mod_gens[SS_GEN_KEYNUM_TO_VOL_ENV_HOLD];
	env->hold_end = timecents_to_samples(mod_gens[SS_GEN_HOLD_VOL_ENV] + hold_excursion, env->sample_rate) + env->attack_end;

	env->decay_end = env->decay_duration + env->hold_end;

	/* If the voice has no attack or delay time, set current db to peak */
	if(env->attack_end == 0) {
		/* This.attenuationCb = this.attenuationTarget; */
		env->state = SS_VOLENV_HOLD;
	}
}

/* ── ss_volume_envelope_start_release ───────────────────────────────────── */

void ss_volume_envelope_start_release(SS_Voice *v,
                                      SS_VolumeEnvelope *env,
                                      const int16_t *mod_gens,
                                      int target_key,
                                      double release_start_time,
                                      double start_time) {
	env->release_start_time_samples = env->sample_time;

	float timecents = v->override_release_vol_env ? v->override_release_vol_env : mod_gens[SS_GEN_RELEASE_VOL_ENV];
	if(timecents < -7200) timecents = -7200;

	env->release_duration = timecents_to_samples(timecents, env->sample_rate);

	if(env->entered_release) {
		/* The envelope is already in release, but we request an update
		 * This can happen with exclusiveClass for example
		 * Don't compute the releaseStartCb as it's tracked in attenuationCb
		 */
		env->release_start_cb = env->attenuation_cb;
	} else {
		/* The envelope now enters the release phase from the current gain
		 * Compute the current gain level in decibel attenuation
		 */

		double sustain_cb = env->sustain_cb;
		if(sustain_cb < 0.0)
			sustain_cb = 0;
		else if(sustain_cb > CB_SILENCE)
			sustain_cb = CB_SILENCE;
		const double fraction = sustain_cb / CB_SILENCE;

		/* Decay: sf spec page 35: the time is for change from attenuation to -100dB,
		 * Therefore, we need to calculate the real time
		 * (changing from attenuation to sustain instead of -100dB)
		 */
		const int key_num_addition = (60 - v->target_key) * mod_gens[SS_GEN_KEYNUM_TO_VOL_ENV_DECAY];

		env->decay_duration = timecents_to_samples(mod_gens[SS_GEN_DECAY_VOL_ENV] + key_num_addition, env->sample_rate) * fraction;

		double release_start_cb;
		switch(env->state) {
			case SS_VOLENV_DELAY:
				/* Delay phase: no sound is produced */
				release_start_cb = CB_SILENCE;
				break;

			case SS_VOLENV_ATTACK: {
				/* Attack phase: get linear gain of the attack phase when release started
				 * And turn it into db as we're ramping the db up linearly
				 * (to make volume go down exponentially)
				 * Attack is linear (in gain) so we need to do get db from that
				 */
				const double elapsed =
				1.0 -
				(double)(env->attack_end - env->release_start_time_samples) /
				(double)env->attack_duration;
				/* Calculate the gain that the attack would have, so
				 * Turn that into cB
				 */
				release_start_cb = 200.0 * log10(elapsed) * -1.0;
				break;
			}

			case SS_VOLENV_HOLD:
				release_start_cb = 0;
				break;

			case SS_VOLENV_DECAY:
				release_start_cb = (1.0 - ((double)(env->decay_end - env->release_start_time_samples) / (double)env->decay_duration)) * sustain_cb;
				break;

			case SS_VOLENV_SUSTAIN:
				release_start_cb = sustain_cb;
				break;
		}
		if(release_start_cb < 0.0)
			release_start_cb = 0;
		else if(release_start_cb > CB_SILENCE)
			release_start_cb = CB_SILENCE;
		env->release_start_cb = release_start_cb;
		env->attenuation_cb = release_start_cb;
	}

	env->entered_release = true;

	/* Release: sf spec page 35: the time is for change from attenuation to -100dB,
	 * Therefore, we need to calculate the real time
	 * (changing from release start to -100dB instead of from peak to -100dB)
	 */
	const double release_fraction = (CB_SILENCE - env->release_start_cb) / CB_SILENCE;
	env->release_duration *= release_fraction;
	/* Voice may be off instantly
	 * Testcase: mono mode
	 */
	if(env->release_start_cb >= PERCEIVED_CB_SILENCE) {
		v->is_active = false;
	}
}

/* ── ss_volume_envelope_process ─────────────────────────────────────────── */

bool ss_volume_envelope_process(SS_VolumeEnvelope *env,
                                int sample_count, float gain_target) {
	const uint64_t release_start_time_samples = env->release_start_time_samples;
	const double release_start_cb = env->release_start_cb;
	const uint64_t release_duration = env->release_duration;
	const uint64_t delay_end = env->delay_end;
	const uint64_t attack_end = env->attack_end;
	const uint64_t attack_duration = env->attack_duration;
	const uint64_t hold_end = env->hold_end;
	const uint64_t decay_end = env->decay_end;
	const uint64_t decay_duration = env->decay_duration;
	const double sustain_cb = env->sustain_cb;

	// Advance time by the entire block to calculate the last sample's gain
	const uint64_t sample_time = (env->sample_time += sample_count);

	if(env->entered_release) {
		/* How much time has passed since release was started? */
		const uint64_t elapsed_release = sample_time - release_start_time_samples;
		const double cb_difference = CB_SILENCE - release_start_cb;

		/* Linearly ramp down decibels */
		env->attenuation_cb = ((double)elapsed_release / (double)release_duration) * cb_difference + release_start_cb;
		env->output_gain = ss_centibel_attenuation_to_gain(env->attenuation_cb) * gain_target;
		return env->attenuation_cb < PERCEIVED_CB_SILENCE;
	}

	for(;;) {
		switch(env->state) {
			case SS_VOLENV_DELAY: {
				/* Delay phase: no sound is produced */
				if(sample_time < delay_end) {
					/* Silence */
					env->attenuation_cb = CB_SILENCE;
					env->output_gain = 0;
					return true;
				}
				env->state++;
				break;
			}

			case SS_VOLENV_ATTACK: {
				/* Attack phase: ramp from 0 to attenuation */
				if(sample_time < attack_end) {
					/* Set current attenuation to peak as its invalid during this phase */
					env->attenuation_cb = 0;
					/* Special case: linear gain ramp instead of linear db ramp */
					const float linear_gain = 1.0 - (double)(attack_end - sample_time) / (double)attack_duration;
					env->output_gain = linear_gain * gain_target;
					return true;
				}
				env->state++;
				break;
			}

			case SS_VOLENV_HOLD: {
				/* Hold/peak phase: stay at max volume */
				if(sample_time < hold_end) {
					/* Peak, no attenuation */
					env->attenuation_cb = 0;
					env->output_gain = gain_target;
					return true;
				}
				env->state++;
				break;
			}
			case SS_VOLENV_DECAY: {
				if(sample_time < decay_end) {
					/* Linear centibel ramp down to sustain */
					env->attenuation_cb = (1.0 - (double)(decay_end - sample_time) / (double)decay_duration) * sustain_cb;
					env->output_gain = gain_target * ss_centibel_attenuation_to_gain(env->attenuation_cb);
					return true;
				}
				env->state++;
				break;
			}

			case SS_VOLENV_SUSTAIN: {
				if(env->can_end_on_silent_sustain && sustain_cb >= PERCEIVED_CB_SILENCE) {
					/* Make sure to fill with silence */
					/* https://github.com/spessasus/spessasynth_core/issues/57 */
					env->attenuation_cb = 0;
					env->output_gain = 0;
					return false;
				}

				/* Sustain phase: stay at sustain */
				env->attenuation_cb = sustain_cb;
				env->output_gain = gain_target * ss_centibel_attenuation_to_gain(sustain_cb);
				return true;
			}
		}
	}
}
