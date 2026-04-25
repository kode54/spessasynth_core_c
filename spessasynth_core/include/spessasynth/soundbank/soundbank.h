#ifndef SS_SOUNDBANK_H
#define SS_SOUNDBANK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#if __has_include(<spessasynth_core/spessasynth.h>)
#include <spessasynth_core/file.h>
#include <spessasynth_core/midi_enums.h>
#include <spessasynth_core/soundbank_enums.h>
#else
#include "../midi/midi_enums.h"
#include "../utils/file.h"
#include "soundbank_enums.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Bump the allocated sample buffers for interpolators, just in case */
#define SS_SAMPLE_COUNT_BUMP (8)

/* ── Modulator ────────────────────────────────────────────────────────────── */

typedef struct {
	uint16_t source_enum; /* packed ModulatorSource */
	uint16_t amount_source_enum;
	uint16_t dest_enum; /* SS_GeneratorType target */
	int16_t transform_amount;
	uint16_t transform_type;
	float current_value; /* runtime: computed modulator output */
	bool is_effect_modulator;
	bool is_default_resonant_modulator;
	bool is_mod_wheel_modulator;
} SS_Modulator;

SS_Modulator ss_modulator_copy(const SS_Modulator *src);
bool ss_modulator_is_identical(const SS_Modulator *a, const SS_Modulator *b);

/* ── Generator ────────────────────────────────────────────────────────────── */

typedef struct {
	SS_GeneratorType type;
	int16_t value;
} SS_Generator;

/* ── Zone (base for both instrument and preset zones) ────────────────────── */

typedef struct {
	int8_t key_range_min; /* -1 = unset */
	int8_t key_range_max;
	int8_t vel_range_min; /* -1 = unset */
	int8_t vel_range_max;
	SS_Generator *generators;
	size_t gen_count;
	SS_Modulator *modulators;
	size_t mod_count;
} SS_Zone;

void ss_zone_free(SS_Zone *z);

/* ── Sample ───────────────────────────────────────────────────────────────── */

typedef enum SS_PCMType {
	SS_SMPLT_8BIT = 0,
	SS_SMPLT_16BIT = 1,
	SS_SMPLT_FLOAT = 2,
	SS_SMPLT_ALAW = 3,
	SS_SMPLT_SPLIT_24BIT = 4,
	SS_SMPLT_COMPRESSED = 5
} SS_PCMType;

typedef struct SS_BasicSample {
	char name[41];
	uint32_t sample_rate;
	uint8_t original_key;
	int8_t pitch_correction;
	SS_SampleType sample_type;
	uint32_t loop_start; /* sample-points from sample start */
	uint32_t loop_end;

	/* Lock protecting the decode function. */
	SS_Mutex *mutex;

	/* Decoded PCM float data (mono). NULL until decoded. */
	float *audio_data;
	size_t audio_data_length; /* sample frames */

	SS_File *audio_file; /* possibly owned range limited file, containing original sample data */
	SS_PCMType audio_file_type;
	size_t audio_file_block_align; /* ignored for SS_SMPLT_COMPRESSED */
	size_t audio_file_sample_offset, audio_file_sample_count;

	SS_File *audio_file_sm24; /* possibly owned range limited file, containing low 8 bits of 24 bit samples */

	/* Raw compressed data for SF3 (Vorbis/FLAC/WAV container). Freed after decode. */
	uint8_t *compressed_data;
	size_t compressed_data_length;
	bool is_compressed;

	/* SF2 or DLS raw s16le slice. Freed after decode. */
	uint8_t *s16le_data;
	size_t s16le_length; /* bytes */

	/* DLS 8-bit */
	uint8_t *u8_data;
	size_t u8_length; /* bytes */

	bool data_overridden;

	/* true for samples owned by the bank; false for per-zone copies that share
	 * compressed_data / s16le_data with the bank sample.  Guards cleanup in
	 * ss_sample_free_data().  audio_data is always freed when non-NULL. */
	bool owns_raw_data;
	bool is_sf2pack;

	/* Zone-level address-offset generator fixups (SF2 gens 0/4 and 1/12).
	 * Applied to audio_data at voice-creation time; zero for bank samples. */
	uint32_t sample_start; /* frame offset into audio_data for playback start */
	int32_t end_adjustment; /* signed adjustment to (audio_data_length - 1) for playback end */

	/* Stereo link (non-owning pointer into bank's samples array). */
	struct SS_BasicSample *linked_sample;
} SS_BasicSample;

/** Decode compressed/s16le data into audio_data. No-op if already decoded. */
bool ss_sample_decode(SS_BasicSample *s);

/** Free all owned data inside the sample (does not free the struct itself). */
void ss_sample_free_data(SS_BasicSample *s);

/* ── Instrument zone ──────────────────────────────────────────────────────── */

typedef struct {
	SS_Zone base;
	/* Owning pointer to a per-zone sample copy.  The copy shares audio_data
	 * with the bank's sample array but has its own metadata (loop points,
	 * root key, etc.) so that zone-level fixups can be applied independently.
	 * NULL when the zone carries no sample (global zone pattern). */
	SS_BasicSample *sample;
} SS_InstrumentZone;

