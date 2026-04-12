/**
 * dls_reader.c
 * DLS (Downloadable Sounds) Level 1 & 2 loader.
 *
 * DLS spec: https://www.midi.org/specifications-old/item/dls-level-1-specification
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if __has_include(<spessasynth_core/spessasynth.h>)
#include <spessasynth_core/indexed_byte_array.h>
#include <spessasynth_core/riff_chunk.h>
#include <spessasynth_core/soundbank.h>
#else
#include "spessasynth/soundbank/soundbank.h"
#include "spessasynth/utils/indexed_byte_array.h"
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
 *           LIST rgn  <- single region
 *             rgnh    <- region header
 *             wsmp    <- wave sample (loop / tuning info)
 *             wlnk    <- wave link (sample reference)
 *         LIST lart / lar2  <- articulation (modulators/generators)
 *     LIST wvpl   <- wave pool
 *       LIST wave  <- single PCM wave (RIFF WAVE inside DLS)
 *     ptbl        <- pool table (maps wlnk index to wave pool position)
 *     dlid        <- DLS unique identifier (optional)
 *     colh        <- collection header (preset / wave counts)
 */

/* Region header */
typedef struct {
	uint16_t key_low, key_high;
	uint16_t vel_low, vel_high;
	uint16_t options;
	uint16_t key_group;
	uint16_t exclusive_class;
} DLS_RegionHeader;

/* Wave sample */
typedef struct {
	uint32_t cbSize;
	uint16_t unity_note;
	int16_t fine_tune;
	int32_t attenuation;
	uint32_t options;
	uint32_t loop_count;
	/* loop: */
	uint32_t loop_size;
	uint32_t loop_type;
	uint32_t loop_start;
	uint32_t loop_length;
} DLS_WaveSample;

static bool parse_wsmp(SS_IBA *iba, DLS_WaveSample *ws) {
	ws->cbSize = (uint32_t)ss_iba_read_le(iba, 4);
	ws->unity_note = (uint16_t)ss_iba_read_le(iba, 2);
	ws->fine_tune = (int16_t)ss_iba_read_le(iba, 2);
	ws->attenuation = (int32_t)ss_iba_read_le(iba, 4);
	ws->options = (uint32_t)ss_iba_read_le(iba, 4);
	ws->loop_count = (uint32_t)ss_iba_read_le(iba, 4);
	ws->loop_start = 0;
	ws->loop_length = 0;
	ws->loop_type = 0;
	if(ws->loop_count > 0) {
		ws->loop_size = (uint32_t)ss_iba_read_le(iba, 4);
		ws->loop_type = (uint32_t)ss_iba_read_le(iba, 4);
		ws->loop_start = (uint32_t)ss_iba_read_le(iba, 4);
		ws->loop_length = (uint32_t)ss_iba_read_le(iba, 4);
	}
	return true;
}

/* Forward declaration */
static bool parse_wave_pool(SS_IBA *waves_iba,
                            const uint32_t *pool_offsets,
                            size_t pool_count,
                            SS_SoundBank *bank);

