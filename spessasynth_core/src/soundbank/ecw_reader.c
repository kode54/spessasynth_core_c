/**
 * ecw_reader.c
 * ECW (E-mu / Creative "ECLW" wave set) soundbank loader.
 *
 * ECW is the wave-set format used by E-mu / Creative AWE-era hardware and the
 * Windows software wavetable synth.  It is a flat little-endian binary with a
 * fixed ~1932-byte header that holds offset/size/count triples for each
 * section: bank/drum/patch maps, instruments, patches, three index arrays,
 * sample headers and the raw 16-bit PCM blob.
 *
 * The on-disk layout and parsing algorithm follow the reference reader in the
 * sibling project libsf (ECW.h / ECWReader.cpp by P. Stuer).  libsf's
 * ECW->SF2 *converter* is incomplete (no presets, no articulation), so the
 * conversion into the engine's SS_SoundBank structures is implemented fresh
 * here, modelled on dls_reader.c.
 *
 * NOTE: ECW envelope/LFO fields are unitless 0-255 with no documented mapping
 * to SF2 timecents/centibels.  The K_* / ecw_*_to_* helpers below are
 * best-effort heuristic approximations and are expected to need tuning.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if __has_include(<spessasynth_core/spessasynth.h>)
#include <spessasynth_core/file.h>
#include <spessasynth_core/soundbank.h>
#else
#include "spessasynth/soundbank/soundbank.h"
#include "spessasynth/utils/file.h"
#endif

/* ── ECW header field offsets (bytes from file start) ────────────────────── */

#define ECW_OFF_ID 0
#define ECW_OFF_COPYRIGHT 16    /* 80 bytes  */
#define ECW_OFF_NAME 96         /* 80 bytes  */
#define ECW_OFF_FILENAME 176    /* 256 bytes */
#define ECW_OFF_DESCRIPTION 432 /* 80 bytes  */
#define ECW_OFF_INFORMATION 512 /* 1280 bytes */

/* Each section is an offs/size/count triple of uint32 (12 bytes). */
#define ECW_OFF_BANKMAP 1796
#define ECW_OFF_DRUMNOTEMAP 1808
#define ECW_OFF_MIDIPATCHMAP 1820
#define ECW_OFF_DRUMKITMAP 1832
#define ECW_OFF_INSTRUMENTS 1844
#define ECW_OFF_PATCHES 1856
#define ECW_OFF_ARRAY1 1872
#define ECW_OFF_ARRAY2 1884
#define ECW_OFF_ARRAY3 1896
#define ECW_OFF_SAMPLEHEADERS 1908
#define ECW_OFF_SAMPLEDATA_OFFS 1924
#define ECW_OFF_SAMPLEDATA_SIZE 1928
#define ECW_HEADER_SIZE 1932

#define ECW_INSTRUMENT_SIZE 23
#define ECW_PATCH_SIZE 76
#define ECW_SAMPLE_SIZE 16
#define ECW_SAMPLESET_SIZE 22
#define ECW_MAP_ENTRIES 128 /* uint16 entries per bank/patch/drum map */
#define ECW_MAP_SIZE 256    /* bytes per map */

#define ECW_SAMPLE_RATE 22050
#define ECW_DRUM_BANK_MSB 128

#define ECW_V2_MAX_DEPTH 8 /* recursion cap for type-255 cascade instruments */

/* ── Articulation heuristics (empirical, see file header note) ───────────── */

/* ECW envelope stage fields are signed-char *rate increments*: the sign is
 * the envelope direction (negative = falling) and the magnitude is the rate.
 * A large magnitude steps the envelope quickly (short time), magnitude 1 is
 * the slowest increment (longest time) and 0 means no increment at all - the
 * stage never progresses, i.e. an infinite/steady stage.  SF2 envelopes have
 * a fixed shape, so the sign is normalised away here (we trust the file to
 * encode attack as rising, decay as falling).  Time is inversely proportional
 * to the rate, so timecents fall linearly with log2(rate). */
#define ECW_RATE_MAX 127     /* magnitude of a signed-char increment   */
#define ECW_TC_FAST (-12000) /* rate 127 -> ~1 ms                      */
#define ECW_TC_SLOW 5000     /* rate 1   -> ~ +5000 tc                 */
#define ECW_TC_STEADY 32767  /* rate 0 -> infinite stage (clamped to max) */
/* Sub-header Amplitude (signed) -> initial attenuation, centibels per unit. */
#define ECW_K_AMP 4
/* Patch Detune (signed) -> fine tune cents per unit. */
#define ECW_K_DETUNE 1
/* Pitch LFO depth (0-255) -> vibrato depth cents. */
#define ECW_K_LFO_DEPTH 0.5
/* Pitch LFO speed (0-255) -> Hz. */
#define ECW_K_LFO_SPEED 0.08
/* PitchEnvelopeLevel (signed) -> modEnvToPitch cents per unit. */
#define ECW_K_PITCH_ENV 40

/* ── Parsed ECW structures (host-side, not the packed on-disk layout) ────── */

typedef struct {
	uint8_t split_point; /* highest MIDI note for this sample */
	uint8_t flags;       /* 0-1 no loop, 2+ loop, 129+ loop-shift */
	int8_t fine_tune;    /* 1/256 semitone */
	int8_t coarse_tune;  /* semitones */
	uint32_t sample_start; /* byte offset into PCM blob (raw field / 8) */
	uint32_t loop_start;
	uint32_t loop_end; /* also the end of the sample */
	uint8_t low_key;   /* derived: previous sample's high_key + 1 */
	uint8_t high_key;  /* derived: == split_point */
} ECW_Sample;

