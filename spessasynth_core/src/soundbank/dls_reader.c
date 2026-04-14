/**
 * dls_reader.c
 * DLS (Downloadable Sounds) Level 1 & 2 loader.
 *
 * DLS spec: https://www.midi.org/specifications-old/item/dls-level-1-specification
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if __has_include(<spessasynth_core/spessasynth.h>)
#include <spessasynth_core/file.h>
#include <spessasynth_core/riff_chunk.h>
#include <spessasynth_core/soundbank.h>
#else
#include "spessasynth/soundbank/soundbank.h"
#include "spessasynth/utils/file.h"
#include "spessasynth/utils/riff_chunk.h"
#endif

/*
 * DLS uses RIFF with "DLS " fourcc.
 * Structure:
 *   RIFF DLS
 *     LIST lins   <- instruments
 *       LIST ins  <- single instrument
 *         insh    <- instrument header
 *         LIST lrgn  <- regions
 *           LIST rgn/rgn2  <- single region
 *             rgnh    <- region header
 *             wsmp    <- wave sample (loop / tuning info)
 *             wlnk    <- wave link (sample reference)
 *             LIST lart/lar2  <- per-region articulation
 *         LIST lart/lar2  <- per-instrument (global) articulation
 *     LIST wvpl   <- wave pool
 *       LIST wave  <- single PCM wave (RIFF WAVE inside DLS)
 *     ptbl        <- pool table (maps wlnk index to wave pool position)
 *     dlid        <- DLS unique identifier (optional)
 *     colh        <- collection header (preset / wave counts)
 */

/* ── Region header ───────────────────────────────────────────────────────── */
typedef struct {
	uint16_t key_low, key_high;
	uint16_t vel_low, vel_high;
	uint16_t options;
	uint16_t key_group;
} DLS_RegionHeader;

/* ── Wave sample (wsmp chunk) ────────────────────────────────────────────── */
typedef struct {
	size_t cbSize;
	uint16_t unity_note;
	int16_t fine_tune; /* cents */
	int32_t gain; /* 32-bit: each unit = 1/655360 dB (DLS2), or millibels (DLS1) */
	uint32_t options;
	uint32_t loop_count;
	/* loop record (one entry): */
	uint32_t loop_size;
	uint32_t loop_type;
	uint32_t loop_start;
	uint32_t loop_length;
} DLS_WaveSample;

static bool parse_wsmp(SS_File *file, DLS_WaveSample *ws, bool riff64) {
	const size_t size_size = riff64 ? 8 : 4;
	size_t pos = ss_file_tell(file);
	ws->cbSize = ss_file_read_le(file, pos, size_size);
	pos += size_size;
	ws->unity_note = (uint16_t)ss_file_read_le(file, pos, 2);
	ws->fine_tune = (int16_t)ss_file_read_le(file, pos + 2, 2);
	ws->gain = (int32_t)ss_file_read_le(file, pos + 4, 4);
	ws->options = (uint32_t)ss_file_read_le(file, pos + 8, 4);
	ws->loop_count = (uint32_t)ss_file_read_le(file, pos + 12, 4);
	ws->loop_size = 0;
	ws->loop_type = 0;
	ws->loop_start = 0;
	ws->loop_length = 0;
	if(ws->loop_count > 0) {
		ws->loop_size = (uint32_t)ss_file_read_le(file, pos + 16, 4);
		ws->loop_type = (uint32_t)ss_file_read_le(file, pos + 20, 4);
		ws->loop_start = (uint32_t)ss_file_read_le(file, pos + 24, 4);
		ws->loop_length = (uint32_t)ss_file_read_le(file, pos + 28, 4);
	}
	return true;
}

/* ── DLS connection block ────────────────────────────────────────────────── */
typedef struct {
	uint16_t us_source;
	uint16_t us_control;
	uint16_t us_dest;
	int32_t l_scale;
	/* Decoded from usTransform */
	SS_DLSTransform output_transform;
	SS_DLSTransform control_transform;
	bool control_bipolar;
	bool control_invert;
	SS_DLSTransform source_transform;
	bool source_bipolar;
	bool source_invert;
} DLS_ConnBlock;

/* ── Articulation buffer ─────────────────────────────────────────────────── */
typedef struct {
	DLS_ConnBlock *blocks;
	size_t count;
	size_t cap;
	bool dls1; /* true = lart/art1 (DLS Level 1) */
} DLS_ArtBuf;

static void art_buf_init(DLS_ArtBuf *a) {
	memset(a, 0, sizeof(*a));
}
static void art_buf_free(DLS_ArtBuf *a) {
	free(a->blocks);
}

static bool art_buf_push(DLS_ArtBuf *a, const DLS_ConnBlock *b) {
	if(a->count >= a->cap) {
		size_t nc = a->cap ? a->cap * 2 : 8;
		DLS_ConnBlock *t = (DLS_ConnBlock *)realloc(a->blocks, nc * sizeof(DLS_ConnBlock));
		if(!t) return false;
		a->blocks = t;
		a->cap = nc;
	}
	a->blocks[a->count++] = *b;
	return true;
}

/* Parse the body of an art1/art2 chunk into the buffer. */
static void parse_art_body(SS_File *file, DLS_ArtBuf *art, bool riff64) {
	const size_t size_size = riff64 ? 8 : 4;
	/*ss_iba_read_le(data, size_size); cbSize */
	size_t pos = ss_file_tell(file);
	pos += size_size; /* cbSize */
	uint32_t n = (uint32_t)ss_file_read_le(file, pos, 4);
	pos += 4;
	for(uint32_t i = 0; i < n; i++) {
		if(ss_file_remaining(file) < 12) break;
		DLS_ConnBlock b;
		b.us_source = (uint16_t)ss_file_read_le(file, pos, 2);
		b.us_control = (uint16_t)ss_file_read_le(file, pos + 2, 2);
		b.us_dest = (uint16_t)ss_file_read_le(file, pos + 4, 2);
		uint16_t t = (uint16_t)ss_file_read_le(file, pos + 6, 2);
		b.l_scale = (int32_t)ss_file_read_le(file, pos + 8, 4);
		pos += 12;

		/*
		 * usTransform bit layout (DLS2 spec §2.10):
		 *   Bits  0- 3: output transform
		 *   Bits  4- 7: control transform
		 *   Bit      8: control bipolar
		 *   Bit      9: control invert
		 *   Bits 10-13: source transform
		 *   Bit     14: source bipolar
		 *   Bit     15: source invert
		 */
		b.output_transform = (SS_DLSTransform)((t) & 0x0f);
		b.control_transform = (SS_DLSTransform)((t >> 4) & 0x0f);
		b.control_bipolar = (t & (1u << 8)) != 0;
		b.control_invert = (t & (1u << 9)) != 0;
		b.source_transform = (SS_DLSTransform)((t >> 10) & 0x0f);
		b.source_bipolar = (t & (1u << 14)) != 0;
		b.source_invert = (t & (1u << 15)) != 0;
		art_buf_push(art, &b);
	}
}