SS_SoundBank *ss_dls_load(const uint8_t *data, size_t size) {
	SS_IBA main_iba;
	ss_iba_wrap(&main_iba, data, size);

	SS_SoundBank *bank = ss_soundbank_new();
	if(!bank) return NULL;
	strncpy(bank->name, "DLS Bank", sizeof(bank->name) - 1);

	/* Skip RIFF header (already verified by caller) */
	main_iba.current_index += 4; /* "RIFF" */
	uint32_t total_size = (uint32_t)ss_iba_read_le(&main_iba, 4);
	(void)total_size;
	main_iba.current_index += 4; /* "DLS " */

	/* Collect top-level chunks */
	uint32_t *pool_offsets = NULL;
	size_t pool_count = 0;
	SS_IBA waves_iba;
	memset(&waves_iba, 0, sizeof(waves_iba));
	bool has_waves = false;

	/* Temporary instrument storage */
	typedef struct {
		uint32_t bank_msb;
		uint32_t patch;
		bool is_drum;
		SS_BasicInstrument *inst;
	} DLS_InstrEntry;
	DLS_InstrEntry *instr_entries = NULL;
	size_t instr_count = 0, instr_cap = 0;

	while(ss_iba_remaining(&main_iba) >= 8) {
		SS_RIFFChunk chunk;
		if(!ss_riff_read_chunk(&main_iba, &chunk, false, false)) break;

		if(strcmp(chunk.header, "ptbl") == 0) {
			/* Pool table */
			uint32_t cbSize = (uint32_t)ss_iba_read_le(&chunk.data, 4);
			(void)cbSize;
			uint32_t cCues = (uint32_t)ss_iba_read_le(&chunk.data, 4);
			pool_count = cCues;
			pool_offsets = (uint32_t *)malloc(cCues * sizeof(uint32_t));
			if(!pool_offsets) goto fail;
			for(uint32_t i = 0; i < cCues; i++)
				pool_offsets[i] = (uint32_t)ss_iba_read_le(&chunk.data, 4);

		} else if(strcmp(chunk.header, "LIST") == 0) {
			char list_id[5];
			ss_iba_read_string(&chunk.data, list_id, 4);

			if(strcmp(list_id, "wvpl") == 0) {
				waves_iba = chunk.data;
				has_waves = true;
			} else if(strcmp(list_id, "lins") == 0) {
				/* Instrument list */
				while(ss_iba_remaining(&chunk.data) >= 8) {
					SS_RIFFChunk ins_list;
					if(!ss_riff_read_chunk(&chunk.data, &ins_list, false, false)) break;
					if(strcmp(ins_list.header, "LIST") != 0) continue;
					char ins_id[5];
					ss_iba_read_string(&ins_list.data, ins_id, 4);
					if(strcmp(ins_id, "ins ") != 0) continue;

					/* Grow instr_entries */
					if(instr_count >= instr_cap) {
						instr_cap = instr_cap ? instr_cap * 2 : 16;
						DLS_InstrEntry *tmp = (DLS_InstrEntry *)realloc(instr_entries,
						                                                instr_cap * sizeof(DLS_InstrEntry));
						if(!tmp) goto fail;
						instr_entries = tmp;
					}

					SS_BasicInstrument *inst = (SS_BasicInstrument *)calloc(1, sizeof(SS_BasicInstrument));
					DLS_InstrEntry *entry = &instr_entries[instr_count++];
					entry->inst = inst;
					entry->bank_msb = 0;
					entry->patch = 0;
					entry->is_drum = false;

					/* Region storage */
					SS_InstrumentZone *zones = NULL;
					size_t zone_count = 0, zone_cap = 0;

					while(ss_iba_remaining(&ins_list.data) >= 8) {
						SS_RIFFChunk sub;
						if(!ss_riff_read_chunk(&ins_list.data, &sub, false, false)) break;

						if(strcmp(sub.header, "insh") == 0) {
							uint32_t n_regions = (uint32_t)ss_iba_read_le(&sub.data, 4);
							uint32_t bank = (uint32_t)ss_iba_read_le(&sub.data, 4);
							uint32_t patch = (uint32_t)ss_iba_read_le(&sub.data, 4);
							entry->bank_msb = (bank >> 8) & 0x7F;
							entry->patch = patch & 0x7F;
							entry->is_drum = (bank & 0x80000000u) != 0;
							(void)n_regions;

						} else if(strcmp(sub.header, "LIST") == 0) {
							char sub_id[5];
							ss_iba_read_string(&sub.data, sub_id, 4);

							if(strcmp(sub_id, "lrgn") == 0) {
								/* Regions */
								while(ss_iba_remaining(&sub.data) >= 8) {
									SS_RIFFChunk rgn_list;
									if(!ss_riff_read_chunk(&sub.data, &rgn_list, false, false)) break;
									if(strcmp(rgn_list.header, "LIST") != 0) continue;
									char rgn_id[5];
									ss_iba_read_string(&rgn_list.data, rgn_id, 4);
									if(strcmp(rgn_id, "rgn ") != 0 &&
									   strcmp(rgn_id, "rgn2") != 0) continue;

									DLS_RegionHeader rh;
									memset(&rh, 0, sizeof(rh));
									DLS_WaveSample ws;
									memset(&ws, 0, sizeof(ws));
									uint32_t wave_index = 0;

									while(ss_iba_remaining(&rgn_list.data) >= 8) {
										SS_RIFFChunk rs;
										if(!ss_riff_read_chunk(&rgn_list.data, &rs, false, false)) break;
										if(strcmp(rs.header, "rgnh") == 0) {
											rh.key_low = (uint16_t)ss_iba_read_le(&rs.data, 2);
											rh.key_high = (uint16_t)ss_iba_read_le(&rs.data, 2);
											rh.vel_low = (uint16_t)ss_iba_read_le(&rs.data, 2);
											rh.vel_high = (uint16_t)ss_iba_read_le(&rs.data, 2);
											rh.options = (uint16_t)ss_iba_read_le(&rs.data, 2);
											rh.key_group = (uint16_t)ss_iba_read_le(&rs.data, 2);
										} else if(strcmp(rs.header, "wsmp") == 0) {
											parse_wsmp(&rs.data, &ws);
										} else if(strcmp(rs.header, "wlnk") == 0) {
											ss_iba_read_le(&rs.data, 2); /* options */
											ss_iba_read_le(&rs.data, 2); /* phase group */
											ss_iba_read_le(&rs.data, 4); /* channel */
											wave_index = (uint32_t)ss_iba_read_le(&rs.data, 4);
										}
									}

									/* Create instrument zone */
									if(zone_count >= zone_cap) {
										zone_cap = zone_cap ? zone_cap * 2 : 8;
										SS_InstrumentZone *tz = (SS_InstrumentZone *)realloc(zones,
										                                                     zone_cap * sizeof(SS_InstrumentZone));
										if(!tz) goto skip_region;
										zones = tz;
									}
									{
										SS_InstrumentZone *iz = &zones[zone_count++];
										memset(iz, 0, sizeof(*iz));
										iz->base.key_range_min = (int8_t)rh.key_low;
										iz->base.key_range_max = (int8_t)rh.key_high;
										iz->base.vel_range_min = (int8_t)rh.vel_low;
										iz->base.vel_range_max = (int8_t)rh.vel_high;

										/* Convert wsmp to SF2 generators */
										size_t ng = 8;
										iz->base.generators = (SS_Generator *)calloc(ng, sizeof(SS_Generator));
										if(iz->base.generators) {
											size_t gi = 0;
											/* unity note */
											iz->base.generators[gi].type = SS_GEN_OVERRIDING_ROOT_KEY;
											iz->base.generators[gi++].value = (int16_t)ws.unity_note;
											/* fine tune */
											iz->base.generators[gi].type = SS_GEN_FINE_TUNE;
											iz->base.generators[gi++].value = ws.fine_tune;
											/* attenuation: DLS uses millibels */
											iz->base.generators[gi].type = SS_GEN_INITIAL_ATTENUATION;
											iz->base.generators[gi++].value = (int16_t)(ws.attenuation / 10);
											/* loop */
											if(ws.loop_count > 0) {
												iz->base.generators[gi].type = SS_GEN_SAMPLE_MODES;
												iz->base.generators[gi++].value = 1;
												/* store loop points as offsets */
												iz->base.generators[gi].type = SS_GEN_STARTLOOP_ADDRS_OFFSET;
												iz->base.generators[gi++].value = 0;
												iz->base.generators[gi].type = SS_GEN_ENDLOOP_ADDRS_OFFSET;
												iz->base.generators[gi++].value = 0;
											}
											/* exclusive class */
											if(rh.key_group > 0) {
												iz->base.generators[gi].type = SS_GEN_EXCLUSIVE_CLASS;
												iz->base.generators[gi++].value = (int16_t)rh.key_group;
											}
											iz->base.gen_count = gi;
										}
										/* Store wave_index temporarily in the sample pointer slot.
										 * We'll resolve it once we have the sample array. */
										iz->sample = (SS_BasicSample *)(uintptr_t)wave_index;
									}
								skip_region:;
								}
							}
						}
					}

					inst->zones = zones;
					inst->zone_count = zone_count;
					snprintf(inst->name, sizeof(inst->name), "Instrument %zu", instr_count - 1);
				}
			}
		}
	}

	/* ── Parse wave pool into samples ─────────────────────────────────────── */
	if(has_waves && pool_offsets) {
		parse_wave_pool(&waves_iba, pool_offsets, pool_count, bank);
	}

	/* ── Build presets from instrument entries ─────────────────────────────── */
	bank->instruments = (SS_BasicInstrument *)calloc(instr_count, sizeof(SS_BasicInstrument));
	bank->instrument_count = instr_count;
	bank->presets = (SS_BasicPreset *)calloc(instr_count, sizeof(SS_BasicPreset));
	bank->preset_count = instr_count;

	for(size_t i = 0; i < instr_count; i++) {
		DLS_InstrEntry *e = &instr_entries[i];
		if(!e->inst) continue;

		/* Copy instrument */
		bank->instruments[i] = *e->inst;
		free(e->inst);

		/* Resolve sample pointers in zones */
		for(size_t z = 0; z < bank->instruments[i].zone_count; z++) {
			SS_InstrumentZone *iz = &bank->instruments[i].zones[z];
			uintptr_t wave_idx = (uintptr_t)iz->sample;
			if(wave_idx < bank->sample_count) {
				/* Allocate a per-zone copy so zones can apply fixups independently.
				 * audio_data / compressed_data / s16le_data remain shared with the
				 * bank sample; only metadata (loop points, root key, etc.) is owned. */
				iz->sample = (SS_BasicSample *)malloc(sizeof(SS_BasicSample));
				if(iz->sample) {
					*iz->sample = bank->samples[wave_idx];
					iz->sample->owns_raw_data = false;
				}
			} else {
				iz->sample = NULL;
			}
		}

		/* Create matching preset */
		SS_BasicPreset *p = &bank->presets[i];
		strncpy(p->name, bank->instruments[i].name, sizeof(p->name) - 1);
		p->program = (uint8_t)(e->patch & 0x7F);
		p->bank_msb = e->bank_msb;
		p->bank_lsb = 0;
		p->is_gm_gs_drum = e->is_drum;
		p->parent_bank = bank;

		/* Create a single preset zone pointing to this instrument */
		p->zones = (SS_PresetZone *)calloc(1, sizeof(SS_PresetZone));
		p->zone_count = 1;
		if(p->zones) {
			p->zones[0].base.key_range_min = -1;
			p->zones[0].base.key_range_max = 127;
			p->zones[0].base.vel_range_min = -1;
			p->zones[0].base.vel_range_max = 127;
			p->zones[0].instrument = &bank->instruments[i];
		}
	}
	free(instr_entries);
	free(pool_offsets);
	return bank;

fail:
	free(pool_offsets);
	free(instr_entries);
	ss_soundbank_free(bank);
	return NULL;
}

