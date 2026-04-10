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
#include <stb_vorbis.c>

#include <stdlib.h>
#include <string.h>
#if __has_include(<spessasynth_core/spessasynth.h>)
#include <spessasynth_core/soundbank.h>
#else
#include "spessasynth/soundbank/soundbank.h"
#endif

bool ss_vorbis_decode(SS_BasicSample *s) {
	if(!s->compressed_data || s->compressed_data_length == 0) return false;

	int channels = 0;
	int sample_rate = 0;
	short *pcm = NULL;

	int n_samples = stb_vorbis_decode_memory(
	s->compressed_data,
	(int)s->compressed_data_length,
	&channels,
	&sample_rate,
	&pcm);

	if(n_samples < 0 || !pcm) return false;

	/* Convert int16 -> float, mixdown to mono if stereo */
	s->audio_data = (float *)malloc((size_t)(n_samples + 4) * sizeof(float));
	memset(s->audio_data + n_samples, 0, sizeof(float) * 4); /* add a little bit for interpolators */
	if(!s->audio_data) {
		free(pcm);
		return false;
	}
	s->audio_data_length = (size_t)n_samples;
	s->sample_rate = (uint32_t)sample_rate;

	if(channels == 1) {
		for(int i = 0; i < n_samples; i++)
			s->audio_data[i] = (float)pcm[i] / 32768.0f;
	} else {
		/* Stereo: take channel 0 (left) as the primary channel.
		 * Linked stereo pairs are handled by the SF2 loader. */
		for(int i = 0; i < n_samples; i++)
			s->audio_data[i] = (float)pcm[i * channels] / 32768.0f;
	}

	free(pcm);

	/* Free compressed data now that it's decoded */
	free(s->compressed_data);
	s->compressed_data = NULL;
	s->compressed_data_length = 0;
	return true;
}

#endif /* SS_HAVE_STB_VORBIS */
