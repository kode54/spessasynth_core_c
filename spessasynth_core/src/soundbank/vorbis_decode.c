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

/* Hacks above and beyond the base STB Vorbis, to implement float reading */
int stb_vorbis_get_frame_float_mono(stb_vorbis *f, int num_c, float *buffer, int num_floats) {
	float **output;
	int len;
	if(num_c == 1) {
		len = stb_vorbis_get_frame_float(f, NULL, &output);
		if(len) {
			memcpy(buffer, output[0], len * sizeof(float));
		}
		return len;
	}
	len = stb_vorbis_get_frame_float(f, NULL, &output);
	if(len) {
		if(len * num_c > num_floats) len = num_floats / num_c;
		for(int i = 0; i < len; i++) {
			float sample = 0;
			for(int j = 0; j < num_c; j++) sample += output[j][i];
			sample /= (float)num_c;
			buffer[i] = sample;
		}
	}
	return len;
}

static int stb_vorbis_decode_memory_float(const uint8 *mem, int len, int *channels, int *sample_rate, float **output) {
	int data_len, offset, total, limit, error;
	float *data;
	stb_vorbis *v = stb_vorbis_open_memory(mem, len, &error, NULL);
	if(v == NULL) return -1;
	limit = 16384;
	*channels = v->channels;
	if(sample_rate)
		*sample_rate = v->sample_rate;
	offset = data_len = 0;
	total = limit;
	data = (float *)malloc(total * sizeof(*data));
	if(data == NULL) {
		stb_vorbis_close(v);
		return -2;
	}
	for(;;) {
		int n = stb_vorbis_get_frame_float_mono(v, v->channels, data + offset, total - offset);
		if(n == 0) break;
		data_len += n;
		offset += n;
		if(offset + limit > total) {
			float *data2;
			total *= 2;
			data2 = (float *)realloc(data, total * sizeof(*data));
			if(data2 == NULL) {
				free(data);
				stb_vorbis_close(v);
				return -2;
			}
			data = data2;
		}
	}
	*output = data;
	stb_vorbis_close(v);
	return data_len;
}

bool ss_vorbis_decode(SS_BasicSample *s) {
	if(!s->audio_file) return false;

	int channels = 0;
	int sample_rate = 0;
	float *pcm = NULL;

	size_t size = ss_file_size(s->audio_file);
	uint8_t *buffer = malloc(size);
	if(!buffer) return false;

	ss_file_read_bytes(s->audio_file, 0, buffer, size);

	int n_samples = stb_vorbis_decode_memory_float(
	buffer,
	(int)size,
	&channels,
	&sample_rate,
	&pcm);

	free(buffer);

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
