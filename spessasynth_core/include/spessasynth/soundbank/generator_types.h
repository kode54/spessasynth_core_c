#ifndef SS_GENERATOR_TYPES_H
#define SS_GENERATOR_TYPES_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
	SS_GEN_INVALID = -1,
	SS_GEN_START_ADDRS_OFFSET = 0,
	SS_GEN_END_ADDR_OFFSET = 1,
	SS_GEN_STARTLOOP_ADDRS_OFFSET = 2,
	SS_GEN_ENDLOOP_ADDRS_OFFSET = 3,
	SS_GEN_START_ADDRS_COARSE_OFFSET = 4,
	SS_GEN_MOD_LFO_TO_PITCH = 5,
	SS_GEN_VIB_LFO_TO_PITCH = 6,
	SS_GEN_MOD_ENV_TO_PITCH = 7,
	SS_GEN_INITIAL_FILTER_FC = 8,
	SS_GEN_INITIAL_FILTER_Q = 9,
	SS_GEN_MOD_LFO_TO_FILTER_FC = 10,
	SS_GEN_MOD_ENV_TO_FILTER_FC = 11,
	SS_GEN_END_ADDRS_COARSE_OFFSET = 12,
	SS_GEN_MOD_LFO_TO_VOLUME = 13,
	SS_GEN_UNUSED1 = 14,
	SS_GEN_CHORUS_EFFECTS_SEND = 15,
	SS_GEN_REVERB_EFFECTS_SEND = 16,
	SS_GEN_PAN = 17,
	SS_GEN_UNUSED2 = 18,
	SS_GEN_UNUSED3 = 19,
	SS_GEN_UNUSED4 = 20,
	SS_GEN_DELAY_MOD_LFO = 21,
	SS_GEN_FREQ_MOD_LFO = 22,
	SS_GEN_DELAY_VIB_LFO = 23,
	SS_GEN_FREQ_VIB_LFO = 24,
	SS_GEN_DELAY_MOD_ENV = 25,
	SS_GEN_ATTACK_MOD_ENV = 26,
	SS_GEN_HOLD_MOD_ENV = 27,
	SS_GEN_DECAY_MOD_ENV = 28,
	SS_GEN_SUSTAIN_MOD_ENV = 29,
	SS_GEN_RELEASE_MOD_ENV = 30,
	SS_GEN_KEYNUM_TO_MOD_ENV_HOLD = 31,
	SS_GEN_KEYNUM_TO_MOD_ENV_DECAY = 32,
	SS_GEN_DELAY_VOL_ENV = 33,
	SS_GEN_ATTACK_VOL_ENV = 34,
	SS_GEN_HOLD_VOL_ENV = 35,
	SS_GEN_DECAY_VOL_ENV = 36,
	SS_GEN_SUSTAIN_VOL_ENV = 37,
	SS_GEN_RELEASE_VOL_ENV = 38,
	SS_GEN_KEYNUM_TO_VOL_ENV_HOLD = 39,
	SS_GEN_KEYNUM_TO_VOL_ENV_DECAY = 40,
	SS_GEN_INSTRUMENT = 41,
	SS_GEN_RESERVED1 = 42,
	SS_GEN_KEY_RANGE = 43,
	SS_GEN_VEL_RANGE = 44,
	SS_GEN_STARTLOOP_ADDRS_COARSE_OFFSET = 45,
	SS_GEN_KEYNUM = 46,
	SS_GEN_VELOCITY = 47,
	SS_GEN_INITIAL_ATTENUATION = 48,
	SS_GEN_RESERVED2 = 49,
	SS_GEN_ENDLOOP_ADDRS_COARSE_OFFSET = 50,
	SS_GEN_COARSE_TUNE = 51,
	SS_GEN_FINE_TUNE = 52,
	SS_GEN_SAMPLE_ID = 53,
	SS_GEN_SAMPLE_MODES = 54,
	SS_GEN_RESERVED3 = 55,
	SS_GEN_SCALE_TUNING = 56,
	SS_GEN_EXCLUSIVE_CLASS = 57,
	SS_GEN_OVERRIDING_ROOT_KEY = 58,
	SS_GEN_UNUSED5 = 59,
	SS_GEN_END_OPER = 60,
	/* Non-standard: used in sysex/dynamic modulators only, not saved to file */
	SS_GEN_AMPLITUDE = 61, /* [-1000;1000] -> 1/10% gain offset */
	SS_GEN_VIB_LFO_RATE = 62, /* [-1000;1000] -> Hz/100 rate offset */
	SS_GEN_VIB_LFO_AMPLITUDE_DEPTH = 63, /* [0;1000] -> 1/10% amplitude depth */
	SS_GEN_VIB_LFO_TO_FILTER_FC = 64, /* like modLfoToFilterFc */
	SS_GEN_MOD_LFO_RATE = 65, /* [-1000;1000] -> Hz/100 rate offset */
	SS_GEN_MOD_LFO_AMPLITUDE_DEPTH = 66, /* [0;1000] -> 1/10% amplitude depth */

	SS_GEN_COUNT = 67
} SS_GeneratorType;

/** Default generator values (SF2 spec defaults). */
extern const int16_t SS_GENERATOR_DEFAULTS[SS_GEN_COUNT];

/** Min/max clamp values per generator. */
typedef struct {
	int16_t min;
	int16_t max;
	int16_t def;
	int16_t nrpn;
} SS_GeneratorLimit;
extern const SS_GeneratorLimit SS_GENERATOR_LIMITS[SS_GEN_COUNT];

/**
 * Clamp a raw generator value to the spec limits for that generator type.
 * Returns def if type is out of range or SS_GEN_INVALID.
 */
int16_t ss_generator_clamp(SS_GeneratorType type, int16_t value);

/**
 * Sum a preset generator and an instrument generator, applying spec clamping.
 * Matches addAndClampGenerator() in generator.ts.
 */
int16_t ss_generator_add_and_clamp(SS_GeneratorType type,
                                   int16_t preset_value,
                                   int16_t instrument_value);

#ifdef __cplusplus
}
#endif

#endif /* SS_GENERATOR_TYPES_H */
