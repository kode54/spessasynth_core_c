#include <math.h>
#include <stdbool.h>
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

#define DEFAULT_ATTENUATION_MOD_AMOUNT 960
#define DEFAULT_ATTENUATION_MOD_CURVE_TYPE SS_MODCURVE_CONCAVE

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
	ss_mutex_enter(s->mutex);
	if(s->audio_data) {
		ss_mutex_leave(s->mutex);
		return true; /* already decoded */
	}

	if(s->audio_file) {
		/* Load the sample from a file object */
		switch(s->audio_file_type) {
			case SS_SMPLT_8BIT: {
				const size_t block_align = s->audio_file_block_align;
				const size_t frame_count = ss_file_size(s->audio_file) / block_align;
				uint8_t *temp = (uint8_t *)malloc(frame_count * block_align);
				s->audio_data = (float *)malloc((frame_count + SS_SAMPLE_COUNT_BUMP) * sizeof(float));
				if(temp && s->audio_data) {
					const uint8_t *in = temp;
					float *out = s->audio_data;
					ss_file_read_bytes(s->audio_file, 0, temp, frame_count * block_align);
					for(size_t i = 0; i < frame_count; i++)
						out[i] = ((float)in[i * block_align] - 128.0) / 128.0;
					memset(s->audio_data + frame_count, 0, SS_SAMPLE_COUNT_BUMP * sizeof(float));
					s->audio_data_length = frame_count;
				}
				free(temp);
				ss_mutex_leave(s->mutex);
				return true;
			}

			case SS_SMPLT_16BIT: {
				const size_t block_align = s->audio_file_block_align;
				const size_t frame_count = ss_file_size(s->audio_file) / block_align;
				uint8_t *temp = (uint8_t *)malloc(frame_count * block_align);
				s->audio_data = (float *)malloc((frame_count + SS_SAMPLE_COUNT_BUMP) * sizeof(float));
				if(temp && s->audio_data) {
					const uint8_t *in = temp;
					float *out = s->audio_data;
					ss_file_read_bytes(s->audio_file, 0, temp, frame_count * block_align);
					for(size_t i = 0; i < frame_count; i++) {
						int16_t sample = (int16_t)((uint16_t)in[i * block_align] | ((uint16_t)in[i * block_align + 1] << 8));
						out[i] = ((float)sample) / 32768.0;
					}
					memset(s->audio_data + frame_count, 0, SS_SAMPLE_COUNT_BUMP * sizeof(float));
					s->audio_data_length = frame_count;
				}
				free(temp);
				ss_mutex_leave(s->mutex);
				return true;
			}

			case SS_SMPLT_SPLIT_24BIT: {
				if(!s->audio_file_sm24) {
					ss_mutex_leave(s->mutex);
					return false;
				}
				const size_t frame_count = ss_file_size(s->audio_file) / 2;
				if(ss_file_size(s->audio_file_sm24) != frame_count) {
					ss_mutex_leave(s->mutex);
					return false;
				}
				uint8_t *temp16 = (uint8_t *)malloc(frame_count * 2);
				uint8_t *temp8 = (uint8_t *)malloc(frame_count);
				s->audio_data = (float *)malloc((frame_count + SS_SAMPLE_COUNT_BUMP) * sizeof(float));
				if(temp16 && temp8 && s->audio_data) {
					const uint8_t *in16 = temp16;
					const uint8_t *in8 = temp8;
					float *out = s->audio_data;
					ss_file_read_bytes(s->audio_file, 0, temp16, frame_count * 2);
					ss_file_read_bytes(s->audio_file, 0, temp8, frame_count);
					for(size_t i = 0; i < frame_count; i++) {
						int32_t sample = (int32_t)(((uint32_t)in16[i * 2] << 16) | ((uint32_t)in16[i * 2 + 1] << 24) | ((uint32_t)in8[i] << 8)); 
						sample >>= 8;
						out[i] = (float)sample / 8388608.0;
					}
					memset(s->audio_data + frame_count, 0, SS_SAMPLE_COUNT_BUMP * sizeof(float));
					s->audio_data_length = frame_count;
				}
				free(temp8);
				free(temp16);
				ss_mutex_leave(s->mutex);
				return true;
			}

			case SS_SMPLT_ALAW: {
				const size_t block_align = s->audio_file_block_align;
				const size_t frame_count = ss_file_size(s->audio_file) / block_align;
				uint8_t *temp = (uint8_t *)malloc(frame_count * block_align);
				s->audio_data = (float *)malloc((frame_count + SS_SAMPLE_COUNT_BUMP) * sizeof(float));
				if(temp && s->audio_data) {
					const uint8_t *in = temp;
					float *out = s->audio_data;
					ss_file_read_bytes(s->audio_file, 0, temp, frame_count * block_align);
					for(size_t i = 0; i < frame_count; i++) {
						const uint8_t input = (int)in[i * block_align];

						/* https://en.wikipedia.org/wiki/G.711#A-law */
						/* Re-toggle toggled bits */
						uint8_t sample = input ^ 0x55;

						/* Remove sign bit */
						sample &= 0x7f;

						/* Extract exponent */
						const uint8_t exponent = sample >> 4;
						/* Extract mantissa */
						int16_t mantissa = sample & 0xf;
						if(exponent > 0) {
							mantissa += 16; /* Add leading '1', if exponent > 0 */
						}

						mantissa = (mantissa << 4) + 0x8;
						if(exponent > 1) {
							mantissa = mantissa << (exponent - 1);
						}

						const int16_t s16sample = input > 127 ? mantissa : -mantissa;

						/* Convert to floating point */
						out[i] = (float)s16sample / 32768.0;
					}
					memset(s->audio_data + frame_count, 0, SS_SAMPLE_COUNT_BUMP * sizeof(float));
					s->audio_data_length = frame_count;
				}
				free(temp);
				ss_mutex_leave(s->mutex);
				return true;
			}

			case SS_SMPLT_FLOAT: {
				const size_t block_align = s->audio_file_block_align;
				const size_t frame_count = ss_file_size(s->audio_file) / block_align;
				s->audio_data = (float *)malloc((frame_count + SS_SAMPLE_COUNT_BUMP) * sizeof(float));
				if(s->audio_data) {
					ss_file_read_bytes(s->audio_file, 0, (uint8_t *)s->audio_data, frame_count * sizeof(float));
					memset(s->audio_data + frame_count, 0, SS_SAMPLE_COUNT_BUMP * sizeof(float));
					s->audio_data_length = frame_count;
				}
				ss_mutex_leave(s->mutex);
				return true;
			}

			case SS_SMPLT_COMPRESSED: {
				size_t size = ss_file_size(s->audio_file);
				if(size >= 4) {
					uint8_t hdr[4];
					ss_file_read_bytes(s->audio_file, 0, hdr, 4);

					if(hdr[0] == 'O' && hdr[1] == 'g' && hdr[2] == 'g' && hdr[3] == 'S') {
#ifdef SS_HAVE_LIBVORBISFILE
						bool res = ss_vorbis_decode(s);
						ss_mutex_leave(s->mutex);
						return res;
#else
						ss_mutex_leave(s->mutex);
						return false;
#endif
					}
					if(hdr[0] == 'f' && hdr[1] == 'L' && hdr[2] == 'a' && hdr[3] == 'C') {
#ifdef SS_HAVE_LIBFLAC
						bool res = ss_flac_decode(s);
						ss_mutex_leave(s->mutex);
						return res;
#else
						return false;
#endif
					}
					break;
				}
			}
		}
	}

	if(s->is_compressed && s->compressed_data) {
		/* Dispatch to vorbis/FLAC/WAV decoder based on magic bytes */
		if(s->is_sf2pack) {
			s->audio_data = (float *)malloc(s->compressed_data_length + SS_SAMPLE_COUNT_BUMP * sizeof(float));
			if(s->audio_data) {
				memcpy(s->audio_data, s->compressed_data, s->compressed_data_length);
				s->audio_data_length = s->compressed_data_length / sizeof(float);
				memset(s->audio_data + s->audio_data_length, 0, SS_SAMPLE_COUNT_BUMP * sizeof(float));
				ss_mutex_leave(s->mutex);
				return true;
			}
			ss_mutex_leave(s->mutex);
			return false;
		}
		if(s->compressed_data_length >= 4) {
			const uint8_t *hdr = s->compressed_data;
			if(hdr[0] == 'O' && hdr[1] == 'g' && hdr[2] == 'g' && hdr[3] == 'S') {
#ifdef SS_HAVE_LIBVORBISFILE
				bool res = ss_vorbis_decode(s);
				ss_mutex_leave(s->mutex);
				return res;
#else
				ss_mutex_leave(s->mutex);
				return false;
#endif
			}
			if(hdr[0] == 'f' && hdr[1] == 'L' && hdr[2] == 'a' && hdr[3] == 'C') {
#ifdef SS_HAVE_LIBFLAC
				bool res = ss_flac_decode(s);
				ss_mutex_leave(s->mutex);
				return res;
#else
				ss_mutex_leave(s->mutex);
				return false;
#endif
			}
		}
		ss_mutex_leave(s->mutex);
		return false;
	}

	if(s->s16le_data && s->s16le_length >= 2) {
		size_t sample_count = s->s16le_length / 2;
		s->audio_data = (float *)malloc((sample_count + SS_SAMPLE_COUNT_BUMP) * sizeof(float)); /* Allocate a little more for interpolators */
		if(!s->audio_data) {
			ss_mutex_leave(s->mutex);
			return false;
		}
		memset(s->audio_data + sample_count, 0, SS_SAMPLE_COUNT_BUMP * sizeof(float));
		s->audio_data_length = sample_count;

		const int16_t *src = (const int16_t *)s->s16le_data;
		for(size_t i = 0; i < sample_count; i++) {
			s->audio_data[i] = (float)src[i] / 32768.0f;
		}
		ss_mutex_leave(s->mutex);
		return true;
	}

	if(s->u8_data && s->u8_length >= 1) {
		size_t sample_count = s->u8_length;
		s->audio_data = (float *)malloc((sample_count + SS_SAMPLE_COUNT_BUMP) * sizeof(float)); /* Allocate a little more for interpolators */
		if(!s->audio_data) {
			ss_mutex_leave(s->mutex);
			return false;
		}
		memset(s->audio_data + sample_count, 0, SS_SAMPLE_COUNT_BUMP * sizeof(float));
		s->audio_data_length = sample_count;

		const uint8_t *src = s->u8_data;
		for(size_t i = 0; i < sample_count; i++) {
			s->audio_data[i] = ((float)src[i] - 128.0) / 128.0;
		}
		ss_mutex_leave(s->mutex);
		return true;
	}

	/* Zero-length sample: create minimal silent buffer */
	s->audio_data = (float *)calloc(1 + SS_SAMPLE_COUNT_BUMP, sizeof(float));
	s->audio_data_length = 1;
	ss_mutex_leave(s->mutex);
	return true;
}