/* Scan an lart/lar2 list body for art1/art2 chunks and parse them. */
static void parse_lart_body(SS_File *list_file, DLS_ArtBuf *art, bool riff64) {
	const size_t size_size = riff64 ? 8 : 4;
	while(ss_file_remaining(list_file) >= 4 + size_size) {
		SS_RIFFChunk c;
		if(!ss_riff_read_chunk(list_file, &c, false, riff64)) break;
		if(strcmp(c.header, "art1") == 0 || strcmp(c.header, "art2") == 0)
			parse_art_body(c.file, art, riff64);
		ss_riff_close_chunk(&c);
	}
}

/* ── Per-zone extra data (resolved after sample pool is ready) ────────────── */
typedef struct {
	/* wsmp fields deferred until sample pointers are resolved */
	int16_t wsmp_fine_tune;
	bool has_wsmp_fine_tune;
	int32_t wsmp_gain; /* raw 32-bit gain/attenuation field */
	bool has_wsmp_gain;
	uint32_t wsmp_loop_start;
	uint32_t wsmp_loop_length;
	uint32_t wsmp_loop_type;
	bool has_wsmp_loop;
	uint16_t wsmp_unity_note;
	bool has_wsmp_unity_note;
	/* Articulation for this region */
	DLS_ArtBuf art;
} DLS_ZoneExtra;

static void zone_extra_init(DLS_ZoneExtra *e) {
	memset(e, 0, sizeof(*e));
}
static void zone_extra_free(DLS_ZoneExtra *e) {
	art_buf_free(&e->art);
}

/* ── Zone generator / modulator helpers ──────────────────────────────────── */

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
	SS_Generator *tmp = (SS_Generator *)realloc(z->generators,
	                                            (z->gen_count + 1) * sizeof(SS_Generator));
	if(!tmp) return;
	z->generators = tmp;
	z->generators[z->gen_count].type = t;
	z->generators[z->gen_count].value = v;
	z->gen_count++;
}

static void zone_add_to_gen(SS_Zone *z, SS_GeneratorType t, int16_t delta) {
	zone_set_gen(z, t, (int16_t)(zone_get_gen(z, t, 0) + (int16_t)delta));
}

/* Pack an SF2 modulator source word:
 *   bits 15-10: curve type
 *   bit      9: bipolar
 *   bit      8: negative (invert)
 *   bit      7: is_cc
 *   bits  6- 0: source index
 */
static uint16_t make_modsrc(SS_ModulatorCurveType curve, bool bipolar,
                            bool negative, bool is_cc, uint8_t idx) {
	return (uint16_t)(((uint16_t)curve << 10) |
	                  ((bipolar ? 1u : 0u) << 9) |
	                  ((negative ? 1u : 0u) << 8) |
	                  ((is_cc ? 1u : 0u) << 7) |
	                  idx);
}

static void zone_add_mod(SS_Zone *z, uint16_t src, uint16_t ctrl,
                         SS_GeneratorType dest, int16_t amount) {
	SS_Modulator *tmp = (SS_Modulator *)realloc(z->modulators,
	                                            (z->mod_count + 1) * sizeof(SS_Modulator));
	if(!tmp) return;
	z->modulators = tmp;
	SS_Modulator *m = &z->modulators[z->mod_count++];
	memset(m, 0, sizeof(*m));
	m->source_enum = src;
	m->amount_source_enum = ctrl;
	m->dest_enum = (uint16_t)dest;
	m->transform_amount = amount;
}

/* ── DLS source → SF2 modulator source word ─────────────────────────────── */

/* Returns 0xFFFF if the source cannot be converted. */
static uint16_t dls_src_to_sf2(uint16_t us_src, SS_DLSTransform transform,
                               bool bipolar, bool invert) {
	bool is_cc = false;
	uint8_t idx = 0;

	switch((SS_DLSSource)us_src) {
		case SS_DLSSRC_NONE:
			idx = SS_MODSRC_NO_CONTROLLER;
			break;
		case SS_DLSSRC_VELOCITOY:
			idx = SS_MODSRC_NOTE_ON_VELOCITY;
			break;
		case SS_DLSSRC_KEYNUM:
			idx = SS_MODSRC_NOTE_ON_KEYNUM;
			break;
		case SS_DLSSRC_PITCH_WHEEL:
			idx = SS_MODSRC_PITCH_WHEEL;
			break;
		case SS_DLSSRC_PITCH_WHEEL_RANGE:
			idx = SS_MODSRC_PITCH_WHEEL_RANGE;
			break;
		case SS_DLSSRC_POLY_PRESSURE:
			idx = SS_MODSRC_POLY_PRESSURE;
			break;
		case SS_DLSSRC_CHANNEL_PRESSURE:
			idx = SS_MODSRC_CHANNEL_PRESSURE;
			break;
		case SS_DLSSRC_MODULATION_WHEEL:
			idx = 1;
			is_cc = true;
			break;
		case SS_DLSSRC_VOLUME:
			idx = 7;
			is_cc = true;
			break;
		case SS_DLSSRC_PAN:
			idx = 10;
			is_cc = true;
			break;
		case SS_DLSSRC_EXPRESSION:
			idx = 11;
			is_cc = true;
			break;
		case SS_DLSSRC_CHORUS:
			idx = 93;
			is_cc = true;
			break;
		case SS_DLSSRC_REVERB:
			idx = 91;
			is_cc = true;
			break;
		default:
			return 0xFFFF;
	}
	return make_modsrc((SS_ModulatorCurveType)transform, bipolar, invert, is_cc, idx);
}

/* ── Combined SF2 generator for DLS source+destination pairs ─────────────── */

/* Some DLS combinations map to a single SF2 generator rather than a modulator.
 * Returns SS_GEN_INVALID if no combined mapping exists. */
static SS_GeneratorType dls_combined_gen(uint16_t us_src, uint16_t us_dest) {
	SS_DLSSource src = (SS_DLSSource)us_src;
	SS_DLSDestination dest = (SS_DLSDestination)us_dest;
	if(src == SS_DLSSRC_VIBRATO_LFO && dest == SS_DLSDEST_PITCH) return SS_GEN_VIB_LFO_TO_PITCH;
	if(src == SS_DLSSRC_MODLFO && dest == SS_DLSDEST_PITCH) return SS_GEN_MOD_LFO_TO_PITCH;
	if(src == SS_DLSSRC_MODLFO && dest == SS_DLSDEST_FILTER_CUTOFF) return SS_GEN_MOD_LFO_TO_FILTER_FC;
	if(src == SS_DLSSRC_MODLFO && dest == SS_DLSDEST_GAIN) return SS_GEN_MOD_LFO_TO_VOLUME;
	if(src == SS_DLSSRC_MODENV && dest == SS_DLSDEST_FILTER_CUTOFF) return SS_GEN_MOD_ENV_TO_FILTER_FC;
	if(src == SS_DLSSRC_MODENV && dest == SS_DLSDEST_PITCH) return SS_GEN_MOD_ENV_TO_PITCH;
	return SS_GEN_INVALID;
}

/* ── Apply DLS connection blocks to an SF2 zone ──────────────────────────── */

