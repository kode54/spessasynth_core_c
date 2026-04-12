/**
 * soundfont_reader.c
 * Parses SoundFont2 (SF2), SF3 (Ogg Vorbis compressed), and SF2Pack
 * files into SS_SoundBank.
 *
 * SF2 spec: https://freepats.zenvoid.org/sf2/sfspec24.pdf
 * SF3 format: https://github.com/FluidSynth/fluidsynth/wiki/SoundFont3Format
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if __has_include(<spessasynth_core/spessasynth.h>)
#include <spessasynth_core/indexed_byte_array.h>
#include <spessasynth_core/midi_enums.h>
#include <spessasynth_core/riff_chunk.h>
#include <spessasynth_core/soundbank.h>
#else
#include "spessasynth/midi/midi_enums.h"
#include "spessasynth/soundbank/soundbank.h"
#include "spessasynth/utils/indexed_byte_array.h"
#include "spessasynth/utils/riff_chunk.h"
#endif

/* ── Internal helpers ───────────────────────────────────────────────────── */

static int16_t read_s16le(SS_IBA *iba) {
	uint16_t u = (uint16_t)ss_iba_read_le(iba, 2);
	return (int16_t)u;
}

static int8_t read_s8(SS_IBA *iba) {
	uint8_t b = ss_iba_read_u8(iba);
	return (int8_t)b;
}

/* ── Modulator reader ────────────────────────────────────────────────────── */

static SS_Modulator read_modulator(SS_IBA *iba) {
	SS_Modulator m;
	memset(&m, 0, sizeof(m));
	m.source_enum = (uint16_t)ss_iba_read_le(iba, 2);
	m.dest_enum = (uint16_t)ss_iba_read_le(iba, 2);
	m.transform_amount = read_s16le(iba);
	m.amount_source_enum = (uint16_t)ss_iba_read_le(iba, 2);
	m.transform_type = (uint16_t)ss_iba_read_le(iba, 2);

	/* Mark effect modulators (CC91 reverb / CC93 chorus) */
	/* source_enum bit 7 = is_cc, bits 6-0 = cc_index */
	bool is_cc = (m.source_enum & 0x80) != 0;
	uint8_t idx = m.source_enum & 0x7F;
	if(is_cc && (idx == SS_MIDCON_REVERB_DEPTH || idx == SS_MIDCON_CHORUS_DEPTH))
		m.is_effect_modulator = true;

	/* Mark default resonant modulator (cc74, linear bipolar) */
	if(is_cc && idx == SS_MIDCON_FILTER_RESONANCE)
		m.is_default_resonant_modulator = true;

	/* Mark modulation wheel modulator */
	if(is_cc && idx == SS_MIDCON_MODULATION_WHEEL && (m.dest_enum == SS_GEN_MOD_LFO_TO_PITCH || m.dest_enum == SS_GEN_VIB_LFO_TO_PITCH))
		m.is_mod_wheel_modulator = true;

	return m;
}

/* ── Generator reader ────────────────────────────────────────────────────── */

static SS_Generator read_generator(SS_IBA *iba) {
	SS_Generator g;
	g.type = (SS_GeneratorType)(int16_t)ss_iba_read_le(iba, 2);
	g.value = read_s16le(iba);
	return g;
}

/* ── Zone index reader ───────────────────────────────────────────────────── */

typedef struct {
	uint32_t gen_index;
	uint32_t mod_index;
} ZoneIndex;

static ZoneIndex read_zone_index(SS_IBA *iba) {
	ZoneIndex z;
	z.gen_index = (uint16_t)ss_iba_read_le(iba, 2);
	z.mod_index = (uint16_t)ss_iba_read_le(iba, 2);
	return z;
}

/* ── Sample header reader (SHDR sub-chunk) ───────────────────────────────── */

static void read_sample_header(SS_IBA *iba, SS_BasicSample *s,
                               uint32_t *out_start, uint32_t *out_end,
                               uint32_t *out_link, uint16_t *out_type,
                               SS_IBA *xiba, bool useXdta) {
	char name[41];
	ss_iba_read_string(iba, name, 20);
	if(useXdta) {
		ss_iba_read_string(xiba, name + 20, 20);
		strncpy(s->name, name, 40);
		s->name[40] = '\0';
	} else {
		strncpy(s->name, name, 20);
		s->name[20] = '\0';
	}

	uint32_t sample_start = (uint32_t)ss_iba_read_le(iba, 4);
	uint32_t sample_end = (uint32_t)ss_iba_read_le(iba, 4);
	s->loop_start = (uint32_t)ss_iba_read_le(iba, 4);
	s->loop_end = (uint32_t)ss_iba_read_le(iba, 4);
	s->sample_rate = (uint32_t)ss_iba_read_le(iba, 4);
	uint8_t pitch = ss_iba_read_u8(iba);
	s->original_key = (pitch > 127) ? 60 : pitch;
	s->pitch_correction = read_s8(iba);

	/* Skip the extensions for the above */
	if(useXdta)
		xiba->current_index += 22;

	*out_link = (uint16_t)ss_iba_read_le(iba, 2);
	if(useXdta)
		*out_link |= (uint32_t)ss_iba_read_le(xiba, 2) * 65536;

	*out_type = (uint16_t)ss_iba_read_le(iba, 2);
	if(useXdta)
		xiba->current_index += 2;

	*out_start = sample_start;
	*out_end = sample_end;
}