typedef struct {
	int8_t pitch_env_level;
	int8_t mod_sensitivity;
	int8_t scale; /* 0 chromatic, 1 fixed pitch, 2 quarter-tone */
	uint16_t array1_index;
	int8_t detune;
	int8_t split_point_adjust;
	/* Pitch envelope (time fields are signed rate increments) */
	int8_t pitch_attack_time, pitch_decay_time, pitch_release_time;
	uint8_t pitch_enable_release;
	/* Amplitude envelope (time fields are signed rate increments) */
	int8_t amp_attack_time, amp_decay_time, amp_release_time;
	uint8_t amp_sustain_level;
	uint8_t amp_enable_release;
	/* Pitch LFO */
	uint8_t lfo_depth, lfo_speed;
	int8_t lfo_delay;
} ECW_Patch;

typedef struct {
	uint32_t sample_index; /* index of the first sample header in the set */
	uint16_t array1_index;
	uint16_t code;
	char name[15];
} ECW_SampleSet;

typedef struct {
	SS_SoundBank *bank;

	uint32_t instr_off, instr_count;
	uint32_t patch_off, patch_count;
	uint32_t midipatchmap_off, midipatchmap_count;
	uint32_t bankmap_off;
	/* NOTE: in ECW.h the two drum sections are named the opposite of their
	 * content.  The section at offset 1808 ("DrumNoteMap", count 1) is the
	 * kit selector: MIDI drum-kit program -> note-map index.  The section at
	 * offset 1832 ("DrumKitMap", count N) holds the N note maps, each mapping
	 * a MIDI note -> ECW instrument index. */
	uint32_t kitsel_off;            /* @1808: kit-program -> note-map index */
	uint32_t notemap_off, notemap_count; /* @1832: note -> instrument maps */

	ECW_Patch *patches;
	ECW_Sample *samples;
	uint32_t sample_count;
	ECW_SampleSet *sets;
	uint32_t set_count;

	/* Index arrays linking patches -> sample runs (see resolve_sample_sets).
	 * A patch's array1_index selects an Array1 slot; Array1[slot] is an index
	 * into the parallel Array2/Array3 pair; Array3[idx] is the first sample of
	 * the run and Array2[idx]'s code identifies the owning sample set. */
	uint16_t *array1;
	uint32_t array1_count;
	uint16_t *array3;
	uint32_t *a3_set;   /* per Array2/3 slot: resolved sample-set index */
	uint32_t a23_count; /* paired Array2/Array3 element count */

	/* Resolution tables */
	int32_t *sample_set_idx; /* per sample: owning set index, -1 if none */

	SS_File *src; /* the input file (non-owning) */
} ECW_Ctx;

/* ── Small helpers ───────────────────────────────────────────────────────── */

static int clampi(int v, int lo, int hi) {
	return v < lo ? lo : (v > hi ? hi : v);
}

/* Read a fixed-width text field, NUL-terminate and trim trailing space/NUL. */
static void ecw_read_text(SS_File *f, size_t off, size_t field_len,
                          char *out, size_t out_size) {
	size_t n = field_len < out_size - 1 ? field_len : out_size - 1;
	ss_file_read_bytes(f, off, (uint8_t *)out, n);
	out[n] = '\0';
	while(n > 0 && (out[n - 1] == ' ' || out[n - 1] == '\0' ||
	                (unsigned char)out[n - 1] < 0x20))
		out[--n] = '\0';
}

/* ── Zone generator / modulator helpers (mirrors dls_reader.c) ───────────── */

static int16_t zone_get_gen(const SS_Zone *z, SS_GeneratorType t, int16_t def) {
	for(size_t i = 0; i < z->gen_count; i++)
		if(z->generators[i].type == t)
			return z->generators[i].value;
	return def;
}

static void zone_set_gen(SS_Zone *z, SS_GeneratorType t, int16_t v) {
	for(size_t i = 0; i < z->gen_count; i++) {
		if(z->generators[i].type == t) {
			z->generators[i].value = v;
			return;
		}
	}
	SS_Generator *tmp = (SS_Generator *)realloc(
	z->generators, (z->gen_count + 1) * sizeof(SS_Generator));
	if(!tmp) return;
	z->generators = tmp;
	z->generators[z->gen_count].type = t;
	z->generators[z->gen_count].value = v;
	z->gen_count++;
}

/* Set a generator, clamping to the SF2 spec limits for that type. */
static void zone_set_gen_clamped(SS_Zone *z, SS_GeneratorType t, int v) {
	if(v < -32768) v = -32768;
	if(v > 32767) v = 32767;
	zone_set_gen(z, t, ss_generator_clamp(t, (int16_t)v));
}

static void zone_add_to_gen(SS_Zone *z, SS_GeneratorType t, int delta) {
	zone_set_gen_clamped(z, t, (int)zone_get_gen(z, t, 0) + delta);
}

/* ── Articulation conversion (heuristic, see file header note) ───────────── */

static int16_t ecw_time_to_tc(int v) {
	int mag = v < 0 ? -v : v; /* sign = direction; magnitude = rate */
	if(mag == 0)
		return ECW_TC_STEADY; /* no increment -> infinite/steady stage */
	if(mag > ECW_RATE_MAX) mag = ECW_RATE_MAX;
	double frac = log2((double)mag) / log2((double)ECW_RATE_MAX);
	double tc = (double)ECW_TC_SLOW - frac * (double)(ECW_TC_SLOW - ECW_TC_FAST);
	return (int16_t)tc;
}

/*
 * Apply one ECW patch + instrument sub-header to an instrument zone, emitting
 * SF2 generators.  Called once per zone (every zone of a sub-header's sample
 * run gets the same articulation).
 */
