#include <math.h>
#include <stdlib.h>
#include <string.h>
#if __has_include(<spessasynth_core/spessasynth.h>)
#include <spessasynth_core/indexed_byte_array.h>
#include <spessasynth_core/midi_enums.h>
#include <spessasynth_core/soundbank.h>
#else
#include "spessasynth/midi/midi_enums.h"
#include "spessasynth/soundbank/soundbank.h"
#include "spessasynth/utils/indexed_byte_array.h"
#endif

typedef enum {
	SS_SYSTEM_GM = 0,
	SS_SYSTEM_GS = 1,
	SS_SYSTEM_XG = 2
} SS_MIDISystem;

/* ── Generator limits table ──────────────────────────────────────────────── */

const SS_GeneratorLimit SS_GENERATOR_LIMITS[SS_GEN_COUNT] = {
	/* 0  startAddrsOffset           */ { 0, 32767, 0, 1 },
	/* 1  endAddrOffset              */ { -32768, 32767, 0, 1 },
	/* 2  startloopAddrsOffset       */ { -32768, 32767, 0, 1 },
	/* 3  endloopAddrsOffset         */ { -32768, 32767, 0, 1 },
	/* 4  startAddrsCoarseOffset     */ { 0, 32767, 0, 1 },
	/* 5  modLfoToPitch              */ { -12000, 12000, 0, 2 },
	/* 6  vibLfoToPitch              */ { -12000, 12000, 0, 2 },
	/* 7  modEnvToPitch              */ { -12000, 12000, 0, 2 },
	/* 8  initialFilterFc            */ { 1500, 13500, 13500, 2 },
	/* 9  initialFilterQ             */ { 0, 960, 0, 1 },
	/* 10 modLfoToFilterFc           */ { -12000, 12000, 0, 2 },
	/* 11 modEnvToFilterFc           */ { -12000, 12000, 0, 2 },
	/* 12 endAddrsCoarseOffset       */ { -32768, 32767, 0, 1 },
	/* 13 modLfoToVolume             */ { -960, 960, 0, 1 },
	/* 14 unused1                    */ { 0, 0, 0, 0 },
	/* 15 chorusEffectsSend          */ { 0, 1000, 0, 1 },
	/* 16 reverbEffectsSend          */ { 0, 1000, 0, 1 },
	/* 17 pan                        */ { -500, 500, 0, 1 },
	/* 18 unused2                    */ { 0, 0, 0, 0 },
	/* 19 unused3                    */ { 0, 0, 0, 0 },
	/* 20 unused4                    */ { 0, 0, 0, 0 },
	/* 21 delayModLFO                */ { -12000, 5000, -12000, 2 },
	/* 22 freqModLFO                 */ { -16000, 4500, 0, 4 },
	/* 23 delayVibLFO                */ { -12000, 5000, -12000, 2 },
	/* 24 freqVibLFO                 */ { -16000, 4500, 0, 4 },
	/* 25 delayModEnv                */ { -32768, 5000, -32768, 2 },
	/* 26 attackModEnv               */ { -32768, 8000, -32768, 2 },
	/* 27 holdModEnv                 */ { -12000, 5000, -12000, 2 },
	/* 28 decayModEnv                */ { -12000, 8000, -12000, 2 },
	/* 29 sustainModEnv              */ { 0, 1000, 0, 1 },
	/* 30 releaseModEnv              */ { -12000, 8000, -12000, 2 },
	/* 31 keyNumToModEnvHold         */ { -1200, 1200, 0, 1 },
	/* 32 keyNumToModEnvDecay        */ { -1200, 1200, 0, 1 },
	/* 33 delayVolEnv                */ { -12000, 5000, -12000, 2 },
	/* 34 attackVolEnv               */ { -12000, 8000, -12000, 2 },
	/* 35 holdVolEnv                 */ { -12000, 5000, -12000, 2 },
	/* 36 decayVolEnv                */ { -12000, 8000, -12000, 2 },
	/* 37 sustainVolEnv              */ { 0, 1440, 0, 1 },
	/* 38 releaseVolEnv              */ { -12000, 8000, -12000, 2 },
	/* 39 keyNumToVolEnvHold         */ { -1200, 1200, 0, 1 },
	/* 40 keyNumToVolEnvDecay        */ { -1200, 1200, 0, 1 },
	/* 41 instrument                 */ { 0, 32767, 0, 0 },
	/* 42 reserved1                  */ { 0, 0, 0, 0 },
	/* 43 keyRange                   */ { 0, 127, 0, 0 },
	/* 44 velRange                   */ { 0, 127, 0, 0 },
	/* 45 startloopAddrsCoarseOffset */ { -32768, 32767, 0, 1 },
	/* 46 keyNum                     */ { -1, 127, -1, 1 },
	/* 47 velocity                   */ { -1, 127, -1, 1 },
	/* 48 initialAttenuation         */ { 0, 1440, 0, 1 },
	/* 49 reserved2                  */ { 0, 0, 0, 0 },
	/* 50 endloopAddrsCoarseOffset   */ { -32768, 32767, 0, 1 },
	/* 51 coarseTune                 */ { -120, 120, 0, 1 },
	/* 52 fineTune                   */ { -12700, 12700, 0, 1 },
	/* 53 sampleID                   */ { 0, 32767, 0, 0 },
	/* 54 sampleModes                */ { 0, 3, 0, 0 },
	/* 55 reserved3                  */ { 0, 0, 0, 0 },
	/* 56 scaleTuning                */ { 0, 1200, 100, 1 },
	/* 57 exclusiveClass             */ { 0, 32767, 0, 0 },
	/* 58 overridingRootKey          */ { -1, 127, -1, 0 },
	/* 59 unused5                    */ { 0, 0, 0, 0 },
	/* 60 endOper                    */ { 0, 0, 0, 0 },
	/* 61 amplitude (non-std)              */ { -1000, 1000, 0, 1 },
	/* 62 vibLfoRate (non-std)             */ { -1000, 1000, 0, 1 },
	/* 63 vibLfoAmplitudeDepth (non-std)   */ { 0, 1000, 0, 1 },
	/* 64 vibLfoToFilterFc (non-std)       */ { -12000, 12000, 0, 2 },
	/* 65 modLfoRate (non-std)             */ { -1000, 1000, 0, 1 },
	/* 66 modLfoAmplitudeDepth (non-std)   */ { 0, 1000, 0, 1 },
};

