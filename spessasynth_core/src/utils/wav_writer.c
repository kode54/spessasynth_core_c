#include <math.h>
#include <stdlib.h>
#include <string.h>
#if __has_include(<spessasynth_core/spessasynth.h>)
#include <spessasynth_core/spessasynth.h>
#else
#include "spessasynth/spessasynth.h"
#endif

/* Write a 4-character FourCC into a buffer */
static void write_cc(uint8_t *p, const char *s) {
	memcpy(p, s, 4);
}

/* Write LE uint16/uint32 */
static void write_le16(uint8_t *p, uint16_t v) {
	p[0] = v & 0xFF;
	p[1] = (v >> 8) & 0xFF;
}
static void write_le32(uint8_t *p, uint32_t v) {
	p[0] = v & 0xFF;
	p[1] = (v >> 8) & 0xFF;
	p[2] = (v >> 16) & 0xFF;
	p[3] = (v >> 24) & 0xFF;
}

/* Clamp float to int16 */
static int16_t float_to_s16(float f) {
	int v = (int)(f * 32767.5f);
	if(v > 32767) v = 32767;
	if(v < -32768) v = -32768;
	return (int16_t)v;
}

bool ss_wav_write(const float *const *channels,
                  uint32_t num_channels,
                  uint32_t num_samples,
                  uint32_t sample_rate,
                  const SS_WavWriteOptions *opts,
                  uint8_t **out_data,
                  size_t *out_size) {
	if(!channels || num_channels == 0 || num_samples == 0) return false;

	/* Optional normalization */
	float normalize_gain = 1.0f;
	if(opts && opts->normalize_audio) {
		float peak = 0.0f;
		for(uint32_t c = 0; c < num_channels; c++) {
			for(uint32_t s = 0; s < num_samples; s++) {
				float abs_val = fabsf(channels[c][s]);
				if(abs_val > peak) peak = abs_val;
			}
		}
		if(peak > 0.0001f) normalize_gain = 1.0f / peak;
	}

	uint32_t bytes_per_sample = 2; /* 16-bit PCM */
	uint32_t data_size = num_samples * num_channels * bytes_per_sample;

	/* Build a minimal LIST/INFO chunk if text metadata was provided */
	uint8_t info_buf[1024];
	size_t info_len = 0;
	if(opts && (opts->title[0] || opts->artist[0] || opts->album[0] || opts->comment[0])) {
		uint8_t *p = info_buf;
		/* LIST header placeholder (fill in later) */
		/*size_t list_hdr_pos = 0;*/ /* p - info_buf */
		write_cc(p, "LIST");
		p += 4;
		p += 4; /* size placeholder */
		write_cc(p, "INFO");
		p += 4;

/* Helper: write a zero-terminated INFO sub-chunk */
#define WRITE_INFO(fourcc, str)                  \
	do {                                         \
		size_t slen = strlen(str);               \
		size_t chunk_len = slen + 1 /* NUL */;   \
		if(chunk_len & 1) chunk_len++; /* pad */ \
		write_cc(p, (fourcc));                   \
		p += 4;                                  \
		write_le32(p, (uint32_t)(slen + 1));     \
		p += 4;                                  \
		memcpy(p, (str), slen);                  \
		p += slen;                               \
		*p++ = 0;                                \
		if((slen + 1) & 1) *p++ = 0;             \
	} while(0)

		if(opts->title[0]) WRITE_INFO("INAM", opts->title);
		if(opts->artist[0]) WRITE_INFO("IART", opts->artist);
		if(opts->album[0]) WRITE_INFO("IPRD", opts->album);
		if(opts->comment[0]) WRITE_INFO("ICMT", opts->comment);

#undef WRITE_INFO

		info_len = (size_t)(p - info_buf);
		uint32_t list_data_size = (uint32_t)(info_len - 8);
		write_le32(info_buf + 4, list_data_size);
	}

	/* smpl chunk for loop points */
	uint8_t smpl_buf[72];
	size_t smpl_len = 0;
	if(opts && opts->loop_end_seconds > opts->loop_start_seconds) {
		uint32_t loop_start_sample = (uint32_t)(opts->loop_start_seconds * (float)sample_rate);
		uint32_t loop_end_sample = (uint32_t)(opts->loop_end_seconds * (float)sample_rate);
		uint8_t *p = smpl_buf;
		write_cc(p, "smpl");
		p += 4;
		write_le32(p, 60);
		p += 4; /* chunk size */
		write_le32(p, 0);
		p += 4; /* manufacturer */
		write_le32(p, 0);
		p += 4; /* product */
		write_le32(p, 1000000000u / sample_rate);
		p += 4; /* sample period ns */
		write_le32(p, 60);
		p += 4; /* MIDI unity note */
		write_le32(p, 0);
		p += 4; /* MIDI pitch fraction */
		write_le32(p, 0);
		p += 4; /* SMPTE format */
		write_le32(p, 0);
		p += 4; /* SMPTE offset */
		write_le32(p, 1);
		p += 4; /* num sample loops */
		write_le32(p, 0);
		p += 4; /* sampler data */
		/* Loop struct */
		write_le32(p, 0);
		p += 4; /* cue point ID */
		write_le32(p, 0);
		p += 4; /* type: forward */
		write_le32(p, loop_start_sample);
		p += 4;
		write_le32(p, loop_end_sample);
		p += 4;
		write_le32(p, 0);
		p += 4; /* fraction */
		write_le32(p, 0);
		p += 4; /* play count (0 = infinite) */
		smpl_len = (size_t)(p - smpl_buf);
	}

	/* Total RIFF size */
	uint32_t fmt_chunk_size = 16;
	uint32_t riff_data_size = 4 /* "WAVE" */
	                          + 8 + fmt_chunk_size /* fmt  */
	                          + 8 + data_size /* data */
	                          + (uint32_t)info_len + (uint32_t)smpl_len;

	size_t total = 8 + riff_data_size;
	uint8_t *buf = (uint8_t *)malloc(total);
	if(!buf) return false;

	uint8_t *p = buf;

	/* RIFF header */
	write_cc(p, "RIFF");
	p += 4;
	write_le32(p, riff_data_size);
	p += 4;
	write_cc(p, "WAVE");
	p += 4;

	/* fmt chunk */
	write_cc(p, "fmt ");
	p += 4;
	write_le32(p, fmt_chunk_size);
	p += 4;
	write_le16(p, 1);
	p += 2; /* PCM */
	write_le16(p, (uint16_t)num_channels);
	p += 2;
	write_le32(p, sample_rate);
	p += 4;
	write_le32(p, sample_rate * num_channels * bytes_per_sample);
	p += 4; /* byte rate */
	write_le16(p, (uint16_t)(num_channels * bytes_per_sample));
	p += 2; /* block align */
	write_le16(p, 16);
	p += 2; /* bits per sample */

	/* data chunk */
	write_cc(p, "data");
	p += 4;
	write_le32(p, data_size);
	p += 4;

	/* Interleave samples */
	for(uint32_t s = 0; s < num_samples; s++) {
		for(uint32_t c = 0; c < num_channels; c++) {
			int16_t sample = float_to_s16(channels[c][s] * normalize_gain);
			p[0] = (uint8_t)(sample & 0xFF);
			p[1] = (uint8_t)((sample >> 8) & 0xFF);
			p += 2;
		}
	}

	/* Append optional chunks */
	if(smpl_len > 0) {
		memcpy(p, smpl_buf, smpl_len);
		p += smpl_len;
	}
	if(info_len > 0) {
		memcpy(p, info_buf, info_len);
		p += info_len;
	}

	(void)p; /* suppress unused-variable warning */
	*out_data = buf;
	*out_size = total;
	return true;
}
