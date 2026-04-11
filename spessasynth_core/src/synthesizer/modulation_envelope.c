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
                                       double current_time);
void ss_modulation_envelope_start_release(SS_ModulationEnvelope *env,
										  const int16_t *mod_gens,
										  int midi_note,
										  double release_start_time,
										  double start_time);

static float tc2Sec(float timecents) {
	/* At such low values, buffer size of 128 may cause clicks in the lowpass filter
	 * -10114 is the lowest for it to fit at least twice in 128 samples@44.1kHz
	 * Testcase: MS_Basic-v0.2.1.sf2 Bass & Lead
	 */
	if (timecents <= -10114) return 0;
	return ss_timecents_to_seconds(timecents);
}

void ss_modulation_envelope_recalculate(SS_ModulationEnvelope *env,
                                        const int16_t *mod_gens,
                                        int midi_note,
										bool is_in_release,
                                        double release_start_time,
                                        double start_time) {
	env->entered_release = false;

	env->sustain_level = 1.0f - (float)mod_gens[SS_GEN_SUSTAIN_MOD_ENV] / 1000.0f;
	if(env->sustain_level < 0.0f) env->sustain_level = 0.0f;
	if(env->sustain_level > 1.0f) env->sustain_level = 1.0f;

	env->attack_duration = tc2Sec(mod_gens[SS_GEN_ATTACK_MOD_ENV]);

	int decay_key_exc = (60 - midi_note) * mod_gens[SS_GEN_KEYNUM_TO_MOD_ENV_DECAY];
	double decay_time = tc2Sec(mod_gens[SS_GEN_DECAY_MOD_ENV] + decay_key_exc);
	env->decay_duration = decay_time * (1.0 - env->sustain_level);

	int hold_key_exc = (60 - midi_note) * mod_gens[SS_GEN_KEYNUM_TO_MOD_ENV_HOLD];
	env->hold_duration = tc2Sec(hold_key_exc + mod_gens[SS_GEN_HOLD_MOD_ENV]);

	int rel_tc = mod_gens[SS_GEN_RELEASE_MOD_ENV];
	if(rel_tc < -7200) rel_tc = -7200;
	double rel_time = tc2Sec(rel_tc);
	env->release_duration = rel_time * env->release_start_level;

	env->delay_end = start_time + tc2Sec(mod_gens[SS_GEN_DELAY_MOD_ENV]);
	env->attack_end = env->delay_end + env->attack_duration;
	env->hold_end = env->attack_end + env->hold_duration;
	env->decay_end = env->hold_end + env->decay_duration;

	if (is_in_release) {
		ss_modulation_envelope_start_release(env, mod_gens, midi_note, release_start_time, start_time);
	}
}

void ss_modulation_envelope_start_release(SS_ModulationEnvelope *env,
                                          const int16_t *mod_gens,
                                          int midi_note,
                                          double release_start_time,
                                          double start_time) {
	env->release_start_level = env->current_value;
	env->entered_release = true;

	/* Min is set to -7200 to prevent lowpass clicks */
	float releasecents = mod_gens[SS_GEN_RELEASE_MOD_ENV];
	if (releasecents < -7200) releasecents = -7200;
	const float release_time = tc2Sec(releasecents);

	/* Release time is from the full level to 0%
	 * To get the actual time, multiply by the release start level
	 */
	env->release_duration = release_time * env->release_start_level;
}

float ss_modulation_envelope_get_value(const SS_ModulationEnvelope *env,
                                       double current_time) {
	SS_ModulationEnvelope *e = (SS_ModulationEnvelope *)env; /* cast away const for currentValue */

	if(e->entered_release) {
		/* If the voice is still in the delay phase,
		 * Start level will be 0 that will result in divide by zero
		 */
		if (e->release_start_level == 0) {
			return 0;
		}
		float value = (1.0 - (current_time - e->release_start_time) / e->release_duration) * e->release_start_level;
		if (value < 0.0) value = 0.0;
		return value;
	}

	if (current_time < e->delay_end) {
		e->current_value = 0;
	} else if (current_time < e->attack_end) {
		/* Modulation envelope uses convex curve for attack */
		int idx = (int)((1.0 - (e->attack_end - current_time) / e->attack_duration) * 1000.0);
		if (idx < 0) idx = 0;
		else if (idx > 999) idx = 999;
		e->current_value = ss_convex_attack(idx);
	} else if (current_time < e->hold_end) {
		/* Hold: stay at 1 */
		e->current_value = 1;
	} else if (current_time < e->decay_end) {
		/* Decay: linear ramp from 1 to sustain level */
		e->current_value = (1.0 - (e->decay_end - current_time) / e->decay_duration) * (e->sustain_level - 1.0) + 1.0;
	} else {
		/* Sustain: stay at sustain level */
		e->current_value = e->sustain_level;
	}
	return e->current_value;
}