/* Parse wave pool: each entry is a RIFF WAVE chunk */
static bool parse_wave_pool(SS_IBA *waves_iba,
                            const uint32_t *pool_offsets,
                            size_t pool_count,
                            SS_SoundBank *bank) {
	bank->samples = (SS_BasicSample *)calloc(pool_count, sizeof(SS_BasicSample));
	bank->sample_count = pool_count;
	if(!bank->samples && pool_count > 0) return false;

	for(size_t i = 0; i < pool_count; i++) {
		uint32_t offset = pool_offsets[i];
		if(offset + 12 > waves_iba->length) continue;

		SS_IBA wave_iba;
		ss_iba_wrap(&wave_iba, waves_iba->data + offset + 4 /* wvpl as indexed */, waves_iba->length - offset);

		SS_RIFFChunk wave_list;
		if(!ss_riff_read_chunk(&wave_iba, &wave_list, false, false)) continue;
		if(strcmp(wave_list.header, "LIST") != 0) continue;

		char list_id[5];
		ss_iba_read_string(&wave_list.data, list_id, 4);
		if(strcmp(list_id, "wave") != 0) continue;

		SS_BasicSample *s = &bank->samples[i];
		snprintf(s->name, sizeof(s->name), "Sample%zu", i);

		uint32_t sample_rate = 44100;
		uint16_t bits_per_sample = 16;
		uint16_t num_channels = 1;
		uint8_t *pcm_data = NULL;
		size_t pcm_len = 0;
		uint32_t loop_start_smpl = 0;
		uint32_t loop_length_smpl = 0;
		bool has_loop = false;

		while(ss_iba_remaining(&wave_list.data) >= 8) {
			SS_RIFFChunk sub;
			if(!ss_riff_read_chunk(&wave_list.data, &sub, false, false)) break;

			if(strcmp(sub.header, "fmt ") == 0) {
				uint16_t fmt_tag = (uint16_t)ss_iba_read_le(&sub.data, 2);
				num_channels = (uint16_t)ss_iba_read_le(&sub.data, 2);
				sample_rate = (uint32_t)ss_iba_read_le(&sub.data, 4);
				ss_iba_read_le(&sub.data, 4); /* byte rate */
				ss_iba_read_le(&sub.data, 2); /* block align */
				bits_per_sample = (uint16_t)ss_iba_read_le(&sub.data, 2);
				(void)fmt_tag;
			} else if(strcmp(sub.header, "data") == 0) {
				pcm_data = sub.data.data;
				pcm_len = sub.data.length;
			} else if(strcmp(sub.header, "wsmp") == 0) {
				DLS_WaveSample ws;
				parse_wsmp(&sub.data, &ws);
				s->original_key = (uint8_t)ws.unity_note;
				s->pitch_correction = (int8_t)(ws.fine_tune / 100);
				if(ws.loop_count > 0) {
					has_loop = true;
					loop_start_smpl = ws.loop_start;
					loop_length_smpl = ws.loop_length;
				}
			}
		}

		if(!pcm_data || pcm_len == 0) continue;

		s->sample_rate = sample_rate;
		s->sample_type = SS_SAMPLE_TYPE_MONO;

		/* Convert PCM to float.  DLS spec says 16-bit signed PCM.
		 * For stereo, take left channel only (mix later if needed). */
		size_t bytes_per_sample = bits_per_sample / 8;
		size_t total_frames = pcm_len / (bytes_per_sample * num_channels);
		uint8_t *out_data = (uint8_t *)malloc(total_frames * bytes_per_sample);
		if(!out_data) continue;

		if(bits_per_sample == 16) {
			const int16_t *src = (const int16_t *)pcm_data;
			int16_t *dest = (int16_t *)out_data;
			for(size_t f = 0; f < total_frames; f++) {
				dest[f] = src[f * num_channels];
			}
			s->s16le_data = out_data;
			s->s16le_length = total_frames * 2;
		} else if(bits_per_sample == 8) {
			for(size_t f = 0; f < total_frames; f++) {
				out_data[f] = pcm_data[f * num_channels];
			}
			s->u8_data = out_data;
			s->u8_length = total_frames;
		}

		if(has_loop) {
			s->loop_start = loop_start_smpl;
			s->loop_end = loop_start_smpl + loop_length_smpl;
			if(s->loop_end > (uint32_t)total_frames) s->loop_end = (uint32_t)total_frames;
		}

		/* DLS samples own their decoded audio_data directly (no lazy decode path). */
		s->owns_raw_data = true;
	}
	return true;
}