static void apply_articulation(SS_Zone *z, const ECW_Patch *p,
                               int8_t amplitude, int8_t pan,
                               int8_t coarse, int8_t fine,
                               uint16_t delay_ms, uint8_t excl_group) {
	/* Volume envelope (amplitude envelope). */
	zone_set_gen_clamped(z, SS_GEN_ATTACK_VOL_ENV, ecw_time_to_tc(p->amp_attack_time));
	zone_set_gen_clamped(z, SS_GEN_DECAY_VOL_ENV, ecw_time_to_tc(p->amp_decay_time));
	if(p->amp_enable_release)
		/* "never enter release" -> hold; use a long release */
		zone_set_gen_clamped(z, SS_GEN_RELEASE_VOL_ENV, 5000);
	else
		zone_set_gen_clamped(z, SS_GEN_RELEASE_VOL_ENV, ecw_time_to_tc(p->amp_release_time));
	/* Sustain: ECW level 255 = full -> 0 cB attenuation. */
	zone_set_gen_clamped(z, SS_GEN_SUSTAIN_VOL_ENV,
	                     (255 - (int)p->amp_sustain_level) * 960 / 255);

	/* Envelope onset delay from the sub-header (milliseconds). */
	if(delay_ms > 0) {
		double tc = 1200.0 * log2((double)delay_ms / 1000.0);
		zone_set_gen_clamped(z, SS_GEN_DELAY_VOL_ENV, (int)tc);
	}

	/* Steady loudness from the sub-header amplitude. */
	if(amplitude != 0)
		zone_add_to_gen(z, SS_GEN_INITIAL_ATTENUATION, -(int)amplitude * ECW_K_AMP);

	/* Pan: ECW -63 = hard left .. 64 = hard right; SF2 -500..+500. */
	if(pan != 0) {
		int sf2_pan = clampi(pan * 500 / 63, -500, 500);
		zone_set_gen_clamped(z, SS_GEN_PAN, sf2_pan);
	}

	/* Tuning from the sub-header. */
	if(coarse != 0)
		zone_add_to_gen(z, SS_GEN_COARSE_TUNE, coarse);
	if(fine != 0)
		zone_add_to_gen(z, SS_GEN_FINE_TUNE, (int)fine * 100 / 256);
	if(p->detune != 0)
		zone_add_to_gen(z, SS_GEN_FINE_TUNE, (int)p->detune * ECW_K_DETUNE);

	/* Scale: 1 = fixed pitch (ignore key), 2 = quarter-tone. */
	if(p->scale == 1)
		zone_set_gen_clamped(z, SS_GEN_SCALE_TUNING, 0);
	else if(p->scale == 2)
		zone_set_gen_clamped(z, SS_GEN_SCALE_TUNING, 50);

	/* Pitch LFO -> vibrato. */
	if(p->lfo_depth > 0) {
		zone_set_gen_clamped(z, SS_GEN_DELAY_VIB_LFO, ecw_time_to_tc(p->lfo_delay));
		double hz = (double)p->lfo_speed * ECW_K_LFO_SPEED;
		if(hz > 0.0)
			zone_set_gen_clamped(z, SS_GEN_FREQ_VIB_LFO,
			                     (int)(1200.0 * log2(hz / 8.176)));
		zone_set_gen_clamped(z, SS_GEN_VIB_LFO_TO_PITCH,
		                     (int)((double)p->lfo_depth * ECW_K_LFO_DEPTH));
	}

	/* Pitch envelope -> modulation envelope to pitch. */
	if(p->pitch_env_level != 0) {
		zone_set_gen_clamped(z, SS_GEN_MOD_ENV_TO_PITCH,
		                     (int)p->pitch_env_level * ECW_K_PITCH_ENV);
		zone_set_gen_clamped(z, SS_GEN_ATTACK_MOD_ENV, ecw_time_to_tc(p->pitch_attack_time));
		zone_set_gen_clamped(z, SS_GEN_DECAY_MOD_ENV, ecw_time_to_tc(p->pitch_decay_time));
		if(!p->pitch_enable_release)
			zone_set_gen_clamped(z, SS_GEN_RELEASE_MOD_ENV,
			                     ecw_time_to_tc(p->pitch_release_time));
	}

	/* Exclusive class (e.g. open/closed hi-hat). */
	if(excl_group != 0)
		zone_set_gen_clamped(z, SS_GEN_EXCLUSIVE_CLASS, excl_group);
}

/* ── Section readers ─────────────────────────────────────────────────────── */

/* Read an offs/size/count triple; returns false on a range that overflows. */
static bool read_triple(SS_File *f, size_t base, size_t file_size,
                        uint32_t *offs, uint32_t *size, uint32_t *count) {
	uint32_t o = ss_file_read_le32(f, base);
	uint32_t s = ss_file_read_le32(f, base + 4);
	uint32_t c = ss_file_read_le32(f, base + 8);
	if(s > 0 && ((size_t)o + s > file_size)) return false;
	if(offs) *offs = o;
	if(size) *size = s;
	if(count) *count = c;
	return true;
}

