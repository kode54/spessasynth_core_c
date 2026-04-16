/**
 * flac_decode.c
 * libFLAC wrapper for SF3 FLAC-compressed samples.
 * Only compiled when SS_HAVE_LIBFLAC is defined (SS_ENABLE_SF3_FLAC=ON).
 */

#ifdef SS_HAVE_LIBFLAC

#include <FLAC/stream_decoder.h>
#include <stdlib.h>
#include <string.h>
#if __has_include(<spessasynth_core/spessasynth.h>)
#include <spessasynth_core/soundbank.h>
#else
#include "spessasynth/soundbank/soundbank.h"
#endif

/* ── Decoder state ────────────────────────────────────────────────────────── */

typedef struct {
	/* Input */
	SS_File *file;
	size_t offset, size;
	size_t sample_count;

	/* Output (accumulated) */
	float *pcm;
	size_t pcm_frames; /* frames decoded so far */
	size_t pcm_cap; /* allocated frames */

	int channels;
	uint32_t sample_rate;
	uint32_t bits_per_sample;

	bool error;
} FlacState;

/* ── FLAC callbacks ───────────────────────────────────────────────────────── */

static FLAC__StreamDecoderReadStatus flac_read_cb(
const FLAC__StreamDecoder *dec,
FLAC__byte *buf, size_t *bytes,
void *client_data) {
	(void)dec;
	FlacState *st = (FlacState *)client_data;
	size_t remaining = ss_file_remaining(st->file);
	if(remaining == 0) {
		*bytes = 0;
		return FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
	}
	size_t to_read = *bytes < remaining ? *bytes : remaining;
	ss_file_read_bytes(st->file, st->offset, buf, to_read);
	st->offset += to_read;
	if(st->offset > st->size) {
		st->offset = st->size;
	}
	*bytes = to_read;
	return FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
}

static FLAC__StreamDecoderSeekStatus flac_seek_cb(
const FLAC__StreamDecoder *dec,
FLAC__uint64 absolute_byte_offset, void *client_data) {
	(void)dec;
	FlacState *st = (FlacState *)client_data;

	ss_file_seek(st->file, absolute_byte_offset);
	st->offset = absolute_byte_offset;
	if(st->offset > st->size) {
		st->offset = st->size;
	}

	return FLAC__STREAM_DECODER_SEEK_STATUS_OK;
}

static FLAC__StreamDecoderTellStatus flac_tell_cb(
const FLAC__StreamDecoder *dec,
FLAC__uint64 *absolute_byte_offset, void *client_data) {
	(void)dec;
	FlacState *st = (FlacState *)client_data;

	*absolute_byte_offset = st->offset;

	return FLAC__STREAM_DECODER_TELL_STATUS_OK;
}

static FLAC__bool flac_eof_cb(const FLAC__StreamDecoder *dec,
                              void *client_data) {
	(void)dec;
	FlacState *st = (FlacState *)client_data;

	return (FLAC__bool)(st->offset == st->size);
}

static FLAC__StreamDecoderLengthStatus flac_length_cb(
const FLAC__StreamDecoder *dec,
FLAC__uint64 *stream_length, void *client_data) {
	(void)dec;
	FlacState *st = (FlacState *)client_data;

	*stream_length = st->size;

	return FLAC__STREAM_DECODER_LENGTH_STATUS_OK;
}