static void apply_conn_blocks(SS_Zone *zone, const DLS_ConnBlock *blocks,
                              size_t count, bool dls1) {
	/* ── Pass 1: static parameters (source=none, control=none) → generators  */
	for(size_t i = 0; i < count; i++) {
		const DLS_ConnBlock *b = &blocks[i];
		if(b->us_source != SS_DLSSRC_NONE || b->us_control != SS_DLSSRC_NONE)
			continue;

		int16_t value = (int16_t)(b->l_scale >> 16);

		switch((SS_DLSDestination)b->us_dest) {
			case SS_DLSDEST_GAIN:
				/* E-MU correction: initialAttenuation (cB) = -(gain16) / 0.4 */
				zone_add_to_gen(zone, SS_GEN_INITIAL_ATTENUATION,
				                (int16_t)(-(float)value / 0.4f));
				break;
			case SS_DLSDEST_PAN:
				zone_set_gen(zone, SS_GEN_PAN, value);
				break;
			case SS_DLSDEST_FILTER_CUTOFF:
				zone_set_gen(zone, SS_GEN_INITIAL_FILTER_FC, value);
				break;
			case SS_DLSDEST_FILTER_Q:
				zone_set_gen(zone, SS_GEN_INITIAL_FILTER_Q, value);
				break;
			case SS_DLSDEST_MOD_LFO_FREQ:
				zone_set_gen(zone, SS_GEN_FREQ_MOD_LFO, value);
				break;
			case SS_DLSDEST_MOD_LFO_DELAY:
				zone_set_gen(zone, SS_GEN_DELAY_MOD_LFO, value);
				break;
			case SS_DLSDEST_VIB_LFO_FREQ:
				zone_set_gen(zone, SS_GEN_FREQ_VIB_LFO, value);
				break;
			case SS_DLSDEST_VIB_LFO_DELAY:
				zone_set_gen(zone, SS_GEN_DELAY_VIB_LFO, value);
				break;
			/* Volume envelope */
			case SS_DLSDEST_VOL_ENV_DELAY:
				zone_set_gen(zone, SS_GEN_DELAY_VOL_ENV, value);
				break;
			case SS_DLSDEST_VOL_ENV_ATTACK:
				zone_set_gen(zone, SS_GEN_ATTACK_VOL_ENV, value);
				break;
			case SS_DLSDEST_VOL_ENV_HOLD:
				zone_set_gen(zone, SS_GEN_HOLD_VOL_ENV, value);
				break;
			case SS_DLSDEST_VOL_ENV_DECAY:
				zone_set_gen(zone, SS_GEN_DECAY_VOL_ENV, value);
				break;
			case SS_DLSDEST_VOL_ENV_RELEASE:
				zone_set_gen(zone, SS_GEN_RELEASE_VOL_ENV, value);
				break;
			case SS_DLSDEST_VOL_ENV_SUSTAIN:
				/* DLS sustain is gain-at-sustain (0 = silence, 1000 = full).
				 * SF2 sustainVolEnv is attenuation in centibels. */
				zone_set_gen(zone, SS_GEN_SUSTAIN_VOL_ENV, (int16_t)(1000 - value));
				break;
			/* Modulation envelope */
			case SS_DLSDEST_MOD_ENV_DELAY:
				zone_set_gen(zone, SS_GEN_DELAY_MOD_ENV, value);
				break;
			case SS_DLSDEST_MOD_ENV_ATTACK:
				zone_set_gen(zone, SS_GEN_ATTACK_MOD_ENV, value);
				break;
			case SS_DLSDEST_MOD_ENV_HOLD:
				zone_set_gen(zone, SS_GEN_HOLD_MOD_ENV, value);
				break;
			case SS_DLSDEST_MOD_ENV_DECAY:
				zone_set_gen(zone, SS_GEN_DECAY_MOD_ENV, value);
				break;
			case SS_DLSDEST_MOD_ENV_RELEASE:
				zone_set_gen(zone, SS_GEN_RELEASE_MOD_ENV, value);
				break;
			case SS_DLSDEST_MOD_ENV_SUSTAIN:
				zone_set_gen(zone, SS_GEN_SUSTAIN_MOD_ENV, (int16_t)(1000 - value));
				break;
			case SS_DLSDEST_REVERB_SEND:
				zone_set_gen(zone, SS_GEN_REVERB_EFFECTS_SEND, value);
				break;
			case SS_DLSDEST_CHORUS_SEND:
				zone_set_gen(zone, SS_GEN_CHORUS_EFFECTS_SEND, value);
				break;
			case SS_DLSDEST_PITCH:
				/* Static pitch offset → SF2 fine tune */
				zone_add_to_gen(zone, SS_GEN_FINE_TUNE, value);
				break;
			default:
				break;
		}
	}

	/* ── Pass 2: non-static blocks ──────────────────────────────────────── */
	for(size_t i = 0; i < count; i++) {
		const DLS_ConnBlock *b = &blocks[i];
		if(b->us_source == SS_DLSSRC_NONE && b->us_control == SS_DLSSRC_NONE)
			continue;

		int16_t value = (int16_t)(b->l_scale >> 16);
		SS_DLSSource src = (SS_DLSSource)b->us_source;
		SS_DLSSource ctrl = (SS_DLSSource)b->us_control;
		SS_DLSDestination dest = (SS_DLSDestination)b->us_dest;

		if(ctrl == SS_DLSSRC_NONE) {
			/* Source-only (no secondary controller) */
			if(src == SS_DLSSRC_KEYNUM) {
				if(dest == SS_DLSDEST_PITCH) {
					/* keyNum → pitch = scale tuning */
					zone_set_gen(zone, SS_GEN_SCALE_TUNING, (int16_t)(value / 128));
					continue;
				}
				/* keyNum → env hold/decay are deferred to pass 3 */
				if(dest == SS_DLSDEST_VOL_ENV_HOLD || dest == SS_DLSDEST_VOL_ENV_DECAY ||
				   dest == SS_DLSDEST_MOD_ENV_HOLD || dest == SS_DLSDEST_MOD_ENV_DECAY)
					continue;
			}

			/* Check for combined SF2 generator (LFO/env source + specific dest) */
			SS_GeneratorType combined = dls_combined_gen(b->us_source, b->us_dest);
			if(combined != SS_GEN_INVALID) {
				zone_set_gen(zone, combined, value);
				continue;
			}
		} else {
			/* Both source and control are set.
			 * DLS: signal-source (LFO/env) + CC-control → dest
			 * SF2: CC-control (primary) → combined-generator
			 *
			 * Example: modWheel (control) + modLFO (source) → pitch
			 *       →  SF2: modWheel → modLfoToPitch  */
			SS_GeneratorType combined = dls_combined_gen(b->us_source, b->us_dest);
			if(combined != SS_GEN_INVALID) {
				uint16_t ctrl_sf = dls_src_to_sf2(b->us_control,
				                                  b->control_transform,
				                                  b->control_bipolar,
				                                  b->control_invert);
				if(ctrl_sf == 0xFFFF) continue;
				uint16_t no_ctrl = make_modsrc(SS_MODCURVE_LINEAR, false, false, false,
				                               SS_MODSRC_NO_CONTROLLER);
				zone_add_mod(zone, ctrl_sf, no_ctrl, combined, value);
				continue;
			}
		}

		/* ── Regular modulator ─────────────────────────────────────────── */
		uint16_t sf_src = dls_src_to_sf2(b->us_source,
		                                 b->source_transform,
		                                 b->source_bipolar,
		                                 b->source_invert);
		if(sf_src == 0xFFFF) continue;

		uint16_t sf_ctrl = dls_src_to_sf2(b->us_control,
		                                  b->control_transform,
		                                  b->control_bipolar,
		                                  b->control_invert);
		if(sf_ctrl == 0xFFFF) continue;

		SS_GeneratorType sf_dest = SS_GEN_INVALID;
		int16_t mod_amount = value;

		switch(dest) {
			case SS_DLSDEST_GAIN:
				sf_dest = SS_GEN_INITIAL_ATTENUATION;
				mod_amount = -value;
				break;
			case SS_DLSDEST_PITCH:
				sf_dest = SS_GEN_FINE_TUNE;
				break;
			case SS_DLSDEST_PAN:
				sf_dest = SS_GEN_PAN;
				break;
			case SS_DLSDEST_VOL_ENV_SUSTAIN:
				sf_dest = SS_GEN_SUSTAIN_VOL_ENV;
				mod_amount = (int16_t)(1000 - value);
				break;
			case SS_DLSDEST_MOD_ENV_SUSTAIN:
				sf_dest = SS_GEN_SUSTAIN_MOD_ENV;
				mod_amount = (int16_t)(1000 - value);
				break;
			case SS_DLSDEST_FILTER_CUTOFF:
				sf_dest = SS_GEN_INITIAL_FILTER_FC;
				break;
			case SS_DLSDEST_FILTER_Q:
				sf_dest = SS_GEN_INITIAL_FILTER_Q;
				break;
			case SS_DLSDEST_VOL_ENV_DELAY:
				sf_dest = SS_GEN_DELAY_VOL_ENV;
				break;
			case SS_DLSDEST_VOL_ENV_ATTACK:
				sf_dest = SS_GEN_ATTACK_VOL_ENV;
				break;
			case SS_DLSDEST_VOL_ENV_HOLD:
				sf_dest = SS_GEN_HOLD_VOL_ENV;
				break;
			case SS_DLSDEST_VOL_ENV_DECAY:
				sf_dest = SS_GEN_DECAY_VOL_ENV;
				break;
			case SS_DLSDEST_VOL_ENV_RELEASE:
				sf_dest = SS_GEN_RELEASE_VOL_ENV;
				break;
			case SS_DLSDEST_MOD_ENV_DELAY:
				sf_dest = SS_GEN_DELAY_MOD_ENV;
				break;
			case SS_DLSDEST_MOD_ENV_ATTACK:
				sf_dest = SS_GEN_ATTACK_MOD_ENV;
				break;
			case SS_DLSDEST_MOD_ENV_HOLD:
				sf_dest = SS_GEN_HOLD_MOD_ENV;
				break;
			case SS_DLSDEST_MOD_ENV_DECAY:
				sf_dest = SS_GEN_DECAY_MOD_ENV;
				break;
			case SS_DLSDEST_MOD_ENV_RELEASE:
				sf_dest = SS_GEN_RELEASE_MOD_ENV;
				break;
			case SS_DLSDEST_REVERB_SEND:
				sf_dest = SS_GEN_REVERB_EFFECTS_SEND;
				break;
			case SS_DLSDEST_CHORUS_SEND:
				sf_dest = SS_GEN_CHORUS_EFFECTS_SEND;
				break;
			default:
				continue;
		}

		/* Apply output transform to primary source when its curve is still linear */
		if(b->output_transform != SS_MODCURVE_LINEAR) {
			uint16_t cur_curve = (sf_src >> 10) & 0x0f;
			if(cur_curve == SS_MODCURVE_LINEAR) {
				sf_src = (uint16_t)((sf_src & ~0xFC00u) |
				                    ((uint16_t)b->output_transform << 10));
			}
		}

		/* Velocity/volume/expression → attenuation must be inverted.
		 * Some DLS banks omit the invert flag; we force it here like the TS code. */
		if(sf_dest == SS_GEN_INITIAL_ATTENUATION &&
		   (src == SS_DLSSRC_VELOCITOY || src == SS_DLSSRC_VOLUME || src == SS_DLSSRC_EXPRESSION)) {
			sf_src |= (1u << 8); /* set negative bit */
			if(mod_amount > 960)
				mod_amount = 960;
			else if(mod_amount < 0)
				mod_amount = 0;
		}

		zone_add_mod(zone, sf_src, sf_ctrl, sf_dest, mod_amount);
	}

	/* ── Pass 3: keyNum → env hold/decay with Airfont 340 correction ────── */
	for(size_t i = 0; i < count; i++) {
		const DLS_ConnBlock *b = &blocks[i];
		if((SS_DLSSource)b->us_source != SS_DLSSRC_KEYNUM) continue;
		if((SS_DLSSource)b->us_control != SS_DLSSRC_NONE) continue;

		int16_t scale = (int16_t)(b->l_scale >> 16);
		SS_DLSDestination dest = (SS_DLSDestination)b->us_dest;

		SS_GeneratorType key_to_gen = SS_GEN_INVALID;
		SS_GeneratorType real_gen = SS_GEN_INVALID;
		SS_DLSDestination real_dls = SS_DLSDEST_NONE;

		switch(dest) {
			case SS_DLSDEST_VOL_ENV_HOLD:
				key_to_gen = SS_GEN_KEYNUM_TO_VOL_ENV_HOLD;
				real_gen = SS_GEN_HOLD_VOL_ENV;
				real_dls = SS_DLSDEST_VOL_ENV_HOLD;
				break;
			case SS_DLSDEST_VOL_ENV_DECAY:
				key_to_gen = SS_GEN_KEYNUM_TO_VOL_ENV_DECAY;
				real_gen = SS_GEN_DECAY_VOL_ENV;
				real_dls = SS_DLSDEST_VOL_ENV_DECAY;
				break;
			case SS_DLSDEST_MOD_ENV_HOLD:
				key_to_gen = SS_GEN_KEYNUM_TO_MOD_ENV_HOLD;
				real_gen = SS_GEN_HOLD_MOD_ENV;
				real_dls = SS_DLSDEST_MOD_ENV_HOLD;
				break;
			case SS_DLSDEST_MOD_ENV_DECAY:
				key_to_gen = SS_GEN_KEYNUM_TO_MOD_ENV_DECAY;
				real_gen = SS_GEN_DECAY_MOD_ENV;
				real_dls = SS_DLSDEST_MOD_ENV_DECAY;
				break;
			default:
				continue;
		}

		/* keyToGen value: DLS scale / -128 */
		int16_t key_to_val = (int16_t)((float)scale / -128.0f);
		zone_set_gen(zone, key_to_gen, key_to_val);

		/* Airfont 340 correction: adjust the absolute env generator.
		 * correction = round((60 / 128) * scale)
		 * new_real   = correction + existing_real_value  */
		if(key_to_val <= 120) {
			int16_t correction = (int16_t)roundf((60.0f / 128.0f) * (float)scale);
			for(size_t j = 0; j < count; j++) {
				const DLS_ConnBlock *sb = &blocks[j];
				if(sb->us_source != SS_DLSSRC_NONE) continue;
				if(sb->us_control != SS_DLSSRC_NONE) continue;
				if((SS_DLSDestination)sb->us_dest != real_dls) continue;
				int16_t real_val = (int16_t)(sb->l_scale >> 16);
				zone_set_gen(zone, real_gen, (int16_t)(correction + real_val));
				break;
			}
		}
	}

	/* ── Pass 4: DLS Level 1 corrections ───────────────────────────────── */
	if(dls1) {
		/* DLS1 only has modulation LFO; map its parameters to vibrato LFO
		 * and convert modLfoToPitch → vibLfoToPitch so GS matrix stays correct. */
		int16_t mod_delay = zone_get_gen(zone, SS_GEN_DELAY_MOD_LFO, -11931);
		int16_t mod_freq = zone_get_gen(zone, SS_GEN_FREQ_MOD_LFO, -725);
		zone_set_gen(zone, SS_GEN_DELAY_VIB_LFO, mod_delay);
		zone_set_gen(zone, SS_GEN_FREQ_VIB_LFO, mod_freq);

		/* Move mod-lfo-to-pitch excursion to vibrato lfo */
		int16_t mod_to_pitch = zone_get_gen(zone, SS_GEN_MOD_LFO_TO_PITCH, 0);
		if(mod_to_pitch != 0) {
			zone_set_gen(zone, SS_GEN_VIB_LFO_TO_PITCH, mod_to_pitch);
			/* Zero out the mod LFO→pitch generator */
			for(size_t i = 0; i < zone->gen_count; i++) {
				if(zone->generators[i].type == SS_GEN_MOD_LFO_TO_PITCH) {
					zone->generators[i].value = 0;
					break;
				}
			}
		}

		/* Redirect any modulator targeting modLfoToPitch */
		for(size_t i = 0; i < zone->mod_count; i++) {
			if(zone->modulators[i].dest_enum == SS_GEN_MOD_LFO_TO_PITCH)
				zone->modulators[i].dest_enum = SS_GEN_VIB_LFO_TO_PITCH;
		}
	}
}