static void decode_patch(SS_File *f, size_t off, ECW_Patch *p) {
	p->pitch_env_level = (int8_t)ss_file_read_u8(f, off + 0);
	p->mod_sensitivity = (int8_t)ss_file_read_u8(f, off + 1);
	p->scale = (int8_t)ss_file_read_u8(f, off + 2);
	p->array1_index = ss_file_read_le16(f, off + 11);
	p->detune = (int8_t)ss_file_read_u8(f, off + 13);
	p->split_point_adjust = (int8_t)ss_file_read_u8(f, off + 16);
	/* Pitch envelope (fields at +27.., see ECW.h). */
	p->pitch_attack_time = (int8_t)ss_file_read_u8(f, off + 30);
	p->pitch_decay_time = (int8_t)ss_file_read_u8(f, off + 32);
	p->pitch_release_time = (int8_t)ss_file_read_u8(f, off + 36);
	p->pitch_enable_release = ss_file_read_u8(f, off + 40);
	/* Amplitude envelope (fields at +57..). */
	p->amp_attack_time = (int8_t)ss_file_read_u8(f, off + 60);
	p->amp_decay_time = (int8_t)ss_file_read_u8(f, off + 62);
	p->amp_sustain_level = ss_file_read_u8(f, off + 65);
	p->amp_release_time = (int8_t)ss_file_read_u8(f, off + 66);
	p->amp_enable_release = ss_file_read_u8(f, off + 70);
	/* Pitch LFO (fields at +72..). */
	p->lfo_depth = ss_file_read_u8(f, off + 72);
	p->lfo_speed = ss_file_read_u8(f, off + 73);
	p->lfo_delay = (int8_t)ss_file_read_u8(f, off + 74);
}

/* ── Sample conversion ───────────────────────────────────────────────────── */

static bool build_samples(ECW_Ctx *ctx, SS_File *sample_blob, size_t blob_size) {
	SS_SoundBank *bank = ctx->bank;
	uint32_t n = ctx->sample_count;
	if(n == 0) return true;

	bank->samples = (SS_BasicSample *)calloc(n, sizeof(SS_BasicSample));
	if(!bank->samples) return false;
	bank->sample_count = n;

	for(uint32_t i = 0; i < n; i++) {
		const ECW_Sample *es = &ctx->samples[i];
		SS_BasicSample *s = &bank->samples[i];

		int32_t set = ctx->sample_set_idx[i];
		if(set >= 0 && (uint32_t)set < ctx->set_count)
			snprintf(s->name, sizeof(s->name), "%.40s", ctx->sets[set].name);
		else
			snprintf(s->name, sizeof(s->name), "Sample%u", i);

		s->sample_rate = ECW_SAMPLE_RATE;
		s->sample_type = SS_SAMPLE_TYPE_MONO;
		s->original_key = (uint8_t)clampi(127 + es->coarse_tune, 0, 127);
		s->pitch_correction = (int8_t)((int)es->fine_tune * 100 / 256);
		s->mutex = ss_mutex_create();
		s->owns_raw_data = true;

		/* sample_start / loop_* are byte offsets into the 16-bit PCM blob;
		 * frame index = byte offset / 2. */
		uint32_t start_b = es->sample_start;
		uint32_t loops_b = es->loop_start;
		uint32_t loope_b = es->loop_end;
		if(start_b > blob_size) start_b = (uint32_t)blob_size;
		if(loope_b > blob_size) loope_b = (uint32_t)blob_size;
		if(loope_b < start_b) loope_b = start_b;
		if(loops_b < start_b) loops_b = start_b;
		if(loops_b > loope_b) loops_b = loope_b;

		uint32_t start_fr = start_b / 2;
		uint32_t loops_fr = loops_b / 2 - start_fr;
		uint32_t loope_fr = loope_b / 2 - start_fr;

		/* Flags >= 129: shift the loop start forward into the sample, by an
		 * amount proportional to (flags - 128).  Heuristic; stays in-bounds. */
		if(es->flags >= 129 && loope_fr > loops_fr) {
			uint32_t span = loope_fr - loops_fr;
			loops_fr += (uint32_t)((es->flags - 128) * span / 128);
			if(loops_fr > loope_fr) loops_fr = loope_fr;
		}

		s->loop_start = loops_fr;
		s->loop_end = loope_fr;

		size_t slice_bytes = (size_t)(loope_b - start_b);
		if(slice_bytes >= 2) {
			s->audio_file = ss_file_slice(sample_blob, start_b, slice_bytes);
			s->audio_file_type = SS_SMPLT_16BIT;
			s->audio_file_block_align = 2;
		}
		/* else: leave audio_file NULL; ss_sample_decode yields a silent buffer */
	}
	return true;
}

/* ── Array / sample-set resolution (ports libsf ECWReader.cpp) ───────────── */

static bool resolve_sample_sets(ECW_Ctx *ctx, const uint16_t *array2) {
	uint32_t n = ctx->sample_count;
	uint32_t pairs = ctx->a23_count;

	/* a3_set[slot] = sample-set index for each Array2/Array3 slot, resolved
	 * by matching Array2[slot] against each set's Code (libsf: a zero or
	 * unmatched code resolves to set 0). */
	ctx->a3_set = (uint32_t *)calloc(pairs ? pairs : 1, sizeof(uint32_t));
	if(!ctx->a3_set) return false;
	for(uint32_t i = 0; i < pairs; i++) {
		uint16_t code = array2[i];
		uint32_t setk = 0;
		if(code != 0) {
			for(uint32_t k = 0; k < ctx->set_count; k++) {
				if(ctx->sets[k].code == code) {
					setk = k;
					break;
				}
			}
		}
		ctx->a3_set[i] = setk;
	}

	/* direct[sample index] = set assigned via the Array3 slot. */
	int32_t *direct = (int32_t *)malloc(n ? n * sizeof(int32_t) : 1);
	if(!direct) return false;
	for(uint32_t i = 0; i < n; i++) direct[i] = -1;
	for(uint32_t i = 0; i < pairs; i++) {
		uint16_t sidx = ctx->array3[i];
		if(sidx < n)
			direct[sidx] = (int32_t)ctx->a3_set[i];
	}

	/* Forward-fill: samples with no direct assignment inherit the previous. */
	int32_t cur = -1;
	for(uint32_t i = 0; i < n; i++) {
		if(direct[i] >= 0) cur = direct[i];
		ctx->sample_set_idx[i] = cur;
	}
	free(direct);
	return true;
}