void ss_sample_free_data(SS_BasicSample *s) {
	/* audio_data is always owned by whichever struct decoded it. */
	free(s->audio_data);
	/* compressed_data / s16le_data / u8_data are owned only by bank samples. */
	if(s->owns_raw_data) {
		ss_mutex_free(s->mutex);
		ss_file_close(s->audio_file);
		free(s->compressed_data);
		free(s->s16le_data);
		free(s->u8_data);
	}
	s->mutex = NULL;
	s->audio_file = NULL;
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
bool ss_modulator_is_identical(const SS_Modulator *a, const SS_Modulator *b) {
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
					if(ss_modulator_is_identical(&preset_mods[k], gm)) {
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
						if(ss_modulator_is_identical(&sd->modulators[k], gm)) {
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
						if(ss_modulator_is_identical(&sd->modulators[k], dm)) {
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
						if(ss_modulator_is_identical(&sd->modulators[k], pm)) {
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
	if(bank) bank->gain = 1.0;
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

/* ── FilteredBank / FilteredBanks ────────────────────────────────────────── */

bool ss_filtered_bank_build_one(SS_FilteredBank *out,
                                SS_SoundBank *bank,
                                const SS_FilteredBankRule *rule) {
	if(!out || !bank || !rule) return false;

	const int src_prog = rule->source_program;
	const int src_bank = rule->source_bank;
	const int dst_prog = rule->destination_program;
	const int dst_bank = rule->destination_bank;
	const int dst_bank_msb = dst_bank & 0xff;
	const int dst_bank_lsb = (dst_bank >> 8) & 0xff;

	SS_BasicPreset *tmp = (SS_BasicPreset *)malloc(bank->preset_count * sizeof(SS_BasicPreset));
	if(!tmp && bank->preset_count > 0) return false;

	size_t kept = 0;
	for(size_t i = 0; i < bank->preset_count; i++) {
		const SS_BasicPreset *p = &bank->presets[i];
		if(src_prog >= 0 && p->program != src_prog) continue;
		if(src_bank >= 0) {
			int p_bank = p->bank_msb | (p->bank_lsb << 8);
			if(p_bank != src_bank) continue;
		}

		SS_BasicPreset copy = *p; /* shallow copy; zones/global_zone pointers shared */
		copy.parent_bank = bank;

		if(src_prog >= 0) {
			copy.program = (uint8_t)(dst_prog & 0x7f);
		} else {
			copy.program = (uint8_t)((p->program + dst_prog) & 0x7f);
		}

		if(src_bank >= 0) {
			copy.bank_msb = (uint8_t)(dst_bank_msb & 0x7f);
			copy.bank_lsb = (uint8_t)(dst_bank_lsb & 0x7f);
		} else {
			copy.bank_msb = (uint8_t)((p->bank_msb + dst_bank_msb) & 0x7f);
			copy.bank_lsb = (uint8_t)((p->bank_lsb + dst_bank_lsb) & 0x7f);
		}

		tmp[kept++] = copy;
	}

	if(kept == 0) {
		free(tmp);
		out->parent_bank = bank;
		out->presets = NULL;
		out->preset_count = 0;
		out->minimum_channel = rule->minimum_channel;
		out->channel_count = rule->channel_count;
		return true;
	}

	/* Shrink to actual kept count; realloc failure is non-fatal (keep the
	 * larger buffer). */
	if(kept < bank->preset_count) {
		SS_BasicPreset *shrunk = (SS_BasicPreset *)realloc(tmp, kept * sizeof(SS_BasicPreset));
		if(shrunk) tmp = shrunk;
	}

	out->parent_bank = bank;
	out->presets = tmp;
	out->preset_count = kept;
	out->minimum_channel = rule->minimum_channel;
	out->channel_count = rule->channel_count;
	return true;
}

void ss_filtered_bank_dispose(SS_FilteredBank *fb) {
	if(!fb) return;
	free(fb->presets);
	fb->presets = NULL;
	fb->preset_count = 0;
	fb->parent_bank = NULL;
	fb->minimum_channel = 0;
	fb->channel_count = 0;
}

SS_FilteredBanks *ss_filtered_banks_build(SS_SoundBank *bank,
                                          const SS_FilteredBankRule *rules,
                                          size_t rule_count) {
	if(!bank) return NULL;

	SS_FilteredBankRule default_rule = { -1, -1, 0, 0, 0, 0 };
	const SS_FilteredBankRule *rule_ptr = rules;
	size_t actual_rule_count = rule_count;
	if(actual_rule_count == 0) {
		rule_ptr = &default_rule;
		actual_rule_count = 1;
	}

	SS_FilteredBanks *rval = (SS_FilteredBanks *)calloc(1, sizeof(*rval));
	if(!rval) return NULL;

	rval->fbanks = (SS_FilteredBank *)calloc(actual_rule_count, sizeof(*rval->fbanks));
	if(!rval->fbanks) {
		free(rval);
		return NULL;
	}
	rval->count = actual_rule_count;

	for(size_t i = 0; i < actual_rule_count; i++) {
		if(!ss_filtered_bank_build_one(&rval->fbanks[i], bank, &rule_ptr[i])) {
			for(size_t j = 0; j < i; j++) ss_filtered_bank_dispose(&rval->fbanks[j]);
			free(rval->fbanks);
			free(rval);
			return NULL;
		}
	}

	return rval;
}

void ss_filtered_banks_free(SS_FilteredBanks *fbs, bool free_banks) {
	if(!fbs) return;
	if(fbs->fbanks) {
		if(free_banks) {
			for(size_t i = 0; i < fbs->count; i++) {
				SS_SoundBank *parent = fbs->fbanks[i].parent_bank;
				if(!parent) continue;
				for(size_t j = i + 1; j < fbs->count; j++) {
					if(fbs->fbanks[j].parent_bank == parent)
						fbs->fbanks[j].parent_bank = NULL;
				}
				ss_soundbank_free(parent);
			}
		}
		for(size_t i = 0; i < fbs->count; i++) {
			free(fbs->fbanks[i].presets);
		}
		free(fbs->fbanks);
	}
	free(fbs);
}

typedef struct {
	SS_BasicPreset *preset;
	uint8_t bank_offset_msb;
	uint8_t bank_offset_lsb;
} SS_SoundBankMatch;

SS_BasicPreset *ss_soundbanks_find_preset(SS_SoundBank **banks,
                                          const uint16_t *bank_offsets,
                                          size_t bank_count,
                                          uint8_t program,
                                          uint16_t bank_msb,
                                          uint16_t bank_lsb,
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

	for(size_t b = 0; b < bank_count; b++) {
		SS_SoundBank *bank = banks[b];
		const unsigned int bank_offset_lsb = bank_offsets[b] >> 8;
		const unsigned int bank_offset_msb = bank_offsets[b] & 0xff;
		for(size_t i = 0; i < bank->preset_count; i++) {
			SS_BasicPreset *p = &bank->presets[i];
			if(p->program != program) continue;
			const bool is_drum_match = (is_drum_channel == p->is_gm_gs_drum);
			if(!is_drum_match && !isXG) continue;
			if((p->bank_lsb + bank_offset_lsb) != bank_lsb || (p->bank_msb + bank_offset_msb) != bank_msb) continue;
			match = p;
			break;
		}
		if(match) break;
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
		for(size_t b = 0; b < bank_count; b++) {
			SS_SoundBank *bank = banks[b];
			for(size_t i = 0; i < bank->preset_count; i++) {
				SS_BasicPreset *p = &bank->presets[i];
				if(p->program == program && p->is_gm_gs_drum) {
					return p;
				}
			}
		}

		/* No match, pick any matching drum */
		for(size_t b = 0; b < bank_count; b++) {
			SS_SoundBank *bank = banks[b];
			for(size_t i = 0; i < bank->preset_count; i++) {
				SS_BasicPreset *p = &bank->presets[i];
				if(p->program == program && (p->is_gm_gs_drum || p->is_xg_drum)) {
					return p;
				}
			}
		}

		/* No match, pick the first drum preset, preferring GM/GS */
		for(size_t b = 0; b < bank_count; b++) {
			SS_SoundBank *bank = banks[b];
			for(size_t i = 0; i < bank->preset_count; i++) {
				SS_BasicPreset *p = &bank->presets[i];
				if(p->is_gm_gs_drum) {
					return p;
				}
			}
		}

		for(size_t b = 0; b < bank_count; b++) {
			SS_SoundBank *bank = banks[b];
			for(size_t i = 0; i < bank->preset_count; i++) {
				SS_BasicPreset *p = &bank->presets[i];
				if(p->is_gm_gs_drum || p->is_xg_drum) {
					return p;
				}
			}
		}
	}

	if(xgDrums) {
		for(size_t b = 0; b < bank_count; b++) {
			SS_SoundBank *bank = banks[b];
			for(size_t i = 0; i < bank->preset_count; i++) {
				SS_BasicPreset *p = &bank->presets[i];
				if(p->program == program && p->is_xg_drum) {
					return p;
				}
			}
		}

		/* No match, pick any matching drum */
		for(size_t b = 0; b < bank_count; b++) {
			SS_SoundBank *bank = banks[b];
			for(size_t i = 0; i < bank->preset_count; i++) {
				SS_BasicPreset *p = &bank->presets[i];
				if(p->program == program && (p->is_xg_drum || p->is_gm_gs_drum)) {
					return p;
				}
			}
		}

		/* Pick any drums, preferring XG */
		for(size_t b = 0; b < bank_count; b++) {
			SS_SoundBank *bank = banks[b];
			for(size_t i = 0; i < bank->preset_count; i++) {
				SS_BasicPreset *p = &bank->presets[i];
				if(p->is_xg_drum) {
					return p;
				}
			}
		}

		for(size_t b = 0; b < bank_count; b++) {
			SS_SoundBank *bank = banks[b];
			for(size_t i = 0; i < bank->preset_count; i++) {
				SS_BasicPreset *p = &bank->presets[i];
				if(p->is_xg_drum || p->is_gm_gs_drum) {
					return p;
				}
			}
		}
	}

	SS_SoundBankMatch *matches = NULL;
	size_t match_count = 0;
	size_t allocated_match_count = 0;

	for(size_t b = 0; b < bank_count; b++) {
		SS_SoundBank *bank = banks[b];
		uint8_t bank_offset_msb = bank_offsets[b] >> 8;
		uint8_t bank_offset_lsb = bank_offsets[b] & 0xff;
		for(size_t i = 0; i < bank->preset_count; i++) {
			SS_BasicPreset *p = &bank->presets[i];
			if(p->program == program && !p->is_gm_gs_drum && !p->is_xg_drum) {
				size_t new_match_count = match_count + 1;
				if(new_match_count > allocated_match_count) {
					allocated_match_count = allocated_match_count ? allocated_match_count * 2 : 16;
					SS_SoundBankMatch *new_matches = (SS_SoundBankMatch *)realloc(matches, allocated_match_count * sizeof(SS_SoundBankMatch));
					if(!new_matches) {
						free(matches);
						return NULL;
					}
					matches = new_matches;
				}
				if(new_match_count < allocated_match_count) {
					size_t idx = match_count++;
					matches[idx].preset = p;
					matches[idx].bank_offset_msb = bank_offset_msb;
					matches[idx].bank_offset_lsb = bank_offset_lsb;
				}
			}
		}
	}

	/* No matches, return the first available preset */
	if(match_count < 1) {
		free(matches);
		return bank_count ? &banks[0]->presets[0] : NULL;
	}

	match = NULL;
	if(isXG) {
		for(size_t i = 0; i < match_count; i++) {
			SS_BasicPreset *p = matches[i].preset;
			const unsigned int bank_offset_lsb = matches[i].bank_offset_lsb;
			if((p->bank_lsb + bank_offset_lsb) == bank_lsb) {
				match = p;
				break;
			}
		}
	} else {
		for(size_t i = 0; i < match_count; i++) {
			SS_BasicPreset *p = matches[i].preset;
			const unsigned int bank_offset_msb = matches[i].bank_offset_msb;
			if((p->bank_msb + bank_offset_msb) == bank_msb) {
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
		const unsigned int bank = bank_msb > bank_lsb ? bank_msb : bank_lsb;
		/* Any matching bank. */
		for(size_t i = 0; i < match_count; i++) {
			SS_BasicPreset *p = matches[i].preset;
			const unsigned int bank_offset_msb = matches[i].bank_offset_msb;
			const unsigned int bank_offset_lsb = matches[i].bank_offset_lsb;
			if((p->bank_lsb + bank_offset_lsb) == bank || (p->bank_msb + bank_offset_msb) == bank) {
				free(matches);
				return p;
			}
		}
	}

	/* Return the first match */
	match = matches[0].preset;
	free(matches);

	return match;
}

/**
 * Single bank helper for the above function.
 *
 * Capital tone fallback will fail seriously if using this to single
 * step multiple banks.
 */
SS_BasicPreset *ss_soundbank_find_preset(SS_SoundBank *bank,
                                         uint16_t bank_offset,
                                         uint8_t program,
                                         uint16_t bank_msb,
                                         uint16_t bank_lsb,
                                         int midi_system,
                                         bool is_drum_channel) {
	return ss_soundbanks_find_preset(&bank, &bank_offset, 1, program, bank_msb,
	                                 bank_lsb, midi_system, is_drum_channel);
}

/* Mirrors the 4-stage cascade in ss_soundbanks_find_preset above, but
 * iterates filtered banks (no runtime offset — offsets are baked into
 * the filtered preset copies) and honors per-fbank channel ranges. */
static inline bool fbank_channel_match(const SS_FilteredBank *fb, int target_channel) {
	if(target_channel < 0) return true;
	if(fb->channel_count <= 0) return true;
	return target_channel >= fb->minimum_channel &&
	       target_channel < fb->minimum_channel + fb->channel_count;
}

SS_BasicPreset *ss_filtered_banks_find_preset(SS_FilteredBank *const *fbanks,
                                              size_t fbank_count,
                                              int target_channel,
                                              uint8_t program,
                                              uint16_t bank_msb,
                                              uint16_t bank_lsb,
                                              int midi_system,
                                              bool is_drum_channel) {
	SS_BasicPreset *match = NULL;
	const bool isXG = midi_system == SS_SYSTEM_XG;

	if(is_drum_channel && isXG) {
		is_drum_channel = false;
		bank_lsb = 0;
		bank_msb = 127;
	}

	const bool xgDrums = (bank_msb == 120 || bank_msb == 127) && isXG;

	for(size_t b = 0; b < fbank_count; b++) {
		SS_FilteredBank *fb = fbanks[b];
		if(!fbank_channel_match(fb, target_channel)) continue;
		for(size_t i = 0; i < fb->preset_count; i++) {
			SS_BasicPreset *p = &fb->presets[i];
			if(p->program != program) continue;
			const bool is_drum_match = (is_drum_channel == p->is_gm_gs_drum);
			if(!is_drum_match && !isXG) continue;
			if(p->bank_lsb != bank_lsb || p->bank_msb != bank_msb) continue;
			match = p;
			break;
		}
		if(match) break;
	}

	if(match) {
		if(!xgDrums || (xgDrums && match->is_xg_drum)) {
			return match;
		}
	}

	if(is_drum_channel) {
		for(size_t b = 0; b < fbank_count; b++) {
			SS_FilteredBank *fb = fbanks[b];
			if(!fbank_channel_match(fb, target_channel)) continue;
			for(size_t i = 0; i < fb->preset_count; i++) {
				SS_BasicPreset *p = &fb->presets[i];
				if(p->program == program && p->is_gm_gs_drum) return p;
			}
		}
		for(size_t b = 0; b < fbank_count; b++) {
			SS_FilteredBank *fb = fbanks[b];
			if(!fbank_channel_match(fb, target_channel)) continue;
			for(size_t i = 0; i < fb->preset_count; i++) {
				SS_BasicPreset *p = &fb->presets[i];
				if(p->program == program && (p->is_gm_gs_drum || p->is_xg_drum)) return p;
			}
		}
		for(size_t b = 0; b < fbank_count; b++) {
			SS_FilteredBank *fb = fbanks[b];
			if(!fbank_channel_match(fb, target_channel)) continue;
			for(size_t i = 0; i < fb->preset_count; i++) {
				SS_BasicPreset *p = &fb->presets[i];
				if(p->is_gm_gs_drum) return p;
			}
		}
		for(size_t b = 0; b < fbank_count; b++) {
			SS_FilteredBank *fb = fbanks[b];
			if(!fbank_channel_match(fb, target_channel)) continue;
			for(size_t i = 0; i < fb->preset_count; i++) {
				SS_BasicPreset *p = &fb->presets[i];
				if(p->is_gm_gs_drum || p->is_xg_drum) return p;
			}
		}
	}

	if(xgDrums) {
		for(size_t b = 0; b < fbank_count; b++) {
			SS_FilteredBank *fb = fbanks[b];
			if(!fbank_channel_match(fb, target_channel)) continue;
			for(size_t i = 0; i < fb->preset_count; i++) {
				SS_BasicPreset *p = &fb->presets[i];
				if(p->program == program && p->is_xg_drum) return p;
			}
		}
		for(size_t b = 0; b < fbank_count; b++) {
			SS_FilteredBank *fb = fbanks[b];
			if(!fbank_channel_match(fb, target_channel)) continue;
			for(size_t i = 0; i < fb->preset_count; i++) {
				SS_BasicPreset *p = &fb->presets[i];
				if(p->program == program && (p->is_xg_drum || p->is_gm_gs_drum)) return p;
			}
		}
		for(size_t b = 0; b < fbank_count; b++) {
			SS_FilteredBank *fb = fbanks[b];
			if(!fbank_channel_match(fb, target_channel)) continue;
			for(size_t i = 0; i < fb->preset_count; i++) {
				SS_BasicPreset *p = &fb->presets[i];
				if(p->is_xg_drum) return p;
			}
		}
		for(size_t b = 0; b < fbank_count; b++) {
			SS_FilteredBank *fb = fbanks[b];
			if(!fbank_channel_match(fb, target_channel)) continue;
			for(size_t i = 0; i < fb->preset_count; i++) {
				SS_BasicPreset *p = &fb->presets[i];
				if(p->is_xg_drum || p->is_gm_gs_drum) return p;
			}
		}
	}

	/* Capital tone fallback: collect all non-drum presets matching program. */
	SS_BasicPreset **matches = NULL;
	size_t match_count = 0;
	size_t allocated_match_count = 0;
	SS_BasicPreset *first_preset = NULL;

	for(size_t b = 0; b < fbank_count; b++) {
		SS_FilteredBank *fb = fbanks[b];
		if(!fbank_channel_match(fb, target_channel)) continue;
		if(!first_preset && fb->preset_count > 0) first_preset = &fb->presets[0];
		for(size_t i = 0; i < fb->preset_count; i++) {
			SS_BasicPreset *p = &fb->presets[i];
			if(p->program == program && !p->is_gm_gs_drum && !p->is_xg_drum) {
				size_t new_match_count = match_count + 1;
				if(new_match_count > allocated_match_count) {
					allocated_match_count = allocated_match_count ? allocated_match_count * 2 : 16;
					SS_BasicPreset **new_matches = (SS_BasicPreset **)realloc(
					matches, allocated_match_count * sizeof(*new_matches));
					if(!new_matches) {
						free(matches);
						return NULL;
					}
					matches = new_matches;
				}
				matches[match_count++] = p;
			}
		}
	}

	if(match_count < 1) {
		free(matches);
		return first_preset;
	}

	match = NULL;
	if(isXG) {
		for(size_t i = 0; i < match_count; i++) {
			if(matches[i]->bank_lsb == bank_lsb) {
				match = matches[i];
				break;
			}
		}
	} else {
		for(size_t i = 0; i < match_count; i++) {
			if(matches[i]->bank_msb == bank_msb) {
				match = matches[i];
				break;
			}
		}
	}

	if(match) {
		free(matches);
		return match;
	}

	if((bank_lsb != 64 && bank_lsb != 126) || !isXG) {
		const unsigned int target = bank_msb > bank_lsb ? bank_msb : bank_lsb;
		for(size_t i = 0; i < match_count; i++) {
			SS_BasicPreset *p = matches[i];
			if(p->bank_lsb == target || p->bank_msb == target) {
				free(matches);
				return p;
			}
		}
	}

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
SS_SoundBank *ss_soundfont_load(SS_File *file, bool riff64);
SS_SoundBank *ss_dls_load(SS_File *file, bool riff64);

/* Entry point: dispatch to SF2/DLS reader */
SS_SoundBank *ss_soundbank_load(SS_File *file) {
	if(!file) return NULL;
	size_t size = ss_file_size(file);
	if(size < 12) return NULL;
	/* SF2/SF3: "RIFF" or "RIFS" + size + "sfbk"/"sfpk"/"sfen" */
	SS_SoundBank *res = NULL;
	char riff_id[5];
	ss_file_read_string(file, 0, riff_id, 4);
	if(riff_id[0] == 'R' && riff_id[1] == 'I' && riff_id[2] == 'F' && (riff_id[3] == 'F' || riff_id[3] == 'S')) {
		const bool riff64 = riff_id[3] == 'S';
		const size_t size_size = riff64 ? 8 : 4;
		if(size >= (8 + size_size)) {
			char sig[5];
			ss_file_read_string(file, 4 + size_size, sig, 4);
			if((sig[0] == 's' && sig[1] == 'f' && sig[2] == 'b' && sig[3] == 'k') ||
			   (sig[0] == 's' && sig[1] == 'f' && sig[2] == 'p' && sig[3] == 'k') ||
			   (sig[0] == 's' && sig[1] == 'f' && sig[2] == 'e' && sig[3] == 'n')) {
				res = ss_soundfont_load(file, riff64);
			}
			/* DLS: "RIFF" + size + "DLS " */
			else if(sig[0] == 'D' && sig[1] == 'L' && sig[2] == 'S' && sig[3] == ' ') {
				res = ss_dls_load(file, riff64);
			}
		}
	}
	if(res) soundbank_parse(res);
	return res;
}

void ss_preset_precache(SS_BasicPreset *p) {
	if(!p) return;
	for(size_t z = 0; z < p->zone_count; z++) {
		SS_PresetZone *zone = &p->zones[z];
		SS_BasicInstrument *inst = zone->instrument;
		if(inst) {
			for(size_t iz = 0; iz < inst->zone_count; iz++) {
				SS_InstrumentZone *instzone = &inst->zones[iz];
				if(instzone->sample) {
					/* No failure checking, the bank will still be loaded. */
					ss_sample_decode(instzone->sample);
				}
			}
		}
	}
}