int16_t ss_generator_clamp(SS_GeneratorType type, int16_t value) {
	if(type < 0 || type >= SS_GEN_COUNT) return 0;
	const SS_GeneratorLimit *lim = &SS_GENERATOR_LIMITS[type];
	if(value < lim->min) return lim->min;
	if(value > lim->max) return lim->max;
	return value;
}

int16_t ss_generator_add_and_clamp(SS_GeneratorType type,
                                   int16_t preset_value,
                                   int16_t instrument_value) {
	if(type < 0 || type >= SS_GEN_COUNT) return 0;
	const SS_GeneratorLimit *lim = &SS_GENERATOR_LIMITS[type];
	int32_t sum = (int32_t)preset_value + (int32_t)instrument_value;
	if(sum < lim->min) sum = lim->min;
	if(sum > lim->max) sum = lim->max;
	return (int16_t)sum;
}

/* ── Modulator ───────────────────────────────────────────────────────────── */

SS_Modulator ss_modulator_copy(const SS_Modulator *src) {
	SS_Modulator dst;
	memcpy(&dst, src, sizeof(SS_Modulator));
	dst.current_value = 0;
	return dst;
}

/* ── Default SF2 modulators (SF2 spec table 8.4.2) ──────────────────────── */

/*
 * Build source enum values:
 * Bits 15-10: curve type
 * Bit 9: is bipolar
 * Bit 8: is negative
 * Bit 7: is CC
 * Bits 6-0: source index
 *
 * Curve types: 0=linear, 1=concave, 2=convex, 3=switch
 * All values follow the SF2 spec exactly.
 */

#define MODSRC(curve, isbip, isneg, iscc, idx) (uint16_t)(((uint16_t)(curve) << 10) | (((isbip) ? 1 : 0) << 9) | (((isneg) ? 1 : 0) << 8) | (((iscc) ? 1 : 0) << 7) | (idx))

#define MODISEFFECT(s1, s2, dest) \
	((s1 == 0x00db || s2 == 0x00dd) && s2 == 0x0000 && (dest == SS_GEN_REVERB_EFFECTS_SEND || dest == SS_GEN_CHORUS_EFFECTS_SEND))

#define MODISDEFAULTRESONANT(s1, s2, dest) \
	(s1 == DEFAULT_RESONANT_MOD_SOURCE && s2 == 0x0 && dest == SS_GEN_INITIAL_FILTER_Q)

#define MODISMODWHEEL(s1, s2, dest) \
	(((((s1 & (1 << 7)) != 0) && ((s1 & 127) == SS_MIDCON_MODULATION_WHEEL)) || ((s1 & (1 << 7)) != 0) && ((s1 & 127) == SS_MIDCON_MODULATION_WHEEL)) && (dest == SS_GEN_MOD_LFO_TO_PITCH || dest == SS_GEN_VIB_LFO_TO_PITCH))

#define MODULATOR(s1, s2, dest, amount, transform) { s1, s2, dest, amount, transform, 0, MODISEFFECT(s1, s2, dest), MODISDEFAULTRESONANT(s1, s2, dest), MODISMODWHEEL(s1, s2, dest) }

#define DEFAULT_ATTENUATION_MOD_AMOUNT 960
#define DEFAULT_ATTENUATION_MOD_CURVE_TYPE SS_MODCURVE_CONCAVE

static const uint16_t DEFAULT_RESONANT_MOD_SOURCE = MODSRC(
SS_MODCURVE_LINEAR,
true,
false,
true,
SS_MIDCON_FILTER_RESONANCE); /* Linear forwards bipolar cc 74 */

