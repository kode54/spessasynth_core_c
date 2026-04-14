#ifndef SS_SOUNDBANK_H
#define SS_SOUNDBANK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#if __has_include(<spessasynth_core/spessasynth.h>)
#include <spessasynth_core/soundbank_enums.h>
#include <spessasynth_core/file.h>
#else
#include "soundbank_enums.h"
#include "../utils/file.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

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

typedef struct SS_BasicSample {
	char name[41];
	uint32_t sample_rate;
	uint8_t original_key;
	int8_t pitch_correction;
	SS_SampleType sample_type;
	uint32_t loop_start; /* sample-points from sample start */
	uint32_t loop_end;

	/* Decoded PCM float data (mono). NULL until decoded. */
	float *audio_data;
	size_t audio_data_length; /* sample frames */

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
 * Find a preset by bank + program.
 * midi_system: 0 = GM, 1 = GS, 2 = XG — affects drum channel lookup.
 */
SS_BasicPreset *ss_soundbank_find_preset(
SS_SoundBank *bank,
uint8_t program,
uint16_t bank_msb,
uint16_t bank_lsb,
uint16_t bank_offset,
int midi_system,
bool is_drum_channel,
bool try_inexact);

/**
 * Load a SoundFont2/SF3 or DLS soundbank from a raw buffer.
 * Caller is responsible for calling ss_soundbank_free() when done.
 */
SS_SoundBank *ss_soundbank_load(SS_File *file);

/* ── Default SF2 modulators list ─────────────────────────────────────────── */

extern const SS_Modulator SS_DEFAULT_MODULATORS[];
extern const size_t SS_DEFAULT_MODULATOR_COUNT;

#ifdef __cplusplus
}
#endif

#endif /* SS_SOUNDBANK_H */
