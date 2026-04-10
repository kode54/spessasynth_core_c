/**
 * modulation_envelope.c
 * SoundFont2 modulation envelope.  Direct port of modulation_envelope.ts.
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>
#if __has_include(<spessasynth_core/spessasynth.h>)
#include <spessasynth_core/synth.h>
#else
#include "spessasynth/synthesizer/synth.h"
#endif

extern float ss_timecents_to_seconds(int tc);
extern float ss_convex_attack(int index_0_to_999);

float ss_modulation_envelope_get_value(const SS_ModulationEnvelope *env,
                                       double current_time,
                                       bool ignore_release);

void ss_modulation_envelope_recalculate(SS_ModulationEnvelope *env,
                                        const int16_t *mod_gens,
                                        int midi_note,
                                        bool is_in_release,
                                        double release_start_time,
                                        double start_time) {
	env->sustain_level = 1.0f - (float)mod_gens[SS_GEN_SUSTAIN_MOD_ENV] / 1000.0f;
	if(env->sustain_level < 0.0f) env->sustain_level = 0.0f;
	if(env->sustain_level > 1.0f) env->sustain_level = 1.0f;

	env->attack_duration = ss_timecents_to_seconds(mod_gens[SS_GEN_ATTACK_MOD_ENV]);

	int decay_key_exc = (60 - midi_note) * mod_gens[SS_GEN_KEYNUM_TO_MOD_ENV_DECAY];
	double decay_time = ss_timecents_to_seconds(mod_gens[SS_GEN_DECAY_MOD_ENV] + decay_key_exc);
	env->decay_duration = decay_time * (1.0 - env->sustain_level);

	int hold_key_exc = (60 - midi_note) * mod_gens[SS_GEN_KEYNUM_TO_MOD_ENV_HOLD];
	env->hold_duration = ss_timecents_to_seconds(
	hold_key_exc + mod_gens[SS_GEN_HOLD_MOD_ENV]);

	int rel_tc = mod_gens[SS_GEN_RELEASE_MOD_ENV];
	if(rel_tc < -7200) rel_tc = -7200;
	double rel_time = ss_timecents_to_seconds(rel_tc);
	env->release_duration = rel_time * env->release_start_level;

	env->delay_end = start_time + ss_timecents_to_seconds(mod_gens[SS_GEN_DELAY_MOD_ENV]);
	env->attack_end = env->delay_end + env->attack_duration;
	env->hold_end = env->attack_end + env->hold_duration;
	env->decay_end = env->hold_end + env->decay_duration;

	if(is_in_release) {
		env->release_start_time = release_start_time;
		env->release_start_level = ss_modulation_envelope_get_value(
		env, release_start_time, true);
		env->release_duration = rel_time * env->release_start_level;
	}
}

void ss_modulation_envelope_start_release(SS_ModulationEnvelope *env,
                                          const int16_t *mod_gens,
                                          int midi_note,
                                          double release_start_time,
                                          double start_time) {
	ss_modulation_envelope_recalculate(env, mod_gens, midi_note,
	                                   true, release_start_time, start_time);
}

float ss_modulation_envelope_get_value(const SS_ModulationEnvelope *env,
                                       double current_time,
                                       bool ignore_release) {
	SS_ModulationEnvelope *e = (SS_ModulationEnvelope *)env; /* cast away const for currentValue */

	if(!ignore_release && env->release_start_time > 0) {
		if(env->release_start_level == 0.0f) return 0.0f;
		double elapsed = current_time - env->release_start_time;
		if(elapsed < 0.0) elapsed = 0.0;
		float val = (float)(1.0 - (elapsed / env->release_duration)) * env->release_start_level;
		if(val < 0.0f) val = 0.0f;
		return val;
	}

	if(current_time < env->delay_end) {
		e->current_value = 0.0f;
	} else if(env->attack_duration > 0.0 && current_time < env->attack_end) {
		float progress = (float)(1.0 - (env->attack_end - current_time) / env->attack_duration);
		int idx = (int)(progress * 1000.0f);
		if(idx < 0) idx = 0;
		if(idx >= 999) idx = 999;
		e->current_value = ss_convex_attack(idx);
	} else if(current_time < env->hold_end) {
		e->current_value = 1.0f;
	} else if(env->decay_duration > 0.0 && current_time < env->decay_end) {
		float frac = (float)(1.0 - (env->decay_end - current_time) / env->decay_duration);
		e->current_value = frac * (env->sustain_level - 1.0f) + 1.0f;
	} else {
		e->current_value = env->sustain_level;
	}
	return e->current_value;
}
