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

static int stb_vorbis_fgetc(void *context) {
	SS_File *file = (SS_File *)context;
	if(ss_file_remaining(file) < 1) {
		return EOF;
	}
	return ss_file_read_u8(file, ss_file_tell(file));
}

static int stb_vorbis_fread(void *out, size_t unit, size_t count, void *context) {
	SS_File *file = (SS_File *)context;
	const size_t total_bytes = unit * count;
	const size_t offset = ss_file_tell(file);
	ss_file_read_bytes(file, offset, (uint8_t *)out, total_bytes);
	const size_t bytes_read = ss_file_tell(file) - offset;
	return (int)(bytes_read / unit);
}

static int stb_vorbis_fseek(void *context, ssize_t offset, int mode) {
	SS_File *file = (SS_File *)context;
	switch(mode) {
		case SEEK_SET:
			break;
		case SEEK_CUR:
			offset += ss_file_tell(file);
			break;

		case SEEK_END:
			offset += ss_file_size(file);
			break;
	}
	ss_file_seek(file, offset);
	return 0;
}

static size_t stb_vorbis_ftell(void *context) {
	SS_File *file = (SS_File *)context;
	return ss_file_tell(file);
}

bool ss_vorbis_decode(SS_BasicSample *s) {
	if(!s->audio_file) return false;

	int channels = 0;
	int sample_rate = 0;
	float *pcm = NULL;

	stb_vorbis_file file_callbacks = {
		.fgetc = &stb_vorbis_fgetc,
		.fread = &stb_vorbis_fread,
		.fseek = &stb_vorbis_fseek,
		.ftell = &stb_vorbis_ftell
	};

	ss_file_seek(s->audio_file, 0);

	int n_samples = stb_vorbis_decode_file_callbacks_float_range(
	&file_callbacks,
	s->audio_file,
	(unsigned int)s->audio_file_sample_offset,
	(unsigned int)s->audio_file_sample_count,
	&channels,
	&sample_rate,
	&pcm);

	if(n_samples < 0 || !pcm) return false;

	/* Convert int16 -> float, mixdown to mono if stereo */
	s->audio_data = pcm;
	s->audio_data_length = (size_t)n_samples;
	s->sample_rate = (uint32_t)sample_rate;

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
