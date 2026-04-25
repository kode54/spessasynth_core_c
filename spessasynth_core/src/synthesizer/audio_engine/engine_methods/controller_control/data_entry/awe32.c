/**
 * awe32.c
 * Per-MIDI-channel NRPN handler for AWE32 messages.
 * Port of awe32.ts.
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

extern void ss_channel_set_generator_override(SS_MIDIChannel *ch, SS_GeneratorType gen, int val, bool realtime, double time);

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
void ss_channel_nrpn_awe32(SS_MIDIChannel *ch, int awe_gen, int data_lsb, int data_msb, double time) {
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