static FLAC__StreamDecoderWriteStatus flac_write_cb(
const FLAC__StreamDecoder *dec,
const FLAC__Frame *frame,
const FLAC__int32 *const *buffer,
void *client_data) {
	(void)dec;
	FlacState *st = (FlacState *)client_data;
	uint32_t block_size = frame->header.blocksize;
	int channels = (int)frame->header.channels;
	uint32_t bps = frame->header.bits_per_sample;

	bool aborting = false;
	if(block_size > st->sample_count) {
		block_size = (uint32_t)st->sample_count;
		aborting = true;
	}
	st->sample_count -= block_size;

	st->channels = channels;
	st->sample_rate = frame->header.sample_rate;
	st->bits_per_sample = bps;

	/* Mix down to mono: average all channels */
	size_t need = st->pcm_frames + block_size;
	if(need > st->pcm_cap) {
		size_t nc = st->pcm_cap ? st->pcm_cap * 2 : 4096;
		while(nc < need) nc *= 2;
		float *tmp = (float *)realloc(st->pcm, nc * sizeof(float));
		if(!tmp) {
			st->error = true;
			return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
		}
		st->pcm = tmp;
		st->pcm_cap = nc;
	}

	float scale = 1.0f / (float)(1u << (bps - 1));
	for(uint32_t i = 0; i < block_size; i++) {
		float sum = 0.0f;
		for(int c = 0; c < channels; c++)
			sum += (float)buffer[c][i];
		st->pcm[st->pcm_frames++] = (sum / (float)channels) * scale;
	}
	return aborting ? FLAC__STREAM_DECODER_WRITE_STATUS_ABORT :
	                  FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

static void flac_metadata_cb(
const FLAC__StreamDecoder *dec,
const FLAC__StreamMetadata *metadata,
void *client_data) {
	(void)dec;
	FlacState *st = (FlacState *)client_data;
	if(metadata->type == FLAC__METADATA_TYPE_STREAMINFO) {
		/* Pre-allocate based on total samples */
		uint64_t total = metadata->data.stream_info.total_samples;
		if(total > 0 && total < 0x10000000) {
			float *tmp = (float *)realloc(st->pcm, (size_t)total * sizeof(float));
			if(tmp) {
				st->pcm = tmp;
				st->pcm_cap = (size_t)total;
			}
		}
	}
}

static void flac_error_cb(
const FLAC__StreamDecoder *dec,
FLAC__StreamDecoderErrorStatus status,
void *client_data) {
	(void)dec;
	FlacState *st = (FlacState *)client_data;
	/* sf2pack writes streams, which may lose sync after their end */
	st->error = status != FLAC__STREAM_DECODER_ERROR_STATUS_LOST_SYNC;
}

/* ── Public entry point ───────────────────────────────────────────────────── */

/**
 * Decode FLAC-compressed data in ss->compressed_data into ss->audio_data.
 * Returns true on success.
 */
bool ss_flac_decode(SS_BasicSample *s) {
	if(!s || !s->audio_file)
		return false;

	FlacState st;
	memset(&st, 0, sizeof(st));
	st.file = s->audio_file;
	st.sample_count = s->audio_file_sample_count;
	ss_file_seek(st.file, 0);
	st.offset = 0;
	st.size = ss_file_size(st.file);

	FLAC__StreamDecoder *dec = FLAC__stream_decoder_new();
	if(!dec) return false;

	FLAC__StreamDecoderInitStatus init_status =
	FLAC__stream_decoder_init_stream(
	dec,
	flac_read_cb,
	flac_seek_cb,
	flac_tell_cb,
	flac_length_cb,
	flac_eof_cb,
	flac_write_cb,
	flac_metadata_cb,
	flac_error_cb,
	&st);

	if(init_status != FLAC__STREAM_DECODER_INIT_STATUS_OK) {
		FLAC__stream_decoder_delete(dec);
		free(st.pcm);
		return false;
	}

	if(s->audio_file_sample_offset) {
		if(!FLAC__stream_decoder_seek_absolute(dec, s->audio_file_sample_offset)) {
			FLAC__stream_decoder_delete(dec);
			free(st.pcm);
			return false;
		}
	}

	FLAC__stream_decoder_process_until_end_of_stream(dec);
	FLAC__stream_decoder_finish(dec);
	FLAC__stream_decoder_delete(dec);

	if(st.error || !st.pcm || st.pcm_frames == 0) {
		free(st.pcm);
		return false;
	}

	/* Transfer ownership to sample */
	s->audio_data = st.pcm;
	s->audio_data_length = st.pcm_frames;
	s->sample_rate = st.sample_rate;

	/* Resize to possibly shrink, and add a bit for interpolators */
	float *audio_data = (float *)realloc(s->audio_data, (s->audio_data_length + SS_SAMPLE_COUNT_BUMP) * sizeof(float));
	if(audio_data) {
		s->audio_data = audio_data;
		memset(audio_data + s->audio_data_length, 0, SS_SAMPLE_COUNT_BUMP * sizeof(float));
	}

	/* Free compressed data */
	if(s->owns_raw_data) {
		free(s->compressed_data);
		ss_file_close(s->audio_file);
	}
	s->compressed_data = NULL;
	s->compressed_data_length = 0;
	s->audio_file = NULL;
	return true;
}

#endif /* SS_HAVE_LIBFLAC */