/* ── Wave pool parser (forward declaration) ──────────────────────────────── */
static bool parse_wave_pool(SS_File *waves_file,
                            const uint32_t *pool_offsets,
                            size_t pool_count,
                            SS_SoundBank *bank,
                            bool riff64);

/* ── Main DLS loader ─────────────────────────────────────────────────────── */

SS_SoundBank *ss_dls_load(SS_File *main_file, bool riff64) {
	const size_t size_size = riff64 ? 8 : 4;

	SS_SoundBank *bank = ss_soundbank_new();
	if(!bank) return NULL;
	strncpy(bank->name, "DLS Bank", sizeof(bank->name) - 1);

	/* Start at the beginning */
	ss_file_seek(main_file, 0);

	/* Skip RIFF header (already verified by caller) */
	/*ss_file_skip(main_file, 4); "RIFF" */
	size_t total_size = ss_file_read_le(main_file, 4, size_size);
	(void)total_size;
	ss_file_skip(main_file, 4); /* "DLS " */

	uint32_t *pool_offsets = NULL;
	size_t pool_count = 0;
	SS_File *waves_file = NULL;
	bool has_waves = false;

	/* Temporary per-instrument storage */
	typedef struct {
		uint32_t bank_msb;
		uint32_t bank_lsb;
		uint32_t patch;
		bool is_drum;
		SS_BasicInstrument *inst;
		DLS_ArtBuf global_art;
		DLS_ZoneExtra *zone_extras;
		size_t zone_extra_count;
		size_t zone_extra_cap;
	} DLS_InstrEntry;

	DLS_InstrEntry *instr_entries = NULL;
	size_t instr_count = 0, instr_cap = 0;

	while(ss_file_remaining(main_file) >= 4 + size_size) {
		SS_RIFFChunk chunk;
		if(!ss_riff_read_chunk(main_file, &chunk, false, riff64)) break;

		if(strcmp(chunk.header, "ptbl") == 0) {
			/*ss_file_skip(chunk.file, size_size); cbSize */
			size_t pos = 4;
			uint32_t cCues = (uint32_t)ss_file_read_le(chunk.file, pos, 4);
			pool_count = cCues;
			pool_offsets = (uint32_t *)malloc(cCues * sizeof(uint32_t));
			if(!pool_offsets) goto fail;
			pos += 4;
			for(uint32_t i = 0; i < cCues; i++, pos += 4)
				pool_offsets[i] = (uint32_t)ss_file_read_le(chunk.file, pos, 4);

		} else if(strcmp(chunk.header, "LIST") == 0) {
			char list_id[5];
			ss_file_read_string(chunk.file, 0, list_id, 4);

			if(strcmp(list_id, "wvpl") == 0) {
				waves_file = chunk.file;
				chunk.file = NULL;
				has_waves = true;

			} else if(strcmp(list_id, "lins") == 0) {
				/* ── Instrument list ────────────────────────────────────── */
				while(ss_file_remaining(chunk.file) >= 4 + size_size) {
					SS_RIFFChunk ins_list;
					if(!ss_riff_read_chunk(chunk.file, &ins_list, false, riff64)) break;
					if(strcmp(ins_list.header, "LIST") != 0) continue;
					char ins_id[5];
					ss_file_read_string(ins_list.file, 0, ins_id, 4);
					if(strcmp(ins_id, "ins ") != 0) continue;

					/* Grow instr_entries */
					if(instr_count >= instr_cap) {
						instr_cap = instr_cap ? instr_cap * 2 : 16;
						DLS_InstrEntry *tmp = (DLS_InstrEntry *)realloc(
						instr_entries, instr_cap * sizeof(DLS_InstrEntry));
						if(!tmp) goto fail;
						instr_entries = tmp;
					}

					SS_BasicInstrument *inst = (SS_BasicInstrument *)calloc(
					1, sizeof(SS_BasicInstrument));
					if(!inst) goto fail;
					DLS_InstrEntry *entry = &instr_entries[instr_count++];
					memset(entry, 0, sizeof(*entry));
					entry->inst = inst;
					art_buf_init(&entry->global_art);

					SS_InstrumentZone *zones = NULL;
					size_t zone_count = 0, zone_cap = 0;

					while(ss_file_remaining(ins_list.file) >= 4 + size_size) {
						SS_RIFFChunk sub;
						if(!ss_riff_read_chunk(ins_list.file, &sub, false, riff64)) break;

						if(strcmp(sub.header, "insh") == 0) {
							uint32_t n_regions = (uint32_t)ss_file_read_le(sub.file, 0, 4);
							uint32_t bank_val = (uint32_t)ss_file_read_le(sub.file, 4, 4);
							uint32_t patch = (uint32_t)ss_file_read_le(sub.file, 8, 4);
							entry->bank_msb = (bank_val >> 8) & 0x7F;
							entry->bank_lsb = bank_val & 0x7F;
							entry->patch = patch & 0x7F;
							entry->is_drum = (bank_val & 0x80000000u) != 0;
							(void)n_regions;

						} else if(strcmp(sub.header, "LIST") == 0) {
							char sub_id[5];
							ss_file_read_string(sub.file, 0, sub_id, 4);

							if(strcmp(sub_id, "lrgn") == 0) {
								/* ── Region list ──────────────────────── */
								while(ss_file_remaining(sub.file) >= 4 + size_size) {
									SS_RIFFChunk rgn_list;
									if(!ss_riff_read_chunk(sub.file, &rgn_list,
									                       false, riff64)) break;
									if(strcmp(rgn_list.header, "LIST") != 0) continue;
									char rgn_id[5];
									ss_file_read_string(rgn_list.file, 0, rgn_id, 4);
									if(strcmp(rgn_id, "rgn ") != 0 &&
									   strcmp(rgn_id, "rgn2") != 0) continue;

									DLS_RegionHeader rh;
									memset(&rh, 0, sizeof(rh));
									DLS_WaveSample ws;
									memset(&ws, 0, sizeof(ws));
									bool has_wsmp = false;
									uint32_t wave_index = 0;
									DLS_ArtBuf rgn_art;
									art_buf_init(&rgn_art);

									while(ss_file_remaining(rgn_list.file) >= 4 + size_size) {
										SS_RIFFChunk rs;
										if(!ss_riff_read_chunk(rgn_list.file, &rs,
										                       false, riff64)) break;

										if(strcmp(rs.header, "rgnh") == 0) {
											rh.key_low = (uint16_t)ss_file_read_le(rs.file, 0, 2);
											rh.key_high = (uint16_t)ss_file_read_le(rs.file, 2, 2);
											rh.vel_low = (uint16_t)ss_file_read_le(rs.file, 4, 2);
											rh.vel_high = (uint16_t)ss_file_read_le(rs.file, 6, 2);
											rh.options = (uint16_t)ss_file_read_le(rs.file, 8, 2);
											rh.key_group = (uint16_t)ss_file_read_le(rs.file, 10, 2);

										} else if(strcmp(rs.header, "wsmp") == 0) {
											parse_wsmp(rs.file, &ws, riff64);
											has_wsmp = true;

										} else if(strcmp(rs.header, "wlnk") == 0) {
											/*ss_iba_read_le(&rs.data, 2); fusOptions */
											/*ss_iba_read_le(&rs.data, 2); phase group */
											/*ss_iba_read_le(&rs.data, 4); channel */
											wave_index = (uint32_t)ss_file_read_le(rs.file, 8, 4);

										} else if(strcmp(rs.header, "LIST") == 0) {
											char art_id[5];
											ss_file_read_string(rs.file, 0, art_id, 4);
											if(strcmp(art_id, "lart") == 0) {
												rgn_art.dls1 = true;
												parse_lart_body(rs.file, &rgn_art, riff64);
											} else if(strcmp(art_id, "lar2") == 0) {
												rgn_art.dls1 = false;
												parse_lart_body(rs.file, &rgn_art, riff64);
											}
										}
										ss_riff_close_chunk(&rs);
									}

									/* Fix vel range: some files have 0,0 meaning full range */
									if(rh.vel_low == 0 && rh.vel_high == 0)
										rh.vel_high = 127;

									/* Create instrument zone */
									if(zone_count >= zone_cap) {
										zone_cap = zone_cap ? zone_cap * 2 : 8;
										SS_InstrumentZone *tz = (SS_InstrumentZone *)realloc(
										zones, zone_cap * sizeof(SS_InstrumentZone));
										if(!tz) {
											art_buf_free(&rgn_art);
											goto skip_region;
										}
										zones = tz;
									}
									{
										SS_InstrumentZone *iz = &zones[zone_count];
										memset(iz, 0, sizeof(*iz));
										iz->base.key_range_min = (int8_t)rh.key_low;
										iz->base.key_range_max = (int8_t)rh.key_high;
										iz->base.vel_range_min = (int8_t)rh.vel_low;
										iz->base.vel_range_max = (int8_t)rh.vel_high;

										/* Exclusive class from key group */
										if(rh.key_group > 0)
											zone_set_gen(&iz->base, SS_GEN_EXCLUSIVE_CLASS,
											             (int16_t)rh.key_group);

										/* Store wave index temporarily in sample pointer */
										iz->sample = (SS_BasicSample *)(uintptr_t)wave_index;

										/* Grow zone_extras in lockstep with zones */
										if(zone_count >= entry->zone_extra_cap) {
											size_t nc = entry->zone_extra_cap ?
											            entry->zone_extra_cap * 2 :
											            8;
											DLS_ZoneExtra *te = (DLS_ZoneExtra *)realloc(
											entry->zone_extras,
											nc * sizeof(DLS_ZoneExtra));
											if(!te) {
												art_buf_free(&rgn_art);
												goto skip_region;
											}
											entry->zone_extras = te;
											entry->zone_extra_cap = nc;
										}

										DLS_ZoneExtra *extra = &entry->zone_extras[zone_count];
										zone_extra_init(extra);

										if(has_wsmp) {
											extra->wsmp_unity_note = ws.unity_note;
											extra->has_wsmp_unity_note = true;
											extra->wsmp_fine_tune = ws.fine_tune;
											extra->has_wsmp_fine_tune = true;
											if(ws.gain != 0) {
												extra->wsmp_gain = ws.gain;
												extra->has_wsmp_gain = true;
											}
											if(ws.loop_count > 0) {
												extra->wsmp_loop_start = ws.loop_start;
												extra->wsmp_loop_length = ws.loop_length;
												extra->wsmp_loop_type = ws.loop_type;
												extra->has_wsmp_loop = true;
											}
										}
										extra->art = rgn_art;
										zone_count++;
									}
									goto next_region;
								skip_region:
									art_buf_free(&rgn_art);
								next_region:;
									ss_riff_close_chunk(&rgn_list);
								}

							} else if(strcmp(sub_id, "lart") == 0) {
								/* Instrument-level (global) articulation */
								entry->global_art.dls1 = true;
								parse_lart_body(sub.file, &entry->global_art, riff64);
							} else if(strcmp(sub_id, "lar2") == 0) {
								entry->global_art.dls1 = false;
								parse_lart_body(sub.file, &entry->global_art, riff64);
							}
						}
						ss_riff_close_chunk(&sub);
					}

					entry->zone_extra_count = zone_count;
					inst->zones = zones;
					inst->zone_count = zone_count;
					snprintf(inst->name, sizeof(inst->name),
					         "Instrument %zu", instr_count - 1);

					ss_riff_close_chunk(&ins_list);
				}
			}
		}
	}

	/* ── Parse wave pool into samples ────────────────────────────────────── */
	if(has_waves && pool_offsets)
		parse_wave_pool(waves_file, pool_offsets, pool_count, bank, riff64);

	/* ── Build presets from instrument entries ───────────────────────────── */
	bank->instruments = (SS_BasicInstrument *)calloc(instr_count, sizeof(SS_BasicInstrument));
	bank->instrument_count = instr_count;
	bank->presets = (SS_BasicPreset *)calloc(instr_count, sizeof(SS_BasicPreset));
	bank->preset_count = instr_count;
	if(!bank->instruments || !bank->presets) goto fail;

	for(size_t i = 0; i < instr_count; i++) {
		DLS_InstrEntry *e = &instr_entries[i];
		if(!e->inst) continue;

		bank->instruments[i] = *e->inst;
		free(e->inst);
		e->inst = NULL;

		SS_BasicInstrument *inst = &bank->instruments[i];

		/* Apply global articulation to the instrument's global zone */
		if(e->global_art.count > 0) {
			apply_conn_blocks(&inst->global_zone,
			                  e->global_art.blocks,
			                  e->global_art.count,
			                  e->global_art.dls1);
		}

		/* Default DLS reverb/chorus modulators (CC91/CC93 → reverb/chorusSend).
		 * Add them to the global zone only if the articulation didn't already
		 * include a modulator targeting those destinations. */
		bool has_reverb_mod = false, has_chorus_mod = false;
		for(size_t m = 0; m < inst->global_zone.mod_count; m++) {
			if(inst->global_zone.modulators[m].dest_enum == SS_GEN_REVERB_EFFECTS_SEND)
				has_reverb_mod = true;
			if(inst->global_zone.modulators[m].dest_enum == SS_GEN_CHORUS_EFFECTS_SEND)
				has_chorus_mod = true;
		}
		/*
		 * DEFAULT_DLS_REVERB: CC 91 (reverb), linear, unipolar, negative → reverbSend, 1000
		 * DEFAULT_DLS_CHORUS: CC 93 (chorus), linear, unipolar, negative → chorusSend, 1000
		 * source_enum = make_modsrc(LINEAR, bipolar=false, negative=true, is_cc=true, idx)
		 *             = (0 << 10) | (0 << 9) | (1 << 8) | (1 << 7) | idx
		 *             = 0x0180 | idx
		 */
		if(!has_reverb_mod) {
			uint16_t src = make_modsrc(SS_MODCURVE_LINEAR, false, true, true, 91);
			uint16_t no_ctrl = make_modsrc(SS_MODCURVE_LINEAR, false, false, false,
			                               SS_MODSRC_NO_CONTROLLER);
			zone_add_mod(&inst->global_zone, src, no_ctrl,
			             SS_GEN_REVERB_EFFECTS_SEND, 1000);
		}
		if(!has_chorus_mod) {
			uint16_t src = make_modsrc(SS_MODCURVE_LINEAR, false, true, true, 93);
			uint16_t no_ctrl = make_modsrc(SS_MODCURVE_LINEAR, false, false, false,
			                               SS_MODSRC_NO_CONTROLLER);
			zone_add_mod(&inst->global_zone, src, no_ctrl,
			             SS_GEN_CHORUS_EFFECTS_SEND, 1000);
		}

		/* Resolve sample pointers and apply per-zone wsmp + articulation */
		for(size_t z = 0; z < inst->zone_count; z++) {
			SS_InstrumentZone *iz = &inst->zones[z];
			DLS_ZoneExtra *extra = (e->zone_extras && z < e->zone_extra_count) ? &e->zone_extras[z] : NULL;

			uintptr_t wave_idx = (uintptr_t)iz->sample;
			if(wave_idx < bank->sample_count) {
				iz->sample = (SS_BasicSample *)malloc(sizeof(SS_BasicSample));
				if(iz->sample) {
					*iz->sample = bank->samples[wave_idx];
					iz->sample->owns_raw_data = false;
				}
			} else {
				iz->sample = NULL;
			}

			if(!extra) continue;

			/* wsmp-derived generators (applied after sample pointer resolved) */
			if(extra->has_wsmp_unity_note && iz->sample) {
				if(extra->wsmp_unity_note != iz->sample->original_key) {
					zone_set_gen(&iz->base, SS_GEN_OVERRIDING_ROOT_KEY,
					             (int16_t)extra->wsmp_unity_note);
				}
			}
			if(extra->has_wsmp_fine_tune && iz->sample) {
				/* Zone fine tune = wsmp fine tune - sample's built-in pitch correction */
				int16_t zone_fine = (int16_t)(extra->wsmp_fine_tune -
				                              (int16_t)iz->sample->pitch_correction);
				if(zone_fine != 0)
					zone_set_gen(&iz->base, SS_GEN_FINE_TUNE, zone_fine);
			}
			if(extra->has_wsmp_gain) {
				/* DLS2: each 32-bit unit = 1/655360 dB → gain16 (upper 16) = 0.1 dB/unit.
				 * Negate (gain → attenuation) and apply E-MU correction. */
				int16_t gain16 = (int16_t)((int32_t)extra->wsmp_gain >> 16);
				int16_t att_cB = (int16_t)(-(float)gain16 / 0.4f);
				if(att_cB != 0)
					zone_set_gen(&iz->base, SS_GEN_INITIAL_ATTENUATION, att_cB);
			}
			if(extra->has_wsmp_loop && iz->sample) {
				int16_t smode = (extra->wsmp_loop_type == SS_DLSLOOP_LOOP_AND_RELEASE) ? 3 : 1;
				zone_set_gen(&iz->base, SS_GEN_SAMPLE_MODES, smode);

				/* Compute loop-point offsets relative to the sample's built-in loop */
				uint32_t ws_end = extra->wsmp_loop_start + extra->wsmp_loop_length;
				int32_t ds = (int32_t)extra->wsmp_loop_start - (int32_t)iz->sample->loop_start;
				int32_t de = (int32_t)ws_end - (int32_t)iz->sample->loop_end;
				if(ds != 0) {
					zone_set_gen(&iz->base, SS_GEN_STARTLOOP_ADDRS_OFFSET,
					             (int16_t)(ds % 32768));
					if(ds / 32768 != 0)
						zone_set_gen(&iz->base, SS_GEN_STARTLOOP_ADDRS_COARSE_OFFSET,
						             (int16_t)(ds / 32768));
				}
				if(de != 0) {
					zone_set_gen(&iz->base, SS_GEN_ENDLOOP_ADDRS_OFFSET,
					             (int16_t)(de % 32768));
					if(de / 32768 != 0)
						zone_set_gen(&iz->base, SS_GEN_ENDLOOP_ADDRS_COARSE_OFFSET,
						             (int16_t)(de / 32768));
				}
			} else if(extra->has_wsmp_loop && !iz->sample) {
				/* No sample but loop requested: set modes at least */
				int16_t smode = (extra->wsmp_loop_type == SS_DLSLOOP_LOOP_AND_RELEASE) ? 3 : 1;
				zone_set_gen(&iz->base, SS_GEN_SAMPLE_MODES, smode);
			}

			/* Per-region articulation */
			if(extra->art.count > 0)
				apply_conn_blocks(&iz->base,
				                  extra->art.blocks,
				                  extra->art.count,
				                  extra->art.dls1);
		}

		/* Create matching preset */
		SS_BasicPreset *p = &bank->presets[i];
		strncpy(p->name, inst->name, sizeof(p->name) - 1);
		p->program = (uint8_t)(e->patch & 0x7F);
		p->bank_msb = (uint8_t)e->bank_msb;
		p->bank_lsb = (uint8_t)e->bank_lsb;
		p->is_gm_gs_drum = e->is_drum;
		p->parent_bank = bank;

		p->zones = (SS_PresetZone *)calloc(1, sizeof(SS_PresetZone));
		p->zone_count = 1;
		if(p->zones) {
			p->zones[0].base.key_range_min = -1;
			p->zones[0].base.key_range_max = 127;
			p->zones[0].base.vel_range_min = -1;
			p->zones[0].base.vel_range_max = 127;
			p->zones[0].instrument = inst;
		}
	}

	/* Cleanup */
	for(size_t i = 0; i < instr_count; i++) {
		DLS_InstrEntry *e = &instr_entries[i];
		art_buf_free(&e->global_art);
		if(e->zone_extras) {
			for(size_t z = 0; z < e->zone_extra_count; z++)
				zone_extra_free(&e->zone_extras[z]);
			free(e->zone_extras);
		}
		free(e->inst); /* NULL if already moved */
	}
	free(instr_entries);
	free(pool_offsets);
	return bank;

fail:
	if(instr_entries) {
		for(size_t i = 0; i < instr_count; i++) {
			DLS_InstrEntry *e = &instr_entries[i];
			art_buf_free(&e->global_art);
			if(e->zone_extras) {
				for(size_t z = 0; z < e->zone_extra_count; z++)
					zone_extra_free(&e->zone_extras[z]);
				free(e->zone_extras);
			}
			if(e->inst) {
				/* Zones may still hold encoded wave indices as fake pointers.
				 * Clear them before ss_instrument_free to avoid bad frees. */
				for(size_t z = 0; z < e->inst->zone_count; z++)
					e->inst->zones[z].sample = NULL;
				ss_instrument_free(e->inst);
				free(e->inst);
			}
		}
		free(instr_entries);
	}
	free(pool_offsets);
	ss_soundbank_free(bank);
	return NULL;
}