/* ── Instrument ───────────────────────────────────────────────────────────── */

typedef struct {
	char name[41];
	SS_Zone global_zone;
	SS_InstrumentZone *zones;
	size_t zone_count;
} SS_BasicInstrument;

void ss_instrument_free(SS_BasicInstrument *inst);

/* ── Preset zone ──────────────────────────────────────────────────────────── */

typedef struct {
	SS_Zone base;
	SS_BasicInstrument *instrument; /* non-owning */
} SS_PresetZone;

/* ── Synthesis data (flattened zone data passed to voice creation) ────────── */

typedef struct {
	SS_BasicSample *sample;
	/* Fully resolved generator array: defaults overridden by instrument layer,
	 * then preset layer summed in (with int16 clamp), EMU attenuation applied. */
	int16_t generators[SS_GEN_COUNT];
	SS_Modulator *modulators; /* inst + inst global (unique) + bank defaults (unique) + preset (summed) */
	size_t mod_count;
} SS_SynthesisData;

/* ── Preset ───────────────────────────────────────────────────────────────── */

typedef struct SS_BasicPreset {
	char name[41];
	uint8_t program;
	uint8_t bank_msb;
	uint8_t bank_lsb;
	bool is_gm_gs_drum;
	bool is_xg_drum;
	SS_Zone global_zone;
	SS_PresetZone *zones;
	size_t zone_count;
	uint32_t library;
	uint32_t genre;
	uint32_t morphology;
	uint32_t minchan; /* Minimum channel match */
	uint32_t numchan; /* Channel count, match all if 0 */
	struct SS_SoundBank *parent_bank; /* non-owning */
} SS_BasicPreset;

void ss_preset_free(SS_BasicPreset *p);

/**
 * Collect all synthesis data (sample + merged generators/modulators) for a
 * given MIDI note and velocity.  Caller must free() out_data when done.
 * Returns count written into out_data; 0 means no matching zones.
 */
size_t ss_preset_get_synthesis_data(
const SS_BasicPreset *preset,
int midi_note, int velocity,
SS_SynthesisData **out_data /* heap-allocated array */
);

void ss_synthesis_data_free_array(SS_SynthesisData *data, size_t count);

/* ── SoundBank ────────────────────────────────────────────────────────────── */

typedef struct SS_SoundBank {
	/* Presets */
	SS_BasicPreset *presets;
	size_t preset_count;
	/* Instruments (owned) */
	SS_BasicInstrument *instruments;
	size_t instrument_count;
	/* Samples (owned) */
	SS_BasicSample *samples;
	size_t sample_count;
	/* Gain change */
	float gain;
	/* Default modulators */
	SS_Modulator *default_modulators;
	size_t default_mod_count;
	bool custom_default_modulators;
	/* Info */
	char name[257];
	char sound_engine[257];
	char software[257];
	struct {
		uint16_t major;
		uint16_t minor;
	} version;
	bool is_xg_bank;
} SS_SoundBank;

SS_SoundBank *ss_soundbank_new(void);
void ss_soundbank_free(SS_SoundBank *bank);

/**
 * Filter/remap rule applied to a source SS_SoundBank to produce an
 * SS_FilteredBank.  Bank values pack MSB in the low 8 bits and LSB in
 * the high 8 bits (so 0x..MSB..LSB when read natively).
 */
typedef struct SS_FilteredBankRule {
	int source_program; /* 0..127, or -1 for all programs */
	int source_bank; /* (MSB) | (LSB << 8), or -1 for all banks */
	int destination_program; /* remap if source_program>=0; else offset added to all programs */
	int destination_bank; /* remap if source_bank>=0; else offset added to all banks */
	int minimum_channel; /* 0-based inclusive lower MIDI channel bound */
	int channel_count; /* 0 = applies to all channels */
} SS_FilteredBankRule;

/**
 * A filtered copy of a source bank's preset list.  Owns only `presets`.
 * Preset inner pointers (zones, global_zone.generators, etc.) are shared
 * with the parent bank and MUST NOT be deep-freed from here.
 */
typedef struct SS_FilteredBank {
	SS_SoundBank *parent_bank; /* non-owning here; ownership tracked at SS_FilteredBanks level */
	SS_BasicPreset *presets; /* OWNED: shallow-copied, remapped presets */
	size_t preset_count;
	int minimum_channel; /* 0-based channel range start */
	int channel_count; /* 0 = applies to all channels */
} SS_FilteredBank;

/**
 * A collection of filtered banks, typically the result of one sflist
 * load or one raw bank wrap.  Owns both the fbanks array and (when
 * freed with free_banks=true) the underlying SS_SoundBanks — which are
 * deduplicated across fbanks so a bank shared by multiple entries is
 * only freed once.
 */
typedef struct SS_FilteredBanks {
	SS_FilteredBank *fbanks;
	size_t count;
} SS_FilteredBanks;

/**
 * Build one SS_FilteredBank from a source bank and a single rule.
 * On success, allocates out->presets with the filtered/remapped copies.
 * Returns false and leaves *out untouched on OOM.
 */