const SS_Modulator SS_DEFAULT_MODULATORS[] = {
	/* 1. Velocity -> Attenuation */
	MODULATOR(
	MODSRC(
	DEFAULT_ATTENUATION_MOD_CURVE_TYPE,
	false,
	true,
	false,
	SS_MODSRC_NOTE_ON_VELOCITY),
	0x0,
	SS_GEN_INITIAL_ATTENUATION,
	DEFAULT_ATTENUATION_MOD_AMOUNT,
	0),

	/* 2. Mod Wheel -> Vibrato */
	MODULATOR(0x0081, 0x0, SS_GEN_VIB_LFO_TO_PITCH, 50, 0),

	/* 3. Volume -> Attenuation */
	MODULATOR(
	MODSRC(
	DEFAULT_ATTENUATION_MOD_CURVE_TYPE,
	false,
	true,
	true,
	SS_MIDCON_MAIN_VOLUME),
	0x0,
	SS_GEN_INITIAL_ATTENUATION,
	DEFAULT_ATTENUATION_MOD_AMOUNT,
	0),

	/* 4. Channel pressure -> Vibrato */
	MODULATOR(0x000d, 0x0, SS_GEN_VIB_LFO_TO_PITCH, 50, 0),

	/* 5. Pitch wheel -> Tuning */
	MODULATOR(0x020e, 0x0010, SS_GEN_FINE_TUNE, 12700, 0),

	/* 6. Pan -> Pan */
	/* Amount is 500, not 1000, see SpessaSynth#59 */
	MODULATOR(0x028a, 0x0, SS_GEN_PAN, 500, 0),

	/* 7. Expression -> Attenuation */
	MODULATOR(
	MODSRC(
	DEFAULT_ATTENUATION_MOD_CURVE_TYPE,
	false,
	true,
	true,
	SS_MIDCON_EXPRESSION),
	0x0,
	SS_GEN_INITIAL_ATTENUATION,
	DEFAULT_ATTENUATION_MOD_AMOUNT,
	0),

	/* 8. Reverb effects to send */
	MODULATOR(0x00db, 0x0, SS_GEN_REVERB_EFFECTS_SEND, 200, 0),

	/* 9. Chorus effects to send */
	MODULATOR(0x00dd, 0x0, SS_GEN_CHORUS_EFFECTS_SEND, 200, 0),

	/* Now all the SpessaSynth default modulators! */
	/* 1. Cc 73 (attack time) to volEnv attack */
	MODULATOR(
	MODSRC(
	SS_MODCURVE_CONVEX,
	true,
	false,
	true,
	SS_MIDCON_ATTACK_TIME), /* Linear forward bipolar cc 72 */
	0x0, /* No controller */
	SS_GEN_ATTACK_VOL_ENV,
	6000,
	0),

	/* 2. Cc 72 (release time) to volEnv release */
	MODULATOR(
	MODSRC(
	SS_MODCURVE_LINEAR,
	true,
	false,
	true,
	SS_MIDCON_RELEASE_TIME), /* Linear forward bipolar cc 72 */
	0x0, /* No controller */
	SS_GEN_RELEASE_VOL_ENV,
	3600,
	0),

	/* 3. Cc 75 (decay time) to vol env decay */
	MODULATOR(
	MODSRC(
	SS_MODCURVE_LINEAR,
	true,
	false,
	true,
	SS_MIDCON_DECAY_TIME), /* Linear forward bipolar cc 75 */
	0x0, /* No controller */
	SS_GEN_DECAY_VOL_ENV,
	3600,
	0),

	/* 4. Cc 74 (brightness) to filterFc */
	MODULATOR(
	MODSRC(
	SS_MODCURVE_LINEAR,
	true,
	false,
	true,
	SS_MIDCON_BRIGHTNESS), /* Linear forwards bipolar cc 74 */
	0x0, /* No controller */
	SS_GEN_INITIAL_FILTER_FC,
	9600,
	0),

	/* 5. Cc 71 (filter Q) to filter Q (default resonant modulator) */
	MODULATOR(
	DEFAULT_RESONANT_MOD_SOURCE,
	0x0, /* No controller */
	SS_GEN_INITIAL_FILTER_Q,
	200,
	0),

	/* 6. Cc 67 (soft pedal) to attenuation */
	MODULATOR(
	MODSRC(
	SS_MODCURVE_SWITCH,
	false,
	false,
	true,
	SS_MIDCON_SOFT_PEDAL), /* Switch unipolar positive 67 */
	0x0, /* No controller */
	SS_GEN_INITIAL_ATTENUATION,
	50,
	0),

	/* 7. Cc 67 (soft pedal) to filter fc */
	MODULATOR(
	MODSRC(
	SS_MODCURVE_SWITCH,
	false,
	false,
	true,
	SS_MIDCON_SOFT_PEDAL), /* Switch unipolar positive 67 */
	0x0, /* No controller */
	SS_GEN_INITIAL_FILTER_FC,
	-2400,
	0),

	/* 8. Cc 8 (balance) to pan */
	MODULATOR(
	MODSRC(
	SS_MODCURVE_LINEAR,
	true,
	false,
	true,
	SS_MIDCON_BALANCE), /* Linear bipolar positive 8 */
	0x0, /* No controller */
	SS_GEN_PAN,
	500,
	0)
};