/* ── Wave pool parser ────────────────────────────────────────────────────── */
static bool parse_wave_pool(SS_File *waves_file,
                            const uint32_t *pool_offsets,
                            size_t pool_count,
                            SS_SoundBank *bank,
                            bool riff64) {
	size_t size_size = riff64 ? 8 : 4;
	bank->samples = (SS_BasicSample *)calloc(pool_count, sizeof(SS_BasicSample));
	bank->sample_count = pool_count;
	if(!bank->samples && pool_count > 0) return false;

	for(size_t i = 0; i < pool_count; i++) {
		uint32_t offset = pool_offsets[i];
		if(offset + 8 + size_size > ss_file_size(waves_file)) continue;

		SS_File *wave_file = ss_file_slice(waves_file, offset + 4, ss_file_size(waves_file) - offset - 4);
		if(!wave_file) return false;

		SS_RIFFChunk wave_list;
		if(!ss_riff_read_chunk(wave_file, &wave_list, false, riff64)) {
			ss_file_close(wave_file);
			continue;
		}
		if(strcmp(wave_list.header, "LIST") != 0) {
			ss_riff_close_chunk(&wave_list);
			ss_file_close(wave_file);
			continue;
		}

		char list_id[5];
		ss_file_read_string(wave_list.file, 0, list_id, 4);
		if(strcmp(list_id, "wave") != 0) continue;

		SS_BasicSample *s = &bank->samples[i];
		snprintf(s->name, sizeof(s->name), "Sample%zu", i);

		uint32_t sample_rate = 44100;
		uint16_t bits_per_sample = 16;
		uint16_t num_channels = 1;
		SS_File *pcm_data = NULL;
		size_t pcm_len = 0;

		while(ss_file_remaining(wave_list.file) >= 4 + size_size) {
			SS_RIFFChunk sub;
			if(!ss_riff_read_chunk(wave_list.file, &sub, false, riff64)) break;

			if(strcmp(sub.header, "fmt ") == 0) {
				/* uint16_t fmt_tag = */ /*ss_iba_read_le(&sub.data, 2);*/
				num_channels = (uint16_t)ss_file_read_le(sub.file, 2, 2);
				sample_rate = (uint32_t)ss_file_read_le(sub.file, 4, 4);
				/*ss_file_read_le(sub.file, 8, 4); byte rate */
				/*ss_file_read_le(sub.data, 12, 2); block align */
				bits_per_sample = (uint16_t)ss_file_read_le(sub.file, 14, 2);

			} else if(strcmp(sub.header, "data") == 0) {
				pcm_data = sub.file;
				pcm_len = ss_file_size(sub.file);
				sub.file = NULL;

			} else if(strcmp(sub.header, "wsmp") == 0) {
				DLS_WaveSample ws;
				parse_wsmp(sub.file, &ws, riff64);
				s->original_key = (uint8_t)ws.unity_note;
				s->pitch_correction = (int8_t)ws.fine_tune; /* fine_tune is in cents */
				if(ws.loop_count > 0) {
					s->loop_start = ws.loop_start;
					s->loop_end = ws.loop_start + ws.loop_length;
				}
			}
			ss_riff_close_chunk(&sub);
		}
		ss_riff_close_chunk(&wave_list);

		if(!pcm_data || pcm_len == 0) continue;

		s->sample_rate = sample_rate;
		s->sample_type = SS_SAMPLE_TYPE_MONO;

		size_t bytes_per_sample = bits_per_sample / 8;
		size_t total_frames = pcm_len / (bytes_per_sample * num_channels);

		if(bits_per_sample == 16) {
			uint8_t *out = (uint8_t *)malloc(total_frames * 2);
			if(!out) continue;
			int16_t *dst = (int16_t *)out;
			for(size_t f = 0; f < total_frames; f++)
				dst[f] = (int16_t)ss_file_read_le(pcm_data, f * num_channels * 2, 2);
			s->s16le_data = out;
			s->s16le_length = total_frames * 2;

			/* Clamp loop end to sample length */
			if(s->loop_end > (uint32_t)total_frames)
				s->loop_end = (uint32_t)total_frames;

		} else if(bits_per_sample == 8) {
			uint8_t *out = (uint8_t *)malloc(total_frames);
			if(!out) continue;
			for(size_t f = 0; f < total_frames; f++)
				out[f] = ss_file_read_u8(pcm_data, f * num_channels);
			s->u8_data = out;
			s->u8_length = total_frames;

			if(s->loop_end > (uint32_t)total_frames)
				s->loop_end = (uint32_t)total_frames;
		}

		s->owns_raw_data = true;
	}
	return true;
}