/* ── Instrument zone construction ────────────────────────────────────────── */

/* Append one zone (sample + key range + articulation) to an instrument. */
static void append_zone(SS_BasicInstrument *inst, SS_SoundBank *bank,
                         uint32_t sample_idx, int key_lo, int key_hi,
                         int loop_mode, const ECW_Patch *patch,
                         int8_t amplitude, int8_t pan, int8_t coarse,
                         int8_t fine, uint16_t delay_ms, uint8_t excl) {
	if(sample_idx >= bank->sample_count) return;
	if(key_lo > key_hi) return;

	SS_InstrumentZone *tmp = (SS_InstrumentZone *)realloc(
	inst->zones, (inst->zone_count + 1) * sizeof(SS_InstrumentZone));
	if(!tmp) return;
	inst->zones = tmp;
	SS_InstrumentZone *iz = &inst->zones[inst->zone_count];
	memset(iz, 0, sizeof(*iz));

	iz->base.key_range_min = (int8_t)clampi(key_lo, 0, 127);
	iz->base.key_range_max = (int8_t)clampi(key_hi, 0, 127);
	iz->base.vel_range_min = -1;
	iz->base.vel_range_max = 127;

	/* Per-zone sample copy: shares audio_file / mutex with the bank sample
	 * (owns_raw_data = false guards against double-free). */
	iz->sample = (SS_BasicSample *)malloc(sizeof(SS_BasicSample));
	if(!iz->sample) return;
	*iz->sample = bank->samples[sample_idx];
	iz->sample->owns_raw_data = false;

	zone_set_gen(&iz->base, SS_GEN_SAMPLE_MODES, (int16_t)loop_mode);
	if(patch)
		apply_articulation(&iz->base, patch, amplitude, pan, coarse, fine,
		                   delay_ms, excl);

	inst->zone_count++;
}

/* Append all zones from one patch's sample run, clipped to [key_lo,key_hi]. */
static void append_patch_zones(ECW_Ctx *ctx, SS_BasicInstrument *inst,
                                const ECW_Patch *patch, int key_lo, int key_hi,
                                int8_t amplitude, int8_t pan, int8_t coarse,
                                int8_t fine, uint16_t delay_ms, uint8_t excl) {
	if(patch->array1_index >= ctx->array1_count) return;
	uint16_t a2idx = ctx->array1[patch->array1_index];
	if(a2idx >= ctx->a23_count) return; /* also catches the 0xFFFF "unused" marker */
	int32_t set = (int32_t)ctx->a3_set[a2idx];
	uint32_t first = ctx->array3[a2idx];

	for(uint32_t i = first; i < ctx->sample_count; i++) {
		if(ctx->sample_set_idx[i] != set) break;
		const ECW_Sample *es = &ctx->samples[i];
		int lo = es->low_key, hi = es->high_key;
		if(lo < key_lo) lo = key_lo;
		if(hi > key_hi) hi = key_hi;
		int loop_mode = (es->flags >= 2) ? 1 : 0;
		append_zone(inst, ctx->bank, i, lo, hi, loop_mode, patch,
		            amplitude, pan, coarse, fine, delay_ms, excl);
	}
}

/* Build all zones for one ECW instrument into `inst`, clipped to a key range.
 * Handles type-2 (sub-header layers / splits) and type-255 (cascade). */
static void build_ecw_instrument(ECW_Ctx *ctx, SS_BasicInstrument *inst,
                                 uint32_t ecw_idx, int key_lo, int key_hi,
                                 int depth) {
	if(ecw_idx >= ctx->instr_count || depth > ECW_V2_MAX_DEPTH) return;

	size_t base = ctx->instr_off + (size_t)ecw_idx * ECW_INSTRUMENT_SIZE;
	uint8_t type = ss_file_read_u8(ctx->src, base);

	if(type == 2) {
		/* ecwInstrument_v1: SubType, NoteThreshold, SubHeaders[2] (10 bytes). */
		uint8_t sub_type = ss_file_read_u8(ctx->src, base + 1);
		uint8_t threshold = ss_file_read_u8(ctx->src, base + 2);

		for(int sh = 0; sh < 2; sh++) {
			/* SubType: 0 = SH0 only, 3 = SH1 only, 1 = both, 2 = split. */
			if(sh == 0 && sub_type == 3) continue;
			if(sh == 1 && sub_type == 0) continue;

			int sh_lo = key_lo, sh_hi = key_hi;
			if(sub_type == 2) {
				/* split at NoteThreshold: SH0 <= threshold, SH1 > threshold */
				if(sh == 0 && threshold < sh_hi) sh_hi = threshold;
				if(sh == 1 && threshold + 1 > sh_lo) sh_lo = threshold + 1;
			}
			if(sh_lo > sh_hi) continue;

			size_t sb = base + 3 + (size_t)sh * 10;
			uint16_t patch_idx = ss_file_read_le16(ctx->src, sb + 0);
			int8_t amplitude = (int8_t)ss_file_read_u8(ctx->src, sb + 2);
			int8_t pan = (int8_t)ss_file_read_u8(ctx->src, sb + 3);
			int8_t coarse = (int8_t)ss_file_read_u8(ctx->src, sb + 4);
			int8_t fine = (int8_t)ss_file_read_u8(ctx->src, sb + 5);
			uint16_t delay = ss_file_read_le16(ctx->src, sb + 6);
			uint8_t excl = ss_file_read_u8(ctx->src, sb + 9);

			if(patch_idx >= ctx->patch_count) continue;
			append_patch_zones(ctx, inst, &ctx->patches[patch_idx],
			                   sh_lo, sh_hi, amplitude, pan, coarse, fine,
			                   delay, excl);
		}
	} else if(type == 255) {
		/* ecwInstrument_v2: Unknown, SubHeaders[7] (3 bytes each). */
		int prev_threshold = -1;
		for(int sh = 0; sh < 7; sh++) {
			size_t sb = base + 2 + (size_t)sh * 3;
			uint16_t ref = ss_file_read_le16(ctx->src, sb + 0);
			uint8_t threshold = ss_file_read_u8(ctx->src, sb + 2);

			int lo = prev_threshold + 1;
			int hi = threshold;
			prev_threshold = threshold;
			if(ref == 0 && threshold == 0) continue; /* empty slot */
			if(ref == ecw_idx) continue;             /* self reference */

			int clo = lo > key_lo ? lo : key_lo;
			int chi = hi < key_hi ? hi : key_hi;
			if(clo > chi) continue;
			build_ecw_instrument(ctx, inst, ref, clo, chi, depth + 1);
		}
	}
	/* other type values: leave the instrument empty */
}