const size_t SS_DEFAULT_MODULATOR_COUNT =
sizeof(SS_DEFAULT_MODULATORS) / sizeof(SS_DEFAULT_MODULATORS[0]);

/* ── Zone ────────────────────────────────────────────────────────────────── */

void ss_zone_free(SS_Zone *z) {
	free(z->generators);
	free(z->modulators);
	z->generators = NULL;
	z->modulators = NULL;
	z->gen_count = 0;
	z->mod_count = 0;
}

/* ── Sample ──────────────────────────────────────────────────────────────── */

/* Forward declarations for the format-specific loaders */
bool ss_vorbis_decode(SS_BasicSample *s);
bool ss_flac_decode(SS_BasicSample *s);

bool ss_sample_decode(SS_BasicSample *s) {
	if(s->audio_data) return true; /* already decoded */

	if(s->is_compressed && s->compressed_data) {
		/* Dispatch to vorbis/FLAC/WAV decoder based on magic bytes */
		if(s->is_sf2pack) {
			s->audio_data = (float *)malloc(s->compressed_data_length + 4 * sizeof(float));
			if(s->audio_data) {
				memcpy(s->audio_data, s->compressed_data, s->compressed_data_length);
				s->audio_data_length = s->compressed_data_length / sizeof(float);
				memset(s->audio_data + s->audio_data_length, 0, 4 * sizeof(float));
				return true;
			}
			return false;
		}
		if(s->compressed_data_length >= 4) {
			const uint8_t *hdr = s->compressed_data;
			if(hdr[0] == 'O' && hdr[1] == 'g' && hdr[2] == 'g' && hdr[3] == 'S') {
#ifdef SS_HAVE_STB_VORBIS
				return ss_vorbis_decode(s);
#endif
				return false;
			}
			if(hdr[0] == 'f' && hdr[1] == 'L' && hdr[2] == 'a' && hdr[3] == 'C') {
#ifdef SS_HAVE_LIBFLAC
				return ss_flac_decode(s);
#endif
				return false;
			}
		}
		return false;
	}

	if(s->s16le_data && s->s16le_length >= 2) {
		size_t sample_count = s->s16le_length / 2;
		s->audio_data = (float *)malloc((sample_count + 4) * sizeof(float)); /* Allocate a little more for interpolators */
		if(!s->audio_data) return false;
		memset(s->audio_data + sample_count, 0, 4 * sizeof(float));
		s->audio_data_length = sample_count;

		const int16_t *src = (const int16_t *)s->s16le_data;
		for(size_t i = 0; i < sample_count; i++) {
			s->audio_data[i] = (float)src[i] / 32768.0f;
		}
		return true;
	}

	if(s->u8_data && s->u8_length >= 1) {
		size_t sample_count = s->u8_length;
		s->audio_data = (float *)malloc((sample_count + 4) * sizeof(float)); /* Allocate a little more for interpolators */
		if(!s->audio_data) return false;
		memset(s->audio_data + sample_count, 0, 4 * sizeof(float));
		s->audio_data_length = sample_count;

		const uint8_t *src = s->u8_data;
		for(size_t i = 0; i < sample_count; i++) {
			s->audio_data[i] = ((float)src[i] - 128.0) / 128.0;
		}
		return true;
	}

	/* Zero-length sample: create minimal silent buffer */
	s->audio_data = (float *)calloc(1, sizeof(float));
	s->audio_data_length = 1;
	return true;
}

void ss_sample_free_data(SS_BasicSample *s) {
	/* audio_data is always owned by whichever struct decoded it. */
	free(s->audio_data);
	/* compressed_data / s16le_data / u8_data are owned only by bank samples. */
	if(s->owns_raw_data) {
		free(s->compressed_data);
		free(s->s16le_data);
		free(s->u8_data);
	}
	s->audio_data = NULL;
	s->compressed_data = NULL;
	s->s16le_data = NULL;
	s->u8_data = NULL;
	s->audio_data_length = 0;
}

/* ── Instrument ──────────────────────────────────────────────────────────── */

void ss_instrument_free(SS_BasicInstrument *inst) {
	ss_zone_free(&inst->global_zone);
	for(size_t i = 0; i < inst->zone_count; i++) {
		ss_zone_free(&inst->zones[i].base);
		if(inst->zones[i].sample) {
			ss_sample_free_data(inst->zones[i].sample);
			free(inst->zones[i].sample);
			inst->zones[i].sample = NULL;
		}
	}
	free(inst->zones);
	inst->zones = NULL;
	inst->zone_count = 0;
}

