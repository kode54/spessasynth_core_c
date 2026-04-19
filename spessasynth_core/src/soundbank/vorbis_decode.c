/**
 * vorbis_decode.c
 * Ogg Vorbis sample decoder for SF3 format using stb_vorbis.
 *
 * stb_vorbis is a single-file, public-domain Ogg Vorbis decoder.
 * Download: https://github.com/nothings/stb/blob/master/stb_vorbis.c
 *
 * This file compiles the stb_vorbis implementation in one translation unit
 * (the "unity build" pattern required by stb headers).
 */

#ifdef SS_HAVE_STB_VORBIS

#define STB_VORBIS_NO_PUSHDATA_API /* we use the in-memory API */
#define STB_VORBIS_HEADER_ONLY /* only if we want just the header; remove for impl */
/* To get the implementation we must define this in exactly one .c file: */
#undef STB_VORBIS_HEADER_ONLY
#include <stb_vorbis.h>

#include <stdlib.h>
#include <string.h>
#if __has_include(<spessasynth_core/spessasynth.h>)
#include <spessasynth_core/soundbank.h>
#else
#include "spessasynth/soundbank/soundbank.h"
#endif

typedef struct {
	SS_File *file;
	unsigned long offset, size;
} SS_FileVorbis;

static int stb_vorbis_fgetc(void *context) {
	SS_FileVorbis *file = (SS_FileVorbis *)context;
	if(file->offset == file->size) {
		return EOF;
	}
	return ss_file_read_u8(file->file, file->offset++);
}

static int stb_vorbis_fread(void *out, unsigned long unit, unsigned long count, void *context) {
	SS_FileVorbis *file = (SS_FileVorbis *)context;
	const unsigned long total_bytes = unit * count;
	const unsigned long offset = file->offset;
	ss_file_read_bytes(file->file, offset, (uint8_t *)out, total_bytes);
	file->offset += total_bytes;
	if(file->offset > file->size) {
		file->offset = file->size;
	}
	const unsigned long bytes_read = file->offset - offset;
	return (int)(bytes_read / unit);
}

static int stb_vorbis_fseek(void *context, long offset, int mode) {
	SS_FileVorbis *file = (SS_FileVorbis *)context;
	switch(mode) {
		case SEEK_SET:
			break;
		case SEEK_CUR:
			offset += file->offset;
			break;

		case SEEK_END:
			offset += file->size;
			break;
	}
	int res = 0;
	if(file->offset > file->size) {
		file->offset = file->size;
		res = -1;
	}
	ss_file_seek(file->file, offset);
	return res;
}

static size_t stb_vorbis_ftell(void *context) {
	SS_FileVorbis *file = (SS_FileVorbis *)context;
	return file->offset;
}

bool ss_vorbis_decode(SS_BasicSample *s) {
	if(!s->audio_file) return false;

	int channels = 0;
	int sample_rate = 0;
	float *pcm = NULL;

	bool partial_sample = (s->audio_file_sample_offset > 0) || (s->audio_file_sample_count != ~0ULL);

	stb_vorbis_file file_callbacks = {
		.fgetc = &stb_vorbis_fgetc,
		.fread = &stb_vorbis_fread,
		.fseek = &stb_vorbis_fseek,
		.ftell = &stb_vorbis_ftell
	};

	SS_FileVorbis fileVorbis = {
		.file = s->audio_file,
		.offset = 0,
		.size = ss_file_size(s->audio_file)
	};

	ss_file_seek(s->audio_file, 0);

	int n_samples = stb_vorbis_decode_file_callbacks_float_range(
	&file_callbacks,
	&fileVorbis,
	(unsigned int)s->audio_file_sample_offset,
	(unsigned int)s->audio_file_sample_count,
	&channels,
	&sample_rate,
	&pcm);

	if(n_samples < 0 || !pcm) return false;

	/* Bump the PCM size, for interpolators */
	float *bumped_pcm = realloc(pcm, (n_samples + SS_SAMPLE_COUNT_BUMP) * sizeof(float));
	if(!bumped_pcm) {
		free(pcm);
		return false;
	}
	pcm = bumped_pcm;
	memset(pcm + n_samples, 0, SS_SAMPLE_COUNT_BUMP * sizeof(float));

	/* Convert int16 -> float, mixdown to mono if stereo */
	s->audio_data = pcm;
	s->audio_data_length = (size_t)n_samples;
	if(!partial_sample)	s->sample_rate = (uint32_t)sample_rate;

	/* Free compressed data now that it's decoded */
	if(s->owns_raw_data) {
		free(s->compressed_data);
		ss_file_close(s->audio_file);
	}
	s->compressed_data = NULL;
	s->compressed_data_length = 0;
	s->audio_file = NULL;
	return true;
}

#endif /* SS_HAVE_STB_VORBIS */