/* ── Preset construction ─────────────────────────────────────────────────── */

static void make_preset_zone(SS_BasicPreset *p, SS_BasicInstrument *inst) {
	p->zones = (SS_PresetZone *)calloc(1, sizeof(SS_PresetZone));
	if(!p->zones) return;
	p->zone_count = 1;
	p->zones[0].base.key_range_min = -1;
	p->zones[0].base.key_range_max = 127;
	p->zones[0].base.vel_range_min = -1;
	p->zones[0].base.vel_range_max = 127;
	p->zones[0].instrument = inst;
}

/* ── Loader entry point ──────────────────────────────────────────────────── */

SS_SoundBank *ss_ecw_load(SS_File *file) {
	SS_SoundBank *bank = NULL;
	SS_File *sample_blob = NULL;
	uint16_t *array2 = NULL;
	ECW_Ctx ctx;
	memset(&ctx, 0, sizeof(ctx));
	ctx.src = file;

	size_t file_size = ss_file_size(file);
	if(file_size < ECW_HEADER_SIZE) return NULL;

	/* Validate the "ECLW" magic. */
	char magic[5];
	ss_file_read_string(file, ECW_OFF_ID, magic, 4);
	if(strcmp(magic, "ECLW") != 0) return NULL;

	bank = ss_soundbank_new();
	if(!bank) return NULL;
	ctx.bank = bank;

	/* ── Header section triples ─────────────────────────────────────────── */
	uint32_t sample_off = 0, sample_size = 0, sample_count = 0;
	uint32_t patch_size = 0, instr_size = 0;
	uint32_t a1_off = 0, a1_size = 0, a1_count = 0;
	uint32_t a2_off = 0, a2_size = 0, a2_count = 0;
	uint32_t a3_off = 0, a3_size = 0, a3_count = 0;
	uint32_t blob_off = 0, blob_size = 0;
	uint32_t bm_size = 0, kitsel_size = 0;

	if(!read_triple(file, ECW_OFF_BANKMAP, file_size, &ctx.bankmap_off, &bm_size, NULL) ||
	   !read_triple(file, ECW_OFF_DRUMNOTEMAP, file_size, &ctx.kitsel_off, &kitsel_size, NULL) ||
	   !read_triple(file, ECW_OFF_MIDIPATCHMAP, file_size, &ctx.midipatchmap_off, NULL, &ctx.midipatchmap_count) ||
	   !read_triple(file, ECW_OFF_DRUMKITMAP, file_size, &ctx.notemap_off, NULL, &ctx.notemap_count) ||
	   !read_triple(file, ECW_OFF_INSTRUMENTS, file_size, &ctx.instr_off, &instr_size, &ctx.instr_count) ||
	   !read_triple(file, ECW_OFF_PATCHES, file_size, &ctx.patch_off, &patch_size, &ctx.patch_count) ||
	   !read_triple(file, ECW_OFF_ARRAY1, file_size, &a1_off, &a1_size, &a1_count) ||
	   !read_triple(file, ECW_OFF_ARRAY2, file_size, &a2_off, &a2_size, &a2_count) ||
	   !read_triple(file, ECW_OFF_ARRAY3, file_size, &a3_off, &a3_size, &a3_count) ||
	   !read_triple(file, ECW_OFF_SAMPLEHEADERS, file_size, &sample_off, &sample_size, &sample_count))
		goto fail;

	blob_off = ss_file_read_le32(file, ECW_OFF_SAMPLEDATA_OFFS);
	blob_size = ss_file_read_le32(file, ECW_OFF_SAMPLEDATA_SIZE);
	if((size_t)blob_off + blob_size > file_size) goto fail;

	/* Sanity: section element strides must match the triple sizes. */
	if(instr_size < ctx.instr_count * ECW_INSTRUMENT_SIZE) ctx.instr_count = 0;
	if(patch_size < ctx.patch_count * ECW_PATCH_SIZE) ctx.patch_count = 0;
	if(sample_size < sample_count * ECW_SAMPLE_SIZE) sample_count = 0;
	ctx.sample_count = sample_count;
	ctx.array1_count = a1_count;
	if(a1_size < a1_count * 2 || a2_size < a2_count * 2 || a3_size < a3_count * 2)
		goto fail;

	/* ── Patches ────────────────────────────────────────────────────────── */
	if(ctx.patch_count > 0) {
		ctx.patches = (ECW_Patch *)calloc(ctx.patch_count, sizeof(ECW_Patch));
		if(!ctx.patches) goto fail;
		for(uint32_t i = 0; i < ctx.patch_count; i++)
			decode_patch(file, ctx.patch_off + (size_t)i * ECW_PATCH_SIZE,
			             &ctx.patches[i]);
	}

	/* ── Sample headers ─────────────────────────────────────────────────── */
	if(ctx.sample_count > 0) {
		ctx.samples = (ECW_Sample *)calloc(ctx.sample_count, sizeof(ECW_Sample));
		if(!ctx.samples) goto fail;
		uint8_t high_key = 0;
		for(uint32_t i = 0; i < ctx.sample_count; i++) {
			size_t o = sample_off + (size_t)i * ECW_SAMPLE_SIZE;
			ECW_Sample *es = &ctx.samples[i];
			es->split_point = ss_file_read_u8(file, o + 0);
			es->flags = ss_file_read_u8(file, o + 1);
			es->fine_tune = (int8_t)ss_file_read_u8(file, o + 2);
			es->coarse_tune = (int8_t)ss_file_read_u8(file, o + 3);
			es->sample_start = ss_file_read_le32(file, o + 4) / 8;
			es->loop_start = ss_file_read_le32(file, o + 8) / 8;
			es->loop_end = ss_file_read_le32(file, o + 12) / 8;
			es->low_key = (high_key == 127) ? 0 : (uint8_t)(high_key + 1);
			es->high_key = es->split_point;
			high_key = es->split_point;
		}
	}

	/* ── Sample sets (embedded in the sample-data blob) ─────────────────── */
	if(blob_size >= 40) {
		uint32_t sc = ss_file_read_le16(file, blob_off + 22);
		size_t table = (size_t)blob_off + 40;
		if(table + (size_t)sc * ECW_SAMPLESET_SIZE <= file_size && sc > 0) {
			ctx.sets = (ECW_SampleSet *)calloc(sc, sizeof(ECW_SampleSet));
			if(!ctx.sets) goto fail;
			ctx.set_count = sc;
			for(uint32_t k = 0; k < sc; k++) {
				size_t o = table + (size_t)k * ECW_SAMPLESET_SIZE;
				ECW_SampleSet *ss = &ctx.sets[k];
				ss->sample_index = ss_file_read_le32(file, o + 0);
				ss->array1_index = ss_file_read_le16(file, o + 4);
				ss->code = ss_file_read_le16(file, o + 6);
				ecw_read_text(file, o + 8, 14, ss->name, sizeof(ss->name));
			}
		}
	}

	/* ── Index arrays ───────────────────────────────────────────────────── */
	ctx.a23_count = a2_count < a3_count ? a2_count : a3_count;
	if(a1_count > 0) {
		ctx.array1 = (uint16_t *)malloc(a1_count * sizeof(uint16_t));
		if(!ctx.array1) goto fail;
		for(uint32_t i = 0; i < a1_count; i++)
			ctx.array1[i] = ss_file_read_le16(file, a1_off + (size_t)i * 2);
	}
	if(a2_count > 0) {
		array2 = (uint16_t *)malloc(a2_count * sizeof(uint16_t));
		if(!array2) goto fail;
		for(uint32_t i = 0; i < a2_count; i++)
			array2[i] = ss_file_read_le16(file, a2_off + (size_t)i * 2);
	}
	if(a3_count > 0) {
		ctx.array3 = (uint16_t *)malloc(a3_count * sizeof(uint16_t));
		if(!ctx.array3) goto fail;
		for(uint32_t i = 0; i < a3_count; i++)
			ctx.array3[i] = ss_file_read_le16(file, a3_off + (size_t)i * 2);
	}

	/* ── Resolve which sample set each sample belongs to ────────────────── */
	if(ctx.sample_count > 0) {
		ctx.sample_set_idx = (int32_t *)malloc(ctx.sample_count * sizeof(int32_t));
		if(!ctx.sample_set_idx) goto fail;
	}
	if(!resolve_sample_sets(&ctx, array2)) goto fail;

	/* ── Samples ────────────────────────────────────────────────────────── */
	sample_blob = ss_file_slice(file, blob_off, blob_size);
	if(!sample_blob) goto fail;
	if(!build_samples(&ctx, sample_blob, blob_size)) goto fail;

	/* ── Decide which drum kits to emit (one synthetic instrument each) ──── */
	uint8_t *emit_kit = (uint8_t *)calloc(128, 1);
	uint16_t *kit_notemap = (uint16_t *)calloc(128, sizeof(uint16_t));
	uint32_t drum_kit_count = 0;
	if(emit_kit && kit_notemap && ctx.notemap_count > 0 && kitsel_size >= ECW_MAP_SIZE) {
		uint16_t base_nm = ss_file_read_le16(file, ctx.kitsel_off);
		for(int k = 0; k < 128; k++) {
			uint16_t nm = ss_file_read_le16(file, ctx.kitsel_off + (size_t)k * 2);
			if(nm >= ctx.notemap_count) continue;
			if(k == 0 || nm != base_nm) {
				emit_kit[k] = 1;
				kit_notemap[k] = nm;
				drum_kit_count++;
			}
		}
	}

	/* ── Allocate the instrument array (per-ECW + synthetic drum kits) ──── */
	size_t total_instr = ctx.instr_count + drum_kit_count;
	if(total_instr > 0) {
		bank->instruments = (SS_BasicInstrument *)calloc(total_instr,
		                                                 sizeof(SS_BasicInstrument));
		if(!bank->instruments) {
			free(emit_kit);
			free(kit_notemap);
			goto fail;
		}
		bank->instrument_count = total_instr;
	}

	/* ── Build one melodic instrument per ECW instrument ────────────────── */
	for(uint32_t i = 0; i < ctx.instr_count; i++) {
		SS_BasicInstrument *inst = &bank->instruments[i];
		build_ecw_instrument(&ctx, inst, i, 0, 127, 0);
		if(inst->zone_count > 0 && inst->zones[0].sample)
			snprintf(inst->name, sizeof(inst->name), "%.40s",
			         inst->zones[0].sample->name);
		else
			snprintf(inst->name, sizeof(inst->name), "Instrument%u", i);
	}

	/* ── Build one synthetic instrument per emitted drum kit ────────────── */
	uint32_t di = 0;
	int8_t drum_kit_for_index[128];
	memset(drum_kit_for_index, -1, sizeof(drum_kit_for_index));
	for(int k = 0; k < 128 && emit_kit; k++) {
		if(!emit_kit[k]) continue;
		uint32_t inst_index = ctx.instr_count + di;
		SS_BasicInstrument *inst = &bank->instruments[inst_index];
		uint16_t nm = kit_notemap[k];
		size_t nm_base = ctx.notemap_off + (size_t)nm * ECW_MAP_SIZE;
		for(int note = 0; note < 128; note++) {
			uint16_t ecw_inst = ss_file_read_le16(file, nm_base + (size_t)note * 2);
			if(ecw_inst >= ctx.instr_count) continue;
			build_ecw_instrument(&ctx, inst, ecw_inst, note, note, 0);
		}
		snprintf(inst->name, sizeof(inst->name), "Drum Kit %d", k);
		drum_kit_for_index[k] = (int8_t)di;
		di++;
	}

	/* ── Count and build presets ────────────────────────────────────────── */
	{
		/* Melodic banks: emit bank 0, plus any bank whose patch map differs. */
		uint8_t emit_bank[128];
		memset(emit_bank, 0, sizeof(emit_bank));
		size_t preset_cap = 0;
		if(bm_size >= ECW_MAP_SIZE && ctx.midipatchmap_count > 0) {
			uint16_t base_pm = ss_file_read_le16(file, ctx.bankmap_off);
			for(int b = 0; b < 128; b++) {
				uint16_t pm = ss_file_read_le16(file, ctx.bankmap_off + (size_t)b * 2);
				if(pm >= ctx.midipatchmap_count) continue;
				if(b == 0 || pm != base_pm) {
					emit_bank[b] = 1;
					preset_cap += 128;
				}
			}
		}
		preset_cap += drum_kit_count;

		if(preset_cap > 0) {
			bank->presets = (SS_BasicPreset *)calloc(preset_cap,
			                                         sizeof(SS_BasicPreset));
			if(!bank->presets) {
				free(emit_kit);
				free(kit_notemap);
				goto fail;
			}
		}
		size_t pc = 0;

		/* Melodic presets. */
		for(int b = 0; b < 128; b++) {
			if(!emit_bank[b]) continue;
			uint16_t pm = ss_file_read_le16(file, ctx.bankmap_off + (size_t)b * 2);
			size_t pm_base = ctx.midipatchmap_off + (size_t)pm * ECW_MAP_SIZE;
			for(int prog = 0; prog < 128; prog++) {
				uint16_t ei = ss_file_read_le16(file, pm_base + (size_t)prog * 2);
				if(ei >= ctx.instr_count) continue;
				if(bank->instruments[ei].zone_count == 0) continue;
				SS_BasicPreset *p = &bank->presets[pc++];
				snprintf(p->name, sizeof(p->name), "%.40s",
				         bank->instruments[ei].name);
				p->program = (uint8_t)prog;
				p->bank_msb = (uint8_t)b;
				p->bank_lsb = 0;
				p->parent_bank = bank;
				make_preset_zone(p, &bank->instruments[ei]);
			}
		}

		/* Drum presets. */
		for(int k = 0; k < 128 && emit_kit; k++) {
			if(!emit_kit[k]) continue;
			int8_t didx = drum_kit_for_index[k];
			if(didx < 0) continue;
			SS_BasicInstrument *inst = &bank->instruments[ctx.instr_count + didx];
			if(inst->zone_count == 0) continue;
			SS_BasicPreset *p = &bank->presets[pc++];
			snprintf(p->name, sizeof(p->name), "Drum Kit %d", k);
			p->program = (uint8_t)k;
			p->bank_msb = ECW_DRUM_BANK_MSB;
			p->bank_lsb = 0;
			p->is_gm_gs_drum = true;
			p->parent_bank = bank;
			make_preset_zone(p, inst);
		}

		bank->preset_count = pc;
	}

	free(emit_kit);
	free(kit_notemap);

	/* ── Bank metadata ──────────────────────────────────────────────────── */
	ecw_read_text(file, ECW_OFF_NAME, 80, bank->name, sizeof(bank->name));
	ecw_read_text(file, ECW_OFF_FILENAME, 256, bank->software, sizeof(bank->software));
	strncpy(bank->sound_engine, "E-mu ECW", sizeof(bank->sound_engine) - 1);

	/* ── Cleanup temporaries ────────────────────────────────────────────── */
	ss_file_close(sample_blob);
	free(ctx.patches);
	free(ctx.samples);
	free(ctx.sets);
	free(ctx.array1);
	free(ctx.array3);
	free(ctx.a3_set);
	free(array2);
	free(ctx.sample_set_idx);
	return bank;

fail:
	if(sample_blob) ss_file_close(sample_blob);
	free(ctx.patches);
	free(ctx.samples);
	free(ctx.sets);
	free(ctx.array1);
	free(ctx.array3);
	free(ctx.a3_set);
	free(array2);
	free(ctx.sample_set_idx);
	ss_soundbank_free(bank);
	return NULL;
}