/* ── Preset ──────────────────────────────────────────────────────────────── */

void ss_preset_free(SS_BasicPreset *p) {
	ss_zone_free(&p->global_zone);
	for(size_t i = 0; i < p->zone_count; i++) {
		ss_zone_free(&p->zones[i].base);
	}
	free(p->zones);
	p->zones = NULL;
	p->zone_count = 0;
}

/* ── SynthesisData ───────────────────────────────────────────────────────── */

void ss_synthesis_data_free_array(SS_SynthesisData *data, size_t count) {
	if(!data) return;
	for(size_t i = 0; i < count; i++) {
		free(data[i].modulators);
	}
	free(data);
}

/* Returns true if two modulators are identical (same sources, dest, transform type).
 * Does not compare transform_amount — used for SF2 spec §9.5 summing. */
static bool mod_is_identical(const SS_Modulator *a, const SS_Modulator *b) {
	return a->source_enum == b->source_enum &&
	       a->amount_source_enum == b->amount_source_enum &&
	       a->dest_enum == b->dest_enum &&
	       a->transform_type == b->transform_type;
}

/*
 * Collect all synthesis data for a (note, velocity) pair.
 * Matches BasicPreset.getVoiceParameters() in basic_preset.ts:
 *  - Generators: defaults, overridden by inst global then inst zone, then preset
 *    generators are added (summed) with int16 clamp.  EMU attenuation applied here.
 *  - Modulators: inst zone + unique inst global + unique bank defaults + preset mods
 *    (identical preset+inst mods have their amounts summed per SF2 spec §9.5).
 */
