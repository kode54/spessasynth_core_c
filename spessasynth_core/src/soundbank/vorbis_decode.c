/**
 * vorbis_decode.c
 * Ogg Vorbis sample decoder for SF3 format using libvorbisfile.
 */

#ifdef SS_HAVE_LIBVORBISFILE

#include <stdio.h>

#include <vorbis/codec.h>
#include <vorbis/vorbisfile.h>

#include <stdlib.h>
#include <string.h>
#if __has_include(<spessasynth_core/spessasynth.h>)
#include <spessasynth_core/soundbank.h>
#else
#include "spessasynth/soundbank/soundbank.h"
#endif

typedef struct {
	SS_File *file;
	ogg_int64_t offset, size;
} SS_FileVorbis;

size_t ssVorbisRead(void *buf, size_t size, size_t nmemb, void *datasource) {
	SS_FileVorbis *file = (SS_FileVorbis *)datasource;

	size_t remaining = file->size - file->offset;
	size_t to_read = size * nmemb;
	if(to_read > remaining) to_read = remaining;
	ss_file_read_bytes(file->file, file->offset, buf, to_read);
	file->offset += to_read;
	return to_read / size;
}

int ssVorbisSeek(void *datasource, ogg_int64_t offset, int whence) {
	SS_FileVorbis *file = (SS_FileVorbis *)datasource;

	switch(whence) {
		case SEEK_CUR:
			offset += file->offset;
			break;

		case SEEK_END:
			offset += file->size;
			break;
	}

	if(offset < 0 || offset > file->size) {
		offset = file->size;
		return -1;
	}

	file->offset = offset;
	return 0;
}

int ssVorbisClose(void *datasource) {
	return 0;
}

long ssVorbisTell(void *datasource) {
	SS_FileVorbis *file = (SS_FileVorbis *)datasource;

	return file->offset;
}

bool ss_vorbis_decode(SS_BasicSample *s) {
	if(!s->audio_file) return false;

	int channels = 0;
	long sample_rate = 0;
	float *pcm = NULL;

	bool partial_sample = (s->audio_file_sample_offset > 0) || (s->audio_file_sample_count != ~0ULL);

	ov_callbacks callbacks = {
		.read_func = ssVorbisRead,
		.seek_func = ssVorbisSeek,
		.close_func = ssVorbisClose,
		.tell_func = ssVorbisTell
	};

	SS_FileVorbis fileVorbis = {
		.file = s->audio_file,
		.offset = 0,
		.size = ss_file_size(s->audio_file)
	};

	ss_file_seek(s->audio_file, 0);

	OggVorbis_File vorbisRef;

	if(ov_open_callbacks(&fileVorbis, &vorbisRef, NULL, 0, callbacks) != 0) {
		return false;
	}

	vorbis_info *vi;

	vi = ov_info(&vorbisRef, -1);

	channels = vi->channels;
	sample_rate = vi->rate;

	long seekable = ov_seekable(&vorbisRef);
	if(!seekable && partial_sample) {
		ov_clear(&vorbisRef);
		return false;
	}

	long totalFrames = ov_pcm_total(&vorbisRef, -1);

	long seekFrame = partial_sample ? (long)s->audio_file_sample_offset : 0;
	long maxFrame = partial_sample ? (long)s->audio_file_sample_count : totalFrames;
	if(maxFrame + seekFrame > totalFrames) {
		maxFrame = totalFrames - seekFrame;
	}

	if(seekFrame > 0) {
		ov_pcm_seek(&vorbisRef, seekFrame);
	}

	pcm = malloc((maxFrame + SS_SAMPLE_COUNT_BUMP) * sizeof(float));
	if(!pcm) {
		ov_clear(&vorbisRef);
		return false;
	}

	long n_samples = 0;
	while(n_samples < maxFrame) {
		long maxBlock = maxFrame - n_samples;
		if(maxBlock > 1024) maxBlock = 1024;
		float **outpcm;
		int currentSection;
		long ret = ov_read_float(&vorbisRef, &outpcm, (int)maxBlock, &currentSection);
		if(ret == OV_HOLE) continue;
		if(ret < 0) {
			ov_clear(&vorbisRef);
			free(pcm);
			return false;
		}
		if(channels == 1) {
			memcpy(pcm + n_samples, outpcm[0], ret * sizeof(float));
		} else {
			for(long i = 0; i < ret; i++) {
				float sample = 0;
				for(int c = 0; c < channels; c++) {
					sample += outpcm[c][i];
				}
				pcm[n_samples + i] = sample / (float)channels;
			}
		}
		n_samples += ret;
	}

	ov_clear(&vorbisRef);

	if(n_samples <= 0) {
		return false;
	}

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

#endif /* SS_HAVE_LIBVORBISFILE */