/* ── Main SF2 loader ─────────────────────────────────────────────────────── */

bool ss_vorbis_decode(SS_BasicSample *s);
bool ss_flac_decode(SS_BasicSample *s);

SS_SoundBank *ss_soundfont_load(const uint8_t *data, size_t size, bool riff64) {
	SS_IBA main_iba;
	ss_iba_wrap(&main_iba, data, size);

	SS_SoundBank *bank = ss_soundbank_new();
	if(!bank) return NULL;

	/* Read top-level RIFF chunk */
	SS_RIFFChunk riff;
	if(!ss_riff_read_chunk(&main_iba, &riff, true, riff64)) goto fail;

	bool is_sf2pack = false;
	char type_id[5];
	ss_iba_read_string(&riff.data, type_id, 4);
	if(strncmp(type_id, "sfpk", 4) == 0)
		is_sf2pack = true;
	else if(strncmp(type_id, "sfbk", 4) != 0 &&
	        strncmp(type_id, "sfen", 4) != 0)
		goto fail;

	/* ── INFO chunk ─────────────────────────────────────────────────────── */
	SS_RIFFChunk info_chunk;
	if(!ss_riff_read_chunk(&riff.data, &info_chunk, false, riff64)) goto fail;
	if(strncasecmp(info_chunk.header, "list", 4) != 0) goto fail;

	char info_id[5];
	ss_iba_read_string(&info_chunk.data, info_id, 4);
	if(strcmp(info_id, "INFO") != 0) goto fail;
	/* Should be "INFO" */

	SS_RIFFChunk xdta;
	SS_RIFFChunk isfe;
	memset(&xdta, 0, sizeof(SS_RIFFChunk));
	memset(&isfe, 0, sizeof(SS_RIFFChunk));

	while(ss_iba_remaining(&info_chunk.data) >= (8 + riff64 * 4)) {
		SS_RIFFChunk sub;
		if(!ss_riff_read_chunk(&info_chunk.data, &sub, false, riff64)) break;
		if(strcmp(sub.header, "INAM") == 0 || strcmp(sub.header, "inam") == 0) {
			size_t n = sub.data.length < 256 ? sub.data.length : 256;
			memcpy(bank->name, sub.data.data, n);
			bank->name[n] = '\0';
		} else if(strcmp(sub.header, "isng") == 0 || strcmp(sub.header, "ISNG") == 0) {
			size_t n = sub.data.length < 256 ? sub.data.length : 256;
			memcpy(bank->sound_engine, sub.data.data, n);
		} else if(strcmp(sub.header, "ISFT") == 0 || strcmp(sub.header, "isft") == 0) {
			size_t n = sub.data.length < 256 ? sub.data.length : 256;
			memcpy(bank->software, sub.data.data, n);
		} else if(strcmp(sub.header, "ifil") == 0) {
			bank->version.major = (uint16_t)ss_iba_read_le(&sub.data, 2);
			bank->version.minor = (uint16_t)ss_iba_read_le(&sub.data, 2);
		} else if(strcmp(sub.header, "DMOD") == 0) {
			/* Custom default modulators */
			size_t n_mods = sub.data.length / 10;
			if(n_mods > 0) {
				bank->default_modulators = (SS_Modulator *)malloc(n_mods * sizeof(SS_Modulator));
				if(bank->default_modulators) {
					for(size_t m = 0; m < n_mods; m++)
						bank->default_modulators[m] = read_modulator(&sub.data);
					bank->default_mod_count = n_mods;
					bank->custom_default_modulators = true;
				}
			}
		} else if(strcmp(sub.header, "LIST") == 0) {
			// possible xdta
			char list_id[5];
			ss_iba_read_string(&sub.data, list_id, 4);
			if(strcmp(list_id, "xdta") == 0) {
				xdta = sub;
			} else if(strcmp(list_id, "ISFe") == 0) {
				isfe = sub;
			}
		}
	}

	/* ── sdta chunk (sample data) ────────────────────────────────────────── */
	SS_RIFFChunk sdta;
	if(!ss_riff_read_chunk(&riff.data, &sdta, false, riff64)) goto fail;

	/* smpl chunk is inside sdta */
	char sdta_id[5];
	ss_iba_read_string(&sdta.data, sdta_id, 4);
	if(strcmp(sdta_id, "sdta") != 0) goto fail;

	SS_IBA smpl_data;
	memset(&smpl_data, 0, sizeof(smpl_data));
	bool smpl_is_float32 = false; /* SF2Pack decoded float */

	SS_BasicSample sf2pack_samples;
	memset(&sf2pack_samples, 0, sizeof(sf2pack_samples));

	while(ss_iba_remaining(&sdta.data) >= (8 + riff64 * 4)) {
		SS_RIFFChunk sub;
		if(!ss_riff_read_chunk(&sdta.data, &sub, false, riff64)) break;
		if(strcmp(sub.header, "smpl") == 0) {
			if(is_sf2pack) {
#if defined(SS_HAVE_STB_VORBIS) || defined(SS_HAVE_LIBFLAC)
				/* SF2Pack: smpl is a single Ogg Vorbis stream covering all samples */
				/* We decode it and get a Float32 array */
				/* For now, copy raw compressed data; decode lazily */
				const uint8_t *hdr = sub.data.data;
				if(sub.data.length >= 4) {
					if(hdr[0] == 'O' && hdr[1] == 'g' && hdr[2] == 'g' && hdr[3] == 'S') {
#ifdef SS_HAVE_STB_VORBIS
						sf2pack_samples.compressed_data = sub.data.data;
						sf2pack_samples.compressed_data_length = sub.data.length;
						if(ss_vorbis_decode(&sf2pack_samples)) {
							smpl_data.data = (uint8_t *)sf2pack_samples.audio_data;
							smpl_data.length = sf2pack_samples.audio_data_length * sizeof(float);
							smpl_data.owns_data = true;
							smpl_is_float32 = true;
						}
#endif
					}
					if(hdr[0] == 'f' && hdr[1] == 'L' && hdr[2] == 'a' && hdr[3] == 'C') {
#ifdef SS_HAVE_LIBFLAC
						sf2pack_samples.compressed_data = sub.data.data;
						sf2pack_samples.compressed_data_length = sub.data.length;
						if(ss_flac_decode(&sf2pack_samples)) {
							smpl_data.data = (uint8_t *)sf2pack_samples.audio_data;
							smpl_data.length = sf2pack_samples.audio_data_length * sizeof(float);
							smpl_data.owns_data = true;
							smpl_is_float32 = true;
						}
#endif
					}
				}

#else
				goto fail;
#endif
			} else {
				smpl_data = sub.data;
			}
		}
		/* sm24 (24-bit extension) is ignored — we only handle 16-bit */
	}

	/* ── pdta chunk (preset/instrument/sample headers) ───────────────────── */
	SS_RIFFChunk pdta;
	if(!ss_riff_read_chunk(&riff.data, &pdta, false, riff64)) goto fail;
	char pdta_id[5];
	ss_iba_read_string(&pdta.data, pdta_id, 4);
	if(strcmp(pdta_id, "pdta") != 0) goto fail;

	/* Collect all sub-chunks of pdta */
	SS_RIFFChunk phdr_c, pbag_c, pmod_c, pgen_c;
	SS_RIFFChunk inst_c, ibag_c, imod_c, igen_c;
	SS_RIFFChunk shdr_c;
	memset(&phdr_c, 0, sizeof(SS_RIFFChunk));
	memset(&pbag_c, 0, sizeof(SS_RIFFChunk));
	memset(&pmod_c, 0, sizeof(SS_RIFFChunk));
	memset(&pgen_c, 0, sizeof(SS_RIFFChunk));
	memset(&inst_c, 0, sizeof(SS_RIFFChunk));
	memset(&ibag_c, 0, sizeof(SS_RIFFChunk));
	memset(&imod_c, 0, sizeof(SS_RIFFChunk));
	memset(&igen_c, 0, sizeof(SS_RIFFChunk));
	memset(&shdr_c, 0, sizeof(SS_RIFFChunk));

	while(ss_iba_remaining(&pdta.data) >= (8 + riff64 * 4)) {
		SS_RIFFChunk sub;
		if(!ss_riff_read_chunk(&pdta.data, &sub, false, riff64)) break;
		if(strcmp(sub.header, "phdr") == 0)
			phdr_c = sub;
		else if(strcmp(sub.header, "pbag") == 0)
			pbag_c = sub;
		else if(strcmp(sub.header, "pmod") == 0)
			pmod_c = sub;
		else if(strcmp(sub.header, "pgen") == 0)
			pgen_c = sub;
		else if(strcmp(sub.header, "inst") == 0)
			inst_c = sub;
		else if(strcmp(sub.header, "ibag") == 0)
			ibag_c = sub;
		else if(strcmp(sub.header, "imod") == 0)
			imod_c = sub;
		else if(strcmp(sub.header, "igen") == 0)
			igen_c = sub;
		else if(strcmp(sub.header, "shdr") == 0)
			shdr_c = sub;
	}

	/* Collect all sub-chunks of xdta, if present */
	SS_RIFFChunk xphdr_c, xpbag_c, xpmod_c, xpgen_c;
	SS_RIFFChunk xinst_c, xibag_c, ximod_c, xigen_c;
	SS_RIFFChunk xshdr_c;
	memset(&xphdr_c, 0, sizeof(SS_RIFFChunk));
	memset(&xpbag_c, 0, sizeof(SS_RIFFChunk));
	memset(&xpmod_c, 0, sizeof(SS_RIFFChunk));
	memset(&xpgen_c, 0, sizeof(SS_RIFFChunk));
	memset(&xinst_c, 0, sizeof(SS_RIFFChunk));
	memset(&xibag_c, 0, sizeof(SS_RIFFChunk));
	memset(&ximod_c, 0, sizeof(SS_RIFFChunk));
	memset(&xigen_c, 0, sizeof(SS_RIFFChunk));
	memset(&xshdr_c, 0, sizeof(SS_RIFFChunk));
	while(ss_iba_remaining(&xdta.data) >= (8 + riff64 * 4)) {
		SS_RIFFChunk sub;
		if(!ss_riff_read_chunk(&xdta.data, &sub, false, riff64)) break;
		if(strcmp(sub.header, "phdr") == 0)
			xphdr_c = sub;
		else if(strcmp(sub.header, "pbag") == 0)
			xpbag_c = sub;
		else if(strcmp(sub.header, "pmod") == 0)
			xpmod_c = sub;
		else if(strcmp(sub.header, "pgen") == 0)
			xpgen_c = sub;
		else if(strcmp(sub.header, "inst") == 0)
			xinst_c = sub;
		else if(strcmp(sub.header, "ibag") == 0)
			xibag_c = sub;
		else if(strcmp(sub.header, "imod") == 0)
			ximod_c = sub;
		else if(strcmp(sub.header, "igen") == 0)
			xigen_c = sub;
		else if(strcmp(sub.header, "shdr") == 0)
			xshdr_c = sub;
	}

	/* ── Parse generators ────────────────────────────────────────────────── */
	size_t pgen_count = pgen_c.data.length / 4;
	SS_Generator *pgens = (SS_Generator *)malloc((pgen_count + 1) * sizeof(SS_Generator));
	if(!pgens && pgen_count > 0) goto fail;
	for(size_t i = 0; i < pgen_count; i++)
		pgens[i] = read_generator(&pgen_c.data);

	size_t igen_count = igen_c.data.length / 4;
	SS_Generator *igens = (SS_Generator *)malloc((igen_count + 1) * sizeof(SS_Generator));
	if(!igens && igen_count > 0) {
		free(pgens);
		goto fail;
	}
	for(size_t i = 0; i < igen_count; i++)
		igens[i] = read_generator(&igen_c.data);

	/* ── Parse modulators ────────────────────────────────────────────────── */
	size_t pmod_count = pmod_c.data.length / 10;
	SS_Modulator *pmods = (SS_Modulator *)malloc((pmod_count + 1) * sizeof(SS_Modulator));
	if(!pmods && pmod_count > 0) {
		free(pgens);
		free(igens);
		goto fail;
	}
	for(size_t i = 0; i < pmod_count; i++)
		pmods[i] = read_modulator(&pmod_c.data);

	size_t imod_count = imod_c.data.length / 10;
	SS_Modulator *imods = (SS_Modulator *)malloc((imod_count + 1) * sizeof(SS_Modulator));
	if(!imods && imod_count > 0) {
		free(pgens);
		free(igens);
		free(pmods);
		goto fail;
	}
	for(size_t i = 0; i < imod_count; i++)
		imods[i] = read_modulator(&imod_c.data);

	/* ── Parse zone index arrays (pbag / ibag) ───────────────────────────── */
	size_t pbag_count = pbag_c.data.length / 4;
	ZoneIndex *pbags = (ZoneIndex *)malloc((pbag_count + 1) * sizeof(ZoneIndex));
	if(!pbags && pbag_count > 0) goto fail_mods;
	for(size_t i = 0; i < pbag_count; i++) {
		pbags[i] = read_zone_index(&pbag_c.data);
	}

	size_t xpbag_count = xpbag_c.data.length / 4;
	if(xpbag_count && xpbag_count == pbag_count) {
		for(size_t i = 0; i < pbag_count; i++) {
			ZoneIndex xp = read_zone_index(&xpbag_c.data);
			pbags[i].gen_index |= xp.gen_index * 65536;
			pbags[i].mod_index |= xp.mod_index * 65536;
		}
	}

	size_t ibag_count = ibag_c.data.length / 4;
	ZoneIndex *ibags = (ZoneIndex *)malloc((ibag_count + 1) * sizeof(ZoneIndex));
	if(!ibags && ibag_count > 0) {
		free(pbags);
		goto fail_mods;
	}
	for(size_t i = 0; i < ibag_count; i++)
		ibags[i] = read_zone_index(&ibag_c.data);

	size_t xibag_count = xibag_c.data.length / 4;
	if(xibag_count && xibag_count == ibag_count) {
		for(size_t i = 0; i < ibag_count; i++) {
			ZoneIndex xi = read_zone_index(&xibag_c.data);
			ibags[i].gen_index |= xi.gen_index * 65536;
			ibags[i].mod_index |= xi.mod_index * 65536;
		}
	}

	/* ── Parse samples (shdr) ────────────────────────────────────────────── */
	size_t shdr_entry_size = 46; /* SF2 spec */
	size_t n_samples = shdr_c.data.length / shdr_entry_size;
	if(n_samples > 0) n_samples--; /* Remove EOS sentinel */

	bank->samples = (SS_BasicSample *)calloc(n_samples, sizeof(SS_BasicSample));
	bank->sample_count = n_samples;
	if(!bank->samples && n_samples > 0) goto fail_bags;

	bool has_xsamples = false;
	size_t n_xsamples = xshdr_c.data.length / shdr_entry_size;
	if(n_xsamples > 0) n_xsamples--; /* Remove EOS sentinel */
	if(n_xsamples && n_xsamples == n_samples) has_xsamples = true;

	uint32_t *sample_starts = (uint32_t *)malloc(n_samples * sizeof(uint32_t));
	uint32_t *sample_ends = (uint32_t *)malloc(n_samples * sizeof(uint32_t));
	uint32_t *sample_links = (uint32_t *)malloc(n_samples * sizeof(uint32_t));
	if((!sample_starts || !sample_ends || !sample_links) && n_samples > 0) {
		free(sample_starts);
		free(sample_ends);
		free(sample_links);
		goto fail_bags;
	}

	for(size_t i = 0; i < n_samples; i++) {
		uint16_t stype;
		read_sample_header(&shdr_c.data, &bank->samples[i],
		                   &sample_starts[i], &sample_ends[i],
		                   &sample_links[i], &stype,
		                   &xshdr_c.data, has_xsamples);
		bool compressed = (stype & SS_SF3_COMPRESSED_FLAG) != 0;
		bank->samples[i].sample_type = (SS_SampleType)(stype & ~SS_SF3_COMPRESSED_FLAG);
		bank->samples[i].is_compressed = compressed;
		bank->samples[i].owns_raw_data = true;
		bank->samples[i].is_sf2pack = false;

		if(smpl_data.data && smpl_data.length > 0) {
			uint32_t byte_start = sample_starts[i] * 2;
			uint32_t byte_end = sample_ends[i] * 2;
			if(byte_end > smpl_data.length) byte_end = (uint32_t)smpl_data.length;
			bank->samples[i].loop_start -= byte_start / 2;
			bank->samples[i].loop_end -= byte_start / 2;

			if(compressed) {
				/* SF3: copy compressed data slice */
				size_t clen = (byte_end > byte_start) ? (byte_end - byte_start) / 2 : 0;
				size_t offset = byte_start / 2;
				bank->samples[i].loop_start += byte_start / 2;
				bank->samples[i].loop_end += byte_start / 2;
				if(offset + clen <= smpl_data.length) {
					bank->samples[i].compressed_data = (uint8_t *)malloc(clen);
					if(bank->samples[i].compressed_data) {
						memcpy(bank->samples[i].compressed_data,
						       smpl_data.data + offset, clen);
						bank->samples[i].compressed_data_length = clen;
					}
				}
			} else if(!smpl_is_float32) {
				/* SF2: process raw s16le slice later */
				if(byte_end > smpl_data.length) byte_end = (uint32_t)smpl_data.length;
				size_t slen = (byte_end > byte_start) ? (byte_end - byte_start) : 0;
				bank->samples[i].s16le_data = (uint8_t *)malloc(slen + 1);
				if(bank->samples[i].s16le_data) {
					memcpy(bank->samples[i].s16le_data,
					       smpl_data.data + byte_start, slen);
					bank->samples[i].s16le_length = slen;
				}
			} else {
				/* SF2Pack: globally compressed to a single chunk, decoded to float already */
				byte_start *= 2;
				byte_end *= 2;
				bank->samples[i].loop_start += byte_start / 4;
				bank->samples[i].loop_end += byte_start / 4;
				if(byte_end > smpl_data.length) byte_end = (uint32_t)smpl_data.length;
				size_t slen = (byte_end > byte_start) ? (byte_end - byte_start) : 0;
				bank->samples[i].compressed_data = (uint8_t *)malloc(slen + 4 * sizeof(float));
				if(bank->samples[i].compressed_data) {
					memcpy(bank->samples[i].compressed_data,
					       smpl_data.data + byte_start, slen);
					bank->samples[i].compressed_data_length = slen;
					memset(bank->samples[i].compressed_data + slen, 0, 4 * sizeof(float));
					bank->samples[i].is_compressed = true;
					bank->samples[i].is_sf2pack = true;
				}
			}
		}
	}

	if(smpl_is_float32) {
		free(smpl_data.data);
		smpl_data.data = NULL;
		smpl_data.owns_data = false;
	}

	/* Fix sample loop points and link stereo pairs */
	for(size_t i = 0; i < n_samples; i++) {
		uint32_t link_idx = sample_links[i];
		SS_SampleType st = bank->samples[i].sample_type;
		if((st == SS_SAMPLE_TYPE_LEFT || st == SS_SAMPLE_TYPE_RIGHT) &&
		   link_idx < n_samples && link_idx != i) {
			bank->samples[i].linked_sample = &bank->samples[link_idx];
		}
	}
	free(sample_starts);
	free(sample_ends);
	free(sample_links);

	/* ── Parse instruments (inst) ────────────────────────────────────────── */
	size_t n_insts = inst_c.data.length / 22;
	if(n_insts > 0) n_insts--; /* EOS */
	bank->instruments = (SS_BasicInstrument *)calloc(n_insts, sizeof(SS_BasicInstrument));
	bank->instrument_count = n_insts;
	if(!bank->instruments && n_insts > 0) goto fail_bags;

	bool has_xinsts = false;
	size_t n_xinsts = xinst_c.data.length / 22;
	if(n_xinsts > 0) n_xinsts--; /* EOS */
	if(n_xinsts && n_xinsts == n_insts) has_xinsts = true;

	uint32_t *inst_bag_indexes = (uint32_t *)malloc((n_insts + 1) * sizeof(uint32_t));
	if(!inst_bag_indexes && n_insts > 0) goto fail_bags;

	for(size_t i = 0; i <= n_insts; i++) {
		char iname[41];
		ss_iba_read_string(&inst_c.data, iname, 20);
		if(has_xinsts)
			ss_iba_read_string(&xinst_c.data, iname + 20, 20);
		uint32_t bag_idx = (uint16_t)ss_iba_read_le(&inst_c.data, 2);
		if(has_xinsts)
			bag_idx |= (uint32_t)ss_iba_read_le(&xinst_c.data, 2) * 65536;
		if(i < n_insts) {
			const int namelen = has_xinsts ? 40 : 20;
			strncpy(bank->instruments[i].name, iname, namelen);
			bank->instruments[i].name[namelen] = '\0';
		}
		inst_bag_indexes[i] = bag_idx;
	}

	/* Fill instrument zones */
	for(size_t ii = 0; ii < n_insts; ii++) {
		uint32_t bag_start = inst_bag_indexes[ii];
		uint32_t bag_end = inst_bag_indexes[ii + 1];
		int zone_count = (int)bag_end - (int)bag_start;
		if(zone_count <= 0) continue;

		/* First zone is global if it has no sampleID generator */
		int zone_offset = 0;
		bool has_global = false;
		{
			uint32_t gs = ibags[bag_start].gen_index;
			uint32_t ge = (bag_start + 1 < ibag_count) ? ibags[bag_start + 1].gen_index : (uint32_t)igen_count;
			for(uint32_t g = gs; g < ge; g++) {
				if(igens[g].type == SS_GEN_SAMPLE_ID) goto not_global_inst;
			}
			has_global = true;
		not_global_inst:;
		}

		if(has_global) {
			/* Parse global zone */
			SS_Zone *gz = &bank->instruments[ii].global_zone;
			uint32_t gs = ibags[bag_start].gen_index;
			uint32_t ge = ibags[bag_start + 1].gen_index;
			uint32_t ms = ibags[bag_start].mod_index;
			uint32_t me = ibags[bag_start + 1].mod_index;
			gz->gen_count = ge - gs;
			gz->generators = (SS_Generator *)malloc(gz->gen_count * sizeof(SS_Generator));
			if(gz->generators) memcpy(gz->generators, igens + gs, gz->gen_count * sizeof(SS_Generator));
			gz->mod_count = me - ms;
			gz->modulators = (SS_Modulator *)malloc(gz->mod_count * sizeof(SS_Modulator));
			if(gz->modulators) memcpy(gz->modulators, imods + ms, gz->mod_count * sizeof(SS_Modulator));
			gz->key_range_min = -1;
			gz->key_range_max = -1;
			gz->vel_range_min = -1;
			gz->vel_range_max = -1;
			zone_offset = 1;
		}

		int actual_zone_count = zone_count - zone_offset;
		if(actual_zone_count <= 0) continue;
		bank->instruments[ii].zones = (SS_InstrumentZone *)calloc(
		(size_t)actual_zone_count, sizeof(SS_InstrumentZone));
		bank->instruments[ii].zone_count = (size_t)actual_zone_count;
		if(!bank->instruments[ii].zones) continue;

		for(int zi = 0; zi < actual_zone_count; zi++) {
			uint32_t bag_idx = (uint32_t)(bag_start + zone_offset + zi);
			uint32_t gs = ibags[bag_idx].gen_index;
			uint32_t ge = (bag_idx + 1 < ibag_count) ? ibags[bag_idx + 1].gen_index : (uint32_t)igen_count;
			uint32_t ms = ibags[bag_idx].mod_index;
			uint32_t me = (bag_idx + 1 < ibag_count) ? ibags[bag_idx + 1].mod_index : (uint32_t)imod_count;

			SS_InstrumentZone *iz = &bank->instruments[ii].zones[zi];
			iz->base.key_range_min = -1;
			iz->base.key_range_max = 127;
			iz->base.vel_range_min = -1;
			iz->base.vel_range_max = 127;

			/* Parse generators; watch for keyRange / velRange / sampleID */
			iz->base.gen_count = ge - gs;
			iz->base.generators = (SS_Generator *)malloc(iz->base.gen_count * sizeof(SS_Generator));
			if(iz->base.generators) {
				memcpy(iz->base.generators, igens + gs, iz->base.gen_count * sizeof(SS_Generator));
				for(size_t g = 0; g < iz->base.gen_count; g++) {
					SS_Generator *gen = &iz->base.generators[g];
					if(gen->type == SS_GEN_KEY_RANGE) {
						iz->base.key_range_min = gen->value & 0xFF;
						iz->base.key_range_max = (gen->value >> 8) & 0xFF;
					} else if(gen->type == SS_GEN_VEL_RANGE) {
						iz->base.vel_range_min = gen->value & 0xFF;
						iz->base.vel_range_max = (gen->value >> 8) & 0xFF;
					} else if(gen->type == SS_GEN_SAMPLE_ID) {
						uint16_t sid = (uint16_t)gen->value;
						if(sid < bank->sample_count) {
							iz->sample = (SS_BasicSample *)malloc(sizeof(SS_BasicSample));
							if(iz->sample) {
								*iz->sample = bank->samples[sid];
								/* Duplicate the compressed data, if necessary */
								if(iz->sample->compressed_data) {
									iz->sample->compressed_data = (uint8_t *)malloc(iz->sample->compressed_data_length);
									if(iz->sample->compressed_data)
										memcpy(iz->sample->compressed_data, bank->samples[sid].compressed_data, iz->sample->compressed_data_length);
								}
								iz->sample->owns_raw_data = !!iz->sample->compressed_data;
							}
						}
					}
				}
			}

			/* Parse modulators */
			iz->base.mod_count = me - ms;
			iz->base.modulators = (SS_Modulator *)malloc(iz->base.mod_count * sizeof(SS_Modulator));
			if(iz->base.modulators)
				memcpy(iz->base.modulators, imods + ms, iz->base.mod_count * sizeof(SS_Modulator));
		}
	}
	free(inst_bag_indexes);

	/* ── Parse presets (phdr) ────────────────────────────────────────────── */
	size_t n_presets = phdr_c.data.length / 38;
	if(n_presets > 0) n_presets--; /* EOP sentinel */
	bank->presets = (SS_BasicPreset *)calloc(n_presets, sizeof(SS_BasicPreset));
	bank->preset_count = n_presets;
	if(!bank->presets && n_presets > 0) goto fail_bags;

	uint32_t *preset_bag_indexes = (uint32_t *)malloc((n_presets + 1) * sizeof(uint32_t));
	if(!preset_bag_indexes && n_presets > 0) goto fail_bags;

	bool has_xpresets = false;
	size_t n_xpresets = xphdr_c.data.length / 38;
	if(n_xpresets > 0) n_xpresets--; /* EOP sentinel */
	if(n_xpresets && n_xpresets == n_presets) has_xpresets = true;

	for(size_t pi = 0; pi <= n_presets; pi++) {
		char pname[41];
		ss_iba_read_string(&phdr_c.data, pname, 20);
		if(has_xpresets)
			ss_iba_read_string(&xphdr_c.data, pname + 20, 20);
		uint16_t preset_num = (uint16_t)ss_iba_read_le(&phdr_c.data, 2);
		uint16_t bank_num = (uint16_t)ss_iba_read_le(&phdr_c.data, 2);
		/* Skip these */
		if(has_xpresets)
			xphdr_c.data.current_index += 4;

		uint32_t bag_idx = (uint16_t)ss_iba_read_le(&phdr_c.data, 2);
		if(has_xpresets)
			bag_idx |= (uint32_t)ss_iba_read_le(&xphdr_c.data, 2) * 65536;

		uint32_t library = (uint32_t)ss_iba_read_le(&phdr_c.data, 4);
		uint32_t genre = (uint32_t)ss_iba_read_le(&phdr_c.data, 4);
		uint32_t morphology = (uint32_t)ss_iba_read_le(&phdr_c.data, 4);
		/* Also skip these */
		if(has_xpresets)
			xphdr_c.data.current_index += 12;

		if(pi < n_presets) {
			SS_BasicPreset *p = &bank->presets[pi];
			const int namelen = has_xpresets ? 40 : 20;
			strncpy(p->name, pname, namelen);
			p->name[namelen] = '\0';
			p->program = (uint8_t)preset_num;
			p->bank_msb = bank_num & 0x7f;
			p->bank_lsb = bank_num >> 8;
			p->library = library;
			p->genre = genre;
			p->morphology = morphology;
			p->parent_bank = bank;
			/* GM drum kit: bank 128 */
			p->is_gm_gs_drum = (bank_num & 0x80) > 0;
		}
		preset_bag_indexes[pi] = bag_idx;
	}

	/* Fill preset zones */
	for(size_t pi = 0; pi < n_presets; pi++) {
		uint32_t bag_start = preset_bag_indexes[pi];
		uint32_t bag_end = preset_bag_indexes[pi + 1];
		int zone_count = (int)bag_end - (int)bag_start;
		if(zone_count <= 0) continue;

		/* Check for global zone (first zone has no instrumentID generator) */
		int zone_offset = 0;
		bool has_global = false;
		{
			uint32_t gs = pbags[bag_start].gen_index;
			uint32_t ge = (bag_start + 1 < pbag_count) ? pbags[bag_start + 1].gen_index : (uint32_t)pgen_count;
			for(uint32_t g = gs; g < ge; g++) {
				if(pgens[g].type == SS_GEN_INSTRUMENT) goto not_global_preset;
			}
			has_global = true;
		not_global_preset:;
		}

		if(has_global) {
			SS_Zone *gz = &bank->presets[pi].global_zone;
			uint32_t gs = pbags[bag_start].gen_index;
			uint32_t ge = pbags[bag_start + 1].gen_index;
			uint32_t ms = pbags[bag_start].mod_index;
			uint32_t me = pbags[bag_start + 1].mod_index;
			gz->gen_count = ge - gs;
			gz->generators = (SS_Generator *)malloc(gz->gen_count * sizeof(SS_Generator));
			if(gz->generators) memcpy(gz->generators, pgens + gs, gz->gen_count * sizeof(SS_Generator));
			gz->mod_count = me - ms;
			gz->modulators = (SS_Modulator *)malloc(gz->mod_count * sizeof(SS_Modulator));
			if(gz->modulators) memcpy(gz->modulators, pmods + ms, gz->mod_count * sizeof(SS_Modulator));
			gz->key_range_min = -1;
			gz->key_range_max = -1;
			gz->vel_range_min = -1;
			gz->vel_range_max = -1;
			zone_offset = 1;
		}

		int actual_zones = zone_count - zone_offset;
		if(actual_zones <= 0) continue;
		bank->presets[pi].zones = (SS_PresetZone *)calloc(
		(size_t)actual_zones, sizeof(SS_PresetZone));
		bank->presets[pi].zone_count = (size_t)actual_zones;
		if(!bank->presets[pi].zones) continue;

		for(int zi = 0; zi < actual_zones; zi++) {
			uint32_t bag_idx = (uint32_t)(bag_start + zone_offset + zi);
			uint32_t gs = pbags[bag_idx].gen_index;
			uint32_t ge = (bag_idx + 1 < pbag_count) ? pbags[bag_idx + 1].gen_index : (uint32_t)pgen_count;
			uint32_t ms = pbags[bag_idx].mod_index;
			uint32_t me = (bag_idx + 1 < pbag_count) ? pbags[bag_idx + 1].mod_index : (uint32_t)pmod_count;

			SS_PresetZone *pz = &bank->presets[pi].zones[zi];
			pz->base.key_range_min = -1;
			pz->base.key_range_max = 127;
			pz->base.vel_range_min = -1;
			pz->base.vel_range_max = 127;

			pz->base.gen_count = ge - gs;
			pz->base.generators = (SS_Generator *)malloc(pz->base.gen_count * sizeof(SS_Generator));
			if(pz->base.generators) {
				memcpy(pz->base.generators, pgens + gs, pz->base.gen_count * sizeof(SS_Generator));
				for(size_t g = 0; g < pz->base.gen_count; g++) {
					SS_Generator *gen = &pz->base.generators[g];
					if(gen->type == SS_GEN_KEY_RANGE) {
						pz->base.key_range_min = gen->value & 0xFF;
						pz->base.key_range_max = (gen->value >> 8) & 0xFF;
					} else if(gen->type == SS_GEN_VEL_RANGE) {
						pz->base.vel_range_min = gen->value & 0xFF;
						pz->base.vel_range_max = (gen->value >> 8) & 0xFF;
					} else if(gen->type == SS_GEN_INSTRUMENT) {
						uint16_t iid = (uint16_t)gen->value;
						if(iid < bank->instrument_count)
							pz->instrument = &bank->instruments[iid];
					}
				}
			}
			pz->base.mod_count = me - ms;
			pz->base.modulators = (SS_Modulator *)malloc(pz->base.mod_count * sizeof(SS_Modulator));
			if(pz->base.modulators)
				memcpy(pz->base.modulators, pmods + ms, pz->base.mod_count * sizeof(SS_Modulator));
		}
	}
	free(preset_bag_indexes);

	/* Cleanup temporary arrays */
	free(pgens);
	free(igens);
	free(pmods);
	free(imods);
	free(pbags);
	free(ibags);

	return bank;

fail_bags:
	free(pbags);
	free(ibags);
fail_mods:
	free(pgens);
	free(igens);
	free(pmods);
	free(imods);
fail:
	ss_soundbank_free(bank);
	return NULL;
}