size_t ss_preset_get_synthesis_data(const SS_BasicPreset *preset,
                                    int midi_note, int velocity,
                                    SS_SynthesisData **out_data) {
	size_t capacity = 32;
	SS_SynthesisData *result = (SS_SynthesisData *)calloc(capacity,
	                                                      sizeof(SS_SynthesisData));
	if(!result) {
		*out_data = NULL;
		return 0;
	}
	size_t count = 0;

	/* Bank default modulators (preset's parent bank, or global defaults) */
	const SS_Modulator *def_mods = SS_DEFAULT_MODULATORS;
	size_t def_mod_count = SS_DEFAULT_MODULATOR_COUNT;
	if(preset->parent_bank && preset->parent_bank->custom_default_modulators) {
		def_mods = preset->parent_bank->default_modulators;
		def_mod_count = preset->parent_bank->default_mod_count;
	}

	for(size_t pi = 0; pi < preset->zone_count; pi++) {
		const SS_PresetZone *pz = &preset->zones[pi];
		if(pz->base.key_range_min >= 0) {
			if(midi_note < pz->base.key_range_min ||
			   midi_note > pz->base.key_range_max) continue;
		}
		if(pz->base.vel_range_min >= 0) {
			if(velocity < pz->base.vel_range_min ||
			   velocity > pz->base.vel_range_max) continue;
		}

		SS_BasicInstrument *inst = pz->instrument;
		if(!inst) continue;

		/* Preset generator offsets: zero-based, type-indexed.
		 * Global zone sets first; local zone overrides (later write wins). */
		int16_t preset_gens[SS_GEN_COUNT];
		memset(preset_gens, 0, sizeof(preset_gens));
		for(size_t g = 0; g < preset->global_zone.gen_count; g++) {
			SS_GeneratorType t = preset->global_zone.generators[g].type;
			if(t >= 0 && t < SS_GEN_COUNT)
				preset_gens[t] = preset->global_zone.generators[g].value;
		}
		for(size_t g = 0; g < pz->base.gen_count; g++) {
			SS_GeneratorType t = pz->base.generators[g].type;
			if(t >= 0 && t < SS_GEN_COUNT)
				preset_gens[t] = pz->base.generators[g].value;
		}

		/* Preset modulator list: preset zone mods first, then unique from preset global. */
		size_t preset_mod_cap = pz->base.mod_count + preset->global_zone.mod_count;
		SS_Modulator *preset_mods = (SS_Modulator *)malloc(
		(preset_mod_cap + 1) * sizeof(SS_Modulator));
		size_t preset_mod_count = 0;
		if(preset_mods) {
			for(size_t m = 0; m < pz->base.mod_count; m++)
				preset_mods[preset_mod_count++] = ss_modulator_copy(&pz->base.modulators[m]);
			for(size_t m = 0; m < preset->global_zone.mod_count; m++) {
				const SS_Modulator *gm = &preset->global_zone.modulators[m];
				bool found = false;
				for(size_t k = 0; k < preset_mod_count; k++) {
					if(mod_is_identical(&preset_mods[k], gm)) {
						found = true;
						break;
					}
				}
				if(!found)
					preset_mods[preset_mod_count++] = ss_modulator_copy(gm);
			}
		}

		for(size_t ii = 0; ii < inst->zone_count; ii++) {
			const SS_InstrumentZone *iz = &inst->zones[ii];
			if(iz->base.key_range_min >= 0) {
				if(midi_note < iz->base.key_range_min ||
				   midi_note > iz->base.key_range_max) continue;
			}
			if(iz->base.vel_range_min >= 0) {
				if(velocity < iz->base.vel_range_min ||
				   velocity > iz->base.vel_range_max) continue;
			}
			if(!iz->sample) continue;

			if(count >= capacity) {
				capacity *= 2;
				SS_SynthesisData *tmp = (SS_SynthesisData *)realloc(
				result, capacity * sizeof(SS_SynthesisData));
				if(!tmp) {
					free(preset_mods);
					ss_synthesis_data_free_array(result, count);
					*out_data = NULL;
					return 0;
				}
				result = tmp;
			}

			SS_SynthesisData *sd = &result[count++];
			memset(sd, 0, sizeof(*sd));
			sd->sample = iz->sample;

			/* Generator array: start with spec defaults, override with inst global,
			 * override with inst zone, then add (sum) preset generator offsets. */
			for(int g = 0; g < SS_GEN_COUNT; g++)
				sd->generators[g] = SS_GENERATOR_LIMITS[g].def;
			for(size_t g = 0; g < inst->global_zone.gen_count; g++) {
				SS_GeneratorType t = inst->global_zone.generators[g].type;
				if(t >= 0 && t < SS_GEN_COUNT)
					sd->generators[t] = inst->global_zone.generators[g].value;
			}
			for(size_t g = 0; g < iz->base.gen_count; g++) {
				SS_GeneratorType t = iz->base.generators[g].type;
				if(t >= 0 && t < SS_GEN_COUNT)
					sd->generators[t] = iz->base.generators[g].value;
			}
			/* Sum preset offsets — clamp to int16 range to prevent overflow
			 * (per-type range limits are applied later in the modulator compute). */
			for(int g = 0; g < SS_GEN_COUNT; g++) {
				int32_t sum = (int32_t)sd->generators[g] + (int32_t)preset_gens[g];
				if(sum > 32767) sum = 32767;
				if(sum < -32768) sum = -32768;
				sd->generators[g] = (int16_t)sum;
			}
			/* EMU initial attenuation correction: multiply by 0.4. All EMU sound
			 * cards have this quirk; all SF2 editors and players emulate it. */
			sd->generators[SS_GEN_INITIAL_ATTENUATION] =
			(int16_t)((int)sd->generators[SS_GEN_INITIAL_ATTENUATION] * 4 / 10);

			/* Modulator list (SF2 spec §9.5):
			 *  1. Inst zone mods
			 *  2. Unique mods from inst global zone
			 *  3. Unique bank default mods
			 *  4. Preset mods: if an identical inst mod exists, sum amounts;
			 *     otherwise append. */
			size_t mod_cap = iz->base.mod_count + inst->global_zone.mod_count +
			                 def_mod_count + preset_mod_count;
			sd->modulators = (SS_Modulator *)malloc((mod_cap + 1) * sizeof(SS_Modulator));
			sd->mod_count = 0;
			if(sd->modulators) {
				/* 1. Inst zone mods */
				for(size_t m = 0; m < iz->base.mod_count; m++)
					sd->modulators[sd->mod_count++] = ss_modulator_copy(&iz->base.modulators[m]);
				/* 2. Unique from inst global zone */
				for(size_t m = 0; m < inst->global_zone.mod_count; m++) {
					const SS_Modulator *gm = &inst->global_zone.modulators[m];
					bool found = false;
					for(size_t k = 0; k < sd->mod_count; k++) {
						if(mod_is_identical(&sd->modulators[k], gm)) {
							found = true;
							break;
						}
					}
					if(!found)
						sd->modulators[sd->mod_count++] = ss_modulator_copy(gm);
				}
				/* 3. Unique bank defaults */
				for(size_t m = 0; m < def_mod_count; m++) {
					const SS_Modulator *dm = &def_mods[m];
					bool found = false;
					for(size_t k = 0; k < sd->mod_count; k++) {
						if(mod_is_identical(&sd->modulators[k], dm)) {
							found = true;
							break;
						}
					}
					if(!found)
						sd->modulators[sd->mod_count++] = ss_modulator_copy(dm);
				}
				/* 4. Preset mods: sum if identical, append if not */
				for(size_t m = 0; m < preset_mod_count; m++) {
					const SS_Modulator *pm = &preset_mods[m];
					size_t match = sd->mod_count;
					for(size_t k = 0; k < sd->mod_count; k++) {
						if(mod_is_identical(&sd->modulators[k], pm)) {
							match = k;
							break;
						}
					}
					if(match == sd->mod_count) {
						sd->modulators[sd->mod_count++] = ss_modulator_copy(pm);
					} else {
						sd->modulators[match].transform_amount += pm->transform_amount;
					}
				}
			}
		}

		free(preset_mods);
	}

	*out_data = result;
	return count;
}

/* ── SoundBank ───────────────────────────────────────────────────────────── */