bool ss_filtered_bank_build_one(SS_FilteredBank *out,
                                SS_SoundBank *bank,
                                const SS_FilteredBankRule *rule);

/** Release the presets array inside a single SS_FilteredBank and zero it.
 *  Does NOT free parent_bank. */
void ss_filtered_bank_dispose(SS_FilteredBank *fb);

/**
 * Build an SS_FilteredBanks from one source bank and N rules.
 * rule_count==0 synthesizes a single passthrough rule
 * ({-1,-1,0,0,0,0}) so every call produces at least one entry.
 * On OOM returns NULL.
 */
SS_FilteredBanks *ss_filtered_banks_build(SS_SoundBank *bank,
                                          const SS_FilteredBankRule *rules,
                                          size_t rule_count);

/**
 * Free an SS_FilteredBanks.  When free_banks is true, each underlying
 * SS_SoundBank is ss_soundbank_free'd exactly once even if referenced
 * by multiple fbanks.
 */
void ss_filtered_banks_free(SS_FilteredBanks *fbs, bool free_banks);

/**
 * Find a preset from an array of filtered banks.  Mirrors
 * ss_soundbanks_find_preset but iterates filtered preset arrays
 * (no runtime bank_offset — offsets are pre-baked by the builder).
 *
 * target_channel: -1 to ignore per-fbank channel filter; otherwise
 * fbanks whose [minimum_channel, minimum_channel+channel_count) range
 * doesn't include target_channel are skipped.
 */
SS_BasicPreset *ss_filtered_banks_find_preset(
SS_FilteredBank *const *fbanks,
size_t fbank_count,
int target_channel,
uint8_t program,
uint16_t bank_msb,
uint16_t bank_lsb,
int midi_system,
bool is_drum_channel);

/**
 * Find a preset by bank + program.
 * midi_system: 0 = GM, 1 = GS, 2 = XG — affects drum channel lookup.
 *
 * Capital tone fallback will fail seriously if using this to single
 * step multiple banks.
 */
SS_BasicPreset *ss_soundbank_find_preset(
SS_SoundBank *bank,
uint16_t bank_offset,
uint8_t program,
uint16_t bank_msb,
uint16_t bank_lsb,
int midi_system,
bool is_drum_channel);

/**
 * Find a preset from multiple banks, using capital tone fallback
 * Otherwise, same parameters as the above function.
 */
SS_BasicPreset *ss_soundbanks_find_preset(
SS_SoundBank **banks,
const uint16_t *bank_offsets,
size_t bank_count,
uint8_t program,
uint16_t bank_msb,
uint16_t bank_lsb,
int midi_system,
bool is_drum_channel);

/**
 * Load a SoundFont2/SF3 or DLS soundbank from a raw buffer.
 * Caller is responsible for calling ss_soundbank_free() when done.
 */
SS_SoundBank *ss_soundbank_load(SS_File *file);

/* ── Default SF2 modulators list ─────────────────────────────────────────── */

#define MODSRC(curve, isbip, isneg, iscc, idx) ((uint16_t)(((uint16_t)(curve) << 10) | (((isbip) ? 1 : 0) << 9) | (((isneg) ? 1 : 0) << 8) | (((iscc) ? 1 : 0) << 7) | (idx)))

#define MODISEFFECT(s1, s2, dest) \
	(((s1) == 0x00db || (s1) == 0x00dd) && (s2) == 0x0000 && ((dest) == SS_GEN_REVERB_EFFECTS_SEND || (dest) == SS_GEN_CHORUS_EFFECTS_SEND))

#define MODISDEFAULTRESONANT(s1, s2, dest) \
	((s1) == DEFAULT_RESONANT_MOD_SOURCE && (s2) == 0x0 && (dest) == SS_GEN_INITIAL_FILTER_Q)

#define ISCC(srcenum) (((srcenum) & (1 << 7)) != 0)
#define SRCIDX(srcenum) ((srcenum) & 127)

#define MODISMODWHEEL(s1, s2, dest) \
	(((ISCC((s1)) && SRCIDX((s1)) == SS_MIDCON_MODULATION_WHEEL) || (ISCC((s2)) && SRCIDX((s2)) == SS_MIDCON_MODULATION_WHEEL)) && ((dest) == SS_GEN_MOD_LFO_TO_PITCH || (dest) == SS_GEN_VIB_LFO_TO_PITCH))

#define MODULATOR(s1, s2, dest, amount, transform) { (s1), (s2), (dest), (amount), (transform), 0, MODISEFFECT((s1), (s2), (dest)), MODISDEFAULTRESONANT((s1), (s2), (dest)), MODISMODWHEEL((s1), (s2), (dest)) }

#define DEFAULT_RESONANT_MOD_SOURCE (MODSRC( \
SS_MODCURVE_LINEAR,                          \
true,                                        \
false,                                       \
true,                                        \
SS_MIDCON_FILTER_RESONANCE)) /* Linear forwards bipolar cc 74 */

extern const SS_Modulator SS_DEFAULT_MODULATORS[];
extern const size_t SS_DEFAULT_MODULATOR_COUNT;

#ifdef __cplusplus
}
#endif

#endif /* SS_SOUNDBANK_H */