SS_SoundBank *ss_soundbank_new(void) {
	SS_SoundBank *bank = (SS_SoundBank *)calloc(1, sizeof(SS_SoundBank));
	return bank;
}

void ss_soundbank_free(SS_SoundBank *bank) {
	if(!bank) return;
	for(size_t i = 0; i < bank->preset_count; i++)
		ss_preset_free(&bank->presets[i]);
	free(bank->presets);
	for(size_t i = 0; i < bank->instrument_count; i++)
		ss_instrument_free(&bank->instruments[i]);
	free(bank->instruments);
	for(size_t i = 0; i < bank->sample_count; i++)
		ss_sample_free_data(&bank->samples[i]);
	free(bank->samples);
	if(bank->custom_default_modulators)
		free(bank->default_modulators);
	free(bank);
}

SS_BasicPreset *ss_soundbank_find_preset(SS_SoundBank *bank,
                                         uint8_t program,
                                         uint16_t bank_msb,
                                         uint16_t bank_lsb,
                                         uint16_t bank_offset,
                                         int midi_system,
                                         bool is_drum_channel) {
	SS_BasicPreset *match = NULL;

	const bool isXG = midi_system == SS_SYSTEM_XG;

	if(is_drum_channel && isXG) {
		/* This shouldn't happen */
		is_drum_channel = false;
		bank_lsb = 0;
		bank_msb = 127;
	}

	const bool xgDrums = (bank_msb == 120 || bank_msb == 127) && isXG;

	for(size_t i = 0; i < bank->preset_count; i++) {
		SS_BasicPreset *p = &bank->presets[i];
		if(p->program != program) continue;
		const bool is_drum_match = (is_drum_channel == p->is_gm_gs_drum);
		if(!is_drum_match && !isXG) continue;
		if((p->bank_lsb + bank_offset) != bank_lsb || (p->bank_msb + bank_offset) != bank_msb) continue;
		match = p;
		break;
	}

	if(match) {
		/* Special case:
		 * Non XG banks sometimes specify melodic "MT" presets at bank 127,
		 * Which matches XG banks.
		 * Testcase: 4gmgsmt-sf2_04-compat.sf2
		 * Only match if the preset declares itself as drums
		 */
		if(!xgDrums || (xgDrums && match->is_xg_drum)) {
			return match;
		}
	}

	/* No exact match... */
	if(is_drum_channel) {
		/* GM/GS drums: check for the exact program match */
		for(size_t i = 0; i < bank->preset_count; i++) {
			SS_BasicPreset *p = &bank->presets[i];
			if(p->program == program && p->is_gm_gs_drum) {
				return p;
			}
		}

		/* No match, pick any matching drum */
		for(size_t i = 0; i < bank->preset_count; i++) {
			SS_BasicPreset *p = &bank->presets[i];
			if(p->program == program && (p->is_gm_gs_drum || p->is_xg_drum)) {
				return p;
			}
		}

		/* No match, pick the first drum preset, preferring GM/GS */
		for(size_t i = 0; i < bank->preset_count; i++) {
			SS_BasicPreset *p = &bank->presets[i];
			if(p->is_gm_gs_drum) {
				return p;
			}
		}

		for(size_t i = 0; i < bank->preset_count; i++) {
			SS_BasicPreset *p = &bank->presets[i];
			if(p->is_gm_gs_drum || p->is_xg_drum) {
				return p;
			}
		}
	}

	if(xgDrums) {
		for(size_t i = 0; i < bank->preset_count; i++) {
			SS_BasicPreset *p = &bank->presets[i];
			if(p->program == program && p->is_xg_drum) {
				return p;
			}
		}

		/* No match, pick any matching drum */
		for(size_t i = 0; i < bank->preset_count; i++) {
			SS_BasicPreset *p = &bank->presets[i];
			if(p->program == program && (p->is_xg_drum || p->is_gm_gs_drum)) {
				return p;
			}
		}

		/* Pick any drums, preferring XG */
		for(size_t i = 0; i < bank->preset_count; i++) {
			SS_BasicPreset *p = &bank->presets[i];
			if(p->is_xg_drum) {
				return p;
			}
		}

		for(size_t i = 0; i < bank->preset_count; i++) {
			SS_BasicPreset *p = &bank->presets[i];
			if(p->is_xg_drum || p->is_gm_gs_drum) {
				return p;
			}
		}
	}

	SS_BasicPreset **matches = NULL;
	size_t match_count = 0;
	size_t allocated_match_count = 0;

	for(size_t i = 0; i < bank->preset_count; i++) {
		SS_BasicPreset *p = &bank->presets[i];
		if(p->program == program && !p->is_gm_gs_drum && !p->is_xg_drum) {
			size_t new_match_count = match_count + 1;
			if(new_match_count > allocated_match_count) {
				allocated_match_count = allocated_match_count ? allocated_match_count * 2 : 16;
				SS_BasicPreset **new_matches = (SS_BasicPreset **)realloc(matches, allocated_match_count * sizeof(SS_BasicPreset *));
				if(!new_matches) {
					free(matches);
					return NULL;
				}
				matches = new_matches;
			}
			if(new_match_count < allocated_match_count) {
				matches[match_count++] = p;
			}
		}
	}

	/* No matches, return the first available preset */
	if(match_count < 1) {
		free(matches);
		return &bank->presets[0];
	}

	match = NULL;
	if(isXG) {
		for(size_t i = 0; i < match_count; i++) {
			SS_BasicPreset *p = matches[i];
			if((p->bank_lsb + bank_offset) == bank_lsb) {
				match = p;
				break;
			}
		}
	} else {
		for(size_t i = 0; i < match_count; i++) {
			SS_BasicPreset *p = matches[i];
			if((p->bank_msb + bank_offset) == bank_msb) {
				match = p;
				break;
			}
		}
	}

	if(match) {
		free(matches);
		return match;
	}

	/* Special XG case: 64 on LSB can't default to 64 MSB.
	 * Testcase: Cybergate.mid
	 * Selects 64 LSB on warm pad, on DLSbyXG.dls it gets replaced with Bird 2 SFX
	 *
	 * Extra case: Bank lsb 126 on Chrono_Trigger_-_To_Far_Away_Times.mid
	 */
	if((bank_lsb != 64 && bank_lsb != 126) || !isXG) {
		const int bank = bank_msb > bank_lsb ? bank_msb : bank_lsb;
		/* Any matching bank. */
		for(size_t i = 0; i < match_count; i++) {
			SS_BasicPreset *p = matches[i];
			if((p->bank_lsb + bank_offset) == bank || (p->bank_msb + bank_offset) == bank) {
				free(matches);
				return p;
			}
		}
	}

	/* Return the first match */
	match = matches[0];
	free(matches);

	return match;
}

static void soundbank_parse(SS_SoundBank *bank) {
	bank->is_xg_bank = false;
	// Definitions for XG:
	// At least one preset with bank 127, 126 or 120
	// MUST be a valid XG bank.
	// Allowed banks: (see XG specification)
	// Note: XG spec numbers the programs from 1...
	static const bool allowedPrograms[128] = {
		[0] = true, [1] = true, [2] = true, [3] = true, [4] = true, [5] = true, [6] = true, [7] = true, [8] = true, [9] = true, [16] = true, [17] = true, [24] = true, [25] = true, [26] = true, [27] = true, [28] = true, [29] = true, [30] = true, [31] = true, [32] = true, [33] = true, [40] = true, [41] = true, [48] = true, [56] = true, [57] = true, [58] = true, [64] = true, [65] = true, [66] = true, [126] = true, [127] = true
	};

	for(size_t i = 0; i < bank->preset_count; i++) {
		SS_BasicPreset *p = &bank->presets[i];
		if(p->bank_msb == 120 || p->bank_msb == 127) {
			bank->is_xg_bank = true;
			if(p->program < 128 && !allowedPrograms[p->program]) {
				// Not valid!
				bank->is_xg_bank = false;
				break;
			}
		}
	}

	if(bank->is_xg_bank) {
		for(size_t i = 0; i < bank->preset_count; i++) {
			SS_BasicPreset *p = &bank->presets[i];
			if(p->bank_msb == 120 || p->bank_msb == 127) {
				p->is_xg_drum = true;
			}
		}
	}
}

/* Forward declarations for the format-specific loaders */
SS_SoundBank *ss_soundfont_load(const uint8_t *data, size_t size, bool riff64);
SS_SoundBank *ss_dls_load(const uint8_t *data, size_t size);

/* Entry point: dispatch to SF2/DLS reader */
SS_SoundBank *ss_soundbank_load(const uint8_t *data, size_t size) {
	if(!data || size < 12) return NULL;
	/* SF2/SF3: "RIFF" or "RIFS" + size + "sfbk"/"sfpk"/"sfen" */
	SS_SoundBank *res = NULL;
	if(data[0] == 'R' && data[1] == 'I' && data[2] == 'F' && (data[3] == 'F' || data[3] == 'S')) {
		bool riff64 = data[3] == 'S';
		if(size >= (12 + riff64 * 4)) {
			const uint8_t *sig = &data[8 + riff64 * 4];
			if((sig[0] == 's' && sig[1] == 'f' && sig[2] == 'b' && sig[3] == 'k') ||
			   (sig[0] == 's' && sig[1] == 'f' && sig[2] == 'p' && sig[3] == 'k') ||
			   (sig[0] == 's' && sig[1] == 'f' && sig[2] == 'e' && sig[3] == 'n')) {
				res = ss_soundfont_load(data, size, riff64);
			}
			/* DLS: "RIFF" + size + "DLS " */
			else if(!riff64 && sig[0] == 'D' && sig[1] == 'L' && sig[2] == 'S' && sig[3] == ' ') {
				res = ss_dls_load(data, size);
			}
		}
	}
	if(res) soundbank_parse(res);
	return res;
}
